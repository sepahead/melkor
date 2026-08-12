#include "melkor/budget.hpp"

#include "melkor/checked.hpp"

#include <chrono>
#include <limits>
#include <string>

namespace melkor {

namespace {

bool valid_kind(BudgetKind kind) noexcept {
    return static_cast<std::size_t>(kind) < static_cast<std::size_t>(BudgetKind::count);
}

Result<void> invalid_kind(BudgetKind kind, std::string_view operation) {
    Diagnostic diagnostic("MK0306_INVALID_BUDGET_KIND", Severity::error,
                          "resource budget kind is invalid");
    diagnostic.with_context("kind", static_cast<std::uint64_t>(kind));
    diagnostic.with_context("operation", std::string(operation));
    return Result<void>::failure(ErrorCode::invalid_argument, std::move(diagnostic));
}

std::uint64_t limit_value(const Limits& limits, BudgetKind kind) noexcept {
    switch (kind) {
    case BudgetKind::input_bytes:
        return limits.max_input_bytes;
    case BudgetKind::resource_bytes:
        return limits.max_resource_bytes;
    case BudgetKind::decoded_bytes:
        return limits.max_decoded_bytes;
    case BudgetKind::memory_bytes:
        return limits.max_memory_bytes;
    case BudgetKind::temp_bytes:
        return limits.max_temp_bytes;
    case BudgetKind::splats:
        return limits.max_splats;
    case BudgetKind::gltf_nodes:
        return limits.max_gltf_nodes;
    case BudgetKind::accessors:
        return limits.max_accessors;
    case BudgetKind::external_resources:
        return limits.max_external_resources;
    case BudgetKind::count:
        break;
    }
    return 0;
}

}  // namespace

struct Budget::SharedState {
    static constexpr std::size_t kKindCount = static_cast<std::size_t>(BudgetKind::count);
    std::mutex mutex;
    std::array<std::uint64_t, kKindCount> used{};
    std::array<std::uint64_t, kKindCount> unowned{};
};

Budget::Charge::Charge(std::shared_ptr<SharedState> state, BudgetKind kind,
                       std::uint64_t amount) noexcept
    : state_(std::move(state)), kind_(kind), amount_(amount) {}

Budget::Charge::~Charge() {
    reset();
}

Budget::Charge::Charge(Charge&& other) noexcept
    : state_(std::move(other.state_)), kind_(other.kind_), amount_(other.amount_) {
    other.kind_ = BudgetKind::count;
    other.amount_ = 0;
}

Budget::Charge& Budget::Charge::operator=(Charge&& other) noexcept {
    if (this == &other)
        return *this;
    reset();
    state_ = std::move(other.state_);
    kind_ = other.kind_;
    amount_ = other.amount_;
    other.kind_ = BudgetKind::count;
    other.amount_ = 0;
    return *this;
}

void Budget::Charge::reset() noexcept {
    Budget::release_shared(state_, kind_, amount_);
    state_.reset();
    kind_ = BudgetKind::count;
    amount_ = 0;
}

void Budget::Charge::shrink_to(std::uint64_t amount) noexcept {
    if (amount >= amount_)
        return;
    Budget::release_shared(state_, kind_, amount_ - amount);
    amount_ = amount;
}

const char* to_string(BudgetKind kind) noexcept {
    switch (kind) {
    case BudgetKind::input_bytes:
        return "input_bytes";
    case BudgetKind::resource_bytes:
        return "resource_bytes";
    case BudgetKind::decoded_bytes:
        return "decoded_bytes";
    case BudgetKind::memory_bytes:
        return "memory_bytes";
    case BudgetKind::temp_bytes:
        return "temp_bytes";
    case BudgetKind::splats:
        return "splats";
    case BudgetKind::gltf_nodes:
        return "gltf_nodes";
    case BudgetKind::accessors:
        return "accessors";
    case BudgetKind::external_resources:
        return "external_resources";
    case BudgetKind::count:
        break;
    }
    return "unknown";
}

std::string override_flag_for(BudgetKind kind, const Limits& current) {
    if (!valid_kind(kind))
        return {};
    const std::uint64_t current_value = limit_value(current, kind);
    for (const LimitsProfile profile : {LimitsProfile::desktop, LimitsProfile::server}) {
        const Limits candidate = Limits::for_profile(profile);
        if (limit_value(candidate, kind) > current_value)
            return std::string("--limits-profile ") + to_string(profile);
    }
    return {};
}

Budget::Budget(Limits limits) : limits_(limits), state_(std::make_shared<SharedState>()) {}

std::uint64_t Budget::limit_for(BudgetKind kind) const noexcept {
    switch (kind) {
    case BudgetKind::input_bytes:
        return limits_.max_input_bytes;
    case BudgetKind::resource_bytes:
        return limits_.max_resource_bytes;
    case BudgetKind::decoded_bytes:
        return limits_.max_decoded_bytes;
    case BudgetKind::memory_bytes:
        return limits_.max_memory_bytes;
    case BudgetKind::temp_bytes:
        return limits_.max_temp_bytes;
    case BudgetKind::splats:
        return limits_.max_splats;
    case BudgetKind::gltf_nodes:
        return limits_.max_gltf_nodes;
    case BudgetKind::accessors:
        return limits_.max_accessors;
    case BudgetKind::external_resources:
        return limits_.max_external_resources;
    case BudgetKind::count:
        break;
    }
    return 0;
}

Result<void> Budget::consume(BudgetKind kind, std::uint64_t amount, std::string_view operation) {
    if (!valid_kind(kind)) {
        return invalid_kind(kind, operation);
    }
    const std::uint64_t limit = limit_for(kind);
    const auto index = static_cast<std::size_t>(kind);

    std::lock_guard<std::mutex> lock(state_->mutex);

    if (limit == 0) {
        Diagnostic diagnostic("MK0303_RESOURCE_LIMIT_NOT_SET", Severity::error,
                              std::string("resource limit is not set: ") + to_string(kind));
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("operation", std::string(operation));
        return Result<void>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    }

    // The running total is itself derived from file-provided numbers, so it can overflow. A
    // wrapped total would come out *below* the limit and the check would pass, which is the
    // one outcome this whole class exists to prevent.
    auto total = checked_add(state_->used[index], amount, to_string(kind));
    if (!total.has_value()) {
        return Result<void>::failure(ErrorCode::resource_limit, total.diagnostics());
    }

    if (total.value() > limit) {
        Diagnostic diagnostic("MK0301_RESOURCE_LIMIT_EXCEEDED", Severity::error,
                              std::string("resource limit exceeded: ") + to_string(kind));
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("limit", limit);
        diagnostic.with_context("already_used", state_->used[index]);
        diagnostic.with_context("requested", amount);
        diagnostic.with_context("would_total", total.value());
        diagnostic.with_context("operation", std::string(operation));
        const std::string override = override_flag_for(kind, limits_);
        if (!override.empty())
            diagnostic.with_context("override", override);
        return Result<void>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    }

    state_->used[index] = total.value();
    if (kind == BudgetKind::memory_bytes || kind == BudgetKind::temp_bytes) {
        state_->unowned[index] += amount;
    }
    return Result<void>::success();
}

Result<Budget::Charge> Budget::reserve(BudgetKind kind, std::uint64_t amount,
                                       std::string_view operation) {
    if (kind != BudgetKind::memory_bytes && kind != BudgetKind::temp_bytes) {
        Diagnostic diagnostic("MK0309_INVALID_BUDGET_RESERVATION", Severity::error,
                              "the resource does not support owned reservations");
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("operation", std::string(operation));
        return Result<Charge>::failure(ErrorCode::invalid_argument, std::move(diagnostic));
    }

    const std::uint64_t limit = limit_for(kind);
    const auto index = static_cast<std::size_t>(kind);
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (limit == 0) {
        Diagnostic diagnostic("MK0303_RESOURCE_LIMIT_NOT_SET", Severity::error,
                              std::string("resource limit is not set: ") + to_string(kind));
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("operation", std::string(operation));
        return Result<Charge>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    }
    auto total = checked_add(state_->used[index], amount, to_string(kind));
    if (!total.has_value()) {
        return Result<Charge>::failure(ErrorCode::resource_limit, total.diagnostics());
    }
    if (total.value() > limit) {
        Diagnostic diagnostic("MK0301_RESOURCE_LIMIT_EXCEEDED", Severity::error,
                              std::string("resource limit exceeded: ") + to_string(kind));
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("limit", limit);
        diagnostic.with_context("already_used", state_->used[index]);
        diagnostic.with_context("requested", amount);
        diagnostic.with_context("would_total", total.value());
        diagnostic.with_context("operation", std::string(operation));
        const std::string override = override_flag_for(kind, limits_);
        if (!override.empty())
            diagnostic.with_context("override", override);
        return Result<Charge>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    }
    state_->used[index] = total.value();
    return Result<Charge>::success(Charge(state_, kind, amount));
}

Result<void> Budget::observe(BudgetKind kind, std::uint64_t amount, std::string_view operation) {
    switch (kind) {
    case BudgetKind::splats:
    case BudgetKind::gltf_nodes:
    case BudgetKind::accessors:
        break;
    default: {
        Diagnostic diagnostic("MK0305_INVALID_BUDGET_OBSERVATION", Severity::error,
                              "the resource does not support high-water observation");
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("operation", std::string(operation));
        return Result<void>::failure(ErrorCode::invalid_argument, std::move(diagnostic));
    }
    }

    const std::uint64_t limit = limit_for(kind);
    const auto index = static_cast<std::size_t>(kind);
    std::lock_guard<std::mutex> lock(state_->mutex);

    if (limit == 0) {
        Diagnostic diagnostic("MK0303_RESOURCE_LIMIT_NOT_SET", Severity::error,
                              std::string("resource limit is not set: ") + to_string(kind));
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("operation", std::string(operation));
        return Result<void>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    }
    if (amount > limit) {
        Diagnostic diagnostic("MK0301_RESOURCE_LIMIT_EXCEEDED", Severity::error,
                              std::string("resource limit exceeded: ") + to_string(kind));
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("limit", limit);
        diagnostic.with_context("already_used", state_->used[index]);
        diagnostic.with_context("observed", amount);
        diagnostic.with_context("operation", std::string(operation));
        const std::string override = override_flag_for(kind, limits_);
        if (!override.empty())
            diagnostic.with_context("override", override);
        return Result<void>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    }
    if (amount > state_->used[index])
        state_->used[index] = amount;
    return Result<void>::success();
}

void Budget::release(BudgetKind kind, std::uint64_t amount) noexcept {
    release_unowned(state_, kind, amount);
}

Result<Budget::Charge> Budget::adopt_charge(BudgetKind kind, std::uint64_t amount,
                                            std::string_view operation) {
    if (kind != BudgetKind::memory_bytes && kind != BudgetKind::temp_bytes) {
        Diagnostic diagnostic("MK0309_INVALID_BUDGET_RESERVATION", Severity::error,
                              "the resource does not support owned reservations");
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("operation", std::string(operation));
        return Result<Charge>::failure(ErrorCode::invalid_argument, std::move(diagnostic));
    }
    if (amount == 0)
        return Result<Charge>::success({});

    const auto index = static_cast<std::size_t>(kind);
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (amount > state_->unowned[index]) {
        Diagnostic diagnostic("MK0308_BUDGET_CHARGE_UNAVAILABLE", Severity::error,
                              "the reclaimable resource charge is not available");
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("requested", amount);
        diagnostic.with_context("available", state_->unowned[index]);
        diagnostic.with_context("operation", std::string(operation));
        return Result<Charge>::failure(ErrorCode::internal_error, std::move(diagnostic));
    }
    state_->unowned[index] -= amount;
    return Result<Charge>::success(Charge(state_, kind, amount));
}

void Budget::release_shared(const std::shared_ptr<SharedState>& state, BudgetKind kind,
                            std::uint64_t amount) noexcept {
    if (state == nullptr)
        return;
    if (!valid_kind(kind))
        return;
    if (kind != BudgetKind::memory_bytes && kind != BudgetKind::temp_bytes)
        return;

    const auto index = static_cast<std::size_t>(kind);
    std::lock_guard<std::mutex> lock(state->mutex);
    // Keep the counter unchanged when an unbalanced release exceeds the current charge.
    if (amount <= state->used[index])
        state->used[index] -= amount;
}

void Budget::release_unowned(const std::shared_ptr<SharedState>& state, BudgetKind kind,
                             std::uint64_t amount) noexcept {
    if (state == nullptr || !valid_kind(kind) ||
        (kind != BudgetKind::memory_bytes && kind != BudgetKind::temp_bytes)) {
        return;
    }
    const auto index = static_cast<std::size_t>(kind);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (amount <= state->unowned[index] && amount <= state->used[index]) {
        state->unowned[index] -= amount;
        state->used[index] -= amount;
    }
}

std::uint64_t Budget::used(BudgetKind kind) const noexcept {
    if (!valid_kind(kind))
        return 0;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->used[static_cast<std::size_t>(kind)];
}

std::uint64_t Budget::remaining(BudgetKind kind) const noexcept {
    if (!valid_kind(kind))
        return 0;
    const std::uint64_t limit = limit_for(kind);
    if (limit == 0)
        return 0;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const std::uint64_t consumed = state_->used[static_cast<std::size_t>(kind)];
    return consumed >= limit ? 0 : limit - consumed;
}

Result<void> Budget::check_decompression_ratio(std::uint64_t compressed_bytes,
                                               std::uint64_t declared_decoded_bytes,
                                               std::string_view operation) const {
    // Checked *before* inflating anything. An absolute decoded-byte cap on its own still lets
    // a 1 KiB file expand all the way to the cap; the ratio catches the shape of the attack
    // rather than only its magnitude.
    if (compressed_bytes == 0 && declared_decoded_bytes == 0) {
        return Result<void>::success();
    }
    if (compressed_bytes == 0) {
        Diagnostic diagnostic("MK0307_EMPTY_COMPRESSED_PAYLOAD", Severity::error,
                              "an empty payload declares nonempty decoded data");
        diagnostic.with_context("declared_decoded_bytes", declared_decoded_bytes);
        diagnostic.with_context("operation", std::string(operation));
        return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }

    const std::uint64_t max_ratio = limits_.max_decompression_ratio;
    if (max_ratio == 0) {
        Diagnostic diagnostic("MK0303_RESOURCE_LIMIT_NOT_SET", Severity::error,
                              "decompression ratio limit is not set");
        diagnostic.with_context("limit_name", std::string("max_decompression_ratio"));
        diagnostic.with_context("operation", std::string(operation));
        return Result<void>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    }

    // Compare declared_decoded_bytes against max_ratio * compressed_bytes rather than dividing.
    //
    // Integer division truncates: declared/compressed for 100999/1000 yields 100, so a strict
    // `ratio > 100` would let a true ratio of 100.999 through. The multiplication form has no
    // such slack. It is done with checked_mul so that a hostile compressed_bytes cannot overflow
    // the threshold into a small number that everything then passes.
    auto threshold = checked_mul(max_ratio, compressed_bytes, "decompression ratio threshold");
    if (!threshold.has_value()) {
        // The threshold overflowed 64 bits, so no plausible declared size can exceed it; the
        // absolute decoded-byte budget is the effective guard in that regime.
        return Result<void>::success();
    }

    if (declared_decoded_bytes > threshold.value()) {
        const std::uint64_t ratio = declared_decoded_bytes / compressed_bytes;
        Diagnostic diagnostic("MK0302_DECOMPRESSION_RATIO_EXCEEDED", Severity::error,
                              "declared decompression ratio exceeds the configured limit");
        diagnostic.with_context("compressed_bytes", compressed_bytes);
        diagnostic.with_context("declared_decoded_bytes", declared_decoded_bytes);
        diagnostic.with_context("ratio", ratio);
        diagnostic.with_context("max_ratio", max_ratio);
        diagnostic.with_context("operation", std::string(operation));
        diagnostic.with_context(
            "note",
            std::string("This is the shape of a decompression bomb. A legitimately highly "
                        "compressible asset can trip it; raise the limit deliberately if you "
                        "trust the source."));
        return Result<void>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    }

    return Result<void>::success();
}

Result<void> CancellationToken::check() const {
    if (is_cancelled()) {
        Diagnostic diagnostic("MK0401_CANCELLED", Severity::note, "operation cancelled");
        return Result<void>::failure(ErrorCode::cancelled, std::move(diagnostic));
    }
    return Result<void>::success();
}

Deadline Deadline::after(std::uint64_t timeout_ms) noexcept {
    Deadline deadline;
    if (timeout_ms == 0) {
        return deadline;
    }

    const TimePoint now = Clock::now();
    const auto available = TimePoint::max() - now;
    const auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(available).count();
    const auto bounded_ms = timeout_ms > static_cast<std::uint64_t>(max_ms)
                                ? max_ms
                                : static_cast<std::int64_t>(timeout_ms);

    deadline.expires_at_ = now + std::chrono::milliseconds(bounded_ms);
    deadline.timeout_ms_ = timeout_ms;
    deadline.is_set_ = true;
    return deadline;
}

Deadline Deadline::at(TimePoint expires_at) noexcept {
    Deadline deadline;
    deadline.expires_at_ = expires_at;
    deadline.is_set_ = true;
    return deadline;
}

bool Deadline::is_expired() const noexcept {
    return is_set_ && Clock::now() >= expires_at_;
}

Result<void> Deadline::check(std::string_view operation) const {
    if (!is_expired()) {
        return Result<void>::success();
    }

    Diagnostic diagnostic("MK0304_DEADLINE_EXCEEDED", Severity::error,
                          "operation deadline exceeded");
    diagnostic.with_context("operation", std::string(operation));
    if (timeout_ms_ != 0) {
        diagnostic.with_context("deadline_ms", timeout_ms_);
    }
    return Result<void>::failure(ErrorCode::resource_limit, std::move(diagnostic));
}

Result<void> OperationContext::consume(BudgetKind kind, std::uint64_t amount,
                                       std::string_view operation) const {
    if (budget == nullptr) {
        // An operation running without a budget is unaccounted, which is the exact condition
        // this design exists to make impossible. Failing loudly beats silently proceeding
        // with no limits, which is how P0-12 happened in the first place.
        Diagnostic diagnostic("MK0310_NO_BUDGET", Severity::error,
                              "operation has no resource budget attached");
        diagnostic.with_context("operation", std::string(operation));
        return Result<void>::failure(ErrorCode::internal_error, std::move(diagnostic));
    }
    return budget->consume(kind, amount, operation);
}

Result<void> OperationContext::observe(BudgetKind kind, std::uint64_t amount,
                                       std::string_view operation) const {
    if (budget == nullptr) {
        Diagnostic diagnostic("MK0310_NO_BUDGET", Severity::error,
                              "operation has no resource budget attached");
        diagnostic.with_context("operation", std::string(operation));
        return Result<void>::failure(ErrorCode::internal_error, std::move(diagnostic));
    }
    return budget->observe(kind, amount, operation);
}

Result<void> OperationContext::check(std::string_view operation) const {
    auto cancelled = cancellation.check();
    if (!cancelled.has_value()) {
        auto diagnostics = cancelled.diagnostics();
        for (auto& diagnostic : diagnostics) {
            diagnostic.with_context("operation", std::string(operation));
        }
        return Result<void>::failure(cancelled.error_code(), std::move(diagnostics));
    }
    return deadline.check(operation);
}

void OperationContext::report(const ProgressEvent& event) const noexcept {
    if (progress != nullptr) {
        try {
            progress->on_progress(event);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // A status callback cannot change the operation result.
        }
    }
}

Result<void> OperationContext::checkpoint(const ProgressEvent& event) const {
    auto control = check(event.operation);
    if (!control.has_value()) {
        return control;
    }
    report(event);
    return check(event.operation);
}

OperationContext make_default_context(Budget& budget) {
    OperationContext context;
    context.budget = &budget;
    context.deadline = Deadline::after(budget.limits().deadline_ms);
    return context;
}

}  // namespace melkor
