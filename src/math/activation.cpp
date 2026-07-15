#include "melkor/math/activation.hpp"

#include <cmath>

namespace melkor::math {
namespace {

Result<float> fail(const char* code, const char* message, double value) {
    Diagnostic diagnostic(code, Severity::error, message);
    diagnostic.with_context("value", value);
    return Result<float>::failure(ErrorCode::invalid_data, std::move(diagnostic));
}

// The largest magnitude the exp() input may take. exp(88) is near the float32 max (~3.4e38), so
// a log-scale beyond this range would overflow to inf. A real scale never approaches this; a
// value that does is corrupt, and failing is better than propagating an infinity.
constexpr double kMaxExpInput = 80.0;

}  // namespace

Result<float> sigmoid_from_logit(float logit) {
    if (!std::isfinite(logit)) {
        return fail("MK1101_NONFINITE_LOGIT", "logit is not finite", logit);
    }
    // Branch to avoid overflow: for large positive x, exp(-x) underflows to 0 cleanly; for large
    // negative x, compute through exp(x) so we never evaluate exp of a large positive number.
    double p;
    if (logit >= 0.0) {
        const double e = std::exp(-static_cast<double>(logit));
        p = 1.0 / (1.0 + e);
    } else {
        const double e = std::exp(static_cast<double>(logit));
        p = e / (1.0 + e);
    }
    return Result<float>::success(static_cast<float>(p));
}

Result<float> logit_from_probability(float probability) {
    if (!std::isfinite(probability)) {
        return fail("MK1102_NONFINITE_PROBABILITY", "probability is not finite", probability);
    }
    if (probability <= 0.0f || probability >= 1.0f) {
        // The endpoints have infinite logits. A decoder that produced an exact 0 or 1 must clamp
        // with a recorded epsilon before calling this; silently clamping here would hide that
        // the value was at the boundary.
        return fail("MK1103_PROBABILITY_OUT_OF_OPEN_INTERVAL",
                    "probability must be strictly inside (0, 1)", probability);
    }
    const double p = probability;
    return Result<float>::success(static_cast<float>(std::log(p / (1.0 - p))));
}

Result<float> linear_scale_from_log(float log_scale) {
    if (!std::isfinite(log_scale)) {
        return fail("MK1104_NONFINITE_LOG_SCALE", "log-scale is not finite", log_scale);
    }
    if (std::fabs(static_cast<double>(log_scale)) > kMaxExpInput) {
        return fail("MK1105_LOG_SCALE_OUT_OF_RANGE",
                    "log-scale is outside the range that maps to a finite positive scale",
                    log_scale);
    }
    const double s = std::exp(static_cast<double>(log_scale));
    return Result<float>::success(static_cast<float>(s));
}

Result<float> log_scale_from_linear(float linear_scale) {
    if (!std::isfinite(linear_scale)) {
        return fail("MK1106_NONFINITE_SCALE", "scale is not finite", linear_scale);
    }
    if (linear_scale <= 0.0f) {
        return fail("MK1107_NONPOSITIVE_SCALE", "scale must be strictly positive", linear_scale);
    }
    return Result<float>::success(static_cast<float>(std::log(static_cast<double>(linear_scale))));
}

}  // namespace melkor::math
