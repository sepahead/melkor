# Build Context

> Auto-generated project handoff. Update this file when release, validation, or
> architecture status materially changes.

## Stack

| Field | Value |
|---|---|
| Template | Polyglot native/ML/viewer repository |
| Architecture pattern | C++17 core and CLI, CPU/Metal/CUDA providers, Python DA3 bridge, Spark/Three web viewer, Tauri desktop shell |
| Completed at | 2026-07-10T23:49:18Z |
| Reviewed baseline | Rewritten 43-commit history and complete `2.0.0-rc.1` candidate tree; final handoff commit follows the rewritten RC tree |

### Skills Installed

- `review-and-iterate`
- `frontend-design-guidelines`
- `number-formatting`
- `product-review`

## Build Status

| Gate | Status |
|---|---|
| Maintained source surfaces reviewed | Yes |
| Local test matrix passing | Yes |
| CI matrix hardened | Yes; ordinary CI and source-evidence workflow now run for `v*-rc.*` tags and must pass remotely after commit |
| Source release candidate | Code-ready at `2.0.0-rc.1`; rewritten `main` is pushed, while the annotated tag remains gated on final main CI |
| Signed/notarized production bundles | No; RC evidence is deliberately unsigned and production signing remains an explicit gate |
| Devnet deployed | No; not applicable to this non-Solana project |
| Mainnet deployed | No; not applicable to this non-Solana project |

### Milestones

- [x] Harden untrusted glTF/GLB parsing and scene traversal.
- [x] Align CPU/Metal/CUDA reference semantics and validate Metal gradients.
- [x] Retire misleading native fitting/feedforward facades.
- [x] Make the DA3 bridge revision-pinned, camera-aware, and fail-closed.
- [x] Quarantine unsupported CoreML and vendored research surfaces.
- [x] Harden the viewer, Tauri CSP, deterministic staging, and asset provenance.
- [x] Add locked CI, dependency/license policy, secret scanning, and install notices.
- [x] Validate local native, Python, Swift, viewer, Rust, and packaging surfaces.
- [x] Add deterministic, backend-free `melkor inspect` diagnostics and hostile-input regression coverage.
- [x] Add private offline local-file picker/drag-drop viewing with transactional static/4D recovery.
- [x] Synchronize `2.0.0-rc.1` across native, npm, Tauri, Cargo, lockfiles, changelog, and notices.
- [x] Add deterministic source archives, file checksums, SPDX source SBOM, unsigned provenance, verification, and RC tag automation.
- [x] Close final RC blockers in glTF graph/optional-attribute validation, PLY record framing, enhanced large-coordinate hashing, SPZ endpoint/quantization round-trips, and evidence generator/ref binding.
- [x] Reject unsupported glTF sparse accessors consistently instead of silently dropping their overlays.
- [x] Bound SPZ fixed-point fractional bits and remove attacker-controlled shift undefined behavior.
- [x] Apply viewer allowlisting after canonical path resolution and cover encoded prefix traversal.
- [x] Make release-evidence fixture commits independent of developers' global GPG-signing configuration.
- [x] Correct project owner attribution to Sepehr Mahmoudian across license, notice, release metadata, desktop metadata, staged assets, and rebuilt binaries.
- [x] Rewrite every reachable commit and both existing annotated tags to remove the incorrect historical attribution; local and fresh GitHub mirror object scans pass.
- [ ] Obtain GitHub Support cache garbage collection: obsolete unreferenced SHAs/blobs remain directly addressable even though no advertised branch, tag, pull request, or fork contains them.
- [ ] Add tag-protected binary signing, notarization, binary SBOM, and artifact attestation.
- [ ] Qualify CUDA at runtime on representative NVIDIA hardware.
- [ ] Lock DA3 transitive Python packages per supported CUDA/Python matrix.

## Review

| Field | Result |
|---|---|
| Security score | A |
| Correctness score | A |
| Quality score | A |
| Source RC engineering score | A- |
| Production distribution score | B |
| Product score | 8.1 / 10 |
| Open critical findings | 0 |
| Open high findings | 0 |
| Ready for mainnet | No; not applicable |

### Remaining findings

| Severity | Area | Finding | Required action |
|---|---|---|---|
| Medium | GitHub object cache | Advertised refs, fresh clones, current PRs, and all forks are clean, but GitHub still serves the obsolete unreferenced commit and two replaced blobs by SHA | With explicit authorization, request GitHub Support cache removal/server GC using first changed commit `e815eae05d07b2416009b3ea23d92465be542fa2`; affected pre-rewrite PRs: 0; forks: 0 |
| Medium | RC tag | The annotated RC tag is intentionally not cut before the rewritten main tip is green | Require final main CI green, then create annotated `v2.0.0-rc.1` and require all tag CI/evidence jobs green |
| Medium | Distribution | RC source evidence is unsigned; there is no production binary signing/notarization/SBOM/attestation pipeline | Implement and verify the production gates in `docs/RELEASE.md` before publishing desktop bundles |
| Medium | Hardware validation | CUDA is compile-gated but was not runtime-tested in this Apple-hosted review | Run provider parity and representative workloads on a real NVIDIA matrix |
| Medium | ML reproducibility | DA3 source/checkpoints are immutable, but upstream Python transitive packages still resolve dynamically | Publish tested lockfiles per Python, PyTorch, and CUDA combination |
| Low | Rust dependencies | Reviewed unmaintained transitive advisories are temporarily excepted through 2026-10-01 | Track Tauri/GTK migration and remove exceptions by the review date |
| Low | Native training | The reference differentiable renderer has no production optimizer/tiled sorter; the fake native fit CLI is disabled | Add benchmarked optimizer and renderer tests before exposing native fitting again |

## Source Reports

- `melkor-sota-review-2026-07-10.html`
- `melkor-product-review-2026-07-10.html`
- `melkor-2.0.0-rc1-readiness.html`
- `../docs/INSPECT.md`
- `../docs/RELEASE.md`
