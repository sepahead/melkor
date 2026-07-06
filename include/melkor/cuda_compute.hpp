#pragma once

#include "melkor/gaussian_data.hpp"
#include <string>
#include <memory>

namespace melkor {
namespace cuda {

// CUDA device information
struct DeviceInfo {
    std::string name;
    size_t total_memory;
    int compute_capability_major;
    int compute_capability_minor;
    int max_threads_per_block;
};

// CUDA compute context
class CudaContext {
public:
    CudaContext();
    ~CudaContext();
    
    // Check if CUDA is available
    static bool isAvailable();
    
    // Initialize with default device
    bool initialize();
    
    // Initialize with specific device by ID
    bool initialize(int device_id);
    
    // Get device info
    DeviceInfo getDeviceInfo() const;
    
    // Check if initialized
    bool isInitialized() const;
    
    // Get device ID
    int getDeviceId() const;
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Gaussian processing operations using CUDA compute
class GaussianProcessor {
public:
    GaussianProcessor(CudaContext& context);
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
    
    // Compute covariance matrices from scale and rotation
    // Returns flattened upper triangular covariance matrices (6 floats per splat)
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

    // Grid-accelerated k-NN statistics, mirroring the Metal
    // GaussianProcessor::knnStatsGrid API. The uniform grid comes from
    // melkor::grid::buildGrid so all backends search identical cells.
    // Returns 4 floats per point (mean distance to k nearest, gap vector
    // xyz = point minus neighbor centroid), or empty on failure.
    std::vector<float> knnStatsGrid(
        const std::vector<float>& positions,
        const std::vector<uint32_t>& cell_entries,
        const std::vector<uint32_t>& cell_starts,
        const std::vector<uint32_t>& cell_counts,
        const float grid_origin[3], float cell_size,
        const int grid_dims[3], int k_neighbors);

    // Grid-accelerated candidate filtering for densification, mirroring the
    // Metal filterCandidatesGrid API. Per candidate: (distance to nearest
    // cloud point, 1.0 if forward support exists within support_radius; a
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

} // namespace cuda
} // namespace melkor

// Note: this header used to alias `namespace melkor::metal = melkor::cuda`
// under MELKOR_HAS_CUDA for legacy call sites. The alias is gone: it
// collides with the real melkor::metal namespace (metal_compute.hpp, whose
// stubs are compiled on every non-Metal platform) in any translation unit
// that includes both headers.
