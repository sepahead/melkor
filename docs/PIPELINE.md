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

- **Auto-detection**: Automatically selects best backend and tool for your platform
- **Multiple tools**: OpenSplat, LichtFeld-Studio, gsplat-mps
- **Cross-platform**: macOS (Metal), Linux (CUDA), CPU fallback
- **Format options**: PLY output, optional SPZ compression
- **Quality presets**: Fast, medium, high quality options
- **Advanced features**: Multi-GPU, custom image paths, memory optimization

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
1. Copies images to workspace
2. Converts HEIC/HEIF to JPEG (macOS)
3. Converts WebP to JPEG if needed
4. Validates minimum image count (3+, 20+ recommended)

**Supported formats:**
- JPG, JPEG, PNG (universal)
- HEIC, HEIF (Apple, auto-converted)
- WebP, TIFF, TIF, BMP, GIF

### Stage 2: COLMAP Reconstruction

**Output:** `sparse/0/cameras.bin`, `images.bin`, `points3D.bin`

Runs COLMAP automatic reconstruction to extract:
- Camera intrinsics (focal length, distortion)
- Camera extrinsics (position, orientation)
- Sparse point cloud (for initialization)

**GPU Acceleration:** On Linux with NVIDIA GPU, COLMAP automatically uses CUDA for SIFT feature extraction and matching, significantly speeding up reconstruction.

Skip with `--skip-colmap` if you have existing reconstruction.

### Stage 3: Gaussian Training

**Output:** PLY file containing trained Gaussians

Trains 3D Gaussian splats using selected tool:
- **OpenSplat**: Cross-platform, production-grade
- **LichtFeld-Studio**: Fastest, Linux CUDA only
- **gsplat-mps**: Flexible, macOS only

### Stage 4: Output Generation

**Output:** `point_cloud.ply` or `splat.ply`

Primary output is always PLY format.

### Stage 5: Format Conversion (Optional)

**Output:** `.spz` file (~90% smaller)

With `--format spz` or `--format both`, converts PLY to compressed SPZ.

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
| `--tool <name>` | `auto`, `opensplat`, `lichtfeld`, `gsplat` | `auto` | Training tool |

### Pipeline Control

| Option | Description |
|--------|-------------|
| `--skip-colmap` | Skip COLMAP/GLOMAP, use existing reconstruction |
| `--sfm <tool>` | SfM tool: `colmap` (default), `glomap` (10-100× faster) |
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
| `--dry-run` | Show what would be done |
| `--setup` | Run full setup |
| `--version` | Show version |
| `--help, -h` | Show help |

---

## Tool Selection

### Automatic Selection

With `--tool auto` (default), the pipeline selects:

| Platform | GPU | Selected Tool |
|----------|-----|---------------|
| Linux | NVIDIA CUDA 12.8+ | LichtFeld-Studio |
| Linux | NVIDIA CUDA < 12.8 | OpenSplat |
| Linux | No GPU | OpenSplat (CPU) |
| macOS | Apple Silicon | OpenSplat (Metal) |
| macOS | Intel | OpenSplat (CPU) |

### Tool Comparison

| Feature | OpenSplat | LichtFeld-Studio | gsplat-mps |
|---------|-----------|------------------|------------|
| **Platform** | All | Linux CUDA | macOS |
| **Speed** | ⚡⚡ | ⚡⚡⚡ | ⚡ |
| **Pose Optimization** | ❌ | ✅ | ❌ |
| **Multi-GPU** | ✅ (wrapper) | ❌ | ❌ |
| **Memory Opts** | ✅ | Limited | ✅ |
| **Custom Training** | Limited | Limited | ✅✅ |

### Manual Selection

```bash
# Force OpenSplat
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool opensplat

# Force LichtFeld-Studio (Linux CUDA only)
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool lichtfeld

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

**Note:** The pipeline does NOT output GLB format. GLB is only supported as an INPUT format for converting existing 3D meshes to Gaussian splats:

```bash
# Convert GLB mesh to Gaussian splats
./build/melkor input.glb output.ply
./build/melkor input.glb output.spz
```

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

# Or use wrapper with more options
./scripts/opensplat_wrapper.sh ~/project \
    --downscale 2 \
    --densify-grad 0.0005 \
    -o output.ply
```

### "Images not found"

**Solution:** Use `--images` to specify correct path:

```bash
./scripts/pipeline.sh ~/colmap_project ~/output/ \
    --skip-colmap \
    --images /actual/path/to/images
```

### "SPZ conversion failed"

**Solution:** Ensure Melkor is built:

```bash
cd build && cmake .. && make -j$(nproc)
```

### Training is Very Slow

**Check:**
1. GPU is being used: `nvidia-smi` or check output for "Metal" message
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
├── point_cloud.ply           # Trained Gaussians (PLY)
└── point_cloud.spz           # Compressed (if --format spz/both)
```

---

## See Also

- [Quick Start Guide](QUICKSTART.md) - Getting started
- [OpenSplat Wrapper](OPENSPLAT_WRAPPER.md) - Advanced OpenSplat options
- [LichtFeld Wrapper](LICHTFELD_WRAPPER.md) - LichtFeld-Studio options
