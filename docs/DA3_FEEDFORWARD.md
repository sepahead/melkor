# Depth Anything 3 integration

Melkor's DA3 bridge converts one image or one jointly inferred multi-view scene
to `ply:da3-gaussian-v1`. This document describes the tested integration as of
**2026-08-12**. The upstream project remains the authority for model architecture
and benchmark results.

The tested path is:

```text
images -> pinned official DA3 checkpoint -> learned Gaussians or camera-aware
depth unprojection -> PLY -> melkor -> SPZ -> viewer
```

The bridge intentionally fails closed. It does not:

- Invent camera poses
- Accept a mutable checkpoint revision
- Divide one scene between unrelated GPU processes
- Present an image-intensity preview as a reconstruction

## Requirements

- Linux with an NVIDIA GPU and a compatible CUDA driver
- Python 3.10 through 3.13 (3.11 or 3.12 recommended)
- Enough VRAM for the selected checkpoint and all views in the joint call
- Git, a Python venv, and network access for initial installation

The native C++ converter and viewer do not require DA3.

## Install

```bash
./scripts/setup_da3.sh --accept-unlocked-dependencies
```

The installer checks out official Depth Anything 3 commit
`41736238f5bced4debf3f2a12375d2466874866d`. It installs the `gs` extra. Then,
it downloads the selected Hugging Face snapshot at a reviewed immutable
revision.
Downloads are staged and checked for regular configuration and weight files.
The installer rejects symbolic links and special files.
It writes the revision marker before it moves the snapshot into place atomically.

The Python dependency set does not have a complete hash lock.
The required flag acknowledges this remaining supply-chain risk.
Use an isolated development environment.
Setup rejects an upstream checkout that contains modified or untracked files.

The 1.1 LARGE, GIANT, and NESTED checkpoints are gated because their model
cards declare CC-BY-NC-4.0 terms:

```bash
./scripts/setup_da3.sh \
  --accept-unlocked-dependencies \
  --accept-noncommercial
```

Review the model card before accepting. Melkor's MIT license does not replace
checkpoint terms.

The setup writes two wrappers at the repository root.
It does not replace a wrapper that has different content.

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

# Inspect after you replace both semantic placeholders with verified values
SOURCE_UNIT_TO_METER='<positive meters per DA3 unit>'
SOURCE_COLOR_SPACE='<srgb_rec709_display or lin_rec709_display>'
./build/dev/melkor inspect scene.ply \
  --input-profile ply:da3-gaussian-v1 \
  --source-unit-to-meter "$SOURCE_UNIT_TO_METER" \
  --source-color-space "$SOURCE_COLOR_SPACE"
cd viewer && bun run serve
```

The pinned model uses an OpenCV-style RDF camera frame for its normalized world.
The model does not guarantee a physical unit or one color transfer function.
Measure the unit scale and verify the color space before native conversion.

Input directory discovery is non-recursive and accepts JPEG, PNG, WebP, TIFF,
and BMP files. Lexicographic filename order becomes view order, so use
zero-padded names for video frames.

The bridge applies these input limits before model inference:

- 256 views
- 512 MiB for each compressed image file
- 8 GiB for all compressed image files
- 100 million pixels in each image
- 1 billion pixels in all images

The bridge copies each input to a private directory and verifies the copied container.
The model reads only these private copies.
Use DA3-Streaming when a sequence exceeds the view limit.

## Tested reconstruction checkpoints

| CLI name | Reconstruction path | Weight terms | Installer revision |
|---|---|---|---|
| `da3-small` | joint depth + camera pose -> point splats | Apache-2.0 | `e08cab65ca0ec38e7826075418411ab90cab4da3` |
| `da3-base` | joint depth + camera pose -> point splats | Apache-2.0 | `f4a6c9b3c95e41c82048423d3493a81ec3fa810e` |
| `da3-large-1.1` | refreshed depth + pose -> point splats | CC-BY-NC-4.0 | `0e109ae307c5982f319a67cf6f9f99ccdc0ec97c` |
| `da3-giant-1.1` | official learned Gaussian head | CC-BY-NC-4.0 | `72ee9f89ce4e50d704e9d55ee9c646ec8dc25a19` |
| `da3nested-giant-large-1.1` | learned Gaussians + metric alignment | CC-BY-NC-4.0 | `b2359bdf726fb44ef62acca04d629dcf158053e7` |

The MONO and METRIC single-view checkpoints output depth without the
multi-view camera data this bridge needs for a world-space splat scene. Use
upstream DA3's depth exporters for those checkpoints. The Melkor bridge rejects
them rather than fabricating geometry.

For SMALL, BASE, and LARGE, the output is a camera-aware colored point-splat
approximation derived from depth, intrinsics, extrinsics, confidence, and sky
masks. It is not equivalent to the learned 3DGS head. GIANT and NESTED preserve
the official predicted means, scale, rotation, degree 0-4 spherical
harmonics, and opacity.

## CLI reference

```text
--input, -i PATH                 image or directory (required)
--output, -o FILE                .ply, .npz, .json, or .glb (required)
--model, -m NAME                 checkpoint; default DA3-BASE
--model-dir DIR                  revision-marked local snapshots
--device {cuda,cpu}              execution device; default cuda
--scale FLOAT                    base point-splat scale; default 0.01
--subsample INT                  keep each Nth pixel in both axes
--confidence-percentile FLOAT    depth confidence cutoff; default 40
--min-depth FLOAT                minimum accepted camera-Z depth; default 0.1
--max-depth FLOAT                maximum accepted camera-Z depth; default 100
--fp32                           diagnostic/full-precision execution
--allow-fallback-depth           explicit preview-only intensity fallback
--allow-lossy-preview            permit GLB point-cloud output
--force                          replace an existing regular output file
```

Numeric arguments are validated for finite values and coherent ranges. `.spz`
is deliberately not accepted directly. Write PLY, then use the canonical native
encoder:

The adapter declares the DA3 profile and its RDF frame.
First verify the unit, color space, and antialiasing state.
The DA3 PLY header does not declare these values.

Use this command after you replace the semantic placeholders:

```bash
./build/dev/melkor convert scene.ply scene.spz \
  --input-profile ply:da3-gaussian-v1 \
  --source-unit-to-meter "$SOURCE_UNIT_TO_METER" \
  --source-color-space "$SOURCE_COLOR_SPACE" \
  --output-antialiased false \
  --allow-loss LOSS_COLOR_SPACE_METADATA_DROPPED \
  --allow-loss LOSS_COORDINATE_METADATA_DROPPED \
  --allow-loss LOSS_SH_DEGREE_TRUNCATED
```

The last approval applies when the learned Gaussian head produces degree-4 SH data.
SPZ v1 through v3 and SparkJS support at most degree 3.
The native converter reports the truncation before it writes the output.
The viewer cannot load the degree-4 DA3 PLY directly.

JSON is a bounded debugging preview.
NPZ and PLY preserve supported model-direct SH data.
GLB is a colored point-cloud visualization that drops Gaussian attributes.
It requires `--allow-lossy-preview`.
SPZ conversion occurs only through the native CLI.

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

The learned Gaussian path uses the upstream border trim.
It keeps valid far-depth Gaussians and complete SH data.
These choices avoid the upstream PLY exporter's lossy defaults.
The depth-derived path rejects invalid depth, sky, and low-confidence pixels.
`--subsample N` uses 2-D pixel-grid subsampling on both paths.
It retains approximately `1/N²` pixels.

## Multi-GPU and long sequences

Do not divide one scene's views among independent DA3 processes. DA3 estimates
poses and geometry jointly, so separately inferred subsets do not share a
coordinate frame. The former `da3-infer-multigpu` entry point now exits with an
explanation instead of concatenating invalid geometry.

Safe options are:

- assign each complete scene in a dataset to a separate GPU
- reduce image count/resolution for a single joint inference call, or
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

This output is not a reconstruction. Do not use it in training, evaluation, or
production pipelines.

## CoreML status

The CoreML port was an experimental single-image surface. It was removed from this
repository on 2026-07-14. See `docs/adapters/index.md`. The notes below describe
its behavior for historical reference.

The removed port has no commands in this repository.
Use the CUDA bridge or a reviewed external tool.

## Validation

The repository's synthetic DA3 tests cover:

- learned Gaussian extraction, border trimming, and far-depth preservation
- camera translation and camera-Z unprojection
- malformed-camera fail-closed behavior
- confidence filtering and consistent 2-D subsampling
- PLY field conventions for scale, opacity, rotation, and SH data through degree 4
- Rejection of non-finite values, invalid scales, invalid opacity, and zero quaternions

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
