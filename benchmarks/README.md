# Benchmarks

Melkor makes no universal quality or performance claim.
A benchmark must measure one defined operation on one declared input set.

The current benchmark scope includes these areas:

- Native inspection and conversion throughput
- Peak memory and temporary output use
- Format fidelity and SPZ quantization error
- Viewer loading and rendering behavior
- External adapter results, when each external tool has an exact identity

Each published measurement needs a versioned manifest and a licensed dataset identity.
It must include raw results and the exact hardware and software.

A quantitative documentation claim must link to a benchmark **result**.
A benchmark **manifest** must reproduce that result for the exact claim scope.
`tools/check_claims.py` checks public prose for unsupported claims.

## Why a manifest, not a number in a README

A number without context is not reproducible or falsifiable. A "10× faster" claim needs:

- The operation, dataset, and input size
- The hardware and software versions
- The repetition count
- The metric implementation

A manifest records this context. A reader can reproduce the run and check the number. A material
input change expires the claim. Examples include a new dependency, dataset, or specification
revision. The project must then measure or rewrite the claim.

## Repository layout

| Path | State | Purpose |
|---|---|---|
| `README.md` | Present | Policy and current status |
| `schema/benchmark-manifest-v1.schema.json` | Present | Declared benchmark inputs |
| `schema/benchmark-result-v1.schema.json` | Present | Recorded benchmark results |
| `datasets/manifest.json` | Present | Dataset identities and license records |
| `manifests/` | Planned | Executable benchmark declarations |
| `runners/` | Planned | Deterministic benchmark programs |
| `baselines/` | Planned | Accepted reference results |
| `reports/` | Planned | Generated publication output |

Add a planned directory only with its first reviewed artifact.

The manifest schema retains legacy category names for compatibility.
Schema acceptance does not make a category part of the current product scope.

Large licensed datasets stay outside Git.
`datasets/manifest.json` records each dataset identity and license.
It will record a digest before a benchmark uses that dataset.
The repository does not contain dataset bytes.

## What a result must record

Each result must record at least:

- The Melkor version, commit, and build flags
- The exact operation as CLI arguments or API configuration
- Dataset IDs, hashes, and licenses
- Each comparator version and configuration
- Seeds, warmups, and repetition count
- The OS, CPU, RAM, and compiler
- The GPU, driver, runtime, browser, and renderer when applicable
- Metric implementations and their versions
- Raw results for each run, not only the averages

A benchmark that reports a mean without its spread hides its variance.

## Status

The schemas and policy are present.
The repository has no executable benchmark manifest, runner, baseline, or published result.

Do not publish a quantitative claim until its reviewed result exists.
