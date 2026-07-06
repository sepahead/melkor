// Metal compute provider — adapts metal::MetalContext +
// metal::GaussianProcessor behind the ComputeProvider interface.
//
// Compiled only on macOS with Metal enabled (part of melkor_metal).
// Also supplies the ComputeProvider::create() factory for Metal builds.

#include "melkor/compute_provider.hpp"
#include "melkor/metal_compute.hpp"

#include <memory>

namespace melkor {

class MetalComputeProvider : public ComputeProvider {
public:
    ComputeBackend backend() const override { return ComputeBackend::Metal; }
    std::string backendName() const override { return "Metal"; }

    bool isAvailable() const override {
        return metal::MetalContext::isAvailable();
    }

    bool initialize() override {
        if (!ctx_) {
            ctx_ = std::make_unique<metal::MetalContext>();
        }
        if (!ctx_->initialize()) {
            ctx_.reset();
            return false;
        }
        processor_ = std::make_unique<metal::GaussianProcessor>(*ctx_);
        return true;
    }

    bool isInitialized() const override {
        return ctx_ && ctx_->isInitialized();
    }

    ComputeDeviceInfo deviceInfo() const override {
        ComputeDeviceInfo info;
        info.backend = ComputeBackend::Metal;
        if (ctx_) {
            auto di = ctx_->getDeviceInfo();
            info.name = di.name;
            info.total_memory = di.recommended_max_working_set_size;
            info.max_threads = di.max_threads_per_threadgroup;
        }
        return info;
    }

    void* rawContext() const override {
        return ctx_.get();
    }

    bool transformCoordinates(GaussianCloud& cloud,
                              const float m[16]) override {
        return processor_ ? processor_->transformCoordinates(cloud, m) : false;
    }

    bool normalizeQuaternions(GaussianCloud& cloud) override {
        return processor_ ? processor_->normalizeQuaternions(cloud) : false;
    }

    bool scalePositions(GaussianCloud& cloud, float scale) override {
        return processor_ ? processor_->scalePositions(cloud, scale) : false;
    }

    bool rgbToShDc(GaussianCloud& cloud) override {
        return processor_ ? processor_->rgbToShDc(cloud) : false;
    }

    bool opacityToLogit(GaussianCloud& cloud) override {
        return processor_ ? processor_->opacityToLogit(cloud) : false;
    }

    bool sortByDistance(GaussianCloud& cloud,
                        float cx, float cy, float cz) override {
        return processor_ ? processor_->sortByDistance(cloud, cx, cy, cz) : false;
    }

    std::vector<float> computeCovariances(const GaussianCloud& cloud) override {
        return processor_ ? processor_->computeCovariances(cloud) : std::vector<float>{};
    }

    bool processCloud(GaussianCloud& cloud, const ProcessConfig& config) override {
        if (!processor_) return false;
        metal::GaussianProcessor::ProcessConfig mtl_cfg;
        mtl_cfg.normalize_quaternions = config.normalize_quaternions;
        mtl_cfg.convert_colors_to_sh = config.convert_colors_to_sh;
        mtl_cfg.convert_opacity_to_logit = config.convert_opacity_to_logit;
        mtl_cfg.position_scale = config.position_scale;
        mtl_cfg.transform_y_up_to_z_up = config.transform_y_up_to_z_up;
        return processor_->processCloud(cloud, mtl_cfg);
    }

private:
    std::unique_ptr<metal::MetalContext> ctx_;
    std::unique_ptr<metal::GaussianProcessor> processor_;
};

// --- Factory (Metal build) ---

std::unique_ptr<ComputeProvider> ComputeProvider::create() {
    if (metal::MetalContext::isAvailable()) {
        auto p = std::make_unique<MetalComputeProvider>();
        if (p->initialize()) return p;
    }
    // Fall back to CPU
    return createCpuProvider();
}

std::unique_ptr<ComputeProvider> ComputeProvider::create(ComputeBackend backend) {
    switch (backend) {
        case ComputeBackend::Metal: {
            auto p = std::make_unique<MetalComputeProvider>();
            if (p->isAvailable() && p->initialize()) return p;
            return nullptr;
        }
        case ComputeBackend::CPU:
            return createCpuProvider();
        case ComputeBackend::CUDA:
            return nullptr;  // CUDA not available on macOS
    }
    return nullptr;
}

} // namespace melkor
