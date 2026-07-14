// Shared resource accounting, cancellation, and progress.
//
// A limit that each parser checks for itself is a limit that a new parser will forget. The
// `Budget` exists so that resource accounting is something an operation *carries*, not
// something it is trusted to remember: every reader, writer, and algorithm receives an
// `OperationContext`, and consuming an unbudgeted byte is not possible without going around
// the API deliberately.
//
// This is also what makes the limits testable. A test can hand an operation a budget of 100
// bytes and assert that it fails cleanly at exactly the right point, which is far more
// convincing than asserting that a 4 GiB file is rejected -- a test nobody wants to run.
//
// Deliberately **not** a global singleton. A process-wide budget would make tests order-
// dependent and would make it impossible for a server to run two jobs with different limits.

#ifndef MELKOR_BUDGET_HPP
#define MELKOR_BUDGET_HPP

#include "melkor/error.hpp"
#include "melkor/limits.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace melkor {

// What is being consumed. Each maps to a limit in `Limits`.
enum class BudgetKind : std::uint8_t {
    input_bytes,
    resource_bytes,
    decoded_bytes,
    memory_bytes,
    temp_bytes,
    splats,
    mesh_vertices,
    mesh_triangles,
    gltf_nodes,
    accessors,
    external_resources,
    metadata_bytes,
    image_pixels,
};

const char* to_string(BudgetKind kind) noexcept;

// The CLI flag that raises a given budget. Carried in the diagnostic so that a limit failure
// tells the user how to proceed rather than merely that they may not.
const char* override_flag_for(BudgetKind kind) noexcept;

// Thread-safe accounting against a `Limits`.
//
// `consume` is the gate. It must be called *before* the allocation it accounts for, not after
// -- accounting for memory you already allocated does not prevent the OOM.
class Budget {
  public:
    explicit Budget(Limits limits);

    // Charges `amount` against `kind`. Fails with `resource_limit` if that would exceed the
    // configured limit, and the diagnostic names the limit, the observed value, and the flag
    // that raises it.
    //
    // `operation` is a short label ("spz.decode", "ply.header") used only in the diagnostic.
    Result<void> consume(BudgetKind kind, std::uint64_t amount, std::string_view operation);

    // Returns memory to the budget. Only meaningful for `memory_bytes` and `temp_bytes`,
    // which are genuinely reclaimable; consuming 10 million splats and then releasing them
    // does not un-read the file.
    void release(BudgetKind kind, std::uint64_t amount) noexcept;

    std::uint64_t used(BudgetKind kind) const noexcept;
    std::uint64_t remaining(BudgetKind kind) const noexcept;
    const Limits& limits() const noexcept { return limits_; }

    // Checks a compression ratio before committing to a decompression.
    //
    // Called with the compressed size and the size the container *claims* it will expand to,
    // so that a bomb is refused before any of it is inflated.
    Result<void> check_decompression_ratio(std::uint64_t compressed_bytes,
                                           std::uint64_t declared_decoded_bytes,
                                           std::string_view operation) const;

  private:
    std::uint64_t limit_for(BudgetKind kind) const noexcept;

    Limits limits_;
    mutable std::mutex mutex_;
    std::uint64_t used_[16] = {};
};

// Cooperative cancellation.
//
// Cancellation is checked at bounded intervals inside long loops -- not once per file, which
// would make Ctrl-C useless on exactly the inputs where a user most wants it. The target is a
// cancellation latency under ~100 ms for CPU parsing and conversion.
//
// Shared state, because the token is observed on a worker thread and set from a signal
// handler or another thread.
class CancellationToken {
  public:
    CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() noexcept { flag_->store(true, std::memory_order_relaxed); }
    bool is_cancelled() const noexcept { return flag_->load(std::memory_order_relaxed); }

    // Convenience for the inside of a loop: `MELKOR_TRY(context.cancellation.check());`
    Result<void> check() const;

  private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// A structured progress event.
//
// Never emitted per splat. A ten-million-splat file would produce ten million events, which
// costs more than the work being reported on. Sinks throttle by time or percentage.
struct ProgressEvent {
    std::string operation;  // "spz.decode"
    std::string phase;      // "spherical_harmonics"
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::string unit;  // "splats"
};

class ProgressSink {
  public:
    virtual ~ProgressSink() = default;
    virtual void on_progress(const ProgressEvent& event) = 0;
};

// Everything a fallible, bounded, cancellable operation needs.
//
// Passed by reference to every reader, writer, and algorithm. An operation that does not take
// one cannot participate in resource accounting or cancellation, which is the point: the type
// system makes the safe thing the default thing.
struct OperationContext {
    Budget* budget = nullptr;              // Never null in production. Tests may pass a tiny one.
    CancellationToken cancellation;
    ProgressSink* progress = nullptr;      // Optional.
    DiagnosticPathPolicy path_policy = DiagnosticPathPolicy::basename;
    std::string path_root;                 // For DiagnosticPathPolicy::relative.

    // Charges the budget, or fails. Shorthand so call sites stay readable.
    Result<void> consume(BudgetKind kind, std::uint64_t amount, std::string_view operation) const;

    // Reports progress, if a sink is attached. Safe to call with no sink.
    void report(const ProgressEvent& event) const;
};

// Builds a context with a `desktop` budget. For tests and simple callers.
//
// The budget is owned by the caller: an OperationContext holds a non-owning pointer, because
// a long-running server wants one budget shared across the stages of one job.
OperationContext make_default_context(Budget& budget);

}  // namespace melkor

#endif  // MELKOR_BUDGET_HPP
