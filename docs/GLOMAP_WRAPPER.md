# COLMAP global mapper wrapper

The `glomap_wrapper.sh` file name remains for command compatibility.
The script now runs `colmap global_mapper`.
It does not run the retired standalone GLOMAP program.

Use this wrapper as a transition tool.
The wrapper does not install or pin COLMAP.
Any future supported adapter must provide a pinned tool contract.

## Requirements

Install a COLMAP build that provides `global_mapper`.
Confirm the command before you process data:

```bash
colmap global_mapper --help
```

The wrapper also uses COLMAP feature extraction and matching commands.
It accepts these input image formats:

- BMP
- JPEG
- PNG
- TIFF

Convert other image formats before you run the wrapper.

Current COLMAP documentation recommends `view_graph_calibrator` before global mapping when
reliable camera intrinsics are unavailable.
That command changes the database in place.
The wrapper does not run it automatically.

Prepare a copied project when you need that calibration:

```bash
cp -a existing-project calibrated-project
colmap view_graph_calibrator --database_path calibrated-project/database.db
./scripts/glomap_wrapper.sh calibrated-project output \
  --skip-features --skip-matching
```

The copied project must contain `database.db` and `images/`.
See the official [COLMAP CLI reference](https://colmap.github.io/cli.html) for the calibration
preconditions and current command behavior.

## Basic use

Run the complete global structure-from-motion sequence:

```bash
./scripts/glomap_wrapper.sh /path/to/images /path/to/project
```

The output directory must not exist.
This rule prevents reuse of stale database or model files.

The wrapper runs these stages:

1. It links the input images into the output project.
2. It runs `colmap feature_extractor`.
3. It runs the selected COLMAP matcher.
4. It runs `colmap global_mapper`.
5. It checks the three required sparse-model files.

The published `images/` entries are absolute symbolic links.
Keep the source image directory at its original path.
Copy the images when you need a portable project.

The sparse model is in `OUTPUT_DIR/sparse/0/` or `OUTPUT_DIR/sparse/`.
The wrapper requires these files:

- `cameras.bin`
- `images.bin`
- `points3D.bin`

## Options

```text
--matcher TYPE       exhaustive, sequential, or vocab_tree
--vocab-tree PATH    Required data file for vocab_tree matching
--quality LEVEL      low, medium, or high
--gpu MODE           auto, 0, or 1
--skip-features      Use an existing database.db
--skip-matching      Use existing matches in database.db
--dry-run            Print commands without changing files
--verbose, -v        Print the selected configuration
--help, -h           Show the help
```

The `vocab_tree` matcher requires `--vocab-tree`.
The wrapper does not download this file.
This rule prevents an unverified data download.

`--skip-matching` requires `--skip-features`.
The input project must contain `database.db` and `images/`.

## Examples

Use sequential matching for ordered images:

```bash
./scripts/glomap_wrapper.sh images project --matcher sequential
```

Select the highest feature-count preset:

```bash
./scripts/glomap_wrapper.sh images project --quality high
```

Use an existing database and existing matches:

```bash
./scripts/glomap_wrapper.sh existing-project new-project \
  --skip-features --skip-matching
```

Review the exact commands without file changes:

```bash
./scripts/glomap_wrapper.sh images project --dry-run
```

## GPU selection

`--gpu auto` enables the COLMAP GPU flags on Linux when `nvidia-smi` is available.
Use `--gpu 0` to disable these flags.
Use `--gpu 1` to enable these flags explicitly.

The selected flag does not prove that COLMAP has CUDA support.
Review the COLMAP output for the active device.

## Performance

Global and incremental mapping use different algorithms.
Their performance depends on the dataset, settings, and hardware.
Melkor does not publish a relative speed claim without a reproducible benchmark.

Record a comparison under `benchmarks/` before you make a project claim.
Include the dataset, hardware, COLMAP revision, settings, and run count.

## Migration

Do not use `scripts/setup_glomap.sh` to install software.
That command is a failure-only migration stub.

See [the migration guide](migrations/2.0-glomap-to-colmap-global.md) for the command change.

The main pipeline accepts `--sfm global` for this path:

```bash
./scripts/pipeline.sh images output --sfm global
```

The old `--sfm glomap` value is no longer valid.
This removal prevents the pipeline from invoking a retired program.

## Failure behavior

The wrapper fails before mapping when `global_mapper` is unavailable.
It does not fall back to a different mapper.

The wrapper also fails when a required sparse-model file is empty or missing.
An exit code of zero from COLMAP is not sufficient evidence of a valid result.

The wrapper refuses an existing destination image name.
This rule prevents a source image from replacing another image silently.
