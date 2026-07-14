# Roadmap

This roadmap lists only work that is scheduled, in progress, or explicitly deferred. It is not a
wish list. An item appears under `v2.0.0` only when it is a release blocker for that version.

Current state: **v2 hardening in progress. No production binary release is currently supported.**
The `1.x` releases remain downloadable but are not the supported production line, and `main` is
development software.

## v2.0.0

The first release that can honestly be called production-grade. Its full blocker register is
[`docs/audit/production-blockers.md`](docs/audit/production-blockers.md); its release gate is the
master checklist in the production-hardening blueprint.

The product boundary for this release is deliberately narrow. Melkor v2 is a **dependable asset
interoperability core**, not an umbrella repository that appears to natively provide every
reconstruction model.

**Core, and supported.**

- Inspect PLY, SPZ, glTF, and GLB assets without initializing a GPU.
- Resolve explicit semantic profiles rather than guessing activation domains or quaternion order.
- Validate canonical invariants and enforce shared resource limits.
- Normalize coordinate frame, units, scale domain, opacity domain, quaternion layout, SH basis
  and order, and colour space.
- Convert between supported PLY, SPZ, and glTF representations with a machine-readable loss
  report.
- Initialize degree-0 Gaussians from mesh vertices or deterministic area-weighted triangle
  samples.
- An installable C ABI and C++17 SDK, a stable `melkor` CLI, and a `melkor3d` Python package.
- A local-only web viewer and a signed desktop package.
- Complete release artifacts: checksums, SBOMs, signatures, attestations, and clean-room install
  evidence.

**Deliberately excluded from v2.0.0.** These are not "later" items that were forgotten; they are
out of scope by design, because adding them would make the feature list longer without making the
product more dependable.

- A native learned scene-reconstruction model.
- Automatic download or execution of unreviewed model code.
- Any claim to select the globally best reconstruction method.
- Network URL loading in core file readers or in the viewer by default.
- Silent coordinate or profile inference when confidence is ambiguous.
- A stable ABI for C++ standard-library types.
- Dynamic third-party plugins loaded from untrusted directories.
- Transparent fallback from an unavailable GPU path to a semantically different CPU algorithm.

## Post-v2.0

Candidate work, not committed. Each item needs its own design and evidence before it is
scheduled.

- Windows ARM64 and a native CUDA Windows package, if repeatable hardware qualification exists.
- Optional glTF compression and packing extensions for Gaussian attributes, once the pinned
  specification permits them and conformance evidence exists.
- An SPZ-in-glTF adapter path, if such an extension exists in the pinned ecosystem revision.
- A WebGPU renderer path in the viewer, promoted from experimental only after it passes the
  target browser matrix.
- A `--replace-input` wrapper that writes and validates a sibling temporary and atomically
  replaces the input only after explicit confirmation.
- An anisotropic scale mode for narrow triangles in `mesh-init --mode surface`.

## Experimental

Built off by default, excluded from production claims, and not covered by the support policy.
An experimental feature may be promoted, kept experimental, or removed — being listed here is not
a commitment to ship it.

- **`melkor complete --method geometric-gap-fill`** — a bounded deterministic geometric
  interpolation of sparse local neighbourhoods. It is not learned reconstruction and it is not a
  replacement for gradient-based training densification. It stays experimental unless its
  false-bridge rate, transmittance behaviour, and useful operating region are demonstrated by
  benchmark.
- **Metal and CUDA backends** — compiled but not production-published until exact-SHA hardware
  parity and performance evidence exists. A compile-only CI job is not hardware qualification.
- **Feedforward and depth adapters** (for example Depth Anything 3) — external, and blocked on
  licence review of both code and weights, hash-pinned weights, explicit user acceptance, and
  reference tests for output semantics.
- **Trainer adapters** (for example OpenSplat, gsplat) — external, and "supported" only once an
  exact pinned configuration passes an end-to-end test.

## What "supported" means

A capability is supported only if all of the following are true. Anything else is labelled
experimental, built off by default, and excluded from production claims.

1. It has a documented public contract.
2. It is enabled in at least one distributed artifact.
3. It has positive and negative automated tests.
4. It is included in the compatibility matrix.
5. It has a named owner.
6. Its dependencies and licence are recorded.
7. It passes the applicable release platform and hardware matrix.
8. Its failures produce stable diagnostics.
9. It is covered by the security policy.
10. It has a support and deprecation policy.
