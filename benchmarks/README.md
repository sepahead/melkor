# Benchmarks

Melkor makes no universal "state of the art" or "fastest" claim. Format fidelity, quantization
error, parser and backend throughput, viewer rendering, mesh initialization, completion, and
external reconstruction pipelines are measured **separately**, each with a versioned manifest, a
licensed dataset identity, raw per-run results, and the exact hardware and software that produced
them.

A quantitative or superlative claim in Melkor's documentation is valid only when it links to a
benchmark **result** produced from a benchmark **manifest** in this directory, for the exact
scope of the claim. `tools/check_claims.py` enforces the prose side of that rule; this directory
is the evidence side.

## Why a manifest, not a number in a README

A number without its context is not reproducible and, worse, not falsifiable. "10× faster" is
meaningless without the operation, the dataset, the input size, the hardware, the software
versions, the number of repetitions, and the metric implementation. A manifest records all of
that, so a reader can reproduce the run and check the number rather than take it on faith. When
any of those inputs changes materially — a new dependency, a different dataset, a spec revision —
the claim expires and must be re-measured or reworded.

## Layout

```text
benchmarks/
├── README.md                     this file
├── schema/
│   ├── benchmark-manifest-v1.schema.json   what a benchmark run declares
│   └── benchmark-result-v1.schema.json     what a benchmark run produces
├── manifests/                    one manifest per benchmark, versioned by ID
│   └── format-roundtrip-v1.json
├── datasets/
│   └── manifest.json             dataset identities, licences, and digests (never the data)
├── runners/                      the code that executes a manifest and emits a result
├── baselines/                    accepted reference results, updated only with review
└── reports/                      generated static reports for publication
```

Large licensed datasets live **outside** git. `datasets/manifest.json` records each dataset's
identity, licence, and digest so a run can fetch and verify it; the bytes themselves are never
committed, because a research dataset's licence rarely permits redistribution and its size does
not belong in the source tree.

## What a result must record

Every result captures, at minimum: the Melkor version, commit, and build flags; the exact
operation (CLI argv or API config); the dataset IDs, hashes, and licences; any comparator's
version and configuration; seeds, warmups, and repetition count; the OS, CPU, RAM, and compiler;
the GPU, driver, runtime, browser, and renderer where relevant; the metric implementations and
their versions; and the **raw per-run measurements, not only the averages**. A benchmark that
reports a mean without its spread is hiding its variance.

## Status

The schema and policy are in place. The runners and the first published results are part of
work package 21, which also wires a release regression gate on stable hardware. Until a result
exists for a claim, that claim may not appear in a public Melkor surface.
