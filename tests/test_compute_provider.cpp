// ComputeProvider abstraction tests.
//
// The provider layer is the single dispatch point between the CLI and the
// CPU/Metal/CUDA backends, so a silent divergence here corrupts every
// conversion. Coverage:
//   1. Factory behavior: create() yields an initialized provider; the CPU
//      backend is always constructible.
//   2. CPU backend math against the melkor::utils reference (quaternion
//      normalization, RGB->SH DC, opacity logit, position scaling).
//   3. transformCoordinates translation/linear semantics.
//   4. Y-up -> Z-up must be the rotation (x,y,z)->(x,-z,y), NOT a Y/Z swap
//      (a swap is a reflection that mirrors asymmetric geometry).
//   5. computeCovariances against a hand-computed diagonal case.
//   6. CPU vs GPU backend parity on the shared operations when a GPU
//      backend initializes on this machine.

#include "melkor/compute_provider.hpp"
#include "melkor/gaussian_data.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

struct Lcg {
    uint32_t state;
    explicit Lcg(uint32_t seed) : state(seed) {}
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    }
};

melkor::GaussianCloud makeRandomCloud(size_t n, uint32_t seed) {
    Lcg rng(seed);
    melkor::GaussianCloud cloud;
    for (size_t i = 0; i < n; ++i) {
        melkor::GaussianSplat s{};
        s.x = rng.next() * 4.0f - 2.0f;
        s.y = rng.next() * 4.0f - 2.0f;
        s.z = rng.next() * 4.0f - 2.0f;
        s.f_dc_0 = rng.next();
        s.f_dc_1 = rng.next();
        s.f_dc_2 = rng.next();
        s.opacity = 0.1f + rng.next() * 0.8f;   // linear (0.1, 0.9)
        s.scale_0 = 0.2f + rng.next();          // linear scales for covariances
        s.scale_1 = 0.2f + rng.next();
        s.scale_2 = 0.2f + rng.next();
        s.rot_0 = rng.next() * 2.0f - 1.0f;
        s.rot_1 = rng.next() * 2.0f - 1.0f;
        s.rot_2 = rng.next() * 2.0f - 1.0f;
        s.rot_3 = rng.next() * 2.0f - 1.0f;
        if (std::abs(s.rot_0) + std::abs(s.rot_1) +
            std::abs(s.rot_2) + std::abs(s.rot_3) < 1e-3f) {
            s.rot_0 = 1.0f;
        }
        cloud.addSplat(s);
    }
    return cloud;
}

bool test_factories() {
    printf("[test] provider factories\n");
    auto best = melkor::ComputeProvider::create();
    check(best != nullptr, "create() returns a provider");
    check(best && best->isInitialized(), "created provider is initialized");
    if (best) {
        printf("  (backend: %s)\n", best->backendName().c_str());
    }

    auto cpu = melkor::ComputeProvider::create(melkor::ComputeBackend::CPU);
    check(cpu != nullptr, "CPU backend is always available");
    check(cpu && cpu->backend() == melkor::ComputeBackend::CPU,
          "CPU provider reports CPU backend");
    check(cpu && cpu->rawContext() == nullptr, "CPU provider has no raw context");
    return true;
}

bool test_cpu_math() {
    printf("[test] CPU backend math vs utils reference\n");
    auto p = melkor::ComputeProvider::create(melkor::ComputeBackend::CPU);
    if (!p) { check(false, "CPU provider exists"); return false; }

    auto cloud = makeRandomCloud(64, 11);
    auto ref = cloud;

    p->normalizeQuaternions(cloud);
    bool unit = true;
    for (size_t i = 0; i < cloud.size(); ++i) {
        const auto& s = cloud[i];
        float len = std::sqrt(s.rot_0 * s.rot_0 + s.rot_1 * s.rot_1 +
                              s.rot_2 * s.rot_2 + s.rot_3 * s.rot_3);
        if (std::abs(len - 1.0f) > 1e-4f) unit = false;
    }
    check(unit, "normalizeQuaternions yields unit quaternions");

    cloud = ref;
    p->rgbToShDc(cloud);
    bool sh_ok = true;
    for (size_t i = 0; i < cloud.size(); ++i) {
        if (std::abs(cloud[i].f_dc_0 - melkor::utils::rgbToShDc(ref[i].f_dc_0)) > 1e-6f)
            sh_ok = false;
    }
    check(sh_ok, "rgbToShDc matches utils::rgbToShDc");

    cloud = ref;
    p->opacityToLogit(cloud);
    bool op_ok = true;
    for (size_t i = 0; i < cloud.size(); ++i) {
        if (std::abs(cloud[i].opacity - melkor::utils::logit(ref[i].opacity)) > 1e-6f)
            op_ok = false;
    }
    check(op_ok, "opacityToLogit matches utils::logit");

    cloud = ref;
    p->scalePositions(cloud, 2.5f);
    bool sp_ok = true;
    for (size_t i = 0; i < cloud.size(); ++i) {
        if (std::abs(cloud[i].x - ref[i].x * 2.5f) > 1e-5f) sp_ok = false;
    }
    check(sp_ok, "scalePositions scales positions");

    cloud = ref;
    p->sortByDistance(cloud, 0.0f, 0.0f, 0.0f);
    bool sorted = true;
    for (size_t i = 1; i < cloud.size(); ++i) {
        auto d = [](const melkor::GaussianSplat& s) {
            return s.x * s.x + s.y * s.y + s.z * s.z;
        };
        if (d(cloud[i - 1]) > d(cloud[i]) + 1e-6f) sorted = false;
    }
    check(sorted, "sortByDistance orders near-to-far");
    return true;
}

bool test_transform_semantics() {
    printf("[test] transformCoordinates translation semantics\n");
    auto p = melkor::ComputeProvider::create(melkor::ComputeBackend::CPU);
    melkor::GaussianCloud cloud;
    melkor::GaussianSplat s{};
    s.x = 1.0f; s.y = 2.0f; s.z = 3.0f; s.rot_0 = 1.0f;
    cloud.addSplat(s);

    // Identity + translation (10, 20, 30) in the layout the provider
    // documents (translation at m[12..14]).
    float m[16] = {1, 0, 0, 0,
                   0, 1, 0, 0,
                   0, 0, 1, 0,
                   10, 20, 30, 1};
    p->transformCoordinates(cloud, m);
    check(std::abs(cloud[0].x - 11.0f) < 1e-5f &&
          std::abs(cloud[0].y - 22.0f) < 1e-5f &&
          std::abs(cloud[0].z - 33.0f) < 1e-5f,
          "identity + translation lands where documented");
    return true;
}

bool test_y_up_to_z_up_is_rotation() {
    printf("[test] Y-up -> Z-up is a rotation, not a reflection\n");
    auto p = melkor::ComputeProvider::create(melkor::ComputeBackend::CPU);
    melkor::GaussianCloud cloud;
    melkor::GaussianSplat s{};
    s.x = 1.0f; s.y = 2.0f; s.z = 3.0f;
    s.opacity = -1.0f;  // pretend logit so no conversion needed
    s.rot_0 = 1.0f;
    cloud.addSplat(s);

    melkor::ProcessConfig cfg;
    cfg.normalize_quaternions = false;
    cfg.convert_colors_to_sh = false;
    cfg.convert_opacity_to_logit = false;
    cfg.transform_y_up_to_z_up = true;
    p->processCloud(cloud, cfg);

    // (x, y, z) -> (x, -z, y): determinant +1. A Y/Z swap would give (1,3,2).
    check(std::abs(cloud[0].x - 1.0f) < 1e-6f &&
          std::abs(cloud[0].y + 3.0f) < 1e-6f &&
          std::abs(cloud[0].z - 2.0f) < 1e-6f,
          "(1,2,3) maps to (1,-3,2)");
    return true;
}

bool test_covariances() {
    printf("[test] computeCovariances diagonal case\n");
    auto p = melkor::ComputeProvider::create(melkor::ComputeBackend::CPU);
    melkor::GaussianCloud cloud;
    melkor::GaussianSplat s{};
    s.scale_0 = 2.0f; s.scale_1 = 3.0f; s.scale_2 = 4.0f;  // LINEAR scale
    s.rot_0 = 1.0f;  // identity rotation
    cloud.addSplat(s);

    auto cov = p->computeCovariances(cloud);
    check(cov.size() == 6, "6 covariance floats per splat");
    if (cov.size() == 6) {
        check(std::abs(cov[0] - 4.0f) < 1e-4f &&   // xx = sx^2
              std::abs(cov[3] - 9.0f) < 1e-4f &&   // yy = sy^2
              std::abs(cov[5] - 16.0f) < 1e-4f &&  // zz = sz^2
              std::abs(cov[1]) < 1e-5f && std::abs(cov[2]) < 1e-5f &&
              std::abs(cov[4]) < 1e-5f,
              "identity rotation yields diag(sx^2, sy^2, sz^2)");
    }

    // Non-identity rotation with anisotropic scale. An identity rotation
    // (above) hides a transposed rotation matrix, since R^T S S^T R equals
    // R S S^T R^T only when R = I; this case does not. Reference computed by
    // hand: 90 deg about +Z maps local x->world y, local y->world -x, so the
    // world covariance is diag(sy^2, sx^2, sz^2).
    melkor::GaussianCloud rot;
    melkor::GaussianSplat rs{};
    rs.scale_0 = 2.0f; rs.scale_1 = 3.0f; rs.scale_2 = 4.0f;   // LINEAR
    const float h = std::sqrt(0.5f);
    rs.rot_0 = h; rs.rot_1 = 0.0f; rs.rot_2 = 0.0f; rs.rot_3 = h;  // 90deg +Z
    rot.addSplat(rs);
    auto rcov = p->computeCovariances(rot);
    check(rcov.size() == 6 &&
          std::abs(rcov[0] - 9.0f) < 1e-3f &&   // xx = sy^2
          std::abs(rcov[3] - 4.0f) < 1e-3f &&   // yy = sx^2
          std::abs(rcov[5] - 16.0f) < 1e-3f &&  // zz = sz^2
          std::abs(rcov[1]) < 1e-3f && std::abs(rcov[2]) < 1e-3f &&
          std::abs(rcov[4]) < 1e-3f,
          "90deg-Z rotation yields diag(sy^2, sx^2, sz^2)");
    return true;
}

bool test_gpu_cpu_parity() {
    printf("[test] GPU vs CPU backend parity (if a GPU backend exists)\n");
    auto gpu = melkor::ComputeProvider::create();
    if (!gpu || gpu->backend() == melkor::ComputeBackend::CPU) {
        printf("  SKIP: no GPU backend on this machine\n");
        return true;
    }
    auto cpu = melkor::ComputeProvider::create(melkor::ComputeBackend::CPU);

    auto a = makeRandomCloud(512, 77);
    auto b = a;

    gpu->normalizeQuaternions(a);
    cpu->normalizeQuaternions(b);
    float max_err = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        max_err = std::max(max_err, std::abs(a[i].rot_0 - b[i].rot_0));
        max_err = std::max(max_err, std::abs(a[i].rot_1 - b[i].rot_1));
    }
    check(max_err < 1e-4f, "normalizeQuaternions parity");

    a = b = makeRandomCloud(512, 78);
    melkor::ProcessConfig cfg;
    cfg.normalize_quaternions = true;
    cfg.convert_colors_to_sh = true;
    cfg.convert_opacity_to_logit = true;
    cfg.position_scale = 1.5f;
    cfg.transform_y_up_to_z_up = true;
    gpu->processCloud(a, cfg);
    cpu->processCloud(b, cfg);
    max_err = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        max_err = std::max(max_err, std::abs(a[i].x - b[i].x));
        max_err = std::max(max_err, std::abs(a[i].y - b[i].y));
        max_err = std::max(max_err, std::abs(a[i].z - b[i].z));
        max_err = std::max(max_err, std::abs(a[i].f_dc_0 - b[i].f_dc_0));
        max_err = std::max(max_err, std::abs(a[i].opacity - b[i].opacity));
    }
    check(max_err < 1e-3f, "processCloud parity (positions, colors, opacity)");

    // Covariance parity. computeCovariances expects LINEAR scale, so build a
    // cloud with positive linear scales and normalized (non-identity)
    // rotations. A transposed rotation in one backend shows up here.
    auto ca = makeRandomCloud(256, 79);
    cpu->normalizeQuaternions(ca);
    for (size_t i = 0; i < ca.size(); ++i) {
        ca[i].scale_0 = 0.3f + std::abs(ca[i].scale_0);
        ca[i].scale_1 = 0.3f + std::abs(ca[i].scale_1);
        ca[i].scale_2 = 0.3f + std::abs(ca[i].scale_2);
    }
    auto gcov = gpu->computeCovariances(ca);
    auto ccov = cpu->computeCovariances(ca);
    check(!gcov.empty() && gcov.size() == ccov.size(),
          "GPU computeCovariances returns data");
    if (!gcov.empty() && gcov.size() == ccov.size()) {
        float cov_err = 0.0f;
        for (size_t i = 0; i < gcov.size(); ++i) {
            cov_err = std::max(cov_err, std::abs(gcov[i] - ccov[i]));
        }
        check(cov_err < 1e-3f, "computeCovariances parity (anisotropic, rotated)");
    }
    return true;
}

} // namespace

int main() {
    printf("melkor compute-provider tests\n");
    test_factories();
    test_cpu_math();
    test_transform_semantics();
    test_y_up_to_z_up_is_rotation();
    test_covariances();
    test_gpu_cpu_parity();

    if (g_failures == 0) {
        printf("\nAll compute-provider tests passed.\n");
        return 0;
    }
    printf("\n%d compute-provider test(s) FAILED.\n", g_failures);
    return 1;
}
