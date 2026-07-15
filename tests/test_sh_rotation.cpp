// Tests for real spherical-harmonic rotation.
//
// A wrong SH rotation corrupts colour silently, so this suite verifies correctness from several
// independent angles rather than trusting the construction:
//   - the band matrices are orthogonal (SH rotation preserves inner products);
//   - identity rotation gives identity matrices;
//   - rotating then inverse-rotating is the identity (round trip);
//   - composing two rotations equals rotating by their product;
//   - the defining identity holds: evaluating the rotated coefficients at a direction equals
//     evaluating the source coefficients at the inverse-rotated direction, for many directions;
//   - a directional lobe rotates to the geometrically expected place (a +X lobe under a 90 deg
//     rotation about Z lands on +Y).
//
// Self-contained (no external test framework).

#include "melkor/math/sh_rotation.hpp"
#include "melkor/math/quaternion.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace melkor;
using melkor::math::Mat3;
using melkor::math::Quat;
using melkor::math::Vec3;

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

// The same 3DGS real-SH basis the rotation is built against, re-implemented here independently so
// the "defining identity" test is a genuine cross-check.
std::array<double, 16> basis(const Vec3& d) {
    const double C0 = 0.28209479177387814, C1 = 0.4886025119029199;
    const double C2[5] = {1.0925484305920792, -1.0925484305920792, 0.31539156525252005,
                          -1.0925484305920792, 0.5462742152960396};
    const double C3[7] = {-0.5900435899266435, 2.890611442640554,  -0.4570457994644658,
                          0.3731763325901154,  -0.4570457994644658, 1.445305721320277,
                          -0.5900435899266435};
    const double x = d[0], y = d[1], z = d[2], xx = x * x, yy = y * y, zz = z * z;
    return {C0,
            -C1 * y,
            C1 * z,
            -C1 * x,
            C2[0] * x * y,
            C2[1] * y * z,
            C2[2] * (2 * zz - xx - yy),
            C2[3] * x * z,
            C2[4] * (xx - yy),
            C3[0] * y * (3 * xx - yy),
            C3[1] * x * y * z,
            C3[2] * y * (4 * zz - xx - yy),
            C3[3] * z * (2 * zz - 3 * xx - 3 * yy),
            C3[4] * x * (4 * zz - xx - yy),
            C3[5] * z * (xx - yy),
            C3[6] * x * (xx - 3 * yy)};
}

Vec3 mul(const Mat3& m, const Vec3& v) {
    return Vec3{m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

Mat3 matmul(const Mat3& a, const Mat3& b) {
    Mat3 c{};
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            for (int k = 0; k < 3; ++k) c[r * 3 + col] += a[r * 3 + k] * b[k * 3 + col];
    return c;
}

Mat3 rot_z(double a) {
    return Mat3{std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a), 0, 0, 0, 1};
}

void test_is_proper_rotation() {
    CHECK(math::is_proper_rotation(Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1}));
    CHECK(math::is_proper_rotation(rot_z(0.7)));
    CHECK(!math::is_proper_rotation(Mat3{2, 0, 0, 0, 1, 0, 0, 0, 1}));       // scale
    CHECK(!math::is_proper_rotation(Mat3{-1, 0, 0, 0, 1, 0, 0, 0, 1}));      // reflection (det -1)
    CHECK(!math::is_proper_rotation(Mat3{1, 0.3, 0, 0, 1, 0, 0, 0, 1}));     // shear
    // Degree above 3 is rejected.
    CHECK(!math::ShRotation::create(rot_z(0.5), 4).has_value());
}

void test_band_matrices_orthogonal() {
    auto q = math::normalize(Quat{0.2, -0.5, 0.3, 0.8}).value();
    auto rot = math::ShRotation::create(math::to_matrix(q), 3);
    CHECK(rot.has_value());
    if (!rot.has_value()) return;
    for (std::uint32_t l = 1; l <= 3; ++l) {
        const auto& m = rot.value().band(l);
        const std::size_t k = 2 * l + 1;
        // M M^T == I.
        for (std::size_t a = 0; a < k; ++a) {
            for (std::size_t b = 0; b < k; ++b) {
                double s = 0.0;
                for (std::size_t c = 0; c < k; ++c) s += m[a * k + c] * m[b * k + c];
                CHECK(approx(s, a == b ? 1.0 : 0.0, 1e-7));
            }
        }
    }
}

void test_identity_is_identity() {
    auto rot = math::ShRotation::create(Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1}, 3);
    CHECK(rot.has_value());
    if (!rot.has_value()) return;
    for (std::uint32_t l = 1; l <= 3; ++l) {
        const auto& m = rot.value().band(l);
        const std::size_t k = 2 * l + 1;
        for (std::size_t a = 0; a < k; ++a)
            for (std::size_t b = 0; b < k; ++b) CHECK(approx(m[a * k + b], a == b ? 1.0 : 0.0, 1e-7));
    }
}

void test_defining_identity() {
    // For random directions d: sum_k c'_k Y_k(d) == sum_k c_k Y_k(R^{-1} d), where c' = M c.
    // Use a "one-hot" coefficient vector per band index to check every column.
    auto q = math::normalize(Quat{-0.1, 0.4, -0.6, 0.7}).value();
    const Mat3 R = math::to_matrix(q);
    const Mat3 Rinv{R[0], R[3], R[6], R[1], R[4], R[7], R[2], R[5], R[8]};
    auto rot = math::ShRotation::create(R, 3);
    CHECK(rot.has_value());
    if (!rot.has_value()) return;

    const std::vector<Vec3> dirs = {{0.3, -0.4, 0.86602540378}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                                    {-0.5, 0.5, 0.70710678}};
    for (std::uint32_t l = 1; l <= 3; ++l) {
        const std::size_t k = 2 * l + 1, base = l * l;
        const auto& m = rot.value().band(l);
        for (std::size_t src = 0; src < k; ++src) {
            // c is one-hot at (base+src); c' = column `src` of M -> row m of M gives c'[m].
            for (const auto& d : dirs) {
                const auto yd = basis(d);
                const auto yrd = basis(mul(Rinv, d));
                double lhs = 0.0;  // sum_m c'_m Y_m(d)
                for (std::size_t row = 0; row < k; ++row) lhs += m[row * k + src] * yd[base + row];
                const double rhs = yrd[base + src];  // sum_k c_k Y_k(R^-1 d) with c one-hot
                CHECK(approx(lhs, rhs, 1e-6));
            }
        }
    }
}

void test_round_trip_and_composition() {
    const Mat3 R1 = rot_z(0.6);
    const Mat3 R2 = math::to_matrix(math::normalize(Quat{0.3, 0.1, -0.2, 0.9}).value());
    const Mat3 R1inv{R1[0], R1[3], R1[6], R1[1], R1[4], R1[7], R1[2], R1[5], R1[8]};

    auto a = math::ShRotation::create(R1, 3).value();
    auto ainv = math::ShRotation::create(R1inv, 3).value();
    auto comp = math::ShRotation::create(matmul(R2, R1), 3).value();
    auto b = math::ShRotation::create(R2, 3).value();

    for (std::uint32_t l = 1; l <= 3; ++l) {
        const std::size_t k = 2 * l + 1;
        const auto& ma = a.band(l);
        const auto& mai = ainv.band(l);
        const auto& mb = b.band(l);
        const auto& mc = comp.band(l);
        for (std::size_t r = 0; r < k; ++r) {
            for (std::size_t c = 0; c < k; ++c) {
                // Round trip: M(R1^-1) M(R1) == I.
                double rt = 0.0;
                for (std::size_t j = 0; j < k; ++j) rt += mai[r * k + j] * ma[j * k + c];
                CHECK(approx(rt, r == c ? 1.0 : 0.0, 1e-6));
                // Composition: M(R2 R1) == M(R2) M(R1).
                double cp = 0.0;
                for (std::size_t j = 0; j < k; ++j) cp += mb[r * k + j] * ma[j * k + c];
                CHECK(approx(cp, mc[r * k + c], 1e-6));
            }
        }
    }
}

void test_lobe_rotates_to_expected_direction() {
    // Build a degree-1 coefficient block whose radiance peaks toward +X, rotate 90 deg about Z
    // (+X -> +Y), and confirm the rotated block now peaks toward +Y.
    auto rot = math::ShRotation::create(rot_z(M_PI / 2.0), 3).value();
    // Degree-1 coefficients that reconstruct the direction +X: basis for +X is
    // [C0, 0, 0, -C1]; a block proportional to the +X basis peaks at +X. Use 1 channel.
    const double C0 = 0.28209479177387814, C1 = 0.4886025119029199;
    std::vector<float> block(16, 0.0f);
    block[0] = static_cast<float>(C0);
    block[3] = static_cast<float>(-C1);  // m=+1 coefficient for +X

    auto radiance = [&](const std::vector<float>& b, const Vec3& d) {
        const auto y = basis(d);
        double s = 0.0;
        for (int i = 0; i < 16; ++i) s += b[i] * y[i];
        return s;
    };
    const double before_x = radiance(block, {1, 0, 0});
    rot.rotate_block(block.data(), 1);
    const double after_y = radiance(block, {0, 1, 0});
    const double after_x = radiance(block, {1, 0, 0});
    // The peak moved from +X to +Y.
    CHECK(approx(after_y, before_x, 1e-5));
    CHECK(after_y > after_x + 0.1);
}

void test_rotate_block_rgb_channels() {
    // Three channels rotate independently but share the band matrices; a per-channel scale must be
    // preserved. Give the R/G/B channels distinct scales of the same +X degree-1 lobe.
    auto rot = math::ShRotation::create(rot_z(M_PI / 2.0), 3).value();
    const double C1 = 0.4886025119029199;
    std::vector<float> block(16 * 3, 0.0f);
    // coefficient 3 (m=+1) for each channel, scaled 1, 2, 3.
    block[3 * 3 + 0] = static_cast<float>(-C1 * 1.0);
    block[3 * 3 + 1] = static_cast<float>(-C1 * 2.0);
    block[3 * 3 + 2] = static_cast<float>(-C1 * 3.0);
    rot.rotate_block(block.data(), 3);
    // After a 90 deg Z rotation the +X lobe becomes a +Y lobe: coefficient 1 (m=-1, the -y term).
    // The ratio between channels (1:2:3) must be preserved.
    const float g_over_r = block[1 * 3 + 1] / block[1 * 3 + 0];
    const float b_over_r = block[1 * 3 + 2] / block[1 * 3 + 0];
    CHECK(approx(g_over_r, 2.0, 1e-4));
    CHECK(approx(b_over_r, 3.0, 1e-4));
    // DC (coefficient 0) stays zero (untouched, was zero).
    CHECK(block[0] == 0.0f && block[1 * 3 + 0] != 0.0f);
}

void test_rotate_block_matches_band_matrices_all_degrees() {
    // rotate_block does the per-band gather/scatter (with a fixed-size `double in[7]`) for degrees
    // up to 3. Cross-check its output on a non-zero degree-3 block against a manual application of
    // each band matrix, so a band>=2 indexing bug is caught at the value level (not just degree 1).
    auto q = math::normalize(Quat{0.2, -0.3, 0.5, 0.7}).value();
    auto rot = math::ShRotation::create(math::to_matrix(q), 3);
    CHECK(rot.has_value());
    if (!rot.has_value()) return;

    std::vector<float> block(16), expected(16);
    for (int i = 0; i < 16; ++i) block[i] = expected[i] = static_cast<float>(i + 1) * 0.37f;
    // Expected: DC unchanged; each higher band rotated by its own matrix.
    for (std::uint32_t l = 1; l <= 3; ++l) {
        const std::size_t k = 2 * l + 1, base = l * l;
        const auto& m = rot.value().band(l);
        std::vector<double> in(k);
        for (std::size_t j = 0; j < k; ++j) in[j] = expected[base + j];
        for (std::size_t r = 0; r < k; ++r) {
            double acc = 0.0;
            for (std::size_t c = 0; c < k; ++c) acc += m[r * k + c] * in[c];
            expected[base + r] = static_cast<float>(acc);
        }
    }
    rot.value().rotate_block(block.data(), 1);
    for (int i = 0; i < 16; ++i) CHECK(approx(block[i], expected[i], 1e-5));
    // The DC term is untouched.
    CHECK(approx(block[0], 0.37f, 1e-6));
}

}  // namespace

int main() {
    test_is_proper_rotation();
    test_rotate_block_matches_band_matrices_all_degrees();
    test_band_matrices_orthogonal();
    test_identity_is_identity();
    test_defining_identity();
    test_round_trip_and_composition();
    test_lobe_rotates_to_expected_direction();
    test_rotate_block_rgb_channels();

    if (g_failures == 0) {
        std::printf("sh rotation: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "sh rotation: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
