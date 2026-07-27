# Melkor threat model

**Applies to:** the `2.0.x` development line (`VERSION` currently reads `2.0.0-dev`).
**Status:** living document. It describes what Melkor defends against today, what it
does not yet defend against, and which release blocker tracks each gap. A control is
listed as implemented only if it exists in the source tree. Each other control is a
residual risk with an assigned blocker.

The companion document is [`SECURITY.md`](../../SECURITY.md), which covers reporting,
supported versions, and scope. This document is the reasoning behind that scope.

---

## 1. Why Melkor needs a threat model

Melkor's core is a parser. It reads PLY, SPZ, and glTF/GLB Gaussian-splat assets,
inspects them, converts between them, and normalizes their contents. These files
frequently come from:

- A collaborator's capture
- A marketplace download
- A chat link
- A file dragged into a browser tab

Every count, offset,
stride, dimension, and length in them is a number chosen by whoever produced the file.

That is the problem. One multiplication on an untrusted file integer can cause a heap
overflow. One allocation from an untrusted file size can stop the machine. Melkor is
written in C++, so
neither failure mode is caught for us by a runtime.

Melkor holds no secrets, authenticates nobody, and serves no traffic. It is not a
target because of what it stores. It is a target because of what it opens.

---

## 2. Assets to be protected

| ID | Asset | Why it matters |
| --- | --- | --- |
| A-1 | Availability of the host's memory, CPU, and disk | A conversion is often run in a batch job or a CI runner. A single file that exhausts memory or fills a disk takes down more than one conversion. |
| A-2 | Integrity of files already on disk | Above all the *destination* file. A user converting `scene.ply` to `scene.spz` for the second time must not lose the `scene.spz` they already had. |
| A-3 | Confidentiality of the filesystem outside the named inputs | A crafted asset must read only named inputs. Its output and reports must not expose other file contents. |
| A-4 | Integrity of the produced asset | Silent corruption is worse than a crash. A conversion that quietly divides color by 255, drops spherical-harmonic coefficients, or renormalizes a quaternion produces an untrustworthy file. |
| A-5 | Integrity of the toolchain and the build | Melkor compiles third-party source into its binary and its setup scripts fetch external toolchains. Code that arrives through either path runs with the user's privileges. |
| A-6 | Integrity of the consuming terminal or tool | Diagnostics are read by humans in terminals and by machines in CI. Bytes taken from an untrusted file and echoed into either are an injection surface. |
| A-7 | The user's own environment details | Inspection reports get pasted into public issue trackers. An absolute path leaks a username and a directory layout. |

Explicit non-asset: the confidentiality of the scene being processed. Melkor is a
local, offline tool. It does not upload, phone home, or share the asset with anything
the user did not invoke.

---

## 3. Trust boundaries

| ID | Boundary | Crossing |
| --- | --- | --- |
| TB-1 | Asset file → parser | The primary boundary. Bytes chosen by an attacker become integers, sizes, offsets, and allocations inside a C++ process. |
| TB-2 | Process → filesystem | Paths taken from the command line, and paths taken from *inside* assets (glTF external URIs), resolve to real files. Symlinks, `..`, and identical-file aliases all live here. |
| TB-3 | Core → external tools | COLMAP, OpenSplat, LichtFeld Studio, gsplat, and DA3 run as separate processes. Their output and status are untrusted Melkor inputs. |
| TB-4 | Network → machine | Setup scripts clone repositories, install Python packages, and download toolchains and model weights. Everything that crosses this boundary is executable in practice. |
| TB-5 | Asset → browser | The viewer loads splat files, including by drag-and-drop, and renders them through WebGL in the user's browser session. |
| TB-6 | Diagnostics → consumer | Melkor's own output crosses back out to a terminal, a JSON consumer, a CI log, and often a public bug report. |
| TB-7 | Upstream source → build | Vendored third-party code (SPZ, tinygltf, stb) is compiled into the binary and inherits all of its privileges. |

---

## 4. Attackers

**AT-1 — The asset supplier.** The primary adversary. They control every byte of a
`.ply`, `.spz`, `.glb`, or an image or buffer referenced from one, plus the filename.
They have no other access to the machine and cannot choose the command-line flags.
Their goals, in descending order of value, are:

1. Execute code in the Melkor process.
2. Read a file elsewhere on the machine.
3. Destroy a file that the user needs.
4. Exhaust the machine's resources.
5. Corrupt the conversion output without detection.

**AT-2 — The network or upstream position during setup.** A party who controls what
`git clone`, `pip install`, `curl`, or a Hugging Face fetch returns. The party can
compromise an upstream repository or the network path. Setup scripts build fetched
code with the user's privileges. Thus, this attacker can execute code directly.

**AT-3 — A local unprivileged process.** Can plant a symlink or win a race in a
directory Melkor writes to. Goal: redirect a write to a file the user did not name.

**AT-4 — A compromised or hostile dependency maintainer.** Can alter upstream source
between the revision that was reviewed and the revision that is built.

Explicit non-adversary: **the operator.** Melkor does not defend the machine against
the person running the command. A user who raises a limit, allows a symlinked output
destination, or points the tool at their own files uses their own authority. This action
does not defeat a control. Melkor must make the consequences visible.

---

## 5. Entry points: the untrusted input inventory

Everything below is attacker-controlled and must be treated as hostile.

**Asset data and structure**
- PLY: ASCII and binary headers, element and property declarations, declared counts,
  property list lengths, and derived strides.
- SPZ: the compressed container, the packed fixed-point fields, the declared
  `fractionalBits` shift exponent, the SH degree, and the format version.
- glTF and GLB: the JSON document, the GLB chunk table, accessors, buffer views,
  buffers, node graphs and their depth, declared and required extensions.
- Compressed streams inside any of the above, and their declared decoded sizes.
- Images referenced by glTF assets, including their declared dimensions.
- Mesh topology: vertex counts, index buffers, and the indices themselves.

**Text taken from assets**
- PLY comments, glTF node/mesh/material names, `asset.extras`, and any other
  metadata string. These reach diagnostics, reports, and the headers Melkor writes.

**Paths and identifiers**
- Filenames, directory names, and the command-line paths themselves.
- glTF external URIs — a path chosen *inside* the asset, pointing *outside* it.

**The external-tool boundary**
- Adapter and conversion manifests, pipeline run state, and stage output files.
- The stdout, stderr, and exit codes of COLMAP, OpenSplat, LichtFeld, gsplat,
  and DA3. Output *discovered* on the filesystem after a stage runs is not proof that
  the stage wrote it.

**The network boundary**
- Cloned repositories and their later contents.
- Python packages and their transitive dependencies.
- Downloaded archives (libtorch), and model weights and checkpoints.

**The interactive boundary**
- Files dropped onto the viewer by drag-and-drop, and files served to it.
- Command-line arguments, and environment variables that alter behavior
  (`MELKOR_DA3_REF`, `MELKOR_TORCH_INDEX_URL`, `MELKOR_PYTHON`, `PORT`, `HOST`,
  `PATH`).

---

## 6. Abuse cases

Each case names the mitigation and its state. "Substrate only" means that tests cover the control.
The required production path does not use the control yet. See section 8.

### Memory safety at TB-1

| # | Abuse case | Mitigation | State |
| --- | --- | --- | --- |
| AC-01 | A file declares a count and stride whose product overflows 64-bit arithmetic. The allocation is small, the fill loop is not, and the heap is overwritten. | `checked_mul` / `checked_array_bytes` (`include/melkor/checked.hpp`). | Implemented in canonical scene storage and glTF accessors. Other paths need migration (P0-12). |
| AC-02 | A glTF buffer view uses offset arithmetic that wraps. A naive range check then permits an out-of-bounds read. | `checked_range` validates the addition before the comparison. | Implemented in the glTF accessor. |
| AC-03 | A 64-bit file-declared size is narrowed implicitly to `size_t`. On 64-bit hosts nothing happens. On a 32-bit build, the value truncates and the allocation is short. | `checked_size_cast` / `checked_u32_cast`, which make every narrowing explicit and fallible. | Implemented in canonical scene storage and glTF resolution. Other paths need migration (P0-12). |
| AC-04 | An SPZ file declares a `fractionalBits` value large enough to make the fixed-point shift undefined behavior, or packs alpha bytes that decode to non-finite logits. | Local patch `third_party/patches/spz/0002-bound-fixed-point-bits-and-finite-logits.patch`, which clamps both. | Implemented. |
| AC-05 | A glTF node graph is deeply nested or cyclic. A recursive or repeated walk exhausts stack or memory. | The glTF reader uses an iterative walk, a visited set, and `max_gltf_nodes`. | Implemented. |

### Resource exhaustion at TB-1 — **in scope**

The previous security policy declared denial of service from large but well-formed
input out of scope. That was wrong, and reversing it is release blocker **P0-12**. A
40 GB PLY that declares two billion splats is not malformed. It is a file that a
machine cannot survive, and "it parsed correctly" is no comfort to the CI runner that
died. Resource exhaustion from large well-formed input *and* from maliciously crafted
input is now in scope and is treated as a security bug.

| # | Abuse case | Mitigation | State |
| --- | --- | --- | --- |
| AC-06 | A small file declares an enormous decompressed size. Inflation exhausts memory. A pure decoded-byte cap still permits a 1 KiB file to expand to the complete cap. | `Budget::check_decompression_ratio` uses the compressed and *claimed* decoded sizes. It rejects the bomb before inflation. `max_decoded_bytes` bounds its shape and size. | Substrate only. No reader calls this check yet (P0-12). |
| AC-07 | An SPZ file must be fully inflated before its version can be read, forcing decompression of attacker-controlled data before any policy check can run. | Local patch `0001-probe-spz-version-without-inflating.patch`, adding a bounded header-only probe. | Implemented. |
| AC-08 | A well-formed file declares an internally consistent but enormous splat, vertex, or triangle count. The allocation succeeds and stops the machine. | `Limits` counts (`max_splats`, `max_mesh_vertices`, `max_mesh_triangles`), charged through `Budget::consume` *before* the allocation. | glTF and PLY enforce count limits. The vendored SPZ decoded allocation remains open (P0-09 and P0-12). |
| AC-09 | An image declares dimensions of 1 × 4,000,000,000. A per-axis check passes, but the decoded buffer does not. | Both `max_image_dimension` and `max_image_pixels`, because either alone is bypassable. | Substrate only (P0-12). |
| AC-10 | An asset carries megabytes of comments or names, or a PLY header that never ends. | `max_ply_header_bytes`, `max_metadata_string_bytes`, `max_metadata_total_bytes`. | PLY enforces the header limit. Metadata totals remain open (P0-12). |
| AC-11 | A conversion of a legitimate but very large asset fills the disk with temporary output. | `max_temp_bytes`, charged by `AtomicWriter::write` as bytes are appended. | Implemented for PLY and SPZ output. |
| AC-12 | A parse takes hours, and Ctrl-C does nothing because cancellation is checked once per file. | `CancellationToken`, checked at bounded intervals inside long loops. Target latency is less than approximately 100 ms. | Substrate only. No reader checks the token yet (P0-12). |

`include/melkor/limits.hpp` contains three named profiles: `web`, `desktop`, and
`server`. Each context needs different limits. One default would be unsuitable for a
batch job or unsafe for a browser tab. A profile can raise a limit.
`melkor::hard_ceiling` restricts each custom profile. Melkor rejects a limit that its
arithmetic cannot represent.

**There is deliberately no "disable all limits" switch.** Checked arithmetic,
structural format limits, path containment, and output integrity remain on regardless
of profile. A user who needs to process a very large asset should specify its size
instead of removing the safeguards.

### Filesystem integrity at TB-2

| # | Abuse case | Mitigation | State |
| --- | --- | --- | --- |
| AC-13 | A glTF URI escapes the asset directory through a special path or scheme. Melkor then reads an unrelated file. | `src/safe_gltf_fs.hpp` resolves each URI before its containment check. It rejects special schemes, NUL bytes, and escaping paths. | Implemented. |
| AC-14 | The output path is a symlink planted by another user, and the write lands wherever it points. | `AtomicWriter` refuses to follow a symlink at the destination unless `allow_output_symlink` is set explicitly. | Implemented. |
| AC-15 | A conversion fails halfway and leaves neither the new file nor the old one. The SPZ writer caused this problem when it used `trunc` and `std::remove`. Release blocker P0-08. | `AtomicWriter` (`src/io/atomic_writer.cpp`) validates first. It creates an unpredictable `O_EXCL` temporary **in the same directory**, writes, flushes, and atomically replaces. It can also use fsync. The destination stays closed until commit. `rename()` is atomic only within one filesystem, so the temporary must share the directory. | Implemented (P0-08 closed). |
| AC-16 | A user names the same input and output through different path spellings. A naive writer then destroys the input. | `melkor::io::is_same_file` compares device and inode identity instead of path text. | Implemented. |
| AC-17 | Overwriting by default destroys a file named by a typo. | `WriteOptions::overwrite` defaults to false. | Implemented. |

### Output and diagnostic hygiene at TB-6

| # | Abuse case | Mitigation | State |
| --- | --- | --- | --- |
| AC-18 | Untrusted text contains terminal control sequences. Inspection then changes the user's terminal state. | `src/safe_text.hpp` escapes C0, C1, DEL, and invalid UTF-8 bytes. This includes the single-character `U+009B` CSI. | Implemented. |
| AC-19 | An inspection report is pasted into a public issue and leaks `/home/alice/clients/acme/…`. | `DiagnosticPathPolicy` defaults to `basename`. Full paths are available only when the caller asks. | Implemented. |
| AC-20 | A consumer greps stderr for the word "invalid", and a reworded message silently becomes a breaking change. | `Diagnostic::code` is a stable machine identifier and `ErrorCode` maps one-to-one onto documented exit codes (`exit_code_for`), so a script branches on codes and never on prose. | Implemented. |
| AC-21 | A newline embedded in untrusted metadata creates a forged PLY header line. | Sanitization of writer comments. | **Not implemented — P1-05.** |

### Supply chain at TB-4 and TB-7

| # | Abuse case | Mitigation | State |
| --- | --- | --- | --- |
| AC-22 | Vendored source changes without a declared update. The build then includes an unknown source tree. | The lock records source and content digests. CI checks the compiled tree. Each local patch has a digest and rationale. | Implemented. |
| AC-23 | A setup script clones a moving branch. A compromised upstream account can then execute code during setup. | Mutable trainer and streaming installers are failure-only or read-only. DA3 and feedforward setup use detached source revisions. | Partial — P0-13 still requires complete manifests. |
| AC-24 | A downloaded archive, Python wheel, or model checkpoint is substituted with a tampered file. | DA3 pins source and model repository revisions. Python dependency resolution still lacks a complete hash lock. | **Not implemented — P0-13.** |
| AC-25 | Native Melkor downloads model weights and accepts a tampered payload. | The core does not download weights. Retired native model options fail closed. External setup inherits the AC-24 risk. | Fails closed. |
| AC-26 | A hostile filename or path is interpolated into a shell command and executes. | The core has no `popen` or `std::system` call. Current wrappers use argument arrays. Some experimental setup scripts still have permissive failure paths. | Core: removed. Scripts: **not implemented — P0-15.** |

### External tools at TB-3, and the viewer at TB-5

| # | Abuse case | Mitigation | State |
| --- | --- | --- | --- |
| AC-27 | A pipeline stage "succeeds", and the next stage picks up a stale or attacker-planted file that a broad filesystem search happened to find. | Explicit adapter result manifests and content-addressed stage state. | **Not implemented — P1-09.** |
| AC-28 | A crafted viewer URL escapes the static root. The server then exposes a private local file. | `viewer/serve.js` resolves the URL before containment and allowlist checks. It binds to loopback and accepts only GET and HEAD. | Implemented. |
| AC-29 | A file dropped into the viewer crashes the tab or exhausts the browser's memory. | The `web` limits profile exists for this budget. The viewer does not consume the shared core through WASM yet. It is one large HTML file without worker isolation. | **Not implemented — P1-11.** |

---

## 7. Mitigations: the substrate, and why each exists

Five parts of the v2 hardening work exist. The sections below describe the failure that
each part prevents. A recorded rationale helps prevent accidental removal of a control.

**`include/melkor/error.hpp` — `Result<T>`, stable diagnostics, exit codes.**
A `bool` cannot identify malformed input, a limit failure, or cancellation. Each
condition needs a different response. An English message is not a contract. Searching
stderr for "invalid" makes its wording an unknown API.

Every fallible operation returns
`Result<T>`
carrying a coarse `ErrorCode` for control flow and diagnostics with stable codes for
reporting. `ErrorCode::resource_limit` is deliberately distinct from
`ErrorCode::invalid_data`.

A file can be valid and too large. In that case, the
remedy is a limits flag, not a new file. `Result` also carries diagnostics on success.
Dropping a warning after a successful operation can cause silent data corruption.

**`include/melkor/checked.hpp`, `src/core/checked.cpp` — checked arithmetic.**
Each parser multiplies a count by a stride or adds an offset to a length. It can also
narrow a file-declared 64-bit number to `size_t` before allocation. Each operation can
cause an integer overflow.

In this position, an integer overflow can cause a heap
overflow. The allocation becomes smaller than the loop that fills it. The header
requires each file-supplied number to use these functions before allocation.

All intermediate arithmetic uses `uint64_t`. Each result is narrowed
explicitly. Thus, a 32-bit build cannot silently truncate a value that a
64-bit build accepted.

**`include/melkor/limits.hpp`, `include/melkor/budget.hpp` — limits and budgets.**
A new parser can omit a limit when each parser is responsible for its own checks.
An operation carries its `Budget` in an `OperationContext`. The operation must call
`consume` before the applicable allocation. Accounting after allocation cannot prevent
an out-of-memory failure.

The budget is thread-safe and is not a global singleton. Thus, a server can run jobs
with different limits.

Tests also remain order-independent. A test can give an
operation a 100-byte budget. It can then verify a clean failure at the correct point.
Diagnostics name the limit, observed value, and applicable override flag. Thus, a
refusal explains how to continue.

**`include/melkor/io/atomic_writer.hpp`, `src/io/atomic_writer.cpp` — atomic output.**
The rule: a failed write must never damage the file that was already there. Every
output uses one implementation. These outputs include PLY, SPZ, glTF, JSON reports, and
run manifests. Format-specific copies caused P0-08 and can cause it again.

If the writer
is destroyed without `commit()`, it removes the temporary file. The destination stays
unchanged.

**`third_party/manifest.lock.json`, `tools/verify_third_party.py` — dependency pinning.**
See AC-22. The two SPZ patches it declares are both security fixes, and both are
recorded with their rationale rather than applied silently.

Continuous integration builds with `-Werror`, runs the test suite under AddressSanitizer
and UndefinedBehaviorSanitizer on macOS, and runs `verify_third_party.py --check` on
every push.

---

## 8. Residual risk

These items remain incomplete.

| Risk | Impact | Blocker |
| --- | --- | --- |
| **SPZ decoded allocation is not fully budgeted before inflation.** PLY, glTF, and SPZ enforce input limits. The vendored SPZ decoder still inflates before Melkor can charge the decoded allocation. | A compressed input can cause excessive decoded allocation within the vendored limit. | **P0-12** and **P0-09** |
| **Long-run fuzzing is incomplete.** Four libFuzzer targets and reviewed seed corpora exist. Scheduled corpus evolution and manifest or CLI targets do not exist. | Hostile input coverage can miss parser and command defects. | **P1-12** |
| **External Python environments lack complete hash locks.** DA3 and feedforward setup pin source revisions. Python packages and all model files still need manifest verification. | A compromised package or model registry can execute code as the user. | **P0-13** |
| **Bash remains the orchestration layer.** Active wrappers use argument arrays and strict mode. Experimental setup scripts still have permissive dependency paths. | A missed dependency failure can create an incomplete external environment. | **P0-15** |
| **The pipeline has no immutable result manifest.** It now uses one explicit PLY path and rejects an existing output directory. | A run lacks complete provenance and content-addressed restart state. | **P1-09** |
| **PLY writer comments are not sanitized for line breaks.** See AC-21. | Header forgery in written PLY files. | **P1-05** |
| **No signed release artifacts.** The evidence tool creates checksums and an unsigned provenance statement. No signed binary, notarized bundle, or attestation exists. | A downloaded artifact has no cryptographic authenticity proof. | **P0-18** |
| **Windows is not qualified.** `AtomicWriter` has a Win32 path, but no Windows build is tested in CI. | The output-integrity guarantees above are verified on macOS and Linux only. | **P0-03** |
| **The viewer has no worker isolation or shared core.** See AC-29. | A hostile asset degrades or crashes the browser tab. | **P1-11** |

---

## 9. Assumptions and non-goals

- **Melkor is not a sandbox.** It reduces the chance that a hostile asset compromises
  the process. It does not contain one that does. If the outcome matters, run
  conversions under OS-level confinement, with the least privilege your platform
  offers, in a directory that contains nothing else.
- **The operator is trusted.** There is no privilege separation between the person
  running the CLI and the person who owns the machine, and none is intended.
- **No multi-tenancy, no authentication, no secrets.** The viewer's server
  (`viewer/serve.js`) is a development server bound to loopback. It is not hardened
  for exposure to untrusted networks and must not be exposed to one.
- **Availability of external services is out of scope.** If Hugging Face is down,
  setup fails. This failure is not a vulnerability.
- **Upstream authenticity currently rests on git and TLS**, plus the commit-SHA and
  content-digest pins in the lock file for vendored source. It does not rest on
  signature verification, because no verification occurs.

---

## 10. Maintaining this document

This document changes with the code, in the same commit. When a blocker in §8 closes,
its row moves into §6 or §7 with the file that implements it named. A mitigation may be
described here only after it exists in the source tree. Each other item belongs in §8
with its blocker. A document that describes intentions can look like a description of
current behavior. Users can then rely on defenses that do not exist.
