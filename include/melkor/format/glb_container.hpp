// GLB binary container framing.
//
// A `.glb` file is a 12-byte header followed by a sequence of length-prefixed chunks: a required
// JSON chunk (the glTF document) and an optional BIN chunk (the binary buffer). Every one of those
// length fields is attacker-controlled, and the classic glTF parser bug is trusting a declared
// chunk length and reading past the end of the file. This module is the one place GLB framing is
// validated, with checked arithmetic on every offset, so no reader reinvents that bounds logic
// (and gets it subtly wrong).
//
// It is deliberately narrow: it validates *framing* only and returns byte ranges into the caller's
// original buffer. It does not parse the JSON, materialise the buffer, or allocate anything on the
// parse path, so it cannot itself be an allocation-bomb vector -- charging the budget belongs to
// the step that actually materialises the JSON or reads an accessor.
//
// Reference: glTF 2.0 specification, "GLB File Format Specification". Little-endian throughout.

#ifndef MELKOR_FORMAT_GLB_CONTAINER_HPP
#define MELKOR_FORMAT_GLB_CONTAINER_HPP

#include "melkor/checked.hpp"
#include "melkor/error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace melkor::format::glb {

// The fixed constants of the format (glTF 2.0). Stored little-endian on disk; these are the host
// values the little-endian readers/writers compare against.
inline constexpr std::uint32_t kMagic = 0x46546C67u;         // 'glTF'
inline constexpr std::uint32_t kVersion = 2u;                // Melkor targets glTF 2.0 only
inline constexpr std::uint32_t kChunkTypeJson = 0x4E4F534Au;  // 'JSON'
inline constexpr std::uint32_t kChunkTypeBin = 0x004E4942u;   // 'BIN\0'
inline constexpr std::size_t kHeaderSize = 12;
inline constexpr std::size_t kChunkHeaderSize = 8;
inline constexpr std::size_t kChunkAlignment = 4;  // every chunk length is a multiple of 4

// The framing of a parsed GLB. `json` and `bin` are ranges into the *original* buffer passed to
// `parse_glb`; they are valid only as long as that buffer lives. The JSON range may include the
// trailing space padding the format mandates -- that is harmless to a JSON parser. The BIN range
// likewise may include trailing zero padding, which accessors never address.
struct GlbFraming {
    ByteRange json;                 // the required JSON chunk's data
    std::optional<ByteRange> bin;   // the optional BIN chunk's data, if present
    std::uint32_t declared_length = 0;  // the header's total-length field (validated <= size)
};

// Parses and validates GLB framing with strict, overflow-safe bounds checking:
//   - the 12-byte header is present, the magic is 'glTF', and the version is exactly 2;
//   - the declared total length is at least the header size and no greater than the buffer;
//   - every chunk length is 4-byte aligned and its data lies wholly within the declared length,
//     with `offset + length` computed through checked arithmetic so a lying length cannot wrap;
//   - the first chunk is JSON, there is exactly one JSON chunk, and at most one BIN chunk;
//   - unknown chunk types are skipped, as the spec requires clients to ignore them.
// Returns a clean failure (never a crash or an out-of-bounds read) on any malformed input.
Result<GlbFraming> parse_glb(const std::uint8_t* data, std::size_t size);

// Builds a GLB from a JSON document and an optional binary buffer, applying the mandated 4-byte
// padding (trailing spaces for JSON, trailing zeros for BIN) and writing a correct total-length
// header. Fails, rather than truncating, if the assembled size would exceed the 32-bit length
// field. Round-trips with `parse_glb`.
Result<std::vector<std::uint8_t>> build_glb(std::string_view json, const std::uint8_t* bin,
                                            std::size_t bin_size);

}  // namespace melkor::format::glb

#endif  // MELKOR_FORMAT_GLB_CONTAINER_HPP
