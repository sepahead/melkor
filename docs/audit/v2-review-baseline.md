# v2 production-hardening baseline

This document freezes the exact repository state from which the `v2.0.0` production-hardening
program starts. It exists so that every later claim ("this was fixed", "this was removed",
"this regressed") can be checked against a known starting point rather than against memory.

Do not edit the recorded facts in this file. If a fact turns out to be wrong, add a correction
section at the end with the date and the evidence.

## Starting point

| Item | Value |
|---|---|
| Baseline commit | `21c8fb53f58e19a78d92a4b01ce479374a7b8633` |
| Branch | `main` |
| `git describe` | `v2.0.0-rc.1-7-g21c8fb5` |
| Working tree | clean |
| Baseline date | 2026-07-14 |
| Host used for the local baseline | macOS (Darwin 25.5.0), arm64, AppleClang |

## Tags at baseline

These tags must survive the entire program unchanged. Any later verification step compares
against the object IDs below.

| Tag | Tag object | Commit |
|---|---|---|
| `v1.2.0` | `efaf633ea70c1d27204449f2c6adafc5681b6a8c` | `dba645770ec538d886840e583e7cc02174da417e` |
| `v1.2.1` | `f6b611f5188a6c6c2986005e14be722e177205a9` | `2ffd39659a27ea8e85843d555e01178cb9a71dcf` |
| `v2.0.0-rc.1` | `43050cb90e5b477bece730e90ce091afc39df52d` | `0c41b503dc4ea8b153af82b4470e441ac164cea1` |

All three are annotated tags.

## GitHub releases at baseline

| Release | Tag | State |
|---|---|---|
| Melkor 1.2.1 | `v1.2.1` | marked **Latest** |
| Melkor 1.2.0 | `v1.2.0` | published |

`v2.0.0-rc.1` has a tag but no GitHub release. This is the concrete shape of finding **P0-01**:
the public Releases page advertises a `1.x` release as the current one while the source tree
identifies itself as `2.0.0-rc.1` and the support statement points users at `main`.

Resolution path: publish `v2.0.0` when its gates pass. It then becomes the latest release, and
the `1.x` releases become correctly-labelled history. No tag or asset is rewritten to achieve
this.

## GitHub "About" metadata at baseline

```text
description: Gaussian splatting pipelines & depth analysis for 3D reconstruction (Python/C++).
homepage:    (empty)
topics:      3d-reconstruction, computer-vision, depth-estimation,
             gaussian-splatting, point-cloud, python
issues:      enabled
discussions: disabled
visibility:  public
```

Two claims here are not currently backed by an artifact:

- the description says **Python/C++**, but the root `pyproject.toml` is a Ruff configuration
  with no `[project]` or `[build-system]` table and nothing is pip-installable (**P0-02**);
- the `python` topic asserts the same thing.

Both are corrected in WP01 and may only be restored once a `melkor3d` wheel is published.

## Release automation at baseline

`.github/workflows/release-candidate.yml` triggers on `v*-rc.*` tag pushes, but it only builds
and uploads a deterministic source-evidence bundle as a workflow artifact. It declares
`permissions: contents: read` and it does **not** create or publish a GitHub release.

Therefore WP00 step 11 ("no workflow may publish a release merely because a tag exists") is
already satisfied at baseline. No change is required; this is recorded so the later release-gate
audit does not have to rediscover it.

## Continuous integration at baseline

`.github/workflows/ci.yml` runs: `secrets-scan`, `dependency-review`, `build-macos`,
`sanitize-macos` (ASan+UBSan), `build-linux`, `build-linux-cuda` (compile only), `lint-python`,
`viewer-tests` (Playwright/Chromium), `tauri-check`.

Recent `main` pushes are green. The two most recent failing runs are Dependabot pull requests
(`github-actions` group bump, `tauri` 2.11.3 → 2.11.5), not `main` failures.

Note for the record: `build-linux-cuda` is a **compile-only** job. It is not hardware
qualification, and it does not entitle the project to claim runtime CUDA support (**P1-13**).

## Local baseline build

Command, per the blueprint's §6.1:

```bash
cmake -S . -B build/baseline -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMELKOR_WERROR=ON
cmake --build build/baseline --parallel
ctest --test-dir build/baseline --output-on-failure
```

Result: see `docs/audit/baseline-20260714.md` for the captured output and any failures.

`python3 -m ruff check .` could not run on the baseline host because Ruff is not installed in
the system interpreter. This is a host gap, not a repository failure; CI's `lint-python` job
runs Ruff and is green on `main`.

## Structural facts that the program depends on

Recorded now so that later work packages can be verified as having actually changed something.

**Build graph.** `melkor_core` links each backend `PUBLIC`, and each backend links `melkor_core`
back `PUBLIC` (`CMakeLists.txt`, the `melkor_metal` / `melkor_cuda` / `melkor_gpu_stub` blocks).
The cycle is resolved by static-archive rescanning and by
`add_link_options("LINKER:-no_warn_duplicate_libraries")` on Apple. This is **P0-05**.

**SDK.** `install()` covers the `melkor` executable, `default.metallib`, and the license files.
It installs no headers, no libraries, no CMake package config, and no export set. There is no
`MelkorConfig.cmake`. Downstream C++ consumption is therefore not possible. This is **P0-04**.

**Version.** There is no root `VERSION` file. The version is stated in `CMakeLists.txt`
(`project(... VERSION 2.0.0)` plus `MELKOR_PRERELEASE "rc.1"`) and duplicated by hand in
`viewer/package.json`. This is the mechanism behind **P0-01**.

**Dependency provenance.** `third_party/` vendors `spz/`, `stb/stb_image.h`, and `tinygltf/`
in-tree. There are no submodules and no `third_party/manifest.lock.json`. Provenance exists only
as prose in `THIRD_PARTY_LICENSES.md` and `release/components.json`. This is part of **P0-13**
and **P2-01**.

**Restricted source in the MIT tree.** `tools/OpenSplat/` (AGPL-3.0-only), `DA3coreml/`
(Apache-2.0 research snapshot), `ml-sharp/` (Apple sample-code licence), and `.superstack/`
(agent handoff artifacts) are all tracked inside the MIT core. This is **P0-16**.

**Tests.** There is no test framework. C++ tests are hand-rolled `main()` functions with manual
assertions, driven by CTest. The two files named `*_fuzz.cpp` are bounded randomized loops, not
coverage-guided fuzzers. This is **P1-12**.

**Viewer.** `viewer/index.html` is a single 1,156-line file containing markup, CSS, and an inline
module script. There is no `viewer/src/`. This is **P1-11**.

**Test data.** `test_data/` is untracked but is referenced by tests and documentation, so the
suite is not reproducible from a clean clone.

## What this baseline does not establish

This is a static record plus one local build. It does **not** establish that the project builds
and runs correctly on Linux, on Windows, on NVIDIA hardware, in a clean container, or from a
released artifact. Every such claim must be earned by evidence produced during the program, not
inferred from this document.

## Correction — 2026-07-15: v1.2.x tags and releases deleted by maintainer decision

The tags and releases recorded above for `v1.2.0` and `v1.2.1` were **deleted** on 2026-07-15,
at the maintainer's explicit, informed instruction. This deliberately overrides the blueprint's
non-negotiable rule #1 ("Preserve every existing tag exactly as it is") and the release-gate
item "All existing tags remain unchanged".

The trade-off was stated plainly before the decision: deleting these tags is irreversible,
breaks provenance for anything that referenced them, and would invalidate a DOI minted against
them. The maintainer chose a clean release history (an empty Releases page ahead of `v2.0.0`)
over that preserved provenance, and accepted the trade-off.

The underlying commits remain in git history; only the `v1.2.0` and `v1.2.1` tag refs and their
GitHub releases were removed. The archival tag `archive/pre-v2-research-bundle-20260714` and the
`v2.0.0-rc.1` tag are unchanged. This correction is the honest record of a rule being overridden
by its owner, not a claim that the rule still holds.
