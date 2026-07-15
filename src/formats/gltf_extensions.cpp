#include "melkor/format/gltf_extensions.hpp"

#include "melkor/format/gltf_khr.hpp"

#include <algorithm>
#include <set>

namespace melkor::format::gltf {

bool is_supported_read_extension(std::string_view name) {
    // The only extension the splat reader acts on is KHR_gaussian_splatting itself. (A future
    // compressed-splat or wide-gamut extension would be added here once implemented and pinned.)
    return name == khr::kExtensionName;
}

ExtensionEvaluation evaluate_extensions(const std::vector<std::string>& used,
                                        const std::vector<std::string>& required) {
    // A name in `required` is required even if it also appears (redundantly) in `used`.
    const std::set<std::string> required_set(required.begin(), required.end());

    ExtensionEvaluation result;
    std::set<std::string> seen_unsupported_required;
    std::set<std::string> seen_ignored_used;

    for (const auto& name : required_set) {
        if (!is_supported_read_extension(name)) {
            if (seen_unsupported_required.insert(name).second) {
                result.unsupported_required.push_back(name);
            }
        }
    }

    for (const auto& name : used) {
        if (required_set.count(name) != 0) {
            continue;  // handled by the required pass
        }
        if (!is_supported_read_extension(name)) {
            if (seen_ignored_used.insert(name).second) {
                result.ignored_used.push_back(name);
            }
        }
    }

    // Deterministic order regardless of input order.
    std::sort(result.unsupported_required.begin(), result.unsupported_required.end());
    std::sort(result.ignored_used.begin(), result.ignored_used.end());
    return result;
}

}  // namespace melkor::format::gltf
