# Security policy

Melkor's core is an MIT-licensed C++ toolkit that inspects, converts, and normalizes
3D Gaussian-splat assets: PLY, SPZ, and glTF/GLB. It parses files that come from other
people. That is what it is for, and it is why the project treats parser bugs and
resource exhaustion as security bugs rather than ordinary defects.

This document states what is in scope, how to report a vulnerability, and what the
project can honestly promise today. The threat model explains the scope, trust
boundaries, attackers, abuse cases, and remaining gaps. See
[`docs/security/threat-model.md`](docs/security/threat-model.md).

## Supported versions

| Version | Status |
| --- | --- |
| `2.0.x` | **Not yet released.** It will be the first supported release line. |
| `2.0.0-rc.*` | Pre-release. Fixes land in the next release candidate. There are no backports to an earlier RC. |
| `1.2.x` | End of life. It predates the v2 hardening work and will not receive security fixes. |
| `main` | Not a support target. |

**No production release of Melkor is currently supported.** The v2.0.0 hardening
program is in progress. The project has no release that it can call supported and
security-maintained. Report vulnerabilities during this work. Fixes land on the 2.0
development line and will ship in the first `2.0.x` release.

When `2.0.0` ships, support attaches to the **immutable release line**, not to a branch.
Security fixes will use a new patch tag on the `2.0.x` line. Each immutable Git tag has
matching release evidence.

The previous policy told users to track `main`. That advice
was incorrect. A moving branch cannot be audited or pinned. It also does not identify
the code that a user runs. Do not track `main` for important work.

## Report a vulnerability privately

Report through **[GitHub Security Advisories](https://github.com/sepahead/melkor/security/advisories/new)**
("Report a vulnerability") on this repository. This is the only channel the project
monitors for security reports.

Do **not** open a public issue, pull request, or discussion for a possible exploit.
Do not disclose it publicly before a fix is available.

## What to include

The more of this you can provide, the faster the report can be triaged:

- **A minimal reproducing input.** For a parser bug this matters more than anything
  else. Attach the smallest file that triggers it, not the 2 GB scene it came from.
- **The exact command** you ran, including flags.
- **Platform and build**: OS and version, compiler, backend. `melkor --info` prints
  most of this.
- **The version or commit** you tested. If you built from source, the commit SHA.
- **What you believe the impact is** — memory corruption, file destruction, reading a
  file outside the named inputs, resource exhaustion — and why.
- **A stack trace under sanitizers if you have one.** An AddressSanitizer or
  UndefinedBehaviorSanitizer report turns a suspicion into a diagnosis. Configure with
  `-fsanitize=address,undefined`.

If a report contains a crash you are unsure about, send it anyway. Deciding whether a
crash is exploitable is the maintainer's job, not the reporter's.

## Response process

Melkor has one maintainer. The targets below are **best-effort open-source targets, not
a service-level agreement**. They do not imply a commercial support contract.

- **Acknowledgment within 3 business days** that the report has been received and read.
- **A triage update within 7 calendar days**: whether the issue is confirmed, what the
  assessed severity is, and what happens next.

After triage, the project will give you fix updates. It will agree on disclosure timing
with you. The advisory and changelog will credit you unless you decline. There is no
bug bounty or monetary reward.

If you do not hear back within those windows, the report has not been ignored on
purpose — please follow up on the same advisory thread.

## Scope and threat model

Melkor parses untrusted input by design. The items below are security bugs,
not ordinary crashes:

**Memory safety in anything reachable from a file.** This category includes:

- Out-of-bounds reads and writes
- Use-after-free defects and wild pointers
- Integer overflow that reaches an allocation
- Undefined behavior in readers, inspection, or image decoding
- Undefined behavior in any code that a crafted asset can reach

**Resource exhaustion — this is now in scope.** Melkor previously excluded denial of
service from "absurdly large but otherwise well-formed input". That exclusion has been
reversed. This change is release blocker **P0-12**. A valid, very large file can exhaust
memory, fill a disk, or stop a CI runner. Successful parsing does not make this result
safe.

**Input-driven unbounded memory, disk, or time use is in scope as a security
bug.** This rule applies to large valid input and small malicious input. It includes
compression bombs, harmful declared sizes, and unbounded recursion.

**Escaping the named inputs and outputs.** This category includes a crafted asset that
causes Melkor to read an unnamed file. A glTF external URI is one example. It also
includes a write outside the specified destination. An output-path symlink is one
example.

**Destroying data.** Any way to make a failed or interrupted operation damage,
truncate, or delete a file that existed before it started.

**Injection.** This category includes command or path injection through user-supplied
paths, filenames, and arguments. It also includes terminal or log injection through
asset metadata in diagnostics or output headers.

**Supply chain.** Tampering with vendored third-party sources, or with the toolchains,
Python packages, and model weights that the setup scripts fetch. The gap here is real
and documented: several setup scripts still clone and pull mutable branches with no
digest pinning (**P0-13**).

**The viewer.** This category includes escape from the static file server's root
directory. It also includes any effect beyond incorrect rendering that a dropped asset
has on the browser session.

**Third-party and vendored code** is in scope when Melkor's supported CLI, viewer,
installer, or runtime exposes it. Please report it upstream as well, but a private
report here lets the vendored copy be patched or quarantined.

### Known gaps to review before use

The threat model explains the incomplete work. Read
[`docs/security/threat-model.md` §8](docs/security/threat-model.md) before deploying
Melkor against hostile input. In summary:

- The limit and budget machinery exists and is tested, but **the readers have not yet
  been migrated onto `OperationContext`**, so those limits are not yet enforced on the
  parsing path (P0-12, in progress).
- **There is no coverage-guided fuzzing.** The two files named `*_fuzz.cpp` are bounded
  randomized unit loops over valid data, not libFuzzer targets. There is no corpus and
  no continuous fuzzing (P1-12). The parsers' robustness against hostile input is
  therefore not empirically validated.
- Several setup scripts fetch mutable branches without pinning (P0-13).

## Generally out of scope

Narrowly, and with reasons:

- **Bugs that require the operator to act against their own interest.** Melkor does not
  defend the machine against the person running it. Raising your own limits, passing
  `--allow-output-symlink`, or pointing the tool at your own files is authority, not a
  vulnerability.
- **Exposing the viewer's development server (`viewer/serve.js`) to an untrusted
  network.** It binds `127.0.0.1`, serves a fixed allowlist, and is a development
  server. Deploying it as a public web server is a misconfiguration, not a bug.
- **Unavailability of third-party services.** If a repository or model host is down or
  removes an artifact, setup fails. That is not a vulnerability.
- **Reports produced solely by an automated scanner** with no demonstrated impact on
  Melkor, and dependency advisories for code paths Melkor does not compile or call.
  Show how it is reachable.
- **Missing hardening that is already tracked and disclosed above.** A report that the
  project has no fuzzing (P1-12) or that a setup script pulls a branch (P0-13) tells us
  what we have already written down. A report that demonstrates *exploitation* of one of
  those gaps is very much in scope and welcome.
- **Crashes in code explicitly marked retired or experimental** that no supported CLI
  path can reach. Send them if you are not sure. The maintainer will decide.

Denial of service is **not** on this list, and that is deliberate. See above.

## Security design

The controls below exist in the source tree today. Each is here because of a specific
failure it prevents.

- **`include/melkor/error.hpp` — `Result<T>` and stable diagnostics.** No exceptions, no
  bare `bool` plus a prose string. A caller must distinguish malformed input, resource
  limits, and cancellation. Each condition needs a different response. `ErrorCode` maps
  one-to-one to documented CLI exit codes. `Diagnostic::code` is a stable machine
  contract, so a script never has to search English text.
- **`include/melkor/checked.hpp` — checked arithmetic.** A count multiplied by a stride,
  an offset added to a length, or a 64-bit file size narrowed to `size_t` can overflow. This
  overflow can cause a heap overflow. The allocation becomes smaller than the loop that
  fills it. File-supplied numbers must reach allocations only through these functions.
  *Note: The substrate exists and has tests. The readers do not use it yet (P0-12).*
- **`include/melkor/limits.hpp` and `include/melkor/budget.hpp` — limits and budgets.**
  The named `web`, `desktop`, and `server` profiles give each context suitable limits.
  A thread-safe `Budget` charges memory *before* allocation. Accounting after allocation
  cannot prevent an out-of-memory failure. Hard ceilings restrict each custom profile.

  A ratio guard rejects a compression bomb before inflation. Without this guard, a 1 KiB file can
  expand to the absolute cap. Cooperative cancellation checks run inside long loops.

  **There is deliberately no "disable all limits" switch.** A profile can raise a
  limit. Checked arithmetic, structural limits, path containment, and output integrity
  always stay active.
- **`include/melkor/io/atomic_writer.hpp` — atomic output.** A failed write must never
  damage the file that was already there. Output is written to an unpredictably-named
  `O_EXCL` temporary in the *same directory* (a `/tmp` temporary would make `rename()`
  non-atomic across filesystems), then atomically installed. The destination is never
  opened until commit, so any failure leaves it exactly as it was. This fixes release
  blocker **P0-08**, where a failed SPZ encode truncated and then deleted the user's
  existing file.
- **Path containment.** glTF external URIs are canonicalized and then required to stay
  within the input asset's directory (`src/safe_gltf_fs.hpp`). The viewer's server
  resolves a path before it checks containment. Checking a raw URL prefix first lets an
  encoded slash escape.
- **Output hygiene.** Metadata taken from an asset is escaped before it reaches a
  terminal. This rule includes C1 control characters encoded as UTF-8
  (`src/safe_text.hpp`). Diagnostics print a basename by default. Thus, a public report
  does not expose a home directory.
- **Dependency pinning.** `third_party/manifest.lock.json` pins every vendored
  dependency by upstream commit SHA and by a content digest over the files that actually
  get compiled. Declare each local patch under `third_party/patches/` with a rationale
  and digest. An undocumented fork is indistinguishable from a supply-chain compromise.
  CI enforces this rule with `tools/verify_third_party.py --check`.
- **CI.** Every push builds with `-Werror` and runs the test suite under AddressSanitizer
  and UndefinedBehaviorSanitizer. Run a sanitizer build locally before touching parser
  code.

### Advice for integrators

Treat every splat or mesh file from an untrusted source as attacker-controlled. Melkor
reduces the chance that such a file compromises the process. It does not isolate a
compromised process. Run conversions with the least available privilege. Use a
directory that contains no important files. Until P0-12 closes, use OS memory and time
limits.

## Verify official releases

Releases are published as immutable git tags with release evidence built by
`scripts/build_release_evidence.py`. That evidence contains a `SHA256SUMS` manifest over
every file in the source release and an SLSA-style provenance statement.

**The evidence is unsigned, and it is not by itself an authenticity proof.** The script
says so in its own header. There are currently no signed binaries, notarized desktop
bundles, or cryptographic attestations. Producing them is release blocker **P0-18**.
Until that closes, verification means:

1. Fetch the release by its **tag**, never by a branch, and record the commit SHA.
2. Check the source tree against the published `SHA256SUMS`.
3. Run `python3 tools/verify_third_party.py --check` to confirm that the vendored
   third-party sources match the pinned revisions and content digests in
   `third_party/manifest.lock.json`.

When signing and attestation land, this section will describe how to verify a signature,
and will say which key.

## Safe harbor

The project's intent is to treat good-faith security research as a contribution and not
as an attack. The project also intends to work with reporters. A formal safe-harbor
statement defines permitted work and protections. Such a statement has legal
consequences. The project will publish one here **only after legal review**.

Until that review is complete, **this document does not grant a safe harbor**. No
paragraph grants authorization for an activity. The project makes one commitment in
the meantime.

The project will receive in good faith a private report about your own Melkor copy and
data. The maintainer will not seek a penalty for the report. If you need more
certainty, ask through the advisory channel before you start. Wait for an answer.
