// Melkor core correctness tests.
//
// These are deliberately self-contained (no external test framework): each
// case builds up an in-memory input, runs a Melkor core routine, and asserts a
// geometric/encoding property. They exist because the previous CI "test" step
// was only `melkor --info`, which exercises no actual conversion logic.
//
// Coverage:
//   1. SPZ quaternion round-trip against the canonical spz decoder (proves the
//      canonical xyzw boundary independently, without a symmetric Melkor-only double-bug).
//   2. Header-driven PLY reader against three layouts: 3DGS ascii, plain
//      red/green/blue ascii, and melkor-written binary.
//   3. PCA normal estimation on a sphere surface (normals should align with the
//      radial direction).
//   4. SH C0 constant round-trip (rgbToShDc / shDcToRgb are inverses).

#include "melkor/enhanced_converter.hpp"
#include "melkor/gaussian_data.hpp"
#include "melkor/glb_reader.hpp"
#include "melkor/math/color.hpp"
#include "melkor/math/quaternion.hpp"
#include "melkor/ply_writer.hpp"

#ifdef MELKOR_HAS_SPZ
#include "melkor/spz_encoder.hpp"
#include "load-spz.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <unistd.h>  // getpid(), for per-process temp file names

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (cond) {
        printf("  PASS: %s\n", msg);
    } else {
        printf("  FAIL: %s\n", msg);
        ++g_failures;
    }
}

// ---- Test 1: SPZ canonical boundary ---------------------------------------
#ifdef MELKOR_HAS_SPZ
melkor::SplatData make_spz_data(std::uint32_t degree, std::vector<float> sh_values = {}) {
    using namespace melkor;
    const std::size_t coefficient_count = static_cast<std::size_t>(degree + 1) * (degree + 1);
    if (sh_values.empty())
        sh_values.assign(coefficient_count * 3, 0.0f);
    SplatBufferInput input;
    input.positions.push_back({});
    input.scales.push_back({0.1f, 0.2f, 0.3f});
    input.rotations.push_back({});
    input.opacities.push_back(0.75f);
    input.sh = ShBuffer::create(degree, 1, std::move(sh_values)).value();
    return SplatData::create(std::move(input)).value();
}

bool has_diagnostic(const melkor::SpzEncodeResult& result, const char* code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [&](const auto& diagnostic) { return diagnostic.code == code; });
}

bool test_spz_quaternion_order() {
    printf("[test] SPZ quaternion order against canonical decoder\n");
    using namespace melkor;
    auto data = make_spz_data(0);
    const auto q = math::normalize({0.4, 0.2, 0.4, 0.8}).value();
    auto edit = data.edit();
    edit.set_rotations({{static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z),
                         static_cast<float>(q.w)}});
    data = edit.commit().value();

    const std::string path = (std::filesystem::temp_directory_path() /
                              ("melkor_test_quat_" + std::to_string(getpid()) + ".spz"))
                                 .string();
    const auto encoded = SpzEncoder{}.encodeToFile(path, data);
    spz::UnpackOptions options;
    options.to = spz::CoordinateSystem::RDF;
    const auto external = encoded.success ? spz::loadSpz(path, options) : spz::GaussianCloud{};
    const bool ok =
        external.numPoints == 1 && external.rotations.size() == 4 &&
        math::angular_distance(q, {external.rotations[0], external.rotations[1],
                                   external.rotations[2], external.rotations[3]}) < 0.03;
    check(encoded.success && ok, "SPZ external decoder preserves canonical xyzw rotation");
    std::filesystem::remove(path);
    return true;
}

bool test_spz_sh_channel_order() {
    printf("[test] SPZ SH coefficient/channel order\n");
    using namespace melkor;
    const std::vector<float> canonical{
        0.1f, 0.2f, 0.3f, 0.8f, -0.4f, -0.6f, 0.4f, -0.8f, 0.2f, 0.0f, 0.6f, -0.2f,
    };
    auto data = make_spz_data(1, canonical);
    std::vector<std::uint8_t> buffer;
    SpzEncodeConfig config;
    config.sh_degree = 1;
    const auto encoded = SpzEncoder{}.encodeToBuffer(buffer, data, config);
    spz::UnpackOptions options;
    options.to = spz::CoordinateSystem::RDF;
    const auto external =
        encoded.success ? spz::loadSpz(buffer.data(), static_cast<int32_t>(buffer.size()), options)
                        : spz::GaussianCloud{};
    bool external_ok = external.sh.size() == 9;
    for (std::size_t i = 0; external_ok && i < 9; ++i) {
        external_ok = std::abs(external.sh[i] - canonical[i + 3]) < 0.09f;
    }
    const auto decoded = encoded.success
                             ? SpzDecoder{}.decodeFromBuffer(buffer.data(), buffer.size())
                             : SpzDecoder::DecodeResult{};
    bool internal_ok = decoded.success && decoded.data.has_value() &&
                       decoded.data->sh().raw().size() == canonical.size();
    for (std::size_t i = 0; internal_ok && i < canonical.size(); ++i) {
        internal_ok = std::abs(decoded.data->sh().raw()[i] - canonical[i]) < 0.09f;
    }
    check(encoded.success && external_ok && internal_ok,
          "SPZ and SplatData share coefficient-major RGB-interleaved higher SH");
    return true;
}

bool test_spz_encoder_input_validation() {
    printf("[test] SPZ encoder canonical validation and reported adjustments\n");
    using namespace melkor;
    SpzEncoder encoder;
    std::vector<std::uint8_t> buffer = {1, 2, 3};

    SplatBufferInput empty_input;
    empty_input.sh = ShBuffer::black(0).value();
    auto empty = SplatData::create(std::move(empty_input)).value();
    check(!encoder.encodeToBuffer(buffer, empty).success && buffer.empty(),
          "SPZ encoder rejects empty canonical data and clears stale output");

    auto data = make_spz_data(0);
    SpzEncodeConfig config;
    config.sh_degree = -2;
    check(!encoder.encodeToBuffer(buffer, data, config).success,
          "SPZ encoder rejects degrees below the preserve-source sentinel");
    config.sh_degree = 4;
    check(!encoder.encodeToBuffer(buffer, data, config).success,
          "SPZ encoder rejects requested degree above three");

    auto range_edit = data.edit();
    range_edit.set_positions({{std::numeric_limits<float>::max(), 0.0f, 0.0f}});
    auto out_of_range = range_edit.commit().value();
    config.sh_degree = -1;
    check(!encoder.encodeToBuffer(buffer, out_of_range, config).success,
          "SPZ encoder rejects finite positions outside signed 24-bit fixed point");

    const std::string preserved_path =
        (std::filesystem::temp_directory_path() /
         ("melkor_spz_preserve_" + std::to_string(getpid()) + ".spz"))
            .string();
    {
        std::ofstream existing(preserved_path, std::ios::binary | std::ios::trunc);
        existing << "preserve-existing-output";
    }
    const auto invalid_file = encoder.encodeToFile(preserved_path, out_of_range, config);
    std::ifstream preserved_stream(preserved_path, std::ios::binary);
    const std::string preserved((std::istreambuf_iterator<char>(preserved_stream)),
                                std::istreambuf_iterator<char>());
    check(!invalid_file.success && preserved == "preserve-existing-output",
          "SPZ validation failure preserves an existing destination");
    std::filesystem::remove(preserved_path);

    auto degree_four = make_spz_data(4);
    check(!encoder.encodeToBuffer(buffer, degree_four, config).success,
          "SPZ v1-v3 rejects degree four rather than silently truncating");

    std::vector<float> degree_three_sh(16 * 3);
    for (std::size_t i = 0; i < degree_three_sh.size(); ++i) {
        degree_three_sh[i] = -0.8f + 0.025f * static_cast<float>(i);
    }
    auto degree_three = make_spz_data(3, degree_three_sh);
    config.sh_degree = 1;
    const auto truncated = encoder.encodeToBuffer(buffer, degree_three, config);
    spz::UnpackOptions options;
    options.to = spz::CoordinateSystem::RDF;
    const auto external =
        truncated.success
            ? spz::loadSpz(buffer.data(), static_cast<int32_t>(buffer.size()), options)
            : spz::GaussianCloud{};
    bool prefix_ok = external.sh.size() == 9;
    for (std::size_t i = 0; prefix_ok && i < 9; ++i) {
        prefix_ok = std::abs(external.sh[i] - degree_three_sh[i + 3]) < 0.09f;
    }
    check(truncated.success && prefix_ok && has_diagnostic(truncated, "MK1322_SPZ_SH_TRUNCATED"),
          "SPZ lower-degree export preserves canonical coefficient prefix and reports loss");

    SplatBufferInput endpoint_input;
    endpoint_input.positions.resize(2);
    endpoint_input.scales.assign(2, {0.1f, 0.1f, 0.1f});
    endpoint_input.rotations.resize(2);
    endpoint_input.opacities = {0.0f, 1.0f};
    endpoint_input.sh = ShBuffer::black(2).value();
    auto endpoints = SplatData::create(std::move(endpoint_input)).value();
    config.sh_degree = -1;
    const auto endpoint_encode = encoder.encodeToBuffer(buffer, endpoints, config);
    const auto endpoint_decode = endpoint_encode.success
                                     ? SpzDecoder{}.decodeFromBuffer(buffer.data(), buffer.size())
                                     : SpzDecoder::DecodeResult{};
    check(endpoint_encode.success &&
              has_diagnostic(endpoint_encode, "MK1321_SPZ_OPACITY_ENDPOINT_CLAMPED") &&
              endpoint_decode.success && endpoint_decode.data.has_value() &&
              endpoint_decode.data->opacities()[0] > 0.0f &&
              endpoint_decode.data->opacities()[1] < 1.0f,
          "SPZ opacity endpoint clamp is explicit and decodes to finite probabilities");
    return true;
}

bool test_spz_fractional_bits_validation() {
    printf("[test] SPZ fixed-point fractional-bit validation\n");
    using namespace melkor;

    auto make_spz = [](uint8_t fractional_bits) {
        // v3, one degree-0 point: 16-byte header followed by 20 bytes of
        // positions/alpha/color/scale/smallest-three rotation data.
        std::vector<uint8_t> unpacked(16 + 20, 0);
        auto write_le32 = [&](size_t offset, uint32_t value) {
            for (size_t byte = 0; byte < 4; ++byte) {
                unpacked[offset + byte] = static_cast<uint8_t>((value >> (byte * 8)) & 0xffu);
            }
        };
        write_le32(0, 0x5053474eu);
        write_le32(4, 3u);
        write_le32(8, 1u);
        unpacked[12] = 0;  // SH degree
        unpacked[13] = fractional_bits;

        std::vector<uint8_t> compressed;
        const bool compressed_ok =
            spz::compressGzipped(unpacked.data(), unpacked.size(), &compressed);
        check(compressed_ok, "crafted SPZ fixture compresses");
        return compressed;
    };

    const auto boundary = make_spz(23);
    const auto boundary_result = SpzDecoder{}.decodeFromBuffer(boundary.data(), boundary.size());
    check(boundary_result.success && boundary_result.data.has_value() &&
              boundary_result.data->size() == 1 &&
              std::isfinite(boundary_result.data->positions()[0].x),
          "23 fractional bits are accepted for a signed 24-bit coordinate");

    const auto invalid = make_spz(255);
    const auto invalid_result = SpzDecoder{}.decodeFromBuffer(invalid.data(), invalid.size());
    check(!invalid_result.success && !invalid_result.data.has_value(),
          "fractional-bit counts wider than the signed 24-bit field are rejected");
    return true;
}
#endif

// ---- Test 1c: enhanced converter pads short attribute arrays --------------
// Multi-primitive GLBs can carry colors/normals for only some primitives;
// the converter must pad rather than read out of bounds, and must not crash
// on degenerate single-point input.
bool test_enhanced_converter_short_attributes() {
    printf("[test] enhanced converter with short colors/normals\n");
    using namespace melkor;
    std::vector<float> positions;
    for (int i = 0; i < 8; ++i) {
        positions.push_back(static_cast<float>(i % 4));
        positions.push_back(static_cast<float>(i / 4));
        positions.push_back(0.0f);
    }
    // Colors and normals for only the first two points.
    std::vector<float> colors = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    std::vector<float> normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};

    EnhancedConverter converter(nullptr);  // CPU path
    EnhancedConversionConfig cfg;
    cfg.knn_neighbors = 3;
    auto result = converter.convertFromMesh(positions, normals, colors, {}, cfg);
    check(result.success && result.cloud.size() == 8, "short attributes: all points converted");
    bool finite = true;
    for (size_t i = 0; i < result.cloud.size(); ++i) {
        const auto& sp = result.cloud[i];
        if (!std::isfinite(sp.x) || !std::isfinite(sp.f_dc_0) || !std::isfinite(sp.scale_0) ||
            !std::isfinite(sp.rot_0))
            finite = false;
    }
    check(finite, "short attributes: all outputs finite");
    if (result.cloud.size() == 8) {
        // Points past the color array get the default color (0.5 -> SH DC 0).
        check(std::abs(result.cloud[5].f_dc_0) < 1e-5f && std::abs(result.cloud[5].f_dc_1) < 1e-5f,
              "short attributes: padded points use default color");
    }

    // Single point with explicit normal: degenerate extent must not crash
    // and must produce a finite splat.
    auto single = converter.convertFromMesh({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {}, cfg);
    check(single.success && single.cloud.size() == 1 && std::isfinite(single.cloud[0].scale_0),
          "single-point input yields one finite splat");

    // Grid coordinates must be relative to the cloud origin. Absolute hashing
    // casts a large finite translation to int (undefined behavior) even when
    // the local shape and distances are completely ordinary.
    auto translated =
        converter.convertFromMesh({1.0e30f, 0.0f, 0.0f, 1.0e30f, 1.0f, 0.0f}, {}, {}, {}, cfg);
    check(translated.success && translated.cloud.size() == 2 &&
              std::isfinite(translated.cloud[0].scale_0) &&
              std::isfinite(translated.cloud[1].scale_0),
          "large finite translations are hashed without integer overflow");
    return true;
}

// ---- Test 1d: enhanced glTF extraction validates untrusted accessors ------
bool test_enhanced_converter_accessor_validation() {
    printf("[test] enhanced converter glTF accessor validation\n");
    using namespace melkor;

    const auto base = std::filesystem::temp_directory_path() /
                      ("melkor_enhanced_accessor_" + std::to_string(getpid()));
    const auto gltf_path = base.string() + ".gltf";
    const auto bin_path = base.string() + ".bin";
    const auto bin_name = std::filesystem::path(bin_path).filename().string();

    // TinyGLTF accepts the container, but the accessor claims 100 VEC3 values
    // in a 12-byte buffer. The enhanced path must reject it before reading.
    {
        std::ofstream bin(bin_path, std::ios::binary);
        const float point[3] = {1.0f, 2.0f, 3.0f};
        bin.write(reinterpret_cast<const char*>(point), sizeof(point));
    }
    {
        std::ofstream gltf(gltf_path);
        gltf << "{\"asset\":{\"version\":\"2.0\"},"
                "\"buffers\":[{\"uri\":\""
             << bin_name
             << "\",\"byteLength\":12}],"
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                "\"count\":100,\"type\":\"VEC3\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    }

    EnhancedConverter converter(nullptr);
    EnhancedConversionConfig cfg;
    cfg.knn_neighbors = 1;
    auto malformed = converter.convertFromFile(gltf_path, cfg);
    check(!malformed.success && malformed.cloud.empty(),
          "oversized accessor span is rejected without producing splats");
    GlbReader basic_reader;
    auto malformed_basic = basic_reader.loadFromFile(gltf_path);
    check(!malformed_basic.success && !malformed_basic.data.has_value(),
          "basic reader rejects the same oversized accessor span");

    // Exercise alignment-safe scalar decoding with a valid point beginning at
    // byte offset 1. memcpy-based reads are defined even when the source is not
    // naturally aligned; a reinterpret_cast<float*> read is not.
    {
        std::ofstream bin(bin_path, std::ios::binary | std::ios::trunc);
        const char prefix = 0;
        const float point[3] = {1.0f, 2.0f, 3.0f};
        bin.write(&prefix, 1);
        bin.write(reinterpret_cast<const char*>(point), sizeof(point));
    }
    {
        std::ofstream gltf(gltf_path, std::ios::trunc);
        gltf << "{\"asset\":{\"version\":\"2.0\"},"
                "\"buffers\":[{\"uri\":\""
             << bin_name
             << "\",\"byteLength\":13}],"
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":1,\"byteLength\":12}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                "\"count\":1,\"type\":\"VEC3\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    }
    auto unaligned = converter.convertFromFile(gltf_path, cfg);
    check(unaligned.success && unaligned.cloud.size() == 1 &&
              std::abs(unaligned.cloud[0].x - 1.0f) < 1e-6f &&
              std::abs(unaligned.cloud[0].y + 3.0f) < 1e-6f &&
              std::abs(unaligned.cloud[0].z - 2.0f) < 1e-6f,
          "unaligned float accessor is decoded safely");

    auto basic_unaligned = basic_reader.loadFromFile(gltf_path);
    check(basic_unaligned.success && basic_unaligned.data.has_value() &&
              basic_unaligned.data->size() == 1 &&
              std::abs(basic_unaligned.data->positions()[0].x - 1.0f) < 1e-6f &&
              std::abs(basic_unaligned.data->scales()[0].x - 0.01f) < 1e-7f &&
              basic_unaligned.data->opacities()[0] == 1.0f &&
              basic_unaligned.data->rotations()[0].w == 1.0f &&
              std::abs(basic_unaligned.data->sh().dc(0, 0) - math::rgb_to_sh_dc(0.5f)) < 1e-6f,
          "basic reader produces canonical linear defaults from an unaligned accessor");
    auto null_memory = basic_reader.loadFromMemory(nullptr, 0);
    check(!null_memory.success, "null in-memory glTF input is rejected");
    const std::string external_memory_gltf =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"" +
        bin_name +
        "\",\"byteLength\":13}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":1,\"byteLength\":12}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
        "\"count\":1,\"type\":\"VEC3\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]}";
    auto inherited_root = basic_reader.loadFromMemory(
        reinterpret_cast<const uint8_t*>(external_memory_gltf.data()), external_memory_gltf.size());
    check(!inherited_root.success,
          "in-memory glTF cannot inherit an external-file root from a prior load");
    GlbConversionConfig invalid_cfg;
    invalid_cfg.default_scale = 0.0f;
    auto invalid_config = basic_reader.loadFromFile(gltf_path, invalid_cfg);
    check(!invalid_config.success, "invalid GLB conversion configuration is rejected");

    // Active-scene traversal must apply node TRS, preserve mesh instancing,
    // ignore inactive scenes, and transform normals with inverse transpose.
    {
        std::ofstream bin(bin_path, std::ios::binary | std::ios::trunc);
        const float values[6] = {1.0f, 2.0f, 3.0f, 1.0f, 1.0f, 0.0f};
        bin.write(reinterpret_cast<const char*>(values), sizeof(values));
    }
    {
        std::ofstream gltf(gltf_path, std::ios::trunc);
        gltf << "{\"asset\":{\"version\":\"2.0\"},"
                "\"buffers\":[{\"uri\":\""
             << bin_name
             << "\",\"byteLength\":24}],"
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12},"
                "{\"buffer\":0,\"byteOffset\":12,\"byteLength\":12}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":"
                "\"VEC3\"},"
                "{\"bufferView\":1,\"componentType\":5126,\"count\":1,\"type\":\"VEC3\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1}}]}],"
                "\"nodes\":[{\"mesh\":0,\"translation\":[10,0,0]},"
                "{\"mesh\":0,\"translation\":[0,5,0],\"scale\":[2,1,1]},"
                "{\"mesh\":0,\"translation\":[100,0,0]}],"
                "\"scenes\":[{\"nodes\":[0,1]},{\"nodes\":[2]}],\"scene\":0}";
    }
    auto transformed = converter.convertFromFile(gltf_path, cfg);
    check(transformed.success && transformed.cloud.size() == 2,
          "enhanced reader traverses active scene and preserves mesh instances");
    if (transformed.cloud.size() == 2) {
        check(std::abs(transformed.cloud[0].x - 11.0f) < 1e-5f &&
                  std::abs(transformed.cloud[0].y + 3.0f) < 1e-5f &&
                  std::abs(transformed.cloud[0].z - 2.0f) < 1e-5f &&
                  std::abs(transformed.cloud[1].x - 2.0f) < 1e-5f &&
                  std::abs(transformed.cloud[1].y + 3.0f) < 1e-5f &&
                  std::abs(transformed.cloud[1].z - 7.0f) < 1e-5f,
              "node translations and non-uniform scale affect positions");
        float rotation[9];
        utils::quatToRotationMatrix(transformed.cloud[1].rot_0, transformed.cloud[1].rot_1,
                                    transformed.cloud[1].rot_2, transformed.cloud[1].rot_3,
                                    rotation);
        check(std::abs(rotation[2] - 0.4472136f) < 1e-4f && std::abs(rotation[5]) < 1e-4f &&
                  std::abs(rotation[8] - 0.8944272f) < 1e-4f,
              "non-uniform node scale uses inverse-transpose normal transform");
    }
    auto transformed_basic = basic_reader.loadFromFile(gltf_path);
    check(transformed_basic.success && transformed_basic.data.has_value() &&
              transformed_basic.data->size() == 2 &&
              std::abs(transformed_basic.data->positions()[0].x - 11.0f) < 1e-5f &&
              std::abs(transformed_basic.data->positions()[1].z - 7.0f) < 1e-5f,
          "basic reader applies active-scene node transforms and instancing");
    if (transformed_basic.success && transformed_basic.data.has_value() &&
        transformed_basic.data->size() == 2) {
        const auto& q = transformed_basic.data->rotations()[1];
        const auto rotation = math::to_matrix({q.x, q.y, q.z, q.w});
        check(std::abs(rotation[2] - 0.4472136) < 1e-4 && std::abs(rotation[5]) < 1e-4 &&
                  std::abs(rotation[8] - 0.8944272) < 1e-4,
              "basic reader stores transformed normal orientation as canonical xyzw");
    }

    // A malformed primitive after a valid one must fail the complete asset;
    // enhanced conversion may not silently emit only the valid prefix.
    {
        std::ofstream gltf(gltf_path, std::ios::trunc);
        gltf << "{\"asset\":{\"version\":\"2.0\"},"
                "\"buffers\":[{\"uri\":\""
             << bin_name
             << "\",\"byteLength\":24}],"
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                "\"count\":1,\"type\":\"VEC3\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}},"
                "{\"attributes\":{\"POSITION\":99}}]}],"
                "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    }
    auto partial_enhanced = converter.convertFromFile(gltf_path, cfg);
    auto partial_basic = basic_reader.loadFromFile(gltf_path);
    check(!partial_enhanced.success && !partial_basic.success,
          "basic and enhanced readers reject a partially malformed mesh");

    const auto write_invalid_optional_attribute = [&](const char* attribute) {
        std::ofstream gltf(gltf_path, std::ios::trunc);
        gltf << "{\"asset\":{\"version\":\"2.0\"},"
                "\"buffers\":[{\"uri\":\""
             << bin_name
             << "\",\"byteLength\":24}],"
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                "\"count\":1,\"type\":\"VEC3\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\""
             << attribute
             << "\":99}}]}],"
                "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    };
    for (const char* attribute : {"COLOR_0", "NORMAL"}) {
        write_invalid_optional_attribute(attribute);
        const std::string message =
            std::string("malformed ") + attribute + " is rejected consistently by both readers";
        check(!converter.convertFromFile(gltf_path, cfg).success &&
                  !basic_reader.loadFromFile(gltf_path).success,
              message.c_str());
    }

    const auto write_graph_fixture = [&](const std::string& nodes,
                                         const std::string& scene = "{\"nodes\":[0]}") {
        std::ofstream gltf(gltf_path, std::ios::trunc);
        gltf << "{\"asset\":{\"version\":\"2.0\"},"
                "\"buffers\":[{\"uri\":\""
             << bin_name
             << "\",\"byteLength\":24}],"
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                "\"count\":1,\"type\":\"VEC3\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                "\"nodes\":"
             << nodes << ",\"scenes\":[" << scene << "],\"scene\":0}";
    };
    const auto graph_is_rejected = [&]() {
        return !converter.convertFromFile(gltf_path, cfg).success &&
               !basic_reader.loadFromFile(gltf_path).success;
    };

    write_graph_fixture("[{\"mesh\":0,\"children\":[1]},{\"children\":[0]}]");
    check(graph_is_rejected(), "cyclic node graphs are rejected by both readers");
    write_graph_fixture("[{\"mesh\":0,\"children\":[99]}]");
    check(graph_is_rejected(), "out-of-range child nodes are rejected by both readers");
    write_graph_fixture("[{\"mesh\":99}]");
    check(graph_is_rejected(), "out-of-range node meshes are rejected by both readers");

    std::filesystem::remove(gltf_path);
    std::filesystem::remove(bin_path);
    return true;
}

// ---- Test 2: header-driven PLY reader -------------------------------------
bool test_ply_reader() {
    printf("[test] header-driven PLY reader\n");
    using namespace melkor;

    PlyReader invalid_buffer_reader;
    check(!invalid_buffer_reader.readFromBuffer(nullptr, 0).success &&
              !invalid_buffer_reader.readFromBuffer(nullptr, 1).success,
          "null and empty PLY buffers fail closed");

    // 2a: classic 3DGS ascii.
    const char* hdr1 =
        "ply\nformat ascii 1.0\nelement vertex 1\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property float nx\nproperty float ny\nproperty float nz\n"
        "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
        "property float opacity\n"
        "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
        "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
        "end_header\n"
        "1 2 3 0 0 1 0.1 0.2 0.3 0.5 -1 -2 -3 0.707 0 0 0.707\n";
    std::string f1(hdr1);
    PlyReader r1;
    auto res1 = r1.readFromBuffer(reinterpret_cast<const uint8_t*>(f1.data()), f1.size());
    check(res1.success, "3DGS ascii: parse succeeds");
    if (res1.success && res1.data.has_value()) {
        const auto& position = res1.data->positions()[0];
        const auto& rotation = res1.data->rotations()[0];
        check(position.x == 1 && position.y == 2 && position.z == 3, "3DGS ascii: position");
        check(std::abs(rotation.z - 0.7071f) < 1e-3 && std::abs(rotation.w - 0.7071f) < 1e-3,
              "3DGS ascii: wxyz converted to canonical xyzw");
    }

    // 2b: non-3DGS PLY with red/green/blue uchar and no normals/scale/rot.
    const char* hdr2 = "ply\nformat ascii 1.0\nelement vertex 1\n"
                       "property float x\nproperty float y\nproperty float z\n"
                       "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                       "end_header\n"
                       "10 20 30 255 0 0\n";
    std::string f2(hdr2);
    PlyReader r2;
    auto res2 = r2.readFromBuffer(reinterpret_cast<const uint8_t*>(f2.data()), f2.size());
    check(res2.success, "rgb-only ascii: parse succeeds");
    if (res2.success && res2.data.has_value()) {
        const auto& position = res2.data->positions()[0];
        check(position.x == 10 && position.y == 20 && position.z == 30, "rgb-only ascii: position");
        const float expect_r = math::rgb_to_sh_dc(1.0f);
        check(std::abs(res2.data->sh().dc(0, 0) - expect_r) < 1e-2f,
              "rgb-only ascii: red mapped to SH DC");
        check(res2.data->rotations()[0].w == 1.0f,
              "rgb-only ascii: missing rotation defaults to identity");
    }

    // 2c: binary round-trip through the writer.
    SplatBufferInput c3_input;
    c3_input.positions.push_back({5.0f, 6.0f, 7.0f});
    c3_input.scales.push_back({1.0f, 1.0f, 1.0f});
    c3_input.rotations.push_back({});
    c3_input.opacities.push_back(0.5f);
    c3_input.sh = ShBuffer::black(1).value();
    auto c3 = SplatData::create(std::move(c3_input)).value();
    PlyWriter w;
    std::vector<uint8_t> buf;
    PlyWriteConfig cfg;
    cfg.format = PlyFormat::Binary;
    auto wres = w.writeToBuffer(buf, c3, cfg);
    check(wres.success, "binary write");
    PlyReader r3;
    auto res3 = r3.readFromBuffer(buf.data(), buf.size());
    check(res3.success, "binary round-trip: parse succeeds");
    if (res3.success && res3.data.has_value()) {
        const auto& position = res3.data->positions()[0];
        check(std::abs(position.x - 5) < 1e-5f && std::abs(position.y - 6) < 1e-5f &&
                  std::abs(position.z - 7) < 1e-5f,
              "binary round-trip: position preserved");
    }
    return g_failures == 0;
}

// ---- Test 3: PCA normal estimation ----------------------------------------
bool test_pca_normals() {
    printf("[test] PCA normal estimation on a sphere\n");
    using namespace melkor;
    // Sample a sphere; PCA normals should align with the radial direction
    // (point - center), since the local tangent plane is perpendicular to it.
    std::vector<float> positions;
    const int n = 20;
    const float radius = 5.0f;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            float theta = static_cast<float>(i) / n * 3.14159265f;
            float phi = static_cast<float>(j) / n * 2.0f * 3.14159265f;
            positions.push_back(radius * std::sin(theta) * std::cos(phi));
            positions.push_back(radius * std::sin(theta) * std::sin(phi));
            positions.push_back(radius * std::cos(theta));
        }
    }
    auto normals = melkor::enhanced::estimateNormals(positions, 8);
    size_t num = positions.size() / 3;
    int aligned = 0;
    for (size_t i = 0; i < num; ++i) {
        float ex = positions[i * 3 + 0], ey = positions[i * 3 + 1], ez = positions[i * 3 + 2];
        float el = std::sqrt(ex * ex + ey * ey + ez * ez);
        ex /= el;
        ey /= el;
        ez /= el;
        float nx = normals[i * 3 + 0], ny = normals[i * 3 + 1], nz = normals[i * 3 + 2];
        float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-6f)
            continue;
        nx /= nl;
        ny /= nl;
        nz /= nl;
        if (ex * nx + ey * ny + ez * nz > 0.9f)
            ++aligned;
    }
    float pct = 100.0f * aligned / static_cast<float>(num);
    printf("    aligned %d/%zu (%.1f%%)\n", aligned, num, pct);
    check(pct > 80.0f, ">=80% normals align with radial direction");
    return g_failures == 0;
}

// ---- Test 4: SH C0 constant round-trip ------------------------------------
bool test_sh_constant() {
    printf("[test] SH C0 rgb<->shdc invertibility\n");
    using namespace melkor;
    bool ok = true;
    const float samples[] = {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f};
    for (float v : samples) {
        float round = utils::shDcToRgb(utils::rgbToShDc(v));
        if (std::abs(round - v) > 1e-5f) {
            ok = false;
            printf("    drift at v=%.3f: round=%.6f\n", v, round);
        }
    }
    check(ok, "rgbToShDc and shDcToRgb are inverses");
    return ok;
}
// ---- Test 5: logit/sigmoid round-trip and edge safety --------------------
bool test_logit_sigmoid() {
    printf("[test] logit/sigmoid round-trip and edge safety\n");
    using namespace melkor;
    bool ok = true;
    // Round-trip: sigmoid(logit(x)) ≈ x for mid-range values
    const float samples[] = {0.01f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 0.99f};
    for (float v : samples) {
        float round = utils::sigmoid(utils::logit(v));
        if (std::abs(round - v) > 1e-4f) {
            ok = false;
            printf("    drift at v=%.4f: round=%.6f\n", v, round);
        }
    }
    check(ok, "sigmoid(logit(x)) round-trips for mid-range values");

    // Edge safety: logit(0) and logit(1) must not produce NaN or inf
    bool edge_ok = true;
    float l0 = utils::logit(0.0f);
    float l1 = utils::logit(1.0f);
    if (std::isnan(l0) || std::isinf(l0)) {
        edge_ok = false;
        printf("    logit(0) = %f\n", l0);
    }
    if (std::isnan(l1) || std::isinf(l1)) {
        edge_ok = false;
        printf("    logit(1) = %f\n", l1);
    }
    check(edge_ok, "logit(0) and logit(1) are finite (no div-by-zero)");

    // Sigmoid stability for extreme values: must not produce NaN/inf.
    // For float precision, sigmoid(-100) underflows to exactly 0.0f — that's
    // correct behavior, not a bug.
    bool sig_ok = true;
    float s_big = utils::sigmoid(100.0f);
    float s_neg = utils::sigmoid(-100.0f);
    if (std::isnan(s_big) || std::isinf(s_big)) {
        sig_ok = false;
        printf("    sigmoid(100) = %f\n", s_big);
    }
    if (std::isnan(s_neg) || std::isinf(s_neg)) {
        sig_ok = false;
        printf("    sigmoid(-100) = %f\n", s_neg);
    }
    if (s_big < 0.99f || s_big > 1.0f) {
        sig_ok = false;
        printf("    sigmoid(100) = %f (expected ~1)\n", s_big);
    }
    if (s_neg < 0.0f || s_neg > 0.01f) {
        sig_ok = false;
        printf("    sigmoid(-100) = %f (expected ~0)\n", s_neg);
    }
    check(sig_ok, "sigmoid is stable for extreme values");
    return ok && edge_ok && sig_ok;
}
// ---- Test 6: empty cloud bounding box safety -----------------------------
bool test_empty_bounding_box() {
    printf("[test] empty cloud bounding box safety\n");
    using namespace melkor;
    GaussianCloud empty;
    float minX = -999, minY = -999, minZ = -999;
    float maxX = -999, maxY = -999, maxZ = -999;
    empty.computeBoundingBox(minX, minY, minZ, maxX, maxY, maxZ);
    check(minX == 0.0f && minY == 0.0f && minZ == 0.0f, "empty bbox min is zero");
    check(maxX == 0.0f && maxY == 0.0f && maxZ == 0.0f, "empty bbox max is zero");
    return g_failures == 0;
}

// ---- Test 7: truncated binary PLY rejection -------------------------------
bool test_truncated_ply() {
    printf("[test] truncated binary PLY rejection\n");
    using namespace melkor;
    std::string header =
        "ply\nformat binary_little_endian 1.0\n"
        "element vertex 100\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property float nx\nproperty float ny\nproperty float nz\n"
        "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
        "property float opacity\n"
        "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
        "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
        "end_header\n";
    std::vector<uint8_t> buf(header.begin(), header.end());
    std::vector<float> one_vertex(17, 1.0f);
    const auto* raw = reinterpret_cast<const uint8_t*>(one_vertex.data());
    buf.insert(buf.end(), raw, raw + one_vertex.size() * sizeof(float));

    PlyReader r;
    auto res = r.readFromBuffer(buf.data(), buf.size());
    check(!res.success, "truncated binary PLY is rejected");
    return g_failures == 0;
}

}  // namespace

int main() {
    test_sh_constant();
    test_logit_sigmoid();
    test_empty_bounding_box();
    test_truncated_ply();
    test_ply_reader();
    test_pca_normals();
    test_enhanced_converter_short_attributes();
    test_enhanced_converter_accessor_validation();
#ifdef MELKOR_HAS_SPZ
    test_spz_quaternion_order();
    test_spz_sh_channel_order();
    test_spz_encoder_input_validation();
    test_spz_fractional_bits_validation();
#else
    printf("[skip] SPZ tests (built without MELKOR_HAS_SPZ)\n");
#endif
    printf("\n=== %s (%d failures) ===\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES",
           g_failures);
    return g_failures == 0 ? 0 : 1;
}
