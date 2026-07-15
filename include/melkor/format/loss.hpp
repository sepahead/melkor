// The conversion loss report.
//
// A format conversion is honest only when it says what it lost. Converting a degree-4 SPZ asset
// to the degree-3 glTF profile drops coefficients; flattening a glTF scene graph into a PLY
// point cloud loses hierarchy; quantising into SPZ introduces measurable error. None of that is
// a failure -- but a conversion that discards it silently, and returns success, is lying by
// omission.
//
// So every conversion produces a `LossReport`, including a zero-loss one, so automation never has
// to infer whether reporting was simply omitted. A `severe` or `fatal` loss aborts the
// conversion before it commits output, unless the caller has approved that exact loss code. This
// is separate from validation diagnostics: a malformed file or a resource-limit failure is an
// error, not a loss, and cannot be waved through the loss policy.
//
// The codes here are stable machine identifiers. A consumer that special-cases
// `LOSS_SH_DEGREE_TRUNCATED` can rely on it meaning the same thing across the 2.x line.

#ifndef MELKOR_FORMAT_LOSS_HPP
#define MELKOR_FORMAT_LOSS_HPP

#include "melkor/error.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace melkor {

// How consequential a loss is. The policy acts on this.
enum class LossSeverity : std::uint8_t {
    // A representational change with no expected rendered difference -- a quaternion renormalised
    // within tolerance, say. Recorded, never blocks.
    info = 0,
    // A measurable but usually acceptable loss, such as quantisation within a published bound.
    // Recorded, does not block by default.
    warning = 1,
    // Semantic data removed or guessed -- SH degree 4 reduced to 3, a scene graph flattened.
    // Blocks the commit unless the caller approves this exact loss code.
    severe = 2,
    // The target cannot represent the asset without violating an invariant. Always blocks;
    // cannot be approved.
    fatal = 3,
};

const char* to_string(LossSeverity severity) noexcept;

// One thing a conversion lost, with the machine code, how many splats it touched, and how to
// avoid it.
struct LossItem {
    std::string code;              // stable, e.g. "LOSS_SH_DEGREE_TRUNCATED"
    LossSeverity severity = LossSeverity::info;
    std::string source_feature;    // what the source had
    std::string target_constraint; // why the target cannot keep it
    std::uint64_t affected_splats = 0;
    std::string remediation;       // what the user can do about it
};

// The stable loss codes. Adding a code is a compatible change; changing what a code means is not.
namespace loss_code {
inline constexpr const char* kShDegreeTruncated = "LOSS_SH_DEGREE_TRUNCATED";
inline constexpr const char* kShCoefficientsDropped = "LOSS_SH_COEFFICIENTS_DROPPED";
// A node rotation applies to the geometry, but its rotation of the (degree >= 1) spherical
// harmonics -- a Wigner-D transform -- is not yet implemented, so the view-dependent colour is left
// in the source frame. Severe: it changes rendered appearance for a rotated splat, so it must be
// approved rather than silently accepted.
inline constexpr const char* kShRotationNotApplied = "LOSS_SH_ROTATION_NOT_APPLIED";
inline constexpr const char* kSceneGraphFlattened = "LOSS_SCENE_GRAPH_FLATTENED";
inline constexpr const char* kNodeNameDropped = "LOSS_NODE_NAME_DROPPED";
inline constexpr const char* kInstanceExpanded = "LOSS_INSTANCE_EXPANDED";
inline constexpr const char* kMaterialApproximated = "LOSS_MATERIAL_APPROXIMATED";
inline constexpr const char* kTextureBaked = "LOSS_TEXTURE_BAKED";
inline constexpr const char* kAntialiasingMetadataDropped = "LOSS_ANTIALIASING_METADATA_DROPPED";
inline constexpr const char* kColorSpaceAssumed = "LOSS_COLOR_SPACE_ASSUMED";
inline constexpr const char* kCoordinateMetadataDropped = "LOSS_COORDINATE_METADATA_DROPPED";
inline constexpr const char* kProvenanceDropped = "LOSS_PROVENANCE_DROPPED";
inline constexpr const char* kQuantizationApplied = "LOSS_QUANTIZATION_APPLIED";
inline constexpr const char* kOpacityClamped = "LOSS_OPACITY_CLAMPED";
inline constexpr const char* kScaleClamped = "LOSS_SCALE_CLAMPED";
inline constexpr const char* kNonfiniteRepaired = "LOSS_NONFINITE_REPAIRED";
inline constexpr const char* kInvalidSplatDropped = "LOSS_INVALID_SPLAT_DROPPED";
inline constexpr const char* kUnknownPropertyDropped = "LOSS_UNKNOWN_PROPERTY_DROPPED";
inline constexpr const char* kExtensionDropped = "LOSS_EXTENSION_DROPPED";
inline constexpr const char* kPrecisionReduced = "LOSS_PRECISION_REDUCED";
}  // namespace loss_code

// The set of losses a conversion would incur, plus the policy that decides whether they may be
// committed.
class LossReport {
  public:
    void add(LossItem item);
    const std::vector<LossItem>& items() const noexcept { return items_; }
    bool empty() const noexcept { return items_.empty(); }

    // True if any item is severe or fatal.
    bool has_blocking() const noexcept;

    // The schema version of the serialised report. New optional fields may be added within a
    // version; an existing field never changes meaning.
    static constexpr int kSchemaVersion = 1;

    // Decides whether this report may be committed, given the exact loss codes the caller has
    // approved. Returns success when every severe loss is approved and no fatal loss exists;
    // otherwise fails with `unsupported_feature` and a diagnostic naming the first unapproved
    // loss and the exact --allow-loss code that would permit it.
    //
    // A fatal loss can never be approved. Approving "all" is a CLI-only escape hatch and is not
    // expressible here: the API requires exact codes, so a program cannot wave through a loss it
    // did not name.
    Result<void> check_policy(const std::vector<std::string>& approved_codes) const;

  private:
    std::vector<LossItem> items_;
};

}  // namespace melkor

#endif  // MELKOR_FORMAT_LOSS_HPP
