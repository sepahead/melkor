# SOTA Feedforward Reconstruction Models

Melkor's feedforward stage turns images into geometry or splats in a single
forward pass — no COLMAP/GLOMAP, no per-scene optimization. Depth-Anything-3
([docs/DA3_FEEDFORWARD.md](DA3_FEEDFORWARD.md)) was the first integration;
`scripts/setup_feedforward_sota.sh` adds the strongest 2025–2026 alternatives.

Install one, a group, or list the catalog:

```bash
./scripts/setup_feedforward_sota.sh list
./scripts/setup_feedforward_sota.sh permissive            # MIT/Apache only
./scripts/setup_feedforward_sota.sh vggt --accept-noncommercial
./scripts/setup_feedforward_sota.sh amb3r --accept-unlicensed
```

Each install clones the upstream repo into `tools/<name>/repo`, builds a
venv, and drops a `./<name>-infer` wrapper at the project root.

> These models need **Linux + NVIDIA CUDA** and download multi-GB weights.
> They do not run on the macOS/Metal build. Melkor itself is MIT; the setup
> script refuses non-permissive weights unless you pass the matching
> `--accept-*` flag, and it re-prints each repo's actual `LICENSE` at install
> time so the terms you see are current.

## Two integration shapes

```
                       ┌── geometry models ──┐
 images ──► feedforward │  VGGT · MapAnything │──► COLMAP sparse/ ──► pipeline.sh
                       │  Pi3 · AMB3R        │        (OpenSplat / gsplat / LichtFeld)
                       └─────────────────────┘
                       ┌── direct-splat ─────┐
 images ──► feedforward │  YoNoSplat          │──► splats (PLY) ──► viewer / SPZ
                       │  SPFSplatV2         │
                       └─────────────────────┘
```

**Geometry models** predict cameras + point maps and can export a COLMAP
`sparse/` reconstruction. That is exactly what melkor's training wrappers
already consume, so they replace the SfM stage:

```bash
./vggt-infer --scene_dir /path/to/scene          # writes .../scene/sparse/
./scripts/opensplat_wrapper.sh /path/to/scene --images /path/to/scene/images -o out.ply
# or the full pipeline, skipping COLMAP:
./scripts/pipeline.sh /path/to/scene out --skip-colmap
```

**Direct-splat models** output 3D Gaussians in one pass; write them to PLY
for the viewer ([viewer/README.md](../viewer/README.md)) or convert to SPZ
with `melkor scene.ply scene.spz`.

## Catalog

| Model | Category | License (code / weights) | Notes |
|-------|----------|--------------------------|-------|
| **AMB3R** | geometry + SLAM/SfM | **none published** | UCL, CVPR'26 Highlight. The only surveyed model both newer than DA3 and reporting direct wins over it (self-reported protocol). Also does VO/SLAM. Research/eval only until a license ships. |
| **MapAnything** | universal geometry | Apache-2.0 / Apache-2.0 (`-apache`) or CC-BY-NC (default) | Meta + CMU, 3DV'26. Ingests images + optional intrinsics/poses/depth; >12 tasks. Use `facebook/map-anything-apache` weights for commercial. |
| **VGGT** | geometry + poses | custom / CC-BY-NC (gated commercial ckpt) | Meta, CVPR'25 Best Paper. **Cleanest COLMAP export** (`demo_colmap.py` → `sparse/`, optional `--use_ba`). Predates DA3 on accuracy but best tooling. |
| **Pi3 (π³)** | pose-free geometry | BSD-3 / CC-BY-NC | ICLR'26. Permutation-equivariant; order-robust. Pi3X (~1B) recommended. |
| **YoNoSplat** | direct feedforward 3DGS | **MIT / MIT** | ETH Zurich, ICLR'26. Posed or unposed, calibrated or not; ~100 views in 2.69 s. Released checkpoints are 224×224 (`re10k_224x224_ctx2to32.ckpt`, `dl3dv_224x224_ctx2to32.ckpt`); high-res "coming soon". Strongest permissive direct-splat option. |
| **SPFSplatV2** | direct feedforward 3DGS | **MIT / MIT** | Imperial College. Self-supervised, pose-free; MASt3R- and VGGT-based variants. |
| **MoGe-2** | single-image geometry | **MIT / MIT** | Microsoft, NeurIPS'25. Metric points + depth + normals + intrinsics from one image. Complements (does not replace) multi-view models. |

## Choosing a model

- **You need a drop-in SfM replacement feeding melkor's trainers** → VGGT
  (best COLMAP tooling; non-commercial weights) or MapAnything-apache
  (permissive). AMB3R if you can accept its unlicensed status for research.
- **You want splats directly, permissively licensed** → YoNoSplat, then
  SPFSplatV2.
- **You need the newest accuracy and report it beating DA3** → AMB3R
  (verify the benchmark protocol and its licensing yourself).
- **Single image → geometry** → MoGe-2.

## Caveats (from the underlying research)

- Every "beats DA3 / beats X" number is **self-reported in each paper's own
  evaluation protocol** and not independently audited; DA3's native-protocol
  numbers differ from the figures competitors quote against it. Treat the
  ranking as indicative, not settled.
- SPFSplatV2's headline RE10K PSNR (26.157) did not survive independent
  verification — the method and its MIT release are solid, that specific
  number is not.
- This is a fast-moving area (the survey window is mid-2025 to mid-2026);
  newer releases will exist. Re-run `setup_feedforward_sota.sh list` and
  check upstream for updates.

## Adding another model

Append one `name|repo|category|license_class|note` line to the `catalog()`
function in `scripts/setup_feedforward_sota.sh`, and add its entry point to
`create_wrapper()`. `license_class` is `permissive`, `noncommercial`, or
`unlicensed`; the gate and the printed license flow from there.
