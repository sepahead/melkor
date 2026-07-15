// Tests for glTF JSON document parsing.
//
// These feed glTF JSON strings (well-formed and adversarial) and pin what the reduced Document
// captures and what it rejects: malformed JSON, wrong-typed fields, negative/out-of-range indices,
// an incomplete KHR_gaussian_splatting extension, and an unsupported accessor type. Reference-graph
// validation is checked so that a document which parses has self-consistent indices.
//
// Self-contained (no external test framework).

#include "melkor/format/gltf_document.hpp"

#include <cstdio>
#include <string>

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

auto parse(const std::string& s) {
    return gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// A minimal but complete splat glTF: one buffer, one bufferView, one POSITION accessor, one mesh
// with a POINTS primitive carrying KHR_gaussian_splatting, one node, one scene.
const char* kSplatDoc = R"({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_gaussian_splatting"],
  "extensionsRequired": ["KHR_gaussian_splatting"],
  "buffers": [{"byteLength": 48}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 48}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "type": "VEC3", "count": 4},
    {"bufferView": 0, "componentType": 5121, "type": "SCALAR", "count": 4, "normalized": true}
  ],
  "meshes": [{
    "primitives": [{
      "mode": 0,
      "attributes": {"POSITION": 0, "KHR_gaussian_splatting:OPACITY": 1},
      "extensions": {"KHR_gaussian_splatting": {"kernel": "ellipse", "colorSpace": "srgb_rec709_display"}}
    }]
  }],
  "nodes": [{"mesh": 0, "translation": [1.0, 2.0, 3.0]}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})";

void test_parses_valid_splat_doc() {
    auto r = parse(kSplatDoc);
    CHECK(r.has_value());
    if (!r.has_value()) return;
    const auto& d = r.value();
    CHECK(d.buffers.size() == 1 && d.buffers[0].byte_length == 48);
    CHECK(d.buffer_views.size() == 1 && d.buffer_views[0].byte_length == 48);
    CHECK(d.accessors.size() == 2);
    CHECK(d.accessors[0].component == gltf::ComponentType::f32);
    CHECK(d.accessors[0].element == gltf::ElementType::vec3);
    CHECK(d.accessors[0].count == 4 && !d.accessors[0].normalized);
    CHECK(d.accessors[1].component == gltf::ComponentType::u8);
    CHECK(d.accessors[1].normalized);
    CHECK(d.meshes.size() == 1 && d.meshes[0].primitives.size() == 1);
    const auto& p = d.meshes[0].primitives[0];
    CHECK(p.mode == 0);
    CHECK(p.attributes.at("POSITION") == 0);
    CHECK(p.attributes.at("KHR_gaussian_splatting:OPACITY") == 1);
    CHECK(p.gaussian.has_value());
    if (p.gaussian.has_value()) {
        CHECK(p.gaussian->kernel == "ellipse");
        CHECK(p.gaussian->color_space == "srgb_rec709_display");
        CHECK(p.gaussian->projection == "perspective");         // default filled in
        CHECK(p.gaussian->sorting_method == "cameraDistance");  // default filled in
    }
    CHECK(d.nodes.size() == 1 && d.nodes[0].mesh.has_value() && d.nodes[0].mesh.value() == 0);
    CHECK(d.nodes[0].translation[0] == 1.0 && d.nodes[0].translation[2] == 3.0);
    CHECK(d.nodes[0].rotation[3] == 1.0);  // identity default
    CHECK(d.scene_roots.size() == 1 && d.scene_roots[0] == 0);
    CHECK(d.extensions_required.size() == 1 && d.extensions_required[0] == "KHR_gaussian_splatting");
}

void test_rejects_malformed_json() {
    CHECK(!parse("").has_value());
    CHECK(!parse("not json").has_value());
    CHECK(!parse("{ \"asset\": ").has_value());  // truncated
    CHECK(!parse("[1,2,3]").has_value());          // root not an object
}

void test_rejects_bad_indices_and_types() {
    // Negative accessor index in an attribute.
    CHECK(!parse(R"({"accessors":[{"componentType":5126,"type":"VEC3","count":1}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":-1}}]}]})")
               .has_value());
    // Out-of-range accessor reference.
    CHECK(!parse(R"({"accessors":[{"componentType":5126,"type":"VEC3","count":1}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":5}}]}]})")
               .has_value());
    // bufferView.buffer out of range.
    CHECK(!parse(R"({"buffers":[{"byteLength":4}],"bufferViews":[{"buffer":9,"byteLength":4}]})")
               .has_value());
    // node.mesh out of range.
    CHECK(!parse(R"({"nodes":[{"mesh":3}]})").has_value());
    // scene root out of range (no nodes).
    CHECK(!parse(R"({"scenes":[{"nodes":[0]}],"scene":0})").has_value());
    // componentType that is not a float where an index expects an integer.
    CHECK(!parse(R"({"accessors":[{"componentType":1.5,"type":"VEC3","count":1}]})").has_value());
}

void test_rejects_unsupported_accessor_type() {
    // MAT4 is valid glTF but unsupported by the splat reader: clean failure, not a silent mishandle.
    CHECK(!parse(R"({"accessors":[{"componentType":5126,"type":"MAT4","count":1}]})").has_value());
    // Unknown componentType constant.
    CHECK(!parse(R"({"accessors":[{"componentType":9999,"type":"VEC3","count":1}]})").has_value());
}

void test_rejects_incomplete_khr() {
    // KHR_gaussian_splatting missing the required colorSpace.
    CHECK(!parse(R"({"meshes":[{"primitives":[{"attributes":{},
      "extensions":{"KHR_gaussian_splatting":{"kernel":"ellipse"}}}]}]})")
               .has_value());
}

void test_matrix_and_trs_nodes() {
    // A node with a 16-element matrix.
    auto r = parse(R"({"nodes":[{"matrix":[1,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1]}]})");
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(r.value().nodes.size() == 1);
        CHECK(r.value().nodes[0].matrix.has_value());
        if (r.value().nodes[0].matrix.has_value()) {
            CHECK(r.value().nodes[0].matrix.value()[12] == 5.0);
            CHECK(r.value().nodes[0].matrix.value()[15] == 1.0);
        }
    }
    // A node.matrix that is the wrong length is rejected.
    CHECK(!parse(R"({"nodes":[{"matrix":[1,0,0]}]})").has_value());
    // A rotation quaternion with the wrong element count is rejected.
    CHECK(!parse(R"({"nodes":[{"rotation":[0,0,1]}]})").has_value());
}

void test_primitive_without_extension_is_not_gaussian() {
    auto r = parse(R"({"accessors":[{"componentType":5126,"type":"VEC3","count":1}],
      "meshes":[{"primitives":[{"mode":4,"attributes":{"POSITION":0}}]}]})");
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(!r.value().meshes[0].primitives[0].gaussian.has_value());
        CHECK(r.value().meshes[0].primitives[0].mode == 4);
    }
}

}  // namespace

int main() {
    test_parses_valid_splat_doc();
    test_rejects_malformed_json();
    test_rejects_bad_indices_and_types();
    test_rejects_unsupported_accessor_type();
    test_rejects_incomplete_khr();
    test_matrix_and_trs_nodes();
    test_primitive_without_extension_is_not_gaussian();

    if (g_failures == 0) {
        std::printf("gltf document: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf document: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
