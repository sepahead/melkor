# Roadmap

Melkor `2.0.0-dev` has no supported production release.
The existing `v2.0.0-rc.1` tag has no production support.

This roadmap defines the product boundary and the remaining release outcomes.
The [production blocker register](docs/audit/production-blockers.md) tracks finding-level evidence.

## Product boundary

Melkor is a Gaussian-splat asset interoperability toolkit.
The native core inspects and converts supported PLY, SPZ, glTF, and GLB assets.

The core includes:

- Explicit format profiles
- Canonical Gaussian data and metadata
- Bounded readers and writers
- Stable diagnostics and JSON reports
- Explicit conversion-loss approval
- Atomic output replacement
- A narrow C ABI

The local viewer is a separate product surface.
It uses pinned SparkJS and three.js files.

Development adapters can invoke external reconstruction tools.
An adapter does not make an external tool part of Melkor.

## v2.0.0 release outcomes

The first supported release must complete these outcomes:

1. Freeze and document the CLI, C ABI, schemas, and profile identifiers.
2. Close every P0 and P1 production blocker.
3. Validate each supported format with licensed conformance fixtures.
4. Run the official glTF validator on each generated GLB fixture.
5. Freeze the declared SPZ v1-v3 and glTF release-candidate compatibility limits.
6. Pass clean native builds on each claimed operating system.
7. Pass sanitizer, fuzz, install, relocation, and hostile-input checks at the release commit.
8. Publish checksums, SBOMs, signatures, attestations, and clean-install evidence.
9. Publish a support matrix that names exact operating systems, compilers, and formats.
10. Obtain the independent review required by [GOVERNANCE.md](GOVERNANCE.md).

The project can release the viewer separately from the native core.
An unsigned developer bundle cannot satisfy a production desktop claim.

## Excluded from v2.0.0

These capabilities are outside the v2 native core:

- Gaussian training from photographs
- A learned reconstruction model
- Mesh-to-Gaussian initialization
- Scene completion or densification
- GPU compute backends
- A Python package
- A stable C++ ABI
- Runtime plugin loading
- Network URL loading
- Automatic model or weight downloads

The project must not retain a dormant public API for an excluded capability.
It can add a capability later through a separate design and compatibility review.

## Post-v2 candidates

These items have no release commitment:

- SPZ revisions after version 3
- A shared WebAssembly inspection core for the viewer
- A typed, modular viewer application
- A Python package over the stable C ABI
- Additional Gaussian-splat glTF extensions
- A WebGPU renderer adapter
- Windows ARM64 packages

Each candidate needs a public contract, threat analysis, dependency record, and conformance evidence.

## Meaning of support

A capability is supported only when all conditions are true:

1. A public contract defines its input, output, and failure behavior.
2. A distributed artifact enables it.
3. Positive, boundary, and hostile-input tests cover it.
4. The release support matrix names it.
5. Its dependencies and licenses are recorded.
6. Its release platforms pass at the exact release commit.
7. Stable diagnostics identify its failures.
8. The security and deprecation policies cover it.

Source code, a compile-only path, or a passing unit test does not establish support by itself.
