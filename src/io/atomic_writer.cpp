#include "melkor/io/atomic_writer.hpp"

#include "melkor/checked.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <random>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace melkor::io {
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kWriteChunkBytes = std::size_t{1024} * 1024;
constexpr std::size_t kAtomicStreamBufferBytes = std::size_t{64} * 1024;

Diagnostic io_error(const std::string& code, const std::string& message, const fs::path& path,
                    const OperationContext& context) {
    Diagnostic diagnostic(code, Severity::error, message);
    diagnostic.with_path(redact_path(path.u8string(), context.path_policy, context.path_root));
    return diagnostic;
}

#if !defined(_WIN32)
Diagnostic errno_diagnostic(const std::string& code, const std::string& message,
                            const fs::path& path, const OperationContext& context, int error) {
    Diagnostic diagnostic = io_error(code, message, path, context);
    diagnostic.with_context("errno", static_cast<std::int64_t>(error));
    diagnostic.with_context("system_error", std::string(std::strerror(error)));
    return diagnostic;
}
#endif

// An unpredictable temporary name.
//
// Predictable temporary names are a classic local attack: an attacker who can guess the name
// pre-creates it as a symlink pointing somewhere sensitive, and the victim process writes
// there. O_EXCL closes most of that window on its own, but a random name removes the ability
// to even attempt it. std::random_device is used rather than a time- or PID-seeded PRNG
// because both of those are exactly what an attacker can predict.
std::string random_suffix() {
    static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device device;
    std::uniform_int_distribution<int> distribution(0, static_cast<int>(sizeof(kAlphabet) - 2));

    std::string suffix;
    suffix.reserve(16);
    for (int i = 0; i < 16; ++i) {
        suffix.push_back(kAlphabet[distribution(device)]);
    }
    return suffix;
}

bool has_embedded_nul(const fs::path& path) {
    const auto& native = path.native();
    return std::find(native.begin(), native.end(), fs::path::value_type{}) != native.end();
}

}  // namespace

Result<std::unique_ptr<AtomicWriter>> AtomicWriter::create(const fs::path& destination,
                                                           const WriteOptions& options,
                                                           const OperationContext& context) try {
    using ResultT = Result<std::unique_ptr<AtomicWriter>>;

    if (context.budget == nullptr) {
        return ResultT::failure(ErrorCode::internal_error,
                                Diagnostic("MK0310_NO_BUDGET", Severity::error,
                                           "atomic output requires a resource budget"));
    }
    if (auto valid = context.budget->limits().validate(); !valid.has_value()) {
        return ResultT::failure(ErrorCode::invalid_argument, valid.diagnostics());
    }
    if (options.durability != Durability::metadata && options.durability != Durability::full) {
        Diagnostic diagnostic("MK0531_INVALID_DURABILITY", Severity::error,
                              "the output durability value is invalid");
        diagnostic.with_context("durability", static_cast<std::uint64_t>(options.durability));
        return ResultT::failure(ErrorCode::invalid_argument, std::move(diagnostic));
    }
    if (auto control = context.check("atomic_writer.create"); !control.has_value()) {
        return ResultT::failure(control.error_code(), control.diagnostics());
    }
    if (destination.empty()) {
        return ResultT::failure(ErrorCode::invalid_argument,
                                Diagnostic("MK0517_OUTPUT_PATH_EMPTY", Severity::error,
                                           "the output path must not be empty"));
    }
    if (has_embedded_nul(destination)) {
        return ResultT::failure(ErrorCode::invalid_argument,
                                Diagnostic("MK0527_OUTPUT_PATH_NUL", Severity::error,
                                           "the output path must not contain a null character"));
    }

    const fs::path parent =
        destination.has_parent_path() ? destination.parent_path() : fs::path(".");
    auto writer = std::unique_ptr<AtomicWriter>(new AtomicWriter());
    writer->destination_ = destination;
    writer->options_ = options;
    writer->context_ = context;

#if defined(_WIN32)
    // Validate the parent before creating the temporary. Keep the Windows path unchanged.
    std::error_code ec;
    const fs::file_status parent_status = fs::status(parent, ec);
    if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory ||
        parent_status.type() == fs::file_type::not_found) {
        return ResultT::failure(ErrorCode::io_error,
                                io_error("MK0501_OUTPUT_PARENT_MISSING",
                                         "the output directory does not exist", parent, context));
    }
    if (ec) {
        Diagnostic diagnostic =
            io_error("MK0514_PERMISSION_FAILED", "could not read the output directory metadata",
                     parent, context);
        diagnostic.with_context("system_error", ec.message());
        diagnostic.with_context("error_value", static_cast<std::int64_t>(ec.value()));
        return ResultT::failure(ErrorCode::io_error, std::move(diagnostic));
    }
    if (!fs::is_directory(parent_status)) {
        return ResultT::failure(ErrorCode::io_error,
                                io_error("MK0502_OUTPUT_PARENT_NOT_DIRECTORY",
                                         "the output path's parent is not a directory", parent,
                                         context));
    }

    HANDLE directory_handle =
        ::CreateFileW(parent.wstring().c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory_handle == INVALID_HANDLE_VALUE) {
        Diagnostic diagnostic = io_error("MK0502_OUTPUT_PARENT_NOT_DIRECTORY",
                                         "could not open the output directory", parent, context);
        diagnostic.with_context("windows_error", static_cast<std::int64_t>(::GetLastError()));
        return ResultT::failure(ErrorCode::io_error, std::move(diagnostic));
    }
    writer->directory_handle_ = directory_handle;
    writer->destination_name_ = destination.filename().wstring();
    if (writer->destination_name_.empty()) {
        return ResultT::failure(ErrorCode::io_error,
                                io_error("MK0504_OUTPUT_IS_DIRECTORY",
                                         "the output path is a directory", destination, context));
    }
    if (writer->destination_name_.find(L':') != std::wstring::npos) {
        return ResultT::failure(
            ErrorCode::invalid_argument,
            io_error("MK0529_OUTPUT_STREAM_UNSUPPORTED",
                     "the output path must not name a Windows alternate data stream", destination,
                     context));
    }

    const fs::file_status destination_status = fs::symlink_status(destination, ec);
    if (ec && ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory) {
        Diagnostic diagnostic =
            io_error("MK0514_PERMISSION_FAILED", "could not read the destination metadata",
                     destination, context);
        diagnostic.with_context("system_error", ec.message());
        diagnostic.with_context("error_value", static_cast<std::int64_t>(ec.value()));
        return ResultT::failure(ErrorCode::io_error, std::move(diagnostic));
    }
    const bool destination_exists = !ec && fs::exists(destination_status);
    if (destination_exists) {
        const bool destination_is_symlink = fs::is_symlink(destination_status);
        if (destination_is_symlink && !options.allow_output_symlink) {
            return ResultT::failure(
                ErrorCode::io_error,
                io_error("MK0503_OUTPUT_IS_SYMLINK",
                         "the output path is a symbolic link. Enable allow_output_symlink to "
                         "replace the link path without changing its target.",
                         destination, context));
        }
        if (!destination_is_symlink && !fs::is_regular_file(destination_status) &&
            !fs::is_directory(destination_status)) {
            return ResultT::failure(ErrorCode::io_error,
                                    io_error("MK0532_OUTPUT_NOT_REGULAR",
                                             "the output path is not a regular file", destination,
                                             context));
        }
        const fs::file_status followed_status = fs::status(destination, ec);
        if (ec && !destination_is_symlink) {
            Diagnostic diagnostic =
                io_error("MK0514_PERMISSION_FAILED", "could not read the destination metadata",
                         destination, context);
            diagnostic.with_context("system_error", ec.message());
            diagnostic.with_context("error_value", static_cast<std::int64_t>(ec.value()));
            return ResultT::failure(ErrorCode::io_error, std::move(diagnostic));
        }
        if (!ec && fs::is_directory(followed_status)) {
            return ResultT::failure(ErrorCode::io_error, io_error("MK0504_OUTPUT_IS_DIRECTORY",
                                                                  "the output path is a directory",
                                                                  destination, context));
        }
        if (!options.overwrite) {
            return ResultT::failure(
                ErrorCode::io_error,
                io_error("MK0505_OUTPUT_EXISTS",
                         "the output file already exists. Pass --force to replace it.", destination,
                         context));
        }
    }
#else
    // Open the parent once. All later path operations use this directory object.
    int directory_flags = O_RDONLY | O_CLOEXEC;
#if defined(O_DIRECTORY)
    directory_flags |= O_DIRECTORY;
#endif
    const int directory_fd = ::open(parent.c_str(), directory_flags);
    if (directory_fd < 0) {
        const int error = errno;
        if (error == ENOTDIR) {
            return ResultT::failure(ErrorCode::io_error,
                                    errno_diagnostic("MK0502_OUTPUT_PARENT_NOT_DIRECTORY",
                                                     "the output path's parent is not a directory",
                                                     parent, context, error));
        }
        return ResultT::failure(ErrorCode::io_error,
                                errno_diagnostic("MK0501_OUTPUT_PARENT_MISSING",
                                                 "the output directory does not exist", parent,
                                                 context, error));
    }
    writer->directory_fd_ = directory_fd;

    struct stat directory_stat{};
    if (::fstat(writer->directory_fd_, &directory_stat) != 0) {
        return ResultT::failure(ErrorCode::io_error,
                                errno_diagnostic("MK0502_OUTPUT_PARENT_NOT_DIRECTORY",
                                                 "could not validate the output directory", parent,
                                                 context, errno));
    }
    if (!S_ISDIR(directory_stat.st_mode)) {
        return ResultT::failure(ErrorCode::io_error,
                                io_error("MK0502_OUTPUT_PARENT_NOT_DIRECTORY",
                                         "the output path's parent is not a directory", parent,
                                         context));
    }

    writer->destination_name_ = destination.filename().string();
    if (!destination.empty() && writer->destination_name_.empty()) {
        return ResultT::failure(ErrorCode::io_error,
                                io_error("MK0504_OUTPUT_IS_DIRECTORY",
                                         "the output path is a directory", destination, context));
    }

    struct stat destination_stat{};
    const int destination_status =
        ::fstatat(writer->directory_fd_, writer->destination_name_.c_str(), &destination_stat,
                  AT_SYMLINK_NOFOLLOW);
    const int destination_error = destination_status == 0 ? 0 : errno;
    const bool destination_exists = destination_status == 0;
    if (!destination_exists && destination_error != ENOENT) {
        return ResultT::failure(ErrorCode::io_error,
                                errno_diagnostic("MK0514_PERMISSION_FAILED",
                                                 "could not read the destination metadata",
                                                 destination, context, destination_error));
    }

    if (destination_exists) {
        const bool destination_is_symlink = S_ISLNK(destination_stat.st_mode);
        if (destination_is_symlink && !options.allow_output_symlink) {
            return ResultT::failure(
                ErrorCode::io_error,
                io_error("MK0503_OUTPUT_IS_SYMLINK",
                         "the output path is a symbolic link. Enable allow_output_symlink to "
                         "replace the link path without changing its target.",
                         destination, context));
        }
        if (!destination_is_symlink && !S_ISREG(destination_stat.st_mode) &&
            !S_ISDIR(destination_stat.st_mode)) {
            return ResultT::failure(ErrorCode::io_error,
                                    io_error("MK0532_OUTPUT_NOT_REGULAR",
                                             "the output path is not a regular file", destination,
                                             context));
        }

        struct stat permission_stat = destination_stat;
        bool permission_stat_valid = true;
        int permission_error = 0;
        if (destination_is_symlink) {
            if (::fstatat(writer->directory_fd_, writer->destination_name_.c_str(),
                          &permission_stat, 0) != 0) {
                permission_stat_valid = false;
                permission_error = errno;
            } else if (S_ISDIR(permission_stat.st_mode)) {
                return ResultT::failure(ErrorCode::io_error,
                                        io_error("MK0504_OUTPUT_IS_DIRECTORY",
                                                 "the output path is a directory", destination,
                                                 context));
            }
        } else if (S_ISDIR(destination_stat.st_mode)) {
            return ResultT::failure(ErrorCode::io_error, io_error("MK0504_OUTPUT_IS_DIRECTORY",
                                                                  "the output path is a directory",
                                                                  destination, context));
        }

        if (!options.overwrite) {
            return ResultT::failure(
                ErrorCode::io_error,
                io_error("MK0505_OUTPUT_EXISTS",
                         "the output file already exists. Pass --force to replace it.", destination,
                         context));
        }
        if (!permission_stat_valid) {
            return ResultT::failure(ErrorCode::io_error,
                                    errno_diagnostic("MK0514_PERMISSION_FAILED",
                                                     "could not read the destination permissions",
                                                     destination, context, permission_error));
        }
        writer->final_permissions_ = static_cast<std::uint32_t>(permission_stat.st_mode & 0777u);
    }
#endif

    // Create the temporary in the same directory.

#if !defined(_WIN32)
    // Query the open directory because POSIX filesystems can use different name limits.
    constexpr std::size_t kTempNameLength = 28;
    errno = 0;
    const long queried_name_max = ::fpathconf(writer->directory_fd_, _PC_NAME_MAX);
    const std::size_t name_max =
        queried_name_max > 0 ? static_cast<std::size_t>(queried_name_max) : 255;
    if (name_max < kTempNameLength) {
        return ResultT::failure(ErrorCode::io_error,
                                io_error("MK0520_TEMP_NAME_LIMIT",
                                         "the output directory name limit is too small", parent,
                                         context));
    }
#endif

    // A handful of attempts, in case an unlikely name collision occurs. Not a loop forever:
    // if O_EXCL keeps failing on random 16-character names, something is wrong that retrying
    // will not fix.
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string candidate_name = ".melkor-" + random_suffix() + ".tmp";
        fs::path candidate = parent / candidate_name;

#if defined(_WIN32)
        // CREATE_NEW is the Windows equivalent of O_EXCL: it fails if the file exists.
        // FILE_FLAG_OPEN_REPARSE_POINT ensures we do not traverse a reparse point that was
        // planted at the temporary's path.
        HANDLE handle = ::CreateFileW(
            candidate.wstring().c_str(), GENERIC_WRITE | DELETE, 0 /* no sharing */, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            writer->handle_ = handle;
            writer->temporary_ = std::move(candidate);
            return ResultT::success(std::move(writer));
        }
        if (::GetLastError() != ERROR_FILE_EXISTS) {
            Diagnostic diagnostic =
                io_error("MK0506_TEMP_CREATE_FAILED", "could not create the temporary output file",
                         candidate, context);
            diagnostic.with_context("windows_error", static_cast<std::int64_t>(::GetLastError()));
            return ResultT::failure(ErrorCode::io_error, std::move(diagnostic));
        }
#else
        // O_EXCL | O_CREAT is atomic: it fails if the path exists, which defeats a symlink
        // planted at the temporary's location. O_NOFOLLOW is belt-and-braces on the same idea.
        //
        // Mode 0600, not 0644: a temporary that is briefly world-readable leaks the contents
        // of the file being written, and the final permissions are set at commit.
        const int fd = ::openat(writer->directory_fd_, candidate_name.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            writer->fd_ = fd;
            writer->temporary_name_ = std::move(candidate_name);
            writer->temporary_ = std::move(candidate);
            return ResultT::success(std::move(writer));
        }
        if (errno != EEXIST) {
            return ResultT::failure(ErrorCode::io_error,
                                    errno_diagnostic("MK0506_TEMP_CREATE_FAILED",
                                                     "could not create the temporary output file",
                                                     candidate, context, errno));
        }
#endif
    }

    return ResultT::failure(
        ErrorCode::io_error,
        io_error("MK0507_TEMP_NAME_COLLISION",
                 "could not find an unused temporary filename after several attempts", destination,
                 context));
} catch (const std::bad_alloc&) {
    return Result<std::unique_ptr<AtomicWriter>>::failure(
        ErrorCode::resource_limit, Diagnostic("MK0521_ATOMIC_CREATE_MEMORY", Severity::error,
                                              "atomic output creation exceeded available memory"));
} catch (const std::length_error&) {
    return Result<std::unique_ptr<AtomicWriter>>::failure(
        ErrorCode::resource_limit,
        Diagnostic("MK0522_ATOMIC_CREATE_SIZE", Severity::error,
                   "the atomic output path exceeds a container size limit"));
} catch (const std::system_error& error) {
    Diagnostic diagnostic("MK0523_ATOMIC_CREATE_SYSTEM", Severity::error,
                          "the system rejected atomic output creation");
    diagnostic.with_context("system_error", std::string(error.what()));
    return Result<std::unique_ptr<AtomicWriter>>::failure(ErrorCode::io_error,
                                                          std::move(diagnostic));
} catch (const std::exception& error) {
    Diagnostic diagnostic("MK0526_ATOMIC_CREATE_FAILED", Severity::error,
                          "atomic output creation failed");
    diagnostic.with_context("error", std::string(error.what()));
    return Result<std::unique_ptr<AtomicWriter>>::failure(ErrorCode::io_error,
                                                          std::move(diagnostic));
} catch (...) {
    return Result<std::unique_ptr<AtomicWriter>>::failure(
        ErrorCode::internal_error,
        Diagnostic("MK0526_ATOMIC_CREATE_FAILED", Severity::error,
                   "atomic output creation failed with an unknown error"));
}

AtomicWriter::~AtomicWriter() noexcept {
    // The safe outcome is the one you get by doing nothing. If the caller returned early, or
    // an exception unwound past them, the temporary is removed and the destination is left
    // exactly as it was.
    if (!committed_) {
        abort();
    }
}

Result<void> AtomicWriter::write(const void* data, std::size_t size) try {
    if (committed_) {
        return Result<void>::failure(ErrorCode::internal_error,
                                     Diagnostic("MK0508_WRITE_AFTER_COMMIT", Severity::error,
                                                "write() called after commit()"));
    }

    if (size == 0) {
        return Result<void>::success();
    }

    if (write_failed_) {
        return Result<void>::failure(
            ErrorCode::io_error, Diagnostic("MK0530_WRITE_ALREADY_FAILED", Severity::error,
                                            "write() called after an earlier output write failed"));
    }

    if (data == nullptr) {
        write_failed_ = true;
        return Result<void>::failure(
            ErrorCode::invalid_argument,
            Diagnostic("MK0518_NULL_WRITE_BUFFER", Severity::error,
                       "write() received a null buffer with a nonzero size"));
    }

    // Charge the temp-disk budget before writing, so an output that grows without bound is
    // stopped rather than filling the disk. After the write would be too late.
    if (context_.budget != nullptr) {
        auto charged = context_.consume(BudgetKind::temp_bytes, size, "atomic_writer.write");
        if (!charged.has_value()) {
            write_failed_ = true;
            return charged;
        }
        temp_bytes_reserved_ += static_cast<std::uint64_t>(size);
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;
    const std::uint64_t target = bytes_written_ + static_cast<std::uint64_t>(size);

    while (remaining > 0) {
        auto control =
            context_.checkpoint({"atomic_writer.write", "write", bytes_written_, target, "bytes"});
        if (!control.has_value()) {
            write_failed_ = true;
            return control;
        }
        const std::size_t request = std::min(remaining, kWriteChunkBytes);
#if defined(_WIN32)
        const DWORD chunk = static_cast<DWORD>(request);
        DWORD written = 0;
        if (!::WriteFile(static_cast<HANDLE>(handle_), bytes, chunk, &written, nullptr)) {
            write_failed_ = true;
            Diagnostic diagnostic("MK0509_WRITE_FAILED", Severity::error,
                                  "failed writing to the temporary output file");
            diagnostic.with_context("windows_error", static_cast<std::int64_t>(::GetLastError()));
            return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
        }
        const std::size_t n = written;
#else
        const ssize_t n = ::write(fd_, bytes, request);
        if (n < 0) {
            // A signal that interrupted the write is not a failure; retry it. Treating EINTR
            // as an error would make output spuriously fail whenever a progress timer fires.
            if (errno == EINTR) {
                continue;
            }
            Diagnostic diagnostic("MK0509_WRITE_FAILED", Severity::error,
                                  "failed writing to the temporary output file");
            diagnostic.with_context("errno", static_cast<std::int64_t>(errno));
            diagnostic.with_context("system_error", std::string(std::strerror(errno)));
            write_failed_ = true;
            return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
        }
#endif
        if (n == 0) {
            write_failed_ = true;
            return Result<void>::failure(
                ErrorCode::io_error,
                Diagnostic("MK0509_WRITE_FAILED", Severity::error,
                           "the filesystem accepted zero bytes; the disk may be full"));
        }

        bytes += n;
        remaining -= static_cast<std::size_t>(n);
        bytes_written_ += static_cast<std::uint64_t>(n);
    }

    auto control =
        context_.checkpoint({"atomic_writer.write", "write", bytes_written_, target, "bytes"});
    if (!control.has_value()) {
        write_failed_ = true;
        return control;
    }

    return Result<void>::success();
} catch (const std::bad_alloc&) {
    write_failed_ = true;
    return Result<void>::failure(ErrorCode::resource_limit,
                                 Diagnostic("MK0536_ATOMIC_WRITE_MEMORY", Severity::error,
                                            "the output write exceeded available memory"));
} catch (const std::length_error&) {
    write_failed_ = true;
    return Result<void>::failure(ErrorCode::resource_limit,
                                 Diagnostic("MK0537_ATOMIC_WRITE_SIZE", Severity::error,
                                            "the output write exceeded a container size limit"));
} catch (...) {
    write_failed_ = true;
    return Result<void>::failure(
        ErrorCode::internal_error,
        Diagnostic("MK0533_ATOMIC_WRITE_EXCEPTION", Severity::error,
                   "the output write failed with an unexpected exception"));
}

Result<void> AtomicWriter::commit() try {
    if (committed_) {
        return Result<void>::failure(
            ErrorCode::internal_error,
            Diagnostic("MK0510_DOUBLE_COMMIT", Severity::error, "commit() called twice"));
    }

    if (write_failed_) {
        abort();
        return Result<void>::failure(ErrorCode::io_error,
                                     Diagnostic("MK0519_COMMIT_AFTER_WRITE_FAILED", Severity::error,
                                                "commit() refused an output after a failed write"));
    }

    auto control = context_.checkpoint(
        {"atomic_writer.commit", "commit", bytes_written_, bytes_written_, "bytes"});
    if (!control.has_value()) {
        abort();
        return control;
    }

#if defined(_WIN32)
    HANDLE handle = static_cast<HANDLE>(handle_);

    if (options_.durability == Durability::full) {
        if (!::FlushFileBuffers(handle)) {
            Diagnostic diagnostic("MK0511_FLUSH_FAILED", Severity::error,
                                  "failed flushing the temporary output file to storage");
            diagnostic.with_context("windows_error", static_cast<std::int64_t>(::GetLastError()));
            abort();
            return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
        }
    }

    // Rename the open file through its handle. Closing it before a path-based rename lets
    // another process replace the temporary between close and rename.
    const std::size_t name_bytes = destination_name_.size() * sizeof(wchar_t);
    const std::size_t rename_bytes = offsetof(FILE_RENAME_INFO, FileName) + name_bytes;
    const std::size_t rename_words =
        (rename_bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    std::vector<std::max_align_t> rename_storage(rename_words);
    std::memset(rename_storage.data(), 0, rename_bytes);
    auto* rename_info = reinterpret_cast<FILE_RENAME_INFO*>(rename_storage.data());
    rename_info->ReplaceIfExists = options_.overwrite ? TRUE : FALSE;
    rename_info->RootDirectory = static_cast<HANDLE>(directory_handle_);
    rename_info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(rename_info->FileName, destination_name_.data(), name_bytes);

    // Antivirus and search indexers can cause a temporary sharing violation.
    const int kMaxAttempts = 10;
    DWORD last_rename_error = ERROR_SUCCESS;
    bool installed = false;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        control =
            context_.checkpoint({"atomic_writer.commit", "rename",
                                 static_cast<std::uint64_t>(attempt), kMaxAttempts, "attempts"});
        if (!control.has_value()) {
            abort();
            return control;
        }
        if (::SetFileInformationByHandle(handle, FileRenameInfo, rename_info,
                                         static_cast<DWORD>(rename_bytes))) {
            installed = true;
            break;
        }
        const DWORD error = ::GetLastError();
        last_rename_error = error;
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            break;
        }
        for (int interval = 0; interval <= attempt; ++interval) {
            ::Sleep(20);
            control = context_.checkpoint({"atomic_writer.commit", "rename_wait",
                                           static_cast<std::uint64_t>(attempt), kMaxAttempts,
                                           "attempts"});
            if (!control.has_value()) {
                abort();
                return control;
            }
        }
    }
    if (!installed) {
        Diagnostic diagnostic("MK0513_REPLACE_FAILED", Severity::error,
                              "failed atomically installing the output. The existing file is "
                              "unchanged.");
        diagnostic.with_context("windows_error", static_cast<std::int64_t>(last_rename_error));
        abort();
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }

    committed_ = true;
    temporary_.clear();
    if (context_.budget != nullptr) {
        context_.budget->release(BudgetKind::temp_bytes, temp_bytes_reserved_);
        temp_bytes_reserved_ = 0;
    }

    if (options_.durability == Durability::full && !::FlushFileBuffers(handle)) {
        Diagnostic diagnostic("MK0515_DIRECTORY_SYNC_FAILED", Severity::error,
                              "the output was committed, but rename durability failed");
        diagnostic.with_context("windows_error", static_cast<std::int64_t>(::GetLastError()));
        ::CloseHandle(handle);
        handle_ = nullptr;
        ::CloseHandle(static_cast<HANDLE>(directory_handle_));
        directory_handle_ = nullptr;
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }

    if (!::CloseHandle(handle)) {
        Diagnostic diagnostic("MK0512_CLOSE_FAILED", Severity::error,
                              "the output was committed, but its file handle did not close");
        diagnostic.with_context("windows_error", static_cast<std::int64_t>(::GetLastError()));
        handle_ = nullptr;
        ::CloseHandle(static_cast<HANDLE>(directory_handle_));
        directory_handle_ = nullptr;
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }
    handle_ = nullptr;
    if (!::CloseHandle(static_cast<HANDLE>(directory_handle_))) {
        Diagnostic diagnostic("MK0525_DIRECTORY_CLOSE_FAILED", Severity::error,
                              "the output was committed, but the directory handle did not close");
        diagnostic.with_context("windows_error", static_cast<std::int64_t>(::GetLastError()));
        directory_handle_ = nullptr;
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }
    directory_handle_ = nullptr;
    return Result<void>::success();

#else
    // Set permissions through the open descriptor. Never use a path-based chmod here.
    if (::fchmod(fd_, static_cast<mode_t>(final_permissions_)) != 0) {
        Diagnostic diagnostic("MK0514_PERMISSION_FAILED", Severity::error,
                              "failed setting the output file permissions");
        diagnostic.with_context("errno", static_cast<std::int64_t>(errno));
        abort();
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }

    if (options_.durability == Durability::full && ::fsync(fd_) != 0) {
        Diagnostic diagnostic("MK0511_FLUSH_FAILED", Severity::error,
                              "failed flushing the temporary output file to storage");
        diagnostic.with_context("errno", static_cast<std::int64_t>(errno));
        abort();
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }

    if (::close(fd_) != 0) {
        // close() can report a deferred write error that write() never saw -- notably on NFS
        // and on some filesystems with delayed allocation. Ignoring it would commit a file
        // whose contents never actually landed.
        Diagnostic diagnostic("MK0512_CLOSE_FAILED", Severity::error,
                              "failed closing the temporary output file; its contents may not "
                              "have reached storage");
        diagnostic.with_context("errno", static_cast<std::int64_t>(errno));
        fd_ = -1;
        abort();
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }
    fd_ = -1;

    control = context_.checkpoint(
        {"atomic_writer.commit", "install", bytes_written_, bytes_written_, "bytes"});
    if (!control.has_value()) {
        abort();
        return control;
    }

    // Use the bound directory for both names. A path swap cannot redirect this operation.
    const int install_result = options_.overwrite
                                   ? ::renameat(directory_fd_, temporary_name_.c_str(),
                                                directory_fd_, destination_name_.c_str())
                                   : ::linkat(directory_fd_, temporary_name_.c_str(), directory_fd_,
                                              destination_name_.c_str(), 0);
    if (install_result != 0) {
        const int error = errno;
        Diagnostic diagnostic("MK0513_REPLACE_FAILED", Severity::error,
                              "failed atomically installing the output. The existing file is "
                              "unchanged.");
        diagnostic.with_context("errno", static_cast<std::int64_t>(error));
        diagnostic.with_context("system_error", std::string(std::strerror(error)));
        abort();
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }

    // The destination exists after either renameat() or linkat() succeeds.
    // Keep this state true if later cleanup or durability work fails.
    committed_ = true;
    if (context_.budget != nullptr) {
        context_.budget->release(BudgetKind::temp_bytes, temp_bytes_reserved_);
        temp_bytes_reserved_ = 0;
    }

    if (!options_.overwrite) {
        int unlink_result = 0;
        do {
            unlink_result = ::unlinkat(directory_fd_, temporary_name_.c_str(), 0);
        } while (unlink_result != 0 && errno == EINTR);
        if (unlink_result != 0) {
            const int error = errno;
            Diagnostic diagnostic("MK0516_TEMP_CLEANUP_FAILED", Severity::error,
                                  "the output was installed, but its temporary link remains");
            diagnostic.with_context("errno", static_cast<std::int64_t>(error));
            const int directory_fd = directory_fd_;
            directory_fd_ = -1;
            if (::close(directory_fd) != 0) {
                diagnostic.with_context("directory_close_errno", static_cast<std::int64_t>(errno));
            }
            return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
        }
    }

    temporary_.clear();
    temporary_name_.clear();

    if (options_.durability == Durability::full) {
        if (::fsync(directory_fd_) != 0) {
            const int error = errno;
            Diagnostic diagnostic("MK0515_DIRECTORY_SYNC_FAILED", Severity::error,
                                  "the output was committed, but directory durability failed");
            diagnostic.with_context("errno", static_cast<std::int64_t>(error));
            const int directory_fd = directory_fd_;
            directory_fd_ = -1;
            if (::close(directory_fd) != 0) {
                diagnostic.with_context("directory_close_errno", static_cast<std::int64_t>(errno));
            }
            return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
        }
    }
    const int directory_fd = directory_fd_;
    directory_fd_ = -1;
    if (::close(directory_fd) != 0) {
        const int error = errno;
        Diagnostic diagnostic("MK0525_DIRECTORY_CLOSE_FAILED", Severity::error,
                              "the output was committed, but the directory handle did not close");
        diagnostic.with_context("errno", static_cast<std::int64_t>(error));
        diagnostic.with_context("system_error", std::string(std::strerror(error)));
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }

    return Result<void>::success();
#endif
} catch (const std::bad_alloc&) {
    if (committed_)
        close_after_commit();
    else
        abort();
    return Result<void>::failure(ErrorCode::resource_limit,
                                 Diagnostic("MK0534_ATOMIC_COMMIT_MEMORY", Severity::error,
                                            "the output commit exceeded available memory"));
} catch (const std::length_error&) {
    if (committed_)
        close_after_commit();
    else
        abort();
    return Result<void>::failure(ErrorCode::resource_limit,
                                 Diagnostic("MK0538_ATOMIC_COMMIT_SIZE", Severity::error,
                                            "the output commit exceeded a container size limit"));
} catch (...) {
    if (committed_)
        close_after_commit();
    else
        abort();
    return Result<void>::failure(
        ErrorCode::internal_error,
        Diagnostic("MK0535_ATOMIC_COMMIT_EXCEPTION", Severity::error,
                   "the output commit failed with an unexpected exception"));
}

void AtomicWriter::close_after_commit() noexcept {
#if defined(_WIN32)
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    if (directory_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(directory_handle_));
        directory_handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (directory_fd_ >= 0) {
        ::close(directory_fd_);
        directory_fd_ = -1;
    }
#endif
}

void AtomicWriter::abort() noexcept {
#if defined(_WIN32)
    bool removed = temporary_.empty();
    if (handle_ != nullptr) {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        const bool marked =
            ::SetFileInformationByHandle(static_cast<HANDLE>(handle_), FileDispositionInfo,
                                         &disposition, sizeof(disposition)) != 0;
        const bool closed = ::CloseHandle(static_cast<HANDLE>(handle_)) != 0;
        handle_ = nullptr;
        removed = marked && closed;
    }
    if (directory_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(directory_handle_));
        directory_handle_ = nullptr;
    }
    if (removed && context_.budget != nullptr) {
        context_.budget->release(BudgetKind::temp_bytes, temp_bytes_reserved_);
        temp_bytes_reserved_ = 0;
        temporary_.clear();
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    bool removed = temporary_name_.empty();
    if (!temporary_name_.empty() && directory_fd_ >= 0) {
        int unlink_result = 0;
        do {
            unlink_result = ::unlinkat(directory_fd_, temporary_name_.c_str(), 0);
        } while (unlink_result != 0 && errno == EINTR);
        removed = unlink_result == 0 || errno == ENOENT;
    }

    if (directory_fd_ >= 0) {
        ::close(directory_fd_);
        directory_fd_ = -1;
    }
    if (removed && context_.budget != nullptr) {
        context_.budget->release(BudgetKind::temp_bytes, temp_bytes_reserved_);
        temp_bytes_reserved_ = 0;
        temporary_.clear();
        temporary_name_.clear();
    }
#endif
}

// ---------------------------------------------------------------------------
// AtomicOutputStream
// ---------------------------------------------------------------------------

// Forwards ostream writes into the AtomicWriter.
//
// Sized so that ordinary formatted output (a PLY ASCII record at a time) does not turn into a
// syscall per field, while staying small enough that the buffer itself is not a memory
// concern for a very large file.
class AtomicOutputStream::Buffer : public std::streambuf {
public:
    Buffer(AtomicWriter& writer, Budget::Charge memory)
        : writer_(writer), memory_(std::move(memory)),
          storage_(new char[kAtomicStreamBufferBytes]) {
        setp(storage_.get(), storage_.get() + kAtomicStreamBufferBytes);
    }

    Buffer(AtomicWriter& writer, ErrorCode code, std::vector<Diagnostic> diagnostics) noexcept
        : writer_(writer), failed_(true), error_code_(code), diagnostics_(std::move(diagnostics)) {
        setp(nullptr, nullptr);
    }

    ~Buffer() override {
        // Not flushed here: a destructor cannot report failure, and silently dropping the
        // tail of a file is precisely the class of bug this whole subsystem exists to remove.
        // AtomicOutputStream's destructor flushes explicitly and records any error.
    }

    bool failed() const noexcept { return failed_; }
    ErrorCode error_code() const noexcept { return error_code_; }
    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

    // Pushes whatever is buffered into the writer. Returns false on failure.
    bool flush_to_writer() noexcept try {
        if (failed_)
            return false;
        const std::ptrdiff_t pending = pptr() - pbase();
        if (pending <= 0) {
            return !failed_;
        }

        auto result = writer_.write(pbase(), static_cast<std::size_t>(pending));
        setp(storage_.get(), storage_.get() + kAtomicStreamBufferBytes);

        if (!result.has_value()) {
            failed_ = true;
            error_code_ = result.error_code();
            diagnostics_ = result.diagnostics();
            return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        return record_exception(ErrorCode::resource_limit,
                                "atomic stream flush exceeded available memory");
    } catch (const std::length_error&) {
        return record_exception(ErrorCode::resource_limit,
                                "atomic stream flush exceeded a container size limit");
    } catch (...) {
        return record_exception(ErrorCode::internal_error,
                                "atomic stream flush failed with an unexpected exception");
    }

protected:
    int_type overflow(int_type ch) override {
        if (!flush_to_writer()) {
            return traits_type::eof();
        }
        if (ch != traits_type::eof()) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }

    int sync() override {
        return traits_type::eq_int_type(overflow(traits_type::eof()), traits_type::eof()) ? -1 : 0;
    }

    // Bulk writes bypass the buffer entirely when they are large. Copying a multi-megabyte
    // binary block through a 64 KiB buffer would be pure overhead.
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (failed_) {
            return 0;
        }
        if (count <= 0) {
            return 0;
        }

        if (count >= static_cast<std::streamsize>(kAtomicStreamBufferBytes)) {
            if (!flush_to_writer()) {
                return 0;
            }
            auto result = writer_.write(data, static_cast<std::size_t>(count));
            if (!result.has_value()) {
                failed_ = true;
                error_code_ = result.error_code();
                diagnostics_ = result.diagnostics();
                return 0;
            }
            return count;
        }

        return std::streambuf::xsputn(data, count);
    }

private:
    AtomicWriter& writer_;
    // Release the charge after the buffer storage is destroyed.
    Budget::Charge memory_;
    std::unique_ptr<char[]> storage_;
    bool failed_ = false;
    ErrorCode error_code_ = ErrorCode::ok;
    std::vector<Diagnostic> diagnostics_;

    bool record_exception(ErrorCode code, const char* message) noexcept {
        failed_ = true;
        error_code_ = code;
        try {
            diagnostics_.clear();
            diagnostics_.emplace_back("MK0528_STREAM_FLUSH_EXCEPTION", Severity::error, message);
        } catch (...) {
            diagnostics_.clear();
        }
        return false;
    }
};

AtomicOutputStream::AtomicOutputStream(AtomicWriter& writer) : std::ostream(nullptr) {
    auto memory = writer.context_.budget->reserve(
        BudgetKind::memory_bytes, kAtomicStreamBufferBytes, "atomic_writer.stream_buffer");
    if (memory.has_value()) {
        buffer_ = std::make_unique<Buffer>(writer, std::move(memory).value());
    } else {
        buffer_ = std::make_unique<Buffer>(writer, memory.error_code(), memory.diagnostics());
    }
    rdbuf(buffer_.get());
    if (buffer_->failed())
        setstate(std::ios::badbit);
}

AtomicOutputStream::~AtomicOutputStream() noexcept {
    // Flush the tail. Any error is recorded rather than thrown, and the caller is expected to
    // check failed() before commit() -- committing after a silently failed write would install
    // a truncated file, which is exactly the outcome this subsystem prevents elsewhere.
    if (buffer_) {
        if (!buffer_->flush_to_writer()) {
            // A user can enable an exception mask on any ostream. Disable it before setstate.
            // The stream is being destroyed, so the previous mask does not need restoration.
            exceptions(std::ios::goodbit);
            setstate(std::ios::badbit);
        }
    }
}

const std::vector<Diagnostic>& AtomicOutputStream::diagnostics() const noexcept {
    return buffer_->diagnostics();
}

ErrorCode AtomicOutputStream::error_code() const noexcept {
    return buffer_->error_code();
}

bool AtomicOutputStream::failed() const noexcept {
    return buffer_->failed() || bad();
}

Result<bool> is_same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;

    for (const fs::path* path : {&a, &b}) {
        static_cast<void>(fs::status(*path, ec));
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory)
            return Result<bool>::success(false);
        if (ec) {
            Diagnostic diagnostic("MK0524_FILE_IDENTITY_FAILED", Severity::error,
                                  "could not read file identity metadata");
            diagnostic.with_context("system_error", ec.message());
            diagnostic.with_context("error_value", static_cast<std::int64_t>(ec.value()));
            return Result<bool>::failure(ErrorCode::io_error, std::move(diagnostic));
        }
    }

    // fs::equivalent compares file identity -- device and inode on POSIX, the file index on
    // Windows -- rather than comparing the path strings. That is the only correct way to ask
    // this: "scene.ply", "./scene.ply", "../work/scene.ply", and a symlink to any of them are
    // all the same file, and a converter that reads and writes it at once will destroy it.
    const bool same = fs::equivalent(a, b, ec);

    if (ec) {
        Diagnostic diagnostic("MK0524_FILE_IDENTITY_FAILED", Severity::error,
                              "could not compare the file identities");
        diagnostic.with_context("system_error", ec.message());
        diagnostic.with_context("error_value", static_cast<std::int64_t>(ec.value()));
        return Result<bool>::failure(ErrorCode::io_error, std::move(diagnostic));
    }
    return Result<bool>::success(same);
}

}  // namespace melkor::io
