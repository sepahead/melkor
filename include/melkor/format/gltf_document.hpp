// glTF JSON document model.
//
// This is the reduced, validated in-memory form of a glTF JSON document -- only the fields the
// `KHR_gaussian_splatting` reader needs (accessors, bufferViews, buffers, meshes/primitives,
// nodes, the default scene, and the extension-use declarations). It is deliberately NOT a general
// glTF loader: unknown fields are ignored (glTF is open-world), but the fields Melkor does read
// are type-checked and their integer indices are validated so the reader can walk the graph
// without re-checking every reference.
//
// Untrusted input is assumed throughout: JSON parsing is exception-free with a try/catch backstop,
// every numeric index must be a non-negative integer, and a malformed document produces a clean
// Result failure, never a throw that escapes or an out-of-range access.
//
// The document does NOT compute node transforms or decode accessor bytes -- that is the reader's
// job, using math/covariance.hpp and format/gltf_accessor.hpp. This module answers only "what does
// the JSON say, structurally?".

#ifndef MELKOR_FORMAT_GLTF_DOCUMENT_HPP
#define MELKOR_FORMAT_GLTF_DOCUMENT_HPP

#include "melkor/error.hpp"
#include "melkor/format/gltf_accessor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace melkor::format::gltf {

struct BufferDesc {
    std::uint64_t byte_length = 0;
    // Absent uri => the buffer is the GLB binary chunk (only valid for buffer 0). A present uri is
    // either an external file or a `data:` URI; the reader decides how to resolve it, and refuses
    // to fetch anything off the local machine.
    std::optional<std::string> uri;
};

struct BufferViewDesc {
    std::uint64_t buffer = 0;
    std::uint64_t byte_offset = 0;
    std::uint64_t byte_length = 0;
    std::uint64_t byte_stride = 0;  // 0 => unspecified (tightly packed)
};

struct AccessorDesc {
    // May be absent for a purely-sparse accessor. Melkor does not support sparse splat attributes,
    // so `has_buffer_view == false` (or `is_sparse`) is surfaced and the reader rejects it.
    bool has_buffer_view = false;
    std::uint64_t buffer_view = 0;
    std::uint64_t byte_offset = 0;
    ComponentType component = ComponentType::f32;
    ElementType element = ElementType::scalar;
    bool normalized = false;
    std::uint64_t count = 0;
    bool is_sparse = false;
};

// The KHR_gaussian_splatting properties on a primitive, when present.
struct GaussianExt {
    std::string kernel;                          // required
    std::string color_space;                     // required
    std::string projection = "perspective";      // spec default
    std::string sorting_method = "cameraDistance";  // spec default
};

struct PrimitiveDesc {
    int mode = 4;  // glTF default is TRIANGLES (4); a splat primitive MUST be POINTS (0)
    std::map<std::string, std::uint64_t> attributes;  // semantic -> accessor index
    std::optional<GaussianExt> gaussian;              // set iff KHR_gaussian_splatting present
};

struct MeshDesc {
    std::vector<PrimitiveDesc> primitives;
};

struct NodeDesc {
    // glTF nodes carry EITHER a 4x4 column-major `matrix` OR a translation/rotation/scale triple.
    // Both are captured faithfully; the reader composes the effective transform.
    std::optional<std::array<double, 16>> matrix;
    std::array<double, 3> translation{{0.0, 0.0, 0.0}};
    std::array<double, 4> rotation{{0.0, 0.0, 0.0, 1.0}};  // xyzw, identity default
    std::array<double, 3> scale{{1.0, 1.0, 1.0}};
    std::optional<std::uint64_t> mesh;
    std::vector<std::uint64_t> children;
};

struct Document {
    std::vector<AccessorDesc> accessors;
    std::vector<BufferViewDesc> buffer_views;
    std::vector<BufferDesc> buffers;
    std::vector<MeshDesc> meshes;
    std::vector<NodeDesc> nodes;
    std::vector<std::uint64_t> scene_roots;  // the default scene's root node indices
    std::vector<std::string> extensions_used;
    std::vector<std::string> extensions_required;
};

// Parses glTF JSON (typically the JSON chunk of a GLB) into a validated Document. Every index that
// the reader will follow -- scene roots, node.mesh, node.children, primitive attribute accessors,
// accessor.bufferView, bufferView.buffer -- is bounds-checked here, so a document that parses
// successfully has a self-consistent reference graph. Returns a clean failure on malformed JSON,
// a wrong-typed field, a negative or out-of-range index, or a missing required sub-field.
Result<Document> parse_gltf_json(const std::uint8_t* data, std::size_t size);

}  // namespace melkor::format::gltf

#endif  // MELKOR_FORMAT_GLTF_DOCUMENT_HPP
