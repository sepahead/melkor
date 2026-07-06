// Scene-completion (densification) tests.
//
// Self-contained like the other melkor tests (no framework). Coverage:
//   1. Uniform grid construction: every point lands in exactly one cell.
//   2. Grid k-NN stats against an exact brute-force reference.
//   3. Hole filling: a synthetic plane with a punched disc gets splats back
//      inside the hole, on the plane, with inherited appearance.
//   4. Boundary containment: a plane WITHOUT a hole must not grow outward
//      (the far-support test must reject rim extrapolation at the scene edge).
//   5. CPU/Metal parity of the grid k-NN and candidate-filter kernels
//      (only when a Metal device is present).

#include "melkor/densifier.hpp"
#include "melkor/gaussian_data.hpp"
#include "melkor/spatial_grid.hpp"

#ifdef MELKOR_HAS_METAL
#include "melkor/metal_compute.hpp"
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (cond) {
        printf("  PASS: %s\n", msg);
    } else {
        printf("  FAIL: %s\n", msg);
        ++g_failures;
    }
}

// Deterministic pseudo-random floats (LCG), so runs are reproducible.
struct Lcg {
    uint32_t state;
    explicit Lcg(uint32_t seed) : state(seed) {}
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    }
};

// Builds a plane of splats on z=0, x/y in [0, (side-1)*spacing], optionally
// punching a disc of the given radius out of the center.
melkor::GaussianCloud makePlane(int side, float spacing, float hole_radius) {
    melkor::GaussianCloud cloud;
    const float cx = (side - 1) * spacing * 0.5f;
    const float cy = cx;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const float px = x * spacing;
            const float py = y * spacing;
            if (hole_radius > 0.0f) {
                const float dx = px - cx, dy = py - cy;
                if (std::sqrt(dx * dx + dy * dy) < hole_radius) continue;
            }
            melkor::GaussianSplat s{};
            s.x = px;
            s.y = py;
            s.z = 0.0f;
            s.f_dc_0 = 0.7f;
            s.f_dc_1 = -0.3f;
            s.f_dc_2 = 0.1f;
            s.opacity = 2.0f;                       // logit space
            s.scale_0 = s.scale_1 = std::log(spacing * 0.6f);
            s.scale_2 = std::log(spacing * 0.15f);
            s.rot_0 = 1.0f;                          // identity quaternion
            s.rot_1 = s.rot_2 = s.rot_3 = 0.0f;
            cloud.addSplat(s);
        }
    }
    return cloud;
}

std::vector<float> positionsOf(const melkor::GaussianCloud& cloud) {
    std::vector<float> p(cloud.size() * 3);
    for (size_t i = 0; i < cloud.size(); ++i) {
        p[i * 3 + 0] = cloud[i].x;
        p[i * 3 + 1] = cloud[i].y;
        p[i * 3 + 2] = cloud[i].z;
    }
    return p;
}

// ---- Test 1: grid construction --------------------------------------------
bool test_grid_build() {
    printf("[test] uniform grid construction\n");
    Lcg rng(42);
    std::vector<float> pos;
    const size_t n = 2000;
    for (size_t i = 0; i < n * 3; ++i) pos.push_back(rng.next() * 10.0f - 5.0f);

    auto g = melkor::grid::buildGrid(pos);
    check(g.valid, "grid is valid for random cloud");
    check(g.entries.size() == n, "every point has exactly one entry");

    // entries must be a permutation of 0..n-1
    std::vector<bool> seen(n, false);
    bool perm = true;
    for (uint32_t e : g.entries) {
        if (e >= n || seen[e]) { perm = false; break; }
        seen[e] = true;
    }
    check(perm, "entries form a permutation of the point indices");

    uint64_t total = 0;
    for (uint32_t c : g.cell_counts) total += c;
    check(total == n, "cell counts sum to the point count");

    auto g_empty = melkor::grid::buildGrid({});
    check(!g_empty.valid, "empty input yields invalid grid");

    std::vector<float> degenerate(30, 1.0f);  // 10 identical points
    auto g_deg = melkor::grid::buildGrid(degenerate);
    check(!g_deg.valid, "zero-extent input yields invalid grid");
    return true;
}

// ---- Test 2: grid k-NN vs brute force --------------------------------------
bool test_knn_stats_vs_bruteforce() {
    printf("[test] grid k-NN stats vs exact brute force\n");
    Lcg rng(7);
    std::vector<float> pos;
    const size_t n = 500;
    const int k = 8;
    for (size_t i = 0; i < n * 3; ++i) pos.push_back(rng.next() * 4.0f);

    auto g = melkor::grid::buildGrid(pos);
    auto stats = melkor::grid::knnStatsCpu(pos, g, k);

    float max_err = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        std::vector<float> d;
        d.reserve(n - 1);
        for (size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            const float dx = pos[j * 3] - pos[i * 3];
            const float dy = pos[j * 3 + 1] - pos[i * 3 + 1];
            const float dz = pos[j * 3 + 2] - pos[i * 3 + 2];
            d.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        std::partial_sort(d.begin(), d.begin() + k, d.end());
        const float ref = std::accumulate(d.begin(), d.begin() + k, 0.0f) / k;
        max_err = std::max(max_err, std::abs(ref - stats[i * 4]));
    }
    check(max_err < 1e-4f, "grid mean k-NN distance matches brute force");
    return true;
}

// ---- Test 3: hole filling ---------------------------------------------------
bool test_fills_plane_hole() {
    printf("[test] hole filling on a punched plane\n");
    const int side = 40;
    const float spacing = 0.1f;
    const float hole_r = 0.35f;  // 3.5 spacings — well within max_hole_size
    auto cloud = makePlane(side, spacing, hole_r);
    const size_t n0 = cloud.size();
    const float cx = (side - 1) * spacing * 0.5f;

    melkor::Densifier densifier;  // CPU path
    melkor::DensifyConfig cfg;
    auto stats = densifier.fillHoles(cloud, cfg);

    check(stats.added > 0, "splats were added");
    check(cloud.size() == n0 + stats.added, "cloud grew by the reported count");
    check(std::abs(stats.median_spacing - spacing) < spacing * 0.5f,
          "median spacing is near the grid spacing");

    // The hole center must be covered now.
    size_t in_hole = 0;
    bool near_center = false;
    bool on_plane = true;
    bool inherits = true;
    for (size_t i = n0; i < cloud.size(); ++i) {
        const auto& s = cloud[i];
        const float dx = s.x - cx, dy = s.y - cx;
        const float r = std::sqrt(dx * dx + dy * dy);
        if (r < hole_r) ++in_hole;
        if (r < spacing * 1.5f) near_center = true;
        if (std::abs(s.z) > spacing * 0.75f) on_plane = false;
        if (std::abs(s.f_dc_0 - 0.7f) > 1e-6f || std::abs(s.opacity - 2.0f) > 1e-6f)
            inherits = false;
        if (!std::isfinite(s.x) || !std::isfinite(s.scale_0) ||
            !std::isfinite(s.rot_0))
            inherits = false;
    }
    check(in_hole > 0, "new splats landed inside the hole");
    check(near_center, "the hole center was reached");
    check(on_plane, "new splats stay on the plane");
    check(inherits, "new splats inherit appearance from their sources");
    check(stats.added <= n0, "growth respects the max_growth cap");
    return true;
}

// ---- Test 4: no outward growth ---------------------------------------------
bool test_no_boundary_growth() {
    printf("[test] outer boundary is not extrapolated\n");
    const int side = 30;
    const float spacing = 0.1f;
    auto cloud = makePlane(side, spacing, 0.0f);  // no hole
    const size_t n0 = cloud.size();

    float min0[3], max0[3], min1[3], max1[3];
    cloud.computeBoundingBox(min0[0], min0[1], min0[2], max0[0], max0[1], max0[2]);

    melkor::Densifier densifier;
    melkor::DensifyConfig cfg;
    auto stats = densifier.fillHoles(cloud, cfg);

    cloud.computeBoundingBox(min1[0], min1[1], min1[2], max1[0], max1[1], max1[2]);

    check(stats.added == 0, "a complete plane gains no splats");
    bool same_bbox = true;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(min1[a] - min0[a]) > 1e-6f ||
            std::abs(max1[a] - max0[a]) > 1e-6f)
            same_bbox = false;
    }
    check(same_bbox, "bounding box unchanged");
    check(cloud.size() == n0, "cloud size unchanged");
    return true;
}

// ---- Test 5: tiny/degenerate inputs ----------------------------------------
bool test_degenerate_inputs() {
    printf("[test] degenerate inputs\n");
    melkor::Densifier densifier;
    melkor::DensifyConfig cfg;

    melkor::GaussianCloud empty;
    auto s0 = densifier.fillHoles(empty, cfg);
    check(s0.added == 0 && empty.empty(), "empty cloud is a no-op");

    melkor::GaussianCloud two;
    for (int i = 0; i < 2; ++i) {
        melkor::GaussianSplat s{};
        s.x = static_cast<float>(i);
        s.rot_0 = 1.0f;
        two.addSplat(s);
    }
    auto s1 = densifier.fillHoles(two, cfg);
    check(s1.added == 0 && two.size() == 2, "sub-minimal cloud is a no-op");

    // Coincident points: zero extent, must not crash or add.
    melkor::GaussianCloud stacked;
    for (int i = 0; i < 16; ++i) {
        melkor::GaussianSplat s{};
        s.x = s.y = s.z = 1.0f;
        s.rot_0 = 1.0f;
        stacked.addSplat(s);
    }
    auto s2 = densifier.fillHoles(stacked, cfg);
    check(s2.added == 0 && stacked.size() == 16, "coincident cloud is a no-op");
    return true;
}

// ---- Test 6: CPU/Metal parity (macOS with a GPU only) -----------------------
#ifdef MELKOR_HAS_METAL
bool test_metal_parity() {
    printf("[test] CPU vs Metal grid-kernel parity\n");
    if (!melkor::metal::MetalContext::isAvailable()) {
        printf("  SKIP: no Metal device\n");
        return true;
    }
    melkor::metal::MetalContext ctx;
    if (!ctx.initialize()) {
        printf("  SKIP: Metal failed to initialize\n");
        return true;
    }

    Lcg rng(1234);
    std::vector<float> pos;
    const size_t n = 20000;  // above the brute-force cutoff on purpose
    const int k = 8;
    for (size_t i = 0; i < n * 3; ++i) pos.push_back(rng.next() * 8.0f);

    auto g = melkor::grid::buildGrid(pos);
    auto cpu = melkor::grid::knnStatsCpu(pos, g, k);

    melkor::metal::GaussianProcessor proc(ctx);
    auto gpu = proc.knnStatsGrid(pos, g.entries, g.cell_starts, g.cell_counts,
                                 g.origin.data(), g.cell_size, g.dims.data(), k);
    check(gpu.size() == cpu.size(), "Metal k-NN stats has the right size");
    if (gpu.size() == cpu.size()) {
        float max_err = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            max_err = std::max(max_err, std::abs(cpu[i * 4] - gpu[i * 4]));
        }
        check(max_err < 1e-3f, "Metal mean k-NN distances match CPU");
    }

    // Candidate filter parity on a plane-with-hole scenario.
    auto cloud = makePlane(40, 0.1f, 0.35f);
    auto ppos = positionsOf(cloud);
    auto pg = melkor::grid::buildGrid(ppos);
    std::vector<float> cands, dirs;
    Lcg rng2(99);
    for (int i = 0; i < 500; ++i) {
        cands.push_back(rng2.next() * 4.0f);
        cands.push_back(rng2.next() * 4.0f);
        cands.push_back(rng2.next() * 0.2f - 0.1f);
        float dx = rng2.next() - 0.5f, dy = rng2.next() - 0.5f, dz = 0.0f;
        const float len = std::sqrt(dx * dx + dy * dy) + 1e-9f;
        dirs.push_back(dx / len);
        dirs.push_back(dy / len);
        dirs.push_back(dz);
    }
    const float min_sep = 0.07f, support = 0.8f;
    auto fc = melkor::grid::candidateFilterCpu(cands, dirs, ppos, pg, min_sep, support);
    auto fg = proc.filterCandidatesGrid(cands, dirs, ppos, pg.entries,
                                        pg.cell_starts, pg.cell_counts,
                                        pg.origin.data(), pg.cell_size,
                                        pg.dims.data(), min_sep, support);
    check(fg.size() == fc.size(), "Metal candidate filter has the right size");
    if (fg.size() == fc.size()) {
        size_t decision_mismatches = 0;
        for (size_t i = 0; i < fc.size() / 2; ++i) {
            const bool cpu_pass = fc[i * 2] >= min_sep && fc[i * 2 + 1] > 0.5f;
            const bool gpu_pass = fg[i * 2] >= min_sep && fg[i * 2 + 1] > 0.5f;
            if (cpu_pass != gpu_pass) ++decision_mismatches;
        }
        check(decision_mismatches == 0, "CPU and Metal accept the same candidates");
    }

    // End-to-end: Metal-backed hole fill also closes the hole.
    auto cloud2 = makePlane(40, 0.1f, 0.35f);
    const size_t before = cloud2.size();
    melkor::Densifier densifier(&ctx);
    melkor::DensifyConfig cfg;
    auto stats = densifier.fillHoles(cloud2, cfg);
    check(stats.added > 0 && cloud2.size() == before + stats.added,
          "Metal-backed fillHoles adds splats");
    return true;
}
#endif

} // namespace

int main() {
    printf("melkor densifier tests\n");
    test_grid_build();
    test_knn_stats_vs_bruteforce();
    test_fills_plane_hole();
    test_no_boundary_growth();
    test_degenerate_inputs();
#ifdef MELKOR_HAS_METAL
    test_metal_parity();
#endif

    if (g_failures == 0) {
        printf("\nAll densifier tests passed.\n");
        return 0;
    }
    printf("\n%d densifier test(s) FAILED.\n", g_failures);
    return 1;
}
