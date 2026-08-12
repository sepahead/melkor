// Shared resource accounting, cancellation, and progress.
//
// A limit that each parser checks for itself is a limit that a new parser will forget. The
// `Budget` provides one resource counter for an operation.
// Readers and writers use it for the resource classes that they currently account for.
// `OperationContext` also provides cancellation and progress for APIs that accept it.
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

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
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
    gltf_nodes,
    accessors,
    external_resources,
    count,
};

const char* to_string(BudgetKind kind) noexcept;

// Return a supported CLI hint when a larger named profile exists.
std::string override_flag_for(BudgetKind kind, const Limits& current);

// Thread-safe accounting against a `Limits`.
//
// `consume` is the gate. It must be called *before* the allocation it accounts for, not after
// -- accounting for memory you already allocated does not prevent the OOM.
class Budget {
private:
    struct SharedState;

public:
    // Owns one previously consumed reclaimable charge.
    // The charge can outlive the Budget object that created it.
    class Charge {
    public:
        Charge() noexcept = default;
        ~Charge();

        Charge(const Charge&) = delete;
        Charge& operator=(const Charge&) = delete;
        Charge(Charge&& other) noexcept;
        Charge& operator=(Charge&& other) noexcept;

        void reset() noexcept;
        void shrink_to(std::uint64_t amount) noexcept;
        std::uint64_t amount() const noexcept { return amount_; }

    private:
        friend class Budget;
        Charge(std::shared_ptr<SharedState> state, BudgetKind kind, std::uint64_t amount) noexcept;

        std::shared_ptr<SharedState> state_;
        BudgetKind kind_ = BudgetKind::count;
        std::uint64_t amount_ = 0;
    };

    explicit Budget(Limits limits);
    Budget(const Budget&) = delete;
    Budget& operator=(const Budget&) = delete;
    Budget(Budget&&) = delete;
    Budget& operator=(Budget&&) = delete;

    // Charges `amount` against `kind`. Fails with `resource_limit` if that would exceed the
    // configured limit, and the diagnostic names the limit, the observed value, and the flag
    // that raises it.
    //
    // `operation` is a short label ("spz.decode", "ply.header") used only in the diagnostic.
    Result<void> consume(BudgetKind kind, std::uint64_t amount, std::string_view operation);

    // Reserve reclaimable storage and return its owner in one atomic operation.
    // Valid kinds are memory_bytes and temp_bytes.
    Result<Charge> reserve(BudgetKind kind, std::uint64_t amount, std::string_view operation);

    // Raise an asset-cardinality counter to at least `amount`.
    // Valid kinds are splats, glTF nodes, and accessors.
    Result<void> observe(BudgetKind kind, std::uint64_t amount, std::string_view operation);

    // Returns memory to the budget. Only meaningful for `memory_bytes` and `temp_bytes`,
    // which are genuinely reclaimable; consuming 10 million splats and then releasing them
    // does not un-read the file.
    void release(BudgetKind kind, std::uint64_t amount) noexcept;

    // Claim responsibility for a reclaimable charge that consume() already accepted.
    // The call fails if another owner already claimed the bytes.
    Result<Charge> adopt_charge(BudgetKind kind, std::uint64_t amount, std::string_view operation);

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
    static void release_shared(const std::shared_ptr<SharedState>& state, BudgetKind kind,
                               std::uint64_t amount) noexcept;
    static void release_unowned(const std::shared_ptr<SharedState>& state, BudgetKind kind,
                                std::uint64_t amount) noexcept;

    Limits limits_;
    std::shared_ptr<SharedState> state_;
};

// Cooperative cancellation.
//
// Cancellation is checked at bounded intervals inside long loops -- not once per file, which
// would make Ctrl-C useless on exactly the inputs where a user most wants it. The target is a
// cancellation latency under ~100 ms for CPU parsing and conversion.
//
// Shared state lets one thread request cancellation while another thread runs the operation.
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

// A monotonic deadline for one operation.
//
// A steady clock prevents wall-clock changes from extending or shortening the deadline.
class Deadline {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    Deadline() noexcept = default;

    // Create a deadline relative to the current time. A zero timeout disables the deadline.
    static Deadline after(std::uint64_t timeout_ms) noexcept;

    // Create an absolute deadline. This function supports schedulers and deterministic tests.
    static Deadline at(TimePoint expires_at) noexcept;

    bool is_set() const noexcept { return is_set_; }
    bool is_expired() const noexcept;
    Result<void> check(std::string_view operation) const;

private:
    TimePoint expires_at_{};
    std::uint64_t timeout_ms_ = 0;
    bool is_set_ = false;
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
// Pass this structure to operations that support shared accounting, cancellation, and progress.
struct OperationContext {
    Budget* budget = nullptr;  // Never null in production. Tests may pass a tiny one.
    CancellationToken cancellation;
    Deadline deadline;
    ProgressSink* progress = nullptr;  // Optional.
    DiagnosticPathPolicy path_policy = DiagnosticPathPolicy::basename;
    std::string path_root;  // UTF-8 root for DiagnosticPathPolicy::relative.

    // Charges the budget, or fails. Shorthand so call sites stay readable.
    Result<void> consume(BudgetKind kind, std::uint64_t amount, std::string_view operation) const;

    // Observe a supported asset cardinality, or fail if it exceeds the budget.
    Result<void> observe(BudgetKind kind, std::uint64_t amount, std::string_view operation) const;

    // Check cancellation and the deadline. Cancellation takes priority if both occurred.
    Result<void> check(std::string_view operation) const;

    // Reports progress, if a sink is attached. Safe to call with no sink.
    void report(const ProgressEvent& event) const noexcept;

    // Check the controls, report progress, and check again after the callback.
    Result<void> checkpoint(const ProgressEvent& event) const;
};

// Builds a context from the caller's budget. It also applies the budget's deadline.
//
// The budget is owned by the caller: an OperationContext holds a non-owning pointer, because
// a long-running server wants one budget shared across the stages of one job.
OperationContext make_default_context(Budget& budget);

}  // namespace melkor

#endif  // MELKOR_BUDGET_HPP
