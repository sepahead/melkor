# LichtFeld-Studio Wrapper - Advanced Training Guide

The `lichtfeld_wrapper.sh` script provides a convenient interface to LichtFeld-Studio, a high-performance 3D Gaussian Splatting implementation using C++23 and CUDA 12.8+.

**Platform:** Linux with NVIDIA CUDA only

## Table of Contents

- [Quick Start](#quick-start)
- [Installation](#installation)
- [Features](#features)
- [Command Reference](#command-reference)
- [Pose Optimization](#pose-optimization)
- [Common Workflows](#common-workflows)
- [Troubleshooting](#troubleshooting)
- [Comparison with OpenSplat](#comparison-with-opensplat)

## Quick Start

```bash
# Basic training
./scripts/lichtfeld_wrapper.sh ~/colmap_project -o ~/output/

# With pose optimization (fixes camera calibration errors)
./scripts/lichtfeld_wrapper.sh ~/colmap_project --pose-opt mlp -o ~/output/

# With custom image path
./scripts/lichtfeld_wrapper.sh ~/colmap_project --images ~/original_images -o ~/output/
```

## Installation

### Prerequisites

| Requirement | Minimum | Recommended |
|-------------|---------|-------------|
| **OS** | Linux | Ubuntu 22.04+ |
| **CUDA** | 12.x (setup warns below 12.8) | 12.8+ |
| **GCC** | 11 | 13+ |
| **GPU** | NVIDIA with Compute 7.0+ | RTX 30/40 series |

### Setup

```bash
# Install LichtFeld-Studio
./scripts/setup_lichtfeld.sh

# Verify installation
./lichtfeld --help
```

The setup script clones LichtFeld-Studio into `tools/LichtFeld-Studio`, downloads LibTorch 2.7.0 for CUDA 12.8 (~2 GB) into `tools/LichtFeld-Studio/external/libtorch`, builds with CMake + Ninja, and creates the `./lichtfeld` wrapper in the project root.

### Manual Prerequisites (Ubuntu)

```bash
# CUDA Toolkit
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt-get update
sudo apt-get install cuda-toolkit-12-8

# Build dependencies
sudo apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    unzip \
    wget \
    libopencv-dev \
    libeigen3-dev \
    libglfw3-dev \
    libglew-dev \
    libglm-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev

# GCC 11+ for C++23
sudo apt-get install gcc-11 g++-11
```

## Features

### Why LichtFeld-Studio?

LichtFeld-Studio offers several advantages over other training tools:

| Feature | LichtFeld-Studio | OpenSplat | gsplat-mps |
|---------|------------------|-----------|------------|
| **Speed** | ⚡⚡⚡ Fastest | ⚡⚡ Fast | ⚡ Fast |
| **Pose Optimization** | ✅ Direct + MLP | ❌ | ❌ |
| **MCMC Densification** | ✅ | ❌ | ✅ |
| **Interactive Viewer** | ✅ | ❌ | ❌ |
| **Dependencies** | LibTorch (auto-downloaded by setup) | LibTorch | PyTorch |
| **Platform** | Linux CUDA | All | macOS |

### Key Capabilities

1. **Pose Optimization**: Corrects camera pose errors from COLMAP
2. **MCMC Densification**: Better Gaussian placement strategy
3. **Fast Training**: ~20 minutes for 60k steps at 4K resolution
4. **Interactive Viewer**: Real-time preview with editing capabilities
5. **Self-Contained Setup**: No Python environment required; the setup script downloads LibTorch 2.7.0 (CUDA 12.8) automatically

## Command Reference

### Required Arguments

| Argument | Description |
|----------|-------------|
| `<colmap_project>` | Path to COLMAP project directory (with `sparse/` folder) |

### Image Path Options

| Option | Description |
|--------|-------------|
| `--images <path>` | Override image directory path |

With `--images`, the wrapper builds a temporary workspace: the `sparse/` reconstruction (and `database.db`, if present) is copied and `.jpg`/`.jpeg`/`.png` files from the given directory are symlinked into `images/`. The workspace is deleted after training.

### Output Options

| Option | Default | Description |
|--------|---------|-------------|
| `-o, --output <dir>` | `output` | Output directory |
| `-n, --iterations <n>` | `30000` | Training iterations |

**Note:** `-n, --iterations` is accepted and shown in the configuration summary, but the wrapper does not currently forward it to the LichtFeld-Studio binary.

### GPU Options

| Option | Default | Description |
|--------|---------|-------------|
| `--gpu <id>` | `0` | GPU to use |

GPU selection is applied by exporting `CUDA_VISIBLE_DEVICES=<id>` before launching the binary.

### Training Options

| Option | Values | Description |
|--------|--------|-------------|
| `--pose-opt <mode>` | `none`, `direct`, `mlp` | Pose optimization mode |
| `--no-mcmc` | | Disable MCMC densification |
| `--eval` | | Run evaluation after training |
| `--gui` | | Launch interactive viewer |

**Notes:**
- `--pose-opt direct` and `--pose-opt mlp` map to the binary flags `--pose-opt-direct` and `--pose-opt-mlp`.
- `--no-mcmc` is accepted and shown in the configuration summary, but is not currently forwarded to the binary.

### Other Options

| Option | Description |
|--------|-------------|
| `--lichtfeld <path>` | Path to LichtFeld-Studio binary |
| `--verbose, -v` | Verbose output |
| `--dry-run` | Show commands without executing |
| `--help, -h` | Show help |

## Pose Optimization

Pose optimization is one of LichtFeld-Studio's unique features. It corrects errors in COLMAP's camera pose estimation, which is especially useful when:

- COLMAP had limited features to track
- Images have motion blur
- Camera calibration is imperfect
- Scene has repeating patterns

### Optimization Modes

#### `none` (Default)

Uses COLMAP poses as-is. Best when:
- COLMAP reconstruction was high quality
- You have many overlapping images
- Scene has good feature distribution

```bash
./scripts/lichtfeld_wrapper.sh ~/project -o output/
```

#### `direct`

Direct pose offset optimization. Good balance of speed and quality.

```bash
./scripts/lichtfeld_wrapper.sh ~/project --pose-opt direct -o output/
```

**Pros:**
- Faster than MLP
- Good for small pose errors

**Cons:**
- May not handle large errors well

#### `mlp`

MLP-based pose optimization. Highest quality but slower.

```bash
./scripts/lichtfeld_wrapper.sh ~/project --pose-opt mlp -o output/
```

**Pros:**
- Handles larger pose errors
- Better final quality
- Can fix systematic calibration issues

**Cons:**
- ~20% slower training
- More memory usage

### When to Use Pose Optimization

| Scenario | Recommended Mode |
|----------|------------------|
| High-quality COLMAP output | `none` |
| Some reconstruction warnings | `direct` |
| Poor COLMAP quality | `mlp` |
| Moving/handheld camera | `mlp` |
| Aerial footage | `direct` or `mlp` |
| Studio with calibrated camera | `none` |

## Common Workflows

### 1. Basic Training

```bash
./scripts/lichtfeld_wrapper.sh ~/colmap_project -o ~/output/
```

### 2. Maximum Quality

```bash
./scripts/lichtfeld_wrapper.sh ~/colmap_project \
    --pose-opt mlp \
    -n 60000 \
    --eval \
    -o ~/output/
```

### 3. Custom Image Path

When COLMAP's stored image paths don't match actual locations:

```bash
./scripts/lichtfeld_wrapper.sh ~/colmap_project \
    --images ~/actual/images \
    -o ~/output/
```

### 4. Interactive Preview

```bash
# Requires display (not headless)
./scripts/lichtfeld_wrapper.sh ~/colmap_project \
    --gui \
    -o ~/output/
```

### 5. Specific GPU

```bash
# Use GPU 1 instead of default GPU 0
./scripts/lichtfeld_wrapper.sh ~/colmap_project \
    --gpu 1 \
    -o ~/output/
```

### 6. Full Pipeline Integration

```bash
# Use LichtFeld-Studio via the main pipeline
./scripts/pipeline.sh ~/Photos/scene ~/output/ \
    --tool lichtfeld \
    --quality high \
    --format both
```

## Troubleshooting

### "LichtFeld-Studio binary not found"

The wrapper searches, in order: `./lichtfeld` (project root), `tools/LichtFeld-Studio/build/LichtFeld-Studio`, then `lichtfeld` / `LichtFeld-Studio` on `PATH`.

```bash
# Install LichtFeld-Studio
./scripts/setup_lichtfeld.sh

# Or specify path manually
./scripts/lichtfeld_wrapper.sh ~/project \
    --lichtfeld /path/to/LichtFeld-Studio \
    -o output/
```

### "CUDA error" or "GPU not found"

```bash
# Check CUDA installation
nvcc --version
nvidia-smi

# Check CUDA paths
export CUDA_HOME=/usr/local/cuda
export PATH=$CUDA_HOME/bin:$PATH
export LD_LIBRARY_PATH=$CUDA_HOME/lib64:$LD_LIBRARY_PATH
```

### "C++23 features not supported"

You need GCC 11 or later:

```bash
# Install GCC 11
sudo apt-get install gcc-11 g++-11

# Set as default
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 11
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-11 11
```

### "Images not found" or "Can't open file"

Use `--images` to specify the correct image location:

```bash
./scripts/lichtfeld_wrapper.sh ~/colmap_project \
    --images /actual/path/to/images \
    -o output/
```

### "Out of memory"

LichtFeld-Studio is memory-efficient, but for very large scenes:

1. Reduce image resolution before training
2. Use a GPU with more VRAM
3. Reduce number of iterations

### Build Failures

```bash
# Clean rebuild
./scripts/setup_lichtfeld.sh --clean

# Check CMake version (need 3.20+)
cmake --version

# Check Ninja
ninja --version
```

## Comparison with OpenSplat

### When to Use LichtFeld-Studio

✅ **Use LichtFeld-Studio when:**
- You have Linux with NVIDIA GPU (CUDA 12.8+)
- You need maximum training speed
- COLMAP poses may be inaccurate
- You want interactive preview
- You're processing many scenes

### When to Use OpenSplat

✅ **Use OpenSplat when:**
- You need cross-platform support (macOS Metal)
- You have an older CUDA toolkit (LichtFeld-Studio targets 12.8+)
- You want simpler setup

For multi-GPU distributed training, use gsplat CUDA instead (see the [gsplat CUDA Guide](GSPLAT_CUDA.md)).

### Performance Comparison

| Metric | LichtFeld-Studio | OpenSplat |
|--------|------------------|----------|
| 30k iters (1080p) | ~10 min | ~15 min |
| 60k iters (4K) | ~20 min | ~35 min |
| Memory (4K images) | ~8 GB | ~10 GB |
| Startup time | Fast | Moderate |

## Advanced Configuration

### Environment Variables

```bash
# Use specific CUDA version
export CUDA_HOME=/usr/local/cuda-12.8

# Limit GPU memory (LibTorch caching allocator)
export PYTORCH_CUDA_ALLOC_CONF=max_split_size_mb:512

# Use specific GPU (only when invoking ./lichtfeld directly)
export CUDA_VISIBLE_DEVICES=1
```

When training through `lichtfeld_wrapper.sh`, the wrapper always sets `CUDA_VISIBLE_DEVICES` from its `--gpu` option (default `0`), overriding any value exported beforehand. Use `--gpu <id>` instead.

### Running Headlessly (Server)

LichtFeld-Studio works without a display for training:

```bash
# Training works headlessly
./scripts/lichtfeld_wrapper.sh ~/project -o output/

# GUI requires display (or virtual framebuffer)
xvfb-run ./scripts/lichtfeld_wrapper.sh ~/project --gui -o output/
```

### Long-Running Training

```bash
# Use screen or tmux
screen -S training
./scripts/lichtfeld_wrapper.sh ~/project --pose-opt mlp -n 100000 -o output/
# Ctrl+A, D to detach
# screen -r training to reattach
```

---

## See Also

- [Quick Start Guide](QUICKSTART.md) - Getting started with Melkor
- [Pipeline Documentation](PIPELINE.md) - Complete pipeline reference
- [OpenSplat Wrapper](OPENSPLAT_WRAPPER.md) - OpenSplat advanced features
- [gsplat CUDA Guide](GSPLAT_CUDA.md) - Multi-GPU distributed training
