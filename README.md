# Melkor

A cross-platform toolkit for 3D Gaussian Splatting on macOS and Linux. Converts GLB meshes to Gaussian splats (PLY/SPZ), and provides training pipelines via OpenSplat, gsplat, and feedforward models (DA3).

## Quick Start

```bash
# Build the converter
chmod +x scripts/setup_deps.sh && ./scripts/setup_deps.sh
mkdir -p build && cd build
cmake .. && make -j$(sysctl -n hw.ncpu)

# Convert a mesh to splats
./melkor input.glb output.ply
```

### Training from photos

```bash
# Full setup + training pipeline
chmod +x scripts/setup_all.sh && ./scripts/setup_all.sh
./scripts/train_from_images.sh ~/Photos/my_scene ~/output/my_scene
```

### Feedforward (no COLMAP, seconds)

```bash
./scripts/setup_da3.sh
./da3-infer --input photo.jpg --output scene.ply
```

## Features

- **Conversion modes**: Basic (fast vertex-to-splat), Enhanced (adaptive scale/surface alignment), Fit (differentiable rendering), Feedforward (neural networks)
- **Format support**: GLB/glTF -> PLY/SPZ, PLY <-> SPZ
- **GPU**: Metal (macOS), CUDA (Linux), CPU fallback
- **Training**: OpenSplat, gsplat (CUDA DDP), gsplat-mps (macOS), LichtFeld-Studio (Linux)
- **Feedforward**: DA3 (DINOv2 transformer, any image count), Splatter-Image, MVSplat
- **SfM**: COLMAP, GLOMAP (10-100x faster)

## Requirements

| Platform | Requirements |
|----------|-------------|
| macOS 13+ | Xcode CLT, CMake 3.20+, Apple Silicon for Metal |
| Linux | GCC 7.5+/Clang 6.0+, CMake 3.20+, CUDA 11.0+ (optional) |

## Usage

### Format conversion
```bash
./build/melkor model.glb output.ply              # Basic
./build/melkor model.glb output.ply --enhanced    # Enhanced
./build/melkor model.glb output.ply --fit --iterations 5000  # Fit
./build/melkor scene.ply scene.spz               # Compress ~90% with SPZ
```

### Training
```bash
./scripts/train_from_images.sh /path/to/photos /path/to/output
./scripts/opensplat_wrapper.sh /path/to/colmap/project --gpu-ids 0,1,2,3 -o output.ply
./da3-infer --input images/ --output scene.ply
```

## Project Structure

```
melkor/
  include/melkor/  C++ headers
  src/             C++ source + Metal/CUDA kernels
  tools/da3/       DA3 feedforward inference
  DA3coreml/       Depth Anything 3 (ByteDance)
  ml-sharp/        Apple ML research
  scripts/         Setup and training scripts
  third_party/     Vendored deps
  docs/            Documentation
```

## GPU Backends

| Platform | Backend | Enable |
|----------|---------|--------|
| macOS (Apple Silicon) | Metal | Default |
| Linux (NVIDIA) | CUDA | `-DMELKOR_USE_CUDA=ON` |
| Any | CPU | Fallback |

## License

MIT License (core Melkor code). See [LICENSE](LICENSE), [NOTICE](NOTICE), and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES.md) for included third-party components (Apache-2.0, AGPL-3.0, Apple Sample Code, MIT).

## References

- [3D Gaussian Splatting](https://github.com/graphdeco-inria/gaussian-splatting)
- [OpenSplat](https://github.com/pierotofy/OpenSplat)
- [gsplat](https://github.com/nerfstudio-project/gsplat)
- [SPZ File Format](https://scaniverse.com/news/spz-gaussian-splat-open-source-file-format)
- [DA3: Any-View Depth Anything](https://arxiv.org/abs/2511.10647)
- [Depth Anything 3](https://github.com/ByteDance-Seed/Depth-Anything-3)
- [ml-sharp](https://github.com/apple/ml-sharp)
- [COLMAP](https://colmap.github.io/)
- [GLOMAP](https://github.com/colmap/glomap)
