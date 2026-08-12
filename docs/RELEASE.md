# Release process

This process separates source verification from production artifact publication.
A green source build does not make an unsigned desktop bundle a supported artifact.

## 1. Verify the native source

Run these commands from a clean checkout of the selected commit:

```bash
./scripts/setup_deps.sh
cmake --preset release
cmake --build --preset release --parallel
ctest --test-dir build/release --output-on-failure --no-tests=error

cmake --preset spz-off
cmake --build --preset spz-off --parallel
ctest --preset spz-off

./scripts/test_sdk_install.sh -DMELKOR_BUILD_SPZ=ON
python3 tools/verify_third_party.py --check
python3 tools/generate_notices.py --check
python3 tools/check_version_sync.py --check
python3 tools/build_source_bundle.py --check
python3 tools/check_profiles.py
python3 tools/check_docs_links.py
python3 tools/check_docs_style.py
python3 tools/check_claims.py
python3 tests/test_release_metadata.py
python3 tests/test_release_evidence.py
git diff --check
```

Use the `asan-ubsan` preset for the sanitizer run.
Run every libFuzzer target against its tracked corpus.

Hosted CI repeats the native checks on macOS, Linux, and Windows.
The Windows lane disables optional SPZ support.

## 2. Verify the viewer

Run these commands when the release includes viewer source or a desktop artifact:

```bash
cd viewer
npm ci --ignore-scripts
npm audit --audit-level=high
./fetch-assets.sh
bun run test -- --project=chromium
bun stage-dist.js

cd src-tauri
rustup run 1.88.0 cargo fmt --check
rustup run 1.88.0 cargo clippy --locked --all-targets -- -D warnings
rustup run 1.88.0 cargo test --locked
rustup run 1.88.0 cargo check --locked
```

Run the pinned `cargo-deny` and `cargo-about` checks from CI.
Build the staged application with Tauri CLI 2.11.4.

`viewer/stage-dist.js` defines the desktop payload.
External developer fixtures must not enter `viewer/dist/`.

## 3. Freeze the version and source

Before the tag:

1. Replace `Unreleased` in `CHANGELOG.md` with the version and date.
2. Synchronize `VERSION`, CMake, npm, Tauri, and Cargo versions.
3. Review each dependency-policy exception and expiry date.
4. Regenerate and review each dependency notice.
5. Verify every source and runtime digest.
6. Create an annotated tag from the reviewed commit.

Do not create a release from a dirty working tree.

## 4. Build source evidence

Build and verify deterministic evidence from the annotated tag:

```bash
python3 scripts/build_release_evidence.py build \
  --ref vX.Y.Z-rc.N \
  --release-tag vX.Y.Z-rc.N \
  --output build-release/evidence-vX.Y.Z-rc.N
python3 scripts/build_release_evidence.py verify \
  build-release/evidence-vX.Y.Z-rc.N
```

The evidence contains these items:

- A deterministic source archive
- Per-file SHA-256 values
- An SPDX 2.3 source SBOM
- Unsigned provenance
- Aggregate checksums

The source archive, manifest, and SBOM use one Git-tree policy decision.
The evidence builder verifies each selected byte against the selected commit.

Pushing an annotated `v*-rc.*` tag runs the full CI matrix and the evidence workflow.
A manual evidence run must select an exact commit or annotated tag.

## 5. Publish production artifacts

The repository currently produces unsigned source evidence and unsigned developer desktop builds.
These artifacts do not prove publisher identity.

Before a production publication:

1. Build each artifact from lockfiles on a declared toolchain.
2. Sign each binary and platform bundle with a protected identity.
3. Notarize each macOS bundle and verify its ticket.
4. Generate artifact checksums and an artifact SBOM.
5. Emit signed or keyless provenance for the tag and commit.
6. Upload immutable artifacts only after every required job passes.
7. Verify each downloaded artifact in a separate clean environment.

Keep signing keys and platform credentials outside the repository.

## 6. Attach release evidence

The production release record must include:

- The exact commit and annotated tag
- The supported operating systems and architectures
- The supported format profile identifiers
- The complete CI result for the release commit
- Licensed conformance-corpus results
- Khronos glTF Validator output for generated GLB fixtures
- Clean install and relocation results for the C SDK
- The independent review required by `GOVERNANCE.md`

Do not claim support for an excluded viewer, adapter, platform, or format revision.
