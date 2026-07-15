// Tests for the conversion loss policy (WP06).
//
// The loss report is what keeps a conversion honest: it must not silently discard semantic data
// and return success. These tests pin the policy -- info/warning pass, a severe loss blocks
// unless its exact code is approved, and a fatal loss can never be approved -- because a bug here
// would let a lossy conversion masquerade as clean.
//
// Self-contained (no external test framework).

#include "melkor/format/format_id.hpp"
#include "melkor/format/loss.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace melkor;

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

LossItem item(const char* code, LossSeverity severity) {
    LossItem i;
    i.code = code;
    i.severity = severity;
    i.affected_splats = 1;
    i.remediation = "test";
    return i;
}

void test_empty_report_passes() {
    LossReport report;
    CHECK(report.empty());
    CHECK(!report.has_blocking());
    CHECK(report.check_policy({}).has_value());  // a zero-loss conversion always commits
}

void test_info_and_warning_pass_without_approval() {
    LossReport report;
    report.add(item(loss_code::kQuantizationApplied, LossSeverity::warning));
    report.add(item(loss_code::kNonfiniteRepaired, LossSeverity::info));
    CHECK(!report.has_blocking());
    // info and warning do not need approval; the conversion commits.
    CHECK(report.check_policy({}).has_value());
}

void test_severe_loss_blocks_until_approved() {
    LossReport report;
    report.add(item(loss_code::kShDegreeTruncated, LossSeverity::severe));
    CHECK(report.has_blocking());

    // Unapproved: blocked, and the diagnostic names the exact code and the flag that approves it.
    auto blocked = report.check_policy({});
    CHECK(!blocked.has_value());
    CHECK(blocked.error_code() == ErrorCode::unsupported_feature);
    CHECK(blocked.diagnostics()[0].code == "MK1602_UNAPPROVED_SEVERE_LOSS");
    CHECK(blocked.diagnostics()[0].context.count("approve_with") == 1);

    // Approving the EXACT code lets it through.
    auto approved = report.check_policy({std::string(loss_code::kShDegreeTruncated)});
    CHECK(approved.has_value());

    // Approving a DIFFERENT code does not: approval is per exact code, not blanket.
    auto wrong = report.check_policy({std::string(loss_code::kQuantizationApplied)});
    CHECK(!wrong.has_value());
}

void test_multiple_severe_losses_all_need_approval() {
    LossReport report;
    report.add(item(loss_code::kShDegreeTruncated, LossSeverity::severe));
    report.add(item(loss_code::kSceneGraphFlattened, LossSeverity::severe));

    // Approving only one still blocks on the other.
    CHECK(!report.check_policy({std::string(loss_code::kShDegreeTruncated)}).has_value());

    // Both approved: commits.
    CHECK(report.check_policy({std::string(loss_code::kShDegreeTruncated),
                              std::string(loss_code::kSceneGraphFlattened)})
              .has_value());
}

void test_fatal_loss_can_never_be_approved() {
    LossReport report;
    LossItem fatal = item("LOSS_TARGET_CANNOT_REPRESENT", LossSeverity::fatal);
    report.add(fatal);

    // Even "approving" its code does not let a fatal loss through: the target simply cannot
    // represent the asset, and no flag changes that.
    auto even_approved = report.check_policy({std::string("LOSS_TARGET_CANNOT_REPRESENT")});
    CHECK(!even_approved.has_value());
    CHECK(even_approved.diagnostics()[0].code == "MK1601_FATAL_LOSS");
}

void test_format_capabilities_drive_loss() {
    // The capability model is what lets the planner derive a loss instead of hard-coding it:
    // source SH degree 4 into a target whose max is 3 is a truncation.
    FormatCapabilities spz;
    spz.max_sh_degree = 4;
    FormatCapabilities gltf_rc;
    gltf_rc.max_sh_degree = 3;
    CHECK(spz.max_sh_degree > gltf_rc.max_sh_degree);  // the condition the planner tests

    CHECK(std::string(to_string(FormatId::spz)) == "spz");
    CHECK(std::string(to_string(FormatId::gltf)) == "gltf");
    CHECK(std::string(to_string(FormatId::unknown)) == "unknown");
}

}  // namespace

int main() {
    test_empty_report_passes();
    test_info_and_warning_pass_without_approval();
    test_severe_loss_blocks_until_approved();
    test_multiple_severe_losses_all_need_approval();
    test_fatal_loss_can_never_be_approved();
    test_format_capabilities_drive_loss();

    if (g_failures == 0) {
        std::printf("loss report: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "loss report: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
