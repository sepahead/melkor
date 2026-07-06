# Melkor Pipeline Documentation

Complete reference for the `pipeline.sh` unified training pipeline.

## Table of Contents

- [Overview](#overview)
- [Quick Start](#quick-start)
- [Pipeline Stages](#pipeline-stages)
- [Command Reference](#command-reference)
- [Tool Selection](#tool-selection)
- [Output Formats](#output-formats)
- [Quality Presets](#quality-presets)
- [Advanced Options](#advanced-options)
- [Examples](#examples)
- [Troubleshooting](#troubleshooting)
- [Output Structure](#output-structure)
- [See Also](#see-also)

---

## Overview

The `pipeline.sh` script provides a unified interface for the complete Gaussian splatting workflow:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         MELKOR PIPELINE                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  PHOTOS ──▶ COLMAP ──▶ TRAINING ──▶ PLY ──▶ CONVERT ──▶ SPZ                │
│  (input)    (SfM)      (3DGS)      (raw)   (optional)   (compressed)       │
│                                                                             │
│  Stage 1:  Stage 2:    Stage 3:    Stage 4: Stage 5:                       │
│  Images    Camera      Train       Output   Compress                       │
│            poses       Gaussians   PLY      to SPZ                         │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Key Features

- **Auto-detection**: Automatically selects the best backend and tool for your platform
- **Multiple tools**: OpenSplat, LichtFeld-Studio, gsplat-cuda, gsplat-mps
- **Cross-platform**: macOS (Metal), Linux (CUDA), CPU fallback
- **Format options**: PLY output, optional SPZ compression
- **Quality presets**: Fast, medium, high
- **Advanced features**: Multi-GPU, GLOMAP SfM, custom image paths, memory optimization

---

## Quick Start

### Basic Usage

```bash
# From photos (full pipeline)
./scripts/pipeline.sh ~/Photos/my_scene ~/output/

# From existing COLMAP project (skip reconstruction)
./scripts/pipeline.sh ~/colmap_project ~/output/ --skip-colmap
```

### Common Options

```bash
# High quality with compressed output
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --quality high \
    --format both

# Fast preview
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --quality fast

# Specific tool
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --tool opensplat
```

---

## Pipeline Stages

### Stage 1: Image Preparation

**Input:** Directory of images (JPG, PNG, HEIC, WebP, TIFF, etc.)

The pipeline:
1. Copies images to the workspace
2. Converts HEIC/HEIF to JPEG (via `sips` on macOS, else ImageMagick or libheif)
3. Converts WebP to JPEG if ImageMagick is available
4. Validates the minimum image count (3+, 20+ recommended)

**Supported formats:**
- JPG, JPEG, PNG (universal)
- HEIC, HEIF (Apple, auto-converted)
- WebP, TIFF, TIF, BMP, GIF

### Stage 2: SfM Reconstruction (COLMAP or GLOMAP)

**Output:** `sparse/0/cameras.bin`, `images.bin`, `points3D.bin`

Runs COLMAP automatic reconstruction (or GLOMAP with `--sfm glomap`) to extract:
- Camera intrinsics (focal length, distortion)
- Camera extrinsics (position, orientation)
- Sparse point cloud (for initialization)

**GPU acceleration:** On Linux with an NVIDIA GPU, COLMAP automatically uses CUDA
for SIFT feature extraction and matching, significantly speeding up reconstruction.

Skip with `--skip-colmap` if you have an existing reconstruction.

### Stage 3: Gaussian Training

**Output:** PLY file containing trained Gaussians

Trains 3D Gaussian splats using the selected tool:
- **OpenSplat**: Cross-platform, production-grade
- **LichtFeld-Studio**: Fastest single-GPU, Linux CUDA only
- **gsplat-cuda**: Multi-GPU DDP, Linux CUDA only
- **gsplat-mps**: Flexible, macOS only

### Stage 4: Output Generation

**Output:** `point_cloud.ply` or `splat.ply`

Primary output is always PLY format.

### Stage 5: Format Conversion (Optional)

**Output:** `.spz` file (~90% smaller)

With `--format spz` or `--format both`, converts the PLY to compressed SPZ using
the Melkor CLI.

---

## Command Reference

### Syntax

```bash
./scripts/pipeline.sh <input> <output> [options]
```

### Arguments

| Argument | Description |
|----------|-------------|
| `<input>` | Path to images directory OR existing COLMAP project |
| `<output>` | Output directory for results |

### Backend Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--backend <type>` | `auto`, `cuda`, `metal`, `cpu` | `auto` | Compute backend |

**Auto-detection logic:**
- macOS Apple Silicon → `metal`
- macOS Intel → `cpu`
- Linux with NVIDIA → `cuda`
- Linux without NVIDIA → `cpu`

### Quality Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--quality <level>` | `fast`, `medium`, `high` | `medium` | Training quality preset |
| `--iterations <n>` | Integer | Auto | Override iteration count |

### Output Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--format <type>` | `ply`, `spz`, `both` | `ply` | Output format |

**Format details:**
- `ply`: Standard PLY only (largest, universal)
- `spz`: PLY + SPZ conversion (~90% smaller)
- `both`: Same as `spz`

### Tool Selection

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--tool <name>` | `auto`, `opensplat`, `gsplat`, `gsplat-cuda`, `lichtfeld` | `auto` | Training tool |

### Pipeline Control

| Option | Description |
|--------|-------------|
| `--skip-colmap` | Skip COLMAP/GLOMAP, use existing reconstruction |
| `--sfm <tool>` | SfM tool: `colmap` (default), `glomap` (10-100× faster mapping, see [GLOMAP_WRAPPER.md](GLOMAP_WRAPPER.md)) |
| `--colmap-quality <level>` | Feature extraction quality: `low`, `medium`, `high` |

### GPU & Memory Options

| Option | Description |
|--------|-------------|
| `--gpu-ids <ids>` | Comma-separated GPU IDs (e.g., `0,1,2`) |
| `--gpu-split <mode>` | Multi-GPU mode: `single`, `data-parallel`, `memory-split` |
| `--downscale <n>` | Downscale images by factor (1, 2, 4, 8) |
| `--images <path>` | Custom path to images |

### Other Options

| Option | Description |
|--------|-------------|
| `--verbose, -v` | Verbose output |
| `--dry-run` | Show configuration without executing |
| `--setup` | Install COLMAP and training tools, build Melkor |
| `--version` | Show version |
| `--help, -h` | Show help |

---

## Tool Selection

### Automatic Selection

With `--tool auto` (default), the pipeline picks the first match among the
tools that are actually installed:

| Condition | Selected Tool |
|-----------|---------------|
| Linux CUDA + `--gpu-ids` set, gsplat-cuda installed | gsplat-cuda (native DDP) |
| Linux CUDA, LichtFeld-Studio installed | LichtFeld-Studio |
| OpenSplat installed (any platform) | OpenSplat |
| gsplat-cuda installed | gsplat-cuda |
| gsplat-mps installed | gsplat-mps |

> **Note:** LichtFeld-Studio's installer requires CUDA 12.8+; on systems where it
> is not installed, the pipeline falls back to OpenSplat. If no tool is found,
> run `./scripts/pipeline.sh --setup`.

### Tool Comparison

| Feature | OpenSplat | LichtFeld-Studio | gsplat-cuda | gsplat-mps |
|---------|-----------|------------------|-------------|------------|
| **Platform** | All | Linux CUDA | Linux CUDA | macOS |
| **Speed** | ⚡⚡ | ⚡⚡⚡ | ⚡⚡⚡ | ⚡ |
| **Pose Optimization** | ❌ | ✅ | ❌ | ❌ |
| **Multi-GPU** | ✅ (wrapper) | ❌ | ✅ (native DDP) | ❌ |
| **Memory Opts** | ✅ | Limited | ✅ | ✅ |
| **Custom Training** | Limited | Limited | ✅ | ✅✅ |

### Manual Selection

```bash
# Force OpenSplat
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool opensplat

# Force LichtFeld-Studio (Linux CUDA only)
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool lichtfeld

# Force gsplat-cuda (Linux CUDA, multi-GPU)
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool gsplat-cuda

# Force gsplat-mps (macOS only)
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool gsplat
```

---

## Output Formats

### PLY (Default)

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ --format ply
```

- **Extension:** `.ply`
- **Size:** Large (100MB - 1GB+)
- **Compatibility:** Universal
- **Best for:** Editing, archival, further processing

### SPZ (Compressed)

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ --format spz
```

- **Extension:** `.spz`
- **Size:** ~10% of PLY
- **Compatibility:** Viewers, web apps
- **Best for:** Deployment, streaming, mobile

### Both Formats

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ --format both
```

Generates both PLY and SPZ files.

### Important: GLB is INPUT Only

The pipeline does NOT output GLB format. GLB is only supported as an INPUT
format for converting existing 3D meshes to Gaussian splats:

```bash
# Convert GLB mesh to Gaussian splats
./build/melkor input.glb output.ply
./build/melkor input.glb output.spz
```

### Post-Processing: Scene Completion

Trained scenes often have occlusion holes and sparse regions. The Melkor CLI can
fill them after the pipeline finishes:

```bash
./build/melkor ~/output/point_cloud.ply filled.ply --fill-holes
```

Parameters (`--fill-iterations`, `--fill-strength`, `--max-hole-size`) and the
algorithm are documented in [SCENE_COMPLETION.md](SCENE_COMPLETION.md).

---

## Quality Presets

### Fast

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ --quality fast
```

- **Iterations:** 7,000
- **Time:** ~5-10 minutes
- **Use for:** Quick preview, testing capture quality

### Medium (Default)

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ --quality medium
```

- **Iterations:** 15,000
- **Time:** ~15-30 minutes
- **Use for:** Standard quality, most use cases

### High

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ --quality high
```

- **Iterations:** 30,000
- **Time:** ~30-60 minutes
- **Use for:** Production, final output

### Custom Iterations

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ --iterations 50000
```

---

## Advanced Options

### Custom Image Path

When images are in a different location than COLMAP expects:

```bash
./scripts/pipeline.sh ~/colmap_project ~/output/ \
    --skip-colmap \
    --images ~/actual/images
```

### Multi-GPU Training

```bash
# Distribute across GPUs
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --gpu-ids 0,1,2,3 \
    --gpu-split data-parallel
```

With `--gpu-ids` set, the pipeline prefers gsplat-cuda (native DDP) when
installed, otherwise routes through the OpenSplat wrapper. See
[GSPLAT_CUDA.md](GSPLAT_CUDA.md) and [OPENSPLAT_WRAPPER.md](OPENSPLAT_WRAPPER.md).

### Memory Optimization

```bash
# Reduce memory usage
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --downscale 2
```

### Verbose Output

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ --verbose
```

### Dry Run

```bash
# Show configuration without executing
./scripts/pipeline.sh ~/Photos/scene ~/output/ --dry-run
```

---

## Examples

### 1. Basic Usage

```bash
# Simplest usage - auto-detects everything
./scripts/pipeline.sh ~/Photos/my_object ~/output/my_object
```

### 2. High Quality with Compression

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --quality high \
    --format both \
    --tool auto
```

### 3. Existing COLMAP Project

```bash
./scripts/pipeline.sh ~/existing_colmap_project ~/output/ \
    --skip-colmap \
    --quality high
```

### 4. Linux CUDA Maximum Performance

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --backend cuda \
    --tool lichtfeld \
    --quality high
```

### 4b. Faster SfM with GLOMAP

```bash
# Use GLOMAP for 10-100× faster reconstruction
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --sfm glomap \
    --quality high
```

### 5. macOS Apple Silicon

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --backend metal \
    --tool opensplat \
    --format both
```

### 6. Multi-GPU Training

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --gpu-ids 0,1,2,3 \
    --gpu-split data-parallel \
    --quality high
```

### 7. Memory-Constrained Environment

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --downscale 2 \
    --quality medium
```

### 8. Custom Image Path with Specific Tool

```bash
./scripts/pipeline.sh ~/colmap_project ~/output/ \
    --skip-colmap \
    --images ~/original_photos \
    --tool opensplat \
    --iterations 30000
```

---

## Troubleshooting

### "COLMAP reconstruction failed"

**Symptoms:** No `sparse/0/cameras.bin` created

**Solutions:**
1. Add more images (minimum 20-30 recommended)
2. Ensure 60-80% overlap between consecutive images
3. Avoid blurry or dark images
4. Try lower COLMAP quality: `--colmap-quality low`

### "No training tool found"

**Solution:** Run setup first:

```bash
./scripts/pipeline.sh --setup
```

### "LichtFeld-Studio requires Linux"

**Solution:** On macOS, LichtFeld-Studio is not available. Use:

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool opensplat
```

### "CUDA out of memory"

**Solutions:**

```bash
# Downscale images
./scripts/pipeline.sh ~/Photos/scene ~/output/ --downscale 2

# Or use the wrapper with more options
./scripts/opensplat_wrapper.sh ~/project \
    --downscale 2 \
    --densify-grad 0.0005 \
    -o output.ply
```

### "Images not found"

**Solution:** Use `--images` to specify the correct path:

```bash
./scripts/pipeline.sh ~/colmap_project ~/output/ \
    --skip-colmap \
    --images /actual/path/to/images
```

### "SPZ conversion failed"

**Solution:** Ensure the Melkor CLI is built:

```bash
./scripts/setup_deps.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Training is Very Slow

**Check:**
1. GPU is being used: `nvidia-smi` (Linux) or check the output for a "Metal" message
2. Use faster quality: `--quality fast`
3. Downscale large images: `--downscale 2`

---

## Output Structure

After running the pipeline:

```
output/
├── workspace/                 # COLMAP workspace
│   ├── images/               # Copied/converted images
│   ├── sparse/
│   │   └── 0/
│   │       ├── cameras.bin   # Camera intrinsics
│   │       ├── images.bin    # Camera poses
│   │       └── points3D.bin  # Sparse points
│   └── database.db           # COLMAP database
├── point_cloud.ply           # Trained Gaussians (PLY; gsplat-mps emits splat.ply)
└── point_cloud.spz           # Compressed (if --format spz/both)
```

To inspect the result locally, use the bundled SparkJS viewer
(`cd viewer && ./fetch-assets.sh && bun run serve` — see
[viewer/README.md](../viewer/README.md)), or drag the PLY into
[SuperSplat](https://playcanvas.com/supersplat).

---

## See Also

- [Quick Start Guide](QUICKSTART.md) - Getting started
- [OpenSplat Wrapper](OPENSPLAT_WRAPPER.md) - Advanced OpenSplat options
- [LichtFeld Wrapper](LICHTFELD_WRAPPER.md) - LichtFeld-Studio options
- [GLOMAP Wrapper](GLOMAP_WRAPPER.md) - Faster global SfM (`--sfm glomap`)
- [gsplat CUDA](GSPLAT_CUDA.md) - Multi-GPU distributed training
- [DA3 Feedforward](DA3_FEEDFORWARD.md) - COLMAP-free reconstruction
- [Scene Completion](SCENE_COMPLETION.md) - Hole filling for trained scenes
- [Viewer](../viewer/README.md) - SparkJS web viewer and Tauri shell
