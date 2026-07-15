#include "melkor/budget.hpp"

#include "melkor/checked.hpp"

#include <string>

namespace melkor {

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
        case BudgetKind::mesh_vertices:
            return "mesh_vertices";
        case BudgetKind::mesh_triangles:
            return "mesh_triangles";
        case BudgetKind::gltf_nodes:
            return "gltf_nodes";
        case BudgetKind::accessors:
            return "accessors";
        case BudgetKind::external_resources:
            return "external_resources";
        case BudgetKind::metadata_bytes:
            return "metadata_bytes";
        case BudgetKind::image_pixels:
            return "image_pixels";
    }
    return "unknown";
}

const char* override_flag_for(BudgetKind kind) noexcept {
    // A limit diagnostic that does not tell the user how to proceed is only half a
    // diagnostic. Every budget that a user can legitimately raise names the flag that raises
    // it; the rest say so explicitly rather than implying an override exists.
    switch (kind) {
        case BudgetKind::input_bytes:
            return "--max-input-bytes";
        case BudgetKind::decoded_bytes:
            return "--max-decoded-bytes";
        case BudgetKind::memory_bytes:
            return "--max-memory";
        case BudgetKind::temp_bytes:
            return "--max-temp-bytes";
        case BudgetKind::splats:
            return "--max-splats";
        case BudgetKind::resource_bytes:
        case BudgetKind::mesh_vertices:
        case BudgetKind::mesh_triangles:
        case BudgetKind::gltf_nodes:
        case BudgetKind::accessors:
        case BudgetKind::external_resources:
        case BudgetKind::metadata_bytes:
        case BudgetKind::image_pixels:
            return "--limits-profile server, or --limits-file";
    }
    return "--limits-file";
}

Budget::Budget(Limits limits) : limits_(limits) {}

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
        case BudgetKind::mesh_vertices:
            return limits_.max_mesh_vertices;
        case BudgetKind::mesh_triangles:
            return limits_.max_mesh_triangles;
        case BudgetKind::gltf_nodes:
            return limits_.max_gltf_nodes;
        case BudgetKind::accessors:
            return limits_.max_accessors;
        case BudgetKind::external_resources:
            return limits_.max_external_resources;
        case BudgetKind::metadata_bytes:
            return limits_.max_metadata_total_bytes;
        case BudgetKind::image_pixels:
            return limits_.max_image_pixels;
    }
    return 0;
}

Result<void> Budget::consume(BudgetKind kind, std::uint64_t amount, std::string_view operation) {
    const std::uint64_t limit = limit_for(kind);
    const auto index = static_cast<std::size_t>(kind);

    std::lock_guard<std::mutex> lock(mutex_);

    // The running total is itself derived from file-provided numbers, so it can overflow. A
    // wrapped total would come out *below* the limit and the check would pass, which is the
    // one outcome this whole class exists to prevent.
    auto total = checked_add(used_[index], amount, to_string(kind));
    if (!total.has_value()) {
        return Result<void>::failure(ErrorCode::resource_limit, total.diagnostics());
    }

    if (limit != 0 && total.value() > limit) {
        Diagnostic diagnostic("MK0301_RESOURCE_LIMIT_EXCEEDED", Severity::error,
                              std::string("resource limit exceeded: ") + to_string(kind));
        diagnostic.with_context("limit_name", std::string(to_string(kind)));
        diagnostic.with_context("limit", limit);
        diagnostic.with_context("already_used", used_[index]);
        diagnostic.with_context("requested", amount);
        diagnostic.with_context("would_total", total.value());
        diagnostic.with_context("operation", std::string(operation));
        diagnostic.with_context("override", std::string(override_flag_for(kind)));
        return Result<void>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    }

    used_[index] = total.value();
    return Result<void>::success();
}

void Budget::release(BudgetKind kind, std::uint64_t amount) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    std::lock_guard<std::mutex> lock(mutex_);
    // Saturate at zero rather than wrapping. An unbalanced release is a bug in the caller, but
    // turning it into a colossal "used" figure would produce a bewildering limit error far
    // from the actual mistake.
    used_[index] = amount > used_[index] ? 0 : used_[index] - amount;
}

std::uint64_t Budget::used(BudgetKind kind) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return used_[static_cast<std::size_t>(kind)];
}

std::uint64_t Budget::remaining(BudgetKind kind) const noexcept {
    const std::uint64_t limit = limit_for(kind);
    // A 0 limit means "unlimited" in consume(); remaining() must agree, or it would report 0
    // headroom for an unbounded budget.
    if (limit == 0) return std::numeric_limits<std::uint64_t>::max();
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t consumed = used_[static_cast<std::size_t>(kind)];
    return consumed >= limit ? 0 : limit - consumed;
}

Result<void> Budget::check_decompression_ratio(std::uint64_t compressed_bytes,
                                               std::uint64_t declared_decoded_bytes,
                                               std::string_view operation) const {
    // Checked *before* inflating anything. An absolute decoded-byte cap on its own still lets
    // a 1 KiB file expand all the way to the cap; the ratio catches the shape of the attack
    // rather than only its magnitude.
    if (compressed_bytes == 0) {
        // Nothing to expand from. Either an empty stream or a malformed header; both are the
        // caller's problem to diagnose, and dividing by zero here is not.
        return Result<void>::success();
    }

    const std::uint64_t max_ratio = limits_.max_decompression_ratio;
    if (max_ratio == 0) {
        return Result<void>::success();
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

Result<void> OperationContext::consume(BudgetKind kind, std::uint64_t amount,
                                       std::string_view operation) const {
    if (budget == nullptr) {
        // An operation running without a budget is unaccounted, which is the exact condition
        // this design exists to make impossible. Failing loudly beats silently proceeding
        // with no limits, which is how P0-12 happened in the first place.
        Diagnostic diagnostic("MK0303_NO_BUDGET", Severity::error,
                              "operation has no resource budget attached");
        diagnostic.with_context("operation", std::string(operation));
        return Result<void>::failure(ErrorCode::internal_error, std::move(diagnostic));
    }
    return budget->consume(kind, amount, operation);
}

void OperationContext::report(const ProgressEvent& event) const {
    if (progress != nullptr) {
        progress->on_progress(event);
    }
}

OperationContext make_default_context(Budget& budget) {
    OperationContext context;
    context.budget = &budget;
    return context;
}

}  // namespace melkor
