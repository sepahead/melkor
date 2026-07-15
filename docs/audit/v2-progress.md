# v2 hardening progress

A living summary of where the `v2.0.0` production-hardening program stands. The authoritative,
per-blocker status is in [`production-blockers.md`](production-blockers.md); this is the
narrative overview and the record of what needs a human decision.

Last updated: 2026-07-15. Baseline commit: `21c8fb5`.

## Blocker status

40 findings tracked (18 P0, 15 P1, 7 P2). As of this update:

- **8 closed** with attached evidence: P0-04, P0-05, P0-07, P0-08, P0-16, P1-02, P1-03, P1-14.
- **11 in progress** with substantial verified work landed: P0-01, P0-06, P0-10, P0-11, P0-12, P0-14, P0-17, P1-06, P1-12, P1-15, P2-04.
- **21 open.**

## What is closed, and how it was verified

Every change below builds with `MELKOR_WERROR=ON` (zero warnings), passes the full test suite,
and — where it touches untrusted-input or numeric code — is clean under AddressSanitizer and
UndefinedBehaviorSanitizer. The suite grew from 13 to 23 targets.

| Blocker | What it was | The fix, and its evidence |
|---|---|---|
| **P0-08** | The SPZ writer truncated your existing file on open, then `std::remove`d it on any failure — a failed conversion destroyed the original | `AtomicWriter`: same-directory `O_EXCL` temp, atomic replace, destination never opened until commit. 68 checks with failure injection at every phase. |
| **P0-07** | `property float red = 0.5` was divided by 255 → rendered near-black | Colour scaling driven by the declared source type. `ply_color_tests`: written to fail against the bug (it did), now 23 checks. |
| **P0-05** | `melkor_core` ↔ backends linked circularly; a *suppressed Apple linker warning* was load-bearing | Registry inversion + a `melkor_runtime` layer. Graph verified acyclic from CMake's own dependency data. |
| **P0-16** | AGPL OpenSplat and research-only weights shipped inside the MIT core | 68,134 lines of restricted source removed; attribution preserved; history tagged `archive/pre-v2-research-bundle-20260714`. |
| **P0-04** | No installable SDK — downstream C++ consumption impossible | Shared `libmelkor` + stable C ABI + `MelkorConfig.cmake`. `scripts/test_sdk_install.sh` proves the build→install→consume-from-C-and-C++→relocate cycle. |
| **P1-02** | `rawContext()` returned a `void*` callers cast to `metal::MetalContext*` | Removed; the two ops it enabled are on the abstract interface. No platform type outside `src/metal/`, `src/cuda/`. |
| **P1-03** | Dead `GaussianFitter`/`FeedforwardModel` facades still compiled and public | 2,952 lines deleted, which also removed the last 20 shell-exec sites from the core. |
| **P1-14** | Only `main` was supported | `SUPPORT.md` + rewritten `SECURITY.md` attach support to the immutable `2.0.x` line. |

## Findings beyond the blueprint

Three things the review surfaced that were not in the original blueprint, all now fixed or
documented:

1. **The vendored SPZ codec was an undocumented fork.** Its two changed files matched no upstream
   tag. The two local changes (a header-only version probe; bounds on `fractionalBits` plus
   finite logits for packed alpha, where even a default opacity of 1.0 round-tripped to
   non-finite) are now numbered patch files with rationale that provably reconstruct the vendored
   tree from pristine upstream.
2. **tinygltf and stb shipped with no licence text at all.** Both are now vendored, along with the
   licence for nlohmann/json, which tinygltf embeds.
3. **An adversarial review of the new code found 17 confirmed bugs** (19 refuted). The load-bearing
   ones — a path-redaction prefix leak, an atomic-writer umask bug making output world-readable,
   model-weights slipping through the source-bundle allowlist, a `package-lock.json` version
   rewrite corrupting dependency versions — are fixed with regression tests.

## Infrastructure now in place

- Single `VERSION` source; every surface derived and CI-enforced (`check_version_sync.py`).
- `third_party/manifest.lock.json` pinning every dependency by commit SHA + content digest, with
  declared patches (`verify_third_party.py`).
- Generated `NOTICE`/`THIRD_PARTY_LICENSES.md` (`generate_notices.py`).
- Allowlist-driven, reproducible source bundle (`build_source_bundle.py`).
- The safety substrate: `Result<T>` + stable diagnostics, checked arithmetic, resource-limit
  profiles + `Budget`, cancellation.
- The canonical math oracle: activation, quaternion, and covariance `Σ' = AΣAᵀ` transform.
- Prose claim lint + benchmark manifest schemas.
- A stable `CI Gate` status check and `CMakePresets.json`.
- Governance: `GOVERNANCE.md`, `MAINTAINERS.md`, `CODE_OF_CONDUCT.md`, issue forms, ten-lens PR
  template. Security: threat model + rewritten policy with DoS in scope.

## What remains, and what it needs

### Large, implementable without external resources (the bulk of the remaining work)

- **P0-06** canonical scene model with validated invariants (SoA storage, no uninitialised
  scalars, SH degree 0–4).
- **P0-17 remainder** SH rotation (Wigner-D, degree 2–4) and migrating the existing transform
  paths onto the oracle.
- **P0-09** SPZ v4: pin and wrap upstream v3.0.0 (which sets file-format v4), preserve coordinate
  and antialiasing metadata. Full two-way interop testing needs the official SPZ CLI.
- **P0-10** glTF `KHR_gaussian_splatting`: the reader is now built bottom-up in eight
  independently-tested, ASan-clean modules — the KHR layout core, GLB container framing, the
  accessor decoder, the JSON document parser, node transforms, the extension policy, accessor
  resolution, and a working per-primitive reader that assembles them into a validated local-space
  `SplatData` (including the coefficient-major→splat-major SH transpose). Remaining: the scene-graph
  walk that composes node transforms across primitives and applies `Σ' = MΣMᵀ` and SH rotation, the
  writer, and conformance against the Khronos glTF Validator.
- **P0-11 remainder** the honest `mesh-init --mode surface` area-weighted sampler.
- **P0-15 / P0-13** the typed pipeline stage runner replacing the shell scripts, with pinned
  adapter manifests.
- **WP06/WP13** format registry, probe, loss reports; `inspect`/`normalize` as the product centre.
- **WP20** coverage-guided fuzz targets (libFuzzer) replacing the randomised loops.

### Gated on resources or decisions only the maintainer can provide

These cannot be *truthfully* completed autonomously; doing so would mean fabricating evidence the
blueprint explicitly forbids faking.

- **P0-02 (Python wheels)** — the package can be built with scikit-build-core, but *qualifying*
  and *publishing* wheels for every advertised tag needs PyPI trusted-publishing credentials and
  clean per-platform test environments.
- **P0-03 (Windows)** — MSVC/clang-cl CI and the portability layer are implementable, but the
  "clean Windows VM install/uninstall" qualification needs a Windows runner.
- **P1-13 (GPU hardware qualification)** — Metal and CUDA parity/performance evidence needs
  actual Apple and NVIDIA hardware. Compile-only CI is not qualification.
- **P0-18 / P2-07 (signed release artifacts)** — SBOMs and checksums are scriptable, but
  signatures, notarization, and attestations need code-signing identities and a protected release
  environment.
- **Repository settings** — branch/tag protection, private vulnerability reporting, Discussions,
  immutable releases, and CODEOWNERS enforcement are applied through the GitHub UI/API by an
  account with admin rights.

### The tag/release cleanup decision — RESOLVED 2026-07-15

The maintainer decided to **delete** the `v1.2.0` and `v1.2.1` tags and GitHub releases, an
informed override of the blueprint's rule #1 ("preserve every existing tag") and the release
gate's immutability check. The irreversibility and the provenance/DOI trade-off were stated
before the decision and accepted. Done on 2026-07-15: both releases and both remote tags are
removed; the Releases page is empty ahead of `v2.0.0`. The underlying commits remain in git
history. The archival tag and `v2.0.0-rc.1` are unchanged. Recorded as a deliberate deviation in
`v2-review-baseline.md`; a Zenodo DOI should therefore be minted against `v2.0.0` when it ships,
not against any deleted tag.
