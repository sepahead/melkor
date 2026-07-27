# Quick Start

This guide builds and tests the current development source.
No supported production binary exists yet.

## Requirements

Install these native build tools:

- Git
- CMake 3.25 or later for the included presets
- Ninja
- A C++17 compiler

Install Python 3.11 and NumPy 2.4.6 for the complete test set.
The native CLI does not require Python at run time.

On macOS, install the Xcode Command Line Tools.
On Linux, install GCC or Clang.

## Get the source

```bash
git clone https://github.com/sepahead/melkor.git
cd melkor
```

Verify the vendored dependency lock:

```bash
./scripts/setup_deps.sh
```

This script does not download live dependencies.
It checks the committed snapshots and their recorded patches.

## Create the test environment

Create this environment when you need the complete CTest set:

```bash
python3.11 -m venv .venv
. .venv/bin/activate
python -m pip install --disable-pip-version-check \
  --only-binary=:all: numpy==2.4.6
```

## Build and test

Use the development preset:

```bash
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

The development executable is `build/dev/melkor`.

Confirm the source version and active backend:

```bash
./build/dev/melkor --version
./build/dev/melkor --info
```

Build the CPU-only topology separately:

```bash
cmake -S . -B build/review-cpu -G Ninja \
  -DMELKOR_USE_METAL=OFF \
  -DMELKOR_USE_CUDA=OFF \
  -DMELKOR_WERROR=ON
cmake --build build/review-cpu --parallel
ctest --test-dir build/review-cpu --output-on-failure --no-tests=error
```

## Convert a mesh

The current mesh path creates one splat for each mesh vertex.
It does not train a scene from photographs.

```bash
./build/dev/melkor model.glb scene.ply --basic
```

Write SPZ instead:

```bash
./build/dev/melkor model.glb scene.spz --basic
```

Use these basic conversion controls when necessary:

```text
--scale FLOAT       Default splat scale
--opacity FLOAT     Default opacity in [0, 1]
--pos-scale FLOAT   Position scale
--no-coord-convert  Keep the input coordinate orientation
--ascii             Write ASCII PLY
```

`--enhanced` is unavailable.
It fails until the canonical area-weighted mesh sampler is complete.

## Convert PLY and SPZ

Convert a 3DGS PLY file to SPZ:

```bash
./build/dev/melkor scene.ply scene.spz
```

Convert a supported SPZ file to PLY:

```bash
./build/dev/melkor scene.spz scene.ply
```

The current decoder reads SPZ file-format versions 1 through 3.
The current encoder writes version 3.
SPZ conversion is lossy.

SPZ v4 support remains a release blocker.
See [the production blocker register](audit/production-blockers.md).

## Inspect an asset

Inspect a PLY, SPZ, GLB, or glTF file:

```bash
./build/dev/melkor inspect scene.ply
./build/dev/melkor inspect scene.spz --json
./build/dev/melkor inspect model.glb --json --strict
```

Use `--strict` to treat warnings as a failed validation.
The command does not initialize a GPU.

The exit codes are:

| Code | Meaning |
|---:|---|
| 0 | The asset has no blocking issue. |
| 1 | The asset is invalid, or strict mode found a warning. |
| 2 | The command use is invalid. |

See [Asset inspection](INSPECT.md) for the JSON contract.

## Convert a Gaussian GLB

The explicit `convert` command currently supports GLB-to-GLB conversion.
It applies the loss policy before it commits the output.

```bash
./build/dev/melkor convert input.glb output.glb
```

If a severe loss is present, approve only its exact code:

```bash
./build/dev/melkor convert input.glb output.glb \
  --allow-loss LOSS_CODE
```

Do not approve a loss code until you review its effect.
See [Loss policy](reference/loss-policy.md).

## Scene completion status

`--fill-holes` currently fails closed.
The internal densifier still uses the retired mutable model.

See [Scene completion](SCENE_COMPLETION.md) for the internal algorithm and migration status.

## Run the viewer

The viewer uses a separate JavaScript workspace.
Fetch the digest-checked runtime assets:

```bash
cd viewer
./fetch-assets.sh --runtime-only
bun run serve
```

Open `http://127.0.0.1:8771/`.
The server binds to the loopback interface by default.

See [the viewer guide](../viewer/README.md) for controls and test commands.

## External reconstruction pipeline

Photo reconstruction uses external tools.
The current shell wrappers are development tools.
They do not provide a pinned supply-chain boundary.

Review [the pipeline guide](PIPELINE.md) before you run an external tool.
Review [the adapter catalog](adapters/index.md) for licenses and model terms.

The pipeline uses incremental COLMAP by default:

```bash
./scripts/pipeline.sh /path/to/images /path/to/output \
  --sfm colmap \
  --opensplat /reviewed/bin/opensplat
```

Use a compatible COLMAP global mapper explicitly:

```bash
./scripts/pipeline.sh /path/to/images /path/to/output \
  --sfm global \
  --opensplat /reviewed/bin/opensplat
```

The old `--sfm glomap` value is invalid.
See [the migration guide](migrations/2.0-glomap-to-colmap-global.md).

The retired setup scripts do not install external trainers.
Use a reviewed external installation and an explicit executable path.

## Install the SDK locally

Configure an installable CPU build:

```bash
cmake --preset release-cpu
cmake --build --preset release-cpu --parallel
cmake --install build/release-cpu --prefix "$HOME/.local"
```

Confirm the installed command:

```bash
"$HOME/.local/bin/melkor" --version
```

The install also provides the C API, C++ headers, and CMake package files.
Use `scripts/test_sdk_install.sh` to test a clean temporary prefix.

## Run repository checks

Run the maintained Python and documentation checks:

```bash
ruff check . --no-unsafe-fixes
python3 tools/verify_third_party.py --check
python3 tools/generate_notices.py --check
python3 tools/check_version_sync.py --check
python3 tools/build_source_bundle.py --check
python3 tools/check_claims.py
python3 tools/check_docs_links.py
python3 tools/check_docs_style.py
python3 tools/check_profiles.py
python3 tests/test_tools.py
```

Check each shell script:

```bash
git ls-files -z -- '*.sh' | xargs -0 -n 1 bash -n --
git ls-files -z -- '*.sh' | xargs -0 shellcheck --severity=warning
```

## Troubleshooting

If CMake cannot find Ninja, install Ninja or select another generator.

If Python-backed tests are absent, reconfigure with the virtual environment active.
You can also set `-DPython3_EXECUTABLE="$VIRTUAL_ENV/bin/python"`.

If SPZ support is absent, verify the vendored dependencies first:

```bash
./scripts/setup_deps.sh
cmake --preset dev --fresh
cmake --build --preset dev --parallel
```

If `colmap global_mapper` is absent, use a compatible COLMAP build.
Do not install the retired standalone GLOMAP program.
