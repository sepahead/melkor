#include "melkor/provenance.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <type_traits>

namespace melkor {
namespace {

using json = nlohmann::json;

Diagnostic provenance_error(const char* code, const std::string& message) {
    return Diagnostic(code, Severity::error, message);
}

bool is_lower_hex_sha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
           });
}

bool looks_like_absolute_path(const std::string& value) {
    if (value.empty()) return false;
    if (value[0] == '/' || value[0] == '\\') return true;
    return value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
           value[1] == ':' && (value[2] == '/' || value[2] == '\\');
}

Result<void> validate_provenance(const Provenance& provenance) {
    if (provenance.source_format.empty()) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1520_PROVENANCE_SOURCE_FORMAT_MISSING",
                             "provenance source format must not be empty"));
    }
    if (provenance.source_profile.empty()) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1521_PROVENANCE_SOURCE_PROFILE_MISSING",
                             "provenance source profile must not be empty"));
    }
    if (provenance.source_sha256.has_value() &&
        !is_lower_hex_sha256(*provenance.source_sha256)) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1522_PROVENANCE_SHA256_INVALID",
                             "provenance SHA-256 must be 64 lowercase hexadecimal characters"));
    }
    for (std::size_t i = 0; i < provenance.operations.size(); ++i) {
        const ProvenanceOperation& operation = provenance.operations[i];
        if (operation.name.empty() || operation.tool_version.empty()) {
            Diagnostic d("MK1523_PROVENANCE_OPERATION_INVALID", Severity::error,
                         "provenance operation name and tool version must not be empty");
            d.with_context("operation_index", static_cast<std::uint64_t>(i));
            return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
        }
        for (const auto& [key, value] : operation.parameters) {
            if (key.empty()) {
                Diagnostic d("MK1526_PROVENANCE_PARAMETER_INVALID", Severity::error,
                             "provenance parameter names must not be empty");
                d.with_context("operation_index", static_cast<std::uint64_t>(i));
                return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
            }
            if (const auto* number = std::get_if<double>(&value);
                number != nullptr && !std::isfinite(*number)) {
                Diagnostic d("MK1526_PROVENANCE_PARAMETER_INVALID", Severity::error,
                             "provenance numeric parameters must be finite");
                d.with_context("operation_index", static_cast<std::uint64_t>(i));
                d.with_context("parameter", key);
                return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
            }
            if (const auto* text = std::get_if<std::string>(&value);
                text != nullptr && looks_like_absolute_path(*text)) {
                Diagnostic d("MK1527_PROVENANCE_ABSOLUTE_PATH", Severity::error,
                             "reproducible provenance must not contain an absolute path");
                d.with_context("operation_index", static_cast<std::uint64_t>(i));
                d.with_context("parameter", key);
                return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
            }
        }
    }
    return Result<void>::success();
}

json scalar_to_json(const JsonScalar& value) {
    return std::visit(
        [](const auto& item) -> json {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return nullptr;
            } else {
                return item;
            }
        },
        value);
}

}  // namespace

const char* to_string(QuaternionOrder value) noexcept {
    return value == QuaternionOrder::xyzw ? "xyzw" : "unknown";
}

const char* to_string(ScaleDomain value) noexcept {
    return value == ScaleDomain::linear ? "linear" : "unknown";
}

const char* to_string(OpacityDomain value) noexcept {
    return value == OpacityDomain::linear ? "linear" : "unknown";
}

const char* to_string(ColorSpace value) noexcept {
    return value == ColorSpace::linear_srgb_rec709 ? "linear_srgb_rec709" : "unknown";
}

const char* to_string(ShBasis value) noexcept {
    return value == ShBasis::real_condon_shortley ? "real_condon_shortley" : "unknown";
}

Result<SplatPrimitive> SplatPrimitive::create(SplatMetadata metadata, SplatData data,
                                               Provenance provenance) {
    SplatPrimitive primitive(std::move(metadata), std::move(data), std::move(provenance));
    auto valid = primitive.validate();
    if (!valid.has_value()) {
        return Result<SplatPrimitive>::failure(valid.error_code(), valid.diagnostics());
    }
    return Result<SplatPrimitive>::success(std::move(primitive));
}

Result<void> SplatPrimitive::validate() const {
    auto data_valid = data_.validate();
    if (!data_valid.has_value()) return data_valid;

    if (metadata_.sh_degree != data_.sh().degree()) {
        Diagnostic d("MK1524_METADATA_SH_DEGREE_MISMATCH", Severity::error,
                     "metadata SH degree does not match the canonical SH buffer");
        d.with_context("metadata_degree", static_cast<std::uint64_t>(metadata_.sh_degree));
        d.with_context("data_degree", static_cast<std::uint64_t>(data_.sh().degree()));
        return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
    }

    if (metadata_.quaternion_order != QuaternionOrder::xyzw ||
        metadata_.scale_domain != ScaleDomain::linear ||
        metadata_.opacity_domain != OpacityDomain::linear ||
        metadata_.color_space != ColorSpace::linear_srgb_rec709 ||
        metadata_.sh_basis != ShBasis::real_condon_shortley) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1529_METADATA_DOMAIN_INVALID",
                             "primitive metadata names a non-canonical storage convention"));
    }
    if (metadata_.frame.id.empty()) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1530_METADATA_FRAME_ID_MISSING",
                             "coordinate-frame identifier must not be empty"));
    }

    auto frame = math::frame_from_basis(metadata_.frame.id, metadata_.frame.to_canonical,
                                        metadata_.frame.unit_to_meter);
    if (!frame.has_value()) {
        return Result<void>::failure(frame.error_code(), frame.diagnostics());
    }
    if (frame.value().includes_reflection != metadata_.frame.includes_reflection) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1525_METADATA_FRAME_REFLECTION_MISMATCH",
                             "coordinate-frame reflection flag disagrees with its basis"));
    }

    return validate_provenance(provenance_);
}

Result<std::string> provenance_to_json(const Provenance& provenance, bool reproducible) {
    auto valid = validate_provenance(provenance);
    if (!valid.has_value()) {
        return Result<std::string>::failure(valid.error_code(), valid.diagnostics());
    }

    try {
        json document = {{"schema_version", 1},
                         {"model_schema", "melkor.scene/v1"},
                         {"source_format", provenance.source_format},
                         {"source_profile", provenance.source_profile},
                         {"source_sha256", provenance.source_sha256.has_value()
                                               ? json(*provenance.source_sha256)
                                               : json(nullptr)},
                         {"operations", json::array()}};

        for (const ProvenanceOperation& operation : provenance.operations) {
            json parameters = json::object();
            for (const auto& [key, value] : operation.parameters) {
                parameters[key] = scalar_to_json(value);
            }
            json item = {{"name", operation.name},
                         {"tool_version", operation.tool_version},
                         {"parameters", std::move(parameters)},
                         {"timestamp", !reproducible && operation.timestamp.has_value()
                                           ? json(*operation.timestamp)
                                           : json(nullptr)}};
            document["operations"].push_back(std::move(item));
        }
        return Result<std::string>::success(document.dump(2));
    } catch (const std::exception&) {
        return Result<std::string>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1528_PROVENANCE_JSON_ENCODING_FAILED",
                             "provenance contains a string that cannot be encoded as JSON"));
    }
}

}  // namespace melkor
