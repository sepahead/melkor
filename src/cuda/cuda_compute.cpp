#include "melkor/cuda_compute.hpp"
#include "melkor/compute_provider.hpp"
#include <cuda_runtime.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <algorithm>

namespace melkor {
namespace cuda {

// Forward declarations for CUDA kernels (implemented in gaussian_processor.cu)
// These return true on success, false on kernel launch error
extern bool launchNormalizeQuaternions(PackedGaussian* d_splats, size_t count);
extern bool launchScalePositions(PackedGaussian* d_splats, size_t count, float scale);
extern bool launchTransformCoordinates(PackedGaussian* d_splats, size_t count, const float* transform);
extern bool launchKnnStatsGrid(const float* d_positions,
                               const unsigned int* d_entries,
                               const unsigned int* d_starts,
                               const unsigned int* d_counts,
                               const float grid_origin[3], float cell_size,
                               const int grid_dims[3], int k_neighbors,
                               size_t num_points, float* d_out_stats);
extern bool launchFilterCandidatesGrid(const float* d_candidates,
                                       const float* d_directions,
                                       const float* d_positions,
                                       const unsigned int* d_entries,
                                       const unsigned int* d_starts,
                                       const unsigned int* d_counts,
                                       const float grid_origin[3], float cell_size,
                                       const int grid_dims[3],
                                       float min_separation, float support_radius,
                                       size_t num_points, size_t num_queries,
                                       float* d_out_filter);

// ============================================================================
// CudaContext Implementation
// ============================================================================

class CudaContext::Impl {
public:
    int device_id = -1;
    bool initialized = false;
    DeviceInfo device_info;
    
    bool initialize(int device) {
        device_id = device;
        
        // Set device
        cudaError_t set_err = cudaSetDevice(device_id);
        if (set_err != cudaSuccess) {
            std::cerr << "Failed to set CUDA device " << device_id
                      << ": " << cudaGetErrorString(set_err) << std::endl;
            return false;
        }
        
        // Get device properties
        cudaDeviceProp prop{};
        cudaError_t prop_err = cudaGetDeviceProperties(&prop, device_id);
        if (prop_err != cudaSuccess) {
            std::cerr << "Failed to get device properties: "
                      << cudaGetErrorString(prop_err) << std::endl;
            return false;
        }
        
        device_info.name = prop.name;
        device_info.total_memory = prop.totalGlobalMem;
        device_info.compute_capability_major = prop.major;
        device_info.compute_capability_minor = prop.minor;
        device_info.max_threads_per_block = prop.maxThreadsPerBlock;
        
        initialized = true;
        return true;
    }
};

CudaContext::CudaContext() : impl_(std::make_unique<Impl>()) {}
CudaContext::~CudaContext() = default;

bool CudaContext::isAvailable() {
    int device_count = 0;
    cudaError_t error = cudaGetDeviceCount(&device_count);
    return (error == cudaSuccess && device_count > 0);
}

bool CudaContext::initialize() {
    return initialize(0);  // Use device 0 by default
}

bool CudaContext::initialize(int device_id) {
    return impl_->initialize(device_id);
}

DeviceInfo CudaContext::getDeviceInfo() const {
    return impl_->device_info;
}

bool CudaContext::isInitialized() const {
    return impl_->initialized;
}

int CudaContext::getDeviceId() const {
    return impl_->device_id;
}

// ============================================================================
// GaussianProcessor Implementation
// ============================================================================

class GaussianProcessor::Impl {
public:
    CudaContext& context;
    PackedGaussian* d_splats = nullptr;
    size_t allocated_count = 0;
    
    Impl(CudaContext& ctx) : context(ctx) {}
    
    ~Impl() {
        if (d_splats) {
            cudaFree(d_splats);
        }
    }
    
    bool ensureDeviceMemory(size_t count) {
        if (count == 0) {
            return true;
        }
        if (count <= allocated_count && d_splats != nullptr) {
            return true;
        }
        
        if (d_splats) {
            cudaFree(d_splats);
            d_splats = nullptr;
            allocated_count = 0;
        }
        
        cudaError_t alloc_err = cudaMalloc(&d_splats, count * sizeof(PackedGaussian));
        if (alloc_err != cudaSuccess) {
            std::cerr << "Failed to allocate device memory for " << count
                      << " PackedGaussian objects: " << cudaGetErrorString(alloc_err)
                      << std::endl;
            return false;
        }
        
        allocated_count = count;
        return true;
    }
    
    bool uploadToDevice(const GaussianCloud& cloud) {
        const size_t count = cloud.size();
        if (!ensureDeviceMemory(count)) {
            return false;
        }
        if (count == 0) {
            return true;
        }
        
        // Convert to GPU-friendly packed format
        std::vector<PackedGaussian> packed = cloud.toPackedFormat();
        if (packed.size() != count) {
            std::cerr << "PackedGaussian size mismatch" << std::endl;
            return false;
        }
        
        cudaError_t copy_err = cudaMemcpy(d_splats,
                                          packed.data(),
                                          count * sizeof(PackedGaussian),
                                          cudaMemcpyHostToDevice);
        if (copy_err != cudaSuccess) {
            std::cerr << "Failed to upload data to device: "
                      << cudaGetErrorString(copy_err) << std::endl;
            return false;
        }
        
        return true;
    }
    
    bool downloadFromDevice(GaussianCloud& cloud) {
        const size_t count = cloud.size();
        if (count == 0) {
            return true;
        }
        if (!d_splats) {
            std::cerr << "Device buffer is null in downloadFromDevice" << std::endl;
            return false;
        }
        
        std::vector<PackedGaussian> packed(count);
        cudaError_t copy_err = cudaMemcpy(packed.data(),
                                          d_splats,
                                          count * sizeof(PackedGaussian),
                                          cudaMemcpyDeviceToHost);
        if (copy_err != cudaSuccess) {
            std::cerr << "Failed to download data from device: "
                      << cudaGetErrorString(copy_err) << std::endl;
            return false;
        }
        
        // Write back into existing splats to preserve sh_rest and other metadata
        auto& splats = cloud.splats();
        if (splats.size() != count) {
            std::cerr << "GaussianCloud size changed during GPU processing" << std::endl;
            return false;
        }
        
        for (size_t i = 0; i < count; ++i) {
            const PackedGaussian& p = packed[i];
            GaussianSplat& s = splats[i];
            
            s.x = p.position[0];
            s.y = p.position[1];
            s.z = p.position[2];
            s.opacity = p.position[3];
            
            s.f_dc_0 = p.color[0];
            s.f_dc_1 = p.color[1];
            s.f_dc_2 = p.color[2];
            
            s.scale_0 = p.scale[0];
            s.scale_1 = p.scale[1];
            s.scale_2 = p.scale[2];
            
            s.rot_0 = p.rotation[0];
            s.rot_1 = p.rotation[1];
            s.rot_2 = p.rotation[2];
            s.rot_3 = p.rotation[3];
        }
        
        return true;
    }
};

GaussianProcessor::GaussianProcessor(CudaContext& context)
    : impl_(std::make_unique<Impl>(context)) {}

GaussianProcessor::~GaussianProcessor() = default;

bool GaussianProcessor::transformCoordinates(GaussianCloud& cloud,
                                             const float transform_matrix[16]) {
    if (!impl_->context.isInitialized()) {
        std::cerr << "CUDA context not initialized" << std::endl;
        return false;
    }
    
    if (!impl_->uploadToDevice(cloud)) {
        return false;
    }
    
    // Upload transform matrix
    float* d_transform = nullptr;
    cudaError_t alloc_err = cudaMalloc(&d_transform, 16 * sizeof(float));
    if (alloc_err != cudaSuccess) {
        std::cerr << "Failed to allocate device memory for transform matrix: "
                  << cudaGetErrorString(alloc_err) << std::endl;
        return false;
    }
    cudaError_t copy_err = cudaMemcpy(d_transform,
                                      transform_matrix,
                                      16 * sizeof(float),
                                      cudaMemcpyHostToDevice);
    if (copy_err != cudaSuccess) {
        std::cerr << "Failed to upload transform matrix to device: "
                  << cudaGetErrorString(copy_err) << std::endl;
        cudaFree(d_transform);
        return false;
    }
    
    if (!launchTransformCoordinates(impl_->d_splats, cloud.size(), d_transform)) {
        std::cerr << "transformCoordinates kernel launch failed" << std::endl;
        cudaFree(d_transform);
        return false;
    }
    cudaFree(d_transform);
    
    cudaError_t sync_err = cudaDeviceSynchronize();
    if (sync_err != cudaSuccess) {
        std::cerr << "CUDA sync error in transformCoordinates: "
                  << cudaGetErrorString(sync_err) << std::endl;
        return false;
    }
    
    return impl_->downloadFromDevice(cloud);
}

bool GaussianProcessor::normalizeQuaternions(GaussianCloud& cloud) {
    if (!impl_->context.isInitialized()) {
        std::cerr << "CUDA context not initialized" << std::endl;
        return false;
    }
    
    if (!impl_->uploadToDevice(cloud)) {
        return false;
    }
    
    if (!launchNormalizeQuaternions(impl_->d_splats, cloud.size())) {
        std::cerr << "normalizeQuaternions kernel launch failed" << std::endl;
        return false;
    }
    
    cudaError_t sync_err = cudaDeviceSynchronize();
    if (sync_err != cudaSuccess) {
        std::cerr << "CUDA sync error in normalizeQuaternions: "
                  << cudaGetErrorString(sync_err) << std::endl;
        return false;
    }
    
    return impl_->downloadFromDevice(cloud);
}

bool GaussianProcessor::scalePositions(GaussianCloud& cloud, float scale) {
    if (!impl_->context.isInitialized()) {
        std::cerr << "CUDA context not initialized" << std::endl;
        return false;
    }
    
    if (!impl_->uploadToDevice(cloud)) {
        return false;
    }
    
    if (!launchScalePositions(impl_->d_splats, cloud.size(), scale)) {
        std::cerr << "scalePositions kernel launch failed" << std::endl;
        return false;
    }
    
    cudaError_t sync_err = cudaDeviceSynchronize();
    if (sync_err != cudaSuccess) {
        std::cerr << "CUDA sync error in scalePositions: "
                  << cudaGetErrorString(sync_err) << std::endl;
        return false;
    }
    
    return impl_->downloadFromDevice(cloud);
}

bool GaussianProcessor::rgbToShDc(GaussianCloud& cloud) {
    // These operations are cheap compared with PCIe transfer. Keep the CUDA
    // provider operation-complete with a deterministic host fallback until a
    // fused device pipeline is warranted.
    auto cpu = createCpuProvider();
    return cpu && cpu->rgbToShDc(cloud);
}

bool GaussianProcessor::opacityToLogit(GaussianCloud& cloud) {
    auto cpu = createCpuProvider();
    return cpu && cpu->opacityToLogit(cloud);
}

bool GaussianProcessor::sortByDistance(GaussianCloud& cloud,
                                       float camera_x, float camera_y, float camera_z) {
    auto cpu = createCpuProvider();
    return cpu && cpu->sortByDistance(cloud, camera_x, camera_y, camera_z);
}

std::vector<float> GaussianProcessor::computeCovariances(const GaussianCloud& cloud) {
    auto cpu = createCpuProvider();
    return cpu ? cpu->computeCovariances(cloud) : std::vector<float>{};
}

namespace {

// RAII device buffer: uploads host data on construction, frees on scope
// exit, so the grid wrappers below cannot leak on any early return.
template <typename T>
class DeviceBuffer {
public:
    DeviceBuffer(const T* host, size_t count) : count_(count) {
        if (count == 0) return;
        if (cudaMalloc(&ptr_, count * sizeof(T)) != cudaSuccess) {
            ptr_ = nullptr;
            return;
        }
        if (host &&
            cudaMemcpy(ptr_, host, count * sizeof(T),
                       cudaMemcpyHostToDevice) != cudaSuccess) {
            cudaFree(ptr_);
            ptr_ = nullptr;
        }
    }
    ~DeviceBuffer() {
        if (ptr_) cudaFree(ptr_);
    }
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    T* get() const { return ptr_; }
    bool ok() const { return count_ == 0 || ptr_ != nullptr; }
    bool download(T* host) const {
        if (!ptr_) return false;
        return cudaMemcpy(host, ptr_, count_ * sizeof(T),
                          cudaMemcpyDeviceToHost) == cudaSuccess;
    }

private:
    T* ptr_ = nullptr;
    size_t count_ = 0;
};

} // namespace

std::vector<float> GaussianProcessor::knnStatsGrid(
    const std::vector<float>& positions,
    const std::vector<uint32_t>& cell_entries,
    const std::vector<uint32_t>& cell_starts,
    const std::vector<uint32_t>& cell_counts,
    const float grid_origin[3], float cell_size,
    const int grid_dims[3], int k_neighbors) {

    const size_t num_points = positions.size() / 3;
    if (num_points == 0 || cell_entries.size() != num_points ||
        cell_starts.empty() || cell_starts.size() != cell_counts.size() ||
        cell_size <= 0.0f || !impl_->context.isInitialized()) {
        return {};
    }
    // The kernels index cell arrays by (z*dy + y)*dx + x; mismatched dims
    // would read past the device buffers.
    if (static_cast<size_t>(grid_dims[0]) * grid_dims[1] * grid_dims[2] !=
        cell_starts.size()) {
        return {};
    }

    DeviceBuffer<float> d_pos(positions.data(), positions.size());
    DeviceBuffer<unsigned int> d_entries(cell_entries.data(), cell_entries.size());
    DeviceBuffer<unsigned int> d_starts(cell_starts.data(), cell_starts.size());
    DeviceBuffer<unsigned int> d_counts(cell_counts.data(), cell_counts.size());
    DeviceBuffer<float> d_out(nullptr, num_points * 4);
    if (!d_pos.ok() || !d_entries.ok() || !d_starts.ok() || !d_counts.ok() ||
        !d_out.ok()) {
        return {};
    }

    if (!launchKnnStatsGrid(d_pos.get(), d_entries.get(), d_starts.get(),
                            d_counts.get(), grid_origin, cell_size, grid_dims,
                            k_neighbors, num_points, d_out.get())) {
        return {};
    }
    cudaError_t sync_err = cudaDeviceSynchronize();
    if (sync_err != cudaSuccess) {
        std::cerr << "knnStatsGrid: CUDA sync error: "
                  << cudaGetErrorString(sync_err) << std::endl;
        return {};
    }

    std::vector<float> stats(num_points * 4);
    if (!d_out.download(stats.data())) return {};
    return stats;
}

std::vector<float> GaussianProcessor::filterCandidatesGrid(
    const std::vector<float>& candidates,
    const std::vector<float>& directions,
    const std::vector<float>& positions,
    const std::vector<uint32_t>& cell_entries,
    const std::vector<uint32_t>& cell_starts,
    const std::vector<uint32_t>& cell_counts,
    const float grid_origin[3], float cell_size,
    const int grid_dims[3],
    float min_separation, float support_radius) {

    const size_t num_queries = candidates.size() / 3;
    const size_t num_points = positions.size() / 3;
    if (num_queries == 0 || directions.size() < num_queries * 3 ||
        num_points == 0 || cell_entries.size() != num_points ||
        cell_starts.empty() || cell_starts.size() != cell_counts.size() ||
        cell_size <= 0.0f || !impl_->context.isInitialized()) {
        return {};
    }
    if (static_cast<size_t>(grid_dims[0]) * grid_dims[1] * grid_dims[2] !=
        cell_starts.size()) {
        return {};
    }

    DeviceBuffer<float> d_cand(candidates.data(), candidates.size());
    DeviceBuffer<float> d_dir(directions.data(), directions.size());
    DeviceBuffer<float> d_pos(positions.data(), positions.size());
    DeviceBuffer<unsigned int> d_entries(cell_entries.data(), cell_entries.size());
    DeviceBuffer<unsigned int> d_starts(cell_starts.data(), cell_starts.size());
    DeviceBuffer<unsigned int> d_counts(cell_counts.data(), cell_counts.size());
    DeviceBuffer<float> d_out(nullptr, num_queries * 2);
    if (!d_cand.ok() || !d_dir.ok() || !d_pos.ok() || !d_entries.ok() ||
        !d_starts.ok() || !d_counts.ok() || !d_out.ok()) {
        return {};
    }

    if (!launchFilterCandidatesGrid(d_cand.get(), d_dir.get(), d_pos.get(),
                                    d_entries.get(), d_starts.get(),
                                    d_counts.get(), grid_origin, cell_size,
                                    grid_dims, min_separation, support_radius,
                                    num_points, num_queries, d_out.get())) {
        return {};
    }
    cudaError_t sync_err = cudaDeviceSynchronize();
    if (sync_err != cudaSuccess) {
        std::cerr << "filterCandidatesGrid: CUDA sync error: "
                  << cudaGetErrorString(sync_err) << std::endl;
        return {};
    }

    std::vector<float> result(num_queries * 2);
    if (!d_out.download(result.data())) return {};
    return result;
}

bool GaussianProcessor::processCloud(GaussianCloud& cloud, const ProcessConfig& config) {
    if (!impl_->context.isInitialized()) {
        std::cerr << "CUDA context not initialized" << std::endl;
        return false;
    }
    
    // Upload once
    if (!impl_->uploadToDevice(cloud)) {
        return false;
    }
    
    // Apply operations on GPU
    if (config.normalize_quaternions) {
        if (!launchNormalizeQuaternions(impl_->d_splats, cloud.size())) {
            std::cerr << "processCloud: normalizeQuaternions kernel launch failed" << std::endl;
            return false;
        }
    }
    
    if (config.position_scale != 1.0f) {
        if (!launchScalePositions(impl_->d_splats, cloud.size(), config.position_scale)) {
            std::cerr << "processCloud: scalePositions kernel launch failed" << std::endl;
            return false;
        }
    }
    
    cudaError_t sync_err = cudaDeviceSynchronize();
    if (sync_err != cudaSuccess) {
        std::cerr << "CUDA sync error in processCloud: "
                  << cudaGetErrorString(sync_err) << std::endl;
        return false;
    }
    
    if (!impl_->downloadFromDevice(cloud)) {
        return false;
    }

    // Apply operations without dedicated CUDA kernels on the CPU side, so
    // they are not silently skipped when requested via ProcessConfig.
    if (config.transform_y_up_to_z_up) {
        for (auto& s : cloud.splats()) {
            float tmp = s.y;
            s.y = -s.z;
            s.z = tmp;
        }
    }
    // When these conversions are requested the cloud still holds raw RGB /
    // linear opacity, so apply the forward transform directly (matching the
    // CPU provider and the Metal process_all kernel).
    if (config.convert_colors_to_sh) {
        for (auto& s : cloud.splats()) {
            s.f_dc_0 = utils::rgbToShDc(s.f_dc_0);
            s.f_dc_1 = utils::rgbToShDc(s.f_dc_1);
            s.f_dc_2 = utils::rgbToShDc(s.f_dc_2);
        }
    }
    if (config.convert_opacity_to_logit) {
        for (auto& s : cloud.splats()) {
            s.opacity = utils::logit(s.opacity);
        }
    }

    return true;
}

} // namespace cuda
} // namespace melkor
