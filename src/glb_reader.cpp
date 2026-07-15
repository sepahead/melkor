#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "tiny_gltf.h"

#include "melkor/glb_reader.hpp"
#include "melkor/format/gltf_khr.hpp"
#include "melkor/math/color.hpp"
#include "melkor/math/quaternion.hpp"
#include "gltf_scene_utils.hpp"
#include "safe_gltf_fs.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <cstring>
#include <limits>

namespace melkor {

namespace {

template <typename T> T readUnaligned(const uint8_t* data) {
    T value{};
    std::memcpy(&value, data, sizeof(value));
    return value;
}

}  // namespace

class GlbReader::Impl {
public:
    tinygltf::TinyGLTF loader;
    gltf_fs::Context fs_context;

    Impl() {
        std::string ignored;
        loader.SetFsCallbacks(gltf_fs::callbacks(fs_context), &ignored);
    }

    bool setInputRoot(const std::string& filepath, std::string& error) {
        return gltf_fs::setRootForInput(fs_context, filepath, error);
    }

    GlbLoadResult processModel(const tinygltf::Model& model, const GlbConversionConfig& config) {
        GlbLoadResult result;
        if (const std::string contract_error = gltf_scene::validateModelContract(model);
            !contract_error.empty()) {
            result.error_message = contract_error;
            return result;
        }
        const auto traversal = gltf_scene::activeMeshInstances(model);
        if (!traversal.success()) {
            result.error_message = traversal.error;
            return result;
        }
        const auto& instances = traversal.instances;
        result.total_meshes = instances.size();

        // This reader intentionally converts ordinary mesh vertices into degree-zero splats. A
        // KHR_gaussian_splatting primitive is already a real splat scene with explicit scale,
        // opacity, rotation, and SH attributes; treating it as a mesh would silently discard all
        // of those fields. The canonical KHR reader owns that profile, and cross-format routing is
        // gated on the format registry (A3/WP13).
        for (const auto& instance : instances) {
            const auto& mesh = model.meshes[instance.mesh_index];
            for (const auto& primitive : mesh.primitives) {
                if (primitive.extensions.count(format::khr::kExtensionName) != 0) {
                    result.error_message =
                        "KHR_gaussian_splatting is not a legacy mesh; use 'melkor convert' for "
                        "GLB-to-GLB conversion until cross-format routing lands (A3/WP13)";
                    return result;
                }
            }
        }

        if (!std::isfinite(config.default_scale) || config.default_scale <= 0.0f ||
            !std::isfinite(config.default_opacity) || config.default_opacity < 0.0f ||
            config.default_opacity > 1.0f || !std::isfinite(config.position_scale) ||
            config.position_scale <= 0.0f || !std::isfinite(config.default_color[0]) ||
            !std::isfinite(config.default_color[1]) || !std::isfinite(config.default_color[2]) ||
            config.default_color[0] < 0.0f || config.default_color[0] > 1.0f ||
            config.default_color[1] < 0.0f || config.default_color[1] > 1.0f ||
            config.default_color[2] < 0.0f || config.default_color[2] > 1.0f) {
            result.error_message = "Invalid conversion configuration";
            return result;
        }
        result.success = true;

        // Count total vertices first for reservation. Only count accessors
        // that pass validation so a crafted count can't force a huge reserve.
        size_t total_verts = 0;
        for (const auto& instance : instances) {
            const auto& mesh = model.meshes[instance.mesh_index];
            for (const auto& primitive : mesh.primitives) {
                auto pos_it = primitive.attributes.find("POSITION");
                if (pos_it != primitive.attributes.end()) {
                    size_t stride = 0;
                    if (validateAndGetAccessorData(model, pos_it->second, stride)) {
                        const size_t count = model.accessors[pos_it->second].count;
                        if (count > std::numeric_limits<size_t>::max() - total_verts) {
                            result.success = false;
                            result.error_message = "Vertex count overflow";
                            return result;
                        }
                        total_verts += count;
                    }
                }
                result.total_primitives++;
            }
        }

        SplatBufferInput canonical_input;
        canonical_input.positions.reserve(total_verts);
        canonical_input.scales.reserve(total_verts);
        canonical_input.rotations.reserve(total_verts);
        canonical_input.opacities.reserve(total_verts);
        std::vector<float> sh_values;
        if (total_verts > std::numeric_limits<size_t>::max() / 3) {
            result.success = false;
            result.error_message = "SH allocation size overflow";
            return result;
        }
        sh_values.reserve(total_verts * 3);

        // Process each mesh
        for (const auto& instance : instances) {
            const auto& mesh = model.meshes[instance.mesh_index];
            for (const auto& primitive : mesh.primitives) {
                processGltfPrimitive(model, primitive, instance.world, config, result,
                                     canonical_input, sh_values);
            }
        }
        result.total_vertices = canonical_input.positions.size();
        // If every primitive was rejected (or the file has none), report
        // failure instead of handing back an empty cloud with success=true —
        // callers would otherwise write empty output files with exit code 0.
        if (canonical_input.positions.empty()) {
            result.success = false;
            if (result.error_message.empty()) {
                result.error_message = "No usable vertex data found in glTF";
            }
            return result;
        }

        auto sh = ShBuffer::create(0, canonical_input.positions.size(), std::move(sh_values));
        if (!sh.has_value()) {
            result.success = false;
            result.error_message = sh.diagnostics().empty()
                                       ? "Could not construct canonical SH data"
                                       : sh.diagnostics()[0].message;
            return result;
        }
        canonical_input.sh = std::move(sh).value();
        auto canonical = SplatData::create(std::move(canonical_input));
        if (!canonical.has_value()) {
            result.success = false;
            result.error_message = canonical.diagnostics().empty()
                                       ? "Mesh-derived splats violate canonical invariants"
                                       : canonical.diagnostics()[0].message;
            return result;
        }
        result.data.emplace(std::move(canonical).value());

        if (!result.error_message.empty()) {
            // A validator/converter must never present a partially decoded
            // asset as wholly valid. Callers may inspect the retained cloud for
            // diagnostics, but writing a lossy output requires an explicit fix
            // to the source asset first.
            result.success = false;
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
                                                     int accessor_index, size_t& out_stride) {
        if (accessor_index < 0 || static_cast<size_t>(accessor_index) >= model.accessors.size()) {
            return nullptr;
        }
        const auto& accessor = model.accessors[accessor_index];

        // Sparse accessors may also carry a base bufferView. Reading only that
        // base silently discards every sparse override and produces a valid-
        // looking but incorrect cloud. Reject the whole accessor until sparse
        // overlay materialization is implemented.
        if (accessor.sparse.isSparse) {
            return nullptr;
        }

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

        const int component_size =
            tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
        const int num_components =
            tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
        if (component_size <= 0 || num_components <= 0) {
            return nullptr;
        }
        const size_t element_size =
            static_cast<size_t>(component_size) * static_cast<size_t>(num_components);
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
                accessor.count - 1 > (available - element_size) / static_cast<size_t>(stride)) {
                return nullptr;
            }
        }

        out_stride = static_cast<size_t>(stride);
        return buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset;
    }

    void processGltfPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
                              const gltf_scene::Matrix& world, const GlbConversionConfig& config,
                              GlbLoadResult& result, SplatBufferInput& canonical_input,
                              std::vector<float>& sh_values) {
        // Get position accessor
        auto pos_it = primitive.attributes.find("POSITION");
        if (pos_it == primitive.attributes.end()) {
            appendError(result, "Skipped primitive: POSITION attribute is missing");
            return;
        }

        size_t pos_stride = 0;
        const uint8_t* pos_data = validateAndGetAccessorData(model, pos_it->second, pos_stride);
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

        // Optional attributes may be absent, but when present they are part of
        // the source contract. Reject malformed data instead of silently
        // treating a damaged accessor as an absent attribute.
        const uint8_t* color_data = nullptr;
        size_t color_stride = 0;
        int color_component_type = TINYGLTF_COMPONENT_TYPE_FLOAT;

        if (config.use_vertex_colors) {
            auto color_it = primitive.attributes.find("COLOR_0");
            if (color_it != primitive.attributes.end()) {
                size_t stride = 0;
                const uint8_t* data = validateAndGetAccessorData(model, color_it->second, stride);
                if (data == nullptr) {
                    appendError(result, "Skipped primitive: COLOR_0 accessor references "
                                        "out-of-range or inconsistent buffer data");
                    return;
                }
                const auto& color_accessor = model.accessors[color_it->second];
                const bool supported_type = color_accessor.type == TINYGLTF_TYPE_VEC3 ||
                                            color_accessor.type == TINYGLTF_TYPE_VEC4;
                const bool supported_component =
                    color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ||
                    color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
                    color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
                const bool valid_normalization =
                    color_accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ||
                    color_accessor.normalized;
                if (!supported_type || !supported_component || !valid_normalization ||
                    color_accessor.count != pos_accessor.count) {
                    appendError(result, "Skipped primitive: COLOR_0 accessor has an "
                                        "unsupported type, normalization, or count");
                    return;
                }
                color_data = data;
                color_stride = stride;
                color_component_type = color_accessor.componentType;
            }
        }

        // A missing normal is allowed and uses identity orientation. A normal
        // attribute that exists must be a complete finite float VEC3 array.
        const uint8_t* normal_data = nullptr;
        size_t normal_stride = 0;

        auto normal_it = primitive.attributes.find("NORMAL");
        if (normal_it != primitive.attributes.end()) {
            size_t stride = 0;
            const uint8_t* data = validateAndGetAccessorData(model, normal_it->second, stride);
            if (data == nullptr) {
                appendError(result, "Skipped primitive: NORMAL accessor references "
                                    "out-of-range or inconsistent buffer data");
                return;
            }
            const auto& normal_accessor = model.accessors[normal_it->second];
            if (normal_accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                normal_accessor.type != TINYGLTF_TYPE_VEC3 ||
                normal_accessor.count != pos_accessor.count) {
                appendError(result, "Skipped primitive: NORMAL accessor must be a complete "
                                    "float VEC3 array");
                return;
            }
            normal_data = data;
            normal_stride = stride;
        }

        // Process each vertex
        size_t rejected_positions = 0;
        for (size_t i = 0; i < pos_accessor.count; ++i) {
            Vec3f canonical_position;

            // Position
            const uint8_t* pos = pos_data + i * pos_stride;
            const float local_position[3] = {
                readUnaligned<float>(pos + 0 * sizeof(float)),
                readUnaligned<float>(pos + 1 * sizeof(float)),
                readUnaligned<float>(pos + 2 * sizeof(float)),
            };
            float transformed_position[3];
            if (!std::isfinite(local_position[0]) || !std::isfinite(local_position[1]) ||
                !std::isfinite(local_position[2]) ||
                !gltf_scene::transformPoint(world, local_position, transformed_position)) {
                ++rejected_positions;
                continue;
            }
            const float px = transformed_position[0];
            const float py = transformed_position[1];
            const float pz = transformed_position[2];

            if (config.convert_coordinate_system) {
                // Convert from Y-up (glTF) to Z-up (common for 3DGS)
                canonical_position.x = px * config.position_scale;
                canonical_position.y = -pz * config.position_scale;  // -Z becomes Y
                canonical_position.z = py * config.position_scale;   // Y becomes Z
            } else {
                canonical_position.x = px * config.position_scale;
                canonical_position.y = py * config.position_scale;
                canonical_position.z = pz * config.position_scale;
            }
            if (!std::isfinite(canonical_position.x) || !std::isfinite(canonical_position.y) ||
                !std::isfinite(canonical_position.z)) {
                ++rejected_positions;
                continue;
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
                    r = readUnaligned<uint16_t>(color_ptr + 0 * sizeof(uint16_t)) / 65535.0f;
                    g = readUnaligned<uint16_t>(color_ptr + 1 * sizeof(uint16_t)) / 65535.0f;
                    b = readUnaligned<uint16_t>(color_ptr + 2 * sizeof(uint16_t)) / 65535.0f;
                } else {
                    r = readUnaligned<float>(color_ptr + 0 * sizeof(float));
                    g = readUnaligned<float>(color_ptr + 1 * sizeof(float));
                    b = readUnaligned<float>(color_ptr + 2 * sizeof(float));
                }
                if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
                    appendError(result, "Skipped primitive: COLOR_0 contains a non-finite value");
                    return;
                }
                r = std::clamp(r, 0.0f, 1.0f);
                g = std::clamp(g, 0.0f, 1.0f);
                b = std::clamp(b, 0.0f, 1.0f);
            } else {
                r = config.default_color[0];
                g = config.default_color[1];
                b = config.default_color[2];
            }

            Quatf canonical_rotation;

            // Rotation quaternion from normal (if available)
            if (normal_data) {
                const uint8_t* n = normal_data + i * normal_stride;
                const float local_normal[3] = {
                    readUnaligned<float>(n + 0 * sizeof(float)),
                    readUnaligned<float>(n + 1 * sizeof(float)),
                    readUnaligned<float>(n + 2 * sizeof(float)),
                };
                float transformed_normal[3] = {0.0f, 0.0f, 1.0f};
                if (!std::isfinite(local_normal[0]) || !std::isfinite(local_normal[1]) ||
                    !std::isfinite(local_normal[2]) ||
                    !gltf_scene::transformNormal(world, local_normal, transformed_normal)) {
                    appendError(result, "Skipped primitive: NORMAL contains a non-finite or "
                                        "non-transformable value");
                    return;
                }
                float nx = transformed_normal[0];
                float ny = transformed_normal[1];
                float nz = transformed_normal[2];

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
                    canonical_rotation = {};
                } else if (dot < -0.9999f) {
                    // Normal is Z-down, rotate 180 around X
                    canonical_rotation = {1.0f, 0.0f, 0.0f, 0.0f};
                } else {
                    // Cross product of Z-axis and normal gives rotation axis
                    float ax = -ny;   // cross(z, normal).x
                    float ay = nx;    // cross(z, normal).y
                    float az = 0.0f;  // cross(z, normal).z

                    float s = std::sqrt((1.0f + dot) * 2.0f);
                    float inv_s = 1.0f / s;

                    canonical_rotation = {ax * inv_s, ay * inv_s, az * inv_s, s * 0.5f};
                }

                auto normalized = math::normalize({canonical_rotation.x, canonical_rotation.y,
                                                   canonical_rotation.z, canonical_rotation.w});
                if (!normalized.has_value()) {
                    appendError(result, "Skipped primitive: NORMAL produced an invalid rotation");
                    return;
                }
                const auto rotation = normalized.value();
                canonical_rotation = {
                    static_cast<float>(rotation.x), static_cast<float>(rotation.y),
                    static_cast<float>(rotation.z), static_cast<float>(rotation.w)};
            }

            canonical_input.positions.push_back(canonical_position);
            canonical_input.scales.push_back(
                {config.default_scale, config.default_scale, config.default_scale});
            canonical_input.rotations.push_back(canonical_rotation);
            canonical_input.opacities.push_back(config.default_opacity);
            sh_values.push_back(math::rgb_to_sh_dc(r));
            sh_values.push_back(math::rgb_to_sh_dc(g));
            sh_values.push_back(math::rgb_to_sh_dc(b));
        }
        if (rejected_positions > 0) {
            appendError(result, "Rejected " + std::to_string(rejected_positions) +
                                    " non-finite or non-transformable POSITION vertices");
        }
    }
};

GlbReader::GlbReader() : impl_(std::make_unique<Impl>()) {}
GlbReader::~GlbReader() = default;

GlbLoadResult GlbReader::loadFromFile(const std::string& filepath,
                                      const GlbConversionConfig& config) {
    GlbLoadResult result;
    if (!impl_->setInputRoot(filepath, result.error_message))
        return result;

    tinygltf::Model model;
    std::string err, warn;
    bool success;

    // Detect the container by its standardized magic, not a case-sensitive
    // filename suffix. This accepts uppercase and extensionless GLBs without
    // misrouting JSON glTF through the binary loader.
    if (gltf_scene::fileHasGlbMagic(filepath)) {
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

    // In-memory documents have no trusted asset base directory. Never inherit
    // one from an earlier file load on the same reader instance.
    impl_->fs_context.root.clear();

    if (data == nullptr || size == 0 ||
        size > static_cast<size_t>(std::numeric_limits<unsigned int>::max())) {
        result.error_message = "Invalid or oversized in-memory glTF buffer";
        return result;
    }

    tinygltf::Model model;
    std::string err, warn;

    // Try binary first (GLB magic: 0x46546C67)
    bool is_glb =
        size >= 4 && data[0] == 0x67 && data[1] == 0x6C && data[2] == 0x54 && data[3] == 0x46;

    bool success;
    if (is_glb) {
        success = impl_->loader.LoadBinaryFromMemory(&model, &err, &warn, data,
                                                     static_cast<unsigned int>(size));
    } else {
        std::string json_str(reinterpret_cast<const char*>(data), size);
        success = impl_->loader.LoadASCIIFromString(&model, &err, &warn, json_str.c_str(),
                                                    static_cast<unsigned int>(size), "");
    }

    if (!success) {
        result.success = false;
        result.error_message = err;
        return result;
    }

    return impl_->processModel(model, config);
}

}  // namespace melkor
