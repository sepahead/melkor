# Repository ten-lens review: 2026-07-25

## Outcome

This review included all 328 tracked paths in the starting inventory.
The inventory contained 50 Markdown files and 278 other tracked files.
The proposed tree contains 331 paths, including 51 Markdown files.

The review corrected code, scripts, configuration, documentation, tests, and two SVG files.
It did not change vendored source, legal text, lockfile records, direct quotations, or historical records.

The repository passes the local verification matrix in this document.
It is not production-ready because the blocker register still contains open P0 and P1 items.

## File coverage

Use `git ls-files` to reproduce the exact path inventory.
The review applied these checks by file class:

| File class | Coverage |
|---|---|
| Markdown | Checked all 50 files for local links, anchors, and path case. Checked 33 active first-party files for style. |
| C, C++, Objective-C++, CUDA, and Metal | Built all locally available targets. Ran normal, Metal, and sanitizer test suites. |
| Shell | Parsed all 19 tracked shell files. Ran ShellCheck at warning severity. Tested active and retired wrapper contracts. |
| Python | Ran Ruff, bytecode compilation, focused tests, and CTest integration. |
| JavaScript and HTML | Parsed each JavaScript file. Ran the full Chromium viewer suite. |
| Rust and TOML | Ran rustfmt, Clippy with warnings denied, and all-target checks. |
| JSON and YAML | Parsed all tracked files. Validated each format profile. |
| Binary fixtures and icons | Checked the tracked size inventory. Exercised format fixtures through CTest and viewer tests. |
| Vendored and generated files | Verified the dependency lock, patch digests, notices, version surfaces, and source bundle. |

The inventory has no case-colliding paths, tracked symlinks, or tracked files larger than 10 MiB.
All project-owned text files end with a newline.
The final diff has no whitespace errors or merge markers.

## Ten review lenses

| Lens | Review result |
|---|---|
| 1. Correctness and data integrity | Fixed false `.gltf` conversion acceptance. Added positive and negative CLI tests. |
| 2. Security and adversarial input | Replaced unsafe wrapper interpolation with argument arrays. Added strict option and input checks. |
| 3. Performance and resources | Removed unsupported performance claims. Preserved the open SPZ allocation and cancellation limits in the threat model. |
| 4. Portability and distribution | Passed CPU and Metal builds. Passed the install, C, C++, and relocated SDK tests. |
| 5. API, CLI, UX, and accessibility | Corrected CLI descriptions. Retired false options with actionable errors. Preserved viewer accessibility tests. |
| 6. Interoperability and ecosystem | Aligned the global mapper wrapper with COLMAP. Kept trainer binaries external and explicit. |
| 7. Scientific validity and claims | Expanded claim checks to 59 public text surfaces. Removed unsupported quality and speed statements. |
| 8. Reproducibility and supply chain | Required explicit acceptance for unlocked Python dependencies. Verified existing checkout origins and commit SHAs. |
| 9. Licensing, privacy, and governance | Retired mutable general installers. Preserved external license boundaries and regenerated notices from the lock. |
| 10. Maintainability and operations | Added link and style gates. Simplified wrapper contracts. Added failure-path and output-preservation tests. |

## Main implementation changes

- `melkor convert` now accepts only GLB input and output paths.
- The COLMAP wrapper now runs `global_mapper` and validates its sparse model.
- The pipeline now uses explicit paths and refuses stale output directories.
- The OpenSplat wrapper stages output before it replaces the destination.
- The LichtFeld wrapper stages its output directory and preserves old output after failure.
- General mutable trainer installers now fail closed.
- Streaming setup now provides a read-only research catalog.
- DA3 and feedforward setup require explicit unlocked-dependency acceptance.
- Existing DA3 and feedforward checkouts must match the expected origin and commit.
- The 4D packer validates arguments and writes its manifest atomically.
- CI now checks documentation links, documentation style, public claims, and ShellCheck warnings.
- Active guides now describe tested development paths without production support claims.

## Verification evidence

| Check | Result |
|---|---|
| Clean CPU developer build | 111 targets built |
| CPU CTest | 41 of 41 passed |
| ASan and UBSan build | 115 targets built |
| ASan and UBSan CTest | 41 of 41 passed |
| Metal build | 120 targets built |
| Metal CTest | 41 of 41 passed |
| SDK install and relocation | C and C++ consumers passed before and after relocation |
| Viewer | 23 Chromium tests passed |
| Rust desktop shell | rustfmt, Clippy, and all-target checks passed |
| Tauri developer bundle | macOS application and DMG builds passed |
| Wrapper and tool tests | 28 of 28 passed |
| Markdown links | 51 of 51 final files passed |
| Active Markdown style | 33 of 33 files passed |
| Public claim checks | 59 of 59 surfaces passed |
| Format profiles | 5 of 5 passed |
| Third-party lock | 3 of 3 dependencies matched |
| Version synchronization | All version surfaces matched `2.0.0-dev` |
| Proposed source bundle | 297 files included and 34 intentionally excluded |

The local host could not run CUDA or Windows qualification.
The review did not create external producer interoperability evidence or signed release artifacts.

## Remaining blockers

The blocker register still reports 9 closed, 13 in-progress, and 18 open findings.
A clean local matrix does not close those findings.

The largest remaining items include:

- SPZ v4 support and decoded-allocation control
- Licensed glTF conformance evidence and external interoperability tests
- A complete format planner and an honest mesh initialization command
- Hash-locked adapter environments and immutable result manifests
- Windows, CUDA hardware, and release artifact qualification
- Viewer worker isolation and a shared core format layer
- Signed artifacts, attestations, and independent release review

See [the blocker register](production-blockers.md) for each required disposition.
