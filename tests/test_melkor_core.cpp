// Melkor core correctness tests.
//
// These are deliberately self-contained (no external test framework): each
// case builds up an in-memory input, runs a Melkor core routine, and asserts a
// geometric/encoding property. They exist because the previous CI "test" step
// was only `melkor --info`, which exercises no actual conversion logic.
//
// Coverage:
//   1. SPZ quaternion round-trip against the canonical spz decoder (proves the
//      xyzw/wxyz reordering is correct and not a symmetric double-bug).
//   2. Header-driven PLY reader against three layouts: 3DGS ascii, plain
//      red/green/blue ascii, and melkor-written binary.
//   3. PCA normal estimation on a sphere surface (normals should align with the
//      radial direction).
//   4. SH C0 constant round-trip (rgbToShDc / shDcToRgb are inverses).

#include "melkor/enhanced_converter.hpp"
#include "melkor/gaussian_data.hpp"
#include "melkor/ply_writer.hpp"

#ifdef MELKOR_HAS_SPZ
#include "melkor/spz_encoder.hpp"
#include "load-spz.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (cond) {
        printf("  PASS: %s\n", msg);
    } else {
        printf("  FAIL: %s\n", msg);
        ++g_failures;
    }
}

// ---- Test 1: SPZ quaternion order -----------------------------------------
#ifdef MELKOR_HAS_SPZ
bool test_spz_quaternion_order() {
    printf("[test] SPZ quaternion order against canonical decoder\n");
    using namespace melkor;
    GaussianCloud cloud;
    GaussianSplat s{};
    s.x = s.y = s.z = 0.0f;
    s.f_dc_0 = s.f_dc_1 = s.f_dc_2 = 0.0f;
    s.opacity = 5.0f;
    s.scale_0 = s.scale_1 = s.scale_2 = -2.0f;
    // Non-symmetric quaternion (all four components differ) so any permutation
    // changes the rotation: w=0.8, x=0.4, y=0.2, z=0.4 (normalized below).
    float w = 0.8f, x = 0.4f, y = 0.2f, z = 0.4f;
    float n = std::sqrt(w * w + x * x + y * y + z * z);
    w /= n; x /= n; y /= n; z /= n;
    s.rot_0 = w; s.rot_1 = x; s.rot_2 = y; s.rot_3 = z;
    cloud.addSplat(s);
    cloud.setShDegree(0);

    SpzEncoder enc;
    SpzEncodeConfig cfg;
    auto res = enc.encodeToFile("/tmp/melkor_test_quat.spz", cloud, cfg);
    if (!res.success) {
        check(false, "encode to file");
        return false;
    }

    // Decode with spz's own loader (NOT melkor's) so a symmetric double-bug
    // cannot hide: melkor stores w-first, spz expects xyzw.
    spz::UnpackOptions uo;
    uo.to = spz::CoordinateSystem::RDF;
    auto spz_cloud = spz::loadSpz("/tmp/melkor_test_quat.spz", uo);
    if (spz_cloud.numPoints != 1) {
        check(false, "spz decoded point count");
        return false;
    }
    // spz rotations are xyzw; melkor wrote (w,x,y,z), so after reordering we
    // expect the same values back within quantization tolerance (9-bit for the
    // smallest-three encoding).
    float gx = spz_cloud.rotations[0], gy = spz_cloud.rotations[1];
    float gz = spz_cloud.rotations[2], gw = spz_cloud.rotations[3];
    const float tol = 2e-2f;  // accounts for 9-bit quaternion quantization
    check(std::abs(gx - x) < tol && std::abs(gy - y) < tol &&
          std::abs(gz - z) < tol && std::abs(gw - w) < tol,
          "spz decoded quaternion matches input (xyzw order)");
    return true;
}
#endif

// ---- Test 2: header-driven PLY reader -------------------------------------
bool test_ply_reader() {
    printf("[test] header-driven PLY reader\n");
    using namespace melkor;

    // 2a: classic 3DGS ascii.
    const char* hdr1 =
        "ply\nformat ascii 1.0\nelement vertex 1\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property float nx\nproperty float ny\nproperty float nz\n"
        "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
        "property float opacity\n"
        "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
        "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
        "end_header\n"
        "1 2 3 0 0 1 0.1 0.2 0.3 0.5 -1 -2 -3 0.707 0 0 0.707\n";
    std::string f1(hdr1);
    PlyReader r1;
    auto res1 = r1.readFromBuffer(reinterpret_cast<const uint8_t*>(f1.data()), f1.size());
    check(res1.success, "3DGS ascii: parse succeeds");
    if (res1.success) {
        const auto& s = res1.cloud[0];
        check(s.x == 1 && s.y == 2 && s.z == 3, "3DGS ascii: position");
        check(std::abs(s.rot_0 - 0.707f) < 1e-3 && std::abs(s.rot_3 - 0.707f) < 1e-3,
              "3DGS ascii: quaternion (last two components)");
    }

    // 2b: non-3DGS PLY with red/green/blue uchar and no normals/scale/rot.
    const char* hdr2 =
        "ply\nformat ascii 1.0\nelement vertex 1\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
        "end_header\n"
        "10 20 30 255 0 0\n";
    std::string f2(hdr2);
    PlyReader r2;
    auto res2 = r2.readFromBuffer(reinterpret_cast<const uint8_t*>(f2.data()), f2.size());
    check(res2.success, "rgb-only ascii: parse succeeds");
    if (res2.success) {
        const auto& s = res2.cloud[0];
        check(s.x == 10 && s.y == 20 && s.z == 30, "rgb-only ascii: position");
        float expect_r = utils::rgbToShDc(1.0f);
        check(std::abs(s.f_dc_0 - expect_r) < 1e-2f, "rgb-only ascii: red mapped to SH DC");
        check(s.rot_0 == 1.0f, "rgb-only ascii: missing rotation defaults to identity");
    }

    // 2c: binary round-trip through the writer.
    GaussianCloud c3;
    GaussianSplat s3{};
    s3.x = 5; s3.y = 6; s3.z = 7;
    s3.opacity = 0.0f;
    s3.rot_0 = 1.0f;
    c3.addSplat(s3);
    PlyWriter w;
    std::vector<uint8_t> buf;
    PlyWriteConfig cfg;
    cfg.format = PlyFormat::Binary;
    auto wres = w.writeToBuffer(buf, c3, cfg);
    check(wres.success, "binary write");
    PlyReader r3;
    auto res3 = r3.readFromBuffer(buf.data(), buf.size());
    check(res3.success, "binary round-trip: parse succeeds");
    if (res3.success) {
        const auto& s = res3.cloud[0];
        check(std::abs(s.x - 5) < 1e-5f && std::abs(s.y - 6) < 1e-5f && std::abs(s.z - 7) < 1e-5f,
              "binary round-trip: position preserved");
    }
    return g_failures == 0;
}

// ---- Test 3: PCA normal estimation ----------------------------------------
bool test_pca_normals() {
    printf("[test] PCA normal estimation on a sphere\n");
    using namespace melkor;
    // Sample a sphere; PCA normals should align with the radial direction
    // (point - center), since the local tangent plane is perpendicular to it.
    std::vector<float> positions;
    const int n = 20;
    const float radius = 5.0f;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            float theta = static_cast<float>(i) / n * 3.14159265f;
            float phi = static_cast<float>(j) / n * 2.0f * 3.14159265f;
            positions.push_back(radius * std::sin(theta) * std::cos(phi));
            positions.push_back(radius * std::sin(theta) * std::sin(phi));
            positions.push_back(radius * std::cos(theta));
        }
    }
    auto normals = melkor::enhanced::estimateNormals(positions, 8);
    size_t num = positions.size() / 3;
    int aligned = 0;
    for (size_t i = 0; i < num; ++i) {
        float ex = positions[i * 3 + 0], ey = positions[i * 3 + 1], ez = positions[i * 3 + 2];
        float el = std::sqrt(ex * ex + ey * ey + ez * ez);
        ex /= el; ey /= el; ez /= el;
        float nx = normals[i * 3 + 0], ny = normals[i * 3 + 1], nz = normals[i * 3 + 2];
        float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-6f) continue;
        nx /= nl; ny /= nl; nz /= nl;
        if (ex * nx + ey * ny + ez * nz > 0.9f) ++aligned;
    }
    float pct = 100.0f * aligned / static_cast<float>(num);
    printf("    aligned %d/%zu (%.1f%%)\n", aligned, num, pct);
    check(pct > 80.0f, ">=80% normals align with radial direction");
    return g_failures == 0;
}

// ---- Test 4: SH C0 constant round-trip ------------------------------------
bool test_sh_constant() {
    printf("[test] SH C0 rgb<->shdc invertibility\n");
    using namespace melkor;
    bool ok = true;
    const float samples[] = {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f};
    for (float v : samples) {
        float round = utils::shDcToRgb(utils::rgbToShDc(v));
        if (std::abs(round - v) > 1e-5f) {
            ok = false;
            printf("    drift at v=%.3f: round=%.6f\n", v, round);
        }
    }
    check(ok, "rgbToShDc and shDcToRgb are inverses");
    return ok;
}
// ---- Test 5: logit/sigmoid round-trip and edge safety --------------------
bool test_logit_sigmoid() {
    printf("[test] logit/sigmoid round-trip and edge safety\n");
    using namespace melkor;
    bool ok = true;
    // Round-trip: sigmoid(logit(x)) ≈ x for mid-range values
    const float samples[] = {0.01f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 0.99f};
    for (float v : samples) {
        float round = utils::sigmoid(utils::logit(v));
        if (std::abs(round - v) > 1e-4f) {
            ok = false;
            printf("    drift at v=%.4f: round=%.6f\n", v, round);
        }
    }
    check(ok, "sigmoid(logit(x)) round-trips for mid-range values");

    // Edge safety: logit(0) and logit(1) must not produce NaN or inf
    bool edge_ok = true;
    float l0 = utils::logit(0.0f);
    float l1 = utils::logit(1.0f);
    if (std::isnan(l0) || std::isinf(l0)) { edge_ok = false; printf("    logit(0) = %f\n", l0); }
    if (std::isnan(l1) || std::isinf(l1)) { edge_ok = false; printf("    logit(1) = %f\n", l1); }
    check(edge_ok, "logit(0) and logit(1) are finite (no div-by-zero)");

    // Sigmoid stability for extreme values: must not produce NaN/inf.
    // For float precision, sigmoid(-100) underflows to exactly 0.0f — that's
    // correct behavior, not a bug.
    bool sig_ok = true;
    float s_big = utils::sigmoid(100.0f);
    float s_neg = utils::sigmoid(-100.0f);
    if (std::isnan(s_big) || std::isinf(s_big)) { sig_ok = false; printf("    sigmoid(100) = %f\n", s_big); }
    if (std::isnan(s_neg) || std::isinf(s_neg)) { sig_ok = false; printf("    sigmoid(-100) = %f\n", s_neg); }
    if (s_big < 0.99f || s_big > 1.0f) { sig_ok = false; printf("    sigmoid(100) = %f (expected ~1)\n", s_big); }
    if (s_neg < 0.0f || s_neg > 0.01f) { sig_ok = false; printf("    sigmoid(-100) = %f (expected ~0)\n", s_neg); }
    check(sig_ok, "sigmoid is stable for extreme values");
    return ok && edge_ok && sig_ok;
}
// ---- Test 6: empty cloud bounding box safety -----------------------------
bool test_empty_bounding_box() {
    printf("[test] empty cloud bounding box safety\n");
    using namespace melkor;
    GaussianCloud empty;
    float minX = -999, minY = -999, minZ = -999;
    float maxX = -999, maxY = -999, maxZ = -999;
    empty.computeBoundingBox(minX, minY, minZ, maxX, maxY, maxZ);
    check(minX == 0.0f && minY == 0.0f && minZ == 0.0f, "empty bbox min is zero");
    check(maxX == 0.0f && maxY == 0.0f && maxZ == 0.0f, "empty bbox max is zero");
    return g_failures == 0;
}

// ---- Test 7: truncated binary PLY rejection -------------------------------
bool test_truncated_ply() {
    printf("[test] truncated binary PLY rejection\n");
    using namespace melkor;
    std::string header =
        "ply\nformat binary_little_endian 1.0\n"
        "element vertex 100\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property float nx\nproperty float ny\nproperty float nz\n"
        "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
        "property float opacity\n"
        "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
        "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
        "end_header\n";
    std::vector<uint8_t> buf(header.begin(), header.end());
    std::vector<float> one_vertex(17, 1.0f);
    const auto* raw = reinterpret_cast<const uint8_t*>(one_vertex.data());
    buf.insert(buf.end(), raw, raw + one_vertex.size() * sizeof(float));

    PlyReader r;
    auto res = r.readFromBuffer(buf.data(), buf.size());
    check(!res.success, "truncated binary PLY is rejected");
    return g_failures == 0;
}

}  // namespace

int main() {
    test_sh_constant();
    test_logit_sigmoid();
    test_empty_bounding_box();
    test_truncated_ply();
    test_ply_reader();
    test_pca_normals();
#ifdef MELKOR_HAS_SPZ
    test_spz_quaternion_order();
#else
    printf("[skip] SPZ quaternion test (built without MELKOR_HAS_SPZ)\n");
#endif
    printf("\n=== %s (%d failures) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
