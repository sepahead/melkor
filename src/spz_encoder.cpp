// SPZ Encoder/Decoder Implementation
// Bridges melkor::GaussianCloud to spz::GaussianCloud

#include "melkor/spz_encoder.hpp"

#ifdef MELKOR_HAS_SPZ

#include "load-spz.h"
#include <fstream>

namespace melkor {

// ============================================================================
// Helper functions to convert between melkor and spz types
// ============================================================================

static spz::GaussianCloud toSpzCloud(const GaussianCloud& cloud, int sh_degree) {
    spz::GaussianCloud spz_cloud;
    spz_cloud.numPoints = static_cast<int32_t>(cloud.size());
    // Clamp the encoded SH degree to both the config and what the cloud actually
    // carries, so requesting a lower degree truncates SH rest (smaller file)
    // and requesting a higher degree than present doesn't pad with garbage.
    int effective_degree = std::min(sh_degree, cloud.shDegree());
    spz_cloud.shDegree = effective_degree;
    spz_cloud.antialiased = false;
    
    // Reserve space
    spz_cloud.positions.reserve(cloud.size() * 3);
    spz_cloud.scales.reserve(cloud.size() * 3);
    spz_cloud.rotations.reserve(cloud.size() * 4);
    spz_cloud.alphas.reserve(cloud.size());
    spz_cloud.colors.reserve(cloud.size() * 3);
    
    // Number of SH-rest coefficients per splat for the effective degree.
    int sh_rest_count = 0;
    switch (effective_degree) {
        case 1: sh_rest_count = 9; break;
        case 2: sh_rest_count = 24; break;
        case 3: sh_rest_count = 45; break;
        default: sh_rest_count = 0; break;
    }
    
    // Convert each splat
    for (size_t i = 0; i < cloud.size(); ++i) {
        const auto& splat = cloud[i];
        
        // Position (x, y, z)
        spz_cloud.positions.push_back(splat.x);
        spz_cloud.positions.push_back(splat.y);
        spz_cloud.positions.push_back(splat.z);
        
        // Scale (log space)
        spz_cloud.scales.push_back(splat.scale_0);
        spz_cloud.scales.push_back(splat.scale_1);
        spz_cloud.scales.push_back(splat.scale_2);
        
        // Rotation quaternion. SPZ's GaussianCloud stores quaternions as
        // (x, y, z, w) -- scalar last (see spz splat-types.h: "xyzw quaternion"
        // and UnpackedGaussian.rotation = {x, y, z, w}; packQuaternionSmallestThree
        // only ever flips the x/y/z components, never w). Melkor stores rotations
        // as (rot_0=w, rot_1=x, rot_2=y, rot_3=z), so we must reorder here.
        spz_cloud.rotations.push_back(splat.rot_1);  // x
        spz_cloud.rotations.push_back(splat.rot_2);  // y
        spz_cloud.rotations.push_back(splat.rot_3);  // z
        spz_cloud.rotations.push_back(splat.rot_0);  // w
        
        // Alpha (logit space)
        spz_cloud.alphas.push_back(splat.opacity);
        
        // Color (SH DC coefficients)
        spz_cloud.colors.push_back(splat.f_dc_0);
        spz_cloud.colors.push_back(splat.f_dc_1);
        spz_cloud.colors.push_back(splat.f_dc_2);
        
        // SH rest: only emit up to sh_rest_count coefficients
        for (int j = 0; j < sh_rest_count; ++j) {
            spz_cloud.sh.push_back(
                (j < static_cast<int>(splat.sh_rest.size())) ? splat.sh_rest[j] : 0.0f);
        }
    }
    
    return spz_cloud;
}

static GaussianCloud fromSpzCloud(const spz::GaussianCloud& spz_cloud) {
    GaussianCloud cloud;
    cloud.setShDegree(spz_cloud.shDegree);
    cloud.reserve(spz_cloud.numPoints);
    
    for (int32_t i = 0; i < spz_cloud.numPoints; ++i) {
        GaussianSplat splat;
        
        // Position
        splat.x = spz_cloud.positions[i * 3 + 0];
        splat.y = spz_cloud.positions[i * 3 + 1];
        splat.z = spz_cloud.positions[i * 3 + 2];
        
        // Scale
        splat.scale_0 = spz_cloud.scales[i * 3 + 0];
        splat.scale_1 = spz_cloud.scales[i * 3 + 1];
        splat.scale_2 = spz_cloud.scales[i * 3 + 2];
        
        // Rotation. SPZ provides quaternions as (x, y, z, w) -- scalar last;
        // convert to Melkor's (w, x, y, z) layout. See toSpzCloud for details.
        splat.rot_0 = spz_cloud.rotations[i * 4 + 3];  // w
        splat.rot_1 = spz_cloud.rotations[i * 4 + 0];  // x
        splat.rot_2 = spz_cloud.rotations[i * 4 + 1];  // y
        splat.rot_3 = spz_cloud.rotations[i * 4 + 2];  // z
        
        // Alpha
        splat.opacity = spz_cloud.alphas[i];
        
        // Color
        splat.f_dc_0 = spz_cloud.colors[i * 3 + 0];
        splat.f_dc_1 = spz_cloud.colors[i * 3 + 1];
        splat.f_dc_2 = spz_cloud.colors[i * 3 + 2];
        
        // Higher order SH
        if (spz_cloud.shDegree > 0 && !spz_cloud.sh.empty()) {
            size_t sh_per_point = 0;
            switch (spz_cloud.shDegree) {
                case 1: sh_per_point = 9; break;
                case 2: sh_per_point = 24; break;
                case 3: sh_per_point = 45; break;
            }
            size_t sh_start = i * sh_per_point;
            for (size_t j = 0; j < sh_per_point && sh_start + j < spz_cloud.sh.size(); ++j) {
                splat.sh_rest.push_back(spz_cloud.sh[sh_start + j]);
            }
        }
        
        cloud.addSplat(std::move(splat));
    }
    
    return cloud;
}

// ============================================================================
// SpzEncoder Implementation
// ============================================================================

SpzEncoder::SpzEncoder() = default;
SpzEncoder::~SpzEncoder() = default;

SpzEncodeResult SpzEncoder::encodeToFile(const std::string& filepath,
                                          const GaussianCloud& cloud,
                                          const SpzEncodeConfig& config) {
    SpzEncodeResult result;
    
    if (cloud.empty()) {
        result.error_message = "Cannot encode empty cloud";
        return result;
    }
    
    // Convert to SPZ cloud
    spz::GaussianCloud spz_cloud = toSpzCloud(cloud, config.sh_degree);
    
    // Set up pack options
    spz::PackOptions options;
    options.from = spz::CoordinateSystem::RDF;  // PLY coordinate system
    
    // Save to file
    if (spz::saveSpz(spz_cloud, options, filepath)) {
        result.success = true;
        
        // Get file size
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
            result.bytes_written = static_cast<size_t>(file.tellg());
        }
    } else {
        result.error_message = "Failed to save SPZ file";
    }
    
    return result;
}

SpzEncodeResult SpzEncoder::encodeToBuffer(std::vector<uint8_t>& buffer,
                                            const GaussianCloud& cloud,
                                            const SpzEncodeConfig& config) {
    SpzEncodeResult result;
    
    if (cloud.empty()) {
        result.error_message = "Cannot encode empty cloud";
        return result;
    }
    
    // Convert to SPZ cloud
    spz::GaussianCloud spz_cloud = toSpzCloud(cloud, config.sh_degree);
    
    // Set up pack options
    spz::PackOptions options;
    options.from = spz::CoordinateSystem::RDF;  // PLY coordinate system
    
    // Save to buffer
    if (spz::saveSpz(spz_cloud, options, &buffer)) {
        result.success = true;
        result.bytes_written = buffer.size();
    } else {
        result.error_message = "Failed to encode SPZ data";
    }
    
    return result;
}

// ============================================================================
// SpzDecoder Implementation
// ============================================================================

SpzDecoder::SpzDecoder() = default;
SpzDecoder::~SpzDecoder() = default;

SpzDecoder::DecodeResult SpzDecoder::decodeFromFile(const std::string& filepath) {
    DecodeResult result;
    
    // Set up unpack options
    spz::UnpackOptions options;
    options.to = spz::CoordinateSystem::RDF;  // PLY coordinate system
    
    try {
        spz::GaussianCloud spz_cloud = spz::loadSpz(filepath, options);
        
        if (spz_cloud.numPoints == 0) {
            result.error_message = "SPZ file contains no points";
            return result;
        }
        
        result.cloud = fromSpzCloud(spz_cloud);
        result.success = true;
    } catch (const std::exception& e) {
        result.error_message = std::string("Failed to load SPZ: ") + e.what();
    }
    
    return result;
}

SpzDecoder::DecodeResult SpzDecoder::decodeFromBuffer(const uint8_t* data, size_t size) {
    DecodeResult result;
    
    // Set up unpack options
    spz::UnpackOptions options;
    options.to = spz::CoordinateSystem::RDF;  // PLY coordinate system
    
    try {
        spz::GaussianCloud spz_cloud = spz::loadSpz(data, static_cast<int32_t>(size), options);
        
        if (spz_cloud.numPoints == 0) {
            result.error_message = "SPZ data contains no points";
            return result;
        }
        
        result.cloud = fromSpzCloud(spz_cloud);
        result.success = true;
    } catch (const std::exception& e) {
        result.error_message = std::string("Failed to decode SPZ: ") + e.what();
    }
    
    return result;
}

} // namespace melkor

#endif // MELKOR_HAS_SPZ
