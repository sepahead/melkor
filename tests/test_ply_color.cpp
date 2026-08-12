// PLY profile boundary tests.

#include "melkor/math/quaternion.hpp"
#include "melkor/ply_writer.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, expression);
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

melkor::PlyReadConfig graphdeco_config() {
    melkor::PlyReadConfig config;
    config.profile = melkor::FormatProfileId::ply_graphdeco_3dgs_v1;
    config.source_frame_id = "gltf-luf";
    config.source_unit_to_meter = 1.0;
    config.source_color_space = melkor::ColorSpace::lin_rec709_display;
    return config;
}

std::string graphdeco_ply(const std::string& scalar_type, const std::string& quaternion) {
    return "ply\n"
           "format ascii 1.0\n"
           "element vertex 1\n"
           "property " +
           scalar_type +
           " x\n"
           "property " +
           scalar_type +
           " y\n"
           "property " +
           scalar_type +
           " z\n"
           "property " +
           scalar_type +
           " f_dc_0\n"
           "property " +
           scalar_type +
           " f_dc_1\n"
           "property " +
           scalar_type +
           " f_dc_2\n"
           "property " +
           scalar_type +
           " opacity\n"
           "property " +
           scalar_type +
           " scale_0\n"
           "property " +
           scalar_type +
           " scale_1\n"
           "property " +
           scalar_type +
           " scale_2\n"
           "property " +
           scalar_type +
           " rot_0\n"
           "property " +
           scalar_type +
           " rot_1\n"
           "property " +
           scalar_type +
           " rot_2\n"
           "property " +
           scalar_type +
           " rot_3\n"
           "end_header\n"
           "0 0 0 1.5 -0.5 0.25 0 0 0 0 " +
           quaternion + "\n";
}

melkor::PlyReader::ReadResult read(const std::string& source, const melkor::PlyReadConfig& config) {
    return melkor::PlyReader{}.readFromBuffer(reinterpret_cast<const std::uint8_t*>(source.data()),
                                              source.size(), config);
}

void test_graphdeco_sh_is_not_color_data() {
    const auto result = read(graphdeco_ply("float", "1 0 0 0"), graphdeco_config());
    CHECK(result.success);
    CHECK(result.data.has_value());
    if (!result.data.has_value())
        return;
    const auto dc = result.data->sh().dc(0);
    CHECK(dc.has_value());
    CHECK(dc->x == 1.5f);
    CHECK(dc->y == -0.5f);
    CHECK(dc->z == 0.25f);
}

void test_graphdeco_raw_quaternion_is_normalized() {
    const auto result = read(graphdeco_ply("float", "1 1 1 1"), graphdeco_config());
    CHECK(result.success);
    CHECK(result.data.has_value());
    if (!result.data.has_value())
        return;
    const auto& rotation = result.data->rotations()[0];
    CHECK(std::fabs(rotation.x - 0.5f) < 1.0e-6f);
    CHECK(std::fabs(rotation.y - 0.5f) < 1.0e-6f);
    CHECK(std::fabs(rotation.z - 0.5f) < 1.0e-6f);
    CHECK(std::fabs(rotation.w - 0.5f) < 1.0e-6f);
}

void test_zero_quaternion_is_rejected() {
    const auto result = read(graphdeco_ply("float", "0 0 0 0"), graphdeco_config());
    CHECK(!result.success);
    CHECK(!result.data.has_value());
    CHECK(result.error_code() == melkor::ErrorCode::invalid_data);
    CHECK(result.diagnostics.size() == 1);
    if (result.diagnostics.size() == 1) {
        CHECK(result.diagnostics[0].code == "MK1202_ZERO_QUATERNION");
        CHECK(result.diagnostics[0].context.at("splat_index") ==
              melkor::JsonScalar{std::uint64_t{0}});
    }
}

void test_profile_values_require_float32() {
    const auto result = read(graphdeco_ply("double", "1 0 0 0"), graphdeco_config());
    CHECK(!result.success);
    CHECK(result.error_message.find("float32") != std::string::npos);
}

void test_generic_points_are_not_gaussians() {
    const std::string source = "ply\nformat ascii 1.0\nelement vertex 1\n"
                               "property float x\nproperty float y\nproperty float z\n"
                               "end_header\n0 0 0\n";
    const auto result = read(source, {});
    CHECK(!result.success);
    CHECK(result.error_code() == melkor::ErrorCode::unsupported_feature);
}

}  // namespace

int main() {
    test_graphdeco_sh_is_not_color_data();
    test_graphdeco_raw_quaternion_is_normalized();
    test_zero_quaternion_is_rejected();
    test_profile_values_require_float32();
    test_generic_points_are_not_gaussians();

    if (failures == 0) {
        std::printf("PLY profiles: %d checks passed\n", checks);
        return 0;
    }
    std::fprintf(stderr, "PLY profiles: %d of %d checks failed\n", failures, checks);
    return 1;
}
