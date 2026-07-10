> **Vendored snapshot:** these are upstream benchmark instructions for the
> captured DA3 source tree, not Melkor's supported inference contract. Model
> identifiers and dependencies may be stale; see `../../docs/DA3_FEEDFORWARD.md`.

# 📏 Visual Geometry Benchmark

This document provides comprehensive instructions for running benchmark evaluation on Depth Anything 3.

## ✨ Highlights

- 🗂️ **Diverse and Challenging Datasets**: 5 datasets (ETH3D, 7Scenes, ScanNet++, HiRoom, DTU) covering from objects to indoor and outdoor scenes. Part of datasets are recalibrated for high accuracy (see [ScanNet++](#scannet) details). All preprocessed datasets are uploaded to [depth-anything/DA3-BENCH](https://huggingface.co/datasets/depth-anything/DA3-BENCH).
- 🔧 **Robust Evaluation Pipeline**: Standardized pipeline featuring RANSAC-based pose alignment for better coordinate system alignment, TSDF fusion for directly reflecting depth 3D consistency.
- 📊 **Standardized Metrics**: Performance measured using established metrics: AUC for pose accuracy, F1-score and Chamfer Distance for reconstruction.

---

## 📑 Table of Contents

- [🚀 Quick Start](#quick-start)
- [📥 Dataset Download](#dataset-download)
- [⚙️ Evaluation Pipeline](#evaluation-pipeline)
- [🔧 Configuration](#configuration)
- [📊 Metrics](#metrics)
- [🗂️ Dataset Details](#dataset-details)
- [💻 Command Reference](#command-reference)
- [🔍 Troubleshooting](#troubleshooting)

---

## 🚀 Quick Start

### 1. Download Benchmark Data

> 💡 **Note:** Install HuggingFace CLI first: `pip install -U huggingface_hub[cli]`
>
> 🌐 **Mirror:** If download is slow, try: `export HF_ENDPOINT=https://hf-mirror.com`

```bash
cd da3_release

# Create directory and download from HuggingFace
mkdir -p workspace/benchmark_dataset
hf download depth-anything/DA3-BENCH \
    --local-dir workspace/benchmark_dataset \
    --repo-type dataset

# Extract all datasets
cd workspace/benchmark_dataset
for f in *.zip; do unzip -q "$f"; done
```

### 2. Run Evaluation

```bash
# Set model (default: depth-anything/DA3-GIANT)
MODEL=depth-anything/DA3-GIANT

# Full evaluation (all datasets, all modes)
python -m depth_anything_3.bench.evaluator model.path=$MODEL

# View results
python -m depth_anything_3.bench.evaluator eval.print_only=true
```

---

## 📥 Dataset Download

All benchmark datasets are hosted on HuggingFace: **[depth-anything/DA3-BENCH](https://huggingface.co/datasets/depth-anything/DA3-BENCH)**

| Dataset | File | Size | Description |
|---------|------|------|-------------|
| ETH3D | `eth3d.zip` | ~14.1 GB | High-resolution multi-view stereo (indoor/outdoor) |
| ScanNet++ | `scannetpp.zip` | ~10.1 GB | High-quality RGB-D indoor scenes |
| DTU-49 | `dtu.zip` | ~8.3 GB | Multi-view stereo benchmark (22 scenes × 49 views) |
| 7Scenes | `7scenes.zip` | ~3.3 GB | RGB-D indoor localization |
| DTU-64 | `dtu64.zip` | ~1.7 GB | DTU subset for pose evaluation (13 scenes × 64 views) |
| HiRoom | `hiroom.zip` | ~0.7 GB | High-resolution indoor rooms |

### Download Options

**Option 1: Download All (Recommended)**
```bash
hf download depth-anything/DA3-BENCH \
    --local-dir workspace/benchmark_dataset \
    --repo-type dataset
```

**Option 2: Download Specific Dataset**
```bash
# Download only HiRoom
hf download depth-anything/DA3-BENCH hiroom.zip \
    --local-dir workspace/benchmark_dataset \
    --repo-type dataset
```

**Option 3: Manual Download**

Visit [https://huggingface.co/datasets/depth-anything/DA3-BENCH](https://huggingface.co/datasets/depth-anything/DA3-BENCH) and download the zip files manually.

### Extract Datasets

```bash
cd workspace/benchmark_dataset

# Extract all
for f in *.zip; do unzip -q "$f"; done

# Or extract specific dataset
unzip hiroom.zip
```

### Expected Directory Structure

After extraction, your directory should look like:
```
workspace/benchmark_dataset/
├── eth3d/
│   ├── courtyard/
│   ├── electro/
│   └── ...
├── 7scenes/
│   └── 7Scenes/
│       ├── chess/
│       └── ...
├── scannetpp/
│   ├── 09c1414f1b/
│   └── ...
├── hiroom/
│   ├── data/
│   ├── fused_pcd/
│   └── selected_scene_list_val.txt
├── dtu/
│   ├── Rectified/
│   ├── Cameras/
│   ├── Points/
│   ├── SampleSet/
│   └── depth_raw/
└── dtu64/
    ├── Cameras/
    ├── scan105/
    └── ...
```

---

## ⚙️ Evaluation Pipeline



### Evaluation Modes

| Mode | Description | Metrics |
|------|-------------|---------|
| `pose` | Camera pose estimation | AUC@3°, AUC@30° |
| `recon_unposed` | 3D reconstruction with **predicted** poses | F-score, Overall |
| `recon_posed` | 3D reconstruction with **GT** poses | F-score, Overall |

### Basic Usage

```bash
cd da3_release
MODEL=depth-anything/DA3-GIANT

# Full evaluation (inference + evaluation + print results)
python -m depth_anything_3.bench.evaluator model.path=$MODEL

# Skip inference, only evaluate existing predictions
python -m depth_anything_3.bench.evaluator eval.eval_only=true

# Only print saved metrics
python -m depth_anything_3.bench.evaluator eval.print_only=true
```

### Selective Evaluation

```bash
# Evaluate specific datasets
python -m depth_anything_3.bench.evaluator model.path=$MODEL eval.datasets=[hiroom]

# Evaluate specific modes
python -m depth_anything_3.bench.evaluator model.path=$MODEL eval.modes=[pose,recon_unposed]

# Combine dataset and mode selection
python -m depth_anything_3.bench.evaluator model.path=$MODEL \
    eval.datasets=[hiroom] \
    eval.modes=[pose]
```

### 🖥️ Multi-GPU Inference

The evaluator automatically distributes inference across available GPUs:

```bash
# Use 4 GPUs
CUDA_VISIBLE_DEVICES=0,1,2,3 python -m depth_anything_3.bench.evaluator model.path=$MODEL

# Use all available GPUs (default)
python -m depth_anything_3.bench.evaluator model.path=$MODEL

# Single GPU
CUDA_VISIBLE_DEVICES=0 python -m depth_anything_3.bench.evaluator model.path=$MODEL
```

---

## 🔧 Configuration

### Config File

Default config: `src/depth_anything_3/bench/configs/eval_bench.yaml`

```yaml
# Model path
model:
  path: depth-anything/DA3-GIANT

# Workspace directory
workspace:
  work_dir: ./workspace/evaluation

# Evaluation settings
eval:
  datasets: [eth3d, 7scenes, scannetpp, hiroom, dtu, dtu64]
  modes: [pose, recon_unposed, recon_posed]
  max_frames: 100      # Max frames per scene (-1 = no limit)
  scenes: null         # Specific scenes (null = all)

# Inference settings
inference:
  num_fusion_workers: 4
  debug: false
```

### Output Structure

```
workspace/evaluation/
├── model_results/              # Inference outputs
│   ├── eth3d/
│   │   └── {scene}/
│   │       ├── unposed/       # Predictions for recon_unposed
│   │       └── posed/         # Predictions for recon_posed
│   ├── 7scenes/
│   ├── scannetpp/
│   ├── hiroom/
│   ├── dtu/
│   └── dtu64/
└── metric_results/             # Evaluation metrics (JSON)
    ├── eth3d_pose.json
    ├── eth3d_recon_unposed.json
    ├── eth3d_recon_posed.json
    └── ...
```

---

## 📊 Metrics

### 🎯 Pose Estimation

| Metric | Description |
|--------|-------------|
| **Auc3** | Area Under Curve at 3° angular error threshold |
| **Auc30** | Area Under Curve at 30° angular error threshold |

### 🏗️ 3D Reconstruction

| Metric | Description | Note |
|--------|-------------|------|
| **F-score** | Harmonic mean of Precision and Recall | Higher is better |
| **Overall** | (Accuracy + Completeness) / 2 | Lower is better (error in meters/mm) |

> **Note:** DTU reports Overall in millimeters; other datasets report in meters.

### Expected Results for DA3-GIANT

If your setup is correct, you should get the following results when evaluating the **DA3-GIANT** model:

```
========================================================
📊 SUMMARY
========================================================

🎯 POSE ESTIMATION
---------------------------------------------------------------------------------------
Metric         Avg         HiRoom      ETH3D       DTU-64      7Scenes     ScanNet++
---------------------------------------------------------------------------------------
Auc3           0.6705      0.8030      0.4872      0.9408      0.2744      0.8470
Auc30          0.9436      0.9592      0.9153      0.9939      0.8668      0.9827

🏗️  RECON_UNPOSED (Pred Pose)
---------------------------------------------------------------------------------------
Metric         Avg*        HiRoom      ETH3D       DTU         7Scenes     ScanNet++
---------------------------------------------------------------------------------------
F-score        0.7345      0.8629      0.7876      N/A         0.5043      0.7831
Overall        0.1682      0.0457      0.4366      1.7927      0.1230      0.0676

🏗️  RECON_POSED (GT Pose)
---------------------------------------------------------------------------------------
Metric         Avg*        HiRoom      ETH3D       DTU         7Scenes     ScanNet++
---------------------------------------------------------------------------------------
F-score        0.7978      0.9546      0.8685      N/A         0.5635      0.8045
Overall        0.1408      0.0213      0.3679      1.7488      0.1092      0.0649

* Avg F-score / Overall = average over HiRoom, ETH3D, 7Scenes, ScanNet++ (4 datasets)
```

---

## 🗂️ Dataset Details

### ETH3D

High-resolution multi-view stereo benchmark with laser-scanned ground truth.

- **Scenes:** 11 (courtyard, electro, kicker, pipes, relief, delivery_area, facade, office, playground, relief_2, terrains)
- **Resolution:** Variable (high-res DSLR images)
- **GT:** Laser-scanned meshes + depth maps

> **⚠️ Image Filtering:** Some images with unusual camera rotations are filtered out for stable evaluation. See `ETH3D_FILTER_KEYS` in `constants.py`.

### 7Scenes

RGB-D dataset for camera relocalization.

- **Scenes:** 7 (chess, fire, heads, office, pumpkin, redkitchen, stairs)
- **Resolution:** 640×480
- **GT:** Poses from KinectFusion, meshes from TSDF fusion

### ScanNet++

High-quality indoor RGB-D dataset with dense annotations.

- **Scenes:** 20 validation scenes
- **Resolution:** 768×1024 (after undistortion)
- **GT:** High-quality meshes from FARO scanner

> **⚠️ Camera Pose Re-calibration:** The default ScanNet++ poses are often inaccurate due to motion blur and textureless frames from iPhone captures. We re-ran COLMAP with the following improvements:
> - **Frame filtering:** Removed blurry images during frame extraction
> - **Fisheye calibration:** Jointly calibrated fisheye camera for wider FOV and better accuracy
> - **Exhaustive matching:** Used COLMAP's exhaustive matcher and mapper for reliable poses (takes several days per scene but necessary for quality)
> - All processed scenes are available at [haotongl/scannetpp_zipnerf](https://huggingface.co/datasets/haotongl/scannetpp_zipnerf)

### HiRoom

Indoor room scenes with high-resolution RGB-D data.

- **Scenes:** 24 validation scenes
- **GT:** Fused point clouds

### DTU-49 (Reconstruction Only)

Multi-view stereo benchmark following MVSNet evaluation protocol.

- **Scenes:** 22 evaluation scenes
- **Views:** 49 images per scene
- **GT:** Laser-scanned point clouds with observation masks
- **Metrics:** Overall only (accuracy + completeness in mm)

### DTU-64 (Pose Only)

DTU subset for pose estimation evaluation.

- **Scenes:** 13 scenes
- **Views:** 64 images per scene
- **Metrics:** AUC@3°, AUC@30°

> **Why two DTU settings?**
> - **DTU-64** (pose): More views = more challenging pose estimation
> - **DTU-49** (recon): Standard MVSNet protocol for fair comparison with MVS methods

---

## 💻 Command Reference

```
python -m depth_anything_3.bench.evaluator [OPTIONS] [KEY=VALUE ...]

Configuration:
  --config PATH                      Config YAML file (default: bench/configs/eval_bench.yaml)

Config Overrides (using dotlist notation):
  model.path=VALUE                   Model path or HuggingFace ID
  workspace.work_dir=VALUE           Working directory for outputs
  eval.datasets=[dataset1,dataset2]  Datasets to evaluate (eth3d,7scenes,scannetpp,hiroom,dtu,dtu64)
  eval.modes=[mode1,mode2]           Evaluation modes (pose,recon_unposed,recon_posed)
  eval.scenes=[scene1,scene2]        Specific scenes to evaluate (null=all)
  eval.max_frames=VALUE              Max frames per scene (-1=no limit, default: 100)
  eval.ref_view_strategy=VALUE       Reference view strategy (default: first)
  eval.eval_only=VALUE               Only run evaluation (skip inference) (true/false)
  eval.print_only=VALUE              Only print saved metrics (true/false)
  inference.num_fusion_workers=VALUE Number of parallel workers (default: 4)
  inference.debug=VALUE              Enable debug mode (true/false)

Special Flags:
  --help, -h                         Show this help message

Multi-GPU:
  Use CUDA_VISIBLE_DEVICES to specify GPUs (auto-detected and distributed)
```

### Examples

```bash
MODEL=depth-anything/DA3-GIANT

# Full evaluation
python -m depth_anything_3.bench.evaluator model.path=$MODEL

# Quick test on HiRoom only
python -m depth_anything_3.bench.evaluator \
    model.path=$MODEL \
    eval.datasets=[hiroom] \
    eval.modes=[pose]

# Pose-only evaluation (all 5 pose datasets)
python -m depth_anything_3.bench.evaluator \
    model.path=$MODEL \
    eval.datasets=[eth3d,7scenes,scannetpp,hiroom,dtu64] \
    eval.modes=[pose]

# Recon-only evaluation (all 5 recon datasets)
python -m depth_anything_3.bench.evaluator \
    model.path=$MODEL \
    eval.datasets=[eth3d,7scenes,scannetpp,hiroom,dtu] \
    eval.modes=[recon_unposed,recon_posed]

# Debug specific scenes
python -m depth_anything_3.bench.evaluator \
    model.path=$MODEL \
    eval.datasets=[eth3d] \
    eval.scenes=[courtyard] \
    inference.debug=true

# Re-evaluate without re-running inference
python -m depth_anything_3.bench.evaluator eval.eval_only=true

# Just view results
python -m depth_anything_3.bench.evaluator eval.print_only=true
```

---

## 🔍 Troubleshooting

### Data Path Issues

Ensure dataset paths in `src/depth_anything_3/utils/constants.py` are correct:

```python
# Default paths (relative to project root)
ETH3D_EVAL_DATA_ROOT = "workspace/benchmark_dataset/eth3d"
SEVENSCENES_EVAL_DATA_ROOT = "workspace/benchmark_dataset/7scenes"
SCANNETPP_EVAL_DATA_ROOT = "workspace/benchmark_dataset/scannetpp"
HIROOM_EVAL_DATA_ROOT = "workspace/benchmark_dataset/hiroom/data"
DTU_EVAL_DATA_ROOT = "workspace/benchmark_dataset/dtu"
DTU64_EVAL_DATA_ROOT = "workspace/benchmark_dataset/dtu64"
```

---

## 📝 Citation

If you find this benchmark useful, please cite:

```
@article{depthanything3,
  title={Depth Anything 3: Recovering the visual space from any views},
  author={Haotong Lin and Sili Chen and Jun Hao Liew and Donny Y. Chen and Zhenyu Li and Guang Shi and Jiashi Feng and Bingyi Kang},
  journal={arXiv preprint arXiv:2511.10647},
  year={2025}
}
```

Please also cite the original dataset papers for each benchmark you use.

---

## 📄 License

The benchmark datasets are provided for research purposes only. Users must follow the original licenses of each dataset:

- **ETH3D:** [https://www.eth3d.net/](https://www.eth3d.net/)
- **7Scenes:** [Microsoft Research](https://www.microsoft.com/en-us/research/project/rgb-d-dataset-7-scenes/)
- **ScanNet++:** [http://www.scan-net.org/](http://www.scan-net.org/)
- **DTU:** [https://roboimagedata.compute.dtu.dk/](https://roboimagedata.compute.dtu.dk/)
- **HiRoom:** [SVLightVerse](https://jerrypiglet.github.io/SVLightVerse/)
