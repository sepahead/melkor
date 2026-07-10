#pragma once

#include "melkor/gaussian_data.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace melkor {

enum class InspectionSeverity {
    Error,
    Warning,
};

struct InspectionIssue {
    InspectionSeverity severity = InspectionSeverity::Error;
    std::string code;
    std::string message;
    size_t count = 0;
    size_t first_index = 0;
    bool has_index = false;
};

struct CloudBounds {
    bool available = false;
    float min[3] = {0.0f, 0.0f, 0.0f};
    float max[3] = {0.0f, 0.0f, 0.0f};
};

struct CloudInspection {
    bool valid = false;
    size_t splat_count = 0;
    int sh_degree = 0;
    CloudBounds bounds;
    size_t error_count = 0;
    size_t warning_count = 0;
    std::vector<InspectionIssue> issues;
};

// Inspect without mutating or normalizing the cloud. Issue order is stable so
// callers can serialize the result deterministically for CI artifacts.
[[nodiscard]] CloudInspection inspectCloud(const GaussianCloud& cloud);

// Source-format adapters use this to append metadata warnings/errors while
// preserving the same deterministic summary accounting.
void addInspectionIssue(CloudInspection& inspection, InspectionSeverity severity,
                        std::string code, std::string message, size_t count = 1,
                        size_t first_index = 0, bool has_index = false);

}  // namespace melkor
