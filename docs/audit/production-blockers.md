# v2.0.0 production blockers

This is the authoritative blocker register for the `v2.0.0` release. It is copied from the
production-hardening review of 2026-07-14 and is the list the release gate checks against.

Severity meanings:

- **P0** — can cause incorrect output, data loss, security exposure, impossible installation, or
  a materially false production claim. No release candidate may be cut while one is open.
- **P1** — major ecosystem, portability, quality, or maintainability defect. No final release may
  be cut while one is open.
- **P2** — important hardening or governance work. May be deferred only with a written, public
  limitation and a named owner and date.

Status values: `open`, `in-progress`, `closed`. A blocker moves to `closed` only when its
acceptance evidence is attached — a passing test, a published artifact, or a settings change with
proof. "It should work" is not evidence.

## P0 — release-candidate blockers

| ID | Area | Finding | Required disposition | WP | Status |
|---|---|---|---|---|---|
| P0-01 | Versioning | GitHub Releases presents `v1.2.1` as Latest while the source identifies as `2.0.0-rc.1`; the support policy points users at `main`. | One version source, one supported release line, immutable tags, synchronized package metadata. | WP01, WP23, WP24, WP26 | open |
| P0-02 | Product claim | The GitHub description says Python/C++, but the root `pyproject.toml` is Ruff configuration, not an installable package. | Publish tested Python bindings, or remove Python from every product claim until they exist. | WP16 | open |
| P0-03 | Product claim | "Cross-platform" currently means macOS and Linux. There is no supported Windows native build. | Add and qualify Windows CPU support, or stop saying cross-platform. | WP17 | open |
| P0-04 | SDK packaging | CMake installs the CLI and notices but not the core library, public headers, package config, version file, or exported targets. | Build and install a consumable SDK; test it from a separate consumer project. | WP03 | **closed** — shared `libmelkor` with a stable C ABI, `MelkorConfig.cmake`/`MelkorTargets`/version file, and installed headers. `scripts/test_sdk_install.sh` builds, installs, consumes from standalone C and C++ projects via `find_package(Melkor)`, and relocates the install. |
| P0-05 | Build graph | `melkor_core` and each backend link to one another `PUBLIC`, creating a static-archive cycle that requires Apple duplicate-library suppression. | Replace with an acyclic target graph; remove the linker-warning suppression. | WP03 | **closed** — registry inversion; graph verified acyclic from CMake's own dependency graph; `-no_warn_duplicate_libraries` removed; `src/gpu_stub.cpp` deleted. |
| P0-06 | Public data model | Default-constructed Gaussian values contain uninitialized scalars; SH degree can be set out of range; mutable storage bypasses invariants. | Initialized, validated constructors/builders and invariant-preserving mutation. | WP04 | open |
| P0-07 | PLY correctness | Plain RGB is normalized as though every source type were 8-bit, so float RGB in `[0,1]` is divided by 255 and renders nearly black. | Typed PLY property conversion plus regression and conformance fixtures. | WP07 | **closed** — colour scaling is now driven by the declared source type (uchar/255, ushort/65535, float as-is). `ply_color_tests`: 23 checks including the float-RGB regression the blueprint names, clean under ASan+UBSan. |
| P0-08 | Output integrity | Writers target the final path directly; the SPZ path may remove the destination after an encode failure. | Route every output through a same-directory atomic writer. Never delete a pre-existing destination on failure. | WP05 | **closed** — `AtomicWriter` (same-directory `O_EXCL` temp, atomic replace, destination never opened until commit). SPZ and PLY writers migrated. 68 checks including failure injection at every phase; clean under ASan+UBSan. |
| P0-09 | SPZ ecosystem | Melkor assumes legacy SPZ v1–v3 while upstream writes file-format v4 with Zstandard streams, coordinate extensions, antialiasing metadata, and SH degree up to 4. | Integrate and pin current upstream SPZ v4; preserve metadata; enforce budgets; prove two-way interoperability. | WP08 | open |
| P0-10 | glTF ecosystem | Required glTF extensions are rejected broadly, and `KHR_gaussian_splatting` is not implemented. | Implement the pinned release-candidate specification, required-extension semantics, and conformance fixtures. | WP09 | open |
| P0-11 | Misleading conversion | The "Enhanced" mesh path decodes indices but does not use topology, creates one splat per vertex, and exposes density/subdivision controls that do nothing. | Remove the name and the dead flags. Ship `mesh-init --mode vertices` and a real triangle-aware surface sampler. | WP10 | in-progress — the unimplemented density/subdivision/`target_splats_per_unit` config fields are removed, the magic `* 50` is named and explained, and the header states plainly that the path is a geometric per-vertex initialiser, not trained reconstruction. **Remaining:** the honest `mesh-init --mode vertices\|surface` command with a real area-weighted triangle sampler, and the CLI rename (WP10/WP15). |
| P0-12 | Resource security | The security policy excludes denial of service from large well-formed input, and core readers leave important sizes effectively unbounded. | Put resource exhaustion in scope; enforce shared byte/count/memory/decompression/time limits. | WP05, WP22 | in-progress — the substrate landed (`Budget`, limit profiles, hard ceilings, checked arithmetic, decompression-ratio guard) and `SECURITY.md` now puts resource exhaustion explicitly **in scope**. **Not closed:** the substrate is not yet enforced on the parsing path — no reader calls the checked-arithmetic helpers or takes an `OperationContext`. That migration is WP07–WP09 and is the single largest remaining gap. |
| P0-13 | External tools | Setup scripts clone or pull mutable branches and download large toolchains or weights with no immutable lock or checksum policy. | Adapter manifests pinning source revisions, archive digests, environments, weights, licences, and command contracts. | WP18 | open |
| P0-14 | Obsolete dependency | The repository installs the deprecated standalone GLOMAP even though GLOMAP moved into COLMAP as `global_mapper`. | Remove the standalone installer; implement a pinned COLMAP global-mapper adapter. | WP18 | open |
| P0-15 | Shell safety | Retired feedforward code uses shell execution and predictable temporary paths; pipeline scripts contain permissive failure patterns such as ignored conversion failures. | Delete retired code from the core, prohibit shell interpolation, rewrite orchestration as a typed stage runner. | WP18, WP22 | open |
| P0-16 | Licensing boundary | AGPL and research-only snapshots ship beside the MIT core, making the redistributable boundary ambiguous. | Remove snapshots from the core source tree; distribute only manifest-driven external adapters. | WP02 | **closed** — `tools/OpenSplat` (AGPL-3.0), `DA3coreml`, `ml-sharp`, and `.superstack` removed; attribution preserved in `docs/adapters/index.md`; history preserved at tag `archive/pre-v2-research-bundle-20260714`. |
| P0-17 | Mathematical semantics | The CPU transform path can change positions without correctly transforming covariance, orientation, scale, or SH; domain metadata is too weak to prevent double activation. | Explicit canonical domains and mathematically correct affine and SH transforms with reference tests. | WP11 | in-progress — canonical math oracle landed: activation domains (single-conversion, anti-double-activation), quaternion contract (stable matrix round-trip incl. 180°, q/-q equivalence, zero-quat rejection), and the covariance affine transform `Σ' = AΣAᵀ` with a deterministic Jacobi eigen-decomposition, all reference-tested (`math_oracle_tests`, 83 checks). **Remaining:** SH rotation (Wigner-D, degree 2–4) and migrating the existing format/backend transform paths onto the oracle. |
| P0-18 | Release claim | The source-evidence workflow does not produce signed platform binaries, installable SDK packages, Python wheels, desktop applications, or hardware-qualified CUDA artifacts. | Implement the complete artifact and release matrix plus clean-room installation tests. | WP26 | open |

## P1 — final-release blockers

| ID | Area | Finding | Required disposition | WP | Status |
|---|---|---|---|---|---|
| P1-01 | Provider API | Backend calls expose raw `void*`, boolean success, and vectors, with no typed capabilities, cancellation, progress, budgets, or stable diagnostics. | Typed backend ABI and a CPU reference contract. | WP12 | open |
| P1-02 | Header portability | Platform-neutral public headers include Metal-specific types or headers. | Move platform types behind private implementation or C ABI handles. | WP03, WP12 | **closed** — `rawContext()` (raw `void*`) removed from `ComputeProvider`; the two GPU grid ops promoted to the abstract interface; no `metal::`/`cuda::` type appears outside `src/metal/` and `src/cuda/`. |
| P1-03 | Dead facades | Feedforward and fitting facades remain in the compiled and public surface although key entry points fail closed. | Delete them from the stable core; reintroduce only as external experimental packages. | WP18 | **closed** — `GaussianFitter`, `DifferentiableRenderer`, `MeshRenderer`, `FeedforwardModel`, `PythonBridge`, and `gpu_stub.cpp` deleted (2,952 lines). This also removed the last 20 shell-execution sites from the core (P0-15 core portion). |
| P1-04 | SPZ metadata | Antialiasing and coordinate metadata can be discarded; default SH settings can silently drop coefficients. | Preserve metadata; make any loss explicit and policy-controlled. | WP08 | open |
| P1-05 | PLY robustness | Writer comments accept untrusted line breaks; ASCII precision is not round-trip safe; missing SH is padded silently; output is not streaming. | Sanitize metadata, use `max_digits10`, enforce profile rules, stream bounded data. | WP07 | open |
| P1-06 | Scene semantics | Mesh and glTF conversion does not preserve scene hierarchy, material or texture appearance, skins or morphs, or explicit loss accounting. | Define supported semantics; refuse or report every unsupported feature. | WP06, WP09 | open |
| P1-07 | Completion quality | Densification copies opacity and appearance and can increase optical density; there is no confidence or provenance and insufficient multiscale control. | Conserve transmittance approximately, mark generated splats, validate local geometry. | WP14 | open |
| P1-08 | CLI contract | The CLI is a monolithic manual parser with ambiguous positional conversion, one exit code, no stable diagnostic codes, no overwrite policy, and limited structured output. | Explicit subcommands and a documented machine contract. | WP15 | open |
| P1-09 | Pipeline provenance | Stages discover outputs with broad filesystem searches, can select stale files, and emit no immutable run manifest. | Explicit adapter result manifests and content-addressed stage state. | WP18 | open |
| P1-10 | DA3 export | Direct Gaussian export can drop higher-order SH; point fusion mixes index domains and contains avoidable quadratic behaviour. | Correct the algorithm and add differential tests, or disable the path. | WP18 | open |
| P1-11 | Viewer architecture | Viewer logic is concentrated in one 1,156-line HTML file with no shared core semantics, worker isolation, browser tests, or release-integrated desktop packaging. | Rebuild as typed modules consuming a shared WASM core format layer. | WP19 | open |
| P1-12 | Fuzzing | Tests called "fuzz" are bounded randomized unit loops, not coverage-guided fuzz targets. | Add libFuzzer targets and continuous corpus execution. | WP20 | open |
| P1-13 | Hardware qualification | Hosted CI compiles CUDA but never runs on NVIDIA hardware. No published backend parity or performance evidence exists. | A controlled hardware qualification workflow before any GPU artifact is released. | WP12, WP23 | open |
| P1-14 | Support | Only `main` is supported, so users cannot stay on an immutable release and still receive fixes. | Publish a support window and release branches. | WP24 | **closed** — `SUPPORT.md` and the rewritten `SECURITY.md` both attach support to the immutable `2.0.x` line, never to `main`, and state plainly that no production release is currently supported. |
| P1-15 | Governance | CODEOWNERS is a single-person bottleneck and there is no public release decision process. | Add maintainers and reviewers, issue forms, governance, and release sign-off rules. | WP24 | in-progress — `GOVERNANCE.md`, `MAINTAINERS.md`, `CODE_OF_CONDUCT.md`, five issue forms, and a ten-lens PR template landed. The bottleneck is **not** resolved: there is still one maintainer, and `GOVERNANCE.md` makes an independent external reviewer a non-waivable release blocker rather than pretending peer review exists. |

## P2 — hardening and governance

| ID | Area | Finding | Required disposition | WP | Status |
|---|---|---|---|---|---|
| P2-01 | Dependency maintenance | Dependabot does not cover Python build/runtime dependencies, CMake-fetched sources, weight manifests, or vendored source freshness. | Ecosystem-native update automation plus scheduled freshness issues; every update stays review-gated. | WP02, WP23 | open |
| P2-02 | Compatibility policy | No documented policy distinguishes the C ABI, C++ source compatibility, CLI compatibility, JSON schema compatibility, and adapter protocol compatibility. | Publish separate compatibility and deprecation rules with automated baselines. | WP15, WP24 | open |
| P2-03 | Compatibility evidence | No public corpus identifies which PLY profiles, SPZ versions, glTF revision, viewers, and trainers are tested. | Publish a licensed, manifest-driven conformance corpus and a generated support matrix. | WP20, WP25 | open |
| P2-04 | Claims | No benchmark policy prevents unsourced speed or "SOTA" language from reappearing. | Gate quantitative and superlative claims on reproducible manifests; add prose-claim lint. | WP21 | in-progress — `tools/check_claims.py` lints the production-landing surfaces (README, ROADMAP, SUPPORT, SECURITY, reference/format/security docs) for unqualified superlatives, allowing an attributed or benchmark-linked form; runs in CI. **Remaining:** the benchmark manifest system (WP21) and bringing the pipeline wrapper docs under the lint when they are rewritten (P0-14/WP18). |
| P2-05 | Supportability | No packaged troubleshooting bundle captures build flags, backend capabilities, format metadata, and redacted diagnostics. | Implement `melkor doctor` and a consent-based redacted support bundle. | WP13, WP15 | open |
| P2-06 | Deprecation | No user-facing schedule explains how old positional CLI syntax and retired flags are removed. | Publish the v1→v2 migration and per-surface deprecation policy before the final release. | WP15, WP25 | open |
| P2-07 | Release trust | Release-candidate evidence is unsigned provenance-shaped JSON, not a signed artifact-attestation flow. | Per-artifact SBOMs, signatures, attestations, checksums, and clean-room verification. | WP22, WP26 | open |

## Rules

- No P0 or P1 item may be waived.
- A P2 deferral requires an owner, a target version, a public limitation statement, and approval.
- "Not applicable" requires a written reason, and two-person review when it affects a public
  claim.
- A stale result from a different commit does not qualify the release. Exact-SHA evidence items
  must name the final release commit.
