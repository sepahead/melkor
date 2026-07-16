# Melkor v2.0.0 — Engineering Handoff

**As of:** 2026-07-16 · **A1 implementation through:** `32cd43f` on `main` · **CI:** exact-SHA
green in run `29452688699` (`CI Gate` job `87481460909`). Confirm the exact current HEAD before
starting; never substitute the latest branch run for an exact-commit result.

This is the single entry point for the next engineer/agent. Read this, then the three linked
planning docs, then start on the first to-do. Everything below is grounded in the actual code and
the blueprint (`/Users/torusprime/Downloads/melkor_production_v2_blueprint.md`).

Author of the project is **Sepehr Mahmoudian**. The end goal is a `v2.0.0` release that can honestly
be called production-grade; the maintainer will mint the tag and a Zenodo DOI at the end.

---

## 0. Read these first (in order)

1. `docs/audit/production-blockers.md` — the authoritative per-blocker register (P0/P1/P2, status + evidence). **Check this first every session.**
2. `docs/audit/remaining-work-roadmap.md` — a scoped, de-risked plan for every remaining area (files, approach, tests, risks, effort, resource-gated?). Produced by a 10-agent scoping pass. **This is your detailed backlog.**
3. `docs/audit/shipping-surface-review-20260715.md` — the 18-finding adversarial review outcome (13 fixed, 5 deferred with reasons). The 5 deferred items are real, small, and listed in the to-do below.
4. `docs/audit/v2-progress.md` and `docs/audit/takeover-20260715.md` — narrative status; the strict PR-level audit (0 of 35 PRs acceptance-complete, 21 partial, 14 open at commit `090126e`).

---

## 1. Non-negotiable working rules (learned the hard way)

- **Confirm CI is green after every push.** Do not stack commits on an unconfirmed push. A previous
  stretch pushed ~15 commits without watching Actions and left `main` red on 4 jobs for a full day.
  After `git push`, poll `gh run view <id>` until the run completes and every job is `success`.
  The stable gate is the **`CI Gate`** job.
- **Every commit builds `-Werror`-clean, passes the full `ctest` suite, and is ASan/UBSan-clean**
  where it touches untrusted-input parsing or math. Add a test for every fix and every feature.
- **No AI co-authors.** No `Co-Authored-By`, no "Generated with…", no 🤖. Commit messages are
  professional, correct, and complete; nothing about the author's PhD or supervisor. (Global rule.)
- **Honesty over completeness.** If something is not done, say so in the blocker register and the
  changelog. Never fake evidence (benchmarks, conformance, signatures). Resource-gated work is
  documented with exact maintainer commands, not stubbed.
- **Review substantive work across ~10 lenses before finishing** (standing instruction). The two
  adversarial-review Workflows this codebase already ran are the template; re-run one over any large
  new subsystem you build.
- **Two configs must build:** the CPU path and at least one accelerator path (Metal on macOS, CUDA
  on Linux). CI covers both; you can only run Metal/CPU locally on this Mac.

## 2. Build & test (copy-paste)

```bash
# Configure + build (strict, CPU-only, matches the CI 'build' shape)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMELKOR_USE_METAL=OFF -DMELKOR_USE_CUDA=OFF -DMELKOR_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure      # 41 test targets as of A1, all must pass

# The CI-only checks you cannot skip (run before pushing):
ruff check .
python3 tools/verify_third_party.py --check
python3 tools/generate_notices.py --check
python3 tools/check_version_sync.py --check
python3 tools/check_claims.py
python3 tools/check_profiles.py
python3 tests/test_tools.py
git ls-files -z -- '*.sh' | xargs -0 -n1 bash -n     # shell syntax
git diff --check                                      # whitespace

# Full ASan/UBSan matrix (the per-commit A1 gate; leak detection is disabled because Apple
# system/framework allocations make LeakSanitizer non-actionable here):
cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMELKOR_USE_METAL=OFF -DMELKOR_USE_CUDA=OFF -DMELKOR_WERROR=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-sanitize --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
  ctest --test-dir build-sanitize --output-on-failure

# libFuzzer needs a fuzzer-capable Clang (not Apple clang on this Mac); CI runs it on Linux.
```

CI jobs (in `.github/workflows/ci.yml`): `build-macos`, `build-linux`, `build-linux-cuda`,
`sanitize-macos`, `Fuzz smoke (libFuzzer)`, `Tauri / Rust 1.88`, `Viewer / Playwright`,
`Python and shell checks`, `Full-history secret scan`, and the aggregate **`CI Gate`**.

## 3. Architecture in one screen

- **Layering (acyclic):** `melkor_core` (formats, math, safety substrate, scene model — knows no
  backend) ← `melkor_runtime` (registers Metal/CUDA backends) ← `melkor` CLI. The C ABI
  (`include/melkor/c/melkor.h`, `src/c_api/melkor_c.cpp`) is the stable consumption boundary.
- **Safety substrate** (`src/core/`): `Result<T>` + stable `MKxxxx` diagnostics + exit-code map
  (`error.cpp`); `checked_*` arithmetic (`checked.cpp`); `Limits` profiles + `Budget`
  (`limits.cpp`, `budget.cpp`). **A `0` limit means UNLIMITED in `Budget::consume` — never leave a
  budget-backed limit at 0.**
- **Math oracle** (`src/math/`): `activation` (logit/log domains), `quaternion` (xyzw, `to_matrix`
  assumes near-unit), `covariance` (Σ=R diag(s²)Rᵀ, affine transform, Jacobi eigensolver,
  `rotation_from_linear` polar decomposition), `sh_rotation` (real-SH rotation degrees 0–3),
  `color`, `coordinate_frame`. This is the single semantic authority — never re-derive transforms.
- **Scene model** (`src/core/scene.cpp`): `SplatData` is the validated canonical public and format
  representation —
  **linear** scale (strictly positive), **linear** opacity `[0,1]`, unit quaternion `xyzw`, SH as
  splat-major blocks `data[s*(coeffs*3)+k*3+c]`. Built only through `SplatData::create` (validates
  every domain). `EditTransaction` is the only bulk-mutation workspace and revalidates atomically.
  The historical training-domain model (LOG scale, LOGIT opacity, WXYZ rotation) is excluded from
  the installed SDK and remains only inside explicitly deferred backend/densifier/mesh-init code.
- **Format layer** (`src/formats/`): loss report + policy (`loss.cpp`, severities info/warning/
  severe/fatal; severe blocks unless the exact code is `--allow-loss`-approved), container probe,
  format profiles (`profiles/*.json`), and the **complete glTF `KHR_gaussian_splatting` codec**
  (`glb_container`, `gltf_accessor/document/resolve/khr/transform/extensions/reader/writer`).
  The PLY, SPZ, and legacy mesh-GLB adapters live in
  `src/{ply_writer,spz_encoder,glb_reader}.cpp`; despite their source location, all now expose and
  exchange canonical `SplatData`.
- **CLI** (`src/main.cpp` + `inspect_command.cpp` + `convert_command.cpp`; command headers live in
  `src/`, included bare). `melkor inspect <file>` and `melkor convert IN.glb OUT.glb` both work.
  The older positional command supports mesh GLB/glTF→PLY/SPZ and PLY↔SPZ on canonical data, but it
  is not the WP06 registry/planner and intentionally rejects a KHR splat GLB rather than degrading
  it through the mesh sampler.

## 4. What this session delivered (do not redo)

- **Complete glTF `KHR_gaussian_splatting` codec** (reader + writer, round-trip verified), pinned to
  the vendored Khronos RC spec (`third_party/specs/KHR_gaussian_splatting/`). Wired into
  `melkor inspect` (reads KHR GLBs as real Gaussian data) and `melkor convert` (GLB→GLB).
- **Real SH rotation (P0-17)** for degrees 0–3 + `rotation_from_linear` polar extraction.
- **Resource budgets (P0-12)** enforced in all three readers (glTF full; PLY full; SPZ input-size).
- **Two adversarial reviews** (glTF: 19 findings fixed; shipping surface: 18 findings, 13 fixed) —
  the whole codebase has now been reviewed across ~20 lenses.
- **CI repaired** (was red on 4 jobs); the fuzz corpus is tracked and fail-closed.
- **A1 / P0-06 scene migration completed at finding level:** validated edit transactions and
  reproducible provenance (`86ee365`); canonical inspection (`3026dcb`); PLY/SPZ/legacy mesh-GLB
  and positional CLI on `SplatData` with one-shot oracle domain conversions and round-trip tests
  (`025f51b`); curated installed SDK with no legacy mutable model (`32cd43f`). CPU, Metal, and full
  ASan/UBSan matrices each passed 41/41, the clean SDK consumer/relocation test passed, and each
  pushed commit's exact `CI Gate` was confirmed green (`29446027102`, `29447287141`,
  `29451389623`, `29452688699`). Do not recreate bridges back to training-domain storage.

---

## 5. TO-DO — prioritized, with context

> Each item: **why it matters · files · approach · tests · risk · effort · gated?**
> Deep detail for each is in `docs/audit/remaining-work-roadmap.md`; this is the ordered summary.

### A. Implementable now (no external resources) — do these first

**A1 — P0-06 scene migration: COMPLETED at finding level (`86ee365`..`32cd43f`).**
- `SplatData` is now the installed/model/inspection/PLY/SPZ/legacy-mesh-GLB/CLI representation.
- Remaining `GaussianCloud` references are private implementation debt with named owners:
  Enhanced→A2/WP10, compute backends→WP12, densifier→WP14. They are not A1 reopeners unless they
  cross the installed SDK or a canonical format/CLI boundary again.
- The blueprint's broader PR-level acceptance classification was not recomputed by this focused
  finding closure; do not convert it into a release claim.

**A2 — P0-11: honest `mesh-init` (effort XL, impact HIGH).**
- *Why:* the "Enhanced" path is misleading (one splat per vertex dressed up as reconstruction).
- *Files:* delete `src/enhanced_converter.*`, the `--enhanced/--knn/--no-surface-align` flags and the
  magic `* 50` in `main.cpp`; add a `mesh_init` module producing `SplatData`.
- *Approach:* `mesh-init --mode vertices` (per-vertex initialiser, salvaged from Enhanced's
  SpatialHash/normal code) and `--mode surface` (a real **area-weighted triangle sampler**:
  cumulative-area CDF over triangles, barycentric sampling, scale from local triangle size). Blueprint
  §17.
- *Tests:* sampler produces N splats on a unit quad with expected density/bounds; determinism.
- *Risk:* low (self-contained algorithm producing canonical `SplatData` — no domain conversion).

**A3 — WP06 + WP13/WP15: format registry + cross-format `convert`/`normalize` (effort XL, impact HIGH).**
- *Why:* `melkor convert` currently only does GLB→GLB (safe canonical path). Users need PLY↔GLB↔SPZ.
- *Files:* new `src/formats/registry.*` (a `FormatCapabilities` table per `FormatId` + a
  probe→read→plan→write planner); extend `convert_command.cpp`.
- *Approach:* the registry reads any format to `SplatData` and writes any target, emitting the loss
  report and honouring `--allow-loss`/`--limits-profile`. **A1 is complete, so this is now
  unblocked.** Wire the existing native PLY/SPZ readers/writers and `gltf::write_glb` behind one
  capability/loss planner rather than adding another model bridge.
- *Risk:* semantic-profile ambiguity and loss-policy mistakes. Do not reimplement the domain/order
  conversions A1 already pinned inside each adapter.

**A4 — WP20: conformance/property/fuzz/E2E test system (effort L–XL, impact HIGH).**
- *Implementable now:* property-based tests for the math/format invariants; fuzz the PLY/SPZ decoders
  through the new budget-bounded entry points; broaden E2E CLI tests.
- *Gated part:* the licensed glTF conformance corpus validated by the **Khronos glTF Validator**
  (needs the external tool / network) — the one thing keeping P0-10 from full closure.

**A5 — WP18: typed pipeline stage runner (effort XL, impact HIGH).**
- Replace the shell scripts with a typed runner + pinned adapter manifests (source rev, archive
  digest, environment, weights, licence, command contract) and immutable run manifests. The runner
  substrate + schemas + the P1-10 DA3-export SH fix are **not** gated; the COLMAP/trainer adapters
  need those external tools (see roadmap for which digests the maintainer must compute on a networked
  host).

**A6 — WP19: viewer rebuild (effort XL, impact MEDIUM).**
- One 1,156-line HTML file → typed modules + a shared WASM core + worker isolation + browser tests
  (Playwright CI already exists). Investigate whether `melkor_core`'s format layer can compile to
  WASM to be that shared core.

**A7 — the 5 deferred shipping-surface review findings (effort S each, mostly LOW).** From
`docs/audit/shipping-surface-review-20260715.md`:
- `spz_encoder.cpp` — bound the SPZ *decoded* size (header peek for `numPoints`); **do with P0-09**.
- `ply_writer.cpp` ASCII path — parse in place instead of copying into a `std::string` (~2× peak mem).
- `ply_writer.cpp` binary `f64` — apply the same out-of-float-range guard the ASCII path has.
- `scene.cpp` — optionally renormalize a within-tolerance quaternion in `create()` (tolerance policy).
- `atomic_writer.cpp` — close the create→rename no-clobber window with `renameat2(RENAME_NOREPLACE)` /
  `renameatx_np` (platform-specific).

### B. Resource-gated — cannot be truthfully done autonomously (leave to the maintainer)

Exact commands are in `docs/audit/remaining-work-roadmap.md` (the `resource-gated` entries). Summary:

- **P0-09 SPZ v4:** vendor Zstandard (BSD-3-Clause, e.g. pin `zstd v1.5.6`), re-pin upstream SPZ to
  v3.0.0 (writes format v4), update `third_party/manifest.lock.json` + patches. Needs the maintainer
  to approve the zstd vendoring and the official SPZ CLI for two-way interop testing.
- **P0-02 Python wheels:** buildable with scikit-build-core, but qualifying/publishing needs PyPI
  trusted-publishing set up on the repo.
- **P0-03 Windows:** MSVC/clang-cl CI + portability layer are implementable, but the clean-VM
  install/uninstall qualification needs a Windows runner.
- **P1-13 GPU parity/perf:** needs real Apple + NVIDIA hardware. Compile-only CI is not qualification.
- **P0-18 / P2-07 signing:** SBOM + checksums are scriptable; signatures/notarization/attestations
  need code-signing identities and a protected release environment.
- **Final release:** publish `v2.0.0`, mint the tag (the `v1.2.x` tags/releases were already deleted
  per the maintainer's informed override — recorded in `v2-review-baseline.md`); maintainer does the
  Zenodo DOI.

---

## 6. Honesty notes / known limitations (state these; do not hide them)

- **PR-level acceptance:** the latest full strict audit, at `090126e`, found 0 of 35 blueprint PRs
  acceptance-complete. A1 closes P0-06 at the finding level; it does not silently reclassify the
  PR matrix. Re-run the complete acceptance audit before publishing a new PR-level count.
- **glTF (P0-10):** codec + CLI + budgets + review done, but **not** acceptance-complete — no
  Validator-verified conformance corpus yet, cross-format convert not done, SH rotation is degrees
  0–3 only (degree 4 for SPZ), and a rotation+reflection/singular node still reports an approvable
  `LOSS_SH_ROTATION_NOT_APPLIED`.
- **SPZ:** compressed input and final canonical allocation are budgeted, but vendored v1–v3 still
  performs a whole-stream inflate internally before Melkor can enforce the profile's decoded-size
  limit; upstream's 10M-point cap is the residual guard (A7/P0-09).
- **Convert:** the explicit `melkor convert` subcommand remains GLB→GLB only. The legacy positional
  command can do mesh GLB/glTF→PLY/SPZ and PLY↔SPZ, but has no registry/capability planner, no
  cross-format loss policy, and rejects KHR splat GLBs to prevent silent data loss.

## 7. Fast repo map

- Blockers/roadmap/reviews: `docs/audit/`
- Formats: `src/formats/` + `include/melkor/format/`
- Math: `src/math/` + `include/melkor/math/`
- Safety substrate + scene: `src/core/` + `include/melkor/{error,checked,limits,budget,scene}.hpp`
- CLI: `src/{main,inspect_command,convert_command}.cpp`
- Canonical root-level adapters: `src/{ply_writer,spz_encoder,glb_reader,cloud_inspector}.cpp`
- Deferred private algorithm code: `src/{enhanced_converter,densifier,cpu_compute_provider}.cpp`
- Tests: `tests/` (self-contained C++ with a tiny `CHECK` macro; some Python CLI/tool tests)
- Fuzz: `fuzz/` (dual-build: libFuzzer + standalone replay; corpus under `fuzz/corpus/`)
- Third-party (pinned): `third_party/` + `manifest.lock.json` + `specifications.lock.json` + `patches/`
- Tooling (CI-enforced): `tools/*.py`
