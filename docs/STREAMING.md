# Streaming Gaussian Splats

"Streaming" covers four distinct problems in the Gaussian-splatting world.
This page maps each to what melkor already does, what it just gained, and
which open-source pieces plug in. Findings below come from a verified
multi-source survey (repo URLs and licenses were checked directly).

## 1. Progressive / streamable loading + LOD (viewer)

The web viewer ([viewer/README.md](../viewer/README.md)) renders with
**Spark 2.1**, which is built for streaming:

- **Progressive display** (implemented in `viewer/index.html`): on the first
  load, the viewer adds `SplatMesh` before the download finishes. Spark
  parses the download incrementally and renders splats as they arrive. The
  viewer shows a progress card instead of a blank screen. During a scene
  switch, the current scene stays visible until the new scene is ready. Thus,
  two scenes do not overlap. The `first load streams progressively`
  Playwright test locks this behavior.
- **Spark 2.x streaming primitives** already in the vendored runtime:
  - `ReadableStream` loading for large URL and drag-and-drop assets
  - The `.RAD` progressive-refinement format
  - A continuous **level-of-detail** system
  - **Virtual splat paging** with a fixed GPU-memory budget

  These primitives support future LOD and out-of-core work. See §4.

**Streamable asset formats** the viewer can serve:

| Format | Size vs PLY | Notes |
|--------|-------------|-------|
| **SPZ** | ~10% (≈90% smaller) | melkor produces it: `melkor scene.ply scene.spz`. Compact, includes SH. |
| **SOG** (Spatially Ordered Gaussians) | ~5–7% (15–20×) | "The WebP of 3DGS": a `meta.json` + several `.webp` grids, Morton-ordered so it is GPU-ready with no load-time processing. The viewer already loads it (the `sutro` scene is SOG). |
| **.RAD** | streaming/LOD | Spark 2.x progressive format with refinement. Ideal for very large scenes. |

To produce SOG for the smallest streamable assets, use PlayCanvas'
open-source tools (both permissive):

- **SuperSplat** editor (playcanvas/supersplat, MIT) — exports SOG from PLY.
- **sogs** (playcanvas/sogs, Apache-2.0) — Python compressor:
  `PLY → meta.json + *.webp`.

Drop the resulting `meta.json`/`.webp` set (or a `.zip` of it) into
`viewer/public/splats/` and add a `SCENES` entry.

## 2. Online / incremental reconstruction (SLAM + streaming FVV)

Build the splat scene *incrementally* — from an RGB-D/RGB SLAM sequence, or
per-frame for free-viewpoint video — instead of offline SfM + training. Their
3DGS PLY output feeds melkor's viewer / SPZ. Install with:

```bash
./scripts/setup_streaming.sh list
./scripts/setup_streaming.sh permissive               # MIT/BSD/Apache
./scripts/setup_streaming.sh 3dgstream --accept-noncommercial
```

> **Reality check (verified against each repo's README):** all are **Linux +
> NVIDIA CUDA only** (custom CUDA rasterizers, tiny-cuda-nn, lietorch), and
> **none takes a plain folder of images** — each needs a *calibrated* dataset
> in a specific SLAM/video format selected via a per-scene config. Their
> conda/CUDA-submodule environments are tool-specific and cannot be installed
> generically, so `setup_streaming.sh` clones + scaffolds each repo, prints
> its license and the exact run command, and defers the env build to the
> repo's README. It does not fake a one-command install.

| Method | Run (verified) | Input | PLY out → melkor | License |
|--------|----------------|-------|------------------|---------|
| **Gaussian-SLAM** | `run_slam.py configs/<ds>/<scene>.yaml --input_path … --output_path …` | Replica/TUM/ScanNet(++), RGB-D | **direct** (`save_ply` writes INRIA 3DGS PLY) | **MIT** |
| **SplaTAM** | `scripts/splatam.py configs/<ds>/splatam.py` then `scripts/export_ply.py` | Replica/TUM/ScanNet(++), RGB-D | via `export_ply.py` | **BSD-3** |
| **Splat-SLAM** | `run.py configs/<ds>/<scene>.yaml` | Replica/TUM/ScanNet, RGB | non-default `save_gaussians()` | Apache-2.0 (repo archived) |
| **3DGStream** | `train_frames.py --config_path … -m <frame0> -v <scene>` | N3DV/Meet-Room multi-view video + COLMAP + frame-0 3DGS | frame-0 PLY only. Later frames are NTC deltas. | Inria **non-commercial** (submodule) |
| **MonoGS** | `slam.py --config configs/{mono,rgbd}/<ds>/<scene>.yaml` | TUM/Replica/EuRoC | GUI/metrics. PLY not documented. | non-commercial (Imperial) |
| **AMB3R** (SLAM mode) | `slam/run.py --data_path …` | video → poses + points | point cloud. See [FEEDFORWARD_SOTA.md](FEEDFORWARD_SOTA.md). | none published |

**Best fits for Melkor:** Gaussian-SLAM emits a standard 3DGS PLY and uses the
MIT license. SplaTAM exports PLY through `export_ply.py` and uses BSD-3.
`melkor scene.ply scene.spz` converts either output into a viewer asset.

3DGStream is a streaming free-viewpoint-video method. Its per-frame output is
an NTC-deformation and added-Gaussian delta, not a PLY. It inherits Inria's
non-commercial license.

## 3. Dynamic / 4D splats — temporal playback (implemented)

Volumetric video is a *sequence* of splat frames over time. A verified survey
of SOTA 4D methods found that **no web splat renderer (Spark, three.js,
PlayCanvas) plays temporal sequences natively** — that player was the gap.
melkor's viewer now has one.

**The drop-in producer** is [4D-GS](https://github.com/hustvl/4DGaussians)
(hustvl/4DGaussians, Apache-2.0). Its `export_perframe_3DGS.py` writes one
**standard 3DGS-layout PLY per timestamp**, such as `time_00000.ply` and
`time_00001.ply`.
Melkor's existing PLY and SPZ input consumes this output without translation.
(License caveat: 4D-GS transitively depends on Inria's
non-commercial `diff-gaussian-rasterization`, so commercial use is
constrained despite the Apache-2.0 top level.)

Install it (and the other verified 4D reconstruction methods) via the `4d`
group of the streaming installer:

```bash
./scripts/setup_streaming.sh list                    # 4d-gs, videogs, gifstream, ...
./scripts/setup_streaming.sh 4d-gs --accept-noncommercial
# train a scene, then export the per-frame PLY sequence:
./4d-gs-4d python train.py -s data/dnerf/bouncingballs --configs arguments/dnerf/bouncingballs.py --expname dnerf/bouncingballs
./4d-gs-4d python export_perframe_3DGS.py --iteration 20000 --configs arguments/dnerf/bouncingballs.py --model_path output/dnerf/bouncingballs
# -> output/dnerf/bouncingballs/gaussian_pertimestamp/time_*.ply
```

Of the surveyed 4D methods, **only 4D-GS produces a standard per-frame PLY
export** that feeds `pack-4d.js` cleanly. VideoGS (V3) leaves packable
per-frame checkpoints. GIFStream, Spacetime-GS, and Ex4DGS use custom
formats instead of PLY. The installer catalogs all five with their real
train/export commands and license class, and gates the non-commercial ones.

**The viewer temporal player:** a 4D scene contains the frame sequence and a
`manifest.json` (`{ "fps": 12, "frames": [...] }`). The player **streams a
bounded window** around the playhead. It keeps frames `[active-2, active+6]`
resident and shows one frame. It also prefetches future frames and evicts
frames outside the window.

Memory use is **O(window), not O(sequence length)**. Thus, a long volumetric
video stays within a fixed budget. If the next frame is not ready, playback
briefly stops instead of dropping the frame.
Seeking loads the target frame on demand. It advances on a play/pause + scrub
timeline at the manifest fps, exposed for automation as
`__viewer.play4D/pause4D/seek4D/get4DState` (the latter reports `buffered`,
the resident window size). Try it: `node viewer/make-4d-demo.js` generates a
synthetic sequence, then pick **Wave · 4D** in the viewer.

**The 4D format producer** (`viewer/pack-4d.js`) closes the pipeline —
reconstruct (4D-GS) → **pack + compress** (melkor) → stream (viewer):

```bash
# a 4D-GS export dir of time_*.ply -> a compressed, streamable 4D scene
node viewer/pack-4d.js /path/to/4dgs_export --spz --fps 24 \
     --out viewer/public/splats/4d/myscene
```

It sorts the per-frame files numerically. It can compress each PLY frame to
SPZ with the Melkor binary. A 4D sequence contains a complete cloud in each
frame, so compression reduces its transfer size. The tool writes
`manifest.json` and prints the required viewer `SCENES` entry. The
temporal player streams the SPZ frames identically to PLY (the demo ships
both **Wave · 4D** and **Wave · 4D (SPZ)**, the latter 94% smaller).

Other 4D methods need converters:

- 3DGStream uses MIT top-level code and inherited non-commercial Inria
  components. It stores a keyframe PLY and per-frame NTC deltas.
- V3/VideoGS uses MIT code and packs frames into hardware-codec 2D video.
- 4DGCPro uses a layered H.264 codec. GIFStream uses Apache-2.0 code and a
  dedicated codec. They need a new container path because they do not use PLY or SPZ.

### Real-time network streaming

Bandwidth-adaptive 4D streaming (PD-4DGS, HPC, ProGS, thin-client HTTP/3) and
remote rendering remain research-stage with no permissive PLY/SPZ drop-in —
tracked as future work.

## 4. Out-of-core / memory streaming

Scenes too large for GPU/RAM. Spark 2.x's **virtual splat paging** already
gives the viewer a fixed-memory path for arbitrarily large worlds when fed a
`.RAD`/LOD asset (§1). On the authoring side, the "Hierarchical 3DGS for
real-time rendering of large scenes" line of work builds an LOD tree offline.
A future Melkor converter can produce a hierarchical LOD asset from a large
PLY. Today, compress the asset to SOG to reduce the working set. Then, use
Spark's paging for display.

## Summary: what to reach for

- **Serve a big scene to the web fast** → convert to SOG (15–20× smaller,
  GPU-ready) and let the viewer stream it progressively (already implemented).
- **Reconstruct incrementally from an RGB-D/RGB sequence** →
  `setup_streaming.sh permissive`: Gaussian-SLAM (MIT, direct PLY) or SplaTAM
  (BSD-3) → PLY → melkor. Needs a calibrated dataset config, not raw images.
- **Streaming free-viewpoint video** → 3DGStream (per-frame 3DGS, NTC deltas,
  non-commercial via Inria submodules).
- **Play a 4D / volumetric-video sequence** → 4D-GS `export_perframe_3DGS.py`
  → per-frame PLY + `manifest.json` → the viewer's temporal player (§3).
- **Huge scene, limited GPU** → SOG + Spark's virtual paging. Hierarchical
  LOD authoring is future work.
- **Play back a 4D / volumetric-video sequence** → the viewer's temporal
  player, fed by a 4D-GS per-frame PLY export + `manifest.json` (§3).
- **4D *network* streaming** (bandwidth-adaptive, remote render) → not yet a
solved, permissively-licensed integration. This work is tracked for the future.

## Sources

- [Spark 2.0 — Streaming 3DGS worlds on the web (World Labs)](https://www.worldlabs.ai/blog/spark-2.0),
  [Spark new features 2.0](https://sparkjs.dev/docs/new-features-2.0/)
- [PlayCanvas: SOG, the WebP of Gaussian Splatting](https://blog.playcanvas.com/playcanvas-open-sources-sog-format-for-gaussian-splatting/),
  [SOG format spec](https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/sog/)
- SLAM / streaming reconstruction: [Splat-SLAM](https://github.com/google-research/Splat-SLAM),
  [Gaussian-SLAM](https://github.com/VladimirYugay/Gaussian-SLAM),
  [SplaTAM](https://spla-tam.github.io/),
  [MonoGS](https://github.com/muskie82/MonoGS),
  [3DGStream](https://github.com/SJoJoK/3DGStream)
- 4D / dynamic: [4D-GS](https://github.com/hustvl/4DGaussians),
  [Spacetime Gaussians](https://github.com/oppo-us-research/SpacetimeGaussians),
  [V3/VideoGS](https://github.com/AuthorityWang/VideoGS),
  [GIFStream](https://github.com/XDimLab/GIFStream),
  [4DGCPro](https://github.com/MediaX-SJTU/4DGCPro)
