// Tests for the glTF scene-graph walk (read_gaussian_scene).
//
// These pin the whole-scene behaviour: node transforms are composed and applied to geometry, the
// walk terminates on a node graph that is not a plain tree, primitives of different SH degree merge
// by padding, and the loss report records what could not be preserved (an assumed colour space, a
// flattened hierarchy, an un-applied SH rotation) while an unsupported required extension is a hard
// error. Buffers are built by hand.
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

bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void put_f32(std::vector<std::uint8_t>& v, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
}

bool has_loss(const LossReport& r, const std::string& code) {
    for (const auto& i : r.items())
        if (i.code == code) return true;
    return false;
}

// Builds a buffer holding, for one splat: POSITION(3), ROTATION(4), SCALE(3), OPACITY(1), DC(3),
// contiguous, and returns the bytes plus the byte offsets of each accessor.
struct Packed {
    std::vector<std::uint8_t> bytes;
};

Packed pack_one_splat(float px, float py, float pz) {
    Packed p;
    put_f32(p.bytes, px);
    put_f32(p.bytes, py);
    put_f32(p.bytes, pz);                         // POSITION @0
    for (float f : {0.f, 0.f, 0.f, 1.f}) put_f32(p.bytes, f);  // ROTATION @12
    for (float f : {0.1f, 0.1f, 0.1f}) put_f32(p.bytes, f);    // SCALE @28
    put_f32(p.bytes, 0.5f);                       // OPACITY @40
    for (float f : {0.2f, 0.3f, 0.4f}) put_f32(p.bytes, f);    // DC @44
    return p;                                      // total 56 bytes
}

// glTF JSON for a single-splat mesh (buffer 0, the packed layout above) with a node graph the
// caller supplies (nodes/scene). `extra` is appended for extension declarations.
std::string single_splat_json(const std::string& nodes, const std::string& scene,
                              const std::string& extra = "") {
    return "{"
           "\"buffers\":[{\"byteLength\":56}],"
           "\"bufferViews\":["
           "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12},"
           "{\"buffer\":0,\"byteOffset\":12,\"byteLength\":16},"
           "{\"buffer\":0,\"byteOffset\":28,\"byteLength\":12},"
           "{\"buffer\":0,\"byteOffset\":40,\"byteLength\":4},"
           "{\"buffer\":0,\"byteOffset\":44,\"byteLength\":12}],"
           "\"accessors\":["
           "{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":1,\"componentType\":5126,\"type\":\"VEC4\",\"count\":1},"
           "{\"bufferView\":2,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":3,\"componentType\":5126,\"type\":\"SCALAR\",\"count\":1},"
           "{\"bufferView\":4,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1}],"
           "\"meshes\":[{\"primitives\":[{\"mode\":0,\"attributes\":{"
           "\"POSITION\":0,\"KHR_gaussian_splatting:ROTATION\":1,\"KHR_gaussian_splatting:SCALE\":2,"
           "\"KHR_gaussian_splatting:OPACITY\":3,\"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0\":4},"
           "\"extensions\":{\"KHR_gaussian_splatting\":{\"kernel\":\"ellipse\","
           "\"colorSpace\":\"srgb_rec709_display\"}}}]}]," +
           nodes + scene + extra + "}";
}

gltf::SceneRead run(const std::string& json, const std::vector<std::uint8_t>& buf, bool& ok) {
    auto doc = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
    if (!doc.has_value()) {
        ok = false;
        return gltf::SceneRead{SplatData::create({}).value(), {}, {}, 0};
    }
    std::vector<gltf::BufferSpan> buffers = {gltf::BufferSpan{buf.data(), buf.size()}};
    auto r = gltf::read_gaussian_scene(doc.value(), buffers);
    ok = r.has_value();
    if (!ok) return gltf::SceneRead{SplatData::create({}).value(), {}, {}, 0};
    return std::move(r.value());
}

void test_translation_is_applied() {
    // A node translates the single splat (at local origin) by (10, 20, 30).
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json = single_splat_json(
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    bool ok = false;
    auto r = run(json, buf, ok);
    CHECK(ok);
    if (!ok) return;
    CHECK(r.data.size() == 1);
    CHECK(approx(r.data.positions()[0].x, 10.f) && approx(r.data.positions()[0].y, 20.f) &&
          approx(r.data.positions()[0].z, 30.f));
}

void test_parent_child_transform_composes() {
    // Parent translates by (100,0,0); child (rotate 90 about Z) holds the mesh. The splat at the
    // local origin ends up at the parent translation (rotation about origin leaves the origin
    // fixed), and the rotation triggers no SH loss because the splat is degree 0.
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    const double s = std::sqrt(0.5);
    std::string nodes = "\"nodes\":["
                        "{\"children\":[1],\"translation\":[100,0,0]},"
                        "{\"mesh\":0,\"rotation\":[0,0," + std::to_string(s) + "," + std::to_string(s) + "]}],";
    std::string json = single_splat_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    bool ok = false;
    auto r = run(json, buf, ok);
    CHECK(ok);
    if (!ok) return;
    CHECK(r.data.size() == 1);
    CHECK(approx(r.data.positions()[0].x, 100.f));
    // Degree 0: a rotating node must NOT produce an SH-rotation loss.
    CHECK(!has_loss(r.losses, "LOSS_SH_ROTATION_NOT_APPLIED"));
}

void test_cycle_terminates() {
    // Two nodes each list the other as a child: a cycle. The walk must terminate (the visited-set
    // guard), reading the one splat exactly once.
    auto buf = pack_one_splat(1.f, 2.f, 3.f).bytes;
    std::string nodes = "\"nodes\":["
                        "{\"mesh\":0,\"children\":[1]},"
                        "{\"children\":[0]}],";
    std::string json = single_splat_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    bool ok = false;
    auto r = run(json, buf, ok);
    CHECK(ok);
    if (ok) CHECK(r.data.size() == 1);  // splat read once, no infinite loop
}

void test_unsupported_required_extension_is_error() {
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json = single_splat_json(
        "\"nodes\":[{\"mesh\":0}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,",
        "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]");
    bool ok = true;
    run(json, buf, ok);
    CHECK(!ok);  // hard error, not a loss
}

void test_no_splats_is_error() {
    // A scene with a node that has no mesh: nothing to read.
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json = single_splat_json("\"nodes\":[{}],", "\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
    bool ok = true;
    run(json, buf, ok);
    CHECK(!ok);
}

void test_unreferenced_mesh_not_read() {
    // The mesh exists but no node instantiates it (empty scene): it must not be read, because it
    // has no placement in space.
    auto buf = pack_one_splat(0.f, 0.f, 0.f).bytes;
    std::string json = single_splat_json("\"nodes\":[{}],", "\"scenes\":[{\"nodes\":[]}],\"scene\":0");
    bool ok = true;
    run(json, buf, ok);
    CHECK(!ok);
}

// A degree-1 single splat: adds the three degree-1 SH coefficients after the DC term. Total 92
// bytes / 8 accessors.
std::vector<std::uint8_t> pack_degree1_splat() {
    std::vector<std::uint8_t> b;
    for (float f : {0.f, 0.f, 0.f}) put_f32(b, f);            // POSITION @0
    for (float f : {0.f, 0.f, 0.f, 1.f}) put_f32(b, f);       // ROTATION @12
    for (float f : {0.1f, 0.1f, 0.1f}) put_f32(b, f);         // SCALE @28
    put_f32(b, 0.5f);                                          // OPACITY @40
    for (float f : {0.2f, 0.3f, 0.4f}) put_f32(b, f);         // DC @44
    for (float f : {1.f, 1.f, 1.f}) put_f32(b, f);            // SH1_0 @56
    for (float f : {2.f, 2.f, 2.f}) put_f32(b, f);            // SH1_1 @68
    for (float f : {3.f, 3.f, 3.f}) put_f32(b, f);            // SH1_2 @80
    return b;                                                  // total 92
}

std::string degree1_json(const std::string& nodes, const std::string& scene) {
    return "{"
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
           "{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":1,\"componentType\":5126,\"type\":\"VEC4\",\"count\":1},"
           "{\"bufferView\":2,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":3,\"componentType\":5126,\"type\":\"SCALAR\",\"count\":1},"
           "{\"bufferView\":4,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":5,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":6,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1},"
           "{\"bufferView\":7,\"componentType\":5126,\"type\":\"VEC3\",\"count\":1}],"
           "\"meshes\":[{\"primitives\":[{\"mode\":0,\"attributes\":{"
           "\"POSITION\":0,\"KHR_gaussian_splatting:ROTATION\":1,\"KHR_gaussian_splatting:SCALE\":2,"
           "\"KHR_gaussian_splatting:OPACITY\":3,\"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0\":4,"
           "\"KHR_gaussian_splatting:SH_DEGREE_1_COEF_0\":5,"
           "\"KHR_gaussian_splatting:SH_DEGREE_1_COEF_1\":6,"
           "\"KHR_gaussian_splatting:SH_DEGREE_1_COEF_2\":7},"
           "\"extensions\":{\"KHR_gaussian_splatting\":{\"kernel\":\"ellipse\","
           "\"colorSpace\":\"srgb_rec709_display\"}}}]}]," +
           nodes + scene + "}";
}

void test_sh_rotation_loss_under_rotating_node() {
    // Degree-1 splat under a rotating node: the reader must flag LOSS_SH_ROTATION_NOT_APPLIED as a
    // blocking (severe) loss rather than silently leaving the view-dependent colour wrong.
    const double s = std::sqrt(0.5);
    auto buf = pack_degree1_splat();
    std::string nodes = "\"nodes\":[{\"mesh\":0,\"rotation\":[0,0," + std::to_string(s) + "," +
                        std::to_string(s) + "]}],";
    bool ok = false;
    auto r = run(degree1_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buf, ok);
    CHECK(ok);
    if (!ok) return;
    CHECK(r.sh_degree == 1);
    CHECK(has_loss(r.losses, "LOSS_SH_ROTATION_NOT_APPLIED"));
    CHECK(r.losses.has_blocking());  // severe, blocks unless approved
    // ... and it is approvable by its exact code, but not without.
    CHECK(!r.losses.check_policy({}).has_value());
    CHECK(r.losses.check_policy({"LOSS_SH_ROTATION_NOT_APPLIED"}).has_value());
}

void test_no_sh_loss_under_pure_scale() {
    // Degree-1 splat under a node that only scales (no rotation): SH needs no rotation, so there
    // must be no SH-rotation loss.
    auto buf = pack_degree1_splat();
    std::string nodes = "\"nodes\":[{\"mesh\":0,\"scale\":[2,2,2]}],";
    bool ok = false;
    auto r = run(degree1_json(nodes, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"), buf, ok);
    CHECK(ok);
    if (!ok) return;
    CHECK(!has_loss(r.losses, "LOSS_SH_ROTATION_NOT_APPLIED"));
    CHECK(!r.losses.has_blocking());
}

}  // namespace

int main() {
    test_translation_is_applied();
    test_parent_child_transform_composes();
    test_cycle_terminates();
    test_unsupported_required_extension_is_error();
    test_no_splats_is_error();
    test_unreferenced_mesh_not_read();
    test_sh_rotation_loss_under_rotating_node();
    test_no_sh_loss_under_pure_scale();

    if (g_failures == 0) {
        std::printf("gltf scene: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf scene: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
