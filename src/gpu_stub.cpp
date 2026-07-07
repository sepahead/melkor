// GPU Stub Implementation
// This file provides stub implementations of the Metal GPU classes
// for platforms that don't support Metal (Linux, etc.)

#include "melkor/compute_provider.hpp"
#include "melkor/metal_compute.hpp"
#include "melkor/gaussian_fitter.hpp"
#include "melkor/enhanced_converter.hpp"

#include <utility>

namespace melkor {
namespace metal {

// ============================================================================
// MetalContext Stub
// ============================================================================

class MetalContext::Impl {
public:
    // No-op implementation
};

MetalContext::MetalContext() : impl_(nullptr) {}
MetalContext::~MetalContext() = default;

bool MetalContext::isAvailable() {
    return false;  // Metal not available on non-Apple platforms
}

bool MetalContext::initialize() {
    return false;
}

bool MetalContext::initialize(const std::string& /*device_name*/) {
    return false;
}

DeviceInfo MetalContext::getDeviceInfo() const {
    return DeviceInfo{
        "No GPU (Metal not available)",
        0,
        0,
        false
    };
}

bool MetalContext::isInitialized() const {
    return false;
}

void* MetalContext::getDevice() const {
    return nullptr;
}

void* MetalContext::getCommandQueue() const {
    return nullptr;
}

void* MetalContext::getLibrary() const {
    return nullptr;
}

// ============================================================================
// GaussianProcessor Stub
// ============================================================================

class GaussianProcessor::Impl {
public:
    // No-op implementation
};

GaussianProcessor::GaussianProcessor(MetalContext& /*context*/) : impl_(nullptr) {}
GaussianProcessor::~GaussianProcessor() = default;

bool GaussianProcessor::transformCoordinates(GaussianCloud& /*cloud*/,
                                             const float /*transform_matrix*/[16]) {
    return false;
}

bool GaussianProcessor::normalizeQuaternions(GaussianCloud& cloud) {
    // CPU fallback. Semantics match the Metal kernel: zero length ->
    // identity, sign canonicalized to w >= 0.
    for (size_t i = 0; i < cloud.size(); ++i) {
        auto& splat = cloud[i];
        float len = std::sqrt(splat.rot_0 * splat.rot_0 +
                             splat.rot_1 * splat.rot_1 +
                             splat.rot_2 * splat.rot_2 +
                             splat.rot_3 * splat.rot_3);
        if (len > 0.0f) {
            splat.rot_0 /= len;
            splat.rot_1 /= len;
            splat.rot_2 /= len;
            splat.rot_3 /= len;
        } else {
            splat.rot_0 = 1.0f;
            splat.rot_1 = splat.rot_2 = splat.rot_3 = 0.0f;
        }
        if (splat.rot_0 < 0.0f) {
            splat.rot_0 = -splat.rot_0;
            splat.rot_1 = -splat.rot_1;
            splat.rot_2 = -splat.rot_2;
            splat.rot_3 = -splat.rot_3;
        }
    }
    return true;
}

bool GaussianProcessor::scalePositions(GaussianCloud& cloud, float scale) {
    // CPU fallback
    for (size_t i = 0; i < cloud.size(); ++i) {
        cloud[i].x *= scale;
        cloud[i].y *= scale;
        cloud[i].z *= scale;
    }
    return true;
}

bool GaussianProcessor::rgbToShDc(GaussianCloud& /*cloud*/) {
    return false;  // Not implemented
}

bool GaussianProcessor::opacityToLogit(GaussianCloud& /*cloud*/) {
    return false;  // Not implemented
}

bool GaussianProcessor::sortByDistance(GaussianCloud& /*cloud*/,
                                       float /*camera_x*/, float /*camera_y*/, float /*camera_z*/) {
    return false;  // Not implemented
}

std::vector<float> GaussianProcessor::computeCovariances(const GaussianCloud& /*cloud*/) {
    return {};  // Not implemented
}

bool GaussianProcessor::processCloud(GaussianCloud& cloud, const ProcessConfig& config) {
    // CPU fallback with basic operations
    if (config.normalize_quaternions) {
        normalizeQuaternions(cloud);
    }
    if (config.position_scale != 1.0f) {
        scalePositions(cloud, config.position_scale);
    }
    return true;
}

std::vector<PackedGaussian> GaussianProcessor::enhancedConvert(
    const std::vector<float>& /*positions*/,
    const std::vector<float>& /*normals*/,
    const std::vector<float>& /*colors*/,
    const std::vector<float>& /*adaptive_scales*/,
    const EnhancedConvertConfig& /*config*/) {
    return {};  // Callers fall back to the CPU conversion path
}

std::vector<float> GaussianProcessor::computeKnnDistancesMetal(
    const std::vector<float>& /*positions*/,
    int /*k_neighbors*/) {
    return {};  // Callers fall back to the CPU k-NN path
}

std::vector<float> GaussianProcessor::knnStatsGrid(
    const std::vector<float>& /*positions*/,
    const std::vector<uint32_t>& /*cell_entries*/,
    const std::vector<uint32_t>& /*cell_starts*/,
    const std::vector<uint32_t>& /*cell_counts*/,
    const float /*grid_origin*/[3], float /*cell_size*/,
    const int /*grid_dims*/[3], int /*k_neighbors*/) {
    return {};  // Callers fall back to melkor::grid::knnStatsCpu
}

std::vector<float> GaussianProcessor::filterCandidatesGrid(
    const std::vector<float>& /*candidates*/,
    const std::vector<float>& /*directions*/,
    const std::vector<float>& /*positions*/,
    const std::vector<uint32_t>& /*cell_entries*/,
    const std::vector<uint32_t>& /*cell_starts*/,
    const std::vector<uint32_t>& /*cell_counts*/,
    const float /*grid_origin*/[3], float /*cell_size*/,
    const int /*grid_dims*/[3],
    float /*min_separation*/, float /*support_radius*/) {
    return {};  // Callers fall back to melkor::grid::candidateFilterCpu
}

} // namespace metal

// ============================================================================
// Camera Stub
// ============================================================================
// Camera's methods are defined in gaussian_fitter.mm on Metal builds; any
// non-Metal caller would otherwise hit undefined references only at Linux
// link time. Stub with identity matrices / a default-constructed camera.

void Camera::computeMatrices() {
    for (int i = 0; i < 16; ++i) {
        const float ident = (i % 5 == 0) ? 1.0f : 0.0f;
        view_matrix[i] = ident;
        proj_matrix[i] = ident;
        view_proj_matrix[i] = ident;
    }
}

Camera Camera::createOrbital(float /*distance*/, float /*azimuth*/,
                             float /*elevation*/, int width, int height,
                             float fov_y) {
    Camera cam{};
    cam.width = width;
    cam.height = height;
    cam.fov_y = fov_y;
    cam.aspect = height > 0 ? static_cast<float>(width) / height : 1.0f;
    cam.up[1] = 1.0f;
    cam.near_plane = 0.01f;
    cam.far_plane = 100.0f;
    cam.computeMatrices();
    return cam;
}

// ============================================================================
// GaussianFitter Stub
// ============================================================================

class GaussianFitter::Impl {
public:
    // No-op implementation
};

GaussianFitter::GaussianFitter() : impl_(nullptr) {}
GaussianFitter::GaussianFitter(metal::MetalContext& /*ctx*/) : impl_(nullptr) {}
GaussianFitter::~GaussianFitter() = default;

std::vector<uint8_t> GaussianFitter::renderView(const GaussianCloud& /*cloud*/,
                                                const Camera& /*camera*/,
                                                int /*width*/, int /*height*/) {
    return {};  // Rendering requires Metal
}

GaussianFitResult GaussianFitter::fitFromGlb(const std::string& /*glb_path*/,
                                              const GaussianFitConfig& /*config*/) {
    GaussianFitResult result;
    result.success = false;
    result.error_message = "Gaussian fitting requires Metal GPU acceleration (macOS only)";
    return result;
}

GaussianFitResult GaussianFitter::fitFromImages(const std::vector<std::vector<uint8_t>>& /*images*/,
                                                 const std::vector<Camera>& /*cameras*/,
                                                 const GaussianCloud& /*initial_cloud*/,
                                                 const GaussianFitConfig& /*config*/) {
    GaussianFitResult result;
    result.success = false;
    result.error_message = "Gaussian fitting requires Metal GPU acceleration (macOS only)";
    return result;
}

// ============================================================================
// DifferentiableRenderer Stub
// ============================================================================
// forward()/backward() and the ForwardResult special members are implemented
// in gaussian_fitter.mm on Metal builds. The stubs below keep every declared
// symbol defined on non-Metal platforms so new call sites fail at runtime
// with empty results, not at Linux link time.

class DifferentiableRenderer::Impl {
public:
    // No-op implementation
};

DifferentiableRenderer::DifferentiableRenderer(metal::MetalContext& /*ctx*/) : impl_(nullptr) {}
DifferentiableRenderer::~DifferentiableRenderer() = default;

DifferentiableRenderer::ForwardResult::~ForwardResult() = default;
DifferentiableRenderer::ForwardResult::ForwardResult(ForwardResult&& other) noexcept
    : image(std::move(other.image)),
      alpha(std::move(other.alpha)),
      internal_state(other.internal_state) {
    other.internal_state = nullptr;
}
DifferentiableRenderer::ForwardResult&
DifferentiableRenderer::ForwardResult::operator=(ForwardResult&& other) noexcept {
    if (this != &other) {
        image = std::move(other.image);
        alpha = std::move(other.alpha);
        internal_state = other.internal_state;
        other.internal_state = nullptr;
    }
    return *this;
}

DifferentiableRenderer::ForwardResult DifferentiableRenderer::forward(
    const std::vector<PackedGaussian>& /*gaussians*/,
    const Camera& /*camera*/,
    const float /*background*/[3]) {
    return {};  // Differentiable rendering requires Metal
}

DifferentiableRenderer::BackwardResult DifferentiableRenderer::backward(
    ForwardResult& /*forward_result*/,
    const std::vector<float>& /*grad_image*/) {
    return {};  // Differentiable rendering requires Metal
}

// ============================================================================
// MeshRenderer Stub
// ============================================================================

class MeshRenderer::Impl {
public:
    // No-op implementation
};

MeshRenderer::MeshRenderer(metal::MetalContext& /*ctx*/) : impl_(nullptr) {}
MeshRenderer::~MeshRenderer() = default;

std::vector<uint8_t> MeshRenderer::render(const Camera& /*camera*/, int /*width*/, int /*height*/) {
    // No-op: mesh rendering is only available with Metal. Return an empty
    // buffer; callers on non-Metal builds should not depend on visual output.
    return {};
}

void MeshRenderer::getBoundingBox(float /*min*/[3], float /*max*/[3]) const {
    // No-op: bounding box stays at caller-initialized values.
}

bool MeshRenderer::loadGlb(const std::string& /*path*/) {
    return false;
}


// ============================================================================
// ComputeProvider factory (CPU-only build)
// ============================================================================
// On CUDA builds this file is compiled into melkor_cuda for the Metal-API
// stubs above, but the factory comes from cuda_compute_provider.cpp — so it
// is compiled out here to avoid a duplicate definition.

#ifndef MELKOR_HAS_CUDA
std::unique_ptr<melkor::ComputeProvider> melkor::ComputeProvider::create() {
    return createCpuProvider();
}

std::unique_ptr<melkor::ComputeProvider> melkor::ComputeProvider::create(ComputeBackend backend) {
    if (backend == ComputeBackend::CPU) {
        return createCpuProvider();
    }
    return nullptr;  // No GPU backends available on this platform
}
#endif // MELKOR_HAS_CUDA
} // namespace melkor
