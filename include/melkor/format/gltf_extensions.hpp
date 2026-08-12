// glTF extension policy.
//
// glTF has a precise extension contract. A reader must understand each name in
// `extensionsRequired`. Otherwise, it must reject the asset. Ignoring required compression can
// make accessor bytes appear valid when they are not. An extension declared only in
// `extensionsUsed` can be ignored.
//
// The pre-v2 behavior rejected some supported assets. This module applies the exact rule. It
// rejects unsupported required names. It reports optional names that Melkor does not use.

#ifndef MELKOR_FORMAT_GLTF_EXTENSIONS_HPP
#define MELKOR_FORMAT_GLTF_EXTENSIONS_HPP

#include <string>
#include <string_view>
#include <vector>

namespace melkor::format::gltf {

// Whether Melkor's splat reader can correctly honor a glTF extension. The reader acts on
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
