#pragma once

#include "melkor/gaussian_data.hpp"
#include "melkor/metal_compute.hpp"
#include <memory>
#include <vector>
#include <functional>

namespace melkor {

// Camera parameters for rendering
struct Camera {
    float position[3];      // Camera position in world space
    float target[3];        // Look-at target
    float up[3];            // Up vector
    float fov_y;            // Vertical field of view in radians
    float aspect;           // Aspect ratio (width/height)
    float near_plane;       // Near clipping plane
    float far_plane;        // Far clipping plane
    int width;              // Image width in pixels
    int height;             // Image height in pixels
    
    // Computed matrices (call computeMatrices() to update)
    float view_matrix[16];
    float proj_matrix[16];
    float view_proj_matrix[16];
    
    void computeMatrices();
    
    // Helper to create orbital camera around origin
    static Camera createOrbital(float distance, float azimuth, float elevation,
                                int width, int height, float fov_y = 0.8f);
};

// Configuration for Gaussian fitting optimization
struct GaussianFitConfig {
    // Optimization parameters
    int num_iterations = 3000;         // Total optimization iterations
    int warmup_iterations = 500;       // Iterations before densification
    int densify_interval = 100;        // Densify every N iterations
    int densify_until = 2000;          // Stop densifying after this iteration
    
    // Learning rates
    float lr_position = 0.00016f;
    float lr_scale = 0.005f;
    float lr_rotation = 0.001f;
    float lr_color = 0.0025f;
    float lr_opacity = 0.05f;
    
    // Loss weights
    float ssim_weight = 0.2f;          // SSIM loss weight (1-ssim_weight = L1 weight)
    
    // Densification thresholds
    float densify_grad_thresh = 0.0002f;   // Gradient threshold for densification
    float densify_size_thresh = 0.01f;     // Scale threshold for splitting
    float cull_opacity_thresh = 0.005f;    // Cull Gaussians below this opacity
    
    // Rendering
    int num_views = 8;                 // Number of camera views for fitting
    int render_width = 512;            // Render resolution
    int render_height = 512;
    float camera_distance = 3.0f;      // Camera distance from origin
    
    // Background color (RGB 0-1)
    float background[3] = {1.0f, 1.0f, 1.0f};
    
    // Progress callback (iteration, loss, num_gaussians)
    std::function<void(int, float, size_t)> progress_callback;
};

// Result of Gaussian fitting
struct GaussianFitResult {
    bool success = false;
    std::string error_message;
    GaussianCloud cloud;
    
    // Statistics
    float final_loss = 0.0f;
    float final_psnr = 0.0f;
    int total_iterations = 0;
    size_t peak_gaussians = 0;
    float fitting_time_seconds = 0.0f;
};

// Experimental differentiable-renderer surface. Fitting entry points fail
// closed until an optimizer and triangle target renderer are implemented.
class GaussianFitter {
public:
    GaussianFitter();
    explicit GaussianFitter(metal::MetalContext& metal_ctx);
    ~GaussianFitter();
    
    // Fit Gaussians to a GLB mesh by rendering target views
    GaussianFitResult fitFromGlb(
        const std::string& glb_path,
        const GaussianFitConfig& config = {});
    
    // Fit Gaussians to target images directly
    GaussianFitResult fitFromImages(
        const std::vector<std::vector<uint8_t>>& images,  // RGBA images
        const std::vector<Camera>& cameras,
        const GaussianCloud& initial_cloud,
        const GaussianFitConfig& config = {});
    
    // Render a single view (for debugging/preview)
    std::vector<uint8_t> renderView(
        const GaussianCloud& cloud,
        const Camera& camera,
        int width, int height);
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Differentiable Gaussian rasterizer on Metal
class DifferentiableRenderer {
public:
    DifferentiableRenderer(metal::MetalContext& ctx);
    ~DifferentiableRenderer();
    
    // Forward pass: render Gaussians to image
    struct ForwardResult {
        std::vector<float> image;      // RGB float image (H*W*3)
        std::vector<float> alpha;      // Alpha channel (H*W)
        // Internal state for backward pass (owned by ForwardResult, freed on
        // destruction or when backward() consumes it). Move-only to prevent
        // double-free of the heap-allocated RenderState.
        void* internal_state = nullptr;

        ForwardResult() = default;
        ~ForwardResult();  // defined in the .mm TU where RenderState is visible
        ForwardResult(const ForwardResult&) = delete;
        ForwardResult& operator=(const ForwardResult&) = delete;
        ForwardResult(ForwardResult&&) noexcept;
        ForwardResult& operator=(ForwardResult&&) noexcept;
    };
    
    ForwardResult forward(
        const std::vector<PackedGaussian>& gaussians,
        const Camera& camera,
        const float background[3]);
    
    // Backward pass: compute gradients
    struct BackwardResult {
        std::vector<float> grad_positions;   // N*3
        std::vector<float> grad_scales;      // N*3
        std::vector<float> grad_rotations;   // N*4
        std::vector<float> grad_colors;      // N*3
        std::vector<float> grad_opacities;   // N*1
    };
    
    BackwardResult backward(
        ForwardResult& forward_result,
        const std::vector<float>& grad_image);  // dL/d(image)
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Simple mesh renderer for generating target images
class MeshRenderer {
public:
    MeshRenderer(metal::MetalContext& ctx);
    ~MeshRenderer();
    
    // Load mesh from GLB
    bool loadGlb(const std::string& path);
    
    // Render mesh to image
    std::vector<uint8_t> render(const Camera& camera, int width, int height);
    
    // Get mesh bounding box
    void getBoundingBox(float min[3], float max[3]) const;
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace melkor
