# Changelog

## 1.2.1 (2026-07-07)

### Fixed
- **Metal covariance transpose**: the `compute_covariances` Metal kernel built the rotation matrix from row vectors passed to a column-major `float3x3` constructor, computing R^T·S·S^T·R instead of R·S·S^T·R^T — wrong for any anisotropic, non-axis-aligned splat (verified: 3.3 max error vs a numpy reference; now 5e-7). Latent (no pipeline caller yet, and the existing test used only an identity rotation, which hides a transpose), but a public-API defect; now covered by a non-identity covariance case and a CPU/Metal covariance parity test.
- **PLY higher-order SH silently dropped**: `PlyWriteConfig::include_sh_rest` defaults false and the CLI never set it, so any degree>0 input (e.g. a degree-3 SPZ scene) lost all view-dependent color on PLY output while the SPZ path preserved it. The CLI now enables it when `shDegree > 0`; verified end to end (PLY→SPZ→PLY preserves all coefficients within quantization) and locked by SH round-trip tests for degrees 1/2/3.
- Curved-surface scene completion behavior is now measured and regression-tested (`test_fills_sphere_cap`): a sphere cap closes within the pass budget with fills bulging outward by at most ~2 median spacings; documented in docs/SCENE_COMPLETION.md.

## 1.2.0 (2026-07-07)

### Added
- `viewer/`: a self-contained SparkJS + THREE.js web viewer for Gaussian splats. Ships a curated set of real-world **city/area** scenes covering all four containers SparkJS auto-detects (SPZ, SOG `.zip`, SPLAT, PLY), with scene switcher, named camera feeds, auto-orbit, keyboard/mouse fly controls, dynamic scene-availability probing, and opacity-weighted auto-framing (works around Spark `getBoundingBox()` returning `NaN` for packed scenes).
- The PLY scene is produced by dogfooding Melkor's own `SPZ → PLY` converter, which both exercises Spark's `.ply` path and validates Melkor output end to end.
- `viewer/tests/`: Playwright render-test suite that loads each scene in headless Chromium, asserts WebGL2 + non-blank framebuffers across all camera feeds, verifies camera motion, and writes per-view screenshots; the optional PLY scene is skipped when not generated.
- `viewer/fetch-assets.sh`: idempotent downloader for the viewer's runtime libs and splat scenes, with optional Melkor-driven PLY generation (all artifacts gitignored).
- `viewer/src-tauri/`: a Tauri v2 desktop shell that wraps the viewer as a native app (WKWebView/WebKitGTK/WebView2). `bun run app` runs it in dev against the bun static server (`serve.js`); `bun run app:build` bundles a lean app via a staged `dist/`.
- **Scene completion (densification / hole filling)** — the 3DGS counterpart of inpainting: `--fill-holes` (+ `--fill-iterations`, `--fill-strength`, `--max-hole-size`) fills occlusion holes and densifies sparse regions of any loadable splat cloud via advancing-front rim extrapolation with a far-support gate (interior holes get bridged, the outer boundary never grows) plus major-axis splitting for under-dense interiors. Deterministic, prior-free, CPU/Metal-identical. New `Densifier` API (`include/melkor/densifier.hpp`), docs in `docs/SCENE_COMPLETION.md`, full test suite in `tests/test_densifier.cpp`.
- `ComputeProvider` abstraction (`include/melkor/compute_provider.hpp`) unifying Metal/CUDA/CPU behind one interface with runtime fallback; the CLI no longer `#ifdef`-dispatches per backend. New backend-parity test suite `tests/test_compute_provider.cpp`.
- Grid-accelerated Metal k-NN (`knn_stats_grid`, `filter_candidates_grid` kernels backed by a host-built uniform grid, `include/melkor/spatial_grid.hpp`): enhanced conversion and scene completion now stay on the GPU for clouds of any size instead of falling back to CPU above 10K points.
- CUDA parity for the grid kernels: `knnStatsGridKernel` / `filterCandidatesGridKernel` mirror the Metal and CPU implementations cell-for-cell, so scene completion and enhanced-conversion k-NN are GPU-accelerated on Linux/NVIDIA too. `Densifier` gained a backend-agnostic `ComputeProvider` constructor that routes to Metal, CUDA, or CPU automatically; the legacy `namespace metal = cuda` alias in `cuda_compute.hpp` (which collided with the real Metal-stub namespace) is removed.
- `melkor --version`, single-sourced from the CMake project version (which previously still read 1.0.0).
- Regression tests for the SPZ SH channel-order fix (verified against the canonical spz decoder, so a symmetric transpose bug cannot hide) and for short colors/normals arrays and single-point input in the enhanced converter.
- CI: a CUDA compile-only Linux job (catches CUDA build breakage without a GPU), a CPU-only (`-DMELKOR_USE_METAL=OFF`) macOS build+test step covering the stub link topology, and `-DMELKOR_WERROR=ON` on the build jobs. First-party code now compiles warning-clean including the Metal target, which previously had no warning flags at all.
- Repo hygiene: issue forms and a PR template mirroring the contributor checklist, `.gitattributes` (vendored trees marked for GitHub language stats, LF normalization), `.clang-format` and `.clang-tidy` advisory configs, and rewritten `CONTRIBUTING.md` / `SECURITY.md` policies.

### Fixed
- **Cross-platform link topology**: Linux/CUDA and CPU-only builds could not link — `gaussian_processor.cu` had been dropped from the CUDA target, the Metal-API stubs were missing `enhancedConvert`/`computeKnnDistancesMetal`, the CUDA branch linked no Metal stubs at all, and tests never linked the stub library. The platform GPU library is now a PUBLIC dependency of `melkor_core` on every platform.
- **Metal memory leaks**: the Objective-C++ sources assumed ARC but were compiled without `-fobjc-arc`; every GPU buffer/pipeline leaked. ARC is now enabled for all Metal targets.
- **SPZ SH ordering**: higher-order SH coefficients were written to (and read from) SPZ in PLY's channel-major order instead of SPZ's channel-interleaved order, garbling view-dependent color in external SPZ consumers. Encode/decode now transpose correctly (round-trips through Melkor were unaffected).
- **Backend consistency**: CPU Y-up→Z-up was a reflection (mirrored geometry) while Metal/CUDA rotate; CUDA `processCloud` color/opacity conversions were identity no-ops; quaternion normalization now canonicalizes sign (w ≥ 0) identically on CPU/Metal/CUDA/stub.
- **GPU failure contract**: `enhancedConvert` returned zero-filled (degenerate) gaussians on command-buffer failure and reported success; it now returns empty and the converter falls back to CPU. `computeCovariances` gained the same pipeline/status guards.
- **Metal robustness**: optional normals/colors are no longer bound as nil buffers (dummy buffer + `has_normals`/`has_colors` flags); the enhanced-convert kernel honors `use_vertex_colors`/`default_color`; mixed-attribute multi-primitive GLBs no longer cause out-of-bounds attribute reads on either the CPU or GPU path (short arrays are padded).
- **GLB reader hardening**: full bounds validation of accessors/bufferViews/buffers/strides on untrusted input (malformed GLBs previously caused out-of-bounds reads or crashes); component types are now checked (float positions/normals; float/normalized ubyte/ushort colors, VEC3 or VEC4). Same validation applied to the Gaussian-fitter's GLB path, which also honors `byteStride` for interleaved exports.
- **PLY reader**: PLY 1.0 sized type aliases (`float32`, `float64`, `uint8`, …) parse with correct strides and conversions; `binary_big_endian` files are byte-swapped instead of silently mis-read; malformed vertex counts return clean errors instead of crashing; CRLF headers and `end_header` inside comments parse correctly.
- **Pipeline scripts**: log output no longer corrupts command-substitution results (silently skipped COLMAP runs, garbage trainer paths, masked training failures); `${VERBOSE:+…}` no longer always expands; `train_from_images.sh` image counting matches what is actually copied and resolves relative output paths; setup scripts invoked by CI/docs are executable.
- **Feedforward weights download**: HTML error pages are now rejected (curl `-f`, content sniffing, size sanity) and partial files deleted instead of being reported as successfully installed weights.
- Enhanced conversion: degenerate (zero-extent) clouds no longer trigger UB in the spatial hash; single-point clouds no longer produce NaN k-NN distances; CLI rejects invalid numeric options (`--knn 0`, zero fit iterations, out-of-range opacity) instead of emitting NaN scales.
- Viewer: `waitRendered()` no longer resolves on frames of the previous scene during a scene switch; `serve.js` path containment now uses platform-correct path primitives (was 403-ing every request on Windows).
- Core tests no longer write to a shared hard-coded `/tmp` path.
- `SpzEncodeConfig::fractional_bits` removed: the spz container fixes position precision at 12 fractional bits and the option was silently ignored.
- A conversion that produces zero splats (e.g. a GLB whose every primitive fails validation) now exits with an error instead of writing an empty output file with exit code 0; the GLB reader reports failure when no usable vertex data exists.
- An empty `CMAKE_BUILD_TYPE` now defaults to `Release` instead of silently producing unoptimized binaries.

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
