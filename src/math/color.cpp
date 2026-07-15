#include "melkor/math/color.hpp"

#include <cmath>

namespace melkor::math {

float srgb_to_linear(float srgb) {
    // The exact piecewise sRGB electro-optical transfer function, not the gamma-2.2 shortcut.
    // The linear segment near zero matters for dark colours, where a pure power curve is wrong.
    if (srgb <= 0.04045f) {
        return srgb / 12.92f;
    }
    return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float linear) {
    if (linear <= 0.0031308f) {
        return linear * 12.92f;
    }
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

float rgb_to_sh_dc(float linear_rgb) {
    // The pinned 3DGS relation: rgb = SH_C0 * sh_dc + 0.5, inverted. Deliberately unclamped --
    // an SH DC term can legitimately be negative or large, and clamping here would corrupt the
    // exact value the format stored.
    return (linear_rgb - 0.5f) / kShC0;
}

float sh_dc_to_rgb(float sh_dc) {
    return sh_dc * kShC0 + 0.5f;
}

}  // namespace melkor::math
