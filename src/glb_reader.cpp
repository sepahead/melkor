#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "tiny_gltf.h"

#include "melkor/glb_reader.hpp"
#include <fstream>
#include <cstring>

namespace melkor {

class GlbReader::Impl {
public:
    tinygltf::TinyGLTF loader;
    
    GlbLoadResult processModel(const tinygltf::Model& model,
                               const GlbConversionConfig& config) {
        GlbLoadResult result;
        result.success = true;
        result.total_meshes = model.meshes.size();
        
        // Count total vertices first for reservation
        size_t total_verts = 0;
        for (const auto& mesh : model.meshes) {
            for (const auto& primitive : mesh.primitives) {
                auto pos_it = primitive.attributes.find("POSITION");
                if (pos_it != primitive.attributes.end()) {
                    const auto& accessor = model.accessors[pos_it->second];
                    total_verts += accessor.count;
                }
                result.total_primitives++;
            }
        }
        
        result.cloud.reserve(total_verts);
        
        // Process each mesh
        for (const auto& mesh : model.meshes) {
            for (const auto& primitive : mesh.primitives) {
                processGltfPrimitive(model, primitive, config, result);
            }
        }
        
        result.total_vertices = result.cloud.size();
        return result;
    }
    
private:
    void processGltfPrimitive(const tinygltf::Model& model,
                              const tinygltf::Primitive& primitive,
                              const GlbConversionConfig& config,
                              GlbLoadResult& result) {
        // Get position accessor
        auto pos_it = primitive.attributes.find("POSITION");
        if (pos_it == primitive.attributes.end()) {
            return;
        }
        
        const auto& pos_accessor = model.accessors[pos_it->second];
        const auto& pos_buffer_view = model.bufferViews[pos_accessor.bufferView];
        const auto& pos_buffer = model.buffers[pos_buffer_view.buffer];
        
        const uint8_t* pos_data = pos_buffer.data.data() + 
                                   pos_buffer_view.byteOffset + 
                                   pos_accessor.byteOffset;
        
        size_t pos_stride = pos_accessor.ByteStride(pos_buffer_view);
        if (pos_stride == 0) pos_stride = sizeof(float) * 3;
        
        // Try to get color accessor
        const uint8_t* color_data = nullptr;
        size_t color_stride = 0;
        int color_type = -1;  // -1: none, 0: VEC3, 1: VEC4
        int color_component_type = TINYGLTF_COMPONENT_TYPE_FLOAT;
        
        if (config.use_vertex_colors) {
            auto color_it = primitive.attributes.find("COLOR_0");
            if (color_it != primitive.attributes.end()) {
                const auto& color_accessor = model.accessors[color_it->second];
                const auto& color_buffer_view = model.bufferViews[color_accessor.bufferView];
                const auto& color_buffer = model.buffers[color_buffer_view.buffer];
                
                color_data = color_buffer.data.data() + 
                            color_buffer_view.byteOffset + 
                            color_accessor.byteOffset;
                
                color_stride = color_accessor.ByteStride(color_buffer_view);
                color_type = (color_accessor.type == TINYGLTF_TYPE_VEC4) ? 1 : 0;
                color_component_type = color_accessor.componentType;
                
                if (color_stride == 0) {
                    int num_components = (color_type == 1) ? 4 : 3;
                    if (color_component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        color_stride = num_components;
                    } else if (color_component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        color_stride = num_components * 2;
                    } else {
                        color_stride = num_components * sizeof(float);
                    }
                }
            }
        }
        
        // Try to get normal accessor for orientation
        const uint8_t* normal_data = nullptr;
        size_t normal_stride = 0;
        
        auto normal_it = primitive.attributes.find("NORMAL");
        if (normal_it != primitive.attributes.end()) {
            const auto& normal_accessor = model.accessors[normal_it->second];
            const auto& normal_buffer_view = model.bufferViews[normal_accessor.bufferView];
            const auto& normal_buffer = model.buffers[normal_buffer_view.buffer];
            
            normal_data = normal_buffer.data.data() + 
                         normal_buffer_view.byteOffset + 
                         normal_accessor.byteOffset;
            
            normal_stride = normal_accessor.ByteStride(normal_buffer_view);
            if (normal_stride == 0) normal_stride = sizeof(float) * 3;
        }
        
        // Process each vertex
        for (size_t i = 0; i < pos_accessor.count; ++i) {
            GaussianSplat splat;
            
            // Position
            const float* pos = reinterpret_cast<const float*>(pos_data + i * pos_stride);
            
            if (config.convert_coordinate_system) {
                // Convert from Y-up (glTF) to Z-up (common for 3DGS)
                splat.x = pos[0] * config.position_scale;
                splat.y = -pos[2] * config.position_scale;  // -Z becomes Y
                splat.z = pos[1] * config.position_scale;   // Y becomes Z
            } else {
                splat.x = pos[0] * config.position_scale;
                splat.y = pos[1] * config.position_scale;
                splat.z = pos[2] * config.position_scale;
            }
            
            // Color -> SH DC coefficients
            float r, g, b;
            if (color_data) {
                const uint8_t* color_ptr = color_data + i * color_stride;
                
                if (color_component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                    r = color_ptr[0] / 255.0f;
                    g = color_ptr[1] / 255.0f;
                    b = color_ptr[2] / 255.0f;
                } else if (color_component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    const uint16_t* c16 = reinterpret_cast<const uint16_t*>(color_ptr);
                    r = c16[0] / 65535.0f;
                    g = c16[1] / 65535.0f;
                    b = c16[2] / 65535.0f;
                } else {
                    const float* cf = reinterpret_cast<const float*>(color_ptr);
                    r = cf[0];
                    g = cf[1];
                    b = cf[2];
                }
            } else {
                r = config.default_color[0];
                g = config.default_color[1];
                b = config.default_color[2];
            }
            
            // Convert RGB to spherical harmonics DC coefficient
            splat.f_dc_0 = utils::rgbToShDc(r);
            splat.f_dc_1 = utils::rgbToShDc(g);
            splat.f_dc_2 = utils::rgbToShDc(b);
            
            // Opacity (in logit space)
            splat.opacity = utils::logit(std::clamp(config.default_opacity, 0.001f, 0.999f));
            
            // Scale (in log space)
            float log_scale = std::log(config.default_scale);
            splat.scale_0 = log_scale;
            splat.scale_1 = log_scale;
            splat.scale_2 = log_scale;
            
            // Rotation quaternion from normal (if available)
            if (normal_data) {
                const float* n = reinterpret_cast<const float*>(normal_data + i * normal_stride);
                float nx = n[0], ny = n[1], nz = n[2];
                
                if (config.convert_coordinate_system) {
                    float tmp = ny;
                    ny = -nz;
                    nz = tmp;
                }
                
                // Create quaternion that rotates Z-axis to normal
                // Using half-angle formula
                float dot = nz;  // dot(normal, z_axis)
                if (dot > 0.9999f) {
                    // Normal is already Z-up
                    splat.rot_0 = 1.0f;  // w
                    splat.rot_1 = 0.0f;  // x
                    splat.rot_2 = 0.0f;  // y
                    splat.rot_3 = 0.0f;  // z
                } else if (dot < -0.9999f) {
                    // Normal is Z-down, rotate 180 around X
                    splat.rot_0 = 0.0f;
                    splat.rot_1 = 1.0f;
                    splat.rot_2 = 0.0f;
                    splat.rot_3 = 0.0f;
                } else {
                    // Cross product of Z-axis and normal gives rotation axis
                    float ax = -ny;  // cross(z, normal).x
                    float ay = nx;   // cross(z, normal).y
                    float az = 0.0f; // cross(z, normal).z
                    
                    float s = std::sqrt((1.0f + dot) * 2.0f);
                    float inv_s = 1.0f / s;
                    
                    splat.rot_0 = s * 0.5f;        // w
                    splat.rot_1 = ax * inv_s;      // x
                    splat.rot_2 = ay * inv_s;      // y
                    splat.rot_3 = az * inv_s;      // z
                }
                
                // Normalize
                utils::normalizeQuaternion(splat.rot_0, splat.rot_1, 
                                          splat.rot_2, splat.rot_3);
            } else {
                // Identity quaternion
                splat.rot_0 = 1.0f;
                splat.rot_1 = 0.0f;
                splat.rot_2 = 0.0f;
                splat.rot_3 = 0.0f;
            }
            
            result.cloud.addSplat(std::move(splat));
        }
    }
};

GlbReader::GlbReader() : impl_(std::make_unique<Impl>()) {}
GlbReader::~GlbReader() = default;

GlbLoadResult GlbReader::loadFromFile(const std::string& filepath,
                                      const GlbConversionConfig& config) {
    GlbLoadResult result;
    
    tinygltf::Model model;
    std::string err, warn;
    bool success;
    
    // Check file extension
    if (filepath.size() >= 4 && 
        filepath.substr(filepath.size() - 4) == ".glb") {
        success = impl_->loader.LoadBinaryFromFile(&model, &err, &warn, filepath);
    } else {
        success = impl_->loader.LoadASCIIFromFile(&model, &err, &warn, filepath);
    }
    
    if (!warn.empty()) {
        // Just warnings, continue
    }
    
    if (!success) {
        result.success = false;
        result.error_message = err;
        return result;
    }
    
    return impl_->processModel(model, config);
}

GlbLoadResult GlbReader::loadFromMemory(const uint8_t* data, size_t size,
                                        const GlbConversionConfig& config) {
    GlbLoadResult result;
    
    tinygltf::Model model;
    std::string err, warn;
    
    // Try binary first (GLB magic: 0x46546C67)
    bool is_glb = size >= 4 && 
                  data[0] == 0x67 && data[1] == 0x6C && 
                  data[2] == 0x54 && data[3] == 0x46;
    
    bool success;
    if (is_glb) {
        success = impl_->loader.LoadBinaryFromMemory(&model, &err, &warn,
                                                     data, static_cast<unsigned int>(size));
    } else {
        std::string json_str(reinterpret_cast<const char*>(data), size);
        success = impl_->loader.LoadASCIIFromString(&model, &err, &warn,
                                                    json_str.c_str(),
                                                    static_cast<unsigned int>(size), "");
    }
    
    if (!success) {
        result.success = false;
        result.error_message = err;
        return result;
    }
    
    return impl_->processModel(model, config);
}

} // namespace melkor
