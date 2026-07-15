// Tests for glTF node transforms.
//
// These pin the two conversions that are easy to get subtly wrong: glTF's column-major matrix
// storage (a transposed read silently mirrors a scene) and the T*R*S / parent-chain composition
// (a wrong order puts the translation in the unrotated frame). The composition is cross-checked by
// applying it to points, which is order- and convention-independent.
//
// Self-contained (no external test framework).

#include "melkor/format/gltf_transform.hpp"

#include <cmath>
#include <cstdio>

namespace {

using namespace melkor;
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

bool approx(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

bool vec_approx(const math::Vec3& v, double x, double y, double z, double eps = 1e-9) {
    return approx(v[0], x, eps) && approx(v[1], y, eps) && approx(v[2], z, eps);
}

gltf::NodeDesc trs(std::array<double, 3> t, std::array<double, 4> r, std::array<double, 3> s) {
    gltf::NodeDesc n;
    n.translation = t;
    n.rotation = r;
    n.scale = s;
    return n;
}

void test_identity() {
    auto id = gltf::identity_transform();
    CHECK(vec_approx(gltf::apply_point(id, {1.0, 2.0, 3.0}), 1.0, 2.0, 3.0));
    // A default node (identity TRS) is the identity transform.
    gltf::NodeDesc def;
    auto lt = gltf::local_node_transform(def);
    CHECK(vec_approx(gltf::apply_point(lt, {4.0, 5.0, 6.0}), 4.0, 5.0, 6.0));
}

void test_translation_only() {
    auto n = trs({10.0, -3.0, 2.0}, {0.0, 0.0, 0.0, 1.0}, {1.0, 1.0, 1.0});
    auto t = gltf::local_node_transform(n);
    CHECK(vec_approx(gltf::apply_point(t, {1.0, 1.0, 1.0}), 11.0, -2.0, 3.0));
}

void test_scale_only() {
    auto n = trs({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}, {2.0, 3.0, 4.0});
    auto t = gltf::local_node_transform(n);
    CHECK(vec_approx(gltf::apply_point(t, {1.0, 1.0, 1.0}), 2.0, 3.0, 4.0));
}

void test_rotation_90_about_z() {
    // 90 degrees about +Z: quaternion (0,0,sin45,cos45). Maps +X -> +Y.
    const double s = std::sqrt(0.5);
    auto n = trs({0.0, 0.0, 0.0}, {0.0, 0.0, s, s}, {1.0, 1.0, 1.0});
    auto t = gltf::local_node_transform(n);
    CHECK(vec_approx(gltf::apply_point(t, {1.0, 0.0, 0.0}), 0.0, 1.0, 0.0, 1e-9));
    CHECK(vec_approx(gltf::apply_point(t, {0.0, 1.0, 0.0}), -1.0, 0.0, 0.0, 1e-9));
}

void test_trs_order_is_translate_rotate_scale() {
    // Scale then rotate then translate: point (1,0,0), scale x by 2 -> (2,0,0), rotate 90 about Z
    // -> (0,2,0), translate by (5,0,0) -> (5,2,0). If the order were wrong (e.g. translation before
    // rotation), the answer would differ.
    const double s = std::sqrt(0.5);
    auto n = trs({5.0, 0.0, 0.0}, {0.0, 0.0, s, s}, {2.0, 1.0, 1.0});
    auto t = gltf::local_node_transform(n);
    CHECK(vec_approx(gltf::apply_point(t, {1.0, 0.0, 0.0}), 5.0, 2.0, 0.0, 1e-9));
}

void test_matrix_form_is_column_major() {
    // glTF column-major matrix with translation (5,6,7) in the fourth column and a diag(2,3,4)
    // linear part. Column-major storage: [c0 | c1 | c2 | c3], each a column.
    gltf::NodeDesc n;
    n.matrix = std::array<double, 16>{
        2, 0, 0, 0,   // column 0
        0, 3, 0, 0,   // column 1
        0, 0, 4, 0,   // column 2
        5, 6, 7, 1};  // column 3 (translation)
    auto t = gltf::local_node_transform(n);
    CHECK(vec_approx(gltf::apply_point(t, {1.0, 1.0, 1.0}), 7.0, 9.0, 11.0));  // 2+5, 3+6, 4+7
    CHECK(vec_approx(t.translation, 5.0, 6.0, 7.0));
}

void test_matrix_off_diagonal_not_transposed() {
    // A shear that is NOT symmetric, so a transposed read would give a different answer. Column 0 is
    // (1, 0.5, 0): the linear map sends (1,0,0) -> (1, 0.5, 0). If read transposed it would send
    // (1,0,0) -> (1, 0, 0), missing the shear.
    gltf::NodeDesc n;
    n.matrix = std::array<double, 16>{
        1.0, 0.5, 0.0, 0.0,   // column 0
        0.0, 1.0, 0.0, 0.0,   // column 1
        0.0, 0.0, 1.0, 0.0,   // column 2
        0.0, 0.0, 0.0, 1.0};  // column 3
    auto t = gltf::local_node_transform(n);
    CHECK(vec_approx(gltf::apply_point(t, {1.0, 0.0, 0.0}), 1.0, 0.5, 0.0));
}

void test_compose_parent_child() {
    // Parent: translate by (10,0,0). Child: rotate 90 about Z. A point (1,0,0) under the child is
    // (0,1,0), then the parent translates to (10,1,0). Composition must equal that.
    const double s = std::sqrt(0.5);
    auto parent = gltf::local_node_transform(trs({10.0, 0.0, 0.0}, {0, 0, 0, 1}, {1, 1, 1}));
    auto child = gltf::local_node_transform(trs({0.0, 0.0, 0.0}, {0, 0, s, s}, {1, 1, 1}));
    auto global = gltf::compose(parent, child);
    CHECK(vec_approx(gltf::apply_point(global, {1.0, 0.0, 0.0}), 10.0, 1.0, 0.0, 1e-9));

    // Composition must equal applying child then parent to any point (definition cross-check).
    const math::Vec3 p{0.3, -0.7, 2.1};
    auto expected = gltf::apply_point(parent, gltf::apply_point(child, p));
    auto viaCompose = gltf::apply_point(global, p);
    CHECK(vec_approx(viaCompose, expected[0], expected[1], expected[2], 1e-9));
}

void test_compose_scaled_parent_translates_child() {
    // A scaling parent scales the child's translation: parent scale 2, child translate (1,0,0).
    // The child origin lands at parent.linear * (1,0,0) = (2,0,0).
    auto parent = gltf::local_node_transform(trs({0, 0, 0}, {0, 0, 0, 1}, {2, 2, 2}));
    auto child = gltf::local_node_transform(trs({1, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}));
    auto global = gltf::compose(parent, child);
    CHECK(vec_approx(gltf::apply_point(global, {0.0, 0.0, 0.0}), 2.0, 0.0, 0.0));
}

void test_degenerate_quaternion_falls_back_to_identity() {
    auto n = trs({0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1});  // zero quaternion
    auto t = gltf::local_node_transform(n);
    CHECK(vec_approx(gltf::apply_point(t, {1.0, 2.0, 3.0}), 1.0, 2.0, 3.0));
}

}  // namespace

int main() {
    test_identity();
    test_translation_only();
    test_scale_only();
    test_rotation_90_about_z();
    test_trs_order_is_translate_rotate_scale();
    test_matrix_form_is_column_major();
    test_matrix_off_diagonal_not_transposed();
    test_compose_parent_child();
    test_compose_scaled_parent_translates_child();
    test_degenerate_quaternion_falls_back_to_identity();

    if (g_failures == 0) {
        std::printf("gltf transform: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf transform: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
