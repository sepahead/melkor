# GLOMAP - Global Structure-from-Motion

GLOMAP is a global Structure-from-Motion (SfM) pipeline that serves as a faster alternative to COLMAP's incremental SfM. It produces COLMAP-compatible sparse reconstructions 10-100× faster while maintaining equal or better quality.

**Platform:** Linux, macOS, Windows

## Table of Contents

- [Overview](#overview)
- [Where GLOMAP Fits in the Pipeline](#where-glomap-fits-in-the-pipeline)
- [Quick Start](#quick-start)
- [Installation](#installation)
- [Usage](#usage)
- [Comparison with COLMAP](#comparison-with-colmap)
- [Command Reference](#command-reference)
- [Troubleshooting](#troubleshooting)

## Overview

### What is GLOMAP?

GLOMAP (Global Mapper) is a global SfM system developed by the COLMAP team. While COLMAP uses incremental reconstruction (adding images one at a time), GLOMAP performs global optimization of all cameras simultaneously.

### Key Features

| Feature | Description |
|---------|-------------|
| **Speed** | 10-100× faster than COLMAP's incremental SfM |
| **Quality** | Equal or better reconstruction quality |
| **Compatibility** | Outputs standard COLMAP format |
| **Integration** | Uses COLMAP for feature extraction and matching |

### How GLOMAP Works

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    GLOMAP vs COLMAP SfM                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  COLMAP INCREMENTAL SfM:                                                   │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                                                                      │   │
│  │  Image 1 → Register → Image 2 → Register → Image 3 → ... → Image N │   │
│  │     ↓                    ↓                    ↓                      │   │
│  │  Bundle    Bundle       Bundle    Bundle     Bundle    ...          │   │
│  │  Adjust    Adjust       Adjust    Adjust     Adjust                 │   │
│  │                                                                      │   │
│  │  Time Complexity: O(N³) or worse (grows with each image)           │   │
│  │                                                                      │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  GLOMAP GLOBAL SfM:                                                        │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                                                                      │   │
│  │  All Images ──▶ Global Rotation ──▶ Global Position ──▶ Bundle Adj │   │
│  │                   Averaging           Triangulation                  │   │
│  │                                                                      │   │
│  │  Time Complexity: O(N) - scales linearly with images                │   │
│  │                                                                      │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Where GLOMAP Fits in the Pipeline

### Melkor Pipeline Architecture

GLOMAP replaces only the **mapping/reconstruction** step of COLMAP, not the feature extraction and matching:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    MELKOR PIPELINE WITH GLOMAP                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  STAGE 1: Feature Extraction (COLMAP)                                      │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  Images → COLMAP feature_extractor → database.db (SIFT features)   │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                              ↓                                              │
│  STAGE 2: Feature Matching (COLMAP)                                        │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  database.db → COLMAP exhaustive/sequential/vocab_tree_matcher     │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                              ↓                                              │
│  STAGE 3: Mapping/Reconstruction (GLOMAP or COLMAP)                        │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                                                                      │   │
│  │  Option A: COLMAP mapper (incremental)                              │   │
│  │    colmap mapper --database_path db --image_path imgs --output out  │   │
│  │    Time: 10-60+ minutes for large datasets                          │   │
│  │                                                                      │   │
│  │  Option B: GLOMAP mapper (global)                                   │   │
│  │    glomap mapper --database_path db --image_path imgs --output out  │   │
│  │    Time: 1-5 minutes for same datasets (10-100× faster)            │   │
│  │                                                                      │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                              ↓                                              │
│  STAGE 4: 3D Gaussian Splatting Training                                   │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  sparse/ + images/ → OpenSplat/LichtFeld/gsplat → point_cloud.ply  │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### When to Use GLOMAP vs COLMAP

| Scenario | Recommended | Why |
|----------|-------------|-----|
| **Large datasets (500+ images)** | GLOMAP | 10-100× faster, same quality |
| **Quick iteration/testing** | GLOMAP | Faster turnaround |
| **Production pipeline** | GLOMAP | Consistent fast results |
| **Small datasets (<50 images)** | Either | Both fast enough |
| **Difficult scenes (few features)** | COLMAP | More robust incremental approach |
| **Need undistortion** | COLMAP | COLMAP has image_undistorter |

### Output Compatibility

GLOMAP outputs are **100% compatible** with COLMAP format:

```
sparse/
└── 0/
    ├── cameras.bin    # Camera intrinsics (focal length, distortion)
    ├── images.bin     # Camera poses (position, orientation)
    └── points3D.bin   # Sparse 3D points
```

All downstream tools (OpenSplat, LichtFeld, gsplat, etc.) work unchanged.

## Quick Start

```bash
# 1. Install GLOMAP
./scripts/setup_glomap.sh

# 2. Use GLOMAP in the pipeline (instead of COLMAP for mapping)
./scripts/pipeline.sh ~/Photos/scene ~/output/ --sfm glomap

# 3. Or use the wrapper directly
./scripts/glomap_wrapper.sh ~/Photos/scene ~/output/
```

## Installation

### Prerequisites

GLOMAP depends on COLMAP for feature extraction and matching:

| Requirement | Minimum | Notes |
|-------------|---------|-------|
| **COLMAP** | Any version | Required for features/matching |
| **CMake** | 3.28+ | For FetchContent, or 3.10+ otherwise |
| **C++ Compiler** | C++17 support | GCC 7+, Clang 6+ |
| **Ninja** | Recommended | Installed by the setup script if missing |
| **PoseLib** | Auto-fetched | Or install manually |

The setup script installs COLMAP itself if it is not found, and installs the remaining build dependencies (Eigen, Ceres, gflags, glog) via apt (Linux) or Homebrew (macOS).

### Setup

```bash
# Install GLOMAP and dependencies
chmod +x scripts/setup_glomap.sh
./scripts/setup_glomap.sh

# Verify installation
./glomap --help
```

### Manual Installation

If you prefer manual installation:

```bash
# 1. Install COLMAP first (for features/matching)
# macOS:
brew install colmap

# Ubuntu:
sudo apt-get install colmap

# 2. Clone and build GLOMAP
git clone https://github.com/colmap/glomap.git
cd glomap
mkdir build && cd build
cmake .. -GNinja
ninja && sudo ninja install
```

## Usage

### Via Pipeline (Recommended)

```bash
# Use GLOMAP for SfM (feature extraction/matching still use COLMAP)
./scripts/pipeline.sh ~/Photos/scene ~/output/ --sfm glomap

# Force COLMAP for SfM (default behavior)
./scripts/pipeline.sh ~/Photos/scene ~/output/ --sfm colmap

# With quality preset
./scripts/pipeline.sh ~/Photos/scene ~/output/ --sfm glomap --quality high
```

See [PIPELINE.md](PIPELINE.md) for the full pipeline reference.

### Via Wrapper Script

```bash
# Basic usage
./scripts/glomap_wrapper.sh ~/Photos/scene ~/output/

# With custom COLMAP matching
./scripts/glomap_wrapper.sh ~/Photos/scene ~/output/ --matcher sequential

# Large-scale dataset
./scripts/glomap_wrapper.sh ~/Photos/scene ~/output/ --matcher vocab_tree

# Existing COLMAP database (GLOMAP mapping only)
./scripts/glomap_wrapper.sh ~/project/ ~/output/ --skip-features --skip-matching
```

### Direct GLOMAP Usage

If you have an existing COLMAP database:

```bash
# Run GLOMAP mapper on existing database
glomap mapper \
    --database_path /path/to/database.db \
    --image_path /path/to/images \
    --output_path /path/to/sparse

# The output can be used directly with OpenSplat, LichtFeld, etc.
./opensplat /path/to/sparse -o output.ply
```

### Full Manual Workflow

```bash
# Step 1: Feature extraction (COLMAP)
colmap feature_extractor \
    --image_path ~/Photos/scene \
    --database_path ~/project/database.db

# Step 2: Feature matching (COLMAP)
# For small datasets (<500 images):
colmap exhaustive_matcher --database_path ~/project/database.db

# For large datasets:
colmap sequential_matcher --database_path ~/project/database.db
# OR
colmap vocab_tree_matcher --database_path ~/project/database.db

# Step 3: Global SfM (GLOMAP)
glomap mapper \
    --database_path ~/project/database.db \
    --image_path ~/Photos/scene \
    --output_path ~/project/sparse

# Step 4: Train 3DGS
./opensplat ~/project -o output.ply
```

## Comparison with COLMAP

### Speed Benchmarks

| Dataset | Images | COLMAP Time | GLOMAP Time | Speedup |
|---------|--------|-------------|-------------|---------|
| Small indoor | 50 | 2 min | 15 sec | 8× |
| Medium outdoor | 200 | 15 min | 1 min | 15× |
| Large building | 500 | 45 min | 2 min | 22× |
| City-scale | 2000 | 4+ hours | 8 min | 30×+ |

*Benchmarks from GLOMAP paper. Actual performance varies by hardware and scene complexity.*

### Quality Comparison

| Metric | COLMAP | GLOMAP |
|--------|--------|--------|
| **Reprojection Error** | Baseline | Equal or better |
| **Completeness** | High | High |
| **Registered Images** | ~95-99% | ~95-99% |
| **Point Cloud Density** | Baseline | Similar |

### Feature Comparison

| Feature | COLMAP | GLOMAP |
|---------|--------|--------|
| Feature extraction | Yes (SIFT, etc.) | No (uses COLMAP) |
| Feature matching | Yes (multiple matchers) | No (uses COLMAP) |
| Incremental SfM | Yes | No |
| Global SfM | No | Yes |
| Image undistortion | Yes | No (use COLMAP) |
| Dense reconstruction | Yes | No (use COLMAP) |
| GUI | Yes | No |

## Command Reference

### glomap mapper

The main reconstruction command:

```bash
glomap mapper [options]
```

| Option | Description | Required |
|--------|-------------|----------|
| `--database_path` | Path to COLMAP database | Yes |
| `--image_path` | Path to images | Yes |
| `--output_path` | Output directory for sparse model | Yes |

### Wrapper Script Options

```bash
./scripts/glomap_wrapper.sh <input_images> <output_dir> [options]
```

`<input_images>` is a directory of images, or a directory containing an existing COLMAP `database.db` when `--skip-features --skip-matching` is used.

| Option | Default | Description |
|--------|---------|-------------|
| `--matcher <type>` | `exhaustive` | COLMAP matcher: `exhaustive` (<500 images), `sequential` (video/ordered images), `vocab_tree` (1000+ images) |
| `--quality <level>` | `medium` | Feature extraction quality: `low` (4096 SIFT features), `medium` (8192), `high` (16384) |
| `--gpu <mode>` | `auto` | GPU for COLMAP feature extraction/matching: `auto`, `0` (disable), `1` (enable). `auto` enables the GPU only on Linux with `nvidia-smi` present |
| `--skip-features` | Off | Skip feature extraction (requires an existing `database.db` in the input directory) |
| `--skip-matching` | Off | Skip feature matching (use existing matches in the database) |
| `--verbose, -v` | Off | Verbose output |
| `--dry-run` | Off | Show commands without executing |
| `--help, -h` | | Show help |

Notes:

- Supported input formats: JPG, JPEG, PNG, TIFF, TIF, BMP. HEIC/HEIF files are converted to JPEG automatically (via `sips` on macOS or ImageMagick `convert`). At least 3 images are required; 20+ are recommended.
- With `--matcher vocab_tree`, the vocabulary tree is looked up in `~/.colmap/` and `/usr/share/colmap/`, and downloaded to `~/.colmap/vocab_tree_flickr100K_words256K.bin` if not found.
- The output directory will contain `database.db`, `images/` (symlinked or copied images), and `sparse/0/` (the GLOMAP reconstruction).

## Troubleshooting

### "glomap: command not found"

```bash
# Ensure GLOMAP is installed (creates a ./glomap wrapper in the repo root)
./scripts/setup_glomap.sh

# Verify
./glomap --help
```

The wrapper script searches for the binary in this order: `./glomap` (repo root), `tools/glomap/build/glomap/glomap`, `tools/glomap/build/glomap`, `PATH`, `/usr/local/bin/glomap`.

### "Empty sparse reconstruction"

This usually means feature matching failed:

1. **Check image overlap**: Need 60-80% overlap between images
2. **Try different matcher**:
   ```bash
   ./scripts/glomap_wrapper.sh ~/Photos/scene ~/output/ --matcher sequential
   ```
3. **Check for motion blur**: Remove blurry images

### "Database not found"

GLOMAP requires a COLMAP database with features and matches:

```bash
# Create database first
colmap feature_extractor --image_path images/ --database_path database.db
colmap exhaustive_matcher --database_path database.db

# Then run GLOMAP
glomap mapper --database_path database.db --image_path images/ --output_path sparse/
```

### "CMake version too old"

GLOMAP's FetchContent-based build requires CMake 3.28+. The setup script warns and attempts the build with older versions, which then requires manually installed dependencies. To upgrade:

```bash
# Install newer CMake
# Ubuntu:
pip install cmake --upgrade

# macOS:
brew install cmake
```

### GLOMAP vs COLMAP: Which to Choose?

```
Is your dataset > 200 images?
├── YES → Use GLOMAP (much faster)
└── NO → Is time critical?
    ├── YES → Use GLOMAP
    └── NO → Either works fine

Is the scene difficult (few features, repetitive patterns)?
├── YES → Try COLMAP first (more robust)
└── NO → Use GLOMAP

Do you need image undistortion?
├── YES → Run COLMAP image_undistorter after GLOMAP
└── NO → Use GLOMAP directly
```

## Integration with Training Tools

### OpenSplat

```bash
# GLOMAP output works directly with OpenSplat
./scripts/glomap_wrapper.sh ~/Photos/scene ~/project/
./opensplat ~/project/ -o output.ply
```

### LichtFeld-Studio

```bash
# GLOMAP output works directly with LichtFeld
./scripts/glomap_wrapper.sh ~/Photos/scene ~/project/
./lichtfeld -d ~/project/ -o ~/output/
```

### gsplat

```bash
# GLOMAP output works directly with gsplat
./scripts/glomap_wrapper.sh ~/Photos/scene ~/project/
./gsplat-cuda-train default --data_dir ~/project/ --result_dir ~/output/
```

## References

- [GLOMAP GitHub Repository](https://github.com/colmap/glomap)
- [COLMAP Documentation](https://colmap.github.io/)

---

## See Also

- [Quick Start Guide](QUICKSTART.md) - Getting started with Melkor
- [Pipeline Documentation](PIPELINE.md) - Complete pipeline reference
- [OpenSplat Wrapper](OPENSPLAT_WRAPPER.md) - OpenSplat training options
- [LichtFeld Wrapper](LICHTFELD_WRAPPER.md) - LichtFeld-Studio options
