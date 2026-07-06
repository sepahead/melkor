#pragma once

#include "melkor/gaussian_data.hpp"
#include <string>
#include <memory>
#include <functional>

namespace melkor {
namespace metal {

// Metal device information
struct DeviceInfo {
    std::string name;
    uint64_t recommended_max_working_set_size;
    uint32_t max_threads_per_threadgroup;
    bool supports_family_apple7;  // Apple Silicon
};

// Metal compute context
class MetalContext {
public:
    MetalContext();
    ~MetalContext();
    
    // Check if Metal is available
    static bool isAvailable();
    
    // Initialize with default device
    bool initialize();
    
    // Initialize with specific device by name
    bool initialize(const std::string& device_name);
    
    // Get device info
    DeviceInfo getDeviceInfo() const;
    
    // Check if initialized
    bool isInitialized() const;
    
    // Get implementation (for internal use)
    void* getDevice() const;       // Returns id<MTLDevice>
    void* getCommandQueue() const; // Returns id<MTLCommandQueue>
    void* getLibrary() const;      // Returns id<MTLLibrary>
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Gaussian processing operations using Metal compute
class GaussianProcessor {
public:
    GaussianProcessor(MetalContext& context);
    ~GaussianProcessor();
    
    // Transform coordinates (e.g., Y-up to Z-up)
    bool transformCoordinates(GaussianCloud& cloud,
                             const float transform_matrix[16]);
    
    // Normalize all quaternions in parallel
    bool normalizeQuaternions(GaussianCloud& cloud);
    
    // Apply scale factor to all positions
    bool scalePositions(GaussianCloud& cloud, float scale);
    
    // Convert colors from RGB to spherical harmonics DC
    bool rgbToShDc(GaussianCloud& cloud);
    
    // Convert opacity from linear to logit space
    bool opacityToLogit(GaussianCloud& cloud);
    
    // Sort splats by distance from camera (for rendering order)
    bool sortByDistance(GaussianCloud& cloud,
                       float camera_x, float camera_y, float camera_z);
    
    // Compute covariance matrices from scale and rotation.
    // Returns flattened upper triangular covariance matrices (6 floats per splat:
    // xx, xy, xz, yy, yz, zz).
    // NOTE: The kernel expects scale in LINEAR space. GaussianCloud stores scale
    // in log space (as written by the GLB reader). Callers must convert
    // (exp) before calling, or use processCloud with scale_to_log first.
    std::vector<float> computeCovariances(const GaussianCloud& cloud);
    
    // Process entire cloud with all transformations
    struct ProcessConfig {
        bool normalize_quaternions = true;
        bool convert_colors_to_sh = true;
        bool convert_opacity_to_logit = true;
        float position_scale = 1.0f;
        bool transform_y_up_to_z_up = false;
    };
    
    bool processCloud(GaussianCloud& cloud, const ProcessConfig& config);

    // Metal-accelerated enhanced conversion: converts mesh vertices to
    // Gaussian splats in parallel on GPU. Each thread handles one vertex:
    // position transform, color to SH DC, opacity logit, adaptive scale to
    // log, quaternion from normal. Returns packed gaussians or empty on
    // failure.
    struct EnhancedConvertConfig {
        float scale_factor = 0.5f;
        float min_scale = 0.001f;
        float max_scale = 0.1f;
        float normal_scale_ratio = 0.3f;
        float default_opacity = 0.95f;
        float position_scale = 1.0f;
        bool convert_coordinate_system = true;
        bool use_surface_alignment = true;
        // Used when no (or short) vertex-color array is supplied — matches
        // the CPU path's EnhancedConversionConfig::default_color.
        float default_color[3] = {0.5f, 0.5f, 0.5f};
    };

    std::vector<PackedGaussian> enhancedConvert(
        const std::vector<float>& positions,
        const std::vector<float>& normals,
        const std::vector<float>& colors,
        const std::vector<float>& adaptive_scales,
        const EnhancedConvertConfig& config);

    // Metal-accelerated brute-force k-NN average distance for small clouds
    // (n < ~10K). O(n^2) but embarrassingly parallel. Returns per-point
    // average distance to k nearest neighbors.
    std::vector<float> computeKnnDistancesMetal(
        const std::vector<float>& positions,
        int k_neighbors);

    // Grid-accelerated k-NN statistics for clouds of any size. The uniform
    // grid comes from melkor::grid::buildGrid so the GPU walks the exact
    // same cells as the CPU reference (spatial_grid.cpp). Returns 4 floats
    // per point (mean distance to k nearest, gap vector xyz = point minus
    // neighbor centroid), or empty on failure.
    std::vector<float> knnStatsGrid(
        const std::vector<float>& positions,
        const std::vector<uint32_t>& cell_entries,
        const std::vector<uint32_t>& cell_starts,
        const std::vector<uint32_t>& cell_counts,
        const float grid_origin[3], float cell_size,
        const int grid_dims[3], int k_neighbors);

    // Grid-accelerated candidate filtering for densification. Per candidate:
    // (distance to nearest cloud point, 1.0 if a cloud point exists within
    // support_radius in the forward half-space of the paired direction; a
    // zero direction skips that test). Returns 2 floats per candidate, or
    // empty on failure.
    std::vector<float> filterCandidatesGrid(
        const std::vector<float>& candidates,
        const std::vector<float>& directions,
        const std::vector<float>& positions,
        const std::vector<uint32_t>& cell_entries,
        const std::vector<uint32_t>& cell_starts,
        const std::vector<uint32_t>& cell_counts,
        const float grid_origin[3], float cell_size,
        const int grid_dims[3],
        float min_separation, float support_radius);
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace metal
} // namespace melkor
