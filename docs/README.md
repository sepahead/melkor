# Melkor documentation

Melkor is a bounded toolkit for Gaussian-splat inspection, conversion, and local viewing.
The native core does not train or render Gaussian scenes.

No production release is currently supported.
Read the [support policy](../SUPPORT.md) before you use a development build for important work.

## Start here

| Goal | Document |
|---|---|
| Understand the product and its limits | [Project README](../README.md) |
| Build and test the source | [Quick Start](QUICKSTART.md) |
| Use every native command and option | [CLI reference](CLI.md) |
| Understand inspection reports | [Asset inspection](INSPECT.md) |
| Open a local asset | [Viewer guide](../viewer/README.md) |
| Check current release gates | [Production blockers](audit/production-blockers.md) |

## Native format contract

| Document | Contract |
|---|---|
| [Canonical semantics](reference/canonical-semantics.md) | Canonical values, coordinate frames, units, color, SH, and provenance |
| [Loss policy](reference/loss-policy.md) | Loss severities, stable codes, and exact approval rules |
| [Resource limits](reference/resource-limits.md) | Named limits, accounting, deadlines, and cancellation |
| [Asset inspection](INSPECT.md) | Validation scope, JSON schema, diagnostics, and exit codes |
| [Pinned KHR update procedure](maintainers/updating-khr-gaussian-splatting.md) | Maintainer steps for a specification revision |

The machine-readable profiles are in [`profiles/`](../profiles/).
The report and profile schemas are in [`schemas/`](../schemas/).

## Viewer and temporal assets

| Document | Contents |
|---|---|
| [Viewer guide](../viewer/README.md) | Local opening, controls, desktop builds, and tests |
| [Streaming and temporal playback](STREAMING.md) | Static formats, 4D manifests, buffering, and packaging |
| [Viewer asset provenance](../viewer/ASSET_PROVENANCE.md) | Packaged assets and external development fixtures |

The viewer has a separate format and security boundary.
Viewer support does not extend the native CLI format contract.

## External development adapters

External tools keep their own source, dependencies, model weights, and licenses.
Current wrappers are development tools, not supported production adapters.

| Document | Contents |
|---|---|
| [Adapter boundary](adapters/index.md) | License, trust, and planned manifest rules |
| [Reconstruction pipeline](PIPELINE.md) | Temporary COLMAP and OpenSplat workflow |
| [COLMAP global mapper wrapper](GLOMAP_WRAPPER.md) | Global mapping stages and migration behavior |
| [OpenSplat wrapper](OPENSPLAT_WRAPPER.md) | Narrow external OpenSplat invocation contract |
| [LichtFeld-Studio wrapper](LICHTFELD_WRAPPER.md) | Narrow external LichtFeld-Studio invocation contract |
| [Depth Anything 3 integration](DA3_FEEDFORWARD.md) | Pinned DA3 development bridge and semantic limits |
| [Feedforward reconstruction catalog](FEEDFORWARD_SOTA.md) | Point-in-time external research catalog |
| [gsplat CUDA status](GSPLAT_CUDA.md) | Retired installer and current external-tool boundary |

Treat output from each external program as untrusted input.
Inspect it before native conversion.

## Security, support, and releases

| Document | Contents |
|---|---|
| [Security policy](../SECURITY.md) | Reporting, supported versions, scope, and current gaps |
| [Threat model](security/threat-model.md) | Assets, trust boundaries, controls, and residual risks |
| [Support policy](../SUPPORT.md) | Current status and the planned supported-release policy |
| [Roadmap](../ROADMAP.md) | Product boundary and v2 release outcomes |
| [Release process](RELEASE.md) | Verification, version freeze, publication, and evidence |
| [Release evidence](../release/README.md) | Deterministic source-evidence bundle and verifier |

The current release gates are in
[the production blocker register](audit/production-blockers.md).
Do not infer support from a clean development build.

## Contributing and governance

| Document | Contents |
|---|---|
| [Contributing](../CONTRIBUTING.md) | Setup, architecture rules, checks, and pull request requirements |
| [Governance](../GOVERNANCE.md) | Product authority, decisions, review, and release approval |
| [Maintainers](../MAINTAINERS.md) | Current role holders and ownership limits |
| [Code of Conduct](../CODE_OF_CONDUCT.md) | Community behavior and enforcement |
| [Benchmark policy](../benchmarks/README.md) | Evidence needed for quantitative claims |
| [Fuzz corpus](../fuzz/corpus/README.md) | Seed provenance and corpus rules |

Project-owned technical prose follows the rules in [`AGENTS.md`](../AGENTS.md).
Legal text, generated notices, vendored documentation, and historical records retain their source form.

## Migration guides

| Document | Use |
|---|---|
| [v1 C++ scene API to v2](migrations/v1-to-v2.md) | Migrate internal source integrations to the validated model |
| [Standalone GLOMAP to COLMAP](migrations/2.0-glomap-to-colmap-global.md) | Replace the retired standalone mapper command |

## Audit records

Two audit files describe the current development state:

- [Production blockers](audit/production-blockers.md)
- [v2 hardening progress](audit/v2-progress.md)

The [2026-08-20 documentation review](audit/documentation-review-20260820.md)
records the complete 50-lens review and its local verification limits.

The other files in [`docs/audit/`](audit/) are point-in-time records.
They intentionally contain old paths, commands, findings, and counts.
Use them for provenance, not for the current product contract.

## Documentation checks

Run these checks from the repository root:

```bash
python3 tools/check_docs_links.py
python3 tools/check_docs_style.py
python3 tools/check_claims.py
```

The link check validates local paths, path case, and Markdown anchors.
The style check applies the repository writing rules to active first-party documentation.
The claim check rejects unsupported public performance and quality claims.
