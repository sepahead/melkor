#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "melkor/gaussian_fitter.hpp"
#include "melkor/enhanced_converter.hpp"
#include <cmath>
#include <chrono>
#include <iostream>

// For GLB loading
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include "tiny_gltf.h"

namespace melkor {

// ============================================================================
// Camera Implementation
// ============================================================================

void Camera::computeMatrices() {
    // Compute view matrix (look-at)
    float fx = target[0] - position[0];
    float fy = target[1] - position[1];
    float fz = target[2] - position[2];
    float f_len = std::sqrt(fx*fx + fy*fy + fz*fz);
    fx /= f_len; fy /= f_len; fz /= f_len;
    
    // right = cross(forward, up)
    float rx = fy * up[2] - fz * up[1];
    float ry = fz * up[0] - fx * up[2];
    float rz = fx * up[1] - fy * up[0];
    float r_len = std::sqrt(rx*rx + ry*ry + rz*rz);
    rx /= r_len; ry /= r_len; rz /= r_len;
    
    // true_up = cross(right, forward)
    float ux = ry * fz - rz * fy;
    float uy = rz * fx - rx * fz;
    float uz = rx * fy - ry * fx;
    
    // View matrix (column-major)
    view_matrix[0] = rx;  view_matrix[4] = ry;  view_matrix[8]  = rz;  view_matrix[12] = -(rx*position[0] + ry*position[1] + rz*position[2]);
    view_matrix[1] = ux;  view_matrix[5] = uy;  view_matrix[9]  = uz;  view_matrix[13] = -(ux*position[0] + uy*position[1] + uz*position[2]);
    view_matrix[2] = -fx; view_matrix[6] = -fy; view_matrix[10] = -fz; view_matrix[14] = (fx*position[0] + fy*position[1] + fz*position[2]);
    view_matrix[3] = 0;   view_matrix[7] = 0;   view_matrix[11] = 0;   view_matrix[15] = 1;
    
    // Projection matrix (OpenGL-style perspective)
    float tan_half_fov = std::tan(fov_y * 0.5f);
    float top = near_plane * tan_half_fov;
    float right_plane = top * aspect;
    
    proj_matrix[0] = near_plane / right_plane;
    proj_matrix[1] = 0;
    proj_matrix[2] = 0;
    proj_matrix[3] = 0;
    
    proj_matrix[4] = 0;
    proj_matrix[5] = near_plane / top;
    proj_matrix[6] = 0;
    proj_matrix[7] = 0;
    
    proj_matrix[8] = 0;
    proj_matrix[9] = 0;
    proj_matrix[10] = -(far_plane + near_plane) / (far_plane - near_plane);
    proj_matrix[11] = -1;
    
    proj_matrix[12] = 0;
    proj_matrix[13] = 0;
    proj_matrix[14] = -2.0f * far_plane * near_plane / (far_plane - near_plane);
    proj_matrix[15] = 0;
    
    // View-projection matrix
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            view_proj_matrix[i * 4 + j] = 0;
            for (int k = 0; k < 4; ++k) {
                view_proj_matrix[i * 4 + j] += proj_matrix[i * 4 + k] * view_matrix[k * 4 + j];
            }
        }
    }
}

Camera Camera::createOrbital(float distance, float azimuth, float elevation,
                              int width, int height, float fov_y) {
    Camera cam;
    
    // Spherical to Cartesian
    float cos_elev = std::cos(elevation);
    cam.position[0] = distance * cos_elev * std::cos(azimuth);
    cam.position[1] = distance * cos_elev * std::sin(azimuth);
    cam.position[2] = distance * std::sin(elevation);
    
    cam.target[0] = 0;
    cam.target[1] = 0;
    cam.target[2] = 0;
    
    cam.up[0] = 0;
    cam.up[1] = 0;
    cam.up[2] = 1;
    
    cam.fov_y = fov_y;
    cam.aspect = static_cast<float>(width) / static_cast<float>(height);
    cam.near_plane = 0.01f;
    cam.far_plane = 100.0f;
    cam.width = width;
    cam.height = height;
    
    cam.computeMatrices();
    
    return cam;
}

// ============================================================================
// DifferentiableRenderer Implementation
// ============================================================================

class DifferentiableRenderer::Impl {
public:
    metal::MetalContext& ctx;
    id<MTLComputePipelineState> projectPipeline = nil;
    id<MTLComputePipelineState> rasterizePipeline = nil;
    id<MTLComputePipelineState> backwardPipeline = nil;
    id<MTLComputePipelineState> l1GradPipeline = nil;
    id<MTLComputePipelineState> adamPipeline = nil;
    
    Impl(metal::MetalContext& context) : ctx(context) {
        createPipelines();
    }
    
    void createPipelines() {
        id<MTLDevice> device = (__bridge id<MTLDevice>)ctx.getDevice();
        NSError* error = nil;
        
        // Load shader library
        NSString* shaderPath = [[NSBundle mainBundle] pathForResource:@"default" ofType:@"metallib"];
        if (!shaderPath) {
            // Try current directory
            shaderPath = @"default.metallib";
        }
        
        id<MTLLibrary> library = [device newLibraryWithFile:shaderPath error:&error];
        if (!library) {
            NSLog(@"Failed to load shader library: %@", error);
            return;
        }
        
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
        
        projectPipeline = createPipeline(@"project_gaussians_forward");
        rasterizePipeline = createPipeline(@"rasterize_gaussians_forward");
        backwardPipeline = createPipeline(@"rasterize_gaussians_backward");
        l1GradPipeline = createPipeline(@"compute_l1_grad");
        adamPipeline = createPipeline(@"adam_step");
    }
};

DifferentiableRenderer::DifferentiableRenderer(metal::MetalContext& ctx)
    : impl_(std::make_unique<Impl>(ctx)) {}

DifferentiableRenderer::~DifferentiableRenderer() = default;

DifferentiableRenderer::ForwardResult DifferentiableRenderer::forward(
    const std::vector<PackedGaussian>& gaussians,
    const Camera& camera,
    const float background[3]) {
    
    ForwardResult result;
    
    if (gaussians.empty()) {
        result.image.resize(camera.width * camera.height * 3, background[0]);
        return result;
    }
    
    id<MTLDevice> device = (__bridge id<MTLDevice>)impl_->ctx.getDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)impl_->ctx.getCommandQueue();
    
    // Create buffers
    size_t num_gaussians = gaussians.size();
    id<MTLBuffer> gaussianBuffer = [device newBufferWithBytes:gaussians.data()
                                                       length:num_gaussians * sizeof(PackedGaussian)
                                                      options:MTLResourceStorageModeShared];
    
    // Projected Gaussians buffer (simplified structure)
    struct ProjectedGaussianGPU {
        float xy[2];
        float depth;
        float conic[3];
        float color[3];
        float opacity;
        int radius;
        uint32_t gaussian_id;
    };
    
    id<MTLBuffer> projectedBuffer = [device newBufferWithLength:num_gaussians * sizeof(ProjectedGaussianGPU)
                                                        options:MTLResourceStorageModeShared];
    
    // Output image buffer (RGBA float)
    size_t num_pixels = camera.width * camera.height;
    id<MTLBuffer> outputBuffer = [device newBufferWithLength:num_pixels * sizeof(float) * 4
                                                     options:MTLResourceStorageModeShared];
    
    // Camera params buffer
    struct CameraParamsGPU {
        float view_matrix[16];
        float proj_matrix[16];
        float view_proj_matrix[16];
        float position[3];
        float fov_x;
        float fov_y;
        float focal_x;
        float focal_y;
        float cx;
        float cy;
        int width;
        int height;
    };
    
    CameraParamsGPU cam_params;
    memcpy(cam_params.view_matrix, camera.view_matrix, sizeof(float) * 16);
    memcpy(cam_params.proj_matrix, camera.proj_matrix, sizeof(float) * 16);
    memcpy(cam_params.view_proj_matrix, camera.view_proj_matrix, sizeof(float) * 16);
    memcpy(cam_params.position, camera.position, sizeof(float) * 3);
    cam_params.fov_x = 2.0f * std::atan(std::tan(camera.fov_y * 0.5f) * camera.aspect);
    cam_params.fov_y = camera.fov_y;
    cam_params.focal_x = camera.width / (2.0f * std::tan(cam_params.fov_x * 0.5f));
    cam_params.focal_y = camera.height / (2.0f * std::tan(camera.fov_y * 0.5f));
    cam_params.cx = camera.width * 0.5f;
    cam_params.cy = camera.height * 0.5f;
    cam_params.width = camera.width;
    cam_params.height = camera.height;
    
    id<MTLBuffer> cameraBuffer = [device newBufferWithBytes:&cam_params
                                                     length:sizeof(cam_params)
                                                    options:MTLResourceStorageModeShared];
    
    // For CPU fallback rendering (simplified)
    // In a full implementation, we'd use the Metal pipelines
    
    // CPU-based forward pass for simplicity
    result.image.resize(num_pixels * 3);
    result.alpha.resize(num_pixels);
    
    // Initialize with background
    for (size_t i = 0; i < num_pixels; ++i) {
        result.image[i * 3 + 0] = background[0];
        result.image[i * 3 + 1] = background[1];
        result.image[i * 3 + 2] = background[2];
        result.alpha[i] = 0.0f;
    }
    
    // Simple CPU rasterization for now
    // TODO: Use Metal pipelines for GPU acceleration
    const float SH_C0 = melkor::utils::SH_C0;
    
    for (size_t g_idx = 0; g_idx < num_gaussians; ++g_idx) {
        const auto& g = gaussians[g_idx];
        
        // Transform position
        float pos[4] = {g.position[0], g.position[1], g.position[2], 1.0f};
        float clip[4] = {0};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                clip[i] += camera.view_proj_matrix[i * 4 + j] * pos[j];
            }
        }
        
        if (clip[3] <= 0.0f) continue;
        
        // NDC to screen
        float ndc_x = clip[0] / clip[3];
        float ndc_y = clip[1] / clip[3];
        float screen_x = (ndc_x * 0.5f + 0.5f) * camera.width;
        float screen_y = (ndc_y * 0.5f + 0.5f) * camera.height;
        
        // Simple isotropic scale for now
        float scale = std::exp(g.scale[0]);
        float radius = std::max(1, static_cast<int>(scale * cam_params.focal_x / clip[3] * 3.0f));
        
        // Color
        float color_r = g.color[0] * SH_C0 + 0.5f;
        float color_g = g.color[1] * SH_C0 + 0.5f;
        float color_b = g.color[2] * SH_C0 + 0.5f;
        color_r = std::clamp(color_r, 0.0f, 1.0f);
        color_g = std::clamp(color_g, 0.0f, 1.0f);
        color_b = std::clamp(color_b, 0.0f, 1.0f);
        
        // Opacity
        float opacity = 1.0f / (1.0f + std::exp(-g.position[3]));
        
        // Rasterize
        int min_x = std::max(0, static_cast<int>(screen_x - radius));
        int max_x = std::min(camera.width - 1, static_cast<int>(screen_x + radius));
        int min_y = std::max(0, static_cast<int>(screen_y - radius));
        int max_y = std::min(camera.height - 1, static_cast<int>(screen_y + radius));
        
        float inv_radius_sq = 1.0f / (radius * radius + 1e-6f);
        
        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                float dx = px + 0.5f - screen_x;
                float dy = py + 0.5f - screen_y;
                float dist_sq = dx * dx + dy * dy;
                
                float gaussian_weight = std::exp(-0.5f * dist_sq * inv_radius_sq);
                float alpha = std::min(0.99f, opacity * gaussian_weight);
                
                if (alpha < 1.0f / 255.0f) continue;
                
                size_t pixel_idx = py * camera.width + px;
                float T = 1.0f - result.alpha[pixel_idx];
                
                result.image[pixel_idx * 3 + 0] = result.image[pixel_idx * 3 + 0] * (1.0f - alpha * T) + color_r * alpha * T;
                result.image[pixel_idx * 3 + 1] = result.image[pixel_idx * 3 + 1] * (1.0f - alpha * T) + color_g * alpha * T;
                result.image[pixel_idx * 3 + 2] = result.image[pixel_idx * 3 + 2] * (1.0f - alpha * T) + color_b * alpha * T;
                result.alpha[pixel_idx] += alpha * T;
            }
        }
    }
    
    return result;
}

DifferentiableRenderer::BackwardResult DifferentiableRenderer::backward(
    const ForwardResult& forward_result,
    const std::vector<float>& grad_image) {
    
    // Simplified backward pass - would need full implementation for production
    BackwardResult result;
    // TODO: Implement proper backward pass
    return result;
}

// ============================================================================
// GaussianFitter Implementation
// ============================================================================

class GaussianFitter::Impl {
public:
    metal::MetalContext* metal_ctx_ = nullptr;
    std::unique_ptr<DifferentiableRenderer> renderer_;
    tinygltf::TinyGLTF loader_;
    
    Impl() = default;
    explicit Impl(metal::MetalContext& ctx) : metal_ctx_(&ctx) {
        renderer_ = std::make_unique<DifferentiableRenderer>(ctx);
    }
    
    // Generate camera views around the object
    std::vector<Camera> generateCameras(int num_views, float distance, int width, int height) {
        std::vector<Camera> cameras;
        cameras.reserve(num_views);
        
        for (int i = 0; i < num_views; ++i) {
            float azimuth = 2.0f * M_PI * i / num_views;
            float elevation = 0.3f;  // ~17 degrees up
            cameras.push_back(Camera::createOrbital(distance, azimuth, elevation, width, height));
        }
        
        return cameras;
    }
    
    // Render mesh to get target images (simplified - returns random colors for demo)
    std::vector<std::vector<uint8_t>> renderMeshTargets(
        const std::vector<float>& positions,
        const std::vector<float>& colors,
        const std::vector<Camera>& cameras) {
        
        std::vector<std::vector<uint8_t>> targets;
        targets.reserve(cameras.size());
        
        // For each camera, render a simple point cloud visualization
        for (const auto& cam : cameras) {
            std::vector<uint8_t> image(cam.width * cam.height * 4, 255);  // White background
            
            size_t num_points = positions.size() / 3;
            for (size_t i = 0; i < num_points; ++i) {
                float pos[4] = {positions[i*3+0], positions[i*3+1], positions[i*3+2], 1.0f};
                float clip[4] = {0};
                
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        clip[r] += cam.view_proj_matrix[r * 4 + c] * pos[c];
                    }
                }
                
                if (clip[3] <= 0.0f) continue;
                
                float ndc_x = clip[0] / clip[3];
                float ndc_y = clip[1] / clip[3];
                
                if (ndc_x < -1.0f || ndc_x > 1.0f || ndc_y < -1.0f || ndc_y > 1.0f) continue;
                
                int px = static_cast<int>((ndc_x * 0.5f + 0.5f) * cam.width);
                int py = static_cast<int>((ndc_y * 0.5f + 0.5f) * cam.height);
                
                if (px >= 0 && px < cam.width && py >= 0 && py < cam.height) {
                    size_t pixel_idx = (py * cam.width + px) * 4;
                    if (colors.size() > i * 3 + 2) {
                        image[pixel_idx + 0] = static_cast<uint8_t>(colors[i*3+0] * 255);
                        image[pixel_idx + 1] = static_cast<uint8_t>(colors[i*3+1] * 255);
                        image[pixel_idx + 2] = static_cast<uint8_t>(colors[i*3+2] * 255);
                    } else {
                        image[pixel_idx + 0] = 128;
                        image[pixel_idx + 1] = 128;
                        image[pixel_idx + 2] = 128;
                    }
                    image[pixel_idx + 3] = 255;
                }
            }
            
            targets.push_back(std::move(image));
        }
        
        return targets;
    }
};

GaussianFitter::GaussianFitter() : impl_(std::make_unique<Impl>()) {}
GaussianFitter::GaussianFitter(metal::MetalContext& metal_ctx)
    : impl_(std::make_unique<Impl>(metal_ctx)) {}
GaussianFitter::~GaussianFitter() = default;

GaussianFitResult GaussianFitter::fitFromGlb(
    const std::string& glb_path,
    const GaussianFitConfig& config) {
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    GaussianFitResult result;
    
    // Load GLB
    tinygltf::Model model;
    std::string err, warn;
    bool success;
    
    if (glb_path.size() >= 4 && glb_path.substr(glb_path.size() - 4) == ".glb") {
        success = impl_->loader_.LoadBinaryFromFile(&model, &err, &warn, glb_path);
    } else {
        success = impl_->loader_.LoadASCIIFromFile(&model, &err, &warn, glb_path);
    }
    
    if (!success) {
        result.error_message = "Failed to load GLB: " + err;
        return result;
    }
    
    // Extract mesh data
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> colors;
    
    for (const auto& mesh : model.meshes) {
        for (const auto& primitive : mesh.primitives) {
            auto pos_it = primitive.attributes.find("POSITION");
            if (pos_it == primitive.attributes.end()) continue;
            
            const auto& pos_accessor = model.accessors[pos_it->second];
            const auto& pos_buffer_view = model.bufferViews[pos_accessor.bufferView];
            const auto& pos_buffer = model.buffers[pos_buffer_view.buffer];
            
            const float* pos_data = reinterpret_cast<const float*>(
                pos_buffer.data.data() + pos_buffer_view.byteOffset + pos_accessor.byteOffset);
            
            for (size_t i = 0; i < pos_accessor.count; ++i) {
                positions.push_back(pos_data[i*3+0]);
                positions.push_back(pos_data[i*3+1]);
                positions.push_back(pos_data[i*3+2]);
            }
            
            // Normals
            auto norm_it = primitive.attributes.find("NORMAL");
            if (norm_it != primitive.attributes.end()) {
                const auto& norm_accessor = model.accessors[norm_it->second];
                const auto& norm_buffer_view = model.bufferViews[norm_accessor.bufferView];
                const auto& norm_buffer = model.buffers[norm_buffer_view.buffer];
                
                const float* norm_data = reinterpret_cast<const float*>(
                    norm_buffer.data.data() + norm_buffer_view.byteOffset + norm_accessor.byteOffset);
                
                for (size_t i = 0; i < norm_accessor.count; ++i) {
                    normals.push_back(norm_data[i*3+0]);
                    normals.push_back(norm_data[i*3+1]);
                    normals.push_back(norm_data[i*3+2]);
                }
            }
            
            // Colors
            auto color_it = primitive.attributes.find("COLOR_0");
            if (color_it != primitive.attributes.end()) {
                const auto& color_accessor = model.accessors[color_it->second];
                const auto& color_buffer_view = model.bufferViews[color_accessor.bufferView];
                const auto& color_buffer = model.buffers[color_buffer_view.buffer];
                
                const uint8_t* color_ptr = color_buffer.data.data() + 
                    color_buffer_view.byteOffset + color_accessor.byteOffset;
                
                for (size_t i = 0; i < color_accessor.count; ++i) {
                    if (color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        colors.push_back(color_ptr[i*3+0] / 255.0f);
                        colors.push_back(color_ptr[i*3+1] / 255.0f);
                        colors.push_back(color_ptr[i*3+2] / 255.0f);
                    } else if (color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                        const float* cf = reinterpret_cast<const float*>(color_ptr);
                        colors.push_back(cf[i*3+0]);
                        colors.push_back(cf[i*3+1]);
                        colors.push_back(cf[i*3+2]);
                    }
                }
            }
        }
    }
    
    if (positions.empty()) {
        result.error_message = "No mesh data found in GLB";
        return result;
    }
    
    // Compute bounding box and center
    float min_pos[3] = {1e10f, 1e10f, 1e10f};
    float max_pos[3] = {-1e10f, -1e10f, -1e10f};
    size_t num_points = positions.size() / 3;
    
    for (size_t i = 0; i < num_points; ++i) {
        for (int j = 0; j < 3; ++j) {
            min_pos[j] = std::min(min_pos[j], positions[i*3+j]);
            max_pos[j] = std::max(max_pos[j], positions[i*3+j]);
        }
    }
    
    float center[3] = {
        (min_pos[0] + max_pos[0]) * 0.5f,
        (min_pos[1] + max_pos[1]) * 0.5f,
        (min_pos[2] + max_pos[2]) * 0.5f
    };
    
    float extent = std::max({max_pos[0] - min_pos[0], max_pos[1] - min_pos[1], max_pos[2] - min_pos[2]});
    
    // Center positions
    for (size_t i = 0; i < num_points; ++i) {
        positions[i*3+0] -= center[0];
        positions[i*3+1] -= center[1];
        positions[i*3+2] -= center[2];
    }
    
    // Generate cameras
    float camera_distance = extent * 2.0f;
    auto cameras = impl_->generateCameras(config.num_views, camera_distance, 
                                           config.render_width, config.render_height);
    
    // Render target images from mesh
    auto target_images = impl_->renderMeshTargets(positions, colors, cameras);
    
    // Initialize Gaussians using enhanced converter
    EnhancedConverter converter;
    EnhancedConversionConfig conv_config;
    conv_config.knn_neighbors = 8;
    conv_config.scale_factor = 0.3f;
    conv_config.use_surface_alignment = true;
    conv_config.convert_coordinate_system = false;  // Already processed
    
    auto conv_result = converter.convertFromMesh(positions, normals, colors, {}, conv_config);
    if (!conv_result.success) {
        result.error_message = "Failed to initialize Gaussians: " + conv_result.error_message;
        return result;
    }
    
    GaussianCloud cloud = std::move(conv_result.cloud);

    std::cout << "Initialized " << cloud.size() << " Gaussians" << std::endl;

    // NOTE on optimization: this fitter has no working backward pass (see
    // DifferentiableRenderer::backward, which is a stub). The previous loop
    // perturbed Gaussian colors with random noise for `num_iterations` rounds
    // and reported the minimum *observed* loss as "final_loss", which made the
    // output strictly worse over time while advertising improvement. Rather
    // than fake gradient descent, we measure the L1 reprojection error of the
    // surface-aligned initialization once and return it honestly. Real
    // differentiable fitting on Metal should be wired through the backward
    // pipeline (see the 3DGS reference) before this mode claims to "fit".
    auto packed = cloud.toPackedFormat();
    float best_loss = 0.0f;
    for (size_t cam_idx = 0; cam_idx < cameras.size(); ++cam_idx) {
        const auto& cam = cameras[cam_idx];
        auto forward_result = impl_->renderer_->forward(packed, cam, config.background);
        const auto& target = target_images[cam_idx];
        double loss_sum = 0.0;
        for (int py = 0; py < cam.height; ++py) {
            for (int px = 0; px < cam.width; ++px) {
                size_t img_idx = (py * cam.width + px) * 3;
                size_t tgt_idx = (py * cam.width + px) * 4;
                loss_sum += std::abs(static_cast<double>(forward_result.image[img_idx + 0]) - target[tgt_idx + 0] / 255.0);
                loss_sum += std::abs(static_cast<double>(forward_result.image[img_idx + 1]) - target[tgt_idx + 1] / 255.0);
                loss_sum += std::abs(static_cast<double>(forward_result.image[img_idx + 2]) - target[tgt_idx + 2] / 255.0);
            }
        }
        best_loss += static_cast<float>(loss_sum / (cam.width * cam.height * 3));
    }
    best_loss /= static_cast<float>(cameras.size());

    if (config.progress_callback) {
        config.progress_callback(0, best_loss, cloud.size());
    }
    std::cout << "Initialization L1 reprojection error: " << best_loss << std::endl;
    std::cout << "(No gradient-based optimization available; returning surface-aligned init.)" << std::endl;

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    result.success = true;
    result.cloud = std::move(cloud);
    result.final_loss = best_loss;
    result.total_iterations = 0;  // no optimization iterations were performed
    result.peak_gaussians = result.cloud.size();
    result.fitting_time_seconds = duration.count() / 1000.0f;

    std::cout << "Fitting complete in " << result.fitting_time_seconds << "s" << std::endl;
    std::cout << "Final loss: " << result.final_loss << std::endl;

    return result;
}

GaussianFitResult GaussianFitter::fitFromImages(
    const std::vector<std::vector<uint8_t>>& images,
    const std::vector<Camera>& cameras,
    const GaussianCloud& initial_cloud,
    const GaussianFitConfig& config) {
    
    GaussianFitResult result;
    result.error_message = "fitFromImages not yet implemented";
    return result;
}

std::vector<uint8_t> GaussianFitter::renderView(
    const GaussianCloud& cloud,
    const Camera& camera,
    int width, int height) {
    
    auto packed = cloud.toPackedFormat();
    float bg[3] = {1.0f, 1.0f, 1.0f};
    
    Camera cam = camera;
    cam.width = width;
    cam.height = height;
    cam.computeMatrices();
    
    auto forward_result = impl_->renderer_->forward(packed, cam, bg);
    
    std::vector<uint8_t> image(width * height * 4);
    for (int i = 0; i < width * height; ++i) {
        image[i * 4 + 0] = static_cast<uint8_t>(std::clamp(forward_result.image[i * 3 + 0] * 255.0f, 0.0f, 255.0f));
        image[i * 4 + 1] = static_cast<uint8_t>(std::clamp(forward_result.image[i * 3 + 1] * 255.0f, 0.0f, 255.0f));
        image[i * 4 + 2] = static_cast<uint8_t>(std::clamp(forward_result.image[i * 3 + 2] * 255.0f, 0.0f, 255.0f));
        image[i * 4 + 3] = 255;
    }
    
    return image;
}

// ============================================================================
// MeshRenderer Implementation (Placeholder)
// ============================================================================

class MeshRenderer::Impl {
public:
    metal::MetalContext& ctx;
    std::vector<float> positions;
    std::vector<float> colors;
    float bbox_min[3] = {0};
    float bbox_max[3] = {0};
    
    Impl(metal::MetalContext& context) : ctx(context) {}
};

MeshRenderer::MeshRenderer(metal::MetalContext& ctx)
    : impl_(std::make_unique<Impl>(ctx)) {}

MeshRenderer::~MeshRenderer() = default;

bool MeshRenderer::loadGlb(const std::string& path) {
    // TODO: Implement full mesh loading
    return false;
}

std::vector<uint8_t> MeshRenderer::render(const Camera& camera, int width, int height) {
    // TODO: Implement mesh rendering
    return std::vector<uint8_t>(width * height * 4, 255);
}

void MeshRenderer::getBoundingBox(float min[3], float max[3]) const {
    memcpy(min, impl_->bbox_min, sizeof(float) * 3);
    memcpy(max, impl_->bbox_max, sizeof(float) * 3);
}

} // namespace melkor
