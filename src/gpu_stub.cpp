// GPU Stub Implementation
// This file provides stub implementations of the Metal GPU classes
// for platforms that don't support Metal (Linux, etc.)

#include "melkor/metal_compute.hpp"
#include "melkor/gaussian_fitter.hpp"
#include "melkor/enhanced_converter.hpp"

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
    // CPU fallback: normalize quaternions on CPU
    for (size_t i = 0; i < cloud.size(); ++i) {
        auto& splat = cloud[i];
        float len = std::sqrt(splat.rot_0 * splat.rot_0 + 
                             splat.rot_1 * splat.rot_1 + 
                             splat.rot_2 * splat.rot_2 + 
                             splat.rot_3 * splat.rot_3);
        if (len > 0.0001f) {
            splat.rot_0 /= len;
            splat.rot_1 /= len;
            splat.rot_2 /= len;
            splat.rot_3 /= len;
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

} // namespace metal

// ============================================================================
// GaussianFitter Stub
// ============================================================================

class GaussianFitter::Impl {
public:
    // No-op implementation
};

GaussianFitter::GaussianFitter(metal::MetalContext& /*ctx*/) : impl_(nullptr) {}
GaussianFitter::~GaussianFitter() = default;

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
// NOTE: forward()/backward() are only declared in the header and implemented
// in the Metal path (gaussian_fitter.mm). The previous stub defined render()
// and computeGradients() returning a GaussianGradient type that no longer
// exist in the public API, which broke Linux/CPU fallback builds. Those dead
// stubs were removed; on non-Metal platforms the renderer simply isn't
// constructible into a working state, which is consistent with the Fit mode
// requiring Metal (see main.cpp).

class DifferentiableRenderer::Impl {
public:
    // No-op implementation
};

DifferentiableRenderer::DifferentiableRenderer(metal::MetalContext& /*ctx*/) : impl_(nullptr) {}
DifferentiableRenderer::~DifferentiableRenderer() = default;

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

} // namespace melkor
