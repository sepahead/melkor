#include "melkor/cloud_inspector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace melkor {
namespace {

struct IssueCounter {
    InspectionSeverity severity;
    const char* code;
    const char* message;
    size_t count = 0;
    size_t first_index = 0;

    void record(size_t index) {
        if (count == 0)
            first_index = index;
        ++count;
    }
};

}  // namespace

void addInspectionIssue(CloudInspection& inspection, InspectionSeverity severity, std::string code,
                        std::string message, size_t count, size_t first_index, bool has_index) {
    if (count == 0)
        return;
    inspection.issues.push_back(
        {severity, std::move(code), std::move(message), count, first_index, has_index});
    if (severity == InspectionSeverity::Error) {
        inspection.error_count += count;
    } else {
        inspection.warning_count += count;
    }
    inspection.valid = inspection.error_count == 0;
}

CloudInspection inspectCloud(const SplatData& cloud) {
    CloudInspection result;
    result.splat_count = cloud.size();
    result.sh_degree = static_cast<int>(cloud.sh().degree());

    if (cloud.empty()) {
        addInspectionIssue(result, InspectionSeverity::Error, "empty_cloud",
                           "The decoded cloud contains no splats.");
        return result;
    }
    if (auto valid = cloud.validate(); !valid.has_value()) {
        addInspectionIssue(result, InspectionSeverity::Error, "canonical_invariant_violation",
                           "Canonical splat data violates a validated scene invariant.");
        return result;
    }

    IssueCounter scale_overflow{InspectionSeverity::Error, "scale_covariance_overflow",
                                "Squared linear scale overflows 32-bit covariance."};
    IssueCounter scale_underflow{InspectionSeverity::Error, "scale_covariance_underflow",
                                 "Squared linear scale rounds to zero in 32-bit covariance."};
    IssueCounter scale_subnormal{InspectionSeverity::Warning, "scale_covariance_subnormal",
                                 "Squared linear scale becomes subnormal in 32-bit covariance."};

    std::array<float, 3> min_bounds{cloud.positions()[0].x, cloud.positions()[0].y,
                                    cloud.positions()[0].z};
    std::array<float, 3> max_bounds = min_bounds;
    for (std::size_t index = 0; index < cloud.size(); ++index) {
        const Vec3f& position = cloud.positions()[index];
        min_bounds[0] = std::min(min_bounds[0], position.x);
        min_bounds[1] = std::min(min_bounds[1], position.y);
        min_bounds[2] = std::min(min_bounds[2], position.z);
        max_bounds[0] = std::max(max_bounds[0], position.x);
        max_bounds[1] = std::max(max_bounds[1], position.y);
        max_bounds[2] = std::max(max_bounds[2], position.z);

        const Vec3f& scale = cloud.scales()[index];
        const std::array<float, 3> squared{scale.x * scale.x, scale.y * scale.y, scale.z * scale.z};
        if (std::any_of(squared.begin(), squared.end(),
                        [](float value) { return !std::isfinite(value); })) {
            scale_overflow.record(index);
        }
        if (std::any_of(squared.begin(), squared.end(),
                        [](float value) { return value == 0.0f; })) {
            scale_underflow.record(index);
        } else if (std::any_of(squared.begin(), squared.end(), [](float value) {
                       return std::fpclassify(value) == FP_SUBNORMAL;
                   })) {
            scale_subnormal.record(index);
        }
    }

    result.bounds.available = true;
    std::copy(min_bounds.begin(), min_bounds.end(), result.bounds.min);
    std::copy(max_bounds.begin(), max_bounds.end(), result.bounds.max);
    for (const IssueCounter* issue : {&scale_overflow, &scale_underflow, &scale_subnormal}) {
        addInspectionIssue(result, issue->severity, issue->code, issue->message, issue->count,
                           issue->first_index, issue->count > 0);
    }
    result.valid = result.error_count == 0;
    return result;
}

}  // namespace melkor
