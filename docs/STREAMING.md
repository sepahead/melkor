# Streaming Gaussian Splats

"Streaming" covers four distinct problems in the Gaussian-splatting world.
This page maps each to what melkor already does, what it just gained, and
which open-source pieces plug in. Findings below come from a verified
multi-source survey (repo URLs and licenses were checked directly).

## 1. Progressive / streamable loading + LOD (viewer)

The web viewer ([viewer/README.md](../viewer/README.md)) renders with
**Spark 2.1**, which is built for streaming:

- **Progressive display** (implemented in `viewer/index.html`): on the first
  load — when nothing is on screen yet — the `SplatMesh` is added to the
  scene *before* the download finishes, so Spark renders splats as they
  stream in (Spark parses the download incrementally) instead of showing a
  blank screen until 100%. The dim/blur overlay drops to a floating progress
  card so the build-up is visible. A scene *switch* keeps the current scene
  until the new one is ready, so two different scenes never overlap
  mid-stream. Locked by the `first load streams progressively` Playwright
  test.
- **Spark 2.x streaming primitives** already in the vendored runtime:
  `ReadableStream` loading (multi-GB assets from a URL or drag-and-drop), the
  `.RAD` progressive-refinement format, a continuous **level-of-detail**
  system, and a **virtual splat paging** system that renders arbitrarily
  large worlds within a fixed GPU-memory budget. These are available to build
  on for LOD/out-of-core work (see §4).

**Streamable asset formats** the viewer can serve:

| Format | Size vs PLY | Notes |
|--------|-------------|-------|
| **SPZ** | ~10% (≈90% smaller) | melkor produces it: `melkor scene.ply scene.spz`. Compact, includes SH. |
| **SOG** (Spatially Ordered Gaussians) | ~5–7% (15–20×) | "The WebP of 3DGS": a `meta.json` + several `.webp` grids, Morton-ordered so it is GPU-ready with no load-time processing. The viewer already loads it (the `sutro` scene is SOG). |
| **.RAD** | streaming/LOD | Spark 2.x progressive format with refinement; ideal for very large scenes. |

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
> conda/CUDA-submodule environments are tool-specific and can't be installed
> generically, so `setup_streaming.sh` clones + scaffolds each repo, prints
> its license and the exact run command, and defers the env build to the
> repo's README. It does not fake a one-command install.

| Method | Run (verified) | Input | PLY out → melkor | License |
|--------|----------------|-------|------------------|---------|
| **Gaussian-SLAM** | `run_slam.py configs/<ds>/<scene>.yaml --input_path … --output_path …` | Replica/TUM/ScanNet(++), RGB-D | **direct** (`save_ply` writes INRIA 3DGS PLY) | **MIT** |
| **SplaTAM** | `scripts/splatam.py configs/<ds>/splatam.py` then `scripts/export_ply.py` | Replica/TUM/ScanNet(++), RGB-D | via `export_ply.py` | **BSD-3** |
| **Splat-SLAM** | `run.py configs/<ds>/<scene>.yaml` | Replica/TUM/ScanNet, RGB | non-default `save_gaussians()` | Apache-2.0 (repo archived) |
| **3DGStream** | `train_frames.py --config_path … -m <frame0> -v <scene>` | N3DV/Meet-Room multi-view video + COLMAP + frame-0 3DGS | frame-0 PLY only; later frames are NTC deltas | Inria **non-commercial** (submodule) |
| **MonoGS** | `slam.py --config configs/{mono,rgbd}/<ds>/<scene>.yaml` | TUM/Replica/EuRoC | GUI/metrics; PLY not documented | non-commercial (Imperial) |
| **AMB3R** (SLAM mode) | `slam/run.py --data_path …` | video → poses + points | point cloud; see [FEEDFORWARD_SOTA.md](FEEDFORWARD_SOTA.md) | none published |

**Best fits for melkor:** Gaussian-SLAM (MIT, emits a standard 3DGS PLY
directly) and SplaTAM (BSD-3, PLY via `export_ply.py`) — both permissive and
produce a PLY that `melkor scene.ply scene.spz` turns into a viewer asset.
3DGStream is the canonical *streaming free-viewpoint-video* method but its
per-frame output is a compact NTC-deformation + added-Gaussian delta, not a
per-frame PLY, and it inherits Inria's non-commercial license.

## 3. Dynamic / 4D splats — temporal playback (implemented)

Volumetric video is a *sequence* of splat frames over time. A verified survey
of SOTA 4D methods found that **no web splat renderer (Spark, three.js,
PlayCanvas) plays temporal sequences natively** — that player was the gap.
melkor's viewer now has one.

**The drop-in producer** is [4D-GS](https://github.com/hustvl/4DGaussians)
(hustvl/4DGaussians, Apache-2.0): its `export_perframe_3DGS.py` writes one
**standard 3DGS-layout PLY per timestamp** (`time_00000.ply`,
`time_00001.ply`, …) — melkor's existing PLY/SPZ IO consumes these with zero
translation. (License caveat: 4D-GS transitively depends on Inria's
non-commercial `diff-gaussian-rasterization`, so commercial use is
constrained despite the Apache-2.0 top level.)

**The viewer temporal player**: a 4D scene is that per-frame sequence plus a
`manifest.json` (`{ "fps": 12, "frames": [...] }`). The player **streams a
bounded window** of frames around the playhead — it keeps only frames
`[active-2, active+6]` resident (in the scene, one visible), prefetches ahead
as it plays, and evicts frames that fall outside the window. Memory is
therefore **O(window), not O(sequence length)**, so arbitrarily long
volumetric video plays within a fixed budget; if the next frame isn't
buffered yet, playback briefly stalls (buffering) rather than dropping it.
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

It sorts the per-frame files numerically, optionally compresses each frame
PLY → **SPZ via the melkor binary (~90–94% smaller)** — 4D sequences are N
frames × a full cloud each, so this is what makes them streamable — writes
the `manifest.json`, and prints the viewer `SCENES` entry to add. The
temporal player streams the SPZ frames identically to PLY (the demo ships
both **Wave · 4D** and **Wave · 4D (SPZ)**, the latter 94% smaller).

Other 4D methods need converters, not drop-in: 3DGStream (MIT) uses a
keyframe PLY + per-frame NTC deltas; V3/VideoGS (MIT) packs frames into a
hardware-codec 2D video; dedicated streaming codecs (4DGCPro H.264 layered,
GIFStream Apache-2.0) don't use PLY/SPZ and would need a new container path.

### Real-time network streaming

Bandwidth-adaptive 4D streaming (PD-4DGS, HPC, ProGS, thin-client HTTP/3) and
remote rendering remain research-stage with no permissive PLY/SPZ drop-in —
tracked as future work.

## 4. Out-of-core / memory streaming

Scenes too large for GPU/RAM. Spark 2.x's **virtual splat paging** already
gives the viewer a fixed-memory path for arbitrarily large worlds when fed a
`.RAD`/LOD asset (§1). On the authoring side, the "Hierarchical 3DGS for
real-time rendering of large scenes" line of work builds an LOD tree offline.
A future melkor addition would be a converter that emits a hierarchical/LOD
asset from a large PLY; today the practical path is: compress to SOG (§1) to
shrink the working set, and rely on Spark's paging for display.

## Summary: what to reach for

- **Serve a big scene to the web fast** → convert to SOG (15–20× smaller,
  GPU-ready) and let the viewer stream it progressively (already implemented).
- **Reconstruct incrementally from an RGB-D/RGB sequence** →
  `setup_streaming.sh permissive`: Gaussian-SLAM (MIT, direct PLY) or SplaTAM
  (BSD-3) → PLY → melkor. Needs a calibrated dataset config, not raw images.
- **Streaming free-viewpoint video** → 3DGStream (per-frame 3DGS; NTC deltas;
  non-commercial via Inria submodules).
- **Play a 4D / volumetric-video sequence** → 4D-GS `export_perframe_3DGS.py`
  → per-frame PLY + `manifest.json` → the viewer's temporal player (§3).
- **Huge scene, limited GPU** → SOG + Spark's virtual paging; hierarchical
  LOD authoring is future work.
- **Play back a 4D / volumetric-video sequence** → the viewer's temporal
  player, fed by a 4D-GS per-frame PLY export + `manifest.json` (§3).
- **4D *network* streaming** (bandwidth-adaptive, remote render) → not yet a
  solved, permissively-licensed integration; tracked as future work.

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
