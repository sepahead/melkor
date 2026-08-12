// Tests for the conversion loss policy (WP06).
//
// The loss report is what keeps a conversion honest: it must not silently discard semantic data
// and return success. These tests pin the policy -- info/warning pass, a severe loss blocks
// unless its exact code is approved, and a fatal loss can never be approved -- because a bug here
// would let a lossy conversion masquerade as clean.
//
// Self-contained (no external test framework).

#include "melkor/format/loss.hpp"
#include "melkor/format/profile.hpp"

#include <algorithm>
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
    i.source_feature = "source test feature";
    i.target_constraint = "target test constraint";
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
    CHECK(report.add(item(loss_code::kQuantizationApplied, LossSeverity::warning)).has_value());
    CHECK(report.add(item(loss_code::kProvenanceDropped, LossSeverity::info)).has_value());
    CHECK(!report.has_blocking());
    // info and warning do not need approval; the conversion commits.
    CHECK(report.check_policy({}).has_value());
}

void test_severe_loss_blocks_until_approved() {
    LossReport report;
    CHECK(report.add(item(loss_code::kShDegreeTruncated, LossSeverity::severe)).has_value());
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
    CHECK(report.add(item(loss_code::kShDegreeTruncated, LossSeverity::severe)).has_value());
    CHECK(report.add(item(loss_code::kSceneGraphFlattened, LossSeverity::severe)).has_value());

    // Approving only one still blocks on the other.
    CHECK(!report.check_policy({std::string(loss_code::kShDegreeTruncated)}).has_value());

    // Both approved: commits.
    CHECK(report
              .check_policy({std::string(loss_code::kShDegreeTruncated),
                             std::string(loss_code::kSceneGraphFlattened)})
              .has_value());
}

void test_fatal_loss_can_never_be_approved() {
    LossReport report;
    LossItem fatal = item(loss_code::kGltfContentDropped, LossSeverity::fatal);
    CHECK(report.add(fatal).has_value());

    // Even "approving" its code does not let a fatal loss through: the target simply cannot
    // represent the asset, and no flag changes that.
    auto even_approved = report.check_policy({std::string(loss_code::kGltfContentDropped)});
    CHECK(!even_approved.has_value());
    CHECK(even_approved.diagnostics()[0].code == "MK1601_FATAL_LOSS");
}

void test_invalid_reports_and_approvals_fail_closed() {
    LossReport report;
    LossItem forged = item(loss_code::kPrecisionReduced, static_cast<LossSeverity>(255));
    auto rejected = report.add(std::move(forged));
    CHECK(!rejected.has_value());
    CHECK(rejected.error_code() == ErrorCode::internal_error);
    CHECK(rejected.diagnostics()[0].code == "MK1603_INVALID_LOSS_REPORT");
    CHECK(report.empty());

    LossReport valid;
    CHECK(valid.add(item(loss_code::kPrecisionReduced, LossSeverity::warning)).has_value());
    auto unknown = valid.check_policy({"LOSS_NOT_REGISTERED"});
    CHECK(!unknown.has_value());
    CHECK(unknown.error_code() == ErrorCode::invalid_argument);
    CHECK(!valid.check_policy({loss_code::kPrecisionReduced, loss_code::kPrecisionReduced})
               .has_value());

    LossReport understated;
    auto weak = understated.add(item(loss_code::kNonfiniteRepaired, LossSeverity::info));
    CHECK(!weak.has_value());
    CHECK(weak.error_code() == ErrorCode::internal_error);
    CHECK(understated.empty());

    std::vector<std::string> excessive(known_loss_codes().size() + 1, loss_code::kPrecisionReduced);
    CHECK(!valid.check_policy(excessive).has_value());
}

void test_loss_code_registry_is_complete_and_unique() {
    const auto& codes = known_loss_codes();
    CHECK(codes.size() == kKnownLossCodeCount);
    for (std::size_t index = 0; index < codes.size(); ++index) {
        CHECK(!codes[index].empty());
        CHECK(is_known_loss_code(codes[index]));
        CHECK(std::find(codes.begin(), codes.begin() + static_cast<std::ptrdiff_t>(index),
                        codes[index]) == codes.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void test_format_capabilities_drive_loss() {
    const auto& canonical = format_profile(FormatProfileId::ply_melkor_canonical_v1);
    const auto& gltf = format_profile(FormatProfileId::gltf_khr_gaussian_splatting_rc_63770cc);
    CHECK(canonical.max_sh_degree > gltf.max_sh_degree);

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
    test_invalid_reports_and_approvals_fail_closed();
    test_loss_code_registry_is_complete_and_unique();
    test_format_capabilities_drive_loss();

    if (g_failures == 0) {
        std::printf("loss report: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "loss report: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
