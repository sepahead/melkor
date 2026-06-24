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
    // All three matrices are stored ROW-MAJOR: element at (row r, col c) is
    // matrix[r*4 + c]. This matches the consumer in DifferentiableRenderer
    // (clip[i] = sum_j view_proj[i*4+j] * pos[j]). The previous version stored
    // view/proj column-major but multiplied/consumed them as row-major, which
    // transposed the camera basis and produced clip[3]=0 for the look-at target.

    // Compute view matrix (look-at).
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

    // View matrix, row-major. Rows: right, up, -forward, translation-negatives.
    view_matrix[0] = rx;  view_matrix[1] = ry;  view_matrix[2]  = rz;  view_matrix[3]  = -(rx*position[0] + ry*position[1] + rz*position[2]);
    view_matrix[4] = ux;  view_matrix[5] = uy;  view_matrix[6]  = uz;  view_matrix[7]  = -(ux*position[0] + uy*position[1] + uz*position[2]);
    view_matrix[8] = -fx; view_matrix[9] = -fy; view_matrix[10] = -fz; view_matrix[11] = (fx*position[0] + fy*position[1] + fz*position[2]);
    view_matrix[12] = 0;  view_matrix[13] = 0;  view_matrix[14] = 0;   view_matrix[15] = 1;

    // Projection matrix (OpenGL-style perspective), row-major.
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
    proj_matrix[11] = -2.0f * far_plane * near_plane / (far_plane - near_plane);

    proj_matrix[12] = 0;
    proj_matrix[13] = 0;
    proj_matrix[14] = -1;
    proj_matrix[15] = 0;

    // view_proj = proj * view, row-major: C[r*4+c] = sum_k A[r*4+k]*B[k*4+c].
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) {
                acc += proj_matrix[r * 4 + k] * view_matrix[k * 4 + c];
            }
            view_proj_matrix[r * 4 + c] = acc;
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

// ============================================================================
// Render state for the differentiable backward pass.
//
// Alpha-blended Gaussian splatting composites gaussians front-to-back as:
//     C  += color * alpha * T
//     T'  = T * (1 - alpha)          (accumulated transmittance)
// where alpha = opacity * exp(-0.5 * d^2 / r^2). To backpropagate dL/d(gaussian
// params) we need, for each pixel, the per-gaussian (alpha, T_at_composite,
// color) used during the forward. We also cache the screen-space center and
// radius so position/scale gradients can be reconstructed without recomputing
// the projection. This is the standard 3DGS backward state in compact form.
//
// NOTE: gaussians are composited in INPUT order (not depth-sorted). For the
// mesh-fitting use case the source geometry is roughly convex so this is an
// acceptable approximation; a production renderer would sort per-tile.
// ============================================================================

struct PixelContribution {
    uint32_t gaussian_id;
    float alpha;       // alpha contribution of this gaussian at this pixel
    float T_at_comp;   // accumulated transmittance just before this gaussian
    float color[3];    // rendered color (post-SH, pre-clamp) of this gaussian
    float opacity;     // sigmoid(logit) of the gaussian (stored to reconstruct
                       // the opacity-logit gradient without the source gaussian)
    float gauss_weight;// alpha / opacity: the spatial falloff, needed for the
                       // d(alpha)/d(logit) = sigmoid' * weight chain rule
};

struct RenderState {
    int width = 0;
    int height = 0;
    // Per-pixel ordered list of contributions (front-to-back as composited).
    std::vector<std::vector<PixelContribution>> pixels;
    // Per-gaussian projection cache (indexed by gaussian id).
    struct Proj {
        float screen_x = 0;
        float screen_y = 0;
        float clip_w = 0;        // clip[3], for position gradient scaling
        float radius = 0;        // pixel radius (pre-exp scale)
        bool visible = false;
    };
    std::vector<Proj> projections;
};

// ForwardResult owns the RenderState via void*. These special members are
// defined here (not in the header) because RenderState is private to this TU.
// The destructor frees the state if backward() never consumed it; the move
// operations transfer ownership and null the source to prevent double-free.
DifferentiableRenderer::ForwardResult::~ForwardResult() {
    delete static_cast<RenderState*>(internal_state);
}
DifferentiableRenderer::ForwardResult::ForwardResult(ForwardResult&& o) noexcept
    : image(std::move(o.image)), alpha(std::move(o.alpha)), internal_state(o.internal_state) {
    o.internal_state = nullptr;
}
DifferentiableRenderer::ForwardResult& DifferentiableRenderer::ForwardResult::operator=(ForwardResult&& o) noexcept {
    if (this != &o) {
        delete static_cast<RenderState*>(internal_state);
        image = std::move(o.image);
        alpha = std::move(o.alpha);
        internal_state = o.internal_state;
        o.internal_state = nullptr;
    }
    return *this;
}

DifferentiableRenderer::ForwardResult DifferentiableRenderer::forward(
    const std::vector<PackedGaussian>& gaussians,
    const Camera& camera,
    const float background[3]) {

    ForwardResult result;

    size_t num_pixels = static_cast<size_t>(camera.width) * camera.height;
    if (gaussians.empty()) {
        result.image.resize(num_pixels * 3);
        result.alpha.resize(num_pixels, 0.0f);
        for (size_t i = 0; i < num_pixels; ++i) {
            result.image[i * 3 + 0] = background[0];
            result.image[i * 3 + 1] = background[1];
            result.image[i * 3 + 2] = background[2];
        }
        return result;
    }

    size_t num_gaussians = gaussians.size();
    const float SH_C0 = melkor::utils::SH_C0;

    result.image.resize(num_pixels * 3);
    result.alpha.assign(num_pixels, 0.0f);

    // Render state for the backward pass. Allocated unconditionally so backward
    // can rely on its structure even if the caller didn't composite anything.
    auto* state = new RenderState();
    state->width = camera.width;
    state->height = camera.height;
    state->pixels.resize(num_pixels);
    state->projections.resize(num_gaussians);

    // Initialize image with background.
    for (size_t i = 0; i < num_pixels; ++i) {
        result.image[i * 3 + 0] = background[0];
        result.image[i * 3 + 1] = background[1];
        result.image[i * 3 + 2] = background[2];
    }

    // Focal length depends only on camera params — hoist out of the per-Gaussian loop.
    float focal_x = camera.width / (2.0f * std::tan(
        (2.0f * std::atan(std::tan(camera.fov_y * 0.5f) * camera.aspect)) * 0.5f));

    for (size_t g_idx = 0; g_idx < num_gaussians; ++g_idx) {
        const auto& g = gaussians[g_idx];
        auto& proj = state->projections[g_idx];

        // Transform position.
        float pos[4] = {g.position[0], g.position[1], g.position[2], 1.0f};
        float clip[4] = {0};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                clip[i] += camera.view_proj_matrix[i * 4 + j] * pos[j];
            }
        }

        if (clip[3] <= 0.0f) {
            proj.visible = false;
            continue;
        }
        proj.visible = true;
        proj.clip_w = clip[3];

        float ndc_x = clip[0] / clip[3];
        float ndc_y = clip[1] / clip[3];
        float screen_x = (ndc_x * 0.5f + 0.5f) * camera.width;
        float screen_y = (ndc_y * 0.5f + 0.5f) * camera.height;
        proj.screen_x = screen_x;
        proj.screen_y = screen_y;

        // Isotropic scale from scale[0] (log space).
        float scale = std::exp(g.scale[0]);
        float radius = std::max(1.0f, scale * focal_x / clip[3] * 3.0f);
        proj.radius = radius;

        // Color from SH-DC. NOTE: no clamp here -- clamping has zero gradient
        // outside [0,1] and would silently kill the backward pass. Colors may
        // go out of gamut during fitting and self-correct via the L1 loss.
        float color_r = g.color[0] * SH_C0 + 0.5f;
        float color_g = g.color[1] * SH_C0 + 0.5f;
        float color_b = g.color[2] * SH_C0 + 0.5f;

        // Opacity from position[3] (logit space).
        float opacity = 1.0f / (1.0f + std::exp(-g.position[3]));

        int min_x = std::max(0, static_cast<int>(screen_x - radius));
        int max_x = std::min(camera.width - 1, static_cast<int>(screen_x + radius));
        int min_y = std::max(0, static_cast<int>(screen_y - radius));
        int max_y = std::min(camera.height - 1, static_cast<int>(screen_y + radius));

        float radius_sq = radius * radius + 1e-6f;
        float inv_radius_sq = 1.0f / radius_sq;

        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                float dx = px + 0.5f - screen_x;
                float dy = py + 0.5f - screen_y;
                float dist_sq = dx * dx + dy * dy;

                float gaussian_weight = std::exp(-0.5f * dist_sq * inv_radius_sq);
                // No clamp on alpha either: min(0.99, ...) is non-differentiable
                // at the boundary. The sigmoid-bounded opacity already keeps
                // alpha < 1, and very small contributions are pruned below.
                float alpha = opacity * gaussian_weight;

                if (alpha < 1.0f / 255.0f) continue;

                size_t pixel_idx = static_cast<size_t>(py) * camera.width + px;
                float T = 1.0f - result.alpha[pixel_idx];

                // Record contribution for the backward pass BEFORE compositing,
                // so T_at_comp is the transmittance seen by this gaussian.
                state->pixels[pixel_idx].push_back({
                    static_cast<uint32_t>(g_idx), alpha, T,
                    {color_r, color_g, color_b},
                    opacity, gaussian_weight});

                result.image[pixel_idx * 3 + 0] += color_r * alpha * T;
                result.image[pixel_idx * 3 + 1] += color_g * alpha * T;
                result.image[pixel_idx * 3 + 2] += color_b * alpha * T;
                result.alpha[pixel_idx] += alpha * T;
            }
        }
    }

    // Hand ownership of the state to the ForwardResult. backward() will delete it.
    result.internal_state = state;
    return result;
}

DifferentiableRenderer::BackwardResult DifferentiableRenderer::backward(
    ForwardResult& forward_result,
    const std::vector<float>& grad_image) {

    BackwardResult result;
    auto* state = static_cast<RenderState*>(forward_result.internal_state);
    if (state == nullptr) {
        // No forward state (e.g. empty cloud) -> no gradients.
        return result;
    }

    const int W = state->width;
    const int H = state->height;
    const size_t num_pixels = static_cast<size_t>(W) * H;

    // We need grad wrt each gaussian's rendered color, opacity, screen position,
    // and scale. Accumulate from every pixel that the gaussian touched.
    // Because we don't know N here from the state alone, infer it from the
    // projection cache size.
    const size_t N = state->projections.size();
    result.grad_positions.assign(N * 3, 0.0f);
    result.grad_scales.assign(N * 3, 0.0f);
    result.grad_rotations.assign(N * 4, 0.0f);
    result.grad_colors.assign(N * 3, 0.0f);
    result.grad_opacities.assign(N, 0.0f);

    // Per-pixel running gradient that flows back through the transmittance
    // chain. In the composite C += color * alpha * T with T = prod(1-alpha) of
    // earlier gaussians, changing a gaussian's alpha affects every LATER
    // gaussian's T (and hence its color contribution). Iterating the composite
    // in REVERSE order, `accum` tracks dL/d(T_next) -- the gradient w.r.t. the
    // transmittance at the composite point of the next-later gaussian (already
    // processed). For the last-composited gaussian there is no later gaussian,
    // so accum starts at 0.
    //
    // Standard 3DGS alpha-blend backward, per pixel, per gaussian i (T_i =
    // transmittance when i composited, recorded during forward; g = dL/dC):
    //   dL/dcolor_i = alpha_i * T_i * g            (direct; no chain needed)
    //   dL/dalpha_i = T_i * (color_i . g - accum)  (direct + transmittance chain)
    //   accum       = accum * (1 - alpha_i) + (color_i . g) * alpha_i
    // Derivation: dL/d(T_{i+1}) = accum, and T_{i+1} = T_i*(1-alpha_i), so
    // d(T_{i+1})/d(alpha_i) = -T_i, giving the indirect term -accum*T_i; the
    // accum update follows from dL/d(T_i) = accum*(1-alpha_i) + (color_i.g)*alpha_i.
    //
    // alpha_i = opacity * gaussian_weight, where opacity = sigmoid(logit) and
    // gaussian_weight is the spatial falloff (both recorded during forward).
    // The full chain rule to the opacity LOGIT (the packed parameter in
    // position[3]) is:
    //   dL/d(logit) = dL/d(alpha) * d(alpha)/d(opacity) * d(opacity)/d(logit)
    //              = dalpha * gaussian_weight * sigmoid'(logit)
    //              = dalpha * gauss_weight * opacity * (1 - opacity)
    // Skipping either factor silently inflates the gradient by 5-25x, which
    // the gradient-check test catches. Position/scale gradients require the
    // source gaussian's opacity/weight split and are left for a richer backward.
    std::vector<float> accum(num_pixels, 0.0f);

    for (size_t pixel_idx = 0; pixel_idx < num_pixels; ++pixel_idx) {
        const auto& contribs = state->pixels[pixel_idx];
        if (contribs.empty()) continue;

        float gx = grad_image[pixel_idx * 3 + 0];
        float gy = grad_image[pixel_idx * 3 + 1];
        float gz = grad_image[pixel_idx * 3 + 2];

        // Replay in reverse composite order (last-composited first).
        for (auto it = contribs.rbegin(); it != contribs.rend(); ++it) {
            const auto& c = *it;
            uint32_t gid = c.gaussian_id;
            float alpha = c.alpha;
            float T = c.T_at_comp;

            // Color gradient. The forward maps the packed SH-DC field to a
            // rendered color as rendered = sh_dc * SH_C0 + 0.5, so the gradient
            // w.r.t. the SH-DC parameter is SH_C0 times the gradient w.r.t. the
            // rendered color: dL/d(sh_dc) = SH_C0 * alpha * T * dL/dC.
            const float SH_C0 = melkor::utils::SH_C0;
            float dcolor_scale = alpha * T * SH_C0;
            result.grad_colors[gid * 3 + 0] += dcolor_scale * gx;
            result.grad_colors[gid * 3 + 1] += dcolor_scale * gy;
            result.grad_colors[gid * 3 + 2] += dcolor_scale * gz;

            // Alpha gradient: direct term + transmittance-chain term.
            float color_dot_grad = c.color[0] * gx + c.color[1] * gy + c.color[2] * gz;
            float dalpha = T * (color_dot_grad - accum[pixel_idx]);

            // Update accum to dL/d(T_i) for the next-earlier gaussian.
            accum[pixel_idx] = accum[pixel_idx] * (1.0f - alpha) + color_dot_grad * alpha;

            // Route the alpha gradient into the opacity LOGIT via the full
            // chain rule (see derivation above). c.gauss_weight is the spatial
            // falloff and c.opacity is sigmoid(logit), so sigmoid'(logit) =
            // opacity * (1 - opacity).
            float sig_prime = c.opacity * (1.0f - c.opacity);
            result.grad_opacities[gid] += dalpha * c.gauss_weight * sig_prime;
        }
    }

    // The forward pass allocated the render state with `new` and transferred
    // ownership via internal_state. backward is the sole consumer, so it frees
    // it here and nulls the pointer so a repeat backward call is a safe no-op.
    delete state;
    forward_result.internal_state = nullptr;

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

    // NOTE on optimization: DifferentiableRenderer::backward now implements a
    // real alpha-blend backward pass (verified by test_gradient_check), but it
    // only computes color and opacity gradients — position and scale gradients
    // are not yet wired. The previous loop perturbed Gaussian colors with random
    // noise for `num_iterations` rounds and reported the minimum *observed* loss
    // as "final_loss", which made the output strictly worse over time while
    // advertising improvement. Rather than run a partial optimizer that ignores
    // position/scale, we measure the L1 reprojection error of the surface-aligned
    // initialization once and return it honestly. Full gradient-based fitting
    // should wire all four gradient channels through the backward pipeline.
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
