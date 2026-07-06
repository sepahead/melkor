// CUDA compute provider — adapts cuda::CudaContext +
// cuda::GaussianProcessor behind the ComputeProvider interface.
//
// Compiled only on Linux with CUDA enabled (part of melkor_cuda).
// Also supplies the ComputeProvider::create() factory for CUDA builds.

#include "melkor/compute_provider.hpp"
#include "melkor/cuda_compute.hpp"

#include <memory>

namespace melkor {

class CudaComputeProvider : public ComputeProvider {
public:
    ComputeBackend backend() const override { return ComputeBackend::CUDA; }
    std::string backendName() const override { return "CUDA"; }

    bool isAvailable() const override {
        return cuda::CudaContext::isAvailable();
    }

    bool initialize() override {
        if (!ctx_) {
            ctx_ = std::make_unique<cuda::CudaContext>();
        }
        if (!ctx_->initialize()) {
            ctx_.reset();
            return false;
        }
        processor_ = std::make_unique<cuda::GaussianProcessor>(*ctx_);
        return true;
    }

    bool isInitialized() const override {
        return ctx_ && ctx_->isInitialized();
    }

    ComputeDeviceInfo deviceInfo() const override {
        ComputeDeviceInfo info;
        info.backend = ComputeBackend::CUDA;
        if (ctx_) {
            auto di = ctx_->getDeviceInfo();
            info.name = di.name;
            info.total_memory = di.total_memory;
            info.max_threads = di.max_threads_per_block;
            info.compute_capability_major = di.compute_capability_major;
            info.compute_capability_minor = di.compute_capability_minor;
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
        cuda::GaussianProcessor::ProcessConfig cuda_cfg;
        cuda_cfg.normalize_quaternions = config.normalize_quaternions;
        cuda_cfg.convert_colors_to_sh = config.convert_colors_to_sh;
        cuda_cfg.convert_opacity_to_logit = config.convert_opacity_to_logit;
        cuda_cfg.position_scale = config.position_scale;
        cuda_cfg.transform_y_up_to_z_up = config.transform_y_up_to_z_up;
        return processor_->processCloud(cloud, cuda_cfg);
    }

private:
    std::unique_ptr<cuda::CudaContext> ctx_;
    std::unique_ptr<cuda::GaussianProcessor> processor_;
};

// --- Factory (CUDA build) ---

std::unique_ptr<ComputeProvider> ComputeProvider::create() {
    if (cuda::CudaContext::isAvailable()) {
        auto p = std::make_unique<CudaComputeProvider>();
        if (p->initialize()) return p;
    }
    // Fall back to CPU
    return createCpuProvider();
}

std::unique_ptr<ComputeProvider> ComputeProvider::create(ComputeBackend backend) {
    switch (backend) {
        case ComputeBackend::CUDA: {
            auto p = std::make_unique<CudaComputeProvider>();
            if (p->isAvailable() && p->initialize()) return p;
            return nullptr;
        }
        case ComputeBackend::CPU:
            return createCpuProvider();
        case ComputeBackend::Metal:
            return nullptr;  // Metal not available on Linux
    }
    return nullptr;
}

} // namespace melkor
