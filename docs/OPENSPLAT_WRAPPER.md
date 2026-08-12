# OpenSplat wrapper

`scripts/opensplat_wrapper.sh` runs one user-supplied OpenSplat executable.
The wrapper does not install OpenSplat.
It does not claim a pinned upstream command contract.

OpenSplat uses AGPL-3.0-only terms.
Review those terms before you install or distribute it.
See [External adapters](adapters/index.md) for the project boundary.

## Status

The wrapper is a development tool.
It is not a production adapter.
The production adapter still needs a source revision, dependency lock, and tested command contract.

`scripts/setup_opensplat.sh` is a failure-only migration stub.
It does not download software.

## Input contract

Provide a COLMAP project with an image directory and a complete sparse model.
The wrapper accepts the sparse model in either location:

- `sparse/0/`
- `sparse/`

The selected directory must contain these nonempty files:

- `cameras.bin`
- `images.bin`
- `points3D.bin`

If the project has no `images/` directory, use `--images PATH`.
The wrapper then creates a temporary workspace with image links.
It removes the workspace when the command ends.

## Basic use

Select the executable with an option:

```bash
./scripts/opensplat_wrapper.sh project \
  --opensplat /reviewed/bin/opensplat \
  --output result.ply
```

You can also set an environment variable:

```bash
MELKOR_OPENSPLAT_BIN=/reviewed/bin/opensplat \
  ./scripts/opensplat_wrapper.sh project --output result.ply
```

## Options

| Option | Purpose |
|---|---|
| `--images PATH` | Use a separate image directory. |
| `-o, --output PATH` | Set the PLY output path. |
| `-n, --iterations N` | Set a positive iteration count. |
| `--gpu ID` | Set `CUDA_VISIBLE_DEVICES` for one process. |
| `--opensplat PATH` | Select an executable. |
| `--force` | Permit replacement of an existing output file. |
| `--dry-run` | Print the command without file changes. |
| `--verbose, -v` | Print the selected paths. |

Pass version-specific OpenSplat options after `--`:

```bash
./scripts/opensplat_wrapper.sh project \
  --opensplat /reviewed/bin/opensplat \
  --output result.ply \
  -- --UPSTREAM_OPTION VALUE
```

Run the selected executable with `--help` before you pass an upstream option.

## Removed options

The wrapper rejects these old options:

- `--gpu-ids`
- `--split`
- `--downscale`
- `--densify-grad`
- `--densify-size`
- `--densify-interval`
- `--stop-densify`
- `--save-every`

The former multi-GPU modes did not implement distributed training.
One mode ran independent jobs and kept only the first result.
Another mode restarted training without checkpoint resume.

Pass a verified upstream option after `--` when your selected revision supports it.
Use a native distributed trainer for a real distributed training run.

## Output safety

The wrapper refuses an existing output file by default.
Use `--force` only after you verify the target path.

The external process writes to a same-directory staging path.
The wrapper replaces the destination only after it receives a nonempty file.
An external process failure preserves an existing destination.

The wrapper requires a nonempty PLY file after the process exits.
It returns a nonzero status when the file is absent or empty.

## Dry run

Use a dry run to inspect quoting and selected paths:

```bash
./scripts/opensplat_wrapper.sh project \
  --opensplat /reviewed/bin/opensplat \
  --output result.ply \
  --dry-run
```

The dry run does not create the output directory or a temporary workspace.
It withholds upstream arguments because they can contain secrets.

## Limits

This wrapper does not verify:

- The OpenSplat source revision
- The binary digest
- The LibTorch version
- The GPU runtime
- The output semantics
- A stage timeout or restricted process environment

Record those items before you use an output as release evidence.
The planned adapter runner will enforce them with a manifest.
