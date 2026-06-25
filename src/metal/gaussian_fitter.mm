#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "melkor/gaussian_fitter.hpp"
#include "melkor/enhanced_converter.hpp"
#include <cmath>
#include <chrono>
#include <iostream>

// For GLB loading — guard against definitions from the CMake target
#ifndef TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#endif
#ifndef TINYGLTF_NO_EXTERNAL_IMAGE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#endif
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
// EWA (Elliptical Weighted Average) splatting helpers.
//
// These port the gsplat/gsplat-metal EWA projection and its VJPs to plain C++.
// All 3x3 matrices are row-major: M[r*3+c] = element at (row r, col c).
// Reference: gsplat mathematical supplement (arXiv:2312.02121) and the
// gsplat_metal.metal kernels in tools/OpenSplat/rasterizer/gsplat-metal/.
// ============================================================================

struct Mat3 {
    float m[9];
    float& operator()(int r, int c) { return m[r * 3 + c]; }
    float operator()(int r, int c) const { return m[r * 3 + c]; }
};

static Mat3 mat3_identity() { return {{1,0,0, 0,1,0, 0,0,1}}; }

static Mat3 mat3_mul(const Mat3& A, const Mat3& B) {
    Mat3 C;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = 0;
            for (int k = 0; k < 3; ++k) s += A(r, k) * B(k, c);
            C(r, c) = s;
        }
    return C;
}

static Mat3 mat3_transpose(const Mat3& A) {
    Mat3 T;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) T(r, c) = A(c, r);
    return T;
}

// Quaternion (w, x, y, z) to 3x3 rotation matrix (row-major).
// PackedGaussian.rotation stores (w, x, y, z) as rotation[0..3].
static Mat3 quat_to_rotmat(float w, float x, float y, float z) {
    float len = std::sqrt(w*w + x*x + y*y + z*z);
    if (len > 0.0f) { float inv = 1.0f / len; w *= inv; x *= inv; y *= inv; z *= inv; }
    else { w = 1.0f; x = y = z = 0.0f; }
    Mat3 R;
    R(0,0) = 1 - 2*(y*y + z*z); R(0,1) = 2*(x*y - w*z);     R(0,2) = 2*(x*z + w*y);
    R(1,0) = 2*(x*y + w*z);     R(1,1) = 1 - 2*(x*x + z*z); R(1,2) = 2*(y*z - w*x);
    R(2,0) = 2*(x*z - w*y);     R(2,1) = 2*(y*z + w*x);     R(2,2) = 1 - 2*(x*x + y*y);
    return R;
}

// Compute 3D covariance (upper triangular, 6 elements) from scale and quaternion.
// cov3d = R * S * S^T * R^T = M * M^T where M = R * S.
// Output: cov3d[0..5] = (xx, xy, xz, yy, yz, zz).
static void scale_rot_to_cov3d(float sx, float sy, float sz,
                               float qw, float qx, float qy, float qz,
                               float cov3d[6]) {
    Mat3 R = quat_to_rotmat(qw, qx, qy, qz);
    Mat3 S = mat3_identity();
    S(0,0) = sx; S(1,1) = sy; S(2,2) = sz;
    Mat3 M = mat3_mul(R, S);
    Mat3 cov = mat3_mul(M, mat3_transpose(M));
    cov3d[0] = cov(0,0); cov3d[1] = cov(0,1); cov3d[2] = cov(0,2);
    cov3d[3] = cov(1,1); cov3d[4] = cov(1,2); cov3d[5] = cov(2,2);
}

// Project 3D covariance to 2D via EWA approximation.
// Returns cov2d upper triangular (xx, xy, yy) and view-space position t[3].
// view_matrix is row-major 4x4. focal_x, focal_y are pixel-space focal lengths.
static void project_cov3d_ewa(const float mean3d[3],
                              const float cov3d[6],
                              const float* view_matrix, // row-major 4x4
                              float focal_x, float focal_y,
                              float tan_fovx, float tan_fovy,
                              float t_out[3],
                              float cov2d_out[3]) {
    // Extract view rotation (upper-left 3x3 of view_matrix, row-major).
    Mat3 W;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            W(r, c) = view_matrix[r * 4 + c];
    float p[3] = {view_matrix[3], view_matrix[7], view_matrix[11]};

    // View-space position: t = W * mean3d + p
    float t[3];
    for (int r = 0; r < 3; ++r) {
        t[r] = p[r];
        for (int c = 0; c < 3; ++c) t[r] += W(r, c) * mean3d[c];
    }

    // Clamp to FOV limits (prevents extreme covariances at edges).
    float lim_x = 1.3f * tan_fovx;
    float lim_y = 1.3f * tan_fovy;
    float txz = t[0] / t[2];
    float tyz = t[1] / t[2];
    t[0] = std::min(lim_x, std::max(-lim_x, txz)) * t[2];
    t[1] = std::min(lim_y, std::max(-lim_y, tyz)) * t[2];

    float rz = 1.0f / t[2];
    float rz2 = rz * rz;

    // Jacobian of perspective projection (row-major).
    Mat3 J = mat3_identity();
    J(0,0) = focal_x * rz;  J(0,1) = 0;            J(0,2) = -focal_x * t[0] * rz2;
    J(1,0) = 0;             J(1,1) = focal_y * rz; J(1,2) = -focal_y * t[1] * rz2;
    J(2,0) = 0;             J(2,1) = 0;            J(2,2) = 0;

    // T = J * W
    Mat3 T = mat3_mul(J, W);

    // Reconstruct full 3x3 cov3d from upper triangular.
    Mat3 V;
    V(0,0) = cov3d[0]; V(0,1) = cov3d[1]; V(0,2) = cov3d[2];
    V(1,0) = cov3d[1]; V(1,1) = cov3d[3]; V(1,2) = cov3d[4];
    V(2,0) = cov3d[2]; V(1,2) = cov3d[4]; V(2,2) = cov3d[5];
    V(2,1) = cov3d[4];

    // cov2d = T * V * T^T
    Mat3 cov = mat3_mul(mat3_mul(T, V), mat3_transpose(T));

    // Add 0.3 blur for anti-aliasing (matches gsplat).
    cov2d_out[0] = cov(0,0) + 0.3f;
    cov2d_out[1] = cov(0,1);
    cov2d_out[2] = cov(1,1) + 0.3f;

    t_out[0] = t[0]; t_out[1] = t[1]; t_out[2] = t[2];
}

// Compute conic (inverse 2D covariance) and bounding radius from cov2d.
// Returns false if covariance is degenerate (zero determinant).
static bool compute_cov2d_bounds(const float cov2d[3],
                                 float conic[3], float& radius) {
    float det = cov2d[0] * cov2d[2] - cov2d[1] * cov2d[1];
    if (det == 0.0f) return false;
    float inv_det = 1.0f / det;
    // Inverse of 2x2 symmetric matrix [[a,b],[b,c]] = [[c,-b],[-b,a]]/det.
    conic[0] = cov2d[2] * inv_det;
    conic[1] = -cov2d[1] * inv_det;
    conic[2] = cov2d[0] * inv_det;

    // Radius from eigenvalues: 3-sigma extent.
    float mid = 0.5f * (cov2d[0] + cov2d[2]);
    float disc = std::sqrt(std::max(0.1f, mid * mid - det));
    float lambda1 = mid + disc;
    float lambda2 = mid - disc;
    radius = std::ceil(3.0f * std::sqrt(std::max(lambda1, lambda2)));
    return true;
}

// ============================================================================
// VJP (vector-Jacobian product) functions for the backward pass.
// ============================================================================

// VJP of screen-space projection: dL/dmean3d from dL/dscreen_xy.
// projmat is row-major 4x4. screen = ndc2pix(projmat * pos / w).
static void project_pix_vjp(const float* projmat, const float p[3],
                            int img_w, int img_h,
                            float v_screen_x, float v_screen_y,
                            float v_mean3d[3]) {
    float p_hom[4];
    for (int i = 0; i < 4; ++i) {
        p_hom[i] = projmat[i*4+0]*p[0] + projmat[i*4+1]*p[1] + projmat[i*4+2]*p[2] + projmat[i*4+3];
    }
    float rw = 1.0f / (p_hom[3] + 1e-6f);

    // Chain: screen → ndc → p_hom → world.
    // screen = 0.5 * img_size * (ndc + 1), so d(ndc)/d(screen) = 2/img_size,
    // and v_ndc = v_screen * d(screen)/d(ndc) = v_screen * 0.5 * img_size.
    float v_ndc_x = 0.5f * img_w * v_screen_x;
    float v_ndc_y = 0.5f * img_h * v_screen_y;
    // ndc = p_hom.xy / p_hom.w. The w-gradient must include p_hom.xy factors:
    // v_proj.w = -(v_ndc.x * p_hom.x + v_ndc.y * p_hom.y) * rw².
    float v_proj[4] = {
        v_ndc_x * rw, v_ndc_y * rw, 0.0f,
        -(v_ndc_x * p_hom[0] + v_ndc_y * p_hom[1]) * rw * rw
    };

    // d(world)/d(proj) = projmat^T (upper 3 rows, 3 cols).
    v_mean3d[0] = projmat[0]*v_proj[0] + projmat[4]*v_proj[1] + projmat[8]*v_proj[2] + projmat[12]*v_proj[3];
    v_mean3d[1] = projmat[1]*v_proj[0] + projmat[5]*v_proj[1] + projmat[9]*v_proj[2] + projmat[13]*v_proj[3];
    v_mean3d[2] = projmat[2]*v_proj[0] + projmat[6]*v_proj[1] + projmat[10]*v_proj[2] + projmat[14]*v_proj[3];
}

// VJP of conic = inverse(cov2d): dL/dcov2d from dL/dconic.
// For conic = inv(Sigma), dL/dSigma = -conic * dL/dconic * conic (2x2).
static void cov2d_to_conic_vjp(const float conic[3], const float v_conic[3],
                                float v_cov2d[3]) {
    // 2x2 symmetric matrices: conic = [[c0, c1], [c1, c2]], v_conic = [[v0, v1], [v1, v2]]
    // v_Sigma = -X * G * X where X = conic, G = v_conic.
    float a = -(conic[0]*v_conic[0]*conic[0] + conic[0]*v_conic[1]*conic[1] + conic[1]*v_conic[0]*conic[1] + conic[1]*v_conic[1]*conic[2]);
    float b = -(conic[0]*v_conic[0]*conic[1] + conic[0]*v_conic[1]*conic[2] + conic[1]*v_conic[0]*conic[2] + conic[1]*v_conic[1]*conic[2]);
    // Actually let me just do the 2x2 matrix multiply properly.
    // X = [[conic[0], conic[1]], [conic[1], conic[2]]]
    // G = [[v_conic[0], v_conic[1]], [v_conic[1], v_conic[2]]]
    // XG = [[c0*v0+c1*v1, c0*v1+c1*v2], [c1*v0+c2*v1, c1*v1+c2*v2]]
    // XGX = [[(XG00)*c0+(XG01)*c1, (XG00)*c1+(XG01)*c2], [(XG10)*c0+(XG11)*c1, (XG10)*c1+(XG11)*c2]]
    float xg00 = conic[0]*v_conic[0] + conic[1]*v_conic[1];
    float xg01 = conic[0]*v_conic[1] + conic[1]*v_conic[2];
    float xg10 = conic[1]*v_conic[0] + conic[2]*v_conic[1];
    float xg11 = conic[1]*v_conic[1] + conic[2]*v_conic[2];
    v_cov2d[0] = -(xg00 * conic[0] + xg01 * conic[1]);
    v_cov2d[1] = -(xg00 * conic[1] + xg01 * conic[2] + xg10 * conic[0] + xg11 * conic[1]);
    v_cov2d[2] = -(xg10 * conic[1] + xg11 * conic[2]);
}

// VJP of EWA projection: dL/dcov3d and dL/dmean3d from dL/dcov2d.
// cov2d = T * V * T^T where T = J * W, V = cov3d (symmetric).
static void project_cov3d_ewa_vjp(const float mean3d[3],
                                  const float cov3d[6],
                                  const float* view_matrix,
                                  float focal_x, float focal_y,
                                  float tan_fovx, float tan_fovy,
                                  const float v_cov2d[3],
                                  float v_mean3d[3],
                                  float v_cov3d[6]) {
    // Reconstruct T = J * W (same as forward).
    Mat3 W;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            W(r, c) = view_matrix[r * 4 + c];
    float p[3] = {view_matrix[3], view_matrix[7], view_matrix[11]};
    float t[3];
    for (int r = 0; r < 3; ++r) {
        t[r] = p[r];
        for (int c = 0; c < 3; ++c) t[r] += W(r, c) * mean3d[c];
    }
    float lim_x = 1.3f * tan_fovx;
    float lim_y = 1.3f * tan_fovy;
    t[0] = std::min(lim_x, std::max(-lim_x, t[0] / t[2])) * t[2];
    t[1] = std::min(lim_y, std::max(-lim_y, t[1] / t[2])) * t[2];
    float rz = 1.0f / t[2];
    float rz2 = rz * rz;

    Mat3 J = mat3_identity();
    J(0,0) = focal_x * rz;  J(0,2) = -focal_x * t[0] * rz2;
    J(1,1) = focal_y * rz;  J(1,2) = -focal_y * t[1] * rz2;
    J(2,2) = 0;

    Mat3 T = mat3_mul(J, W);
    Mat3 Tt = mat3_transpose(T);

    Mat3 V;
    V(0,0) = cov3d[0]; V(0,1) = cov3d[1]; V(0,2) = cov3d[2];
    V(1,0) = cov3d[1]; V(1,1) = cov3d[3]; V(1,2) = cov3d[4];
    V(2,0) = cov3d[2]; V(2,1) = cov3d[4]; V(2,2) = cov3d[5];
    Mat3 Vt = mat3_transpose(V);

    // v_cov (3x3, symmetric) from v_cov2d (upper triangular).
    Mat3 v_cov;
    v_cov(0,0) = v_cov2d[0]; v_cov(0,1) = 0.5f * v_cov2d[1]; v_cov(0,2) = 0;
    v_cov(1,0) = 0.5f * v_cov2d[1]; v_cov(1,1) = v_cov2d[2]; v_cov(1,2) = 0;
    v_cov(2,0) = 0; v_cov(2,1) = 0; v_cov(2,2) = 0;

    // d/dV = T^T * v_cov * T
    Mat3 v_V = mat3_mul(mat3_mul(Tt, v_cov), T);

    // d/dT = v_cov * T * V^T + v_cov^T * T * V
    Mat3 v_T = mat3_mul(mat3_mul(v_cov, T), Vt);
    Mat3 v_T2 = mat3_mul(mat3_mul(mat3_transpose(v_cov), T), V);
    for (int i = 0; i < 9; ++i) v_T.m[i] += v_T2.m[i];

    // v_cov3d from v_V (extract upper triangular, off-diagonals sum).
    v_cov3d[0] = v_V(0,0);
    v_cov3d[1] = v_V(0,1) + v_V(1,0);
    v_cov3d[2] = v_V(0,2) + v_V(2,0);
    v_cov3d[3] = v_V(1,1);
    v_cov3d[4] = v_V(1,2) + v_V(2,1);
    v_cov3d[5] = v_V(2,2);

    // v_mean3d from v_T through J and W.
    // v_J = v_T * W^T
    Mat3 Wt = mat3_transpose(W);
    Mat3 v_J = mat3_mul(v_T, Wt);

    // v_t from v_J (J depends on t).
    // Metal column-major: v_J[col][row] = our v_J(row, col).
    // gsplat: v_t[0] = -fx*rz2*v_J[2][0] = -fx*rz2*v_J(0,2) in row-major.
    float rz3 = rz2 * rz;
    float v_t[3];
    v_t[0] = -focal_x * rz2 * v_J(0, 2);
    v_t[1] = -focal_y * rz2 * v_J(1, 2);
    v_t[2] = -focal_x * rz2 * v_J(0, 0) + 2.0f * focal_x * t[0] * rz3 * v_J(0, 2)
            - focal_y * rz2 * v_J(1, 1) + 2.0f * focal_y * t[1] * rz3 * v_J(1, 2);

    // v_mean3d = W^T * v_t
    for (int c = 0; c < 3; ++c) {
        v_mean3d[c] = 0;
        for (int r = 0; r < 3; ++r) v_mean3d[c] += W(r, c) * v_t[r];
    }
}

// VJP of quaternion to rotation matrix: dL/dquat from dL/dR.
// quat = (w, x, y, z). R is row-major 3x3.
// Ported from gsplat_metal.metal quat_to_rotmat_vjp, converting column-major
// Metal indexing (v_R[col][row]) to row-major (v_R(row, col)).
// Includes the normalization chain: the forward normalizes quat before
// computing R, so the gradient w.r.t. the original (unnormalized) quat
// requires projecting onto the tangent plane: v_quat -= (q·v_quat) * q / |q|².
static void quat_to_rotmat_vjp(float qw, float qx, float qy, float qz,
                               const Mat3& v_R,
                               float v_quat[4]) {
    float qlen_sq = qw*qw + qx*qx + qy*qy + qz*qz;
    float len = std::sqrt(qlen_sq);
    float nw = qw, nx = qx, ny = qy, nz = qz;
    if (len > 0.0f) { float inv = 1.0f / len; nw *= inv; nx *= inv; ny *= inv; nz *= inv; }
    else { nw = 1.0f; nx = ny = nz = 0.0f; }

    // In gsplat-metal, v_R is column-major: v_R[col][row].
    // In our row-major: v_R(row, col). So gsplat's v_R[c][r] = our v_R(r, c).
    // The antisymmetric terms (differences) swap sign under transpose.
    v_quat[0] = 2.0f * (nx*(v_R(2,1) - v_R(1,2)) + ny*(v_R(0,2) - v_R(2,0)) + nz*(v_R(1,0) - v_R(0,1)));
    v_quat[1] = 2.0f * (-2.0f*nx*(v_R(1,1) + v_R(2,2)) + ny*(v_R(0,1) + v_R(1,0)) + nz*(v_R(0,2) + v_R(2,0)) + nw*(v_R(2,1) - v_R(1,2)));
    v_quat[2] = 2.0f * (nx*(v_R(0,1) + v_R(1,0)) - 2.0f*ny*(v_R(0,0) + v_R(2,2)) + nz*(v_R(1,2) + v_R(2,1)) + nw*(v_R(0,2) - v_R(2,0)));
    v_quat[3] = 2.0f * (nx*(v_R(0,2) + v_R(2,0)) + ny*(v_R(1,2) + v_R(2,1)) - 2.0f*nz*(v_R(0,0) + v_R(1,1)) + nw*(v_R(1,0) - v_R(0,1)));

    // Chain through normalization: q_norm = q / |q|.
    // dL/dq = (I - q*q^T / |q|²) * dL/dq_norm
    //       = v_quat - (q · v_quat) / |q|² * q
    float dot = qw*v_quat[0] + qx*v_quat[1] + qy*v_quat[2] + qz*v_quat[3];
    float scale = dot / qlen_sq;
    v_quat[0] -= scale * qw;
    v_quat[1] -= scale * qx;
    v_quat[2] -= scale * qy;
    v_quat[3] -= scale * qz;
}

// VJP of 3D covariance: dL/dscale and dL/dquat from dL/dcov3d.
// cov3d = M * M^T where M = R * S.
static void scale_rot_to_cov3d_vjp(float sx, float sy, float sz,
                                   float qw, float qx, float qy, float qz,
                                   const float v_cov3d[6],
                                   float v_scale[3], float v_quat[4]) {
    // Reconstruct symmetric v_V from upper triangular v_cov3d.
    Mat3 v_V;
    v_V(0,0) = v_cov3d[0]; v_V(0,1) = 0.5f*v_cov3d[1]; v_V(0,2) = 0.5f*v_cov3d[2];
    v_V(1,0) = 0.5f*v_cov3d[1]; v_V(1,1) = v_cov3d[3]; v_V(1,2) = 0.5f*v_cov3d[4];
    v_V(2,0) = 0.5f*v_cov3d[2]; v_V(2,1) = 0.5f*v_cov3d[4]; v_V(2,2) = v_cov3d[5];

    Mat3 R = quat_to_rotmat(qw, qx, qy, qz);
    Mat3 S = mat3_identity();
    S(0,0) = sx; S(1,1) = sy; S(2,2) = sz;
    Mat3 M = mat3_mul(R, S);

    // v_M = 2 * v_V * M (for Sigma = M * M^T).
    Mat3 v_M = mat3_mul(v_V, M);
    for (int i = 0; i < 9; ++i) v_M.m[i] *= 2.0f;

    // v_scale = diag(R^T * v_M)
    v_scale[0] = R(0,0)*v_M(0,0) + R(1,0)*v_M(1,0) + R(2,0)*v_M(2,0);
    v_scale[1] = R(0,1)*v_M(0,1) + R(1,1)*v_M(1,1) + R(2,1)*v_M(2,1);
    v_scale[2] = R(0,2)*v_M(0,2) + R(1,2)*v_M(1,2) + R(2,2)*v_M(2,2);

    // v_R = v_M * S^T = v_M * S (S is diagonal).
    Mat3 v_R = mat3_mul(v_M, S);

    // v_quat from v_R.
    quat_to_rotmat_vjp(qw, qx, qy, qz, v_R, v_quat);
}

// ============================================================================
// Render state for the differentiable backward pass.
//
// Alpha-blended Gaussian splatting composites gaussians front-to-back as:
//     C  += color * alpha * T
//     T'  = T * (1 - alpha)          (accumulated transmittance)
// where alpha = opacity * exp(-sigma) and sigma is the Mahalanobis distance
// using the conic (inverse 2D covariance). To backpropagate dL/d(gaussian
// params) we need, for each pixel, the per-gaussian (alpha, T_at_composite,
// color, opacity, gauss_weight, pixel offset) used during the forward. We also
// cache per-gaussian projection data (conic, cov3d, view-space position) so
// position/scale/rotation gradients can be reconstructed without recomputing
// the EWA projection.
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
    float opacity;     // sigmoid(logit) of the gaussian
    float gauss_weight;// exp(-sigma): the spatial falloff
    float delta_x;     // screen_x - pixel_x (for conic/xy gradients)
    float delta_y;     // screen_y - pixel_y
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
        float conic[3] = {0, 0, 0};    // inverse 2D covariance (xx, xy, yy)
        float cov3d[6] = {0, 0, 0, 0, 0, 0}; // 3D covariance upper triangular
        float t_view[3] = {0, 0, 0};   // view-space position (for EWA backward)
        float mean3d[3] = {0, 0, 0};   // world-space position (for projection VJP)
        float scale[3] = {0, 0, 0};    // exp(scale_log) — actual scale values
        float quat[4] = {0, 0, 0, 0};  // quaternion (w, x, y, z)
        float radius = 0;
        bool visible = false;
    };
    std::vector<Proj> projections;
    // Camera data for the backward pass (position/scale/rotation VJPs).
    bool has_camera = false;
    float focal_x = 0;
    float focal_y = 0;
    float tan_fovx = 0;
    float tan_fovy = 0;
    float view_matrix[16] = {0};
    float view_proj_matrix[16] = {0};
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

    auto* state = new RenderState();
    state->width = camera.width;
    state->height = camera.height;
    state->pixels.resize(num_pixels);
    state->projections.resize(num_gaussians);
    // Store camera data for the backward pass (position/scale/rotation VJPs).
    state->has_camera = true;
    state->focal_x = camera.width / (2.0f * std::tan(
        (2.0f * std::atan(std::tan(camera.fov_y * 0.5f) * camera.aspect)) * 0.5f));
    state->focal_y = camera.height / (2.0f * std::tan(camera.fov_y * 0.5f));
    state->tan_fovx = 0.5f * camera.width / state->focal_x;
    state->tan_fovy = 0.5f * camera.height / state->focal_y;
    memcpy(state->view_matrix, camera.view_matrix, sizeof(float) * 16);
    memcpy(state->view_proj_matrix, camera.view_proj_matrix, sizeof(float) * 16);

    for (size_t i = 0; i < num_pixels; ++i) {
        result.image[i * 3 + 0] = background[0];
        result.image[i * 3 + 1] = background[1];
        result.image[i * 3 + 2] = background[2];
    }

    // Focal lengths and FOV tangents (hoisted out of the per-Gaussian loop).
    float focal_x = state->focal_x;
    float focal_y = state->focal_y;
    float tan_fovx = 0.5f * camera.width / focal_x;
    float tan_fovy = 0.5f * camera.height / focal_y;

    for (size_t g_idx = 0; g_idx < num_gaussians; ++g_idx) {
        const auto& g = gaussians[g_idx];
        auto& proj = state->projections[g_idx];

        // Store world-space position and scale/quat for the backward pass.
        proj.mean3d[0] = g.position[0];
        proj.mean3d[1] = g.position[1];
        proj.mean3d[2] = g.position[2];
        proj.scale[0] = std::exp(g.scale[0]);
        proj.scale[1] = std::exp(g.scale[1]);
        proj.scale[2] = std::exp(g.scale[2]);
        proj.quat[0] = g.rotation[0]; // w
        proj.quat[1] = g.rotation[1]; // x
        proj.quat[2] = g.rotation[2]; // y
        proj.quat[3] = g.rotation[3]; // z

        // Transform position to clip space.
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

        // Compute 3D covariance from scale and quaternion.
        float cov3d[6];
        scale_rot_to_cov3d(proj.scale[0], proj.scale[1], proj.scale[2],
                           proj.quat[0], proj.quat[1], proj.quat[2], proj.quat[3],
                           cov3d);
        memcpy(proj.cov3d, cov3d, sizeof(float) * 6);

        // Project 3D covariance to 2D via EWA.
        float t_view[3];
        float cov2d[3];
        project_cov3d_ewa(proj.mean3d, cov3d, camera.view_matrix,
                          focal_x, focal_y, tan_fovx, tan_fovy,
                          t_view, cov2d);
        memcpy(proj.t_view, t_view, sizeof(float) * 3);

        // Compute conic (inverse 2D covariance) and bounding radius.
        float conic[3];
        float radius;
        if (!compute_cov2d_bounds(cov2d, conic, radius)) {
            proj.visible = false;
            continue;
        }
        memcpy(proj.conic, conic, sizeof(float) * 3);
        proj.radius = radius;
        proj.visible = true;

        // Project center to screen space.
        float ndc_x = clip[0] / clip[3];
        float ndc_y = clip[1] / clip[3];
        float screen_x = (ndc_x * 0.5f + 0.5f) * camera.width;
        float screen_y = (ndc_y * 0.5f + 0.5f) * camera.height;
        proj.screen_x = screen_x;
        proj.screen_y = screen_y;

        // Color from SH-DC (no clamp — clamping kills gradients outside [0,1]).
        float color_r = g.color[0] * SH_C0 + 0.5f;
        float color_g = g.color[1] * SH_C0 + 0.5f;
        float color_b = g.color[2] * SH_C0 + 0.5f;

        // Opacity from position[3] (logit space).
        float opacity = 1.0f / (1.0f + std::exp(-g.position[3]));

        int min_x = std::max(0, static_cast<int>(screen_x - radius));
        int max_x = std::min(camera.width - 1, static_cast<int>(screen_x + radius));
        int min_y = std::max(0, static_cast<int>(screen_y - radius));
        int max_y = std::min(camera.height - 1, static_cast<int>(screen_y + radius));

        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                // delta = center - pixel (gsplat convention).
                float dx = screen_x - (px + 0.5f);
                float dy = screen_y - (py + 0.5f);

                // Mahalanobis distance: sigma = 0.5 * d^T * conic * d.
                float sigma = 0.5f * (conic[0] * dx * dx + 2.0f * conic[1] * dx * dy + conic[2] * dy * dy);
                if (sigma < 0.0f) continue;

                float gaussian_weight = std::exp(-sigma);
                float alpha = opacity * gaussian_weight;

                if (alpha < 1.0f / 255.0f) continue;

                size_t pixel_idx = static_cast<size_t>(py) * camera.width + px;
                float T = 1.0f - result.alpha[pixel_idx];

                state->pixels[pixel_idx].push_back({
                    static_cast<uint32_t>(g_idx), alpha, T,
                    {color_r, color_g, color_b},
                    opacity, gaussian_weight, dx, dy});

                result.image[pixel_idx * 3 + 0] += color_r * alpha * T;
                result.image[pixel_idx * 3 + 1] += color_g * alpha * T;
                result.image[pixel_idx * 3 + 2] += color_b * alpha * T;
                result.alpha[pixel_idx] += alpha * T;
            }
        }
    }

    result.internal_state = state;
    return result;
}

DifferentiableRenderer::BackwardResult DifferentiableRenderer::backward(
    ForwardResult& forward_result,
    const std::vector<float>& grad_image) {

    BackwardResult result;
    auto* state = static_cast<RenderState*>(forward_result.internal_state);
    if (state == nullptr) {
        return result;
    }

    const int W = state->width;
    const int H = state->height;
    const size_t num_pixels = static_cast<size_t>(W) * H;
    const size_t N = state->projections.size();

    result.grad_positions.assign(N * 3, 0.0f);
    result.grad_scales.assign(N * 3, 0.0f);
    result.grad_rotations.assign(N * 4, 0.0f);
    result.grad_colors.assign(N * 3, 0.0f);
    result.grad_opacities.assign(N, 0.0f);

    // Intermediate gradient accumulators (per-gaussian, accumulated across pixels).
    std::vector<float> v_conic(N * 3, 0.0f);     // dL/dconic
    std::vector<float> v_screen_xy(N * 2, 0.0f); // dL/d(screen_x, screen_y)

    const float SH_C0 = melkor::utils::SH_C0;
    std::vector<float> accum(num_pixels, 0.0f);

    // Pass 1: per-pixel backward through the alpha-blend composite.
    // Computes dL/dcolor, dL/dopacity_logit, and accumulates dL/dconic and
    // dL/dscreen_xy from the Mahalanobis distance gradient.
    //
    // Standard 3DGS alpha-blend backward (see gsplat mathematical supplement):
    //   dL/dcolor_i  = alpha_i * T_i * dL/dC
    //   dL/dalpha_i  = T_i * (color_i . dL/dC - accum)
    //   accum        = accum * (1 - alpha_i) + (color_i . dL/dC) * alpha_i
    // alpha = opacity * exp(-sigma), sigma = 0.5 * d^T * conic * d.
    for (size_t pixel_idx = 0; pixel_idx < num_pixels; ++pixel_idx) {
        const auto& contribs = state->pixels[pixel_idx];
        if (contribs.empty()) continue;

        float gx = grad_image[pixel_idx * 3 + 0];
        float gy = grad_image[pixel_idx * 3 + 1];
        float gz = grad_image[pixel_idx * 3 + 2];

        for (auto it = contribs.rbegin(); it != contribs.rend(); ++it) {
            const auto& c = *it;
            uint32_t gid = c.gaussian_id;
            float alpha = c.alpha;
            float T = c.T_at_comp;

            // Color gradient: dL/d(sh_dc) = SH_C0 * alpha * T * dL/dC.
            float dcolor_scale = alpha * T * SH_C0;
            result.grad_colors[gid * 3 + 0] += dcolor_scale * gx;
            result.grad_colors[gid * 3 + 1] += dcolor_scale * gy;
            result.grad_colors[gid * 3 + 2] += dcolor_scale * gz;

            // Alpha gradient: direct + transmittance chain.
            float color_dot_grad = c.color[0] * gx + c.color[1] * gy + c.color[2] * gz;
            float dalpha = T * (color_dot_grad - accum[pixel_idx]);
            accum[pixel_idx] = accum[pixel_idx] * (1.0f - alpha) + color_dot_grad * alpha;

            // Opacity logit gradient: dalpha * gauss_weight * sigmoid'(logit).
            float sig_prime = c.opacity * (1.0f - c.opacity);
            result.grad_opacities[gid] += dalpha * c.gauss_weight * sig_prime;

            // Spatial gradient: dL/dsigma = -opacity * exp(-sigma) * dalpha = -alpha * dalpha.
            float v_sigma = -alpha * dalpha;

            // dL/dconic from sigma = 0.5 * (conic.x*dx² + 2*conic.y*dx*dy + conic.z*dy²).
            // Note: all three terms get 0.5 factor (matching gsplat-metal), so that
            // v_conic[1] = 0.5 * v_sigma * dx * dy. This makes the G matrix in
            // cov2d_to_conic_vjp equal to the true full-matrix gradient G_full.
            v_conic[gid * 3 + 0] += v_sigma * 0.5f * c.delta_x * c.delta_x;
            v_conic[gid * 3 + 1] += v_sigma * 0.5f * c.delta_x * c.delta_y;
            v_conic[gid * 3 + 2] += v_sigma * 0.5f * c.delta_y * c.delta_y;

            // dL/d(screen_xy) from sigma. delta = center - pixel, d/d(center) = +1.
            const auto& proj = state->projections[gid];
            v_screen_xy[gid * 2 + 0] += v_sigma * (proj.conic[0] * c.delta_x + proj.conic[1] * c.delta_y);
            v_screen_xy[gid * 2 + 1] += v_sigma * (proj.conic[1] * c.delta_x + proj.conic[2] * c.delta_y);
        }
    }

    // Pass 2: convert intermediate gradients to position, scale, rotation.
    // Requires camera matrices and focal lengths stored in RenderState.
    if (state->has_camera) {
        float focal_x = state->focal_x;
        float focal_y = state->focal_y;

        for (size_t g = 0; g < N; ++g) {
            const auto& proj = state->projections[g];
            if (!proj.visible) continue;

            // dL/dmean3d from dL/dscreen_xy (projection VJP).
            float v_mean3d_proj[3] = {0, 0, 0};
            project_pix_vjp(state->view_proj_matrix, proj.mean3d,
                            state->width, state->height,
                            v_screen_xy[g * 2 + 0], v_screen_xy[g * 2 + 1],
                            v_mean3d_proj);

            // dL/dcov2d from dL/dconic (conic = inv(cov2d)).
            float v_cov2d[3];
            cov2d_to_conic_vjp(proj.conic, &v_conic[g * 3], v_cov2d);

            // dL/dcov3d and dL/dmean3d (EWA) from dL/dcov2d.
            float v_mean3d_ewa[3] = {0, 0, 0};
            float v_cov3d[6];
            project_cov3d_ewa_vjp(proj.mean3d, proj.cov3d, state->view_matrix,
                                  focal_x, focal_y, state->tan_fovx, state->tan_fovy,
                                  v_cov2d,
                                  v_mean3d_ewa, v_cov3d);

            // Combine position gradients from projection and EWA.
            result.grad_positions[g * 3 + 0] = v_mean3d_proj[0] + v_mean3d_ewa[0];
            result.grad_positions[g * 3 + 1] = v_mean3d_proj[1] + v_mean3d_ewa[1];
            result.grad_positions[g * 3 + 2] = v_mean3d_proj[2] + v_mean3d_ewa[2];

            // dL/dscale and dL/dquat from dL/dcov3d.
            // Scale gradients are w.r.t. exp(scale_log), so chain: dL/d(log) = dL/d(scale) * scale.
            float v_scale[3], v_quat[4];
            scale_rot_to_cov3d_vjp(proj.scale[0], proj.scale[1], proj.scale[2],
                                   proj.quat[0], proj.quat[1], proj.quat[2], proj.quat[3],
                                   v_cov3d, v_scale, v_quat);
            result.grad_scales[g * 3 + 0] = v_scale[0] * proj.scale[0];
            result.grad_scales[g * 3 + 1] = v_scale[1] * proj.scale[1];
            result.grad_scales[g * 3 + 2] = v_scale[2] * proj.scale[2];
            result.grad_rotations[g * 4 + 0] = v_quat[0];
            result.grad_rotations[g * 4 + 1] = v_quat[1];
            result.grad_rotations[g * 4 + 2] = v_quat[2];
            result.grad_rotations[g * 4 + 3] = v_quat[3];
        }
    }

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
    // full EWA backward pass computing gradients for all 5 Gaussian parameters
    // (position, scale, rotation, color, opacity), verified by test_gradient_check.
    // The fitting pipeline below measures L1 reprojection error of the
    // surface-aligned initialization. Full gradient-based optimization would
    // use backward() to drive an Adam optimizer over all parameters.
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
