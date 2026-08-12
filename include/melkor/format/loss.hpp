// The conversion loss report.
//
// A format conversion is honest only when it says what it lost. Converting a canonical degree-4
// asset to the degree-3 glTF profile drops coefficients. Flattening a glTF scene graph into PLY
// loses hierarchy. Quantizing into SPZ introduces measurable error. These changes are not failures.
// A conversion that discards them silently gives a false success result.
//
// Every conversion produces a `LossReport`, including a zero-loss report. Automation never has to
// infer whether the conversion omitted its report. An unapproved severe loss stops the commit.
// A fatal loss always stops the commit. Validation diagnostics use a separate mechanism.
// A malformed file or resource-limit failure is an error and cannot pass through the loss policy.
//
// The codes here are stable machine identifiers. A consumer that special-cases
// `LOSS_SH_DEGREE_TRUNCATED` can rely on it meaning the same thing across the 2.x line.

#ifndef MELKOR_FORMAT_LOSS_HPP
#define MELKOR_FORMAT_LOSS_HPP

#include "melkor/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace melkor {

// How consequential a loss is. The policy acts on this.
enum class LossSeverity : std::uint8_t {
    // A representational change with no expected rendered difference -- a quaternion normalized
    // within tolerance, or a node hierarchy flattened into the canonical flat splat cloud (which
    // cannot represent a hierarchy, so no rendered detail is lost once the transforms are baked).
    // Recorded, never blocks.
    info = 0,
    // A measurable but usually acceptable loss, such as quantization within a published bound.
    // Recorded, does not block by default.
    warning = 1,
    // Semantic data removed or guessed -- SH degree 4 reduced to 3, or a color space assumed
    // because the source named an unrecognized one. Blocks the commit unless the caller approves
    // this exact loss code.
    severe = 2,
    // The target cannot represent the asset without violating an invariant. Always blocks;
    // cannot be approved.
    fatal = 3,
};

const char* to_string(LossSeverity severity) noexcept;

// One thing a conversion lost, with the machine code, how many splats it touched, and how to
// avoid it.
struct LossItem {
    std::string code;  // stable, e.g. "LOSS_SH_DEGREE_TRUNCATED"
    LossSeverity severity = LossSeverity::info;
    std::string source_feature;     // what the source had
    std::string target_constraint;  // why the target cannot keep it
    std::uint64_t affected_splats = 0;
    std::string remediation;  // what the user can do about it
};

// The stable loss codes. A code keeps one meaning. A new code requires a new report schema version.
namespace loss_code {
inline constexpr const char* kShDegreeTruncated = "LOSS_SH_DEGREE_TRUNCATED";
inline constexpr const char* kShCoefficientsDropped = "LOSS_SH_COEFFICIENTS_DROPPED";
inline constexpr const char* kShCoefficientsClamped = "LOSS_SH_COEFFICIENTS_CLAMPED";
// Reserved for compatibility with the v2 release-candidate loss registry.
// The current KHR profile rejects transforms that would require this loss.
inline constexpr const char* kShRotationNotApplied = "LOSS_SH_ROTATION_NOT_APPLIED";
inline constexpr const char* kSceneGraphFlattened = "LOSS_SCENE_GRAPH_FLATTENED";
inline constexpr const char* kNodeNameDropped = "LOSS_NODE_NAME_DROPPED";
inline constexpr const char* kInstanceExpanded = "LOSS_INSTANCE_EXPANDED";
inline constexpr const char* kMaterialApproximated = "LOSS_MATERIAL_APPROXIMATED";
inline constexpr const char* kTextureBaked = "LOSS_TEXTURE_BAKED";
inline constexpr const char* kAntialiasingMetadataDropped = "LOSS_ANTIALIASING_METADATA_DROPPED";
inline constexpr const char* kColorSpaceAssumed = "LOSS_COLOR_SPACE_ASSUMED";
inline constexpr const char* kColorSpacesConflict = "LOSS_COLOR_SPACES_CONFLICT";
inline constexpr const char* kColorClamped = "LOSS_COLOR_CLAMPED";
inline constexpr const char* kColorSpaceMetadataDropped = "LOSS_COLOR_SPACE_METADATA_DROPPED";
inline constexpr const char* kCoordinateMetadataDropped = "LOSS_COORDINATE_METADATA_DROPPED";
inline constexpr const char* kProvenanceDropped = "LOSS_PROVENANCE_DROPPED";
inline constexpr const char* kAttributionDropped = "LOSS_ATTRIBUTION_DROPPED";
inline constexpr const char* kQuantizationApplied = "LOSS_QUANTIZATION_APPLIED";
inline constexpr const char* kOpacityClamped = "LOSS_OPACITY_CLAMPED";
inline constexpr const char* kScaleClamped = "LOSS_SCALE_CLAMPED";
inline constexpr const char* kNonfiniteRepaired = "LOSS_NONFINITE_REPAIRED";
inline constexpr const char* kInvalidSplatDropped = "LOSS_INVALID_SPLAT_DROPPED";
inline constexpr const char* kUnknownPropertyDropped = "LOSS_UNKNOWN_PROPERTY_DROPPED";
inline constexpr const char* kMetadataDropped = "LOSS_METADATA_DROPPED";
inline constexpr const char* kVertexNormalsDropped = "LOSS_VERTEX_NORMALS_DROPPED";
inline constexpr const char* kQuaternionNormalized = "LOSS_QUATERNION_NORMALIZED";
inline constexpr const char* kExtensionDropped = "LOSS_EXTENSION_DROPPED";
inline constexpr const char* kExtensionDeclarationDropped = "LOSS_EXTENSION_DECLARATION_DROPPED";
inline constexpr const char* kGltfContentDropped = "LOSS_GLTF_CONTENT_DROPPED";
inline constexpr const char* kPrecisionReduced = "LOSS_PRECISION_REDUCED";
inline constexpr const char* kGaussianAttributesGenerated = "LOSS_GAUSSIAN_ATTRIBUTES_GENERATED";
}  // namespace loss_code

inline constexpr std::size_t kKnownLossCodeCount = 31;

// Return true when `code` is part of the stable Melkor loss-code registry.
bool is_known_loss_code(std::string_view code) noexcept;

// Return the complete stable registry in declaration order.
const std::array<std::string_view, kKnownLossCodeCount>& known_loss_codes() noexcept;

// The set of losses a conversion would incur, plus the policy that decides whether they may be
// committed.
class LossReport {
public:
    // Validate and append one item. A failure does not change the report.
    Result<void> add(LossItem item);

    // Append a complete report. A failure does not change either report.
    Result<void> append(const LossReport& other);
    const std::vector<LossItem>& items() const noexcept { return items_; }
    bool empty() const noexcept { return items_.empty(); }

    // True if any item is severe or fatal.
    bool has_blocking() const noexcept;

    // Validate all report items. This check rejects forged severity values and incomplete items.
    Result<void> validate() const;

    // The schema version of the serialized report. Version 1 has a closed field set.
    // A field change or meaning change requires a new schema version.
    static constexpr int kSchemaVersion = 1;

    // Decides whether this report may be committed, given the exact loss codes the caller has
    // approved. Returns success when every severe loss is approved and no fatal loss exists;
    // otherwise fails with `unsupported_feature` and a diagnostic naming the first unapproved
    // loss and the exact --allow-loss code that would permit it.
    //
    // A fatal loss can never be approved. The API and CLI require exact codes. A caller cannot
    // approve a loss that it did not name.
    Result<void> check_policy(const std::vector<std::string>& approved_codes) const;

private:
    std::vector<LossItem> items_;
};

}  // namespace melkor

#endif  // MELKOR_FORMAT_LOSS_HPP
