# Melkor v2.0.0 — Engineering Handoff

**As of:** 2026-07-15 · **HEAD:** `f397142` on `main` · **CI:** green (confirm the latest run before you start: `gh run list --branch main --limit 1`).

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
ctest --test-dir build --output-on-failure      # ~39 test targets, all must pass

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

# ASan/UBSan a specific module (pattern used throughout):
clang++ -std=c++17 -g -O1 -fsanitize=address,undefined -Iinclude -isystem third_party/tinygltf \
  tests/test_X.cpp src/.../X.cpp ... -o /tmp/x_asan && /tmp/x_asan

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
- **Scene model** (`src/core/scene.cpp`): `SplatData` is the validated canonical representation —
  **linear** scale (strictly positive), **linear** opacity `[0,1]`, unit quaternion `xyzw`, SH as
  splat-major blocks `data[s*(coeffs*3)+k*3+c]`. Built only through `SplatData::create` (validates
  every domain). **This is the target model; the legacy `GaussianCloud` (`gaussian_data.hpp`) uses
  the 3DGS training domains (LOG scale, LOGIT opacity, WXYZ rotation, `f_dc` SH) and is being
  migrated away from — see to-do #1.**
- **Format layer** (`src/formats/`): loss report + policy (`loss.cpp`, severities info/warning/
  severe/fatal; severe blocks unless the exact code is `--allow-loss`-approved), container probe,
  format profiles (`profiles/*.json`), and the **complete glTF `KHR_gaussian_splatting` codec**
  (`glb_container`, `gltf_accessor/document/resolve/khr/transform/extensions/reader/writer`).
  Legacy readers still live in `src/{ply_writer,spz_encoder,glb_reader}.cpp`.
- **CLI** (`src/main.cpp` + `inspect_command.cpp` + `convert_command.cpp`; command headers live in
  `src/`, included bare). `melkor inspect <file>` and `melkor convert IN.glb OUT.glb` both work.

## 4. What this session delivered (do not redo)

- **Complete glTF `KHR_gaussian_splatting` codec** (reader + writer, round-trip verified), pinned to
  the vendored Khronos RC spec (`third_party/specs/KHR_gaussian_splatting/`). Wired into
  `melkor inspect` (reads KHR GLBs as real Gaussian data) and `melkor convert` (GLB→GLB).
- **Real SH rotation (P0-17)** for degrees 0–3 + `rotation_from_linear` polar extraction.
- **Resource budgets (P0-12)** enforced in all three readers (glTF full; PLY full; SPZ input-size).
- **Two adversarial reviews** (glTF: 19 findings fixed; shipping surface: 18 findings, 13 fixed) —
  the whole codebase has now been reviewed across ~20 lenses.
- **CI repaired** (was red on 4 jobs); the fuzz corpus is tracked and fail-closed.

---

## 5. TO-DO — prioritized, with context

> Each item: **why it matters · files · approach · tests · risk · effort · gated?**
> Deep detail for each is in `docs/audit/remaining-work-roadmap.md`; this is the ordered summary.

### A. Implementable now (no external resources) — do these first

**A1 — P0-06: migrate the legacy `GaussianCloud` onto `SplatData` (effort XL, impact HIGH).**
- *Why:* two parallel models is the largest remaining source of the domain-conversion bugs the
  reviews keep finding (e.g. the inspect bridge that put linear scale into log fields). The blueprint
  wants one validated model.
- *Files:* `grep -rl GaussianCloud src include` to map every site; readers (`ply_writer.cpp`,
  `spz_encoder.cpp`, `glb_reader.cpp`), `cloud_inspector.cpp`, `main.cpp`, `include/melkor/scene.hpp`.
- *Approach:* start model-side (add an `EditTransaction` on `SplatData` that re-validates on commit —
  blueprint §11.3 — and scene provenance/metadata), then migrate readers to produce `SplatData`
  directly (applying the correct linear↔log/logit conversions **once**, via `math/activation.hpp`),
  keeping `GaussianCloud` as a thin legacy shim until the CLI/inspect are moved over.
- *Tests:* per-reader domain round-trips; that the migrated inspect stats match canonical values.
- *Risk:* domain-conversion direction (sigmoid/exp) — a sign error silently corrupts everything.
  Do it through the oracle, and cross-check with a known PLY↔canonical fixture.

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
  report and honouring `--allow-loss`/`--limits-profile`. **Blocked on A1** (needs readers producing
  `SplatData` with correct domains). Also wire the existing `gltf::write_glb` behind it.
- *Risk:* the same domain-conversion risk as A1; that is why GLB→GLB was shipped first.

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

- **PR-level acceptance:** 0 of the blueprint's 35 PRs are acceptance-complete. The blocker register
  tracks *finding-level* closures, which is a different, narrower thing.
- **glTF (P0-10):** codec + CLI + budgets + review done, but **not** acceptance-complete — no
  Validator-verified conformance corpus yet, cross-format convert not done, SH rotation is degrees
  0–3 only (degree 4 for SPZ), and a rotation+reflection/singular node still reports an approvable
  `LOSS_SH_ROTATION_NOT_APPLIED`.
- **SPZ:** decode allocation is bounded only by upstream's 10M-point cap, not melkor's profile (A7).
- **Convert:** GLB→GLB only.

## 7. Fast repo map

- Blockers/roadmap/reviews: `docs/audit/`
- Formats: `src/formats/` + `include/melkor/format/`
- Math: `src/math/` + `include/melkor/math/`
- Safety substrate + scene: `src/core/` + `include/melkor/{error,checked,limits,budget,scene}.hpp`
- CLI: `src/{main,inspect_command,convert_command}.cpp`
- Legacy readers/writers: `src/{ply_writer,spz_encoder,glb_reader,cloud_inspector,enhanced_converter}.cpp`
- Tests: `tests/` (self-contained C++ with a tiny `CHECK` macro; some Python CLI/tool tests)
- Fuzz: `fuzz/` (dual-build: libFuzzer + standalone replay; corpus under `fuzz/corpus/`)
- Third-party (pinned): `third_party/` + `manifest.lock.json` + `specifications.lock.json` + `patches/`
- Tooling (CI-enforced): `tools/*.py`
