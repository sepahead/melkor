#include "melkor/gaussian_data.hpp"
#include <cmath>
#include <limits>
#include <algorithm>

namespace melkor {

void GaussianCloud::addSplat(const GaussianSplat& splat) {
    splats_.push_back(splat);
}

void GaussianCloud::addSplat(GaussianSplat&& splat) {
    splats_.push_back(std::move(splat));
}

void GaussianCloud::reserve(size_t count) {
    splats_.reserve(count);
}

void GaussianCloud::clear() {
    splats_.clear();
}

std::vector<PackedGaussian> GaussianCloud::toPackedFormat() const {
    std::vector<PackedGaussian> packed;
    packed.reserve(splats_.size());
    
    for (const auto& splat : splats_) {
        PackedGaussian p;
        p.position[0] = splat.x;
        p.position[1] = splat.y;
        p.position[2] = splat.z;
        p.position[3] = splat.opacity;
        
        p.color[0] = splat.f_dc_0;
        p.color[1] = splat.f_dc_1;
        p.color[2] = splat.f_dc_2;
        p.color[3] = 0.0f;  // padding
        
        p.scale[0] = splat.scale_0;
        p.scale[1] = splat.scale_1;
        p.scale[2] = splat.scale_2;
        p.scale[3] = 0.0f;  // padding
        
        p.rotation[0] = splat.rot_0;
        p.rotation[1] = splat.rot_1;
        p.rotation[2] = splat.rot_2;
        p.rotation[3] = splat.rot_3;
        
        packed.push_back(p);
    }
    
    return packed;
}

GaussianCloud GaussianCloud::fromPackedFormat(const std::vector<PackedGaussian>& packed) {
    GaussianCloud cloud;
    cloud.reserve(packed.size());
    
    for (const auto& p : packed) {
        GaussianSplat splat;
        splat.x = p.position[0];
        splat.y = p.position[1];
        splat.z = p.position[2];
        splat.opacity = p.position[3];
        
        splat.f_dc_0 = p.color[0];
        splat.f_dc_1 = p.color[1];
        splat.f_dc_2 = p.color[2];
        
        splat.scale_0 = p.scale[0];
        splat.scale_1 = p.scale[1];
        splat.scale_2 = p.scale[2];
        
        splat.rot_0 = p.rotation[0];
        splat.rot_1 = p.rotation[1];
        splat.rot_2 = p.rotation[2];
        splat.rot_3 = p.rotation[3];
        
        cloud.addSplat(std::move(splat));
    }
    
    return cloud;
}

void GaussianCloud::computeBoundingBox(float& minX, float& minY, float& minZ,
                                       float& maxX, float& maxY, float& maxZ) const {
    if (splats_.empty()) {
        minX = minY = minZ = 0.0f;
        maxX = maxY = maxZ = 0.0f;
        return;
    }
    
    minX = minY = minZ = std::numeric_limits<float>::max();
    maxX = maxY = maxZ = std::numeric_limits<float>::lowest();
    
    for (const auto& splat : splats_) {
        minX = std::min(minX, splat.x);
        minY = std::min(minY, splat.y);
        minZ = std::min(minZ, splat.z);
        maxX = std::max(maxX, splat.x);
        maxY = std::max(maxY, splat.y);
        maxZ = std::max(maxZ, splat.z);
    }
}

namespace utils {

void normalizeQuaternion(float& w, float& x, float& y, float& z) {
    float len = std::sqrt(w*w + x*x + y*y + z*z);
    if (len > 0.0f) {
        float inv_len = 1.0f / len;
        w *= inv_len;
        x *= inv_len;
        y *= inv_len;
        z *= inv_len;
    } else {
        // Default to identity quaternion
        w = 1.0f;
        x = y = z = 0.0f;
    }
    
    // Ensure w is positive (canonical form)
    if (w < 0.0f) {
        w = -w;
        x = -x;
        y = -y;
        z = -z;
    }
}

} // namespace utils
} // namespace melkor
