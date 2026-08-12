#include "melkor/format/gltf_document.hpp"

#include "melkor/checked.hpp"
#include "melkor/format/gltf_khr.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace melkor::format::gltf {

namespace {

using json = nlohmann::json;

Result<Document> fail(const char* code, std::string message) {
    Diagnostic d(code, Severity::error, std::move(message));
    return Result<Document>::failure(ErrorCode::invalid_data, std::move(d));
}

Result<Document> resource_failure(const char* code, std::string message) {
    Diagnostic d(code, Severity::error, std::move(message));
    return Result<Document>::failure(ErrorCode::resource_limit, std::move(d));
}

Result<Document> unsupported_failure(const char* code, std::string message) {
    Diagnostic d(code, Severity::error, std::move(message));
    return Result<Document>::failure(ErrorCode::unsupported_feature, std::move(d));
}

// A non-negative integer index. glTF indices are non-negative; a negative or fractional value is
// malformed, and a float where an index is expected is a common corruption we reject rather than
// truncate.
bool as_index(const json& j, std::uint64_t& out) {
    if (j.is_number_unsigned()) {
        out = j.get<std::uint64_t>();
        return true;
    }
    if (j.is_number_integer()) {
        const std::int64_t v = j.get<std::int64_t>();
        if (v >= 0) {
            out = static_cast<std::uint64_t>(v);
            return true;
        }
    }
    return false;
}

bool as_double(const json& j, double& out) {
    if (j.is_number()) {
        out = j.get<double>();
        return true;
    }
    return false;
}

// Reads an optional non-negative integer member; returns true if absent or a valid index (writing
// `out` only when present), false if present-but-invalid.
bool opt_index(const json& obj, const char* key, std::uint64_t& out, bool& present) {
    present = false;
    auto it = obj.find(key);
    if (it == obj.end())
        return true;
    present = true;
    return as_index(*it, out);
}

std::optional<ElementType> element_type_from_string(const std::string& s) {
    if (s == "SCALAR")
        return ElementType::scalar;
    if (s == "VEC2")
        return ElementType::vec2;
    if (s == "VEC3")
        return ElementType::vec3;
    if (s == "VEC4")
        return ElementType::vec4;
    if (s == "MAT2")
        return ElementType::mat2;
    if (s == "MAT3")
        return ElementType::mat3;
    if (s == "MAT4")
        return ElementType::mat4;
    return std::nullopt;
}

std::size_t document_component_count(ElementType type) noexcept {
    switch (type) {
    case ElementType::mat2:
        return 4;
    case ElementType::mat3:
        return 9;
    case ElementType::mat4:
        return 16;
    default:
        return component_count(type);
    }
}

bool matrix_decomposes_to_trs(const std::array<double, 16>& matrix) noexcept {
    std::array<std::array<double, 3>, 3> columns{{
        {{matrix[0], matrix[1], matrix[2]}},
        {{matrix[4], matrix[5], matrix[6]}},
        {{matrix[8], matrix[9], matrix[10]}},
    }};
    for (auto& column : columns) {
        const double length = std::hypot(column[0], column[1], column[2]);
        if (!std::isfinite(length))
            return false;
        if (length == 0.0)
            continue;
        for (double& value : column)
            value /= length;
    }
    constexpr double kOrthogonalityTolerance = 1e-6;
    for (std::size_t left = 0; left < columns.size(); ++left) {
        for (std::size_t right = left + 1; right < columns.size(); ++right) {
            const double dot = columns[left][0] * columns[right][0] +
                               columns[left][1] * columns[right][1] +
                               columns[left][2] * columns[right][2];
            if (std::fabs(dot) > kOrthogonalityTolerance)
                return false;
        }
    }
    return true;
}

// Reads a fixed-length array of numbers (e.g. a 16-element matrix). Returns false on any shape or
// type mismatch.
template <std::size_t N>
bool read_number_array(const json& obj, const char* key, std::array<double, N>& out,
                       bool& present) {
    present = false;
    auto it = obj.find(key);
    if (it == obj.end())
        return true;
    present = true;
    if (!it->is_array() || it->size() != N)
        return false;
    for (std::size_t i = 0; i < N; ++i) {
        if (!as_double((*it)[i], out[i]))
            return false;
    }
    return true;
}

bool read_string_array(const json& arr, std::vector<std::string>& out) {
    if (!arr.is_array() || arr.empty())
        return false;
    for (const auto& e : arr) {
        if (!e.is_string())
            return false;
        std::string value = e.get<std::string>();
        if (value.empty())
            return false;
        out.push_back(std::move(value));
    }
    std::sort(out.begin(), out.end());
    if (std::adjacent_find(out.begin(), out.end()) != out.end())
        return false;
    return true;
}

enum class MinimumVersionStatus {
    supported,
    unsupported,
    malformed,
};

MinimumVersionStatus minimum_version_status(std::string_view version) {
    const std::size_t separator = version.find('.');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == version.size() ||
        version.find('.', separator + 1) != std::string_view::npos) {
        return MinimumVersionStatus::malformed;
    }

    const auto parse_component = [](std::string_view component, std::uint32_t& output) -> bool {
        // glTF permits at most nine digits. It permits a leading zero only for the value zero.
        if (component.empty() || component.size() > 9 ||
            (component.size() > 1 && component.front() == '0')) {
            return false;
        }
        output = 0;
        for (char value : component) {
            if (value < '0' || value > '9')
                return false;
            output = output * 10U + static_cast<std::uint32_t>(value - '0');
        }
        return true;
    };

    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    if (!parse_component(version.substr(0, separator), major) ||
        !parse_component(version.substr(separator + 1), minor)) {
        return MinimumVersionStatus::malformed;
    }
    return major < 2 || (major == 2 && minor == 0) ? MinimumVersionStatus::supported
                                                   : MinimumVersionStatus::unsupported;
}

bool member_is_allowed(const std::string& member, std::initializer_list<const char*> allowed) {
    return std::any_of(allowed.begin(), allowed.end(),
                       [&](const char* candidate) { return member == candidate; });
}

bool is_supported_splat_attribute(const std::string& semantic) {
    if (semantic == khr::kAttrPosition || semantic == khr::kAttrRotation ||
        semantic == khr::kAttrScale || semantic == khr::kAttrOpacity) {
        return true;
    }
    const auto address = khr::parse_sh_attribute(semantic);
    return address.has_value() && address->degree <= khr::kMaxProfileShDegree;
}

constexpr std::size_t kFeatureSampleLimit = 32;
constexpr std::size_t kMaxJsonDepth = 256;
constexpr std::uint64_t kJsonByteMemoryFactor = 32;
constexpr std::uint64_t kJsonObjectMemoryBytes = 512;
constexpr std::uint64_t kJsonArrayMemoryBytes = 64;

struct JsonControlStop {};

struct FeatureScanControl {
    const OperationContext* context = nullptr;
    ErrorCode* error = nullptr;
    std::vector<Diagnostic>* diagnostics = nullptr;
    std::uint64_t visited = 0;

    void visit() {
        if (visited != std::numeric_limits<std::uint64_t>::max())
            ++visited;
        if (context == nullptr || visited % 4096 != 0)
            return;
        auto control = context->checkpoint({"gltf.read", "source_features", visited, 0, "members"});
        if (control.has_value())
            return;
        *error = control.error_code();
        *diagnostics = control.diagnostics();
        throw JsonControlStop{};
    }

    void finish() {
        if (context == nullptr)
            return;
        auto control =
            context->checkpoint({"gltf.read", "source_features", visited, visited, "members"});
        if (control.has_value())
            return;
        *error = control.error_code();
        *diagnostics = control.diagnostics();
        throw JsonControlStop{};
    }
};

struct ExtensionOccurrenceScan {
    std::set<std::string> names;
    std::optional<std::string> invalid_container;
};

void scan_extension_occurrences(const json& value, const std::string& path,
                                ExtensionOccurrenceScan& scan, FeatureScanControl& control) {
    if (value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            control.visit();
            scan_extension_occurrences(value[index], path + "[" + std::to_string(index) + "]", scan,
                                       control);
        }
        return;
    }
    if (!value.is_object())
        return;

    for (auto member = value.begin(); member != value.end(); ++member) {
        control.visit();
        if (member.key() == "extras") {
            // `extras` contains application data. A member named `extensions` in that data is not
            // a glTF extension occurrence.
            continue;
        }
        if (member.key() != "extensions") {
            scan_extension_occurrences(member.value(), path + "." + member.key(), scan, control);
            continue;
        }

        const std::string extension_path = path + ".extensions";
        if (!member->is_object()) {
            if (!scan.invalid_container.has_value())
                scan.invalid_container = extension_path;
            continue;
        }
        for (auto extension = member->begin(); extension != member->end(); ++extension) {
            control.visit();
            scan.names.insert(extension.key());
            scan_extension_occurrences(extension.value(), extension_path + "." + extension.key(),
                                       scan, control);
        }
    }
}

struct JsonContainerState {
    bool is_object = false;
    std::string pending_key;
    std::set<std::string> keys;
};

struct JsonStructureScan {
    bool depth_exceeded = false;
    std::uint64_t object_count = 0;
    std::uint64_t array_count = 0;
};

bool is_data_uri(std::string_view value) noexcept {
    constexpr std::string_view prefix = "data:";
    if (value.size() < prefix.size())
        return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        auto byte = static_cast<unsigned char>(value[index]);
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<unsigned char>(byte + ('a' - 'A'));
        if (byte != static_cast<unsigned char>(prefix[index]))
            return false;
    }
    return true;
}

Result<JsonStructureScan> scan_json_structure(const std::uint8_t* data, std::size_t size,
                                              const OperationContext* context) {
    JsonStructureScan result;
    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < size; ++i) {
        if (context != nullptr && i % (std::size_t{1024} * 1024) == 0) {
            auto control = context->checkpoint({"gltf.read", "json_structure", i, size, "bytes"});
            if (!control.has_value()) {
                return Result<JsonStructureScan>::failure(control.error_code(),
                                                          control.diagnostics());
            }
        }
        const char value = static_cast<char>(data[i]);
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                in_string = false;
            }
            continue;
        }
        if (value == '"') {
            in_string = true;
        } else if (value == '{' || value == '[') {
            ++depth;
            if (value == '{' && result.object_count != std::numeric_limits<std::uint64_t>::max())
                ++result.object_count;
            if (value == '[' && result.array_count != std::numeric_limits<std::uint64_t>::max())
                ++result.array_count;
            if (depth > kMaxJsonDepth)
                result.depth_exceeded = true;
        } else if ((value == '}' || value == ']') && depth != 0) {
            --depth;
        }
    }
    if (context != nullptr) {
        auto control = context->checkpoint({"gltf.read", "json_structure", size, size, "bytes"});
        if (!control.has_value()) {
            return Result<JsonStructureScan>::failure(control.error_code(), control.diagnostics());
        }
    }
    return Result<JsonStructureScan>::success(result);
}

Result<std::uint64_t> json_memory_bound(std::size_t size, const JsonStructureScan& structure) {
    auto bytes = checked_mul(static_cast<std::uint64_t>(size), kJsonByteMemoryFactor,
                             "glTF JSON byte working memory");
    auto objects = checked_mul(structure.object_count, kJsonObjectMemoryBytes,
                               "glTF JSON object working memory");
    auto arrays =
        checked_mul(structure.array_count, kJsonArrayMemoryBytes, "glTF JSON array working memory");
    if (!bytes.has_value() || !objects.has_value() || !arrays.has_value()) {
        return Result<std::uint64_t>::failure(
            ErrorCode::resource_limit, Diagnostic("MK2149_GLTF_JSON_ALLOCATION", Severity::error,
                                                  "glTF JSON working memory is not representable"));
    }
    auto with_objects = checked_add(bytes.value(), objects.value(), "glTF JSON working memory");
    auto total =
        with_objects.has_value()
            ? checked_add(with_objects.value(), arrays.value(), "glTF JSON working memory")
            : Result<std::uint64_t>::failure(with_objects.error_code(), with_objects.diagnostics());
    if (!total.has_value()) {
        return Result<std::uint64_t>::failure(
            ErrorCode::resource_limit, Diagnostic("MK2149_GLTF_JSON_ALLOCATION", Severity::error,
                                                  "glTF JSON working memory is not representable"));
    }
    return total;
}

void record_feature(std::uint64_t& count, std::vector<std::string>& samples, std::string path) {
    if (count != std::numeric_limits<std::uint64_t>::max())
        ++count;
    if (samples.size() < kFeatureSampleLimit)
        samples.push_back(std::move(path));
}

void record_unknown_members(const json& object, const std::string& path,
                            std::initializer_list<const char*> allowed,
                            SourceFeatureSummary& summary, FeatureScanControl& control) {
    if (!object.is_object())
        return;
    for (auto member = object.begin(); member != object.end(); ++member) {
        control.visit();
        if (!member_is_allowed(member.key(), allowed)) {
            record_feature(summary.property_count, summary.property_samples,
                           path + "." + member.key());
        }
    }
}

void record_extensions(const json& object, const std::string& path, SourceFeatureSummary& summary,
                       FeatureScanControl& control, bool allow_gaussian = false) {
    if (!object.is_object())
        return;
    auto extensions = object.find("extensions");
    if (extensions == object.end())
        return;
    if (!extensions->is_object()) {
        record_feature(summary.extension_count, summary.extension_samples, path + ".extensions");
        return;
    }
    for (auto extension = extensions->begin(); extension != extensions->end(); ++extension) {
        control.visit();
        if (!allow_gaussian || extension.key() != khr::kExtensionName) {
            record_feature(summary.extension_count, summary.extension_samples,
                           path + ".extensions." + extension.key());
        }
    }
}

void summarize_source_features(const json& doc, SourceFeatureSummary& summary,
                               FeatureScanControl& control) {
    record_unknown_members(doc, "root",
                           {"asset", "extensions", "extensionsUsed", "extensionsRequired",
                            "buffers", "bufferViews", "accessors", "meshes", "nodes", "scenes",
                            "scene"},
                           summary, control);
    record_extensions(doc, "root", summary, control);

    if (auto asset = doc.find("asset"); asset != doc.end()) {
        record_unknown_members(*asset, "asset",
                               {"version", "minVersion", "generator", "copyright", "extensions"},
                               summary, control);
        record_extensions(*asset, "asset", summary, control);
        if (asset->contains("generator")) {
            record_feature(summary.provenance_count, summary.provenance_samples, "asset.generator");
        }
        if (asset->contains("copyright")) {
            record_feature(summary.attribution_count, summary.attribution_samples,
                           "asset.copyright");
        }
    }

    if (auto buffers = doc.find("buffers"); buffers != doc.end() && buffers->is_array()) {
        for (std::size_t i = 0; i < buffers->size(); ++i) {
            control.visit();
            const std::string path = "buffers[" + std::to_string(i) + "]";
            record_unknown_members((*buffers)[i], path, {"byteLength", "uri", "extensions"},
                                   summary, control);
            record_extensions((*buffers)[i], path, summary, control);
        }
    }

    if (auto views = doc.find("bufferViews"); views != doc.end() && views->is_array()) {
        for (std::size_t i = 0; i < views->size(); ++i) {
            control.visit();
            const std::string path = "bufferViews[" + std::to_string(i) + "]";
            record_unknown_members(
                (*views)[i], path,
                {"buffer", "byteOffset", "byteLength", "byteStride", "target", "extensions"},
                summary, control);
            record_extensions((*views)[i], path, summary, control);
        }
    }

    if (auto accessors = doc.find("accessors"); accessors != doc.end() && accessors->is_array()) {
        for (std::size_t i = 0; i < accessors->size(); ++i) {
            control.visit();
            const std::string path = "accessors[" + std::to_string(i) + "]";
            record_unknown_members((*accessors)[i], path,
                                   {"bufferView", "byteOffset", "componentType", "normalized",
                                    "count", "type", "min", "max", "extensions"},
                                   summary, control);
            record_extensions((*accessors)[i], path, summary, control);
        }
    }

    if (auto meshes = doc.find("meshes"); meshes != doc.end() && meshes->is_array()) {
        for (std::size_t mesh_index = 0; mesh_index < meshes->size(); ++mesh_index) {
            control.visit();
            const json& mesh = (*meshes)[mesh_index];
            const std::string mesh_path = "meshes[" + std::to_string(mesh_index) + "]";
            record_unknown_members(mesh, mesh_path, {"primitives", "extensions"}, summary, control);
            record_extensions(mesh, mesh_path, summary, control);
            if (!mesh.is_object())
                continue;
            auto primitives = mesh.find("primitives");
            if (primitives == mesh.end() || !primitives->is_array())
                continue;
            for (std::size_t primitive_index = 0; primitive_index < primitives->size();
                 ++primitive_index) {
                control.visit();
                const json& primitive = (*primitives)[primitive_index];
                const std::string primitive_path =
                    mesh_path + ".primitives[" + std::to_string(primitive_index) + "]";
                if (!primitive.is_object())
                    continue;
                auto extensions = primitive.find("extensions");
                const bool has_gaussian =
                    extensions != primitive.end() && extensions->is_object() &&
                    extensions->find(khr::kExtensionName) != extensions->end();
                if (!has_gaussian) {
                    if (summary.non_gaussian_primitive_count !=
                        std::numeric_limits<std::uint64_t>::max()) {
                        ++summary.non_gaussian_primitive_count;
                    }
                    continue;
                }

                record_unknown_members(primitive, primitive_path,
                                       {"attributes", "mode", "extensions"}, summary, control);
                record_extensions(primitive, primitive_path, summary, control, true);
                if (auto attributes = primitive.find("attributes");
                    attributes != primitive.end() && attributes->is_object()) {
                    for (auto attribute = attributes->begin(); attribute != attributes->end();
                         ++attribute) {
                        control.visit();
                        if (!is_supported_splat_attribute(attribute.key())) {
                            record_feature(summary.property_count, summary.property_samples,
                                           primitive_path + ".attributes." + attribute.key());
                        }
                    }
                }
                const json& gaussian = (*extensions)[khr::kExtensionName];
                record_unknown_members(
                    gaussian, primitive_path + ".extensions." + khr::kExtensionName,
                    {"kernel", "colorSpace", "projection", "sortingMethod", "extensions"}, summary,
                    control);
                record_extensions(gaussian, primitive_path + ".extensions." + khr::kExtensionName,
                                  summary, control);
            }
        }
    }

    if (auto nodes = doc.find("nodes"); nodes != doc.end() && nodes->is_array()) {
        for (std::size_t i = 0; i < nodes->size(); ++i) {
            control.visit();
            const std::string path = "nodes[" + std::to_string(i) + "]";
            record_unknown_members(
                (*nodes)[i], path,
                {"matrix", "translation", "rotation", "scale", "mesh", "children", "extensions"},
                summary, control);
            record_extensions((*nodes)[i], path, summary, control);
        }
    }

    if (auto scenes = doc.find("scenes"); scenes != doc.end() && scenes->is_array()) {
        for (std::size_t i = 0; i < scenes->size(); ++i) {
            control.visit();
            const std::string path = "scenes[" + std::to_string(i) + "]";
            record_unknown_members((*scenes)[i], path, {"nodes", "extensions"}, summary, control);
            record_extensions((*scenes)[i], path, summary, control);
        }
    }
    control.finish();
}

}  // namespace

static Result<Document> parse_gltf_json_impl(const std::uint8_t* data, std::size_t size,
                                             const OperationContext* context) {
    if (data == nullptr || size == 0) {
        return fail("MK2130_GLTF_EMPTY", "empty glTF JSON");
    }
    auto structure_result = scan_json_structure(data, size, context);
    if (!structure_result.has_value()) {
        return Result<Document>::failure(structure_result.error_code(),
                                         structure_result.diagnostics());
    }
    const JsonStructureScan structure = structure_result.value();
    if (structure.depth_exceeded) {
        return resource_failure("MK2148_GLTF_JSON_DEPTH",
                                "glTF JSON exceeds the structural depth limit");
    }

    ErrorCode control_error = ErrorCode::internal_error;
    std::vector<Diagnostic> control_diagnostics;

    if (context != nullptr) {
        auto control = context->checkpoint({"gltf.read", "json_parse", 0, size, "bytes"});
        if (!control.has_value())
            return Result<Document>::failure(control.error_code(), control.diagnostics());
    }

    try {
        Budget::Charge json_memory;
        if (context != nullptr) {
            auto memory_bound = json_memory_bound(size, structure);
            if (!memory_bound.has_value()) {
                return Result<Document>::failure(memory_bound.error_code(),
                                                 memory_bound.diagnostics());
            }
            auto reserved = context->budget->reserve(BudgetKind::memory_bytes, memory_bound.value(),
                                                     "gltf.json_document");
            if (!reserved.has_value()) {
                return Result<Document>::failure(reserved.error_code(), reserved.diagnostics());
            }
            json_memory = std::move(reserved).value();
        }
        bool duplicate_key = false;
        std::uint64_t parse_events = 0;
        std::uint64_t metadata_bytes = 0;
        std::vector<JsonContainerState> containers;
        const Limits* limits = context != nullptr ? &context->budget->limits() : nullptr;

        auto stop_for_metadata_limit = [&](const char* message, std::uint64_t observed,
                                           std::uint64_t limit) {
            control_error = ErrorCode::resource_limit;
            Diagnostic diagnostic("MK2147_GLTF_METADATA_LIMIT", Severity::error, message);
            diagnostic.with_context("observed", observed);
            diagnostic.with_context("limit", limit);
            control_diagnostics = {std::move(diagnostic)};
            throw JsonControlStop{};
        };

        auto account_metadata = [&](const std::string& value, bool is_data_payload) {
            if (limits == nullptr || is_data_payload)
                return;
            const std::uint64_t bytes = value.size();
            if (bytes > limits->max_metadata_string_bytes) {
                stop_for_metadata_limit("a glTF metadata string exceeds the configured limit",
                                        bytes, limits->max_metadata_string_bytes);
            }
            if (metadata_bytes > limits->max_metadata_total_bytes ||
                bytes > limits->max_metadata_total_bytes - metadata_bytes) {
                const std::uint64_t observed =
                    bytes > std::numeric_limits<std::uint64_t>::max() - metadata_bytes
                        ? std::numeric_limits<std::uint64_t>::max()
                        : metadata_bytes + bytes;
                stop_for_metadata_limit("glTF metadata exceeds the configured total limit",
                                        observed, limits->max_metadata_total_bytes);
            }
            metadata_bytes += bytes;
        };

        const json::parser_callback_t callback = [&](int, json::parse_event_t event, json& parsed) {
            ++parse_events;
            if (context != nullptr && parse_events % 4096 == 0) {
                auto control =
                    context->checkpoint({"gltf.read", "json_parse", parse_events, 0, "events"});
                if (!control.has_value()) {
                    control_error = control.error_code();
                    control_diagnostics = control.diagnostics();
                    throw JsonControlStop{};
                }
            }
            if (event == json::parse_event_t::object_start) {
                containers.push_back(JsonContainerState{true, {}, {}});
            } else if (event == json::parse_event_t::array_start) {
                containers.push_back(JsonContainerState{false, {}, {}});
            } else if (event == json::parse_event_t::key && !containers.empty()) {
                const std::string& key = parsed.get_ref<const std::string&>();
                account_metadata(key, false);
                containers.back().pending_key = key;
                if (!containers.back().keys.insert(key).second)
                    duplicate_key = true;
            } else if (event == json::parse_event_t::value && parsed.is_string()) {
                const std::string& value = parsed.get_ref<const std::string&>();
                const bool is_data_payload = !containers.empty() && containers.back().is_object &&
                                             containers.back().pending_key == "uri" &&
                                             is_data_uri(value);
                account_metadata(value, is_data_payload);
            } else if ((event == json::parse_event_t::object_end ||
                        event == json::parse_event_t::array_end) &&
                       !containers.empty()) {
                containers.pop_back();
            }
            return true;
        };
        json doc = json::parse(data, data + size, callback, /*allow_exceptions=*/false,
                               /*ignore_comments=*/false);
        if (doc.is_discarded()) {
            return fail("MK2131_GLTF_BAD_JSON", "glTF JSON is not well-formed");
        }
        if (!doc.is_object()) {
            return fail("MK2131_GLTF_BAD_JSON", "glTF root is not a JSON object");
        }
        if (duplicate_key) {
            return fail("MK2131_GLTF_BAD_JSON", "glTF JSON contains a duplicate object key");
        }
        if (context != nullptr) {
            auto control = context->checkpoint({"gltf.read", "json_parse", size, size, "bytes"});
            if (!control.has_value())
                return Result<Document>::failure(control.error_code(), control.diagnostics());
        }

        auto model_checkpoint = [&](const char* phase, std::uint64_t completed,
                                    std::uint64_t total) {
            if (context == nullptr || (completed != total && completed % 4096 != 0))
                return;
            auto control = context->checkpoint({"gltf.read", phase, completed, total, "objects"});
            if (!control.has_value()) {
                control_error = control.error_code();
                control_diagnostics = control.diagnostics();
                throw JsonControlStop{};
            }
        };

        if (context != nullptr) {
            const auto array_size = [&](const char* key) -> std::uint64_t {
                auto member = doc.find(key);
                return member != doc.end() && member->is_array()
                           ? static_cast<std::uint64_t>(member->size())
                           : 0;
            };
            auto structural_count = checked_add(array_size("accessors"), array_size("bufferViews"),
                                                "glTF accessor and buffer-view count");
            if (!structural_count.has_value()) {
                return Result<Document>::failure(structural_count.error_code(),
                                                 structural_count.diagnostics());
            }
            auto structural = context->observe(BudgetKind::accessors, structural_count.value(),
                                               "gltf.document.structure");
            if (!structural.has_value()) {
                return Result<Document>::failure(structural.error_code(), structural.diagnostics());
            }
            auto nodes = context->observe(BudgetKind::gltf_nodes, array_size("nodes"),
                                          "gltf.document.nodes");
            if (!nodes.has_value()) {
                return Result<Document>::failure(nodes.error_code(), nodes.diagnostics());
            }
        }

        model_checkpoint("document_model", 0, 1);
        Document out(std::move(json_memory));
        std::vector<std::uint64_t> all_scene_roots;
        FeatureScanControl feature_control{context, &control_error, &control_diagnostics};
        ExtensionOccurrenceScan extension_occurrences;
        scan_extension_occurrences(doc, "root", extension_occurrences, feature_control);
        if (extension_occurrences.invalid_container.has_value()) {
            return fail("MK2132_GLTF_BAD_FIELD",
                        extension_occurrences.invalid_container.value() + " is not an object");
        }
        summarize_source_features(doc, out.source_features, feature_control);
        model_checkpoint("document_model", 1, 1);

        // glTF 2.0 requires an asset object with the exact major and minor version.
        auto asset = doc.find("asset");
        if (asset == doc.end() || !asset->is_object()) {
            return fail("MK2138_GLTF_ASSET", "glTF is missing the required asset object");
        }
        auto version = asset->find("version");
        if (version == asset->end() || !version->is_string() ||
            version->get<std::string>() != "2.0") {
            return fail("MK2138_GLTF_ASSET", "asset.version must be the string '2.0'");
        }
        if (auto minimum = asset->find("minVersion"); minimum != asset->end()) {
            if (!minimum->is_string()) {
                return fail("MK2138_GLTF_ASSET", "asset.minVersion must be a string");
            }
            const MinimumVersionStatus status =
                minimum_version_status(minimum->get_ref<const std::string&>());
            if (status == MinimumVersionStatus::malformed) {
                return fail("MK2138_GLTF_ASSET",
                            "asset.minVersion must use the major.minor numeric form");
            }
            if (status == MinimumVersionStatus::unsupported) {
                return unsupported_failure("MK2134_GLTF_UNSUPPORTED",
                                           "asset.minVersion requires an unsupported glTF version");
            }
        }
        if (auto generator = asset->find("generator");
            generator != asset->end() && !generator->is_string()) {
            return fail("MK2138_GLTF_ASSET", "asset.generator must be a string");
        }
        if (auto copyright = asset->find("copyright");
            copyright != asset->end() && !copyright->is_string()) {
            return fail("MK2138_GLTF_ASSET", "asset.copyright must be a string");
        }

        // ---- accessors ----
        if (auto it = doc.find("accessors"); it != doc.end()) {
            if (!it->is_array() || it->empty())
                return fail("MK2132_GLTF_BAD_FIELD", "'accessors' must be a nonempty array");
            for (std::size_t accessor_index = 0; accessor_index < it->size(); ++accessor_index) {
                model_checkpoint("accessors", accessor_index, it->size());
                const auto& a = (*it)[accessor_index];
                if (!a.is_object())
                    return fail("MK2132_GLTF_BAD_FIELD", "an accessor is not an object");
                AccessorDesc desc;
                std::uint64_t bv = 0;
                bool bv_present = false;
                if (!opt_index(a, "bufferView", bv, bv_present)) {
                    return fail("MK2132_GLTF_BAD_FIELD",
                                "accessor.bufferView is not a valid index");
                }
                desc.has_buffer_view = bv_present;
                desc.buffer_view = bv;

                std::uint64_t off = 0;
                bool off_present = false;
                if (!opt_index(a, "byteOffset", off, off_present)) {
                    return fail("MK2132_GLTF_BAD_FIELD",
                                "accessor.byteOffset is not a valid index");
                }
                desc.byte_offset = off;
                if (!desc.has_buffer_view && off_present) {
                    return fail("MK2132_GLTF_BAD_FIELD",
                                "accessor.byteOffset requires accessor.bufferView");
                }

                auto ct = a.find("componentType");
                std::uint64_t ct_val = 0;
                if (ct == a.end() || !as_index(*ct, ct_val)) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "accessor is missing a valid componentType");
                }
                if (ct_val > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                    return fail("MK2134_GLTF_UNSUPPORTED",
                                "unsupported accessor componentType " + std::to_string(ct_val));
                }
                auto comp = component_type_from_int(static_cast<int>(ct_val));
                if (!comp.has_value()) {
                    return fail("MK2134_GLTF_UNSUPPORTED",
                                "unsupported accessor componentType " + std::to_string(ct_val));
                }
                desc.component = comp.value();

                auto ty = a.find("type");
                if (ty == a.end() || !ty->is_string()) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "accessor is missing a valid type");
                }
                auto elem = element_type_from_string(ty->get<std::string>());
                if (!elem.has_value()) {
                    return fail("MK2134_GLTF_UNSUPPORTED",
                                "unsupported accessor type '" + ty->get<std::string>() + "'");
                }
                desc.element = elem.value();

                auto cnt = a.find("count");
                std::uint64_t cnt_val = 0;
                if (cnt == a.end() || !as_index(*cnt, cnt_val)) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "accessor is missing a valid count");
                }
                if (cnt_val == 0) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "accessor.count must be greater than zero");
                }
                desc.count = cnt_val;

                if (auto n = a.find("normalized"); n != a.end()) {
                    if (!n->is_boolean()) {
                        return fail("MK2132_GLTF_BAD_FIELD",
                                    "accessor.normalized is not a boolean");
                    }
                    desc.normalized = n->get<bool>();
                }
                if (desc.normalized && (desc.component == ComponentType::f32 ||
                                        desc.component == ComponentType::u32)) {
                    return fail("MK2132_GLTF_BAD_FIELD",
                                "accessor.normalized is not valid for this componentType");
                }
                desc.is_sparse = a.find("sparse") != a.end();

                const std::size_t bound_components = document_component_count(desc.element);
                for (const char* key : {"min", "max"}) {
                    auto bound = a.find(key);
                    if (bound == a.end())
                        continue;
                    if (bound_components == 0 || !bound->is_array() ||
                        bound->size() != bound_components) {
                        return fail("MK2132_GLTF_BAD_FIELD", std::string("accessor.") + key +
                                                                 " has the wrong element count");
                    }
                    for (const json& value : *bound) {
                        double number = 0.0;
                        if (!as_double(value, number) || !std::isfinite(number)) {
                            return fail("MK2132_GLTF_BAD_FIELD",
                                        std::string("accessor.") + key +
                                            " contains a non-finite number");
                        }
                        if (std::string_view(key) == "min")
                            desc.minimum.push_back(number);
                        else
                            desc.maximum.push_back(number);
                    }
                }
                if (!desc.minimum.empty() && !desc.maximum.empty()) {
                    for (std::size_t i = 0; i < desc.minimum.size(); ++i) {
                        if (desc.minimum[i] > desc.maximum[i]) {
                            return fail("MK2132_GLTF_BAD_FIELD",
                                        "accessor minimum exceeds its maximum");
                        }
                    }
                }
                out.accessors.push_back(std::move(desc));
            }
        }

        // ---- bufferViews ----
        if (auto it = doc.find("bufferViews"); it != doc.end()) {
            if (!it->is_array() || it->empty())
                return fail("MK2132_GLTF_BAD_FIELD", "'bufferViews' must be a nonempty array");
            for (std::size_t view_index = 0; view_index < it->size(); ++view_index) {
                model_checkpoint("buffer_views", view_index, it->size());
                const auto& b = (*it)[view_index];
                if (!b.is_object())
                    return fail("MK2132_GLTF_BAD_FIELD", "a bufferView is not an object");
                BufferViewDesc desc;
                auto buf = b.find("buffer");
                if (buf == b.end() || !as_index(*buf, desc.buffer)) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "bufferView is missing a valid buffer index");
                }
                std::uint64_t v = 0;
                bool present = false;
                if (!opt_index(b, "byteOffset", v, present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "bufferView.byteOffset is invalid");
                }
                desc.byte_offset = present ? v : 0;
                auto len = b.find("byteLength");
                if (len == b.end() || !as_index(*len, desc.byte_length)) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "bufferView is missing a valid byteLength");
                }
                if (desc.byte_length == 0) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "bufferView.byteLength must be greater than zero");
                }
                if (!opt_index(b, "byteStride", v, present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "bufferView.byteStride is invalid");
                }
                desc.byte_stride = present ? v : 0;
                if (present &&
                    (desc.byte_stride < 4 || desc.byte_stride > 252 || desc.byte_stride % 4 != 0)) {
                    return fail("MK2132_GLTF_BAD_FIELD",
                                "bufferView.byteStride must be a multiple of 4 from 4 through 252");
                }
                if (!opt_index(b, "target", v, present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "bufferView.target is invalid");
                }
                if (present) {
                    if (v == static_cast<std::uint64_t>(BufferViewTarget::array_buffer)) {
                        desc.target = BufferViewTarget::array_buffer;
                    } else if (v ==
                               static_cast<std::uint64_t>(BufferViewTarget::element_array_buffer)) {
                        desc.target = BufferViewTarget::element_array_buffer;
                    } else {
                        return fail("MK2132_GLTF_BAD_FIELD",
                                    "bufferView.target must be ARRAY_BUFFER or "
                                    "ELEMENT_ARRAY_BUFFER");
                    }
                }
                out.buffer_views.push_back(desc);
            }
        }

        // ---- buffers ----
        if (auto it = doc.find("buffers"); it != doc.end()) {
            if (!it->is_array() || it->empty())
                return fail("MK2132_GLTF_BAD_FIELD", "'buffers' must be a nonempty array");
            for (std::size_t buffer_index = 0; buffer_index < it->size(); ++buffer_index) {
                model_checkpoint("buffers", buffer_index, it->size());
                const auto& b = (*it)[buffer_index];
                if (!b.is_object())
                    return fail("MK2132_GLTF_BAD_FIELD", "a buffer is not an object");
                BufferDesc desc;
                auto len = b.find("byteLength");
                if (len == b.end() || !as_index(*len, desc.byte_length)) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "buffer is missing a valid byteLength");
                }
                if (desc.byte_length == 0) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "buffer.byteLength must be greater than zero");
                }
                if (auto u = b.find("uri"); u != b.end()) {
                    if (!u->is_string())
                        return fail("MK2132_GLTF_BAD_FIELD", "buffer.uri is not a string");
                    desc.uri = u->get<std::string>();
                }
                out.buffers.push_back(std::move(desc));
            }
        }

        // ---- meshes and their primitives ----
        if (auto it = doc.find("meshes"); it != doc.end()) {
            if (!it->is_array() || it->empty())
                return fail("MK2132_GLTF_BAD_FIELD", "'meshes' must be a nonempty array");
            for (std::size_t mesh_index = 0; mesh_index < it->size(); ++mesh_index) {
                model_checkpoint("meshes", mesh_index, it->size());
                const auto& m = (*it)[mesh_index];
                if (!m.is_object())
                    return fail("MK2132_GLTF_BAD_FIELD", "a mesh is not an object");
                MeshDesc mesh;
                auto prims = m.find("primitives");
                if (prims == m.end() || !prims->is_array() || prims->empty()) {
                    return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                "mesh needs a nonempty 'primitives' array");
                }
                for (const auto& p : *prims) {
                    if (!p.is_object())
                        return fail("MK2132_GLTF_BAD_FIELD", "a primitive is not an object");
                    PrimitiveDesc prim;
                    if (auto mode = p.find("mode"); mode != p.end()) {
                        std::uint64_t mode_val = 0;
                        if (!as_index(*mode, mode_val) || mode_val > 6) {
                            return fail("MK2132_GLTF_BAD_FIELD",
                                        "primitive.mode is not a valid glTF primitive mode (0-6)");
                        }
                        prim.mode = static_cast<int>(mode_val);
                    }
                    auto attrs = p.find("attributes");
                    if (attrs == p.end() || !attrs->is_object() || attrs->empty()) {
                        return fail("MK2133_GLTF_ACCESSOR_INCOMPLETE",
                                    "primitive needs a nonempty 'attributes' object");
                    }
                    for (auto a = attrs->begin(); a != attrs->end(); ++a) {
                        std::uint64_t acc = 0;
                        if (!as_index(a.value(), acc)) {
                            return fail("MK2132_GLTF_BAD_FIELD", "primitive attribute '" + a.key() +
                                                                     "' is not a valid index");
                        }
                        prim.attributes.emplace(a.key(), acc);
                    }
                    // KHR_gaussian_splatting extension, if present.
                    if (auto ext = p.find("extensions"); ext != p.end()) {
                        if (!ext->is_object()) {
                            return fail("MK2132_GLTF_BAD_FIELD",
                                        "primitive.extensions is not an object");
                        }
                        if (auto g = ext->find("KHR_gaussian_splatting"); g != ext->end()) {
                            if (!g->is_object()) {
                                return fail("MK2135_GLTF_KHR_INCOMPLETE",
                                            "KHR_gaussian_splatting is not an object");
                            }
                            GaussianExt ge;
                            auto k = g->find("kernel");
                            auto cs = g->find("colorSpace");
                            if (k == g->end() || !k->is_string() || cs == g->end() ||
                                !cs->is_string()) {
                                return fail("MK2135_GLTF_KHR_INCOMPLETE",
                                            "KHR_gaussian_splatting requires string 'kernel' and "
                                            "'colorSpace'");
                            }
                            ge.kernel = k->get<std::string>();
                            ge.color_space = cs->get<std::string>();
                            if (auto pr = g->find("projection"); pr != g->end()) {
                                if (!pr->is_string()) {
                                    return fail("MK2135_GLTF_KHR_INCOMPLETE",
                                                "KHR_gaussian_splatting.projection must be a "
                                                "string");
                                }
                                ge.projection = pr->get<std::string>();
                            }
                            if (auto sm = g->find("sortingMethod"); sm != g->end()) {
                                if (!sm->is_string()) {
                                    return fail("MK2135_GLTF_KHR_INCOMPLETE",
                                                "KHR_gaussian_splatting.sortingMethod must be a "
                                                "string");
                                }
                                ge.sorting_method = sm->get<std::string>();
                            }
                            prim.gaussian = std::move(ge);
                        }
                    }
                    mesh.primitives.push_back(std::move(prim));
                }
                out.meshes.push_back(std::move(mesh));
            }
        }

        // ---- nodes ----
        if (auto it = doc.find("nodes"); it != doc.end()) {
            if (!it->is_array() || it->empty())
                return fail("MK2132_GLTF_BAD_FIELD", "'nodes' must be a nonempty array");
            for (std::size_t node_index = 0; node_index < it->size(); ++node_index) {
                model_checkpoint("nodes", node_index, it->size());
                const auto& n = (*it)[node_index];
                if (!n.is_object())
                    return fail("MK2132_GLTF_BAD_FIELD", "a node is not an object");
                NodeDesc node;
                bool matrix_present = false;
                bool translation_present = false;
                bool rotation_present = false;
                bool scale_present = false;
                std::array<double, 16> matrix{};
                if (!read_number_array(n, "matrix", matrix, matrix_present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "node.matrix is not 16 numbers");
                }
                if (matrix_present)
                    node.matrix = matrix;
                if (!read_number_array(n, "translation", node.translation, translation_present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "node.translation is not 3 numbers");
                }
                if (!read_number_array(n, "rotation", node.rotation, rotation_present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "node.rotation is not 4 numbers");
                }
                if (!read_number_array(n, "scale", node.scale, scale_present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "node.scale is not 3 numbers");
                }
                if (matrix_present && (translation_present || rotation_present || scale_present)) {
                    return fail("MK2140_GLTF_NODE_TRANSFORM",
                                "a node cannot define matrix together with translation, rotation, "
                                "or scale");
                }
                if (matrix_present) {
                    for (double value : matrix) {
                        if (!std::isfinite(value)) {
                            return fail("MK2140_GLTF_NODE_TRANSFORM",
                                        "node.matrix contains a non-finite value");
                        }
                    }
                    if (matrix[3] != 0.0 || matrix[7] != 0.0 || matrix[11] != 0.0 ||
                        matrix[15] != 1.0) {
                        return fail("MK2140_GLTF_NODE_TRANSFORM",
                                    "node.matrix must be an affine transform");
                    }
                    if (!matrix_decomposes_to_trs(matrix)) {
                        return fail("MK2140_GLTF_NODE_TRANSFORM",
                                    "node.matrix must decompose to translation, rotation, and "
                                    "scale without shear");
                    }
                }
                if (rotation_present) {
                    for (double value : node.rotation) {
                        if (!std::isfinite(value) || value < -1.0 || value > 1.0) {
                            return fail("MK2140_GLTF_NODE_TRANSFORM",
                                        "node.rotation contains a value outside [-1,1]");
                        }
                    }
                    constexpr double kQuaternionTolerance = 1e-4;
                    const double rotation_norm =
                        std::hypot(std::hypot(node.rotation[0], node.rotation[1]),
                                   std::hypot(node.rotation[2], node.rotation[3]));
                    if (rotation_norm <= 1e-10 ||
                        std::fabs(rotation_norm - 1.0) > kQuaternionTolerance) {
                        return fail("MK2140_GLTF_NODE_TRANSFORM",
                                    "node.rotation must be a unit quaternion");
                    }
                }
                for (double value : node.translation) {
                    if (!std::isfinite(value)) {
                        return fail("MK2140_GLTF_NODE_TRANSFORM",
                                    "node.translation contains a non-finite value");
                    }
                }
                for (double value : node.scale) {
                    if (!std::isfinite(value)) {
                        return fail("MK2140_GLTF_NODE_TRANSFORM",
                                    "node.scale contains a non-finite value");
                    }
                }
                std::uint64_t mesh_idx = 0;
                bool mesh_present = false;
                if (!opt_index(n, "mesh", mesh_idx, mesh_present)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "node.mesh is not a valid index");
                }
                if (mesh_present)
                    node.mesh = mesh_idx;
                if (auto ch = n.find("children"); ch != n.end()) {
                    if (!ch->is_array() || ch->empty()) {
                        return fail("MK2132_GLTF_BAD_FIELD",
                                    "node.children must be a nonempty array");
                    }
                    for (const auto& c : *ch) {
                        std::uint64_t ci = 0;
                        if (!as_index(c, ci)) {
                            return fail("MK2132_GLTF_BAD_FIELD",
                                        "node.children contains an invalid index");
                        }
                        node.children.push_back(ci);
                    }
                }
                out.nodes.push_back(std::move(node));
            }
        }

        // ---- default scene roots ----
        {
            std::uint64_t scene_index = 0;
            bool has_scene = false;
            if (auto s = doc.find("scene"); s != doc.end()) {
                if (!as_index(*s, scene_index)) {
                    return fail("MK2132_GLTF_BAD_FIELD", "'scene' is not a valid index");
                }
                has_scene = true;
                out.has_default_scene = true;
            }
            auto scenes = doc.find("scenes");
            if (scenes == doc.end()) {
                if (has_scene) {
                    return fail("MK2136_GLTF_BAD_INDEX",
                                "the default scene index has no scenes array");
                }
            } else {
                if (!scenes->is_array() || scenes->empty()) {
                    return fail("MK2132_GLTF_BAD_FIELD", "'scenes' must be a nonempty array");
                }
                out.scene_count = scenes->size();
                if (has_scene && scene_index >= scenes->size()) {
                    return fail("MK2136_GLTF_BAD_INDEX", "default scene index " +
                                                             std::to_string(scene_index) +
                                                             " is out of range");
                }
                for (std::size_t index = 0; index < scenes->size(); ++index) {
                    const auto& scene = (*scenes)[index];
                    if (!scene.is_object()) {
                        return fail("MK2132_GLTF_BAD_FIELD", "a scene is not an object");
                    }
                    if (auto nodes = scene.find("nodes"); nodes != scene.end()) {
                        if (!nodes->is_array() || nodes->empty()) {
                            return fail("MK2132_GLTF_BAD_FIELD",
                                        "scene.nodes must be a nonempty array");
                        }
                        std::set<std::uint64_t> roots;
                        for (const auto& root : *nodes) {
                            std::uint64_t root_index = 0;
                            if (!as_index(root, root_index) || root_index >= out.nodes.size()) {
                                return fail("MK2132_GLTF_BAD_FIELD",
                                            "scene.nodes contains an invalid node index");
                            }
                            if (!roots.insert(root_index).second) {
                                return fail("MK2146_GLTF_NODE_GRAPH",
                                            "a scene contains a duplicate root node");
                            }
                            all_scene_roots.push_back(root_index);
                            if (has_scene && index == scene_index)
                                out.scene_roots.push_back(root_index);
                        }
                    }
                }
            }
        }

        // ---- extension declarations ----
        if (auto it = doc.find("extensionsUsed"); it != doc.end()) {
            if (!read_string_array(*it, out.extensions_used)) {
                return fail("MK2132_GLTF_BAD_FIELD", "'extensionsUsed' is not an array of strings");
            }
        }
        if (auto it = doc.find("extensionsRequired"); it != doc.end()) {
            if (!read_string_array(*it, out.extensions_required)) {
                return fail("MK2132_GLTF_BAD_FIELD",
                            "'extensionsRequired' is not an array of strings");
            }
        }
        for (const std::string& required : out.extensions_required) {
            if (std::find(out.extensions_used.begin(), out.extensions_used.end(), required) ==
                out.extensions_used.end()) {
                return fail("MK2139_GLTF_EXTENSION_DECLARATION",
                            "required extension '" + required + "' is missing from extensionsUsed");
            }
        }
        for (const std::string& occurrence : extension_occurrences.names) {
            if (std::find(out.extensions_used.begin(), out.extensions_used.end(), occurrence) ==
                out.extensions_used.end()) {
                return fail("MK2139_GLTF_EXTENSION_DECLARATION",
                            "extension '" + occurrence +
                                "' is used but is missing from "
                                "extensionsUsed");
            }
        }

        // ---- reference-graph validation: every index the reader follows is in range ----
        const std::uint64_t n_accessors = out.accessors.size();
        const std::uint64_t n_buffer_views = out.buffer_views.size();
        const std::uint64_t n_buffers = out.buffers.size();
        const std::uint64_t n_meshes = out.meshes.size();
        const std::uint64_t n_nodes = out.nodes.size();

        // Each message names the offending element index and the observed vs available count, so a
        // caller can locate the fault in a large asset (per the diagnostic contract in error.hpp).
        for (std::size_t i = 0; i < out.accessors.size(); ++i) {
            model_checkpoint("validate_accessors", i, out.accessors.size());
            const auto& a = out.accessors[i];
            if (a.has_buffer_view && a.buffer_view >= n_buffer_views) {
                return fail("MK2136_GLTF_BAD_INDEX",
                            "accessor " + std::to_string(i) + " references bufferView " +
                                std::to_string(a.buffer_view) + " but there are only " +
                                std::to_string(n_buffer_views));
            }
        }
        for (std::size_t i = 0; i < out.buffer_views.size(); ++i) {
            model_checkpoint("validate_buffer_views", i, out.buffer_views.size());
            if (out.buffer_views[i].buffer >= n_buffers) {
                return fail("MK2136_GLTF_BAD_INDEX",
                            "bufferView " + std::to_string(i) + " references buffer " +
                                std::to_string(out.buffer_views[i].buffer) +
                                " but there are only " + std::to_string(n_buffers));
            }
            const BufferDesc& buffer =
                out.buffers[static_cast<std::size_t>(out.buffer_views[i].buffer)];
            const BufferViewDesc& view = out.buffer_views[i];
            if (view.byte_offset > buffer.byte_length ||
                view.byte_length > buffer.byte_length - view.byte_offset) {
                return fail("MK2137_GLTF_RANGE", "bufferView " + std::to_string(i) +
                                                     " extends past its declared buffer length");
            }
        }
        for (std::size_t mi = 0; mi < out.meshes.size(); ++mi) {
            model_checkpoint("validate_meshes", mi, out.meshes.size());
            const auto& m = out.meshes[mi];
            for (std::size_t pi = 0; pi < m.primitives.size(); ++pi) {
                std::map<std::uint64_t, std::uint64_t> first_accessor;
                std::optional<std::uint64_t> vertex_count;
                for (const auto& [semantic, acc] : m.primitives[pi].attributes) {
                    if (acc >= n_accessors) {
                        return fail("MK2136_GLTF_BAD_INDEX",
                                    "mesh " + std::to_string(mi) + " primitive " +
                                        std::to_string(pi) + " attribute '" + semantic +
                                        "' references accessor " + std::to_string(acc) +
                                        " but there are only " + std::to_string(n_accessors));
                    }
                    const AccessorDesc& accessor = out.accessors[static_cast<std::size_t>(acc)];
                    if (!vertex_count.has_value()) {
                        vertex_count = accessor.count;
                    } else if (vertex_count.value() != accessor.count) {
                        return fail("MK2132_GLTF_BAD_FIELD",
                                    "all vertex attributes in a primitive need the same count");
                    }
                    if (accessor.has_buffer_view) {
                        const BufferViewDesc& view =
                            out.buffer_views[static_cast<std::size_t>(accessor.buffer_view)];
                        if (view.target.has_value() &&
                            view.target.value() != BufferViewTarget::array_buffer) {
                            return fail("MK2132_GLTF_BAD_FIELD",
                                        "a vertex attribute cannot use an ELEMENT_ARRAY_BUFFER "
                                        "bufferView");
                        }
                        const auto [first, inserted] =
                            first_accessor.emplace(accessor.buffer_view, acc);
                        if (!inserted && first->second != acc && view.byte_stride == 0) {
                            return fail("MK2132_GLTF_BAD_FIELD",
                                        "a bufferView shared by vertex accessors requires "
                                        "byteStride");
                        }
                    }
                    if (semantic == "POSITION") {
                        const AccessorDesc& position = out.accessors[static_cast<std::size_t>(acc)];
                        if (position.element != ElementType::vec3 || position.minimum.size() != 3 ||
                            position.maximum.size() != 3) {
                            return fail("MK2145_GLTF_POSITION_BOUNDS",
                                        "a POSITION accessor needs VEC3 min and max bounds");
                        }
                    }
                }
            }
        }
        for (std::size_t i = 0; i < out.nodes.size(); ++i) {
            model_checkpoint("validate_nodes", i, out.nodes.size());
            const auto& node = out.nodes[i];
            if (node.mesh.has_value() && node.mesh.value() >= n_meshes) {
                return fail("MK2136_GLTF_BAD_INDEX",
                            "node " + std::to_string(i) + " references mesh " +
                                std::to_string(node.mesh.value()) + " but there are only " +
                                std::to_string(n_meshes));
            }
            for (std::uint64_t c : node.children) {
                if (c >= n_nodes) {
                    return fail("MK2136_GLTF_BAD_INDEX",
                                "node " + std::to_string(i) + " lists child " + std::to_string(c) +
                                    " but there are only " + std::to_string(n_nodes) + " nodes");
                }
            }
        }
        for (std::uint64_t r : out.scene_roots) {
            if (r >= n_nodes) {
                return fail("MK2136_GLTF_BAD_INDEX",
                            "scene root references node " + std::to_string(r) +
                                " but there are only " + std::to_string(n_nodes) + " nodes");
            }
        }

        // Validate the complete node graph, including nodes outside the default scene.
        std::vector<std::uint8_t> parent_count(out.nodes.size(), 0);
        for (std::size_t node_index = 0; node_index < out.nodes.size(); ++node_index) {
            model_checkpoint("validate_node_graph", node_index, out.nodes.size());
            for (std::uint64_t child : out.nodes[node_index].children) {
                std::uint8_t& count = parent_count[static_cast<std::size_t>(child)];
                if (count != 0) {
                    return fail("MK2146_GLTF_NODE_GRAPH", "a glTF node has more than one parent");
                }
                count = 1;
            }
        }
        for (std::uint64_t root : all_scene_roots) {
            if (parent_count[static_cast<std::size_t>(root)] != 0) {
                return fail("MK2146_GLTF_NODE_GRAPH", "a scene root node also has a parent");
            }
        }

        std::vector<std::size_t> queue;
        queue.reserve(out.nodes.size());
        for (std::size_t i = 0; i < parent_count.size(); ++i) {
            if (parent_count[i] == 0)
                queue.push_back(i);
        }
        std::size_t queue_read = 0;
        while (queue_read < queue.size()) {
            model_checkpoint("validate_node_graph", queue_read, out.nodes.size());
            const std::size_t node_index = queue[queue_read++];
            for (std::uint64_t child : out.nodes[node_index].children) {
                std::uint8_t& count = parent_count[static_cast<std::size_t>(child)];
                count = 0;
                queue.push_back(static_cast<std::size_t>(child));
            }
        }
        if (queue.size() != out.nodes.size()) {
            return fail("MK2146_GLTF_NODE_GRAPH", "the glTF node graph contains a cycle");
        }

        std::vector<std::uint8_t> root_seen(out.nodes.size(), 0);
        for (std::uint64_t root : out.scene_roots) {
            const std::size_t root_index = static_cast<std::size_t>(root);
            if (root_seen[root_index] != 0) {
                return fail("MK2146_GLTF_NODE_GRAPH",
                            "the default scene contains a duplicate root node");
            }
            root_seen[root_index] = 1;
        }

        return Result<Document>::success(std::move(out));
    } catch (const JsonControlStop&) {
        return Result<Document>::failure(control_error, std::move(control_diagnostics));
    } catch (const std::bad_alloc&) {
        return resource_failure("MK2149_GLTF_JSON_ALLOCATION",
                                "glTF JSON parsing exceeded available memory");
    } catch (const std::length_error&) {
        return resource_failure("MK2149_GLTF_JSON_ALLOCATION",
                                "glTF JSON parsing exceeded a container size limit");
    } catch (const std::exception& e) {
        return fail("MK2131_GLTF_BAD_JSON",
                    std::string("glTF JSON could not be parsed: ") + e.what());
    } catch (...) {
        return fail("MK2131_GLTF_BAD_JSON", "glTF JSON could not be parsed");
    }
}

Result<Document> parse_gltf_json(const std::uint8_t* data, std::size_t size) {
    return parse_gltf_json_impl(data, size, nullptr);
}

Result<Document> parse_gltf_json(const std::uint8_t* data, std::size_t size,
                                 const OperationContext& context) {
    if (context.budget == nullptr) {
        Diagnostic diagnostic("MK0310_NO_BUDGET", Severity::error,
                              "the glTF JSON parser requires a resource budget");
        return Result<Document>::failure(ErrorCode::internal_error, std::move(diagnostic));
    }
    if (auto valid = context.budget->limits().validate(); !valid.has_value())
        return Result<Document>::failure(valid.error_code(), valid.diagnostics());
    return parse_gltf_json_impl(data, size, &context);
}

}  // namespace melkor::format::gltf
