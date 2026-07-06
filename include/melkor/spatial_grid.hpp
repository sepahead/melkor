#pragma once

// Uniform spatial grid shared by the CPU and Metal neighbor-search paths.
//
// The grid is always built on the host (counting sort, O(n)) and its flat
// arrays are either walked directly on the CPU or uploaded as Metal buffers.
// Building once and sharing the arrays guarantees both backends search the
// same cells in the same order, so CPU/GPU results differ only by float
// rounding, never by candidate set.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace melkor {
namespace grid {

struct UniformGrid {
    std::array<float, 3> origin{{0.f, 0.f, 0.f}};
    float cell_size = 1.0f;
    std::array<int, 3> dims{{1, 1, 1}};
    // Point indices grouped by cell (counting sort), addressed via
    // cell_starts/cell_counts. Cell index = (z * dims.y + y) * dims.x + x.
    std::vector<uint32_t> entries;
    std::vector<uint32_t> cell_starts;
    std::vector<uint32_t> cell_counts;
    size_t num_points = 0;
    bool valid = false;
};

// Build a grid over xyz-interleaved positions. cell_size 0 picks
// extent / cbrt(n) * 2 (the same heuristic as the k-NN spatial hash).
// The cell size grows as needed so the dense cell arrays never exceed
// max_cells. Returns an invalid grid for empty or degenerate (zero-extent)
// inputs.
UniformGrid buildGrid(const std::vector<float>& positions,
                      float cell_size = 0.0f,
                      size_t max_cells = size_t(1) << 22);

// Per-point k-NN statistics against the grid's own points.
// Returns 4 floats per point: mean distance to the k nearest neighbors,
// then the gap vector (point minus neighbor centroid). The gap vector points
// away from the local mass of neighbors — toward empty space — and its
// magnitude relative to the mean distance measures how one-sided the
// neighborhood is (≈0 interior, large on a boundary or hole rim).
// k is clamped to [1, 32].
std::vector<float> knnStatsCpu(const std::vector<float>& positions,
                               const UniformGrid& g,
                               int k_neighbors);

// Per-candidate acceptance statistics for densification.
// For each candidate (xyz) with associated direction (xyz, may be zero):
//   out[i*2 + 0] = distance to the nearest cloud point (within the searched
//                  radius; guaranteed exact when < min_separation),
//   out[i*2 + 1] = 1.0 if any cloud point lies within support_radius in the
//                  forward half-space dot(q - c, dir) > 0, else 0.0.
//                  A zero direction skips the half-space test (support = 1).
// The forward-support test is what separates an interior hole (a far rim
// exists across the gap) from the outer boundary of the scene (nothing
// beyond), so hole filling never grows the cloud outward unboundedly.
std::vector<float> candidateFilterCpu(const std::vector<float>& candidates,
                                      const std::vector<float>& directions,
                                      const std::vector<float>& positions,
                                      const UniformGrid& g,
                                      float min_separation,
                                      float support_radius);

} // namespace grid
} // namespace melkor
