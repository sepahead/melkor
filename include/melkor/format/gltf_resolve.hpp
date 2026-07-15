// glTF accessor resolution.
//
// Decoding a glTF attribute means following a two-level indirection: an accessor names a
// bufferView (a slice of a buffer with an optional stride) and an offset within it, and the
// bufferView names a buffer and its own offset. This module composes those offsets, validates that
// the accessor's data lies within its bufferView and the bufferView within its buffer, and then
// decodes the bytes. It is the bridge between the parsed Document (which knows the indices) and the
// accessor decoder (which knows the numeric layout).
//
// Every bound is re-checked here with checked arithmetic even though the parser already validated
// the index graph, because the parser validated *indices*, not *byte extents*: an accessor whose
// declared count times stride runs off the end of its bufferView is a distinct, attacker-reachable
// error the parser does not catch.

#ifndef MELKOR_FORMAT_GLTF_RESOLVE_HPP
#define MELKOR_FORMAT_GLTF_RESOLVE_HPP

#include "melkor/error.hpp"
#include "melkor/format/gltf_document.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace melkor::format::gltf {

// A buffer's bytes, indexed by glTF buffer index. `data` may be null for a buffer whose bytes were
// not supplied (an external URI the reader declined to fetch); resolving an accessor into such a
// buffer is a clean error, not a crash.
struct BufferSpan {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

// Resolves accessor `accessor_index` in `doc` against `buffers` and decodes it into a flat,
// row-major vector of floats (see decode_accessor). Fails cleanly if:
//   - the index is out of range, or the accessor has no bufferView or is sparse (unsupported for
//     splat attributes);
//   - the referenced buffer's bytes were not supplied;
//   - the bufferView runs past the end of its buffer, or the accessor runs past the end of its
//     bufferView.
Result<std::vector<float>> resolve_and_decode_accessor(const Document& doc,
                                                       std::uint64_t accessor_index,
                                                       const std::vector<BufferSpan>& buffers);

}  // namespace melkor::format::gltf

#endif  // MELKOR_FORMAT_GLTF_RESOLVE_HPP
