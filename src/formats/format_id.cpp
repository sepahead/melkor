#include "melkor/format/format_id.hpp"

namespace melkor {

bool is_known_format(FormatId id) noexcept {
    switch (id) {
    case FormatId::ply:
    case FormatId::spz:
    case FormatId::gltf:
    case FormatId::glb:
        return true;
    case FormatId::unknown:
        return false;
    }
    return false;
}

const char* to_string(FormatId id) noexcept {
    switch (id) {
    case FormatId::unknown:
        return "unknown";
    case FormatId::ply:
        return "ply";
    case FormatId::spz:
        return "spz";
    case FormatId::gltf:
        return "gltf";
    case FormatId::glb:
        return "glb";
    }
    return "unknown";
}

}  // namespace melkor
