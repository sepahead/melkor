#include "melkor/densifier.hpp"
#include "melkor/metal_compute.hpp"
#include "melkor/spatial_grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace melkor {

namespace {

struct Candidate {
    float pos[3];
    float dir[3];   // gap direction for rim candidates, zero for splits
    uint32_t source; // splat whose appearance the new splat inherits
};

// Unit vector of the splat's largest principal axis (world space).
void majorAxis(const GaussianSplat& s, float out[3]) {
    int a = 0;
    if (s.scale_1 > s.scale_0) a = 1;
    if (s.scale_2 > (a == 0 ? s.scale_0 : s.scale_1)) a = 2;
    float rot[9];
    utils::quatToRotationMatrix(s.rot_0, s.rot_1, s.rot_2, s.rot_3, rot);
    out[0] = rot[a];
    out[1] = rot[3 + a];
    out[2] = rot[6 + a];
}

// Sequential dedup among accepted candidates: a simple hash grid at
// min_separation resolution, checked over the 27 surrounding cells.
class AcceptedGrid {
public:
    explicit AcceptedGrid(float cell) : inv_(1.0f / cell) {}

    bool tooClose(const float p[3], float min_sep) const {
        const float min_sq = min_sep * min_sep;
        const int cx = cellOf(p[0]), cy = cellOf(p[1]), cz = cellOf(p[2]);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    auto it = cells_.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == cells_.end()) continue;
                    for (size_t idx : it->second) {
                        const float* q = &points_[idx * 3];
                        const float ddx = q[0] - p[0];
                        const float ddy = q[1] - p[1];
                        const float ddz = q[2] - p[2];
                        if (ddx * ddx + ddy * ddy + ddz * ddz < min_sq)
                            return true;
                    }
                }
        return false;
    }

    void insert(const float p[3]) {
        const size_t idx = points_.size() / 3;
        points_.insert(points_.end(), {p[0], p[1], p[2]});
        cells_[key(cellOf(p[0]), cellOf(p[1]), cellOf(p[2]))].push_back(idx);
    }

private:
    int cellOf(float v) const { return static_cast<int>(std::floor(v * inv_)); }
    static uint64_t key(int x, int y, int z) {
        return (static_cast<uint64_t>(x & 0x1FFFFF) << 42) |
               (static_cast<uint64_t>(y & 0x1FFFFF) << 21) |
               (static_cast<uint64_t>(z & 0x1FFFFF));
    }
    float inv_;
    std::vector<float> points_;
    std::unordered_map<uint64_t, std::vector<size_t>> cells_;
};

} // namespace

class Densifier::Impl {
public:
    explicit Impl(metal::MetalContext* ctx) : ctx_(ctx) {}

    DensifyStats fillHoles(GaussianCloud& cloud, const DensifyConfig& cfg) {
        DensifyStats stats;
        const size_t n0 = cloud.size();
        if (n0 < 4) return stats;

        const size_t max_added = static_cast<size_t>(
            static_cast<double>(n0) * std::max(0.0f, cfg.max_growth));

        std::unique_ptr<metal::GaussianProcessor> gpu;
        if (ctx_ && cfg.use_gpu) {
            gpu = std::make_unique<metal::GaussianProcessor>(*ctx_);
        }

        for (int pass = 0; pass < cfg.max_iterations; ++pass) {
            if (stats.added >= max_added) break;

            const size_t n = cloud.size();
            std::vector<float> positions(n * 3);
            for (size_t i = 0; i < n; ++i) {
                positions[i * 3 + 0] = cloud[i].x;
                positions[i * 3 + 1] = cloud[i].y;
                positions[i * 3 + 2] = cloud[i].z;
            }

            const auto g = grid::buildGrid(positions);
            if (!g.valid) break;

            const int k = std::clamp(cfg.k_neighbors, 1,
                                     static_cast<int>(std::min<size_t>(n - 1, 32)));

            // Neighborhood stats: Metal when available, CPU otherwise.
            std::vector<float> knn;
            if (gpu) {
                knn = gpu->knnStatsGrid(positions, g.entries, g.cell_starts,
                                        g.cell_counts, g.origin.data(),
                                        g.cell_size, g.dims.data(), k);
            }
            if (knn.size() != n * 4) {
                knn = grid::knnStatsCpu(positions, g, k);
            }

            std::vector<float> spacings;
            spacings.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                if (knn[i * 4] > 0.0f) spacings.push_back(knn[i * 4]);
            }
            if (spacings.empty()) break;
            const size_t mid = spacings.size() / 2;
            std::nth_element(spacings.begin(), spacings.begin() + mid,
                             spacings.end());
            const float median = spacings[mid];
            if (!(median > 0.0f)) break;
            if (pass == 0) stats.median_spacing = median;

            const float step = median * cfg.spacing_multiplier;
            const float min_sep = median * cfg.min_separation_ratio;
            const float support_radius = median * cfg.max_hole_size;

            // Candidate generation.
            std::vector<Candidate> cands;
            for (size_t i = 0; i < n; ++i) {
                const float d = knn[i * 4 + 0];
                if (!(d > 0.0f)) continue;
                const float gx = knn[i * 4 + 1];
                const float gy = knn[i * 4 + 2];
                const float gz = knn[i * 4 + 3];
                const float glen = std::sqrt(gx * gx + gy * gy + gz * gz);

                if (glen > cfg.boundary_ratio * d) {
                    // Hole-rim point: advance the front along the gap.
                    Candidate c;
                    const float inv = 1.0f / glen;
                    c.dir[0] = gx * inv;
                    c.dir[1] = gy * inv;
                    c.dir[2] = gz * inv;
                    c.pos[0] = positions[i * 3 + 0] + c.dir[0] * step;
                    c.pos[1] = positions[i * 3 + 1] + c.dir[1] * step;
                    c.pos[2] = positions[i * 3 + 2] + c.dir[2] * step;
                    c.source = static_cast<uint32_t>(i);
                    cands.push_back(c);
                } else if (d > cfg.sparse_ratio * median) {
                    // Interior under-density: split along the splat's major
                    // axis. Zero dir = no far-support requirement.
                    float axis[3];
                    majorAxis(cloud[i], axis);
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        Candidate c;
                        c.dir[0] = c.dir[1] = c.dir[2] = 0.0f;
                        const float off = 0.5f * d * static_cast<float>(sgn);
                        c.pos[0] = positions[i * 3 + 0] + axis[0] * off;
                        c.pos[1] = positions[i * 3 + 1] + axis[1] * off;
                        c.pos[2] = positions[i * 3 + 2] + axis[2] * off;
                        c.source = static_cast<uint32_t>(i);
                        cands.push_back(c);
                    }
                }
            }
            if (cands.empty()) break;

            std::vector<float> cand_pos(cands.size() * 3);
            std::vector<float> cand_dir(cands.size() * 3);
            for (size_t i = 0; i < cands.size(); ++i) {
                for (int a = 0; a < 3; ++a) {
                    cand_pos[i * 3 + a] = cands[i].pos[a];
                    cand_dir[i * 3 + a] = cands[i].dir[a];
                }
            }

            std::vector<float> filter;
            if (gpu) {
                filter = gpu->filterCandidatesGrid(
                    cand_pos, cand_dir, positions, g.entries, g.cell_starts,
                    g.cell_counts, g.origin.data(), g.cell_size, g.dims.data(),
                    min_sep, support_radius);
            }
            if (filter.size() != cands.size() * 2) {
                filter = grid::candidateFilterCpu(cand_pos, cand_dir, positions,
                                                  g, min_sep, support_radius);
            }

            // Acceptance: far enough from existing splats, supported ahead
            // (rim candidates only), and not clustering with other accepted
            // candidates.
            AcceptedGrid accepted(std::max(min_sep, 1e-6f));
            const float max_log_scale = std::log(1.5f * median);
            size_t added_this_pass = 0;
            for (size_t i = 0; i < cands.size(); ++i) {
                if (stats.added + added_this_pass >= max_added) break;
                if (filter[i * 2 + 0] < min_sep) continue;
                if (filter[i * 2 + 1] < 0.5f) continue;
                if (accepted.tooClose(cands[i].pos, min_sep)) continue;

                GaussianSplat s = cloud[cands[i].source];
                s.x = cands[i].pos[0];
                s.y = cands[i].pos[1];
                s.z = cands[i].pos[2];
                // Cap the fill splats at the local spacing so a single
                // oversized rim splat cannot smear across the filled region.
                s.scale_0 = std::min(s.scale_0, max_log_scale);
                s.scale_1 = std::min(s.scale_1, max_log_scale);
                s.scale_2 = std::min(s.scale_2, max_log_scale);
                accepted.insert(cands[i].pos);
                cloud.addSplat(std::move(s));
                ++added_this_pass;
            }

            stats.passes = static_cast<size_t>(pass + 1);
            stats.added += added_this_pass;
            if (added_this_pass == 0) break;
        }
        return stats;
    }

private:
    metal::MetalContext* ctx_;
};

Densifier::Densifier(metal::MetalContext* ctx)
    : impl_(std::make_unique<Impl>(ctx)) {}

Densifier::~Densifier() = default;

DensifyStats Densifier::fillHoles(GaussianCloud& cloud,
                                  const DensifyConfig& config) {
    return impl_->fillHoles(cloud, config);
}

} // namespace melkor
