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
        q = float4(1.0f, 0.0f, 0.0f, 0.0f);
    }
    
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
kernel void rgb_to_sh_dc(
    device PackedGaussian* gaussians [[buffer(0)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float3 rgb = gaussians[id].color.xyz;
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
    gaussians[id].color.xyz = sh_dc * SH_C0 + 0.5f;
}

// Kernel: Convert linear opacity to logit space
kernel void opacity_to_logit(
    device PackedGaussian* gaussians [[buffer(0)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float opacity = clamp(gaussians[id].position.w, 0.001f, 0.999f);
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
    gaussians[id].position.w = 1.0f / (1.0f + exp(-logit));
}

// Kernel: Convert linear scale to log space
kernel void scale_to_log(
    device PackedGaussian* gaussians [[buffer(0)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float3 scale = max(gaussians[id].scale.xyz, float3(1e-7f));
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
kernel void compute_distances(
    device const PackedGaussian* gaussians [[buffer(0)]],
    device float2* distances [[buffer(1)]],
    constant CameraParams& camera [[buffer(2)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float3 pos = gaussians[id].position.xyz;
    float3 diff = pos - camera.position;
    float dist = dot(diff, diff);
    
    distances[id] = float2(dist, float(id));
}

// Kernel: Compute covariance matrices from scale and rotation
kernel void compute_covariances(
    device const PackedGaussian* gaussians [[buffer(0)]],
    device float* covariances [[buffer(1)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    float3 s = gaussians[id].scale.xyz;
    float4 q = gaussians[id].rotation;
    float w = q.x, x = q.y, y = q.z, z = q.w;
    
    float3x3 R = float3x3(
        float3(1.0f - 2.0f*(y*y + z*z), 2.0f*(x*y - w*z), 2.0f*(x*z + w*y)),
        float3(2.0f*(x*y + w*z), 1.0f - 2.0f*(x*x + z*z), 2.0f*(y*z - w*x)),
        float3(2.0f*(x*z - w*y), 2.0f*(y*z + w*x), 1.0f - 2.0f*(x*x + y*y))
    );
    
    float3x3 S = float3x3(
        float3(s.x, 0.0f, 0.0f),
        float3(0.0f, s.y, 0.0f),
        float3(0.0f, 0.0f, s.z)
    );
    
    float3x3 RS = R * S;
    float3x3 cov = RS * transpose(RS);
    
    uint base = id * 6;
    covariances[base + 0] = cov[0][0];
    covariances[base + 1] = cov[0][1];
    covariances[base + 2] = cov[0][2];
    covariances[base + 3] = cov[1][1];
    covariances[base + 4] = cov[1][2];
    covariances[base + 5] = cov[2][2];
}

// Kernel: Combined processing (normalize quaternions + all conversions)
kernel void process_all(
    device PackedGaussian* gaussians [[buffer(0)]],
    constant uint& flags [[buffer(1)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size) return;
    
    if (flags & 0x1) {
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
        float3 rgb = gaussians[id].color.xyz;
        gaussians[id].color.xyz = (rgb - 0.5f) / SH_C0;
    }
    
    if (flags & 0x4) {
        float opacity = clamp(gaussians[id].position.w, 0.001f, 0.999f);
        gaussians[id].position.w = log(opacity / (1.0f - opacity));
    }
    
    if (flags & 0x8) {
        float3 scale = max(gaussians[id].scale.xyz, float3(1e-7f));
        gaussians[id].scale.xyz = log(scale);
    }
}

// ============================================================================
// ENHANCED CONVERSION KERNELS
// These kernels accelerate the EnhancedConverter per-point loop and k-NN
// distance computation on GPU. The per-point conversion is embarrassingly
// parallel; the k-NN is O(n^2) brute-force but parallel, faster than CPU
// spatial hash for small clouds (n < ~10K) on Apple Silicon.
// ============================================================================

struct EnhancedConvertConfig {
    float scale_factor;
    float min_scale;
    float max_scale;
    float normal_scale_ratio;
    float default_opacity;
    float position_scale;
    int   convert_coordinate_system;
    int   use_surface_alignment;
};

void quaternion_from_normal(float3 n, thread float4& q) {
    float len = length(n);
    if (len > 0.0f) {
        n /= len;
    } else {
        n = float3(0.0f, 0.0f, 1.0f);
    }

    float dot = n.z;

    if (dot > 0.9999f) {
        q = float4(1.0f, 0.0f, 0.0f, 0.0f);
    } else if (dot < -0.9999f) {
        q = float4(0.0f, 1.0f, 0.0f, 0.0f);
    } else {
        float3 axis = float3(-n.y, n.x, 0.0f);
        float s = sqrt((1.0f + dot) * 2.0f);
        float inv_s = 1.0f / s;
        q = float4(s * 0.5f, axis.x * inv_s, axis.y * inv_s, 0.0f);
    }

    float qlen = length(q);
    if (qlen > 0.0f) q /= qlen;
}

kernel void enhanced_convert_points(
    device const float* positions       [[buffer(0)]],
    device const float* normals         [[buffer(1)]],
    device const float* colors          [[buffer(2)]],
    device const float* adaptive_scales [[buffer(3)]],
    device PackedGaussian* output       [[buffer(4)]],
    constant EnhancedConvertConfig& cfg [[buffer(5)]],
    constant uint& num_points           [[buffer(6)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size || id >= num_points) return;

    float3 pos = float3(positions[id * 3 + 0],
                        positions[id * 3 + 1],
                        positions[id * 3 + 2]);

    float3 out_pos;
    if (cfg.convert_coordinate_system != 0) {
        out_pos = float3(pos.x, -pos.z, pos.y) * cfg.position_scale;
    } else {
        out_pos = pos * cfg.position_scale;
    }

    float3 color;
    if (colors != nullptr) {
        color = float3(colors[id * 3 + 0], colors[id * 3 + 1], colors[id * 3 + 2]);
    } else {
        color = float3(0.5f, 0.5f, 0.5f);
    }
    float3 sh_dc = (color - 0.5f) / SH_C0;

    float opacity_logit = log(cfg.default_opacity / (1.0f - cfg.default_opacity));

    float base_scale = adaptive_scales[id] * cfg.scale_factor;
    base_scale = clamp(base_scale, cfg.min_scale, cfg.max_scale);

    float3 scale_log;
    float4 quat;

    if (cfg.use_surface_alignment != 0 && normals != nullptr) {
        float3 n = float3(normals[id * 3 + 0],
                          normals[id * 3 + 1],
                          normals[id * 3 + 2]);
        if (cfg.convert_coordinate_system != 0) {
            n = float3(n.x, -n.z, n.y);
        }

        float tangent_scale = base_scale;
        float normal_scale = base_scale * cfg.normal_scale_ratio;
        scale_log = float3(log(tangent_scale), log(tangent_scale), log(normal_scale));
        quaternion_from_normal(n, quat);
    } else {
        float ls = log(base_scale);
        scale_log = float3(ls, ls, ls);
        quat = float4(1.0f, 0.0f, 0.0f, 0.0f);
    }

    PackedGaussian g;
    g.position = float4(out_pos, opacity_logit);
    g.color = float4(sh_dc, 0.0f);
    g.scale = float4(scale_log, 0.0f);
    g.rotation = quat;
    output[id] = g;
}

kernel void compute_knn_distances(
    device const float* positions      [[buffer(0)]],
    device float* output_distances     [[buffer(1)]],
    constant uint& num_points          [[buffer(2)]],
    constant int& k_neighbors          [[buffer(3)]],
    uint id [[thread_position_in_grid]],
    uint grid_size [[threads_per_grid]]
) {
    if (id >= grid_size || id >= num_points) return;

    float3 p = float3(positions[id * 3 + 0],
                      positions[id * 3 + 1],
                      positions[id * 3 + 2]);

    const int MAX_K = 32;
    float best[32];
    for (int i = 0; i < MAX_K; ++i) best[i] = 1e30f;

    int filled = 0;
    for (uint j = 0; j < num_points; ++j) {
        if (j == id) continue;
        float3 q = float3(positions[j * 3 + 0],
                          positions[j * 3 + 1],
                          positions[j * 3 + 2]);
        float d = distance(p, q);

        if (filled < MAX_K) {
            int pos = filled;
            while (pos > 0 && best[pos - 1] > d) {
                best[pos] = best[pos - 1];
                --pos;
            }
            best[pos] = d;
            ++filled;
        } else if (d < best[MAX_K - 1]) {
            int pos = MAX_K - 1;
            while (pos > 0 && best[pos - 1] > d) {
                best[pos] = best[pos - 1];
                --pos;
            }
            best[pos] = d;
        }
    }

    int actual_k = min(k_neighbors, filled);
    float sum = 0.0f;
    for (int i = 0; i < actual_k; ++i) {
        sum += best[i];
    }
    output_distances[id] = (actual_k > 0) ? (sum / float(actual_k)) : 0.0f;
}
