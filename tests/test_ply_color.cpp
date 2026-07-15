// PLY colour conversion: the source property TYPE decides the scaling, not folklore.
//
// This suite exists because of a shipped correctness bug (P0-07). The reader collapsed every
// property to a float and then divided every red/green/blue value by 255 unconditionally --
// as though every PLY on earth stored colour as an 8-bit byte. So a point cloud authored with
//
//     property float red      (value 0.5, meaning mid-grey in [0,1])
//
// was decoded as 0.5 / 255 = 0.00196, i.e. essentially black. The scene rendered nearly
// unlit, and nothing in the old suite noticed because nothing tested a float-RGB PLY.
//
// The rule this suite pins: colour normalisation depends on the declared source type.
//   - unsigned 8-bit  -> divide by 255
//   - unsigned 16-bit -> divide by 65535
//   - float / double  -> already in [0,1], divide by nothing
//
// A `float red` of 0.5 must come back as 0.5, full stop.
//
// Self-contained (no external test framework), matching the existing suite's convention.

#include "melkor/math/color.hpp"
#include "melkor/ply_writer.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

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

using melkor::PlyReader;

// The DC coefficient that a linear RGB value canonically maps to. This is the value the reader
// must produce; comparing against it is how we prove no spurious /255 crept in.
float expected_dc(float linear_rgb) {
    return melkor::math::rgb_to_sh_dc(linear_rgb);
}

bool approx(float a, float b, float tol = 1e-5f) {
    return std::fabs(a - b) <= tol;
}

// ---------------------------------------------------------------------------
// The regression: float RGB in [0,1] must NOT be divided by 255.
// ---------------------------------------------------------------------------

void test_ascii_float_rgb_is_not_divided_by_255() {
    // Three grey levels the blueprint names explicitly: 0.25, 0.5, 1.0.
    const std::string ply = "ply\n"
                            "format ascii 1.0\n"
                            "element vertex 3\n"
                            "property float x\n"
                            "property float y\n"
                            "property float z\n"
                            "property float red\n"
                            "property float green\n"
                            "property float blue\n"
                            "end_header\n"
                            "0 0 0 0.25 0.25 0.25\n"
                            "1 0 0 0.5 0.5 0.5\n"
                            "2 0 0 1.0 1.0 1.0\n";

    PlyReader reader;
    auto result = reader.readFromBuffer(reinterpret_cast<const uint8_t*>(ply.data()), ply.size());
    CHECK(result.success);
    CHECK(result.data.has_value());
    if (!result.data.has_value())
        return;
    const auto& splats = *result.data;
    CHECK(splats.size() == 3);
    if (splats.size() != 3)
        return;

    // If the bug were present, these would be rgbToShDc(0.25/255) -- a completely different,
    // near-black value. The assertion is against the honest linear value.
    CHECK(approx(splats.sh().dc(0, 0), expected_dc(0.25f)));
    CHECK(approx(splats.sh().dc(1, 0), expected_dc(0.5f)));
    CHECK(approx(splats.sh().dc(2, 0), expected_dc(1.0f)));

    // And prove it is NOT the buggy value, so a future regression that reintroduces /255
    // cannot pass this test by coincidence.
    CHECK(!approx(splats.sh().dc(1, 0), expected_dc(0.5f / 255.0f)));
}

void test_binary_float_rgb_is_not_divided_by_255() {
    // Same content, binary little-endian, so both decode paths are covered.
    std::vector<std::uint8_t> ply;
    auto append = [&](const char* s) { ply.insert(ply.end(), s, s + std::strlen(s)); };
    auto append_float = [&](float value) {
        std::uint8_t bytes[4];
        std::memcpy(bytes, &value, 4);  // host is little-endian on every supported platform
        ply.insert(ply.end(), bytes, bytes + 4);
    };

    append("ply\n"
           "format binary_little_endian 1.0\n"
           "element vertex 2\n"
           "property float x\n"
           "property float y\n"
           "property float z\n"
           "property float red\n"
           "property float green\n"
           "property float blue\n"
           "end_header\n");
    // Vertex 0: grey 0.5
    append_float(0.0f);
    append_float(0.0f);
    append_float(0.0f);
    append_float(0.5f);
    append_float(0.5f);
    append_float(0.5f);
    // Vertex 1: white 1.0
    append_float(1.0f);
    append_float(0.0f);
    append_float(0.0f);
    append_float(1.0f);
    append_float(1.0f);
    append_float(1.0f);

    PlyReader reader;
    auto result = reader.readFromBuffer(ply.data(), ply.size());
    CHECK(result.success);
    CHECK(result.data.has_value());
    if (!result.data.has_value())
        return;
    const auto& splats = *result.data;
    CHECK(splats.size() == 2);
    if (splats.size() != 2)
        return;
    CHECK(approx(splats.sh().dc(0, 0), expected_dc(0.5f)));
    CHECK(approx(splats.sh().dc(1, 0), expected_dc(1.0f)));
    CHECK(!approx(splats.sh().dc(0, 0), expected_dc(0.5f / 255.0f)));
}

// ---------------------------------------------------------------------------
// The other side of the same rule: uchar RGB genuinely IS 0..255 and must be scaled.
// ---------------------------------------------------------------------------

void test_uchar_rgb_is_divided_by_255() {
    std::vector<std::uint8_t> ply;
    auto append = [&](const char* s) { ply.insert(ply.end(), s, s + std::strlen(s)); };
    auto append_float = [&](float value) {
        std::uint8_t bytes[4];
        std::memcpy(bytes, &value, 4);
        ply.insert(ply.end(), bytes, bytes + 4);
    };

    append("ply\n"
           "format binary_little_endian 1.0\n"
           "element vertex 2\n"
           "property float x\n"
           "property float y\n"
           "property float z\n"
           "property uchar red\n"
           "property uchar green\n"
           "property uchar blue\n"
           "end_header\n");
    // Vertex 0: byte 128 -> ~0.502 linear
    append_float(0.0f);
    append_float(0.0f);
    append_float(0.0f);
    ply.push_back(128);
    ply.push_back(128);
    ply.push_back(128);
    // Vertex 1: byte 255 -> 1.0 linear
    append_float(1.0f);
    append_float(0.0f);
    append_float(0.0f);
    ply.push_back(255);
    ply.push_back(255);
    ply.push_back(255);

    PlyReader reader;
    auto result = reader.readFromBuffer(ply.data(), ply.size());
    CHECK(result.success);
    CHECK(result.data.has_value());
    if (!result.data.has_value())
        return;
    const auto& splats = *result.data;
    CHECK(splats.size() == 2);
    if (splats.size() != 2)
        return;
    // 128/255 = 0.50196..., which must round-trip through the DC mapping.
    CHECK(approx(splats.sh().dc(0, 0), expected_dc(128.0f / 255.0f)));
    CHECK(approx(splats.sh().dc(1, 0), expected_dc(255.0f / 255.0f)));
}

// ---------------------------------------------------------------------------
// 16-bit unsigned colour scales by 65535, not 255.
// ---------------------------------------------------------------------------

void test_ushort_rgb_is_divided_by_65535() {
    std::vector<std::uint8_t> ply;
    auto append = [&](const char* s) { ply.insert(ply.end(), s, s + std::strlen(s)); };
    auto append_float = [&](float value) {
        std::uint8_t bytes[4];
        std::memcpy(bytes, &value, 4);
        ply.insert(ply.end(), bytes, bytes + 4);
    };
    auto append_u16 = [&](std::uint16_t value) {
        std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xff),
                                 static_cast<std::uint8_t>(value >> 8)};
        ply.insert(ply.end(), bytes, bytes + 2);
    };

    append("ply\n"
           "format binary_little_endian 1.0\n"
           "element vertex 1\n"
           "property float x\n"
           "property float y\n"
           "property float z\n"
           "property ushort red\n"
           "property ushort green\n"
           "property ushort blue\n"
           "end_header\n");
    append_float(0.0f);
    append_float(0.0f);
    append_float(0.0f);
    append_u16(32768);
    append_u16(32768);
    append_u16(32768);  // ~0.5 in 16-bit

    PlyReader reader;
    auto result = reader.readFromBuffer(ply.data(), ply.size());
    CHECK(result.success);
    CHECK(result.data.has_value());
    if (!result.data.has_value())
        return;
    const auto& splats = *result.data;
    CHECK(splats.size() == 1);
    if (splats.size() != 1)
        return;
    // If this were scaled by 255 instead of 65535, the value would be ~128x too large and
    // wildly out of range.
    CHECK(approx(splats.sh().dc(0, 0), expected_dc(32768.0f / 65535.0f)));
}

// ---------------------------------------------------------------------------
// 3DGS f_dc_* is NOT colour-scaled at all: it is already an SH coefficient.
// ---------------------------------------------------------------------------

void test_fdc_is_not_colour_scaled() {
    const std::string ply = "ply\n"
                            "format ascii 1.0\n"
                            "element vertex 1\n"
                            "property float x\n"
                            "property float y\n"
                            "property float z\n"
                            "property float f_dc_0\n"
                            "property float f_dc_1\n"
                            "property float f_dc_2\n"
                            "end_header\n"
                            "0 0 0 1.5 -0.5 0.25\n";

    PlyReader reader;
    auto result = reader.readFromBuffer(reinterpret_cast<const uint8_t*>(ply.data()), ply.size());
    CHECK(result.success);
    CHECK(result.data.has_value());
    if (!result.data.has_value())
        return;
    CHECK(result.data->size() == 1);
    if (result.data->size() != 1)
        return;

    // f_dc_* values are SH coefficients already. They pass through unchanged -- no division,
    // no rgbToShDc, and they may legitimately be negative or exceed 1.
    CHECK(approx(result.data->sh().dc(0, 0), 1.5f));
    CHECK(approx(result.data->sh().dc(0, 1), -0.5f));
    CHECK(approx(result.data->sh().dc(0, 2), 0.25f));
}

}  // namespace

int main() {
    test_ascii_float_rgb_is_not_divided_by_255();
    test_binary_float_rgb_is_not_divided_by_255();
    test_uchar_rgb_is_divided_by_255();
    test_ushort_rgb_is_divided_by_65535();
    test_fdc_is_not_colour_scaled();

    if (g_failures == 0) {
        std::printf("ply colour: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "ply colour: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
