# External adapters

Melkor's core is an MIT-licensed asset interoperability library. It does **not** contain a
learned 3D reconstruction model, and it does not redistribute one.

Reconstruction, training, depth, and feedforward systems are separate programs with their own
licences, versions, hardware requirements, and quality characteristics. Melkor reaches them
through **adapters**: immutable manifests that describe how to obtain, verify, invoke, and
validate an external tool. An adapter describes a tool; it does not vendor it.

## Why the research snapshots were removed

Until the v2 hardening program, this repository carried four external trees inside the MIT
core:

| Removed tree | What it was | Licence | Why it could not stay |
|---|---|---|---|
| `tools/OpenSplat/` | Full upstream OpenSplat source snapshot | **AGPL-3.0-only** | Copyleft source shipped inside an MIT distribution makes the redistributable boundary ambiguous for every downstream consumer. `scripts/setup_opensplat.sh` cloned upstream anyway, so the tracked copy was also redundant. |
| `DA3coreml/` | ByteDance Depth Anything 3 research port, plus a Swift CoreML surface | Apache-2.0 code; **model weights under separate, more restrictive terms** | A research snapshot, ~24 MB of tracked source, whose weight terms are not the terms of the MIT core. Code licence and weight licence are different things and were being presented as one. |
| `ml-sharp/` | Apple ml-sharp research snapshot | **Apple Sample Code Licence**; weights research-only, no commercial use | Explicitly quarantined and unused. It contributed nothing to the build and imposed non-commercial terms on anyone reading the tree. |
| `.superstack/` | Agent handoff artifacts and HTML review reports | n/a | Working notes, not product. |

None of this is a judgement about the quality of those projects — they are good work, and the
citations below stand. The point is narrower: **a permissively licensed core must not ship
copyleft or research-only source inside its own distribution**, and a user must be able to tell
which terms apply to what they downloaded.

The full history is preserved. The tree containing all four is tagged:

```text
archive/pre-v2-research-bundle-20260714
```

Nothing was erased; it was moved out of the redistributable artifact.

## Attribution

These projects remain the upstream systems Melkor interoperates with. Cite them, not Melkor,
for the methods they implement.

**OpenSplat** — Piero Toffanin and contributors.
<https://github.com/pierotofy/OpenSplat>. AGPL-3.0-only. A production 3D Gaussian-splat trainer
that runs on CPU, CUDA, and Metal.

**Depth Anything 3** — ByteDance.
<https://github.com/ByteDance-Seed/Depth-Anything-3>. Apache-2.0 code; model weights carry their
own terms and must be accepted separately.

**ml-sharp** — Apple Machine Learning Research. <https://github.com/apple/ml-sharp>. Apple Sample
Code Licence; model weights are licensed for research purposes only, with no commercial use.

**COLMAP** — Johannes Schönberger and contributors. <https://github.com/colmap/colmap>.
BSD-3-Clause. Structure-from-motion and multi-view stereo. Note that **standalone GLOMAP is
deprecated**: global mapping now lives inside COLMAP as the `global_mapper` command, and Melkor's
adapter targets that ([P0-14](../audit/production-blockers.md)).

**gsplat** — Nerfstudio project. <https://github.com/nerfstudio-project/gsplat>. Apache-2.0. A
CUDA-accelerated Gaussian-splat rasterizer and training library.

## How an adapter works

An adapter manifest is **data, not code**. It records, at minimum:

- the exact upstream repository and a full 40-character commit SHA — never a branch, never a
  floating version, never a mutable container tag;
- the source archive URL and its digest;
- the licence of the code, and **separately**, the licence of any model weights;
- whether the terms require explicit user acceptance before installation or execution;
- the executables it provides and how to verify their version;
- the exact command as an argv token array — never a shell string;
- its declared inputs and outputs, with the media type and semantic profile of each;
- resource limits: timeout, maximum log bytes;
- how to verify that the outputs are actually correct, not merely that the process exited zero.

An adapter is called **supported** only when that exact pinned configuration has passed an
end-to-end test. Anything else is experimental, off by default, and excluded from production
claims.

The adapter protocol, the process runner, and the run manifests are specified in
[the pipeline documentation](../pipeline/index.md). They are delivered by work package 18 and are
not yet complete; until then, the `scripts/setup_*.sh` installers remain in place and still
perform mutable clones, which is tracked as
[P0-13](../audit/production-blockers.md).

## Licence boundary

Melkor's core is MIT. Invoking an externally installed AGPL program from an MIT program does not
relicense the MIT program — but **distributing** them together, or offering them as a network
service, raises questions that depend on your jurisdiction and your distribution model.

Melkor's position is deliberately conservative:

- restricted source and restricted weights are **not** in the core source bundle, the Python
  wheel, or the viewer artifacts;
- the adapter manifest records code terms and weight terms as separate fields, because they
  routinely differ;
- an adapter whose terms require acceptance refuses to install or execute until that acceptance is
  recorded, with the adapter ID, the digest of the licence text that was accepted, and when;
- accepting one adapter's terms grants nothing for any other adapter.

Review each adapter's licence, and each weight file's licence, before you install or use it.
Melkor does not and cannot grant you rights to them.
