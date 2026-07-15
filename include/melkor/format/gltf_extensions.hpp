// glTF extension policy.
//
// glTF's extension model has a precise contract that a correct reader must honour: an extension in
// `extensionsRequired` MUST be understood or the asset MUST be rejected, because ignoring a required
// extension can silently produce wrong geometry (a required compression extension, for instance,
// means the accessor bytes are not what they appear). An extension in `extensionsUsed` but not
// required MAY be ignored.
//
// The pre-v2 behaviour rejected required extensions too broadly (P0-10): it refused assets it could
// in fact have read. This module makes the decision precisely -- reject only the required extensions
// Melkor does not implement, and report (rather than silently drop) the used-but-ignored ones -- so
// a conforming `KHR_gaussian_splatting` asset is accepted and an asset that genuinely needs
// something Melkor cannot do is refused with a clear reason.

#ifndef MELKOR_FORMAT_GLTF_EXTENSIONS_HPP
#define MELKOR_FORMAT_GLTF_EXTENSIONS_HPP

#include <string>
#include <string_view>
#include <vector>

namespace melkor::format::gltf {

// Whether Melkor's splat reader can correctly honour a glTF extension. The reader acts on
// `KHR_gaussian_splatting`; everything else it neither needs nor understands, so a *required* other
// extension makes the asset unreadable. This is the allowlist that decision is made against.
bool is_supported_read_extension(std::string_view name);

struct ExtensionEvaluation {
    // Required extensions Melkor does not implement: the asset MUST be rejected. Non-empty here
    // means "refuse", with these names as the reason.
    std::vector<std::string> unsupported_required;
    // Extensions declared used (and not required) that Melkor does not act on: safely ignored, but
    // reported so the inspection output is honest about what was present and skipped.
    std::vector<std::string> ignored_used;
};

// Evaluates a document's `extensionsUsed`/`extensionsRequired` against what the reader supports.
// A name that appears in both lists is treated as required. The evaluation is order-independent and
// de-duplicates, so a repeated declaration does not produce a repeated finding.
ExtensionEvaluation evaluate_extensions(const std::vector<std::string>& used,
                                        const std::vector<std::string>& required);

}  // namespace melkor::format::gltf

#endif  // MELKOR_FORMAT_GLTF_EXTENSIONS_HPP
