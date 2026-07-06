# OpenSplat Wrapper - Advanced Training Guide

The `opensplat_wrapper.sh` script provides advanced features for OpenSplat training, including:

- **Custom image paths** - Use images from a different location than COLMAP expects
- **Multi-GPU modes** - Run training across multiple GPUs (independent parallel runs or sequential rotation; see [Multi-GPU Modes](#multi-gpu-modes))
- **Memory optimization** - Reduce VRAM usage for large scenes

## Table of Contents

- [Quick Start](#quick-start)
- [Installation](#installation)
- [Common Use Cases](#common-use-cases)
- [Command Reference](#command-reference)
- [Multi-GPU Modes](#multi-gpu-modes)
- [Memory Optimization](#memory-optimization)
- [Troubleshooting](#troubleshooting)
- [Linux-Specific Notes](#linux-specific-notes)
- [Performance Tips](#performance-tips)
- [Worked Example](#worked-example)
- [See Also](#see-also)

## Quick Start

```bash
# Basic usage with custom image path
./scripts/opensplat_wrapper.sh /path/to/colmap/project \
    --images /actual/path/to/images \
    -o output.ply

# Multi-GPU training
./scripts/opensplat_wrapper.sh /path/to/project \
    --gpu-ids 0,1,2,3 \
    --split data-parallel \
    -n 30000

# Memory-optimized training
./scripts/opensplat_wrapper.sh /path/to/project \
    --downscale 2 \
    --densify-grad 0.0005 \
    --stop-densify 10000
```

## Installation

### Prerequisites

**Linux (Ubuntu/Debian):**
```bash
# Required packages
sudo apt-get update
sudo apt-get install -y bash coreutils findutils

# For GPU training - NVIDIA drivers and CUDA
sudo apt-get install -y nvidia-driver-535  # or your preferred version
sudo apt-get install -y nvidia-cuda-toolkit

# Verify CUDA
nvidia-smi
nvcc --version
```

**macOS:**
```bash
# Bash 4.0+ required (macOS ships with bash 3.x)
brew install bash

# Add to your shell config (~/.zshrc or ~/.bashrc)
export PATH="/usr/local/bin:$PATH"  # or /opt/homebrew/bin for M1+
```

### Setting Up OpenSplat

```bash
# Clone and build OpenSplat
cd /path/to/melkor
./scripts/setup_opensplat.sh

# Verify installation
./opensplat --help
```

The setup script downloads LibTorch 2.2.0, clones and builds [OpenSplat](https://github.com/pierotofy/OpenSplat) under `tools/OpenSplat/`, and creates an `./opensplat` wrapper in the repository root. On macOS it builds with the Metal (MPS) runtime; on Linux it selects CUDA when `nvidia-smi` and `nvcc` are present, otherwise CPU.

Environment variables recognized by `setup_opensplat.sh`:

| Variable | Description |
|----------|-------------|
| `LIBTORCH_DIR` | Use an existing LibTorch installation instead of downloading |
| `OPENSPLAT_SRC` | Build from an existing OpenSplat source checkout instead of cloning |

### Make Wrapper Executable

```bash
chmod +x scripts/opensplat_wrapper.sh
```

## Common Use Cases

### 1. Images in a Different Location

When COLMAP stored relative paths like `./images/IMG_*.JPG` but your actual images are elsewhere:

```bash
# COLMAP project is at: ~/projects/scene1
# Actual images are at:  ~/photos/scene1_originals

./scripts/opensplat_wrapper.sh ~/projects/scene1 \
    --images ~/photos/scene1_originals \
    --opensplat ./opensplat \
    -n 30000 \
    -o ~/output/splat.ply
```

**What this does:**

1. Creates a temporary workspace
2. Copies COLMAP database files (`sparse/0/*`, `database.db`)
3. Creates symlinks from `workspace/images/` to your actual images
4. Runs OpenSplat in the temporary workspace
5. Cleans up when done

### 2. Out of Memory (OOM) Errors

If you're running out of GPU memory:

```bash
# Option A: Downscale images (reduces memory significantly)
./scripts/opensplat_wrapper.sh ./project \
    --downscale 2 \
    -o output.ply

# Option B: Reduce densification (fewer Gaussians = less memory)
./scripts/opensplat_wrapper.sh ./project \
    --densify-grad 0.0005 \
    --stop-densify 10000 \
    -o output.ply

# Option C: Combine both
./scripts/opensplat_wrapper.sh ./project \
    --downscale 2 \
    --densify-grad 0.0005 \
    --stop-densify 10000 \
    -o output.ply
```

### 3. Multi-GPU Training

```bash
# Use all 4 GPUs
./scripts/opensplat_wrapper.sh ./project \
    --gpu-ids 0,1,2,3 \
    --split data-parallel \
    -n 30000 \
    -o output.ply

# Use specific GPUs (e.g., skip GPU 2)
./scripts/opensplat_wrapper.sh ./project \
    --gpu-ids 0,1,3 \
    --split data-parallel \
    -o output.ply
```

### 4. Using a Specific GPU

```bash
# Use GPU 1 instead of GPU 0
./scripts/opensplat_wrapper.sh ./project \
    --gpu 1 \
    -o output.ply
```

### 5. Full Example with All Options

```bash
./scripts/opensplat_wrapper.sh ~/colmap_project \
    --images ~/original_images \
    --opensplat /path/to/opensplat \
    --gpu-ids 0,1 \
    --split data-parallel \
    --iterations 30000 \
    --downscale 2 \
    --densify-grad 0.0005 \
    --stop-densify 15000 \
    --save-every 5000 \
    --verbose \
    -o ~/output/final_splat.ply
```

## Command Reference

### Required Arguments

| Argument | Description |
|----------|-------------|
| `<colmap_project>` | Path to COLMAP project directory (must contain `sparse/` folder) |

### Image Path Options

| Option | Description |
|--------|-------------|
| `--images <path>` | Override image directory path. Use when images are in a different location than COLMAP expects. |

### Output Options

| Option | Default | Description |
|--------|---------|-------------|
| `-o, --output <file>` | `output.ply` | Output PLY file path |
| `-n, --iterations <n>` | `30000` | Number of training iterations |
| `--save-every <n>` | `0` (disabled) | Save checkpoint every N iterations |

### GPU Options

| Option | Default | Description |
|--------|---------|-------------|
| `--gpu <id>` | `0` | Use a specific single GPU (forces `--split single`) |
| `--gpu-ids <ids>` | | Comma-separated GPU IDs for multi-GPU |
| `--split <mode>` | `single` | Multi-GPU mode: `single`, `data-parallel`, `memory-split` |

### Memory Optimization

| Option | Default | Description |
|--------|---------|-------------|
| `--downscale <factor>` | `1` | Downscale images by factor (1, 2, 4, 8) |
| `--densify-grad <val>` | `0.0002`* | Gradient threshold for densification (higher = fewer Gaussians) |
| `--densify-size <val>` | `0.01`* | Size threshold for densification |
| `--densify-interval <n>` | `100`* | Interval between densification operations |
| `--stop-densify <n>` | `15000`* | Stop densification after N iterations |
| `--batch-size <n>` | | Accepted for forward compatibility; currently not forwarded to OpenSplat |

\* OpenSplat's built-in default. The wrapper passes the corresponding OpenSplat flag (`--densify-grad-thresh`, `--densify-size-thresh`, `--densify-every`, `--stop-densify-at`) only when the option is set explicitly.

### Other Options

| Option | Description |
|--------|-------------|
| `--opensplat <path>` | Path to opensplat binary. Auto-detected if not specified: `./opensplat` (repo root), `tools/OpenSplat/build/opensplat`, `PATH`, `/usr/local/bin/opensplat`, `~/opensplat` |
| `--verbose, -v` | Enable verbose output (prints the full OpenSplat command) |
| `--dry-run` | Show what would be done without executing (honored in `single` split mode) |
| `--help, -h` | Show help message |

All wrapper log messages are written to stderr; stdout carries only the output of the OpenSplat process itself, so the wrapper is safe to use in command substitution and pipelines.

## Multi-GPU Modes

In `data-parallel` and `memory-split` modes the wrapper forwards only `--downscale`, `--densify-grad`, and `--stop-densify` to each training run; `--densify-size`, `--densify-interval`, and `--save-every` apply to `single` mode only.

### `single` (Default)

Uses a single GPU. Best for:
- Simple training runs
- When you have one GPU
- Testing configurations

```bash
./scripts/opensplat_wrapper.sh ./project --gpu 0
```

### `data-parallel`

Runs independent training on each GPU simultaneously. The result from GPU 0 is used as the final output.

**Best for:**
- Quick experimentation (see results faster)
- When you want to try different random seeds
- Utilizing idle GPUs

**Note:** This doesn't actually distribute the workload - each GPU runs the full training independently.

```bash
./scripts/opensplat_wrapper.sh ./project \
    --gpu-ids 0,1,2,3 \
    --split data-parallel
```

### `memory-split`

Runs training sequentially on different GPUs, splitting iterations across them.

**Best for:**
- When each GPU has limited memory
- Rotating through GPUs to prevent thermal throttling

**Note:** OpenSplat does not support resuming from checkpoints, so each GPU trains from scratch for `iterations / num_gpus` steps. Only the final GPU's result is written to the output path; intermediate results are discarded.

```bash
./scripts/opensplat_wrapper.sh ./project \
    --gpu-ids 0,1 \
    --split memory-split
```

## Memory Optimization

### Understanding GPU Memory Usage

OpenSplat memory usage depends on:
1. **Image resolution** - Higher resolution = more memory
2. **Number of Gaussians** - More Gaussians = more memory
3. **Batch size** - More images per batch = more memory

### Memory Reduction Strategies

#### 1. Downscale Images

Reduces resolution by the specified factor:

| Factor | Effect | Memory Reduction |
|--------|--------|------------------|
| 1 | Original resolution | None |
| 2 | 1/2 width & height | ~75% |
| 4 | 1/4 width & height | ~94% |
| 8 | 1/8 width & height | ~98% |

```bash
--downscale 2  # Recommended starting point
```

#### 2. Reduce Densification

Limits the number of Gaussians created:

```bash
# Higher gradient threshold = fewer new Gaussians
--densify-grad 0.0005  # Default is 0.0002

# Stop densification earlier
--stop-densify 10000  # Default is 15000
```

#### 3. Combine Strategies

For very limited memory (e.g., 8GB VRAM):

```bash
./scripts/opensplat_wrapper.sh ./project \
    --downscale 4 \
    --densify-grad 0.001 \
    --stop-densify 7000 \
    -n 15000 \
    -o output.ply
```

## Troubleshooting

### "OpenSplat binary not found"

```bash
# Specify the path explicitly
./scripts/opensplat_wrapper.sh ./project \
    --opensplat /full/path/to/opensplat \
    -o output.ply

# Or add to your PATH
export PATH="/path/to/opensplat/dir:$PATH"
```

### "Can't open/read file" or "Segmentation fault"

This usually means OpenSplat can't find the images. Use `--images` to specify the correct path:

```bash
./scripts/opensplat_wrapper.sh ./colmap_project \
    --images /actual/path/to/images \
    -o output.ply
```

### "CUDA out of memory"

Try these in order:

1. **Downscale images:**
   ```bash
   --downscale 2
   ```

2. **Reduce densification:**
   ```bash
   --densify-grad 0.0005 --stop-densify 10000
   ```

3. **Use a different GPU with more memory:**
   ```bash
   --gpu 1  # Try a different GPU
   ```

4. **Reduce iterations:**
   ```bash
   -n 15000
   ```

### "Bash 4.0+ required"

**macOS:**
```bash
brew install bash
# Use the full path to new bash
/usr/local/bin/bash scripts/opensplat_wrapper.sh ...
```

**Linux (older distros):**
```bash
sudo apt-get install bash
```

### "nvidia-smi not found"

```bash
# Install NVIDIA drivers
sudo apt-get install nvidia-driver-535

# Or on Ubuntu with proprietary drivers
sudo ubuntu-drivers autoinstall

# Reboot after installation
sudo reboot
```

### Training Seems Stuck

Use `--verbose` to see what's happening:

```bash
./scripts/opensplat_wrapper.sh ./project \
    --verbose \
    -o output.ply
```

## Linux-Specific Notes

### CUDA Environment Setup

Add to your `~/.bashrc`:

```bash
# CUDA paths
export CUDA_HOME=/usr/local/cuda
export PATH=$CUDA_HOME/bin:$PATH
export LD_LIBRARY_PATH=$CUDA_HOME/lib64:$LD_LIBRARY_PATH

# Optional: Limit GPU memory growth
export PYTORCH_CUDA_ALLOC_CONF=max_split_size_mb:512
```

### Checking GPU Status

```bash
# View GPU usage
watch -n 1 nvidia-smi

# Check which GPUs are available
nvidia-smi --list-gpus

# Check available memory on each GPU
nvidia-smi --query-gpu=index,name,memory.free --format=csv
```

### Running on a Server (No Display)

The wrapper works headlessly. For COLMAP reconstruction:

```bash
# Install virtual framebuffer
sudo apt-get install xvfb

# Run COLMAP with virtual display
xvfb-run colmap automatic_reconstructor \
    --workspace_path ./project \
    --image_path ./images
```

### Systemd Service (Long-Running Training)

Create `/etc/systemd/system/opensplat-train.service`:

```ini
[Unit]
Description=OpenSplat Training
After=network.target

[Service]
Type=simple
User=your_username
WorkingDirectory=/path/to/melkor
ExecStart=/path/to/melkor/scripts/opensplat_wrapper.sh /path/to/project -o /path/to/output.ply
Restart=no

[Install]
WantedBy=multi-user.target
```

Usage:
```bash
sudo systemctl start opensplat-train
sudo systemctl status opensplat-train
journalctl -u opensplat-train -f  # Follow logs
```

### Running in Screen/Tmux

For long training runs:

```bash
# Start a screen session
screen -S training

# Run training
./scripts/opensplat_wrapper.sh ./project -n 30000 -o output.ply

# Detach: Ctrl+A, then D
# Reattach later:
screen -r training
```

## Performance Tips

1. **Use NVMe/SSD storage** - Image loading is faster
2. **Pre-downscale images** - Faster than runtime downscaling
3. **Close other GPU applications** - Free up VRAM
4. **Monitor GPU temperature** - Throttling reduces performance
5. **Use the right number of iterations:**
   - Fast preview: 7,000-10,000
   - Standard quality: 20,000-30,000
   - High quality: 50,000+

## Worked Example

For a COLMAP project whose images have been moved since reconstruction (the database references paths that no longer exist):

```bash
# COLMAP project: ~/projects/scene1
# OpenSplat binary: /path/to/melkor/opensplat
# Actual image location: /data/photos/scene1

./scripts/opensplat_wrapper.sh ~/projects/scene1 \
    --images /data/photos/scene1 \
    --opensplat /path/to/melkor/opensplat \
    -n 30000 \
    -o output.ply
```

If training runs out of GPU memory:

```bash
./scripts/opensplat_wrapper.sh ~/projects/scene1 \
    --images /data/photos/scene1 \
    --opensplat /path/to/melkor/opensplat \
    --downscale 2 \
    --densify-grad 0.0005 \
    -n 30000 \
    -o output.ply
```

---

## See Also

- [Quick Start Guide](QUICKSTART.md) - Getting started with Melkor
- [Pipeline Documentation](PIPELINE.md) - Complete pipeline reference
- [GLOMAP Wrapper](GLOMAP_WRAPPER.md) - Fast global SfM for sparse reconstruction
- [LichtFeld Wrapper](LICHTFELD_WRAPPER.md) - LichtFeld-Studio options
