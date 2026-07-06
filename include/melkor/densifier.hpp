#pragma once

// Scene completion for Gaussian splat clouds — the 3DGS counterpart of image
// inpainting. The Gaussian-splatting literature calls the mechanism
// "densification" (after the original paper's Adaptive Density Control) and
// the editing task "hole filling" / "scene completion"; this module does both
// without any learned prior:
//
//   * Hole bridging: points whose k-NN neighborhood is strongly one-sided
//     (large gap vector) sit on the rim of a hole. New splats are extrapolated
//     along the gap direction, advancing the rim inward each pass until the
//     fronts meet. A candidate is only accepted when existing geometry lies
//     AHEAD of it within max_hole_size — i.e. there is a far rim to bridge
//     to — which cleanly separates interior holes from the scene's outer
//     boundary and keeps the cloud from growing outward.
//   * Sparse-region splitting: interior points whose local spacing is far
//     above the median get companions along their splat's major axis (the
//     classic clone/split densification on the primitive's own shape).
//
// New splats inherit color/SH, opacity, scale, and orientation from their
// source splat, so filled regions blend with the surrounding appearance.
// The whole pass is deterministic (no RNG) and runs the neighbor searches on
// Metal when a context is provided, with a bit-compatible CPU fallback.

#include "melkor/gaussian_data.hpp"

#include <cstddef>
#include <memory>

namespace melkor {

namespace metal { class MetalContext; }

struct DensifyConfig {
    int k_neighbors = 8;            // neighborhood size for density stats
    int max_iterations = 3;         // advancing-front passes
    float spacing_multiplier = 1.0f; // fill spacing in units of median spacing
                                     // (lower = denser fill)
    float boundary_ratio = 0.30f;   // |gap| / mean_dist above which a point is
                                     // treated as a hole-rim point (a straight
                                     // lattice edge measures ~0.42, interior
                                     // ~0; outer-boundary growth is prevented
                                     // by the far-support test, not this)
    float sparse_ratio = 2.5f;      // mean_dist / median above which an
                                     // interior point is split
    float min_separation_ratio = 0.7f; // reject candidates closer than this
                                        // (x median spacing) to existing points
    float max_hole_size = 8.0f;     // largest bridgeable hole, in multiples of
                                     // the median spacing
    float max_growth = 1.0f;        // cap on added splats as a fraction of the
                                     // input size
    bool use_gpu = true;            // use Metal for neighbor searches when a
                                     // context is available
};

struct DensifyStats {
    size_t added = 0;       // total splats synthesized
    size_t passes = 0;      // passes that actually ran
    float median_spacing = 0.0f; // median k-NN spacing of the input cloud
};

class Densifier {
public:
    // ctx may be null: all neighbor searches then run on the CPU. On non-Metal
    // platforms the Metal calls are stubs that return empty, which triggers
    // the same CPU fallback.
    explicit Densifier(metal::MetalContext* ctx = nullptr);
    ~Densifier();

    // Fills holes and densifies sparse regions in place.
    DensifyStats fillHoles(GaussianCloud& cloud, const DensifyConfig& config);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace melkor
