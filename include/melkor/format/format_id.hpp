// Stable identifiers for the containers that Melkor supports.

#ifndef MELKOR_FORMAT_FORMAT_ID_HPP
#define MELKOR_FORMAT_FORMAT_ID_HPP

#include <cstdint>

namespace melkor {

enum class FormatId : std::uint32_t {
    unknown = 0,
    ply = 1,
    spz = 2,
    gltf = 3,
    glb = 4,
};

// Return true only for a supported container ID. `unknown` is a sentinel, not a container.
bool is_known_format(FormatId id) noexcept;

const char* to_string(FormatId id) noexcept;

}  // namespace melkor

#endif  // MELKOR_FORMAT_FORMAT_ID_HPP
