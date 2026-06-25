#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "melkor/metal_compute.hpp"
#include <vector>

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
        computeCovPipeline = createPipeline(@"compute_covariances");
    }
    
    bool runKernel(id<MTLComputePipelineState> pipeline,
                   id<MTLBuffer> dataBuffer,
                   size_t count,
                   id<MTLBuffer> paramsBuffer = nil) {
        if (!pipeline) return false;
        
        id<MTLDevice> device = (__bridge id<MTLDevice>)context.getDevice();
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)context.getCommandQueue();
        
        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:dataBuffer offset:0 atIndex:0];
        
        if (paramsBuffer) {
            [encoder setBuffer:paramsBuffer offset:0 atIndex:1];
        }
        
        // Calculate threadgroup size
        NSUInteger threadExecutionWidth = [pipeline threadExecutionWidth];
        NSUInteger maxThreadsPerGroup = [pipeline maxTotalThreadsPerThreadgroup];
        NSUInteger threadsPerGroup = MIN(maxThreadsPerGroup, 256);
        
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
    
    auto packed = cloud.toPackedFormat();
    id<MTLBuffer> dataBuffer = impl_->createBuffer(packed);
    
    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->context.getDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)impl_->context.getCommandQueue();
    
    // Output buffer: 6 floats per splat
    size_t outputSize = cloud.size() * 6 * sizeof(float);
    id<MTLBuffer> outputBuffer = [device newBufferWithLength:outputSize
                                                     options:MTLResourceStorageModeShared];
    
    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    
    [encoder setComputePipelineState:impl_->computeCovPipeline];
    [encoder setBuffer:dataBuffer offset:0 atIndex:0];
    [encoder setBuffer:outputBuffer offset:0 atIndex:1];
    
    NSUInteger threadsPerGroup = MIN([impl_->computeCovPipeline maxTotalThreadsPerThreadgroup], 256);
    MTLSize gridSize = MTLSizeMake(cloud.size(), 1, 1);
    MTLSize threadgroupSize = MTLSizeMake(threadsPerGroup, 1, 1);
    
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
    [encoder endEncoding];
    
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    
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
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)impl_->context.getCommandQueue();
    
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
    std::vector<PackedGaussian> output(num_points);
    if (num_points == 0) return output;

    if (!impl_->enhancedConvertPipeline) return {};

    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->context.getDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)impl_->context.getCommandQueue();

    // Create buffers
    size_t pos_bytes = positions.size() * sizeof(float);
    id<MTLBuffer> posBuffer = [device newBufferWithBytes:positions.data()
                                                  length:pos_bytes
                                                 options:MTLResourceStorageModeShared];

    id<MTLBuffer> normBuffer = nil;
    if (!normals.empty()) {
        normBuffer = [device newBufferWithBytes:normals.data()
                                         length:normals.size() * sizeof(float)
                                        options:MTLResourceStorageModeShared];
    }

    id<MTLBuffer> colorBuffer = nil;
    if (!colors.empty()) {
        colorBuffer = [device newBufferWithBytes:colors.data()
                                           length:colors.size() * sizeof(float)
                                          options:MTLResourceStorageModeShared];
    }

    id<MTLBuffer> scaleBuffer = [device newBufferWithBytes:adaptive_scales.data()
                                                    length:adaptive_scales.size() * sizeof(float)
                                                   options:MTLResourceStorageModeShared];

    id<MTLBuffer> outBuffer = [device newBufferWithLength:num_points * sizeof(PackedGaussian)
                                                  options:MTLResourceStorageModeShared];

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
    } cfg = {
        config.scale_factor,
        config.min_scale,
        config.max_scale,
        config.normal_scale_ratio,
        std::clamp(config.default_opacity, 0.001f, 0.999f),
        config.position_scale,
        config.convert_coordinate_system ? 1 : 0,
        config.use_surface_alignment ? 1 : 0
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

    NSUInteger threadsPerGroup = MIN([impl_->enhancedConvertPipeline maxTotalThreadsPerThreadgroup], 256);
    MTLSize gridSize = MTLSizeMake(num_points, 1, 1);
    MTLSize threadgroupSize = MTLSizeMake(threadsPerGroup, 1, 1);
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
    [encoder endEncoding];

    [cmdBuffer commit];
    [cmdBuffer waitUntilCompleted];

    if ([cmdBuffer status] == MTLCommandBufferStatusCompleted) {
        memcpy(output.data(), [outBuffer contents], num_points * sizeof(PackedGaussian));
    }

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

    NSUInteger threadsPerGroup = MIN([impl_->knnDistancesPipeline maxTotalThreadsPerThreadgroup], 64);
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

} // namespace metal
} // namespace melkor
