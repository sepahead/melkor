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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

bool approx(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

struct Attr {
    std::string semantic;
    int comps;               // 1=SCALAR, 3=VEC3, 4=VEC4
    std::vector<float> vals;  // length comps*n, splat-major
};

void put_f32(std::vector<std::uint8_t>& v, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
}

const char* type_name(int comps) {
    return comps == 1 ? "SCALAR" : (comps == 3 ? "VEC3" : "VEC4");
}

// Builds the glTF JSON + BIN buffer for one POINTS primitive with the given attributes and the
// KHR_gaussian_splatting extension. `mode` and `kernel` are overridable for the rejection tests.
struct Built {
    std::string json;
    std::vector<std::uint8_t> buffer;
};

Built build(std::size_t n, const std::vector<Attr>& attrs, int mode = 0,
            const std::string& kernel = "ellipse",
            const std::string& color_space = "srgb_rec709_display") {
    Built out;
    std::string views, accessors, attrmap;
    std::size_t offset = 0;
    for (std::size_t i = 0; i < attrs.size(); ++i) {
        const auto& a = attrs[i];
        const std::size_t len = static_cast<std::size_t>(a.comps) * 4 * n;
        for (float f : a.vals) put_f32(out.buffer, f);
        if (!views.empty()) views += ",";
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                 ",\"byteLength\":" + std::to_string(len) + "}";
        if (!accessors.empty()) accessors += ",";
        accessors += "{\"bufferView\":" + std::to_string(i) +
                     ",\"componentType\":5126,\"type\":\"" + type_name(a.comps) +
                     "\",\"count\":" + std::to_string(n) + "}";
        if (!attrmap.empty()) attrmap += ",";
        attrmap += "\"" + a.semantic + "\":" + std::to_string(i);
        offset += len;
    }
    out.json = "{\"buffers\":[{\"byteLength\":" + std::to_string(out.buffer.size()) + "}],"
               "\"bufferViews\":[" + views + "],"
               "\"accessors\":[" + accessors + "],"
               "\"meshes\":[{\"primitives\":[{\"mode\":" + std::to_string(mode) +
               ",\"attributes\":{" + attrmap + "},"
               "\"extensions\":{\"KHR_gaussian_splatting\":{\"kernel\":\"" + kernel +
               "\",\"colorSpace\":\"" + color_space + "\"}}}]}]}";
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

gltf::PrimitiveRead read(const Built& b, bool& ok) {
    auto doc = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(b.json.data()), b.json.size());
    if (!doc.has_value()) {
        ok = false;
        return gltf::PrimitiveRead{SplatData::create({}).value(), {}, false, 0};
    }
    std::vector<gltf::BufferSpan> buffers = {gltf::BufferSpan{b.buffer.data(), b.buffer.size()}};
    auto r = gltf::read_primitive_local(doc.value(), doc.value().meshes[0].primitives[0], buffers);
    ok = r.has_value();
    if (!ok) return gltf::PrimitiveRead{SplatData::create({}).value(), {}, false, 0};
    return std::move(r.value());
}

void test_degree0_reads() {
    auto attrs = geometry(2, {1.f, 2.f, 3.f, 4.f, 5.f, 6.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3,
                     {0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f}});
    bool ok = false;
    auto r = read(build(2, attrs), ok);
    CHECK(ok);
    if (!ok) return;
    CHECK(r.data.size() == 2);
    CHECK(r.source_sh_degree == 0);
    CHECK(!r.color_space_assumed);
    CHECK(r.color_space == khr::ColorSpace::srgb_rec709_display);
    CHECK(approx(r.data.positions()[0].x, 1.f) && approx(r.data.positions()[1].z, 6.f));
    CHECK(approx(r.data.opacities()[0], 0.5f));
    CHECK(approx(r.data.scales()[0].y, 0.2f));
    CHECK(r.data.rotations()[0].w == 1.f);
    CHECK(approx(r.data.sh().dc(0, 0), 0.01f) && approx(r.data.sh().dc(1, 2), 0.06f));
}

void test_degree1_transpose() {
    // The key test: distinct values per coefficient, checked at their splat-major destinations.
    auto attrs = geometry(2, {0.f, 0.f, 0.f, 0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3,
                     {0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f}});   // flat 0
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_1_COEF_0", 3,
                     {1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f}});         // flat 1
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_1_COEF_1", 3,
                     {2.0f, 2.1f, 2.2f, 2.3f, 2.4f, 2.5f}});         // flat 2
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_1_COEF_2", 3,
                     {3.0f, 3.1f, 3.2f, 3.3f, 3.4f, 3.5f}});         // flat 3
    bool ok = false;
    auto r = read(build(2, attrs), ok);
    CHECK(ok);
    if (!ok) return;
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

void test_unknown_color_space_is_assumed() {
    auto attrs = geometry(1, {0.f, 0.f, 0.f});
    attrs.push_back({"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", 3, {0.f, 0.f, 0.f}});
    bool ok = false;
    auto r = read(build(1, attrs, 0, "ellipse", "aces_ap0"), ok);
    CHECK(ok);
    if (ok) {
        CHECK(r.color_space_assumed);
        CHECK(r.color_space == khr::ColorSpace::srgb_rec709_display);
    }
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

}  // namespace

int main() {
    test_degree0_reads();
    test_degree1_transpose();
    test_rejects_wrong_mode_and_kernel();
    test_rejects_missing_attribute();
    test_rejects_partial_sh_degree();
    test_unknown_color_space_is_assumed();
    test_rejects_invalid_splat_values();

    if (g_failures == 0) {
        std::printf("gltf reader: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf reader: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
