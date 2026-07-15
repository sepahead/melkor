// Tests for the glTF extension policy.
//
// The rule being pinned: a required extension Melkor cannot honour makes the asset unreadable
// (reject), a supported required extension is fine (accept), and a used-but-not-required unsupported
// extension is safely ignored but reported. This is the P0-10 fix -- the old behaviour rejected
// required extensions too broadly.
//
// Self-contained (no external test framework).

#include "melkor/format/gltf_extensions.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace gltf = melkor::format::gltf;

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

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

void test_supported_predicate() {
    CHECK(gltf::is_supported_read_extension("KHR_gaussian_splatting"));
    CHECK(!gltf::is_supported_read_extension("KHR_draco_mesh_compression"));
    CHECK(!gltf::is_supported_read_extension(""));
}

void test_supported_required_is_accepted() {
    auto e = gltf::evaluate_extensions({"KHR_gaussian_splatting"}, {"KHR_gaussian_splatting"});
    CHECK(e.unsupported_required.empty());  // nothing to reject
    CHECK(e.ignored_used.empty());
}

void test_unsupported_required_is_rejected() {
    auto e = gltf::evaluate_extensions(
        {"KHR_gaussian_splatting", "KHR_draco_mesh_compression"},
        {"KHR_gaussian_splatting", "KHR_draco_mesh_compression"});
    CHECK(e.unsupported_required.size() == 1);
    CHECK(contains(e.unsupported_required, "KHR_draco_mesh_compression"));
    CHECK(e.ignored_used.empty());
}

void test_used_but_not_required_is_ignored_and_reported() {
    // EXT_meshopt is used but not required, and unsupported: ignore it, but report it.
    auto e = gltf::evaluate_extensions({"KHR_gaussian_splatting", "EXT_meshopt_compression"},
                                       {"KHR_gaussian_splatting"});
    CHECK(e.unsupported_required.empty());  // not required, so not a rejection
    CHECK(e.ignored_used.size() == 1);
    CHECK(contains(e.ignored_used, "EXT_meshopt_compression"));
}

void test_required_in_both_lists_counts_as_required() {
    // A required extension also (redundantly) listed as used must be treated as required, i.e. a
    // rejection, not an ignorable used one.
    auto e = gltf::evaluate_extensions({"KHR_materials_unlit"}, {"KHR_materials_unlit"});
    CHECK(e.unsupported_required.size() == 1);
    CHECK(e.ignored_used.empty());
}

void test_dedup_and_order() {
    // Duplicates must not produce duplicate findings, and the output order is deterministic.
    auto e = gltf::evaluate_extensions(
        {"B_ext", "A_ext", "A_ext"}, {"B_ext", "A_ext", "B_ext"});
    CHECK(e.unsupported_required.size() == 2);
    CHECK(e.unsupported_required[0] == "A_ext" && e.unsupported_required[1] == "B_ext");
    CHECK(e.ignored_used.empty());  // both are required
}

void test_empty() {
    auto e = gltf::evaluate_extensions({}, {});
    CHECK(e.unsupported_required.empty() && e.ignored_used.empty());
}

}  // namespace

int main() {
    test_supported_predicate();
    test_supported_required_is_accepted();
    test_unsupported_required_is_rejected();
    test_used_but_not_required_is_ignored_and_reported();
    test_required_in_both_lists_counts_as_required();
    test_dedup_and_order();
    test_empty();

    if (g_failures == 0) {
        std::printf("gltf extensions: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf extensions: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
