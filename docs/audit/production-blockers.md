# v2.0.0 production blockers

This register lists the current release gates for the narrow Melkor v2 product.
It replaces the 2026-07-14 implementation backlog.
The dated audit files preserve that historical review.

Last reviewed against `main`: 2026-08-20.

## Release boundary

The native v2 release contains these supported surfaces:

- The `melkor inspect` and `melkor convert` commands
- The stable C ABI in `melkor/c/melkor.h`
- The five exact format profiles under `profiles/`
- The JSON schemas under `schemas/`
- The CMake install package

The native release does not include a GPU backend, training, mesh conversion, scene completion, or a Python package.
The local viewer and development adapters have separate release decisions.

## Implemented native baseline

The current source tree implements these release-critical controls:

- Strict PLY, SPZ v1-v3, JSON glTF, and GLB profile handling
- One validated canonical Gaussian model
- Shared limits, memory accounting, cancellation, deadlines, and progress
- Checked arithmetic at untrusted allocation and range boundaries
- Explicit conversion-loss reports and exact severe-loss approval
- Same-directory atomic output with no-overwrite behavior
- A stable C ABI with clean install and relocation consumers
- Stable CLI exit classes, diagnostic codes, and inspect JSON
- Deterministic source evidence from an exact Git tree
- Native CI lanes for macOS, Linux, and Windows

These controls do not make an untagged working tree a supported release.

## Open production gates

| ID | Gate | Required evidence | Status |
|---|---|---|---|
| R0-01 | Exact-commit verification | The full CI matrix, sanitizers, fuzz smoke, no-SPZ build, SDK install, viewer, and release checks pass on the release commit. | Open until the release commit exists. |
| R0-02 | Format conformance | A licensed corpus covers each supported profile. Generated GLB fixtures pass a pinned Khronos glTF Validator. | Open. |
| R0-03 | Release authenticity | Each production artifact has checksums, an SBOM, a signature, and verifiable provenance. | Open. Current evidence is unsigned. |
| R0-04 | Independent review | The external review required by `GOVERNANCE.md` covers parsers, limits, claims, and release evidence. | Open. |
| R0-05 | Supported-release setup | Protected tags, release permissions, private vulnerability reporting, and the support branch policy are verified. | Open. Requires repository administration. |

No production-supported `v2.0.0` release can proceed while an R0 gate is open.

## Conditional distribution gates

| ID | Surface | Condition | Required evidence |
|---|---|---|---|
| R1-01 | Desktop viewer | Apply only if a production desktop bundle ships. | Signed bundles, offline startup, hostile-file tests, and target-operating-system qualification. |
| R1-02 | Development adapters | Apply only if an adapter receives a production support claim. | Immutable tool, Python, model, license, command, and output manifests. |

An excluded surface does not block the native source release.
Release notes must state each exclusion.

## Declared compatibility limits

These limits are product decisions, not unfinished hidden features:

- SPZ support stops at versions 1 through 3.
- glTF support targets `khr-gaussian-splatting-rc-63770cc`.
- The installed SDK exposes a C ABI, not a stable C++ ABI.
- The native product has no network loader or runtime plugin system.

A future format revision needs a new profile identifier and conformance evidence.

## Evidence rules

- Use evidence from the exact release commit.
- Record every tool version and command.
- Treat a compile-only result as compile evidence only.
- Do not convert an open gate into a limitation statement.
- Do not waive R0 gates.
