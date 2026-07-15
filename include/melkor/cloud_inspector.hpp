#pragma once

#include "melkor/scene.hpp"

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

// Inspect canonical data without mutating it. SplatData already guarantees finite values,
// canonical domains, complete SH storage, and unit rotations, so this concentrates on useful
// derived diagnostics: empty input, bounds, and whether squared linear scales are representable
// as float32 covariance entries. Issue order is stable for deterministic CI artifacts.
[[nodiscard]] CloudInspection inspectCloud(const SplatData& cloud);

// Source-format adapters use this to append metadata warnings/errors while
// preserving the same deterministic summary accounting.
void addInspectionIssue(CloudInspection& inspection, InspectionSeverity severity, std::string code,
                        std::string message, size_t count = 1, size_t first_index = 0,
                        bool has_index = false);

}  // namespace melkor
