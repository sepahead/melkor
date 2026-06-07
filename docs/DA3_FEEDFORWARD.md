# Depth-Anything-3 Feedforward 3D Gaussian Splatting

Depth-Anything-3 (DA3) from ByteDance enables **feedforward** 3D Gaussian Splatting - generating 3D Gaussian splats from images in a single forward pass without iterative optimization.

**Platform:** Linux with NVIDIA CUDA only

## Table of Contents

- [Overview](#overview)
- [Quick Start](#quick-start)
- [Installation](#installation)
- [COLMAP-Free vs COLMAP Approaches](#colmap-free-vs-colmap-approaches)
- [Single-GPU Usage](#single-gpu-usage)
- [Multi-GPU Usage](#multi-gpu-usage)
- [Model Variants](#model-variants)
- [Output Formats](#output-formats)
- [Preprocessing & Post-Processing](#preprocessing--post-processing)
- [Bring Your Own Setup](#bring-your-own-setup)
- [Map Anything / SAM Integration](#map-anything--sam-integration)
- [How It Works](#how-it-works)
- [Comparison with Other Methods](#comparison-with-other-methods)
- [Command Reference](#command-reference)
- [Troubleshooting](#troubleshooting)
- [DINOv2 and DINOv3](#dinov2-and-dinov3-foundation-models-for-vision)

## Overview

### What is Depth-Anything-3?

DA3 is a transformer-based model that predicts **depth** and **ray directions** from images. Unlike traditional 3DGS methods that require iterative optimization (7,000-50,000 steps), DA3 generates geometry in a single forward pass.

### Key Features

| Feature | Description |
|---------|-------------|
| **Feedforward** | Single forward pass, no iterative optimization |
| **Multi-GPU** | Native distributed inference via PyTorch DDP |
| **Multi-View** | Handles single or multiple input images |
| **Fast** | ~1-5 seconds per image on modern GPUs |
| **No COLMAP** | Doesn't require camera poses (estimates them internally) |

### When to Use DA3

✅ **Use DA3 when:**
- You need fast results (seconds vs minutes)
- You have 1-10 images of a scene
- You don't have camera poses from COLMAP
- You want to leverage multiple GPUs for batch processing

❌ **Use traditional 3DGS when:**
- You have 50+ images with good coverage
- You need maximum quality
- You have existing COLMAP reconstruction
- Training time isn't critical

## Quick Start

```bash
# 1. Install DA3 with CUDA support
./scripts/setup_da3.sh

# 2. Single image to 3DGS
./da3-infer --input photo.jpg --output scene.ply

# 3. Directory of images (single GPU)
./da3-infer --input images/ --output scene.ply

# 4. Multi-GPU inference
./da3-infer-multigpu --input images/ --output scene.ply

# 5. Compress PLY to SPZ (~90% smaller)
./build/melkor scene.ply scene.spz

# 6. Export to GLB (point cloud visualization)
./da3-infer --input images/ --output scene.glb
```

## Installation

### Prerequisites

> **CUDA Required:** DA3 feedforward models run on NVIDIA GPUs with CUDA. If you have CUDA available, the models will automatically use GPU acceleration for fast inference.

| Requirement | Minimum | Recommended |
|-------------|---------|-------------|
| **OS** | Linux | Ubuntu 22.04+ |
| **CUDA** | 11.8 | 12.1+ (12.4 supported) |
| **Python** | 3.10 | 3.11+ |
| **GPU** | NVIDIA with 8GB+ VRAM | RTX 3090/4090, A100 |
| **RAM** | 16 GB | 32 GB+ |

### Setup Steps

```bash
# 1. Ensure CUDA is installed
nvcc --version
nvidia-smi

# 2. Run the setup script
chmod +x scripts/setup_da3.sh
./scripts/setup_da3.sh

# 3. Select models to download when prompted:
#    - DA3-BASE (recommended, ~2GB)
#    - DA3-LARGE (~4GB, higher quality)
#    - DA3-SMALL (~1GB, fastest)

# 4. Verify installation
./da3-infer --help
```

### What Gets Installed

- **Depth-Anything-3** - DA3 model from ByteDance
- **PyTorch with CUDA** - Automatically matched to your CUDA version
- **Model weights** - Downloaded from HuggingFace
- **Wrapper scripts:**
  - `da3-infer` - Single-GPU inference
  - `da3-infer-multigpu` - Multi-GPU distributed inference
  - `da3-python` - Python with DA3 environment

### Build Time

| Component | Time |
|-----------|------|
| Clone repository | ~1 min |
| Install PyTorch + CUDA | ~3-5 min |
| Download DA3-BASE | ~2-5 min |
| **Total** | **~10-15 min** |

---

## COLMAP-Free vs COLMAP Approaches

Melkor supports two fundamentally different approaches to 3D Gaussian Splatting:

### Comparison Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    COLMAP-FREE vs COLMAP APPROACHES                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  COLMAP-FREE (DA3 Feedforward):                                            │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                                                                      │   │
│  │  Images ──▶ DA3 Neural Network ──▶ Depth + Rays ──▶ 3D Gaussians   │   │
│  │                                                                      │   │
│  │  • Single forward pass (seconds)                                    │   │
│  │  • No camera pose estimation step                                   │   │
│  │  • Works with 1-10 images                                           │   │
│  │  • Poses estimated internally by the model                          │   │
│  │                                                                      │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  COLMAP-BASED (Traditional 3DGS):                                          │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                                                                      │   │
│  │  Images ──▶ COLMAP SfM ──▶ Camera Poses ──▶ 3DGS Training ──▶ PLY  │   │
│  │              (SIFT)         (sparse pts)   (7K-50K iterations)      │   │
│  │                                                                      │   │
│  │  • Multi-step pipeline (minutes to hours)                           │   │
│  │  • Explicit camera pose estimation via SIFT features                │   │
│  │  • Best with 50+ images with good overlap                           │   │
│  │  • Higher quality for complex scenes                                │   │
│  │                                                                      │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### When to Use Each Approach

| Scenario | COLMAP-Free (DA3) | COLMAP-Based |
|----------|-------------------|---------------|
| **Few images (1-10)** | ✅ Recommended | ❌ May fail |
| **Many images (50+)** | ⚠️ Works but slower | ✅ Recommended |
| **No camera poses available** | ✅ Only option | ❌ Requires poses |
| **Existing COLMAP project** | ⚠️ Can ignore poses | ✅ Use existing |
| **Maximum quality needed** | ⚠️ Good quality | ✅ Best quality |
| **Real-time / fast results** | ✅ Seconds | ❌ Minutes-hours |
| **Textureless surfaces** | ✅ Works well | ❌ SIFT may fail |
| **Reflective surfaces** | ✅ Works well | ❌ SIFT may fail |
| **Batch processing** | ✅ Fast per-scene | ❌ Slow per-scene |

### Usage Examples

```bash
# COLMAP-FREE: Direct feedforward inference (no poses needed)
./da3-infer --input images/ --output scene.ply

# COLMAP-BASED: Traditional pipeline
./scripts/pipeline.sh ~/Photos/scene ~/output/

# Hybrid: Use existing COLMAP reconstruction (skip SfM step)
./scripts/pipeline.sh ~/colmap_project ~/output/ --skip-colmap
```

### Technical Differences

| Aspect | COLMAP-Free (DA3) | COLMAP-Based |
|--------|-------------------|---------------|
| **Pose estimation** | Implicit (neural network) | Explicit (SIFT + bundle adjustment) |
| **Feature matching** | DINOv2 dense features | SIFT sparse keypoints |
| **Optimization** | None (feedforward) | Iterative (gradient descent) |
| **Memory usage** | Per-image | Accumulates with images |
| **GPU requirement** | CUDA for inference | CUDA optional (helps COLMAP) |
| **Failure modes** | Rare (model generalizes) | Common on difficult scenes |

## Single-GPU Usage

### Basic Usage

```bash
# Single image
./da3-infer --input photo.jpg --output scene.ply

# Directory of images
./da3-infer --input images/ --output scene.ply

# With specific model
./da3-infer --model DA3-LARGE --input images/ --output scene.ply
```

### Adjusting Output Quality

```bash
# Smaller Gaussians (more detail, more points)
./da3-infer --input images/ --output scene.ply --scale 0.005

# Larger Gaussians (smoother, fewer points)
./da3-infer --input images/ --output scene.ply --scale 0.02

# Subsample for faster processing / fewer Gaussians
./da3-infer --input images/ --output scene.ply --subsample 2
```

### Memory Optimization

```bash
# Use FP32 for stability (uses more memory)
./da3-infer --input images/ --output scene.ply --fp32

# Subsample to reduce memory and output size
./da3-infer --input images/ --output scene.ply --subsample 4
```

## Multi-GPU Usage

### How Multi-GPU Works

DA3 uses **data parallelism** for multi-GPU inference:

1. Images are distributed across GPUs
2. Each GPU runs DA3 independently on its subset
3. Results are gathered and merged on GPU 0

```
┌─────────────────────────────────────────────────────────────────┐
│                    Multi-GPU Architecture                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Images: [1,2,3,4,5,6,7,8,9,10,11,12]                           │
│                    │                                             │
│        ┌──────────┼──────────┬──────────┐                       │
│        ▼          ▼          ▼          ▼                       │
│     GPU 0      GPU 1      GPU 2      GPU 3                      │
│   [1,2,3]    [4,5,6]    [7,8,9]   [10,11,12]                   │
│        │          │          │          │                       │
│     DA3 ↓      DA3 ↓      DA3 ↓      DA3 ↓                      │
│        │          │          │          │                       │
│  Gaussians   Gaussians  Gaussians  Gaussians                    │
│        │          │          │          │                       │
│        └──────────┴──────────┴──────────┘                       │
│                    │                                             │
│              Gather on GPU 0                                    │
│                    │                                             │
│              Merged Gaussians                                   │
│                    │                                             │
│              Save to PLY                                        │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Basic Multi-GPU Usage

```bash
# Use all available GPUs
./da3-infer-multigpu --input images/ --output scene.ply

# Specify number of GPUs
./da3-infer-multigpu --gpus=4 --input images/ --output scene.ply

# Use specific GPUs
CUDA_VISIBLE_DEVICES=0,1,2,3 ./da3-infer-multigpu --input images/ --output scene.ply
```

### Using torchrun Directly

```bash
# Activate DA3 environment first
source tools/da3/venv/bin/activate

# Run with torchrun
cd tools/da3
torchrun --nproc_per_node=4 multigpu_inference.py \
    --input /path/to/images \
    --output /path/to/output.ply
```

### Multi-GPU Performance

| GPUs | Speedup | Images/sec (approx) |
|------|---------|--------------------|
| 1 | 1× | ~0.5-2 |
| 2 | ~1.9× | ~1-4 |
| 4 | ~3.5× | ~2-7 |
| 8 | ~6× | ~3-12 |

*Performance varies by GPU model, image resolution, and model variant.*

### Technical Details

- **Gathering method**: Uses `torch.distributed.all_gather()` for GPU-to-GPU tensor transfer
- **Large scene handling**: Automatically falls back to file-based gathering for >10M Gaussians
- **Timeout**: 30-minute NCCL timeout with helpful error messages
- **Cleanup**: Proper temp file cleanup on all ranks to prevent disk space leaks

## Model Variants

DA3 offers multiple model variants, each producing different outputs optimized for specific use cases. Understanding these differences is crucial for selecting the right model.

### Main Series (Any-View 3DGS)

These models predict **depth + ray directions** from arbitrary viewpoints, enabling full 3D reconstruction:

| Model | Parameters | Size | Speed | Quality | Output Type |
|-------|------------|------|-------|---------|-------------|
| **DA3-SMALL** | ~24M | ~1GB | ⚡⚡⚡ | Good | Depth + Rays + 3DGS |
| **DA3-BASE** | ~97M | ~2GB | ⚡⚡ | Very Good | Depth + Rays + 3DGS |
| **DA3-LARGE** | ~335M | ~4GB | ⚡ | Excellent | Depth + Rays + 3DGS |
| **DA3-GIANT** | ~1.15B | ~8GB | 🐢 | Best | Depth + Rays + 3DGS |

### Specialized Series

These models are optimized for specific tasks and produce different output types:

| Model | Size | Purpose | Output Type | Multi-View |
|-------|------|---------|-------------|------------|
| **DA3MONO-LARGE** | ~4GB | Single-view depth | Relative depth only | ❌ |
| **DA3METRIC-LARGE** | ~4GB | Metric depth (meters) | Absolute depth in meters | ❌ |
| **DA3NESTED-GIANT-LARGE** | ~12GB | Combined reconstruction | Depth + Rays + Metric depth | ✅ |

### Why Models Output Different Files

#### Output Differences by Model Type

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    MODEL OUTPUT COMPARISON                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  DA3-SMALL/BASE/LARGE/GIANT (Any-View Series):                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ Input: N images (any viewpoint)                                     │   │
│  │ Output (from Melkor wrapper):                                       │   │
│  │   • output.ply       - Fused 3D Gaussian splats (main output)       │   │
│  │                                                                      │   │
│  │ Internal processing (depth-ray representation):                     │   │
│  │   • depth: Per-pixel depth values (H × W × N)                       │   │
│  │   • rays:  Per-pixel ray directions (H × W × 3 × N)                 │   │
│  │   • poses: Estimated camera poses (inferred internally)             │   │
│  │                                                                      │   │
│  │ Gaussian Count: 100K - 10M+ (depends on resolution & image count)   │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  DA3MONO-LARGE (Monocular Series):                                         │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ Input: 1 image                                                       │   │
│  │ Output:                                                              │   │
│  │   • output.ply        - Point cloud (single viewpoint only)         │   │
│  │                                                                      │   │
│  │ Note: Outputs relative depth (0-1 normalized), not metric scale.    │   │
│  │ Best for: Depth visualization, AR effects, not full 3D recon.       │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  DA3METRIC-LARGE (Metric Series):                                          │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ Input: 1 image                                                       │   │
│  │ Output:                                                              │   │
│  │   • output.ply       - Metric point cloud (single view)             │   │
│  │                                                                      │   │
│  │ Note: Outputs absolute depth in meters - real-world scale.          │   │
│  │ Best for: Robotics, navigation, measurement applications.           │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  DA3NESTED-GIANT-LARGE (Combined Series):                                  │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ Input: N images                                                      │   │
│  │ Output:                                                              │   │
│  │   • output.ply       - Full 3D Gaussian splats with metric scale    │   │
│  │                                                                      │   │
│  │ Note: Combines DA3-GIANT (best quality) with metric depth           │   │
│  │ alignment for real-world scale. Largest model, highest quality.     │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### Gaussian Count Factors

The number of Gaussians generated varies based on:

| Factor | Effect on Output |
|--------|------------------|
| **Model size** | Larger models → more detail → more Gaussians |
| **Input resolution** | Higher resolution → more pixels → more Gaussians |
| **Number of images** | More images → more viewpoints → more Gaussians |
| **`--subsample` flag** | Higher subsample → fewer pixels → fewer Gaussians |
| **`--scale` parameter** | Affects Gaussian size, not count |
| **Depth range** | `--min-depth`/`--max-depth` filter invalid points |
| **Scene complexity** | Complex scenes with more valid depths → more Gaussians |

#### Typical Output Sizes

| Model | 1 Image (1080p) | 10 Images (1080p) | 100 Images (1080p) |
|-------|-----------------|-------------------|--------------------|
| DA3-SMALL | ~500K Gaussians | ~2M Gaussians | ~10M Gaussians |
| DA3-BASE | ~800K Gaussians | ~4M Gaussians | ~20M Gaussians |
| DA3-LARGE | ~1M Gaussians | ~6M Gaussians | ~30M Gaussians |
| DA3-GIANT | ~1.5M Gaussians | ~10M Gaussians | ~50M Gaussians |

*Note: Actual counts depend on scene content and depth validity.*

### Selecting a Model

```bash
# Fast preview (smallest, fastest)
./da3-infer --model DA3-SMALL --input images/ --output preview.ply

# Balanced quality (recommended for most use cases)
./da3-infer --model DA3-BASE --input images/ --output scene.ply

# High quality (slower, more detail)
./da3-infer --model DA3-LARGE --input images/ --output high_quality.ply

# Best quality (flagship model, ~1.15B parameters)
./da3-infer --model DA3-GIANT --input images/ --output best.ply

# Monocular depth only (single image, no 3D reconstruction)
./da3-infer --model DA3MONO-LARGE --input single_image.jpg --output depth.npy

# Metric depth for robotics (real-world scale in meters)
./da3-infer --model DA3METRIC-LARGE --input image.jpg --output metric.ply

# Full reconstruction with metric scale
./da3-infer --model DA3NESTED-GIANT-LARGE --input images/ --output full_metric.ply
```

### Downloading Additional Models

If you skipped model download during setup:

```bash
# Using the wrapper
./da3-python -c "
from huggingface_hub import snapshot_download
snapshot_download('depth-anything/DA3-LARGE', local_dir='$HOME/.melkor/models/da3/DA3-LARGE')
"

# Download the flagship GIANT model
./da3-python -c "
from huggingface_hub import snapshot_download
snapshot_download('depth-anything/DA3-GIANT', local_dir='$HOME/.melkor/models/da3/DA3-GIANT')
"
```

---

## Output Formats

DA3 can output Gaussian splats in multiple formats:

### Supported Output Formats

| Format | Extension | Size | Use Case |
|--------|-----------|------|----------|
| **PLY** | `.ply` | Large (100MB-1GB+) | Standard format, editing, archival |
| **SPZ** | `.spz` | Small (~10% of PLY) | Web, mobile, streaming |
| **GLB** | `.glb` | Medium | Point cloud visualization |
| **NPZ** | `.npz` | Compressed | Python/NumPy workflows |

### PLY Output (Default)

```bash
# Standard PLY output
./da3-infer --input images/ --output scene.ply
```

PLY is the default and most compatible format for 3DGS viewers.

### SPZ Compression (~90% Smaller)

PLY files can be compressed to SPZ format using Niantic's compression:

```bash
# Generate PLY, then compress to SPZ
./da3-infer --input images/ --output scene.ply
./build/melkor scene.ply scene.spz

# Or use the pipeline with automatic compression
./scripts/pipeline.sh ~/Photos/scene ~/output/ --format both
```

**SPZ Benefits:**
- ~90% file size reduction
- Fast decompression
- Ideal for web/mobile deployment
- Supported by major 3DGS viewers

### GLB Output (Point Cloud)

Some DA3 models can export directly to GLB for point cloud visualization:

```bash
# Export as GLB point cloud (requires trimesh)
./da3-infer --input images/ --output scene.glb --export-format glb
```

**Note:** GLB export creates a point cloud representation, not a full mesh. This is useful for:
- Quick visualization in 3D software
- Integration with game engines
- WebGL viewers

### NPZ Output (NumPy)

```bash
# Export for Python workflows
./da3-infer --input images/ --output scene.npz --export-format npz
```

NPZ contains compressed NumPy arrays with all Gaussian parameters.

---

## Preprocessing & Post-Processing

### Image Preprocessing

Good input images significantly improve output quality:

#### Recommended Image Preprocessing

```bash
# 1. Ensure uniform exposure across images
# Use exposure bracketing or manual exposure lock when capturing

# 2. Remove blurry images
# Motion blur and shallow DOF cause "floater" artifacts

# 3. Resize large images (optional, saves memory)
./da3-infer --input images/ --output scene.ply --subsample 2
```

#### Preprocessing Checklist

| Factor | Good ✅ | Bad ❌ |
|--------|---------|--------|
| **Focus** | Sharp, uniform focus | Shallow DOF, blur |
| **Exposure** | Consistent across images | Variable exposure |
| **Motion** | Stable, no motion blur | Camera shake |
| **Coverage** | 60-80% overlap | Sparse coverage |
| **Lighting** | Even, diffuse | Harsh shadows, glare |
| **Weather** | Clear conditions | Rain, snow, fog |

#### Advanced: Weather/Artifact Removal

For outdoor captures with weather artifacts:

```python
# Example: Remove snow/rain artifacts before DA3 processing
# (Requires external tools like WeatherGS or similar)

# 1. Apply atmospheric effect filter
# 2. Mask lens occlusions (water droplets, etc.)
# 3. Run DA3 on cleaned images
```

### Gaussian Post-Processing

After generating Gaussians, several post-processing options are available:

#### 1. Pruning (Reduce Gaussian Count)

```bash
# Use subsampling during generation
./da3-infer --input images/ --output scene.ply --subsample 2

# Or use voxel downsampling for fusion
./da3-infer --input images/ --output scene.ply --voxel-size 0.05
```

#### 2. Compression (SPZ)

```bash
# Compress PLY to SPZ (~90% smaller)
./build/melkor scene.ply scene.spz
```

#### 3. Scale Adjustment

```bash
# Smaller Gaussians = more detail, larger files
./da3-infer --input images/ --output scene.ply --scale 0.005

# Larger Gaussians = smoother, smaller files
./da3-infer --input images/ --output scene.ply --scale 0.02
```

#### 4. Depth Filtering

```bash
# Remove very close or very far points
./da3-infer --input images/ --output scene.ply --min-depth 0.5 --max-depth 50.0
```

#### Post-Processing Pipeline

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    RECOMMENDED POST-PROCESSING PIPELINE                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Raw Gaussians ──▶ Prune ──▶ Filter Depth ──▶ Compress ──▶ Final Output   │
│                                                                             │
│  Steps:                                                                     │
│  1. Generate with appropriate --scale and --subsample                      │
│  2. Filter outliers with --min-depth and --max-depth                       │
│  3. (Optional) Use external tools for mesh extraction                      │
│  4. Compress to SPZ for deployment                                         │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Bring Your Own Setup

You can use your own DA3 installation or gsplat CUDA setup with Melkor.

### Using Your Own DA3 Installation

If you already have Depth-Anything-3 installed:

```bash
# Option 1: Point to your existing installation
export DA3_MODEL_DIR=/path/to/your/da3/models
./da3-infer --model-dir $DA3_MODEL_DIR --input images/ --output scene.ply

# Option 2: Use your own Python environment
source /path/to/your/venv/bin/activate
python tools/da3/inference.py --input images/ --output scene.ply

# Option 3: Direct Python usage
python -c "
from depth_anything_3.api import DepthAnything3
model = DepthAnything3.from_pretrained('/path/to/model')
# Your custom inference code
"
```

### Using Your Own gsplat CUDA Installation

If you have gsplat with CUDA already set up:

```bash
# Option 1: Use existing gsplat for training (COLMAP-based approach)
source /path/to/your/gsplat/venv/bin/activate
python -m gsplat.examples.simple_trainer --data_dir /path/to/colmap_project

# Option 2: Point Melkor to your gsplat installation
export GSPLAT_DIR=/path/to/your/gsplat
./scripts/pipeline.sh ~/Photos/scene ~/output/ --tool gsplat-cuda

# Option 3: Use gsplat's native multi-GPU training
torchrun --nproc_per_node=4 -m gsplat.examples.simple_trainer \
    --data_dir /path/to/colmap_project \
    --result_dir ./output
```

### Configuration for Custom Setups

Create a custom configuration file:

```bash
# ~/.melkor/config.sh
export DA3_MODEL_DIR="/custom/path/to/da3/models"
export GSPLAT_DIR="/custom/path/to/gsplat"
export CUDA_VISIBLE_DEVICES="0,1,2,3"
export PYTORCH_CUDA_ALLOC_CONF="max_split_size_mb:512"
```

Then source it before running:

```bash
source ~/.melkor/config.sh
./da3-infer --input images/ --output scene.ply
```

### Feedforward Mode with CUDA

The `--feedforward` mode in Melkor uses neural network inference and supports CUDA:

```bash
# Feedforward mode automatically uses CUDA if available
./build/melkor input.glb output.ply --feedforward --model da3-base

# Force specific device
./da3-infer --input images/ --output scene.ply --device cuda

# CPU fallback (slower)
./da3-infer --input images/ --output scene.ply --device cpu
```

**Requirements for CUDA feedforward:**
- NVIDIA GPU with CUDA 11.8+
- PyTorch with CUDA support
- Sufficient VRAM (8GB+ recommended)

---

## Map Anything / SAM Integration

### What is Map Anything?

**Segment Anything Model (SAM)** from Meta AI is a powerful 2D segmentation model that can segment any object in images. When combined with 3D Gaussian Splatting, it enables:

- **Object-level 3D segmentation**: Isolate specific objects in 3D
- **Semantic Gaussians**: Add semantic labels to Gaussians
- **Editable 3D scenes**: Select and modify objects in 3D

### SAM + 3DGS Integration Methods

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    SAM + 3D GAUSSIAN SPLATTING                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Method 1: SAGA (Segment Any 3D GAussians)                                 │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ SAM 2D masks → Distill into 3D Gaussian affinity features          │   │
│  │ Result: Promptable 3D segmentation                                  │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  Method 2: Gaussian Grouping                                               │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ Augment Gaussians with identity encodings                          │   │
│  │ Supervise with SAM's 2D masks                                       │   │
│  │ Result: Object-level segmentation in 3D                             │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  Method 3: SA3D                                                            │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ Project SAM 2D masks into 3D mask grids                            │   │
│  │ Use density-guided inverse rendering                                │   │
│  │ Result: 3D segmentation from 2D prompts                             │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Using SAM with DA3

DA3 outputs can be combined with SAM for object segmentation:

```python
# Example: SAM + DA3 pipeline (conceptual)
import torch
from segment_anything import SamPredictor, sam_model_registry

# 1. Run DA3 to get Gaussians
# ./da3-infer --input images/ --output scene.ply

# 2. Load SAM for 2D segmentation
sam = sam_model_registry["vit_h"](checkpoint="sam_vit_h.pth")
predictor = SamPredictor(sam)

# 3. For each input image:
#    - Get SAM masks for objects of interest
#    - Project masks to 3D using depth from DA3
#    - Assign object labels to corresponding Gaussians

# 4. Result: Gaussians with per-object segmentation
```

### Future Integration Plans

Melkor plans to integrate SAM-based segmentation:

1. **Automatic object extraction**: Segment and export individual objects
2. **Semantic labels**: Add per-Gaussian semantic embeddings
3. **Interactive editing**: Click to select objects in 3D
4. **Scene composition**: Combine objects from different captures

## How It Works

### DA3 Pipeline

```
┌─────────────────────────────────────────────────────────────────┐
│                    DA3 → 3DGS Pipeline                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Input Image          DA3 Transformer        Depth-Ray Output   │
│  ┌─────────┐         ┌─────────────┐        ┌─────────────┐     │
│  │  RGB    │ ──────▶ │   DINOv2    │ ─────▶ │ Depth Map   │     │
│  │  Image  │         │   Backbone  │        │ Ray Dirs    │     │
│  └─────────┘         └─────────────┘        └─────────────┘     │
│                                                    │             │
│                                                    ▼             │
│                                             ┌─────────────┐     │
│                                             │ 3D Position │     │
│                                             │ P = d × ray │     │
│                                             └─────────────┘     │
│                                                    │             │
│                                                    ▼             │
│                                             ┌─────────────┐     │
│                                             │  Gaussian   │     │
│                                             │   Splats    │     │
│                                             └─────────────┘     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Depth-Ray Representation

DA3 predicts:
- **Depth (d)**: Distance from camera to surface for each pixel
- **Ray Direction (r)**: Unit vector pointing from camera through each pixel

3D position is computed as: `P = d × r`

### Gaussian Parameters

From the depth-ray output, we generate Gaussian parameters:

| Parameter | Computation |
|-----------|-------------|
| **Position** | `P = depth × ray` |
| **Color** | RGB from input image |
| **Scale** | Adaptive based on depth (further = larger) |
| **Rotation** | Identity (isotropic Gaussians) |
| **Opacity** | Fixed at 0.9 |

## Comparison with Other Methods

### Feedforward vs Iterative

| Aspect | DA3 (Feedforward) | Traditional 3DGS (Iterative) |
|--------|-------------------|------------------------------|
| **Time** | Seconds | Minutes to hours |
| **Input** | 1+ images | 50+ images recommended |
| **COLMAP** | Not required | Required |
| **Quality** | Good | Excellent |
| **Multi-GPU** | Data parallel (easy) | DDP training (complex) |
| **Memory** | Per-image | Accumulates |

### Feedforward Model Comparison

| Model | Architecture | Input | Multi-GPU | Quality |
|-------|--------------|-------|-----------|--------|
| **DA3** | DINOv2 Transformer | Any # of images | ✅ Native | ⭐⭐⭐⭐ |
| **Splatter-Image** | U-Net | Single image | ❌ | ⭐⭐⭐ |
| **MVSplat** | Cost Volume | 2 images | ❌ | ⭐⭐⭐⭐ |

### When DA3 Excels

- **Quick reconstruction** from few images
- **Real-time applications** requiring fast inference
- **Batch processing** large numbers of scenes
- **No-pose scenarios** where COLMAP fails

### When Traditional 3DGS Excels

- **Maximum quality** with many images
- **Detailed scenes** requiring fine optimization
- **Existing COLMAP** reconstructions
- **Research** requiring training customization

## Command Reference

### da3-infer (Single-GPU)

```bash
./da3-infer [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--input, -i` | Required | Input image or directory |
| `--output, -o` | Required | Output file (.ply or .json) |
| `--model, -m` | DA3-BASE | Model variant |
| `--model-dir` | ~/.melkor/models/da3 | Model weights directory |
| `--device` | cuda | Device (cuda, cpu) |
| `--scale` | 0.01 | Base Gaussian scale |
| `--subsample` | 1 | Pixel subsampling factor |
| `--min-depth` | 0.1 | Minimum valid depth |
| `--max-depth` | 100.0 | Maximum valid depth |
| `--voxel-size` | 0.05 | Voxel size for fusion |
| `--fp32` | False | Use FP32 instead of FP16 |
| `--verbose, -v` | False | Verbose output |

### da3-infer-multigpu (Multi-GPU)

```bash
./da3-infer-multigpu [options]
```

All options from `da3-infer` plus:

| Option | Default | Description |
|--------|---------|-------------|
| `--gpus` | All GPUs | Number of GPUs to use |

## Troubleshooting

### "CUDA out of memory"

```bash
# Use FP16 (default)
./da3-infer --input images/ --output scene.ply

# Subsample to reduce memory
./da3-infer --input images/ --output scene.ply --subsample 2

# Use smaller model
./da3-infer --model DA3-SMALL --input images/ --output scene.ply
```

### "No module named 'depth_anything_3'"

```bash
# Reinstall DA3
cd tools/da3/Depth-Anything-3
source ../venv/bin/activate
pip install -e .
```

### "NCCL error" in multi-GPU

The multi-GPU inference uses NCCL with a 30-minute timeout. Common solutions:

```bash
# Enable debug logging to see what's happening
export NCCL_DEBUG=INFO
./da3-infer-multigpu --input images/ --output scene.ply

# Disable InfiniBand if not available
export NCCL_IB_DISABLE=1
./da3-infer-multigpu --input images/ --output scene.ply

# Use a different network interface
export NCCL_SOCKET_IFNAME=eth0
./da3-infer-multigpu --input images/ --output scene.ply
```

**Error messages you might see:**
- `[Rank N] Failed to initialize distributed process group` - Check GPU accessibility
- `NCCL timeout` - Increase timeout or check network connectivity between GPUs

### Poor Quality Results

1. **Use larger model**: `--model DA3-LARGE`
2. **Reduce scale**: `--scale 0.005`
3. **More images**: Add more input images for better coverage
4. **Check depth range**: Adjust `--min-depth` and `--max-depth`

### Slow Performance

1. **Use FP16**: Don't use `--fp32`
2. **Use multi-GPU**: `./da3-infer-multigpu`
3. **Subsample**: `--subsample 2`
4. **Use smaller model**: `--model DA3-SMALL`

## Environment Variables

| Variable | Description |
|----------|-------------|
| `CUDA_VISIBLE_DEVICES` | Restrict visible GPUs |
| `MASTER_ADDR` | DDP master address (default: localhost) |
| `MASTER_PORT` | DDP master port (default: 29500) |
| `NCCL_DEBUG` | NCCL debugging (INFO, WARN) |
| `NCCL_IB_DISABLE` | Disable InfiniBand (set to 1 if not available) |
| `NCCL_SOCKET_IFNAME` | Network interface for NCCL (e.g., eth0, ens3) |

## Example: Processing 100 Images on 4 GPUs

```bash
# Setup
./scripts/setup_da3.sh

# Check GPUs
nvidia-smi

# Process all images
./da3-infer-multigpu \
    --input ~/photos/scene/ \
    --output ~/output/scene.ply \
    --model DA3-BASE \
    --scale 0.008

# Expected:
# - ~25 images per GPU
# - ~20-60 seconds total (vs 80-240s single GPU)
# - ~1-10M Gaussians depending on images
```

---

## DINOv2 and DINOv3: Foundation Models for Vision

### What is DINO?

DINO (Self-**DI**stillation with **NO** labels) is a family of self-supervised Vision Transformers developed by Meta AI. These models learn rich visual representations without requiring labeled data.

### DINOv2 (Current DA3 Backbone)

DA3 uses **DINOv2** as its backbone encoder:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    DA3 ARCHITECTURE (DINOv2 Backbone)                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Input Image(s)                                                             │
│       │                                                                     │
│       ▼                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    DINOv2 Vision Transformer                        │   │
│  │  ┌───────────┐    ┌───────────┐    ┌───────────┐    ┌───────────┐  │   │
│  │  │  Patch    │───▶│  Multi-   │───▶│  Layer    │───▶│  Dense    │  │   │
│  │  │  Embed    │    │  Head     │    │  Norm     │    │  Features │  │   │
│  │  │           │    │  Attn     │    │           │    │           │  │   │
│  │  └───────────┘    └───────────┘    └───────────┘    └───────────┘  │   │
│  │                                                                     │   │
│  │  Variants: ViT-S (22M), ViT-B (86M), ViT-L (304M), ViT-G (1.1B)   │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│       │                                                                     │
│       ▼                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    DualDPT Decoder                                  │   │
│  │  Dense features → Depth Map + Ray Directions                       │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│       │                                                                     │
│       ▼                                                                     │
│  Depth + Rays → 3D Gaussian Splats                                         │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

**DINOv2 Features:**
- Self-supervised pretraining on 142M curated images
- Strong dense spatial features for pixel-level tasks
- Multiple scales (ViT-S/B/L/G) for different compute budgets
- Excellent transfer to depth estimation, segmentation, correspondence

### DINOv3 (Released - Ready for Integration)

**DINOv3** was released by Meta AI in 2025 and is now available for use:

**Official Repository:** [github.com/facebookresearch/dinov3](https://github.com/facebookresearch/dinov3)

| Feature | DINOv2 | DINOv3 |
|---------|--------|--------|
| **Parameters** | Up to 1.1B | 7B |
| **Training images** | 142M | 1.7B |
| **Training technique** | Standard SSL | Gram anchoring loss |
| **Dense feature quality** | Excellent | State-of-the-art |
| **3D understanding** | Good | Significantly improved |
| **Status** | Stable | Released (2025) |

**DINOv3 Key Improvements:**

1. **Gram Anchoring Loss**: Stabilizes dense feature learning during long training, preventing degradation of local patch features

2. **Better 3D Understanding**: 
   - State-of-the-art 3D keypoint matching
   - Improved camera pose estimation
   - Better multi-view consistency

3. **Enhanced Depth Estimation**:
   - State-of-the-art on NYUv2 depth benchmark
   - More accurate monocular depth
   - Better edge preservation

**Availability:**
- GitHub: [facebookresearch/dinov3](https://github.com/facebookresearch/dinov3) (8.6k+ stars)
- Hugging Face Transformers: v4.56.0+
- PyTorch Image Models (timm): v1.0.20+
- License: Commercial use allowed

### Potential DINOv3 Applications in Melkor

DINOv3 could enhance the Melkor pipeline in several ways:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    POTENTIAL DINOv3 INTEGRATIONS                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. IMPROVED DEPTH ESTIMATION                                               │
│     ┌─────────────────────────────────────────────────────────────────┐    │
│     │ DINOv3 → Better monocular depth → Higher quality 3DGS          │    │
│     │ Use case: Single-image 3D reconstruction                        │    │
│     └─────────────────────────────────────────────────────────────────┘    │
│                                                                             │
│  2. FEATURE MATCHING FOR COLMAP                                            │
│     ┌─────────────────────────────────────────────────────────────────┐    │
│     │ DINOv3 features → Replace/augment SIFT features                 │    │
│     │ Use case: More robust camera pose estimation                    │    │
│     │ Benefit: Works on textureless surfaces, reflections             │    │
│     └─────────────────────────────────────────────────────────────────┘    │
│                                                                             │
│  3. SEMANTIC GAUSSIAN SPLATTING                                            │
│     ┌─────────────────────────────────────────────────────────────────┐    │
│     │ DINOv3 semantic features → Per-Gaussian semantic labels         │    │
│     │ Use case: Editable 3D scenes, object segmentation               │    │
│     └─────────────────────────────────────────────────────────────────┘    │
│                                                                             │
│  4. MULTI-VIEW CONSISTENCY                                                 │
│     ┌─────────────────────────────────────────────────────────────────┐    │
│     │ DINOv3 correspondence → Better multi-view Gaussian fusion       │    │
│     │ Use case: Fewer artifacts in overlapping regions                │    │
│     └─────────────────────────────────────────────────────────────────┘    │
│                                                                             │
│  5. REAL-TIME TRACKING                                                     │
│     ┌─────────────────────────────────────────────────────────────────┐    │
│     │ DINOv3 temporal features → Track Gaussians across frames        │    │
│     │ Use case: Dynamic scene reconstruction, video processing        │    │
│     └─────────────────────────────────────────────────────────────────┘    │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Integration Roadmap

**Current State:**
- DA3 uses DINOv2 (ViT-S/B/L/G variants)
- Excellent quality for feedforward 3DGS

**Future Possibilities:**
1. **DA3 with DINOv3 backbone**: When ByteDance releases DA3 updated with DINOv3
2. **Direct DINOv3 depth**: Use DINOv3 for standalone depth estimation
3. **DINOv3 feature matcher**: Replace COLMAP's SIFT with DINOv3 features
4. **Semantic Gaussians**: Add per-Gaussian semantic embeddings

### Using DINOv3 Directly

DINOv3 is officially released and can be used via multiple methods:

```python
# Method 1: Hugging Face Transformers (recommended)
# Note: Model IDs shown are illustrative - check official docs for actual names
from transformers import AutoModel, AutoImageProcessor

processor = AutoImageProcessor.from_pretrained("facebook/dinov3-giant")  # Check HF for actual ID
model = AutoModel.from_pretrained("facebook/dinov3-giant")
model.eval()

# Process image
from PIL import Image
import torch
image = Image.open("photo.jpg")
inputs = processor(images=image, return_tensors="pt")

with torch.no_grad():
    outputs = model(**inputs)
    features = outputs.last_hidden_state  # Dense features

# Method 2: PyTorch Image Models (timm)
import timm

# Check timm for actual model name - pattern shown is illustrative
model = timm.create_model('vit_giant_patch14_dinov3', pretrained=True)
model.eval()

# Method 3: Official Facebook Research repo (recommended for latest)
# git clone https://github.com/facebookresearch/dinov3
# See repo README for exact usage instructions
```

**Available DINOv3 Models:**

| Model | Parameters | Notes |
|-------|------------|-------|
| DINOv3-S | ~22M | Smallest, fastest |
| DINOv3-B | ~86M | Balanced |
| DINOv3-L | ~304M | High quality |
| DINOv3-G | ~1.1B | Very high quality |
| DINOv3-7B | ~7B | Flagship model |

> **Note:** Model identifiers (HuggingFace IDs, timm names) shown above are illustrative patterns.
> Always check the [official DINOv3 repository](https://github.com/facebookresearch/dinov3) for accurate model names and usage.

**Important Notes:**
- DINOv3-7B requires ~24GB+ VRAM
- Smaller variants (S/B/L) work on consumer GPUs
- For most use cases, **DA3 with DINOv2 already provides excellent results**
- DINOv3 integration into DA3 is planned for future releases

## See Also

- [Quick Start Guide](QUICKSTART.md) - Getting started with Melkor
- [Pipeline Documentation](PIPELINE.md) - Complete pipeline reference
- [gsplat CUDA Guide](GSPLAT_CUDA.md) - Traditional 3DGS multi-GPU training
- [Feedforward Models](#feedforward-model-comparison) - Other feedforward options
