# Scene completion development status

Melkor contains an internal geometric densifier.
The public CLI does not expose this implementation.
`--fill-holes` fails until the densifier accepts canonical `SplatData`.

Do not automate the compatibility flags.
They parse only to provide a clear migration error.

The internal implementation covers a family of operations with several established names:

- **Densification** — the 3DGS-native mechanism (the original paper's
  *Adaptive Density Control*): adding Gaussians where the scene is
  under-represented, by cloning or splitting existing primitives.
- **Hole filling / scene completion** — the editing task: reconstructing
  plausible geometry in regions the capture never saw (occlusion shadows,
  scan gaps, removed objects).
- **3D inpainting** — the same task viewed from the image-editing tradition.
  diffusion-based methods (InFusion, GScream, Inpaint360GS, …) use that name.

The prototype uses geometric densification.
It extends local scene structure without a learned prior.
This method cannot reconstruct appearance that is absent from the source.

## Internal configuration

`DensifyConfig` defines the prototype controls.
The values are not a stable CLI contract.

| Field | Default | Meaning |
|---|---:|---|
| `k_neighbors` | 8 | Neighborhood size for density statistics |
| `max_iterations` | 3 | Maximum advancing-front passes |
| `spacing_multiplier` | 1.0 | Fill spacing in median-spacing units |
| `max_hole_size` | 8.0 | Maximum bridge distance in median-spacing units |
| `max_growth` | 1.0 | Added-splat limit as an input-size fraction |

## Algorithm

Each pass:

1. **Neighborhood statistics.** For every splat, the mean distance to its
   k nearest neighbors and the *gap vector* — the offset from the splat to
   the centroid of those neighbors. The gap vector points toward empty
   space. Its magnitude measures neighborhood asymmetry relative to local
   spacing. The value is near zero inside a surface and large on a hole rim.
   Computed on a uniform grid, on Metal when available
   (`knn_stats_grid` kernel) with a cell-identical CPU fallback.
2. **Candidate generation.**
   - *Hole rims*: splats with a strongly one-sided neighborhood propose a
     new splat one fill-spacing along the gap direction — the rim advances
     into the hole.
   - *Sparse interiors*: splats whose local spacing is far above the median
     propose companions along their own major axis (classic clone/split
     densification).
3. **Candidate filtering** (`filter_candidates_grid` kernel, CPU fallback):
   - reject candidates closer than `0.7 x` median spacing to the existing
     cloud or to an already-accepted candidate (no clumping).
   - **far-support gate**: a rim candidate is accepted only if existing
     geometry lies *ahead of it* (in the forward half-space of its gap
     direction) within `max_hole_size` median spacings. An interior hole
     always has a far rim to bridge to. The scene's outer boundary has
     nothing beyond it. This is what keeps hole filling from growing the
     scene outward indefinitely.
4. **Synthesis.** Accepted candidates become splats that inherit color/SH,
   opacity, scale (capped at ~1.5x the median spacing), and orientation
   from their source splat, so filled regions blend with the surrounding
   appearance.

Passes repeat until the fronts meet, no candidate passes, or the growth cap is reached.
The procedure does not use random numbers.
Backend parity tests allow normal floating-point differences.

## Choosing parameters

- Occlusion shadows behind objects in ground-level scans are usually a few
  splat spacings wide: the defaults close them.
- Larger voids require a larger `max_hole_size` and more iterations.
  Expect a flat continuation of the rim geometry.
- A `spacing_multiplier` below 1.0 makes a denser fill.
  A later optimizer can process that fill.

A sphere test uses a cap-hole radius of approximately six median spacings.
The front closes the cap in six passes. At the cap center, the fill extends at
most two median spacings beyond the true surface. This distance is
approximately one third of the hole radius. The algorithm extrapolates the
rim instead of curving it. `test_fills_sphere_cap` in
`tests/test_densifier.cpp` locks this behavior.

## Limitations

- Purely geometric: the fill continues local structure and copies nearby
  appearance. It will not hallucinate texture detail the way
  diffusion-based 3D inpainting does.
- Holes larger than `max_hole_size` median spacings are deliberately left
  open (the far-support gate cannot distinguish them from open boundary).
- The scene's outer boundary is never extended — by design.

## Testing

`tests/test_densifier.cpp` covers:

- Grid construction
- Grid k-NN against an exact brute-force reference
- Hole closure on a punched plane
- Outer-boundary containment and degenerate inputs
- CPU and Metal parity for both kernels

The Metal tests skip themselves on machines without a GPU.
