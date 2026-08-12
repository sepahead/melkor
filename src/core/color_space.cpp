#include "melkor/color_space.hpp"

namespace melkor {

const char* to_string(ColorSpace space) noexcept {
    switch (space) {
    case ColorSpace::srgb_rec709_display:
        return "srgb_rec709_display";
    case ColorSpace::lin_rec709_display:
        return "lin_rec709_display";
    }
    return "unknown";
}

std::optional<ColorSpace> color_space_from_string(std::string_view value) noexcept {
    if (value == "srgb_rec709_display")
        return ColorSpace::srgb_rec709_display;
    if (value == "lin_rec709_display")
        return ColorSpace::lin_rec709_display;
    return std::nullopt;
}

bool is_valid(ColorSpace space) noexcept {
    return space == ColorSpace::srgb_rec709_display || space == ColorSpace::lin_rec709_display;
}

}  // namespace melkor
