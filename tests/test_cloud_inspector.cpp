#include "melkor/cloud_inspector.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool hasIssue(const melkor::CloudInspection& result, const std::string& code, size_t count = 1) {
    for (const auto& issue : result.issues) {
        if (issue.code == code && issue.count == count)
            return true;
    }
    return false;
}

melkor::SplatData canonicalData(std::vector<melkor::Vec3f> positions,
                                std::vector<melkor::Vec3f> scales, std::uint32_t degree = 0) {
    const std::size_t count = positions.size();
    melkor::SplatBufferInput input;
    input.positions = std::move(positions);
    input.scales = std::move(scales);
    input.rotations.assign(count, melkor::Quatf{});
    input.opacities.assign(count, 0.5f);
    const std::size_t coefficients = (degree + 1) * (degree + 1);
    input.sh =
        melkor::ShBuffer::create(degree, count, std::vector<float>(count * coefficients * 3, 0.0f))
            .value();
    return melkor::SplatData::create(std::move(input)).value();
}

}  // namespace

int main() {
    // Canonical inspection never converts back through the legacy log/logit domains.
    {
        const auto empty = canonicalData({}, {});
        const auto result = melkor::inspectCloud(empty);
        check(!result.valid && hasIssue(result, "empty_cloud"), "empty canonical cloud is invalid");
    }

    {
        const auto cloud = canonicalData({{-1.0f, 2.0f, 3.0f}, {5.0f, -4.0f, 1.0f}},
                                         {{0.01f, 0.01f, 0.01f}, {0.02f, 0.02f, 0.02f}}, 4);
        const auto result = melkor::inspectCloud(cloud);
        check(result.valid, "finite canonical degree-four cloud is valid");
        check(result.sh_degree == 4, "canonical inspector reports degree four");
        check(result.bounds.available && result.bounds.min[0] == -1.0f &&
                  result.bounds.min[1] == -4.0f && result.bounds.max[0] == 5.0f &&
                  result.bounds.max[2] == 3.0f,
              "canonical bounds cover all positions");
    }

    {
        const auto cloud = canonicalData({{0.0f, 0.0f, 0.0f}}, {{1.0e20f, 1.0e-30f, 1.0f}});
        const auto result = melkor::inspectCloud(cloud);
        check(!result.valid, "canonical covariance overflow and underflow are invalid");
        check(hasIssue(result, "scale_covariance_overflow"),
              "canonical squared-scale overflow reported");
        check(hasIssue(result, "scale_covariance_underflow"),
              "canonical squared-scale underflow reported");
    }

    {
        const auto cloud = canonicalData({{0.0f, 0.0f, 0.0f}}, {{1.0e-20f, 1.0f, 1.0f}});
        const auto result = melkor::inspectCloud(cloud);
        check(result.valid, "canonical subnormal covariance is warning-only");
        check(hasIssue(result, "scale_covariance_subnormal"),
              "canonical subnormal covariance reported");
    }

    if (failures == 0) {
        std::puts("Cloud inspector tests passed");
        return 0;
    }
    std::fprintf(stderr, "%d cloud inspector test(s) failed\n", failures);
    return 1;
}
