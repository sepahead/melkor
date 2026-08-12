#include "melkor/provenance.hpp"

#include "melkor/budget.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace melkor {
namespace {

using json = nlohmann::json;

Diagnostic provenance_error(const char* code, const std::string& message) {
    return Diagnostic(code, Severity::error, message);
}

constexpr bool is_ascii_digit(unsigned char value) noexcept {
    return value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9');
}

constexpr bool is_ascii_alpha(unsigned char value) noexcept {
    return (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) ||
           (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z'));
}

constexpr unsigned char ascii_lower(unsigned char value) noexcept {
    return value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')
               ? static_cast<unsigned char>(value + ('a' - 'A'))
               : value;
}

bool is_lower_hex_sha256(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return is_ascii_digit(c) || (c >= 'a' && c <= 'f');
           });
}

bool looks_like_absolute_path(const std::string& value) {
    if (value.empty())
        return false;
    if (value.size() >= 5) {
        const bool file_scheme = ascii_lower(static_cast<unsigned char>(value[0])) == 'f' &&
                                 ascii_lower(static_cast<unsigned char>(value[1])) == 'i' &&
                                 ascii_lower(static_cast<unsigned char>(value[2])) == 'l' &&
                                 ascii_lower(static_cast<unsigned char>(value[3])) == 'e' &&
                                 value[4] == ':';
        if (file_scheme)
            return true;
    }
    if (value.size() >= 2 && value[0] == '~' && (value[1] == '/' || value[1] == '\\')) {
        return true;
    }
    if (value[0] == '/' || value[0] == '\\')
        return true;
    return value.size() >= 2 && is_ascii_alpha(static_cast<unsigned char>(value[0])) &&
           value[1] == ':';
}

bool parse_decimal(const std::string& value, std::size_t offset, std::size_t length,
                   unsigned& result) {
    if (offset > value.size() || length > value.size() - offset) {
        return false;
    }
    result = 0;
    for (std::size_t index = offset; index < offset + length; ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (!is_ascii_digit(character)) {
            return false;
        }
        result = result * 10U + static_cast<unsigned>(character - static_cast<unsigned char>('0'));
    }
    return true;
}

bool is_leap_year(unsigned year) noexcept {
    return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

bool is_rfc3339_timestamp(const std::string& value) {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != 't') || value[13] != ':' || value[16] != ':') {
        return false;
    }

    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    if (!parse_decimal(value, 0, 4, year) || !parse_decimal(value, 5, 2, month) ||
        !parse_decimal(value, 8, 2, day) || !parse_decimal(value, 11, 2, hour) ||
        !parse_decimal(value, 14, 2, minute) || !parse_decimal(value, 17, 2, second)) {
        return false;
    }

    static constexpr unsigned kDaysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 || second > 60) {
        return false;
    }
    unsigned days = kDaysPerMonth[month - 1];
    if (month == 2 && is_leap_year(year)) {
        ++days;
    }
    if (day == 0 || day > days) {
        return false;
    }

    std::size_t offset = 19;
    if (offset < value.size() && value[offset] == '.') {
        ++offset;
        const std::size_t fraction_start = offset;
        while (offset < value.size() && is_ascii_digit(static_cast<unsigned char>(value[offset]))) {
            ++offset;
        }
        if (offset == fraction_start) {
            return false;
        }
    }

    if (offset + 1 == value.size() && (value[offset] == 'Z' || value[offset] == 'z')) {
        return true;
    }
    if (offset + 6 != value.size() || (value[offset] != '+' && value[offset] != '-') ||
        value[offset + 3] != ':') {
        return false;
    }
    unsigned zone_hour = 0;
    unsigned zone_minute = 0;
    return parse_decimal(value, offset + 1, 2, zone_hour) &&
           parse_decimal(value, offset + 4, 2, zone_minute) && zone_hour <= 23 && zone_minute <= 59;
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
    if (provenance.source_sha256.has_value() && !is_lower_hex_sha256(*provenance.source_sha256)) {
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
        if (operation.timestamp.has_value() && !is_rfc3339_timestamp(*operation.timestamp)) {
            Diagnostic d("MK1532_PROVENANCE_TIMESTAMP_INVALID", Severity::error,
                         "provenance timestamps must use RFC 3339 date-time syntax");
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

Result<void> validate_primitive_metadata(const SplatMetadata& metadata, const SplatData& data,
                                         const Provenance& provenance) {
    if (metadata.sh_degree != data.sh().degree()) {
        Diagnostic diagnostic("MK1524_METADATA_SH_DEGREE_MISMATCH", Severity::error,
                              "metadata SH degree does not match the canonical SH buffer");
        diagnostic.with_context("metadata_degree", static_cast<std::uint64_t>(metadata.sh_degree));
        diagnostic.with_context("data_degree", static_cast<std::uint64_t>(data.sh().degree()));
        return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }

    if (metadata.quaternion_order != QuaternionOrder::xyzw ||
        metadata.scale_domain != ScaleDomain::linear ||
        metadata.opacity_domain != OpacityDomain::linear || !is_valid(metadata.color_space) ||
        metadata.sh_basis != ShBasis::real_condon_shortley) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1529_METADATA_DOMAIN_INVALID",
                             "primitive metadata names a non-canonical storage convention"));
    }
    if (metadata.frame.id.empty()) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1530_METADATA_FRAME_ID_MISSING",
                             "coordinate-frame identifier must not be empty"));
    }

    auto frame = math::frame_from_basis(metadata.frame.id, metadata.frame.to_canonical,
                                        metadata.frame.unit_to_meter);
    if (!frame.has_value()) {
        return Result<void>::failure(frame.error_code(), frame.diagnostics());
    }
    if (frame.value().includes_reflection != metadata.frame.includes_reflection) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1525_METADATA_FRAME_REFLECTION_MISMATCH",
                             "coordinate-frame reflection flag disagrees with its basis"));
    }
    const math::CoordinateFrame canonical = math::canonical_frame();
    if (metadata.frame.id != canonical.id || metadata.frame.unit_to_meter != 1.0 ||
        metadata.frame.includes_reflection ||
        metadata.frame.to_canonical != canonical.to_canonical) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1531_METADATA_FRAME_NOT_CANONICAL",
                             "primitive metadata must name the canonical storage frame"));
    }

    return validate_provenance(provenance);
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

const char* to_string(ShBasis value) noexcept {
    return value == ShBasis::real_condon_shortley ? "real_condon_shortley" : "unknown";
}

Result<SplatPrimitive> SplatPrimitive::create(SplatMetadata metadata, SplatData data,
                                              Provenance provenance) {
    SplatPrimitive primitive(std::move(metadata), std::move(data), std::move(provenance));
    auto valid =
        validate_primitive_metadata(primitive.metadata_, primitive.data_, primitive.provenance_);
    if (!valid.has_value()) {
        return Result<SplatPrimitive>::failure(valid.error_code(), valid.diagnostics());
    }
    return Result<SplatPrimitive>::success(std::move(primitive));
}

Result<SplatPrimitive> SplatPrimitive::create(SplatMetadata metadata, SplatData data,
                                              Provenance provenance,
                                              const OperationContext& context) {
    SplatPrimitive primitive(std::move(metadata), std::move(data), std::move(provenance));
    auto control = context.check("primitive.create");
    if (!control.has_value()) {
        return Result<SplatPrimitive>::failure(control.error_code(), control.diagnostics());
    }
    auto valid =
        validate_primitive_metadata(primitive.metadata_, primitive.data_, primitive.provenance_);
    if (!valid.has_value()) {
        return Result<SplatPrimitive>::failure(valid.error_code(), valid.diagnostics());
    }
    return Result<SplatPrimitive>::success(std::move(primitive));
}

Result<void> SplatPrimitive::validate_impl(const OperationContext* context) const {
    auto data_valid = context == nullptr ? data_.validate() : data_.validate(*context);
    if (!data_valid.has_value())
        return data_valid;

    return validate_primitive_metadata(metadata_, data_, provenance_);
}

Result<void> SplatPrimitive::validate() const {
    return validate_impl(nullptr);
}

Result<void> SplatPrimitive::validate(const OperationContext& context) const {
    return validate_impl(&context);
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
            json timestamp = nullptr;
            if (!reproducible && operation.timestamp.has_value()) {
                // Validation above guarantees that an engaged value is a valid timestamp.
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                timestamp = *operation.timestamp;
            }
            json item = {{"name", operation.name},
                         {"tool_version", operation.tool_version},
                         {"parameters", std::move(parameters)},
                         {"timestamp", std::move(timestamp)}};
            document["operations"].push_back(std::move(item));
        }
        return Result<std::string>::success(document.dump(2));
    } catch (const std::bad_alloc&) {
        return Result<std::string>::failure(
            ErrorCode::resource_limit,
            provenance_error("MK1528_PROVENANCE_JSON_ENCODING_FAILED",
                             "provenance JSON allocation exceeded available memory"));
    } catch (const std::length_error&) {
        return Result<std::string>::failure(
            ErrorCode::resource_limit,
            provenance_error("MK1528_PROVENANCE_JSON_ENCODING_FAILED",
                             "provenance JSON exceeded a container size limit"));
    } catch (const nlohmann::json::exception&) {
        return Result<std::string>::failure(
            ErrorCode::invalid_data,
            provenance_error("MK1528_PROVENANCE_JSON_ENCODING_FAILED",
                             "provenance contains a string that cannot be encoded as JSON"));
    } catch (const std::exception&) {
        return Result<std::string>::failure(
            ErrorCode::internal_error, provenance_error("MK1528_PROVENANCE_JSON_ENCODING_FAILED",
                                                        "provenance JSON encoding failed"));
    }
}

}  // namespace melkor
