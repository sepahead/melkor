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

## 3. Network streaming of dynamic / 4D splats

Real-time streaming of *moving* (4D) splats for volumetric video and remote
rendering is still research-stage — no permissively-licensed drop-in was
found. Track: bandwidth-adaptive 4D decomposition (PD-4DGS), hierarchical
latent streaming compression (HPC), progressive 3DGS coding (ProGS), and
thin-client interactive streaming over HTTP/3. melkor's static PLY/SPZ IO and
the viewer are a foundation, but 4D playback would need a new time-varying
container and player — out of scope for now, noted for future work.

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
- **Huge scene, limited GPU** → SOG + Spark's virtual paging; hierarchical
  LOD authoring is future work.
- **4D / volumetric video network streaming** → not yet a solved,
  permissively-licensed integration; tracked as future work.

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
