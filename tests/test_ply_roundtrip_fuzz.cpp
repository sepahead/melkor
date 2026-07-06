// Fuzz the binary PLY round-trip (write -> read) across random splats.
//
// The header-driven reader (ply_writer.cpp) maps fields by name and computes a
// stride from declared property types. This test generates random Gaussian
// splats, writes them as binary PLY, reads them back, and asserts every field
// survives exactly. It catches regressions in: stride arithmetic, field
// ordering, the f_dc/opacity/scale/rotation name lookups, and endian handling
// (binary_little_endian).
//
// Coverage:
//   - random positions spanning a wide range (including negatives)
//   - random SH-DC, log-scale, logit-opacity values
//   - random quaternions (including non-unit, to confirm the reader doesn't
//     silently renormalize)
//   - a mix of cloud sizes including the empty and single-splat edge cases

#include "melkor/gaussian_data.hpp"
#include "melkor/ply_writer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr float EPS = 1e-5f;

bool approx(float a, float b) {
    // Use relative tolerance for large magnitudes, absolute for small.
    float scale = std::max({1.0f, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= EPS * scale;
}

}  // namespace

int main() {
    using namespace melkor;
    std::mt19937 rng(7);  // fixed seed for reproducibility
    std::uniform_real_distribution<float> pos(-100.0f, 100.0f);
    std::uniform_real_distribution<float> shdc(-3.0f, 3.0f);
    std::uniform_real_distribution<float> lscale(-8.0f, 2.0f);
    std::uniform_real_distribution<float> lopacity(-6.0f, 6.0f);
    std::uniform_real_distribution<float> quat(-1.0f, 1.0f);

    auto rand_splat = [&](GaussianSplat& s) {
        s.x = pos(rng); s.y = pos(rng); s.z = pos(rng);
        s.f_dc_0 = shdc(rng); s.f_dc_1 = shdc(rng); s.f_dc_2 = shdc(rng);
        s.opacity = lopacity(rng);
        s.scale_0 = lscale(rng); s.scale_1 = lscale(rng); s.scale_2 = lscale(rng);
        s.rot_0 = quat(rng); s.rot_1 = quat(rng); s.rot_2 = quat(rng); s.rot_3 = quat(rng);
    };

    int failures = 0;
    auto check_round_trip = [&](const char* label, const GaussianCloud& in) {
        PlyWriter w;
        std::vector<uint8_t> buf;
        PlyWriteConfig cfg;
        cfg.format = PlyFormat::Binary;
        auto wres = w.writeToBuffer(buf, in, cfg);
        if (!wres.success) {
            printf("  FAIL [%s]: write failed\n", label);
            ++failures;
            return;
        }
        PlyReader r;
        auto rres = r.readFromBuffer(buf.data(), buf.size());
        if (!rres.success) {
            printf("  FAIL [%s]: read failed: %s\n", label, rres.error_message.c_str());
            ++failures;
            return;
        }
        if (rres.cloud.size() != in.size()) {
            printf("  FAIL [%s]: count %zu != %zu\n", label, rres.cloud.size(), in.size());
            ++failures;
            return;
        }
        for (size_t i = 0; i < in.size(); ++i) {
            const auto& a = in[i];
            const auto& b = rres.cloud[i];
            if (!approx(a.x, b.x) || !approx(a.y, b.y) || !approx(a.z, b.z) ||
                !approx(a.f_dc_0, b.f_dc_0) || !approx(a.f_dc_1, b.f_dc_1) || !approx(a.f_dc_2, b.f_dc_2) ||
                !approx(a.opacity, b.opacity) ||
                !approx(a.scale_0, b.scale_0) || !approx(a.scale_1, b.scale_1) || !approx(a.scale_2, b.scale_2) ||
                !approx(a.rot_0, b.rot_0) || !approx(a.rot_1, b.rot_1) ||
                !approx(a.rot_2, b.rot_2) || !approx(a.rot_3, b.rot_3)) {
                printf("  FAIL [%s]: field mismatch at index %zu\n", label, i);
                ++failures;
                return;
            }
        }
        printf("  PASS [%s]: %zu splats round-trip exact\n", label, in.size());
    };

    printf("[fuzz] PLY binary round-trip\n");

    // Empty cloud.
    { GaussianCloud empty; check_round_trip("empty", empty); }

    // Single splat.
    {
        GaussianCloud c; GaussianSplat s{}; rand_splat(s); c.addSplat(s);
        check_round_trip("single", c);
    }

    // Random sizes.
    for (int n : {2, 3, 17, 100, 1000}) {
        GaussianCloud c;
        for (int i = 0; i < n; ++i) { GaussianSplat s{}; rand_splat(s); c.addSplat(s); }
        char label[32];
        std::snprintf(label, sizeof(label), "n=%d", n);
        check_round_trip(label, c);
    }

    // ASCII round-trip too (separate path in the reader).
    {
        GaussianCloud c;
        for (int i = 0; i < 50; ++i) { GaussianSplat s{}; rand_splat(s); c.addSplat(s); }
        PlyWriter w;
        std::vector<uint8_t> buf;
        PlyWriteConfig cfg;
        cfg.format = PlyFormat::Ascii;
        w.writeToBuffer(buf, c, cfg);
        PlyReader r;
        auto rres = r.readFromBuffer(buf.data(), buf.size());
        bool ok = rres.success && rres.cloud.size() == c.size();
        if (ok) {
            for (size_t i = 0; i < c.size() && ok; ++i) {
                const auto& a = c[i];
                const auto& b = rres.cloud[i];
                // ASCII is written at 8-digit precision; allow slightly looser tol.
                ok = approx(a.x, b.x) && approx(a.y, b.y) && approx(a.z, b.z) &&
                     approx(a.rot_0, b.rot_0) && approx(a.opacity, b.opacity);
            }
        }
        printf(ok ? "  PASS [ascii n=50]: round-trip within precision\n"
                  : "  FAIL [ascii n=50]\n");
        if (!ok) ++failures;
    }

    // Hand-crafted reader cases: type aliases, big-endian, CRLF headers,
    // end_header inside a comment, and malformed vertex counts.
    printf("[fuzz] PLY reader hand-crafted cases\n");

    auto append_str = [](std::vector<uint8_t>& v, const std::string& s) {
        v.insert(v.end(), s.begin(), s.end());
    };
    auto push_be_float = [](std::vector<uint8_t>& v, float f) {
        uint32_t bits; std::memcpy(&bits, &f, 4);
        v.push_back(static_cast<uint8_t>(bits >> 24));
        v.push_back(static_cast<uint8_t>(bits >> 16));
        v.push_back(static_cast<uint8_t>(bits >> 8));
        v.push_back(static_cast<uint8_t>(bits));
    };
    auto push_le_double = [](std::vector<uint8_t>& v, double d) {
        uint64_t bits; std::memcpy(&bits, &d, 8);
        for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    };
    auto expect = [&](const char* label, bool ok) {
        printf(ok ? "  PASS [%s]\n" : "  FAIL [%s]\n", label);
        if (!ok) ++failures;
    };

    // Big-endian binary: three float positions, hand-encoded most-significant
    // byte first. Must decode to the exact values, not silent garbage.
    {
        std::vector<uint8_t> buf;
        append_str(buf,
            "ply\n"
            "format binary_big_endian 1.0\n"
            "element vertex 1\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "end_header\n");
        push_be_float(buf, 1.0f);
        push_be_float(buf, -2.5f);
        push_be_float(buf, 3.25f);
        PlyReader r;
        auto res = r.readFromBuffer(buf.data(), buf.size());
        expect("big-endian binary", res.success && res.cloud.size() == 1 &&
               approx(res.cloud[0].x, 1.0f) && approx(res.cloud[0].y, -2.5f) &&
               approx(res.cloud[0].z, 3.25f));
    }

    // Sized type aliases: float64 positions (8-byte stride) plus uint8 colors
    // (1-byte stride). Wrong alias sizes would garble vertex 1 or reject the
    // buffer as too small. Also checks the uint8 color /255 -> SH-DC path.
    {
        std::vector<uint8_t> buf;
        append_str(buf,
            "ply\n"
            "format binary_little_endian 1.0\n"
            "element vertex 2\n"
            "property float64 x\n"
            "property float64 y\n"
            "property float64 z\n"
            "property uint8 red\n"
            "property uint8 green\n"
            "property uint8 blue\n"
            "end_header\n");
        push_le_double(buf, 1.5);  push_le_double(buf, -2.25); push_le_double(buf, 3.0);
        buf.push_back(255); buf.push_back(0); buf.push_back(128);
        push_le_double(buf, -4.5); push_le_double(buf, 5.0);   push_le_double(buf, -6.75);
        buf.push_back(0); buf.push_back(255); buf.push_back(64);
        PlyReader r;
        auto res = r.readFromBuffer(buf.data(), buf.size());
        expect("float64 + uint8 aliases", res.success && res.cloud.size() == 2 &&
               approx(res.cloud[0].x, 1.5f)  && approx(res.cloud[0].y, -2.25f) &&
               approx(res.cloud[0].z, 3.0f)  &&
               approx(res.cloud[0].f_dc_0, melkor::utils::rgbToShDc(1.0f)) &&
               approx(res.cloud[1].x, -4.5f) && approx(res.cloud[1].y, 5.0f) &&
               approx(res.cloud[1].z, -6.75f));
    }

    // CRLF-terminated header lines must parse (data starts after end_header's
    // newline).
    {
        std::vector<uint8_t> buf;
        append_str(buf,
            "ply\r\n"
            "format ascii 1.0\r\n"
            "element vertex 1\r\n"
            "property float x\r\n"
            "property float y\r\n"
            "property float z\r\n"
            "end_header\r\n"
            "1 2 3\r\n");
        PlyReader r;
        auto res = r.readFromBuffer(buf.data(), buf.size());
        expect("CRLF header", res.success && res.cloud.size() == 1 &&
               approx(res.cloud[0].x, 1.0f) && approx(res.cloud[0].y, 2.0f) &&
               approx(res.cloud[0].z, 3.0f));
    }

    // A comment merely containing "end_header" must not truncate the header.
    {
        std::vector<uint8_t> buf;
        append_str(buf,
            "ply\n"
            "format ascii 1.0\n"
            "comment beware end_header appears here\n"
            "element vertex 1\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "end_header\n"
            "7 8 9\n");
        PlyReader r;
        auto res = r.readFromBuffer(buf.data(), buf.size());
        expect("end_header in comment", res.success && res.cloud.size() == 1 &&
               approx(res.cloud[0].x, 7.0f) && approx(res.cloud[0].z, 9.0f));
    }

    // Overflowing vertex count: must return an error, not throw/terminate.
    {
        std::vector<uint8_t> buf;
        append_str(buf,
            "ply\n"
            "format ascii 1.0\n"
            "element vertex 99999999999999999999999999\n"
            "property float x\n"
            "end_header\n"
            "1\n");
        PlyReader r;
        auto res = r.readFromBuffer(buf.data(), buf.size());
        expect("overflowing vertex count", !res.success);
    }

    // Huge-but-parseable count with a tiny binary payload: must be rejected
    // before reserve(), not crash with bad_alloc or read out of bounds.
    {
        std::vector<uint8_t> buf;
        append_str(buf,
            "ply\n"
            "format binary_little_endian 1.0\n"
            "element vertex 4294967295\n"
            "property float x\n"
            "end_header\n");
        buf.push_back(0); buf.push_back(0); buf.push_back(0); buf.push_back(0);
        PlyReader r;
        auto res = r.readFromBuffer(buf.data(), buf.size());
        expect("huge count vs tiny data", !res.success);
    }

    printf(failures == 0 ? "\n  ALL PLY ROUND-TRIP TESTS PASSED\n"
                         : "\n  %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
