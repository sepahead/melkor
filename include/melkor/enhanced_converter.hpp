#pragma once

#include "melkor/gaussian_data.hpp"
#include "melkor/compute_provider.hpp"
#include <memory>
#include <vector>
#include <array>

namespace melkor {

// Configuration for enhanced conversion
struct EnhancedConversionConfig {
    bool use_gpu = true;                  // Allow Metal/CUDA acceleration

    // Adaptive scale estimation
    int knn_neighbors = 8;              // Number of neighbors for density estimation
    float scale_factor = 0.5f;          // Multiplier for computed scale
    float min_scale = 0.001f;           // Minimum scale clamp
    float max_scale = 0.1f;             // Maximum scale clamp
    
    // Surface alignment
    bool use_surface_alignment = true;  // Align Gaussians to surface normals
    float normal_scale_ratio = 0.3f;    // Scale along normal vs tangent (flatter = smaller)
    
    // Density handling
    bool adaptive_density = true;       // Adjust density based on local spacing
    float target_splats_per_unit = 100; // Target splat density
    int max_subdivision = 2;            // Maximum subdivision level
    
    // Color handling
    bool use_vertex_colors = true;
    float default_color[3] = {0.5f, 0.5f, 0.5f};
    
    // Coordinate system
    bool convert_coordinate_system = true;  // Y-up to Z-up
    float position_scale = 1.0f;
    
    // Opacity
    float default_opacity = 0.95f;
};

// Point with additional attributes for enhanced processing
struct EnhancedPoint {
    float position[3];
    float normal[3];
    float color[3];
    float uv[2];
    float local_density;      // Estimated from neighbors
    float adaptive_scale[3];  // Computed scale (anisotropic)
    float tangent[3];         // Surface tangent
    float bitangent[3];       // Surface bitangent
};

// Result of enhanced conversion
struct EnhancedConversionResult {
    bool success = false;
    std::string error_message;
    GaussianCloud cloud;
    
    // Statistics
    size_t original_vertices = 0;
    size_t output_splats = 0;
    size_t subdivided_splats = 0;
    float avg_scale = 0.0f;
};

// Enhanced GLB to Gaussian converter
class EnhancedConverter {
public:
    EnhancedConverter();
    explicit EnhancedConverter(ComputeProvider* provider);
    ~EnhancedConverter();
    
    // Convert GLB file with enhanced processing
    EnhancedConversionResult convertFromFile(
        const std::string& filepath,
        const EnhancedConversionConfig& config = {});
    
    // Convert from pre-loaded mesh data
    EnhancedConversionResult convertFromMesh(
        const std::vector<float>& positions,      // xyz interleaved
        const std::vector<float>& normals,        // xyz interleaved (optional, can be empty)
        const std::vector<float>& colors,         // rgb interleaved (optional)
        const std::vector<uint32_t>& indices,     // triangle indices (optional)
        const EnhancedConversionConfig& config = {});
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Utility functions for enhanced processing
namespace enhanced {
    // Compute k-nearest neighbor distances for adaptive scaling
    std::vector<float> computeKnnDistances(
        const std::vector<float>& positions,
        int k,
        ComputeProvider* provider = nullptr);
    
    // Compute surface frame (tangent, bitangent) from normal
    void computeSurfaceFrame(
        const float normal[3],
        float tangent[3],
        float bitangent[3]);
    
    // Create quaternion from surface frame
    void surfaceFrameToQuaternion(
        const float normal[3],
        const float tangent[3],
        float quat[4]);  // w, x, y, z
    
    // Estimate normals from positions using PCA
    std::vector<float> estimateNormals(
        const std::vector<float>& positions,
        int k_neighbors = 8);
}

} // namespace melkor
