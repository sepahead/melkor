# Melkor threat model

**Applies to:** the `2.0.x` development line.

**Status:** This living document describes the current source tree.
It does not describe planned controls as implemented controls.

See [`SECURITY.md`](../../SECURITY.md) for reporting instructions and supported versions.

## 1. Security objective

Melkor reads untrusted Gaussian-splat assets.
A valid file can still contain hostile sizes, paths, metadata, or numeric values.

The native product must fail safely when an input exceeds its contract.
It must not damage an existing output after a failed conversion.
It must report each representational loss before it commits output.

Melkor is not a sandbox.
Run a high-risk conversion inside an operating-system sandbox when process isolation is required.

## 2. Protected assets

| ID | Asset | Required protection |
|---|---|---|
| A-1 | Host resources | Bound memory, input, decoded data, temporary output, structure, and execution time. |
| A-2 | Existing files | Preserve the destination after each failure before output installation. |
| A-3 | Unnamed files | Read only the primary input and permitted adjacent glTF buffer resources. |
| A-4 | Output semantics | Reject malformed values and report each permitted representational loss. |
| A-5 | Terminal and JSON consumers | Escape untrusted text and keep machine diagnostics stable. |
| A-6 | Build and release inputs | Pin vendored source and verify the release inventory. |

Melkor processes scene contents locally.
The native CLI does not upload an asset or open a network connection.

## 3. Trust boundaries

| ID | Boundary | Untrusted data |
|---|---|---|
| TB-1 | Asset to native parser | PLY headers and records, SPZ streams, glTF JSON, GLB chunks, and binary buffers. |
| TB-2 | Asset to filesystem | Command paths and glTF buffer URIs. |
| TB-3 | Native writer to filesystem | Destination state, parent directories, links, and concurrent changes. |
| TB-4 | Asset to viewer | Local PLY, SPZ, SPLAT, KSPLAT, SOG, and ZIP files. |
| TB-5 | Dependency to build | Vendored C++ source, Rust crates, JavaScript modules, and development adapter dependencies. |
| TB-6 | Diagnostic to consumer | File names, metadata, error context, and loss details. |

Development adapters run outside the native core.
They can invoke external tools and cross a separate network and process boundary.

## 4. Native input contract

The native registry accepts five exact semantic profiles:

- `ply:melkor-canonical-v1`
- `ply:graphdeco-3dgs-v1`
- `ply:da3-gaussian-v1`
- `spz:spz-v1-v3`
- `khr-gaussian-splatting-rc-63770cc`

The glTF reader accepts a narrow Gaussian subset.
It records optional content outside that subset as severe source-read losses.
This content includes non-Gaussian primitives, materials, skins, animations, cameras, morph targets, and optional extension content.
A conversion needs each exact loss approval before it drops that content.
The reader rejects malformed fields and unsupported required extensions.

JSON glTF can use a data URI or a portable relative buffer path.
The native reader rejects schemes, absolute paths, parent traversal, links, device names, queries, and fragments.
It opens each permitted resource below the bound asset directory.

## 5. Native controls

### Checked structure and numeric values

The checked-arithmetic helpers reject overflowing products, additions, ranges, and host-size conversions.
Readers validate counts and allocation sizes before they allocate output storage.

The canonical model rejects nonfinite values and invalid SH layouts.
Format readers enforce profile-specific property, accessor, transform, and color rules.

The glTF reader rejects cycles, shared nodes, invalid roots, and excessive depth.
It rejects affine transforms that cannot preserve the supported Gaussian semantics.

### Shared resource controls

Each native operation uses one `OperationContext` and one `Budget`.
The selected `web`, `desktop`, or `server` profile defines nonzero limits.
The CLI does not expose the `custom` profile.

Native readers and writers account for these resources before use:

- Primary input and glTF sidecar bytes
- Decoded data and working memory
- Temporary output bytes
- Splats, glTF nodes, accessors, and external resources
- PLY header and metadata sizes
- SPZ expanded size and decompression ratio

Long loops check cancellation and the monotonic deadline at bounded intervals.
The CLI maps `SIGINT` to cooperative cancellation.

### Input identity and change detection

PLY and SPZ use a shared verified input handle.
The reader checks the file type and size before it reads data.
It verifies that the file identity and metadata did not change during the read.

The glTF reader binds resource traversal to the asset directory on POSIX and Windows.
It rejects symbolic links and Windows reparse points.

### Output integrity

Native file writers use `AtomicWriter`.
The writer creates an exclusive temporary file in the destination directory.
It writes through an open handle and installs the result through the bound directory.

The default policy refuses an existing destination.
`--force` enables replacement.
The CLI does not approve a symbolic-link destination.

A failure before installation leaves the old destination unchanged.
A full-durability failure can occur after installation.
The API reports this state through `committed()`.

### Loss and diagnostic integrity

Each successful `melkor convert` command writes one schema-valid loss report.
A severe loss needs approval for its exact stable code.
A fatal loss cannot receive approval.

The CLI escapes control bytes and invalid UTF-8 in display text.
It emits stable diagnostic codes and stable error-class exit codes.
Inspect JSON reports contain the input basename, not its absolute path.

## 6. Viewer controls

The viewer processes local files inside the browser or desktop webview.
It does not upload a selected file or retain persistent filesystem access.

The viewer applies a 256 MiB file limit and a 5,000,000-splat limit.
It accepts only bounded binary PLY layouts with exact record sizes.
It accepts one KSPLAT section and requires matching header and section counts.
It checks KSPLAT numeric values before Spark processes them.
It rejects scales outside the Spark packed range instead of silently clamping them.
SOG and ZIP input also has these limits:

- At most 4,096 entries
- At most 512 MiB of expanded data
- At most a 100:1 declared expansion ratio
- One UTF-8 SOG metadata file of at most 64 KiB
- Static PNG or WebP images of at most 5,000,000 pixels each
- At most 256 MiB for all decoded SOG RGBA images

The ZIP preflight binds each central record to its local header.
It rejects unsafe names, duplicate names, unsupported flags, unsupported methods, overlaps, and inconsistent sizes.
It streams each stored or deflated entry before Spark processes the archive.
It verifies the actual expanded size and CRC-32 value.
It validates SOG metadata, image references, image headers, and decoded pixel bounds.

The development server binds to loopback by default.
It accepts only `GET` and `HEAD`.
It serves an allowlist below its canonical root.

The packaged Tauri frontend uses a strict content security policy.
It has no Tauri IPC permissions.
The distribution script uses an exact asset allowlist and verifies runtime module digests.

## 7. Supply-chain controls

`third_party/manifest.lock.json` records each upstream review target, content digest, and local patch.
`tools/verify_third_party.py` checks the vendored bytes and patches against local digests.
The offline check does not fetch upstream or prove the recorded revision relationship.

The source-bundle policy selects the release boundary from the Git tree.
The evidence generator verifies the selected bytes, manifest, and SPDX file list.

The viewer locks Rust and JavaScript dependencies.
Rust policy checks cover advisories, licenses, duplicate crates, and source origins.
The desktop license inventory comes from the locked Rust graph.

## 8. Residual risks

| Risk | Impact | Required action |
|---|---|---|
| Long-run fuzzing and a licensed conformance corpus remain incomplete. | Parser defects can remain outside the reviewed regression set. | Run sustained fuzzing and publish exact-corpus results for the release commit. |
| Khronos validator evidence is not yet attached to the release commit. | The custom glTF subset can diverge from the base glTF contract. | Validate generated GLB output and publish the exact tool version and result. |
| The viewer uses SparkJS instead of the native parser. | Native limits and semantic checks do not protect the browser path. | Keep browser limits independent and add a shared reviewed parser before a stronger security claim. |
| Viewer parsing is not isolated in a separate process. | A hostile file can degrade or crash the browser tab or webview. | Add process or worker isolation before multi-tenant use. |
| Development adapters have external Python and model dependencies. | A compromised package or model can execute with the operator's authority. | Keep adapters optional and add complete hash manifests before distribution. |
| Release artifacts are not signed or notarized. | A user cannot verify publisher identity from the artifact alone. | Sign, attest, and verify each production artifact. |
| One maintainer controls the repository. | Independent security review and release approval can become a bottleneck. | Require an independent reviewer before the production release. |

## 9. Assumptions and non-goals

- The operator controls command arguments and local file permissions.
- The native CLI does not provide authentication, tenancy, or network service isolation.
- The viewer server is a local development server. Do not expose it to an untrusted network.
- Operating-system confinement remains necessary for hostile files when process compromise is unacceptable.
- Availability of an external adapter service is not a native Melkor security property.

## 10. Maintenance rule

Update this document in the same commit as each security control.
Name the implementing file only after the control exists.
Move each remaining risk only after exact-commit evidence verifies the result.
