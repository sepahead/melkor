# Security policy

Melkor's core is an MIT-licensed C++ toolkit that inspects, converts, and normalises
3D Gaussian-splat assets: PLY, SPZ, and glTF/GLB. It parses files that come from other
people. That is what it is for, and it is why the project treats parser bugs and
resource exhaustion as security bugs rather than ordinary defects.

This document states what is in scope, how to report a vulnerability, and what the
project can honestly promise today. The reasoning behind the scope — assets, trust
boundaries, attackers, abuse cases, and the gaps that remain — is in
[`docs/security/threat-model.md`](docs/security/threat-model.md).

## Supported versions

| Version | Status |
| --- | --- |
| `2.0.x` | **Not yet released.** It will be the first supported release line. |
| `2.0.0-rc.*` | Pre-release. Fixes land in the next release candidate; there are no backports to an earlier RC. |
| `1.2.x` | End of life. It predates the v2 hardening work and will not receive security fixes. |
| `main` | Not a support target. |

**No production release of Melkor is currently supported.** The v2.0.0 hardening
programme is in progress, and the honest position is that the project has nothing it is
prepared to call a supported, security-maintained release. Please report vulnerabilities
anyway: fixes land on the 2.0 development line and will ship in the first `2.0.x`
release.

When `2.0.0` ships, support attaches to the **immutable release line**, not to a branch.
Security fixes will be published as a new patch tag on the `2.0.x` line — an immutable
git tag with matching release evidence. The previous version of this policy told users
to track `main`. That was wrong: a moving branch cannot be audited, cannot be pinned,
and gives a user no way to state which code they are running. Do not track `main` for
anything you care about.

## Report a vulnerability privately

Report through **[GitHub Security Advisories](https://github.com/sepahead/melkor/security/advisories/new)**
("Report a vulnerability") on this repository. This is the only channel the project
monitors for security reports.

Please do **not** open a public issue, a pull request, or a discussion for anything that
looks exploitable, and please do not disclose it publicly before a fix is available.

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

Melkor is maintained by one person. The following are **best-effort targets for an open
source project, not a service-level agreement**, and no commercial support contract is
implied by them:

- **Acknowledgement within 3 business days** that the report has been received and read.
- **A triage update within 7 calendar days**: whether the issue is confirmed, what the
  assessed severity is, and what happens next.

After triage, the project will keep you informed as a fix is developed, agree
disclosure timing with you, and credit you in the advisory and the changelog unless you
ask not to be. There is no bug bounty and no monetary reward.

If you do not hear back within those windows, the report has not been ignored on
purpose — please follow up on the same advisory thread.

## Scope and threat model

Melkor parses untrusted input by design. The following are treated as security bugs,
not ordinary crashes:

**Memory safety in anything reachable from a file.** Out-of-bounds reads and writes,
use-after-free, wild pointers, integer overflow that reaches an allocation, and
undefined behaviour in the PLY, SPZ, GLB, or glTF readers, in the inspection path, in
image decoding, or in any code a crafted asset can reach.

**Resource exhaustion — this is now in scope.** Melkor previously excluded denial of
service from "absurdly large but otherwise well-formed input". That exclusion has been
reversed; reversing it is release blocker **P0-12**. A file that is perfectly valid and
simply enormous will still exhaust a machine's memory, fill its disk, or wedge a CI
runner, and "it parsed correctly" is no consolation. **Unbounded memory, disk, or time
consumption driven by input — whether the input is large and well-formed, or small and
maliciously crafted — is in scope and will be treated as a security bug.** This
explicitly includes compression bombs, declared counts and dimensions that are
internally consistent but ruinous, and unbounded recursion.

**Escaping the named inputs and outputs.** A crafted asset causing Melkor to read a
file the user did not name (for example through a glTF external URI), or to write
anywhere other than the destination the user gave it (for example through a symlink at
the output path).

**Destroying data.** Any way to make a failed or interrupted operation damage,
truncate, or delete a file that existed before it started.

**Injection.** Command or path injection through the pipeline and setup scripts'
handling of user-supplied paths, filenames, and arguments; terminal-escape or log
injection through metadata taken from an asset and echoed into diagnostics or written
into an output header.

**Supply chain.** Tampering with vendored third-party sources, or with the toolchains,
Python packages, and model weights that the setup scripts fetch. The gap here is real
and documented: several setup scripts still clone and pull mutable branches with no
digest pinning (**P0-13**).

**The viewer.** Escaping the static file server's root directory; anything a dropped
asset can do to the browser session beyond rendering incorrectly.

**Third-party and vendored code** is in scope when Melkor's supported CLI, viewer,
installer, or runtime exposes it. Please report it upstream as well, but a private
report here lets the vendored copy be patched or quarantined.

### Known gaps you should know about before relying on this

The threat model is candid about what is not yet done, and you should read
[`docs/security/threat-model.md` §8](docs/security/threat-model.md) before deploying
Melkor against hostile input. In summary:

- The limit and budget machinery exists and is tested, but **the readers have not yet
  been migrated onto `OperationContext`**, so those limits are not yet enforced on the
  parsing path (P0-12, in progress).
- **There is no coverage-guided fuzzing.** The two files named `*_fuzz.cpp` are bounded
  randomised unit loops over valid data, not libFuzzer targets. There is no corpus and
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
  path can reach. Send them anyway if you are unsure; the maintainer will decide.

Denial of service is **not** on this list, and that is deliberate. See above.

## Security design

The controls below exist in the source tree today. Each is here because of a specific
failure it prevents.

- **`include/melkor/error.hpp` — `Result<T>` and stable diagnostics.** No exceptions, no
  bare `bool` plus a prose string. A caller must be able to distinguish malformed input
  from a resource limit from a cancellation, because those demand different responses;
  `ErrorCode` maps one-to-one onto documented CLI exit codes, and `Diagnostic::code` is
  a stable machine contract, so a script never has to grep English.
- **`include/melkor/checked.hpp` — checked arithmetic.** A count multiplied by a stride,
  an offset added to a length, a 64-bit file-declared size narrowed to `size_t`: each is
  an integer overflow, and an overflow in that position is a heap overflow, because the
  allocation ends up smaller than the loop that fills it. Numbers that come from a file
  must reach an allocation only through these functions. *Note: the substrate is landed
  and tested; the readers are not yet migrated onto it (P0-12).*
- **`include/melkor/limits.hpp` and `include/melkor/budget.hpp` — limits and budgets.**
  Named `web`, `desktop`, and `server` profiles, because the correct limit genuinely
  differs by context; a thread-safe `Budget` that must be charged *before* an allocation,
  since accounting for memory you already allocated does not prevent the OOM; hard
  ceilings no custom profile may exceed; a decompression-ratio guard that refuses a bomb
  by its *shape* before inflating any of it, because an absolute cap alone still lets a
  1 KiB file expand to the whole cap; and cooperative cancellation checked inside long
  loops rather than once per file. **There is deliberately no "disable all limits"
  switch:** a profile may raise a limit, but checked arithmetic, structural limits, path
  containment, and output integrity stay on regardless.
- **`include/melkor/io/atomic_writer.hpp` — atomic output.** A failed write must never
  damage the file that was already there. Output is written to an unpredictably-named
  `O_EXCL` temporary in the *same directory* (a `/tmp` temporary would make `rename()`
  non-atomic across filesystems), then atomically installed. The destination is never
  opened until commit, so any failure leaves it exactly as it was. This fixes release
  blocker **P0-08**, where a failed SPZ encode truncated and then deleted the user's
  existing file.
- **Path containment.** glTF external URIs are canonicalised and then required to stay
  within the input asset's directory (`src/safe_gltf_fs.hpp`); the viewer's server
  resolves before it checks containment, because checking a raw URL prefix first lets an
  encoded slash walk out.
- **Output hygiene.** Metadata taken from an asset is escaped before it reaches a
  terminal, including C1 control characters encoded as UTF-8 (`src/safe_text.hpp`), and
  diagnostics print a basename by default so that a report pasted into a public issue
  does not leak a home directory.
- **Dependency pinning.** `third_party/manifest.lock.json` pins every vendored
  dependency by upstream commit SHA and by a content digest over the files that actually
  get compiled; local patches must be declared under `third_party/patches/` with a
  rationale and a digest, because an undocumented fork is indistinguishable from a
  supply-chain compromise. CI enforces this with `tools/verify_third_party.py --check`.
- **CI.** Every push builds with `-Werror` and runs the test suite under AddressSanitizer
  and UndefinedBehaviorSanitizer. Run a sanitizer build locally before touching parser
  code.

### Advice for integrators

Treat every splat or mesh file from an untrusted source as attacker-controlled. Melkor
reduces the chance that such a file compromises the process; it does not contain one
that does. Run conversions under the least privilege your platform offers, in a
directory that contains nothing else you care about, and — until P0-12 closes — with an
external memory and time limit imposed by the OS.

## Verify official releases

Releases are published as immutable git tags with release evidence built by
`scripts/build_release_evidence.py`. That evidence contains a `SHA256SUMS` manifest over
every file in the source release and an SLSA-style provenance statement.

**The evidence is unsigned, and it is not by itself an authenticity proof.** The script
says so in its own header. There are currently no signed binaries, no notarised desktop
bundles, and no cryptographic attestations; producing them is release blocker **P0-18**.
Until that closes, verification means:

1. Fetch the release by its **tag**, never by a branch, and record the commit SHA.
2. Check the source tree against the published `SHA256SUMS`.
3. Run `python3 tools/verify_third_party.py --check` to confirm that the vendored
   third-party sources match the pinned revisions and content digests in
   `third_party/manifest.lock.json`.

When signing and attestation land, this section will describe how to verify a signature,
and will say which key.

## Safe harbour

The project's intent is to treat good-faith security research as a contribution and not
as an attack, and to work with reporters rather than against them. A formal safe-harbour
statement — one that actually tells you what you are authorised to do and what
protections you have — carries legal consequences, and it will be **published here only
after legal review**.

Until that review is complete, **this document does not grant a safe harbour**, and no
paragraph in it should be read as legal authorisation for any particular activity. What
the project can commit to in the meantime is straightforward: a report made privately
through the advisory channel above, against your own copy of Melkor and your own data,
will be received in good faith, and the maintainer will not seek to have you penalised
for making it. If you need more certainty than that before you begin, please ask first
through the advisory channel, and wait for an answer.
