# Changelog

## Unreleased

Work toward `v2.0.0`, the first release that can honestly be called production-grade. The blocker
register is in `docs/audit/production-blockers.md`.

### Breaking

### Added

- Canonical math oracle (`include/melkor/math/`), the single semantic authority for the
  transforms P0-17 is about. `activation` converts opacity/scale between training (logit/log)
  and canonical (linear) domains exactly once, with bounded exp and interval checks that make
  double activation a diagnostic rather than a plausible-but-wrong value. `quaternion` provides
  a numerically stable matrix round-trip (correct even at 180°, where the naive formula loses
  all precision), q/-q sign equivalence via angular distance, and rejection of a zero quaternion
  rather than silently promoting it to identity. `covariance` builds `Σ = R diag(s²) Rᵀ` and
  applies an affine node transform as `Σ' = A Σ Aᵀ`, decomposing back via a deterministic Jacobi
  eigensolver — correct for rotation, non-uniform scale, shear, and reflection, where moving only
  the mean (the bug) is demonstrably wrong. Reference-tested in `math_oracle_tests` (83 checks),
  clean under ASan+UBSan.

- Installable SDK (P0-04). A shared `libmelkor` exposing a stable C ABI
  (`include/melkor/c/melkor.h`), installed with `MelkorConfig.cmake`, an exported target set
  (`Melkor::melkor`), a `SameMajorVersion` version file, the public headers, and the generated
  `version.h`. `scripts/test_sdk_install.sh` proves the clean-room cycle: build, install to a
  prefix, `find_package(Melkor 2 CONFIG REQUIRED)` and link from standalone C and C++ consumer
  projects, run them, then relocate the install and consume again. The vendored SPZ dependency
  is `EXCLUDE_FROM_ALL` so its CLIs and headers no longer pollute the SDK.

- Root `VERSION` file as the single authoritative version source. CMake parses it before
  `project()` via `cmake/MelkorVersion.cmake` and generates `<melkor/version.h>` carrying the
  semantic version, the C ABI version, the JSON schema versions, and the adapter protocol
  version. No wall-clock timestamp is embedded; `SOURCE_DATE_EPOCH` is honoured.
- `tools/check_version_sync.py --check|--write` verifies every derived version surface
  (viewer `package.json`, both lockfiles, the Tauri config and Cargo manifest, the Python
  distribution's PEP 440 spelling, `CITATION.cff`, and the changelog) and can rewrite them.
  `--check` never modifies a file and runs in CI as `release_metadata_tests`.
- `third_party/manifest.lock.json` pins every compiled dependency by upstream commit SHA and by
  a content digest over the vendored tree, and declares every local patch.
  `tools/verify_third_party.py --check` enforces it in CI.
- `third_party/patches/spz/` makes the previously invisible SPZ fork reviewable. The vendored
  codec was a *modified* copy of upstream v2.1.0 with no record of what had been changed; the
  two local changes (a header-only version probe, and bounds on `fractionalBits` plus finite
  logits for packed alpha) are now numbered patch files with rationale that provably reconstruct
  the vendored tree from pristine upstream.
- Missing licence texts for `tinygltf` and `stb` are now vendored. Their source was being
  redistributed with no licence file at all.
- `tools/generate_notices.py --write|--check` generates `NOTICE` and `THIRD_PARTY_LICENSES.md`
  from the manifest, separating what Melkor *redistributes* from what it merely *invokes*.
- `tools/build_source_bundle.py` builds the source release from an explicit allowlist rather
  than "everything tracked", and produces byte-identical output for a given commit.
- `docs/adapters/index.md` records the external upstream projects, their licence terms, why the
  research snapshots were removed, and the separation between code licences and weight licences.
- Safety substrate (`include/melkor/{error,checked,limits,budget}.hpp`): `Result<T>` with stable
  diagnostic codes and a documented exit-code contract; checked arithmetic for every
  file-provided count, offset, and stride that reaches an allocation; `web`/`desktop`/`server`
  resource-limit profiles with hard ceilings; a thread-safe `Budget` with a decompression-ratio
  guard; cooperative cancellation; and path redaction that defaults to a basename so an
  inspection report is safe to paste in public. 112 checks in `safety_substrate_tests`.

- `docs/audit/v2-review-baseline.md` and `docs/audit/baseline-20260714.md` record the exact
  commit, tags, releases, GitHub metadata, CI state, and local build result that the v2 program
  starts from.
- `docs/audit/production-blockers.md` records the P0, P1, and P2 release blockers and the work
  package that closes each one.
- `ROADMAP.md` states the v2 product boundary, the deliberate non-goals, and what "supported"
  means.
- `SUPPORT.md` states plainly that no production release is currently supported, and defines the
  support window that takes effect once `v2.0.0` ships.

### Changed

- A real coverage-guided fuzz target for the PLY reader (`fuzz/fuzz_ply.cpp`), replacing the
  randomized-loop-over-valid-data the pre-v2 tests called fuzzing (P1-12). It builds two ways
  from one source: a libFuzzer binary that runs a bounded session in the new `fuzz-smoke` CI job,
  and a standalone corpus-replay executable registered as a ctest that runs the seed corpus and
  built-in adversarial inputs on every platform (clean under ASan+UBSan). Every fixed crash
  becomes a permanent corpus entry.
- Benchmark infrastructure under `benchmarks/`: versioned manifest and result JSON schemas, an
  example format-fidelity manifest, and a dataset registry that records dataset identity, licence,
  and digest without committing the data. A public quantitative claim is valid only when it links
  to a result produced from a manifest here, for the exact scope of the claim; a comparator that
  is cited but not reproduced is marked `attribution_only` and never combined into a
  Melkor-measured table. Pairs with the prose claim lint (WP21).
- `tools/check_claims.py` lints the production-landing surfaces for unqualified superlative and
  speed claims ("SOTA", "10-100x", "fastest", "lossless", "production-grade", "universal"),
  permitting a phrase only when it is attributed to its upstream source on the same line or
  backed by a benchmark link. Existing violations were removed or attributed (the GLOMAP speed
  figures are now marked as the authors' reported numbers, not Melkor-reproduced facts). Runs in
  CI (P2-04).
- **The build graph is acyclic (P0-05).** `melkor_core` and each backend linked to one another
  `PUBLIC`, because `ComputeProvider::create()` was declared in platform-neutral core but
  *defined* inside each backend. The resulting mutually dependent static archives linked only
  because the linker rescans them, and on Apple only after `-no_warn_duplicate_libraries`
  suppressed the diagnostic — a suppressed linker warning was load-bearing. Core now owns a
  `BackendRegistry` and knows nothing about Metal or CUDA; the new `melkor_runtime` layer is the
  only place allowed to name a concrete backend. Dependencies point one way, verified against
  CMake's own dependency graph. The suppression is gone.
- `ComputeProvider` no longer exposes `rawContext()` (P1-01, P1-02). It returned a raw `void*`
  that callers cast to `metal::MetalContext*`, which is how platform types reached
  platform-neutral code. The two operations that needed it — grid k-NN statistics and candidate
  filtering — are part of the abstract contract now, implemented by every backend, with CPU as
  the semantic reference.

- The CLI reads its version from the generated `<melkor/version.h>` instead of a
  `MELKOR_VERSION` compile definition, and the release-evidence builder reads the `VERSION`
  file instead of re-parsing `CMakeLists.txt`. Both previously restated the version
  independently, which is the mechanism behind P0-01.
- CMake minimum raised to 3.24.
- GitHub About metadata no longer claims Python. The description and topics stated a
  Python distribution that does not exist (P0-02); `python` and `depth-estimation` are
  removed from the topic list and will be restored only when the corresponding artifact
  passes its release gate.
- README no longer says "cross-platform" (there is no supported Windows build, P0-03),
  states the development status prominently, and drops the unsourced "around 90% smaller"
  SPZ compression figure, which had no benchmark behind it.

### Fixed

- Adversarial-review fixes across the safety substrate and tooling: `redact_path` required the
  root to align on a path-component boundary (a sibling directory `/x/workshop` under root
  `/x/work` was wrongly treated as contained and leaked `shop/...`); the atomic writer set the
  committed file's mode through `fchmod(0666 & ~umask)` before close instead of `chmod(0644)`
  by path after close (which ignored the umask and made output world-readable under a
  restrictive umask); the decompression-ratio guard compares against `max_ratio * compressed`
  rather than truncating integer division; `Limits::validate()` now requires every
  budget-backed limit positive (a custom profile that left `max_mesh_triangles` at 0 silently
  disabled it); `MELKOR_TRY_AS` takes the return type as a variadic tail so a `Result<map<K,V>>`
  compiles. `tools/build_source_bundle.py` excludes model-weight and key/cert extensions case-
  insensitively and every `.env*`/credential file, refuses to store symlinks as file content,
  and compresses to match the output name (it wrote uncompressed data to `.tar.zst` before);
  `check_version_sync.py` rewrites only the two self-versions in `package-lock.json` (a blanket
  `str.replace` corrupted dependency versions) and no longer accepts a `-rc` heading for a
  stable release; `generate_notices.py` degrades gracefully on a patch missing its rationale;
  `verify_third_party.py` digests git-tracked files, not whatever is on disk. Covered by
  `tools_tests` and expanded C++ regressions.
- **Float RGB rendered nearly black (P0-07).** The PLY reader divided every red/green/blue
  value by 255 unconditionally, as though every PLY stored colour as an 8-bit byte. A point
  cloud authored with `property float red` holds a value already in `[0,1]`, so mid-grey (0.5)
  decoded as `0.5/255 = 0.00196` and the scene rendered essentially unlit. Colour scaling is now
  driven by the declared source type: `uchar`/255, `ushort`/65535, `float`/`double` unchanged.
  `f_dc_*` (already an SH coefficient) is never colour-scaled. Regression: `ply_color_tests`.
- **Data loss on a failed write (P0-08).** The SPZ writer opened the destination with
  `std::ios::trunc` — destroying an existing file before writing a single new byte — and then
  called `std::remove(filepath)` from each of its error handlers. An encode that failed partway
  through therefore truncated the user's good `scene.spz` and then deleted it, leaving neither
  the new asset nor the old one. The PLY writer truncated on open for the same reason. Both now
  route through `melkor::io::AtomicWriter`: bytes go to an exclusively-created, unpredictably
  named temporary in the same directory, and the destination is replaced atomically only after
  the write fully succeeds. Any failure leaves the pre-existing file byte-for-byte intact.

### Security

- **`SECURITY.md` rewritten, and resource exhaustion is now in scope (P0-12).** The old policy
  explicitly excluded denial of service from large well-formed input, which meant a valid but
  enormous asset exhausting memory was, by policy, not a security bug. It is now in scope, and
  the exclusion is called out as having been wrong.
- Support attaches to the immutable `2.0.x` release line, never to `main` (P1-14). The policy
  states plainly that no production release is currently supported, rather than implying a
  release candidate carries a support promise.
- `docs/security/threat-model.md`: assets, trust boundaries, attackers, entry points, 29 abuse
  cases each mapped to a mitigation and its **true** state. It records 21 places where a control
  is planned but not yet implemented — most importantly that the resource-limit substrate is not
  yet wired into the readers — rather than describing the design as though it were the code.

### Removed

- The standalone-GLOMAP installer's mutable clone (P0-14, and the mutable-clone half of P0-13).
  `scripts/setup_glomap.sh` cloned `github.com/colmap/glomap` and tracked its `main` branch --
  a non-reproducible install of a now-deprecated tool. It is replaced by a deprecation stub
  pointing at COLMAP's built-in `global_mapper` and the migration guide. Global SfM lives in
  COLMAP itself now, verified against current COLMAP `main`
  (`docs/migrations/2.0-glomap-to-colmap-global.md`).
- Three unimplemented fields from `EnhancedConversionConfig` — `adaptive_density`,
  `target_splats_per_unit`, and `max_subdivision` (P0-11). They exposed density and subdivision
  controls the converter never implemented. The header now states plainly that the path is a
  geometric per-vertex initialiser, not trained reconstruction, and the magic `scale_factor * 50`
  is replaced by a named, explained constant (no behaviour change). The full honest
  `mesh-init --mode vertices|surface` with a real triangle-area sampler is WP10.
- The dead `GaussianFitter`, `DifferentiableRenderer`, `MeshRenderer`, `FeedforwardModel`, and
  `PythonBridge` facades (P1-03), 2,952 lines. `--fit` and `--feedforward` already failed closed
  at the CLI; the classes behind them remained in the public surface. Deleting them also removed
  the last 20 shell-execution sites from the core.
- `src/gpu_stub.cpp`. It existed only to fake the entire `metal::` namespace so that
  platform-neutral code — which should never have named a Metal type — would link on non-Apple
  platforms. Nothing needs stubbing now.

- `tools/OpenSplat/` (AGPL-3.0-only), `DA3coreml/` (research port with separately licensed
  weights), `ml-sharp/` (Apple sample-code licence, research-only weights), and `.superstack/`
  (agent artifacts) are removed from the MIT core (P0-16). Attribution and the reasoning are
  preserved in `docs/adapters/index.md`, and the tree that contained them is preserved at the
  signed tag `archive/pre-v2-research-bundle-20260714`. The CI job that built the CoreML
  surface is removed with it.

## 2.0.0-rc.1 (2026-07-11)

### Security

- Constrained glTF external resources to the input asset directory, including
  canonical symlink containment, and reject unsupported versions, required
  extensions, KHR Gaussian semantics, and lossy partial primitive decodes.
- Bounded SPZ inflation to its validated v1-v3 header-derived payload size and
  reject null/oversized buffers. SPZ encoding now rejects fixed-point and SH
  quantization overflow before upstream integer casts, and endpoint alpha
  bytes decode to finite logits. PLY now preflights a capped header, enforces
  element order and one exact ASCII record per vertex, and validates scalar,
  property, and SH declarations before allocating.
- Hardened GLB/glTF ingestion as an untrusted-input boundary: checked accessor,
  buffer-view, stride, alignment, count, scene traversal, transform, memory,
  finite-value, and configuration bounds now fail cleanly instead of reading
  outside buffers or propagating invalid values. Cycles, multiple parents,
  invalid roots/children/meshes, and malformed optional color/normal accessors
  are rejected consistently by inspect, basic, and enhanced modes.
- Restricted the viewer development server to GET/HEAD and an explicit public
  path allowlist; malformed URLs, traversal, private files, and unsupported
  methods fail closed. The Tauri shell now has a restrictive CSP and
  `nosniff` response policy.
- Pinned GitHub Actions, DA3 source/checkpoint revisions, feedforward catalog
  repositories, Swift dependencies, Bun, Rust, Ruff, cargo-deny, cargo-about,
  and Tauri CLI tooling. Model downloads are staged, validated, revision
  marked, and moved atomically; viewer lockfile installs disable dependency
  lifecycle scripts before Playwright installs its browser explicitly.
- Replaced executable `eval`, shell command construction, unsafe pickle loads,
  unrestricted `torch.load`, and remote-code trust in maintained/vendored ML
  paths. The stale ml-sharp snapshot is explicitly quarantined.
- Added dependency review, Dependabot, CODEOWNERS, npm audit, Rust advisory /
  source / wildcard / license policy, and generated locked-crate license
  inventory checks.

### Breaking changes

- `PlyReader::ReadResult` and `SpzDecoder::DecodeResult` now expose container
  metadata. Downstream structured bindings over the former three-field
  aggregates must add the metadata field or access members by name.
- Retired native `--fit`: the former path returned success without an
  optimization loop. Use OpenSplat for trained fitting.
- Retired native `--feedforward`, its built-in weight manager, and
  `scripts/setup_feedforward.sh`: they did not implement model-correct
  adapters. Use the pinned `da3-infer` bridge or the reviewed catalog.
- Retired `da3-infer-multigpu` view sharding because independently inferred
  view subsets do not share DA3's joint camera frame. Keep a scene on one GPU
  or use official DA3-Streaming for long sequences.
- Reduced the experimental CoreML CLI to single-image `infer` and `benchmark`.
  Conversion, fusion, streaming, and 3DGS commands remain disabled until
  sequence-level PyTorch parity is demonstrated.

### Added

- Added `melkor inspect INPUT [--json] [--strict]`, a deterministic,
  backend-free validation surface for PLY, SPZ, GLB, and glTF inputs. It
  reports container metadata, bounds, field provenance, numeric hazards, and
  stable issue codes without mutating the source.
- Added private, offline local-file opening to the web and desktop viewer by
  picker or drag-and-drop for PLY, SPZ, SPLAT, KSPLAT, SOG, and ZIP scenes.
  Files stream directly from the browser into Spark and can be reopened after
  failures without uploading data or granting filesystem permissions.
- Added active-scene glTF traversal with node matrices/TRS, instancing, cycle
  protection, world-space positions, and inverse-transpose normal transforms.
- Added a bounded-memory 4D PLY/SPZ player with circular prefetch, latest-seek
  semantics, finite retry/backoff, visible recovery, and producer scripts.
- Added responsive/accessibility/reduced-motion viewer behavior, WebGL context
  recovery, deterministic generated SPLAT/4D fixtures, exact desktop staging,
  provenance records, and third-party notices.
- Added strict CLI contract tests, scene-graph/malformed-input tests, DA3
  geometry tests, renderer reference/gradient tests, and mobile/server/4D
  Playwright coverage.
- Added a dated, license-aware feedforward integration catalog with immutable
  repository revisions and explicit gates for non-commercial or unspecified
  checkpoint terms.

### Fixed

- Kept inspector JSON valid for empty clouds, control-byte paths, malformed
  UTF-8 parser errors, and every failure path; scale checks now classify the
  actual float32 covariance operation at overflow/subnormal/zero boundaries.
- Made normal viewer loads transactional while a 4D sequence is active, so a
  damaged local file cannot clear the current frame cache; cancelling a
  replacement chooser retains recovery actions, while fatal 4D errors keep
  their Retry action visible.
- Corrected differentiable rendering to depth-sort front-to-back, composite the
  background through final transmittance, and include background in the
  backward recurrence. Stable analytic gradients now match finite differences.
- Standardized robust quaternion normalization, log-scale covariance input,
  opacity epsilon, near-to-far sorting, CPU fallbacks, allocation/command
  failure handling, and little-endian PLY output across compute backends.
- Reworked DA3 reconstruction to use one joint multi-view call, preserve the
  official learned Gaussian fields on GIANT/NESTED, perform camera-Z (not
  Euclidean-ray) unprojection for depth models, filter confidence/sky/borders,
  and reject missing or malformed cameras and empty output.
- Made CLI numeric parsing fully consume finite tokens and reject missing,
  conflicting, unknown, extra, or out-of-range arguments.
- Made enhanced CPU spatial hashing origin-relative and range-checked, removing
  float-to-integer and neighbor-addition undefined behavior for large finite
  scene translations. Conversion validates clouds both before and after every
  mutating backend/densification stage.
- Made SPZ output self-round-trippable at default opacity, normalized extreme
  finite quaternions safely, preserved channel-major SH when exporting a lower
  degree, and completed file encoding before opening the destination.
- Escaped invalid UTF-8 plus terminal C0/C1 controls in human paths, parser
  failures, CLI arguments, and writer diagnostics.
- Removed fixed-count and unsupported benchmark/model claims from public docs;
  the DA3 and CoreML documentation now matches the executable surfaces.

### Distribution

- Synchronized the native CLI, npm, Tauri, Cargo, lockfiles, changelog, and
  generated Rust notices at `2.0.0-rc.1`.
- Added deterministic source release evidence: normalized archive, per-file
  SHA-256 manifest, SPDX 2.3 source SBOM, unsigned in-toto/SLSA-shaped
  provenance, aggregate checksums, self-verification, and an RC tag workflow.
  The generator is byte-bound to the selected Git tree, and verification
  rejects unlisted nested, symlinked, or non-regular payloads.
- Native `cmake --install` now includes the project and third-party notices.
- The desktop bundle includes only project-owned generated captures; external
  test scenes without explicit redistribution terms are excluded.
- Tauri packaging embeds project/Spark/three.js notices and a cargo-about
  inventory generated from the locked Rust graph. CI verifies the inventory,
  license policy, CSP staging, and a real `tauri build --no-bundle` path.

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
