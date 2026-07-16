// Canonical scene metadata and reproducible provenance.
//
// Format adapters decode their storage conventions into SplatData; callers and the future WP06
// registry can record the observed conventions here. The canonical domains are explicit in the
// type so a later conversion does not guess from numeric ranges and accidentally apply
// exp/sigmoid twice. The current flat adapters return SplatData plus format-specific metadata;
// automatic SplatPrimitive assembly is registry work, not an implicit claim of this model API.

#ifndef MELKOR_PROVENANCE_HPP
#define MELKOR_PROVENANCE_HPP

#include "melkor/error.hpp"
#include "melkor/math/coordinate_frame.hpp"
#include "melkor/scene.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace melkor {

enum class QuaternionOrder : std::uint8_t { xyzw = 0 };
enum class ScaleDomain : std::uint8_t { linear = 0 };
enum class OpacityDomain : std::uint8_t { linear = 0 };
enum class ColorSpace : std::uint8_t { linear_srgb_rec709 = 0 };
enum class ShBasis : std::uint8_t { real_condon_shortley = 0 };

const char* to_string(QuaternionOrder value) noexcept;
const char* to_string(ScaleDomain value) noexcept;
const char* to_string(OpacityDomain value) noexcept;
const char* to_string(ColorSpace value) noexcept;
const char* to_string(ShBasis value) noexcept;

struct SplatMetadata {
    math::CoordinateFrame frame = math::canonical_frame();
    QuaternionOrder quaternion_order = QuaternionOrder::xyzw;
    ScaleDomain scale_domain = ScaleDomain::linear;
    OpacityDomain opacity_domain = OpacityDomain::linear;
    ColorSpace color_space = ColorSpace::linear_srgb_rec709;
    ShBasis sh_basis = ShBasis::real_condon_shortley;
    std::uint8_t sh_degree = 0;
    bool antialiased = false;
};

struct ProvenanceOperation {
    std::string name;
    std::string tool_version;
    std::map<std::string, JsonScalar> parameters;
    std::optional<std::string> timestamp;
};

struct Provenance {
    std::string source_format;
    std::string source_profile;
    std::optional<std::string> source_sha256;
    std::vector<ProvenanceOperation> operations;
};

// A canonical primitive cannot be constructed with metadata that disagrees with its data. The
// full hierarchy-preserving Scene/Node model belongs to the registry work; this minimal primitive
// is the format-neutral unit A1 needs and keeps that later extension additive.
class SplatPrimitive {
  public:
    static Result<SplatPrimitive> create(SplatMetadata metadata, SplatData data,
                                         Provenance provenance);

    const SplatMetadata& metadata() const noexcept { return metadata_; }
    const SplatData& data() const noexcept { return data_; }
    const Provenance& provenance() const noexcept { return provenance_; }

    Result<void> validate() const;

  private:
    SplatPrimitive(SplatMetadata metadata, SplatData data, Provenance provenance)
        : metadata_(std::move(metadata)),
          data_(std::move(data)),
          provenance_(std::move(provenance)) {}

    SplatMetadata metadata_;
    SplatData data_;
    Provenance provenance_;
};

// Deterministic JSON matching schemas/provenance-v1.schema.json. In reproducible mode (the
// default), every operation timestamp is emitted as null even when the caller supplied one.
// Provenance has deliberately no source-path field, so the default report cannot leak an absolute
// path. A future opt-in diagnostic path policy must remain separate from this public artifact.
Result<std::string> provenance_to_json(const Provenance& provenance,
                                       bool reproducible = true);

}  // namespace melkor

#endif  // MELKOR_PROVENANCE_HPP
