#include "melkor/limits.hpp"

#include <algorithm>
#include <thread>

namespace melkor {
namespace {

constexpr std::uint64_t KiB = 1024ULL;
constexpr std::uint64_t MiB = 1024ULL * KiB;
constexpr std::uint64_t GiB = 1024ULL * MiB;

std::uint32_t default_thread_count() {
    const unsigned hardware = std::thread::hardware_concurrency();
    // hardware_concurrency() is allowed to return 0 when it cannot tell. Treating that as
    // "zero threads" would deadlock, so fall back to something conservative and workable.
    const std::uint32_t detected = hardware == 0 ? 4u : static_cast<std::uint32_t>(hardware);
    // Cap the default even on very large machines: spawning 128 workers to parse one file
    // mostly buys contention, and an unbounded default is itself a resource hazard.
    return std::min<std::uint32_t>(detected, 32u);
}

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
    if (name == "custom") {
        return Result<LimitsProfile>::success(LimitsProfile::custom);
    }

    Diagnostic diagnostic("MK0201_UNKNOWN_LIMITS_PROFILE", Severity::error,
                          "unknown limits profile");
    diagnostic.with_context("value", name);
    diagnostic.with_context("supported", std::string("web, desktop, server, custom"));
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
            // Bounded, not 0: Budget treats a 0 limit as *unlimited*, so a 0 here would give the
            // tightest profile an UNBOUNDED temp-disk budget -- the opposite of the intent. Native
            // use of the web profile (`--limits-profile web`) still writes output through the atomic
            // temp file, so it is bounded to the same order as the input/decoded caps.
            limits.max_temp_bytes = 2 * GiB;
            limits.max_decompression_ratio = 100;
            limits.max_splats = 8'000'000;
            limits.max_mesh_vertices = 8'000'000;
            limits.max_mesh_triangles = 16'000'000;
            limits.max_gltf_nodes = 100'000;
            limits.max_accessors = 100'000;
            limits.max_external_resources = 64;
            limits.max_ply_header_bytes = 1 * MiB;
            limits.max_metadata_string_bytes = 256 * KiB;
            limits.max_metadata_total_bytes = 4 * MiB;
            limits.max_scene_depth = 64;
            limits.max_image_dimension = 8192;
            limits.max_image_pixels = 67'108'864;  // 8192^2
            limits.max_threads = 4;
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
            limits.max_mesh_vertices = 25'000'000;
            limits.max_mesh_triangles = 50'000'000;
            limits.max_gltf_nodes = 1'000'000;
            limits.max_accessors = 1'000'000;
            limits.max_external_resources = 512;
            limits.max_ply_header_bytes = 4 * MiB;
            limits.max_metadata_string_bytes = 1 * MiB;
            limits.max_metadata_total_bytes = 16 * MiB;
            limits.max_scene_depth = 64;
            limits.max_image_dimension = 16384;
            limits.max_image_pixels = 268'435'456;
            limits.max_threads = default_thread_count();
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
            limits.max_mesh_vertices = 150'000'000;
            limits.max_mesh_triangles = 300'000'000;
            limits.max_gltf_nodes = 5'000'000;
            limits.max_accessors = 5'000'000;
            limits.max_external_resources = 4096;
            limits.max_ply_header_bytes = 16 * MiB;
            limits.max_metadata_string_bytes = 4 * MiB;
            limits.max_metadata_total_bytes = 64 * MiB;
            limits.max_scene_depth = 256;
            limits.max_image_dimension = 32768;
            limits.max_image_pixels = 1'073'741'824;
            limits.max_threads = default_thread_count();
            limits.deadline_ms = 0;
            break;

        case LimitsProfile::custom:
            // Everything zero. A custom profile that is never populated must fail validate()
            // rather than quietly mean "no limits" -- an all-zeros struct is the most likely
            // way someone would accidentally disable resource accounting entirely.
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
                "note",
                std::string("A zero limit is not 'unlimited'. Melkor has no way to disable "
                            "resource accounting; set an explicit value."));
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

    // Every limit that is enforced through a BudgetKind must be required positive here.
    //
    // Budget::consume treats a limit of 0 as "unlimited", so a limit that is both budget-backed
    // and left at 0 is silently disabled -- exactly the accidental "unbounded" the header
    // promises to reject. The original list covered only eight fields and missed the rest, so a
    // custom profile that populated the obvious ones and forgot, say, max_mesh_triangles passed
    // validation and then accepted an unbounded triangle count feeding an allocation.
    //
    // Every budget-backed limit -- including max_temp_bytes -- must be positive: a 0 there is read
    // by Budget as "unlimited", so leaving it 0 silently disables the bound (this is exactly how
    // the web profile once granted an unbounded temp-disk budget).
    require_positive(max_temp_bytes, "max_temp_bytes");
    require_positive(max_input_bytes, "max_input_bytes");
    require_positive(max_resource_bytes, "max_resource_bytes");
    require_positive(max_decoded_bytes, "max_decoded_bytes");
    require_positive(max_memory_bytes, "max_memory_bytes");
    require_positive(max_splats, "max_splats");
    require_positive(max_mesh_vertices, "max_mesh_vertices");
    require_positive(max_mesh_triangles, "max_mesh_triangles");
    require_positive(max_gltf_nodes, "max_gltf_nodes");
    require_positive(max_accessors, "max_accessors");
    require_positive(max_external_resources, "max_external_resources");
    require_positive(max_ply_header_bytes, "max_ply_header_bytes");
    require_positive(max_metadata_total_bytes, "max_metadata_total_bytes");
    require_positive(max_image_pixels, "max_image_pixels");
    require_positive(max_scene_depth, "max_scene_depth");
    require_positive(max_threads, "max_threads");
    require_positive(max_decompression_ratio, "max_decompression_ratio");

    require_ceiling(max_splats, hard_ceiling::kMaxSplats, "max_splats");
    require_ceiling(max_mesh_vertices, hard_ceiling::kMaxMeshVertices, "max_mesh_vertices");
    require_ceiling(max_mesh_triangles, hard_ceiling::kMaxMeshTriangles, "max_mesh_triangles");
    require_ceiling(max_image_dimension, hard_ceiling::kMaxImageDimension, "max_image_dimension");
    require_ceiling(max_scene_depth, hard_ceiling::kMaxSceneDepth, "max_scene_depth");
    require_ceiling(max_threads, hard_ceiling::kMaxThreads, "max_threads");

    // A decoded-bytes cap below the input cap would reject any file that does not compress,
    // which is a configuration mistake rather than a policy.
    if (max_decoded_bytes != 0 && max_input_bytes != 0 && max_decoded_bytes < max_input_bytes) {
        Diagnostic diagnostic("MK0204_LIMITS_INCONSISTENT", Severity::error,
                              "max_decoded_bytes is smaller than max_input_bytes");
        diagnostic.with_context("max_decoded_bytes", max_decoded_bytes);
        diagnostic.with_context("max_input_bytes", max_input_bytes);
        diagnostic.with_context(
            "note",
            std::string("An uncompressed input decodes to at least its own size, so this "
                        "configuration would reject every such file."));
        diagnostics.push_back(std::move(diagnostic));
    }

    if (!diagnostics.empty()) {
        return Result<void>::failure(ErrorCode::invalid_argument, std::move(diagnostics));
    }
    return Result<void>::success();
}

}  // namespace melkor
