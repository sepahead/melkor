# Release process

This checklist separates a reviewed source tree from a distributable desktop
release. A green source build does **not** imply that a macOS/Windows/Linux
bundle is signed, notarized, attested, or ready to publish.

## 1. Source release preflight

Run from a clean checkout of the intended tag. CI performs the same classes of
checks across macOS and Linux; CUDA is compiled in hosted CI but needs a real
NVIDIA runner for runtime parity.

```bash
./scripts/setup_deps.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMELKOR_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error
cmake --install build --prefix /tmp/melkor-install

python3 tests/test_release_metadata.py
python3 tests/test_release_evidence.py
python3 tests/test_gaussians_from_prediction.py
git diff --check
```

For the viewer and desktop shell:

```bash
cd viewer
npm ci --ignore-scripts
npm audit --audit-level=high
./fetch-assets.sh --runtime-only
bun run test -- --project=chromium

cd src-tauri
cargo fmt --check
cargo clippy --locked --all-targets -- -D warnings
cargo check --locked
cargo deny check --config deny.toml advisories bans licenses sources
cargo about generate --frozen --fail \
  -o ../RUST_THIRD_PARTY_LICENSES.html about.hbs
```

The staged desktop payload must contain only the explicit allowlist in
`viewer/stage-dist.js`. In particular, external developer/test scenes are not
redistributable assets and must never enter `viewer/dist/`.

## 2. Version and provenance gate

- Update `CHANGELOG.md` and replace `Unreleased` with the release version/date.
- Keep CMake's numeric base plus `MELKOR_PRERELEASE`, npm, Tauri, and Cargo at
  the same full SemVer; the metadata test fails on drift.
- Review every dependency-policy exception and its expiry date.
- Regenerate and review Rust third-party notices from the locked graph.
- Verify runtime/model/repository digests and immutable revisions without
  weakening a failed check.
- Create the tag from a clean, reviewed commit; do not release a dirty tree.

Build and independently verify the deterministic source evidence:

```bash
python3 scripts/build_release_evidence.py build \
  --ref v2.0.0-rc.1 \
  --release-tag v2.0.0-rc.1 \
  --output build-release/evidence-v2.0.0-rc.1
python3 scripts/build_release_evidence.py verify \
  build-release/evidence-v2.0.0-rc.1
```

Pushing an annotated `v*-rc.*` tag runs the full CI matrix and
`.github/workflows/release-candidate.yml`, which uploads the deterministic
source archive, per-file checksums, SPDX 2.3 source SBOM, unsigned provenance,
and aggregate checksums for review. Manual dispatches package the exact selected
commit; when the input resolves to a tag, they enforce the same annotated
tag/version contract as tag pushes. These unsigned artifacts support
self-consistency checks and byte-for-byte reproduction, but do not prove
publisher identity or prevent an attacker from replacing both content and
digests.

## 3. Production distribution gate

The repository currently builds unsigned developer artifacts and unsigned RC
source evidence. Before publishing native bundles, add and exercise a
tag-protected production workflow that:

1. builds each platform from lockfiles on a declared toolchain;
2. signs binaries and platform bundles using protected release identities;
3. notarizes macOS artifacts and verifies the notarization ticket;
4. generates checksums plus SPDX or CycloneDX SBOMs;
5. emits build provenance/attestations tied to the tag and commit;
6. uploads immutable artifacts only after all platform and policy jobs pass;
7. verifies every downloaded artifact from a separate clean environment.

Signing keys, certificates, provisioning profiles, and notarization credentials
must stay outside the repository. The root `.gitignore` blocks common local
credential formats, while the full-history secret scan remains the backstop.

## 4. Hardware/model qualification

Before describing a configuration as production-supported, record results for:

- CUDA runtime parity on every advertised compute capability;
- Metal on the oldest supported macOS/hardware combination;
- DA3 checkpoint inference on the documented Python/CUDA matrix;
- representative large and malformed GLB/PLY/SPZ inputs;
- viewer desktop bundles on each target OS, including offline startup;
- performance and memory benchmarks with reproducible inputs and commands.

No release note should claim algorithmic or benchmark state of the art unless a
dated, reproducible evaluation against named baselines is attached.
