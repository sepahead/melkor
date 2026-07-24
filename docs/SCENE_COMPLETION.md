# Scene Completion (Densification / Hole Filling)

Melkor can fill holes and densify sparse regions of a Gaussian-splat scene
directly, without a learned prior. In the 3D Gaussian Splatting literature
this family of operations goes by several names:

- **Densification** — the 3DGS-native mechanism (the original paper's
  *Adaptive Density Control*): adding Gaussians where the scene is
  under-represented, by cloning or splitting existing primitives.
- **Hole filling / scene completion** — the editing task: reconstructing
  plausible geometry in regions the capture never saw (occlusion shadows,
  scan gaps, removed objects).
- **3D inpainting** — the same task viewed from the image-editing tradition.
  diffusion-based methods (InFusion, GScream, Inpaint360GS, …) use that name.

Melkor implements the *geometric* form: deterministic, prior-free
densification that extends the scene's own local structure into its gaps.
It runs in milliseconds on Metal. The CPU fallback has the same behavior.
Thus, automated pipelines do not need a diffusion model.

## Usage

```bash
melkor scene.spz completed.spz --fill-holes
melkor scene.ply completed.ply --fill-holes --fill-strength 0.8 --max-hole-size 12
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--fill-holes` | off | Enable scene completion |
| `--fill-iterations <int>` | 3 | Advancing-front passes. Each pass can close roughly one ring of the hole. |
| `--fill-strength <float>` | 1.0 | Fill spacing in units of the median splat spacing (lower = denser fill) |
| `--max-hole-size <float>` | 8.0 | Largest bridgeable hole, in multiples of the median splat spacing |
| `--knn <int>` | 8 | Neighborhood size used for the density statistics |

This operation works on each input that Melkor can load (PLY, SPZ, and
GLB-derived clouds). Melkor writes the filled cloud in the requested format.

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
     direction) within `--max-hole-size` median spacings. An interior hole
     always has a far rim to bridge to. The scene's outer boundary has
     nothing beyond it. This is what keeps hole filling from growing the
     scene outward indefinitely.
4. **Synthesis.** Accepted candidates become splats that inherit color/SH,
   opacity, scale (capped at ~1.5x the median spacing), and orientation
   from their source splat, so filled regions blend with the surrounding
   appearance.

Passes repeat until the fronts meet, nothing is accepted, or the growth cap
(`max_growth`, default 1.0x the input size) is reached. The whole procedure
is deterministic — no RNG — so identical inputs produce identical outputs
across runs and across CPU/Metal backends.

## Choosing parameters

- Occlusion shadows behind objects in ground-level scans are usually a few
  splat spacings wide: the defaults close them.
- Larger voids (unscanned courtyards, roof gaps in aerial captures) need a
  bigger `--max-hole-size` and more `--fill-iterations`. Expect a flat
  continuation of the rim geometry.
- `--fill-strength` below 1.0 fills more densely than the surrounding
  scene. This setting is useful when a trainer will reoptimize the fill.

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
- Holes larger than `--max-hole-size` median spacings are deliberately left
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
