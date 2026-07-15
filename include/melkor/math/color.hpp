// Colour conversions.
//
// Two distinct conversions live here, and conflating them is a common source of dark or washed-
// out splats:
//
//   1. sRGB <-> linear RGB. A gamma (transfer-function) conversion. A texture's bytes are sRGB;
//      lighting and blending happen in linear. Getting the direction wrong makes a scene look
//      too dark or too bright.
//   2. linear RGB <-> the degree-0 spherical-harmonic coefficient (the "DC" term). This is the
//      pinned 3DGS relation `rgb = SH_C0 * sh_dc + 0.5`, i.e. `sh_dc = (rgb - 0.5) / SH_C0`.
//      It is NOT a gamma conversion, and applying it in place of one -- or twice -- corrupts
//      colour.
//
// Colour-space conversion is not clamping: an HDR or SH-derived value can be mathematically
// valid even when it falls outside displayable [0,1]. A writer that must land in a bounded range
// reports the clipping as an explicit loss; it does not silently clamp here.

#ifndef MELKOR_MATH_COLOR_HPP
#define MELKOR_MATH_COLOR_HPP

#include <cstdint>

namespace melkor::math {

// The zeroth-order real spherical-harmonic basis value. The pinned 3DGS DC<->RGB relation is
// built on this constant; it is defined once here so no adapter hard-codes its own copy.
inline constexpr float kShC0 = 0.28209479177387814f;

// sRGB encoded channel in [0,1] -> linear RGB in [0,1]. The standard piecewise sRGB transfer
// function (a small linear segment near zero, a gamma curve above it), not the 2.2 approximation.
float srgb_to_linear(float srgb);

// linear RGB channel -> sRGB encoded. Inverse of srgb_to_linear.
float linear_to_srgb(float linear);

// linear RGB channel -> degree-0 SH coefficient, via the pinned relation. The result may be
// negative or exceed the input range; that is correct, not an error.
float rgb_to_sh_dc(float linear_rgb);

// degree-0 SH coefficient -> linear RGB channel. Inverse of rgb_to_sh_dc.
float sh_dc_to_rgb(float sh_dc);

}  // namespace melkor::math

#endif  // MELKOR_MATH_COLOR_HPP
