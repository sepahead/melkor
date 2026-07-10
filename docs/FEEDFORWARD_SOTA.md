# Curated Feedforward Reconstruction Catalog

Melkor's feedforward stage turns images into geometry or splats in a single
forward pass — no COLMAP/GLOMAP, no per-scene optimization. Depth-Anything-3
([docs/DA3_FEEDFORWARD.md](DA3_FEEDFORWARD.md)) was the first integration;
`scripts/setup_feedforward_sota.sh` provides a reviewed snapshot of useful
alternatives as of **2026-07-10**. This is an integration catalog, not a claim
that every listed model is the current state of the art on every benchmark.

Install one, a group, or list the catalog:

```bash
./scripts/setup_feedforward_sota.sh list
./scripts/setup_feedforward_sota.sh permissive            # MIT/Apache only
./scripts/setup_feedforward_sota.sh vggt --accept-noncommercial
./scripts/setup_feedforward_sota.sh amb3r --accept-unlicensed
```

Each install checks out a reviewed commit into `tools/<name>/repo`, builds a
venv, and drops a `./<name>-infer` wrapper at the project root.

> These models need **Linux + NVIDIA CUDA** and download multi-GB weights.
> They do not run on the macOS/Metal build. Melkor itself is MIT; the setup
> script refuses known non-permissive or unspecified weights unless you pass
> the matching `--accept-*` flag. It prints the pinned repository's code
> license at install time; checkpoint terms are a separate artifact and remain
> gated when their model card is non-commercial or unspecified.

## Three integration shapes

Entry points and outputs below are verified against each repo's README/docs.

```
                        ┌── COLMAP export ─────┐
 images ──► feedforward │  VGGT · MapAnything  │──► COLMAP sparse/ ──► pipeline.sh --skip-colmap
                        └──────────────────────┘        (OpenSplat / gsplat / LichtFeld)
                        ┌── PLY point cloud ───┐
 images ──► feedforward │  Pi3 · AMB3R         │──► PLY points ──► viewer / SPZ
                        └──────────────────────┘
                        ┌── dataset eval ──────┐
 dataset+index ──────►  │  YoNoSplat · SPFSplat│──► novel views / splats (research/eval)
                        └──────────────────────┘
```

**COLMAP-export models** (VGGT `demo_colmap.py`, MapAnything
`scripts/demo_colmap.py`) predict cameras + point maps and write a COLMAP
`sparse/` reconstruction — exactly what melkor's training wrappers consume,
so they replace the SfM stage:

```bash
# VGGT: put images in SCENE/images/, then
./vggt-infer --scene_dir=/path/to/scene           # writes /path/to/scene/sparse/
# MapAnything (add --apache for the Apache-2.0 weights):
./mapanything-infer --images_dir=/path/to/images --output_dir=/path/to/scene --apache
# then train, skipping COLMAP:
./scripts/pipeline.sh /path/to/scene out --skip-colmap
```

**PLY-point-cloud models** (Pi3 `example_mm.py`, AMB3R `sfm/run.py`) write a
colored point cloud you can view or compress:

```bash
./pi3-infer --data_path /path/to/images --save_path scene.ply
./build/melkor scene.ply scene.spz            # for the viewer
```

**Dataset-eval models** (YoNoSplat, SPFSplatV2) are **not** folder-of-images
tools despite outputting splats: they run via Hydra (`python -m src.main
+experiment=… mode=test`) over a pixelSplat-convention dataset plus a JSON
view-sampler index, for novel-view-synthesis evaluation. Use them for
research/benchmarking against RE10K/ACID/DL3DV, not in-the-wild capture. The
`./yonosplat-infer` / `./spfsplatv2-infer` wrappers pass your Hydra args
straight through; see each repo's `EVALUATION.md` / `DATASETS.md`.

## Catalog

| Model | Output → melkor | License (code / weights) | Notes |
|-------|-----------------|--------------------------|-------|
| **VGGT** | COLMAP `sparse/` | custom / CC-BY-NC (gated commercial ckpt) | Meta, CVPR'25 Best Paper. `demo_colmap.py --scene_dir=DIR` (images in `DIR/images/`), optional `--use_ba`. **Cleanest COLMAP export.** |
| **MapAnything** | COLMAP `sparse/` (or GLB) | Apache-2.0 / Apache-2.0 (`-apache`) or CC-BY-NC (default) | Meta + CMU, 3DV'26. `scripts/demo_colmap.py --images_dir --output_dir`; add `--apache` for commercial weights. Ingests images + optional intrinsics/poses/depth. |
| **AMB3R** | PLY point cloud | **none published** | UCL, CVPR'26 Highlight. Only surveyed model both newer than DA3 and reporting wins over it (self-reported). `demo.py`, `sfm/run.py`, `slam/run.py` (VO/SLAM). Research/eval only until a license ships. |
| **Pi3 (π³)** | PLY point cloud | BSD-3 / CC-BY-NC | ICLR'26. `example_mm.py --data_path --save_path out.ply`. Permutation-equivariant; Pi3X (~1B) recommended. |
| **MoGe-2** | per-image points/depth/normals | **MIT / MIT** | Microsoft, NeurIPS'25. `moge infer -i IMAGES -o OUT --ply`. Single-image; complements multi-view models. |
| **YoNoSplat** | novel views (dataset eval) | **MIT / MIT** | ETH Zurich, ICLR'26. Hydra `python -m src.main +experiment=… mode=test` over a pixelSplat dataset + index — **not** a folder-of-images tool. 224×224 checkpoints released. |
| **SPFSplatV2** | novel views (dataset eval) | **MIT / unspecified** | Imperial College. The repository code is MIT, but the published Hugging Face checkpoint has no declared model-card license; installation is gated as unlicensed. |

## Newer systems tracked but not yet integrated

- **[VGGT-Omega](https://github.com/facebookresearch/vggt-omega)** (Meta, May
  2026) extends VGGT to much longer sequences and is
  the most important successor missing from the executable catalog. Its
  integration should wait for a pinned checkpoint/license review and a tested
  COLMAP or Gaussian export adapter.
- **[Déjà View](https://github.com/nv-tlabs/dvlt)** (NVIDIA Research) is also
  tracked for long-horizon visual
  geometry. It is not presented as a drop-in Melkor backend until its public
  interface, checkpoint terms, and output-frame conventions have an end-to-end
  adapter test.

## Choosing a model

- **Drop-in SfM replacement feeding melkor's trainers** → VGGT (best COLMAP
  tooling; non-commercial weights) or MapAnything-`apache` (permissive,
  commercial). AMB3R if you accept its unlicensed status for research.
- **A quick colored point cloud from photos** → Pi3 (PLY) or AMB3R.
- **Newest accuracy that reports beating DA3** → AMB3R (verify the benchmark
  protocol and its licensing yourself).
- **Single image → geometry** → MoGe-2.
- **Benchmarking novel-view synthesis on RE10K/ACID/DL3DV** → YoNoSplat or
  SPFSplatV2 (dataset-driven; not for in-the-wild capture).

## Caveats (from the underlying research)

- Every "beats DA3 / beats X" number is **self-reported in each paper's own
  evaluation protocol** and not independently audited; DA3's native-protocol
  numbers differ from the figures competitors quote against it. Treat the
  ranking as indicative, not settled.
- SPFSplatV2 reports 26.157 PSNR with evaluation-time pose alignment; Melkor
  has not independently reproduced that result.
- This is a fast-moving area;
  newer releases will exist. Re-run `setup_feedforward_sota.sh list` and
  check upstream for updates.

## Adding another model

Append one `name|repo|category|license_class|note` line to the `catalog()`
function in `scripts/setup_feedforward_sota.sh`, and add its entry point to
`create_wrapper()`. `license_class` is `permissive`, `noncommercial`, or
`unlicensed`; the gate and the printed license flow from there.
