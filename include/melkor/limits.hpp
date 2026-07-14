// Resource limits.
//
// Melkor treats every file as untrusted, and "untrusted" includes *well-formed but enormous*.
// A 40-gigabyte PLY that declares two billion splats is not malformed; it is simply larger
// than the machine can survive. Refusing to bound that case -- and the old security policy
// explicitly declared it out of scope -- means a single file can exhaust memory, fill a disk,
// or wedge a machine, which is release blocker P0-12.
//
// Every limit here is enforced through a shared `Budget` (see budget.hpp), so that a new
// parser cannot accidentally opt out of resource accounting by forgetting to check something.
//
// The numbers below are *safety defaults*, not scientific truths. They were chosen to let
// realistic assets through while stopping runaway ones, and they must be revisited against
// benchmark data before the final release. Changing one changes which files Melkor accepts,
// so every change needs a changelog entry.
//
// There is deliberately **no "disable all limits" switch.** A profile may raise a limit, but
// checked arithmetic, structural format limits, path containment, and output integrity stay
// on regardless. A user who wants to process a huge asset should say how huge, not turn off
// the brakes.

#ifndef MELKOR_LIMITS_HPP
#define MELKOR_LIMITS_HPP

#include "melkor/error.hpp"

#include <cstdint>
#include <string>

namespace melkor {

// Named limit profiles.
//
// These exist because the right limits genuinely differ by context, and pretending otherwise
// forces either an unusable default or an unsafe one. A browser tab must not allocate 16 GiB;
// a batch server processing a scanned building legitimately might.
enum class LimitsProfile : std::uint8_t {
    web,      // A browser tab. Tight: the page must stay responsive and cannot swap.
    desktop,  // The default for the CLI on a workstation.
    server,   // Batch processing on a machine dedicated to the job.
    custom,   // Explicitly constructed by the caller.
};

const char* to_string(LimitsProfile profile) noexcept;
Result<LimitsProfile> limits_profile_from_string(const std::string& name);

struct Limits {
    // ---- Input volume ----------------------------------------------------------------
    std::uint64_t max_input_bytes = 0;      // Bytes read from the primary file.
    std::uint64_t max_resource_bytes = 0;   // glTF external buffers, images, sidecars.
    std::uint64_t max_decoded_bytes = 0;    // Cumulative bytes after decompression.
    std::uint64_t max_memory_bytes = 0;     // Budgeted working allocations.
    std::uint64_t max_temp_bytes = 0;       // Temporary files written during output.

    // ---- Decompression -----------------------------------------------------------------
    // A compression bomb is a small file that expands enormously. An absolute decoded-byte
    // cap alone is not enough: it lets a 1 KiB file expand to the full cap. The ratio guard
    // catches the shape of the attack rather than only its magnitude.
    //
    // A legitimately highly-compressible asset can exceed this and will need an override.
    // That is the correct trade: the default must fail safely, and the user who knows their
    // data is fine can say so.
    std::uint64_t max_decompression_ratio = 0;

    // ---- Counts ------------------------------------------------------------------------
    std::uint64_t max_splats = 0;
    std::uint64_t max_mesh_vertices = 0;
    std::uint64_t max_mesh_triangles = 0;
    std::uint64_t max_gltf_nodes = 0;
    std::uint64_t max_accessors = 0;       // Accessors + buffer views: structural objects.
    std::uint64_t max_external_resources = 0;  // Count of URIs/files, not their size.

    // ---- Structure ---------------------------------------------------------------------
    std::uint64_t max_ply_header_bytes = 0;
    std::uint64_t max_metadata_string_bytes = 0;   // Any single comment/name/extra.
    std::uint64_t max_metadata_total_bytes = 0;    // All of them together.
    std::uint64_t max_scene_depth = 0;             // Guards unbounded recursion.

    // ---- Images -------------------------------------------------------------------------
    // Both a per-axis and a total-pixel cap. A 1 x 4,000,000,000 image passes a naive
    // per-axis check while still decoding to an absurd buffer.
    std::uint64_t max_image_dimension = 0;
    std::uint64_t max_image_pixels = 0;

    // ---- Execution -----------------------------------------------------------------------
    std::uint32_t max_threads = 0;
    std::uint64_t deadline_ms = 0;  // 0 == no deadline. The local CLI has none by default.

    // Builds one of the named profiles.
    static Limits for_profile(LimitsProfile profile);

    // Rejects a configuration that cannot be satisfied safely regardless of what the user
    // asked for -- for example a count that cannot be represented, or a zeroed limit that
    // would mean "unbounded" by accident.
    Result<void> validate() const;
};

// Hard ceilings that no custom profile may exceed.
//
// A user may raise a limit deliberately. They may not raise it past the point where Melkor's
// own arithmetic stops being able to represent the result -- at that point the limit is not
// protecting anything and the failure mode becomes an overflow rather than a clean refusal.
namespace hard_ceiling {
constexpr std::uint64_t kMaxSplats = 4'000'000'000ULL;
constexpr std::uint64_t kMaxMeshVertices = 4'000'000'000ULL;
constexpr std::uint64_t kMaxMeshTriangles = 8'000'000'000ULL;
constexpr std::uint64_t kMaxImageDimension = 1ULL << 20;
constexpr std::uint64_t kMaxSceneDepth = 4096;
constexpr std::uint32_t kMaxThreads = 1024;
}  // namespace hard_ceiling

}  // namespace melkor

#endif  // MELKOR_LIMITS_HPP
