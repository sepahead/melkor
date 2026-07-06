# Melkor Quick Start Guide

Complete guide to 3D Gaussian Splatting with Melkor: from photos to PLY or SPZ output.

## Table of Contents

1. [Overview](#overview)
2. [Installation](#installation)
3. [Complete Pipeline: Photos → 3D](#complete-pipeline-photos--3d)
4. [Output Formats](#output-formats)
5. [Tool Selection](#tool-selection)
6. [Common Workflows](#common-workflows)
7. [Troubleshooting](#troubleshooting)
8. [Preprocessing & Postprocessing](#preprocessing--postprocessing)
9. [Quick Reference](#quick-reference)
10. [Next Steps](#next-steps)

---

## Overview

### What is Melkor?

Melkor is a unified toolkit for 3D Gaussian Splatting that provides:

- **Multiple training backends**: OpenSplat, LichtFeld-Studio, gsplat (CUDA and MPS), DA3
- **Two approaches**: COLMAP-based (traditional) and COLMAP-free (feedforward)
- **Format conversion**: GLB → PLY → SPZ (GLB is input only)
- **Scene completion**: densification-based hole filling via `--fill-holes` — see [SCENE_COMPLETION.md](SCENE_COMPLETION.md)
- **Bundled viewer**: SparkJS web viewer with an optional Tauri desktop shell — see [viewer/README.md](../viewer/README.md)
- **Cross-platform GPU support**: Metal (macOS), CUDA (Linux), CPU fallback; `melkor --info` prints the active backend

### Two Approaches: COLMAP vs COLMAP-Free

#### COLMAP-Based Pipeline (Traditional - Best Quality)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    COLMAP-BASED PIPELINE (Traditional 3DGS)                 │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  PHOTOS ──▶ COLMAP ──▶ TRAINING ──▶ PLY ──▶ CONVERT ──▶ SPZ               │
│  (50+)      (SfM)      (3DGS)      (raw)   (optional)   (web/app)          │
│             SIFT       7K-50K                                               │
│             features   iterations                                           │
│                                                                             │
│  Time: Minutes to hours | Best for: Maximum quality, many images           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### COLMAP-Free Pipeline (DA3 Feedforward - Fastest)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    COLMAP-FREE PIPELINE (DA3 Feedforward)                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  PHOTOS ──▶ DA3 Neural Network ──▶ Depth + Rays ──▶ 3D Gaussians ──▶ PLY  │
│  (1-10)     (DINOv2 backbone)      (per pixel)     (fused)                 │
│                                                                             │
│  • Single forward pass (no iterative optimization)                         │
│  • No camera pose estimation needed                                        │
│  • Works with 1-10 images                                                  │
│  • Poses estimated internally by the model                                 │
│                                                                             │
│  Time: Seconds | Best for: Quick results, few images, no poses available   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Which Approach Should I Use?

| Scenario | Recommended | Why |
|----------|-------------|-----|
| **Few images (1-10)** | COLMAP-Free (DA3) | COLMAP may fail with sparse images |
| **Many images (50+)** | COLMAP-Based | Higher quality reconstruction |
| **No camera poses** | COLMAP-Free (DA3) | DA3 estimates poses internally |
| **Existing COLMAP project** | COLMAP-Based | Use existing reconstruction |
| **Maximum quality** | COLMAP-Based | Iterative optimization is superior |
| **Quick preview** | COLMAP-Free (DA3) | Results in seconds |
| **Textureless surfaces** | COLMAP-Free (DA3) | SIFT features may fail |
| **Reflective surfaces** | COLMAP-Free (DA3) | SIFT features may fail |
| **Batch processing** | COLMAP-Free (DA3) | Fast per-scene processing |

---

## Installation

### Prerequisites

| Platform | Requirements |
|----------|-------------|
| **macOS** | Xcode CLI tools, Homebrew, Python 3.10+ |
| **Linux** | GCC 11+, CMake, NVIDIA drivers (for GPU) |
| **Both** | COLMAP (camera pose estimation) |

### Quick Install

```bash
# Clone Melkor
git clone https://github.com/sepahead/melkor.git
cd melkor

# Build the Melkor CLI (fetches pinned third-party deps, then builds)
./scripts/setup_deps.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Verify the build and see which GPU backend is active (Metal/CUDA/CPU)
./build/melkor --info

# Install the training tools (auto-detects platform; installs COLMAP too)
./scripts/setup_all.sh
```

> **Note:** `setup_all.sh` installs COLMAP automatically (Homebrew on macOS, apt on
> Linux). If that step fails, install it manually: `brew install colmap` or
> `sudo apt-get install colmap`.

### What Gets Installed

| Tool | Platform | Setup script | Description |
|------|----------|--------------|-------------|
| **Melkor CLI** | All | `setup_deps.sh` + CMake build | Format conversion, scene completion |
| **COLMAP** | All | `setup_all.sh` | Structure-from-motion |
| **OpenSplat** | All | `setup_all.sh` / `setup_opensplat.sh` | Cross-platform, production-grade |
| **gsplat-mps** | macOS | `setup_all.sh` / `setup_gsplat_mps.sh` | Research, flexible |
| **LichtFeld-Studio** | Linux CUDA | `setup_all.sh` / `setup_lichtfeld.sh` | Fastest, pose optimization |
| **gsplat-cuda** (optional) | Linux CUDA | `setup_gsplat_cuda.sh` | Multi-GPU DDP training |
| **GLOMAP** (optional) | All | `setup_glomap.sh` | 10-100× faster SfM mapping |
| **DA3** (optional) | Linux CUDA | `setup_da3.sh` | COLMAP-free feedforward |

---

## Complete Pipeline: Photos → 3D

### Option 1: COLMAP-Free with DA3 (Fastest - Seconds)

No COLMAP, no camera poses, just images → 3D Gaussians
(full reference: [DA3_FEEDFORWARD.md](DA3_FEEDFORWARD.md)):

```bash
# 1. Install DA3 (Linux CUDA only)
./scripts/setup_da3.sh

# 2. Single image to 3DGS
./da3-infer --input photo.jpg --output scene.ply

# 3. Multiple images (no COLMAP required)
./da3-infer --input ~/Photos/my_scene/ --output scene.ply

# 4. Multi-GPU for faster processing
./da3-infer-multigpu --input ~/Photos/my_scene/ --output scene.ply

# 5. With specific model variant
./da3-infer --model DA3-LARGE --input images/ --output high_quality.ply

# 6. Compress to SPZ
./build/melkor scene.ply scene.spz
```

**DA3 Model Variants:**

| Model | Size | Speed | Use Case |
|-------|------|-------|----------|
| `DA3-SMALL` | ~1GB | ⚡⚡⚡ | Quick preview |
| `DA3-BASE` | ~2GB | ⚡⚡ | Balanced (default) |
| `DA3-LARGE` | ~4GB | ⚡ | High quality |
| `DA3-GIANT` | ~8GB | 🐢 | Maximum quality |
| `DA3METRIC-LARGE` | ~4GB | ⚡⚡ | Metric depth (meters) |

### Option 2: COLMAP-Based Pipeline (Best Quality - Minutes)

Traditional approach with camera pose estimation
(full option reference: [PIPELINE.md](PIPELINE.md)):

```bash
# Basic usage
./scripts/pipeline.sh ~/Photos/my_scene ~/output/my_scene

# With options
./scripts/pipeline.sh ~/Photos/my_scene ~/output/my_scene \
    --quality high \
    --format both \
    --tool auto

# Minimal alternative: COLMAP + OpenSplat/gsplat-mps, fewer options
# (--tool opensplat|gsplat, --quality fast|high; default: high)
./scripts/train_from_images.sh ~/Photos/my_scene ~/output/my_scene
```

### Option 3: Step-by-Step (Manual Control)

#### Step 1: Prepare Images

Requirements for good results:
- **Minimum**: 20-30 images (more is better)
- **Overlap**: 60-80% overlap between consecutive images
- **Coverage**: Capture from multiple angles
- **Quality**: Avoid blur, ensure good lighting

```bash
# Check your images
ls ~/Photos/my_scene/*.{jpg,jpeg,png,JPG,JPEG,PNG} | wc -l
```

#### Step 2: Run COLMAP (Camera Pose Estimation)

```bash
mkdir -p ~/output/my_scene

# COLMAP automatic reconstruction
# On Linux with an NVIDIA GPU, append:
#   --SiftExtraction.use_gpu 1 --SiftMatching.use_gpu 1
colmap automatic_reconstructor \
    --workspace_path ~/output/my_scene \
    --image_path ~/Photos/my_scene \
    --quality high \
    --single_camera 1

# Verify output
ls ~/output/my_scene/sparse/0/
# Should see: cameras.bin, images.bin, points3D.bin
```

> **Note:** The pipeline scripts automatically detect NVIDIA GPUs and enable CUDA
> acceleration for COLMAP. For much faster SfM on large image sets, see
> [GLOMAP_WRAPPER.md](GLOMAP_WRAPPER.md).

#### Step 3: Train Gaussian Splat

**Choose your tool:**

```bash
# OpenSplat (cross-platform, recommended)
./opensplat ~/output/my_scene -n 30000 -o ~/output/my_scene/splat.ply

# LichtFeld-Studio (Linux CUDA, fastest)
./lichtfeld -d ~/output/my_scene -o ~/output/my_scene/

# gsplat-mps (macOS, research)
source tools/gsplat-mps/venv/bin/activate
cd tools/gsplat-mps
python examples/simple_trainer.py \
    --data_dir ~/output/my_scene \
    --result_dir ~/output/my_scene \
    --max_steps 30000
```

#### Step 4: Convert to Desired Format

```bash
# PLY → SPZ (compressed, ~90% smaller)
./build/melkor ~/output/my_scene/splat.ply ~/output/my_scene/splat.spz

# GLB → PLY (convert a 3D mesh to Gaussian splats)
./build/melkor model.glb output.ply

# GLB → SPZ (convert mesh directly to compressed format)
./build/melkor model.glb output.spz

# Optional: fill holes / densify sparse regions before converting
./build/melkor ~/output/my_scene/splat.ply filled.ply --fill-holes
```

> **Note:** GLB is only supported as an INPUT format (for converting 3D meshes to
> Gaussian splats). PLY and SPZ are the supported OUTPUT formats.
> Run `./build/melkor --help` for all conversion modes (`--basic`, `--enhanced`,
> `--fit`, `--feedforward`) and options. `--no-gpu` forces the CPU path
> (`--no-metal` is a deprecated alias). Hole-filling parameters are documented in
> [SCENE_COMPLETION.md](SCENE_COMPLETION.md).

---

## Output Formats

### Format Comparison

| Format | Extension | Size | Direction | Best For |
|--------|-----------|------|-----------|----------|
| **PLY** | `.ply` | Large | Input/Output | Editing, archival, universal |
| **SPZ** | `.spz` | Small (~10%) | Input/Output | Web, mobile, streaming |
| **GLB** | `.glb` | Varies | Input only | Converting 3D meshes to splats |

### Choosing Output Format

```bash
# Pipeline with format selection
./scripts/pipeline.sh ~/Photos/scene ~/output/ --format ply    # PLY only (default)
./scripts/pipeline.sh ~/Photos/scene ~/output/ --format spz    # PLY + SPZ conversion
./scripts/pipeline.sh ~/Photos/scene ~/output/ --format both   # PLY + SPZ

# Manual conversion after training
./build/melkor input.ply output.spz   # PLY → SPZ (compress)
./build/melkor input.spz output.ply   # SPZ → PLY (decompress)
./build/melkor input.glb output.ply   # GLB → PLY (mesh to splats)
./build/melkor input.glb output.spz   # GLB → SPZ (mesh to compressed splats)
```

### Format Details

#### PLY (Polygon File Format)
- **Pros**: Universal, editable, lossless
- **Cons**: Large files (100MB - 1GB+)
- **Use when**: Editing in SuperSplat, archival, further processing

#### SPZ (Compressed Gaussian Splat)
- **Pros**: ~90% smaller, fast loading
- **Cons**: Lossy compression
- **Use when**: Web deployment, mobile apps, streaming

#### GLB (GL Binary) - INPUT ONLY
- **Pros**: Standard 3D mesh format, converts meshes to Gaussian splats
- **Cons**: Input only, not a Gaussian splat output format
- **Use when**: Converting existing 3D models to Gaussian splats

---

## Tool Selection

### Training Tools

| Tool | Speed | Quality | Platform | COLMAP Required | Features |
|------|-------|---------|----------|-----------------|----------|
| **DA3 (Feedforward)** | ⚡⚡⚡⚡ | Good | Linux CUDA | ❌ No | Multi-GPU, any # images |
| **LichtFeld-Studio** | ⚡⚡⚡ | High | Linux CUDA 12.8+ | ✅ Yes | Pose optimization, MCMC |
| **OpenSplat** | ⚡⚡ | High | All | ✅ Yes | Cross-platform, reliable |
| **gsplat-mps** | ⚡ | Highest | macOS Metal | ✅ Yes | Research, customizable |
| **gsplat-cuda** | ⚡⚡ | Highest | Linux CUDA | ✅ Yes | Multi-GPU DDP, 4× less memory |

### Automatic Selection

```bash
# Let Melkor choose the best tool for your platform
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool auto

# Linux + CUDA: LichtFeld-Studio (or gsplat-cuda when --gpu-ids is set)
# Linux without CUDA: OpenSplat (CPU)
# macOS: OpenSplat (Metal on Apple Silicon)
# Selection falls back to whichever tools are installed
```

### Manual Selection

```bash
# Force specific tool
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool opensplat
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool lichtfeld    # Linux CUDA only
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool gsplat-cuda  # Linux CUDA, multi-GPU
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool gsplat       # macOS only
```

---

## Common Workflows

### 1. Quick Preview (5-10 minutes)

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --quality fast \
    --format ply

# Uses 7,000 iterations, lower resolution
# Good for checking if your images work
```

### 2. Production Quality (20-60 minutes)

```bash
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --quality high \
    --format both \
    --tool auto

# Uses 30,000+ iterations
# Outputs both PLY and SPZ
```

### 3. Maximum Quality with LichtFeld (Linux)

```bash
./scripts/lichtfeld_wrapper.sh ~/colmap_project \
    --pose-opt mlp \
    -o ~/output/

# Includes pose optimization to fix camera errors
# Uses MCMC densification for better Gaussian placement
```

### 4. Custom Image Path

When your images are in a different location than COLMAP expects:

```bash
# OpenSplat
./scripts/opensplat_wrapper.sh ~/colmap_project \
    --images ~/actual/image/path \
    -o output.ply

# LichtFeld-Studio
./scripts/lichtfeld_wrapper.sh ~/colmap_project \
    --images ~/actual/image/path \
    -o output/
```

### 5. Out of Memory (OOM) Fix

```bash
# Reduce memory usage
./scripts/opensplat_wrapper.sh ~/project \
    --downscale 2 \
    --densify-grad 0.0005 \
    --stop-densify 10000 \
    -o output.ply

# Or use pipeline with downscaling
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --downscale 2
```

### 6. Multi-GPU Training

```bash
# Distribute across multiple GPUs (OpenSplat wrapper)
./scripts/opensplat_wrapper.sh ~/project \
    --gpu-ids 0,1,2,3 \
    --split data-parallel \
    -o output.ply

# Or via the pipeline (prefers gsplat-cuda when installed)
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --gpu-ids 0,1,2,3 \
    --gpu-split data-parallel
```

### 7. Using Existing COLMAP Project

```bash
# Skip COLMAP, just train
./scripts/pipeline.sh ~/existing_colmap_project ~/output/ \
    --skip-colmap \
    --format both
```

### 8. Convert Existing Files

```bash
# GLB from another source → PLY
./build/melkor model.glb output.ply

# Compress PLY for web
./build/melkor large_scene.ply compressed.spz

# Batch conversion
for f in *.ply; do
    ./build/melkor "$f" "${f%.ply}.spz"
done
```

---

## Troubleshooting

### "COLMAP reconstruction failed"

**Symptoms**: No `sparse/0/cameras.bin` after COLMAP

**Solutions**:
1. Add more images (minimum 20-30)
2. Ensure 60-80% overlap between images
3. Avoid blurry or dark images
4. Try different COLMAP quality: `--colmap-quality low` or `--colmap-quality medium`

### "Can't open/read file" or "Segmentation fault"

**Symptom**: OpenSplat crashes when loading images

**Solution**: Images are in a different location. Use `--images`:
```bash
./scripts/opensplat_wrapper.sh ~/colmap_project \
    --images ~/actual/path/to/images \
    -o output.ply
```

### "CUDA out of memory"

**Solution**: Reduce memory usage:
```bash
./scripts/opensplat_wrapper.sh ~/project \
    --downscale 2 \
    --densify-grad 0.0005 \
    -o output.ply
```

### "LichtFeld-Studio not found"

**Solution**: Install it (Linux CUDA only):
```bash
./scripts/setup_lichtfeld.sh
```

### "OpenSplat not found"

**Solution**:
```bash
./scripts/setup_opensplat.sh
```

### Training is Very Slow

**Possible causes**:
1. **No GPU**: Check with `nvidia-smi` (Linux) or `./build/melkor --info` (prints the active Metal/CUDA/CPU backend)
2. **Too many iterations**: Try `--quality fast` first
3. **Large images**: Use `--downscale 2` or `--downscale 4`

### Output Quality is Poor

**Solutions**:
1. Use more images (50-100+)
2. Increase iterations: `-n 50000` or `-n 100000`
3. Use pose optimization (LichtFeld): `--pose-opt mlp`
4. Improve image quality (better lighting, no blur)

---

## Preprocessing & Postprocessing

### Image Preprocessing

```bash
# Resize large images (reduces memory, speeds up training)
mogrify -resize 1920x1080\> images/*.jpg

# Convert HEIC to JPEG (macOS)
for f in *.HEIC; do sips -s format jpeg "$f" --out "${f%.HEIC}.jpg"; done

# Remove blurry images (OpenCV Laplacian variance check)
python3 -c "
import cv2, os, sys
for f in os.listdir('images'):
    if f.lower().endswith(('.jpg', '.jpeg', '.png')):
        img = cv2.imread(f'images/{f}')
        var = cv2.Laplacian(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY), cv2.CV_64F).var()
        if var < 100:
            print(f'Blurry (variance={var:.0f}): {f}')
"
```

### Gaussian Postprocessing

```bash
# Compress PLY to SPZ (~90% smaller)
./build/melkor scene.ply scene.spz

# Fill holes / densify sparse regions (scene completion)
./build/melkor scene.ply scene_filled.ply --fill-holes

# Tune fill density and the largest bridgeable hole
./build/melkor scene.spz filled.spz --fill-holes --fill-strength 0.8 --max-hole-size 12

# Remove outliers and downsample (requires open3d)
python3 -c "
import open3d as o3d
pcd = o3d.io.read_point_cloud('scene.ply')
pcd, _ = pcd.remove_statistical_outlier(nb_neighbors=20, std_ratio=2.0)
pcd = pcd.voxel_down_sample(voxel_size=0.02)
o3d.io.write_point_cloud('scene_cleaned.ply', pcd)
print(f'Cleaned: {len(pcd.points)} points')
"
```

Hole-filling algorithm, parameters, and limits: [SCENE_COMPLETION.md](SCENE_COMPLETION.md).

### Viewers & Editors

- **Bundled SparkJS viewer** (PLY/SPZ/SOG/SPLAT): `cd viewer && ./fetch-assets.sh && bun run serve`;
  `bun run test` runs the Playwright render tests, `bun run app` opens the Tauri
  desktop shell — see [viewer/README.md](../viewer/README.md)
- **SuperSplat** (web editor): https://playcanvas.com/supersplat — drag & drop PLY for cleaning, filtering, compression

---

## Quick Reference

### Pipeline Commands

```bash
# Full pipeline (images → PLY + SPZ)
./scripts/pipeline.sh <images> <output> [options]

# Options:
#   --backend auto|cuda|metal|cpu    Compute backend (default: auto)
#   --quality fast|medium|high       Training quality (default: medium)
#   --iterations <n>                 Override iteration count
#   --format ply|spz|both            Output format (default: ply)
#   --tool auto|opensplat|gsplat|gsplat-cuda|lichtfeld
#   --sfm colmap|glomap              SfM tool (default: colmap)
#   --colmap-quality low|medium|high Feature extraction quality
#   --skip-colmap                    Skip SfM (use existing project)
#   --gpu-ids 0,1,2                  Multi-GPU (comma-separated)
#   --gpu-split single|data-parallel|memory-split
#   --downscale 1|2|4|8              Downscale images (memory saver)
#   --images <path>                  Custom image path
#   --verbose | --dry-run | --setup | --version
```

### Direct Tool Usage

```bash
# DA3 Feedforward (COLMAP-free, Linux CUDA)
./da3-infer --input <images> --output <output.ply>
./da3-infer-multigpu --input <images> --output <output.ply>  # Multi-GPU

# OpenSplat (COLMAP-based)
./opensplat <colmap_project> -n <iterations> -o <output.ply>

# LichtFeld-Studio (Linux CUDA, COLMAP-based)
./lichtfeld -d <colmap_project> -o <output_dir>/

# gsplat CUDA (COLMAP-based, multi-GPU)
./gsplat-cuda-train default --data_dir <colmap_project> --result_dir ./output
./gsplat-cuda-train-distributed --gpus 0,1,2,3 -- default --data_dir <colmap_project>

# Melkor conversion (formats auto-detected from extensions)
./build/melkor <input> <output> [options]
```

### Wrapper Scripts (Advanced Features)

```bash
# OpenSplat with custom images, multi-GPU
./scripts/opensplat_wrapper.sh <project> [options]

# LichtFeld with pose optimization
./scripts/lichtfeld_wrapper.sh <project> [options]

# GLOMAP global SfM (10-100× faster mapping)
./scripts/glomap_wrapper.sh <images> <output_dir> [options]
```

---

## Next Steps

- **View your splat**: Use the bundled [SparkJS viewer](../viewer/README.md) (`cd viewer && bun run serve`) or upload the PLY to [SuperSplat](https://playcanvas.com/supersplat)
- **Fill holes**: Run `melkor <in> <out> --fill-holes` on trained scenes — [SCENE_COMPLETION.md](SCENE_COMPLETION.md)
- **Mobile/Web**: Use SPZ format for streaming
- **Pipeline reference**: [PIPELINE.md](PIPELINE.md) for all `pipeline.sh` options
- **Advanced docs**: [OPENSPLAT_WRAPPER.md](OPENSPLAT_WRAPPER.md), [LICHTFELD_WRAPPER.md](LICHTFELD_WRAPPER.md), [GLOMAP_WRAPPER.md](GLOMAP_WRAPPER.md), [GSPLAT_CUDA.md](GSPLAT_CUDA.md), [DA3_FEEDFORWARD.md](DA3_FEEDFORWARD.md)
