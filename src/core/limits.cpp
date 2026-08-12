#include "melkor/limits.hpp"

namespace melkor {
namespace {

constexpr std::uint64_t KiB = 1024ULL;
constexpr std::uint64_t MiB = 1024ULL * KiB;
constexpr std::uint64_t GiB = 1024ULL * MiB;

}  // namespace

const char* to_string(LimitsProfile profile) noexcept {
    switch (profile) {
    case LimitsProfile::web:
        return "web";
    case LimitsProfile::desktop:
        return "desktop";
    case LimitsProfile::server:
        return "server";
    case LimitsProfile::custom:
        return "custom";
    }
    return "unknown";
}

Result<LimitsProfile> limits_profile_from_string(const std::string& name) {
    if (name == "web") {
        return Result<LimitsProfile>::success(LimitsProfile::web);
    }
    if (name == "desktop") {
        return Result<LimitsProfile>::success(LimitsProfile::desktop);
    }
    if (name == "server") {
        return Result<LimitsProfile>::success(LimitsProfile::server);
    }
    Diagnostic diagnostic("MK0201_UNKNOWN_LIMITS_PROFILE", Severity::error,
                          "unknown limits profile");
    diagnostic.with_context("value", name);
    diagnostic.with_context("supported", std::string("web, desktop, server"));
    return Result<LimitsProfile>::failure(ErrorCode::invalid_argument, std::move(diagnostic));
}

Limits Limits::for_profile(LimitsProfile profile) {
    Limits limits;

    switch (profile) {
    case LimitsProfile::web:
        // A browser tab. It cannot swap, and exceeding memory kills the page rather than
        // returning an error, so these are the tightest.
        limits.max_input_bytes = 2 * GiB;
        limits.max_resource_bytes = 512 * MiB;
        limits.max_decoded_bytes = 2 * GiB;
        limits.max_memory_bytes = 1 * GiB;
        // Native web-profile use writes output through an atomic temporary file.
        // Bound that file to the same order as the input and decoded data.
        limits.max_temp_bytes = 2 * GiB;
        limits.max_decompression_ratio = 100;
        limits.max_splats = 8'000'000;
        limits.max_gltf_nodes = 100'000;
        limits.max_accessors = 100'000;
        limits.max_external_resources = 64;
        limits.max_ply_header_bytes = 1 * MiB;
        limits.max_metadata_string_bytes = 256 * KiB;
        limits.max_metadata_total_bytes = 4 * MiB;
        limits.max_scene_depth = 64;
        limits.deadline_ms = 60'000;  // A tab that hangs for a minute is already a bug.
        break;

    case LimitsProfile::desktop:
        limits.max_input_bytes = 4 * GiB;
        limits.max_resource_bytes = 4 * GiB;
        limits.max_decoded_bytes = 8 * GiB;
        limits.max_memory_bytes = 4 * GiB;
        limits.max_temp_bytes = 16 * GiB;
        limits.max_decompression_ratio = 1000;
        limits.max_splats = 25'000'000;
        limits.max_gltf_nodes = 1'000'000;
        limits.max_accessors = 1'000'000;
        limits.max_external_resources = 512;
        limits.max_ply_header_bytes = 4 * MiB;
        limits.max_metadata_string_bytes = 1 * MiB;
        limits.max_metadata_total_bytes = 16 * MiB;
        limits.max_scene_depth = 64;
        limits.deadline_ms = 0;  // No deadline: a local CLI job may legitimately take hours.
        break;

    case LimitsProfile::server:
        // A machine dedicated to the job. Still bounded -- "server" does not mean
        // "unlimited", it means "the operator chose these numbers knowingly".
        limits.max_input_bytes = 32 * GiB;
        limits.max_resource_bytes = 64 * GiB;
        limits.max_decoded_bytes = 64 * GiB;
        limits.max_memory_bytes = 16 * GiB;
        limits.max_temp_bytes = 128 * GiB;
        limits.max_decompression_ratio = 1000;
        limits.max_splats = 150'000'000;
        limits.max_gltf_nodes = 5'000'000;
        limits.max_accessors = 5'000'000;
        limits.max_external_resources = 4096;
        limits.max_ply_header_bytes = 16 * MiB;
        limits.max_metadata_string_bytes = 4 * MiB;
        limits.max_metadata_total_bytes = 64 * MiB;
        limits.max_scene_depth = 256;
        limits.deadline_ms = 0;
        break;

    case LimitsProfile::custom:
        // Reject an unconfigured custom profile. An all-zero value must not disable resource
        // accounting.
        break;
    }

    return limits;
}

Result<void> Limits::validate() const {
    std::vector<Diagnostic> diagnostics;

    auto require_positive = [&diagnostics](std::uint64_t value, const char* name) {
        if (value == 0) {
            Diagnostic diagnostic("MK0202_LIMIT_NOT_SET", Severity::error,
                                  std::string("resource limit is zero: ") + name);
            diagnostic.with_context("limit", std::string(name));
            diagnostic.with_context(
                "note", std::string("A zero limit is not 'unlimited'. Melkor cannot disable "
                                    "resource accounting. Set an explicit value."));
            diagnostics.push_back(std::move(diagnostic));
        }
    };

    auto require_ceiling = [&diagnostics](std::uint64_t value, std::uint64_t ceiling,
                                          const char* name) {
        if (value > ceiling) {
            Diagnostic diagnostic("MK0203_LIMIT_EXCEEDS_CEILING", Severity::error,
                                  std::string("resource limit exceeds the implementation "
                                              "ceiling: ") +
                                      name);
            diagnostic.with_context("limit", std::string(name));
            diagnostic.with_context("requested", value);
            diagnostic.with_context("ceiling", ceiling);
            diagnostic.with_context(
                "note",
                std::string("Beyond this point Melkor's own arithmetic cannot represent the "
                            "result, so the limit would no longer be protecting anything."));
            diagnostics.push_back(std::move(diagnostic));
        }
    };

    // Every budget-backed limit must be positive. Budget rejects zero as defense in depth.
    // Include max_temp_bytes so each profile bounds atomic output files.
    require_positive(max_temp_bytes, "max_temp_bytes");
    require_positive(max_input_bytes, "max_input_bytes");
    require_positive(max_resource_bytes, "max_resource_bytes");
    require_positive(max_decoded_bytes, "max_decoded_bytes");
    require_positive(max_memory_bytes, "max_memory_bytes");
    require_positive(max_splats, "max_splats");
    require_positive(max_gltf_nodes, "max_gltf_nodes");
    require_positive(max_accessors, "max_accessors");
    require_positive(max_external_resources, "max_external_resources");
    require_positive(max_ply_header_bytes, "max_ply_header_bytes");
    require_positive(max_metadata_string_bytes, "max_metadata_string_bytes");
    require_positive(max_metadata_total_bytes, "max_metadata_total_bytes");
    require_positive(max_scene_depth, "max_scene_depth");
    require_positive(max_decompression_ratio, "max_decompression_ratio");

    require_ceiling(max_splats, hard_ceiling::kMaxSplats, "max_splats");
    require_ceiling(max_gltf_nodes, hard_ceiling::kMaxStructuralObjects, "max_gltf_nodes");
    require_ceiling(max_accessors, hard_ceiling::kMaxStructuralObjects, "max_accessors");
    require_ceiling(max_external_resources, hard_ceiling::kMaxStructuralObjects,
                    "max_external_resources");
    require_ceiling(max_scene_depth, hard_ceiling::kMaxSceneDepth, "max_scene_depth");

    if (max_metadata_string_bytes != 0 && max_metadata_total_bytes != 0 &&
        max_metadata_string_bytes > max_metadata_total_bytes) {
        Diagnostic diagnostic("MK0204_LIMITS_INCONSISTENT", Severity::error,
                              "the metadata string limit exceeds the metadata total limit");
        diagnostic.with_context("max_metadata_string_bytes", max_metadata_string_bytes);
        diagnostic.with_context("max_metadata_total_bytes", max_metadata_total_bytes);
        diagnostics.push_back(std::move(diagnostic));
    }

    if (!diagnostics.empty()) {
        return Result<void>::failure(ErrorCode::invalid_argument, std::move(diagnostics));
    }
    return Result<void>::success();
}

}  // namespace melkor
