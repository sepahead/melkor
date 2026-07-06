#include "melkor/gaussian_data.hpp"
#include <cuda_runtime.h>
#include <cmath>
#include <cstdio>

namespace melkor {
namespace cuda {

// Cache optimal thread block size per device
// Note: This global is not thread-safe, but the race is benign:
// worst case is multiple threads compute the same value simultaneously.
static int g_optimal_threads_per_block = 0;

static int getOptimalThreadsPerBlock() {
    if (g_optimal_threads_per_block > 0) {
        return g_optimal_threads_per_block;
    }
    
    int device_id = 0;
    cudaGetDevice(&device_id);
    
    cudaDeviceProp prop;
    cudaError_t err = cudaGetDeviceProperties(&prop, device_id);
    if (err != cudaSuccess) {
        // Fallback to safe default
        g_optimal_threads_per_block = 256;
        return g_optimal_threads_per_block;
    }
    
    // Use optimal block size based on device capabilities
    // For simple element-wise kernels, 256 is usually optimal for occupancy
    // but we cap at device max and prefer multiples of warp size (32)
    int max_threads = prop.maxThreadsPerBlock;
    int warp_size = prop.warpSize;
    
    // Start with 256 as a good default for most modern GPUs
    int optimal = 256;
    
    // For older GPUs with lower limits, reduce
    if (max_threads < optimal) {
        optimal = (max_threads / warp_size) * warp_size;
    }
    
    // For very high-end GPUs (e.g., H100), can use larger blocks
    if (prop.major >= 9) {
        optimal = 512;
    }
    
    g_optimal_threads_per_block = optimal > 0 ? optimal : 256;
    return g_optimal_threads_per_block;
}

// CUDA kernel to normalize quaternions
__global__ void normalizeQuaternionsKernel(PackedGaussian* splats, size_t count) {
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= count) return;
    
    PackedGaussian& splat = splats[idx];
    
    float& w = splat.rotation[0];
    float& x = splat.rotation[1];
    float& y = splat.rotation[2];
    float& z = splat.rotation[3];
    
    float len = sqrtf(w * w + x * x + y * y + z * z);

    // Semantics match the Metal normalize_quaternions kernel: zero length
    // -> identity, sign canonicalized to w >= 0 (q and -q are the same
    // rotation), so all backends produce component-comparable output.
    if (len > 0.0f) {
        float inv_len = 1.0f / len;
        w *= inv_len;
        x *= inv_len;
        y *= inv_len;
        z *= inv_len;
    } else {
        w = 1.0f;
        x = y = z = 0.0f;
    }
    if (w < 0.0f) {
        w = -w;
        x = -x;
        y = -y;
        z = -z;
    }
}

// CUDA kernel to scale positions
__global__ void scalePositionsKernel(PackedGaussian* splats, size_t count, float scale) {
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= count) return;
    
    PackedGaussian& splat = splats[idx];
    splat.position[0] *= scale; // x
    splat.position[1] *= scale; // y
    splat.position[2] *= scale; // z
}

// CUDA kernel to transform coordinates
__global__ void transformCoordinatesKernel(PackedGaussian* splats, size_t count,
                                          const float* transform) {
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= count) return;
    
    PackedGaussian& splat = splats[idx];
    
    float x = splat.position[0];
    float y = splat.position[1];
    float z = splat.position[2];
    
    splat.position[0] = transform[0] * x + transform[4] * y + transform[8]  * z + transform[12];
    splat.position[1] = transform[1] * x + transform[5] * y + transform[9]  * z + transform[13];
    splat.position[2] = transform[2] * x + transform[6] * y + transform[10] * z + transform[14];
    
    // For now, we don't transform normals or rotation; that could be added if needed.
}

// Host functions to launch kernels with error checking
// Returns true on success, false on error

bool launchNormalizeQuaternions(PackedGaussian* d_splats, size_t count) {
    if (count == 0) return true;
    const int threads_per_block = getOptimalThreadsPerBlock();
    const int num_blocks = static_cast<int>((count + threads_per_block - 1) / threads_per_block);
    
    normalizeQuaternionsKernel<<<num_blocks, threads_per_block>>>(d_splats, count);
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        std::printf("normalizeQuaternionsKernel launch failed: %s\n", cudaGetErrorString(err));
        return false;
    }
    return true;
}

bool launchScalePositions(PackedGaussian* d_splats, size_t count, float scale) {
    if (count == 0) return true;
    const int threads_per_block = getOptimalThreadsPerBlock();
    const int num_blocks = static_cast<int>((count + threads_per_block - 1) / threads_per_block);
    
    scalePositionsKernel<<<num_blocks, threads_per_block>>>(d_splats, count, scale);
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        std::printf("scalePositionsKernel launch failed: %s\n", cudaGetErrorString(err));
        return false;
    }
    return true;
}

bool launchTransformCoordinates(PackedGaussian* d_splats, size_t count,
                                const float* d_transform) {
    if (count == 0) return true;
    if (d_transform == nullptr) {
        std::printf("transformCoordinatesKernel: null transform matrix\n");
        return false;
    }
    
    const int threads_per_block = getOptimalThreadsPerBlock();
    const int num_blocks = static_cast<int>((count + threads_per_block - 1) / threads_per_block);
    
    transformCoordinatesKernel<<<num_blocks, threads_per_block>>>(d_splats, count, d_transform);
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        std::printf("transformCoordinatesKernel launch failed: %s\n", cudaGetErrorString(err));
        return false;
    }
    return true;
}

} // namespace cuda
} // namespace melkor
