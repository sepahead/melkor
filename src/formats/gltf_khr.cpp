#include "melkor/format/gltf_khr.hpp"

#include <array>
#include <cstring>

namespace melkor::format::khr {

const char* to_string(ColorSpace space) noexcept {
    switch (space) {
        case ColorSpace::srgb_rec709_display:
            return "srgb_rec709_display";
        case ColorSpace::lin_rec709_display:
            return "lin_rec709_display";
    }
    return "srgb_rec709_display";
}

std::optional<ColorSpace> color_space_from_string(std::string_view s) noexcept {
    if (s == "srgb_rec709_display") return ColorSpace::srgb_rec709_display;
    if (s == "lin_rec709_display") return ColorSpace::lin_rec709_display;
    return std::nullopt;
}

namespace {

// Exact integer square root by increment: for a flat index f, find the largest d with d*d <= f.
// The SH pyramid is tiny (degree <= 4 gives 25 coefficients), so this loop runs a handful of
// times and never touches floating point -- there is no sqrt rounding boundary to get wrong.
std::uint32_t integer_floor_sqrt(std::size_t f) noexcept {
    std::uint32_t d = 0;
    while (static_cast<std::size_t>(d + 1) * static_cast<std::size_t>(d + 1) <= f) {
        ++d;
    }
    return d;
}

// Parses a run of ASCII digits starting at `pos` in `s`, into `out`, advancing `pos`. Returns
// false on no digits, a leading zero followed by another digit, or overflow of the small range we
// accept. Rejecting "01" keeps semantics one-to-one with their canonical spelling.
bool parse_uint(std::string_view s, std::size_t& pos, std::uint32_t& out) noexcept {
    const std::size_t start = pos;
    std::uint32_t value = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        const std::uint32_t digit = static_cast<std::uint32_t>(s[pos] - '0');
        if (value > (0xFFFFFFFFu - digit) / 10u) {
            return false;  // would overflow
        }
        value = value * 10u + digit;
        ++pos;
    }
    if (pos == start) {
        return false;  // no digits
    }
    if (pos - start > 1 && s[start] == '0') {
        return false;  // leading zero, e.g. "01"
    }
    out = value;
    return true;
}

}  // namespace

std::string sh_attribute(ShAddress address) {
    std::string out = "KHR_gaussian_splatting:SH_DEGREE_";
    out += std::to_string(address.degree);
    out += "_COEF_";
    out += std::to_string(address.coef);
    return out;
}

std::optional<ShAddress> parse_sh_attribute(std::string_view semantic) {
    constexpr std::string_view kPrefix = "KHR_gaussian_splatting:SH_DEGREE_";
    constexpr std::string_view kMid = "_COEF_";
    if (semantic.size() < kPrefix.size() || semantic.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    std::size_t pos = kPrefix.size();
    std::uint32_t degree = 0;
    if (!parse_uint(semantic, pos, degree)) {
        return std::nullopt;
    }
    if (semantic.size() < pos + kMid.size() || semantic.substr(pos, kMid.size()) != kMid) {
        return std::nullopt;
    }
    pos += kMid.size();
    std::uint32_t coef = 0;
    if (!parse_uint(semantic, pos, coef)) {
        return std::nullopt;
    }
    if (pos != semantic.size()) {
        return std::nullopt;  // trailing junk
    }
    // The coefficient index must be in range for the degree: n in [0, 2*degree]. A conforming
    // asset never breaks this; an adversarial one might, and mapping it into the pyramid would
    // otherwise read the wrong slot.
    if (coef >= sh_coefficients_at_degree(degree)) {
        return std::nullopt;
    }
    return ShAddress{degree, coef};
}

ShAddress sh_flat_to_address(std::size_t flat_coef) noexcept {
    const std::uint32_t degree = integer_floor_sqrt(flat_coef);
    const std::uint32_t coef =
        static_cast<std::uint32_t>(flat_coef - static_cast<std::size_t>(degree) * degree);
    return ShAddress{degree, coef};
}

std::size_t sh_address_to_flat(ShAddress address) noexcept {
    return static_cast<std::size_t>(address.degree) * address.degree + address.coef;
}

math::Mat3 c_matrix(const math::Quat& rotation, const math::Vec3& scale) {
    // R is the rotation matrix from the (x,y,z,w) quaternion; C scales each column j of R by
    // scale[j]. This reproduces the spec's explicit C matrix exactly (verified element-wise in
    // the tests), and C C^T = R diag(s^2) R^T is the local covariance.
    const math::Mat3 r = math::to_matrix(rotation);
    math::Mat3 c{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            c[static_cast<std::size_t>(row) * 3 + col] =
                r[static_cast<std::size_t>(row) * 3 + col] * scale[static_cast<std::size_t>(col)];
        }
    }
    return c;
}

}  // namespace melkor::format::khr
