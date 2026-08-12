// Tests for the glTF scene-graph walk (read_gaussian_scene).
//
// These tests pin the whole-scene behavior. Node transforms compose, invalid node graphs fail,
// and different SH degrees merge by padding. Buffers are built by hand.
//
// Self-contained (no external test framework).

#include "melkor/format/gltf_reader.hpp"

#include "melkor/format/glb_container.hpp"
#include "melkor/format/gltf_document.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace melkor;
namespace gltf = melkor::format::gltf;

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

bool approx(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

void put_f32(std::vector<std::uint8_t>& v, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
}

void put_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
}

void set_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes[offset + static_cast<std::size_t>(shift / 8)] =
            static_cast<std::uint8_t>((value >> shift) & 0xffU);
}

bool has_loss(const LossReport& r, const std::string& code) {
    for (const auto& i : r.items())
        if (i.code == code)
            return true;
    return false;
}

class RecordingProgressSink final : public ProgressSink {
public:
    void on_progress(const ProgressEvent& event) override { events.push_back(event); }

    std::vector<ProgressEvent> events;
};

class CancelOnPhaseSink final : public ProgressSink {
public:
    CancelOnPhaseSink(CancellationToken token, std::string phase)
        : token_(std::move(token)), phase_(std::move(phase)) {}

    void on_progress(const ProgressEvent& event) override {
        if (event.phase == phase_) {
            token_.cancel();
        }
    }

private:
    CancellationToken token_;
    std::string phase_;
};

// Builds a buffer holding, for one splat: POSITION(3), ROTATION(4), SCALE(3), OPACITY(1), DC(3),
// contiguous, and returns the bytes plus the byte offsets of each accessor.
struct Packed {
    std::vector<std::uint8_t> bytes;
};

Packed pack_one_splat(float px, float py, float pz) {
    Packed p;
    put_f32(p.bytes, px);
    put_f32(p.bytes, py);
    put_f32(p.bytes, pz);  // POSITION @0
    for (float f : {0.f, 0.f, 0.f, 1.f})
        put_f32(p.bytes, f);  // ROTATION @12
    for (float f : {0.1f, 0.1f, 0.1f})
        put_f32(p.bytes, f);  // SCALE @28
    put_f32(p.bytes, 0.5f);   // OPACITY @40
    for (float f : {0.2f, 0.3f, 0.4f})
        put_f32(p.bytes, f);  // DC @44
    return p;                 // total 56 bytes
}

// glTF JSON for a single-splat mesh (buffer 0, the packed layout above) with a node graph the
// caller supplies (nodes/scene). `extra` is appended for extension declarations.
std::string single_splat_json(const std::string& nodes, const std::string& scene,
                              const std::string& extra = "",
                              const std::array<float, 3>& position = {0.0f, 0.0f, 0.0f}) {
    return "{"
           "\"asset\":{\"version\":\"2.0\"},"
           "\"extensionsUsed\":[\"KHR_gaussian_splatting\"],"
           "\"buffers\":[{\"byteLength\":56}],"
           "\"bufferViews\":["
           "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12},"
           "{\"buffer\":0,\"byteOffset\":12,\"byteLength\":16},"
           "{\"buffer\":0,\"byteOffset\":28,\"byteLength\":12},"
           "{\"buffer\":0,\"byteOffset\":40,\"byteLength\":4},"
           "{\"buffer\":0,\"byteOffset\":44,\"byteLength\":12}],"
           "\"accessors\":["
           "{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1,"
           "\"min\":[" +
           std::to_string(position[0]) + "," + std::to_string(position[1]) + "," +
           std::to_string(position[2]) + "],\"max\":[" + std::to_string(position[0]) + "," +
           std::to_string(position[1]) + "," + std::to_string(position[2]) + "]}," +
           "{\"bufferView\":1,\"componentType\":5126,\"type\":\"VEC4\",\"count\":1},"
           "{\"bufferView\":2,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":3,\"componentType\":5126,\"type\":\"SCALAR\",\"count\":1},"
           "{\"bufferView\":4,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1}],"
           "\"meshes\":[{\"primitives\":[{\"mode\":0,\"attributes\":{"
           "\"POSITION\":0,\"KHR_gaussian_splatting:ROTATION\":1,\"KHR_gaussian_splatting:SCALE\":"
           "2,"
           "\"KHR_gaussian_splatting:OPACITY\":3,\"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0\":4},"
           "\"extensions\":{\"KHR_gaussian_splatting\":{\"kernel\":\"ellipse\","
           "\"colorSpace\":\"srgb_rec709_display\"}}}]}]," +
           nodes + scene + extra + "}";
}

gltf::SceneRead run(const std::string& json, const std::vector<std::uint8_t>& buf, bool& ok,
                    const Limits& limits = Limits::for_profile(LimitsProfile::desktop)) {
    auto doc =
        gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
    if (!doc.has_value()) {
        ok = false;
        return gltf::SceneRead(SplatData::create({}).value(), {}, {}, 0, {});
    }
    std::vector<gltf::BufferSpan> buffers = {gltf::BufferSpan{buf.data(), buf.size()}};
    auto r = gltf::read_gaussian_scene(doc.value(), buffers, limits);
    ok = r.has_value();
    if (!ok)
        return gltf::SceneRead(SplatData::create({}).value(), {}, {}, 0, {});
    return std::move(r.value());
}

void test_translation_is_applied() {
    // A node translates the single splat (at local origin) by (10, 20, 30).
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json = single_splat_json("\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}],",
                                         "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    bool ok = false;
    auto r = run(json, buf, ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(r.data.size() == 1);
    CHECK(approx(r.data.positions()[0].x, 10.f) && approx(r.data.positions()[0].y, 20.f) &&
          approx(r.data.positions()[0].z, 30.f));
    CHECK(has_loss(r.losses, "LOSS_SCENE_GRAPH_FLATTENED"));
}

void test_parent_child_transform_composes() {
    // Parent translates by (100,0,0); child (rotate 90 about Z) holds the mesh. The splat at the
    // local origin ends up at the parent translation (rotation about origin leaves the origin
    // fixed), and the rotation triggers no SH loss because the splat is degree 0.
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    const double s = std::sqrt(0.5);
    std::string nodes = "\"nodes\":["
                        "{\"children\":[1],\"translation\":[100,0,0]},"
                        "{\"mesh\":0,\"rotation\":[0,0," +
                        std::to_string(s) + "," + std::to_string(s) + "]}],";
    std::string json = single_splat_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    bool ok = false;
    auto r = run(json, buf, ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(r.data.size() == 1);
    CHECK(approx(r.data.positions()[0].x, 100.f));
    CHECK(has_loss(r.losses, "LOSS_SCENE_GRAPH_FLATTENED"));
    // Degree 0: a rotating node must NOT produce an SH-rotation loss.
    CHECK(!has_loss(r.losses, "LOSS_SH_ROTATION_NOT_APPLIED"));
}

void test_invalid_node_graphs_fail() {
    auto buf = pack_one_splat(1.f, 2.f, 3.f).bytes;
    const std::string cycle =
        single_splat_json("\"nodes\":[{\"mesh\":0,\"children\":[1]},{\"children\":[0]}],",
                          "\"scenes\":[{\"nodes\":[0]}],\"scene\":0", "", {1.f, 2.f, 3.f});
    bool cycle_ok = true;
    run(cycle, buf, cycle_ok);
    CHECK(!cycle_ok);

    const std::string shared =
        single_splat_json("\"nodes\":[{\"children\":[2]},{\"children\":[2]},{\"mesh\":0}],",
                          "\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0", "", {1.f, 2.f, 3.f});
    bool shared_ok = true;
    run(shared, buf, shared_ok);
    CHECK(!shared_ok);

    Limits shallow = Limits::for_profile(LimitsProfile::desktop);
    shallow.max_scene_depth = 1;
    const std::string deep =
        single_splat_json("\"nodes\":[{\"children\":[1]},{\"mesh\":0}],",
                          "\"scenes\":[{\"nodes\":[0]}],\"scene\":0", "", {1.f, 2.f, 3.f});
    bool deep_ok = true;
    run(deep, buf, deep_ok, shallow);
    CHECK(!deep_ok);

    const std::string reused_mesh =
        single_splat_json("\"nodes\":[{\"mesh\":0},{\"mesh\":0,\"translation\":[10,0,0]}],",
                          "\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0", "", {1.f, 2.f, 3.f});
    bool reused_mesh_ok = false;
    const auto reused = run(reused_mesh, buf, reused_mesh_ok);
    CHECK(reused_mesh_ok);
    if (reused_mesh_ok) {
        CHECK(reused.data.size() == 2);
        CHECK(has_loss(reused.losses, "LOSS_INSTANCE_EXPANDED"));
        CHECK(has_loss(reused.losses, "LOSS_SCENE_GRAPH_FLATTENED"));
    }

    Limits one_splat = Limits::for_profile(LimitsProfile::desktop);
    one_splat.max_splats = 1;
    reused_mesh_ok = true;
    run(reused_mesh, buf, reused_mesh_ok, one_splat);
    CHECK(!reused_mesh_ok);
}

void test_unsupported_required_extension_is_error() {
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json =
        single_splat_json("\"nodes\":[{\"mesh\":0}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,",
                          "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]");
    bool ok = true;
    run(json, buf, ok);
    CHECK(!ok);  // hard error, not a loss
}

void test_optional_extension_declaration_and_unknown_color_space() {
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string optional =
        single_splat_json("\"nodes\":[{\"mesh\":0}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    const std::string extension_list = "[\"KHR_gaussian_splatting\"]";
    const auto extension_position = optional.find(extension_list);
    CHECK(extension_position != std::string::npos);
    if (extension_position == std::string::npos)
        return;
    optional.replace(extension_position, extension_list.size(),
                     "[\"KHR_gaussian_splatting\",\"EXT_meshopt_compression\"]");
    bool ok = false;
    auto result = run(optional, buf, ok);
    CHECK(ok);
    if (ok) {
        CHECK(has_loss(result.losses, "LOSS_EXTENSION_DECLARATION_DROPPED"));
        CHECK(!result.losses.has_blocking());
        CHECK(result.losses.check_policy({}).has_value());
    }

    optional.insert(optional.size() - 1, ",\"extensions\":{\"EXT_meshopt_compression\":{}}");
    ok = false;
    result = run(optional, buf, ok);
    CHECK(ok);
    if (ok) {
        CHECK(has_loss(result.losses, "LOSS_EXTENSION_DROPPED"));
        CHECK(has_loss(result.losses, "LOSS_EXTENSION_DECLARATION_DROPPED"));
        for (const LossItem& item : result.losses.items()) {
            if (item.code == "LOSS_EXTENSION_DECLARATION_DROPPED") {
                CHECK(item.remediation ==
                      "none needed because optional declarations do not change this output");
            }
        }
    }

    std::string unknown =
        single_splat_json("\"nodes\":[{\"mesh\":0}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    const std::string known = "srgb_rec709_display";
    const auto position = unknown.find(known);
    CHECK(position != std::string::npos);
    if (position != std::string::npos) {
        unknown.replace(position, known.size(), "unknown_color_space");
        ok = false;
        result = run(unknown, buf, ok);
        CHECK(!ok);
    }
}

void test_attribution_loss_requires_approval() {
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json =
        single_splat_json("\"nodes\":[{\"mesh\":0}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    const std::string asset = "\"asset\":{\"version\":\"2.0\"}";
    const auto position = json.find(asset);
    CHECK(position != std::string::npos);
    if (position == std::string::npos)
        return;
    json.replace(position, asset.size(),
                 "\"asset\":{\"version\":\"2.0\",\"copyright\":\"asset owner\"}");

    bool ok = false;
    auto result = run(json, buf, ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(has_loss(result.losses, loss_code::kAttributionDropped));
    CHECK(result.losses.has_blocking());
    CHECK(!result.losses.check_policy({}).has_value());
    CHECK(result.losses.check_policy({loss_code::kAttributionDropped}).has_value());
}

void test_no_splats_is_error() {
    // A scene with a node that has no mesh: nothing to read.
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json =
        single_splat_json("\"nodes\":[{}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    bool ok = true;
    run(json, buf, ok);
    CHECK(!ok);
}

void test_unreferenced_mesh_not_read() {
    // The mesh exists but no node instantiates it (empty scene): it must not be read, because it
    // has no placement in space.
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json =
        single_splat_json("\"nodes\":[{}],", "\"scenes\":[{\"nodes\":[]}],\"scene\":0");
    bool ok = true;
    run(json, buf, ok);
    CHECK(!ok);
}

// A degree-1 single splat: adds the three degree-1 SH coefficients after the DC term. Total 92
// bytes / 8 accessors.
std::vector<std::uint8_t> pack_degree1_splat() {
    std::vector<std::uint8_t> b;
    for (float f : {0.f, 0.f, 0.f})
        put_f32(b, f);  // POSITION @0
    for (float f : {0.f, 0.f, 0.f, 1.f})
        put_f32(b, f);  // ROTATION @12
    for (float f : {0.1f, 0.1f, 0.1f})
        put_f32(b, f);  // SCALE @28
    put_f32(b, 0.5f);   // OPACITY @40
    for (float f : {0.2f, 0.3f, 0.4f})
        put_f32(b, f);  // DC @44
    for (float f : {1.f, 1.f, 1.f})
        put_f32(b, f);  // SH1_0 @56
    for (float f : {2.f, 2.f, 2.f})
        put_f32(b, f);  // SH1_1 @68
    for (float f : {3.f, 3.f, 3.f})
        put_f32(b, f);  // SH1_2 @80
    return b;           // total 92
}

std::string degree1_json(const std::string& nodes, const std::string& scene) {
    return "{"
           "\"asset\":{\"version\":\"2.0\"},"
           "\"extensionsUsed\":[\"KHR_gaussian_splatting\"],"
           "\"buffers\":[{\"byteLength\":92}],"
           "\"bufferViews\":["
           "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12},"
           "{\"buffer\":0,\"byteOffset\":12,\"byteLength\":16},"
           "{\"buffer\":0,\"byteOffset\":28,\"byteLength\":12},"
           "{\"buffer\":0,\"byteOffset\":40,\"byteLength\":4},"
           "{\"buffer\":0,\"byteOffset\":44,\"byteLength\":12},"
           "{\"buffer\":0,\"byteOffset\":56,\"byteLength\":12},"
           "{\"buffer\":0,\"byteOffset\":68,\"byteLength\":12},"
           "{\"buffer\":0,\"byteOffset\":80,\"byteLength\":12}],"
           "\"accessors\":["
           "{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1,"
           "\"min\":[0,0,0],\"max\":[0,0,0]},"
           "{\"bufferView\":1,\"componentType\":5126,\"type\":\"VEC4\",\"count\":1},"
           "{\"bufferView\":2,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":3,\"componentType\":5126,\"type\":\"SCALAR\",\"count\":1},"
           "{\"bufferView\":4,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":5,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":6,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":7,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1}],"
           "\"meshes\":[{\"primitives\":[{\"mode\":0,\"attributes\":{"
           "\"POSITION\":0,\"KHR_gaussian_splatting:ROTATION\":1,\"KHR_gaussian_splatting:SCALE\":"
           "2,"
           "\"KHR_gaussian_splatting:OPACITY\":3,\"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0\":4,"
           "\"KHR_gaussian_splatting:SH_DEGREE_1_COEF_0\":5,"
           "\"KHR_gaussian_splatting:SH_DEGREE_1_COEF_1\":6,"
           "\"KHR_gaussian_splatting:SH_DEGREE_1_COEF_2\":7},"
           "\"extensions\":{\"KHR_gaussian_splatting\":{\"kernel\":\"ellipse\","
           "\"colorSpace\":\"srgb_rec709_display\"}}}]}]," +
           nodes + scene + "}";
}

void test_sh_rotation_applied_under_pure_rotation() {
    // Degree-1 splat under a pure rotation (90 deg about Z): the SH are rotated, so there is NO
    // LOSS_SH_ROTATION_NOT_APPLIED. A rotation about Z leaves the z-aligned degree-1 coefficient
    // (m=0, flat coefficient 2) invariant while mixing the m=-1/+1 coefficients, which is a
    // precise, convention-independent property to check end to end.
    const double s = std::sqrt(0.5);
    auto buf = pack_degree1_splat();  // DC {0.2,0.3,0.4}; deg-1 coeffs {1,1,1},{2,2,2},{3,3,3}
    std::string nodes = "\"nodes\":[{\"mesh\":0,\"rotation\":[0,0," + std::to_string(s) + "," +
                        std::to_string(s) + "]}],";
    bool ok = false;
    auto r = run(degree1_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buf, ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(r.sh_degree == 1);
    CHECK(!has_loss(r.losses, "LOSS_SH_ROTATION_NOT_APPLIED"));
    CHECK(!r.losses.has_blocking());
    const auto& raw = r.data.sh().raw();  // one splat: [DC, c1, c2, c3] x 3 channels
    // DC (coefficient 0) is rotation-invariant.
    CHECK(approx(raw[0], 0.2f) && approx(raw[1], 0.3f) && approx(raw[2], 0.4f));
    // m=0 (flat coefficient 2, the z term) is invariant under a rotation about Z.
    CHECK(approx(raw[6], 2.0f) && approx(raw[7], 2.0f) && approx(raw[8], 2.0f));
    // m=-1 and m=+1 (flat coefficients 1 and 3) were mixed by the rotation.
    const bool c1_changed = !(approx(raw[3], 1.0f) && approx(raw[4], 1.0f) && approx(raw[5], 1.0f));
    const bool c3_changed =
        !(approx(raw[9], 3.0f) && approx(raw[10], 3.0f) && approx(raw[11], 3.0f));
    CHECK(c1_changed && c3_changed);
}

void test_sh_rotation_applied_under_rotation_and_scale() {
    // A node combining a rotation (90 deg about Z) with a non-uniform scale: the rotation component
    // is now extracted by polar decomposition and applied to the SH, so there is NO loss, and the
    // m=0 (z-aligned) degree-1 coefficient stays invariant because the extracted rotation is about
    // Z.
    const double s = std::sqrt(0.5);
    auto buf = pack_degree1_splat();
    std::string nodes = "\"nodes\":[{\"mesh\":0,\"rotation\":[0,0," + std::to_string(s) + "," +
                        std::to_string(s) + "],\"scale\":[2,1,1]}],";
    bool ok = false;
    auto r = run(degree1_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buf, ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(!has_loss(r.losses, "LOSS_SH_ROTATION_NOT_APPLIED"));
    CHECK(!r.losses.has_blocking());
    const auto& raw = r.data.sh().raw();
    CHECK(approx(raw[6], 2.0f) && approx(raw[7], 2.0f) && approx(raw[8], 2.0f));  // m=0 invariant
    const bool c1_changed = !(approx(raw[3], 1.0f) && approx(raw[4], 1.0f) && approx(raw[5], 1.0f));
    CHECK(c1_changed);
}

void test_sh_rotation_applied_for_small_rotation() {
    auto buf = pack_degree1_splat();
    const std::string nodes = "\"nodes\":[{\"mesh\":0,\"rotation\":[0,0,4e-7,0.99999999999992]}],";
    bool ok = false;
    auto result = run(degree1_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buf, ok);
    CHECK(ok);
    if (!ok)
        return;
    const auto& raw = result.data.sh().raw();
    CHECK(raw[3] != 1.0f || raw[9] != 3.0f);
}

void test_undefined_profile_transforms_are_rejected() {
    // KHR rendering is undefined for a reflection, a singular transform, or material shear.
    auto buf = pack_degree1_splat();
    std::string nodes = "\"nodes\":[{\"mesh\":0,\"scale\":[-1,1,1]}],";
    bool ok = false;
    run(degree1_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buf, ok);
    CHECK(!ok);

    nodes = "\"nodes\":[{\"mesh\":0,\"scale\":[0,1,1]}],";
    ok = true;
    run(degree1_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buf, ok);
    CHECK(!ok);

    nodes = "\"nodes\":[{\"mesh\":0,\"matrix\":[1,0.5,0,0, 0,1,0,0, 0,0,1,0, "
            "0,0,0,1]}],";
    ok = true;
    run(degree1_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buf, ok);
    CHECK(!ok);
}

void test_small_positive_profile_transforms() {
    auto buffer = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string nodes = "\"nodes\":[{\"mesh\":0,\"scale\":[1e-20,1e-20,1e-20]}],";
    bool ok = false;
    const auto small =
        run(single_splat_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buffer, ok);
    CHECK(ok);
    if (ok) {
        CHECK(small.data.scales()[0].x > 0.0f);
        CHECK(small.data.scales()[0].x < 1e-20f);
    }

    nodes = "\"nodes\":[{\"mesh\":0,\"scale\":[1e-45,1e-45,1e-45]}],";
    ok = true;
    run(single_splat_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buffer, ok);
    CHECK(!ok);
}

void test_no_sh_loss_under_pure_scale() {
    // Degree-1 splat under a node that only scales (no rotation): SH needs no rotation, so there
    // must be no SH-rotation loss.
    auto buf = pack_degree1_splat();
    std::string nodes = "\"nodes\":[{\"mesh\":0,\"scale\":[2,2,2]}],";
    bool ok = false;
    auto r = run(degree1_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buf, ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(!has_loss(r.losses, "LOSS_SH_ROTATION_NOT_APPLIED"));
    CHECK(!r.losses.has_blocking());
}

void test_budget_bounds_allocation() {
    // The resource budget must refuse an allocation before it is made. A memory limit too small for
    // even one splat's SH block (a degree-0 splat needs 1*3*4 = 12 bytes) fails the read; the
    // default limits accept the same asset. This is the guard against a small shared-mesh file
    // describing an unbounded splat cloud.
    auto bin = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json =
        single_splat_json("\"nodes\":[{\"mesh\":0}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    auto glb = melkor::format::glb::build_glb(json, bin.data(), bin.size());
    CHECK(glb.has_value());
    if (!glb.has_value())
        return;
    melkor::Limits tight = melkor::Limits::for_profile(melkor::LimitsProfile::desktop);
    tight.max_memory_bytes = 8;  // less than one degree-0 splat's 12-byte SH allocation
    CHECK(!gltf::read_glb(glb.value().data(), glb.value().size(), tight).has_value());
    CHECK(gltf::read_glb(glb.value().data(), glb.value().size()).has_value());

    Limits accessors = Limits::for_profile(LimitsProfile::desktop);
    accessors.max_accessors = 9;
    CHECK(!gltf::read_glb(glb.value().data(), glb.value().size(), accessors).has_value());

    const std::string two_nodes = single_splat_json("\"nodes\":[{\"mesh\":0},{}],",
                                                    "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    auto two_node_glb = melkor::format::glb::build_glb(two_nodes, bin.data(), bin.size());
    CHECK(two_node_glb.has_value());
    if (two_node_glb.has_value()) {
        Limits nodes = Limits::for_profile(LimitsProfile::desktop);
        nodes.max_gltf_nodes = 1;
        CHECK(!gltf::read_glb(two_node_glb.value().data(), two_node_glb.value().size(), nodes)
                   .has_value());
    }

    const melkor::Limits invalid = melkor::Limits::for_profile(melkor::LimitsProfile::custom);
    CHECK(!gltf::read_glb(glb.value().data(), glb.value().size(), invalid).has_value());
}

void test_glb_buffer_uri_is_not_ignored() {
    auto bin = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json =
        single_splat_json("\"nodes\":[{\"mesh\":0}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    const std::string buffer = "\"byteLength\":56";
    const auto position = json.find(buffer);
    CHECK(position != std::string::npos);
    if (position == std::string::npos)
        return;
    json.insert(position + buffer.size(), ",\"uri\":\"external.bin\"");
    auto glb = melkor::format::glb::build_glb(json, bin.data(), bin.size());
    CHECK(glb.has_value());
    if (glb.has_value()) {
        CHECK(!gltf::read_glb(glb.value().data(), glb.value().size()).has_value());
    }
}

void test_read_glb_end_to_end() {
    // Wrap the JSON + BIN in a real GLB container with build_glb, then read it back with read_glb.
    // This exercises the whole path: container framing -> document -> scene -> SplatData.
    auto bin = pack_one_splat(7.f, 8.f, 9.f).bytes;
    std::string json =
        single_splat_json("\"nodes\":[{\"mesh\":0,\"translation\":[1,1,1]}],",
                          "\"scenes\":[{\"nodes\":[0]}],\"scene\":0", "", {7.f, 8.f, 9.f});
    auto glb = melkor::format::glb::build_glb(json, bin.data(), bin.size());
    CHECK(glb.has_value());
    if (!glb.has_value())
        return;
    auto r = gltf::read_glb(glb.value().data(), glb.value().size());
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(r.value().data.size() == 1);
        // splat at local (7,8,9), node translates by (1,1,1) -> (8,9,10).
        CHECK(approx(r.value().data.positions()[0].x, 8.f) &&
              approx(r.value().data.positions()[0].y, 9.f) &&
              approx(r.value().data.positions()[0].z, 10.f));
    }
    // A truncated GLB is a clean failure, not a crash.
    CHECK(!gltf::read_glb(glb.value().data(), 8).has_value());
    // Garbage bytes are a clean failure.
    std::vector<std::uint8_t> junk(64, 0xAB);
    CHECK(!gltf::read_glb(junk.data(), junk.size()).has_value());
}

void test_unknown_glb_chunk_is_blocking_loss() {
    auto bin = pack_one_splat(1.f, 2.f, 3.f).bytes;
    const std::string json =
        single_splat_json("\"nodes\":[{\"mesh\":0}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0",
                          "", {1.f, 2.f, 3.f});
    auto glb = melkor::format::glb::build_glb(json, bin.data(), bin.size());
    CHECK(glb.has_value());
    if (!glb.has_value())
        return;
    put_u32(glb.value(), 0);
    put_u32(glb.value(), 0x12345678U);
    set_u32(glb.value(), 8, static_cast<std::uint32_t>(glb.value().size()));

    const auto read = gltf::read_glb(glb.value().data(), glb.value().size());
    CHECK(read.has_value());
    if (!read.has_value())
        return;
    CHECK(has_loss(read.value().losses, "LOSS_GLTF_CONTENT_DROPPED"));
    CHECK(read.value().losses.has_blocking());
}

std::vector<std::uint8_t> make_control_test_glb() {
    auto bin = pack_one_splat(1.f, 2.f, 3.f).bytes;
    std::string json =
        single_splat_json("\"nodes\":[{\"mesh\":0}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0",
                          "", {1.f, 2.f, 3.f});
    return melkor::format::glb::build_glb(json, bin.data(), bin.size()).value();
}

void test_context_controls_glb_read() {
    const auto glb = make_control_test_glb();

    Budget cancelled_budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext cancelled_context = make_default_context(cancelled_budget);
    cancelled_context.cancellation.cancel();
    auto cancelled = gltf::read_glb(glb.data(), glb.size(), cancelled_context);
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error_code() == ErrorCode::cancelled);
    CHECK(cancelled_budget.used(BudgetKind::input_bytes) == 0);

    Budget deadline_budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext deadline_context = make_default_context(deadline_budget);
    deadline_context.deadline = Deadline::at(Deadline::TimePoint::min());
    auto expired = gltf::read_glb(glb.data(), glb.size(), deadline_context);
    CHECK(!expired.has_value());
    CHECK(expired.error_code() == ErrorCode::resource_limit);
    CHECK(expired.diagnostics()[0].code == "MK0304_DEADLINE_EXCEEDED");
    CHECK(deadline_budget.used(BudgetKind::input_bytes) == 0);
}

void test_context_reports_and_can_cancel_glb_read() {
    const auto glb = make_control_test_glb();

    Budget progress_budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext progress_context = make_default_context(progress_budget);
    RecordingProgressSink progress;
    progress_context.progress = &progress;
    {
        auto read = gltf::read_glb(glb.data(), glb.size(), progress_context);
        CHECK(read.has_value());
        if (read.has_value()) {
            CHECK(read.value().retained_memory_bytes() > 0);
            CHECK(progress_budget.used(BudgetKind::memory_bytes) ==
                  read.value().retained_memory_bytes());
        }
    }
    CHECK(progress_budget.used(BudgetKind::memory_bytes) == 0);
    CHECK(!progress.events.empty());
    CHECK(progress.events.front().phase == "container");
    CHECK(progress.events.back().phase == "complete");
    CHECK(progress.events.back().completed == 1);

    Budget cancel_budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext cancel_context = make_default_context(cancel_budget);
    CancelOnPhaseSink cancel(cancel_context.cancellation, "canonicalize");
    cancel_context.progress = &cancel;
    auto cancelled = gltf::read_glb(glb.data(), glb.size(), cancel_context);
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error_code() == ErrorCode::cancelled);
}

}  // namespace

int main() {
    test_translation_is_applied();
    test_parent_child_transform_composes();
    test_invalid_node_graphs_fail();
    test_unsupported_required_extension_is_error();
    test_optional_extension_declaration_and_unknown_color_space();
    test_attribution_loss_requires_approval();
    test_no_splats_is_error();
    test_unreferenced_mesh_not_read();
    test_sh_rotation_applied_under_pure_rotation();
    test_sh_rotation_applied_under_rotation_and_scale();
    test_sh_rotation_applied_for_small_rotation();
    test_undefined_profile_transforms_are_rejected();
    test_small_positive_profile_transforms();
    test_no_sh_loss_under_pure_scale();
    test_budget_bounds_allocation();
    test_read_glb_end_to_end();
    test_unknown_glb_chunk_is_blocking_loss();
    test_glb_buffer_uri_is_not_ignored();
    test_context_controls_glb_read();
    test_context_reports_and_can_cancel_glb_read();

    if (g_failures == 0) {
        std::printf("gltf scene: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf scene: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
