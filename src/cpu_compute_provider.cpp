// CPU compute provider — a first-class backend that implements all
// ComputeProvider operations directly on the host.  Replaces the former
// gpu_stub.cpp GaussianProcessor stubs for the common operations.
//
// Compiled as part of melkor_core so it is available on every platform.
// The per-platform GPU library (melkor_metal / melkor_cuda / melkor_gpu_stub)
// supplies the ComputeProvider::create() factory, which falls back to
// CpuComputeProvider when no GPU is available.

#include "melkor/compute_provider.hpp"
#include "melkor/gaussian_data.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace melkor {

class CpuComputeProvider : public ComputeProvider {
public:
    ComputeBackend backend() const override { return ComputeBackend::CPU; }
    std::string backendName() const override { return "CPU"; }

    bool isAvailable() const override { return true; }

    bool initialize() override {
        initialized_ = true;
        return true;
    }

    bool isInitialized() const override { return initialized_; }

    ComputeDeviceInfo deviceInfo() const override {
        ComputeDeviceInfo info;
        info.name = "CPU";
        info.backend = ComputeBackend::CPU;
        return info;
    }

    void* rawContext() const override { return nullptr; }

    bool transformCoordinates(GaussianCloud& cloud,
                              const float m[16]) override {
        if (cloud.empty()) return true;
        // 4x4 row-major matrix applied to (x, y, z, 1)
        for (auto& s : cloud.splats()) {
            float x = s.x, y = s.y, z = s.z;
            s.x = m[0]*x + m[4]*y + m[8]*z  + m[12];
            s.y = m[1]*x + m[5]*y + m[9]*z  + m[13];
            s.z = m[2]*x + m[6]*y + m[10]*z + m[14];
        }
        return true;
    }

    bool normalizeQuaternions(GaussianCloud& cloud) override {
        // Semantics must match the Metal normalize_quaternions kernel so
        // backends stay component-comparable: zero length -> identity, and
        // the sign is canonicalized to w >= 0 (q and -q encode the same
        // rotation).
        for (auto& s : cloud.splats()) {
            const float max_component = std::max({std::abs(s.rot_0), std::abs(s.rot_1),
                                                   std::abs(s.rot_2), std::abs(s.rot_3)});
            if (std::isfinite(max_component) && max_component > 0.0f) {
                s.rot_0 /= max_component;
                s.rot_1 /= max_component;
                s.rot_2 /= max_component;
                s.rot_3 /= max_component;
                const float len = std::sqrt(s.rot_0*s.rot_0 + s.rot_1*s.rot_1 +
                                            s.rot_2*s.rot_2 + s.rot_3*s.rot_3);
                s.rot_0 /= len;
                s.rot_1 /= len;
                s.rot_2 /= len;
                s.rot_3 /= len;
            } else {
                s.rot_0 = 1.0f;
                s.rot_1 = s.rot_2 = s.rot_3 = 0.0f;
            }
            if (s.rot_0 < 0.0f) {
                s.rot_0 = -s.rot_0;
                s.rot_1 = -s.rot_1;
                s.rot_2 = -s.rot_2;
                s.rot_3 = -s.rot_3;
            }
        }
        return true;
    }

    bool scalePositions(GaussianCloud& cloud, float scale) override {
        for (auto& s : cloud.splats()) {
            s.x *= scale;
            s.y *= scale;
            s.z *= scale;
        }
        return true;
    }

    bool rgbToShDc(GaussianCloud& cloud) override {
        for (auto& s : cloud.splats()) {
            s.f_dc_0 = utils::rgbToShDc(s.f_dc_0);
            s.f_dc_1 = utils::rgbToShDc(s.f_dc_1);
            s.f_dc_2 = utils::rgbToShDc(s.f_dc_2);
        }
        return true;
    }

    bool opacityToLogit(GaussianCloud& cloud) override {
        for (auto& s : cloud.splats()) {
            s.opacity = utils::logit(s.opacity);
        }
        return true;
    }

    bool sortByDistance(GaussianCloud& cloud,
                        float cx, float cy, float cz) override {
        if (cloud.empty()) return true;
        auto& splats = cloud.splats();
        std::sort(splats.begin(), splats.end(),
                  [cx, cy, cz](const GaussianSplat& a, const GaussianSplat& b) {
                      float da = (a.x-cx)*(a.x-cx) + (a.y-cy)*(a.y-cy) + (a.z-cz)*(a.z-cz);
                      float db = (b.x-cx)*(b.x-cx) + (b.y-cy)*(b.y-cy) + (b.z-cz)*(b.z-cz);
                      return da < db;
                  });
        return true;
    }

    std::vector<float> computeCovariances(const GaussianCloud& cloud) override {
        const size_t n = cloud.size();
        if (n == 0) return {};

        std::vector<float> cov(n * 6);
        for (size_t i = 0; i < n; ++i) {
            const auto& s = cloud[i];
            // GaussianCloud stores log scale; covariance uses linear scale.
            float sx = std::exp(s.scale_0);
            float sy = std::exp(s.scale_1);
            float sz = std::exp(s.scale_2);
            // Build 3x3 rotation matrix R from quaternion
            float w = s.rot_0, x = s.rot_1, y = s.rot_2, z = s.rot_3;
            float r00 = 1.f - 2.f*(y*y + z*z);
            float r01 = 2.f*(x*y - w*z);
            float r02 = 2.f*(x*z + w*y);
            float r10 = 2.f*(x*y + w*z);
            float r11 = 1.f - 2.f*(x*x + z*z);
            float r12 = 2.f*(y*z - w*x);
            float r20 = 2.f*(x*z - w*y);
            float r21 = 2.f*(y*z + w*x);
            float r22 = 1.f - 2.f*(x*x + y*y);
            // Sigma = R * S * S^T * R^T
            // S*S^T = diag(sx^2, sy^2, sz^2)
            float sxx = sx*sx, syy = sy*sy, szz = sz*sz;
            // M = R * diag(sxx, syy, szz)
            float m00 = r00*sxx, m01 = r01*syy, m02 = r02*szz;
            float m10 = r10*sxx, m11 = r11*syy, m12 = r12*szz;
            float m20 = r20*sxx, m21 = r21*syy, m22 = r22*szz;
            // Sigma = M * R^T
            cov[i*6 + 0] = m00*r00 + m01*r01 + m02*r02;  // xx
            cov[i*6 + 1] = m00*r10 + m01*r11 + m02*r12;  // xy
            cov[i*6 + 2] = m00*r20 + m01*r21 + m02*r22;  // xz
            cov[i*6 + 3] = m10*r10 + m11*r11 + m12*r12;  // yy
            cov[i*6 + 4] = m10*r20 + m11*r21 + m12*r22;  // yz
            cov[i*6 + 5] = m20*r20 + m21*r21 + m22*r22;  // zz
        }
        return cov;
    }

    bool processCloud(GaussianCloud& cloud, const ProcessConfig& config) override {
        if (cloud.empty()) return true;
        if (config.normalize_quaternions) {
            normalizeQuaternions(cloud);
        }
        if (config.convert_colors_to_sh) {
            rgbToShDc(cloud);
        }
        if (config.convert_opacity_to_logit) {
            opacityToLogit(cloud);
        }
        if (config.position_scale != 1.0f) {
            scalePositions(cloud, config.position_scale);
        }
        if (config.transform_y_up_to_z_up) {
            // Proper rotation (x, y, z) -> (x, -z, y), matching the Metal
            // and CUDA backends. A plain Y/Z swap is a reflection and would
            // mirror asymmetric geometry on the CPU fallback.
            for (auto& s : cloud.splats()) {
                float y = s.y;
                s.y = -s.z;
                s.z = y;
            }
        }
        return true;
    }

private:
    bool initialized_ = false;
};

} // namespace melkor

std::unique_ptr<melkor::ComputeProvider> melkor::createCpuProvider() {
    auto p = std::make_unique<CpuComputeProvider>();
    p->initialize();
    return p;
}
