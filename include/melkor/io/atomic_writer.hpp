// Atomic file output.
//
// The rule, and the reason this class exists:
//
//     **A failed write must never damage the file that was already there.**
//
// Before this, it did. The SPZ writer opened the destination with `std::ios::trunc` --
// destroying the user's existing file the instant it began -- and then called
// `std::remove(filepath)` from its error handlers. A conversion that ran out of memory
// halfway through would truncate your good `scene.spz` and then delete it. The user is left
// with nothing: not the new file, and not the old one either. That is release blocker P0-08.
//
// Every output in Melkor -- PLY, SPZ, glTF, JSON reports, run manifests -- goes through this
// one implementation. Format-specific copies of "open the file and hope" are exactly how the
// bug happened once and would happen again.
//
// The algorithm:
//
//   1. Validate the parent directory and the overwrite policy *before* touching anything.
//   2. Refuse a destination that is a directory.
//   3. Refuse to follow a symlink at the destination, unless explicitly allowed. Otherwise a
//      symlink planted at the output path redirects the write to somewhere the user never
//      named.
//   4. Create an unpredictably-named temporary in the *same directory*, with O_EXCL.
//   5. Write, flush, and (for full durability) fsync it.
//   6. Only then, atomically replace the destination.
//   7. On any failure at any step, remove only the temporary. The destination is untouched
//      because it was never opened.
//
// The temporary must be in the same directory as the destination, not in /tmp: `rename()` is
// only atomic within a single filesystem, and /tmp is frequently a different one. A
// cross-device rename silently degrades into copy-then-delete, which is not atomic and
// reintroduces the window this class exists to close.

#ifndef MELKOR_IO_ATOMIC_WRITER_HPP
#define MELKOR_IO_ATOMIC_WRITER_HPP

#include "melkor/budget.hpp"
#include "melkor/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ostream>
#include <vector>

namespace melkor::io {

enum class Durability : std::uint8_t {
    // Contents reach the OS; the rename is atomic with respect to other processes. A power
    // failure may still lose the write. This is the right default for ordinary output: fsync
    // is expensive and most users are not writing a database.
    metadata,

    // fsync the file before rename, and fsync the parent directory after. Survives power
    // loss. Used by release artifacts and pipeline manifests, where losing the record of what
    // happened is worse than the write being slow.
    full,
};

struct WriteOptions {
    // Refuse to replace an existing file unless the caller explicitly opts in. Overwriting by
    // default is how a mistyped output path destroys someone's afternoon.
    bool overwrite = false;

    Durability durability = Durability::metadata;

    // Following a symlink at the output path means writing wherever it points, which may be
    // somewhere the user never named and may not own. Off by default.
    bool allow_output_symlink = false;
};

// Streams bytes to a temporary and atomically installs it on commit.
//
// If the writer is destroyed without commit() -- because the caller returned early, or an
// exception unwound the stack -- the temporary is removed and the destination is left exactly
// as it was. The safe outcome is the one you get by doing nothing.
class AtomicWriter {
  public:
    // Validates the destination and opens the temporary. Fails without side effects if the
    // destination exists and `overwrite` is false, if it is a directory, or if it is a symlink
    // and symlinks are not allowed.
    static Result<std::unique_ptr<AtomicWriter>> create(const std::filesystem::path& destination,
                                                        const WriteOptions& options,
                                                        const OperationContext& context);

    ~AtomicWriter();

    AtomicWriter(const AtomicWriter&) = delete;
    AtomicWriter& operator=(const AtomicWriter&) = delete;

    // Appends to the temporary. Charges the temp-disk budget, so an output that grows without
    // bound is stopped rather than filling the disk.
    Result<void> write(const void* data, std::size_t size);

    // Flushes, optionally fsyncs, closes, and atomically replaces the destination.
    //
    // After this returns success, the destination is the new file. If it returns failure, the
    // destination is whatever it was before -- unchanged, not truncated, not removed.
    Result<void> commit();

    // Explicitly discards. The destructor does this anyway; calling it makes intent obvious
    // at a call site that is bailing out.
    void abort() noexcept;

    std::uint64_t bytes_written() const noexcept { return bytes_written_; }
    const std::filesystem::path& destination() const noexcept { return destination_; }

    // Where the bytes are going right now. Some third-party encoders can only be handed a
    // filename, not a handle; they get this one, and the result is validated and adopted
    // before commit. Exposed for that case, and for tests.
    const std::filesystem::path& temporary_path() const noexcept { return temporary_; }

  private:
    AtomicWriter() = default;

    std::filesystem::path destination_;
    std::filesystem::path temporary_;

    // The open output handle. Conditional rather than carrying both, because an unused
    // member is a warning under -Werror and because carrying a dead field invites someone to
    // eventually use the wrong one.
#if defined(_WIN32)
    void* handle_ = nullptr;  // Windows HANDLE, as void* to keep this header platform-free.
#else
    int fd_ = -1;  // POSIX file descriptor; -1 when closed.
#endif

    bool committed_ = false;
    std::uint64_t bytes_written_ = 0;
    WriteOptions options_;
    Budget* budget_ = nullptr;
};

// A std::ostream that writes into an AtomicWriter.
//
// Exists so that a writer with a `std::ostream&` interface can gain atomic output without
// first buffering its entire result in memory. Buffering would be the easy migration, but a
// 25-million-splat PLY is several gigabytes, and trading a data-loss bug for an out-of-memory
// bug is not a fix.
//
// Errors surface two ways: the stream's badbit is set (so idiomatic `if (!stream)` checks
// work), and `last_error()` carries the real diagnostic, because a streambuf cannot return a
// Result.
class AtomicOutputStream : public std::ostream {
  public:
    explicit AtomicOutputStream(AtomicWriter& writer);
    ~AtomicOutputStream() override;

    // The first write error, if any. Callers must check this before commit: an ostream
    // swallows failures by design, and committing after a silently failed write would install
    // a truncated file.
    const std::vector<Diagnostic>& diagnostics() const noexcept;
    bool failed() const noexcept;

  private:
    class Buffer;
    std::unique_ptr<Buffer> buffer_;
};

// Are two paths the same file?
//
// Compares file *identity* (device + inode on POSIX, file index on Windows), not the strings.
// `scene.ply` and `./scene.ply` and `../work/scene.ply` and a symlink to any of them are all
// the same file, and a converter that reads and writes the same file simultaneously will
// destroy it. String comparison cannot see that; this can.
Result<bool> is_same_file(const std::filesystem::path& a, const std::filesystem::path& b);

}  // namespace melkor::io

#endif  // MELKOR_IO_ATOMIC_WRITER_HPP
