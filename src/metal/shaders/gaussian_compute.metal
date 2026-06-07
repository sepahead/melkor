#include <metal_stdlib>
using namespace metal;

// Packed Gaussian structure matching C++ PackedGaussian
struct PackedGaussian {
    float4 position;   // x, y, z, opacity
    float4 color;      // f_dc_0, f_dc_1, f_dc_2, padding
    float4 scale;      // scale_0, scale_1, scale_2, padding
    float4 rotation;   // rot_0, rot_1, rot_2, rot_3 (quaternion)
};

// Transform parameters
struct TransformParams {
    float4x4 matrix;
};

// Scale parameters
struct ScaleParams {
    float scale;
};

// Camera parameters for sorting
struct CameraParams {
    float3 position;
};

// Constants
constant float SH_C0 = 0.28209479177387814f;
// constant float PI = 3.14159265358979323846f;  // Reserved for future use

// Kernel: Transform coordinates using a 4x4 matrix
kernel void transform_coordinates(
    device PackedGaussian* gaussians [[buffer(0)]],
    constant TransformParams& params [[buffer(1)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float4 pos = float4(gaussians[id].position.xyz, 1.0f);
    float4 transformed = params.matrix * pos;
    gaussians[id].position.xyz = transformed.xyz;
}

// Kernel: Normalize quaternions
kernel void normalize_quaternions(
    device PackedGaussian* gaussians [[buffer(0)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float4 q = gaussians[id].rotation;
    float len = length(q);
    
    if (len > 0.0f) {
        q /= len;
    } else {
        q = float4(1.0f, 0.0f, 0.0f, 0.0f);  // Identity quaternion
    }
    
    // Ensure w (rot_0) is positive for canonical form
    if (q.x < 0.0f) {
        q = -q;
    }
    
    gaussians[id].rotation = q;
}

// Kernel: Scale positions
kernel void scale_positions(
    device PackedGaussian* gaussians [[buffer(0)]],
    constant ScaleParams& params [[buffer(1)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    gaussians[id].position.xyz *= params.scale;
}

// Kernel: Convert RGB to Spherical Harmonics DC coefficients
// Input color should be in RGB [0,1] range
// Output is SH DC coefficient space
kernel void rgb_to_sh_dc(
    device PackedGaussian* gaussians [[buffer(0)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float3 rgb = gaussians[id].color.xyz;
    
    // Convert RGB [0,1] to SH DC: (rgb - 0.5) / C0
    gaussians[id].color.xyz = (rgb - 0.5f) / SH_C0;
}

// Kernel: Convert SH DC to RGB
kernel void sh_dc_to_rgb(
    device PackedGaussian* gaussians [[buffer(0)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float3 sh_dc = gaussians[id].color.xyz;
    
    // Convert SH DC to RGB [0,1]: sh_dc * C0 + 0.5
    gaussians[id].color.xyz = sh_dc * SH_C0 + 0.5f;
}

// Kernel: Convert linear opacity to logit space
kernel void opacity_to_logit(
    device PackedGaussian* gaussians [[buffer(0)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float opacity = gaussians[id].position.w;
    
    // Clamp to avoid infinity
    opacity = clamp(opacity, 0.001f, 0.999f);
    
    // Logit: log(x / (1 - x))
    gaussians[id].position.w = log(opacity / (1.0f - opacity));
}

// Kernel: Convert logit opacity to linear
kernel void logit_to_opacity(
    device PackedGaussian* gaussians [[buffer(0)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float logit = gaussians[id].position.w;
    
    // Sigmoid: 1 / (1 + exp(-x))
    gaussians[id].position.w = 1.0f / (1.0f + exp(-logit));
}

// Kernel: Convert linear scale to log space
kernel void scale_to_log(
    device PackedGaussian* gaussians [[buffer(0)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float3 scale = gaussians[id].scale.xyz;
    
    // Clamp to avoid log(0)
    scale = max(scale, float3(1e-7f));
    
    gaussians[id].scale.xyz = log(scale);
}

// Kernel: Convert log scale to linear
kernel void log_to_scale(
    device PackedGaussian* gaussians [[buffer(0)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float3 log_scale = gaussians[id].scale.xyz;
    gaussians[id].scale.xyz = exp(log_scale);
}

// Kernel: Compute distance from camera (for sorting)
// Output buffer stores (distance, original_index)
kernel void compute_distances(
    device const PackedGaussian* gaussians [[buffer(0)]],
    device float2* distances [[buffer(1)]],  // (distance, index)
    constant CameraParams& camera [[buffer(2)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float3 pos = gaussians[id].position.xyz;
    float3 diff = pos - camera.position;
    float dist = dot(diff, diff);  // Squared distance for efficiency
    
    distances[id] = float2(dist, float(id));
}

// Kernel: Compute covariance matrices from scale and rotation
// Output: 6 floats per splat (upper triangular of symmetric 3x3 matrix)
kernel void compute_covariances(
    device const PackedGaussian* gaussians [[buffer(0)]],
    device float* covariances [[buffer(1)]],  // 6 floats per splat
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    // Get scale (in linear space, assuming already converted from log)
    float3 s = gaussians[id].scale.xyz;
    
    // Get quaternion (w, x, y, z) = (rot_0, rot_1, rot_2, rot_3)
    float4 q = gaussians[id].rotation;
    float w = q.x, x = q.y, y = q.z, z = q.w;
    
    // Build rotation matrix R from quaternion
    float3x3 R = float3x3(
        float3(1.0f - 2.0f*(y*y + z*z), 2.0f*(x*y - w*z), 2.0f*(x*z + w*y)),
        float3(2.0f*(x*y + w*z), 1.0f - 2.0f*(x*x + z*z), 2.0f*(y*z - w*x)),
        float3(2.0f*(x*z - w*y), 2.0f*(y*z + w*x), 1.0f - 2.0f*(x*x + y*y))
    );
    
    // Scale matrix S
    float3x3 S = float3x3(
        float3(s.x, 0.0f, 0.0f),
        float3(0.0f, s.y, 0.0f),
        float3(0.0f, 0.0f, s.z)
    );
    
    // Covariance = R * S * S^T * R^T = R * S^2 * R^T
    float3x3 RS = R * S;
    float3x3 cov = RS * transpose(RS);
    
    // Store upper triangular (6 elements)
    uint base = id * 6;
    covariances[base + 0] = cov[0][0];  // xx
    covariances[base + 1] = cov[0][1];  // xy
    covariances[base + 2] = cov[0][2];  // xz
    covariances[base + 3] = cov[1][1];  // yy
    covariances[base + 4] = cov[1][2];  // yz
    covariances[base + 5] = cov[2][2];  // zz
}

// Kernel: Combined processing (normalize quaternions + all conversions)
kernel void process_all(
    device PackedGaussian* gaussians [[buffer(0)]],
    constant uint& flags [[buffer(1)]],  // Bit flags for operations
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    // Flag bits:
    // 0: normalize quaternions
    // 1: rgb to sh dc
    // 2: opacity to logit
    // 3: scale to log
    
    if (flags & 0x1) {
        // Normalize quaternion
        float4 q = gaussians[id].rotation;
        float len = length(q);
        if (len > 0.0f) {
            q /= len;
        } else {
            q = float4(1.0f, 0.0f, 0.0f, 0.0f);
        }
        if (q.x < 0.0f) q = -q;
        gaussians[id].rotation = q;
    }
    
    if (flags & 0x2) {
        // RGB to SH DC
        float3 rgb = gaussians[id].color.xyz;
        gaussians[id].color.xyz = (rgb - 0.5f) / SH_C0;
    }
    
    if (flags & 0x4) {
        // Opacity to logit
        float opacity = clamp(gaussians[id].position.w, 0.001f, 0.999f);
        gaussians[id].position.w = log(opacity / (1.0f - opacity));
    }
    
    if (flags & 0x8) {
        // Scale to log
        float3 scale = max(gaussians[id].scale.xyz, float3(1e-7f));
        gaussians[id].scale.xyz = log(scale);
    }
}
