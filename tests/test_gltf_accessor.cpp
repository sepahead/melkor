// Tests for glTF accessor decoding.
//
// These pin the two things that go wrong quietly: the normalized-integer decode rules (unsigned
// c/max vs signed max(c/max, -1)) and the bounds arithmetic on count/stride/offset. The negative
// cases assert a clean failure with no out-of-bounds read (run under ASan to enforce that).
//
// Self-contained (no external test framework).

#include "melkor/format/gltf_accessor.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

void put_i16(std::vector<std::uint8_t>& v, std::int16_t x) {
    const std::uint16_t u = static_cast<std::uint16_t>(x);
    v.push_back(static_cast<std::uint8_t>(u & 0xFF));
    v.push_back(static_cast<std::uint8_t>((u >> 8) & 0xFF));
}

void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
}

void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}

void test_sizes() {
    CHECK(gltf::component_size(gltf::ComponentType::i8) == 1);
    CHECK(gltf::component_size(gltf::ComponentType::u16) == 2);
    CHECK(gltf::component_size(gltf::ComponentType::f32) == 4);
    CHECK(gltf::component_count(gltf::ElementType::scalar) == 1);
    CHECK(gltf::component_count(gltf::ElementType::vec3) == 3);
    CHECK(gltf::component_count(gltf::ElementType::vec4) == 4);
    CHECK(gltf::component_type_from_int(5126) == gltf::ComponentType::f32);
    CHECK(gltf::component_type_from_int(5121) == gltf::ComponentType::u8);
    CHECK(!gltf::component_type_from_int(1234).has_value());
}

void test_float_vec3() {
    std::vector<std::uint8_t> buf;
    put_f32(buf, 1.0f);
    put_f32(buf, -2.5f);
    put_f32(buf, 3.25f);
    put_f32(buf, 4.0f);
    put_f32(buf, 5.0f);
    put_f32(buf, 6.0f);
    gltf::AccessorView v;
    v.component = gltf::ComponentType::f32;
    v.element = gltf::ElementType::vec3;
    v.count = 2;
    auto r = gltf::decode_accessor(v, buf.data(), buf.size());
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(r.value().size() == 6);
        CHECK(approx(r.value()[0], 1.0f) && approx(r.value()[1], -2.5f) && approx(r.value()[2], 3.25f));
        CHECK(approx(r.value()[3], 4.0f) && approx(r.value()[5], 6.0f));
    }
}

void test_normalized_unsigned_byte() {
    // Opacity as normalized u8: 0->0, 255->1, 128->0.50196...
    std::vector<std::uint8_t> buf = {0, 255, 128};
    gltf::AccessorView v;
    v.component = gltf::ComponentType::u8;
    v.element = gltf::ElementType::scalar;
    v.normalized = true;
    v.count = 3;
    auto r = gltf::decode_accessor(v, buf.data(), buf.size());
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(approx(r.value()[0], 0.0f));
        CHECK(approx(r.value()[1], 1.0f));
        CHECK(approx(r.value()[2], 128.0f / 255.0f));
    }
    // Same bytes, NOT normalized: exact integer values.
    v.normalized = false;
    auto r2 = gltf::decode_accessor(v, buf.data(), buf.size());
    CHECK(r2.has_value());
    if (r2.has_value()) {
        CHECK(approx(r2.value()[1], 255.0f));
        CHECK(approx(r2.value()[2], 128.0f));
    }
}

void test_normalized_signed_byte_clamps() {
    // Signed normalized: -128 and -127 both map to -1 (the max(c/127,-1) clamp), 127 -> 1.
    std::vector<std::uint8_t> buf = {static_cast<std::uint8_t>(-128), static_cast<std::uint8_t>(-127),
                                     127, 0};
    gltf::AccessorView v;
    v.component = gltf::ComponentType::i8;
    v.element = gltf::ElementType::scalar;
    v.normalized = true;
    v.count = 4;
    auto r = gltf::decode_accessor(v, buf.data(), buf.size());
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(approx(r.value()[0], -1.0f));  // -128/127 = -1.0078 clamped to -1
        CHECK(approx(r.value()[1], -1.0f));  // -127/127 = -1
        CHECK(approx(r.value()[2], 1.0f));   // 127/127 = 1
        CHECK(approx(r.value()[3], 0.0f));
    }
}

void test_normalized_unsigned_short() {
    std::vector<std::uint8_t> buf;
    put_u16(buf, 0);
    put_u16(buf, 65535);
    put_u16(buf, 32768);
    gltf::AccessorView v;
    v.component = gltf::ComponentType::u16;
    v.element = gltf::ElementType::scalar;
    v.normalized = true;
    v.count = 3;
    auto r = gltf::decode_accessor(v, buf.data(), buf.size());
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(approx(r.value()[0], 0.0f));
        CHECK(approx(r.value()[1], 1.0f));
        CHECK(approx(r.value()[2], 32768.0f / 65535.0f));
    }
}

void test_interleaved_stride() {
    // Two vec3 floats interleaved with 4 bytes of padding after each element (stride 16).
    std::vector<std::uint8_t> buf;
    put_f32(buf, 1.0f);
    put_f32(buf, 2.0f);
    put_f32(buf, 3.0f);
    put_f32(buf, 0.0f);  // padding
    put_f32(buf, 4.0f);
    put_f32(buf, 5.0f);
    put_f32(buf, 6.0f);
    put_f32(buf, 0.0f);  // padding
    gltf::AccessorView v;
    v.component = gltf::ComponentType::f32;
    v.element = gltf::ElementType::vec3;
    v.count = 2;
    v.byte_stride = 16;
    auto r = gltf::decode_accessor(v, buf.data(), buf.size());
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(approx(r.value()[0], 1.0f) && approx(r.value()[2], 3.0f));
        CHECK(approx(r.value()[3], 4.0f) && approx(r.value()[5], 6.0f));
    }
}

void test_byte_offset() {
    std::vector<std::uint8_t> buf = {0xAA, 0xBB};  // 2 bytes of unrelated leading data
    put_f32(buf, 7.0f);
    gltf::AccessorView v;
    v.component = gltf::ComponentType::f32;
    v.element = gltf::ElementType::scalar;
    v.count = 1;
    v.byte_offset = 2;
    auto r = gltf::decode_accessor(v, buf.data(), buf.size());
    CHECK(r.has_value());
    if (r.has_value()) CHECK(approx(r.value()[0], 7.0f));
}

void test_bounds_failures() {
    std::vector<std::uint8_t> buf(8, 0);  // room for two floats

    // Count larger than the buffer holds.
    gltf::AccessorView v;
    v.component = gltf::ComponentType::f32;
    v.element = gltf::ElementType::vec3;  // 12 bytes per element
    v.count = 1;                          // needs 12 bytes, only 8 present
    CHECK(!gltf::decode_accessor(v, buf.data(), buf.size()).has_value());

    // Offset past the end.
    gltf::AccessorView v2;
    v2.component = gltf::ComponentType::f32;
    v2.element = gltf::ElementType::scalar;
    v2.count = 1;
    v2.byte_offset = 100;
    CHECK(!gltf::decode_accessor(v2, buf.data(), buf.size()).has_value());

    // A colossal count that would overflow if the arithmetic were naive.
    gltf::AccessorView v3;
    v3.component = gltf::ComponentType::f32;
    v3.element = gltf::ElementType::vec4;
    v3.count = static_cast<std::size_t>(-1) / 4;
    CHECK(!gltf::decode_accessor(v3, buf.data(), buf.size()).has_value());

    // Stride smaller than the element is rejected (would make elements overlap).
    gltf::AccessorView v4;
    v4.component = gltf::ComponentType::f32;
    v4.element = gltf::ElementType::vec3;  // element 12 bytes
    v4.count = 1;
    v4.byte_stride = 4;
    CHECK(!gltf::decode_accessor(v4, buf.data(), buf.size()).has_value());
}

void test_normalized_signed_short() {
    // KHR permits ROTATION as normalized signed short. -32768 and -32767 both clamp to -1 (the
    // max(c/32767, -1) rule); 32767 -> 1; 0 -> 0.
    std::vector<std::uint8_t> buf;
    put_i16(buf, -32768);
    put_i16(buf, -32767);
    put_i16(buf, 32767);
    put_i16(buf, 0);
    gltf::AccessorView v;
    v.component = gltf::ComponentType::i16;
    v.element = gltf::ElementType::scalar;
    v.normalized = true;
    v.count = 4;
    auto r = gltf::decode_accessor(v, buf.data(), buf.size());
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(approx(r.value()[0], -1.0f) && approx(r.value()[1], -1.0f));
        CHECK(approx(r.value()[2], 1.0f) && approx(r.value()[3], 0.0f));
    }
}

void test_unsigned_int_decode() {
    // u32 (5125): normalized maps 0/2^32-1 to 0/1; unnormalized returns the exact value.
    std::vector<std::uint8_t> buf;
    put_u32(buf, 0);
    put_u32(buf, 0xFFFFFFFFu);
    gltf::AccessorView v;
    v.component = gltf::ComponentType::u32;
    v.element = gltf::ElementType::scalar;
    v.normalized = true;
    v.count = 2;
    auto r = gltf::decode_accessor(v, buf.data(), buf.size());
    CHECK(r.has_value());
    if (r.has_value()) CHECK(approx(r.value()[0], 0.0f) && approx(r.value()[1], 1.0f));

    std::vector<std::uint8_t> buf2;
    put_u32(buf2, 1000);
    gltf::AccessorView v2;
    v2.component = gltf::ComponentType::u32;
    v2.element = gltf::ElementType::scalar;
    v2.normalized = false;
    v2.count = 1;
    auto r2 = gltf::decode_accessor(v2, buf2.data(), buf2.size());
    CHECK(r2.has_value());
    if (r2.has_value()) CHECK(approx(r2.value()[0], 1000.0f));
}

void test_zero_count_is_empty() {
    std::vector<std::uint8_t> buf(4, 0);
    gltf::AccessorView v;
    v.component = gltf::ComponentType::f32;
    v.element = gltf::ElementType::vec3;
    v.count = 0;
    auto r = gltf::decode_accessor(v, buf.data(), buf.size());
    CHECK(r.has_value());
    if (r.has_value()) CHECK(r.value().empty());
}

}  // namespace

int main() {
    test_sizes();
    test_float_vec3();
    test_normalized_unsigned_byte();
    test_normalized_signed_byte_clamps();
    test_normalized_unsigned_short();
    test_interleaved_stride();
    test_byte_offset();
    test_bounds_failures();
    test_normalized_signed_short();
    test_unsigned_int_decode();
    test_zero_count_is_empty();

    if (g_failures == 0) {
        std::printf("gltf accessor: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf accessor: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
