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

## 2. Online / incremental reconstruction (SLAM)

Reconstruct a scene *while* capturing a video/image stream, rather than
running SfM + training offline. These produce a splat cloud (PLY) that feeds
melkor's viewer and further training. Verified, wrappable options:

| Method | Repo | License | Input |
|--------|------|---------|-------|
| **Splat-SLAM** | google-research/Splat-SLAM | Apache-2.0 | RGB-only video, globally optimized |
| **Gaussian-SLAM** | VladimirYugay/Gaussian-SLAM | MIT | RGB-D |
| **SplaTAM** | spla-tam/SplaTAM | BSD-3-Clause | RGB-D, track & map |
| **MonoGS** | muskie82/MonoGS | research-only (custom) | monocular, CVPR'24 |
| **AMB3R** (SLAM mode) | HengyiWang/amb3r | none published | video → poses + points; see [FEEDFORWARD_SOTA.md](FEEDFORWARD_SOTA.md) |

Integration: run the SLAM method on your capture, export its Gaussian cloud
to PLY, then `melkor scene.ply scene.spz` for the viewer or feed the poses
into training. AMB3R's `slam/run.py` also exports a COLMAP-style
reconstruction that melkor's training path consumes directly.

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
  GPU-ready) and let the viewer stream it progressively.
- **Reconstruct live from video** → Splat-SLAM (Apache-2.0) or Gaussian-SLAM
  (MIT) → PLY → melkor.
- **Huge scene, limited GPU** → SOG + Spark's virtual paging; hierarchical
  LOD authoring is future work.
- **4D / volumetric video streaming** → not yet a solved, permissively-licensed
  integration; tracked as future work.

## Sources

- [Spark 2.0 — Streaming 3DGS worlds on the web (World Labs)](https://www.worldlabs.ai/blog/spark-2.0),
  [Spark new features 2.0](https://sparkjs.dev/docs/new-features-2.0/)
- [PlayCanvas: SOG, the WebP of Gaussian Splatting](https://blog.playcanvas.com/playcanvas-open-sources-sog-format-for-gaussian-splatting/),
  [SOG format spec](https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/sog/)
- SLAM: [Splat-SLAM](https://github.com/google-research/Splat-SLAM),
  [Gaussian-SLAM](https://github.com/VladimirYugay/Gaussian-SLAM),
  [SplaTAM](https://spla-tam.github.io/),
  [MonoGS](https://github.com/muskie82/MonoGS)
