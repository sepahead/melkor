#include "melkor/format/gltf_khr.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace melkor::format::khr {

namespace {

// Return the exact floor of the integer square root in bounded time.
std::uint32_t integer_floor_sqrt(std::size_t f) noexcept {
    const std::size_t max_degree =
        std::min<std::size_t>(f, std::numeric_limits<std::uint32_t>::max());
    std::size_t low = 0;
    std::size_t high = max_degree;
    while (low < high) {
        const std::size_t middle = low + (high - low + 1u) / 2u;
        if (middle <= f / middle) {
            low = middle;
        } else {
            high = middle - 1u;
        }
    }
    return static_cast<std::uint32_t>(low);
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
    if (static_cast<std::size_t>(coef) >= sh_coefficients_at_degree(degree)) {
        return std::nullopt;
    }
    return ShAddress{degree, coef};
}

std::optional<ShAddress> sh_flat_to_address(std::size_t flat_coef) noexcept {
    if (flat_coef >= sh_total_coefficients(kMaxCanonicalShDegree))
        return std::nullopt;
    const std::uint32_t degree = integer_floor_sqrt(flat_coef);
    const std::uint32_t coef =
        static_cast<std::uint32_t>(flat_coef - static_cast<std::size_t>(degree) * degree);
    return ShAddress{degree, coef};
}

std::optional<std::size_t> sh_address_to_flat(ShAddress address) noexcept {
    if (address.coef >= sh_coefficients_at_degree(address.degree))
        return std::nullopt;
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
