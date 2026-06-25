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
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace metal
} // namespace melkor
