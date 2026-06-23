// Fuzz the SPZ quaternion round-trip against rotation preservation.
//
// The single-value test in test_melkor_core.cpp proves the xyzw/wxyz reordering
// is correct for one known quaternion. This fuzz test exercises the full
// rotation space, including the edge cases that break naive implementations:
//   - identity and 180-degree flips
//   - antipodal quaternions (q and -q encode the same rotation)
//   - quaternions near the "smallest three" encoding's boundary cases
//
// The correct invariant is ROTATION PRESERVATION, not component equality,
// because the spz "smallest three" packing may canonicalize the sign of the
// largest component. We therefore compare rotation matrices, with a tolerance
// that accounts for the 9-bit quaternion quantization inside spz.
//
// Crucially, this test decodes with spz's OWN loader (loadSpz), not melkor's
// SpzDecoder. A symmetric double-bug (wrong order in BOTH encode and decode)
// would pass a melkor-only round trip but fail this test.

#include "melkor/gaussian_data.hpp"
#include "melkor/spz_encoder.hpp"

#ifdef MELKOR_HAS_SPZ
#include "load-spz.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <array>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr int N_RANDOM = 400;
constexpr int N_TRIALS_PER_QUAT = 1;
// spz packs quaternions at 9-bit precision via "smallest three"; the resulting
// rotation error is bounded but non-trivial. 0.02 rad (~1.1 deg) per axis is a
// generous-but-meaningful bar; if ordering were wrong the error would be ~pi/2.
constexpr float ROT_TOL = 0.02f;

float mat_diff(const float a[9], const float b[9]) {
    float max_abs = 0.0f;
    for (int i = 0; i < 9; ++i) {
        max_abs = std::max(max_abs, std::abs(a[i] - b[i]));
    }
    return max_abs;
}

// Build a unit quaternion from a rotation axis (arbitrary) and angle.
void axis_angle_quat(float ax, float ay, float az, float angle,
                     float& w, float& x, float& y, float& z) {
    float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len < 1e-9f) { w = 1; x = y = z = 0; return; }
    ax /= len; ay /= len; az /= len;
    float h = angle * 0.5f;
    float s = std::sin(h);
    w = std::cos(h); x = ax * s; y = ay * s; z = az * s;
}

}  // namespace

int main() {
#ifdef MELKOR_HAS_SPZ
    using namespace melkor;
    std::mt19937 rng(42);  // fixed seed for reproducibility
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::uniform_real_distribution<float> ang(0.0f, 6.2831853f);

    // Build the quaternion set: random unit quats + edge cases.
    std::vector<std::array<float, 4>> quats;
    for (int i = 0; i < N_RANDOM; ++i) {
        float w, x, y, z;
        axis_angle_quat(u(rng), u(rng), u(rng), ang(rng), w, x, y, z);
        quats.push_back({w, x, y, z});
    }
    // Edge cases.
    quats.push_back({1, 0, 0, 0});               // identity
    quats.push_back({0, 1, 0, 0});               // 180deg around X
    quats.push_back({0, 0, 1, 0});               // 180deg around Y
    quats.push_back({0, 0, 0, 1});               // 180deg around Z
    quats.push_back({0.5, 0.5, 0.5, 0.5});       // 120deg, symmetric
    quats.push_back({0.70710678f, 0.70710678f, 0, 0});  // 90deg X
    quats.push_back({0.9999f, 0.01f, 0.01f, 0.01f});    // near-identity

    int checked = 0;
    int over_tol = 0;
    float worst = 0.0f;

    for (const auto& q : quats) {
        // Encode through melkor (w-first).
        GaussianCloud cloud;
        GaussianSplat s{};
        s.x = s.y = s.z = 0.0f;
        s.f_dc_0 = s.f_dc_1 = s.f_dc_2 = 0.0f;
        s.opacity = 5.0f;
        s.scale_0 = s.scale_1 = s.scale_2 = -2.0f;
        s.rot_0 = q[0]; s.rot_1 = q[1]; s.rot_2 = q[2]; s.rot_3 = q[3];
        cloud.addSplat(s);
        cloud.setShDegree(0);

        SpzEncoder enc;
        SpzEncodeConfig cfg;
        std::vector<uint8_t> buf;
        auto res = enc.encodeToBuffer(buf, cloud, cfg);
        if (!res.success) {
            printf("FAIL: encode failed for quaternion\n");
            return 1;
        }

        // Decode with spz's OWN loader (canonical xyzw).
        spz::UnpackOptions uo;
        uo.to = spz::CoordinateSystem::RDF;
        auto spz_cloud = spz::loadSpz(buf.data(), static_cast<int>(buf.size()), uo);
        if (spz_cloud.numPoints != 1) {
            printf("FAIL: decoded %d points, expected 1\n", spz_cloud.numPoints);
            return 1;
        }

        // spz gives xyzw; back to melkor w-first for comparison.
        float dx = spz_cloud.rotations[0], dy = spz_cloud.rotations[1];
        float dz = spz_cloud.rotations[2], dw = spz_cloud.rotations[3];

        float m_in[9], m_out[9];
        utils::quatToRotationMatrix(q[0], q[1], q[2], q[3], m_in);
        utils::quatToRotationMatrix(dw, dx, dy, dz, m_out);

        float diff = mat_diff(m_in, m_out);
        worst = std::max(worst, diff);
        ++checked;
        if (diff > ROT_TOL) ++over_tol;
    }

    printf("[fuzz] SPZ quaternion rotation preservation\n");
    printf("    checked: %d quaternions (random + edge cases)\n", checked);
    printf("    worst matrix-element diff: %.6f\n", worst);
    printf("    over tolerance (%.4f): %d\n", ROT_TOL, over_tol);
    float pct = over_tol == 0 ? 0.0f : 100.0f * over_tol / checked;
    // Allow a small fraction to marginally exceed due to quantization, but the
    // bar is strict: a wrong ordering would push nearly ALL over by ~1.0.
    bool ok = (over_tol == 0) || (pct <= 1.0f && worst < 4 * ROT_TOL);
    printf(ok ? "  PASS: rotation preserved within quantization tolerance\n"
              : "  FAIL: too many rotations drifted beyond tolerance\n");
    return ok ? 0 : 1;
#else
    printf("[skip] SPZ quaternion fuzz (built without MELKOR_HAS_SPZ)\n");
    return 0;
#endif
}
