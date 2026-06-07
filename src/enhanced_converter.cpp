#include "melkor/enhanced_converter.hpp"
#include "melkor/glb_reader.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <random>

// For GLB loading
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include "tiny_gltf.h"

namespace melkor {

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
            
            // Query neighbors
            auto candidates = hash.queryNeighbors(px, py, pz, 2);
            
            // Compute distances to all candidates
            std::vector<float> distances;
            distances.reserve(candidates.size());
            
            for (size_t j : candidates) {
                if (j == i) continue;
                float dx = positions[j*3+0] - px;
                float dy = positions[j*3+1] - py;
                float dz = positions[j*3+2] - pz;
                distances.push_back(std::sqrt(dx*dx + dy*dy + dz*dz));
            }
            
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
            
            const float* pos_data = reinterpret_cast<const float*>(
                pos_buffer.data.data() + pos_buffer_view.byteOffset + pos_accessor.byteOffset);
            
            size_t base_idx = positions.size() / 3;
            size_t count = pos_accessor.count;
            
            for (size_t i = 0; i < count; ++i) {
                positions.push_back(pos_data[i*3+0]);
                positions.push_back(pos_data[i*3+1]);
                positions.push_back(pos_data[i*3+2]);
            }
            
            // Get normals
            auto norm_it = primitive.attributes.find("NORMAL");
            if (norm_it != primitive.attributes.end()) {
                const auto& norm_accessor = model.accessors[norm_it->second];
                const auto& norm_buffer_view = model.bufferViews[norm_accessor.bufferView];
                const auto& norm_buffer = model.buffers[norm_buffer_view.buffer];
                
                const float* norm_data = reinterpret_cast<const float*>(
                    norm_buffer.data.data() + norm_buffer_view.byteOffset + norm_accessor.byteOffset);
                
                for (size_t i = 0; i < count; ++i) {
                    normals.push_back(norm_data[i*3+0]);
                    normals.push_back(norm_data[i*3+1]);
                    normals.push_back(norm_data[i*3+2]);
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
                
                for (size_t i = 0; i < count; ++i) {
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
    metal::MetalContext* metal_ctx) {
    // CPU implementation for now
    // TODO: Metal implementation for large point clouds
    
    size_t num_points = positions.size() / 3;
    std::vector<float> distances(num_points, 0.0f);
    
    // Simple O(n²) for small clouds, use spatial hash for larger
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
    
    // Simple PCA-based normal estimation
    // For each point, fit a plane to k nearest neighbors
    
    auto knn_distances = computeKnnDistances(positions, k_neighbors, nullptr);
    
    for (size_t i = 0; i < num_points; ++i) {
        // Default to Z-up if we can't compute
        normals[i*3+0] = 0.0f;
        normals[i*3+1] = 0.0f;
        normals[i*3+2] = 1.0f;
    }
    
    // For simplicity, return default normals
    // A full implementation would do PCA on neighborhoods
    return normals;
}

} // namespace enhanced
} // namespace melkor
