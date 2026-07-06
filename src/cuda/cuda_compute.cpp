#include "melkor/cuda_compute.hpp"
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

bool GaussianProcessor::rgbToShDc(GaussianCloud& /*cloud*/) {
    // Not implemented yet - can be added later if needed
    return false;
}

bool GaussianProcessor::opacityToLogit(GaussianCloud& /*cloud*/) {
    // Not implemented yet - can be added later if needed
    return false;
}

bool GaussianProcessor::sortByDistance(GaussianCloud& /*cloud*/,
                                       float /*camera_x*/, float /*camera_y*/, float /*camera_z*/) {
    // Not implemented yet - can be added later if needed
    return false;
}

std::vector<float> GaussianProcessor::computeCovariances(const GaussianCloud& /*cloud*/) {
    // Not implemented yet - can be added later if needed
    return {};
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
