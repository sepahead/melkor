// Tests for glTF accessor resolution.
//
// End-to-end over the real path: parse a small glTF into a Document, supply a byte buffer, and
// resolve+decode an accessor -- then the adversarial cases where the bufferView runs past the
// buffer or the accessor runs past its bufferView, which the index-level parser does not catch and
// the resolver must. Run under ASan to confirm no over-read on the failing cases.
//
// Self-contained (no external test framework).

#include "melkor/format/gltf_resolve.hpp"

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

bool approx(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) <= eps; }

void put_f32(std::vector<std::uint8_t>& v, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    v.push_back(static_cast<std::uint8_t>(bits & 0xFF));
    v.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFF));
}

gltf::Document parse(const std::string& s) {
    auto r = gltf::parse_gltf_json(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    if (!r.has_value()) {
        std::fprintf(stderr, "unexpected parse failure in test setup\n");
        return gltf::Document{};
    }
    return r.value();
}

std::vector<gltf::BufferSpan> one_buffer(const std::vector<std::uint8_t>& b) {
    return {gltf::BufferSpan{b.data(), b.size()}};
}

void test_resolves_vec3() {
    auto doc = parse(R"({
      "buffers":[{"byteLength":24}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":24}],
      "accessors":[{"bufferView":0,"componentType":5126,"type":"VEC3","count":2}]
    })");
    std::vector<std::uint8_t> buf;
    for (float f : {1.f, 2.f, 3.f, 4.f, 5.f, 6.f}) put_f32(buf, f);
    auto r = gltf::resolve_and_decode_accessor(doc, 0, one_buffer(buf));
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(r.value().size() == 6);
        CHECK(approx(r.value()[0], 1.f) && approx(r.value()[3], 4.f) && approx(r.value()[5], 6.f));
    }
}

void test_composes_offsets() {
    // bufferView starts at byte 4; accessor starts a further 4 bytes in => absolute offset 8.
    auto doc = parse(R"({
      "buffers":[{"byteLength":12}],
      "bufferViews":[{"buffer":0,"byteOffset":4,"byteLength":8}],
      "accessors":[{"bufferView":0,"byteOffset":4,"componentType":5126,"type":"SCALAR","count":1}]
    })");
    std::vector<std::uint8_t> buf(8, 0);  // 8 bytes of lead-in (offset 0..7)
    put_f32(buf, 42.5f);                  // the value lands at byte 8 (bufferView offset 4 + acc 4)
    auto r = gltf::resolve_and_decode_accessor(doc, 0, one_buffer(buf));
    CHECK(r.has_value());
    if (r.has_value()) CHECK(r.value().size() == 1 && approx(r.value()[0], 42.5f));
}

void test_rejects_bufferview_past_buffer() {
    auto doc = parse(R"({
      "buffers":[{"byteLength":999}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":100}],
      "accessors":[{"bufferView":0,"componentType":5126,"type":"VEC3","count":2}]
    })");
    std::vector<std::uint8_t> buf(24, 0);  // only 24 bytes actually present
    CHECK(!gltf::resolve_and_decode_accessor(doc, 0, one_buffer(buf)).has_value());
}

void test_rejects_accessor_past_bufferview() {
    // bufferView is 12 bytes; a VEC3 float count 2 needs 24.
    auto doc = parse(R"({
      "buffers":[{"byteLength":12}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":12}],
      "accessors":[{"bufferView":0,"componentType":5126,"type":"VEC3","count":2}]
    })");
    std::vector<std::uint8_t> buf(12, 0);
    CHECK(!gltf::resolve_and_decode_accessor(doc, 0, one_buffer(buf)).has_value());
}

void test_rejects_stride_smaller_than_element() {
    auto doc = parse(R"({
      "buffers":[{"byteLength":24}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":24,"byteStride":4}],
      "accessors":[{"bufferView":0,"componentType":5126,"type":"VEC3","count":1}]
    })");
    std::vector<std::uint8_t> buf(24, 0);
    CHECK(!gltf::resolve_and_decode_accessor(doc, 0, one_buffer(buf)).has_value());
}

void test_rejects_unavailable_buffer() {
    auto doc = parse(R"({
      "buffers":[{"byteLength":24}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":24}],
      "accessors":[{"bufferView":0,"componentType":5126,"type":"VEC3","count":2}]
    })");
    // No buffers supplied at all.
    CHECK(!gltf::resolve_and_decode_accessor(doc, 0, {}).has_value());
    // A null buffer span.
    std::vector<gltf::BufferSpan> null_span = {gltf::BufferSpan{nullptr, 0}};
    CHECK(!gltf::resolve_and_decode_accessor(doc, 0, null_span).has_value());
}

void test_rejects_bad_index() {
    auto doc = parse(R"({"buffers":[{"byteLength":4}],
      "bufferViews":[{"buffer":0,"byteLength":4}],
      "accessors":[{"bufferView":0,"componentType":5126,"type":"SCALAR","count":1}]})");
    std::vector<std::uint8_t> buf(4, 0);
    CHECK(!gltf::resolve_and_decode_accessor(doc, 5, one_buffer(buf)).has_value());
}

void test_strided_interleave() {
    // Two VEC3 accessors interleaved in one bufferView with stride 24: POSITION at offset 0, a
    // second attribute at offset 12.
    auto doc = parse(R"({
      "buffers":[{"byteLength":48}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48,"byteStride":24}],
      "accessors":[
        {"bufferView":0,"byteOffset":0,"componentType":5126,"type":"VEC3","count":2},
        {"bufferView":0,"byteOffset":12,"componentType":5126,"type":"VEC3","count":2}
      ]
    })");
    std::vector<std::uint8_t> buf;
    // element 0: pos(1,2,3) other(7,8,9); element 1: pos(4,5,6) other(10,11,12)
    for (float f : {1.f, 2.f, 3.f, 7.f, 8.f, 9.f, 4.f, 5.f, 6.f, 10.f, 11.f, 12.f}) put_f32(buf, f);
    auto pos = gltf::resolve_and_decode_accessor(doc, 0, one_buffer(buf));
    auto other = gltf::resolve_and_decode_accessor(doc, 1, one_buffer(buf));
    CHECK(pos.has_value() && other.has_value());
    if (pos.has_value()) CHECK(approx(pos.value()[0], 1.f) && approx(pos.value()[3], 4.f));
    if (other.has_value()) CHECK(approx(other.value()[0], 7.f) && approx(other.value()[3], 10.f));
}

}  // namespace

int main() {
    test_resolves_vec3();
    test_composes_offsets();
    test_rejects_bufferview_past_buffer();
    test_rejects_accessor_past_bufferview();
    test_rejects_stride_smaller_than_element();
    test_rejects_unavailable_buffer();
    test_rejects_bad_index();
    test_strided_interleave();

    if (g_failures == 0) {
        std::printf("gltf resolve: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf resolve: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
