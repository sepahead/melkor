// Error, diagnostic, and result types.
//
// Melkor's core does not use exceptions for error handling, and it never returns a bare
// `bool` plus a free-form English string. Both are inadequate for the job:
//
//   - A `bool` cannot tell a caller whether the input was malformed, whether a resource
//     limit was hit, or whether the user cancelled. Those need different responses, and a
//     script needs to distinguish them without parsing prose.
//   - An English message is not a contract. The moment someone greps stderr for "invalid",
//     the wording becomes an API that nobody knows they must not change.
//
// Instead every fallible operation returns `Result<T>`, carrying a coarse `ErrorCode` for
// control flow and a list of `Diagnostic`s with *stable codes* for reporting. The codes are
// the machine contract; the messages are for humans and may be reworded freely.
//
// This header is platform-neutral, allocates only through the standard library, and does not
// depend on any other Melkor header.

#ifndef MELKOR_ERROR_HPP
#define MELKOR_ERROR_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace melkor {

// Coarse error classes. These map one-to-one onto CLI exit codes, so a script can branch on
// what *kind* of thing went wrong without understanding the specific diagnostic.
//
// Keep this list short. Fine-grained detail belongs in the diagnostic code, not here: adding
// an ErrorCode changes the exit-code contract, whereas adding a diagnostic does not.
enum class ErrorCode : std::uint32_t {
    ok = 0,

    // The caller asked for something incoherent: a bad flag, an out-of-range option, two
    // mutually exclusive settings. The input file is not at fault. -> exit 2
    invalid_argument = 2,

    // The data is malformed, or it violates a canonical invariant. This is the file's fault,
    // not the caller's. -> exit 3
    invalid_data = 3,

    // The operation is well-formed but Melkor cannot represent or perform it: an unsupported
    // format profile, a required extension we do not implement, or a severe conversion loss
    // the user did not approve. -> exit 4
    unsupported_feature = 4,

    // Open, read, write, flush, rename, permission, or disk failure. -> exit 5
    io_error = 5,

    // A byte, count, memory, time, decompression, thread, or GPU budget was exceeded.
    // Deliberately distinct from invalid_data: the file may be perfectly valid and simply
    // too large for the configured profile, and the remedy is a limits flag, not a new file.
    // -> exit 6
    resource_limit = 6,

    // The requested backend is unavailable, or a qualified backend operation failed.
    // -> exit 7
    backend_unavailable = 7,

    // The user cancelled. Not a failure of the software. -> exit 130
    cancelled = 8,

    // An invariant Melkor guarantees was violated. This is always a bug in Melkor, never the
    // user's fault, and it should be reported as one. -> exit 8
    internal_error = 9,
};

const char* to_string(ErrorCode code) noexcept;

// Maps an error class to the CLI's documented exit code. Defined here, next to the enum, so
// the two cannot drift apart.
int exit_code_for(ErrorCode code) noexcept;

enum class Severity : std::uint8_t {
    note = 0,     // Informational. Does not affect the outcome.
    warning = 1,  // Something is suspicious or was adjusted. The operation continues.
    error = 2,    // The operation cannot complete correctly.
};

const char* to_string(Severity severity) noexcept;

// A JSON-representable scalar, for attaching structured context to a diagnostic.
//
// Context is what makes a diagnostic actionable. "Count limit exceeded" tells a user nothing;
// "observed 40000000, limit 25000000, override --max-splats" tells them exactly what to do.
using JsonScalar = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double,
                                std::string>;

// One thing that went wrong, or one thing worth telling the user about.
//
// `code` is the stable identifier and the machine contract. It is never recycled to mean
// something different: a consumer that special-cases MK1203_PLY_RGB_RANGE must be able to
// rely on it meaning the same thing in every 2.x release.
struct Diagnostic {
    std::string code;
    Severity severity = Severity::error;
    std::string message;

    // What the diagnostic is about. A display name by default, not an absolute path -- a
    // report should be safe to paste into a public bug tracker without leaking a home
    // directory. See `DiagnosticPathPolicy`.
    std::string path;

    // Where in the file, when that is knowable. A user chasing a malformed record needs the
    // offset far more than they need the prose.
    std::optional<std::uint64_t> byte_offset;

    // Structured detail: observed value, permitted value, the flag that overrides it.
    std::map<std::string, JsonScalar> context;

    Diagnostic() = default;
    Diagnostic(std::string code_, Severity severity_, std::string message_)
        : code(std::move(code_)), severity(severity_), message(std::move(message_)) {}

    // Builder-style helpers. These exist so that adding context at the call site is cheap
    // enough that people actually do it, rather than dropping the detail and leaving the
    // user with an unactionable message.
    Diagnostic& with_path(std::string value) &;
    Diagnostic& with_offset(std::uint64_t value) &;
    Diagnostic& with_context(std::string key, JsonScalar value) &;
};

// How much of a filesystem path may appear in a diagnostic.
//
// The default is `basename`, because inspection reports are routinely pasted into public
// issues, and an absolute path leaks a username and a directory layout. Full paths are
// available, but only when the caller asks for them.
enum class DiagnosticPathPolicy : std::uint8_t {
    basename = 0,  // "scene.ply"
    relative = 1,  // "assets/scene.ply", relative to a caller-supplied root
    full = 2,      // "/home/alice/work/assets/scene.ply"
};

// Applies the path policy. Callers should route every path through this rather than
// formatting one into a message directly.
std::string redact_path(const std::string& path, DiagnosticPathPolicy policy,
                        const std::string& root = {});

// The outcome of a fallible operation.
//
// A `Result` is either a value or an error class, and it carries diagnostics in *both* cases.
// That matters: a successful operation can still have warnings worth surfacing (a quaternion
// was renormalized, an unknown property was skipped), and dropping them because the operation
// "worked" is how silent data corruption gets shipped.
//
// There is no implicit conversion to bool. `if (result)` reads as "if it worked", which is
// exactly the reading that leads to ignoring an error; `result.has_value()` does not.
template <class T>
class Result {
  public:
    static Result success(T value, std::vector<Diagnostic> diagnostics = {}) {
        Result result;
        result.value_ = std::move(value);
        result.code_ = ErrorCode::ok;
        result.diagnostics_ = std::move(diagnostics);
        return result;
    }

    static Result failure(ErrorCode code, std::vector<Diagnostic> diagnostics) {
        Result result;
        result.code_ = code;
        result.diagnostics_ = std::move(diagnostics);
        return result;
    }

    static Result failure(ErrorCode code, Diagnostic diagnostic) {
        return failure(code, std::vector<Diagnostic>{std::move(diagnostic)});
    }

    bool has_value() const noexcept { return code_ == ErrorCode::ok; }
    ErrorCode error_code() const noexcept { return code_; }

    // Precondition: has_value(). Calling this on a failed Result is a programming error.
    const T& value() const& { return *value_; }
    T& value() & { return *value_; }
    T&& value() && { return std::move(*value_); }

    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }
    std::vector<Diagnostic>& diagnostics() noexcept { return diagnostics_; }

    // Carries diagnostics forward when propagating a failure through a layer that adds its
    // own context. Without this, the innermost cause is routinely lost.
    void add_diagnostics(std::vector<Diagnostic> more) {
        diagnostics_.insert(diagnostics_.end(), std::make_move_iterator(more.begin()),
                            std::make_move_iterator(more.end()));
    }

    bool has_errors() const noexcept {
        for (const auto& diagnostic : diagnostics_) {
            if (diagnostic.severity == Severity::error) {
                return true;
            }
        }
        return false;
    }

  private:
    Result() = default;

    std::optional<T> value_;
    ErrorCode code_ = ErrorCode::internal_error;
    std::vector<Diagnostic> diagnostics_;
};

// `Result<void>`, for operations that can fail but produce nothing.
template <>
class Result<void> {
  public:
    static Result success(std::vector<Diagnostic> diagnostics = {}) {
        Result result;
        result.code_ = ErrorCode::ok;
        result.diagnostics_ = std::move(diagnostics);
        return result;
    }

    static Result failure(ErrorCode code, std::vector<Diagnostic> diagnostics) {
        Result result;
        result.code_ = code;
        result.diagnostics_ = std::move(diagnostics);
        return result;
    }

    static Result failure(ErrorCode code, Diagnostic diagnostic) {
        return failure(code, std::vector<Diagnostic>{std::move(diagnostic)});
    }

    bool has_value() const noexcept { return code_ == ErrorCode::ok; }
    ErrorCode error_code() const noexcept { return code_; }

    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }
    std::vector<Diagnostic>& diagnostics() noexcept { return diagnostics_; }

    void add_diagnostics(std::vector<Diagnostic> more) {
        diagnostics_.insert(diagnostics_.end(), std::make_move_iterator(more.begin()),
                            std::make_move_iterator(more.end()));
    }

    bool has_errors() const noexcept {
        for (const auto& diagnostic : diagnostics_) {
            if (diagnostic.severity == Severity::error) {
                return true;
            }
        }
        return false;
    }

  private:
    Result() = default;

    ErrorCode code_ = ErrorCode::internal_error;
    std::vector<Diagnostic> diagnostics_;
};

// Propagates a failed Result out of the current function, preserving its diagnostics.
//
// Written as a macro because C++17 has no `?` operator and the alternative -- an explicit
// `if (!r.has_value()) return Result<X>::failure(r.error_code(), r.diagnostics());` at every
// call site -- is verbose enough that people start skipping the check.
#define MELKOR_TRY(result_expr)                                                       \
    do {                                                                              \
        auto&& melkor_try_result = (result_expr);                                     \
        if (!melkor_try_result.has_value()) {                                         \
            return ::melkor::Result<void>::failure(melkor_try_result.error_code(),    \
                                                   melkor_try_result.diagnostics());  \
        }                                                                             \
    } while (false)

// As MELKOR_TRY, but for a function whose own return type is `Result<U>`.
#define MELKOR_TRY_AS(ReturnType, result_expr)                                        \
    do {                                                                              \
        auto&& melkor_try_result = (result_expr);                                     \
        if (!melkor_try_result.has_value()) {                                         \
            return ::melkor::Result<ReturnType>::failure(melkor_try_result.error_code(), \
                                                         melkor_try_result.diagnostics()); \
        }                                                                             \
    } while (false)

}  // namespace melkor

#endif  // MELKOR_ERROR_HPP
