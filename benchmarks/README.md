# Benchmarks

Melkor makes no universal "state of the art" or "fastest" claim. It measures each category
separately:

- Format fidelity and quantization error
- Parser and backend throughput
- Viewer rendering
- Mesh initialization and completion
- External reconstruction pipelines

Each measurement has a versioned manifest and a licensed dataset identity. It also records raw
results and the exact hardware and software.

A quantitative or superlative documentation claim must link to a benchmark **result**. A
benchmark **manifest** in this directory must produce that result for the exact claim scope.
`tools/check_claims.py` checks the prose. This directory contains the evidence.

## Why a manifest, not a number in a README

A number without context is not reproducible or falsifiable. A "10× faster" claim needs:

- The operation, dataset, and input size
- The hardware and software versions
- The repetition count
- The metric implementation

A manifest records this context. A reader can reproduce the run and check the number. A material
input change expires the claim. Examples include a new dependency, dataset, or specification
revision. The project must then measure or rewrite the claim.

## Layout

```text
benchmarks/
├── README.md                     this file
├── schema/
│   ├── benchmark-manifest-v1.schema.json   what a benchmark run declares
│   └── benchmark-result-v1.schema.json     what a benchmark run produces
├── manifests/                    reviewed benchmark manifests
├── datasets/
│   └── manifest.json             dataset identities, licenses, and digests (never the data)
├── runners/                      the code that executes a manifest and emits a result
├── baselines/                    accepted reference results, updated only with review
└── reports/                      generated static reports for publication
```

Large licensed datasets live **outside** Git. `datasets/manifest.json` records each dataset's
identity, license, and digest. A run uses this information to fetch and verify the dataset. The
repository does not contain the dataset bytes. Research licenses rarely permit redistribution,
and large datasets do not belong in the source tree.

## What a result must record

Each result records at least:

- The Melkor version, commit, and build flags
- The exact operation as CLI arguments or API configuration
- Dataset IDs, hashes, and licenses
- Each comparator version and configuration
- Seeds, warmups, and repetition count
- The OS, CPU, RAM, and compiler
- The GPU, driver, runtime, browser, and renderer when applicable
- Metric implementations and their versions
- **Raw results for each run, not only the averages**

A benchmark that reports a mean without its spread hides its variance.

## Status

The schema and policy are in place. The repository has no executable benchmark manifest or
published result. Work package 21 adds runners and a release gate on stable hardware.

Do not publish a quantitative claim until its reviewed result exists.
