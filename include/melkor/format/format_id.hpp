// Format identity and capabilities.
//
// A stable integer FormatId, not a display string, identifies a container. Capabilities describe
// what a format can and cannot represent, so the conversion planner can compute the loss report
// (loss.hpp) from data rather than from a pile of special cases: if the source has SH degree 4
// and the target's max_sh_degree is 3, that is a `LOSS_SH_DEGREE_TRUNCATED`, derived, not
// hard-coded per format pair.

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

const char* to_string(FormatId id) noexcept;

// What a format can represent. The planner compares source and target capabilities to predict
// loss; a reader/writer that claims a capability must actually honour it.
struct FormatCapabilities {
    bool can_probe = false;
    bool can_read = false;
    bool can_write = false;
    bool supports_scene_graph = false;
    bool supports_multiple_primitives = false;
    std::uint8_t max_sh_degree = 0;
    bool preserves_antialiasing = false;
    bool preserves_color_space = false;
    bool preserves_provenance = false;
};

}  // namespace melkor

#endif  // MELKOR_FORMAT_FORMAT_ID_HPP
