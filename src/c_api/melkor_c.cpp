// Implement the stable C ABI from melkor/c/melkor.h.
// No C++ exception can cross this file boundary.

#include "melkor/c/melkor.h"

#include "melkor/cloud_inspector.hpp"
#include "melkor/error.hpp"
#include "melkor/limits.hpp"
#include "melkor/ply_writer.hpp"
#include "melkor/version.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

static_assert(MELKOR_OK == static_cast<std::uint32_t>(melkor::ErrorCode::ok));
static_assert(MELKOR_INVALID_ARGUMENT ==
              static_cast<std::uint32_t>(melkor::ErrorCode::invalid_argument));
static_assert(MELKOR_INVALID_DATA == static_cast<std::uint32_t>(melkor::ErrorCode::invalid_data));
static_assert(MELKOR_UNSUPPORTED_FEATURE ==
              static_cast<std::uint32_t>(melkor::ErrorCode::unsupported_feature));
static_assert(MELKOR_IO_ERROR == static_cast<std::uint32_t>(melkor::ErrorCode::io_error));
static_assert(MELKOR_RESOURCE_LIMIT ==
              static_cast<std::uint32_t>(melkor::ErrorCode::resource_limit));
static_assert(MELKOR_BACKEND_UNAVAILABLE ==
              static_cast<std::uint32_t>(melkor::ErrorCode::backend_unavailable));
static_assert(MELKOR_CANCELLED == static_cast<std::uint32_t>(melkor::ErrorCode::cancelled));
static_assert(MELKOR_INTERNAL_ERROR ==
              static_cast<std::uint32_t>(melkor::ErrorCode::internal_error));

melkor_status to_c_status(melkor::ErrorCode code) noexcept {
    switch (code) {
    case melkor::ErrorCode::ok:
        return MELKOR_OK;
    case melkor::ErrorCode::invalid_argument:
        return MELKOR_INVALID_ARGUMENT;
    case melkor::ErrorCode::invalid_data:
        return MELKOR_INVALID_DATA;
    case melkor::ErrorCode::unsupported_feature:
        return MELKOR_UNSUPPORTED_FEATURE;
    case melkor::ErrorCode::io_error:
        return MELKOR_IO_ERROR;
    case melkor::ErrorCode::resource_limit:
        return MELKOR_RESOURCE_LIMIT;
    case melkor::ErrorCode::backend_unavailable:
        return MELKOR_BACKEND_UNAVAILABLE;
    case melkor::ErrorCode::cancelled:
        return MELKOR_CANCELLED;
    case melkor::ErrorCode::internal_error:
        return MELKOR_INTERNAL_ERROR;
    }
    return MELKOR_INTERNAL_ERROR;
}

constexpr std::size_t kPlyOptionsRequiredSize =
    offsetof(melkor_ply_inspect_options, limits_profile) +
    sizeof(melkor_ply_inspect_options::limits_profile);
constexpr std::size_t kPlyInfoRequiredSize =
    offsetof(melkor_ply_info, sh_degree) + sizeof(melkor_ply_info::sh_degree);
constexpr std::size_t kMaximumApiPathBytes = 32768;

void copy_complete_field(void* target, std::size_t target_size, std::size_t offset,
                         const void* value, std::size_t value_size) noexcept {
    if (offset <= target_size && value_size <= target_size - offset) {
        std::memcpy(static_cast<unsigned char*>(target) + offset, value, value_size);
    }
}

bool contains_complete_field(std::size_t structure_size, std::size_t offset,
                             std::size_t field_size) noexcept {
    return offset <= structure_size && field_size <= structure_size - offset;
}

bool is_utf8_continuation(unsigned char value) noexcept {
    return value >= 0x80 && value <= 0xbf;
}

bool is_valid_utf8(const char* value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value);
    for (std::size_t index = 0;;) {
        if (index >= kMaximumApiPathBytes)
            return false;
        const unsigned char lead = bytes[index];
        if (lead == 0)
            return true;
        if (lead <= 0x7f) {
            ++index;
            continue;
        }
        if (lead >= 0xc2 && lead <= 0xdf) {
            if (index + 1 >= kMaximumApiPathBytes)
                return false;
            if (!is_utf8_continuation(bytes[index + 1]))
                return false;
            index += 2;
            continue;
        }
        if (lead >= 0xe0 && lead <= 0xef) {
            if (index + 2 >= kMaximumApiPathBytes)
                return false;
            const unsigned char second = bytes[index + 1];
            const unsigned char third = second == 0 ? 0 : bytes[index + 2];
            if (!is_utf8_continuation(second) || !is_utf8_continuation(third) ||
                (lead == 0xe0 && second < 0xa0) || (lead == 0xed && second > 0x9f)) {
                return false;
            }
            index += 3;
            continue;
        }
        if (lead >= 0xf0 && lead <= 0xf4) {
            if (index + 3 >= kMaximumApiPathBytes)
                return false;
            const unsigned char second = bytes[index + 1];
            const unsigned char third = second == 0 ? 0 : bytes[index + 2];
            const unsigned char fourth = third == 0 ? 0 : bytes[index + 3];
            if (!is_utf8_continuation(second) || !is_utf8_continuation(third) ||
                !is_utf8_continuation(fourth) || (lead == 0xf0 && second < 0x90) ||
                (lead == 0xf4 && second > 0x8f)) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
}

bool select_limits(melkor_limits_profile profile, melkor::Limits& limits) {
    switch (profile) {
    case MELKOR_LIMITS_PROFILE_WEB:
        limits = melkor::Limits::for_profile(melkor::LimitsProfile::web);
        return true;
    case MELKOR_LIMITS_PROFILE_DESKTOP:
        limits = melkor::Limits::for_profile(melkor::LimitsProfile::desktop);
        return true;
    case MELKOR_LIMITS_PROFILE_SERVER:
        limits = melkor::Limits::for_profile(melkor::LimitsProfile::server);
        return true;
    default:
        return false;
    }
}

bool select_ply_profile(melkor_ply_profile profile, melkor::PlyReadConfig& config) {
    switch (profile) {
    case MELKOR_PLY_PROFILE_AUTO:
        return true;
    case MELKOR_PLY_PROFILE_CANONICAL:
        config.profile = melkor::FormatProfileId::ply_melkor_canonical_v1;
        return true;
    case MELKOR_PLY_PROFILE_GRAPHDECO_3DGS:
        config.profile = melkor::FormatProfileId::ply_graphdeco_3dgs_v1;
        return true;
    case MELKOR_PLY_PROFILE_DA3_GAUSSIAN:
        config.profile = melkor::FormatProfileId::ply_da3_gaussian_v1;
        return true;
    default:
        return false;
    }
}

bool select_source_frame(melkor_coordinate_frame frame, melkor::PlyReadConfig& config) {
    switch (frame) {
    case MELKOR_COORDINATE_FRAME_AUTO:
        return true;
    case MELKOR_COORDINATE_FRAME_GLTF_LUF:
        config.source_frame_id = "gltf-luf";
        return true;
    case MELKOR_COORDINATE_FRAME_PLY_RDF:
        config.source_frame_id = "ply-rdf";
        return true;
    case MELKOR_COORDINATE_FRAME_SPZ_RUB:
        config.source_frame_id = "spz-rub";
        return true;
    default:
        return false;
    }
}

bool select_color_space(melkor_color_space color_space, melkor::PlyReadConfig& config) {
    switch (color_space) {
    case MELKOR_COLOR_SPACE_AUTO:
        return true;
    case MELKOR_COLOR_SPACE_SRGB_REC709_DISPLAY:
        config.source_color_space = melkor::ColorSpace::srgb_rec709_display;
        return true;
    case MELKOR_COLOR_SPACE_LIN_REC709_DISPLAY:
        config.source_color_space = melkor::ColorSpace::lin_rec709_display;
        return true;
    default:
        return false;
    }
}

melkor_ply_profile c_ply_profile(melkor::FormatProfileId profile) noexcept {
    switch (profile) {
    case melkor::FormatProfileId::ply_melkor_canonical_v1:
        return MELKOR_PLY_PROFILE_CANONICAL;
    case melkor::FormatProfileId::ply_graphdeco_3dgs_v1:
        return MELKOR_PLY_PROFILE_GRAPHDECO_3DGS;
    case melkor::FormatProfileId::ply_da3_gaussian_v1:
        return MELKOR_PLY_PROFILE_DA3_GAUSSIAN;
    default:
        return MELKOR_PLY_PROFILE_AUTO;
    }
}

melkor_coordinate_frame c_source_frame(const std::optional<std::string>& frame) noexcept {
    if (frame == "gltf-luf")
        return MELKOR_COORDINATE_FRAME_GLTF_LUF;
    if (frame == "ply-rdf")
        return MELKOR_COORDINATE_FRAME_PLY_RDF;
    if (frame == "spz-rub")
        return MELKOR_COORDINATE_FRAME_SPZ_RUB;
    return MELKOR_COORDINATE_FRAME_AUTO;
}

melkor_color_space c_color_space(const std::optional<melkor::ColorSpace>& color_space) noexcept {
    if (color_space == melkor::ColorSpace::srgb_rec709_display)
        return MELKOR_COLOR_SPACE_SRGB_REC709_DISPLAY;
    if (color_space == melkor::ColorSpace::lin_rec709_display)
        return MELKOR_COLOR_SPACE_LIN_REC709_DISPLAY;
    return MELKOR_COLOR_SPACE_AUTO;
}

melkor_optional_bool c_optional_bool(const std::optional<bool>& value) noexcept {
    if (!value.has_value())
        return MELKOR_OPTIONAL_BOOL_UNKNOWN;
    return *value ? MELKOR_OPTIONAL_BOOL_TRUE : MELKOR_OPTIONAL_BOOL_FALSE;
}

melkor_status inspect_ply_file(const char* path, const melkor_ply_inspect_options* options,
                               melkor_ply_info* info) {
    if (info == nullptr || info->struct_size < kPlyInfoRequiredSize) {
        return MELKOR_INVALID_ARGUMENT;
    }
    if (path == nullptr || path[0] == '\0' || !is_valid_utf8(path)) {
        return MELKOR_INVALID_ARGUMENT;
    }

    melkor_limits_profile profile = MELKOR_LIMITS_PROFILE_DESKTOP;
    melkor::PlyReadConfig config;
    if (options != nullptr) {
        if (options->struct_size < kPlyOptionsRequiredSize) {
            return MELKOR_INVALID_ARGUMENT;
        }
        profile = options->limits_profile;
        const melkor_ply_profile ply_profile =
            contains_complete_field(options->struct_size,
                                    offsetof(melkor_ply_inspect_options, profile),
                                    sizeof(options->profile))
                ? options->profile
                : MELKOR_PLY_PROFILE_AUTO;
        const melkor_coordinate_frame source_frame =
            contains_complete_field(options->struct_size,
                                    offsetof(melkor_ply_inspect_options, source_frame),
                                    sizeof(options->source_frame))
                ? options->source_frame
                : MELKOR_COORDINATE_FRAME_AUTO;
        const melkor_color_space source_color_space =
            contains_complete_field(options->struct_size,
                                    offsetof(melkor_ply_inspect_options, source_color_space),
                                    sizeof(options->source_color_space))
                ? options->source_color_space
                : MELKOR_COLOR_SPACE_AUTO;
        const double source_unit_to_meter =
            contains_complete_field(options->struct_size,
                                    offsetof(melkor_ply_inspect_options, source_unit_to_meter),
                                    sizeof(options->source_unit_to_meter))
                ? options->source_unit_to_meter
                : 0.0;
        if (!select_ply_profile(ply_profile, config) ||
            !select_source_frame(source_frame, config) ||
            !select_color_space(source_color_space, config) ||
            !std::isfinite(source_unit_to_meter) || source_unit_to_meter < 0.0) {
            return MELKOR_INVALID_ARGUMENT;
        }
        if (source_unit_to_meter > 0.0)
            config.source_unit_to_meter = source_unit_to_meter;
    }

    melkor::Limits limits;
    if (!select_limits(profile, limits)) {
        return MELKOR_INVALID_ARGUMENT;
    }
    if (!limits.validate().has_value()) {
        return MELKOR_INTERNAL_ERROR;
    }
    config.limits = limits;

    melkor::Budget budget(limits);
    melkor::OperationContext context = melkor::make_default_context(budget);
    const std::filesystem::path file_path = std::filesystem::u8path(path);
    melkor::PlyReader reader;
    auto result = reader.readFromFile(file_path, config, context);
    if (!result.success) {
        return to_c_status(result.error_code());
    }
    if (!result.data.has_value()) {
        return MELKOR_INTERNAL_ERROR;
    }
    if (result.data->size() > std::numeric_limits<std::uint64_t>::max()) {
        return MELKOR_RESOURCE_LIMIT;
    }
    auto inspection = melkor::inspectCloud(*result.data, context);
    if (!inspection.has_value()) {
        return to_c_status(inspection.error_code());
    }
    if (inspection.value().error_count != 0) {
        return MELKOR_INVALID_DATA;
    }

    melkor_ply_info local{};
    local.struct_size = sizeof(local);
    local.splat_count = static_cast<std::uint64_t>(result.data->size());
    local.sh_degree = result.data->sh().degree();
    local.profile = c_ply_profile(result.metadata.profile);
    local.source_frame = c_source_frame(result.metadata.source_frame_id);
    local.source_color_space = c_color_space(result.metadata.color_space);
    local.antialiased = c_optional_bool(result.metadata.antialiased);
    local.source_unit_to_meter = result.metadata.source_unit_to_meter.value_or(0.0);
    local.loss_count = static_cast<std::uint64_t>(result.losses.items().size());
    local.blocking_loss_count = static_cast<std::uint64_t>(
        std::count_if(result.losses.items().begin(), result.losses.items().end(),
                      [](const melkor::LossItem& item) {
                          return item.severity == melkor::LossSeverity::severe ||
                                 item.severity == melkor::LossSeverity::fatal;
                      }));

    const std::size_t caller_size = info->struct_size;
    copy_complete_field(info, caller_size, offsetof(melkor_ply_info, splat_count),
                        &local.splat_count, sizeof(local.splat_count));
    copy_complete_field(info, caller_size, offsetof(melkor_ply_info, sh_degree), &local.sh_degree,
                        sizeof(local.sh_degree));
    copy_complete_field(info, caller_size, offsetof(melkor_ply_info, profile), &local.profile,
                        sizeof(local.profile));
    copy_complete_field(info, caller_size, offsetof(melkor_ply_info, source_frame),
                        &local.source_frame, sizeof(local.source_frame));
    copy_complete_field(info, caller_size, offsetof(melkor_ply_info, source_color_space),
                        &local.source_color_space, sizeof(local.source_color_space));
    copy_complete_field(info, caller_size, offsetof(melkor_ply_info, antialiased),
                        &local.antialiased, sizeof(local.antialiased));
    copy_complete_field(info, caller_size, offsetof(melkor_ply_info, reserved), &local.reserved,
                        sizeof(local.reserved));
    copy_complete_field(info, caller_size, offsetof(melkor_ply_info, source_unit_to_meter),
                        &local.source_unit_to_meter, sizeof(local.source_unit_to_meter));
    copy_complete_field(info, caller_size, offsetof(melkor_ply_info, loss_count), &local.loss_count,
                        sizeof(local.loss_count));
    copy_complete_field(info, caller_size, offsetof(melkor_ply_info, blocking_loss_count),
                        &local.blocking_loss_count, sizeof(local.blocking_loss_count));
    return MELKOR_OK;
}

}  // namespace

extern "C" {

const char* melkor_status_string(melkor_status status) noexcept {
    switch (status) {
    case MELKOR_OK:
        return "ok";
    case MELKOR_INVALID_ARGUMENT:
        return "invalid_argument";
    case MELKOR_INVALID_DATA:
        return "invalid_data";
    case MELKOR_UNSUPPORTED_FEATURE:
        return "unsupported_feature";
    case MELKOR_IO_ERROR:
        return "io_error";
    case MELKOR_RESOURCE_LIMIT:
        return "resource_limit";
    case MELKOR_BACKEND_UNAVAILABLE:
        return "backend_unavailable";
    case MELKOR_CANCELLED:
        return "cancelled";
    case MELKOR_INTERNAL_ERROR:
        return "internal_error";
    }
    return "unknown";
}

melkor_status melkor_get_version(melkor_version_info* info) noexcept {
    if (info == nullptr || info->struct_size < sizeof(info->struct_size)) {
        return MELKOR_INVALID_ARGUMENT;
    }

    melkor_version_info local{};
    local.struct_size = sizeof(local);
    local.version_string = MELKOR_VERSION_STRING;
    local.version_major = MELKOR_VERSION_MAJOR;
    local.version_minor = MELKOR_VERSION_MINOR;
    local.version_patch = MELKOR_VERSION_PATCH;
    local.abi_version = MELKOR_ABI_VERSION;
    local.inspect_schema_version = MELKOR_INSPECT_SCHEMA_VERSION;
    local.loss_schema_version = MELKOR_LOSS_SCHEMA_VERSION;
    local.build_commit = MELKOR_BUILD_COMMIT;

    const std::size_t caller_size = info->struct_size;
#define MELKOR_COPY_VERSION_FIELD(field)                                                           \
    copy_complete_field(info, caller_size, offsetof(melkor_version_info, field),                   \
                        static_cast<const void*>(&local.field), sizeof(local.field))
    MELKOR_COPY_VERSION_FIELD(version_string);
    MELKOR_COPY_VERSION_FIELD(version_major);
    MELKOR_COPY_VERSION_FIELD(version_minor);
    MELKOR_COPY_VERSION_FIELD(version_patch);
    MELKOR_COPY_VERSION_FIELD(abi_version);
    MELKOR_COPY_VERSION_FIELD(inspect_schema_version);
    MELKOR_COPY_VERSION_FIELD(loss_schema_version);
    MELKOR_COPY_VERSION_FIELD(build_commit);
#undef MELKOR_COPY_VERSION_FIELD
    return MELKOR_OK;
}

melkor_status melkor_inspect_ply_file(const char* path, const melkor_ply_inspect_options* options,
                                      melkor_ply_info* info) noexcept {
    try {
        return inspect_ply_file(path, options, info);
    } catch (const std::bad_alloc&) {
        return MELKOR_RESOURCE_LIMIT;
    } catch (const std::length_error&) {
        return MELKOR_RESOURCE_LIMIT;
    } catch (const std::filesystem::filesystem_error&) {
        return MELKOR_IO_ERROR;
    } catch (...) {
        return MELKOR_INTERNAL_ERROR;
    }
}

}  // extern "C"
