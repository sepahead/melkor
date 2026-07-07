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

// ============================================================================
// Grid-accelerated neighbor search (mirrors the Metal knn_stats_grid /
// filter_candidates_grid kernels and the melkor::grid CPU implementation).
// The uniform grid is built on the host (spatial_grid.cpp) and uploaded as
// flat arrays; the kernels walk cells in the exact same shell order as the
// CPU path so all backends consider identical candidate sets.
// ============================================================================

// Namespace scope (not anonymous): __global__ kernel signatures must not
// reference internal-linkage types under separable compilation.
constexpr int GRID_MAX_R = 16;   // matches kMaxShellRadius (CPU) / GRID_MAX_R (Metal)
constexpr int GRID_MAX_K = 32;   // matches kMaxK (CPU) / GRID_MAX_K (Metal)

struct GridSearchParams {
    float origin[3];
    float cell_size;
    int dims[3];
    int k;
    unsigned int num_points;
    unsigned int num_queries;
    float min_separation;
    float support_radius;
};

__device__ inline int clampCellCuda(float v, int dim) {
    int c = static_cast<int>(floorf(v));
    return c < 0 ? 0 : (c >= dim ? dim - 1 : c);
}

__global__ void knnStatsGridKernel(
    const float* positions,
    const unsigned int* cell_entries,
    const unsigned int* cell_starts,
    const unsigned int* cell_counts,
    GridSearchParams gp,
    float* out_stats) {   // 4 floats per point: mean_dist, gap xyz

    unsigned int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= gp.num_points) return;

    const float px = positions[id * 3 + 0];
    const float py = positions[id * 3 + 1];
    const float pz = positions[id * 3 + 2];
    const float inv = 1.0f / gp.cell_size;
    const int k = gp.k < 1 ? 1 : (gp.k > GRID_MAX_K ? GRID_MAX_K : gp.k);
    const int cx = clampCellCuda((px - gp.origin[0]) * inv, gp.dims[0]);
    const int cy = clampCellCuda((py - gp.origin[1]) * inv, gp.dims[1]);
    const int cz = clampCellCuda((pz - gp.origin[2]) * inv, gp.dims[2]);

    float best_d[GRID_MAX_K];
    unsigned int best_i[GRID_MAX_K];
    int filled = 0;

    for (int r = 0; r <= GRID_MAX_R; ++r) {
        for (int dz = -r; dz <= r; ++dz) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    int cheb = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                    cheb = cheb > abs(dz) ? cheb : abs(dz);
                    if (cheb != r) continue;  // shell only
                    const int x = cx + dx, y = cy + dy, z = cz + dz;
                    if (x < 0 || y < 0 || z < 0 ||
                        x >= gp.dims[0] || y >= gp.dims[1] || z >= gp.dims[2])
                        continue;
                    const unsigned int ci =
                        (static_cast<unsigned int>(z) * gp.dims[1] + y) * gp.dims[0] + x;
                    const unsigned int start = cell_starts[ci];
                    const unsigned int count = cell_counts[ci];
                    for (unsigned int t = start; t < start + count; ++t) {
                        const unsigned int j = cell_entries[t];
                        if (j == id) continue;
                        const float ddx = positions[j * 3 + 0] - px;
                        const float ddy = positions[j * 3 + 1] - py;
                        const float ddz = positions[j * 3 + 2] - pz;
                        const float d = sqrtf(ddx * ddx + ddy * ddy + ddz * ddz);
                        if (filled < k) {
                            int p = filled;
                            while (p > 0 && best_d[p - 1] > d) {
                                best_d[p] = best_d[p - 1];
                                best_i[p] = best_i[p - 1];
                                --p;
                            }
                            best_d[p] = d;
                            best_i[p] = j;
                            ++filled;
                        } else if (d < best_d[k - 1]) {
                            int p = k - 1;
                            while (p > 0 && best_d[p - 1] > d) {
                                best_d[p] = best_d[p - 1];
                                best_i[p] = best_i[p - 1];
                                --p;
                            }
                            best_d[p] = d;
                            best_i[p] = j;
                        }
                    }
                }
            }
        }
        // Unvisited points lie beyond r * cell_size — safe to stop.
        if (filled >= k && best_d[k - 1] <= static_cast<float>(r) * gp.cell_size)
            break;
    }

    if (filled == 0) {
        out_stats[id * 4 + 0] = 0.0f;
        out_stats[id * 4 + 1] = 0.0f;
        out_stats[id * 4 + 2] = 0.0f;
        out_stats[id * 4 + 3] = 0.0f;
        return;
    }
    const int kk = filled < k ? filled : k;
    float sum = 0.0f, cnx = 0.0f, cny = 0.0f, cnz = 0.0f;
    for (int t = 0; t < kk; ++t) {
        sum += best_d[t];
        const unsigned int j = best_i[t];
        cnx += positions[j * 3 + 0];
        cny += positions[j * 3 + 1];
        cnz += positions[j * 3 + 2];
    }
    const float inv_k = 1.0f / static_cast<float>(kk);
    out_stats[id * 4 + 0] = sum * inv_k;
    out_stats[id * 4 + 1] = px - cnx * inv_k;
    out_stats[id * 4 + 2] = py - cny * inv_k;
    out_stats[id * 4 + 3] = pz - cnz * inv_k;
}

__global__ void filterCandidatesGridKernel(
    const float* candidates,
    const float* directions,
    const float* positions,
    const unsigned int* cell_entries,
    const unsigned int* cell_starts,
    const unsigned int* cell_counts,
    GridSearchParams gp,
    float* out_filter) {   // 2 floats per candidate: min_dist, support 0/1

    unsigned int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= gp.num_queries) return;

    const float px = candidates[id * 3 + 0];
    const float py = candidates[id * 3 + 1];
    const float pz = candidates[id * 3 + 2];
    const float dxd = directions[id * 3 + 0];
    const float dyd = directions[id * 3 + 1];
    const float dzd = directions[id * 3 + 2];
    const bool need_support = (dxd != 0.0f || dyd != 0.0f || dzd != 0.0f);

    const float inv = 1.0f / gp.cell_size;
    const int r_min = static_cast<int>(ceilf(gp.min_separation * inv));
    const int r_sup = static_cast<int>(ceilf(gp.support_radius * inv));
    int r_max = r_min > r_sup ? r_min : r_sup;
    r_max = r_max > GRID_MAX_R ? GRID_MAX_R : r_max;

    const int cx = clampCellCuda((px - gp.origin[0]) * inv, gp.dims[0]);
    const int cy = clampCellCuda((py - gp.origin[1]) * inv, gp.dims[1]);
    const int cz = clampCellCuda((pz - gp.origin[2]) * inv, gp.dims[2]);

    float min_dist = 3.402823466e38f;  // FLT_MAX, matches the CPU sentinel
    bool support = !need_support;

    for (int r = 0; r <= r_max; ++r) {
        for (int dz = -r; dz <= r; ++dz) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    int cheb = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                    cheb = cheb > abs(dz) ? cheb : abs(dz);
                    if (cheb != r) continue;
                    const int x = cx + dx, y = cy + dy, z = cz + dz;
                    if (x < 0 || y < 0 || z < 0 ||
                        x >= gp.dims[0] || y >= gp.dims[1] || z >= gp.dims[2])
                        continue;
                    const unsigned int ci =
                        (static_cast<unsigned int>(z) * gp.dims[1] + y) * gp.dims[0] + x;
                    const unsigned int start = cell_starts[ci];
                    const unsigned int count = cell_counts[ci];
                    for (unsigned int t = start; t < start + count; ++t) {
                        const unsigned int j = cell_entries[t];
                        const float qx = positions[j * 3 + 0] - px;
                        const float qy = positions[j * 3 + 1] - py;
                        const float qz = positions[j * 3 + 2] - pz;
                        const float d = sqrtf(qx * qx + qy * qy + qz * qz);
                        min_dist = d < min_dist ? d : min_dist;
                        if (!support && d <= gp.support_radius &&
                            qx * dxd + qy * dyd + qz * dzd > 1e-6f) {
                            support = true;
                        }
                    }
                }
            }
        }
        const bool min_settled = min_dist < gp.min_separation || r >= r_min;
        const bool support_settled = support || r >= r_sup;
        if (min_settled && support_settled) break;
    }

    out_filter[id * 2 + 0] = min_dist;
    out_filter[id * 2 + 1] = support ? 1.0f : 0.0f;
}

// Host launchers for the grid kernels. All pointers are device pointers;
// the params struct is passed by value.

bool launchKnnStatsGrid(const float* d_positions,
                        const unsigned int* d_entries,
                        const unsigned int* d_starts,
                        const unsigned int* d_counts,
                        const float grid_origin[3], float cell_size,
                        const int grid_dims[3], int k_neighbors,
                        size_t num_points,
                        float* d_out_stats) {
    if (num_points == 0) return true;
    GridSearchParams gp{};
    gp.origin[0] = grid_origin[0];
    gp.origin[1] = grid_origin[1];
    gp.origin[2] = grid_origin[2];
    gp.cell_size = cell_size;
    gp.dims[0] = grid_dims[0];
    gp.dims[1] = grid_dims[1];
    gp.dims[2] = grid_dims[2];
    gp.k = k_neighbors;
    gp.num_points = static_cast<unsigned int>(num_points);

    const int threads_per_block = getOptimalThreadsPerBlock();
    const int num_blocks = static_cast<int>(
        (num_points + threads_per_block - 1) / threads_per_block);
    knnStatsGridKernel<<<num_blocks, threads_per_block>>>(
        d_positions, d_entries, d_starts, d_counts, gp, d_out_stats);
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        std::printf("knnStatsGridKernel launch failed: %s\n", cudaGetErrorString(err));
        return false;
    }
    return true;
}

bool launchFilterCandidatesGrid(const float* d_candidates,
                                const float* d_directions,
                                const float* d_positions,
                                const unsigned int* d_entries,
                                const unsigned int* d_starts,
                                const unsigned int* d_counts,
                                const float grid_origin[3], float cell_size,
                                const int grid_dims[3],
                                float min_separation, float support_radius,
                                size_t num_points, size_t num_queries,
                                float* d_out_filter) {
    if (num_queries == 0) return true;
    GridSearchParams gp{};
    gp.origin[0] = grid_origin[0];
    gp.origin[1] = grid_origin[1];
    gp.origin[2] = grid_origin[2];
    gp.cell_size = cell_size;
    gp.dims[0] = grid_dims[0];
    gp.dims[1] = grid_dims[1];
    gp.dims[2] = grid_dims[2];
    gp.num_points = static_cast<unsigned int>(num_points);
    gp.num_queries = static_cast<unsigned int>(num_queries);
    gp.min_separation = min_separation;
    gp.support_radius = support_radius;

    const int threads_per_block = getOptimalThreadsPerBlock();
    const int num_blocks = static_cast<int>(
        (num_queries + threads_per_block - 1) / threads_per_block);
    filterCandidatesGridKernel<<<num_blocks, threads_per_block>>>(
        d_candidates, d_directions, d_positions, d_entries, d_starts, d_counts,
        gp, d_out_filter);
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        std::printf("filterCandidatesGridKernel launch failed: %s\n", cudaGetErrorString(err));
        return false;
    }
    return true;
}

} // namespace cuda
} // namespace melkor
