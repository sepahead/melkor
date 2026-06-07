# gsplat CUDA - Multi-GPU Training Guide

gsplat is a high-performance CUDA library for 3D Gaussian Splatting with native multi-GPU support via PyTorch Distributed Data Parallel (DDP).

**Platform:** Linux with NVIDIA CUDA only

## Table of Contents

- [Quick Start](#quick-start)
- [Installation](#installation)
- [Multi-GPU Training](#multi-gpu-training)
- [Training Strategies](#training-strategies)
- [Command Reference](#command-reference)
- [Performance Tuning](#performance-tuning)
- [Comparison with Other Tools](#comparison-with-other-tools)
- [Troubleshooting](#troubleshooting)

## Quick Start

```bash
# Install gsplat with CUDA
./scripts/setup_gsplat_cuda.sh

# Single-GPU training
./gsplat-cuda-train default --data_dir /path/to/colmap_project --result_dir ./output

# Multi-GPU distributed training (all GPUs)
./gsplat-cuda-train-distributed -- default --data_dir /path/to/colmap_project --result_dir ./output

# Specific GPUs (e.g., 0,1,2,3 out of 5)
./gsplat-cuda-train-distributed --gpus 0,1,2,3 -- default --data_dir /path/to/colmap_project --result_dir ./output
```

## Installation

### Prerequisites

| Requirement | Minimum | Recommended |
|-------------|---------|-------------|
| **OS** | Linux | Ubuntu 22.04+ |
| **CUDA** | 11.8 | 12.1+ |
| **Python** | 3.10 | 3.11+ |
| **GPU** | NVIDIA with Compute 7.0+ | RTX 30/40 series |
| **RAM** | 16 GB | 32 GB+ |

### Setup Steps

```bash
# 1. Ensure CUDA is installed
nvcc --version
nvidia-smi

# 2. Run the setup script (takes 10-15 minutes)
chmod +x scripts/setup_gsplat_cuda.sh
./scripts/setup_gsplat_cuda.sh

# 3. Verify installation
./gsplat-cuda-train --help
```

### What Gets Installed

- **gsplat library** - CUDA-accelerated Gaussian Splatting
- **PyTorch with CUDA** - Automatically matched to your CUDA version
- **Training examples** - Simple trainer and MCMC trainer
- **Wrapper scripts:**
  - `gsplat-cuda-train` - Single-GPU training
  - `gsplat-cuda-train-distributed` - Multi-GPU distributed training
  - `gsplat-cuda-python` - Python with gsplat environment

### Build Time

| Component | Time |
|-----------|------|
| Clone repository | ~1 min |
| Install PyTorch + CUDA | ~3-5 min |
| Build CUDA kernels | ~5-10 min |
| **Total** | **~10-15 min** |

## Multi-GPU Training

### How It Works

gsplat uses PyTorch's Distributed Data Parallel (DDP) with NCCL backend:

1. **Data Parallelism** - Each GPU processes different training images in parallel
2. **Gradient Synchronization** - NCCL averages gradients across all GPUs
3. **Model Replication** - Each GPU holds a copy of the Gaussian model
4. **Memory Efficiency** - gsplat uses 4× less memory than original 3DGS implementation

```
┌─────────────────────────────────────────────────────────────────┐
│                    Multi-GPU Architecture (DDP)                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  GPU 0          GPU 1          GPU 2          GPU 3              │
│  ┌─────┐        ┌─────┐        ┌─────┐        ┌─────┐            │
│  │Image│        │Image│        │Image│        │Image│            │
│  │Batch│        │Batch│        │Batch│        │Batch│            │
│  │ A   │        │ B   │        │ C   │        │ D   │            │
│  └──┬──┘        └──┬──┘        └──┬──┘        └──┬──┘            │
│     │              │              │              │                │
│     └──────────────┴──────────────┴──────────────┘                │
│                         │                                         │
│                    NCCL AllReduce                                │
│                    (Gradient Averaging)                          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Benefits of Multi-GPU Training

| Benefit | Description |
|---------|-------------|
| **Faster Training** | ~2-4× speedup with 4 GPUs (scales sub-linearly due to sync overhead) |
| **Larger Effective Batch** | Process more images per step for potentially better convergence |
| **Utilize All Hardware** | Make use of multiple GPUs efficiently |

**Note on Memory:** With standard DDP, each GPU holds a full copy of the model. However, gsplat is optimized to use 4× less memory than the original 3DGS implementation, making it more feasible to train on multiple GPUs. If you're running out of memory, use `--data_factor 2` to downscale images.

### Enabling Multi-GPU

#### Method 1: Distributed Wrapper (Recommended)

```bash
# Use all available GPUs
./gsplat-cuda-train-distributed -- default --data_dir /path/to/colmap_project --result_dir ./output

# Use specific GPUs
./gsplat-cuda-train-distributed --gpus 0,1,2,3 -- default --data_dir /path/to/colmap_project --result_dir ./output

# Custom port (for multiple jobs)
./gsplat-cuda-train-distributed --gpus 0,1 --port 29501 -- default --data_dir /path/to/data --result_dir ./output
```

#### Method 2: Environment Variables

```bash
# Set environment variables manually
export CUDA_VISIBLE_DEVICES=0,1,2,3
export MASTER_ADDR=localhost
export MASTER_PORT=29500

# Use torchrun directly
cd tools/gsplat-cuda
source venv/bin/activate
torchrun --nproc_per_node=4 examples/simple_trainer.py default --data_dir /path/to/data
```

#### Method 3: Single GPU Selection

```bash
# Use only GPU 2
CUDA_VISIBLE_DEVICES=2 ./gsplat-cuda-train default --data_dir /path/to/data --result_dir ./output
```

### Runtime Multi-GPU Options

| Option | Description | Example |
|--------|-------------|---------|
| `--gpus` | Comma-separated GPU IDs | `--gpus 0,1,2,3` |
| `--nproc` | Number of processes | `--nproc 4` |
| `--port` | Master port for NCCL | `--port 29501` |

## Training Strategies

### `default` - Standard 3DGS

The original 3D Gaussian Splatting algorithm:

```bash
./gsplat-cuda-train default \
    --data_dir /path/to/colmap_project \
    --result_dir ./results/my_scene
```

**Characteristics:**
- Gradient-based densification
- Good for most scenes
- Predictable training behavior

### `mcmc` - MCMC Densification

Monte Carlo Markov Chain based densification (often better quality):

```bash
./gsplat-cuda-train mcmc \
    --data_dir /path/to/colmap_project \
    --result_dir ./results/my_scene
```

**Characteristics:**
- Better Gaussian placement
- Often higher quality results
- May require more iterations

## Command Reference

### Training Options

| Option | Default | Description |
|--------|---------|-------------|
| `--data_dir` | Required | Path to COLMAP project |
| `--result_dir` | `./results` | Output directory |
| `--max_steps` | 30000 | Number of training steps |
| `--data_factor` | 1 | Downscale factor for images |
| `--test_every` | 1000 | Test/eval frequency |
| `--save_steps` | 7000 30000 | Steps at which to save |

### Common Configurations

```bash
# Quick preview (10 minutes)
./gsplat-cuda-train default \
    --data_dir ~/data/scene \
    --max_steps 7000 \
    --data_factor 2

# Standard quality (30-60 minutes)
./gsplat-cuda-train default \
    --data_dir ~/data/scene \
    --max_steps 30000

# High quality with MCMC (60+ minutes)
./gsplat-cuda-train mcmc \
    --data_dir ~/data/scene \
    --max_steps 50000

# Multi-GPU high quality
./gsplat-cuda-train-distributed --gpus 0,1,2,3 -- mcmc \
    --data_dir ~/data/scene \
    --max_steps 50000
```

## Performance Tuning

### Reducing Memory Usage

```bash
# Downscale images (2x = 4x less memory)
./gsplat-cuda-train default --data_dir ~/data --data_factor 2

# Reduce batch size (if supported)
./gsplat-cuda-train default --data_dir ~/data --batch_size 1
```

### Speed Optimization

```bash
# Use all GPUs for maximum speed
./gsplat-cuda-train-distributed --gpus 0,1,2,3,4 -- default --data_dir ~/data

# Compile for your specific GPU architecture
export TORCH_CUDA_ARCH_LIST="8.6"  # For RTX 30xx
```

### Best Practices

1. **Match GPU memory** - Use GPUs with similar memory for balanced load
2. **Use NVLink if available** - Faster GPU-to-GPU communication
3. **SSD/NVMe storage** - Faster image loading
4. **Pre-downscale large images** - Reduces I/O and memory

## Comparison with Other Tools

### gsplat vs OpenSplat

| Feature | gsplat CUDA | OpenSplat |
|---------|-------------|------------|
| **Multi-GPU** | Native DDP | Wrapper-based |
| **Memory Efficiency** | 4× less than original | Standard |
| **Training Speed** | 15% faster | Standard |
| **Quality** | Matches official 3DGS | Matches official 3DGS |
| **Flexibility** | High (Python) | Low (C++) |
| **Custom Training** | Easy | Hard |
| **Dependencies** | PyTorch + CUDA | LibTorch + CUDA |

### gsplat vs LichtFeld-Studio

| Feature | gsplat CUDA | LichtFeld-Studio |
|---------|-------------|------------------|
| **Multi-GPU** | ✅ Yes | ❌ No |
| **Training Speed** | Fast | Fastest |
| **Pose Optimization** | ❌ No | ✅ Yes |
| **Interactive GUI** | ❌ No | ✅ Yes |
| **Language** | Python | C++23 |
| **Flexibility** | High | Low |

### When to Use gsplat CUDA

✅ **Use gsplat when:**
- You have multiple GPUs and want true distributed training
- You need lower memory usage per GPU
- You want Python flexibility for custom training
- You're doing research and need to modify training

❌ **Don't use gsplat when:**
- You have a single powerful GPU (use LichtFeld instead)
- You need pose optimization (use LichtFeld)
- You want the absolute fastest training (use LichtFeld)

## Troubleshooting

### "CUDA out of memory"

```bash
# Reduce image resolution
./gsplat-cuda-train default --data_dir ~/data --data_factor 2

# Use more GPUs to distribute memory
./gsplat-cuda-train-distributed --gpus 0,1,2,3 -- default --data_dir ~/data
```

### "NCCL error" or "Connection refused"

```bash
# Use a different port
./gsplat-cuda-train-distributed --port 29501 -- default --data_dir ~/data

# Check if port is in use
lsof -i :29500
```

### "No module named 'gsplat'"

```bash
# Reinstall gsplat
cd tools/gsplat-cuda
source venv/bin/activate
pip install -e ".[dev]"
```

### "Build failed" during setup

```bash
# Ensure CUDA toolkit matches PyTorch CUDA version
nvcc --version
python -c "import torch; print(torch.version.cuda)"

# They should match (e.g., both CUDA 12.1)
```

### Slow Training

```bash
# Check GPU utilization
watch -n 1 nvidia-smi

# Should see high utilization on all GPUs
# If low, check I/O bottleneck (use SSD)
```

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `CUDA_VISIBLE_DEVICES` | Which GPUs to use | All |
| `MASTER_ADDR` | DDP master address | localhost |
| `MASTER_PORT` | DDP master port | 29500 |
| `NCCL_DEBUG` | NCCL debugging | OFF |
| `TORCH_CUDA_ARCH_LIST` | Target GPU architectures | Auto |

## Example: Training with 5× RTX 3090

```bash
# Setup
./scripts/setup_gsplat_cuda.sh

# Check GPUs
nvidia-smi
# 0: RTX 3090 (24GB)
# 1: RTX 3090 (24GB)
# 2: RTX 3090 (24GB)
# 3: RTX 3090 (24GB)
# 4: RTX 3090 (24GB)

# Multi-GPU training (use all 5)
./gsplat-cuda-train-distributed --gpus 0,1,2,3,4 -- mcmc \
    --data_dir ~/colmap_project \
    --result_dir ~/output \
    --max_steps 50000

# Training speed: ~3-4× faster than single GPU
```

---

## See Also

- [Quick Start Guide](QUICKSTART.md) - Getting started with Melkor
- [Pipeline Documentation](PIPELINE.md) - Complete pipeline reference  
- [OpenSplat Wrapper](OPENSPLAT_WRAPPER.md) - OpenSplat advanced features
- [LichtFeld Wrapper](LICHTFELD_WRAPPER.md) - LichtFeld-Studio options
