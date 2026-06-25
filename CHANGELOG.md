# Changelog

## 1.1.0 (2026-06-24)

### Security
- Fixed command injection in PythonBridge: `shellEscape()` helper applied to all `popen`/`system` call sites
- `runCode` now writes to temp file instead of `python -c "..."` to avoid shell quoting issues
- Fixed `predictFromGlb` view direction division by zero when camera coincides with cloud center
- `downloadWeights` now properly checks `WIFEXITED`/`WEXITSTATUS` on POSIX for `std::system` return value

### Correctness
- Fixed `logit()` division by zero: clamps input to `(1e-6, 1-1e-6)` preventing `-inf`/`+inf` at 0 and 1
- Fixed `sigmoid()` numerical stability: branch-on-sign avoids `exp` overflow for extreme values
- Fixed PLY ASCII writer `bytes_written` not tracking actual data size (was reporting header size only)
- Fixed Metal `initialize()` setting `initialized=true` when shader library loading failed (silent no-op)
- Fixed `computeAdaptiveScales` returning `min_scale` for all points: distances vector was never populated from candidates
- Fixed enhanced converter missing `ByteStride` for positions/normals/colors (assumed tightly packed)
- Fixed enhanced converter missing `UNSIGNED_SHORT`/`VEC4` color type handling
- Fixed `computeKnnDistances` returning zeros for >=10K points (implemented spatial hash k-NN path)
- Fixed `SpzEncodeConfig.sh_degree` being ignored: `toSpzCloud` now clamps and truncates SH-rest
- Fixed `computeAdaptiveScales` fixed search radius finding zero neighbors on sparse clouds

### Robustness
- Fixed `main.cpp` uncaught `stof`/`stoi` exceptions on invalid CLI args
- Fixed `main.cpp` silently ignoring unknown flags and extra positional args
- Fixed PLY ASCII reader not validating `stream >> vals[j]` per-property
- Fixed feedforward `predict()`/`predictMultiView()` not cleaning temp files on all error paths
- Fixed CUDA `processCloud` silently ignoring `convert_colors_to_sh`, `convert_opacity_to_logit`, `transform_y_up_to_z_up`
- Fixed `fromSpzCloud` missing bounds checks on positions/scales/rotations/alphas/colors arrays vs `numPoints`
- Fixed `predictMultiView` missing temp directory cleanup on early file-open failure
- Replaced deprecated `newLibraryWithFile:` with `newLibraryWithURL:` in gaussian_fitter.mm

### Build Quality
- Added `-Wall -Wextra -Wpedantic` warning flags to `melkor_core` and `melkor` targets
- Added `#ifndef` guards for `TINYGLTF_NO_STB_IMAGE_WRITE`/`TINYGLTF_NO_EXTERNAL_IMAGE` macro redefinitions
- Marked unused Metal shader constants as `[[maybe_unused]]`
- Fixed unused `SpatialHash::cell_size_` field and unused `indices` parameter warnings
- Stripped trailing whitespace from `main.cpp`

### CI
- Added ASan/UBSan sanitizer build job to CI workflow

### API Design
- Added `[[nodiscard]]` to all `GaussianCloud` accessors
- Documented `computeCovariances` scale space requirement (linear vs log)

### Testing
- Added logit/sigmoid round-trip test (mid-range invertibility, edge safety, extreme value stability)
- Added empty cloud bounding box safety test
- Added truncated binary PLY rejection test

## 1.0.0 (2025-12-21)

- Initial release
- GLB/PLY/SPZ conversion with Metal/CUDA acceleration
- OpenSplat, gsplat, gsplat-mps training backends
- Feedforward model support (DA3, Splatter-Image, MVSplat)
- COLMAP and GLOMAP SfM integration
- DA3 CoreML and ml-sharp integration
