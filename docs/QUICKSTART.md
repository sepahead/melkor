# Quick Start

This guide builds and tests the current Melkor development source.
No supported production binary exists.

## Requirements

Install these native build tools:

- Git
- CMake 3.24 or later
- Ninja
- A C++17 compiler
- zlib for SPZ support

Install Python 3.11 for the complete CTest set.
The native CLI does not need Python at run time.

## Get the source

```bash
git clone https://github.com/sepahead/melkor.git
cd melkor
```

Verify the committed dependency snapshots:

```bash
./scripts/setup_deps.sh
```

The script verifies source digests and patch records.
It does not download a live dependency.

## Create the test environment

```bash
python3.11 -m venv .venv
. .venv/bin/activate
python -m pip install --disable-pip-version-check \
  --only-binary=:all: jsonschema==4.26.0 numpy==2.4.6
```

## Build and test

```bash
cmake --preset dev -DPython3_EXECUTABLE="$VIRTUAL_ENV/bin/python"
cmake --build --preset dev --parallel
ctest --preset dev
```

The executable is `build/dev/melkor`.
See the [CLI reference](CLI.md) for every command option and profile.

The `MELKOR_BUILD_SPZ` setting accepts `AUTO`, `ON`, or `OFF`.
`AUTO` enables SPZ when CMake finds the vendored source and zlib.
`ON` stops configuration when either requirement is absent.
`OFF` removes SPZ support.

Test the no-SPZ build separately:

```bash
cmake --preset spz-off
cmake --build --preset spz-off --parallel
ctest --preset spz-off
```

## Inspect an asset

Inspect a self-describing asset:

```bash
./build/dev/melkor inspect scene.glb
./build/dev/melkor inspect scene.gltf --json --strict
./build/dev/melkor inspect scene.ply --limits-profile desktop
```

Use `--strict` to fail when the report contains a warning.
Inspection does not modify the input.

PLY and SPZ omit some source semantics.
Supply each missing value from trusted producer information.

For example, inspect a Graphdeco PLY file with verified source semantics:

```bash
./build/dev/melkor inspect trained.ply \
  --input-profile ply:graphdeco-3dgs-v1 \
  --source-frame gltf-luf \
  --source-unit-to-meter 1 \
  --source-color-space lin_rec709_display
```

Do not copy these semantic values without checking the producer.
A Graphdeco PLY header does not define them.

Inspect an SPZ file with its out-of-band unit and color space:

```bash
./build/dev/melkor inspect scene.spz \
  --source-unit-to-meter 1 \
  --source-color-space lin_rec709_display
```

See [Asset inspection](INSPECT.md) for the report schema and exit codes.

## Convert an asset

Convert through the canonical model:

```bash
./build/dev/melkor convert scene.glb scene.ply
./build/dev/melkor convert scene.ply roundtrip.glb
```

The command writes one JSON loss report to stdout.
It commits the output only after validation and loss-policy checks pass.
The command writes the report after output staging and before the atomic commit.
Use the report only when the command returns exit status zero.

An SPZ output omits coordinate and color-space metadata.
Approve these losses only when the sidecar or workflow preserves that information:

```bash
./build/dev/melkor convert scene.ply scene.spz \
  --allow-loss LOSS_COLOR_SPACE_METADATA_DROPPED \
  --allow-loss LOSS_COORDINATE_METADATA_DROPPED
```

SPZ output also quantizes numeric values.
The report records that warning without requiring approval.

Convert SPZ back to a self-describing PLY file:

```bash
./build/dev/melkor convert scene.spz scene.ply \
  --source-unit-to-meter 1 \
  --source-color-space lin_rec709_display
```

Melkor reads SPZ file-format versions 1 through 3.
It writes version 3.
SPZ version 4 remains unsupported.

## Select a format explicitly

Melkor normally selects a container from its suffix and verified content.
Use an explicit format for a suffixless path:

```bash
./build/dev/melkor inspect asset --input-format glb
./build/dev/melkor convert input.glb output --output-format ply
```

Use `--` before a path that starts with a hyphen.

## Use resource profiles

Every command uses one bounded profile:

- `web` for small browser-oriented assets
- `desktop` for interactive workstation use
- `server` for larger controlled jobs

Select a profile explicitly when the default is unsuitable:

```bash
./build/dev/melkor inspect scene.glb --limits-profile server
```

There is no unlimited profile.
See [Resource limits](reference/resource-limits.md) for exact ceilings.

## Install the C SDK

```bash
cmake --preset release
cmake --build --preset release --parallel
cmake --install build/release --prefix "$PWD/build/install"
```

The installation contains:

- The `melkor` executable
- The `libmelkor` shared library
- `melkor/c/melkor.h`
- `melkor/version.h`
- CMake package files
- Format profiles and schemas
- License files

The installation does not expose an internal C++ ABI.
C++ programs call the C ABI.

Run the clean install and relocation test:

```bash
./scripts/test_sdk_install.sh -DMELKOR_BUILD_SPZ=ON
```

## Run the local viewer

The viewer uses a separate JavaScript workspace.

```bash
cd viewer
./fetch-assets.sh --runtime-only
bun run serve
```

Open `http://127.0.0.1:8771/`.
The development server binds to the loopback interface.

See the [Viewer guide](../viewer/README.md) for controls and desktop build steps.

## Use an external reconstruction adapter

Melkor does not train a scene from photographs.
The shell scripts can call a user-supplied external trainer.

Review these boundaries before you run one:

- [Pipeline guide](PIPELINE.md)
- [Adapter boundary](adapters/index.md)
- [Third-party licenses](../THIRD_PARTY_LICENSES.md)

External code and model weights keep their own license terms.

## Run repository checks

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

If Python tests are absent, reconfigure with the virtual environment active.
You can also set `-DPython3_EXECUTABLE="$VIRTUAL_ENV/bin/python"`.

If SPZ configuration fails, verify the vendored source and zlib:

```bash
./scripts/setup_deps.sh
cmake --preset dev --fresh
cmake --build --preset dev --parallel
```

Use the `spz-off` preset when zlib is intentionally unavailable.
