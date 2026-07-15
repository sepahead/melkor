#include "melkor/format/glb_container.hpp"

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

}  // namespace

Result<GlbFraming> parse_glb(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < kHeaderSize) {
        return fail("MK2101_GLB_TRUNCATED_HEADER",
                    "GLB is shorter than the 12-byte header", 0);
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
                    "GLB header declares " + std::to_string(declared_length) +
                        " bytes but only " + std::to_string(size) + " are present (truncated)",
                    8);
    }

    // Chunk iteration is bounded by the *declared* length, never the raw buffer size: bytes past
    // the declared length are not part of the GLB. `length` is our authoritative end.
    const std::uint64_t length = declared_length;

    GlbFraming framing;
    framing.declared_length = declared_length;
    bool have_json = false;
    bool have_bin = false;
    bool first_chunk = true;

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
                        "chunk length " + std::to_string(chunk_length) +
                            " is not a multiple of 4",
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
            if (!first_chunk) {
                return fail("MK2108_GLB_JSON_NOT_FIRST",
                            "the JSON chunk must be the first chunk in a GLB", offset);
            }
            if (have_json) {
                return fail("MK2109_GLB_DUPLICATE_JSON",
                            "a GLB must contain exactly one JSON chunk", offset);
            }
            have_json = true;
            framing.json = range.value();
        } else if (chunk_type == kChunkTypeBin) {
            if (first_chunk) {
                return fail("MK2108_GLB_JSON_NOT_FIRST",
                            "the first chunk of a GLB must be JSON, not BIN", offset);
            }
            if (have_bin) {
                return fail("MK2110_GLB_DUPLICATE_BIN",
                            "a GLB must contain at most one BIN chunk", offset);
            }
            have_bin = true;
            framing.bin = range.value();
        } else {
            // Unknown chunk type: the spec requires clients to ignore it. But an unknown type
            // cannot be the first chunk, which must be JSON.
            if (first_chunk) {
                return fail("MK2108_GLB_JSON_NOT_FIRST",
                            "the first chunk of a GLB must be JSON", offset);
            }
        }

        offset = range.value().end();
        first_chunk = false;
    }

    if (!have_json) {
        return fail("MK2111_GLB_MISSING_JSON", "a GLB must contain a JSON chunk", 0);
    }
    return Result<GlbFraming>::success(framing);
}

Result<std::vector<std::uint8_t>> build_glb(std::string_view json, const std::uint8_t* bin,
                                            std::size_t bin_size) {
    const std::size_t json_pad = padding_to_alignment(json.size());
    const std::size_t bin_pad = padding_to_alignment(bin_size);
    const bool have_bin = bin != nullptr && bin_size > 0;

    // Assemble the total length with checked arithmetic: header + JSON chunk (+ BIN chunk),
    // failing rather than silently truncating if it overflows the 32-bit length field.
    auto total = checked_add(kHeaderSize, kChunkHeaderSize, "glb size");
    if (total.has_value()) total = checked_add(total.value(), json.size(), "glb size");
    if (total.has_value()) total = checked_add(total.value(), json_pad, "glb size");
    if (have_bin) {
        if (total.has_value()) total = checked_add(total.value(), kChunkHeaderSize, "glb size");
        if (total.has_value()) total = checked_add(total.value(), bin_size, "glb size");
        if (total.has_value()) total = checked_add(total.value(), bin_pad, "glb size");
    }
    if (!total.has_value()) {
        return Result<std::vector<std::uint8_t>>::failure(total.error_code(), total.diagnostics());
    }
    auto total_u32 = checked_u32_cast(total.value(), "glb total length");
    if (!total_u32.has_value()) {
        Diagnostic d = glb_error("MK2112_GLB_TOO_LARGE",
                                 "the assembled GLB is larger than the 4 GiB the format's 32-bit "
                                 "length field can represent");
        return Result<std::vector<std::uint8_t>>::failure(ErrorCode::invalid_data, std::move(d));
    }

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(total.value()));

    // Header.
    write_le_u32(out, kMagic);
    write_le_u32(out, kVersion);
    write_le_u32(out, total_u32.value());

    // JSON chunk: length includes the space padding.
    write_le_u32(out, static_cast<std::uint32_t>(json.size() + json_pad));
    write_le_u32(out, kChunkTypeJson);
    out.insert(out.end(), json.begin(), json.end());
    out.insert(out.end(), json_pad, static_cast<std::uint8_t>(' '));

    // BIN chunk: length includes the zero padding.
    if (have_bin) {
        write_le_u32(out, static_cast<std::uint32_t>(bin_size + bin_pad));
        write_le_u32(out, kChunkTypeBin);
        out.insert(out.end(), bin, bin + bin_size);
        out.insert(out.end(), bin_pad, static_cast<std::uint8_t>(0));
    }

    return Result<std::vector<std::uint8_t>>::success(std::move(out));
}

}  // namespace melkor::format::glb
