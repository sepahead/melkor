#include "melkor/format/profile.hpp"

#include <array>

namespace melkor {
namespace {

constexpr std::uint32_t container_bit(FormatId id) noexcept {
    return std::uint32_t{1} << static_cast<std::uint32_t>(id);
}

constexpr FormatProfile kUnknown{};

constexpr std::array<FormatProfile, 5> kProfiles{{
    {
        FormatProfileId::ply_melkor_canonical_v1,
        "ply:melkor-canonical-v1",
        container_bit(FormatId::ply),
        container_bit(FormatId::ply),
        4,
    },
    {
        FormatProfileId::ply_graphdeco_3dgs_v1,
        "ply:graphdeco-3dgs-v1",
        container_bit(FormatId::ply),
        container_bit(FormatId::ply),
        3,
    },
    {
        FormatProfileId::ply_da3_gaussian_v1,
        "ply:da3-gaussian-v1",
        container_bit(FormatId::ply),
        container_bit(FormatId::ply),
        4,
    },
    {
        FormatProfileId::spz_v1_v3,
        "spz:spz-v1-v3",
        container_bit(FormatId::spz),
        container_bit(FormatId::spz),
        3,
    },
    {
        FormatProfileId::gltf_khr_gaussian_splatting_rc_63770cc,
        "khr-gaussian-splatting-rc-63770cc",
        container_bit(FormatId::gltf) | container_bit(FormatId::glb),
        container_bit(FormatId::glb),
        3,
    },
}};

}  // namespace

const FormatProfile& format_profile(FormatProfileId id) noexcept {
    for (const FormatProfile& profile : kProfiles) {
        if (profile.id == id)
            return profile;
    }
    return kUnknown;
}

std::optional<FormatProfileId> format_profile_from_string(std::string_view value) noexcept {
    for (const FormatProfile& profile : kProfiles) {
        if (profile.profile_id == value)
            return profile.id;
    }
    return std::nullopt;
}

std::optional<FormatProfileId> default_write_profile(FormatId container) noexcept {
    switch (container) {
    case FormatId::ply:
        return FormatProfileId::ply_melkor_canonical_v1;
    case FormatId::spz:
        return FormatProfileId::spz_v1_v3;
    case FormatId::glb:
        return FormatProfileId::gltf_khr_gaussian_splatting_rc_63770cc;
    case FormatId::unknown:
    case FormatId::gltf:
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace melkor
