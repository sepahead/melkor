#include "melkor/format/gltf_accessor.hpp"

#include "melkor/checked.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace melkor::format::gltf {

std::size_t component_size(ComponentType type) noexcept {
    switch (type) {
        case ComponentType::i8:
        case ComponentType::u8:
            return 1;
        case ComponentType::i16:
        case ComponentType::u16:
            return 2;
        case ComponentType::u32:
        case ComponentType::f32:
            return 4;
    }
    return 0;
}

std::size_t component_count(ElementType type) noexcept {
    return static_cast<std::size_t>(type);
}

std::optional<ComponentType> component_type_from_int(int value) noexcept {
    switch (value) {
        case 5120:
            return ComponentType::i8;
        case 5121:
            return ComponentType::u8;
        case 5122:
            return ComponentType::i16;
        case 5123:
            return ComponentType::u16;
        case 5125:
            return ComponentType::u32;
        case 5126:
            return ComponentType::f32;
        default:
            return std::nullopt;
    }
}

namespace {

std::uint16_t read_le_u16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t read_le_u32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

float read_le_f32(const std::uint8_t* p) noexcept {
    // IEEE-754 little-endian on the wire; reassemble the bit pattern and memcpy into a float so
    // there is no strict-aliasing violation and it is correct on a big-endian host.
    const std::uint32_t bits = read_le_u32(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Decodes one component at `p` into a float, applying normalization per the glTF rules.
float decode_component(const std::uint8_t* p, ComponentType type, bool normalized) noexcept {
    switch (type) {
        case ComponentType::f32:
            return read_le_f32(p);
        case ComponentType::u8: {
            const std::uint8_t c = p[0];
            return normalized ? static_cast<float>(c) / 255.0f : static_cast<float>(c);
        }
        case ComponentType::i8: {
            const std::int8_t c = static_cast<std::int8_t>(p[0]);
            if (!normalized) return static_cast<float>(c);
            return std::max(static_cast<float>(c) / 127.0f, -1.0f);
        }
        case ComponentType::u16: {
            const std::uint16_t c = read_le_u16(p);
            return normalized ? static_cast<float>(c) / 65535.0f : static_cast<float>(c);
        }
        case ComponentType::i16: {
            const std::int16_t c = static_cast<std::int16_t>(read_le_u16(p));
            if (!normalized) return static_cast<float>(c);
            return std::max(static_cast<float>(c) / 32767.0f, -1.0f);
        }
        case ComponentType::u32: {
            const std::uint32_t c = read_le_u32(p);
            // A normalized 32-bit unsigned maps to [0,1] by /(2^32 - 1); unnormalized returns the
            // exact value (which may lose precision above 2^24, an inherent float limitation).
            return normalized ? static_cast<float>(static_cast<double>(c) / 4294967295.0)
                              : static_cast<float>(c);
        }
    }
    return 0.0f;
}

Result<std::vector<float>> fail(const char* code, std::string message) {
    Diagnostic d(code, Severity::error, std::move(message));
    return Result<std::vector<float>>::failure(ErrorCode::invalid_data, std::move(d));
}

}  // namespace

Result<std::vector<float>> decode_accessor(const AccessorView& view, const std::uint8_t* buffer,
                                           std::size_t buffer_size) {
    const std::size_t comp_size = component_size(view.component);
    if (comp_size == 0) {
        return fail("MK2120_GLTF_BAD_COMPONENT_TYPE", "unrecognised glTF component type");
    }
    const std::size_t comps = component_count(view.element);
    const std::size_t element_size = comp_size * comps;  // <= 4*4, cannot overflow size_t

    // Effective stride: 0 means tightly packed. A non-zero stride smaller than the element would
    // make successive elements overlap, which no conforming asset does and which we reject.
    std::size_t stride = view.byte_stride;
    if (stride == 0) {
        stride = element_size;
    } else if (stride < element_size) {
        return fail("MK2121_GLTF_STRIDE_TOO_SMALL",
                    "accessor byteStride " + std::to_string(view.byte_stride) +
                        " is smaller than the element size " + std::to_string(element_size));
    }

    if (view.count == 0) {
        return Result<std::vector<float>>::success({});
    }

    // The last element begins at byte_offset + (count-1)*stride and occupies element_size bytes;
    // that end must lie within the buffer. Computed with checked arithmetic so a huge count or
    // offset cannot wrap.
    auto span = checked_mul(static_cast<std::uint64_t>(view.count - 1),
                            static_cast<std::uint64_t>(stride), "accessor span");
    if (!span.has_value()) {
        return fail("MK2122_GLTF_ACCESSOR_OOB", "accessor size overflows");
    }
    auto last_begin = checked_add(static_cast<std::uint64_t>(view.byte_offset), span.value(),
                                  "accessor offset");
    if (!last_begin.has_value()) {
        return fail("MK2122_GLTF_ACCESSOR_OOB", "accessor offset overflows");
    }
    auto range = checked_range(last_begin.value(), static_cast<std::uint64_t>(element_size),
                               static_cast<std::uint64_t>(buffer_size), "accessor element");
    if (!range.has_value()) {
        return fail("MK2122_GLTF_ACCESSOR_OOB",
                    "accessor reads past the end of the buffer (" + std::to_string(view.count) +
                        " elements, stride " + std::to_string(stride) + ", offset " +
                        std::to_string(view.byte_offset) + ", buffer " +
                        std::to_string(buffer_size) + ")");
    }

    std::vector<float> out;
    out.resize(view.count * comps);  // count*comps <= buffer_size/1 in practice; comps<=4
    std::size_t w = 0;
    for (std::size_t i = 0; i < view.count; ++i) {
        const std::uint8_t* element = buffer + view.byte_offset + i * stride;
        for (std::size_t c = 0; c < comps; ++c) {
            out[w++] = decode_component(element + c * comp_size, view.component, view.normalized);
        }
    }
    return Result<std::vector<float>>::success(std::move(out));
}

}  // namespace melkor::format::gltf
