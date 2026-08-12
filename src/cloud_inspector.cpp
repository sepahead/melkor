#include "melkor/cloud_inspector.hpp"

#include "melkor/budget.hpp"

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

constexpr std::size_t kControlInterval = 4096;

}  // namespace

void addInspectionIssue(CloudInspection& inspection, InspectionSeverity severity, std::string code,
                        std::string message, size_t count, size_t first_index, bool has_index) {
    if (count == 0)
        return;
    InspectionIssue issue;
    issue.severity = severity;
    issue.code = std::move(code);
    issue.message = std::move(message);
    issue.count = count;
    issue.first_index = first_index;
    issue.has_index = has_index;
    inspection.issues.push_back(std::move(issue));
    if (severity == InspectionSeverity::Error) {
        inspection.error_count += count;
    } else {
        inspection.warning_count += count;
    }
    inspection.valid = inspection.error_count == 0;
}

namespace {

Result<CloudInspection> inspect_cloud(const SplatData& cloud, const OperationContext* context) {
    CloudInspection result;
    result.splat_count = cloud.size();
    result.sh_degree = static_cast<int>(cloud.sh().degree());

    if (cloud.empty()) {
        addInspectionIssue(result, InspectionSeverity::Error, "empty_cloud",
                           "The decoded cloud contains no splats.");
        return Result<CloudInspection>::success(std::move(result));
    }
    auto valid = context == nullptr ? cloud.validate() : cloud.validate(*context);
    if (!valid.has_value()) {
        if (valid.error_code() != ErrorCode::invalid_data) {
            return Result<CloudInspection>::failure(valid.error_code(), valid.diagnostics());
        }
        addInspectionIssue(result, InspectionSeverity::Error, "canonical_invariant_violation",
                           "Canonical splat data violates a validated scene invariant.");
        return Result<CloudInspection>::success(std::move(result));
    }

    IssueCounter scale_overflow{InspectionSeverity::Warning, "scale_covariance_overflow",
                                "Squared linear scale overflows 32-bit covariance."};
    IssueCounter scale_underflow{InspectionSeverity::Warning, "scale_covariance_underflow",
                                 "Squared linear scale rounds to zero in 32-bit covariance."};
    IssueCounter scale_subnormal{InspectionSeverity::Warning, "scale_covariance_subnormal",
                                 "Squared linear scale becomes subnormal in 32-bit covariance."};

    std::array<float, 3> min_bounds{cloud.positions()[0].x, cloud.positions()[0].y,
                                    cloud.positions()[0].z};
    std::array<float, 3> max_bounds = min_bounds;
    for (std::size_t index = 0; index < cloud.size(); ++index) {
        if (context != nullptr && index % kControlInterval == 0) {
            auto control =
                context->checkpoint({"inspect.cloud", "bounds", index, cloud.size(), "splats"});
            if (!control.has_value()) {
                return Result<CloudInspection>::failure(control.error_code(),
                                                        control.diagnostics());
            }
        }
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
        const std::array<float, 3> linear{scale.x, scale.y, scale.z};
        bool underflow = false;
        for (std::size_t axis = 0; axis < linear.size(); ++axis) {
            if (linear[axis] > 0.0f && squared[axis] == 0.0f) {
                underflow = true;
                break;
            }
        }
        if (underflow) {
            scale_underflow.record(index);
        } else if (std::any_of(squared.begin(), squared.end(), [](float value) {
                       return std::fpclassify(value) == FP_SUBNORMAL;
                   })) {
            scale_subnormal.record(index);
        }
    }
    if (context != nullptr) {
        auto control =
            context->checkpoint({"inspect.cloud", "bounds", cloud.size(), cloud.size(), "splats"});
        if (!control.has_value()) {
            return Result<CloudInspection>::failure(control.error_code(), control.diagnostics());
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
    return Result<CloudInspection>::success(std::move(result));
}

}  // namespace

CloudInspection inspectCloud(const SplatData& cloud) {
    return std::move(inspect_cloud(cloud, nullptr)).value();
}

Result<CloudInspection> inspectCloud(const SplatData& cloud, const OperationContext& context) {
    return inspect_cloud(cloud, &context);
}

}  // namespace melkor
