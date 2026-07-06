# Contributing to Melkor

Thanks for considering a contribution. This document covers the development
setup, the project's correctness rules, and what a pull request needs before
review.

## Development setup

```bash
# Fetch pinned third-party dependencies (tinygltf, stb, spz)
./scripts/setup_deps.sh

# Configure and build (Metal is enabled automatically on macOS,
# CUDA on Linux with -DMELKOR_USE_CUDA=ON)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run the test suite
cd build && ctest --output-on-failure
```

The viewer is a separate workspace: `cd viewer && ./fetch-assets.sh &&
bun run serve` (tests: `bun run test`). See [viewer/README.md](viewer/README.md).

## Backend parity rules

Melkor ships three implementations of its compute operations — Metal
(`src/metal/`), CUDA (`src/cuda/`), and CPU (`src/cpu_compute_provider.cpp`,
`src/spatial_grid.cpp`) — behind the `ComputeProvider` interface. They must
stay **operation-for-operation consistent**:

- Any semantic change to one backend (normalization behavior, coordinate
  transforms, color/opacity conversion, neighbor-search order) must be made
  to all three in the same pull request.
- The neighbor-search kernels (`knn_stats_grid` / `filter_candidates_grid`
  and their CUDA/CPU mirrors) walk a shared host-built uniform grid in
  identical shell order. Keep the loop structure literally parallel across
  the three implementations so results differ only by float rounding.
- `tests/test_compute_provider.cpp` and `tests/test_densifier.cpp` enforce
  parity where hardware allows; extend them when you add operations.

Because CUDA typically cannot be compiled on the primary development
machines, backend changes must additionally be verified against the stub
configuration, which has the same link topology as Linux CPU builds:

```bash
cmake -B build-cpu -DMELKOR_USE_METAL=OFF
cmake --build build-cpu -j
cd build-cpu && ctest --output-on-failure
```

## Correctness conventions

- Parsers for external formats (GLB, PLY, SPZ) treat all input as untrusted:
  validate indices, strides, counts, and sizes before reading; return
  `{success=false, error_message}` rather than crash or throw.
- GPU entry points return empty/false on failure so callers can fall back to
  the CPU path — never partially-initialized or zero-filled data with a
  success status.
- Functions whose stdout is captured by shell command substitution must log
  to stderr only (see the `log_*` helpers in `scripts/pipeline.sh`).
- Objective-C++ builds with `-fobjc-arc`; do not add manual retain/release.

## Code style

- C++17, 4-space indentation; match the surrounding file's conventions.
- Strict warnings are enabled on first-party code (`-Wall -Wextra
  -Wpedantic`); new warnings fail review.
- Python passes `ruff check` with the repository defaults.
- Shell scripts use 2-space indentation and must pass `bash -n`.
- Tests are self-contained (no framework): build an input in memory, run a
  core routine, `check()` a geometric or encoding property.

## Pull request checklist

- [ ] `ctest` passes in the default configuration
- [ ] `ctest` passes with `-DMELKOR_USE_METAL=OFF` (stub/CPU topology)
- [ ] Backend-affecting changes applied to Metal, CUDA, and CPU together
- [ ] No new compiler warnings
- [ ] Python changes pass `ruff check`
- [ ] Viewer changes pass `bun run test` in `viewer/`
- [ ] `CHANGELOG.md` updated for user-visible changes
- [ ] Documentation updated where behavior or flags changed

## Reporting issues

Use GitHub issues for bugs and feature requests. Include the platform, the
active backend (`melkor --info`), exact commands, and a minimal input where
possible. For security-sensitive reports, follow [SECURITY.md](SECURITY.md)
instead of opening a public issue.
