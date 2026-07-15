// KHR_gaussian_splatting layout core.
//
// This module is the semantic contract of the glTF `KHR_gaussian_splatting` extension, pinned to
// the Khronos release-candidate commit recorded in `third_party/specifications.lock.json`
// (63770cc70a37). It is deliberately pure: attribute-semantic naming, the spherical-harmonic
// coefficient ordering, the spec's covariance-building `C` matrix, and the small set of required
// enumerated strings -- no file I/O and no glTF container parsing. The reader and the writer both
// build on this so that the on-the-wire meaning of a splat lives in exactly one, unit-tested place.
//
// Why a separate module: the subtle, easy-to-get-wrong parts of this format are not the container
// framing -- they are the SH `m`-ordering (COEF_n runs from m=-l to m=+l), the quaternion order
// (glTF is x,y,z,w), and the covariance construction. Isolating them here lets a test pin each one
// against the vendored spec text directly.
//
// The pinned profile supports SH degrees 0-3. The canonical Melkor scene stores 0-4, so a degree-4
// source targeting this profile is a `LOSS_SH_DEGREE_TRUNCATED` loss (see format/loss.hpp), never a
// silent truncation. That policy decision is enforced by the adapter, not here; this module only
// states the profile's degree ceiling.

#ifndef MELKOR_FORMAT_GLTF_KHR_HPP
#define MELKOR_FORMAT_GLTF_KHR_HPP

#include "melkor/math/quaternion.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace melkor::format::khr {

// ---- Spec identity (pinned RC 63770cc70a37) -------------------------------------------------

inline constexpr const char* kExtensionName = "KHR_gaussian_splatting";
inline constexpr const char* kKernelEllipse = "ellipse";
inline constexpr const char* kProjectionPerspective = "perspective";      // spec default
inline constexpr const char* kSortingCameraDistance = "cameraDistance";   // spec default

// The mesh primitive MUST use POINTS mode, and the referenced material MUST be ignored for splat
// rendering. These are the two hard glTF dependencies the extension imposes.
inline constexpr int kPrimitiveModePoints = 0;

// This profile supports SH degrees 0..3 inclusive (COEF counts 1/3/5/7). The canonical scene
// supports 0..4; the adapter converts a degree-4 source with LOSS_SH_DEGREE_TRUNCATED.
inline constexpr std::uint32_t kMaxProfileShDegree = 3;

// ---- Colour space ---------------------------------------------------------------------------
//
// `colorSpace` is REQUIRED and refers only to the reconstructed splat colour values. The two
// values the base extension defines are display-referred BT.709 sRGB and linear. An unknown string
// is allowed by the schema (the property is open) but is not one Melkor can interpret, so it is
// surfaced as an assumption (LOSS_COLOR_SPACE_ASSUMED), never silently treated as sRGB.

enum class ColorSpace : std::uint8_t {
    srgb_rec709_display,  // "srgb_rec709_display": BT.709 sRGB, display-referred
    lin_rec709_display,   // "lin_rec709_display": BT.709 linear, display-referred
};

const char* to_string(ColorSpace space) noexcept;

// Parses one of the two defined colour-space strings. Returns nullopt for any other string,
// including the empty string; the caller decides the policy for an unknown-but-present value.
std::optional<ColorSpace> color_space_from_string(std::string_view s) noexcept;

// ---- Spherical-harmonic layout --------------------------------------------------------------
//
// The extension stores each SH coefficient as its own VEC3 float accessor named
// `KHR_gaussian_splatting:SH_DEGREE_{l}_COEF_{n}`. For degree l there are exactly 2l+1 coefficients
// (n in [0, 2l]), packed from the lowest order m=-l (COEF_0) to the highest m=+l (COEF_2l). Melkor's
// canonical ShBuffer is coefficient-major over the (degree+1)^2 coefficients in the same order --
// DC at flat index 0, then degree 1's three coefficients at 1..3, degree 2's five at 4..8, and so
// on -- so the flat<->address mapping below is a pure index reshuffle with no reordering.

// Number of SH coefficients at exactly degree l: 2l+1.
constexpr std::size_t sh_coefficients_at_degree(std::uint32_t l) noexcept {
    return static_cast<std::size_t>(2u) * l + 1u;
}

// Total SH coefficients through degree `degree` inclusive: (degree+1)^2.
constexpr std::size_t sh_total_coefficients(std::uint32_t degree) noexcept {
    const std::size_t d = degree + 1u;
    return d * d;
}

// A coefficient's address within the SH pyramid: its degree and its 0-based index within that
// degree (0..2*degree).
struct ShAddress {
    std::uint32_t degree = 0;
    std::uint32_t coef = 0;
};

// The glTF attribute semantic for one SH coefficient, e.g.
// sh_attribute({0,0}) == "KHR_gaussian_splatting:SH_DEGREE_0_COEF_0".
std::string sh_attribute(ShAddress address);

// Parses an SH attribute semantic back to its address. Returns nullopt if the string is not a
// well-formed `KHR_gaussian_splatting:SH_DEGREE_l_COEF_n` semantic, or if n is out of range for l
// (n must be <= 2l), which a conforming asset never violates but an adversarial one might.
std::optional<ShAddress> parse_sh_attribute(std::string_view semantic);

// Maps a canonical flat SH coefficient index (coefficient-major over the pyramid) to its address,
// and back. flat = degree^2 + coef; degree = floor(sqrt(flat)). Computed with an exact integer
// method (degrees are tiny), never floating-point sqrt, so there is no rounding boundary bug.
ShAddress sh_flat_to_address(std::size_t flat_coef) noexcept;
std::size_t sh_address_to_flat(ShAddress address) noexcept;

// The non-SH attribute semantics.
inline constexpr const char* kAttrPosition = "POSITION";
inline constexpr const char* kAttrRotation = "KHR_gaussian_splatting:ROTATION";
inline constexpr const char* kAttrScale = "KHR_gaussian_splatting:SCALE";
inline constexpr const char* kAttrOpacity = "KHR_gaussian_splatting:OPACITY";

// ---- Covariance construction ----------------------------------------------------------------
//
// The spec builds the local covariance as Sigma = C C^T with C = R(q) diag(s), and then applies
// the node's upper-left 3x3 M as Sigma' = M Sigma M^T = (M C)(M C)^T. This returns C row-major, so
// that a writer/reader uses the spec's own construction verbatim. By identity C C^T equals
// math::covariance_from_rotation_scale(q, s); a test pins that equality so the two never drift.

// C = R(q) diag(s), row-major (C[r*3+j] = R[r][j] * s[j]). `rotation` is a unit quaternion in
// x,y,z,w order; `scale` is per-axis linear scale.
math::Mat3 c_matrix(const math::Quat& rotation, const math::Vec3& scale);

}  // namespace melkor::format::khr

#endif  // MELKOR_FORMAT_GLTF_KHR_HPP
