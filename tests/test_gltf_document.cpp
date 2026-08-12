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
#include <utility>

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

auto parse_raw(const std::string& s) {
    return gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

auto parse(const std::string& s) {
    if (s.find("\"asset\"") != std::string::npos)
        return parse_raw(s);
    if (s.empty() || s.front() != '{')
        return parse_raw(s);
    return parse_raw("{\"asset\":{\"version\":\"2.0\"}," + s.substr(1));
}

class CancelOnJsonProgress final : public ProgressSink {
public:
    explicit CancelOnJsonProgress(CancellationToken cancellation)
        : cancellation_(std::move(cancellation)) {}

    void on_progress(const ProgressEvent& event) override {
        if (event.phase == "json_parse" && event.unit == "events" && event.completed >= 4096)
            cancellation_.cancel();
    }

private:
    CancellationToken cancellation_;
};

class CancelOnStructureProgress final : public ProgressSink {
public:
    explicit CancelOnStructureProgress(CancellationToken cancellation)
        : cancellation_(std::move(cancellation)) {}

    void on_progress(const ProgressEvent& event) override {
        if (event.phase == "json_structure")
            cancellation_.cancel();
    }

private:
    CancellationToken cancellation_;
};

class CancelOnFeatureProgress final : public ProgressSink {
public:
    explicit CancelOnFeatureProgress(CancellationToken cancellation)
        : cancellation_(std::move(cancellation)) {}

    void on_progress(const ProgressEvent& event) override {
        if (event.phase == "source_features" && event.completed >= 4096)
            cancellation_.cancel();
    }

private:
    CancellationToken cancellation_;
};

// A complete splat glTF with one interleaved vertex buffer and one scene.
const char* kSplatDoc = R"({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_gaussian_splatting"],
  "extensionsRequired": ["KHR_gaussian_splatting"],
  "buffers": [{"byteLength": 64}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 64,
                   "byteStride": 16, "target": 34962}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "type": "VEC3", "count": 4,
     "min": [0.0, 0.0, 0.0], "max": [1.0, 1.0, 1.0]},
    {"bufferView": 0, "byteOffset": 12, "componentType": 5121,
     "type": "SCALAR", "count": 4, "normalized": true}
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
    if (!r.has_value())
        return;
    const auto& d = r.value();
    CHECK(d.buffers.size() == 1 && d.buffers[0].byte_length == 64);
    CHECK(d.buffer_views.size() == 1 && d.buffer_views[0].byte_length == 64);
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
    CHECK(d.extensions_required.size() == 1 &&
          d.extensions_required[0] == "KHR_gaussian_splatting");
}

void test_rejects_malformed_json() {
    CHECK(!parse("").has_value());
    CHECK(!parse("not json").has_value());
    CHECK(!parse("{ \"asset\": ").has_value());  // truncated
    CHECK(!parse("[1,2,3]").has_value());        // root not an object
    CHECK(!parse_raw(R"({"asset":{"version":"2.0","version":"2.0"}})").has_value());

    std::string deep = R"({"asset":{"version":"2.0"},"extras":)";
    deep.append(257, '[');
    deep += '0';
    deep.append(257, ']');
    deep += '}';
    auto depth = parse_raw(deep);
    CHECK(!depth.has_value());
    CHECK(depth.error_code() == ErrorCode::resource_limit);
    CHECK(depth.diagnostics()[0].code == "MK2148_GLTF_JSON_DEPTH");
}

void test_context_controls_json_parse() {
    std::string document = R"({"asset":{"version":"2.0"},"extras":[)";
    for (int i = 0; i < 5000; ++i) {
        if (i != 0)
            document += ',';
        document += "0";
    }
    document += "]}";

    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    CancelOnJsonProgress progress(context.cancellation);
    context.progress = &progress;
    auto cancelled = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(document.data()),
                                           document.size(), context);
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error_code() == ErrorCode::cancelled);

    Budget structure_budget(limits);
    OperationContext structure_context = make_default_context(structure_budget);
    CancelOnStructureProgress structure_progress(structure_context.cancellation);
    structure_context.progress = &structure_progress;
    auto structure_cancelled = gltf::parse_gltf_json(
        reinterpret_cast<const std::uint8_t*>(document.data()), document.size(), structure_context);
    CHECK(!structure_cancelled.has_value());
    CHECK(structure_cancelled.error_code() == ErrorCode::cancelled);

    OperationContext no_budget;
    auto missing_budget = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(kSplatDoc),
                                                std::string(kSplatDoc).size(), no_budget);
    CHECK(!missing_budget.has_value());
    CHECK(missing_budget.error_code() == ErrorCode::internal_error);
}

void test_context_controls_feature_scan() {
    std::string document = R"({"asset":{"version":"2.0"},"buffers":[)";
    for (int i = 0; i < 4200; ++i) {
        if (i != 0)
            document += ',';
        document += R"({"byteLength":1})";
    }
    document += "]}";

    Budget budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext context = make_default_context(budget);
    CancelOnFeatureProgress progress(context.cancellation);
    context.progress = &progress;
    auto cancelled = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(document.data()),
                                           document.size(), context);
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error_code() == ErrorCode::cancelled);
}

void test_context_enforces_structural_limits() {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    limits.max_accessors = 1;
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    auto limited = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(kSplatDoc),
                                         std::string(kSplatDoc).size(), context);
    CHECK(!limited.has_value());
    CHECK(limited.error_code() == ErrorCode::resource_limit);

    Limits metadata_limits = Limits::for_profile(LimitsProfile::desktop);
    metadata_limits.max_metadata_string_bytes = 10;
    metadata_limits.max_metadata_total_bytes = 64;
    Budget metadata_budget(metadata_limits);
    OperationContext metadata_context = make_default_context(metadata_budget);
    const std::string long_generator = R"({"asset":{"version":"2.0","generator":"12345678901"}})";
    auto long_string =
        gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(long_generator.data()),
                              long_generator.size(), metadata_context);
    CHECK(!long_string.has_value());
    CHECK(long_string.error_code() == ErrorCode::resource_limit);
    CHECK(long_string.diagnostics()[0].code == "MK2147_GLTF_METADATA_LIMIT");

    metadata_limits.max_metadata_string_bytes = 16;
    metadata_limits.max_metadata_total_bytes = 20;
    Budget total_budget(metadata_limits);
    OperationContext total_context = make_default_context(total_budget);
    const std::string too_much_metadata =
        R"({"asset":{"version":"2.0","generator":"tool","copyright":"owner"}})";
    auto total =
        gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(too_much_metadata.data()),
                              too_much_metadata.size(), total_context);
    CHECK(!total.has_value());
    CHECK(total.error_code() == ErrorCode::resource_limit);

    metadata_limits.max_metadata_string_bytes = 16;
    metadata_limits.max_metadata_total_bytes = 64;
    Budget data_uri_budget(metadata_limits);
    OperationContext data_uri_context = make_default_context(data_uri_budget);
    const std::string data_uri =
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":1,"uri":"data:application/octet-stream;base64,AA=="}]})";
    auto payload = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(data_uri.data()),
                                         data_uri.size(), data_uri_context);
    CHECK(payload.has_value());
}

void test_context_owns_document_memory_charge() {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    {
        auto parsed = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(kSplatDoc),
                                            std::string(kSplatDoc).size(), context);
        CHECK(parsed.has_value());
        CHECK(budget.used(BudgetKind::memory_bytes) > 0);
    }
    CHECK(budget.used(BudgetKind::memory_bytes) == 0);

    limits.max_memory_bytes = 1;
    Budget tight_budget(limits);
    OperationContext tight_context = make_default_context(tight_budget);
    auto limited = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(kSplatDoc),
                                         std::string(kSplatDoc).size(), tight_context);
    CHECK(!limited.has_value());
    CHECK(limited.error_code() == ErrorCode::resource_limit);
    CHECK(tight_budget.used(BudgetKind::memory_bytes) == 0);
}

void test_records_content_that_the_canonical_scene_drops() {
    auto parsed = parse(R"({
      "asset":{"version":"2.0","generator":"tool","copyright":"owner"},
      "accessors":[{"componentType":5126,"type":"SCALAR","count":1,
                    "sparse":{"count":1}}]
    })");
    CHECK(parsed.has_value());
    if (!parsed.has_value())
        return;
    CHECK(parsed.value().source_features.provenance_count == 1);
    CHECK(parsed.value().source_features.attribution_count == 1);
    CHECK(parsed.value().source_features.attribution_samples[0] == "asset.copyright");
    CHECK(parsed.value().source_features.property_count == 1);
    CHECK(parsed.value().source_features.property_samples[0] == "accessors[0].sparse");
}

void test_rejects_asset_and_extension_contracts() {
    CHECK(!parse_raw(R"({})").has_value());
    CHECK(!parse_raw(R"({"asset":{}})").has_value());
    CHECK(!parse_raw(R"({"asset":{"version":2.0}})").has_value());
    CHECK(!parse_raw(R"({"asset":{"version":"1.0"}})").has_value());
    CHECK(parse_raw(R"({"asset":{"version":"2.0","minVersion":"1.0"}})").has_value());
    auto future_version = parse_raw(R"({"asset":{"version":"2.0","minVersion":"2.1"}})");
    CHECK(!future_version.has_value());
    CHECK(future_version.error_code() == ErrorCode::unsupported_feature);
    auto malformed_version = parse_raw(R"({"asset":{"version":"2.0","minVersion":"next"}})");
    CHECK(!malformed_version.has_value());
    CHECK(malformed_version.error_code() == ErrorCode::invalid_data);
    auto overlong_version =
        parse_raw(R"({"asset":{"version":"2.0","minVersion":"999999999999999999999999.0"}})");
    CHECK(!overlong_version.has_value());
    CHECK(overlong_version.error_code() == ErrorCode::invalid_data);
    CHECK(!parse_raw(R"({"asset":{"version":"2.0","minVersion":"02.0"}})").has_value());
    CHECK(!parse_raw(R"({"asset":{"version":"2.0","minVersion":"2.00"}})").has_value());
    auto largest_future = parse_raw(R"({"asset":{"version":"2.0","minVersion":"999999999.0"}})");
    CHECK(!largest_future.has_value());
    CHECK(largest_future.error_code() == ErrorCode::unsupported_feature);
    CHECK(parse_raw(R"({"asset":{"version":"2.0","generator":"","copyright":""}})").has_value());
    CHECK(!parse_raw(R"({"asset":{"version":"2.0","generator":1}})").has_value());
    CHECK(!parse_raw(R"({"asset":{"version":"2.0","copyright":false}})").has_value());
    CHECK(!parse_raw(R"({"asset":{"version":"2.0"},
      "extensionsRequired":["KHR_gaussian_splatting"]})")
               .has_value());
}

void test_requires_declarations_for_extension_occurrences() {
    CHECK(!parse(R"({"extensions":{"EXT_example":{}}})").has_value());

    auto declared = parse(R"({"extensionsUsed":["EXT_example"],
      "extensions":{"EXT_example":{}}})");
    CHECK(declared.has_value());
    if (declared.has_value()) {
        CHECK(declared.value().source_features.extension_count == 1);
    }

    CHECK(!parse(R"({"materials":[{"extensions":{"EXT_example":{}}}]})").has_value());
    CHECK(parse(R"({"extensionsUsed":["EXT_example"],
      "materials":[{"extensions":{"EXT_example":{}}}]})")
              .has_value());

    // Application data in `extras` does not declare a glTF extension occurrence.
    CHECK(parse(R"({"extras":{"extensions":{"application-key":true}}})").has_value());

    CHECK(!parse(R"({"extensions":[]})").has_value());
    CHECK(!parse(R"({"materials":[{"extensions":[]}]})").has_value());

    CHECK(!parse(R"({"extensionsUsed":["EXT_outer"],
      "extensions":{"EXT_outer":{"extensions":{"EXT_inner":{}}}}})")
               .has_value());
    CHECK(parse(R"({"extensionsUsed":["EXT_inner","EXT_outer"],
      "extensions":{"EXT_outer":{"extensions":{"EXT_inner":{}}}}})")
              .has_value());
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
    CHECK(!parse(R"({"scene":0})").has_value());
    CHECK(!parse(R"({"scenes":{},"scene":0})").has_value());
    CHECK(!parse(R"({"scenes":[],"scene":0})").has_value());
    CHECK(!parse(R"({"scenes":[1],"scene":0})").has_value());
    CHECK(!parse(R"({"scenes":[{"nodes":{}}],"scene":0})").has_value());
    CHECK(!parse(R"({"nodes":[{}],"scenes":[{"nodes":[]}],"scene":0})").has_value());
    CHECK(!parse(R"({"nodes":[{}],"scenes":[{"nodes":[0,0]}],"scene":0})").has_value());
    CHECK(!parse(R"({"nodes":[{}],"scenes":[{}, {"nodes":[2]}],"scene":0})").has_value());
}

void test_rejects_unsupported_accessor_type() {
    // MAT4 is valid glTF. The structural parser accepts it when no splat attribute uses it.
    CHECK(parse(R"({"accessors":[{"componentType":5126,"type":"MAT4","count":1}]})").has_value());
    CHECK(parse(R"({"accessors":[{"componentType":5126,"type":"MAT4","count":1,
      "min":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
      "max":[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]}]})")
              .has_value());
    // Unknown componentType constant.
    CHECK(!parse(R"({"accessors":[{"componentType":9999,"type":"VEC3","count":1}]})").has_value());
    CHECK(!parse(R"({"accessors":[{"componentType":5126,"type":"VEC3","count":0}]})").has_value());
    CHECK(!parse(R"({"accessors":[{"componentType":5126,"type":"VEC3","count":1,
      "normalized":true}]})")
               .has_value());
    CHECK(!parse(R"({"accessors":[{"byteOffset":4,"componentType":5126,
      "type":"SCALAR","count":1}]})")
               .has_value());
    CHECK(!parse(R"({"accessors":[]})").has_value());
    CHECK(!parse(R"({"meshes":[]})").has_value());
    CHECK(!parse(R"({"nodes":[]})").has_value());
    CHECK(!parse(R"({"buffers":[{"byteLength":0}]})").has_value());
    CHECK(!parse(R"({"buffers":[{"byteLength":8}],
      "bufferViews":[{"buffer":0,"byteLength":0}]})")
               .has_value());
    CHECK(!parse(R"({"buffers":[{"byteLength":8}],
      "bufferViews":[{"buffer":0,"byteLength":8,"byteStride":3}]})")
               .has_value());
    CHECK(!parse(R"({"buffers":[{"byteLength":256}],
      "bufferViews":[{"buffer":0,"byteLength":256,"byteStride":256}]})")
               .has_value());
    CHECK(!parse(R"({"buffers":[{"byteLength":8}],
      "bufferViews":[{"buffer":0,"byteLength":8,"target":1}]})")
               .has_value());
}

void test_vertex_buffer_view_contract() {
    const char* prefix = R"({"buffers":[{"byteLength":24}],"bufferViews":[{"buffer":0,
      "byteLength":24,)";
    const char* suffix = R"(}],"accessors":[
      {"bufferView":0,"componentType":5126,"type":"VEC3","count":1,"min":[0,0,0],"max":[0,0,0]},
      {"bufferView":0,"byteOffset":12,"componentType":5126,"type":"VEC3","count":1}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1}}]}]})";

    CHECK(parse(std::string(prefix) + "\"target\":34962,\"byteStride\":12" + suffix).has_value());
    CHECK(!parse(std::string(prefix) + "\"target\":34963,\"byteStride\":12" + suffix).has_value());
    CHECK(!parse(std::string(prefix) + "\"target\":34962" + suffix).has_value());
}

void test_rejects_incomplete_khr() {
    // KHR_gaussian_splatting missing the required colorSpace.
    CHECK(!parse(R"({"meshes":[{"primitives":[{"attributes":{},
      "extensions":{"KHR_gaussian_splatting":{"kernel":"ellipse"}}}]}]})")
               .has_value());
    CHECK(!parse(R"({"meshes":[{"primitives":[{"attributes":{},"extensions":[]}]}]})").has_value());
    CHECK(!parse(R"({"extensionsUsed":["KHR_gaussian_splatting"],
      "meshes":[{"primitives":[{"attributes":{},
      "extensions":{"KHR_gaussian_splatting":1}}]}]})")
               .has_value());
    CHECK(!parse(R"({"meshes":[{"primitives":[{"attributes":{},
      "extensions":{"KHR_gaussian_splatting":{"kernel":"ellipse",
      "colorSpace":"srgb_rec709_display"}}}]}]})")
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
    // A rotated, nonuniform scale matrix decomposes to TRS.
    CHECK(parse(R"({"nodes":[{"matrix":[0,2,0,0, -3,0,0,0, 0,0,4,0, 5,6,7,1]}]})").has_value());
    // A shear matrix does not decompose to TRS.
    CHECK(!parse(R"({"nodes":[{"matrix":[1,0,0,0, 0.5,1,0,0, 0,0,1,0, 0,0,0,1]}]})").has_value());
    // A node.matrix that is the wrong length is rejected.
    CHECK(!parse(R"({"nodes":[{"matrix":[1,0,0]}]})").has_value());
    // A rotation quaternion with the wrong element count is rejected.
    CHECK(!parse(R"({"nodes":[{"rotation":[0,0,1]}]})").has_value());
    // The glTF transform contract does not permit matrix together with TRS fields.
    CHECK(!parse(R"({"nodes":[{"matrix":[1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
      "translation":[1,2,3]}]})")
               .has_value());
    // A projective matrix cannot be represented by the affine scene model.
    CHECK(!parse(R"({"nodes":[{"matrix":[1,0,0,0.5, 0,1,0,0, 0,0,1,0, 0,0,0,1]}]})").has_value());
    CHECK(!parse(R"({"nodes":[{"matrix":[1,0,0,0.0000000000001, 0,1,0,0,
      0,0,1,0, 0,0,0,1]}]})")
               .has_value());
    CHECK(!parse(R"({"nodes":[{"matrix":[1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,2]}]})").has_value());
    // A zero or non-unit quaternion must not turn into a different transform.
    CHECK(!parse(R"({"nodes":[{"rotation":[0,0,0,0]}]})").has_value());
    CHECK(!parse(R"({"nodes":[{"rotation":[0,0,0,2]}]})").has_value());
    CHECK(!parse(R"({"nodes":[{"rotation":[0,0,0,1.00001]}]})").has_value());
}

void test_scene_roots_and_vertex_counts() {
    CHECK(!parse(R"({"nodes":[{"children":[1]},{}],
      "scenes":[{"nodes":[0]},{"nodes":[1]}],"scene":0})")
               .has_value());
    CHECK(!parse(R"({"accessors":[
      {"componentType":5126,"type":"VEC3","count":1,"min":[0,0,0],"max":[0,0,0]},
      {"componentType":5126,"type":"VEC3","count":2}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1}}]}]})")
               .has_value());
    CHECK(!parse(R"({"meshes":[{"primitives":[{"attributes":{}}]}]})").has_value());
}

void test_primitive_without_extension_is_not_gaussian() {
    auto r = parse(R"({"accessors":[{"componentType":5126,"type":"VEC3","count":1,
      "min":[0,0,0],"max":[0,0,0]}],
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
    test_context_controls_json_parse();
    test_context_controls_feature_scan();
    test_context_enforces_structural_limits();
    test_context_owns_document_memory_charge();
    test_records_content_that_the_canonical_scene_drops();
    test_rejects_asset_and_extension_contracts();
    test_requires_declarations_for_extension_occurrences();
    test_rejects_bad_indices_and_types();
    test_rejects_unsupported_accessor_type();
    test_vertex_buffer_view_contract();
    test_rejects_incomplete_khr();
    test_matrix_and_trs_nodes();
    test_scene_roots_and_vertex_counts();
    test_primitive_without_extension_is_not_gaussian();

    if (g_failures == 0) {
        std::printf("gltf document: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf document: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
