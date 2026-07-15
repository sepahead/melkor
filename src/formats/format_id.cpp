#include "melkor/format/format_id.hpp"

namespace melkor {

const char* to_string(FormatId id) noexcept {
    switch (id) {
        case FormatId::unknown: return "unknown";
        case FormatId::ply:     return "ply";
        case FormatId::spz:     return "spz";
        case FormatId::gltf:    return "gltf";
        case FormatId::glb:     return "glb";
    }
    return "unknown";
}

}  // namespace melkor
