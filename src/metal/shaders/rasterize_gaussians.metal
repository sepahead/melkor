#include <metal_stdlib>
using namespace metal;

// Constants for tile-based rasterization (reserved for future tile-based path)
constant int BLOCK_X [[maybe_unused]] = 16;
constant int BLOCK_Y [[maybe_unused]] = 16;
constant float SH_C0 = 0.28209479177387814f;

// Packed Gaussian structure matching C++ PackedGaussian
struct PackedGaussian {
    float4 position;   // x, y, z, opacity (logit)
    float4 color;      // f_dc_0, f_dc_1, f_dc_2, padding
    float4 scale;      // scale_0, scale_1, scale_2, padding (log space)
    float4 rotation;   // rot_0, rot_1, rot_2, rot_3 (quaternion w,x,y,z)
};

// Camera parameters
struct CameraParams {
    float4x4 view_matrix;
    float4x4 proj_matrix;
    float4x4 view_proj_matrix;
    float3 position;
    float fov_x;
    float fov_y;
    float focal_x;
    float focal_y;
    float cx;
    float cy;
    int width;
    int height;
};

// Projected Gaussian for rasterization
struct ProjectedGaussian {
    float2 xy;           // Screen space position
    float depth;         // View space depth
    float3 conic;        // Inverse 2D covariance (xx, xy, yy)
    float3 color;        // RGB color
    float opacity;       // Opacity (after sigmoid)
    int radius;          // Tile coverage radius
    uint gaussian_id;    // Original Gaussian index
};

// Tile bin info
struct TileBin {
    uint start;
    uint count;
};

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

float sigmoid(float x) {
    return 1.0f / (1.0f + exp(-x));
}

float3x3 quatToRotMat(float4 q) {
    float w = q.x, x = q.y, y = q.z, z = q.w;
    
    return float3x3(
        float3(1.0f - 2.0f*(y*y + z*z), 2.0f*(x*y - w*z), 2.0f*(x*z + w*y)),
        float3(2.0f*(x*y + w*z), 1.0f - 2.0f*(x*x + z*z), 2.0f*(y*z - w*x)),
        float3(2.0f*(x*z - w*y), 2.0f*(y*z + w*x), 1.0f - 2.0f*(x*x + y*y))
    );
}

float3 computeCov2D(float3 mean, float3 scale, float4 quat, float4x4 viewMat, 
                    float focal_x, float focal_y, float tan_fovx, float tan_fovy) {
    // Compute 3D covariance
    float3x3 R = quatToRotMat(quat);
    float3x3 S = float3x3(
        float3(scale.x, 0.0f, 0.0f),
        float3(0.0f, scale.y, 0.0f),
        float3(0.0f, 0.0f, scale.z)
    );
    float3x3 RS = R * S;
    float3x3 cov3D = RS * transpose(RS);
    
    // Transform to view space
    float3 t = (viewMat * float4(mean, 1.0f)).xyz;
    
    // Compute Jacobian of projection
    float limx = 1.3f * tan_fovx;
    float limy = 1.3f * tan_fovy;
    float txtz = t.x / t.z;
    float tytz = t.y / t.z;
    t.x = min(limx, max(-limx, txtz)) * t.z;
    t.y = min(limy, max(-limy, tytz)) * t.z;
    
    float3x3 J = float3x3(
        float3(focal_x / t.z, 0.0f, -(focal_x * t.x) / (t.z * t.z)),
        float3(0.0f, focal_y / t.z, -(focal_y * t.y) / (t.z * t.z)),
        float3(0.0f, 0.0f, 0.0f)
    );
    
    float3x3 W = float3x3(
        float3(viewMat[0][0], viewMat[1][0], viewMat[2][0]),
        float3(viewMat[0][1], viewMat[1][1], viewMat[2][1]),
        float3(viewMat[0][2], viewMat[1][2], viewMat[2][2])
    );
    
    float3x3 T = J * W;
    float3x3 cov2D = T * cov3D * transpose(T);
    
    // Add blur for anti-aliasing
    cov2D[0][0] += 0.3f;
    cov2D[1][1] += 0.3f;
    
    return float3(cov2D[0][0], cov2D[0][1], cov2D[1][1]);
}

// ============================================================================
// FORWARD PASS KERNELS
// ============================================================================

// Kernel 1: Project 3D Gaussians to 2D
kernel void project_gaussians_forward(
    device const PackedGaussian* gaussians [[buffer(0)]],
    device ProjectedGaussian* projected [[buffer(1)]],
    device atomic_uint* num_visible [[buffer(2)]],
    constant CameraParams& camera [[buffer(3)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    PackedGaussian g = gaussians[id];
    
    // Get position and transform to clip space
    float3 pos = g.position.xyz;
    float4 clip = camera.view_proj_matrix * float4(pos, 1.0f);
    
    // Frustum culling
    if (clip.w <= 0.0f || 
        clip.x < -clip.w * 1.3f || clip.x > clip.w * 1.3f ||
        clip.y < -clip.w * 1.3f || clip.y > clip.w * 1.3f ||
        clip.z < 0.0f || clip.z > clip.w) {
        projected[id].radius = -1;  // Mark as culled
        return;
    }
    
    // NDC to screen space
    float2 ndc = clip.xy / clip.w;
    float2 screen = float2(
        (ndc.x * 0.5f + 0.5f) * float(camera.width),
        (ndc.y * 0.5f + 0.5f) * float(camera.height)
    );
    
    // Get view-space depth
    float4 view_pos = camera.view_matrix * float4(pos, 1.0f);
    float depth = view_pos.z;
    
    // Compute 2D covariance
    float3 scale = exp(g.scale.xyz);
    float4 quat = g.rotation / length(g.rotation);
    
    float tan_fovx = tan(camera.fov_x * 0.5f);
    float tan_fovy = tan(camera.fov_y * 0.5f);
    
    float3 cov2D = computeCov2D(pos, scale, quat, camera.view_matrix,
                                 camera.focal_x, camera.focal_y,
                                 tan_fovx, tan_fovy);
    
    // Invert covariance to get conic
    float det = cov2D.x * cov2D.z - cov2D.y * cov2D.y;
    if (det <= 0.0f) {
        projected[id].radius = -1;
        return;
    }
    float inv_det = 1.0f / det;
    float3 conic = float3(cov2D.z * inv_det, -cov2D.y * inv_det, cov2D.x * inv_det);
    
    // Compute radius (3 sigma)
    float mid = 0.5f * (cov2D.x + cov2D.z);
    float lambda1 = mid + sqrt(max(0.1f, mid * mid - det));
    float lambda2 = mid - sqrt(max(0.1f, mid * mid - det));
    int radius = int(ceil(3.0f * sqrt(max(lambda1, lambda2))));
    
    if (radius <= 0) {
        projected[id].radius = -1;
        return;
    }
    
    // Color (SH DC to RGB)
    float3 color = g.color.xyz * SH_C0 + 0.5f;
    color = clamp(color, 0.0f, 1.0f);
    
    // Opacity
    float opacity = sigmoid(g.position.w);
    
    // Store projected Gaussian
    ProjectedGaussian p;
    p.xy = screen;
    p.depth = depth;
    p.conic = conic;
    p.color = color;
    p.opacity = opacity;
    p.radius = radius;
    p.gaussian_id = id;
    
    projected[id] = p;
    
    // Count visible
    atomic_fetch_add_explicit(num_visible, 1, memory_order_relaxed);
}

// Kernel 2: Rasterize Gaussians to pixels (simple per-pixel approach)
kernel void rasterize_gaussians_forward(
    device const ProjectedGaussian* projected [[buffer(0)]],
    device float4* output_image [[buffer(1)]],         // RGB + transmittance
    device float* output_depth [[buffer(2)]],          // For sorting
    constant uint& num_gaussians [[buffer(3)]],
    constant float3& background [[buffer(4)]],
    constant int2& image_size [[buffer(5)]],
    uint2 pixel_id [[thread_position_in_grid]]
) {
    if (pixel_id.x >= uint(image_size.x) || pixel_id.y >= uint(image_size.y)) return;
    
    float2 pixel_center = float2(pixel_id) + 0.5f;
    uint pixel_idx = pixel_id.y * uint(image_size.x) + pixel_id.x;
    
    // Accumulate color with alpha blending (front to back)
    float3 accumulated_color = float3(0.0f);
    float T = 1.0f;  // Transmittance
    
    // Simple approach: iterate all Gaussians (for small counts)
    // TODO: Use tile-based binning for large counts
    for (uint i = 0; i < num_gaussians; ++i) {
        ProjectedGaussian g = projected[i];
        if (g.radius < 0) continue;  // Culled
        
        // Check if pixel is within radius
        float2 d = pixel_center - g.xy;
        if (abs(d.x) > float(g.radius) || abs(d.y) > float(g.radius)) continue;
        
        // Compute Gaussian weight
        float power = -0.5f * (g.conic.x * d.x * d.x + 2.0f * g.conic.y * d.x * d.y + g.conic.z * d.y * d.y);
        if (power > 0.0f) continue;
        
        float alpha = min(0.99f, g.opacity * exp(power));
        if (alpha < 1.0f / 255.0f) continue;
        
        // Accumulate
        accumulated_color += T * alpha * g.color;
        T *= (1.0f - alpha);
        
        // Early termination
        if (T < 0.0001f) break;
    }
    
    // Add background
    accumulated_color += T * background;
    
    output_image[pixel_idx] = float4(accumulated_color, 1.0f - T);
}

// ============================================================================
// BACKWARD PASS KERNELS
// ============================================================================

// Kernel: Compute gradients for Gaussians
kernel void rasterize_gaussians_backward(
    device const ProjectedGaussian* projected [[buffer(0)]],
    device const float4* output_image [[buffer(1)]],
    device const float4* grad_output [[buffer(2)]],     // dL/d(output)
    device float4* grad_position [[buffer(3)]],          // dL/d(position, opacity_logit)
    device float4* grad_color [[buffer(4)]],             // dL/d(color)
    device float4* grad_scale [[buffer(5)]],             // dL/d(log_scale)
    device float4* grad_rotation [[buffer(6)]],          // dL/d(quaternion)
    constant uint& num_gaussians [[buffer(7)]],
    constant int2& image_size [[buffer(8)]],
    uint id [[thread_position_in_grid]]
) {
    if (id >= num_gaussians) return;
    
    ProjectedGaussian g = projected[id];
    if (g.radius < 0) {
        grad_position[id] = float4(0.0f);
        grad_color[id] = float4(0.0f);
        grad_scale[id] = float4(0.0f);
        grad_rotation[id] = float4(0.0f);
        return;
    }
    
    float4 d_pos = float4(0.0f);
    float4 d_color = float4(0.0f);
    float d_opacity_logit = 0.0f;
    
    // Accumulate gradients from all affected pixels
    int min_x = max(0, int(g.xy.x) - g.radius);
    int max_x = min(image_size.x - 1, int(g.xy.x) + g.radius);
    int min_y = max(0, int(g.xy.y) - g.radius);
    int max_y = min(image_size.y - 1, int(g.xy.y) + g.radius);
    
    for (int py = min_y; py <= max_y; ++py) {
        for (int px = min_x; px <= max_x; ++px) {
            float2 pixel_center = float2(px, py) + 0.5f;
            float2 d = pixel_center - g.xy;
            
            // Compute Gaussian weight
            float power = -0.5f * (g.conic.x * d.x * d.x + 2.0f * g.conic.y * d.x * d.y + g.conic.z * d.y * d.y);
            if (power > 0.0f) continue;
            
            float gaussian = exp(power);
            float alpha = min(0.99f, g.opacity * gaussian);
            if (alpha < 1.0f / 255.0f) continue;
            
            uint pixel_idx = py * image_size.x + px;
            float4 dL_dout = grad_output[pixel_idx];
            
            // Simplified gradient computation
            // dL/d(color) += dL/d(output) * T * alpha
            // This is simplified - full version needs transmittance tracking
            d_color.xyz += dL_dout.xyz * alpha;
            
            // dL/d(opacity_logit) via chain rule through sigmoid
            float sigmoid_grad = g.opacity * (1.0f - g.opacity);
            d_opacity_logit += dot(dL_dout.xyz, g.color) * gaussian * sigmoid_grad;
        }
    }
    
    grad_position[id] = float4(d_pos.xyz, d_opacity_logit);
    grad_color[id] = d_color;
    grad_scale[id] = float4(0.0f);     // Simplified - would need full cov2D backward
    grad_rotation[id] = float4(0.0f);  // Simplified - would need full rotation backward
}

// ============================================================================
// ADAM OPTIMIZER KERNEL
// ============================================================================

struct AdamParams {
    float lr;
    float beta1;
    float beta2;
    float epsilon;
    int step;
};

kernel void adam_step(
    device float* params [[buffer(0)]],
    device const float* grads [[buffer(1)]],
    device float* m [[buffer(2)]],          // First moment
    device float* v [[buffer(3)]],          // Second moment
    constant AdamParams& adam [[buffer(4)]],
    constant uint& num_params [[buffer(5)]],
    uint id [[thread_position_in_grid]]
) {
    if (id >= num_params) return;
    
    float g = grads[id];
    float m_old = m[id];
    float v_old = v[id];
    
    // Update moments
    float m_new = adam.beta1 * m_old + (1.0f - adam.beta1) * g;
    float v_new = adam.beta2 * v_old + (1.0f - adam.beta2) * g * g;
    
    // Bias correction
    float m_hat = m_new / (1.0f - pow(adam.beta1, float(adam.step)));
    float v_hat = v_new / (1.0f - pow(adam.beta2, float(adam.step)));
    
    // Update parameter
    params[id] -= adam.lr * m_hat / (sqrt(v_hat) + adam.epsilon);
    
    // Store updated moments
    m[id] = m_new;
    v[id] = v_new;
}

// ============================================================================
// LOSS COMPUTATION KERNELS
// ============================================================================

// L1 loss per pixel
kernel void compute_l1_loss(
    device const float4* rendered [[buffer(0)]],
    device const float4* target [[buffer(1)]],
    device float* loss [[buffer(2)]],
    constant uint& num_pixels [[buffer(3)]],
    uint id [[thread_position_in_grid]]
) {
    if (id >= num_pixels) return;
    
    float3 diff = abs(rendered[id].xyz - target[id].xyz);
    loss[id] = (diff.x + diff.y + diff.z) / 3.0f;
}

// Gradient of L1 loss
kernel void compute_l1_grad(
    device const float4* rendered [[buffer(0)]],
    device const float4* target [[buffer(1)]],
    device float4* grad [[buffer(2)]],
    constant uint& num_pixels [[buffer(3)]],
    uint id [[thread_position_in_grid]]
) {
    if (id >= num_pixels) return;
    
    float3 diff = rendered[id].xyz - target[id].xyz;
    float3 g = sign(diff) / 3.0f;
    grad[id] = float4(g, 0.0f);
}
