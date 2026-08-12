// Tests for the glTF KHR_gaussian_splatting per-primitive reader.
//
// A small splat primitive is built by hand -- a byte buffer plus the glTF JSON that describes it --
// and read into a SplatData. The load-bearing check is the spherical-harmonic transpose: KHR stores
// one accessor per coefficient (coefficient-major across splats), and the scene model stores
// splat-major per-splat blocks, so a degree-1 case with distinct per-coefficient values pins that
// the reader puts every value where it belongs. The rest are structural rejections (wrong mode,
// wrong kernel, a missing attribute, a partial SH degree).
//
// Self-contained (no external test framework).

#include "melkor/format/gltf_reader.hpp"

#include "melkor/format/gltf_document.hpp"
#include "melkor/format/glb_container.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace {

using namespace melkor;
namespace gltf = melkor::format::gltf;
namespace khr = melkor::format::khr;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

bool approx(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

struct Attr {
    std::string semantic;
    int comps;
    std::vector<float> vals;
    int component_type = 5126;
    bool normalized = false;
};

void put_f32(std::vector<std::uint8_t>& v, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
}

const char* type_name(int comps) {
    return comps == 1 ? "SCALAR" : (comps == 3 ? "VEC3" : "VEC4");
}

std::size_t component_bytes(int component_type) {
    return component_type == 5120 || component_type == 5121
               ? 1
               : (component_type == 5122 || component_type == 5123 ? 2 : 4);
}

void put_component(std::vector<std::uint8_t>& out, int component_type, float value) {
    if (component_type == 5120 || component_type == 5121) {
        out.push_back(static_cast<std::uint8_t>(static_cast<int>(value)));
        return;
    }
    if (component_type == 5122 || component_type == 5123) {
        const std::uint16_t bits = static_cast<std::uint16_t>(static_cast<int>(value));
        out.push_back(static_cast<std::uint8_t>(bits & 0xffu));
        out.push_back(static_cast<std::uint8_t>((bits >> 8u) & 0xffu));
        return;
    }
    put_f32(out, value);
}

// Builds the glTF JSON + BIN buffer for one POINTS primitive with the given attributes and the
// KHR_gaussian_splatting extension. `mode` and `kernel` are overridable for the rejection tests.
struct Built {
    std::string json;
    std::vector<std::uint8_t> buffer;
};

class TempDirectory {
public:
    TempDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("melkor-gltf-reader-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

bool write_bytes(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    if (size != 0)
        stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return stream.good();
}

std::string base64(const std::vector<std::uint8_t>& bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const std::uint32_t a = bytes[index];
        const std::uint32_t b = index + 1 < bytes.size() ? bytes[index + 1] : 0;
        const std::uint32_t c = index + 2 < bytes.size() ? bytes[index + 2] : 0;
        const std::uint32_t value = (a << 16U) | (b << 8U) | c;
        result.push_back(alphabet[(value >> 18U) & 63U]);
        result.push_back(alphabet[(value >> 12U) & 63U]);
        result.push_back(index + 1 < bytes.size() ? alphabet[(value >> 6U) & 63U] : '=');
        result.push_back(index + 2 < bytes.size() ? alphabet[value & 63U] : '=');
    }
    return result;
}

std::string scene_json(Built built, const std::string& uri) {
    const std::string buffer =
        "\"buffers\":[{\"byteLength\":" + std::to_string(built.buffer.size()) + "}]";
    const std::string replacement =
        "\"buffers\":[{\"byteLength\":" + std::to_string(built.buffer.size()) + ",\"uri\":\"" +
        uri + "\"}]";
    const std::size_t location = built.json.find(buffer);
    if (location == std::string::npos)
        return {};
    built.json.replace(location, buffer.size(), replacement);
    built.json.pop_back();
    built.json += ",\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    return built.json;
}

Built build(std::size_t n, const std::vector<Attr>& attrs, int mode = 0,
            const std::string& kernel = "ellipse",
            const std::string& color_space = "srgb_rec709_display") {
    Built out;
    std::string views, accessors, attrmap;
    std::size_t offset = 0;
    for (std::size_t i = 0; i < attrs.size(); ++i) {
        const auto& a = attrs[i];
        const std::size_t len =
            static_cast<std::size_t>(a.comps) * component_bytes(a.component_type) * n;
        for (float f : a.vals)
            put_component(out.buffer, a.component_type, f);
        if (!views.empty())
            views += ",";
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                 ",\"byteLength\":" + std::to_string(len) + "}";
        if (!accessors.empty())
            accessors += ",";
        accessors += "{\"bufferView\":" + std::to_string(i) +
                     ",\"componentType\":" + std::to_string(a.component_type) + ",\"type\":\"" +
                     type_name(a.comps) + "\",\"count\":" + std::to_string(n);
        if (a.normalized)
            accessors += ",\"normalized\":true";
        if (a.semantic == "POSITION" && a.comps == 3 && !a.vals.empty()) {
            float minimum[3] = {a.vals[0], a.vals[1], a.vals[2]};
            float maximum[3] = {a.vals[0], a.vals[1], a.vals[2]};
            for (std::size_t value = 3; value < a.vals.size(); ++value) {
                const std::size_t component = value % 3;
                minimum[component] = std::min(minimum[component], a.vals[value]);
                maximum[component] = std::max(maximum[component], a.vals[value]);
            }
            accessors += ",\"min\":[" + std::to_string(minimum[0]) + "," +
                         std::to_string(minimum[1]) + "," + std::to_string(minimum[2]) +
                         "],\"max\":[" + std::to_string(maximum[0]) + "," +
                         std::to_string(maximum[1]) + "," + std::to_string(maximum[2]) + "]";
        }
        accessors += "}";
        if (!attrmap.empty())
            attrmap += ",";
        attrmap += "\"" + a.semantic + "\":" + std::to_string(i);
        offset += len;
    }
    out.json = "{\"asset\":{\"version\":\"2.0\"},"
               "\"extensionsUsed\":[\"KHR_gaussian_splatting\"],"
               "\"buffers\":[{\"byteLength\":" +
               std::to_string(out.buffer.size()) +
               "}],"
               "\"bufferViews\":[" +
               views +
               "],"
               "\"accessors\":[" +
               accessors +
               "],"
               "\"meshes\":[{\"primitives\":[{\"mode\":" +
               std::to_string(mode) + ",\"attributes\":{" + attrmap +
               "},"
               "\"extensions\":{\"KHR_gaussian_splatting\":{\"kernel\":\"" +
               kernel + "\",\"colorSpace\":\"" + color_space + "\"}}}]}]}";
    return out;
}

// Standard valid geometry for n splats: identity rotation, small positive scale, mid opacity.
std::vector<Attr> geometry(std::size_t n, std::vector<float> positions) {
    std::vector<float> rot, scale, opacity;
    for (std::size_t s = 0; s < n; ++s) {
        rot.insert(rot.end(), {0.f, 0.f, 0.f, 1.f});
        scale.insert(scale.end(), {0.1f, 0.2f, 0.3f});
        opacity.push_back(0.5f);
    }
    return {
        {"POSITION", 3, std::move(positions)},
        {"KHR_gaussian_splatting:ROTATION", 4, std::move(rot)},
        {"KHR_gaussian_splatting:SCALE", 3, std::move(scale)},
        {"KHR_gaussian_splatting:OPACITY", 1, std::move(opacity)},
    };
}

Result<gltf::PrimitiveRead> read_result(const Built& b) {
    auto doc =
        gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(b.json.data()), b.json.size());
    if (!doc.has_value()) {
        return Result<gltf::PrimitiveRead>::failure(doc.error_code(), doc.diagnostics());
    }
    std::vector<gltf::BufferSpan> buffers = {gltf::BufferSpan{b.buffer.data(), b.buffer.size()}};
    melkor::Budget budget(melkor::Limits::for_profile(melkor::LimitsProfile::desktop));
    return gltf::read_primitive_local(doc.value(), doc.value().meshes[0].primitives[0], buffers,
                                      budget);
}

gltf::PrimitiveRead read(const Built& b, bool& ok) {
    auto r = read_result(b);
    ok = r.has_value();
    if (!ok)
        return gltf::PrimitiveRead(SplatData::create({}).value(), {}, 0, {});
    return std::move(r.value());
}

void test_degree0_reads() {
    auto attrs = geometry(2, {1.f, 2.f, 3.f, 4.f, 5.f, 6.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0",
                     3,
                     {0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f}});
    bool ok = false;
    auto r = read(build(2, attrs), ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(r.data.size() == 2);
    CHECK(r.source_sh_degree == 0);
    CHECK(r.color_space == khr::ColorSpace::srgb_rec709_display);
    CHECK(approx(r.data.positions()[0].x, 1.f) && approx(r.data.positions()[1].z, 6.f));
    CHECK(approx(r.data.opacities()[0], 0.5f));
    CHECK(approx(r.data.scales()[0].y, 0.2f));
    CHECK(r.data.rotations()[0].w == 1.f);
    CHECK(r.data.sh().dc(0).has_value() && r.data.sh().dc(1).has_value());
    CHECK(approx(r.data.sh().dc(0).value().x, 0.01f) && approx(r.data.sh().dc(1).value().z, 0.06f));
}

void test_degree1_transpose() {
    // The key test: distinct values per coefficient, checked at their splat-major destinations.
    auto attrs = geometry(2, {0.f, 0.f, 0.f, 0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0",
                     3,
                     {0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f}});  // flat 0
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_1_COEF_0",
                     3,
                     {1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f}});  // flat 1
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_1_COEF_1",
                     3,
                     {2.0f, 2.1f, 2.2f, 2.3f, 2.4f, 2.5f}});  // flat 2
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_1_COEF_2",
                     3,
                     {3.0f, 3.1f, 3.2f, 3.3f, 3.4f, 3.5f}});  // flat 3
    bool ok = false;
    auto r = read(build(2, attrs), ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(r.source_sh_degree == 1);
    CHECK(r.data.sh().degree() == 1 && r.data.sh().coefficients() == 4);
    const auto& raw = r.data.sh().raw();
    // splat-major block layout: raw[s*(coeffs*3) + k*3 + c], coeffs=4.
    CHECK(approx(raw[0 * 12 + 0 * 3 + 0], 0.01f));  // splat0, flat0 (DC), R
    CHECK(approx(raw[0 * 12 + 1 * 3 + 0], 1.0f));   // splat0, flat1, R
    CHECK(approx(raw[0 * 12 + 3 * 3 + 2], 3.2f));   // splat0, flat3, B
    CHECK(approx(raw[1 * 12 + 0 * 3 + 2], 0.06f));  // splat1, flat0 (DC), B
    CHECK(approx(raw[1 * 12 + 2 * 3 + 1], 2.4f));   // splat1, flat2, G
    CHECK(approx(raw[1 * 12 + 3 * 3 + 0], 3.3f));   // splat1, flat3, R
}

void test_rejects_wrong_mode_and_kernel() {
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    bool ok = true;
    read(build(1, attrs, /*mode=*/4), ok);  // TRIANGLES, not POINTS
    CHECK(!ok);
    ok = true;
    read(build(1, attrs, /*mode=*/0, /*kernel=*/"gaussian2d"), ok);  // unsupported kernel
    CHECK(!ok);
}

void test_rejects_missing_attribute() {
    // No SCALE attribute.
    std::vector<Attr> attrs;
    std::vector<float> rot, opacity;
    rot.insert(rot.end(), {0.f, 0.f, 0.f, 1.f});
    opacity.push_back(0.5f);
    attrs.push_back({"POSITION", 3, {0.f, 0.f, 0.f}});
    attrs.push_back({"KHR_gaussian_splatting:ROTATION", 4, std::move(rot)});
    attrs.push_back({"KHR_gaussian_splatting:OPACITY", 1, std::move(opacity)});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    bool ok = true;
    read(build(1, attrs), ok);
    CHECK(!ok);
}

void test_rejects_partial_sh_degree() {
    // Degree 1 present but only COEF_0 and COEF_1 (missing COEF_2): a partial degree.
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_1_COEF_0", 3, {0.f, 0.f, 0.f}});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_1_COEF_1", 3, {0.f, 0.f, 0.f}});
    bool ok = true;
    read(build(1, attrs), ok);
    CHECK(!ok);
}

void test_rejects_unknown_color_space() {
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    auto result = read_result(build(1, attrs, 0, "ellipse", "aces_ap0"));
    CHECK(!result.has_value());
    CHECK(result.error_code() == ErrorCode::unsupported_feature);
}

void test_rejects_sh_degree_gap() {
    // Degree 0 present plus a stray degree-2 coefficient while degree 1 is absent: a non-contiguous
    // SH pyramid the KHR spec forbids. It must be rejected, not silently read as degree 0 with the
    // degree-2 color dropped.
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_2_COEF_0", 3, {0.f, 0.f, 0.f}});
    bool ok = true;
    read(build(1, attrs), ok);
    CHECK(!ok);
}

void test_rejects_unsupported_profile_values() {
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});

    Built projection = build(1, attrs);
    const std::string extension_end = "\"colorSpace\":\"srgb_rec709_display\"";
    auto position = projection.json.find(extension_end);
    CHECK(position != std::string::npos);
    if (position != std::string::npos) {
        projection.json.insert(position + extension_end.size(), ",\"projection\":\"orthographic\"");
        bool ok = true;
        read(projection, ok);
        CHECK(!ok);
    }

    Built sorting = build(1, attrs);
    position = sorting.json.find(extension_end);
    CHECK(position != std::string::npos);
    if (position != std::string::npos) {
        sorting.json.insert(position + extension_end.size(), ",\"sortingMethod\":\"unknown\"");
        bool ok = true;
        read(sorting, ok);
        CHECK(!ok);
    }

    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_4_COEF_0", 3, {0.f, 0.f, 0.f}});
    bool ok = true;
    read(build(1, attrs), ok);
    CHECK(!ok);
}

void test_quantized_rotation_is_renormalized() {
    std::vector<Attr> attrs;
    attrs.push_back({"POSITION", 3, {0.f, 0.f, 0.f}});
    attrs.push_back({"KHR_gaussian_splatting:ROTATION", 4, {90.f, 0.f, 0.f, 90.f}, 5120, true});
    attrs.push_back({"KHR_gaussian_splatting:SCALE", 3, {0.1f, 0.1f, 0.1f}});
    attrs.push_back({"KHR_gaussian_splatting:OPACITY", 1, {0.5f}});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    bool ok = false;
    auto r = read(build(1, attrs), ok);
    CHECK(ok);
    if (ok) {
        const auto& q = r.data.rotations()[0];
        const float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        CHECK(approx(norm, 1.0f));
        CHECK(approx(q.x, 0.70710678f) && approx(q.w, 0.70710678f));
    }
}

void test_rejects_invalid_component_encodings() {
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});

    attrs[0].component_type = 5121;
    attrs[0].normalized = true;
    bool ok = true;
    read(build(1, attrs), ok);
    CHECK(!ok);

    attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs[1] = {"KHR_gaussian_splatting:ROTATION", 4, {127.f, 127.f, 127.f, 127.f}, 5120, true};
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    ok = true;
    read(build(1, attrs), ok);
    CHECK(!ok);

    attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs[3].component_type = 5121;
    attrs[3].vals = {128.f};
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    ok = true;
    read(build(1, attrs), ok);
    CHECK(!ok);

    attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}, 5121, true});
    ok = true;
    read(build(1, attrs), ok);
    CHECK(!ok);
}

void test_rejects_invalid_splat_values() {
    // A non-unit rotation must be rejected by SplatData validation flowing through the reader.
    std::vector<Attr> attrs;
    attrs.push_back({"POSITION", 3, {0.f, 0.f, 0.f}});
    attrs.push_back({"KHR_gaussian_splatting:ROTATION", 4, {0.f, 0.f, 0.f, 0.f}});  // zero quat
    attrs.push_back({"KHR_gaussian_splatting:SCALE", 3, {0.1f, 0.1f, 0.1f}});
    attrs.push_back({"KHR_gaussian_splatting:OPACITY", 1, {0.5f}});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    bool ok = true;
    read(build(1, attrs), ok);
    CHECK(!ok);
}

void test_position_bounds_and_zero_scale_contracts() {
    auto attrs = geometry(1, {1.0f, 2.0f, 3.0f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    const Built built = build(1, attrs);
    auto document = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(built.json.data()),
                                          built.json.size());
    CHECK(document.has_value());
    if (document.has_value()) {
        document.value().accessors[0].maximum[0] = 0.5;
        std::vector<gltf::BufferSpan> buffers = {
            gltf::BufferSpan{built.buffer.data(), built.buffer.size()}};
        Budget budget(Limits::for_profile(LimitsProfile::desktop));
        auto result = gltf::read_primitive_local(
            document.value(), document.value().meshes[0].primitives[0], buffers, budget);
        CHECK(!result.has_value());
        CHECK(result.error_code() == ErrorCode::invalid_data);
        CHECK(!result.diagnostics().empty());
        if (!result.diagnostics().empty()) {
            CHECK(result.diagnostics()[0].code == "MK2207_GLTF_ACCESSOR_BOUNDS");
        }
    }

    document = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(built.json.data()),
                                     built.json.size());
    CHECK(document.has_value());
    if (document.has_value()) {
        document.value().accessors[0].minimum[0] = 0.0;
        document.value().accessors[0].maximum[0] = 2.0;
        std::vector<gltf::BufferSpan> buffers = {
            gltf::BufferSpan{built.buffer.data(), built.buffer.size()}};
        Budget budget(Limits::for_profile(LimitsProfile::desktop));
        auto result = gltf::read_primitive_local(
            document.value(), document.value().meshes[0].primitives[0], buffers, budget);
        CHECK(!result.has_value());
        CHECK(result.error_code() == ErrorCode::invalid_data);
        CHECK(!result.diagnostics().empty());
        if (!result.diagnostics().empty())
            CHECK(result.diagnostics()[0].code == "MK2207_GLTF_ACCESSOR_BOUNDS");
    }

    attrs = geometry(1, {0.0f, 0.0f, 0.0f});
    attrs[2].vals[0] = 0.0f;
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    auto zero_scale = read_result(build(1, attrs));
    CHECK(zero_scale.has_value());
    if (zero_scale.has_value())
        CHECK(zero_scale.value().data.scales()[0].x == 0.0f);
}

void test_rejects_public_document_accessor_index() {
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    const Built built = build(1, attrs);
    auto document = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(built.json.data()),
                                          built.json.size());
    CHECK(document.has_value());
    if (!document.has_value())
        return;

    gltf::PrimitiveDesc primitive = document.value().meshes[0].primitives[0];
    primitive.attributes[khr::kAttrPosition] = document.value().accessors.size();
    std::vector<gltf::BufferSpan> buffers = {
        gltf::BufferSpan{built.buffer.data(), built.buffer.size()}};
    Budget invalid_budget(Limits::for_profile(LimitsProfile::custom));
    OperationContext invalid_context = make_default_context(invalid_budget);
    const auto invalid_limits =
        gltf::read_primitive_local(document.value(), primitive, buffers, invalid_context);
    CHECK(!invalid_limits.has_value());
    CHECK(invalid_limits.error_code() == ErrorCode::invalid_argument);

    Budget budget(Limits::for_profile(LimitsProfile::desktop));
    const auto result = gltf::read_primitive_local(document.value(), primitive, buffers, budget);
    CHECK(!result.has_value());
    CHECK(!result.diagnostics().empty());
    if (!result.diagnostics().empty()) {
        CHECK(result.diagnostics()[0].code == "MK2157_GLTF_ACCESSOR_INDEX");
    }
}

void test_file_reader_resolves_local_and_data_buffers() {
    auto attrs = geometry(1, {1.f, 2.f, 3.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.1f, 0.2f, 0.3f}});
    const Built built = build(1, attrs);
    TempDirectory directory;
    const auto binary_path = directory.path() / "cloud data.bin";
    const auto json_path = directory.path() / "cloud.gltf";
    const std::string external_json = scene_json(built, "cloud%20data.bin");
    CHECK(!external_json.empty());
    CHECK(write_bytes(binary_path, built.buffer.data(), built.buffer.size()));
    CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(external_json.data()),
                      external_json.size()));

    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    auto external = gltf::read_file(json_path, context);
    CHECK(external.has_value());
    if (external.has_value()) {
        CHECK(external.value().data.size() == 1);
        CHECK(approx(external.value().data.positions()[0].x, 1.f));
        CHECK(budget.used(BudgetKind::external_resources) == 1);
        CHECK(budget.used(BudgetKind::resource_bytes) == built.buffer.size());
        CHECK(budget.used(BudgetKind::memory_bytes) == external.value().retained_memory_bytes());
    }

    const std::string data_uri = "DATA:Application/Octet-Stream;BASE64," + base64(built.buffer);
    const std::string inline_json = scene_json(built, data_uri);
    const auto inline_path = directory.path() / "inline.gltf";
    CHECK(write_bytes(inline_path, reinterpret_cast<const std::uint8_t*>(inline_json.data()),
                      inline_json.size()));
    Budget inline_budget(limits);
    OperationContext inline_context = make_default_context(inline_budget);
    auto inlined = gltf::read_file(inline_path, inline_context);
    CHECK(inlined.has_value());
    if (inlined.has_value()) {
        CHECK(approx(inlined.value().data.positions()[0].z, 3.f));
        CHECK(inline_budget.used(BudgetKind::external_resources) == 1);
        CHECK(inline_budget.used(BudgetKind::resource_bytes) == built.buffer.size());
        CHECK(inline_budget.used(BudgetKind::decoded_bytes) == built.buffer.size());
    }
}

void test_file_reader_rejects_undeclared_resource_bytes() {
    auto attrs = geometry(1, {1.f, 2.f, 3.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.1f, 0.2f, 0.3f}});
    const Built built = build(1, attrs);
    std::vector<std::uint8_t> oversized = built.buffer;
    oversized.push_back(0);

    TempDirectory directory;
    const auto binary_path = directory.path() / "cloud.bin";
    const auto json_path = directory.path() / "cloud.gltf";
    const std::string external_json = scene_json(built, "cloud.bin");
    CHECK(write_bytes(binary_path, oversized.data(), oversized.size()));
    CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(external_json.data()),
                      external_json.size()));

    auto external = gltf::read_file(json_path);
    CHECK(!external.has_value());
    if (!external.has_value() && !external.diagnostics().empty())
        CHECK(external.diagnostics()[0].code == "MK2193_GLTF_RESOURCE_LENGTH");

    const std::string inline_json =
        scene_json(built, "data:application/octet-stream;base64," + base64(oversized));
    CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(inline_json.data()),
                      inline_json.size()));
    auto inlined = gltf::read_file(json_path);
    CHECK(!inlined.has_value());
    if (!inlined.has_value() && !inlined.diagnostics().empty())
        CHECK(inlined.diagnostics()[0].code == "MK2193_GLTF_RESOURCE_LENGTH");
}

void test_file_reader_accepts_utf8_resource_name() {
    auto attrs = geometry(1, {1.f, 2.f, 3.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.1f, 0.2f, 0.3f}});
    const Built built = build(1, attrs);
    TempDirectory directory;
    const auto binary_path = directory.path() / std::filesystem::u8path(u8"wolke-\u2601.bin");
    const auto json_path = directory.path() / "cloud.gltf";
    const std::string document = scene_json(built, u8"wolke-\u2601.bin");
    CHECK(write_bytes(binary_path, built.buffer.data(), built.buffer.size()));
    CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(document.data()),
                      document.size()));

    const auto result = gltf::read_file(json_path);
    CHECK(result.has_value());
    if (result.has_value())
        CHECK(approx(result.value().data.positions()[0].y, 2.f));
}

void test_file_reader_skips_unused_external_buffers() {
    auto attrs = geometry(1, {1.f, 2.f, 3.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.1f, 0.2f, 0.3f}});
    const Built built = build(1, attrs);
    TempDirectory directory;
    const auto binary_path = directory.path() / "cloud.bin";
    const auto json_path = directory.path() / "cloud.gltf";
    std::string document = scene_json(built, "cloud.bin");
    const std::string buffer_array =
        "\"buffers\":[{\"byteLength\":" + std::to_string(built.buffer.size()) +
        ",\"uri\":\"cloud.bin\"}]";
    const auto position = document.find(buffer_array);
    CHECK(position != std::string::npos);
    if (position == std::string::npos)
        return;
    const std::string replacement = buffer_array.substr(0, buffer_array.size() - 1) +
                                    ",{\"byteLength\":4,\"uri\":\"missing.bin\"}]";
    document.replace(position, buffer_array.size(), replacement);
    CHECK(write_bytes(binary_path, built.buffer.data(), built.buffer.size()));
    CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(document.data()),
                      document.size()));

    Budget budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext context = make_default_context(budget);
    const auto result = gltf::read_file(json_path, context);
    CHECK(result.has_value());
    if (!result.has_value())
        return;
    CHECK(result.value().data.size() == 1);
    CHECK(budget.used(BudgetKind::external_resources) == 1);
    bool found = false;
    for (const auto& loss : result.value().losses.items()) {
        found = found || loss.code == "LOSS_GLTF_CONTENT_DROPPED";
    }
    CHECK(found);
    CHECK(result.value().losses.has_blocking());
}

void test_file_reader_reports_unused_glb_bin() {
    auto attrs = geometry(1, {1.f, 2.f, 3.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.1f, 0.2f, 0.3f}});
    const Built built = build(1, attrs);
    TempDirectory directory;
    const auto binary_path = directory.path() / "cloud.bin";
    const auto glb_path = directory.path() / "cloud.glb";
    const std::string document = scene_json(built, "cloud.bin");
    const std::vector<std::uint8_t> unused_bin = {1, 2, 3, 4};
    const auto glb = melkor::format::glb::build_glb(document, unused_bin.data(), unused_bin.size());
    CHECK(glb.has_value());
    if (!glb.has_value())
        return;
    CHECK(write_bytes(binary_path, built.buffer.data(), built.buffer.size()));
    CHECK(write_bytes(glb_path, glb.value().data(), glb.value().size()));

    const auto result = gltf::read_file(glb_path);
    CHECK(result.has_value());
    if (!result.has_value())
        return;
    bool found = false;
    for (const auto& loss : result.value().losses.items()) {
        found = found || loss.code == "LOSS_GLTF_CONTENT_DROPPED";
    }
    CHECK(found);
    CHECK(result.value().losses.has_blocking());
}

void test_file_reader_rejects_escaping_and_symlink_resources() {
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    const Built built = build(1, attrs);
    TempDirectory directory;
    const auto json_path = directory.path() / "cloud.gltf";

    const std::string escaping = scene_json(built, "../outside.bin");
    CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(escaping.data()),
                      escaping.size()));
    auto result = gltf::read_file(json_path);
    CHECK(!result.has_value());
    if (!result.has_value() && !result.diagnostics().empty())
        CHECK(result.diagnostics()[0].code == "MK2185_GLTF_BUFFER_URI");

    const std::string encoded_escape = scene_json(built, "%2e%2e/outside.bin");
    CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(encoded_escape.data()),
                      encoded_escape.size()));
    result = gltf::read_file(json_path);
    CHECK(!result.has_value());
    if (!result.has_value() && !result.diagnostics().empty())
        CHECK(result.diagnostics()[0].code == "MK2185_GLTF_BUFFER_URI");

    const auto target_path = directory.path() / "target.bin";
    const auto link_path = directory.path() / "link.bin";
    CHECK(write_bytes(target_path, built.buffer.data(), built.buffer.size()));
    std::error_code link_error;
    std::filesystem::create_symlink(target_path.filename(), link_path, link_error);
    if (!link_error) {
        const std::string symlink_json = scene_json(built, "link.bin");
        CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(symlink_json.data()),
                          symlink_json.size()));
        result = gltf::read_file(json_path);
        CHECK(!result.has_value());
        if (!result.has_value() && !result.diagnostics().empty())
            CHECK(result.diagnostics()[0].code == "MK2187_GLTF_RESOURCE_OPEN");
    }
}

void test_file_reader_rejects_nonportable_resource_names() {
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    const Built built = build(1, attrs);
    TempDirectory directory;
    const auto json_path = directory.path() / "cloud.gltf";
    const std::array<const char*, 7> names = {
        "AUX.bin",  "cloud%3f.bin",  "cloud%7c.bin", "cloud.bin.",
        "cloud%20", "COM%C2%B9.bin", "lpt%C2%B2",
    };

    for (const char* name : names) {
        const std::string document = scene_json(built, name);
        CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(document.data()),
                          document.size()));
        const auto result = gltf::read_file(json_path);
        CHECK(!result.has_value());
        if (!result.has_value() && !result.diagnostics().empty())
            CHECK(result.diagnostics()[0].code == "MK2185_GLTF_BUFFER_URI");
    }
}

void test_file_reader_enforces_external_resource_budget() {
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    const Built built = build(1, attrs);
    TempDirectory directory;
    const auto binary_path = directory.path() / "cloud.bin";
    const auto json_path = directory.path() / "cloud.gltf";
    const std::string document = scene_json(built, "cloud.bin");
    CHECK(write_bytes(binary_path, built.buffer.data(), built.buffer.size()));
    CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(document.data()),
                      document.size()));

    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_resource_bytes = built.buffer.size() - 1;
    auto result = gltf::read_file(json_path, limits);
    CHECK(!result.has_value());
    CHECK(result.error_code() == ErrorCode::resource_limit);

    const std::string data_uri = "data:application/octet-stream;base64," + base64(built.buffer);
    const std::string inline_document = scene_json(built, data_uri);
    const auto inline_path = directory.path() / "inline.gltf";
    CHECK(write_bytes(inline_path, reinterpret_cast<const std::uint8_t*>(inline_document.data()),
                      inline_document.size()));
    result = gltf::read_file(inline_path, limits);
    CHECK(!result.has_value());
    CHECK(result.error_code() == ErrorCode::resource_limit);
}

void test_file_reader_honors_expected_encoding() {
    TempDirectory directory;
    const auto glb_path = directory.path() / "damaged.glb";
    const std::uint8_t damaged[] = {'b', 'a', 'd'};
    CHECK(write_bytes(glb_path, damaged, sizeof(damaged)));

    auto result = gltf::read_file(glb_path, gltf::FileEncoding::binary_glb);
    CHECK(!result.has_value());
    if (!result.has_value() && !result.diagnostics().empty())
        CHECK(result.diagnostics()[0].code == "MK2101_GLB_TRUNCATED_HEADER");

    const std::uint8_t glb_magic[] = {'g', 'l', 'T', 'F'};
    CHECK(write_bytes(glb_path, glb_magic, sizeof(glb_magic)));
    result = gltf::read_file(glb_path, gltf::FileEncoding::json);
    CHECK(!result.has_value());
    if (!result.has_value() && !result.diagnostics().empty())
        CHECK(result.diagnostics()[0].code == "MK2196_GLTF_CONTAINER_MISMATCH");
}

void test_file_reader_rejects_fifos_without_blocking() {
#if defined(__unix__) || defined(__APPLE__)
    TempDirectory directory;
    const auto primary_fifo = directory.path() / "cloud.gltf";
    CHECK(::mkfifo(primary_fifo.c_str(), 0600) == 0);
    auto result = gltf::read_file(primary_fifo);
    CHECK(!result.has_value());
    CHECK(result.error_code() == ErrorCode::io_error);
    if (!result.diagnostics().empty())
        CHECK(result.diagnostics()[0].code == "MK2180_GLTF_FILE_TYPE");

    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    const Built built = build(1, attrs);
    const auto json_path = directory.path() / "external.gltf";
    const auto external_fifo = directory.path() / "cloud.bin";
    const std::string document = scene_json(built, "cloud.bin");
    CHECK(write_bytes(json_path, reinterpret_cast<const std::uint8_t*>(document.data()),
                      document.size()));
    CHECK(::mkfifo(external_fifo.c_str(), 0600) == 0);
    result = gltf::read_file(json_path);
    CHECK(!result.has_value());
    CHECK(result.error_code() == ErrorCode::io_error);
    if (!result.diagnostics().empty())
        CHECK(result.diagnostics()[0].code == "MK2180_GLTF_FILE_TYPE");
#endif
}

}  // namespace

int main() {
    test_degree0_reads();
    test_degree1_transpose();
    test_rejects_wrong_mode_and_kernel();
    test_rejects_missing_attribute();
    test_rejects_partial_sh_degree();
    test_rejects_sh_degree_gap();
    test_rejects_unsupported_profile_values();
    test_quantized_rotation_is_renormalized();
    test_rejects_invalid_component_encodings();
    test_rejects_unknown_color_space();
    test_rejects_invalid_splat_values();
    test_position_bounds_and_zero_scale_contracts();
    test_rejects_public_document_accessor_index();
    test_file_reader_resolves_local_and_data_buffers();
    test_file_reader_rejects_undeclared_resource_bytes();
    test_file_reader_accepts_utf8_resource_name();
    test_file_reader_skips_unused_external_buffers();
    test_file_reader_reports_unused_glb_bin();
    test_file_reader_rejects_escaping_and_symlink_resources();
    test_file_reader_rejects_nonportable_resource_names();
    test_file_reader_enforces_external_resource_budget();
    test_file_reader_honors_expected_encoding();
    test_file_reader_rejects_fifos_without_blocking();

    if (g_failures == 0) {
        std::printf("gltf reader: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf reader: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
