#include "melkor/cloud_inspector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
        if (count == 0) first_index = index;
        ++count;
    }
};

size_t shRestCount(int degree) {
    switch (degree) {
        case 0: return 0;
        case 1: return 9;
        case 2: return 24;
        case 3: return 45;
        default: return 0;
    }
}

bool allFinite(std::initializer_list<float> values) {
    return std::all_of(values.begin(), values.end(), [](float value) {
        return std::isfinite(value);
    });
}

}  // namespace

void addInspectionIssue(CloudInspection& inspection, InspectionSeverity severity,
                        std::string code, std::string message, size_t count,
                        size_t first_index, bool has_index) {
    if (count == 0) return;
    inspection.issues.push_back({severity, std::move(code), std::move(message), count,
                                 first_index, has_index});
    if (severity == InspectionSeverity::Error) {
        inspection.error_count += count;
    } else {
        inspection.warning_count += count;
    }
    inspection.valid = inspection.error_count == 0;
}

CloudInspection inspectCloud(const GaussianCloud& cloud) {
    CloudInspection result;
    result.splat_count = cloud.size();
    result.sh_degree = cloud.shDegree();

    if (cloud.empty()) {
        addInspectionIssue(result, InspectionSeverity::Error, "empty_cloud",
                           "The decoded cloud contains no splats.");
        return result;
    }
    if (cloud.shDegree() < 0 || cloud.shDegree() > 3) {
        addInspectionIssue(result, InspectionSeverity::Error, "invalid_sh_degree",
                           "The spherical-harmonics degree must be between 0 and 3.");
    }

    IssueCounter nonfinite_position{InspectionSeverity::Error, "nonfinite_position",
                                    "Position contains NaN or infinity."};
    IssueCounter nonfinite_color{InspectionSeverity::Error, "nonfinite_color",
                                 "DC color contains NaN or infinity."};
    IssueCounter nonfinite_opacity{InspectionSeverity::Error, "nonfinite_opacity",
                                   "Opacity logit contains NaN or infinity."};
    IssueCounter nonfinite_scale{InspectionSeverity::Error, "nonfinite_scale",
                                 "Log scale contains NaN or infinity."};
    IssueCounter scale_overflow{InspectionSeverity::Error, "scale_covariance_overflow",
                                "Squared linear scale overflows 32-bit covariance."};
    IssueCounter scale_underflow{InspectionSeverity::Error, "scale_covariance_underflow",
                                 "Squared linear scale rounds to zero in 32-bit covariance."};
    IssueCounter scale_subnormal{InspectionSeverity::Warning, "scale_covariance_subnormal",
                                 "Squared linear scale becomes subnormal in 32-bit covariance."};
    IssueCounter nonfinite_rotation{InspectionSeverity::Error, "nonfinite_rotation",
                                    "Quaternion contains NaN or infinity."};
    IssueCounter zero_quaternion{InspectionSeverity::Error, "zero_quaternion",
                                 "Quaternion has zero length."};
    IssueCounter nonunit_quaternion{InspectionSeverity::Warning, "nonunit_quaternion",
                                    "Quaternion length differs from one."};
    IssueCounter sh_count_mismatch{InspectionSeverity::Error, "sh_count_mismatch",
                                   "SH-rest coefficient count does not match the cloud degree."};
    IssueCounter nonfinite_sh{InspectionSeverity::Error, "nonfinite_sh",
                              "SH-rest coefficients contain NaN or infinity."};

    const size_t expected_sh = shRestCount(cloud.shDegree());
    bool have_bounds = false;
    std::array<float, 3> min_bounds{};
    std::array<float, 3> max_bounds{};

    for (size_t index = 0; index < cloud.size(); ++index) {
        const GaussianSplat& splat = cloud[index];
        if (!allFinite({splat.x, splat.y, splat.z})) {
            nonfinite_position.record(index);
        } else if (!have_bounds) {
            min_bounds = {splat.x, splat.y, splat.z};
            max_bounds = min_bounds;
            have_bounds = true;
        } else {
            min_bounds[0] = std::min(min_bounds[0], splat.x);
            min_bounds[1] = std::min(min_bounds[1], splat.y);
            min_bounds[2] = std::min(min_bounds[2], splat.z);
            max_bounds[0] = std::max(max_bounds[0], splat.x);
            max_bounds[1] = std::max(max_bounds[1], splat.y);
            max_bounds[2] = std::max(max_bounds[2], splat.z);
        }
        if (!allFinite({splat.f_dc_0, splat.f_dc_1, splat.f_dc_2})) {
            nonfinite_color.record(index);
        }
        if (!std::isfinite(splat.opacity)) nonfinite_opacity.record(index);

        const std::array<float, 3> scales{splat.scale_0, splat.scale_1, splat.scale_2};
        if (!std::all_of(scales.begin(), scales.end(), [](float value) {
                return std::isfinite(value);
            })) {
            nonfinite_scale.record(index);
        } else {
            if (std::any_of(scales.begin(), scales.end(), [](float value) {
                    const float linear = std::exp(value);
                    return !std::isfinite(linear * linear);
                })) {
                scale_overflow.record(index);
            }
            if (std::any_of(scales.begin(), scales.end(), [](float value) {
                    const float linear = std::exp(value);
                    return linear * linear == 0.0f;
                })) {
                scale_underflow.record(index);
            } else if (std::any_of(scales.begin(), scales.end(), [](float value) {
                           const float linear = std::exp(value);
                           return std::fpclassify(linear * linear) == FP_SUBNORMAL;
                       })) {
                scale_subnormal.record(index);
            }
        }

        if (!allFinite({splat.rot_0, splat.rot_1, splat.rot_2, splat.rot_3})) {
            nonfinite_rotation.record(index);
        } else {
            const double norm = std::hypot(
                std::hypot(static_cast<double>(splat.rot_0), static_cast<double>(splat.rot_1)),
                std::hypot(static_cast<double>(splat.rot_2), static_cast<double>(splat.rot_3)));
            if (norm == 0.0) {
                zero_quaternion.record(index);
            } else if (std::abs(norm - 1.0) > 1e-3) {
                nonunit_quaternion.record(index);
            }
        }

        if (splat.sh_rest.size() != expected_sh) sh_count_mismatch.record(index);
        if (!std::all_of(splat.sh_rest.begin(), splat.sh_rest.end(), [](float value) {
                return std::isfinite(value);
            })) {
            nonfinite_sh.record(index);
        }
    }

    if (have_bounds) {
        result.bounds.available = true;
        std::copy(min_bounds.begin(), min_bounds.end(), result.bounds.min);
        std::copy(max_bounds.begin(), max_bounds.end(), result.bounds.max);
    }

    for (const IssueCounter* issue : {
             &nonfinite_position, &nonfinite_color, &nonfinite_opacity,
             &nonfinite_scale, &scale_overflow, &scale_underflow, &scale_subnormal,
             &nonfinite_rotation, &zero_quaternion, &nonunit_quaternion,
             &sh_count_mismatch, &nonfinite_sh,
         }) {
        addInspectionIssue(result, issue->severity, issue->code, issue->message,
                           issue->count, issue->first_index, issue->count > 0);
    }
    result.valid = result.error_count == 0;
    return result;
}

}  // namespace melkor
