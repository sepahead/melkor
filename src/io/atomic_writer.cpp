#include "melkor/io/atomic_writer.hpp"

#include "melkor/checked.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <random>
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

Diagnostic io_error(const std::string& code, const std::string& message,
                    const fs::path& path, const OperationContext& context) {
    Diagnostic diagnostic(code, Severity::error, message);
    diagnostic.with_path(redact_path(path.string(), context.path_policy, context.path_root));
    return diagnostic;
}

Diagnostic errno_diagnostic(const std::string& code, const std::string& message,
                            const fs::path& path, const OperationContext& context, int error) {
    Diagnostic diagnostic = io_error(code, message, path, context);
    diagnostic.with_context("errno", static_cast<std::int64_t>(error));
    diagnostic.with_context("system_error", std::string(std::strerror(error)));
    return diagnostic;
}

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

}  // namespace

Result<std::unique_ptr<AtomicWriter>> AtomicWriter::create(const fs::path& destination,
                                                           const WriteOptions& options,
                                                           const OperationContext& context) {
    using ResultT = Result<std::unique_ptr<AtomicWriter>>;

    std::error_code ec;

    // ---- Validate the destination BEFORE creating anything -----------------------------
    //
    // Everything in this block must fail without side effects. A validation failure that has
    // already truncated the destination is the bug we are here to remove.

    const fs::path parent = destination.has_parent_path() ? destination.parent_path() : fs::path(".");

    if (!fs::exists(parent, ec) || ec) {
        return ResultT::failure(
            ErrorCode::io_error,
            io_error("MK0501_OUTPUT_PARENT_MISSING",
                     "the output directory does not exist", parent, context));
    }
    if (!fs::is_directory(parent, ec)) {
        return ResultT::failure(
            ErrorCode::io_error,
            io_error("MK0502_OUTPUT_PARENT_NOT_DIRECTORY",
                     "the output path's parent is not a directory", parent, context));
    }

    const bool destination_exists = fs::exists(fs::symlink_status(destination, ec));

    if (destination_exists) {
        // A symlink at the output path redirects the write to wherever it points -- possibly
        // outside the directory the user named, possibly to a file they do not own. Refuse by
        // default rather than silently following it.
        if (fs::is_symlink(fs::symlink_status(destination, ec)) && !options.allow_output_symlink) {
            return ResultT::failure(
                ErrorCode::io_error,
                io_error("MK0503_OUTPUT_IS_SYMLINK",
                         "the output path is a symbolic link; refusing to follow it. Pass "
                         "--allow-output-symlink only if you intend to write through it.",
                         destination, context));
        }

        if (fs::is_directory(destination, ec)) {
            return ResultT::failure(
                ErrorCode::io_error,
                io_error("MK0504_OUTPUT_IS_DIRECTORY",
                         "the output path is a directory", destination, context));
        }

        if (!options.overwrite) {
            return ResultT::failure(
                ErrorCode::io_error,
                io_error("MK0505_OUTPUT_EXISTS",
                         "the output file already exists. Pass --force to replace it.",
                         destination, context));
        }
    }

    // ---- Create the temporary, in the SAME directory ------------------------------------
    //
    // Same directory, not /tmp: rename() is only atomic within one filesystem. A temporary on
    // a different device turns the rename into copy-then-delete, which is not atomic and
    // reopens the window this class exists to close.

    auto writer = std::unique_ptr<AtomicWriter>(new AtomicWriter());
    writer->destination_ = destination;
    writer->options_ = options;
    writer->budget_ = context.budget;

    std::string base = destination.filename().string();
#if !defined(_WIN32)
    // The candidate temp name adds ~29 characters around `base` ("." prefix + ".melkor-" + a
    // 16-char random suffix + ".tmp"). A destination filename near NAME_MAX would push the temp
    // name past it, so open() would reject a perfectly legal output path with ENAMETOOLONG. Clamp
    // the embedded base (the real destination is unaffected -- only the temporary's name is).
    constexpr std::size_t kNameMax = 255;      // POSIX minimum guaranteed NAME_MAX
    constexpr std::size_t kTempOverhead = 29;  // "." + ".melkor-" + 16-char suffix + ".tmp"
    if (base.size() + kTempOverhead > kNameMax) {
        base.resize(kNameMax - kTempOverhead);
    }
#endif

    // A handful of attempts, in case an unlikely name collision occurs. Not a loop forever:
    // if O_EXCL keeps failing on random 16-character names, something is wrong that retrying
    // will not fix.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const fs::path candidate = parent / ("." + base + ".melkor-" + random_suffix() + ".tmp");

#if defined(_WIN32)
        // CREATE_NEW is the Windows equivalent of O_EXCL: it fails if the file exists.
        // FILE_FLAG_OPEN_REPARSE_POINT ensures we do not traverse a reparse point that was
        // planted at the temporary's path.
        HANDLE handle = ::CreateFileW(
            candidate.wstring().c_str(), GENERIC_WRITE, 0 /* no sharing */, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            writer->handle_ = handle;
            writer->temporary_ = candidate;
            return ResultT::success(std::move(writer));
        }
        if (::GetLastError() != ERROR_FILE_EXISTS) {
            Diagnostic diagnostic = io_error("MK0506_TEMP_CREATE_FAILED",
                                             "could not create the temporary output file",
                                             candidate, context);
            diagnostic.with_context("windows_error",
                                    static_cast<std::int64_t>(::GetLastError()));
            return ResultT::failure(ErrorCode::io_error, std::move(diagnostic));
        }
#else
        // O_EXCL | O_CREAT is atomic: it fails if the path exists, which defeats a symlink
        // planted at the temporary's location. O_NOFOLLOW is belt-and-braces on the same idea.
        //
        // Mode 0600, not 0644: a temporary that is briefly world-readable leaks the contents
        // of the file being written, and the final permissions are set at commit.
        const int fd = ::open(candidate.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            writer->fd_ = fd;
            writer->temporary_ = candidate;
            return ResultT::success(std::move(writer));
        }
        if (errno != EEXIST) {
            return ResultT::failure(
                ErrorCode::io_error,
                errno_diagnostic("MK0506_TEMP_CREATE_FAILED",
                                 "could not create the temporary output file", candidate,
                                 context, errno));
        }
#endif
    }

    return ResultT::failure(
        ErrorCode::io_error,
        io_error("MK0507_TEMP_NAME_COLLISION",
                 "could not find an unused temporary filename after several attempts",
                 destination, context));
}

AtomicWriter::~AtomicWriter() {
    // The safe outcome is the one you get by doing nothing. If the caller returned early, or
    // an exception unwound past them, the temporary is removed and the destination is left
    // exactly as it was.
    if (!committed_) {
        abort();
    }
}

Result<void> AtomicWriter::write(const void* data, std::size_t size) {
    if (committed_) {
        return Result<void>::failure(
            ErrorCode::internal_error,
            Diagnostic("MK0508_WRITE_AFTER_COMMIT", Severity::error,
                       "write() called after commit()"));
    }

    if (size == 0) {
        return Result<void>::success();
    }

    // Charge the temp-disk budget before writing, so an output that grows without bound is
    // stopped rather than filling the disk. After the write would be too late.
    if (budget_ != nullptr) {
        auto charged = budget_->consume(BudgetKind::temp_bytes, size, "atomic_writer.write");
        if (!charged.has_value()) {
            return charged;
        }
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;

    while (remaining > 0) {
#if defined(_WIN32)
        DWORD chunk = static_cast<DWORD>(
            remaining > 0x7FFFFFFFu ? 0x7FFFFFFFu : remaining);
        DWORD written = 0;
        if (!::WriteFile(static_cast<HANDLE>(handle_), bytes, chunk, &written, nullptr)) {
            Diagnostic diagnostic("MK0509_WRITE_FAILED", Severity::error,
                                  "failed writing to the temporary output file");
            diagnostic.with_context("windows_error", static_cast<std::int64_t>(::GetLastError()));
            return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
        }
        const std::size_t n = written;
#else
        const ssize_t n = ::write(fd_, bytes, remaining);
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
            return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
        }
#endif
        if (n == 0) {
            return Result<void>::failure(
                ErrorCode::io_error,
                Diagnostic("MK0509_WRITE_FAILED", Severity::error,
                           "the filesystem accepted zero bytes; the disk may be full"));
        }

        bytes += n;
        remaining -= static_cast<std::size_t>(n);
        bytes_written_ += static_cast<std::uint64_t>(n);
    }

    return Result<void>::success();
}

Result<void> AtomicWriter::commit() {
    if (committed_) {
        return Result<void>::failure(
            ErrorCode::internal_error,
            Diagnostic("MK0510_DOUBLE_COMMIT", Severity::error, "commit() called twice"));
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

    if (!::CloseHandle(handle)) {
        Diagnostic diagnostic("MK0512_CLOSE_FAILED", Severity::error,
                              "failed closing the temporary output file");
        diagnostic.with_context("windows_error", static_cast<std::int64_t>(::GetLastError()));
        handle_ = nullptr;
        abort();
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }
    handle_ = nullptr;

    // MoveFileExW with MOVEFILE_REPLACE_EXISTING is the atomic replace. Windows also offers
    // ReplaceFileW, which preserves the destination's ACLs and attributes; that is desirable
    // for in-place edits of an existing document, but here the new file is the authority and
    // the simpler primitive has fewer failure modes.
    //
    // Antivirus and search indexers routinely hold a transient handle on a just-written file,
    // which makes the replace fail with a sharing violation. Retrying briefly is the
    // difference between "works" and "randomly fails on Windows".
    const int kMaxAttempts = 10;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (::MoveFileExW(temporary_.wstring().c_str(), destination_.wstring().c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            committed_ = true;
            return Result<void>::success();
        }
        const DWORD error = ::GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            break;
        }
        ::Sleep(20 * (attempt + 1));  // Deterministic backoff; no jitter, so tests are stable.
    }

    Diagnostic diagnostic("MK0513_REPLACE_FAILED", Severity::error,
                          "failed atomically replacing the destination. The existing file is "
                          "unchanged.");
    diagnostic.with_context("windows_error", static_cast<std::int64_t>(::GetLastError()));
    abort();
    return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));

#else
    if (options_.durability == Durability::full) {
        // fsync before rename. Without it, a power failure can leave the rename durable but
        // the *contents* not -- the destination then exists, has the right name, and is empty
        // or garbage. That is worse than either outcome alone.
        if (::fsync(fd_) != 0) {
            Diagnostic diagnostic("MK0511_FLUSH_FAILED", Severity::error,
                                  "failed flushing the temporary output file to storage");
            diagnostic.with_context("errno", static_cast<std::int64_t>(errno));
            abort();
            return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
        }
    }

    // Set the final permissions on the open descriptor, before close and before rename.
    //
    // The temporary was created 0600 so its contents were never briefly world-readable during
    // the write. The committed file, however, should have the permissions a normally created
    // file gets -- which means honouring the process umask, not forcing a fixed mode.
    //
    // A previous version called chmod(path, 0644), which is wrong twice over: chmod ignores the
    // umask entirely (only file-creating syscalls apply it), so a user running umask 0077 -- who
    // expects their output private -- got a world-readable 0644 file, a local information leak
    // on a shared host. And chmod-by-path after close is a TOCTOU window. Replicate exactly what
    // open(path, O_CREAT, 0666) would have produced: 0666 masked by the umask, applied through
    // the fd we still hold.
    const mode_t current_umask = ::umask(0);  // read-and-clear
    ::umask(current_umask);                    // restore immediately
    const mode_t final_mode = static_cast<mode_t>(0666) & ~current_umask;
    ::fchmod(fd_, final_mode);

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

    // The atomic step. rename(2) is guaranteed atomic within a filesystem: another process
    // sees either the old file or the new one, never a partial one, and never nothing.
    if (::rename(temporary_.c_str(), destination_.c_str()) != 0) {
        Diagnostic diagnostic("MK0513_REPLACE_FAILED", Severity::error,
                              "failed atomically replacing the destination. The existing file "
                              "is unchanged.");
        diagnostic.with_context("errno", static_cast<std::int64_t>(errno));
        diagnostic.with_context("system_error", std::string(std::strerror(errno)));
        abort();
        return Result<void>::failure(ErrorCode::io_error, std::move(diagnostic));
    }

    if (options_.durability == Durability::full) {
        // fsync the *directory*, not the file: the rename is a directory operation, and
        // without this the rename itself can be lost in a power failure even though the file
        // contents are durable.
        const fs::path parent =
            destination_.has_parent_path() ? destination_.parent_path() : fs::path(".");
        const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }
        // A failure to open the directory is not fatal: the data is already committed and
        // visible. Only the durability guarantee is weakened, and reporting an error here
        // would suggest the write failed when it did not.
    }

    committed_ = true;
    return Result<void>::success();
#endif
}

void AtomicWriter::abort() noexcept {
#if defined(_WIN32)
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif

    // Remove ONLY the temporary. The destination is not touched -- it was never opened, never
    // truncated, and is not removed here. This is the entire point of the class.
    if (!temporary_.empty()) {
        std::error_code ec;
        fs::remove(temporary_, ec);  // Best effort; a leftover temp is untidy, not dangerous.
    }
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
    explicit Buffer(AtomicWriter& writer) : writer_(writer), storage_(kBufferSize) {
        setp(storage_.data(), storage_.data() + storage_.size());
    }

    ~Buffer() override {
        // Not flushed here: a destructor cannot report failure, and silently dropping the
        // tail of a file is precisely the class of bug this whole subsystem exists to remove.
        // AtomicOutputStream's destructor flushes explicitly and records any error.
    }

    bool failed() const noexcept { return failed_; }
    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

    // Pushes whatever is buffered into the writer. Returns false on failure.
    bool flush_to_writer() {
        const std::ptrdiff_t pending = pptr() - pbase();
        if (pending <= 0) {
            return !failed_;
        }

        auto result = writer_.write(pbase(), static_cast<std::size_t>(pending));
        setp(storage_.data(), storage_.data() + storage_.size());

        if (!result.has_value()) {
            failed_ = true;
            diagnostics_ = result.diagnostics();
            return false;
        }
        return true;
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
        return ch;
    }

    int sync() override { return flush_to_writer() ? 0 : -1; }

    // Bulk writes bypass the buffer entirely when they are large. Copying a multi-megabyte
    // binary block through a 64 KiB buffer would be pure overhead.
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (failed_) {
            return 0;
        }

        if (count >= static_cast<std::streamsize>(kBufferSize)) {
            if (!flush_to_writer()) {
                return 0;
            }
            auto result = writer_.write(data, static_cast<std::size_t>(count));
            if (!result.has_value()) {
                failed_ = true;
                diagnostics_ = result.diagnostics();
                return 0;
            }
            return count;
        }

        return std::streambuf::xsputn(data, count);
    }

  private:
    static constexpr std::size_t kBufferSize = 64 * 1024;

    AtomicWriter& writer_;
    std::vector<char> storage_;
    bool failed_ = false;
    std::vector<Diagnostic> diagnostics_;
};

AtomicOutputStream::AtomicOutputStream(AtomicWriter& writer)
    : std::ostream(nullptr), buffer_(std::make_unique<Buffer>(writer)) {
    rdbuf(buffer_.get());
}

AtomicOutputStream::~AtomicOutputStream() {
    // Flush the tail. Any error is recorded rather than thrown, and the caller is expected to
    // check failed() before commit() -- committing after a silently failed write would install
    // a truncated file, which is exactly the outcome this subsystem prevents elsewhere.
    if (buffer_) {
        if (!buffer_->flush_to_writer()) {
            setstate(std::ios::badbit);
        }
    }
}

const std::vector<Diagnostic>& AtomicOutputStream::diagnostics() const noexcept {
    return buffer_->diagnostics();
}

bool AtomicOutputStream::failed() const noexcept {
    return buffer_->failed() || bad();
}

Result<bool> is_same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;

    // fs::equivalent compares file identity -- device and inode on POSIX, the file index on
    // Windows -- rather than comparing the path strings. That is the only correct way to ask
    // this: "scene.ply", "./scene.ply", "../work/scene.ply", and a symlink to any of them are
    // all the same file, and a converter that reads and writes it at once will destroy it.
    const bool same = fs::equivalent(a, b, ec);

    if (ec) {
        // One of them does not exist. That is not an error for this question: if the output
        // does not exist yet, it certainly is not the input.
        return Result<bool>::success(false);
    }
    return Result<bool>::success(same);
}

}  // namespace melkor::io
