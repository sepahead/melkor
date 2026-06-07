#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <string>
#include <cmath>

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
    size_t size() const { return splats_.size(); }
    bool empty() const { return splats_.empty(); }
    
    const GaussianSplat& operator[](size_t idx) const { return splats_[idx]; }
    GaussianSplat& operator[](size_t idx) { return splats_[idx]; }
    
    const std::vector<GaussianSplat>& splats() const { return splats_; }
    std::vector<GaussianSplat>& splats() { return splats_; }
    
    // Direct access to underlying storage (use with care)
    const GaussianSplat* data() const { return splats_.data(); }
    GaussianSplat* data() { return splats_.data(); }
    
    // Convert to GPU-friendly packed format
    std::vector<PackedGaussian> toPackedFormat() const;
    
    // Create from packed format
    static GaussianCloud fromPackedFormat(const std::vector<PackedGaussian>& packed);
    
    // Spherical harmonics degree (0-3)
    int shDegree() const { return sh_degree_; }
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
    // Convert RGB [0,1] to spherical harmonics DC coefficient
    inline float rgbToShDc(float rgb) {
        // SH DC = (rgb - 0.5) / 0.28209479177387814 (C0 constant)
        return (rgb - 0.5f) / 0.28209479177387814f;
    }
    
    // Convert spherical harmonics DC coefficient to RGB [0,1]
    inline float shDcToRgb(float sh_dc) {
        return sh_dc * 0.28209479177387814f + 0.5f;
    }
    
    // Sigmoid function for opacity
    inline float sigmoid(float x) {
        return 1.0f / (1.0f + std::exp(-x));
    }
    
    // Inverse sigmoid (logit)
    inline float logit(float x) {
        return std::log(x / (1.0f - x));
    }
    
    // Normalize quaternion
    void normalizeQuaternion(float& w, float& x, float& y, float& z);
}

} // namespace melkor
