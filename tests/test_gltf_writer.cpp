// Tests for the glTF KHR_gaussian_splatting writer, primarily by round-trip.
//
// The strongest correctness check for a codec pair is that write then read returns what went in.
// A degree-1 SplatData is written to a GLB and read back, and every field -- positions, scales,
// rotations, opacities, and the transposed spherical harmonics -- must match to float precision.
// A degree-4 source additionally pins the degree-3 truncation loss.
//
// Self-contained (no external test framework).

#include "melkor/format/gltf_writer.hpp"

#include "melkor/format/gltf_reader.hpp"
#include "melkor/scene.hpp"

#include <cmath>
#include <cstdio>
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

bool has_loss(const LossReport& r, const std::string& code) {
    for (const auto& i : r.items())
        if (i.code == code) return true;
    return false;
}

// Builds a valid degree-`degree` SplatData with `n` splats and distinct, in-domain values.
SplatData make_splats(std::size_t n, std::uint32_t degree) {
    SplatBufferInput in;
    const std::size_t coeffs = (degree + 1) * (degree + 1);
    std::vector<float> sh;
    for (std::size_t s = 0; s < n; ++s) {
        in.positions.push_back(Vec3f{1.0f + s, 2.0f + s, 3.0f + s});
        in.scales.push_back(Vec3f{0.10f + 0.01f * s, 0.20f + 0.01f * s, 0.30f + 0.01f * s});
        // A distinct unit quaternion per splat: rotate about Z by a small angle.
        const float a = 0.2f * static_cast<float>(s);
        in.rotations.push_back(Quatf{0.0f, 0.0f, std::sin(a * 0.5f), std::cos(a * 0.5f)});
        in.opacities.push_back(0.3f + 0.05f * s);
        for (std::size_t k = 0; k < coeffs; ++k) {
            for (int c = 0; c < 3; ++c) {
                sh.push_back(static_cast<float>(s) * 0.5f + static_cast<float>(k) +
                             static_cast<float>(c) * 0.1f);
            }
        }
    }
    in.sh = ShBuffer::create(degree, n, std::move(sh)).value();
    return SplatData::create(std::move(in)).value();
}

void test_roundtrip_degree1() {
    const std::size_t n = 4;
    auto original = make_splats(n, 1);

    auto written = gltf::write_glb(original, khr::ColorSpace::lin_rec709_display);
    CHECK(written.has_value());
    if (!written.has_value()) return;
    CHECK(written.value().losses.empty());  // degree 1 fits the profile: no loss

    auto read = gltf::read_glb(written.value().bytes.data(), written.value().bytes.size());
    CHECK(read.has_value());
    if (!read.has_value()) return;
    const SplatData& back = read.value().data;

    CHECK(back.size() == n);
    CHECK(read.value().sh_degree == 1);
    CHECK(read.value().color_space == khr::ColorSpace::lin_rec709_display);

    for (std::size_t s = 0; s < n; ++s) {
        CHECK(approx(back.positions()[s].x, original.positions()[s].x));
        CHECK(approx(back.positions()[s].z, original.positions()[s].z));
        CHECK(approx(back.scales()[s].y, original.scales()[s].y));
        CHECK(approx(back.opacities()[s], original.opacities()[s]));
        // Identity node write/read keeps the rotation exactly (no eigendecomposition reshuffle).
        CHECK(approx(back.rotations()[s].z, original.rotations()[s].z));
        CHECK(approx(back.rotations()[s].w, original.rotations()[s].w));
    }
    // The spherical harmonics must survive the there-and-back transpose exactly.
    const auto& a = original.sh().raw();
    const auto& b = back.sh().raw();
    CHECK(a.size() == b.size());
    if (a.size() == b.size()) {
        bool all = true;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!approx(a[i], b[i])) all = false;
        CHECK(all);
    }
}

void test_empty_scene_roundtrips() {
    // Zero splats is a valid (degenerate) SplatData; writing and reading it back should not crash,
    // though the reader treats a splat-less scene as an error (nothing to read).
    auto original = make_splats(0, 0);
    auto written = gltf::write_glb(original, khr::ColorSpace::srgb_rec709_display);
    CHECK(written.has_value());
    if (written.has_value()) {
        // The GLB is structurally valid, but has no splats, so read_glb reports "no splats".
        auto read = gltf::read_glb(written.value().bytes.data(), written.value().bytes.size());
        CHECK(!read.has_value());
    }
}

void test_degree4_is_truncated_with_loss() {
    auto original = make_splats(2, 4);
    auto written = gltf::write_glb(original, khr::ColorSpace::srgb_rec709_display);
    CHECK(written.has_value());
    if (!written.has_value()) return;
    // Degree 4 exceeds the KHR RC profile: a severe, approvable truncation loss must be reported.
    CHECK(has_loss(written.value().losses, "LOSS_SH_DEGREE_TRUNCATED"));
    CHECK(written.value().losses.has_blocking());
    CHECK(!written.value().losses.check_policy({}).has_value());
    CHECK(written.value().losses.check_policy({"LOSS_SH_DEGREE_TRUNCATED"}).has_value());

    auto read = gltf::read_glb(written.value().bytes.data(), written.value().bytes.size());
    CHECK(read.has_value());
    if (read.has_value()) {
        CHECK(read.value().sh_degree == 3);  // written at the profile ceiling
        // The degree 0-3 coefficients that survived must match the original's.
        const auto& a = original.sh().raw();  // 25 coeffs per splat
        const auto& b = read.value().data.sh().raw();  // 16 coeffs per splat
        // splat 0, DC (flat 0), all channels.
        for (int c = 0; c < 3; ++c) {
            CHECK(approx(a[static_cast<std::size_t>(0 * 25 * 3 + 0 * 3 + c)],
                         b[static_cast<std::size_t>(0 * 16 * 3 + 0 * 3 + c)]));
        }
        // splat 1, flat coefficient 15 (the last degree-3 coefficient).
        for (int c = 0; c < 3; ++c) {
            CHECK(approx(a[static_cast<std::size_t>(1 * 25 * 3 + 15 * 3 + c)],
                         b[static_cast<std::size_t>(1 * 16 * 3 + 15 * 3 + c)]));
        }
    }
}

}  // namespace

int main() {
    test_roundtrip_degree1();
    test_empty_scene_roundtrips();
    test_degree4_is_truncated_with_loss();

    if (g_failures == 0) {
        std::printf("gltf writer: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "gltf writer: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
