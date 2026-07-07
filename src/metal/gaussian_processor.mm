#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "melkor/metal_compute.hpp"
#include <vector>
#include <algorithm>

namespace melkor {
namespace metal {

class GaussianProcessor::Impl {
public:
    MetalContext& context;
    
    // Pipeline states for each kernel
    id<MTLComputePipelineState> transformPipeline = nil;
    id<MTLComputePipelineState> normalizeQuatPipeline = nil;
    id<MTLComputePipelineState> scalePosPipeline = nil;
    id<MTLComputePipelineState> rgbToShPipeline = nil;
    id<MTLComputePipelineState> opacityToLogitPipeline = nil;
    id<MTLComputePipelineState> computeCovPipeline = nil;
    id<MTLComputePipelineState> processAllPipeline = nil;
    id<MTLComputePipelineState> enhancedConvertPipeline = nil;
    id<MTLComputePipelineState> knnDistancesPipeline = nil;
    id<MTLComputePipelineState> knnStatsGridPipeline = nil;
    id<MTLComputePipelineState> filterCandidatesPipeline = nil;
    
    Impl(MetalContext& ctx) : context(ctx) {
        createPipelines();
    }
    
    void createPipelines() {
        id<MTLLibrary> library = (__bridge id<MTLLibrary>)context.getLibrary();
        if (!library) return;
        
        id<MTLDevice> device = (__bridge id<MTLDevice>)context.getDevice();
        NSError* error = nil;
        
        auto createPipeline = [&](NSString* name) -> id<MTLComputePipelineState> {
            id<MTLFunction> function = [library newFunctionWithName:name];
            if (!function) {
                NSLog(@"Failed to find function: %@", name);
                return nil;
            }
            id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
            if (error) {
                NSLog(@"Failed to create pipeline for %@: %@", name, error);
            }
            return pipeline;
        };
        
        transformPipeline = createPipeline(@"transform_coordinates");
        normalizeQuatPipeline = createPipeline(@"normalize_quaternions");
        scalePosPipeline = createPipeline(@"scale_positions");
        rgbToShPipeline = createPipeline(@"rgb_to_sh_dc");
        processAllPipeline = createPipeline(@"process_all");
        enhancedConvertPipeline = createPipeline(@"enhanced_convert_points");
        knnDistancesPipeline = createPipeline(@"compute_knn_distances");
        knnStatsGridPipeline = createPipeline(@"knn_stats_grid");
        filterCandidatesPipeline = createPipeline(@"filter_candidates_grid");
        computeCovPipeline = createPipeline(@"compute_covariances");
    }
    
    bool runKernel(id<MTLComputePipelineState> pipeline,
                   id<MTLBuffer> dataBuffer,
                   size_t count,
                   id<MTLBuffer> paramsBuffer = nil) {
        if (!pipeline) return false;
        
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)context.getCommandQueue();
        
        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:dataBuffer offset:0 atIndex:0];
        
        if (paramsBuffer) {
            [encoder setBuffer:paramsBuffer offset:0 atIndex:1];
        }
        
        // Calculate threadgroup size
        NSUInteger maxThreadsPerGroup = [pipeline maxTotalThreadsPerThreadgroup];
        NSUInteger threadsPerGroup = std::min<NSUInteger>(maxThreadsPerGroup, 256);
        
        MTLSize gridSize = MTLSizeMake(count, 1, 1);
        MTLSize threadgroupSize = MTLSizeMake(threadsPerGroup, 1, 1);
        
        [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
        [encoder endEncoding];
        
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        return [commandBuffer status] == MTLCommandBufferStatusCompleted;
    }
    
    id<MTLBuffer> createBuffer(const std::vector<PackedGaussian>& packed) {
        id<MTLDevice> device = (__bridge id<MTLDevice>)context.getDevice();
        return [device newBufferWithBytes:packed.data()
                                   length:packed.size() * sizeof(PackedGaussian)
                                  options:MTLResourceStorageModeShared];
    }
    
    void readBackBuffer(id<MTLBuffer> buffer, std::vector<PackedGaussian>& packed) {
        memcpy(packed.data(), [buffer contents], packed.size() * sizeof(PackedGaussian));
    }
    
    void updateCloudFromPacked(GaussianCloud& cloud, const std::vector<PackedGaussian>& packed) {
        auto& splats = cloud.splats();
        for (size_t i = 0; i < splats.size(); ++i) {
            const auto& p = packed[i];
            splats[i].x = p.position[0];
            splats[i].y = p.position[1];
            splats[i].z = p.position[2];
            splats[i].opacity = p.position[3];
            splats[i].f_dc_0 = p.color[0];
            splats[i].f_dc_1 = p.color[1];
            splats[i].f_dc_2 = p.color[2];
            splats[i].scale_0 = p.scale[0];
            splats[i].scale_1 = p.scale[1];
            splats[i].scale_2 = p.scale[2];
            splats[i].rot_0 = p.rotation[0];
            splats[i].rot_1 = p.rotation[1];
            splats[i].rot_2 = p.rotation[2];
            splats[i].rot_3 = p.rotation[3];
        }
    }
};

GaussianProcessor::GaussianProcessor(MetalContext& context) 
    : impl_(std::make_unique<Impl>(context)) {}

GaussianProcessor::~GaussianProcessor() = default;

bool GaussianProcessor::transformCoordinates(GaussianCloud& cloud,
                                             const float transform_matrix[16]) {
    if (cloud.empty()) return true;
    
    auto packed = cloud.toPackedFormat();
    id<MTLBuffer> dataBuffer = impl_->createBuffer(packed);
    
    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->context.getDevice();
    
    // Create params buffer with transform matrix
    struct TransformParams {
        float matrix[16];
    } params;
    memcpy(params.matrix, transform_matrix, sizeof(float) * 16);
    
    id<MTLBuffer> paramsBuffer = [device newBufferWithBytes:&params
                                                     length:sizeof(params)
                                                    options:MTLResourceStorageModeShared];
    
    bool success = impl_->runKernel(impl_->transformPipeline, dataBuffer, cloud.size(), paramsBuffer);
    
    if (success) {
        impl_->readBackBuffer(dataBuffer, packed);
        impl_->updateCloudFromPacked(cloud, packed);
    }
    
    return success;
}

bool GaussianProcessor::normalizeQuaternions(GaussianCloud& cloud) {
    if (cloud.empty()) return true;
    
    auto packed = cloud.toPackedFormat();
    id<MTLBuffer> dataBuffer = impl_->createBuffer(packed);
    
    bool success = impl_->runKernel(impl_->normalizeQuatPipeline, dataBuffer, cloud.size());
    
    if (success) {
        impl_->readBackBuffer(dataBuffer, packed);
        impl_->updateCloudFromPacked(cloud, packed);
    }
    
    return success;
}

bool GaussianProcessor::scalePositions(GaussianCloud& cloud, float scale) {
    if (cloud.empty()) return true;
    
    auto packed = cloud.toPackedFormat();
    id<MTLBuffer> dataBuffer = impl_->createBuffer(packed);
    
    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->context.getDevice();
    
    struct ScaleParams {
        float scale;
    } params = { scale };
    
    id<MTLBuffer> paramsBuffer = [device newBufferWithBytes:&params
                                                     length:sizeof(params)
                                                    options:MTLResourceStorageModeShared];
    
    bool success = impl_->runKernel(impl_->scalePosPipeline, dataBuffer, cloud.size(), paramsBuffer);
    
    if (success) {
        impl_->readBackBuffer(dataBuffer, packed);
        impl_->updateCloudFromPacked(cloud, packed);
    }
    
    return success;
}

bool GaussianProcessor::rgbToShDc(GaussianCloud& cloud) {
    if (cloud.empty()) return true;
    
    auto packed = cloud.toPackedFormat();
    id<MTLBuffer> dataBuffer = impl_->createBuffer(packed);
    
    bool success = impl_->runKernel(impl_->rgbToShPipeline, dataBuffer, cloud.size());
    
    if (success) {
        impl_->readBackBuffer(dataBuffer, packed);
        impl_->updateCloudFromPacked(cloud, packed);
    }
    
    return success;
}

bool GaussianProcessor::opacityToLogit(GaussianCloud& cloud) {
    if (cloud.empty()) return true;
    
    auto packed = cloud.toPackedFormat();
    id<MTLBuffer> dataBuffer = impl_->createBuffer(packed);
    
    bool success = impl_->runKernel(impl_->opacityToLogitPipeline, dataBuffer, cloud.size());
    
    if (success) {
        impl_->readBackBuffer(dataBuffer, packed);
        impl_->updateCloudFromPacked(cloud, packed);
    }
    
    return success;
}

bool GaussianProcessor::sortByDistance(GaussianCloud& cloud,
                                       float camera_x, float camera_y, float camera_z) {
    // For sorting, we'd need a parallel sort algorithm
    // For now, do CPU sort as Metal doesn't have built-in parallel sort
    if (cloud.empty()) return true;
    
    auto& splats = cloud.splats();
    
    // Create index-distance pairs
    std::vector<std::pair<float, size_t>> distances(splats.size());
    for (size_t i = 0; i < splats.size(); ++i) {
        float dx = splats[i].x - camera_x;
        float dy = splats[i].y - camera_y;
        float dz = splats[i].z - camera_z;
        distances[i] = {dx*dx + dy*dy + dz*dz, i};
    }
    
    // Sort by distance (back to front for alpha blending)
    std::sort(distances.begin(), distances.end(), 
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // Reorder splats
    std::vector<GaussianSplat> sorted;
    sorted.reserve(splats.size());
    for (const auto& [dist, idx] : distances) {
        sorted.push_back(std::move(splats[idx]));
    }
    splats = std::move(sorted);
    
    return true;
}

std::vector<float> GaussianProcessor::computeCovariances(const GaussianCloud& cloud) {
    if (cloud.empty()) return {};
    if (!impl_->computeCovPipeline) return {};

    auto packed = cloud.toPackedFormat();
    id<MTLBuffer> dataBuffer = impl_->createBuffer(packed);

    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->context.getDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)impl_->context.getCommandQueue();

    // Output buffer: 6 floats per splat
    size_t outputSize = cloud.size() * 6 * sizeof(float);
    id<MTLBuffer> outputBuffer = [device newBufferWithLength:outputSize
                                                     options:MTLResourceStorageModeShared];
    if (!dataBuffer || !outputBuffer) return {};

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    
    [encoder setComputePipelineState:impl_->computeCovPipeline];
    [encoder setBuffer:dataBuffer offset:0 atIndex:0];
    [encoder setBuffer:outputBuffer offset:0 atIndex:1];
    
    NSUInteger threadsPerGroup = std::min<NSUInteger>([impl_->computeCovPipeline maxTotalThreadsPerThreadgroup], 256);
    MTLSize gridSize = MTLSizeMake(cloud.size(), 1, 1);
    MTLSize threadgroupSize = MTLSizeMake(threadsPerGroup, 1, 1);
    
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
    [encoder endEncoding];
    
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    // Empty on GPU failure so callers can distinguish errors from data.
    if ([commandBuffer status] != MTLCommandBufferStatusCompleted) return {};

    // Read back results
    std::vector<float> result(cloud.size() * 6);
    memcpy(result.data(), [outputBuffer contents], outputSize);

    return result;
}

bool GaussianProcessor::processCloud(GaussianCloud& cloud, const ProcessConfig& config) {
    if (cloud.empty()) return true;
    
    auto packed = cloud.toPackedFormat();
    id<MTLBuffer> dataBuffer = impl_->createBuffer(packed);
    
    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->context.getDevice();
    
    // Build flags
    uint32_t flags = 0;
    if (config.normalize_quaternions) flags |= 0x1;
    if (config.convert_colors_to_sh) flags |= 0x2;
    if (config.convert_opacity_to_logit) flags |= 0x4;
    // Note: scale is already in log space from GLB loader
    
    id<MTLBuffer> flagsBuffer = [device newBufferWithBytes:&flags
                                                    length:sizeof(flags)
                                                   options:MTLResourceStorageModeShared];
    
    bool success = impl_->runKernel(impl_->processAllPipeline, dataBuffer, cloud.size(), flagsBuffer);
    
    // Apply position scale if needed (separate kernel)
    if (success && config.position_scale != 1.0f) {
        struct ScaleParams { float scale; } params = { config.position_scale };
        id<MTLBuffer> scaleParamsBuffer = [device newBufferWithBytes:&params
                                                              length:sizeof(params)
                                                             options:MTLResourceStorageModeShared];
        success = impl_->runKernel(impl_->scalePosPipeline, dataBuffer, cloud.size(), scaleParamsBuffer);
    }
    
    // Apply Y-up to Z-up transform if needed
    if (success && config.transform_y_up_to_z_up) {
        // Transform matrix for Y-up to Z-up:
        // [1  0  0  0]
        // [0  0 -1  0]
        // [0  1  0  0]
        // [0  0  0  1]
        float matrix[16] = {
            1, 0, 0,  0,
            0, 0, 1,  0,
            0, -1, 0, 0,
            0, 0, 0,  1
        };
        
        struct TransformParams { float matrix[16]; } params;
        memcpy(params.matrix, matrix, sizeof(matrix));
        
        id<MTLBuffer> transformBuffer = [device newBufferWithBytes:&params
                                                            length:sizeof(params)
                                                           options:MTLResourceStorageModeShared];
        success = impl_->runKernel(impl_->transformPipeline, dataBuffer, cloud.size(), transformBuffer);
    }
    
    if (success) {
        impl_->readBackBuffer(dataBuffer, packed);
        impl_->updateCloudFromPacked(cloud, packed);
    }
    
    return success;
}
// ============================================================================
// Metal-accelerated enhanced conversion and k-NN
// ============================================================================

std::vector<PackedGaussian> GaussianProcessor::enhancedConvert(
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<float>& colors,
    const std::vector<float>& adaptive_scales,
    const EnhancedConvertConfig& config) {

    size_t num_points = positions.size() / 3;
    if (num_points == 0) return {};
    if (!impl_->enhancedConvertPipeline) return {};
    if (adaptive_scales.size() < num_points) return {};

    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->context.getDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)impl_->context.getCommandQueue();

    // Optional attribute arrays are used only when they cover every point;
    // a short array (mixed-attribute multi-primitive input) would otherwise
    // read out of bounds on the GPU.
    const bool has_normals = normals.size() >= positions.size();
    const bool has_colors = colors.size() >= positions.size();

    // Create buffers. Metal buffer bindings must never be nil, so absent
    // attributes bind a small dummy buffer and the kernel is told via
    // has_normals/has_colors not to read them.
    const float dummy[3] = {0.0f, 0.0f, 0.0f};
    size_t pos_bytes = positions.size() * sizeof(float);
    id<MTLBuffer> posBuffer = [device newBufferWithBytes:positions.data()
                                                  length:pos_bytes
                                                 options:MTLResourceStorageModeShared];

    id<MTLBuffer> normBuffer = [device newBufferWithBytes:has_normals ? normals.data() : dummy
                                                   length:has_normals ? normals.size() * sizeof(float) : sizeof(dummy)
                                                  options:MTLResourceStorageModeShared];

    id<MTLBuffer> colorBuffer = [device newBufferWithBytes:has_colors ? colors.data() : dummy
                                                    length:has_colors ? colors.size() * sizeof(float) : sizeof(dummy)
                                                   options:MTLResourceStorageModeShared];

    id<MTLBuffer> scaleBuffer = [device newBufferWithBytes:adaptive_scales.data()
                                                    length:adaptive_scales.size() * sizeof(float)
                                                   options:MTLResourceStorageModeShared];

    id<MTLBuffer> outBuffer = [device newBufferWithLength:num_points * sizeof(PackedGaussian)
                                                  options:MTLResourceStorageModeShared];
    if (!posBuffer || !normBuffer || !colorBuffer || !scaleBuffer || !outBuffer) {
        return {};
    }

    // Config struct matching the Metal shader
    struct MTLEnhancedConfig {
        float scale_factor;
        float min_scale;
        float max_scale;
        float normal_scale_ratio;
        float default_opacity;
        float position_scale;
        int   convert_coordinate_system;
        int   use_surface_alignment;
        float default_r;
        float default_g;
        float default_b;
        int   has_normals;
        int   has_colors;
    } cfg = {
        config.scale_factor,
        config.min_scale,
        config.max_scale,
        config.normal_scale_ratio,
        std::clamp(config.default_opacity, 0.001f, 0.999f),
        config.position_scale,
        config.convert_coordinate_system ? 1 : 0,
        config.use_surface_alignment ? 1 : 0,
        config.default_color[0],
        config.default_color[1],
        config.default_color[2],
        has_normals ? 1 : 0,
        has_colors ? 1 : 0
    };

    id<MTLBuffer> cfgBuffer = [device newBufferWithBytes:&cfg
                                                  length:sizeof(cfg)
                                                 options:MTLResourceStorageModeShared];

    uint32_t np = static_cast<uint32_t>(num_points);
    id<MTLBuffer> npBuffer = [device newBufferWithBytes:&np
                                                 length:sizeof(np)
                                                options:MTLResourceStorageModeShared];

    id<MTLCommandBuffer> cmdBuffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuffer computeCommandEncoder];

    [encoder setComputePipelineState:impl_->enhancedConvertPipeline];
    [encoder setBuffer:posBuffer offset:0 atIndex:0];
    [encoder setBuffer:normBuffer offset:0 atIndex:1];
    [encoder setBuffer:colorBuffer offset:0 atIndex:2];
    [encoder setBuffer:scaleBuffer offset:0 atIndex:3];
    [encoder setBuffer:outBuffer offset:0 atIndex:4];
    [encoder setBuffer:cfgBuffer offset:0 atIndex:5];
    [encoder setBuffer:npBuffer offset:0 atIndex:6];

    NSUInteger threadsPerGroup = std::min<NSUInteger>([impl_->enhancedConvertPipeline maxTotalThreadsPerThreadgroup], 256);
    MTLSize gridSize = MTLSizeMake(num_points, 1, 1);
    MTLSize threadgroupSize = MTLSizeMake(threadsPerGroup, 1, 1);
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
    [encoder endEncoding];

    [cmdBuffer commit];
    [cmdBuffer waitUntilCompleted];

    // Contract: empty on failure, so callers fall back to the CPU path
    // instead of receiving num_points zero-filled (degenerate) gaussians.
    if ([cmdBuffer status] != MTLCommandBufferStatusCompleted) {
        return {};
    }

    std::vector<PackedGaussian> output(num_points);
    memcpy(output.data(), [outBuffer contents], num_points * sizeof(PackedGaussian));
    return output;
}

std::vector<float> GaussianProcessor::computeKnnDistancesMetal(
    const std::vector<float>& positions,
    int k_neighbors) {

    size_t num_points = positions.size() / 3;
    std::vector<float> distances(num_points, 0.0f);
    if (num_points == 0) return distances;

    if (!impl_->knnDistancesPipeline) return distances;

    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->context.getDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)impl_->context.getCommandQueue();

    id<MTLBuffer> posBuffer = [device newBufferWithBytes:positions.data()
                                                  length:positions.size() * sizeof(float)
                                                 options:MTLResourceStorageModeShared];

    id<MTLBuffer> outBuffer = [device newBufferWithLength:num_points * sizeof(float)
                                                  options:MTLResourceStorageModeShared];

    uint32_t np = static_cast<uint32_t>(num_points);
    int32_t k = static_cast<int32_t>(std::min(k_neighbors, 32));

    id<MTLBuffer> npBuffer = [device newBufferWithBytes:&np
                                                 length:sizeof(np)
                                                options:MTLResourceStorageModeShared];
    id<MTLBuffer> kBuffer = [device newBufferWithBytes:&k
                                                length:sizeof(k)
                                               options:MTLResourceStorageModeShared];

    id<MTLCommandBuffer> cmdBuffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuffer computeCommandEncoder];

    [encoder setComputePipelineState:impl_->knnDistancesPipeline];
    [encoder setBuffer:posBuffer offset:0 atIndex:0];
    [encoder setBuffer:outBuffer offset:0 atIndex:1];
    [encoder setBuffer:npBuffer offset:0 atIndex:2];
    [encoder setBuffer:kBuffer offset:0 atIndex:3];

    NSUInteger threadsPerGroup = std::min<NSUInteger>([impl_->knnDistancesPipeline maxTotalThreadsPerThreadgroup], 64);
    MTLSize gridSize = MTLSizeMake(num_points, 1, 1);
    MTLSize threadgroupSize = MTLSizeMake(threadsPerGroup, 1, 1);
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
    [encoder endEncoding];

    [cmdBuffer commit];
    [cmdBuffer waitUntilCompleted];

    if ([cmdBuffer status] == MTLCommandBufferStatusCompleted) {
        memcpy(distances.data(), [outBuffer contents], num_points * sizeof(float));
    }

    return distances;
}

// Host-side mirror of the GridSearchParams struct in gaussian_compute.metal.
// float4/int4 in MSL are 16-byte aligned; the trailing scalars pack into one
// more 16-byte slot, giving 48 bytes on both sides.
namespace {
struct alignas(16) GridSearchParams {
    float origin[4];      // xyz origin, w = cell size
    int32_t dims[4];      // xyz dims, w = k neighbors
    uint32_t num_points;
    uint32_t num_queries;
    float min_separation;
    float support_radius;
};
static_assert(sizeof(GridSearchParams) == 48, "must match MSL layout");
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
        cell_size <= 0.0f) {
        return {};
    }
    // The kernel indexes cell arrays by (z*dy + y)*dx + x; mismatched dims
    // would read past the buffers on the GPU.
    if (grid_dims[0] <= 0 || grid_dims[1] <= 0 || grid_dims[2] <= 0 ||
        static_cast<size_t>(grid_dims[0]) * grid_dims[1] * grid_dims[2] !=
            cell_starts.size()) {
        return {};
    }
    if (!impl_->knnStatsGridPipeline) return {};

    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->context.getDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)impl_->context.getCommandQueue();

    GridSearchParams gp{};
    gp.origin[0] = grid_origin[0];
    gp.origin[1] = grid_origin[1];
    gp.origin[2] = grid_origin[2];
    gp.origin[3] = cell_size;
    gp.dims[0] = grid_dims[0];
    gp.dims[1] = grid_dims[1];
    gp.dims[2] = grid_dims[2];
    gp.dims[3] = k_neighbors;
    gp.num_points = static_cast<uint32_t>(num_points);

    id<MTLBuffer> posBuffer = [device newBufferWithBytes:positions.data()
                                                  length:positions.size() * sizeof(float)
                                                 options:MTLResourceStorageModeShared];
    id<MTLBuffer> entriesBuffer = [device newBufferWithBytes:cell_entries.data()
                                                      length:cell_entries.size() * sizeof(uint32_t)
                                                     options:MTLResourceStorageModeShared];
    id<MTLBuffer> startsBuffer = [device newBufferWithBytes:cell_starts.data()
                                                     length:cell_starts.size() * sizeof(uint32_t)
                                                    options:MTLResourceStorageModeShared];
    id<MTLBuffer> countsBuffer = [device newBufferWithBytes:cell_counts.data()
                                                     length:cell_counts.size() * sizeof(uint32_t)
                                                    options:MTLResourceStorageModeShared];
    id<MTLBuffer> gpBuffer = [device newBufferWithBytes:&gp
                                                 length:sizeof(gp)
                                                options:MTLResourceStorageModeShared];
    // out_stats is float4-aligned: 4 floats per point.
    id<MTLBuffer> outBuffer = [device newBufferWithLength:num_points * 4 * sizeof(float)
                                                  options:MTLResourceStorageModeShared];
    if (!posBuffer || !entriesBuffer || !startsBuffer || !countsBuffer ||
        !gpBuffer || !outBuffer) {
        return {};
    }

    id<MTLCommandBuffer> cmdBuffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuffer computeCommandEncoder];

    [encoder setComputePipelineState:impl_->knnStatsGridPipeline];
    [encoder setBuffer:posBuffer offset:0 atIndex:0];
    [encoder setBuffer:entriesBuffer offset:0 atIndex:1];
    [encoder setBuffer:startsBuffer offset:0 atIndex:2];
    [encoder setBuffer:countsBuffer offset:0 atIndex:3];
    [encoder setBuffer:gpBuffer offset:0 atIndex:4];
    [encoder setBuffer:outBuffer offset:0 atIndex:5];

    NSUInteger threadsPerGroup = std::min<NSUInteger>([impl_->knnStatsGridPipeline maxTotalThreadsPerThreadgroup], 128);
    [encoder dispatchThreads:MTLSizeMake(num_points, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(threadsPerGroup, 1, 1)];
    [encoder endEncoding];

    [cmdBuffer commit];
    [cmdBuffer waitUntilCompleted];

    if ([cmdBuffer status] != MTLCommandBufferStatusCompleted) return {};

    std::vector<float> stats(num_points * 4);
    memcpy(stats.data(), [outBuffer contents], stats.size() * sizeof(float));
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
        cell_size <= 0.0f) {
        return {};
    }
    if (grid_dims[0] <= 0 || grid_dims[1] <= 0 || grid_dims[2] <= 0 ||
        static_cast<size_t>(grid_dims[0]) * grid_dims[1] * grid_dims[2] !=
            cell_starts.size()) {
        return {};
    }
    if (!impl_->filterCandidatesPipeline) return {};

    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->context.getDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)impl_->context.getCommandQueue();

    GridSearchParams gp{};
    gp.origin[0] = grid_origin[0];
    gp.origin[1] = grid_origin[1];
    gp.origin[2] = grid_origin[2];
    gp.origin[3] = cell_size;
    gp.dims[0] = grid_dims[0];
    gp.dims[1] = grid_dims[1];
    gp.dims[2] = grid_dims[2];
    gp.num_points = static_cast<uint32_t>(num_points);
    gp.num_queries = static_cast<uint32_t>(num_queries);
    gp.min_separation = min_separation;
    gp.support_radius = support_radius;

    id<MTLBuffer> candBuffer = [device newBufferWithBytes:candidates.data()
                                                   length:candidates.size() * sizeof(float)
                                                  options:MTLResourceStorageModeShared];
    id<MTLBuffer> dirBuffer = [device newBufferWithBytes:directions.data()
                                                  length:directions.size() * sizeof(float)
                                                 options:MTLResourceStorageModeShared];
    id<MTLBuffer> posBuffer = [device newBufferWithBytes:positions.data()
                                                  length:positions.size() * sizeof(float)
                                                 options:MTLResourceStorageModeShared];
    id<MTLBuffer> entriesBuffer = [device newBufferWithBytes:cell_entries.data()
                                                      length:cell_entries.size() * sizeof(uint32_t)
                                                     options:MTLResourceStorageModeShared];
    id<MTLBuffer> startsBuffer = [device newBufferWithBytes:cell_starts.data()
                                                     length:cell_starts.size() * sizeof(uint32_t)
                                                    options:MTLResourceStorageModeShared];
    id<MTLBuffer> countsBuffer = [device newBufferWithBytes:cell_counts.data()
                                                     length:cell_counts.size() * sizeof(uint32_t)
                                                    options:MTLResourceStorageModeShared];
    id<MTLBuffer> gpBuffer = [device newBufferWithBytes:&gp
                                                 length:sizeof(gp)
                                                options:MTLResourceStorageModeShared];
    // out_filter is float2 per candidate.
    id<MTLBuffer> outBuffer = [device newBufferWithLength:num_queries * 2 * sizeof(float)
                                                  options:MTLResourceStorageModeShared];
    if (!candBuffer || !dirBuffer || !posBuffer || !entriesBuffer ||
        !startsBuffer || !countsBuffer || !gpBuffer || !outBuffer) {
        return {};
    }

    id<MTLCommandBuffer> cmdBuffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuffer computeCommandEncoder];

    [encoder setComputePipelineState:impl_->filterCandidatesPipeline];
    [encoder setBuffer:candBuffer offset:0 atIndex:0];
    [encoder setBuffer:dirBuffer offset:0 atIndex:1];
    [encoder setBuffer:posBuffer offset:0 atIndex:2];
    [encoder setBuffer:entriesBuffer offset:0 atIndex:3];
    [encoder setBuffer:startsBuffer offset:0 atIndex:4];
    [encoder setBuffer:countsBuffer offset:0 atIndex:5];
    [encoder setBuffer:gpBuffer offset:0 atIndex:6];
    [encoder setBuffer:outBuffer offset:0 atIndex:7];

    NSUInteger threadsPerGroup = std::min<NSUInteger>([impl_->filterCandidatesPipeline maxTotalThreadsPerThreadgroup], 128);
    [encoder dispatchThreads:MTLSizeMake(num_queries, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(threadsPerGroup, 1, 1)];
    [encoder endEncoding];

    [cmdBuffer commit];
    [cmdBuffer waitUntilCompleted];

    if ([cmdBuffer status] != MTLCommandBufferStatusCompleted) return {};

    std::vector<float> result(num_queries * 2);
    memcpy(result.data(), [outBuffer contents], result.size() * sizeof(float));
    return result;
}

} // namespace metal
} // namespace melkor
