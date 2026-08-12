// Runtime format-profile registry.
//
// A container name does not define Gaussian semantics. This registry identifies each supported
// profile and states the capabilities that conversion planning can use.

#ifndef MELKOR_FORMAT_PROFILE_HPP
#define MELKOR_FORMAT_PROFILE_HPP

#include "melkor/format/format_id.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace melkor {

enum class FormatProfileId : std::uint8_t {
    unknown = 0,
    ply_melkor_canonical_v1,
    ply_graphdeco_3dgs_v1,
    ply_da3_gaussian_v1,
    spz_v1_v3,
    gltf_khr_gaussian_splatting_rc_63770cc,
};

struct FormatProfile {
    FormatProfileId id = FormatProfileId::unknown;
    std::string_view profile_id;
    std::uint32_t read_container_mask = 0;
    std::uint32_t write_container_mask = 0;
    std::uint8_t max_sh_degree = 0;

    constexpr bool supports_read_container(FormatId container) const noexcept {
        const auto bit = static_cast<std::uint32_t>(container);
        return bit < 32u && (read_container_mask & (std::uint32_t{1} << bit)) != 0;
    }

    constexpr bool supports_write_container(FormatId container) const noexcept {
        const auto bit = static_cast<std::uint32_t>(container);
        return bit < 32u && (write_container_mask & (std::uint32_t{1} << bit)) != 0;
    }
};

// Return a stable profile descriptor. The unknown ID returns the unknown descriptor.
const FormatProfile& format_profile(FormatProfileId id) noexcept;

// Parse an exact profile ID. Short aliases are intentionally not accepted in this core API.
std::optional<FormatProfileId> format_profile_from_string(std::string_view value) noexcept;

// Return the default write profile for a container. glTF JSON has no default writer.
std::optional<FormatProfileId> default_write_profile(FormatId container) noexcept;

}  // namespace melkor

#endif  // MELKOR_FORMAT_PROFILE_HPP
