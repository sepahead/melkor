// Reference tests for the canonical math oracle.
//
// These pin the semantics that release blocker P0-17 is about: a scene transform must reshape a
// Gaussian's covariance, orientation, and scale together, not move only its mean. The headline
// test proves that transforming a Gaussian's shape through A Σ Aᵀ genuinely changes the
// covariance under a rotation, and that decomposing the result reproduces that covariance.
//
// Covariance is compared as a matrix (Frobenius norm of the difference), and quaternions by
// angular distance -- never by raw components, because a quaternion's components and an
// eigenvector basis's column signs are not unique, so component equality would be both too
// strict and, paradoxically, able to pass a wrong answer.
//
// Self-contained (no external test framework), matching the existing suite's convention.

#include "melkor/math/activation.hpp"
#include "melkor/math/covariance.hpp"
#include "melkor/math/quaternion.hpp"

#include <cmath>
#include <cstdio>

// MSVC does not define M_PI without _USE_MATH_DEFINES; provide it so the test is portable.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

using namespace melkor;
using namespace melkor::math;

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

bool approx(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// Frobenius distance between two 3x3 matrices.
double mat_distance(const Mat3& a, const Mat3& b) {
    double sum = 0.0;
    for (int i = 0; i < 9; ++i) {
        const double d = a[i] - b[i];
        sum += d * d;
    }
    return std::sqrt(sum);
}

Mat3 matmul(const Mat3& a, const Mat3& b) {
    Mat3 c{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) s += a[i * 3 + k] * b[k * 3 + j];
            c[i * 3 + j] = s;
        }
    return c;
}

Mat3 transpose(const Mat3& m) {
    return Mat3{m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

void test_activation_roundtrips() {
    // sigmoid/logit are inverses on the interior.
    for (float logit : {-8.0f, -1.0f, 0.0f, 0.5f, 3.0f}) {
        auto p = sigmoid_from_logit(logit);
        CHECK(p.has_value());
        auto back = logit_from_probability(p.value());
        CHECK(back.has_value());
        CHECK(approx(back.value(), logit, 1e-4));
    }

    // The sigmoid is stable at large magnitude rather than overflowing.
    auto big = sigmoid_from_logit(500.0f);
    CHECK(big.has_value() && approx(big.value(), 1.0, 1e-6));
    auto small = sigmoid_from_logit(-500.0f);
    CHECK(small.has_value() && approx(small.value(), 0.0, 1e-6));

    // The probability endpoints have infinite logits and must be rejected, not clamped.
    CHECK(!logit_from_probability(0.0f).has_value());
    CHECK(!logit_from_probability(1.0f).has_value());

    // log/linear scale are inverses.
    for (float linear : {0.001f, 0.1f, 1.0f, 12.5f}) {
        auto log = log_scale_from_linear(linear);
        CHECK(log.has_value());
        auto back = linear_scale_from_log(log.value());
        CHECK(back.has_value());
        CHECK(approx(back.value(), linear, 1e-5));
    }

    // A non-positive linear scale, and an out-of-range log-scale, both fail rather than
    // producing a nonsense value. This is the double-activation guard.
    CHECK(!log_scale_from_linear(0.0f).has_value());
    CHECK(!log_scale_from_linear(-1.0f).has_value());
    CHECK(!linear_scale_from_log(1000.0f).has_value());  // would overflow to inf
}

// ---------------------------------------------------------------------------
// Quaternion
// ---------------------------------------------------------------------------

void test_quaternion_matrix_roundtrip() {
    // A batch of rotations, including the numerically hard 180-degree cases where the naive
    // matrix->quaternion formula loses all precision.
    const Quat rotations[] = {
        identity_quat(),
        Quat{0.0, 0.0, std::sin(M_PI / 4), std::cos(M_PI / 4)},   // 90 deg about z
        Quat{std::sin(M_PI / 4), 0.0, 0.0, std::cos(M_PI / 4)},   // 90 deg about x
        Quat{1.0, 0.0, 0.0, 0.0},                                 // 180 deg about x (w = 0)
        Quat{0.0, 1.0, 0.0, 0.0},                                 // 180 deg about y
        Quat{0.5, 0.5, 0.5, 0.5},                                 // 120 deg about (1,1,1)
    };

    for (const Quat& q0 : rotations) {
        auto qn = normalize(q0);
        CHECK(qn.has_value());
        const Mat3 m = to_matrix(qn.value());
        auto q1 = from_matrix(m);
        CHECK(q1.has_value());
        // Compare as rotations, not components. angular_distance treats q and -q as identical.
        CHECK(angular_distance(qn.value(), q1.value()) < 1e-6);
    }
}

void test_quaternion_sign_equivalence() {
    Quat q{0.0, 0.0, std::sin(M_PI / 4), std::cos(M_PI / 4)};
    Quat neg{-q.x, -q.y, -q.z, -q.w};
    // q and -q are the same rotation.
    CHECK(angular_distance(q, neg) < 1e-9);
}

void test_zero_quaternion_is_rejected_not_promoted() {
    // A zero quaternion has no direction. It must fail, not silently become identity -- that
    // would invent an orientation the data never had.
    auto r = normalize(Quat{0.0, 0.0, 0.0, 0.0});
    CHECK(!r.has_value());
    CHECK(r.diagnostics()[0].code == "MK1202_ZERO_QUATERNION");
}

void test_from_frame() {
    // The canonical axes give identity.
    auto q = from_frame({1, 0, 0}, {0, 1, 0}, {0, 0, 1});
    CHECK(q.has_value());
    CHECK(angular_distance(q.value(), identity_quat()) < 1e-9);

    // A skewed (non-orthonormal) frame is rejected rather than forced into a non-rotation.
    auto bad = from_frame({1, 0, 0}, {0.5, 0.5, 0}, {0, 0, 1});
    CHECK(!bad.has_value());
}

// ---------------------------------------------------------------------------
// Covariance -- the P0-17 heart
// ---------------------------------------------------------------------------

void test_covariance_is_symmetric_and_decomposes() {
    const Quat rot = [] {
        auto n = normalize(Quat{0.2, -0.5, 0.3, 0.8});
        return n.value();
    }();
    const Vec3 scale{2.0, 0.5, 1.0};  // anisotropic on purpose

    auto sigma = covariance_from_rotation_scale(rot, scale);
    CHECK(sigma.has_value());
    const Mat3& s = sigma.value();

    // Symmetric by construction.
    CHECK(approx(s[1], s[3]) && approx(s[2], s[6]) && approx(s[5], s[7]));

    // Decompose and rebuild: the recovered (rotation, scale) must reproduce the SAME covariance.
    // We compare the covariance, not the quaternion, because the decomposition of an anisotropic
    // Gaussian is unique only up to eigenvector sign/order, which the covariance is invariant to.
    auto rs = rotation_scale_from_covariance(s);
    CHECK(rs.has_value());
    auto sigma2 = covariance_from_rotation_scale(rs.value().rotation, rs.value().scale);
    CHECK(sigma2.has_value());
    CHECK(mat_distance(s, sigma2.value()) < 1e-9);
}

void test_affine_transform_reshapes_covariance() {
    // THE headline. An anisotropic Gaussian, transformed by a rotation, must have its covariance
    // reshaped as A Σ Aᵀ. Prove both that the result equals A Σ Aᵀ, and -- crucially -- that it
    // actually DIFFERS from the untransformed covariance, so a "position-only" transform (the
    // bug) would give a demonstrably wrong shape.
    const Quat rot = identity_quat();
    const Vec3 scale{3.0, 1.0, 1.0};  // a long, thin Gaussian along x

    auto sigma0 = covariance_from_rotation_scale(rot, scale);
    CHECK(sigma0.has_value());

    // A: 90-degree rotation about z. This should swap the long axis from x to y.
    const Mat3 A{0.0, -1.0, 0.0,
                 1.0, 0.0, 0.0,
                 0.0, 0.0, 1.0};

    auto rs = affine_transform_gaussian(A, rot, scale);
    CHECK(rs.has_value());
    auto sigma_out = covariance_from_rotation_scale(rs.value().rotation, rs.value().scale);
    CHECK(sigma_out.has_value());

    // The correct reference: A Σ Aᵀ computed directly.
    const Mat3 reference = matmul(matmul(A, sigma0.value()), transpose(A));
    CHECK(mat_distance(sigma_out.value(), reference) < 1e-9);

    // And it must be a DIFFERENT matrix from the original -- otherwise the transform did nothing,
    // which is exactly the position-only bug. The long axis moved from x (index 0) to y (index 4).
    CHECK(mat_distance(sigma_out.value(), sigma0.value()) > 1.0);
    CHECK(approx(sigma_out.value()[4], 9.0, 1e-6));  // variance along y is now 3² = 9
    CHECK(approx(sigma_out.value()[0], 1.0, 1e-6));  // variance along x is now 1
}

void test_affine_transform_nonuniform_scale() {
    // A non-uniform scale is where "just scale the components" goes wrong for a rotated Gaussian.
    // Verify the covariance path handles it: Σ' = A Σ Aᵀ exactly.
    const Quat rot = [] {
        auto n = normalize(Quat{0.0, 0.0, std::sin(M_PI / 6), std::cos(M_PI / 6)});  // 60 deg z
        return n.value();
    }();
    const Vec3 scale{2.0, 1.0, 1.0};

    auto sigma0 = covariance_from_rotation_scale(rot, scale);
    CHECK(sigma0.has_value());

    const Mat3 A{2.0, 0.0, 0.0,
                 0.0, 3.0, 0.0,
                 0.0, 0.0, 1.0};  // non-uniform scale

    auto rs = affine_transform_gaussian(A, rot, scale);
    CHECK(rs.has_value());
    auto sigma_out = covariance_from_rotation_scale(rs.value().rotation, rs.value().scale);
    CHECK(sigma_out.has_value());

    const Mat3 reference = matmul(matmul(A, sigma0.value()), transpose(A));
    CHECK(mat_distance(sigma_out.value(), reference) < 1e-8);
}

void test_affine_transform_reflection() {
    // A reflection (negative determinant) must still produce a valid proper-rotation quaternion
    // with the correct covariance, because the covariance is invariant to reflection of an axis.
    const Quat rot = identity_quat();
    const Vec3 scale{2.0, 1.0, 0.5};

    auto sigma0 = covariance_from_rotation_scale(rot, scale);
    CHECK(sigma0.has_value());

    const Mat3 A{-1.0, 0.0, 0.0,
                 0.0, 1.0, 0.0,
                 0.0, 0.0, 1.0};  // reflect across x, determinant -1

    auto rs = affine_transform_gaussian(A, rot, scale);
    CHECK(rs.has_value());
    // The recovered rotation must be a valid unit quaternion (a proper rotation).
    CHECK(is_unit(rs.value().rotation));
    auto sigma_out = covariance_from_rotation_scale(rs.value().rotation, rs.value().scale);
    CHECK(sigma_out.has_value());

    const Mat3 reference = matmul(matmul(A, sigma0.value()), transpose(A));
    CHECK(mat_distance(sigma_out.value(), reference) < 1e-8);
}

void test_singular_transform_is_rejected() {
    // A transform that collapses the Gaussian to a plane has no valid positive-scale
    // decomposition and must fail rather than emit a degenerate scale.
    const Mat3 A{1.0, 0.0, 0.0,
                 0.0, 1.0, 0.0,
                 0.0, 0.0, 0.0};  // determinant 0
    auto rs = affine_transform_gaussian(A, identity_quat(), Vec3{1.0, 1.0, 1.0});
    CHECK(!rs.has_value());
    CHECK(rs.diagnostics()[0].code == "MK1306_SINGULAR_TRANSFORM");
}

void test_non_covariance_is_rejected() {
    // A symmetric matrix with a substantial negative eigenvalue is not a covariance.
    const Mat3 not_psd{-5.0, 0.0, 0.0,
                       0.0, 1.0, 0.0,
                       0.0, 0.0, 1.0};
    auto rs = rotation_scale_from_covariance(not_psd);
    CHECK(!rs.has_value());
    CHECK(rs.diagnostics()[0].code == "MK1304_NOT_POSITIVE_SEMIDEFINITE");
}

}  // namespace

int main() {
    test_activation_roundtrips();

    test_quaternion_matrix_roundtrip();
    test_quaternion_sign_equivalence();
    test_zero_quaternion_is_rejected_not_promoted();
    test_from_frame();

    test_covariance_is_symmetric_and_decomposes();
    test_affine_transform_reshapes_covariance();
    test_affine_transform_nonuniform_scale();
    test_affine_transform_reflection();
    test_singular_transform_is_rejected();
    test_non_covariance_is_rejected();

    if (g_failures == 0) {
        std::printf("math oracle: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "math oracle: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
