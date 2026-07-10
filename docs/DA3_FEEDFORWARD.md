# Depth Anything 3 integration

Melkor's DA3 bridge converts one image or one jointly inferred multi-view scene
to a standard 3DGS-layout PLY. This document describes the tested integration
as of **2026-07-10**; the upstream project remains the authority for model
architecture and benchmark results.

The supported path is:

```text
images -> pinned official DA3 checkpoint -> learned Gaussians or camera-aware
depth unprojection -> PLY -> melkor -> SPZ -> viewer
```

The bridge is intentionally fail-closed. It does not silently invent camera
poses, accept a mutable checkpoint revision, shard the views of one scene
across unrelated GPU processes, or present an image-intensity preview as a
reconstruction.

## Requirements

- Linux with an NVIDIA GPU and a compatible CUDA driver
- Python 3.10 through 3.13 (3.11 or 3.12 recommended)
- Enough VRAM for the selected checkpoint and all views in the joint call
- Git, a Python venv, and network access for initial installation

The native C++ converter and viewer do not require DA3.

## Install

```bash
./scripts/setup_da3.sh
```

The installer checks out official Depth Anything 3 commit
`41736238f5bced4debf3f2a12375d2466874866d`, installs its `gs` extra, and
downloads the chosen Hugging Face snapshot at a reviewed immutable revision.
Downloads are staged, checked for configuration and weight files, marked with
their revision, and moved into place atomically.

The 1.1 LARGE, GIANT, and NESTED checkpoints are gated because their model
cards declare CC-BY-NC-4.0 terms:

```bash
./scripts/setup_da3.sh --accept-noncommercial
```

Review the model card before accepting. Melkor's MIT license does not replace
checkpoint terms.

The setup writes two wrappers at the repository root:

- `./da3-infer` — run the reconstruction bridge in its pinned venv
- `./da3-python` — run arbitrary Python in that venv

## Quick start

```bash
# One image or a directory containing one scene
./da3-infer --input images/ --output scene.ply

# Higher-quality learned Gaussian head (non-commercial checkpoint)
./da3-infer \
  --model da3-giant-1.1 \
  --input images/ \
  --output scene.ply

# Compress and inspect
./build/melkor scene.ply scene.spz
cd viewer && bun run serve
```

Input directory discovery is non-recursive and accepts JPEG, PNG, WebP, TIFF,
and BMP files. Lexicographic filename order becomes view order, so use
zero-padded names for video frames.

## Supported reconstruction checkpoints

| CLI name | Reconstruction path | Weight terms | Installer revision |
|---|---|---|---|
| `da3-small` | joint depth + camera pose -> point splats | Apache-2.0 | `e08cab65ca0ec38e7826075418411ab90cab4da3` |
| `da3-base` | joint depth + camera pose -> point splats | Apache-2.0 | `f4a6c9b3c95e41c82048423d3493a81ec3fa810e` |
| `da3-large-1.1` | refreshed depth + pose -> point splats | CC-BY-NC-4.0 | `0e109ae307c5982f319a67cf6f9f99ccdc0ec97c` |
| `da3-giant-1.1` | official learned Gaussian head | CC-BY-NC-4.0 | `72ee9f89ce4e50d704e9d55ee9c646ec8dc25a19` |
| `da3nested-giant-large-1.1` | learned Gaussians + metric alignment | CC-BY-NC-4.0 | `b2359bdf726fb44ef62acca04d629dcf158053e7` |

The MONO and METRIC single-view checkpoints output depth without the
multi-view camera data this bridge needs for a world-space splat scene. Use
upstream DA3's depth exporters for those checkpoints; the Melkor bridge rejects
them rather than fabricating geometry.

For SMALL, BASE, and LARGE, the output is a camera-aware colored point-splat
approximation derived from depth, intrinsics, extrinsics, confidence, and sky
masks. It is not equivalent to the learned 3DGS head. GIANT and NESTED preserve
the official predicted means, scale, rotation, DC spherical harmonics, and
opacity.

## CLI reference

```text
--input, -i PATH                 image or directory (required)
--output, -o FILE                .ply, .npz, .json, or .glb (required)
--model, -m NAME                 checkpoint; default DA3-BASE
--model-dir DIR                  verified local snapshots
--device {cuda,cpu}              execution device; default cuda
--scale FLOAT                    base point-splat scale; default 0.01
--subsample INT                  keep each Nth pixel in both axes
--confidence-percentile FLOAT    depth confidence cutoff; default 40
--min-depth FLOAT                minimum accepted camera-Z depth; default 0.1
--max-depth FLOAT                maximum accepted camera-Z depth; default 100
--fp32                           diagnostic/full-precision execution
--allow-fallback-depth           explicit preview-only intensity fallback
--verbose, -v                    extra diagnostics
```

Numeric arguments are validated for finite values and coherent ranges. `.spz`
is deliberately not accepted directly; write PLY, then use the canonical native
encoder:

```bash
./build/melkor scene.ply scene.spz
```

JSON is a bounded debugging preview, NPZ preserves arrays for Python analysis,
and GLB is a colored point-cloud visualization rather than a Gaussian-splat
container. PLY/SPZ are the supported reconstruction interchange formats.

## Geometry and filtering contract

DA3 reports camera-Z depth. For a pixel `(u, v)`, the bridge computes the
unnormalized camera vector `K^-1 [u, v, 1]`, rotates it into world space, and
evaluates:

```text
world_point = camera_origin + camera_z_depth * world_depth_vector
```

Normalizing that vector would incorrectly treat camera-Z depth as Euclidean ray
distance. Missing, malformed, singular, or non-finite camera matrices abort the
reconstruction unless the user explicitly selected preview fallback.

The learned Gaussian path follows the official exporter by trimming image
borders and removing each view's farthest 10% of depths. The depth-derived path
also rejects invalid depth, sky, and low-confidence pixels. `--subsample N`
uses 2-D pixel-grid subsampling on both paths, retaining roughly `1/N²` pixels.

## Multi-GPU and long sequences

Do not divide one scene's views among independent DA3 processes. DA3 estimates
poses and geometry jointly, so separately inferred subsets do not share a
coordinate frame. The former `da3-infer-multigpu` entry point now exits with an
explanation instead of concatenating invalid geometry.

Safe options are:

- assign each complete scene in a dataset to a separate GPU;
- reduce image count/resolution for a single joint inference call; or
- use the official DA3-Streaming project for a long sequence.

## Preview fallback

If the model cannot load, the normal behavior is failure. For UI plumbing or
file-format smoke tests only, an explicit flag permits a deterministic
intensity-derived pseudo-depth preview:

```bash
./da3-infer \
  --allow-fallback-depth \
  --input image.jpg \
  --output preview.ply
```

This output is not a reconstruction and should not enter training, evaluation,
or production pipelines.

## CoreML status

`DA3coreml/coreml/` is an experimental single-image port. Its inference and
benchmark commands remain available for research, but its multi-view fusion,
streaming, conversion, and Gaussian-export commands are disabled until they
match official DA3 semantics end to end. It is not the supported replacement
for the CUDA bridge.

## Validation

The repository's synthetic DA3 tests cover:

- learned Gaussian extraction and official border/depth pruning;
- camera translation and camera-Z unprojection;
- malformed-camera fail-closed behavior;
- confidence filtering and consistent 2-D subsampling; and
- PLY field conventions for scale, opacity, rotation, and SH DC color.

Run them without a checkpoint download:

```bash
python tests/test_gaussians_from_prediction.py
```

For an installed environment, also run a small real scene and verify the PLY in
the viewer before processing a large dataset.

## Upstream references

- [Depth Anything 3 repository](https://github.com/ByteDance-Seed/Depth-Anything-3)
- [Depth Anything 3 model collection](https://huggingface.co/depth-anything)
- [DA3-Streaming](https://github.com/ByteDance-Seed/Depth-Anything-3/tree/main/da3_streaming)

Checkpoint licenses and interfaces can change independently of Melkor. Re-run
the installer only after reviewing and updating both the pinned source commit
and the per-checkpoint revision table.
