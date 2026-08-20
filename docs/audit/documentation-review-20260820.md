# Documentation review: 2026-08-20

## Outcome

This review started from commit `e9d257bf25efff9495dcd7ed81a53ca469020148`.
It examined every project-owned Markdown file and each source surface needed to verify a documentation claim.

The review corrected active documentation, repository ownership rules, issue-form examples, and two current audit dates.
It added a documentation index and a complete native CLI reference.

The review did not rewrite legal text, generated notices, vendored documentation, or dated historical findings.
Those files retain their original meaning and evidence value.

Melkor remains a development project with open production gates.
Documentation quality does not change that release status.

## First-principles contract

Each active document must answer four questions without inference:

1. What does the current source implement?
2. What must a user supply or decide?
3. What can fail, lose data, or exceed a limit?
4. What evidence supports the stated release status?

Claims were compared with code, schemas, profiles, build configuration, tests, scripts, and current policy files.
An older document did not override a current executable contract.

## Fifty review lenses

| # | Lens | Result |
|---:|---|---|
| 1 | Product identity | Corrected the public documentation to describe a bounded inspection, conversion, and local-viewing toolkit. |
| 2 | Intended audience | Separated first-time users, integrators, viewer users, contributors, and release maintainers. |
| 3 | First-use path | Matched the documented sequence with setup scripts, build presets, and CI. |
| 4 | Scope boundary | Removed native GPU, mesh, scene-completion, and learned-model implications from active documentation. |
| 5 | Support status | Preserved the explicit statement that no production release is supported. |
| 6 | Version consistency | Verified `2.0.0-dev` against the authoritative version surfaces. |
| 7 | Release state | Kept release claims subordinate to the production blocker register and exact-commit evidence. |
| 8 | Format containers | Verified native reads for PLY, SPZ, glTF, and GLB. |
| 9 | Profile identifiers | Documented all five exact profile identifiers and their intended containers. |
| 10 | Read and write asymmetry | Documented that glTF is read-only and GLB is readable and writable. |
| 11 | CLI grammar | Added the complete `inspect` and `convert` command grammar. |
| 12 | Option coverage | Matched the CLI reference with every option in both command implementations. |
| 13 | Option applicability | Documented format-specific option rejection and source-semantic requirements. |
| 14 | Path handling | Documented suffix inference, explicit format overrides, and `--` for hyphen-leading paths. |
| 15 | Exit behavior | Linked the stable inspection and diagnostic contracts instead of inventing new exit meanings. |
| 16 | Machine output | Distinguished JSON inspection output from the conversion loss report. |
| 17 | Loss policy | Documented exact severe-loss approval and the successful-report boundary. |
| 18 | Approval safety | Preserved the rule that users must review each reported loss before approval. |
| 19 | Resource profiles | Documented web, desktop, server, and API-defined custom limits. |
| 20 | Cancellation | Verified cancellation, deadline, and progress support in the operation contract. |
| 21 | Output installation | Kept the same-directory atomic output and no-overwrite guarantees within their implemented limits. |
| 22 | C ABI | Corrected the project tree to distinguish private C++ headers from the public C ABI header. |
| 23 | Installation | Matched the installation guide with CMake rules and installed-ABI tests. |
| 24 | Build prerequisites | Verified CMake 3.24, Ninja, C++17, zlib, Python, and pinned Python test dependencies. |
| 25 | Platform claims | Kept platform statements tied to the configured CI matrix. |
| 26 | Test claims | Removed unsupported fixture wording and avoided fixed test-count guarantees. |
| 27 | Viewer scope | Kept viewer-only SPLAT, KSPLAT, and SOG/ZIP support outside the native format contract. |
| 28 | Viewer privacy | Verified that local file bytes stay in the browser or webview. |
| 29 | Viewer limits | Preserved file, splat, header, archive, and decoded-image limits. |
| 30 | Viewer accessibility | Preserved the documented keyboard, focus, reduced-motion, and status behavior. |
| 31 | External adapter boundary | Kept external programs, models, and licenses outside the MIT native core. |
| 32 | Adapter reproducibility | Corrected documents that implied nonexistent manifest runners or production adapters. |
| 33 | Upstream migration | Corrected the COLMAP guide and added the current view-graph calibration precondition. |
| 34 | Security reporting | Aligned private-report instructions and preserved the repository-setting verification gate. |
| 35 | Threat model | Kept trust boundaries, controls, and known gaps connected to current release gates. |
| 36 | Supply chain | Verified pinned dependency snapshots, digest checks, and notice generation. |
| 37 | Dependency licensing | Preserved third-party terms and did not rewrite generated or legal notices. |
| 38 | Asset provenance | Kept downloaded viewer fixtures subject to pinned digest verification. |
| 39 | Benchmark evidence | Replaced an aspirational benchmark layout with an honest current-state contract. |
| 40 | Fuzz evidence | Preserved reviewed corpus provenance and avoided unsupported coverage claims. |
| 41 | Release evidence | Verified source-bundle, metadata, evidence, checksum, SBOM, and signing boundaries. |
| 42 | Support policy | Kept development, release, and end-of-life statements aligned. |
| 43 | Roadmap alignment | Verified that active documentation does not present planned work as implemented work. |
| 44 | Governance accuracy | Removed a nonexistent public command and corrected the adapter decision boundary. |
| 45 | Maintainer continuity | Preserved the single-maintainer risk and aligned security-report follow-up guidance. |
| 46 | Ownership rules | Replaced a nonexistent source path and added current format, profile, viewer, and template paths. |
| 47 | Contribution experience | Updated issue-form version examples and preserved reproducible-report requirements. |
| 48 | Documentation navigation | Added one index for users, integrators, security reviewers, and release maintainers. |
| 49 | Automated documentation checks | Passed local link, style, claim, whitespace, version, notice, and source-boundary checks. |
| 50 | Historical integrity | Left dated audit statements intact and labeled current records as authoritative. |

## Material corrections

- Added [`docs/README.md`](../README.md) as the documentation entry point.
- Added [`docs/CLI.md`](../CLI.md) as the native command reference.
- Corrected the benchmark guide to describe only implemented evidence surfaces.
- Corrected governance text that named a removed public command.
- Corrected migration text that described future adapter infrastructure as present.
- Corrected viewer fixture and data-flow descriptions.
- Corrected ownership paths and issue-form version examples.

## Verification evidence

| Check | Result |
|---|---|
| Markdown links and anchors | Passed for 50 project-owned Markdown files. |
| Active Markdown style | Passed for 34 first-party Markdown files. |
| Public claim lint | Passed for 65 public text surfaces. |
| Version synchronization | Passed for `2.0.0-dev`. |
| Dependency setup verification | Passed without downloading live dependencies. |
| Third-party snapshot verification | Passed for all three locked dependencies. |
| Generated notice verification | Passed. |
| Deterministic source-bundle check | Passed. |
| Release metadata | Passed. |
| Release evidence tests | Passed all 36 tests. |
| DA3 synthetic Gaussian conversion tests | Passed. |
| Shell syntax | Passed for all tracked shell scripts. |
| JSON syntax | Passed for all tracked JSON files. |
| Python repository tests | Passed 102 tests and skipped one. Three schema test classes need the unavailable `jsonschema` dependency. |
| Format-profile schema validation | Not run locally because this host does not provide `jsonschema==4.26.0`. |
| Native configure, build, and CTest | Not run locally because this host does not provide CMake. |
| YAML syntax | Passed for all 10 tracked and proposed YAML files. |
| Working-tree whitespace | Passed `git diff --check`. |

The final GitHub Actions result remains the exact-commit evidence for hosted platforms and native builds.

## Remaining limits

- The current production gates in [`production-blockers.md`](production-blockers.md) remain open.
- This host could not run the native build because CMake is unavailable.
- The local Python environment does not contain `jsonschema==4.26.0`.
- Repository-administration settings need separate authenticated verification.
- At review time, the public GitHub About description named removed product surfaces.

Use this replacement during the authenticated repository-metadata update:

> Bounded C++17 toolkit for 3D Gaussian-splat inspection and conversion across PLY, SPZ,
> glTF, and GLB, with a separate local viewer. Explicit profiles, resource limits, loss reports,
> atomic output, and a stable C ABI. v2 hardening; no supported production release.

These limits prevent this review from claiming production readiness or complete release qualification.
