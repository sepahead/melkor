# Feedforward reconstruction catalog

This catalog records experimental external tools reviewed on 2026-07-10.
It is not a support list or a performance ranking.

Run the read-only catalog command:

```bash
./scripts/setup_feedforward_sota.sh list
```

The setup script pins each source checkout to a full commit SHA.
It does not provide a complete Python dependency hash lock.
It also cannot verify every file that an external model downloads at run time.

## Installation gate

Any installation requires explicit acceptance of the unlocked dependency set:

```bash
./scripts/setup_feedforward_sota.sh mapanything \
  --accept-unlocked-dependencies
```

Known non-commercial weights require another flag:

```bash
./scripts/setup_feedforward_sota.sh vggt \
  --accept-unlocked-dependencies \
  --accept-noncommercial
```

Missing or unspecified terms require `--accept-unlicensed`.
Review the current upstream license and model card before acceptance.
The setup command fails before it writes files when an acceptance flag is absent.

Use an isolated development environment.
Setup rejects an upstream checkout that contains modified or untracked files.
Do not use this setup as a production supply-chain boundary.

## Recorded catalog

| ID | Local category | Pinned source revision | Recorded terms class |
|---|---|---|---|
| `vggt` | COLMAP geometry export | `a288dd0f14786c93483e45524328726ab7b1b4ce` | Non-commercial weights |
| `mapanything` | COLMAP geometry export | `c845b8f4f6cde0c20aecd87573656c3f69f5b2b0` | Permissive option recorded |
| `pi3` | PLY point geometry | `9fa3ddb3f8d53041f8b2738df404f62223bbaa7b` | Non-commercial weights |
| `amb3r` | PLY point geometry | `92c4081f910f98e683503092b85301861519175e` | No source license recorded |
| `yonosplat` | Dataset evaluation | `8bbbfa861cabf0e815b06acf560d491aa81031f7` | Permissive terms recorded |
| `spfsplatv2` | Dataset evaluation | `14baff7ac8b4e3de59e76f23a74f92b327e76c07` | Weight terms unspecified |
| `moge2` | Single-image geometry | `07444410f1e33f402353b99d6ccd26bd31e469e8` | Permissive terms recorded |

The table reports the review record in the repository.
It does not replace the current upstream license text.

## Output categories

The script groups tools by their documented local integration shape.

### COLMAP geometry export

A tool in this category can create a COLMAP-style sparse project.
Inspect the result before you use it with the pipeline.

```bash
./scripts/pipeline.sh external-colmap-project result \
  --skip-colmap \
  --opensplat /reviewed/bin/opensplat
```

The pipeline requires a complete sparse model and an image directory.

### PLY point geometry

A tool in this category writes point geometry as PLY.
That file is not automatically a trained Gaussian scene.

Inspect it before conversion:

```bash
./build/dev/melkor inspect external.ply --strict
```

The conversion preserves only the semantics that the PLY file provides.

### Dataset evaluation

These tools use an external dataset configuration.
They are not generic folder-of-images commands.

Read the documentation from the pinned checkout before use.
Do not infer an input contract from the generated wrapper name.

### Single-image geometry

This category produces per-image geometry.
It does not provide a joint multi-view scene contract through Melkor.

## Generated wrappers

The setup command can create a project-root `*-infer` wrapper.
Each wrapper activates one local virtual environment.
It checks the pinned source revision and the clean checkout state.
It then runs the entry point recorded in the setup script.

Treat that entry point as experimental.
Run its local help and inspect the pinned checkout before execution.

## Evidence requirements

Record these items for each external run:

- Catalog ID and pinned source SHA
- Current code license
- Current weight license
- Python environment lock or package inventory
- Model revision and file digests
- Input dataset identity
- Full command and configuration
- Output file digest

Do not make quality or speed claims without a reproducible benchmark.
See [Benchmarks](../benchmarks/README.md) for the evidence format.

## Production status

No catalog entry is a production Melkor adapter.
The planned adapter runner must pin dependencies and validate output semantics.

Production support requires the conditional adapter gate in
[the production blocker register](audit/production-blockers.md).
