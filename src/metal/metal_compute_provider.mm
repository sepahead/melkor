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

    // The grid operations, forwarding to the Metal kernels. Previously a caller reached
    // these by casting rawContext() to a metal:: context pointer and constructing a
    // metal::GaussianProcessor itself, which is how platform types leaked into
    // platform-neutral code and forced melkor_core to link back against this library
    // (P0-05, P1-02). They are part of the abstract ComputeProvider contract now.
    //
    // An empty return means "this backend could not do it", which the caller treats as a
    // signal to fall back -- never as "there were no neighbours".
    std::vector<float> knnStatsGrid(
        const std::vector<float>& positions,
        const std::vector<uint32_t>& cell_entries,
        const std::vector<uint32_t>& cell_starts,
        const std::vector<uint32_t>& cell_counts,
        const float grid_origin[3], float cell_size,
        const int grid_dims[3], int k_neighbors) override {
        if (!ctx_ || !ctx_->isInitialized()) return {};
        metal::GaussianProcessor processor(*ctx_);
        return processor.knnStatsGrid(positions, cell_entries, cell_starts, cell_counts,
                                      grid_origin, cell_size, grid_dims, k_neighbors);
    }

    std::vector<float> filterCandidatesGrid(
        const std::vector<float>& candidates,
        const std::vector<float>& directions,
        const std::vector<float>& positions,
        const std::vector<uint32_t>& cell_entries,
        const std::vector<uint32_t>& cell_starts,
        const std::vector<uint32_t>& cell_counts,
        const float grid_origin[3], float cell_size,
        const int grid_dims[3],
        float min_separation, float support_radius) override {
        if (!ctx_ || !ctx_->isInitialized()) return {};
        metal::GaussianProcessor processor(*ctx_);
        return processor.filterCandidatesGrid(candidates, directions, positions, cell_entries,
                                              cell_starts, cell_counts, grid_origin, cell_size,
                                              grid_dims, min_separation, support_radius);
    }

private:
    std::unique_ptr<metal::MetalContext> ctx_;
    std::unique_ptr<metal::GaussianProcessor> processor_;
};

// --- Factory (Metal build) ---



// The one entry point this backend exposes to the runtime layer.
//
// It deliberately does NOT define ComputeProvider::create(). A factory declared in
// platform-neutral core but defined inside the backend is precisely what forced core to link
// back to the backend and produced the circular static archives (P0-05). Core owns the factory
// now and consults a registry; melkor_runtime calls this function to populate it.
//
// Returns nullptr when the backend is compiled in but unusable on this machine -- no device,
// no driver, an unsupported GPU. That is a different condition from "not built into this
// binary", and a caller is entitled to tell them apart.
std::unique_ptr<ComputeProvider> createMetalProvider() {
    auto provider = std::make_unique<MetalComputeProvider>();
    if (!provider->isAvailable() || !provider->initialize()) {
        return nullptr;
    }
    return provider;
}

} // namespace melkor
