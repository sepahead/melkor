# Melkor Quick Start Guide

**Complete guide to 3D Gaussian Splatting: from photos to PLY or SPZ output.**

## Table of Contents

1. [Overview](#overview)
2. [Installation](#installation)
3. [Complete Pipeline: Photos → 3D](#complete-pipeline-photos--3d)
4. [Output Formats](#output-formats)
5. [Tool Selection](#tool-selection)
6. [Common Workflows](#common-workflows)
7. [Troubleshooting](#troubleshooting)

---

## Overview

### What is Melkor?

Melkor is a unified toolkit for 3D Gaussian Splatting that provides:

- **Multiple training backends**: OpenSplat, LichtFeld-Studio, gsplat-mps, DA3
- **Two approaches**: COLMAP-based (traditional) and COLMAP-Free (feedforward)
- **Format conversion**: GLB → PLY → SPZ (GLB is input only)
- **Cross-platform support**: macOS (Metal), Linux (CUDA), CPU fallback

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
git clone https://github.com/sepehrmn/melkor.git
cd melkor

# Install everything (auto-detects platform)
./scripts/setup_all.sh

# Install COLMAP (required for training from images)
# macOS:
brew install colmap

# Linux:
sudo apt-get install colmap
```

### What Gets Installed

| Tool | Platform | Description |
|------|----------|-------------|
| **OpenSplat** | All | Cross-platform, production-grade |
| **LichtFeld-Studio** | Linux CUDA | Fastest, pose optimization |
| **gsplat-mps** | macOS | Research, flexible |
| **Melkor CLI** | All | Format conversion |

---

## Complete Pipeline: Photos → 3D

### Option 1: COLMAP-Free with DA3 (Fastest - Seconds)

No COLMAP, no camera poses, just images → 3D Gaussians:

```bash
# 1. Install DA3 (Linux CUDA only)
./scripts/setup_da3.sh

# 2. Single image to 3DGS
./da3-infer --input photo.jpg --output scene.ply

# 3. Multiple images (no COLMAP needed!)
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

Traditional approach with camera pose estimation:

```bash
# Basic usage
./scripts/pipeline.sh ~/Photos/my_scene ~/output/my_scene

# With options
./scripts/pipeline.sh ~/Photos/my_scene ~/output/my_scene \
    --quality high \
    --format both \
    --tool auto
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
# On Linux with NVIDIA GPU, add --SiftExtraction.use_gpu 1 --SiftMatching.use_gpu 1
colmap automatic_reconstructor \
    --workspace_path ~/output/my_scene \
    --image_path ~/Photos/my_scene \
    --quality high \
    --SiftExtraction.use_gpu 1 \
    --SiftMatching.use_gpu 1

# Verify output
ls ~/output/my_scene/sparse/0/
# Should see: cameras.bin, images.bin, points3D.bin
```

> **Note:** The pipeline scripts automatically detect NVIDIA GPUs and enable CUDA acceleration for COLMAP.

#### Step 3: Train Gaussian Splat

**Choose your tool:**

```bash
# OpenSplat (cross-platform, recommended)
./opensplat ~/output/my_scene -n 30000 -o ~/output/my_scene/splat.ply

# LichtFeld-Studio (Linux CUDA, fastest)
./lichtfeld -d ~/output/my_scene -o ~/output/my_scene/

# gsplat-mps (macOS, research)
source tools/gsplat-mps/venv/bin/activate
python tools/gsplat-mps/examples/simple_trainer.py \
    --data_dir ~/output/my_scene \
    --result_dir ~/output/my_scene
```

#### Step 4: Convert to Desired Format

```bash
# PLY → SPZ (compressed, ~90% smaller)
./build/melkor ~/output/my_scene/splat.ply ~/output/my_scene/splat.spz

# GLB → PLY (convert a 3D mesh to Gaussian splats)
./build/melkor model.glb output.ply

# GLB → SPZ (convert mesh directly to compressed format)
./build/melkor model.glb output.spz
```

> **Note:** GLB is only supported as an INPUT format (for converting 3D meshes to Gaussian splats). 
> PLY and SPZ are the supported OUTPUT formats.

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

# On Linux with CUDA: Uses LichtFeld-Studio
# On Linux without CUDA: Uses OpenSplat (CPU)
# On macOS: Uses OpenSplat (Metal) or gsplat-mps
```

### Manual Selection

```bash
# Force specific tool
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool opensplat
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool lichtfeld   # Linux CUDA only
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
# Distribute across multiple GPUs (OpenSplat)
./scripts/opensplat_wrapper.sh ~/project \
    --gpu-ids 0,1,2,3 \
    --split data-parallel \
    -o output.ply
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
4. Try different COLMAP quality: `--quality low` or `--quality medium`

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
1. **No GPU**: Check with `nvidia-smi` (Linux) or ensure Metal is working (macOS)
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

### Web-Based Editor

- **SuperSplat**: https://playcanvas.com/supersplat - Drag & drop PLY for cleaning, filtering, compression

---

## Quick Reference

### Pipeline Commands

```bash
# Full pipeline (images → PLY + SPZ)
./scripts/pipeline.sh <images> <output> [options]

# Options:
#   --quality fast|medium|high   Training quality (default: medium)
#   --format ply|spz|both        Output format (default: ply)
#   --tool auto|opensplat|lichtfeld|gsplat
#   --skip-colmap                Skip COLMAP (use existing project)
#   --gpu-ids 0,1,2              Multi-GPU (comma-separated)
#   --downscale 1|2|4            Downscale images (memory saver)
#   --images <path>              Custom image path
```

### Direct Tool Usage

```bash
# DA3 Feedforward (COLMAP-Free, Linux CUDA)
./da3-infer --input <images> --output <output.ply>
./da3-infer-multigpu --input <images> --output <output.ply>  # Multi-GPU

# OpenSplat (COLMAP-Based)
./opensplat <colmap_project> -n <iterations> -o <output.ply>

# LichtFeld-Studio (Linux CUDA, COLMAP-Based)
./lichtfeld -d <colmap_project> -o <output_dir>/

# gsplat CUDA (COLMAP-Based, Multi-GPU)
./gsplat-cuda-train default --data_dir <colmap_project> --result_dir ./output
./gsplat-cuda-train-distributed --gpus 0,1,2,3 -- default --data_dir <colmap_project>

# Melkor conversion
./build/melkor <input> <output>   # Auto-detects formats
```

### Wrapper Scripts (Advanced Features)

```bash
# OpenSplat with custom images, multi-GPU
./scripts/opensplat_wrapper.sh <project> [options]

# LichtFeld with pose optimization
./scripts/lichtfeld_wrapper.sh <project> [options]
```

---

## Next Steps

- **View your splat**: Upload PLY to [SuperSplat](https://playcanvas.com/supersplat)
- **Mobile/Web**: Use SPZ format for streaming
- **Game engines**: Many engines now support PLY splats directly
- **Advanced docs**: See `docs/OPENSPLAT_WRAPPER.md` for all options
