// Tests for the safety substrate: checked arithmetic, resource budgets, cancellation, and
// the diagnostic/result contract.
//
// These are the tests the pre-v2 suite did not have. It passed 13/13 while float RGB was
// being divided by 255 and while a well-formed 40 GiB file could exhaust memory, because
// nothing exercised those paths. A green suite is evidence about the tests, not about the
// software.
//
// The bar here is adversarial: for every value that arrives from a file and reaches an
// allocation, there is a case that tries to make it overflow.
//
// Self-contained (no external test framework), matching the existing suite's convention.

#include "melkor/budget.hpp"
#include "melkor/checked.hpp"
#include "melkor/error.hpp"
#include "melkor/limits.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

using namespace melkor;

constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------

void test_checked_add() {
    auto ok = checked_add(2, 3);
    CHECK(ok.has_value());
    CHECK(ok.value() == 5);

    // The boundary: the largest sum that does not overflow.
    auto edge = checked_add(kU64Max - 1, 1);
    CHECK(edge.has_value());
    CHECK(edge.value() == kU64Max);

    // One past it.
    auto overflow = checked_add(kU64Max, 1);
    CHECK(!overflow.has_value());
    CHECK(overflow.error_code() == ErrorCode::invalid_data);
    CHECK(!overflow.diagnostics().empty());
    CHECK(overflow.diagnostics()[0].code == "MK0101_INTEGER_OVERFLOW");

    auto both_huge = checked_add(kU64Max, kU64Max);
    CHECK(!both_huge.has_value());
}

void test_checked_mul() {
    auto ok = checked_mul(1000, 1000);
    CHECK(ok.has_value());
    CHECK(ok.value() == 1'000'000);

    // Zero must not be mistaken for an overflow: 0 * anything is 0, and the naive
    // "b > MAX / a" guard divides by zero if a == 0 is not special-cased.
    auto zero = checked_mul(0, kU64Max);
    CHECK(zero.has_value());
    CHECK(zero.value() == 0);

    auto zero_rhs = checked_mul(kU64Max, 0);
    CHECK(zero_rhs.has_value());
    CHECK(zero_rhs.value() == 0);

    auto overflow = checked_mul(kU64Max, 2);
    CHECK(!overflow.has_value());
    CHECK(overflow.error_code() == ErrorCode::invalid_data);

    // The realistic shape: a file declares a splat count, and each splat has a stride.
    // 2^33 splats * 248 bytes overflows a 32-bit size computation and is caught here.
    auto realistic = checked_mul(8'589'934'592ULL, 248);
    CHECK(realistic.has_value());  // Fits in 64-bit...
    auto too_big = checked_size_cast(realistic.value());
    // ...and on a 64-bit host it also fits size_t, so the *budget* is what must reject it.
    // That separation is deliberate: arithmetic safety and resource policy are different jobs.
    CHECK(too_big.has_value() == (sizeof(std::size_t) == 8));
}

void test_checked_sub() {
    auto ok = checked_sub(10, 3);
    CHECK(ok.has_value());
    CHECK(ok.value() == 7);

    // Underflow wraps to a colossal number in unsigned arithmetic, which then passes every
    // subsequent "is it too big" check by being interpreted as a length.
    auto underflow = checked_sub(3, 10);
    CHECK(!underflow.has_value());
    CHECK(underflow.diagnostics()[0].code == "MK0102_INTEGER_UNDERFLOW");
}

void test_checked_range() {
    // The bug this exists to prevent, stated as a test:
    //
    //     if (offset + length <= total) { read(buffer + offset, length); }
    //
    // With these operands the sum wraps to 8, the naive check passes, and the read runs off
    // the end of the buffer. checked_range must refuse it.
    auto wrap = checked_range(kU64Max - 7, 16, 1024);
    CHECK(!wrap.has_value());
    CHECK(wrap.error_code() == ErrorCode::invalid_data);

    // Ordinary out-of-bounds.
    auto past_end = checked_range(1000, 100, 1024);
    CHECK(!past_end.has_value());
    CHECK(past_end.diagnostics()[0].code == "MK0105_RANGE_OUT_OF_BOUNDS");

    // Exactly to the end is legal: [1000, 1024) in a 1024-byte buffer.
    auto exact = checked_range(1000, 24, 1024);
    CHECK(exact.has_value());
    CHECK(exact.value().offset == 1000);
    CHECK(exact.value().end() == 1024);

    // A zero-length range at the very end is legal and must not be treated as out of bounds.
    auto empty_at_end = checked_range(1024, 0, 1024);
    CHECK(empty_at_end.has_value());

    // But an offset past the end is not, even with zero length.
    auto past = checked_range(1025, 0, 1024);
    CHECK(!past.has_value());
}

void test_checked_sh_counts() {
    // (degree + 1)^2 coefficients per channel.
    auto d0 = checked_sh_coefficient_count(0);
    CHECK(d0.has_value() && d0.value() == 1);

    auto d3 = checked_sh_coefficient_count(3);
    CHECK(d3.has_value() && d3.value() == 16);

    // Degree 4 must be supported: SPZ v4 carries it, and refusing it here would force a
    // silent truncation somewhere downstream.
    auto d4 = checked_sh_coefficient_count(4);
    CHECK(d4.has_value() && d4.value() == 25);

    // Degree 5 is outside the canonical model and must be rejected at the boundary, not
    // clamped -- clamping would silently change what the file said.
    auto d5 = checked_sh_coefficient_count(5);
    CHECK(!d5.has_value());
    CHECK(d5.diagnostics()[0].code == "MK0106_SH_DEGREE_OUT_OF_RANGE");

    // A file-declared degree of 4 billion must not wrap.
    auto absurd = checked_sh_coefficient_count(4'000'000'000u);
    CHECK(!absurd.has_value());

    // Total floats: 1000 splats, degree 3 -> 1000 * 16 * 3.
    auto total = checked_sh_total_floats(1000, 3);
    CHECK(total.has_value());
    CHECK(total.value() == 1000ULL * 16 * 3);

    // A colossal splat count times degree-4 storage must overflow-check, not wrap.
    auto huge = checked_sh_total_floats(kU64Max, 4);
    CHECK(!huge.has_value());
}

void test_decompression_ratio_boundary_is_exact() {
    // Integer division truncates, so an earlier version compared declared/compressed and let a
    // true ratio just under (max+1) slip past. With compressed=1000 and max=100, a declared
    // size of 100999 is a true ratio of 100.999 and must be REJECTED, not rounded down to 100.
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_decompression_ratio = 100;
    Budget budget(limits);

    // Exactly at the limit: 100 * 1000 = 100000. Allowed.
    CHECK(budget.check_decompression_ratio(1000, 100000, "test").has_value());

    // One byte over the exact threshold must fail, even though 100001/1000 truncates to 100.
    auto over = budget.check_decompression_ratio(1000, 100001, "test");
    CHECK(!over.has_value());
    CHECK(over.error_code() == ErrorCode::resource_limit);

    // The old truncation bug would have accepted anything up to 100999; prove it does not.
    CHECK(!budget.check_decompression_ratio(1000, 100999, "test").has_value());
}

void test_custom_profile_missing_any_budget_limit_fails_validation() {
    // A limit that is enforced through a BudgetKind but left at 0 is silently unlimited. A
    // custom profile that populates the common fields but forgets one of the less obvious ones
    // must not validate -- otherwise the forgotten limit is disabled and the header's promise of
    // no accidental "unbounded" is broken.
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_mesh_triangles = 0;  // the field the original validate() missed
    auto result = limits.validate();
    CHECK(!result.has_value());

    limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_image_pixels = 0;
    CHECK(!limits.validate().has_value());

    limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_metadata_total_bytes = 0;
    CHECK(!limits.validate().has_value());

    // But zeroing max_temp_bytes must STILL validate: the web profile legitimately sets it to 0
    // (a browser has no temp directory), so requiring it positive would reject a real profile.
    limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_temp_bytes = 0;
    CHECK(limits.validate().has_value());
}

// ---------------------------------------------------------------------------
// Budgets
// ---------------------------------------------------------------------------

void test_budget_enforces_limits() {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_splats = 1000;  // Tiny, so the failure path is cheap to exercise.
    Budget budget(limits);

    CHECK(budget.consume(BudgetKind::splats, 600, "test").has_value());
    CHECK(budget.used(BudgetKind::splats) == 600);
    CHECK(budget.remaining(BudgetKind::splats) == 400);

    // Right up to the limit is fine.
    CHECK(budget.consume(BudgetKind::splats, 400, "test").has_value());
    CHECK(budget.remaining(BudgetKind::splats) == 0);

    // One past it is not.
    auto over = budget.consume(BudgetKind::splats, 1, "test");
    CHECK(!over.has_value());
    CHECK(over.error_code() == ErrorCode::resource_limit);
    CHECK(exit_code_for(over.error_code()) == 6);

    // The diagnostic must be actionable: it names the limit, what was observed, and the flag
    // that raises it. "Limit exceeded" on its own tells a user nothing they can act on.
    const Diagnostic& diagnostic = over.diagnostics()[0];
    CHECK(diagnostic.code == "MK0301_RESOURCE_LIMIT_EXCEEDED");
    CHECK(diagnostic.context.count("limit") == 1);
    CHECK(diagnostic.context.count("already_used") == 1);
    CHECK(diagnostic.context.count("requested") == 1);
    CHECK(diagnostic.context.count("override") == 1);
}

void test_budget_rejects_overflowing_total() {
    // A running total derived from file-provided numbers can itself overflow. If it wraps, the
    // wrapped value comes out *below* the limit and the check passes -- the one outcome the
    // budget exists to prevent.
    Limits limits = Limits::for_profile(LimitsProfile::server);
    Budget budget(limits);

    CHECK(budget.consume(BudgetKind::decoded_bytes, 1024, "test").has_value());

    auto overflow = budget.consume(BudgetKind::decoded_bytes, kU64Max, "test");
    CHECK(!overflow.has_value());
    CHECK(overflow.error_code() == ErrorCode::resource_limit);
}

void test_budget_release_saturates() {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    Budget budget(limits);

    CHECK(budget.consume(BudgetKind::memory_bytes, 1000, "test").has_value());
    budget.release(BudgetKind::memory_bytes, 400);
    CHECK(budget.used(BudgetKind::memory_bytes) == 600);

    // An unbalanced release is a caller bug, but it must saturate at zero rather than wrap to
    // a colossal "used" figure that produces a bewildering error far from the real mistake.
    budget.release(BudgetKind::memory_bytes, 10'000);
    CHECK(budget.used(BudgetKind::memory_bytes) == 0);
}

void test_decompression_bomb_guard() {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_decompression_ratio = 100;
    Budget budget(limits);

    // A 1 MiB file expanding to 50 MiB: a 50x ratio, plausible for real data.
    CHECK(budget.check_decompression_ratio(1'048'576, 52'428'800, "spz.decode").has_value());

    // A 1 KiB file claiming to expand to 4 GiB: a ratio of ~4 million. This is the shape of
    // the attack, and it must be refused *before* anything is inflated -- an absolute
    // decoded-byte cap alone would happily let this expand all the way up to the cap.
    auto bomb = budget.check_decompression_ratio(1024, 4'294'967'296ULL, "spz.decode");
    CHECK(!bomb.has_value());
    CHECK(bomb.error_code() == ErrorCode::resource_limit);
    CHECK(bomb.diagnostics()[0].code == "MK0302_DECOMPRESSION_RATIO_EXCEEDED");

    // Zero compressed bytes must not divide by zero.
    CHECK(budget.check_decompression_ratio(0, 1000, "spz.decode").has_value());
}

void test_budget_is_thread_safe() {
    Limits limits = Limits::for_profile(LimitsProfile::server);
    limits.max_splats = 100'000;
    Budget budget(limits);

    // Eight threads each consuming 10,000 must total exactly 80,000. A racy counter would
    // lose updates and under-report, which would let a limit be exceeded in production.
    constexpr int kThreads = 8;
    constexpr std::uint64_t kEach = 10'000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&budget]() {
            for (int j = 0; j < 100; ++j) {
                (void)budget.consume(BudgetKind::splats, kEach / 100, "test");
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    CHECK(budget.used(BudgetKind::splats) == kThreads * kEach);
}

void test_context_without_budget_fails_closed() {
    // An operation running with no budget is unaccounted. That must be an error, not a silent
    // "no limits" -- silently proceeding unbounded is exactly how P0-12 arose.
    OperationContext context;
    auto result = context.consume(BudgetKind::splats, 1, "test");
    CHECK(!result.has_value());
    CHECK(result.error_code() == ErrorCode::internal_error);
    CHECK(result.diagnostics()[0].code == "MK0303_NO_BUDGET");
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

void test_limit_profiles_validate() {
    CHECK(Limits::for_profile(LimitsProfile::web).validate().has_value());
    CHECK(Limits::for_profile(LimitsProfile::desktop).validate().has_value());
    CHECK(Limits::for_profile(LimitsProfile::server).validate().has_value());

    // A default-constructed custom profile is all zeros. It must NOT validate: an all-zeros
    // struct is the most plausible way somebody accidentally turns off resource accounting,
    // and "zero" must never be read as "unlimited".
    CHECK(!Limits::for_profile(LimitsProfile::custom).validate().has_value());

    // web must genuinely be tighter than server, or the profile names are lying.
    const Limits web = Limits::for_profile(LimitsProfile::web);
    const Limits server = Limits::for_profile(LimitsProfile::server);
    CHECK(web.max_memory_bytes < server.max_memory_bytes);
    CHECK(web.max_splats < server.max_splats);
    CHECK(web.max_image_pixels < server.max_image_pixels);
}

void test_limits_reject_absurd_overrides() {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_splats = hard_ceiling::kMaxSplats + 1;

    auto result = limits.validate();
    CHECK(!result.has_value());
    CHECK(result.diagnostics()[0].code == "MK0203_LIMIT_EXCEEDS_CEILING");

    // A decoded cap below the input cap would reject every incompressible file.
    Limits inconsistent = Limits::for_profile(LimitsProfile::desktop);
    inconsistent.max_decoded_bytes = 1024;
    inconsistent.max_input_bytes = 1'048'576;
    auto bad = inconsistent.validate();
    CHECK(!bad.has_value());
}

void test_limits_profile_parsing() {
    CHECK(limits_profile_from_string("desktop").has_value());
    auto unknown = limits_profile_from_string("unlimited");
    CHECK(!unknown.has_value());
    CHECK(unknown.error_code() == ErrorCode::invalid_argument);
    // The diagnostic must list what IS valid, since "unlimited" is precisely what a user
    // reaching for it wants and precisely what does not exist.
    CHECK(unknown.diagnostics()[0].context.count("supported") == 1);
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

void test_cancellation() {
    CancellationToken token;
    CHECK(!token.is_cancelled());
    CHECK(token.check().has_value());

    token.cancel();
    CHECK(token.is_cancelled());

    auto cancelled = token.check();
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error_code() == ErrorCode::cancelled);
    // 130 == 128 + SIGINT, the shell convention, so `melkor ...; echo $?` behaves like the
    // user's other tools.
    CHECK(exit_code_for(cancelled.error_code()) == 130);
}

void test_cancellation_token_shares_state() {
    // The token is set from a signal handler or another thread and observed on a worker.
    // A copy that did not share state would silently never cancel.
    CancellationToken token;
    CancellationToken copy = token;

    token.cancel();
    CHECK(copy.is_cancelled());
}

// ---------------------------------------------------------------------------
// Result and diagnostics
// ---------------------------------------------------------------------------

void test_result_carries_diagnostics_on_success() {
    // A successful operation can still have warnings worth surfacing -- a quaternion was
    // renormalized, an unknown property was skipped. Dropping them because the operation
    // "worked" is how silent data corruption ships.
    std::vector<Diagnostic> warnings;
    warnings.emplace_back("MK9999_TEST", Severity::warning, "a renormalization happened");

    auto result = Result<int>::success(42, std::move(warnings));
    CHECK(result.has_value());
    CHECK(result.value() == 42);
    CHECK(result.diagnostics().size() == 1);
    CHECK(!result.has_errors());  // A warning is not an error.
}

void test_result_void() {
    auto ok = Result<void>::success();
    CHECK(ok.has_value());

    auto failed = Result<void>::failure(
        ErrorCode::io_error, Diagnostic("MK9998_TEST", Severity::error, "disk exploded"));
    CHECK(!failed.has_value());
    CHECK(failed.error_code() == ErrorCode::io_error);
    CHECK(failed.has_errors());
}

void test_exit_code_mapping_is_exhaustive() {
    // The exit codes are a public contract; a script may branch on them. Pin them here so a
    // change has to be deliberate.
    CHECK(exit_code_for(ErrorCode::ok) == 0);
    CHECK(exit_code_for(ErrorCode::invalid_argument) == 2);
    CHECK(exit_code_for(ErrorCode::invalid_data) == 3);
    CHECK(exit_code_for(ErrorCode::unsupported_feature) == 4);
    CHECK(exit_code_for(ErrorCode::io_error) == 5);
    CHECK(exit_code_for(ErrorCode::resource_limit) == 6);
    CHECK(exit_code_for(ErrorCode::backend_unavailable) == 7);
    CHECK(exit_code_for(ErrorCode::internal_error) == 8);
    CHECK(exit_code_for(ErrorCode::cancelled) == 130);
}

void test_path_redaction() {
    const std::string absolute = "/home/alice/secret-project/scene.ply";

    // The default. An inspection report is routinely pasted into a public issue; an absolute
    // path leaks a username and a directory layout.
    CHECK(redact_path(absolute, DiagnosticPathPolicy::basename) == "scene.ply");

    CHECK(redact_path(absolute, DiagnosticPathPolicy::full) == absolute);

    CHECK(redact_path(absolute, DiagnosticPathPolicy::relative, "/home/alice/secret-project") ==
          "scene.ply");
    CHECK(redact_path("/home/alice/proj/assets/scene.ply", DiagnosticPathPolicy::relative,
                      "/home/alice/proj") == "assets/scene.ply");

    // A path outside the declared root must fall back to the *stricter* policy, never the
    // looser one. Falling back to `full` here would leak precisely the paths the caller was
    // trying to contain.
    CHECK(redact_path("/etc/passwd", DiagnosticPathPolicy::relative, "/home/alice") ==
          "passwd");

    // The prefix must align on a path-component boundary. A sibling directory whose name merely
    // *starts with* the root -- "/home/alice/workshop" under root "/home/alice/work" -- is NOT
    // inside the root, and treating it as a string prefix would both leak "shop/..." and produce
    // a nonsense relative path. It must fall back to basename.
    CHECK(redact_path("/home/alice/workshop/secret.ply", DiagnosticPathPolicy::relative,
                      "/home/alice/work") == "secret.ply");

    // A trailing separator on the root must behave identically to one without.
    CHECK(redact_path("/home/alice/proj/scene.ply", DiagnosticPathPolicy::relative,
                      "/home/alice/proj/") == "scene.ply");
}

}  // namespace

int main() {
    test_checked_add();
    test_checked_mul();
    test_checked_sub();
    test_checked_range();
    test_checked_sh_counts();

    test_budget_enforces_limits();
    test_budget_rejects_overflowing_total();
    test_budget_release_saturates();
    test_decompression_bomb_guard();
    test_budget_is_thread_safe();
    test_context_without_budget_fails_closed();

    test_limit_profiles_validate();
    test_limits_reject_absurd_overrides();
    test_limits_profile_parsing();
    test_custom_profile_missing_any_budget_limit_fails_validation();

    test_decompression_ratio_boundary_is_exact();

    test_cancellation();
    test_cancellation_token_shares_state();

    test_result_carries_diagnostics_on_success();
    test_result_void();
    test_exit_code_mapping_is_exhaustive();
    test_path_redaction();

    if (g_failures == 0) {
        std::printf("safety substrate: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "safety substrate: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
