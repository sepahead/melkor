// glTF accessor decoding.
//
// A glTF vertex attribute is a typed view over a slice of a binary buffer: a component type
// (float, or a signed/unsigned 8/16/32-bit integer), an element type (SCALAR/VEC2/VEC3/VEC4), an
// optional "normalized" flag, a count, a starting byte offset, and a byte stride (which may be
// larger than the element for interleaved data). Turning that into floats is exactly the kind of
// arithmetic that goes wrong quietly: a normalized unsigned byte is `c/255`, a normalized signed
// byte is `max(c/127, -1)`, and mixing those up shifts every colour or rotation subtly. This
// module is the one place that decode is done, bounds-checked and pinned by tests.
//
// It is deliberately independent of JSON parsing: it takes an already-resolved `AccessorView` and
// a buffer, so it can be unit-tested with hand-built bytes, and the JSON layer's only job is to
// produce a correct `AccessorView`. Every offset is validated with checked arithmetic against the
// buffer size, so a malformed accessor yields a clean error, never an out-of-bounds read.

#ifndef MELKOR_FORMAT_GLTF_ACCESSOR_HPP
#define MELKOR_FORMAT_GLTF_ACCESSOR_HPP

#include "melkor/error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace melkor::format::gltf {

// glTF component types, by their spec constant. Values are the glTF `componentType` enum so a
// JSON reader can map directly.
enum class ComponentType : int {
    i8 = 5120,   // BYTE
    u8 = 5121,   // UNSIGNED_BYTE
    i16 = 5122,  // SHORT
    u16 = 5123,  // UNSIGNED_SHORT
    u32 = 5125,  // UNSIGNED_INT
    f32 = 5126,  // FLOAT
};

// glTF element types, by their component count.
enum class ElementType : std::uint8_t {
    scalar = 1,
    vec2 = 2,
    vec3 = 3,
    vec4 = 4,
};

// Size of one component in bytes, or 0 for an unrecognised value.
std::size_t component_size(ComponentType type) noexcept;

// Number of components in one element (1/2/3/4).
std::size_t component_count(ElementType type) noexcept;

// Maps a raw glTF `componentType` integer to the enum, if recognised.
std::optional<ComponentType> component_type_from_int(int value) noexcept;

// A resolved accessor: everything needed to read the values, with the buffer supplied separately.
struct AccessorView {
    ComponentType component = ComponentType::f32;
    ElementType element = ElementType::scalar;
    bool normalized = false;
    std::size_t count = 0;         // number of elements
    std::size_t byte_offset = 0;   // start of the first element within the buffer
    std::size_t byte_stride = 0;   // 0 means tightly packed (component_size * component_count)
};

// Decodes an accessor into a flat, row-major vector of `count * component_count` floats:
//   - float components are returned as-is;
//   - integer components, when `normalized`, are mapped to [0,1] (unsigned) or [-1,1] (signed) per
//     the glTF rules; when not normalized, to their exact integer value as a float;
//   - the effective stride defaults to the tightly-packed element size when `byte_stride` is 0,
//     and every element's bytes are validated to lie within `buffer_size`.
// Fails with a diagnostic (never an out-of-bounds read) on an unrecognised type, a zero-size
// component, or any element that would fall outside the buffer.
Result<std::vector<float>> decode_accessor(const AccessorView& view, const std::uint8_t* buffer,
                                           std::size_t buffer_size);

}  // namespace melkor::format::gltf

#endif  // MELKOR_FORMAT_GLTF_ACCESSOR_HPP
