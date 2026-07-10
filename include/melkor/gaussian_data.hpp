#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <string>
#include <cmath>
#include <algorithm>

namespace melkor {

// Single Gaussian splat representation
struct GaussianSplat {
    // Position (x, y, z)
    float x, y, z;
    
    // Color (direct current spherical harmonics coefficients)
    float f_dc_0, f_dc_1, f_dc_2;
    
    // Opacity (logit space, needs sigmoid for actual opacity)
    float opacity;
    
    // Scale (log space)
    float scale_0, scale_1, scale_2;
    
    // Rotation quaternion (w, x, y, z) - stored as (rot_0, rot_1, rot_2, rot_3)
    float rot_0, rot_1, rot_2, rot_3;
    
    // Higher order spherical harmonics (optional, for SH degree > 0)
    std::vector<float> sh_rest;  // Up to 45 additional coefficients for degree 3
};

// GPU-friendly packed format (for Metal compute)
struct alignas(16) PackedGaussian {
    float position[4];   // x, y, z, opacity
    float color[4];      // f_dc_0, f_dc_1, f_dc_2, padding
    float scale[4];      // scale_0, scale_1, scale_2, padding
    float rotation[4];   // rot_0, rot_1, rot_2, rot_3 (quaternion)
};

// Collection of Gaussian splats
class GaussianCloud {
public:
    GaussianCloud() = default;
    ~GaussianCloud() = default;
    
    // Move semantics for efficiency
    GaussianCloud(GaussianCloud&&) = default;
    GaussianCloud& operator=(GaussianCloud&&) = default;
    
    // Copy semantics
    GaussianCloud(const GaussianCloud&) = default;
    GaussianCloud& operator=(const GaussianCloud&) = default;
    
    // Add a splat
    void addSplat(const GaussianSplat& splat);
    void addSplat(GaussianSplat&& splat);
    
    // Reserve capacity
    void reserve(size_t count);
    
    // Clear all splats
    void clear();
    
    // Accessors
    [[nodiscard]] size_t size() const { return splats_.size(); }
    [[nodiscard]] bool empty() const { return splats_.empty(); }
    
    [[nodiscard]] const GaussianSplat& operator[](size_t idx) const { return splats_[idx]; }
    GaussianSplat& operator[](size_t idx) { return splats_[idx]; }
    
    [[nodiscard]] const std::vector<GaussianSplat>& splats() const { return splats_; }
    std::vector<GaussianSplat>& splats() { return splats_; }
    
    // Direct access to underlying storage (use with care)
    [[nodiscard]] const GaussianSplat* data() const { return splats_.data(); }
    GaussianSplat* data() { return splats_.data(); }
    
    // Convert to GPU-friendly packed format
    [[nodiscard]] std::vector<PackedGaussian> toPackedFormat() const;
    
    // Create from packed format
    [[nodiscard]] static GaussianCloud fromPackedFormat(const std::vector<PackedGaussian>& packed);
    
    // Spherical harmonics degree (0-3)
    [[nodiscard]] int shDegree() const { return sh_degree_; }
    void setShDegree(int degree) { sh_degree_ = degree; }
    
    // Compute bounding box
    void computeBoundingBox(float& minX, float& minY, float& minZ,
                           float& maxX, float& maxY, float& maxZ) const;
    
private:
    std::vector<GaussianSplat> splats_;
    int sh_degree_ = 0;
};

// Utility functions
namespace utils {
    // Spherical-harmonics DC (band-0) constant. The single source of truth for
    // the 3DGS color convention: rgb = SH_C0 * sh_dc + 0.5, and
    // sh_dc = (rgb - 0.5) / SH_C0. Defined once here and re-exported by name
    // from every call site to prevent silent drift across the C++/Metal and
    // Python layers.
    //
    // Value: 1 / (2 * sqrt(pi)) = 0.28209479177387814...
    constexpr float SH_C0 = 0.28209479177387814f;

    // Convert RGB [0,1] to spherical harmonics DC coefficient
    inline float rgbToShDc(float rgb) {
        return (rgb - 0.5f) / SH_C0;
    }

    // Convert spherical harmonics DC coefficient to RGB [0,1]
    inline float shDcToRgb(float sh_dc) {
        return sh_dc * SH_C0 + 0.5f;
    }
    
    // Sigmoid function for opacity. Numerically stable: avoids exp overflow
    // for large negative x by branching on the sign.
    inline float sigmoid(float x) {
        if (x >= 0.0f) {
            return 1.0f / (1.0f + std::exp(-x));
        } else {
            float ex = std::exp(x);
            return ex / (1.0f + ex);
        }
    }
    
    // Inverse sigmoid (logit). Clamps input to (eps, 1-eps) to prevent
    // division by zero when x is exactly 0 or 1, which would produce -inf/+inf.
    inline float logit(float x) {
        x = std::clamp(x, 1e-6f, 1.0f - 1e-6f);
        return std::log(x / (1.0f - x));
    }

    // Convert a quaternion (w, x, y, z) to a 3x3 rotation matrix stored
    // row-major in out[9]. Used by tests to compare rotations: two quaternions
    // q and -q encode the same rotation, so the correct invariant under
    // encoding round-trips (which may canonicalize sign) is matrix equality,
    // not component equality. Normalizes the input inline so it has no
    // dependency on the (separately compiled) normalizeQuaternion.
    inline void quatToRotationMatrix(float w, float x, float y, float z,
                                     float out[9]) {
        const float max_component = std::max({std::abs(w), std::abs(x), std::abs(y), std::abs(z)});
        if (std::isfinite(max_component) && max_component > 0.0f) {
            w /= max_component; x /= max_component; y /= max_component; z /= max_component;
            const float len = std::sqrt(w*w + x*x + y*y + z*z);
            w /= len; x /= len; y /= len; z /= len;
        }
        else { w = 1.0f; x = y = z = 0.0f; }
        float xx = x * x, yy = y * y, zz = z * z;
        float xy = x * y, xz = x * z, yz = y * z;
        float wx = w * x, wy = w * y, wz = w * z;
        out[0] = 1 - 2 * (yy + zz); out[1] = 2 * (xy - wz);     out[2] = 2 * (xz + wy);
        out[3] = 2 * (xy + wz);     out[4] = 1 - 2 * (xx + zz); out[5] = 2 * (yz - wx);
        out[6] = 2 * (xz - wy);     out[7] = 2 * (yz + wx);     out[8] = 1 - 2 * (xx + yy);
    }

    // Normalize quaternion
    void normalizeQuaternion(float& w, float& x, float& y, float& z);
}

} // namespace melkor
