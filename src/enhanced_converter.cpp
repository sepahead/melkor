#include "melkor/enhanced_converter.hpp"
#include "melkor/glb_reader.hpp"
#include <array>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <random>

// For GLB loading — guard against definitions from the CMake target
#ifndef TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#endif
#ifndef TINYGLTF_NO_EXTERNAL_IMAGE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#endif
#include "tiny_gltf.h"

namespace melkor {

namespace {

// Analytical eigendecomposition of a real symmetric 3x3 matrix
// (Smith, "The Eigenvalues and Eigenvectors of 3x3 Matrices", 2002).
// Out: eigenvalues w0<=w1<=w2 (ascending), and corresponding unit eigenvectors.
// Used for PCA normal estimation: the normal is the eigenvector for the
// smallest eigenvalue (direction of least variance in the neighborhood).
void eigenSymmetric3x3(
    float a11, float a12, float a13,
    float a22, float a23, float a33,
    float& w0, float& w1, float& w2,  // eigenvalues, ascending
    float v0[3], float v1[3], float v2[3]) {  // eigenvectors (unit)
    float p1 = a12 * a12 + a13 * a13 + a23 * a23;
    if (p1 < 1e-18f) {
        // Matrix is already diagonal.
        w0 = a11; w1 = a22; w2 = a33;
        // Order them ascending with a stable sort of {0,1,2}.
        struct E { float w; int i; };
        E e[3] = {{a11, 0}, {a22, 1}, {a33, 2}};
        std::sort(e, e + 3, [](const E& x, const E& y) { return x.w < y.w; });
        w0 = e[0].w; w1 = e[1].w; w2 = e[2].w;
        for (int k = 0; k < 3; ++k) {
            float* v = (k == 0) ? v0 : (k == 1) ? v1 : v2;
            v[0] = (e[k].i == 0) ? 1.0f : 0.0f;
            v[1] = (e[k].i == 1) ? 1.0f : 0.0f;
            v[2] = (e[k].i == 2) ? 1.0f : 0.0f;
        }
        return;
    }
    float q = (a11 + a22 + a33) / 3.0f;
    float p2 = (a11 - q) * (a11 - q) + (a22 - q) * (a22 - q) +
               (a33 - q) * (a33 - q) + 2.0f * p1;
    float p = std::sqrt(p2 / 6.0f);
    // B = (1/p) (A - q I)
    float b11 = (a11 - q) / p, b12 = a12 / p, b13 = a13 / p;
    float b22 = (a22 - q) / p, b23 = a23 / p;
    float b33 = (a33 - q) / p;
    // det(B)
    float detB = b11 * (b22 * b33 - b23 * b23) -
                 b12 * (b12 * b33 - b23 * b13) +
                 b13 * (b12 * b23 - b22 * b13);
    float r = std::clamp(detB / 2.0f, -1.0f, 1.0f);
    float phi = std::acos(r) / 3.0f;
    // Eigenvalues of A, ordered chi1 >= chi2 >= chi3.
    float chi1 = q + 2.0f * p * std::cos(phi);
    float chi3 = q + 2.0f * p * std::cos(phi + 2.0943951023931953f /* 2pi/3 */);
    float chi2 = 3.0f * q - chi1 - chi3;
    // Ascending.
    w0 = chi3; w1 = chi2; w2 = chi1;

    // Eigenvector for eigenvalue w: solve (A - w I) v = 0 via the cross product
    // of two rows of (A - w I). Pick the two rows with the largest-magnitude
    // entries to maximize numerical stability.
    auto eigvec = [&](float w) -> std::array<float, 3> {
        float m00 = a11 - w, m01 = a12, m02 = a13;
        float m10 = a12, m11 = a22 - w, m12 = a23;
        float m20 = a13, m21 = a23, m22 = a33 - w;
        // Row norms.
        float r0 = m00 * m00 + m01 * m01 + m02 * m02;
        float r1 = m10 * m10 + m11 * m11 + m12 * m12;
        float r2 = m20 * m20 + m21 * m21 + m22 * m22;
        // Cross product of the two smallest-norm rows (least trust -> discard).
        float ax, ay, az, bx, by, bz;
        if (r0 <= r1 && r0 <= r2) { ax = m10; ay = m11; az = m12; bx = m20; by = m21; bz = m22; }
        else if (r1 <= r0 && r1 <= r2) { ax = m00; ay = m01; az = m02; bx = m20; by = m21; bz = m22; }
        else { ax = m00; ay = m01; az = m02; bx = m10; by = m11; bz = m12; }
        float cx = ay * bz - az * by;
        float cy = az * bx - ax * bz;
        float cz = ax * by - ay * bx;
        float len = std::sqrt(cx * cx + cy * cy + cz * cz);
        if (len < 1e-12f) {
            // Degenerate; fall back to an axis.
            return {1.0f, 0.0f, 0.0f};
        }
        return {cx / len, cy / len, cz / len};
    };
    auto e0 = eigvec(w0); auto e1 = eigvec(w1); auto e2 = eigvec(w2);
    v0[0] = e0[0]; v0[1] = e0[1]; v0[2] = e0[2];
    v1[0] = e1[0]; v1[1] = e1[1]; v1[2] = e1[2];
    v2[0] = e2[0]; v2[1] = e2[1]; v2[2] = e2[2];
}

// Return only the eigenvector for the smallest eigenvalue of the symmetric
// covariance matrix (the surface normal under the local-plane PCA model).
void smallestEigenvector3x3(
    float sxx, float sxy, float sxz, float syy, float syz, float szz,
    float& nx, float& ny, float& nz) {
    float w0, w1, w2;
    float v0[3], v1[3], v2[3];
    eigenSymmetric3x3(sxx, sxy, sxz, syy, syz, szz, w0, w1, w2, v0, v1, v2);
    nx = v0[0]; ny = v0[1]; nz = v0[2];
}

}  // namespace

// Simple spatial hash for k-NN queries
class SpatialHash {
public:
    SpatialHash(const std::vector<float>& positions, float cell_size)
        : cell_size_(cell_size), inv_cell_size_(1.0f / cell_size) {
        size_t num_points = positions.size() / 3;
        for (size_t i = 0; i < num_points; ++i) {
            int cx = static_cast<int>(std::floor(positions[i*3+0] * inv_cell_size_));
            int cy = static_cast<int>(std::floor(positions[i*3+1] * inv_cell_size_));
            int cz = static_cast<int>(std::floor(positions[i*3+2] * inv_cell_size_));
            uint64_t key = hashCell(cx, cy, cz);
            cells_[key].push_back(i);
        }
    }
    
    std::vector<size_t> queryNeighbors(float x, float y, float z, int radius = 1) const {
        std::vector<size_t> result;
        int cx = static_cast<int>(std::floor(x * inv_cell_size_));
        int cy = static_cast<int>(std::floor(y * inv_cell_size_));
        int cz = static_cast<int>(std::floor(z * inv_cell_size_));
        
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    uint64_t key = hashCell(cx + dx, cy + dy, cz + dz);
                    auto it = cells_.find(key);
                    if (it != cells_.end()) {
                        result.insert(result.end(), it->second.begin(), it->second.end());
                    }
                }
            }
        }
        return result;
    }
    
private:
    static uint64_t hashCell(int x, int y, int z) {
        // Simple spatial hash
        return (static_cast<uint64_t>(x & 0x1FFFFF) << 42) |
               (static_cast<uint64_t>(y & 0x1FFFFF) << 21) |
               (static_cast<uint64_t>(z & 0x1FFFFF));
    }
    
    float cell_size_;
    float inv_cell_size_;
    std::unordered_map<uint64_t, std::vector<size_t>> cells_;
};

class EnhancedConverter::Impl {
public:
    metal::MetalContext* metal_ctx_ = nullptr;
    tinygltf::TinyGLTF loader_;
    
    Impl() = default;
    explicit Impl(metal::MetalContext* ctx) : metal_ctx_(ctx) {}
    
    EnhancedConversionResult convert(
        const std::vector<float>& positions,
        const std::vector<float>& normals,
        const std::vector<float>& colors,
        const std::vector<uint32_t>& indices,
        const EnhancedConversionConfig& config) {
        
        EnhancedConversionResult result;
        result.original_vertices = positions.size() / 3;
        
        if (positions.empty()) {
            result.error_message = "No positions provided";
            return result;
        }
        
        // Step 1: Compute adaptive scales using k-NN
        std::vector<float> adaptive_scales = computeAdaptiveScales(
            positions, config.knn_neighbors, config);
        
        // Step 2: Compute or use provided normals
        std::vector<float> final_normals = normals;
        if (final_normals.empty()) {
            final_normals = enhanced::estimateNormals(positions, config.knn_neighbors);
        }
        
        // Step 3: Convert each point to a Gaussian splat
        size_t num_points = positions.size() / 3;
        result.cloud.reserve(num_points * 2);  // Reserve extra for potential subdivision
        
        float total_scale = 0.0f;
        
        for (size_t i = 0; i < num_points; ++i) {
            GaussianSplat splat;
            
            // Position (with coordinate conversion if needed)
            float px = positions[i*3+0] * config.position_scale;
            float py = positions[i*3+1] * config.position_scale;
            float pz = positions[i*3+2] * config.position_scale;
            
            if (config.convert_coordinate_system) {
                // Y-up to Z-up
                splat.x = px;
                splat.y = -pz;
                splat.z = py;
            } else {
                splat.x = px;
                splat.y = py;
                splat.z = pz;
            }
            
            // Color -> SH DC coefficients
            float r, g, b;
            if (!colors.empty() && config.use_vertex_colors) {
                r = colors[i*3+0];
                g = colors[i*3+1];
                b = colors[i*3+2];
            } else {
                r = config.default_color[0];
                g = config.default_color[1];
                b = config.default_color[2];
            }
            splat.f_dc_0 = utils::rgbToShDc(r);
            splat.f_dc_1 = utils::rgbToShDc(g);
            splat.f_dc_2 = utils::rgbToShDc(b);
            
            // Opacity (in logit space)
            splat.opacity = utils::logit(std::clamp(config.default_opacity, 0.001f, 0.999f));
            
            // Adaptive anisotropic scale
            float base_scale = adaptive_scales[i] * config.scale_factor;
            base_scale = std::clamp(base_scale, config.min_scale, config.max_scale);
            
            float tangent_scale = base_scale;
            float normal_scale = base_scale * config.normal_scale_ratio;
            
            if (config.use_surface_alignment && !final_normals.empty()) {
                // Anisotropic scale: flatter along normal
                splat.scale_0 = std::log(tangent_scale);
                splat.scale_1 = std::log(tangent_scale);
                splat.scale_2 = std::log(normal_scale);
            } else {
                // Isotropic scale
                float log_scale = std::log(base_scale);
                splat.scale_0 = log_scale;
                splat.scale_1 = log_scale;
                splat.scale_2 = log_scale;
            }
            
            total_scale += base_scale;
            
            // Rotation from surface normal
            if (config.use_surface_alignment && !final_normals.empty()) {
                float nx = final_normals[i*3+0];
                float ny = final_normals[i*3+1];
                float nz = final_normals[i*3+2];
                
                if (config.convert_coordinate_system) {
                    float tmp = ny;
                    ny = -nz;
                    nz = tmp;
                }
                
                // Compute quaternion that rotates Z-axis to normal
                computeQuaternionFromNormal(nx, ny, nz,
                    splat.rot_0, splat.rot_1, splat.rot_2, splat.rot_3);
            } else {
                // Identity quaternion
                splat.rot_0 = 1.0f;
                splat.rot_1 = 0.0f;
                splat.rot_2 = 0.0f;
                splat.rot_3 = 0.0f;
            }
            
            result.cloud.addSplat(std::move(splat));
        }
        
        result.output_splats = result.cloud.size();
        result.avg_scale = total_scale / static_cast<float>(num_points);
        result.success = true;
        
        return result;
    }
    
    std::vector<float> computeAdaptiveScales(
        const std::vector<float>& positions,
        int k,
        const EnhancedConversionConfig& config) {
        
        size_t num_points = positions.size() / 3;
        std::vector<float> scales(num_points);
        
        // Compute bounding box for cell size estimation
        float min_x = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        float min_y = min_x, max_y = max_x, min_z = min_x, max_z = max_x;
        
        for (size_t i = 0; i < num_points; ++i) {
            min_x = std::min(min_x, positions[i*3+0]);
            max_x = std::max(max_x, positions[i*3+0]);
            min_y = std::min(min_y, positions[i*3+1]);
            max_y = std::max(max_y, positions[i*3+1]);
            min_z = std::min(min_z, positions[i*3+2]);
            max_z = std::max(max_z, positions[i*3+2]);
        }
        
        float extent = std::max({max_x - min_x, max_y - min_y, max_z - min_z});
        float cell_size = extent / std::cbrt(static_cast<float>(num_points)) * 2.0f;
        
        // Build spatial hash
        SpatialHash hash(positions, cell_size);
        
        // For each point, find k nearest neighbors and compute average distance
        for (size_t i = 0; i < num_points; ++i) {
            float px = positions[i*3+0];
            float py = positions[i*3+1];
            float pz = positions[i*3+2];
            
            // Query neighbors, expanding the search radius until we have at
            // least k+1 candidates (matching the pattern in estimateNormals).
            auto candidates = hash.queryNeighbors(px, py, pz, 2);
            int radius = 2;
            while (static_cast<int>(candidates.size()) < k + 1 && radius < 16) {
                candidates = hash.queryNeighbors(px, py, pz, ++radius);
            }
            
            // Compute distances to all candidates
            std::vector<float> distances;
            distances.reserve(candidates.size());
            
            if (distances.empty()) {
                scales[i] = config.min_scale;
                continue;
            }
            
            // Sort and take k nearest
            std::sort(distances.begin(), distances.end());
            int actual_k = std::min(k, static_cast<int>(distances.size()));
            
            // Average of k nearest distances
            float avg_dist = 0.0f;
            for (int j = 0; j < actual_k; ++j) {
                avg_dist += distances[j];
            }
            avg_dist /= static_cast<float>(actual_k);
            
            // Scale should cover approximately half the distance to nearest neighbor
            scales[i] = avg_dist * 0.5f;
        }
        
        return scales;
    }
    
    void computeQuaternionFromNormal(float nx, float ny, float nz,
                                     float& w, float& x, float& y, float& z) {
        // Normalize normal
        float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (len > 0.0f) {
            nx /= len;
            ny /= len;
            nz /= len;
        } else {
            // Default to Z-up
            nx = 0.0f;
            ny = 0.0f;
            nz = 1.0f;
        }
        
        // Compute quaternion that rotates Z-axis (0,0,1) to normal (nx,ny,nz)
        float dot = nz;  // dot(Z-axis, normal)
        
        if (dot > 0.9999f) {
            // Normal is already Z-up
            w = 1.0f;
            x = 0.0f;
            y = 0.0f;
            z = 0.0f;
        } else if (dot < -0.9999f) {
            // Normal is Z-down, rotate 180° around X
            w = 0.0f;
            x = 1.0f;
            y = 0.0f;
            z = 0.0f;
        } else {
            // General case: rotation axis = cross(Z, normal)
            float ax = -ny;  // cross(Z, normal).x = 0*nz - 1*ny = -ny
            float ay = nx;   // cross(Z, normal).y = 1*nx - 0*nz = nx
            float az = 0.0f; // cross(Z, normal).z = 0*ny - 0*nx = 0
            
            // Half-angle formula
            float s = std::sqrt((1.0f + dot) * 2.0f);
            float inv_s = 1.0f / s;
            
            w = s * 0.5f;
            x = ax * inv_s;
            y = ay * inv_s;
            z = az * inv_s;
        }
        
        // Normalize quaternion
        utils::normalizeQuaternion(w, x, y, z);
    }
};

EnhancedConverter::EnhancedConverter() : impl_(std::make_unique<Impl>()) {}
EnhancedConverter::EnhancedConverter(metal::MetalContext* metal_ctx)
    : impl_(std::make_unique<Impl>(metal_ctx)) {}
EnhancedConverter::~EnhancedConverter() = default;

EnhancedConversionResult EnhancedConverter::convertFromFile(
    const std::string& filepath,
    const EnhancedConversionConfig& config) {
    
    EnhancedConversionResult result;
    
    tinygltf::Model model;
    std::string err, warn;
    bool success;
    
    if (filepath.size() >= 4 && filepath.substr(filepath.size() - 4) == ".glb") {
        success = impl_->loader_.LoadBinaryFromFile(&model, &err, &warn, filepath);
    } else {
        success = impl_->loader_.LoadASCIIFromFile(&model, &err, &warn, filepath);
    }
    
    if (!success) {
        result.error_message = err;
        return result;
    }
    
    // Extract mesh data from GLB
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> colors;
    std::vector<uint32_t> indices;
    
    for (const auto& mesh : model.meshes) {
        for (const auto& primitive : mesh.primitives) {
            // Get positions
            auto pos_it = primitive.attributes.find("POSITION");
            if (pos_it == primitive.attributes.end()) continue;
            
            const auto& pos_accessor = model.accessors[pos_it->second];
            const auto& pos_buffer_view = model.bufferViews[pos_accessor.bufferView];
            const auto& pos_buffer = model.buffers[pos_buffer_view.buffer];
            
            const uint8_t* pos_data = pos_buffer.data.data() + 
                pos_buffer_view.byteOffset + pos_accessor.byteOffset;
            size_t pos_stride = pos_accessor.ByteStride(pos_buffer_view);
            if (pos_stride == 0) pos_stride = sizeof(float) * 3;
            
            size_t base_idx = positions.size() / 3;
            size_t count = pos_accessor.count;
            
            for (size_t i = 0; i < count; ++i) {
                const float* pos = reinterpret_cast<const float*>(pos_data + i * pos_stride);
                positions.push_back(pos[0]);
                positions.push_back(pos[1]);
                positions.push_back(pos[2]);
            }
            
            // Get normals
            auto norm_it = primitive.attributes.find("NORMAL");
            if (norm_it != primitive.attributes.end()) {
                const auto& norm_accessor = model.accessors[norm_it->second];
                const auto& norm_buffer_view = model.bufferViews[norm_accessor.bufferView];
                const auto& norm_buffer = model.buffers[norm_buffer_view.buffer];
                
                const uint8_t* norm_data = norm_buffer.data.data() + 
                    norm_buffer_view.byteOffset + norm_accessor.byteOffset;
                size_t norm_stride = norm_accessor.ByteStride(norm_buffer_view);
                if (norm_stride == 0) norm_stride = sizeof(float) * 3;
                
                for (size_t i = 0; i < count; ++i) {
                    const float* n = reinterpret_cast<const float*>(norm_data + i * norm_stride);
                    normals.push_back(n[0]);
                    normals.push_back(n[1]);
                    normals.push_back(n[2]);
                }
            }
            
            // Get colors
            auto color_it = primitive.attributes.find("COLOR_0");
            if (color_it != primitive.attributes.end()) {
                const auto& color_accessor = model.accessors[color_it->second];
                const auto& color_buffer_view = model.bufferViews[color_accessor.bufferView];
                const auto& color_buffer = model.buffers[color_buffer_view.buffer];
                
                const uint8_t* color_ptr = color_buffer.data.data() + 
                    color_buffer_view.byteOffset + color_accessor.byteOffset;
                
                // Determine stride and component count
                int num_components = (color_accessor.type == TINYGLTF_TYPE_VEC4) ? 4 : 3;
                size_t color_stride = color_accessor.ByteStride(color_buffer_view);
                if (color_stride == 0) {
                    if (color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        color_stride = num_components;
                    } else if (color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        color_stride = num_components * 2;
                    } else {
                        color_stride = num_components * sizeof(float);
                    }
                }
                
                for (size_t i = 0; i < count; ++i) {
                    const uint8_t* cp = color_ptr + i * color_stride;
                    float r, g, b;
                    if (color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        r = cp[0] / 255.0f;
                        g = cp[1] / 255.0f;
                        b = cp[2] / 255.0f;
                    } else if (color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        const uint16_t* c16 = reinterpret_cast<const uint16_t*>(cp);
                        r = c16[0] / 65535.0f;
                        g = c16[1] / 65535.0f;
                        b = c16[2] / 65535.0f;
                    } else {
                        const float* cf = reinterpret_cast<const float*>(cp);
                        r = cf[0];
                        g = cf[1];
                        b = cf[2];
                    }
                    colors.push_back(r);
                    colors.push_back(g);
                    colors.push_back(b);
                    // VEC4: ignore alpha — splat opacity is controlled separately
                }
            }
            
            // Get indices (for potential triangle-based processing)
            if (primitive.indices >= 0) {
                const auto& idx_accessor = model.accessors[primitive.indices];
                const auto& idx_buffer_view = model.bufferViews[idx_accessor.bufferView];
                const auto& idx_buffer = model.buffers[idx_buffer_view.buffer];
                
                const uint8_t* idx_ptr = idx_buffer.data.data() + 
                    idx_buffer_view.byteOffset + idx_accessor.byteOffset;
                
                for (size_t i = 0; i < idx_accessor.count; ++i) {
                    uint32_t idx;
                    if (idx_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        idx = reinterpret_cast<const uint16_t*>(idx_ptr)[i];
                    } else if (idx_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                        idx = reinterpret_cast<const uint32_t*>(idx_ptr)[i];
                    } else {
                        idx = idx_ptr[i];
                    }
                    indices.push_back(static_cast<uint32_t>(base_idx + idx));
                }
            }
        }
    }
    
    return impl_->convert(positions, normals, colors, indices, config);
}

EnhancedConversionResult EnhancedConverter::convertFromMesh(
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<float>& colors,
    const std::vector<uint32_t>& indices,
    const EnhancedConversionConfig& config) {
    return impl_->convert(positions, normals, colors, indices, config);
}

namespace enhanced {

std::vector<float> computeKnnDistances(
    const std::vector<float>& positions,
    int k,
    metal::MetalContext* /*metal_ctx*/) {

    size_t num_points = positions.size() / 3;
    std::vector<float> distances(num_points, 0.0f);
    if (num_points == 0) return distances;

    // O(n²) brute force for small clouds; spatial hash for larger ones.
    if (num_points < 10000) {
        for (size_t i = 0; i < num_points; ++i) {
            std::vector<float> dists;
            dists.reserve(num_points - 1);
            
            for (size_t j = 0; j < num_points; ++j) {
                if (i == j) continue;
                float dx = positions[j*3+0] - positions[i*3+0];
                float dy = positions[j*3+1] - positions[i*3+1];
                float dz = positions[j*3+2] - positions[i*3+2];
                dists.push_back(std::sqrt(dx*dx + dy*dy + dz*dz));
            }
            
            std::sort(dists.begin(), dists.end());
            int actual_k = std::min(k, static_cast<int>(dists.size()));
            for (int j = 0; j < actual_k; ++j) {
                distances[i] += dists[j];
            }
            distances[i] /= static_cast<float>(actual_k);
        }
    } else {
        // Spatial hash k-NN for large clouds (same pattern as estimateNormals).
        float min_x = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        float min_y = min_x, max_y = max_x, min_z = min_x, max_z = max_x;
        for (size_t i = 0; i < num_points; ++i) {
            min_x = std::min(min_x, positions[i*3+0]);
            max_x = std::max(max_x, positions[i*3+0]);
            min_y = std::min(min_y, positions[i*3+1]);
            max_y = std::max(max_y, positions[i*3+1]);
            min_z = std::min(min_z, positions[i*3+2]);
            max_z = std::max(max_z, positions[i*3+2]);
        }
        float extent = std::max({max_x - min_x, max_y - min_y, max_z - min_z});
        if (extent < 1e-9f) return distances;
        float cell_size = extent / std::cbrt(static_cast<float>(num_points)) * 2.0f;
        SpatialHash hash(positions, cell_size);

        for (size_t i = 0; i < num_points; ++i) {
            float px = positions[i*3+0];
            float py = positions[i*3+1];
            float pz = positions[i*3+2];

            auto candidates = hash.queryNeighbors(px, py, pz, 2);
            int radius = 2;
            while (static_cast<int>(candidates.size()) < k + 1 && radius < 16) {
                candidates = hash.queryNeighbors(px, py, pz, ++radius);
            }

            std::vector<float> dists;
            dists.reserve(candidates.size());
            for (size_t j : candidates) {
                if (j == i) continue;
                float dx = positions[j*3+0] - px;
                float dy = positions[j*3+1] - py;
                float dz = positions[j*3+2] - pz;
                dists.push_back(std::sqrt(dx*dx + dy*dy + dz*dz));
            }
            if (dists.empty()) continue;

            std::sort(dists.begin(), dists.end());
            int actual_k = std::min(k, static_cast<int>(dists.size()));
            for (int j = 0; j < actual_k; ++j) {
                distances[i] += dists[j];
            }
            distances[i] /= static_cast<float>(actual_k);
        }
    }
    
    return distances;
}

void computeSurfaceFrame(
    const float normal[3],
    float tangent[3],
    float bitangent[3]) {
    
    // Find a vector not parallel to normal
    float ref[3] = {1.0f, 0.0f, 0.0f};
    if (std::abs(normal[0]) > 0.9f) {
        ref[0] = 0.0f;
        ref[1] = 1.0f;
    }
    
    // tangent = cross(normal, ref)
    tangent[0] = normal[1] * ref[2] - normal[2] * ref[1];
    tangent[1] = normal[2] * ref[0] - normal[0] * ref[2];
    tangent[2] = normal[0] * ref[1] - normal[1] * ref[0];
    
    // Normalize tangent
    float len = std::sqrt(tangent[0]*tangent[0] + tangent[1]*tangent[1] + tangent[2]*tangent[2]);
    if (len > 0.0f) {
        tangent[0] /= len;
        tangent[1] /= len;
        tangent[2] /= len;
    }
    
    // bitangent = cross(normal, tangent)
    bitangent[0] = normal[1] * tangent[2] - normal[2] * tangent[1];
    bitangent[1] = normal[2] * tangent[0] - normal[0] * tangent[2];
    bitangent[2] = normal[0] * tangent[1] - normal[1] * tangent[0];
}

void surfaceFrameToQuaternion(
    const float normal[3],
    const float tangent[3],
    float quat[4]) {
    
    // Build rotation matrix from surface frame
    // Column 0: tangent, Column 1: bitangent, Column 2: normal
    float bitangent[3];
    bitangent[0] = normal[1] * tangent[2] - normal[2] * tangent[1];
    bitangent[1] = normal[2] * tangent[0] - normal[0] * tangent[2];
    bitangent[2] = normal[0] * tangent[1] - normal[1] * tangent[0];
    
    // Convert rotation matrix to quaternion
    float m00 = tangent[0], m01 = bitangent[0], m02 = normal[0];
    float m10 = tangent[1], m11 = bitangent[1], m12 = normal[1];
    float m20 = tangent[2], m21 = bitangent[2], m22 = normal[2];
    
    float trace = m00 + m11 + m22;
    
    if (trace > 0) {
        float s = 0.5f / std::sqrt(trace + 1.0f);
        quat[0] = 0.25f / s;  // w
        quat[1] = (m21 - m12) * s;  // x
        quat[2] = (m02 - m20) * s;  // y
        quat[3] = (m10 - m01) * s;  // z
    } else if (m00 > m11 && m00 > m22) {
        float s = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        quat[0] = (m21 - m12) / s;
        quat[1] = 0.25f * s;
        quat[2] = (m01 + m10) / s;
        quat[3] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        quat[0] = (m02 - m20) / s;
        quat[1] = (m01 + m10) / s;
        quat[2] = 0.25f * s;
        quat[3] = (m12 + m21) / s;
    } else {
        float s = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        quat[0] = (m10 - m01) / s;
        quat[1] = (m02 + m20) / s;
        quat[2] = (m12 + m21) / s;
        quat[3] = 0.25f * s;
    }
    
    // Normalize
    float len = std::sqrt(quat[0]*quat[0] + quat[1]*quat[1] + quat[2]*quat[2] + quat[3]*quat[3]);
    if (len > 0.0f) {
        quat[0] /= len;
        quat[1] /= len;
        quat[2] /= len;
        quat[3] /= len;
    }
}

std::vector<float> estimateNormals(
    const std::vector<float>& positions,
    int k_neighbors) {

    size_t num_points = positions.size() / 3;
    std::vector<float> normals(num_points * 3);

    // Default to Z-up everywhere; overwritten where a real normal is computed.
    for (size_t i = 0; i < num_points; ++i) {
        normals[i * 3 + 0] = 0.0f;
        normals[i * 3 + 1] = 0.0f;
        normals[i * 3 + 2] = 1.0f;
    }

    if (num_points == 0) {
        return normals;
    }

    // Build a spatial hash so k-NN is ~O(1) per point instead of O(n).
    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_y = min_x, max_y = max_x, min_z = min_x, max_z = max_x;
    for (size_t i = 0; i < num_points; ++i) {
        min_x = std::min(min_x, positions[i * 3 + 0]);
        max_x = std::max(max_x, positions[i * 3 + 0]);
        min_y = std::min(min_y, positions[i * 3 + 1]);
        max_y = std::max(max_y, positions[i * 3 + 1]);
        min_z = std::min(min_z, positions[i * 3 + 2]);
        max_z = std::max(max_z, positions[i * 3 + 2]);
    }
    float extent = std::max({max_x - min_x, max_y - min_y, max_z - min_z});
    if (extent < 1e-9f) {
        // Degenerate cloud (all points coincident): keep Z-up defaults.
        return normals;
    }
    float cell_size = extent / std::cbrt(static_cast<float>(num_points)) * 2.0f;
    SpatialHash hash(positions, cell_size);

    int k = std::max(3, k_neighbors);

    for (size_t i = 0; i < num_points; ++i) {
        float px = positions[i * 3 + 0];
        float py = positions[i * 3 + 1];
        float pz = positions[i * 3 + 2];

        // Gather candidate neighbors, expanding the search radius until we have
        // at least k of them (capped to avoid pathological scans on clusters).
        auto candidates = hash.queryNeighbors(px, py, pz, 2);
        int radius = 2;
        while (static_cast<int>(candidates.size()) < k && radius < 16) {
            candidates = hash.queryNeighbors(px, py, pz, ++radius);
        }

        // Compute squared distances and keep the k nearest (excluding self).
        std::vector<std::pair<float, size_t>> nbrs;
        nbrs.reserve(candidates.size());
        for (size_t j : candidates) {
            if (j == i) continue;
            float dx = positions[j * 3 + 0] - px;
            float dy = positions[j * 3 + 1] - py;
            float dz = positions[j * 3 + 2] - pz;
            nbrs.emplace_back(dx * dx + dy * dy + dz * dz, j);
        }
        if (nbrs.empty()) {
            continue;  // keep Z-up default for this isolated point
        }
        std::partial_sort(nbrs.begin(),
                          nbrs.begin() + std::min<int>(k, static_cast<int>(nbrs.size())),
                          nbrs.end());

        int knn = std::min<int>(k, static_cast<int>(nbrs.size()));
        if (knn < 3) {
            // Not enough neighbors to define a plane reliably.
            continue;
        }

        // Centroid of the neighborhood (including the query point).
        float cx = px, cy = py, cz = pz;
        for (int n = 0; n < knn; ++n) {
            size_t j = nbrs[n].second;
            cx += positions[j * 3 + 0];
            cy += positions[j * 3 + 1];
            cz += positions[j * 3 + 2];
        }
        float inv_count = 1.0f / static_cast<float>(knn + 1);
        cx *= inv_count;
        cy *= inv_count;
        cz *= inv_count;

        // Covariance matrix entries (symmetric).
        float sxx = 0, sxy = 0, sxz = 0, syy = 0, syz = 0, szz = 0;
        auto accumulate = [&](size_t j) {
            float dx = positions[j * 3 + 0] - cx;
            float dy = positions[j * 3 + 1] - cy;
            float dz = positions[j * 3 + 2] - cz;
            sxx += dx * dx; sxy += dx * dy; sxz += dx * dz;
            syy += dy * dy; syz += dy * dz;
            szz += dz * dz;
        };
        accumulate(i);
        for (int n = 0; n < knn; ++n) {
            accumulate(nbrs[n].second);
        }
        sxx *= inv_count; sxy *= inv_count; sxz *= inv_count;
        syy *= inv_count; syz *= inv_count; szz *= inv_count;

        // Smallest-eigenvalue eigenvector of the symmetric 3x3 covariance via
        // the analytical method. The normal is the direction of least variance,
        // i.e. the eigenvector for the smallest eigenvalue.
        float nx, ny, nz;
        smallestEigenvector3x3(sxx, sxy, sxz, syy, syz, szz, nx, ny, nz);

        // Orient the normal consistently: point it toward the "outside" of the
        // cloud, approximated by the vector from the centroid to the query
        // point. Without a sensor viewpoint this is the best local heuristic.
        float ox = px - cx, oy = py - cy, oz = pz - cz;
        if (nx * ox + ny * oy + nz * oz < 0.0f) {
            nx = -nx;
            ny = -ny;
            nz = -nz;
        }

        normals[i * 3 + 0] = nx;
        normals[i * 3 + 1] = ny;
        normals[i * 3 + 2] = nz;
    }

    return normals;
}

} // namespace enhanced
} // namespace melkor
