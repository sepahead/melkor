#pragma once

// Abstract compute provider — unifies Metal, CUDA, and CPU backends behind a
// single interface for Gaussian cloud processing operations.
//
// Backends are compiled in optionally (Metal on Apple, CUDA on Linux/NVIDIA) and CPU is
// always present as the semantic reference implementation. The interface is identical across
// all of them, so no caller needs #ifdef dispatch.
//
// The concrete backends are constructed through BackendRegistry (backend_registry.hpp), which
// they register themselves into. melkor_core deliberately does not know that Metal or CUDA
// exist: it was core's knowledge of them -- through a factory declared here but defined inside
// each backend -- that created the circular link this design removes (P0-05).
//
// There is no raw-pointer escape hatch. An earlier rawContext() returned a void* that callers
// cast to metal::MetalContext*, which is how platform types ended up inside platform-neutral
// code (P1-02). Every operation a caller needs is on this interface.

#include "melkor/gaussian_data.hpp"
#include <cstdint>
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
    // Scale is read in GaussianCloud's native log space and exponentiated
    // internally before covariance construction.
    virtual std::vector<float> computeCovariances(const GaussianCloud& cloud) = 0;

    // Apply all enabled transformations in one batch.
    virtual bool processCloud(GaussianCloud& cloud,
                              const ProcessConfig& config) = 0;

    // --- Grid-accelerated neighbourhood operations ---
    //
    // These were previously reached by casting rawContext() to a metal::MetalContext* and
    // constructing a metal::GaussianProcessor directly, which dragged platform types into
    // platform-neutral code and forced melkor_core to link against the backend. They are part
    // of the abstract contract now, so a caller never names a backend.
    //
    // Every backend must implement them. The CPU implementation is the semantic reference: a
    // GPU result that disagrees with it is a bug in the GPU path, not a new answer.

    // Neighbourhood statistics over a uniform grid. Returns 4 floats per point: the mean
    // distance to the k nearest neighbours, then the gap vector (point minus neighbour
    // centroid) as xyz. Returns empty on failure, which the caller must treat as "fall back",
    // not as "there were no neighbours".
    virtual std::vector<float> knnStatsGrid(
        const std::vector<float>& positions,
        const std::vector<uint32_t>& cell_entries,
        const std::vector<uint32_t>& cell_starts,
        const std::vector<uint32_t>& cell_counts,
        const float grid_origin[3], float cell_size,
        const int grid_dims[3], int k_neighbors) = 0;

    // Candidate filtering for densification. Returns 2 floats per candidate: the distance to
    // the nearest existing point, and 1.0 when a point exists within support_radius in the
    // forward half-space of the paired direction (a zero direction skips that test). Returns
    // empty on failure.
    virtual std::vector<float> filterCandidatesGrid(
        const std::vector<float>& candidates,
        const std::vector<float>& directions,
        const std::vector<float>& positions,
        const std::vector<uint32_t>& cell_entries,
        const std::vector<uint32_t>& cell_starts,
        const std::vector<uint32_t>& cell_counts,
        const float grid_origin[3], float cell_size,
        const int grid_dims[3],
        float min_separation, float support_radius) = 0;
};

// Backend construction entry points.
//
// Each is defined in its own backend library and is called ONLY by melkor_runtime's
// register_builtin_backends(). Core never calls them: core must not know that Metal exists.
//
// Each returns nullptr when its backend is compiled in but unusable on this machine.
std::unique_ptr<ComputeProvider> createCpuProvider();

#if defined(MELKOR_HAS_METAL)
std::unique_ptr<ComputeProvider> createMetalProvider();
#endif

#if defined(MELKOR_HAS_CUDA)
std::unique_ptr<ComputeProvider> createCudaProvider();
#endif

} // namespace melkor
