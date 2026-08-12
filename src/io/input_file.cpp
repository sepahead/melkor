#include "input_file.hpp"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace melkor::io {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kReadChunkBytes = std::size_t{1024} * 1024;

bool has_embedded_nul(const fs::path& path) {
    const auto& native = path.native();
    return std::find(native.begin(), native.end(), fs::path::value_type{}) != native.end();
}

template <class T>
Result<T> fail(ErrorCode error, const char* code, std::string message, const fs::path& path,
               const OperationContext& context) {
    Diagnostic diagnostic(code, Severity::error, std::move(message));
    diagnostic.with_path(redact_path(path.u8string(), context.path_policy, context.path_root));
    return Result<T>::failure(error, std::move(diagnostic));
}

#if defined(__unix__) || defined(__APPLE__)
int open_nointr(const char* path, int flags) noexcept {
    int result = -1;
    do {
        result = ::open(path, flags);
    } while (result < 0 && errno == EINTR);
    return result;
}

int fstat_nointr(int file, struct stat* output) noexcept {
    int result = -1;
    do {
        result = ::fstat(file, output);
    } while (result < 0 && errno == EINTR);
    return result;
}

ssize_t pread_nointr(int file, void* output, std::size_t amount, off_t offset) noexcept {
    ssize_t result = -1;
    do {
        result = ::pread(file, output, amount, offset);
    } while (result < 0 && errno == EINTR);
    return result;
}

bool same_file_state(const struct stat& before, const struct stat& after) noexcept {
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size) {
        return false;
    }
#if defined(__APPLE__)
    return before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
           before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
           before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
           before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
    return before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
}
#elif defined(_WIN32)
bool same_file_state(const BY_HANDLE_FILE_INFORMATION& before,
                     const BY_HANDLE_FILE_INFORMATION& after) noexcept {
    return before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
           before.nFileIndexHigh == after.nFileIndexHigh &&
           before.nFileIndexLow == after.nFileIndexLow &&
           before.nFileSizeHigh == after.nFileSizeHigh &&
           before.nFileSizeLow == after.nFileSizeLow &&
           CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0;
}
#endif

}  // namespace

struct InputFile::Impl {
    fs::path path;
    std::uint64_t size = 0;
#if defined(__unix__) || defined(__APPLE__)
    int file = -1;
    struct stat state{};

    ~Impl() {
        if (file >= 0)
            ::close(file);
    }
#elif defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION state{};

    ~Impl() {
        if (file != nullptr && file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
#else
    std::ifstream stream;
    fs::file_time_type modified{};
#endif
};

InputFile::InputFile(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
InputFile::~InputFile() = default;
InputFile::InputFile(InputFile&&) noexcept = default;
InputFile& InputFile::operator=(InputFile&&) noexcept = default;

Result<InputFile> InputFile::open(const fs::path& path, const OperationContext& context) try {
    if (path.empty() || has_embedded_nul(path)) {
        return fail<InputFile>(ErrorCode::invalid_argument, "MK2306_INPUT_PATH",
                               "the input path is empty or contains a null character", path,
                               context);
    }
    auto impl = std::make_unique<Impl>();
    impl->path = path;
#if defined(__unix__) || defined(__APPLE__)
    impl->file = open_nointr(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (impl->file < 0) {
        if (errno == ELOOP) {
            return fail<InputFile>(ErrorCode::io_error, "MK2302_INPUT_TYPE",
                                   "the input path is not a regular file", path, context);
        }
        return fail<InputFile>(ErrorCode::io_error, "MK2301_INPUT_OPEN",
                               "the input file could not be opened", path, context);
    }
    if (fstat_nointr(impl->file, &impl->state) != 0 || !S_ISREG(impl->state.st_mode)) {
        return fail<InputFile>(ErrorCode::io_error, "MK2302_INPUT_TYPE",
                               "the input path is not a regular file", path, context);
    }
    if (impl->state.st_size < 0) {
        return fail<InputFile>(ErrorCode::resource_limit, "MK2303_INPUT_SIZE",
                               "the input file size is not representable", path, context);
    }
    impl->size = static_cast<std::uint64_t>(impl->state.st_size);
#elif defined(_WIN32)
    impl->file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (impl->file == INVALID_HANDLE_VALUE) {
        return fail<InputFile>(ErrorCode::io_error, "MK2301_INPUT_OPEN",
                               "the input file could not be opened", path, context);
    }
    if (!GetFileInformationByHandle(impl->file, &impl->state) ||
        GetFileType(impl->file) != FILE_TYPE_DISK ||
        (impl->state.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return fail<InputFile>(ErrorCode::io_error, "MK2302_INPUT_TYPE",
                               "the input path is not a regular file", path, context);
    }
    impl->size =
        (static_cast<std::uint64_t>(impl->state.nFileSizeHigh) << 32U) | impl->state.nFileSizeLow;
    if (impl->size > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
        return fail<InputFile>(ErrorCode::resource_limit, "MK2303_INPUT_SIZE",
                               "the input file size is not representable", path, context);
    }
#else
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error || !fs::is_regular_file(status)) {
        return fail<InputFile>(ErrorCode::io_error, "MK2302_INPUT_TYPE",
                               "the input path is not a regular file", path, context);
    }
    const std::uintmax_t file_size = fs::file_size(path, error);
    if (error || file_size > std::numeric_limits<std::uint64_t>::max() ||
        file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max())) {
        return fail<InputFile>(ErrorCode::resource_limit, "MK2303_INPUT_SIZE",
                               "the input file size is not representable", path, context);
    }
    impl->modified = fs::last_write_time(path, error);
    if (error) {
        return fail<InputFile>(ErrorCode::io_error, "MK2301_INPUT_OPEN",
                               "the input file state could not be read", path, context);
    }
    impl->stream.open(path, std::ios::binary);
    if (!impl->stream) {
        return fail<InputFile>(ErrorCode::io_error, "MK2301_INPUT_OPEN",
                               "the input file could not be opened", path, context);
    }
    impl->size = static_cast<std::uint64_t>(file_size);
#endif
    return Result<InputFile>::success(InputFile(std::move(impl)));
} catch (const std::bad_alloc&) {
    return fail<InputFile>(ErrorCode::resource_limit, "MK2303_INPUT_SIZE",
                           "the input file handle allocation failed", path, context);
} catch (const std::length_error&) {
    return fail<InputFile>(ErrorCode::resource_limit, "MK2303_INPUT_SIZE",
                           "the input path exceeds a container limit", path, context);
} catch (const fs::filesystem_error&) {
    return fail<InputFile>(ErrorCode::io_error, "MK2301_INPUT_OPEN",
                           "the input file could not be opened", path, context);
}

std::uint64_t InputFile::size() const noexcept {
    return impl_ == nullptr ? 0 : impl_->size;
}

Result<void> InputFile::read_exact(std::uint64_t offset, std::uint8_t* output, std::size_t amount,
                                   const OperationContext& context, const char* operation,
                                   const char* phase) try {
    if (impl_ == nullptr || (output == nullptr && amount != 0) || operation == nullptr ||
        phase == nullptr) {
        return fail<void>(ErrorCode::invalid_argument, "MK2304_INPUT_READ",
                          "the input read request is invalid",
                          impl_ == nullptr ? fs::path{} : impl_->path, context);
    }
    if (offset > impl_->size || static_cast<std::uint64_t>(amount) > impl_->size - offset) {
        return fail<void>(ErrorCode::io_error, "MK2305_INPUT_CHANGED",
                          "the input file size changed before the read completed", impl_->path,
                          context);
    }

    std::size_t completed = 0;
    while (completed < amount) {
        auto control = context.checkpoint(
            {operation, phase, completed, static_cast<std::uint64_t>(amount), "bytes"});
        if (!control.has_value())
            return control;
        const std::size_t request = std::min(kReadChunkBytes, amount - completed);
#if defined(__unix__) || defined(__APPLE__)
        const std::uint64_t absolute = offset + completed;
        if (absolute > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            return fail<void>(ErrorCode::resource_limit, "MK2303_INPUT_SIZE",
                              "the input read offset is not representable", impl_->path, context);
        }
        const ssize_t received =
            pread_nointr(impl_->file, output + completed, request, static_cast<off_t>(absolute));
        if (received <= 0) {
            return fail<void>(ErrorCode::io_error, "MK2304_INPUT_READ",
                              "the input file could not be read", impl_->path, context);
        }
        completed += static_cast<std::size_t>(received);
#elif defined(_WIN32)
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset + completed);
        if (!SetFilePointerEx(impl_->file, position, nullptr, FILE_BEGIN)) {
            return fail<void>(ErrorCode::io_error, "MK2304_INPUT_READ",
                              "the input file offset could not be selected", impl_->path, context);
        }
        DWORD received = 0;
        if (!ReadFile(impl_->file, output + completed, static_cast<DWORD>(request), &received,
                      nullptr) ||
            received == 0) {
            return fail<void>(ErrorCode::io_error, "MK2304_INPUT_READ",
                              "the input file could not be read", impl_->path, context);
        }
        completed += received;
#else
        impl_->stream.seekg(static_cast<std::streamoff>(offset + completed), std::ios::beg);
        impl_->stream.read(reinterpret_cast<char*>(output + completed),
                           static_cast<std::streamsize>(request));
        if (impl_->stream.gcount() != static_cast<std::streamsize>(request)) {
            return fail<void>(ErrorCode::io_error, "MK2304_INPUT_READ",
                              "the input file could not be read", impl_->path, context);
        }
        completed += request;
#endif
    }

#if defined(__unix__) || defined(__APPLE__)
    struct stat current{};
    if (fstat_nointr(impl_->file, &current) != 0 || !same_file_state(impl_->state, current)) {
        return fail<void>(ErrorCode::io_error, "MK2305_INPUT_CHANGED",
                          "the input file changed while Melkor read it", impl_->path, context);
    }
#elif defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION current{};
    if (!GetFileInformationByHandle(impl_->file, &current) ||
        !same_file_state(impl_->state, current)) {
        return fail<void>(ErrorCode::io_error, "MK2305_INPUT_CHANGED",
                          "the input file changed while Melkor read it", impl_->path, context);
    }
#else
    std::error_code error;
    const std::uintmax_t current_size = fs::file_size(impl_->path, error);
    const fs::file_time_type current_modified = fs::last_write_time(impl_->path, error);
    if (error || current_size != impl_->size || current_modified != impl_->modified) {
        return fail<void>(ErrorCode::io_error, "MK2305_INPUT_CHANGED",
                          "the input file changed while Melkor read it", impl_->path, context);
    }
#endif

    auto completed_control =
        context.checkpoint({operation, phase, static_cast<std::uint64_t>(amount),
                            static_cast<std::uint64_t>(amount), "bytes"});
    if (!completed_control.has_value())
        return completed_control;
    return Result<void>::success();
} catch (const std::bad_alloc&) {
    return fail<void>(ErrorCode::resource_limit, "MK2303_INPUT_SIZE",
                      "the input read exhausted available memory",
                      impl_ == nullptr ? fs::path{} : impl_->path, context);
} catch (const std::length_error&) {
    return fail<void>(ErrorCode::resource_limit, "MK2303_INPUT_SIZE",
                      "the input read exceeded a container limit",
                      impl_ == nullptr ? fs::path{} : impl_->path, context);
} catch (const fs::filesystem_error&) {
    return fail<void>(ErrorCode::io_error, "MK2304_INPUT_READ", "the input file could not be read",
                      impl_ == nullptr ? fs::path{} : impl_->path, context);
}

}  // namespace melkor::io
