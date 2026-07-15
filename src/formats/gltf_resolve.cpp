#include "melkor/format/gltf_resolve.hpp"

#include "melkor/checked.hpp"
#include "melkor/format/gltf_accessor.hpp"

#include <string>

namespace melkor::format::gltf {

namespace {

Result<std::vector<float>> fail(const char* code, std::string message) {
    Diagnostic d(code, Severity::error, std::move(message));
    return Result<std::vector<float>>::failure(ErrorCode::invalid_data, std::move(d));
}

}  // namespace

Result<std::vector<float>> resolve_and_decode_accessor(const Document& doc,
                                                       std::uint64_t accessor_index,
                                                       const std::vector<BufferSpan>& buffers) {
    if (accessor_index >= doc.accessors.size()) {
        return fail("MK2140_GLTF_ACCESSOR_INDEX", "accessor index is out of range");
    }
    const AccessorDesc& acc = doc.accessors[static_cast<std::size_t>(accessor_index)];

    if (acc.is_sparse) {
        return fail("MK2141_GLTF_SPARSE_UNSUPPORTED",
                    "sparse accessors are not supported for splat attributes");
    }
    if (!acc.has_buffer_view) {
        return fail("MK2142_GLTF_NO_BUFFERVIEW",
                    "a splat attribute accessor must reference a bufferView");
    }

    // The parser guaranteed these indices are in range; assert-by-check anyway so this function is
    // safe in isolation.
    if (acc.buffer_view >= doc.buffer_views.size()) {
        return fail("MK2140_GLTF_ACCESSOR_INDEX", "accessor.bufferView is out of range");
    }
    const BufferViewDesc& bv = doc.buffer_views[static_cast<std::size_t>(acc.buffer_view)];
    if (bv.buffer >= doc.buffers.size()) {
        return fail("MK2140_GLTF_ACCESSOR_INDEX", "bufferView.buffer is out of range");
    }
    if (bv.buffer >= buffers.size() || buffers[static_cast<std::size_t>(bv.buffer)].data == nullptr) {
        return fail("MK2143_GLTF_BUFFER_UNAVAILABLE",
                    "buffer " + std::to_string(bv.buffer) +
                        " has no bytes available (an external buffer that was not loaded)");
    }
    const BufferSpan& span = buffers[static_cast<std::size_t>(bv.buffer)];

    // The bufferView must lie within its buffer's bytes.
    auto bv_range = checked_range(bv.byte_offset, bv.byte_length, span.size, "bufferView");
    if (!bv_range.has_value()) {
        return fail("MK2144_GLTF_BUFFERVIEW_OOB",
                    "bufferView extends past the end of its buffer");
    }

    // Element geometry.
    const std::size_t comp_size = component_size(acc.component);
    const std::size_t comps = component_count(acc.element);
    if (comp_size == 0) {
        return fail("MK2140_GLTF_ACCESSOR_INDEX", "accessor has an unknown component type");
    }
    const std::uint64_t element_size = static_cast<std::uint64_t>(comp_size) * comps;
    const std::uint64_t stride = bv.byte_stride != 0 ? bv.byte_stride : element_size;
    if (bv.byte_stride != 0 && bv.byte_stride < element_size) {
        return fail("MK2145_GLTF_STRIDE_TOO_SMALL",
                    "bufferView.byteStride is smaller than the accessor element");
    }

    // The accessor's data (offset + (count-1)*stride + element_size) must lie within the bufferView.
    if (acc.count != 0) {
        auto last = checked_mul(acc.count - 1, stride, "accessor span");
        if (last.has_value()) last = checked_add(last.value(), acc.byte_offset, "accessor offset");
        if (!last.has_value()) {
            return fail("MK2146_GLTF_ACCESSOR_OOB", "accessor extent overflows");
        }
        auto within = checked_range(last.value(), element_size, bv.byte_length, "accessor in view");
        if (!within.has_value()) {
            return fail("MK2146_GLTF_ACCESSOR_OOB",
                        "accessor extends past the end of its bufferView");
        }
    }

    // Absolute offset into the buffer for the decoder.
    auto abs_offset = checked_add(bv.byte_offset, acc.byte_offset, "accessor absolute offset");
    if (!abs_offset.has_value()) {
        return fail("MK2146_GLTF_ACCESSOR_OOB", "accessor absolute offset overflows");
    }
    auto abs_size = checked_size_cast(abs_offset.value(), "accessor absolute offset");
    if (!abs_size.has_value()) {
        return fail("MK2146_GLTF_ACCESSOR_OOB", "accessor absolute offset does not fit in size_t");
    }

    AccessorView view;
    view.component = acc.component;
    view.element = acc.element;
    view.normalized = acc.normalized;
    view.count = static_cast<std::size_t>(acc.count);
    view.byte_offset = abs_size.value();
    view.byte_stride = static_cast<std::size_t>(stride);
    return decode_accessor(view, span.data, span.size);
}

}  // namespace melkor::format::gltf
