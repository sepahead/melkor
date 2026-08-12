#include "melkor/format/glb_container.hpp"

#include <cstring>
#include <new>
#include <stdexcept>

namespace melkor::format::glb {

namespace {

// Reads a little-endian uint32 from data[pos..pos+4). The caller guarantees the four bytes exist.
// Written byte-by-byte rather than a memcpy of native bytes so it is correct on a big-endian host
// too -- GLB is little-endian regardless of the machine.
std::uint32_t read_le_u32(const std::uint8_t* data, std::size_t pos) noexcept {
    return static_cast<std::uint32_t>(data[pos]) |
           (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
           (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
           (static_cast<std::uint32_t>(data[pos + 3]) << 24);
}

void write_le_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

Diagnostic glb_error(const char* code, std::string message) {
    return Diagnostic(code, Severity::error, std::move(message));
}

Result<GlbFraming> fail(const char* code, std::string message, std::uint64_t offset) {
    Diagnostic d = glb_error(code, std::move(message));
    d.with_offset(offset);
    return Result<GlbFraming>::failure(ErrorCode::invalid_data, std::move(d));
}

std::size_t padding_to_alignment(std::size_t n) noexcept {
    const std::size_t rem = n % kChunkAlignment;
    return rem == 0 ? 0 : (kChunkAlignment - rem);
}

struct GlbBuildLayout {
    std::uint64_t total_size = 0;
    std::uint32_t total_size_u32 = 0;
    std::uint32_t json_chunk_size = 0;
    std::uint32_t bin_chunk_size = 0;
    std::uint64_t bin_offset = 0;
    std::size_t json_padding = 0;
    bool have_bin = false;
};

Result<GlbBuildLayout> calculate_build_layout(std::size_t json_size, std::size_t bin_size) {
    const std::uint64_t json_size_u64 = static_cast<std::uint64_t>(json_size);
    const std::uint64_t bin_size_u64 = static_cast<std::uint64_t>(bin_size);
    const std::size_t json_padding = padding_to_alignment(json_size);
    const std::size_t bin_padding = padding_to_alignment(bin_size);
    const bool have_bin = bin_size > 0;

    auto json_chunk_size = checked_add(json_size_u64, json_padding, "GLB JSON chunk size");
    if (!json_chunk_size.has_value()) {
        return Result<GlbBuildLayout>::failure(json_chunk_size.error_code(),
                                               json_chunk_size.diagnostics());
    }
    auto json_chunk_u32 = checked_u32_cast(json_chunk_size.value(), "GLB JSON chunk size");
    if (!json_chunk_u32.has_value()) {
        Diagnostic diagnostic = glb_error(
            "MK2112_GLB_TOO_LARGE", "the JSON chunk is larger than the GLB 32-bit format limit");
        return Result<GlbBuildLayout>::failure(ErrorCode::unsupported_feature,
                                               std::move(diagnostic));
    }

    auto total = checked_add(kHeaderSize, kChunkHeaderSize, "GLB size");
    if (total.has_value()) {
        total = checked_add(total.value(), json_chunk_size.value(), "GLB size");
    }

    std::uint64_t bin_offset = 0;
    std::uint32_t bin_chunk_u32 = 0;
    if (have_bin && total.has_value()) {
        total = checked_add(total.value(), kChunkHeaderSize, "GLB size");
        if (total.has_value()) {
            bin_offset = total.value();
        }
        auto bin_chunk_size = checked_add(bin_size_u64, bin_padding, "GLB BIN chunk size");
        if (!bin_chunk_size.has_value()) {
            return Result<GlbBuildLayout>::failure(bin_chunk_size.error_code(),
                                                   bin_chunk_size.diagnostics());
        }
        auto checked_bin_u32 = checked_u32_cast(bin_chunk_size.value(), "GLB BIN chunk size");
        if (!checked_bin_u32.has_value()) {
            Diagnostic diagnostic = glb_error(
                "MK2112_GLB_TOO_LARGE", "the BIN chunk is larger than the GLB 32-bit format limit");
            return Result<GlbBuildLayout>::failure(ErrorCode::unsupported_feature,
                                                   std::move(diagnostic));
        }
        bin_chunk_u32 = checked_bin_u32.value();
        if (total.has_value()) {
            total = checked_add(total.value(), bin_chunk_size.value(), "GLB size");
        }
    }
    if (!total.has_value()) {
        return Result<GlbBuildLayout>::failure(total.error_code(), total.diagnostics());
    }
    auto total_u32 = checked_u32_cast(total.value(), "GLB total length");
    if (!total_u32.has_value()) {
        Diagnostic diagnostic = glb_error(
            "MK2112_GLB_TOO_LARGE", "the assembled GLB is larger than the 4 GiB format limit");
        return Result<GlbBuildLayout>::failure(ErrorCode::unsupported_feature,
                                               std::move(diagnostic));
    }

    GlbBuildLayout layout;
    layout.total_size = total.value();
    layout.total_size_u32 = total_u32.value();
    layout.json_chunk_size = json_chunk_u32.value();
    layout.bin_chunk_size = bin_chunk_u32;
    layout.bin_offset = bin_offset;
    layout.json_padding = json_padding;
    layout.have_bin = have_bin;
    return Result<GlbBuildLayout>::success(layout);
}

template <class T> Result<T> allocation_failure(const char* message) {
    Diagnostic diagnostic = glb_error("MK2113_GLB_ALLOCATION_FAILED", message);
    return Result<T>::failure(ErrorCode::resource_limit, std::move(diagnostic));
}

}  // namespace

Result<GlbFraming> parse_glb(const std::uint8_t* data, std::size_t size) try {
    if (data == nullptr || size < kHeaderSize) {
        return fail("MK2101_GLB_TRUNCATED_HEADER", "GLB is shorter than the 12-byte header", 0);
    }

    const std::uint32_t magic = read_le_u32(data, 0);
    if (magic != kMagic) {
        return fail("MK2102_GLB_BAD_MAGIC",
                    "not a GLB: the first four bytes are not the 'glTF' magic", 0);
    }
    const std::uint32_t version = read_le_u32(data, 4);
    if (version != kVersion) {
        return fail("MK2103_GLB_BAD_VERSION",
                    "unsupported GLB version " + std::to_string(version) +
                        "; Melkor targets glTF 2.0 (version 2)",
                    4);
    }
    const std::uint32_t declared_length = read_le_u32(data, 8);
    if (declared_length < kHeaderSize) {
        return fail("MK2104_GLB_BAD_LENGTH",
                    "GLB header length " + std::to_string(declared_length) +
                        " is smaller than the 12-byte header",
                    8);
    }
    if (declared_length > size) {
        return fail("MK2104_GLB_BAD_LENGTH",
                    "GLB header declares " + std::to_string(declared_length) + " bytes but only " +
                        std::to_string(size) + " are present (truncated)",
                    8);
    }
    if (declared_length != size) {
        return fail("MK2104_GLB_BAD_LENGTH",
                    "GLB header declares " + std::to_string(declared_length) +
                        " bytes but the file contains " + std::to_string(size),
                    8);
    }

    // Chunk iteration uses the declared length after the exact-size check.
    const std::uint64_t length = declared_length;

    GlbFraming framing;
    framing.declared_length = declared_length;
    bool have_json = false;
    bool have_bin = false;
    bool first_chunk = true;
    std::size_t chunk_index = 0;

    std::uint64_t offset = kHeaderSize;
    while (offset < length) {
        // The 8-byte chunk header must fit within the declared length.
        auto header_end = checked_add(offset, kChunkHeaderSize, "chunk header");
        if (!header_end.has_value() || header_end.value() > length) {
            return fail("MK2105_GLB_CHUNK_HEADER_OOB",
                        "a chunk header extends past the end of the GLB", offset);
        }
        const std::uint32_t chunk_length = read_le_u32(data, static_cast<std::size_t>(offset));
        const std::uint32_t chunk_type = read_le_u32(data, static_cast<std::size_t>(offset + 4));

        if (chunk_length % kChunkAlignment != 0) {
            return fail("MK2106_GLB_CHUNK_MISALIGNED",
                        "chunk length " + std::to_string(chunk_length) + " is not a multiple of 4",
                        offset);
        }

        const std::uint64_t data_start = header_end.value();
        // Validate [data_start, data_start + chunk_length) lies within the declared length. This
        // is the wraparound-safe check: a huge chunk_length cannot pass by overflowing.
        auto range = checked_range(data_start, chunk_length, length, "chunk data");
        if (!range.has_value()) {
            return fail("MK2107_GLB_CHUNK_DATA_OOB",
                        "chunk data of length " + std::to_string(chunk_length) +
                            " extends past the end of the GLB",
                        offset);
        }

        if (chunk_type == kChunkTypeJson) {
            if (have_json) {
                return fail("MK2109_GLB_DUPLICATE_JSON",
                            "a GLB must contain exactly one JSON chunk", offset);
            }
            if (!first_chunk) {
                return fail("MK2108_GLB_JSON_NOT_FIRST",
                            "the JSON chunk must be the first chunk in a GLB", offset);
            }
            have_json = true;
            framing.json = range.value();
        } else if (chunk_type == kChunkTypeBin) {
            if (first_chunk) {
                return fail("MK2108_GLB_JSON_NOT_FIRST",
                            "the first chunk of a GLB must be JSON, not BIN", offset);
            }
            if (have_bin) {
                return fail("MK2110_GLB_DUPLICATE_BIN", "a GLB must contain at most one BIN chunk",
                            offset);
            }
            if (chunk_index != 1) {
                return fail("MK2116_GLB_BIN_NOT_SECOND",
                            "the BIN chunk must be the second GLB chunk", offset);
            }
            have_bin = true;
            framing.bin = range.value();
        } else {
            // Unknown chunk type: the spec requires clients to ignore it. But an unknown type
            // cannot be the first chunk, which must be JSON.
            if (first_chunk) {
                return fail("MK2108_GLB_JSON_NOT_FIRST", "the first chunk of a GLB must be JSON",
                            offset);
            }
            ++framing.unknown_chunk_count;
        }

        offset = range.value().end();
        first_chunk = false;
        ++chunk_index;
    }

    if (!have_json) {
        return fail("MK2111_GLB_MISSING_JSON", "a GLB must contain a JSON chunk", 0);
    }
    return Result<GlbFraming>::success(framing);
} catch (const std::bad_alloc&) {
    return allocation_failure<GlbFraming>("memory allocation failed while parsing the GLB");
} catch (const std::length_error&) {
    return allocation_failure<GlbFraming>("a GLB diagnostic exceeded a container limit");
}

Result<std::uint64_t> encoded_glb_size(std::size_t json_size, std::size_t bin_size) try {
    auto layout = calculate_build_layout(json_size, bin_size);
    if (!layout.has_value()) {
        return Result<std::uint64_t>::failure(layout.error_code(), layout.diagnostics());
    }
    return Result<std::uint64_t>::success(layout.value().total_size);
} catch (const std::bad_alloc&) {
    return allocation_failure<std::uint64_t>("memory allocation failed while sizing the GLB");
} catch (const std::length_error&) {
    return allocation_failure<std::uint64_t>("a GLB size diagnostic exceeded a container limit");
}

Result<GlbBuildBuffer> build_glb_buffer(std::string_view json, std::size_t bin_size) try {
    auto layout = calculate_build_layout(json.size(), bin_size);
    if (!layout.has_value()) {
        return Result<GlbBuildBuffer>::failure(layout.error_code(), layout.diagnostics());
    }
    auto total_size = checked_size_cast(layout.value().total_size, "GLB total length");
    if (!total_size.has_value()) {
        return Result<GlbBuildBuffer>::failure(total_size.error_code(), total_size.diagnostics());
    }

    GlbBuildBuffer result;
    result.bytes.reserve(total_size.value());

    write_le_u32(result.bytes, kMagic);
    write_le_u32(result.bytes, kVersion);
    write_le_u32(result.bytes, layout.value().total_size_u32);
    write_le_u32(result.bytes, layout.value().json_chunk_size);
    write_le_u32(result.bytes, kChunkTypeJson);
    result.bytes.insert(result.bytes.end(), json.begin(), json.end());
    result.bytes.insert(result.bytes.end(), layout.value().json_padding,
                        static_cast<std::uint8_t>(' '));

    if (layout.value().have_bin) {
        write_le_u32(result.bytes, layout.value().bin_chunk_size);
        write_le_u32(result.bytes, kChunkTypeBin);
        auto payload = checked_range(layout.value().bin_offset, bin_size, layout.value().total_size,
                                     "GLB BIN payload");
        if (!payload.has_value()) {
            return Result<GlbBuildBuffer>::failure(payload.error_code(), payload.diagnostics());
        }
        result.bin_payload = payload.value();
        result.bytes.resize(total_size.value(), 0);
    }

    if (result.bytes.size() != total_size.value()) {
        Diagnostic diagnostic =
            glb_error("MK2114_GLB_BUILD_INVARIANT", "the GLB builder produced an incorrect size");
        return Result<GlbBuildBuffer>::failure(ErrorCode::internal_error, std::move(diagnostic));
    }
    return Result<GlbBuildBuffer>::success(std::move(result));
} catch (const std::bad_alloc&) {
    return allocation_failure<GlbBuildBuffer>("memory allocation failed while building the GLB");
} catch (const std::length_error&) {
    return allocation_failure<GlbBuildBuffer>("the GLB exceeds a container allocation limit");
}

Result<std::vector<std::uint8_t>> build_glb(std::string_view json, const std::uint8_t* bin,
                                            std::size_t bin_size) try {
    if (bin == nullptr && bin_size != 0) {
        Diagnostic diagnostic =
            glb_error("MK2115_GLB_NULL_BIN", "the GLB BIN pointer is null for a nonzero size");
        return Result<std::vector<std::uint8_t>>::failure(ErrorCode::invalid_argument,
                                                          std::move(diagnostic));
    }
    auto built = build_glb_buffer(json, bin_size);
    if (!built.has_value()) {
        return Result<std::vector<std::uint8_t>>::failure(built.error_code(), built.diagnostics());
    }
    if (bin_size > 0) {
        const std::size_t offset = static_cast<std::size_t>(built.value().bin_payload.offset());
        std::memcpy(built.value().bytes.data() + offset, bin, bin_size);
    }
    return Result<std::vector<std::uint8_t>>::success(std::move(built.value().bytes));
} catch (const std::bad_alloc&) {
    return allocation_failure<std::vector<std::uint8_t>>(
        "memory allocation failed while building the GLB");
} catch (const std::length_error&) {
    return allocation_failure<std::vector<std::uint8_t>>(
        "the GLB exceeds a container allocation limit");
}

}  // namespace melkor::format::glb
