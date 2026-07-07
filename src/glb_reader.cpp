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
        
        // Count total vertices first for reservation. Only count accessors
        // that pass validation so a crafted count can't force a huge reserve.
        size_t total_verts = 0;
        for (const auto& mesh : model.meshes) {
            for (const auto& primitive : mesh.primitives) {
                auto pos_it = primitive.attributes.find("POSITION");
                if (pos_it != primitive.attributes.end()) {
                    size_t stride = 0;
                    if (validateAndGetAccessorData(model, pos_it->second, stride)) {
                        total_verts += model.accessors[pos_it->second].count;
                    }
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
        // If every primitive was rejected (or the file has none), report
        // failure instead of handing back an empty cloud with success=true —
        // callers would otherwise write empty output files with exit code 0.
        if (result.cloud.empty()) {
            result.success = false;
            if (result.error_message.empty()) {
                result.error_message = "No usable vertex data found in glTF";
            }
        }
        return result;
    }
    
private:
    static void appendError(GlbLoadResult& result, const std::string& message) {
        if (!result.error_message.empty()) {
            result.error_message += "; ";
        }
        result.error_message += message;
    }

    // Validates an attribute accessor index against untrusted GLB data:
    // accessor index, bufferView index (bufferView == -1, legal for sparse/
    // zero-filled accessors, is not supported here), buffer index, a positive
    // byte stride (ByteStride() returns -1 for invalid combinations), and
    // that every element lies within the buffer view and the buffer view
    // lies within the buffer. Returns a pointer to the first element and
    // writes the stride to out_stride on success; returns nullptr otherwise.
    static const uint8_t* validateAndGetAccessorData(const tinygltf::Model& model,
                                                     int accessor_index,
                                                     size_t& out_stride) {
        if (accessor_index < 0 ||
            static_cast<size_t>(accessor_index) >= model.accessors.size()) {
            return nullptr;
        }
        const auto& accessor = model.accessors[accessor_index];

        if (accessor.bufferView < 0 ||
            static_cast<size_t>(accessor.bufferView) >= model.bufferViews.size()) {
            return nullptr;
        }
        const auto& buffer_view = model.bufferViews[accessor.bufferView];

        if (buffer_view.buffer < 0 ||
            static_cast<size_t>(buffer_view.buffer) >= model.buffers.size()) {
            return nullptr;
        }
        const auto& buffer = model.buffers[buffer_view.buffer];

        // Buffer view must lie within the buffer
        if (buffer_view.byteOffset > buffer.data.size() ||
            buffer_view.byteLength > buffer.data.size() - buffer_view.byteOffset) {
            return nullptr;
        }

        const int stride = accessor.ByteStride(buffer_view);
        if (stride <= 0) {
            return nullptr;
        }

        const int component_size = tinygltf::GetComponentSizeInBytes(
            static_cast<uint32_t>(accessor.componentType));
        const int num_components = tinygltf::GetNumComponentsInType(
            static_cast<uint32_t>(accessor.type));
        if (component_size <= 0 || num_components <= 0) {
            return nullptr;
        }
        const size_t element_size = static_cast<size_t>(component_size) *
                                    static_cast<size_t>(num_components);
        if (static_cast<size_t>(stride) < element_size) {
            return nullptr;
        }

        // All elements must lie within the buffer view:
        // byteOffset + (count - 1) * stride + element_size <= byteLength
        // (rearranged to avoid overflow)
        if (accessor.byteOffset > buffer_view.byteLength) {
            return nullptr;
        }
        const size_t available = buffer_view.byteLength - accessor.byteOffset;
        if (accessor.count > 0) {
            if (element_size > available ||
                accessor.count - 1 >
                    (available - element_size) / static_cast<size_t>(stride)) {
                return nullptr;
            }
        }

        out_stride = static_cast<size_t>(stride);
        return buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset;
    }

    void processGltfPrimitive(const tinygltf::Model& model,
                              const tinygltf::Primitive& primitive,
                              const GlbConversionConfig& config,
                              GlbLoadResult& result) {
        // Get position accessor
        auto pos_it = primitive.attributes.find("POSITION");
        if (pos_it == primitive.attributes.end()) {
            return;
        }
        
        size_t pos_stride = 0;
        const uint8_t* pos_data =
            validateAndGetAccessorData(model, pos_it->second, pos_stride);
        if (pos_data == nullptr) {
            appendError(result, "Skipped primitive: POSITION accessor references "
                                "out-of-range or inconsistent buffer data");
            return;
        }

        const auto& pos_accessor = model.accessors[pos_it->second];
        if (pos_accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
            pos_accessor.type != TINYGLTF_TYPE_VEC3) {
            appendError(result, "Skipped primitive: POSITION accessor must be "
                                "float VEC3");
            return;
        }

        // Try to get color accessor. Invalid or unsupported color data is
        // ignored (the default color is used instead).
        const uint8_t* color_data = nullptr;
        size_t color_stride = 0;
        int color_component_type = TINYGLTF_COMPONENT_TYPE_FLOAT;

        if (config.use_vertex_colors) {
            auto color_it = primitive.attributes.find("COLOR_0");
            if (color_it != primitive.attributes.end()) {
                size_t stride = 0;
                const uint8_t* data =
                    validateAndGetAccessorData(model, color_it->second, stride);
                if (data != nullptr) {
                    const auto& color_accessor = model.accessors[color_it->second];
                    const bool supported_type =
                        color_accessor.type == TINYGLTF_TYPE_VEC3 ||
                        color_accessor.type == TINYGLTF_TYPE_VEC4;
                    const bool supported_component =
                        color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ||
                        color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
                        color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
                    if (supported_type && supported_component &&
                        color_accessor.count >= pos_accessor.count) {
                        color_data = data;
                        color_stride = stride;
                        color_component_type = color_accessor.componentType;
                    }
                }
            }
        }

        // Try to get normal accessor for orientation. Invalid or non-float
        // normals are ignored (the identity quaternion is used instead).
        const uint8_t* normal_data = nullptr;
        size_t normal_stride = 0;

        auto normal_it = primitive.attributes.find("NORMAL");
        if (normal_it != primitive.attributes.end()) {
            size_t stride = 0;
            const uint8_t* data =
                validateAndGetAccessorData(model, normal_it->second, stride);
            if (data != nullptr) {
                const auto& normal_accessor = model.accessors[normal_it->second];
                if (normal_accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT &&
                    normal_accessor.type == TINYGLTF_TYPE_VEC3 &&
                    normal_accessor.count >= pos_accessor.count) {
                    normal_data = data;
                    normal_stride = stride;
                }
            }
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
