# Reconstruction pipeline

`scripts/pipeline.sh` is a development wrapper for COLMAP and OpenSplat.
It is not part of the native Melkor CLI.

The wrapper does not install external software.
It also does not verify an external binary revision or license.
The planned adapter runner will replace this script.

## Scope

The wrapper performs these stages:

1. Use an image directory or an existing COLMAP project.
2. Run incremental or global COLMAP mapping when necessary.
3. Run one user-supplied OpenSplat executable.
4. Require a nonempty PLY output.
5. Optionally use Melkor to create an SPZ output.

Melkor code does not train the scene.
Review [External adapters](adapters/index.md) before you select an external tool.

## Requirements

Provide these programs for a full image-to-splat run:

- A COLMAP build with the selected mapper
- A reviewed OpenSplat executable
- Melkor, when you request SPZ output

Set the external executable with `--opensplat PATH`.
You can also set `MELKOR_OPENSPLAT_BIN`.

The retired setup scripts do not install these tools.
Build each external tool in an isolated and reviewed environment.

## Basic use

Run incremental COLMAP mapping and OpenSplat:

```bash
./scripts/pipeline.sh images result \
  --opensplat /reviewed/bin/opensplat
```

The output directory must not exist.
The wrapper writes `result/point_cloud.ply`.

Use a complete COLMAP project:

```bash
./scripts/pipeline.sh colmap-project result \
  --skip-colmap \
  --opensplat /reviewed/bin/opensplat
```

The project must contain a complete sparse model.
Use `--images PATH` when the project has no `images/` directory.

## Structure-from-motion modes

The default `--sfm colmap` mode runs `colmap automatic_reconstructor`.

```bash
./scripts/pipeline.sh images result \
  --sfm colmap \
  --opensplat /reviewed/bin/opensplat
```

The `--sfm global` mode uses `colmap global_mapper`.
It calls the compatibility wrapper named `glomap_wrapper.sh`.

```bash
./scripts/pipeline.sh images result \
  --sfm global \
  --matcher sequential \
  --opensplat /reviewed/bin/opensplat
```

The script rejects the old `--sfm glomap` value.
See [COLMAP global mapper wrapper](GLOMAP_WRAPPER.md) for its exact stages.

## Training options

The quality preset selects only an iteration count:

| Preset | Iterations |
|---|---:|
| `fast` | 7,000 |
| `medium` | 15,000 |
| `high` | 30,000 |

Use `--iterations N` to replace the preset value.
An iteration count does not guarantee output quality.

Use `--gpu ID` to set one CUDA device for the OpenSplat process.
The pipeline does not implement multi-GPU training.

The pipeline accepts `--tool auto` and `--tool opensplat` for compatibility.
It rejects all other trainer values.

## SPZ output

Request the PLY and SPZ files:

```bash
./scripts/pipeline.sh images result \
  --opensplat /reviewed/bin/opensplat \
  --melkor "$PWD/build/dev/melkor" \
  --format both
```

Use `--format spz` when SPZ is the requested final format.
The PLY training result remains in the output directory.

The pipeline fails when it cannot find the Melkor executable.
It does not silently skip a requested SPZ output.

## Dry run

Inspect the planned commands without file changes:

```bash
./scripts/pipeline.sh images result \
  --sfm global \
  --opensplat /reviewed/bin/opensplat \
  --dry-run
```

The dry run validates options and the input directory.
It does not create the output directory.
It does not require the external executables to exist.

## Failure behavior

The wrapper fails before work when the output directory exists.
This rule prevents stale output selection and accidental replacement.

The wrapper also fails for these conditions:

- Fewer than three supported source images
- An incomplete COLMAP sparse model
- A missing external executable
- A missing or empty PLY result
- A requested SPZ result that Melkor did not create

The external tools still write their own files directly.
The wrapper cannot make those writes atomic.

## Retired options

The script rejects these old behaviors:

- Automatic external-tool installation
- Simulated multi-GPU OpenSplat modes
- Automatic trainer selection by host type
- Unverified image downscale forwarding
- Broad searches for an unspecified output PLY file

Use a tool-specific command when you need a contract that this wrapper does not provide.

## Limits

The pipeline does not create a signed or content-addressed run manifest.
It does not verify the COLMAP or OpenSplat source revision.

Record the complete environment and output digest for reproducible work.
Track the manifest-driven replacement in [the blocker register](audit/production-blockers.md).
