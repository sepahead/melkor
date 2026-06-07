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
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cuda
} // namespace melkor

// Alias for compatibility with existing code that uses metal namespace
#ifdef MELKOR_HAS_CUDA
namespace melkor {
namespace metal = cuda;
}
#endif
