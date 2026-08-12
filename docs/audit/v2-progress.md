# v2 hardening progress

Melkor now has a narrow and internally consistent v2 product design.
The authoritative release gates are in [`production-blockers.md`](production-blockers.md).

Last updated: 2026-08-12.

## Current product state

The native product inspects and converts Gaussian-splat assets through one canonical model.
It supports five exact profiles for PLY, SPZ v1-v3, and Gaussian glTF/GLB.

The project removed the old GPU, mesh, scene-completion, and learned-model core surfaces.
It also removed public C++ model headers from the installed SDK.

The CLI now has two explicit commands:

- `melkor inspect`
- `melkor convert`

Conversion uses an exact source profile, exact target profile, resource limits, loss policy, and atomic output.
Inspection uses the same native readers and reports stable JSON when requested.

## Completed hardening areas

- Canonical domains, transforms, covariance, quaternion, and SH rotation
- Strict PLY profile semantics and portable numeric parsing
- Bounded SPZ v1-v3 probe, inflate, decode, and encode
- Strict Gaussian glTF/GLB subset with contained resources
- Shared input identity checks and operation controls
- Stable C ABI and relocatable CMake package
- Exact loss-code policy and report schema
- Deterministic source boundary and release evidence
- Local viewer limits, accessibility behavior, and desktop staging
- Explicit documentation of unsupported surfaces

## Remaining release work

The remaining work needs release evidence or authority outside the implementation:

- Run the complete matrix on the final release commit.
- Publish a licensed format corpus and Khronos validator results.
- Sign and attest production artifacts.
- Complete the independent external review.
- Verify protected repository and support settings.

The viewer needs its conditional desktop gate only if the release publishes a desktop bundle.
Development adapters need their conditional gate only if the release claims production support for them.

## Historical audit records

The dated files in this directory describe earlier repository states.
They contain removed APIs and old finding counts by design.
Do not use them as a current capability matrix.
