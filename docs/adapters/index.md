# External adapters

Melkor's core is an MIT-licensed asset interoperability library. It does **not** contain a
learned 3D reconstruction model, and it does not redistribute one.

Reconstruction, training, depth, and feedforward systems are separate programs.
They have their own licenses, versions, hardware needs, and quality characteristics.

Melkor plans immutable adapter manifests for these tools.
The manifest runner is not complete.
Current shell wrappers are development tools and do not provide this boundary.

## Why the research snapshots were removed

Until the v2 hardening program, this repository carried four external trees inside the MIT
core:

| Removed tree | What it was | License | Why it could not stay |
|---|---|---|---|
| `tools/OpenSplat/` | Full upstream OpenSplat source snapshot | **AGPL-3.0-only** | Copyleft source shipped inside an MIT distribution makes the redistributable boundary ambiguous for every downstream consumer. `scripts/setup_opensplat.sh` cloned upstream anyway, so the tracked copy was also redundant. |
| `DA3coreml/` | ByteDance Depth Anything 3 research port, plus a Swift CoreML surface | Apache-2.0 code. **Model weights have separate, more restrictive terms.** | A research snapshot, ~24 MB of tracked source, whose weight terms are not the terms of the MIT core. Code and weight licenses are different and were presented as one. |
| `ml-sharp/` | Apple ml-sharp research snapshot | **Apple Sample Code License**. Weights are research-only with no commercial use. | Explicitly quarantined and unused. It contributed nothing to the build and imposed non-commercial terms on anyone reading the tree. |
| `.superstack/` | Agent handoff artifacts and HTML review reports | n/a | Working notes, not product. |

This removal is not a judgment about the quality of these projects. The citations below remain.
The reason is narrow. **A permissively licensed core must not ship copyleft or research-only
source in its distribution.** Users must also know which terms apply to each download.

The full history is preserved. The tree containing all four is tagged:

```text
archive/pre-v2-research-bundle-20260714
```

Nothing was erased. It was moved out of the redistributable artifact.

## Attribution

These projects remain the upstream systems Melkor interoperates with. Cite them, not Melkor,
for the methods they implement.

**OpenSplat** — Piero Toffanin and contributors.
<https://github.com/pierotofy/OpenSplat>. AGPL-3.0-only. An external 3D Gaussian-splat trainer.

**Depth Anything 3** — ByteDance.
<https://github.com/ByteDance-Seed/Depth-Anything-3>. Apache-2.0 code. Model weights carry their
own terms and must be accepted separately.

**ml-sharp** — Apple Machine Learning Research. <https://github.com/apple/ml-sharp>. Apple Sample
Code License. Model weights are licensed for research purposes only, with no commercial use.

**COLMAP** — Johannes Schönberger and contributors. <https://github.com/colmap/colmap>.
BSD-3-Clause. Structure-from-motion and multi-view stereo. Note that **standalone GLOMAP is
deprecated**: global mapping now lives inside COLMAP as the `global_mapper` command, and Melkor's
adapter targets that ([P0-14](../audit/production-blockers.md)).

**gsplat** — Nerfstudio project. <https://github.com/nerfstudio-project/gsplat>. Apache-2.0. A
CUDA-accelerated Gaussian-splat rasterizer and training library.

## Planned adapter contract

An adapter manifest is **data, not code**. It records, at minimum:

- The exact upstream repository and a full 40-character commit SHA. Do not use a
  branch, floating version, or mutable container tag.
- The source archive URL and its digest
- The code license and, **separately**, the license of each model weight
- Whether the terms require explicit user acceptance before installation or execution
- The executables it provides and their version checks
- The exact command as an argument array. Never use a shell string.
- Its declared inputs and outputs with their media types and semantic profiles
- Resource limits, including timeout and maximum log bytes
- Output validation that does not rely only on a zero process exit code

An adapter is called **supported** only when that exact pinned configuration has passed an
end-to-end test. Anything else is experimental, off by default, and excluded from production
claims.

The adapter protocol, process runner, and run manifests are not complete.
[The pipeline documentation](../PIPELINE.md) describes the temporary wrapper.

Mutable general trainer installers now fail closed.
DA3 and feedforward setup use detached source revisions.
Their Python dependencies still need complete hash locks.
The blocker register tracks this issue as
[P0-13](../audit/production-blockers.md).

## License boundary

Melkor's core is MIT. Invoking an externally installed AGPL program from an MIT program does not
relicense the MIT program. However, combined **distribution** or network service can create
additional obligations. These obligations depend on your jurisdiction and distribution model.

Melkor's position is deliberately conservative:

- Restricted source and restricted weights are **not** in the core source bundle, Python wheel,
  or viewer artifacts.
- The adapter manifest records code terms and weight terms in separate fields because they
  frequently differ.
- An adapter that needs acceptance refuses to install or run before acceptance. The record
  includes the adapter ID, accepted-license digest, and acceptance time.
- Acceptance of one adapter's terms grants nothing for a different adapter.

Review each adapter license and weight-file license before installation or use.
Melkor does not and cannot grant you rights to them.
