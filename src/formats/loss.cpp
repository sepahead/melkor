#include "melkor/format/loss.hpp"

#include <algorithm>

namespace melkor {

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

void LossReport::add(LossItem item) { items_.push_back(std::move(item)); }

bool LossReport::has_blocking() const noexcept {
    return std::any_of(items_.begin(), items_.end(), [](const LossItem& item) {
        return item.severity == LossSeverity::severe || item.severity == LossSeverity::fatal;
    });
}

Result<void> LossReport::check_policy(const std::vector<std::string>& approved_codes) const {
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
            const bool approved =
                std::find(approved_codes.begin(), approved_codes.end(), item.code) !=
                approved_codes.end();
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
                d.with_context("approve_with",
                               std::string("--allow-loss ") + item.code);
                return Result<void>::failure(ErrorCode::unsupported_feature, std::move(d));
            }
        }
    }
    return Result<void>::success();
}

}  // namespace melkor
