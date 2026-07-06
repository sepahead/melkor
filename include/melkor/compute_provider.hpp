#pragma once

// Abstract compute provider — unifies Metal, CUDA, and CPU backends behind a
// single interface for Gaussian cloud processing operations.
//
// Backends are selected at compile time (Metal on macOS, CUDA on Linux/NVIDIA,
// CPU fallback elsewhere) but the interface is identical, so callers in
// melkor_core and main.cpp never need #ifdef dispatch.  Metal-specific
// features (EnhancedConverter, GaussianFitter, DifferentiableRenderer) still
// take a metal::MetalContext* directly; obtain it from
// ComputeProvider::rawContext() when the backend is Metal.

#include "melkor/gaussian_data.hpp"
#include <memory>
#include <string>
#include <vector>

namespace melkor {

// Available compute backends
enum class ComputeBackend {
    Metal,
    CUDA,
    CPU
};

// Unified device information across all backends.
// Backend-specific fields (compute_capability) are zero when not applicable.
struct ComputeDeviceInfo {
    std::string name;
    ComputeBackend backend = ComputeBackend::CPU;
    size_t total_memory = 0;         // bytes (Metal: recommended working set, CUDA: device mem, CPU: 0)
    int max_threads = 0;             // Metal: threads/threadgroup, CUDA: threads/block, CPU: 0
    int compute_capability_major = 0; // CUDA only
    int compute_capability_minor = 0; // CUDA only
};

// Configuration for batch cloud processing.
// Identical to the former metal::GaussianProcessor::ProcessConfig /
// cuda::GaussianProcessor::ProcessConfig — promoted to a single top-level
// struct so the provider interface is self-contained.
struct ProcessConfig {
    bool normalize_quaternions = true;
    bool convert_colors_to_sh = true;
    bool convert_opacity_to_logit = true;
    float position_scale = 1.0f;
    bool transform_y_up_to_z_up = false;
};

// Abstract compute provider for GPU/CPU Gaussian processing.
class ComputeProvider {
public:
    virtual ~ComputeProvider() = default;

    // Factory: creates the best available backend for this platform.
    // Falls back to CPU if the preferred GPU backend is unavailable.
    static std::unique_ptr<ComputeProvider> create();

    // Factory: creates a specific backend.
    // Returns nullptr if the requested backend is unavailable on this platform.
    static std::unique_ptr<ComputeProvider> create(ComputeBackend backend);

    // --- Backend identification ---

    virtual ComputeBackend backend() const = 0;
    virtual std::string backendName() const = 0;

    // --- Lifecycle ---

    virtual bool isAvailable() const = 0;
    virtual bool initialize() = 0;
    virtual bool isInitialized() const = 0;
    virtual ComputeDeviceInfo deviceInfo() const = 0;

    // Raw handle access for backend-specific components.
    // Metal: returns metal::MetalContext*.
    // CUDA:  returns cuda::CudaContext*.
    // CPU:   returns nullptr.
    virtual void* rawContext() const = 0;

    // --- Gaussian processing operations ---

    virtual bool transformCoordinates(GaussianCloud& cloud,
                                      const float transform_matrix[16]) = 0;
    virtual bool normalizeQuaternions(GaussianCloud& cloud) = 0;
    virtual bool scalePositions(GaussianCloud& cloud, float scale) = 0;
    virtual bool rgbToShDc(GaussianCloud& cloud) = 0;
    virtual bool opacityToLogit(GaussianCloud& cloud) = 0;
    virtual bool sortByDistance(GaussianCloud& cloud,
                                float camera_x, float camera_y, float camera_z) = 0;

    // Compute covariance matrices from scale and rotation.
    // Returns flattened upper triangular covariance (6 floats per splat:
    // xx, xy, xz, yy, yz, zz).
    // NOTE: expects scale in LINEAR space.  GaussianCloud stores scale in log
    // space; callers must convert (exp) or use processCloud first.
    virtual std::vector<float> computeCovariances(const GaussianCloud& cloud) = 0;

    // Apply all enabled transformations in one batch.
    virtual bool processCloud(GaussianCloud& cloud,
                              const ProcessConfig& config) = 0;
};

// Internal: creates and initializes the CPU provider.  Implemented in
// cpu_compute_provider.cpp (melkor_core).  Used by the per-platform GPU
// library factories as a fallback when no GPU is available.
std::unique_ptr<ComputeProvider> createCpuProvider();

} // namespace melkor
