# gsplat CUDA integration status

Melkor does not include or install gsplat.
The repository also does not provide a pinned gsplat command contract.

The old `scripts/setup_gsplat_cuda.sh` command cloned a mutable branch.
It also installed unpinned Python packages.
The command is now a failure-only migration stub.

## Current boundary

Use gsplat as an independent external tool.
Review its source revision, dependencies, CUDA runtime, and license before use.

Provide the generated PLY file to Melkor after the external run:

```bash
./build/dev/melkor inspect external-result.ply --strict
./build/dev/melkor inspect result.spz --strict
```

Melkor validates and converts the output file.
It does not attest to the training process or the output quality.

## Distributed training

The removed setup script generated a `torchrun` wrapper.
That generated wrapper is not part of the reviewed repository.
Melkor does not test its distributed behavior.

Use the command from your pinned gsplat revision.
Run its local help before you start a distributed job.
Record these items with the result:

- Source revision
- Python lock or environment digest
- PyTorch and CUDA versions
- GPU models and count
- Full command and configuration
- Dataset identity and digest
- Output file digest

Do not compare run time or quality without the same dataset and settings.

## Pipeline status

`scripts/pipeline.sh` supports only the narrow OpenSplat wrapper contract.
It rejects the old `--tool gsplat-cuda` value.

Run gsplat directly until the adapter runner provides a pinned manifest.
Track that work in [the production blocker register](audit/production-blockers.md).

## Security

Create the gsplat environment outside the source tree when possible.
Do not run mutable setup commands with production credentials.
Do not load an untrusted Python checkpoint in a privileged process.

Treat the external output as untrusted input.
Use `melkor inspect` with a suitable limits profile before conversion.

## References

The external project is listed in [External adapters](adapters/index.md).
Melkor does not mirror its documentation because the upstream interface can change.
