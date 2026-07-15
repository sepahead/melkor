#include "melkor/format/gltf_document.hpp"

#include "json.hpp"

#include <string>

namespace melkor::format::gltf {

namespace {

using json = nlohmann::json;

Result<Document> fail(const char* code, std::string message) {
    Diagnostic d(code, Severity::error, std::move(message));
    return Result<Document>::failure(ErrorCode::invalid_data, std::move(d));
}

// A non-negative integer index. glTF indices are non-negative; a negative or fractional value is
// malformed, and a float where an index is expected is a common corruption we reject rather than
// truncate.
bool as_index(const json& j, std::uint64_t& out) {
    if (j.is_number_unsigned()) {
        out = j.get<std::uint64_t>();
        return true;
    }
    if (j.is_number_integer()) {
        const std::int64_t v = j.get<std::int64_t>();
        if (v >= 0) {
            out = static_cast<std::uint64_t>(v);
            return true;
        }
    }
    return false;
}

bool as_double(const json& j, double& out) {
    if (j.is_number()) {
        out = j.get<double>();
        return true;
    }
    return false;
}

// Reads an optional non-negative integer member; returns true if absent or a valid index (writing
// `out` only when present), false if present-but-invalid.
bool opt_index(const json& obj, const char* key, std::uint64_t& out, bool& present) {
    present = false;
    auto it = obj.find(key);
    if (it == obj.end()) return true;
    present = true;
    return as_index(*it, out);
}

std::optional<ElementType> element_type_from_string(const std::string& s) {
    if (s == "SCALAR") return ElementType::scalar;
    if (s == "VEC2") return ElementType::vec2;
    if (s == "VEC3") return ElementType::vec3;
    if (s == "VEC4") return ElementType::vec4;
    return std::nullopt;  // MAT2/MAT3/MAT4 are valid glTF but unsupported by the splat reader
}

// Reads a fixed-length array of numbers (e.g. a 16-element matrix). Returns false on any shape or
// type mismatch.
template <std::size_t N>
bool read_number_array(const json& obj, const char* key, std::array<double, N>& out, bool& present) {
    present = false;
    auto it = obj.find(key);
    if (it == obj.end()) return true;
    present = true;
    if (!it->is_array() || it->size() != N) return false;
    for (std::size_t i = 0; i < N; ++i) {
        if (!as_double((*it)[i], out[i])) return false;
    }
    return true;
}

bool read_string_array(const json& arr, std::vector<std::string>& out) {
    if (!arr.is_array()) return false;
    for (const auto& e : arr) {
        if (!e.is_string()) return false;
        out.push_back(e.get<std::string>());
    }
    return true;
}

}  // namespace

Result<Document> parse_gltf_json(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return fail("MK2130_GLTF_EMPTY", "empty glTF JSON");
    }

    // Exception-free parse; a malformed document yields a discarded value rather than throwing.
    // The try/catch is a backstop against any exception from deeper access on adversarial input.
    try {
        json doc = json::parse(data, data + size, nullptr, /*allow_exceptions=*/false,
                               /*ignore_comments=*/false);
        if (doc.is_discarded()) {
            return fail("MK2131_GLTF_BAD_JSON", "glTF JSON is not well-formed");
        }
        if (!doc.is_object()) {
            return fail("MK2131_GLTF_BAD_JSON", "glTF root is not a JSON object");
        }

        Document out;

        // ---- accessors ----
        if (auto it = doc.find("accessors"); it != doc.end()) {
            if (!it->is_array()) return fail("MK2132_GLTF_BAD_FIELD", "'accessors' is not an array");
            for (const auto& a : *it) {
                if (!a.is_object()) return fail("MK2132_GLTF_BAD_FIELD", "an accessor is not an object");
                AccessorDesc desc;
                std::uint64_t bv = 0;
                bool bv_present = false;
                if (!opt_index(a, "bufferView", bv, bv_present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "accessor.bufferView is not a valid index");
                }
                desc.has_buffer_view = bv_present;
                desc.buffer_view = bv;

                std::uint64_t off = 0;
                bool off_present = false;
                if (!opt_index(a, "byteOffset", off, off_present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "accessor.byteOffset is not a valid index");
                }
                desc.byte_offset = off;

                auto ct = a.find("componentType");
                std::uint64_t ct_val = 0;
                if (ct == a.end() || !as_index(*ct, ct_val)) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "accessor is missing a valid componentType");
                }
                auto comp = component_type_from_int(static_cast<int>(ct_val));
                if (!comp.has_value()) {
                    return fail("MK2134_GLTF_UNSUPPORTED",
                                "unsupported accessor componentType " + std::to_string(ct_val));
                }
                desc.component = comp.value();

                auto ty = a.find("type");
                if (ty == a.end() || !ty->is_string()) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "accessor is missing a valid type");
                }
                auto elem = element_type_from_string(ty->get<std::string>());
                if (!elem.has_value()) {
                    return fail("MK2134_GLTF_UNSUPPORTED",
                                "unsupported accessor type '" + ty->get<std::string>() +
                                    "' (the splat reader handles SCALAR/VEC2/VEC3/VEC4)");
                }
                desc.element = elem.value();

                auto cnt = a.find("count");
                std::uint64_t cnt_val = 0;
                if (cnt == a.end() || !as_index(*cnt, cnt_val)) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "accessor is missing a valid count");
                }
                desc.count = cnt_val;

                if (auto n = a.find("normalized"); n != a.end()) {
                    if (!n->is_boolean()) {
                        return fail("MK2132_GLTF_BAD_FIELD", "accessor.normalized is not a boolean");
                    }
                    desc.normalized = n->get<bool>();
                }
                desc.is_sparse = a.find("sparse") != a.end();
                out.accessors.push_back(std::move(desc));
            }
        }

        // ---- bufferViews ----
        if (auto it = doc.find("bufferViews"); it != doc.end()) {
            if (!it->is_array()) return fail("MK2132_GLTF_BAD_FIELD", "'bufferViews' is not an array");
            for (const auto& b : *it) {
                if (!b.is_object()) return fail("MK2132_GLTF_BAD_FIELD", "a bufferView is not an object");
                BufferViewDesc desc;
                auto buf = b.find("buffer");
                if (buf == b.end() || !as_index(*buf, desc.buffer)) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "bufferView is missing a valid buffer index");
                }
                std::uint64_t v = 0;
                bool present = false;
                if (!opt_index(b, "byteOffset", v, present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "bufferView.byteOffset is invalid");
                }
                desc.byte_offset = present ? v : 0;
                auto len = b.find("byteLength");
                if (len == b.end() || !as_index(*len, desc.byte_length)) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "bufferView is missing a valid byteLength");
                }
                if (!opt_index(b, "byteStride", v, present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "bufferView.byteStride is invalid");
                }
                desc.byte_stride = present ? v : 0;
                out.buffer_views.push_back(desc);
            }
        }

        // ---- buffers ----
        if (auto it = doc.find("buffers"); it != doc.end()) {
            if (!it->is_array()) return fail("MK2132_GLTF_BAD_FIELD", "'buffers' is not an array");
            for (const auto& b : *it) {
                if (!b.is_object()) return fail("MK2132_GLTF_BAD_FIELD", "a buffer is not an object");
                BufferDesc desc;
                auto len = b.find("byteLength");
                if (len == b.end() || !as_index(*len, desc.byte_length)) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "buffer is missing a valid byteLength");
                }
                if (auto u = b.find("uri"); u != b.end()) {
                    if (!u->is_string()) return fail("MK2132_GLTF_BAD_FIELD", "buffer.uri is not a string");
                    desc.uri = u->get<std::string>();
                }
                out.buffers.push_back(std::move(desc));
            }
        }

        // ---- meshes and their primitives ----
        if (auto it = doc.find("meshes"); it != doc.end()) {
            if (!it->is_array()) return fail("MK2132_GLTF_BAD_FIELD", "'meshes' is not an array");
            for (const auto& m : *it) {
                if (!m.is_object()) return fail("MK2132_GLTF_BAD_FIELD", "a mesh is not an object");
                MeshDesc mesh;
                auto prims = m.find("primitives");
                if (prims == m.end() || !prims->is_array()) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "mesh is missing a 'primitives' array");
                }
                for (const auto& p : *prims) {
                    if (!p.is_object()) return fail("MK2132_GLTF_BAD_FIELD", "a primitive is not an object");
                    PrimitiveDesc prim;
                    if (auto mode = p.find("mode"); mode != p.end()) {
                        std::uint64_t mode_val = 0;
                        if (!as_index(*mode, mode_val) || mode_val > 6) {
                            return fail("MK2132_GLTF_BAD_FIELD",
                                        "primitive.mode is not a valid glTF primitive mode (0-6)");
                        }
                        prim.mode = static_cast<int>(mode_val);
                    }
                    auto attrs = p.find("attributes");
                    if (attrs == p.end() || !attrs->is_object()) {
                        return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                    "primitive is missing an 'attributes' object");
                    }
                    for (auto a = attrs->begin(); a != attrs->end(); ++a) {
                        std::uint64_t acc = 0;
                        if (!as_index(a.value(), acc)) {
                            return fail("MK2132_GLTF_BAD_FIELD",
                                        "primitive attribute '" + a.key() + "' is not a valid index");
                        }
                        prim.attributes.emplace(a.key(), acc);
                    }
                    // KHR_gaussian_splatting extension, if present.
                    if (auto ext = p.find("extensions"); ext != p.end() && ext->is_object()) {
                        if (auto g = ext->find("KHR_gaussian_splatting");
                            g != ext->end() && g->is_object()) {
                            GaussianExt ge;
                            auto k = g->find("kernel");
                            auto cs = g->find("colorSpace");
                            if (k == g->end() || !k->is_string() || cs == g->end() ||
                                !cs->is_string()) {
                                return fail("MK2135_GLTF_KHR_INCOMPLETE",
                                            "KHR_gaussian_splatting requires string 'kernel' and "
                                            "'colorSpace'");
                            }
                            ge.kernel = k->get<std::string>();
                            ge.color_space = cs->get<std::string>();
                            if (auto pr = g->find("projection"); pr != g->end() && pr->is_string()) {
                                ge.projection = pr->get<std::string>();
                            }
                            if (auto sm = g->find("sortingMethod"); sm != g->end() && sm->is_string()) {
                                ge.sorting_method = sm->get<std::string>();
                            }
                            prim.gaussian = std::move(ge);
                        }
                    }
                    mesh.primitives.push_back(std::move(prim));
                }
                out.meshes.push_back(std::move(mesh));
            }
        }

        // ---- nodes ----
        if (auto it = doc.find("nodes"); it != doc.end()) {
            if (!it->is_array()) return fail("MK2132_GLTF_BAD_FIELD", "'nodes' is not an array");
            for (const auto& n : *it) {
                if (!n.is_object()) return fail("MK2132_GLTF_BAD_FIELD", "a node is not an object");
                NodeDesc node;
                bool present = false;
                std::array<double, 16> matrix{};
                if (!read_number_array(n, "matrix", matrix, present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "node.matrix is not 16 numbers");
                }
                if (present) node.matrix = matrix;
                if (!read_number_array(n, "translation", node.translation, present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "node.translation is not 3 numbers");
                }
                if (!read_number_array(n, "rotation", node.rotation, present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "node.rotation is not 4 numbers");
                }
                if (!read_number_array(n, "scale", node.scale, present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "node.scale is not 3 numbers");
                }
                std::uint64_t mesh_idx = 0;
                bool mesh_present = false;
                if (!opt_index(n, "mesh", mesh_idx, mesh_present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "node.mesh is not a valid index");
                }
                if (mesh_present) node.mesh = mesh_idx;
                if (auto ch = n.find("children"); ch != n.end()) {
                    if (!ch->is_array()) return fail("MK2132_GLTF_BAD_FIELD", "node.children is not an array");
                    for (const auto& c : *ch) {
                        std::uint64_t ci = 0;
                        if (!as_index(c, ci)) {
                            return fail("MK2132_GLTF_BAD_FIELD", "node.children contains an invalid index");
                        }
                        node.children.push_back(ci);
                    }
                }
                out.nodes.push_back(std::move(node));
            }
        }

        // ---- default scene roots ----
        {
            std::uint64_t scene_index = 0;
            bool has_scene = false;
            if (auto s = doc.find("scene"); s != doc.end()) {
                if (!as_index(*s, scene_index)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "'scene' is not a valid index");
                }
                has_scene = true;
            }
            if (auto scenes = doc.find("scenes"); scenes != doc.end() && scenes->is_array()) {
                // Default to scene 0 when no explicit default scene is given but scenes exist.
                const std::uint64_t chosen = has_scene ? scene_index : 0;
                if (chosen >= scenes->size()) {
                    return fail("MK2136_GLTF_BAD_INDEX",
                                "default scene index " + std::to_string(chosen) + " is out of range");
                }
                const auto& scene = (*scenes)[chosen];
                if (scene.is_object()) {
                    if (auto nodes = scene.find("nodes"); nodes != scene.end() && nodes->is_array()) {
                        for (const auto& r : *nodes) {
                            std::uint64_t ri = 0;
                            if (!as_index(r, ri)) {
                                return fail("MK2132_GLTF_BAD_FIELD", "scene.nodes contains an invalid index");
                            }
                            out.scene_roots.push_back(ri);
                        }
                    }
                }
            }
        }

        // ---- extension declarations ----
        if (auto it = doc.find("extensionsUsed"); it != doc.end()) {
            if (!read_string_array(*it, out.extensions_used)) {
                return fail("MK2132_GLTF_BAD_FIELD", "'extensionsUsed' is not an array of strings");
            }
        }
        if (auto it = doc.find("extensionsRequired"); it != doc.end()) {
            if (!read_string_array(*it, out.extensions_required)) {
                return fail("MK2132_GLTF_BAD_FIELD", "'extensionsRequired' is not an array of strings");
            }
        }

        // ---- reference-graph validation: every index the reader follows is in range ----
        const std::uint64_t n_accessors = out.accessors.size();
        const std::uint64_t n_buffer_views = out.buffer_views.size();
        const std::uint64_t n_buffers = out.buffers.size();
        const std::uint64_t n_meshes = out.meshes.size();
        const std::uint64_t n_nodes = out.nodes.size();

        for (const auto& a : out.accessors) {
            if (a.has_buffer_view && a.buffer_view >= n_buffer_views) {
                return fail("MK2136_GLTF_BAD_INDEX", "accessor.bufferView is out of range");
            }
        }
        for (const auto& b : out.buffer_views) {
            if (b.buffer >= n_buffers) {
                return fail("MK2136_GLTF_BAD_INDEX", "bufferView.buffer is out of range");
            }
        }
        for (const auto& m : out.meshes) {
            for (const auto& p : m.primitives) {
                for (const auto& [semantic, acc] : p.attributes) {
                    if (acc >= n_accessors) {
                        return fail("MK2136_GLTF_BAD_INDEX",
                                    "primitive attribute '" + semantic + "' references an out-of-range "
                                    "accessor");
                    }
                }
            }
        }
        for (const auto& n : out.nodes) {
            if (n.mesh.has_value() && n.mesh.value() >= n_meshes) {
                return fail("MK2136_GLTF_BAD_INDEX", "node.mesh is out of range");
            }
            for (std::uint64_t c : n.children) {
                if (c >= n_nodes) {
                    return fail("MK2136_GLTF_BAD_INDEX", "node.children references an out-of-range node");
                }
            }
        }
        for (std::uint64_t r : out.scene_roots) {
            if (r >= n_nodes) {
                return fail("MK2136_GLTF_BAD_INDEX", "scene root references an out-of-range node");
            }
        }

        return Result<Document>::success(std::move(out));
    } catch (const std::exception& e) {
        return fail("MK2131_GLTF_BAD_JSON",
                    std::string("glTF JSON could not be parsed: ") + e.what());
    } catch (...) {
        return fail("MK2131_GLTF_BAD_JSON", "glTF JSON could not be parsed");
    }
}

}  // namespace melkor::format::gltf
