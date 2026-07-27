# LichtFeld-Studio wrapper

`scripts/lichtfeld_wrapper.sh` runs one user-supplied LichtFeld-Studio executable.
The wrapper does not install LichtFeld-Studio.
It does not claim a pinned upstream command contract.

Review the upstream source and dependency terms before use.
See [External adapters](adapters/index.md) for the project boundary.

## Status

The wrapper is a development tool.
It is not a production adapter.
The production adapter still needs a source revision, dependency lock, and tested command contract.

`scripts/setup_lichtfeld.sh` is a failure-only migration stub.
It does not change system packages or download software.

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
./scripts/lichtfeld_wrapper.sh project \
  --lichtfeld /reviewed/bin/LichtFeld-Studio \
  --output result
```

You can also set an environment variable:

```bash
MELKOR_LICHTFELD_BIN=/reviewed/bin/LichtFeld-Studio \
  ./scripts/lichtfeld_wrapper.sh project --output result
```

## Options

| Option | Purpose |
|---|---|
| `--images PATH` | Use a separate image directory. |
| `-o, --output PATH` | Set the output directory. |
| `--gpu ID` | Set `CUDA_VISIBLE_DEVICES` for one process. |
| `--lichtfeld PATH` | Select an executable. |
| `--force` | Permit use of an existing output directory. |
| `--dry-run` | Print the command without file changes. |
| `--verbose, -v` | Print the selected paths. |

Pass version-specific options after `--`:

```bash
./scripts/lichtfeld_wrapper.sh project \
  --lichtfeld /reviewed/bin/LichtFeld-Studio \
  --output result \
  -- --UPSTREAM_OPTION VALUE
```

Run the selected executable with `--help` before you pass an upstream option.

## Removed options

The wrapper rejects these old options:

- `--iterations`
- `--pose-opt`
- `--no-mcmc`
- `--eval`
- `--gui`

The old wrapper accepted some options without forwarding them.
It also translated other options without a pinned command contract.
Those behaviors could make a run differ from its printed configuration.

Pass only options that your selected upstream revision documents.

## Output safety

The wrapper refuses an existing output directory by default.
Use `--force` only after you verify the target path.

The external process writes into a same-parent staging directory.
The wrapper requires at least one output item before it replaces the destination.
An external process failure preserves an existing destination.

## Dry run

Use a dry run to inspect quoting and selected paths:

```bash
./scripts/lichtfeld_wrapper.sh project \
  --lichtfeld /reviewed/bin/LichtFeld-Studio \
  --output result \
  --dry-run
```

The dry run does not create the output directory or a temporary workspace.

## Limits

This wrapper does not verify:

- The LichtFeld-Studio source revision
- The binary digest
- The dependency versions
- The GPU runtime
- The output format or semantics

Record those items before you use an output as release evidence.
The planned adapter runner will enforce them with a manifest.
