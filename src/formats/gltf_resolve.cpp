#include "melkor/format/gltf_resolve.hpp"

#include "melkor/checked.hpp"
#include "melkor/format/gltf_accessor.hpp"

#include <cmath>
#include <limits>
#include <string>

namespace melkor::format::gltf {

namespace {

Result<DecodedAccessor> fail_with(ErrorCode error_code, const char* code, std::string message) {
    Diagnostic d(code, Severity::error, std::move(message));
    return Result<DecodedAccessor>::failure(error_code, std::move(d));
}

Result<DecodedAccessor> fail(const char* code, std::string message) {
    return fail_with(ErrorCode::invalid_data, code, std::move(message));
}

Result<DecodedAccessor> resolve_and_decode_accessor_impl(const Document& doc,
                                                         std::uint64_t accessor_index,
                                                         const std::vector<BufferSpan>& buffers,
                                                         const OperationContext* context);

bool integer_bound_is_representable(double value, ComponentType component) noexcept {
    if (!std::isfinite(value) || std::trunc(value) != value)
        return false;
    switch (component) {
    case ComponentType::i8:
        return value >= std::numeric_limits<std::int8_t>::min() &&
               value <= std::numeric_limits<std::int8_t>::max();
    case ComponentType::u8:
        return value >= 0.0 && value <= std::numeric_limits<std::uint8_t>::max();
    case ComponentType::i16:
        return value >= std::numeric_limits<std::int16_t>::min() &&
               value <= std::numeric_limits<std::int16_t>::max();
    case ComponentType::u16:
        return value >= 0.0 && value <= std::numeric_limits<std::uint16_t>::max();
    case ComponentType::u32:
        return value >= 0.0 && value <= std::numeric_limits<std::uint32_t>::max();
    case ComponentType::f32:
        return false;
    }
    return false;
}

Result<double> canonical_bound(double value, ComponentType component) {
    if (component == ComponentType::f32) {
        const float rounded = static_cast<float>(value);
        if (!std::isfinite(rounded)) {
            Diagnostic diagnostic("MK2207_GLTF_ACCESSOR_BOUNDS", Severity::error,
                                  "an accessor bound is not a finite single-precision value");
            diagnostic.with_context("declared", value);
            return Result<double>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
        return Result<double>::success(static_cast<double>(rounded));
    }
    if (!integer_bound_is_representable(value, component)) {
        Diagnostic diagnostic("MK2207_GLTF_ACCESSOR_BOUNDS", Severity::error,
                              "an accessor bound is not representable by its component type");
        diagnostic.with_context("declared", value)
            .with_context("component_type", static_cast<std::int64_t>(component));
        return Result<double>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }
    return Result<double>::success(value);
}

Result<void> validate_declared_bounds(const AccessorDesc& accessor,
                                      const DecodedAccessor& decoded) {
    const std::size_t components = decoded.raw_component_count();
    if (components == 0)
        return Result<void>::success();

    auto validate = [&](const std::vector<double>& declared, bool minimum) -> Result<void> {
        if (declared.empty())
            return Result<void>::success();
        if (declared.size() != components) {
            Diagnostic diagnostic("MK2207_GLTF_ACCESSOR_BOUNDS", Severity::error,
                                  "an accessor bound has the wrong component count");
            return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
        for (std::size_t component = 0; component < components; ++component) {
            auto bound = canonical_bound(declared[component], accessor.component);
            if (!bound.has_value()) {
                return Result<void>::failure(bound.error_code(), bound.diagnostics());
            }
            const double actual =
                minimum ? decoded.raw_minimum(component) : decoded.raw_maximum(component);
            if (bound.value() != actual) {
                Diagnostic diagnostic("MK2207_GLTF_ACCESSOR_BOUNDS", Severity::error,
                                      "an accessor bound does not match the binary data");
                diagnostic.with_context("bound", std::string(minimum ? "min" : "max"))
                    .with_context("component", static_cast<std::uint64_t>(component))
                    .with_context("declared", declared[component])
                    .with_context("actual", actual);
                return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
            }
        }
        return Result<void>::success();
    };

    auto minimum = validate(accessor.minimum, true);
    if (!minimum.has_value())
        return minimum;
    return validate(accessor.maximum, false);
}

}  // namespace

Result<DecodedAccessor> resolve_and_decode_accessor(const Document& doc,
                                                    std::uint64_t accessor_index,
                                                    const std::vector<BufferSpan>& buffers) {
    return resolve_and_decode_accessor_impl(doc, accessor_index, buffers, nullptr);
}

namespace {

Result<DecodedAccessor> resolve_and_decode_accessor_impl(const Document& doc,
                                                         std::uint64_t accessor_index,
                                                         const std::vector<BufferSpan>& buffers,
                                                         const OperationContext* context) {
    if (accessor_index >= doc.accessors.size()) {
        return fail("MK2200_GLTF_ACCESSOR_INDEX", "accessor index is out of range");
    }
    const AccessorDesc& acc = doc.accessors[static_cast<std::size_t>(accessor_index)];

    if (acc.count == 0) {
        return fail("MK2200_GLTF_ACCESSOR_INDEX", "accessor count must be greater than zero");
    }

    if (acc.is_sparse) {
        return fail_with(ErrorCode::unsupported_feature, "MK2201_GLTF_SPARSE_UNSUPPORTED",
                         "sparse accessors are not supported for splat attributes");
    }
    if (!acc.has_buffer_view) {
        return fail_with(ErrorCode::unsupported_feature, "MK2202_GLTF_NO_BUFFERVIEW",
                         "a splat attribute accessor must reference a bufferView");
    }

    // The parser guaranteed these indices are in range; assert-by-check anyway so this function is
    // safe in isolation.
    if (acc.buffer_view >= doc.buffer_views.size()) {
        return fail("MK2200_GLTF_ACCESSOR_INDEX", "accessor.bufferView is out of range");
    }
    const BufferViewDesc& bv = doc.buffer_views[static_cast<std::size_t>(acc.buffer_view)];
    if (bv.target.has_value() && bv.target != BufferViewTarget::array_buffer) {
        return fail("MK2200_GLTF_ACCESSOR_INDEX",
                    "a vertex attribute cannot use an index bufferView");
    }
    if (bv.buffer >= doc.buffers.size()) {
        return fail("MK2200_GLTF_ACCESSOR_INDEX", "bufferView.buffer is out of range");
    }
    if (bv.buffer >= buffers.size() ||
        buffers[static_cast<std::size_t>(bv.buffer)].data == nullptr) {
        return fail_with(ErrorCode::unsupported_feature, "MK2203_GLTF_BUFFER_UNAVAILABLE",
                         "buffer " + std::to_string(bv.buffer) +
                             " has no bytes available (an external buffer that was not loaded)");
    }
    const BufferSpan& span = buffers[static_cast<std::size_t>(bv.buffer)];
    const BufferDesc& declared_buffer = doc.buffers[static_cast<std::size_t>(bv.buffer)];
    if (span.size < declared_buffer.byte_length) {
        return fail("MK2204_GLTF_BUFFERVIEW_OOB",
                    "available buffer bytes are shorter than buffer.byteLength");
    }

    // The bufferView must fit both the declared buffer and the available bytes.
    auto declared_range =
        checked_range(bv.byte_offset, bv.byte_length, declared_buffer.byte_length, "bufferView");
    if (!declared_range.has_value()) {
        return fail("MK2204_GLTF_BUFFERVIEW_OOB",
                    "bufferView extends past the declared buffer.byteLength");
    }
    auto bv_range = checked_range(bv.byte_offset, bv.byte_length, span.size, "bufferView");
    if (!bv_range.has_value()) {
        return fail("MK2204_GLTF_BUFFERVIEW_OOB", "bufferView extends past the end of its buffer");
    }

    // Element geometry.
    const std::size_t comp_size = component_size(acc.component);
    const std::size_t comps = component_count(acc.element);
    if (comp_size == 0 || comps == 0) {
        return fail("MK2200_GLTF_ACCESSOR_INDEX", "accessor has an unknown component type");
    }
    auto element_size_result = checked_mul(comp_size, comps, "glTF accessor element size");
    if (!element_size_result.has_value()) {
        return fail("MK2206_GLTF_ACCESSOR_OOB", "accessor element size overflows");
    }
    const std::uint64_t element_size = element_size_result.value();
    const std::uint64_t stride = bv.byte_stride != 0 ? bv.byte_stride : element_size;
    if (acc.byte_offset % comp_size != 0) {
        return fail("MK2124_GLTF_ACCESSOR_MISALIGNED",
                    "accessor.byteOffset is not aligned to its component size");
    }
    if (acc.byte_offset % 4 != 0) {
        return fail("MK2124_GLTF_ACCESSOR_MISALIGNED",
                    "a vertex attribute accessor.byteOffset must be a multiple of 4");
    }
    if (bv.byte_stride != 0 &&
        (bv.byte_stride < 4 || bv.byte_stride > 252 || bv.byte_stride % 4 != 0)) {
        return fail("MK2205_GLTF_STRIDE_TOO_SMALL", "bufferView.byteStride is invalid");
    }
    if (bv.byte_stride != 0 && bv.byte_stride < element_size) {
        return fail("MK2205_GLTF_STRIDE_TOO_SMALL",
                    "bufferView.byteStride is smaller than the accessor element");
    }

    // The accessor's data (offset + (count-1)*stride + element_size) must lie within the
    // bufferView.
    if (acc.count != 0) {
        auto last = checked_mul(acc.count - 1, stride, "accessor span");
        if (last.has_value())
            last = checked_add(last.value(), acc.byte_offset, "accessor offset");
        if (!last.has_value()) {
            return fail("MK2206_GLTF_ACCESSOR_OOB", "accessor extent overflows");
        }
        auto within = checked_range(last.value(), element_size, bv.byte_length, "accessor in view");
        if (!within.has_value()) {
            return fail("MK2206_GLTF_ACCESSOR_OOB",
                        "accessor extends past the end of its bufferView");
        }
    }

    // Absolute offset into the buffer for the decoder.
    auto abs_offset = checked_add(bv.byte_offset, acc.byte_offset, "accessor absolute offset");
    if (!abs_offset.has_value()) {
        return fail("MK2206_GLTF_ACCESSOR_OOB", "accessor absolute offset overflows");
    }
    if (abs_offset.value() % comp_size != 0) {
        return fail("MK2124_GLTF_ACCESSOR_MISALIGNED",
                    "the absolute accessor offset is not aligned to its component size");
    }
    auto abs_size = checked_size_cast(abs_offset.value(), "accessor absolute offset");
    if (!abs_size.has_value()) {
        return fail("MK2206_GLTF_ACCESSOR_OOB", "accessor absolute offset does not fit in size_t");
    }

    AccessorView view;
    view.component = acc.component;
    view.element = acc.element;
    view.normalized = acc.normalized;
    auto count = checked_size_cast(acc.count, "accessor count");
    if (!count.has_value()) {
        return fail("MK2206_GLTF_ACCESSOR_OOB", "accessor count does not fit in size_t");
    }
    view.count = count.value();
    view.byte_offset = abs_size.value();
    auto host_stride = checked_size_cast(stride, "accessor byte stride");
    if (!host_stride.has_value()) {
        return fail("MK2206_GLTF_ACCESSOR_OOB", "accessor byte stride does not fit in size_t");
    }
    view.byte_stride = host_stride.value();
    auto decoded = context == nullptr ? decode_accessor(view, span.data, span.size)
                                      : decode_accessor(view, span.data, span.size, *context);
    if (!decoded.has_value())
        return decoded;
    auto bounds = validate_declared_bounds(acc, decoded.value());
    if (!bounds.has_value()) {
        return Result<DecodedAccessor>::failure(bounds.error_code(), bounds.diagnostics());
    }
    return decoded;
}

}  // namespace

Result<DecodedAccessor> resolve_and_decode_accessor(const Document& doc,
                                                    std::uint64_t accessor_index,
                                                    const std::vector<BufferSpan>& buffers,
                                                    const OperationContext& context) {
    if (context.budget == nullptr) {
        return fail_with(ErrorCode::internal_error, "MK0310_NO_BUDGET",
                         "the glTF accessor resolver requires a resource budget");
    }
    return resolve_and_decode_accessor_impl(doc, accessor_index, buffers, &context);
}

}  // namespace melkor::format::gltf
