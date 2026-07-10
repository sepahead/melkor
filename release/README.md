# Release evidence

`scripts/build_release_evidence.py` creates an unsigned, deterministic evidence
bundle from an exact Git commit. It never packages uncommitted working-tree
content.

Snapshot review:

```bash
python3 scripts/build_release_evidence.py build \
  --ref HEAD \
  --output build-release/evidence
python3 scripts/build_release_evidence.py verify build-release/evidence
```

Release-candidate review uses an annotated tag whose name must match the full
CMake base plus `MELKOR_PRERELEASE` version exactly:

```bash
python3 scripts/build_release_evidence.py build \
  --ref vX.Y.Z-rc.N \
  --release-tag vX.Y.Z-rc.N \
  --output build-release/evidence-vX.Y.Z-rc.N
```

The output directory must not already exist. It contains:

- a normalized `tar.gz` source archive with a zero gzip timestamp;
- SHA-256 hashes for every tracked source file;
- an SPDX 2.3 JSON source SBOM built from `components.json`;
- an unsigned in-toto statement using the SLSA provenance v1 predicate shape;
- `SHA256SUMS` covering every generated evidence file.

The build fails closed when the executing generator differs from the selected
Git tree, or for unresolved Git LFS pointer blobs, submodules, unsafe paths or
escaping symlinks, missing dependency manifests, missing license evidence,
duplicate component identities, and tag/version mismatches. The
verifier reads the archive without extracting it and compares every member to
the tracked-file digest manifest. It also rejects unlisted, nested, symlinked,
or otherwise non-regular evidence-directory entries.

This is source-release evidence, not a complete binary SBOM. Vendored source is
file-analyzed; downloaded runtime components are declared but marked
`filesAnalyzed: false`. Platform package SBOMs should later be generated from
the actual signed bundles. The provenance statement is deterministic but
unsigned, so authenticity still requires a protected signing or keyless
attestation step.
