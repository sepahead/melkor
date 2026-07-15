// Tests for the KHR_gaussian_splatting layout core.
//
// These pin the parts of the extension that are subtle and version-critical against the vendored
// spec text (third_party/specs/KHR_gaussian_splatting/63770cc70a37): the SH attribute naming and
// m-ordering, the exact `C` matrix, and the two defined colour spaces. If Melkor's understanding
// of the wire format ever drifts from the pinned spec, one of these fails.
//
// Self-contained (no external test framework).

#include "melkor/format/gltf_khr.hpp"
#include "melkor/math/covariance.hpp"
#include "melkor/math/quaternion.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace melkor;
namespace khr = melkor::format::khr;

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

bool approx(double a, double b, double eps = 1e-12) { return std::fabs(a - b) <= eps; }

void test_coefficient_counts() {
    // The spec: degree l has 2l+1 coefficients; the totals are the perfect squares.
    CHECK(khr::sh_coefficients_at_degree(0) == 1);
    CHECK(khr::sh_coefficients_at_degree(1) == 3);
    CHECK(khr::sh_coefficients_at_degree(2) == 5);
    CHECK(khr::sh_coefficients_at_degree(3) == 7);
    CHECK(khr::sh_total_coefficients(0) == 1);
    CHECK(khr::sh_total_coefficients(1) == 4);
    CHECK(khr::sh_total_coefficients(2) == 9);
    CHECK(khr::sh_total_coefficients(3) == 16);   // 45 SH floats = 15 coefficients * 3 channels + DC
    CHECK(khr::sh_total_coefficients(4) == 25);
    // The pinned profile ceiling is degree 3.
    CHECK(khr::kMaxProfileShDegree == 3);
}

void test_attribute_naming() {
    CHECK(khr::sh_attribute({0, 0}) == "KHR_gaussian_splatting:SH_DEGREE_0_COEF_0");
    CHECK(khr::sh_attribute({1, 2}) == "KHR_gaussian_splatting:SH_DEGREE_1_COEF_2");
    CHECK(khr::sh_attribute({3, 6}) == "KHR_gaussian_splatting:SH_DEGREE_3_COEF_6");
    CHECK(std::string(khr::kAttrPosition) == "POSITION");
    CHECK(std::string(khr::kAttrRotation) == "KHR_gaussian_splatting:ROTATION");
    CHECK(std::string(khr::kAttrScale) == "KHR_gaussian_splatting:SCALE");
    CHECK(std::string(khr::kAttrOpacity) == "KHR_gaussian_splatting:OPACITY");
}

void test_attribute_parse_roundtrip_and_rejects() {
    for (std::uint32_t d = 0; d <= 4; ++d) {
        for (std::uint32_t n = 0; n < khr::sh_coefficients_at_degree(d); ++n) {
            auto parsed = khr::parse_sh_attribute(khr::sh_attribute({d, n}));
            CHECK(parsed.has_value());
            CHECK(parsed->degree == d && parsed->coef == n);
        }
    }
    // Not an SH semantic.
    CHECK(!khr::parse_sh_attribute("POSITION").has_value());
    CHECK(!khr::parse_sh_attribute("KHR_gaussian_splatting:SCALE").has_value());
    CHECK(!khr::parse_sh_attribute("").has_value());
    // Malformed numerics and trailing junk.
    CHECK(!khr::parse_sh_attribute("KHR_gaussian_splatting:SH_DEGREE__COEF_0").has_value());
    CHECK(!khr::parse_sh_attribute("KHR_gaussian_splatting:SH_DEGREE_1_COEF_").has_value());
    CHECK(!khr::parse_sh_attribute("KHR_gaussian_splatting:SH_DEGREE_01_COEF_0").has_value());
    CHECK(!khr::parse_sh_attribute("KHR_gaussian_splatting:SH_DEGREE_1_COEF_0x").has_value());
    // n out of range for the degree: degree 1 has only COEF_0..2, so COEF_3 is invalid.
    CHECK(!khr::parse_sh_attribute("KHR_gaussian_splatting:SH_DEGREE_1_COEF_3").has_value());
    CHECK(!khr::parse_sh_attribute("KHR_gaussian_splatting:SH_DEGREE_0_COEF_1").has_value());
}

void test_flat_address_mapping() {
    // Flat coefficient-major index <-> (degree, coef) must round-trip over the whole pyramid, and
    // match the closed forms flat = degree^2 + coef.
    for (std::size_t flat = 0; flat < 25; ++flat) {
        auto addr = khr::sh_flat_to_address(flat);
        CHECK(khr::sh_address_to_flat(addr) == flat);
        CHECK(addr.coef < khr::sh_coefficients_at_degree(addr.degree));
    }
    // Spot-check the boundaries where floor(sqrt) matters most.
    CHECK(khr::sh_flat_to_address(0).degree == 0);
    CHECK(khr::sh_flat_to_address(1).degree == 1 && khr::sh_flat_to_address(1).coef == 0);
    CHECK(khr::sh_flat_to_address(3).degree == 1 && khr::sh_flat_to_address(3).coef == 2);
    CHECK(khr::sh_flat_to_address(4).degree == 2 && khr::sh_flat_to_address(4).coef == 0);
    CHECK(khr::sh_flat_to_address(8).degree == 2 && khr::sh_flat_to_address(8).coef == 4);
    CHECK(khr::sh_flat_to_address(9).degree == 3 && khr::sh_flat_to_address(9).coef == 0);
    CHECK(khr::sh_flat_to_address(15).degree == 3 && khr::sh_flat_to_address(15).coef == 6);
    CHECK(khr::sh_flat_to_address(16).degree == 4 && khr::sh_flat_to_address(16).coef == 0);
    CHECK(khr::sh_flat_to_address(24).degree == 4 && khr::sh_flat_to_address(24).coef == 8);
}

void test_color_space() {
    CHECK(std::string(khr::to_string(khr::ColorSpace::srgb_rec709_display)) == "srgb_rec709_display");
    CHECK(std::string(khr::to_string(khr::ColorSpace::lin_rec709_display)) == "lin_rec709_display");
    CHECK(khr::color_space_from_string("srgb_rec709_display") == khr::ColorSpace::srgb_rec709_display);
    CHECK(khr::color_space_from_string("lin_rec709_display") == khr::ColorSpace::lin_rec709_display);
    // An unknown-but-schema-legal string is not interpretable: nullopt, not a silent sRGB default.
    CHECK(!khr::color_space_from_string("aces_ap0").has_value());
    CHECK(!khr::color_space_from_string("").has_value());
    CHECK(!khr::color_space_from_string("SRGB_REC709_DISPLAY").has_value());  // case-sensitive
}

void test_c_matrix_matches_spec_literal() {
    // Verify c_matrix against the spec's explicit element-wise C for a non-trivial rotation and
    // anisotropic scale. Using the spec's own formula so this is a genuine cross-check, not a
    // restatement of the implementation.
    math::Quat q{0.1, -0.2, 0.3, 0.9};  // x,y,z,w (not yet unit)
    auto qn = math::normalize(q);
    CHECK(qn.has_value());
    q = qn.value();
    const double qx = q.x, qy = q.y, qz = q.z, qw = q.w;
    const math::Vec3 s{2.0, 0.5, 1.3};
    const double sx = s[0], sy = s[1], sz = s[2];

    math::Mat3 expected{
        sx * (1 - 2 * (qy * qy + qz * qz)), sy * (2 * (qx * qy - qw * qz)), sz * (2 * (qx * qz + qw * qy)),
        sx * (2 * (qx * qy + qw * qz)), sy * (1 - 2 * (qx * qx + qz * qz)), sz * (2 * (qy * qz - qw * qx)),
        sx * (2 * (qx * qz - qw * qy)), sy * (2 * (qy * qz + qw * qx)), sz * (1 - 2 * (qx * qx + qy * qy)),
    };
    auto c = khr::c_matrix(q, s);
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(approx(c[i], expected[i], 1e-12));
    }
}

void test_c_matrix_covariance_consistency() {
    // C C^T must equal the canonical covariance R diag(s^2) R^T, so the writer/reader building
    // covariance via the spec's C agrees with the math oracle used everywhere else.
    math::Quat q{0.1, -0.2, 0.3, 0.9};
    q = math::normalize(q).value();
    const math::Vec3 s{2.0, 0.5, 1.3};
    auto c = khr::c_matrix(q, s);

    // (C C^T)[a][b] = sum_j C[a][j] C[b][j]
    math::Mat3 cct{};
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            double sum = 0.0;
            for (int j = 0; j < 3; ++j) {
                sum += c[static_cast<std::size_t>(a) * 3 + j] * c[static_cast<std::size_t>(b) * 3 + j];
            }
            cct[static_cast<std::size_t>(a) * 3 + b] = sum;
        }
    }
    auto sigma = math::covariance_from_rotation_scale(q, s);
    CHECK(sigma.has_value());
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(approx(cct[i], sigma.value()[i], 1e-12));
    }
    // And the covariance must be symmetric, as both constructions guarantee.
    CHECK(approx(cct[1], cct[3], 1e-12));
    CHECK(approx(cct[2], cct[6], 1e-12));
    CHECK(approx(cct[5], cct[7], 1e-12));
}

}  // namespace

int main() {
    test_coefficient_counts();
    test_attribute_naming();
    test_attribute_parse_roundtrip_and_rejects();
    test_flat_address_mapping();
    test_color_space();
    test_c_matrix_matches_spec_literal();
    test_c_matrix_covariance_consistency();

    if (g_failures == 0) {
        std::printf("gltf khr layout: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf khr layout: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
