#include "melkor/provenance.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace melkor;
using json = nlohmann::json;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

SplatData scene(std::uint32_t degree = 0) {
    SplatBufferInput input;
    input.positions = {{0.0f, 0.0f, 0.0f}};
    input.scales = {{1.0f, 1.0f, 1.0f}};
    input.rotations = {Quatf{}};
    input.opacities = {0.5f};
    const std::size_t coefficients = (degree + 1) * (degree + 1);
    input.sh = ShBuffer::create(degree, 1, std::vector<float>(coefficients * 3, 0.0f)).value();
    return SplatData::create(std::move(input)).value();
}

Provenance provenance() {
    Provenance p;
    p.source_format = "ply";
    p.source_profile = "graphdeco-3dgs-v1";
    p.source_sha256 = std::string(64, 'a');
    ProvenanceOperation operation;
    operation.name = "normalize";
    operation.tool_version = "2.0.0";
    operation.parameters["clamp"] = true;
    operation.parameters["count"] = std::uint64_t{3};
    operation.parameters["note"] = std::string("deterministic");
    operation.timestamp = "2026-07-15T12:00:00Z";
    p.operations.push_back(std::move(operation));
    return p;
}

void test_primitive_validates_metadata_against_data() {
    SplatMetadata metadata;
    metadata.sh_degree = 1;
    auto valid = SplatPrimitive::create(metadata, scene(1), provenance());
    CHECK(valid.has_value());
    CHECK(valid.value().validate().has_value());
    CHECK(valid.value().metadata().frame.id == "gltf-luf");
    CHECK(valid.value().metadata().scale_domain == ScaleDomain::linear);
    CHECK(valid.value().metadata().opacity_domain == OpacityDomain::linear);
    CHECK(valid.value().metadata().color_space == ColorSpace::lin_rec709_display);
    CHECK(!valid.value().metadata().antialiased.has_value());

    metadata.color_space = ColorSpace::srgb_rec709_display;
    auto display_color = SplatPrimitive::create(metadata, scene(1), provenance());
    CHECK(display_color.has_value());

    metadata.color_space = static_cast<ColorSpace>(99);
    auto bad_color = SplatPrimitive::create(metadata, scene(1), provenance());
    CHECK(!bad_color.has_value());
    CHECK(bad_color.diagnostics()[0].code == "MK1529_METADATA_DOMAIN_INVALID");

    metadata.sh_degree = 0;
    metadata.color_space = ColorSpace::lin_rec709_display;
    auto mismatch = SplatPrimitive::create(metadata, scene(1), provenance());
    CHECK(!mismatch.has_value());
    CHECK(mismatch.diagnostics()[0].code == "MK1524_METADATA_SH_DEGREE_MISMATCH");

    metadata.sh_degree = 1;
    metadata.scale_domain = static_cast<ScaleDomain>(99);
    auto bad_domain = SplatPrimitive::create(metadata, scene(1), provenance());
    CHECK(!bad_domain.has_value());
    CHECK(bad_domain.diagnostics()[0].code == "MK1529_METADATA_DOMAIN_INVALID");

    metadata.scale_domain = ScaleDomain::linear;
    metadata.frame =
        math::frame_from_basis("ply-rdf", math::Mat3{-1, 0, 0, 0, -1, 0, 0, 0, 1}, 1.0).value();
    auto noncanonical_frame = SplatPrimitive::create(metadata, scene(1), provenance());
    CHECK(!noncanonical_frame.has_value());
    CHECK(noncanonical_frame.diagnostics()[0].code == "MK1531_METADATA_FRAME_NOT_CANONICAL");
}

void test_reproducible_json_is_deterministic_and_timestamp_free() {
    auto first = provenance_to_json(provenance());
    auto second = provenance_to_json(provenance());
    CHECK(first.has_value());
    CHECK(second.has_value());
    CHECK(first.value() == second.value());

    json document = json::parse(first.value());
    CHECK(document["schema_version"] == 1);
    CHECK(document["model_schema"] == "melkor.scene/v1");
    CHECK(document["source_format"] == "ply");
    CHECK(document["source_profile"] == "graphdeco-3dgs-v1");
    CHECK(document["source_sha256"] == std::string(64, 'a'));
    CHECK(document["operations"][0]["timestamp"].is_null());
    CHECK(document["operations"][0]["parameters"]["clamp"] == true);

    auto timestamped = provenance_to_json(provenance(), false);
    CHECK(timestamped.has_value());
    CHECK(json::parse(timestamped.value())["operations"][0]["timestamp"] == "2026-07-15T12:00:00Z");
}

void test_missing_or_invalid_source_identity_fails() {
    Provenance p = provenance();
    p.source_format.clear();
    auto no_format = provenance_to_json(p);
    CHECK(!no_format.has_value());
    CHECK(no_format.diagnostics()[0].code == "MK1520_PROVENANCE_SOURCE_FORMAT_MISSING");

    p = provenance();
    p.source_sha256 = "ABC";
    auto bad_hash = provenance_to_json(p);
    CHECK(!bad_hash.has_value());
    CHECK(bad_hash.diagnostics()[0].code == "MK1522_PROVENANCE_SHA256_INVALID");
}

void test_absolute_paths_and_nonfinite_parameters_fail_closed() {
    Provenance p = provenance();
    p.operations[0].parameters["input_path"] = std::string("/Users/alice/secret/scene.ply");
    auto leaked = provenance_to_json(p);
    CHECK(!leaked.has_value());
    CHECK(leaked.diagnostics()[0].code == "MK1527_PROVENANCE_ABSOLUTE_PATH");

    p = provenance();
    p.operations[0].parameters["input_path"] = std::string("C:\\Users\\alice\\scene.ply");
    CHECK(!provenance_to_json(p).has_value());

    p = provenance();
    p.operations[0].parameters["input_path"] = std::string("file:///Users/alice/secret/scene.ply");
    CHECK(!provenance_to_json(p).has_value());

    p = provenance();
    p.operations[0].parameters["input_path"] = std::string("FILE:C:/Users/alice/scene.ply");
    CHECK(!provenance_to_json(p).has_value());

    p = provenance();
    p.operations[0].parameters["input_path"] = std::string("~/secret/scene.ply");
    CHECK(!provenance_to_json(p).has_value());

    p = provenance();
    p.operations[0].parameters["value"] = std::numeric_limits<double>::infinity();
    auto nonfinite = provenance_to_json(p);
    CHECK(!nonfinite.has_value());
    CHECK(nonfinite.diagnostics()[0].code == "MK1526_PROVENANCE_PARAMETER_INVALID");
}

void test_timestamps_use_rfc3339() {
    Provenance p = provenance();
    p.operations[0].timestamp = "2024-02-29T23:59:59.125-07:30";
    CHECK(provenance_to_json(p, false).has_value());

    p.operations[0].timestamp = "2025-02-29T12:00:00Z";
    auto invalid_day = provenance_to_json(p, false);
    CHECK(!invalid_day.has_value());
    CHECK(invalid_day.diagnostics()[0].code == "MK1532_PROVENANCE_TIMESTAMP_INVALID");

    p.operations[0].timestamp = "2026-07-15T24:00:00Z";
    CHECK(!provenance_to_json(p, false).has_value());

    p.operations[0].timestamp = "2026-07-15 12:00:00";
    CHECK(!provenance_to_json(p, false).has_value());

    p.operations[0].timestamp = "2026-07-15T12:00:00.123+02:30";
    CHECK(provenance_to_json(p, false).has_value());

    p.operations[0].timestamp = "2016-12-31T23:59:60Z";
    CHECK(provenance_to_json(p, false).has_value());

    p.operations[0].timestamp = "2016-12-31T23:59:61Z";
    CHECK(!provenance_to_json(p, false).has_value());
}

}  // namespace

int main() {
    test_primitive_validates_metadata_against_data();
    test_reproducible_json_is_deterministic_and_timestamp_free();
    test_missing_or_invalid_source_identity_fails();
    test_absolute_paths_and_nonfinite_parameters_fail_closed();
    test_timestamps_use_rfc3339();

    if (g_failures == 0) {
        std::printf("provenance: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "provenance: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
