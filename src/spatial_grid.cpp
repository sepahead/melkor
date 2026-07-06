#include "melkor/spatial_grid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace melkor {
namespace grid {

namespace {

// Search caps shared with the Metal kernels (see gaussian_compute.metal).
// MAX_R bounds the expanding shell search; MAX_K bounds the neighbor heap.
constexpr int kMaxShellRadius = 16;
constexpr int kMaxK = 32;

inline int clampCell(float v, int dim) {
    int c = static_cast<int>(std::floor(v));
    return std::clamp(c, 0, dim - 1);
}

} // namespace

UniformGrid buildGrid(const std::vector<float>& positions,
                      float cell_size,
                      size_t max_cells) {
    UniformGrid g;
    const size_t n = positions.size() / 3;
    if (n == 0 || max_cells == 0) return g;

    float min_v[3] = {std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
    float max_v[3] = {std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest()};
    for (size_t i = 0; i < n; ++i) {
        for (int a = 0; a < 3; ++a) {
            const float v = positions[i * 3 + a];
            if (!std::isfinite(v)) return g;
            min_v[a] = std::min(min_v[a], v);
            max_v[a] = std::max(max_v[a], v);
        }
    }
    const float extent = std::max({max_v[0] - min_v[0],
                                   max_v[1] - min_v[1],
                                   max_v[2] - min_v[2]});
    if (extent < 1e-9f) return g;

    if (cell_size <= 0.0f) {
        cell_size = extent / std::cbrt(static_cast<float>(n)) * 2.0f;
    }

    // Grow the cell size until the dense cell arrays fit the budget.
    uint64_t dims[3];
    for (;;) {
        uint64_t total = 1;
        for (int a = 0; a < 3; ++a) {
            dims[a] = static_cast<uint64_t>(
                          std::ceil((max_v[a] - min_v[a]) / cell_size));
            dims[a] = std::max<uint64_t>(dims[a], 1);
            total *= dims[a];
        }
        if (total <= max_cells) break;
        cell_size *= 1.5f;
    }

    g.origin = {min_v[0], min_v[1], min_v[2]};
    g.cell_size = cell_size;
    g.dims = {static_cast<int>(dims[0]), static_cast<int>(dims[1]),
              static_cast<int>(dims[2])};
    g.num_points = n;

    const size_t num_cells = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    g.cell_counts.assign(num_cells, 0);
    g.cell_starts.assign(num_cells, 0);
    g.entries.resize(n);

    const float inv = 1.0f / cell_size;
    std::vector<uint32_t> cell_of(n);
    for (size_t i = 0; i < n; ++i) {
        const int cx = clampCell((positions[i * 3 + 0] - g.origin[0]) * inv, g.dims[0]);
        const int cy = clampCell((positions[i * 3 + 1] - g.origin[1]) * inv, g.dims[1]);
        const int cz = clampCell((positions[i * 3 + 2] - g.origin[2]) * inv, g.dims[2]);
        const uint32_t ci = static_cast<uint32_t>(
            (static_cast<size_t>(cz) * g.dims[1] + cy) * g.dims[0] + cx);
        cell_of[i] = ci;
        ++g.cell_counts[ci];
    }
    uint32_t running = 0;
    for (size_t c = 0; c < num_cells; ++c) {
        g.cell_starts[c] = running;
        running += g.cell_counts[c];
    }
    std::vector<uint32_t> cursor(g.cell_starts.begin(), g.cell_starts.end());
    for (size_t i = 0; i < n; ++i) {
        g.entries[cursor[cell_of[i]]++] = static_cast<uint32_t>(i);
    }

    g.valid = true;
    return g;
}

std::vector<float> knnStatsCpu(const std::vector<float>& positions,
                               const UniformGrid& g,
                               int k_neighbors) {
    const size_t n = positions.size() / 3;
    std::vector<float> out(n * 4, 0.0f);
    if (!g.valid || n == 0) return out;

    const int k = std::clamp(k_neighbors, 1, kMaxK);
    const float inv = 1.0f / g.cell_size;

    for (size_t i = 0; i < n; ++i) {
        const float px = positions[i * 3 + 0];
        const float py = positions[i * 3 + 1];
        const float pz = positions[i * 3 + 2];
        const int cx = clampCell((px - g.origin[0]) * inv, g.dims[0]);
        const int cy = clampCell((py - g.origin[1]) * inv, g.dims[1]);
        const int cz = clampCell((pz - g.origin[2]) * inv, g.dims[2]);

        float best_d[kMaxK];
        uint32_t best_i[kMaxK];
        int filled = 0;

        for (int r = 0; r <= kMaxShellRadius; ++r) {
            for (int dz = -r; dz <= r; ++dz) {
                for (int dy = -r; dy <= r; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        // Shell only: skip cells already visited at radius r-1.
                        if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r)
                            continue;
                        const int x = cx + dx, y = cy + dy, z = cz + dz;
                        if (x < 0 || y < 0 || z < 0 ||
                            x >= g.dims[0] || y >= g.dims[1] || z >= g.dims[2])
                            continue;
                        const size_t ci =
                            (static_cast<size_t>(z) * g.dims[1] + y) * g.dims[0] + x;
                        const uint32_t start = g.cell_starts[ci];
                        const uint32_t count = g.cell_counts[ci];
                        for (uint32_t t = start; t < start + count; ++t) {
                            const uint32_t j = g.entries[t];
                            if (j == i) continue;
                            const float ddx = positions[j * 3 + 0] - px;
                            const float ddy = positions[j * 3 + 1] - py;
                            const float ddz = positions[j * 3 + 2] - pz;
                            const float d = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                            if (filled < k) {
                                int p = filled;
                                while (p > 0 && best_d[p - 1] > d) {
                                    best_d[p] = best_d[p - 1];
                                    best_i[p] = best_i[p - 1];
                                    --p;
                                }
                                best_d[p] = d;
                                best_i[p] = j;
                                ++filled;
                            } else if (d < best_d[k - 1]) {
                                int p = k - 1;
                                while (p > 0 && best_d[p - 1] > d) {
                                    best_d[p] = best_d[p - 1];
                                    best_i[p] = best_i[p - 1];
                                    --p;
                                }
                                best_d[p] = d;
                                best_i[p] = j;
                            }
                        }
                    }
                }
            }
            // Any point not yet visited lies in a cell at Chebyshev distance
            // > r, i.e. farther than r * cell_size — safe to stop.
            if (filled >= k &&
                best_d[k - 1] <= static_cast<float>(r) * g.cell_size)
                break;
        }

        if (filled == 0) continue;
        const int kk = std::min(k, filled);
        float sum = 0.0f, cx_n = 0.0f, cy_n = 0.0f, cz_n = 0.0f;
        for (int t = 0; t < kk; ++t) {
            sum += best_d[t];
            const uint32_t j = best_i[t];
            cx_n += positions[j * 3 + 0];
            cy_n += positions[j * 3 + 1];
            cz_n += positions[j * 3 + 2];
        }
        const float inv_k = 1.0f / static_cast<float>(kk);
        out[i * 4 + 0] = sum * inv_k;
        out[i * 4 + 1] = px - cx_n * inv_k;
        out[i * 4 + 2] = py - cy_n * inv_k;
        out[i * 4 + 3] = pz - cz_n * inv_k;
    }
    return out;
}

std::vector<float> candidateFilterCpu(const std::vector<float>& candidates,
                                      const std::vector<float>& directions,
                                      const std::vector<float>& positions,
                                      const UniformGrid& g,
                                      float min_separation,
                                      float support_radius) {
    const size_t m = candidates.size() / 3;
    std::vector<float> out(m * 2, 0.0f);
    if (m == 0) return out;
    if (!g.valid || directions.size() < m * 3) {
        // No grid to test against: nothing nearby, no support.
        for (size_t i = 0; i < m; ++i) {
            out[i * 2 + 0] = std::numeric_limits<float>::max();
            out[i * 2 + 1] = 0.0f;
        }
        return out;
    }

    const float inv = 1.0f / g.cell_size;
    const int r_min = static_cast<int>(std::ceil(min_separation * inv));
    const int r_sup = static_cast<int>(std::ceil(support_radius * inv));
    const int r_max = std::min(std::max(r_min, r_sup), kMaxShellRadius);

    for (size_t i = 0; i < m; ++i) {
        const float px = candidates[i * 3 + 0];
        const float py = candidates[i * 3 + 1];
        const float pz = candidates[i * 3 + 2];
        const float dx_dir = directions[i * 3 + 0];
        const float dy_dir = directions[i * 3 + 1];
        const float dz_dir = directions[i * 3 + 2];
        const bool need_support =
            (dx_dir != 0.0f || dy_dir != 0.0f || dz_dir != 0.0f);

        const int cx = clampCell((px - g.origin[0]) * inv, g.dims[0]);
        const int cy = clampCell((py - g.origin[1]) * inv, g.dims[1]);
        const int cz = clampCell((pz - g.origin[2]) * inv, g.dims[2]);

        float min_dist = std::numeric_limits<float>::max();
        bool support = !need_support;

        for (int r = 0; r <= r_max; ++r) {
            for (int dz = -r; dz <= r; ++dz) {
                for (int dy = -r; dy <= r; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r)
                            continue;
                        const int x = cx + dx, y = cy + dy, z = cz + dz;
                        if (x < 0 || y < 0 || z < 0 ||
                            x >= g.dims[0] || y >= g.dims[1] || z >= g.dims[2])
                            continue;
                        const size_t ci =
                            (static_cast<size_t>(z) * g.dims[1] + y) * g.dims[0] + x;
                        const uint32_t start = g.cell_starts[ci];
                        const uint32_t count = g.cell_counts[ci];
                        for (uint32_t t = start; t < start + count; ++t) {
                            const uint32_t j = g.entries[t];
                            const float qx = positions[j * 3 + 0] - px;
                            const float qy = positions[j * 3 + 1] - py;
                            const float qz = positions[j * 3 + 2] - pz;
                            const float d = std::sqrt(qx * qx + qy * qy + qz * qz);
                            min_dist = std::min(min_dist, d);
                            if (!support && d <= support_radius &&
                                qx * dx_dir + qy * dy_dir + qz * dz_dir > 1e-6f) {
                                support = true;
                            }
                        }
                    }
                }
            }
            // The min-distance question is settled once we either found a
            // point closer than min_separation or searched past it.
            const bool min_settled =
                min_dist < min_separation || r >= r_min;
            const bool support_settled = support || r >= r_sup;
            if (min_settled && support_settled) break;
        }

        out[i * 2 + 0] = min_dist;
        out[i * 2 + 1] = support ? 1.0f : 0.0f;
    }
    return out;
}

} // namespace grid
} // namespace melkor
