#include "melkor/format/gltf_accessor.hpp"

#include "melkor/checked.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
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
    switch (type) {
    case ElementType::scalar:
        return 1;
    case ElementType::vec2:
        return 2;
    case ElementType::vec3:
        return 3;
    case ElementType::vec4:
        return 4;
    case ElementType::mat2:
    case ElementType::mat3:
    case ElementType::mat4:
        return 0;
    }
    return 0;
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

std::int8_t signed_i8(std::uint8_t value) noexcept {
    const std::int16_t widened = value <= 0x7fU ? static_cast<std::int16_t>(value)
                                                : static_cast<std::int16_t>(value) - 0x100;
    return static_cast<std::int8_t>(widened);
}

std::int16_t signed_i16(std::uint16_t value) noexcept {
    const std::int32_t widened = value <= 0x7fffU ? static_cast<std::int32_t>(value)
                                                  : static_cast<std::int32_t>(value) - 0x10000;
    return static_cast<std::int16_t>(widened);
}

double raw_component(const std::uint8_t* p, ComponentType type) noexcept {
    switch (type) {
    case ComponentType::f32:
        return static_cast<double>(read_le_f32(p));
    case ComponentType::u8:
        return static_cast<double>(p[0]);
    case ComponentType::i8:
        return static_cast<double>(signed_i8(p[0]));
    case ComponentType::u16:
        return static_cast<double>(read_le_u16(p));
    case ComponentType::i16:
        return static_cast<double>(signed_i16(read_le_u16(p)));
    case ComponentType::u32:
        return static_cast<double>(read_le_u32(p));
    }
    return 0.0;
}

// Converts one exact stored component to the float value used by the canonical reader.
float decode_component(double value, ComponentType type, bool normalized) noexcept {
    if (!normalized)
        return static_cast<float>(value);
    switch (type) {
    case ComponentType::u8:
        return static_cast<float>(value) / 255.0f;
    case ComponentType::i8:
        return std::max(static_cast<float>(value) / 127.0f, -1.0f);
    case ComponentType::u16:
        return static_cast<float>(value) / 65535.0f;
    case ComponentType::i16:
        return std::max(static_cast<float>(value) / 32767.0f, -1.0f);
    case ComponentType::u32:
    case ComponentType::f32:
        break;
    }
    return static_cast<float>(value);
}

Result<DecodedAccessor> fail(const char* code, std::string message) {
    Diagnostic d(code, Severity::error, std::move(message));
    return Result<DecodedAccessor>::failure(ErrorCode::invalid_data, std::move(d));
}

Result<DecodedAccessor> decode_accessor_impl(const AccessorView& view, const std::uint8_t* buffer,
                                             std::size_t buffer_size,
                                             const OperationContext* context);

}  // namespace

Result<DecodedAccessor> decode_accessor(const AccessorView& view, const std::uint8_t* buffer,
                                        std::size_t buffer_size) try {
    return decode_accessor_impl(view, buffer, buffer_size, nullptr);
} catch (const std::bad_alloc&) {
    Diagnostic d("MK2125_GLTF_ACCESSOR_ALLOCATION", Severity::error,
                 "decoded accessor allocation failed");
    return Result<DecodedAccessor>::failure(ErrorCode::resource_limit, std::move(d));
} catch (const std::length_error&) {
    Diagnostic d("MK2125_GLTF_ACCESSOR_ALLOCATION", Severity::error,
                 "decoded accessor size exceeds the container limit");
    return Result<DecodedAccessor>::failure(ErrorCode::resource_limit, std::move(d));
}

Result<DecodedAccessor> decode_accessor(const AccessorView& view, const std::uint8_t* buffer,
                                        std::size_t buffer_size,
                                        const OperationContext& context) try {
    if (context.budget == nullptr) {
        Diagnostic diagnostic("MK0310_NO_BUDGET", Severity::error,
                              "the glTF accessor decoder requires a resource budget");
        return Result<DecodedAccessor>::failure(ErrorCode::internal_error, std::move(diagnostic));
    }
    return decode_accessor_impl(view, buffer, buffer_size, &context);
} catch (const std::bad_alloc&) {
    Diagnostic d("MK2125_GLTF_ACCESSOR_ALLOCATION", Severity::error,
                 "decoded accessor allocation failed");
    return Result<DecodedAccessor>::failure(ErrorCode::resource_limit, std::move(d));
} catch (const std::length_error&) {
    Diagnostic d("MK2125_GLTF_ACCESSOR_ALLOCATION", Severity::error,
                 "decoded accessor size exceeds the container limit");
    return Result<DecodedAccessor>::failure(ErrorCode::resource_limit, std::move(d));
}

namespace {

Result<DecodedAccessor> decode_accessor_impl(const AccessorView& view, const std::uint8_t* buffer,
                                             std::size_t buffer_size,
                                             const OperationContext* context) {
    static_assert(sizeof(float) == 4);
    static_assert(std::numeric_limits<float>::is_iec559);

    const std::size_t comp_size = component_size(view.component);
    if (comp_size == 0) {
        return fail("MK2120_GLTF_BAD_COMPONENT_TYPE", "unrecognized glTF component type");
    }
    const std::size_t comps = component_count(view.element);
    if (comps == 0 || comps > 4) {
        return fail("MK2123_GLTF_BAD_ELEMENT_TYPE",
                    "accessor element type is not a supported scalar or vector");
    }
    if (view.normalized &&
        (view.component == ComponentType::f32 || view.component == ComponentType::u32)) {
        return fail("MK2126_GLTF_INVALID_NORMALIZATION",
                    "FLOAT and UNSIGNED_INT accessors cannot be normalized");
    }
    const std::size_t element_size = comp_size * comps;
    if (view.byte_offset % comp_size != 0) {
        return fail("MK2124_GLTF_ACCESSOR_MISALIGNED",
                    "accessor byte offset is not aligned to its component size");
    }

    // Effective stride: 0 means tightly packed. A non-zero stride smaller than the element would
    // make successive elements overlap, which no conforming asset does and which we reject.
    std::size_t stride = view.byte_stride;
    if (stride == 0) {
        stride = element_size;
    } else if (stride < element_size) {
        return fail("MK2121_GLTF_STRIDE_TOO_SMALL",
                    "accessor byteStride " + std::to_string(view.byte_stride) +
                        " is smaller than the element size " + std::to_string(element_size));
    } else if (stride % comp_size != 0) {
        return fail("MK2124_GLTF_ACCESSOR_MISALIGNED",
                    "accessor byte stride is not aligned to its component size");
    }

    if (view.count == 0) {
        return Result<DecodedAccessor>::success(DecodedAccessor({}));
    }
    if (buffer == nullptr) {
        return fail("MK2122_GLTF_ACCESSOR_OOB", "accessor buffer is null for a non-empty accessor");
    }

    // The last element begins at byte_offset + (count-1)*stride and occupies element_size bytes;
    // that end must lie within the buffer. Computed with checked arithmetic so a huge count or
    // offset cannot wrap.
    auto span = checked_mul(static_cast<std::uint64_t>(view.count - 1),
                            static_cast<std::uint64_t>(stride), "accessor span");
    if (!span.has_value()) {
        return fail("MK2122_GLTF_ACCESSOR_OOB", "accessor size overflows");
    }
    auto last_begin =
        checked_add(static_cast<std::uint64_t>(view.byte_offset), span.value(), "accessor offset");
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

    auto output_count =
        checked_mul(static_cast<std::uint64_t>(view.count), static_cast<std::uint64_t>(comps),
                    "decoded accessor float count");
    if (!output_count.has_value() ||
        output_count.value() > std::numeric_limits<std::size_t>::max()) {
        return fail("MK2125_GLTF_ACCESSOR_ALLOCATION",
                    "decoded accessor size is not representable");
    }
    auto output_bytes =
        checked_mul(output_count.value(), sizeof(float), "decoded accessor output bytes");
    if (!output_bytes.has_value()) {
        return fail("MK2125_GLTF_ACCESSOR_ALLOCATION",
                    "decoded accessor byte size is not representable");
    }
    Budget::Charge retained_memory;
    if (context != nullptr) {
        auto control = context->check("gltf.read.decode_accessor");
        if (!control.has_value()) {
            return Result<DecodedAccessor>::failure(control.error_code(), control.diagnostics());
        }
        auto reserved = context->budget->reserve(BudgetKind::memory_bytes, output_bytes.value(),
                                                 "gltf.accessor.decoded");
        if (!reserved.has_value()) {
            return Result<DecodedAccessor>::failure(reserved.error_code(), reserved.diagnostics());
        }
        retained_memory = std::move(reserved).value();
    }
    std::vector<float> out;
    out.resize(static_cast<std::size_t>(output_count.value()));
    std::array<double, 4> raw_minimum{};
    std::array<double, 4> raw_maximum{};
    std::size_t w = 0;
    for (std::size_t i = 0; i < view.count; ++i) {
        if (context != nullptr && i % 4096 == 0) {
            auto control =
                context->checkpoint({"gltf.read", "decode_accessor", i, view.count, "elements"});
            if (!control.has_value()) {
                return Result<DecodedAccessor>::failure(control.error_code(),
                                                        control.diagnostics());
            }
        }
        const std::uint8_t* element = buffer + view.byte_offset + i * stride;
        for (std::size_t c = 0; c < comps; ++c) {
            const double raw = raw_component(element + c * comp_size, view.component);
            if (!std::isfinite(raw)) {
                return fail("MK2127_GLTF_ACCESSOR_VALUE",
                            "an accessor contains a non-finite component value");
            }
            if (i == 0) {
                raw_minimum[c] = raw;
                raw_maximum[c] = raw;
            } else {
                raw_minimum[c] = std::min(raw_minimum[c], raw);
                raw_maximum[c] = std::max(raw_maximum[c], raw);
            }
            out[w++] = decode_component(raw, view.component, view.normalized);
        }
    }
    if (context != nullptr) {
        auto control = context->checkpoint(
            {"gltf.read", "decode_accessor", view.count, view.count, "elements"});
        if (!control.has_value()) {
            return Result<DecodedAccessor>::failure(control.error_code(), control.diagnostics());
        }
    }
    return Result<DecodedAccessor>::success(DecodedAccessor(
        std::move(out), std::move(retained_memory), raw_minimum, raw_maximum, comps));
}

}  // namespace

}  // namespace melkor::format::gltf
