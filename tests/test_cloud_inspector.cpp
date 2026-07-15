#include "melkor/cloud_inspector.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
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

melkor::GaussianSplat validSplat() {
    melkor::GaussianSplat splat{};
    splat.x = -1.0f;
    splat.y = 2.0f;
    splat.z = 3.0f;
    splat.f_dc_0 = 0.1f;
    splat.f_dc_1 = 0.2f;
    splat.f_dc_2 = 0.3f;
    splat.opacity = 2.0f;
    splat.scale_0 = splat.scale_1 = splat.scale_2 = std::log(0.01f);
    splat.rot_0 = 1.0f;
    return splat;
}

bool hasIssue(const melkor::CloudInspection& result, const std::string& code,
              size_t count = 1) {
    for (const auto& issue : result.issues) {
        if (issue.code == code && issue.count == count) return true;
    }
    return false;
}

melkor::SplatData canonicalData(std::vector<melkor::Vec3f> positions,
                                std::vector<melkor::Vec3f> scales,
                                std::uint32_t degree = 0) {
    const std::size_t count = positions.size();
    melkor::SplatBufferInput input;
    input.positions = std::move(positions);
    input.scales = std::move(scales);
    input.rotations.assign(count, melkor::Quatf{});
    input.opacities.assign(count, 0.5f);
    const std::size_t coefficients = (degree + 1) * (degree + 1);
    input.sh = melkor::ShBuffer::create(
                   degree, count, std::vector<float>(count * coefficients * 3, 0.0f))
                   .value();
    return melkor::SplatData::create(std::move(input)).value();
}

}  // namespace

int main() {
    // Canonical inspection never converts back through the legacy log/logit domains.
    {
        const auto empty = canonicalData({}, {});
        const auto result = melkor::inspectCloud(empty);
        check(!result.valid && hasIssue(result, "empty_cloud"),
              "empty canonical cloud is invalid");
    }

    {
        const auto cloud = canonicalData({{-1.0f, 2.0f, 3.0f}, {5.0f, -4.0f, 1.0f}},
                                         {{0.01f, 0.01f, 0.01f},
                                          {0.02f, 0.02f, 0.02f}},
                                         4);
        const auto result = melkor::inspectCloud(cloud);
        check(result.valid, "finite canonical degree-four cloud is valid");
        check(result.sh_degree == 4, "canonical inspector reports degree four");
        check(result.bounds.available && result.bounds.min[0] == -1.0f &&
                  result.bounds.min[1] == -4.0f && result.bounds.max[0] == 5.0f &&
                  result.bounds.max[2] == 3.0f,
              "canonical bounds cover all positions");
    }

    {
        const auto cloud = canonicalData({{0.0f, 0.0f, 0.0f}},
                                         {{1.0e20f, 1.0e-30f, 1.0f}});
        const auto result = melkor::inspectCloud(cloud);
        check(!result.valid, "canonical covariance overflow and underflow are invalid");
        check(hasIssue(result, "scale_covariance_overflow"),
              "canonical squared-scale overflow reported");
        check(hasIssue(result, "scale_covariance_underflow"),
              "canonical squared-scale underflow reported");
    }

    {
        const auto cloud = canonicalData({{0.0f, 0.0f, 0.0f}},
                                         {{1.0e-20f, 1.0f, 1.0f}});
        const auto result = melkor::inspectCloud(cloud);
        check(result.valid, "canonical subnormal covariance is warning-only");
        check(hasIssue(result, "scale_covariance_subnormal"),
              "canonical subnormal covariance reported");
    }

    // The remainder pins the temporary legacy validator until PLY/SPZ and algorithms migrate.
    {
        melkor::GaussianCloud cloud;
        const auto result = melkor::inspectLegacyCloud(cloud);
        check(!result.valid && hasIssue(result, "empty_cloud"), "empty cloud is invalid");
    }

    {
        melkor::GaussianCloud cloud;
        cloud.addSplat(validSplat());
        auto second = validSplat();
        second.x = 5.0f;
        second.y = -4.0f;
        cloud.addSplat(second);
        const auto result = melkor::inspectLegacyCloud(cloud);
        check(result.valid, "finite degree-zero cloud is valid");
        check(result.error_count == 0 && result.warning_count == 0,
              "valid cloud has no issues");
        check(result.bounds.available && result.bounds.min[0] == -1.0f &&
                  result.bounds.min[1] == -4.0f && result.bounds.max[0] == 5.0f &&
                  result.bounds.max[2] == 3.0f,
              "bounds cover all positions");
    }

    {
        melkor::GaussianCloud cloud;
        auto splat = validSplat();
        splat.x = std::numeric_limits<float>::quiet_NaN();
        splat.opacity = std::numeric_limits<float>::infinity();
        splat.scale_0 = 100.0f;
        splat.rot_0 = 0.0f;
        splat.sh_rest.push_back(1.0f);
        cloud.addSplat(splat);
        const auto result = melkor::inspectLegacyCloud(cloud);
        check(!result.valid, "invalid numeric fields fail inspection");
        check(hasIssue(result, "nonfinite_position"), "nonfinite position reported");
        check(hasIssue(result, "nonfinite_opacity"), "nonfinite opacity reported");
        check(hasIssue(result, "scale_covariance_overflow"), "scale overflow reported");
        check(hasIssue(result, "zero_quaternion"), "zero quaternion reported");
        check(hasIssue(result, "sh_count_mismatch"), "SH mismatch reported");
    }

    {
        melkor::GaussianCloud cloud;
        auto splat = validSplat();
        splat.rot_0 = 2.0f;
        splat.scale_0 = -45.0f;
        cloud.addSplat(splat);
        const auto result = melkor::inspectLegacyCloud(cloud);
        check(result.valid, "warning-only cloud remains valid");
        check(result.warning_count == 2, "warning counts are stable");
        check(hasIssue(result, "scale_covariance_subnormal"),
              "subnormal covariance warning reported");
        check(hasIssue(result, "nonunit_quaternion"), "non-unit quaternion warning reported");
    }

    {
        melkor::GaussianCloud cloud;
        auto splat = validSplat();
        splat.scale_0 = 44.4f;
        splat.scale_1 = -52.0f;
        cloud.addSplat(splat);
        const auto result = melkor::inspectLegacyCloud(cloud);
        check(!result.valid, "covariance overflow and zero-underflow are invalid");
        check(hasIssue(result, "scale_covariance_overflow"),
              "squared scale overflow reported");
        check(hasIssue(result, "scale_covariance_underflow"),
              "squared scale underflow reported");
    }

    {
        melkor::GaussianCloud cloud;
        auto splat = validSplat();
        splat.scale_0 = 44.0f;
        splat.scale_1 = -51.5f;
        cloud.addSplat(splat);
        const auto result = melkor::inspectLegacyCloud(cloud);
        check(result.valid, "representable covariance boundary values remain valid");
        check(result.error_count == 0, "boundary values have no numeric errors");
        check(hasIssue(result, "scale_covariance_subnormal"),
              "representable subnormal covariance is warned");
    }

    {
        for (int degree = 1; degree <= 3; ++degree) {
            melkor::GaussianCloud cloud;
            cloud.setShDegree(degree);
            auto splat = validSplat();
            splat.sh_rest.resize(degree == 1 ? 9 : degree == 2 ? 24 : 45, 0.1f);
            cloud.addSplat(std::move(splat));
            check(melkor::inspectLegacyCloud(cloud).valid, "valid SH degree accepted");
        }
    }

    if (failures == 0) {
        std::puts("Cloud inspector tests passed");
        return 0;
    }
    std::fprintf(stderr, "%d cloud inspector test(s) failed\n", failures);
    return 1;
}
