// Canonical color-space identity.

#ifndef MELKOR_COLOR_SPACE_HPP
#define MELKOR_COLOR_SPACE_HPP

#include <cstdint>
#include <optional>
#include <string_view>

namespace melkor {

// These values match KHR_gaussian_splatting. They describe reconstructed splat colors.
enum class ColorSpace : std::uint8_t {
    srgb_rec709_display = 0,
    lin_rec709_display = 1,
};

const char* to_string(ColorSpace space) noexcept;
std::optional<ColorSpace> color_space_from_string(std::string_view value) noexcept;
bool is_valid(ColorSpace space) noexcept;

}  // namespace melkor

#endif  // MELKOR_COLOR_SPACE_HPP
