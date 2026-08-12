#include "melkor/format/loss.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace melkor {

namespace {

struct LossCodeDefinition {
    std::string_view code;
    LossSeverity minimum_severity;
};

constexpr std::array<LossCodeDefinition, kKnownLossCodeCount> kLossCodeRegistry = {{
    {loss_code::kShDegreeTruncated, LossSeverity::severe},
    {loss_code::kShCoefficientsDropped, LossSeverity::severe},
    {loss_code::kShCoefficientsClamped, LossSeverity::severe},
    {loss_code::kShRotationNotApplied, LossSeverity::severe},
    {loss_code::kSceneGraphFlattened, LossSeverity::info},
    {loss_code::kNodeNameDropped, LossSeverity::info},
    {loss_code::kInstanceExpanded, LossSeverity::info},
    {loss_code::kMaterialApproximated, LossSeverity::severe},
    {loss_code::kTextureBaked, LossSeverity::severe},
    {loss_code::kAntialiasingMetadataDropped, LossSeverity::severe},
    {loss_code::kColorSpaceAssumed, LossSeverity::severe},
    {loss_code::kColorSpacesConflict, LossSeverity::severe},
    {loss_code::kColorClamped, LossSeverity::warning},
    {loss_code::kColorSpaceMetadataDropped, LossSeverity::severe},
    {loss_code::kCoordinateMetadataDropped, LossSeverity::severe},
    {loss_code::kProvenanceDropped, LossSeverity::info},
    {loss_code::kAttributionDropped, LossSeverity::severe},
    {loss_code::kQuantizationApplied, LossSeverity::warning},
    {loss_code::kOpacityClamped, LossSeverity::warning},
    {loss_code::kScaleClamped, LossSeverity::severe},
    {loss_code::kNonfiniteRepaired, LossSeverity::severe},
    {loss_code::kInvalidSplatDropped, LossSeverity::severe},
    {loss_code::kUnknownPropertyDropped, LossSeverity::severe},
    {loss_code::kMetadataDropped, LossSeverity::info},
    {loss_code::kVertexNormalsDropped, LossSeverity::severe},
    {loss_code::kQuaternionNormalized, LossSeverity::info},
    {loss_code::kExtensionDropped, LossSeverity::severe},
    {loss_code::kExtensionDeclarationDropped, LossSeverity::info},
    {loss_code::kGltfContentDropped, LossSeverity::severe},
    {loss_code::kPrecisionReduced, LossSeverity::warning},
    {loss_code::kGaussianAttributesGenerated, LossSeverity::severe},
}};

const LossCodeDefinition* find_loss_code(std::string_view code) noexcept {
    const auto found = std::find_if(
        kLossCodeRegistry.begin(), kLossCodeRegistry.end(),
        [code](const LossCodeDefinition& definition) { return definition.code == code; });
    return found == kLossCodeRegistry.end() ? nullptr : &*found;
}

Result<void> validate_item(const LossItem& item, std::size_t index) {
    const bool valid_severity =
        item.severity == LossSeverity::info || item.severity == LossSeverity::warning ||
        item.severity == LossSeverity::severe || item.severity == LossSeverity::fatal;
    const LossCodeDefinition* definition = find_loss_code(item.code);
    if (definition == nullptr || !valid_severity || item.source_feature.empty() ||
        item.target_constraint.empty() || item.remediation.empty()) {
        Diagnostic diagnostic("MK1603_INVALID_LOSS_REPORT", Severity::error,
                              "the conversion produced an invalid loss report item");
        diagnostic.with_context("item_index", static_cast<std::uint64_t>(index));
        diagnostic.with_context("loss_code", item.code);
        return Result<void>::failure(ErrorCode::internal_error, std::move(diagnostic));
    }
    if (static_cast<std::uint8_t>(item.severity) <
        static_cast<std::uint8_t>(definition->minimum_severity)) {
        Diagnostic diagnostic("MK1603_INVALID_LOSS_REPORT", Severity::error,
                              "the loss severity is lower than its stable code permits");
        diagnostic.with_context("item_index", static_cast<std::uint64_t>(index));
        diagnostic.with_context("loss_code", item.code);
        diagnostic.with_context("severity", std::string(to_string(item.severity)));
        diagnostic.with_context("minimum_severity",
                                std::string(to_string(definition->minimum_severity)));
        return Result<void>::failure(ErrorCode::internal_error, std::move(diagnostic));
    }
    return Result<void>::success();
}

}  // namespace

const char* to_string(LossSeverity severity) noexcept {
    switch (severity) {
    case LossSeverity::info:
        return "info";
    case LossSeverity::warning:
        return "warning";
    case LossSeverity::severe:
        return "severe";
    case LossSeverity::fatal:
        return "fatal";
    }
    return "unknown";
}

Result<void> LossReport::add(LossItem item) try {
    if (auto valid = validate_item(item, items_.size()); !valid.has_value())
        return valid;
    items_.push_back(std::move(item));
    return Result<void>::success();
} catch (const std::bad_alloc&) {
    return Result<void>::failure(ErrorCode::resource_limit,
                                 Diagnostic("MK1605_LOSS_REPORT_MEMORY", Severity::error,
                                            "the loss report exceeded available memory"));
} catch (const std::length_error&) {
    return Result<void>::failure(ErrorCode::resource_limit,
                                 Diagnostic("MK1606_LOSS_REPORT_SIZE", Severity::error,
                                            "the loss report exceeded a container size limit"));
}

Result<void> LossReport::append(const LossReport& other) try {
    if (auto valid = other.validate(); !valid.has_value())
        return valid;
    std::vector<LossItem> combined = items_;
    combined.insert(combined.end(), other.items_.begin(), other.items_.end());
    items_.swap(combined);
    return Result<void>::success();
} catch (const std::bad_alloc&) {
    return Result<void>::failure(ErrorCode::resource_limit,
                                 Diagnostic("MK1605_LOSS_REPORT_MEMORY", Severity::error,
                                            "the loss report exceeded available memory"));
} catch (const std::length_error&) {
    return Result<void>::failure(ErrorCode::resource_limit,
                                 Diagnostic("MK1606_LOSS_REPORT_SIZE", Severity::error,
                                            "the loss report exceeded a container size limit"));
}

const std::array<std::string_view, kKnownLossCodeCount>& known_loss_codes() noexcept {
    static constexpr std::array<std::string_view, kKnownLossCodeCount> kKnownCodes = [] {
        std::array<std::string_view, kKnownLossCodeCount> codes{};
        for (std::size_t index = 0; index < codes.size(); ++index)
            codes[index] = kLossCodeRegistry[index].code;
        return codes;
    }();
    return kKnownCodes;
}

bool is_known_loss_code(std::string_view code) noexcept {
    return find_loss_code(code) != nullptr;
}

bool LossReport::has_blocking() const noexcept {
    return std::any_of(items_.begin(), items_.end(), [](const LossItem& item) {
        switch (item.severity) {
        case LossSeverity::info:
        case LossSeverity::warning:
            return false;
        case LossSeverity::severe:
        case LossSeverity::fatal:
            return true;
        }
        return true;
    });
}

Result<void> LossReport::validate() const {
    for (std::size_t index = 0; index < items_.size(); ++index) {
        if (auto valid = validate_item(items_[index], index); !valid.has_value())
            return valid;
    }
    return Result<void>::success();
}

Result<void> LossReport::check_policy(const std::vector<std::string>& approved_codes) const {
    if (auto valid = validate(); !valid.has_value()) {
        return valid;
    }

    if (approved_codes.size() > known_loss_codes().size()) {
        Diagnostic diagnostic("MK1604_INVALID_LOSS_APPROVAL", Severity::error,
                              "the loss approval list contains too many entries");
        diagnostic.with_context("approval_count",
                                static_cast<std::uint64_t>(approved_codes.size()));
        diagnostic.with_context("maximum_count",
                                static_cast<std::uint64_t>(known_loss_codes().size()));
        return Result<void>::failure(ErrorCode::invalid_argument, std::move(diagnostic));
    }

    for (std::size_t index = 0; index < approved_codes.size(); ++index) {
        const std::string& code = approved_codes[index];
        const bool duplicate =
            std::find(approved_codes.begin(),
                      approved_codes.begin() + static_cast<std::ptrdiff_t>(index),
                      code) != approved_codes.begin() + static_cast<std::ptrdiff_t>(index);
        if (!is_known_loss_code(code) || duplicate) {
            Diagnostic diagnostic("MK1604_INVALID_LOSS_APPROVAL", Severity::error,
                                  "the loss approval is unknown or duplicated");
            diagnostic.with_context("loss_code", code);
            return Result<void>::failure(ErrorCode::invalid_argument, std::move(diagnostic));
        }
    }

    for (const LossItem& item : items_) {
        if (item.severity == LossSeverity::fatal) {
            // A fatal loss means the target cannot represent the asset without breaking an
            // invariant. It is never approvable; the conversion simply cannot be done into this
            // target.
            Diagnostic d("MK1601_FATAL_LOSS", Severity::error,
                         "the target format cannot represent this asset without a fatal loss");
            d.with_context("loss_code", item.code);
            d.with_context("source_feature", item.source_feature);
            d.with_context("target_constraint", item.target_constraint);
            d.with_context("affected_splats", item.affected_splats);
            return Result<void>::failure(ErrorCode::unsupported_feature, std::move(d));
        }

        if (item.severity == LossSeverity::severe) {
            const bool approved = std::find(approved_codes.begin(), approved_codes.end(),
                                            item.code) != approved_codes.end();
            if (!approved) {
                // A severe loss removes or guesses semantic data. The caller must approve this
                // exact code -- the diagnostic tells them which one -- so the loss is a deliberate
                // decision, recorded, rather than a silent one.
                Diagnostic d("MK1602_UNAPPROVED_SEVERE_LOSS", Severity::error,
                             "this conversion has a severe loss that was not approved");
                d.with_context("loss_code", item.code);
                d.with_context("source_feature", item.source_feature);
                d.with_context("target_constraint", item.target_constraint);
                d.with_context("affected_splats", item.affected_splats);
                d.with_context("remediation", item.remediation);
                d.with_context("approve_with", std::string("--allow-loss ") + item.code);
                return Result<void>::failure(ErrorCode::unsupported_feature, std::move(d));
            }
        }
    }
    return Result<void>::success();
}

}  // namespace melkor
