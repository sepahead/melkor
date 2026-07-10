<div align="center">

<img src="assets/melkor-logo.svg" alt="Melkor splat forge — inside an ember-dark hexagon, a cloud of glowing anisotropic Gaussian splats reconstructs a wireframe peak." width="200" height="200" />

# Melkor

**A cross-platform 3D Gaussian Splatting toolkit — conversion, training pipelines, scene completion, and a web viewer.**

[![CI](https://github.com/sepahead/melkor/actions/workflows/ci.yml/badge.svg)](https://github.com/sepahead/melkor/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/sepahead/melkor)](https://github.com/sepahead/melkor/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Linux-lightgrey.svg)
![C++17](https://img.shields.io/badge/C%2B%2B-17-informational.svg)

[Quick Start](#quick-start) ·
[Features](#features) ·
[Usage](#usage) ·
[Architecture](#architecture) ·
[Documentation](#documentation) ·
[Contributing](#contributing)

</div>

---

Melkor turns meshes and photo sets into 3D Gaussian splat scenes and gives you the tools to refine, compress, complete, and view them. The native CLI covers GLB/PLY/SPZ conversion, densification-based scene completion, and GPU acceleration on Metal (macOS) and CUDA (Linux), with a behaviorally consistent CPU fallback. Around the core sit curated training pipelines (OpenSplat, gsplat, LichtFeld-Studio), a reviewed Depth Anything 3 bridge, and a SparkJS web viewer with a Tauri desktop shell.

## Features

**Core CLI**
- **Two honest conversion modes** — Basic (fast vertex-to-splat) and Enhanced (k-NN adaptive scale + surface alignment). Trained fitting and neural reconstruction use the dedicated OpenSplat/DA3 pipelines; the native facades were retired because they did not implement those contracts.
- **Deterministic inspection** — `melkor inspect INPUT [--json] [--strict]` validates metadata, counts, bounds, field provenance, and numeric hazards without initializing a GPU or changing the source ([docs/INSPECT.md](docs/INSPECT.md))
- **Format support** — GLB/glTF 2.0 → PLY/SPZ, PLY ↔ SPZ v1-v3 (SPZ compresses ~90% vs PLY, including spherical-harmonics data)
- **Scene completion** — densification-based hole filling, the 3DGS counterpart of inpainting: bridges occlusion voids and densifies sparse regions deterministically, with no learned prior ([docs/SCENE_COMPLETION.md](docs/SCENE_COMPLETION.md))

**Acceleration**
- **Unified compute backends** — one `ComputeProvider` interface dispatches to Metal, CUDA, or CPU at runtime; CPU/Metal behavior is locked by runtime parity tests, while CUDA is compile-gated in hosted CI and shares the same reference semantics
- **Grid-accelerated k-NN** — uniform-grid neighbor search kernels (Metal + CUDA, with an identical CPU reference) keep enhanced conversion and scene completion on the GPU for clouds of any size

**Pipelines & viewing**
- **Training** — OpenSplat (multi-GPU), gsplat (CUDA DDP), gsplat-mps (Apple Silicon), LichtFeld-Studio (Linux)
- **Feedforward** — a pinned DA3 / Depth Anything 3 bridge plus a dated, license-aware catalog of MapAnything, VGGT, Pi3, AMB3R, YoNoSplat, SPFSplatV2, and MoGe-2 integrations ([docs/FEEDFORWARD_SOTA.md](docs/FEEDFORWARD_SOTA.md))
- **Structure-from-Motion** — COLMAP and GLOMAP (10–100× faster mapping)
- **Web viewer** — SparkJS + THREE.js viewer for SPZ/SOG/SPLAT/PLY scenes with private offline local-file opening, drag-and-drop, camera feeds, auto-orbit, fly controls, and **progressive streaming** (renders splats as they download); ships with a Playwright render-test suite and an optional Tauri desktop build ([viewer/README.md](viewer/README.md))
- **Streaming** — progressive/LOD viewer loading, streamable formats (SPZ, SOG, `.RAD`), a **4D temporal player** for volumetric-video sequences (fed by 4D-GS per-frame PLY exports), and an installable online/streaming **3DGS reconstruction** stage (`setup_streaming.sh`: Gaussian-SLAM, SplaTAM, Splat-SLAM, 3DGStream → PLY → melkor); see [docs/STREAMING.md](docs/STREAMING.md)

## Quick Start

```bash
# Verify vendored third-party snapshots and build (Metal is automatic on macOS)
./scripts/setup_deps.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Verify the toolchain and see which GPU backend is active
./build/melkor --info

# Convert a mesh to splats
./build/melkor input.glb output.ply

# Validate before converting or publishing
./build/melkor inspect output.ply --json --strict
```

**Training from photos** (full pipeline: SfM → training → output):

```bash
./scripts/setup_all.sh
./scripts/train_from_images.sh ~/Photos/my_scene ~/output/my_scene
```

**Feedforward reconstruction** (no COLMAP, typically seconds to minutes):

```bash
./scripts/setup_da3.sh
./da3-infer --input images/ --output scene.ply
```

See [docs/QUICKSTART.md](docs/QUICKSTART.md) for the complete walkthrough.

## Usage

### Format conversion

```bash
./build/melkor model.glb output.ply                            # Basic (default)
./build/melkor model.glb output.ply --enhanced                 # Adaptive scale + surface alignment
./build/melkor scene.ply scene.spz                             # Compress to SPZ
./build/melkor scene.spz scene.ply                             # Decompress back to PLY
```

For trained fitting, use OpenSplat through `scripts/opensplat_wrapper.sh`. For
neural reconstruction, use `da3-infer`; the native `--fit` and
`--feedforward` switches deliberately fail closed.

### Validate or inventory an asset

```bash
./build/melkor inspect scene.ply
./build/melkor inspect scene.spz --json --strict
./build/melkor inspect model.glb --json
```

Exit `0` means no errors, exit `1` means invalid (or warnings under `--strict`),
and exit `2` means invalid command usage. See [docs/INSPECT.md](docs/INSPECT.md)
for the deterministic `melkor.inspect.v1` JSON contract and format limits.

### Scene completion (hole filling / densification)

```bash
# Fill occlusion holes and densify sparse regions of a trained scene
./build/melkor scene.spz completed.spz --fill-holes

# Denser fill, larger bridgeable holes
./build/melkor scene.ply completed.ply --fill-holes --fill-strength 0.8 --max-hole-size 12
```

Interior holes are bridged by an advancing front; the scene's outer boundary is never extended. Details, parameters, and the algorithm are documented in [docs/SCENE_COMPLETION.md](docs/SCENE_COMPLETION.md).

### Training

```bash
./scripts/train_from_images.sh /path/to/photos /path/to/output
./scripts/opensplat_wrapper.sh /path/to/colmap/project --gpu-ids 0,1,2,3 -o output.ply
./da3-infer --input images/ --output scene.ply
```

### Viewing

```bash
cd viewer
./fetch-assets.sh          # one-time: viewer libs + sample scenes
bun run serve              # http://127.0.0.1:8771
bun run test               # headless render tests
```

Use **Open local splat** or drop a PLY/SPZ/SPLAT/KSPLAT/SOG/ZIP file onto the
viewer. Bytes remain inside the browser/webview and work without a network.

## Architecture

```mermaid
flowchart LR
    subgraph Inputs
        P[Photos] --> SfM[COLMAP / GLOMAP]
        M[GLB / glTF mesh]
    end
    SfM --> T[Training<br/>OpenSplat · gsplat · LichtFeld]
    P --> FF[Feedforward<br/>DA3 · reviewed adapters]
    M --> C[melkor CLI<br/>basic · enhanced]
    T --> S[(Splat cloud)]
    FF --> S
    C --> S
    S --> SC[Scene completion<br/>--fill-holes]
    SC --> S
    S --> F[PLY / SPZ]
    F --> V[Web viewer<br/>SparkJS · Tauri]
```

The core library (`melkor_core`) is platform-independent; GPU work goes through the `ComputeProvider` interface, backed per platform by `melkor_metal`, `melkor_cuda`, or a CPU stub. Neighbor searches share a single host-built uniform grid, so the Metal, CUDA, and CPU paths walk identical cells and differ only by float rounding.

## GPU Backends

| Platform | Backend | Enable | Notes |
|----------|---------|--------|-------|
| macOS (Apple Silicon) | Metal | default | compute kernels + reference differentiable rasterizer API |
| Linux (NVIDIA) | CUDA | `-DMELKOR_USE_CUDA=ON` | SM 60–90; grid k-NN and cloud processing on-device |
| Any | CPU | automatic fallback | bit-consistent reference implementation |

`melkor --info` prints the active backend and device. `--no-gpu` forces the CPU path.

## Documentation

| Document | Contents |
|----------|----------|
| [docs/QUICKSTART.md](docs/QUICKSTART.md) | End-to-end setup and first conversion/training run |
| [docs/INSPECT.md](docs/INSPECT.md) | Deterministic asset validation and JSON automation contract |
| [docs/PIPELINE.md](docs/PIPELINE.md) | The `pipeline.sh` photos-to-splats orchestrator |
| [docs/SCENE_COMPLETION.md](docs/SCENE_COMPLETION.md) | Hole filling / densification: algorithm, parameters, limits |
| [docs/OPENSPLAT_WRAPPER.md](docs/OPENSPLAT_WRAPPER.md) | Multi-GPU OpenSplat training wrapper |
| [docs/GLOMAP_WRAPPER.md](docs/GLOMAP_WRAPPER.md) | GLOMAP structure-from-motion wrapper |
| [docs/GSPLAT_CUDA.md](docs/GSPLAT_CUDA.md) | gsplat with CUDA and distributed training |
| [docs/LICHTFELD_WRAPPER.md](docs/LICHTFELD_WRAPPER.md) | LichtFeld-Studio training wrapper |
| [docs/DA3_FEEDFORWARD.md](docs/DA3_FEEDFORWARD.md) | Depth Anything 3 feedforward reconstruction |
| [docs/FEEDFORWARD_SOTA.md](docs/FEEDFORWARD_SOTA.md) | Dated feedforward integration catalog (MapAnything, VGGT, YoNoSplat, …) |
| [docs/STREAMING.md](docs/STREAMING.md) | Streaming splats: progressive/LOD viewer loading, SOG format, SLAM |
| [docs/RELEASE.md](docs/RELEASE.md) | Reproducible source checks and signed-distribution release gates |
| [viewer/README.md](viewer/README.md) | SparkJS web viewer, Tauri shell, render tests |

## Testing

```bash
cd build && ctest --output-on-failure
```

The test suites cover format round trips and hostile input, deterministic inspection, release evidence, scene-graph transforms, compute-provider parity, scene completion, differentiable-renderer gradients, strict CLI parsing, and the DA3 extraction path. CI builds macOS (Metal), macOS with AddressSanitizer, Linux (CPU), and Linux CUDA compile-only targets; it also runs Python/shell checks, dependency review, Rust policy checks, and the viewer's Playwright suite.

When touching backend code, verify both configurations locally:

```bash
cmake -B build && cmake --build build -j && (cd build && ctest)            # Metal/CUDA
cmake -B build-cpu -DMELKOR_USE_METAL=OFF && cmake --build build-cpu -j \
  && (cd build-cpu && ctest)                                               # CPU/stub topology
```

## Project Structure

```
melkor/
├── include/melkor/    Public C++ headers (ComputeProvider, Densifier, converters, IO)
├── src/               Core library + CLI
│   ├── metal/         Metal backend (Objective-C++ hosts + .metal kernels)
│   └── cuda/          CUDA backend (host wrappers + .cu kernels)
├── tests/             Self-contained C++ test suites + Python tests
├── viewer/            SparkJS web viewer, Tauri shell, Playwright tests
├── scripts/           Setup, SfM, and training pipeline scripts
├── docs/              Per-component documentation
├── tools/da3/         DA3 feedforward inference
├── DA3coreml/         Depth Anything 3 CoreML port (ByteDance)
├── ml-sharp/          Apple ML research (vendored)
└── third_party/       Pinned vendored dependencies (tinygltf, stb, spz)
```

## Contributing

Contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for the development setup, backend-parity rules, and the pull-request checklist. Security issues should follow [SECURITY.md](SECURITY.md).

## License

MIT for the core Melkor code — see [LICENSE](LICENSE). Bundled third-party components keep their own licenses (Apache-2.0, AGPL-3.0, Apple Sample Code, MIT); see [NOTICE](NOTICE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Acknowledgments

- [3D Gaussian Splatting](https://github.com/graphdeco-inria/gaussian-splatting) — the original technique and reference implementation
- [OpenSplat](https://github.com/pierotofy/OpenSplat), [gsplat](https://github.com/nerfstudio-project/gsplat), [LichtFeld-Studio](https://github.com/MrNeRF/LichtFeld-Studio) — training backends
- [SPZ](https://scaniverse.com/news/spz-gaussian-splat-open-source-file-format) — compressed splat container by Niantic Scaniverse
- [Depth Anything 3](https://github.com/ByteDance-Seed/Depth-Anything-3) ([paper](https://arxiv.org/abs/2511.10647)) — feedforward reconstruction; the vendored ml-sharp research snapshot is quarantined pending an upstream refresh
- [COLMAP](https://colmap.github.io/) and [GLOMAP](https://github.com/colmap/glomap) — structure-from-motion
- [Spark](https://sparkjs.dev/) — WebGL Gaussian-splat renderer used by the viewer
