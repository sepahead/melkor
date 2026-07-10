// Melkor core correctness tests.
//
// These are deliberately self-contained (no external test framework): each
// case builds up an in-memory input, runs a Melkor core routine, and asserts a
// geometric/encoding property. They exist because the previous CI "test" step
// was only `melkor --info`, which exercises no actual conversion logic.
//
// Coverage:
//   1. SPZ quaternion round-trip against the canonical spz decoder (proves the
//      xyzw/wxyz reordering is correct and not a symmetric double-bug).
//   2. Header-driven PLY reader against three layouts: 3DGS ascii, plain
//      red/green/blue ascii, and melkor-written binary.
//   3. PCA normal estimation on a sphere surface (normals should align with the
//      radial direction).
//   4. SH C0 constant round-trip (rgbToShDc / shDcToRgb are inverses).

#include "melkor/enhanced_converter.hpp"
#include "melkor/gaussian_data.hpp"
#include "melkor/glb_reader.hpp"
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

// ---- Test 1: SPZ quaternion order -----------------------------------------
#ifdef MELKOR_HAS_SPZ
bool test_spz_quaternion_order() {
    printf("[test] SPZ quaternion order against canonical decoder\n");
    using namespace melkor;
    GaussianCloud cloud;
    GaussianSplat s{};
    s.x = s.y = s.z = 0.0f;
    s.f_dc_0 = s.f_dc_1 = s.f_dc_2 = 0.0f;
    s.opacity = 5.0f;
    s.scale_0 = s.scale_1 = s.scale_2 = -2.0f;
    // Non-symmetric quaternion (all four components differ) so any permutation
    // changes the rotation: w=0.8, x=0.4, y=0.2, z=0.4 (normalized below).
    float w = 0.8f, x = 0.4f, y = 0.2f, z = 0.4f;
    float n = std::sqrt(w * w + x * x + y * y + z * z);
    w /= n; x /= n; y /= n; z /= n;
    s.rot_0 = w; s.rot_1 = x; s.rot_2 = y; s.rot_3 = z;
    cloud.addSplat(s);
    cloud.setShDegree(0);

    // Unique per-process path so concurrent CI runners (or different users
    // sharing /tmp) cannot collide; removed on every exit path below.
    const std::string spz_path =
        (std::filesystem::temp_directory_path() /
         ("melkor_test_quat_" + std::to_string(getpid()) + ".spz")).string();

    SpzEncoder enc;
    SpzEncodeConfig cfg;
    auto res = enc.encodeToFile(spz_path, cloud, cfg);
    if (!res.success) {
        check(false, "encode to file");
        std::filesystem::remove(spz_path);
        return false;
    }

    // Decode with spz's own loader (NOT melkor's) so a symmetric double-bug
    // cannot hide: melkor stores w-first, spz expects xyzw.
    spz::UnpackOptions uo;
    uo.to = spz::CoordinateSystem::RDF;
    auto spz_cloud = spz::loadSpz(spz_path, uo);
    if (spz_cloud.numPoints != 1) {
        check(false, "spz decoded point count");
        std::filesystem::remove(spz_path);
        return false;
    }
    // spz rotations are xyzw; melkor wrote (w,x,y,z), so after reordering we
    // expect the same values back within quantization tolerance (9-bit for the
    // smallest-three encoding).
    float gx = spz_cloud.rotations[0], gy = spz_cloud.rotations[1];
    float gz = spz_cloud.rotations[2], gw = spz_cloud.rotations[3];
    const float tol = 2e-2f;  // accounts for 9-bit quaternion quantization
    check(std::abs(gx - x) < tol && std::abs(gy - y) < tol &&
          std::abs(gz - z) < tol && std::abs(gw - w) < tol,
          "spz decoded quaternion matches input (xyzw order)");
    std::filesystem::remove(spz_path);
    return true;
}

// ---- Test 1b: SPZ SH-rest channel order against canonical decoder ---------
// Melkor stores sh_rest channel-major (all R coefficients, then G, then B —
// the 3DGS PLY convention); SPZ interleaves the channel as the fastest axis.
// Decode with spz's own loader so a symmetric transpose double-bug in
// melkor's encode+decode cannot hide.
bool test_spz_sh_channel_order() {
    printf("[test] SPZ SH-rest channel order against canonical decoder\n");
    using namespace melkor;
    GaussianCloud cloud;
    GaussianSplat s{};
    s.opacity = 5.0f;
    s.scale_0 = s.scale_1 = s.scale_2 = -2.0f;
    s.rot_0 = 1.0f;
    // Degree 1: 3 coefficients x 3 channels, channel-major. All nine values
    // pairwise distinct by >= 0.2 so any mis-ordering exceeds the
    // quantization tolerance below.
    const float r[3] = {0.8f, 0.4f, 0.0f};
    const float g[3] = {-0.4f, -0.8f, 0.6f};
    const float b[3] = {-0.6f, 0.2f, -0.2f};
    for (float v : r) s.sh_rest.push_back(v);
    for (float v : g) s.sh_rest.push_back(v);
    for (float v : b) s.sh_rest.push_back(v);
    cloud.addSplat(s);
    cloud.setShDegree(1);

    const std::string spz_path =
        (std::filesystem::temp_directory_path() /
         ("melkor_test_sh_" + std::to_string(getpid()) + ".spz")).string();

    SpzEncoder enc;
    SpzEncodeConfig cfg;
    cfg.sh_degree = 1;
    if (!enc.encodeToFile(spz_path, cloud, cfg).success) {
        check(false, "SH encode to file");
        std::filesystem::remove(spz_path);
        return false;
    }

    spz::UnpackOptions uo;
    uo.to = spz::CoordinateSystem::RDF;
    auto spz_cloud = spz::loadSpz(spz_path, uo);
    if (spz_cloud.numPoints != 1 || spz_cloud.sh.size() != 9) {
        check(false, "spz decoded SH size");
        std::filesystem::remove(spz_path);
        return false;
    }
    // Expected SPZ layout: c0r c0g c0b, c1r c1g c1b, c2r c2g c2b.
    const float expected[9] = {r[0], g[0], b[0], r[1], g[1], b[1],
                               r[2], g[2], b[2]};
    const float tol = 0.09f;  // 8-bit SH quantization
    bool interleaved_ok = true;
    for (int i = 0; i < 9; ++i) {
        if (std::abs(spz_cloud.sh[i] - expected[i]) > tol) interleaved_ok = false;
    }
    check(interleaved_ok, "spz stores SH channel-interleaved (c0 rgb, c1 rgb, ...)");

    // Melkor's own decoder must transpose back to channel-major.
    SpzDecoder dec;
    check(!dec.decodeFromBuffer(nullptr, 0).success &&
          !dec.decodeFromBuffer(nullptr, 1).success,
          "SPZ decoder rejects null and empty buffers");
    const uint8_t dummy = 0;
    check(!dec.decodeFromBuffer(
              &dummy, static_cast<size_t>(std::numeric_limits<int32_t>::max()) + 1).success,
          "SPZ decoder rejects sizes that would narrow to int32");
    auto round = dec.decodeFromFile(spz_path);
    bool round_ok = round.success && round.cloud.size() == 1 &&
                    round.cloud[0].sh_rest.size() == 9;
    if (round_ok) {
        for (int i = 0; i < 9; ++i) {
            if (std::abs(round.cloud[0].sh_rest[i] - s.sh_rest[i]) > tol)
                round_ok = false;
        }
    }
    check(round_ok, "melkor decode restores channel-major sh_rest");
    std::filesystem::remove(spz_path);
    return true;
}

bool test_spz_encoder_input_validation() {
    printf("[test] SPZ encoder rejects unsafe public-API inputs\n");
    using namespace melkor;

    SpzEncoder encoder;
    SpzEncodeConfig config;
    std::vector<uint8_t> buffer = {1, 2, 3};

    GaussianCloud empty;
    auto empty_result = encoder.encodeToBuffer(buffer, empty, config);
    check(!empty_result.success && buffer.empty(),
          "SPZ encoder rejects an empty cloud and clears stale output");

    GaussianCloud cloud;
    GaussianSplat splat{};
    splat.opacity = 0.0f;
    splat.scale_0 = splat.scale_1 = splat.scale_2 = -2.0f;
    splat.rot_0 = 1.0f;
    cloud.addSplat(splat);
    cloud.setShDegree(0);

    config.sh_degree = -1;
    buffer = {9};
    auto negative_degree = encoder.encodeToBuffer(buffer, cloud, config);
    check(!negative_degree.success && buffer.empty() &&
              negative_degree.error_message.find("requested SH degree") != std::string::npos,
          "SPZ encoder rejects a negative requested SH degree");

    config.sh_degree = 4;
    auto high_degree = encoder.encodeToBuffer(buffer, cloud, config);
    check(!high_degree.success &&
              high_degree.error_message.find("requested SH degree") != std::string::npos,
          "SPZ encoder rejects a requested SH degree above three");

    config.sh_degree = 0;
    cloud[0].x = std::numeric_limits<float>::quiet_NaN();
    auto nonfinite = encoder.encodeToBuffer(buffer, cloud, config);
    check(!nonfinite.success &&
              nonfinite.error_message.find("nonfinite_position") != std::string::npos,
          "SPZ encoder rejects non-finite positions before integer quantization");

    const std::string preserved_path =
        (std::filesystem::temp_directory_path() /
         ("melkor_spz_preserve_" + std::to_string(getpid()) + ".spz")).string();
    {
        std::ofstream existing(preserved_path, std::ios::binary | std::ios::trunc);
        existing << "preserve-existing-output";
    }
    auto invalid_file = encoder.encodeToFile(preserved_path, cloud, config);
    std::ifstream preserved_stream(preserved_path, std::ios::binary);
    const std::string preserved((std::istreambuf_iterator<char>(preserved_stream)),
                                std::istreambuf_iterator<char>());
    check(!invalid_file.success && preserved == "preserve-existing-output",
          "SPZ validation failure preserves an existing destination file");
    std::filesystem::remove(preserved_path);
    cloud[0].x = 0.0f;

    cloud[0].x = std::numeric_limits<float>::max();
    auto position_range = encoder.encodeToBuffer(buffer, cloud, config);
    check(!position_range.success &&
              position_range.error_message.find("24-bit fixed-point range") != std::string::npos,
          "SPZ encoder rejects finite positions that would overflow or wrap quantization");
    cloud[0].x = 0.0f;

    cloud[0].rot_0 = 0.0f;
    auto zero_rotation = encoder.encodeToBuffer(buffer, cloud, config);
    check(!zero_rotation.success &&
              zero_rotation.error_message.find("zero_quaternion") != std::string::npos,
          "SPZ encoder rejects a zero quaternion");
    cloud[0].rot_0 = 1.0f;

    cloud.setShDegree(1);
    config.sh_degree = 1;
    auto short_sh = encoder.encodeToBuffer(buffer, cloud, config);
    check(!short_sh.success &&
              short_sh.error_message.find("sh_count_mismatch") != std::string::npos,
          "SPZ encoder rejects SH arrays that do not match the cloud degree");

    cloud[0].sh_rest.assign(9, 0.0f);
    cloud[0].sh_rest[0] = std::numeric_limits<float>::max();
    auto unsafe_sh = encoder.encodeToBuffer(buffer, cloud, config);
    check(!unsafe_sh.success &&
              unsafe_sh.error_message.find("safe quantization range") != std::string::npos,
          "SPZ encoder rejects finite SH values that would overflow float-to-int conversion");

    // Lower-degree export must retain the first coefficients from each source
    // channel, not treat the shortened destination stride as the source stride.
    GaussianCloud degree_three;
    GaussianSplat degree_three_splat = splat;
    degree_three_splat.sh_rest.resize(45);
    for (int coefficient = 0; coefficient < 15; ++coefficient) {
        degree_three_splat.sh_rest[coefficient] = -0.8f + 0.03f * coefficient;
        degree_three_splat.sh_rest[15 + coefficient] = -0.2f + 0.02f * coefficient;
        degree_three_splat.sh_rest[30 + coefficient] = 0.3f - 0.01f * coefficient;
    }
    degree_three.addSplat(degree_three_splat);
    degree_three.setShDegree(3);
    config.sh_degree = 1;
    auto truncated_degree = encoder.encodeToBuffer(buffer, degree_three, config);
    spz::UnpackOptions unpack_options;
    unpack_options.to = spz::CoordinateSystem::RDF;
    const auto truncated_cloud = truncated_degree.success
        ? spz::loadSpz(buffer.data(), static_cast<int32_t>(buffer.size()), unpack_options)
        : spz::GaussianCloud{};
    bool truncated_channels_ok = truncated_cloud.numPoints == 1 &&
                                 truncated_cloud.shDegree == 1 &&
                                 truncated_cloud.sh.size() == 9;
    if (truncated_channels_ok) {
        const float expected[9] = {
            degree_three_splat.sh_rest[0], degree_three_splat.sh_rest[15],
            degree_three_splat.sh_rest[30], degree_three_splat.sh_rest[1],
            degree_three_splat.sh_rest[16], degree_three_splat.sh_rest[31],
            degree_three_splat.sh_rest[2], degree_three_splat.sh_rest[17],
            degree_three_splat.sh_rest[32],
        };
        for (int coefficient = 0; coefficient < 9; ++coefficient) {
            if (std::abs(truncated_cloud.sh[coefficient] - expected[coefficient]) > 0.09f) {
                truncated_channels_ok = false;
            }
        }
    }
    check(truncated_degree.success && truncated_channels_ok,
          "SPZ lower-degree export preserves source channel strides");

    cloud[0].sh_rest.clear();
    cloud.setShDegree(4);
    config.sh_degree = 3;
    auto invalid_cloud_degree = encoder.encodeToBuffer(buffer, cloud, config);
    check(!invalid_cloud_degree.success &&
              invalid_cloud_degree.error_message.find("cloud SH degree") != std::string::npos,
          "SPZ encoder rejects an invalid cloud SH degree");

    cloud.setShDegree(0);
    config.sh_degree = 0;
    cloud[0].rot_0 = cloud[0].rot_1 = cloud[0].rot_2 = cloud[0].rot_3 =
        std::numeric_limits<float>::max();
    auto valid = encoder.encodeToBuffer(buffer, cloud, config);
    const auto normalized_cloud = valid.success
        ? spz::loadSpz(buffer.data(), static_cast<int32_t>(buffer.size()), unpack_options)
        : spz::GaussianCloud{};
    bool normalized_finite = normalized_cloud.numPoints == 1 &&
                             normalized_cloud.rotations.size() == 4;
    if (normalized_finite) {
        for (float component : normalized_cloud.rotations) {
            normalized_finite &= std::isfinite(component);
        }
    }
    check(valid.success && !buffer.empty() && normalized_finite,
          "SPZ encoder safely normalizes a finite large quaternion");

    GaussianCloud alpha_endpoints;
    for (const float probability : {0.0f, 1.0f}) {
        GaussianSplat endpoint = splat;
        endpoint.opacity = utils::logit(probability);
        alpha_endpoints.addSplat(endpoint);
    }
    alpha_endpoints.setShDegree(0);
    auto endpoint_encode = encoder.encodeToBuffer(buffer, alpha_endpoints, config);
    auto endpoint_decode = endpoint_encode.success
        ? SpzDecoder{}.decodeFromBuffer(buffer.data(), buffer.size())
        : SpzDecoder::DecodeResult{};
    const bool finite_endpoints = endpoint_decode.success &&
                                  endpoint_decode.cloud.size() == 2 &&
                                  std::isfinite(endpoint_decode.cloud[0].opacity) &&
                                  std::isfinite(endpoint_decode.cloud[1].opacity);
    check(finite_endpoints,
          "SPZ alpha bytes 0 and 255 decode to finite endpoint logits");
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
                unpacked[offset + byte] =
                    static_cast<uint8_t>((value >> (byte * 8)) & 0xffu);
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
    const auto boundary_result =
        SpzDecoder{}.decodeFromBuffer(boundary.data(), boundary.size());
    check(boundary_result.success && boundary_result.cloud.size() == 1 &&
              std::isfinite(boundary_result.cloud[0].x),
          "23 fractional bits are accepted for a signed 24-bit coordinate");

    const auto invalid = make_spz(255);
    const auto invalid_result =
        SpzDecoder{}.decodeFromBuffer(invalid.data(), invalid.size());
    check(!invalid_result.success && invalid_result.cloud.empty(),
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
    check(result.success && result.cloud.size() == 8,
          "short attributes: all points converted");
    bool finite = true;
    for (size_t i = 0; i < result.cloud.size(); ++i) {
        const auto& sp = result.cloud[i];
        if (!std::isfinite(sp.x) || !std::isfinite(sp.f_dc_0) ||
            !std::isfinite(sp.scale_0) || !std::isfinite(sp.rot_0))
            finite = false;
    }
    check(finite, "short attributes: all outputs finite");
    if (result.cloud.size() == 8) {
        // Points past the color array get the default color (0.5 -> SH DC 0).
        check(std::abs(result.cloud[5].f_dc_0) < 1e-5f &&
              std::abs(result.cloud[5].f_dc_1) < 1e-5f,
              "short attributes: padded points use default color");
    }

    // Single point with explicit normal: degenerate extent must not crash
    // and must produce a finite splat.
    auto single = converter.convertFromMesh({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f},
                                            {}, {}, cfg);
    check(single.success && single.cloud.size() == 1 &&
          std::isfinite(single.cloud[0].scale_0),
          "single-point input yields one finite splat");

    // Grid coordinates must be relative to the cloud origin. Absolute hashing
    // casts a large finite translation to int (undefined behavior) even when
    // the local shape and distances are completely ordinary.
    auto translated = converter.convertFromMesh(
        {1.0e30f, 0.0f, 0.0f, 1.0e30f, 1.0f, 0.0f}, {}, {}, {}, cfg);
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
                "\"buffers\":[{\"uri\":\"" << bin_name << "\",\"byteLength\":12}],"
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
    check(!malformed_basic.success && malformed_basic.cloud.empty(),
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
                "\"buffers\":[{\"uri\":\"" << bin_name << "\",\"byteLength\":13}],"
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
    check(basic_unaligned.success && basic_unaligned.cloud.size() == 1 &&
          std::abs(basic_unaligned.cloud[0].x - 1.0f) < 1e-6f,
          "basic reader decodes an unaligned float accessor safely");
    auto null_memory = basic_reader.loadFromMemory(nullptr, 0);
    check(!null_memory.success, "null in-memory glTF input is rejected");
    const std::string external_memory_gltf =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"" + bin_name + "\",\"byteLength\":13}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":1,\"byteLength\":12}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
        "\"count\":1,\"type\":\"VEC3\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]}";
    auto inherited_root = basic_reader.loadFromMemory(
        reinterpret_cast<const uint8_t*>(external_memory_gltf.data()),
        external_memory_gltf.size());
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
                "\"buffers\":[{\"uri\":\"" << bin_name << "\",\"byteLength\":24}],"
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12},"
                "{\"buffer\":0,\"byteOffset\":12,\"byteLength\":12}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"VEC3\"},"
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
        utils::quatToRotationMatrix(transformed.cloud[1].rot_0,
                                    transformed.cloud[1].rot_1,
                                    transformed.cloud[1].rot_2,
                                    transformed.cloud[1].rot_3, rotation);
        check(std::abs(rotation[2] - 0.4472136f) < 1e-4f &&
              std::abs(rotation[5]) < 1e-4f &&
              std::abs(rotation[8] - 0.8944272f) < 1e-4f,
              "non-uniform node scale uses inverse-transpose normal transform");
    }
    auto transformed_basic = basic_reader.loadFromFile(gltf_path);
    check(transformed_basic.success && transformed_basic.cloud.size() == 2 &&
          std::abs(transformed_basic.cloud[0].x - 11.0f) < 1e-5f &&
          std::abs(transformed_basic.cloud[1].z - 7.0f) < 1e-5f,
          "basic reader applies active-scene node transforms and instancing");

    // A malformed primitive after a valid one must fail the complete asset;
    // enhanced conversion may not silently emit only the valid prefix.
    {
        std::ofstream gltf(gltf_path, std::ios::trunc);
        gltf << "{\"asset\":{\"version\":\"2.0\"},"
                "\"buffers\":[{\"uri\":\"" << bin_name << "\",\"byteLength\":24}],"
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
                "\"buffers\":[{\"uri\":\"" << bin_name << "\",\"byteLength\":24}],"
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                "\"count\":1,\"type\":\"VEC3\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\""
             << attribute << "\":99}}]}],"
                "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    };
    for (const char* attribute : {"COLOR_0", "NORMAL"}) {
        write_invalid_optional_attribute(attribute);
        const std::string message = std::string("malformed ") + attribute +
                                    " is rejected consistently by both readers";
        check(!converter.convertFromFile(gltf_path, cfg).success &&
                  !basic_reader.loadFromFile(gltf_path).success,
              message.c_str());
    }

    const auto write_graph_fixture = [&](const std::string& nodes,
                                         const std::string& scene = "{\"nodes\":[0]}") {
        std::ofstream gltf(gltf_path, std::ios::trunc);
        gltf << "{\"asset\":{\"version\":\"2.0\"},"
                "\"buffers\":[{\"uri\":\"" << bin_name << "\",\"byteLength\":24}],"
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                "\"count\":1,\"type\":\"VEC3\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                "\"nodes\":" << nodes << ",\"scenes\":[" << scene << "],\"scene\":0}";
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
    if (res1.success) {
        const auto& s = res1.cloud[0];
        check(s.x == 1 && s.y == 2 && s.z == 3, "3DGS ascii: position");
        check(std::abs(s.rot_0 - 0.707f) < 1e-3 && std::abs(s.rot_3 - 0.707f) < 1e-3,
              "3DGS ascii: quaternion (last two components)");
    }

    // 2b: non-3DGS PLY with red/green/blue uchar and no normals/scale/rot.
    const char* hdr2 =
        "ply\nformat ascii 1.0\nelement vertex 1\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
        "end_header\n"
        "10 20 30 255 0 0\n";
    std::string f2(hdr2);
    PlyReader r2;
    auto res2 = r2.readFromBuffer(reinterpret_cast<const uint8_t*>(f2.data()), f2.size());
    check(res2.success, "rgb-only ascii: parse succeeds");
    if (res2.success) {
        const auto& s = res2.cloud[0];
        check(s.x == 10 && s.y == 20 && s.z == 30, "rgb-only ascii: position");
        float expect_r = utils::rgbToShDc(1.0f);
        check(std::abs(s.f_dc_0 - expect_r) < 1e-2f, "rgb-only ascii: red mapped to SH DC");
        check(s.rot_0 == 1.0f, "rgb-only ascii: missing rotation defaults to identity");
    }

    // 2c: binary round-trip through the writer.
    GaussianCloud c3;
    GaussianSplat s3{};
    s3.x = 5; s3.y = 6; s3.z = 7;
    s3.opacity = 0.0f;
    s3.rot_0 = 1.0f;
    c3.addSplat(s3);
    PlyWriter w;
    std::vector<uint8_t> buf;
    PlyWriteConfig cfg;
    cfg.format = PlyFormat::Binary;
    auto wres = w.writeToBuffer(buf, c3, cfg);
    check(wres.success, "binary write");
    PlyReader r3;
    auto res3 = r3.readFromBuffer(buf.data(), buf.size());
    check(res3.success, "binary round-trip: parse succeeds");
    if (res3.success) {
        const auto& s = res3.cloud[0];
        check(std::abs(s.x - 5) < 1e-5f && std::abs(s.y - 6) < 1e-5f && std::abs(s.z - 7) < 1e-5f,
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
        ex /= el; ey /= el; ez /= el;
        float nx = normals[i * 3 + 0], ny = normals[i * 3 + 1], nz = normals[i * 3 + 2];
        float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-6f) continue;
        nx /= nl; ny /= nl; nz /= nl;
        if (ex * nx + ey * ny + ez * nz > 0.9f) ++aligned;
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
    if (std::isnan(l0) || std::isinf(l0)) { edge_ok = false; printf("    logit(0) = %f\n", l0); }
    if (std::isnan(l1) || std::isinf(l1)) { edge_ok = false; printf("    logit(1) = %f\n", l1); }
    check(edge_ok, "logit(0) and logit(1) are finite (no div-by-zero)");

    // Sigmoid stability for extreme values: must not produce NaN/inf.
    // For float precision, sigmoid(-100) underflows to exactly 0.0f — that's
    // correct behavior, not a bug.
    bool sig_ok = true;
    float s_big = utils::sigmoid(100.0f);
    float s_neg = utils::sigmoid(-100.0f);
    if (std::isnan(s_big) || std::isinf(s_big)) { sig_ok = false; printf("    sigmoid(100) = %f\n", s_big); }
    if (std::isnan(s_neg) || std::isinf(s_neg)) { sig_ok = false; printf("    sigmoid(-100) = %f\n", s_neg); }
    if (s_big < 0.99f || s_big > 1.0f) { sig_ok = false; printf("    sigmoid(100) = %f (expected ~1)\n", s_big); }
    if (s_neg < 0.0f || s_neg > 0.01f) { sig_ok = false; printf("    sigmoid(-100) = %f (expected ~0)\n", s_neg); }
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
    printf("\n=== %s (%d failures) ===\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
