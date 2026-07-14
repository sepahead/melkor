# Governance

This document describes how decisions are made in Melkor, who is allowed to make them, and
what has to be true before a release can be called supported. It describes the project as it
actually is today, including where the current arrangement is weak.

Melkor is maintained by one person. That is a real limitation, not a formality, and this
document says so plainly in [§3](#3-bus-factor-and-the-independent-review-requirement)
rather than describing a review process that does not exist.

## 1. Scope and product boundary

Governance decisions are bounded by what this project is. Melkor is an **asset
interoperability core**: it inspects, validates, normalizes, and converts 3D Gaussian-splat and
mesh assets, and it initializes degree-0 Gaussians from mesh geometry.

Melkor is **not** a reconstruction or training system. It contains no learned
scene-reconstruction model, and it does not vendor or redistribute one. Reconstruction,
training, depth estimation, and feedforward inference are separate programs with their own
licences, hardware requirements, and quality characteristics. Melkor reaches them through
**external adapters**: pinned manifests that describe how to obtain, verify, invoke, and
validate an external tool. An adapter describes a tool; it does not become that tool, and the
tool's behaviour is not Melkor's contract. See [`docs/adapters/index.md`](docs/adapters/index.md)
and the scope section of [`ROADMAP.md`](ROADMAP.md).

This boundary exists because a permissively licensed interoperability core cannot honestly ship
copyleft or research-only model code inside its own distribution, and because a project that
appears to natively provide every reconstruction method cannot support any of them. A proposal
that erodes the boundary — vendoring a trainer, adding a native learned model, downloading and
executing unreviewed model code — is out of scope regardless of its technical merit, and the
maintainer will decline it on scope grounds. Changing the boundary itself is a governance
change and follows [§6](#6-decision-process).

## 2. Roles

Roles are described by responsibility, not by seniority. One person may hold several roles;
today one person holds all of them. Current holders are listed in
[`MAINTAINERS.md`](MAINTAINERS.md).

### Contributor

Anyone who opens an issue, a discussion, or a pull request. Contributors need no permissions and
make no commitments. Contributions are accepted under the repository's licence
([`LICENSE`](LICENSE)) and are subject to [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).

### Reviewer

A reviewer has no write access. A reviewer reads pull requests in a declared area of competence
(for example, a compute backend, a file format, the viewer, or the packaging pipeline) and gives
a substantive verdict against the ten-lens checklist in the pull-request template. A reviewer's
approval is advisory: it cannot merge, but a maintainer is expected to state publicly why a
reviewer's objection was overridden.

The reviewer role exists so that competence in an area can be recognized before write access is
granted, and so that an area can gain independent scrutiny without waiting for a second
maintainer.

### Maintainer

A maintainer has write access to the repository. A maintainer may merge pull requests, apply
labels and milestones, cut branches, and change repository settings. A maintainer is expected
to:

- review changes against the ten-lens checklist and the correctness rules in
  [`CONTRIBUTING.md`](CONTRIBUTING.md);
- refuse changes that widen the public contract without documentation, tests, and a changelog
  entry;
- refuse changes that break backend parity (Metal, CUDA, CPU) by landing in only one backend;
- keep the blocker register in [`docs/audit/production-blockers.md`](docs/audit/production-blockers.md)
  honest, including by adding blockers that are inconvenient.

A maintainer may not merge their own change without an independent review **when that change is
in a release-critical area** (see [§7](#7-release-approval)). Today that rule is not satisfiable
internally, which is exactly the problem described in
[§3](#3-bus-factor-and-the-independent-review-requirement).

### Release manager

The release manager decides what goes into a release, tags it, produces the evidence bundle
(source archive, checksums, SBOM, provenance; see [`release/README.md`](release/README.md) and
[`docs/RELEASE.md`](docs/RELEASE.md)), and publishes the artifacts. The release manager is
accountable for the accuracy of the release notes, the support statement, and the platform
matrix — that is, for the claims a release makes, not only for the bits it ships.

The release manager is the only role permitted to publish under the project's package-index and
signing identities ([§9](#9-package-indexes-and-signing-identities)).

### Security responder

The security responder receives private vulnerability reports through GitHub private
vulnerability reporting, triages them against the threat model in [`SECURITY.md`](SECURITY.md),
coordinates a fix under embargo, requests a CVE where appropriate, and publishes the advisory.
The security responder may use the emergency exception in [§8](#8-emergency-security-exception).

The security responder is expected to acknowledge a report within a few days. With one person in
the role, that target is honest only while the person is available; a report that goes
unacknowledged for longer than two weeks should be escalated as described in
[`SECURITY.md`](SECURITY.md), and reporters remain free to disclose on their own timeline.

## 3. Bus factor and the independent review requirement

**Melkor has one maintainer. The bus factor is one.** If that person becomes unavailable, no
one else can merge a fix, publish a release, revoke a signing identity, or respond to a
vulnerability report. This is the project's largest governance risk and it is not mitigated by
anything in this document. It is stated here so that a user evaluating Melkor for a dependency
can price that risk correctly instead of inferring a team that does not exist.

Two consequences follow, and they are binding:

1. **Self-merge is the normal path today, and it is not represented as review.** A change
   authored and merged by the same person has been reviewed by one person. Project
   communication, release notes, and audit documents must not describe this as peer review, and
   must not describe a two-person review process as if it existed.

2. **No release may be declared production-supported until at least one independent external
   reviewer, who is not the maintainer, has reviewed the release candidate and recorded a
   verdict in public.** "Independent" means not the author of the change under review and not
   acting under the maintainer's direction. The review must cover, at minimum: the untrusted-input
   parsers, the resource limits, the release evidence and its verification, and the accuracy of
   the support and platform claims. The reviewer's verdict — including an unfavourable one — is
   linked from the release notes. This is a release blocker for `v2.0.0`. It is not waivable by
   the maintainer, because a rule that the sole maintainer can waive alone is not a control.

The project's stated preference is to reduce the bus factor by adding reviewers first
([§5](#5-adding-and-removing-role-holders)) rather than by granting write access quickly to
whoever appears. Write access to a project that parses untrusted input and publishes signed
artifacts is a security boundary, and it is not a reward for enthusiasm.

## 4. Ownership map

Ownership of a path means the owner must approve changes to it; it does not mean the owner wrote
it. The machine-readable form is [`.github/CODEOWNERS`](.github/CODEOWNERS), which is the
authority when the two disagree.

| Area | Why it is owned | Owner |
|---|---|---|
| CI/release workflows, `dependabot.yml` | Executes with repository credentials; a change here can exfiltrate secrets or forge an artifact | Maintainer |
| `SECURITY.md`, threat model | Defines what the project promises to treat as a vulnerability | Security responder |
| `third_party/`, dependency manifests, licence evidence | Changes the redistributable boundary and the licence obligations of every downstream user | Maintainer |
| Release tooling (`scripts/build_release_evidence.py`, `release/`, `tools/`) | Produces the evidence a consumer uses to decide whether to trust an artifact | Release manager |
| Untrusted-input parsers (`src/io/`, GLB/PLY/SPZ readers) | The primary attack surface | Maintainer |
| Governance files (this file, `MAINTAINERS.md`, `CODE_OF_CONDUCT.md`, `CODEOWNERS`) | Self-amendment must not be silent | Maintainer |

Every one of these currently resolves to the same person. That concentration is stated in
`.github/CODEOWNERS` deliberately: recording the real bottleneck is more useful than distributing
ownership across handles that do not exist.

## 5. Adding and removing role holders

**Adding a reviewer.** A maintainer nominates a candidate in a public issue, naming the area of
competence and citing the specific work that demonstrates it (reviews, reports, or merged
changes). The nomination stays open for at least seven days for public objection. The role is
recorded in `MAINTAINERS.md`.

**Adding a maintainer.** The candidate should normally have served as a reviewer and shown
sustained, competent judgement — including having said no to a change that should not have
landed. Nomination is public, open for at least fourteen days, and requires the agreement of the
existing maintainers. Write access is granted only after the candidate has read `SECURITY.md`,
this document, and the release process, and has agreed to the handling rules for signing material
in [§9](#9-package-indexes-and-signing-identities).

**Stepping down.** Any role holder may resign at any time by opening a pull request that moves
their entry to the *Emeritus* section of `MAINTAINERS.md`. No justification is required.

**Removal.** A role holder may be removed for a serious or repeated breach of the code of
conduct, for a breach of the security or signing rules, or for sustained unresponsiveness (see
[§10](#10-inactivity-and-succession)). Removal is proposed in public unless doing so would expose
an unfixed vulnerability or a personal-safety issue, in which case it is executed first and
explained afterwards with the sensitive detail withheld. Access is revoked at the moment removal
takes effect, not when the paperwork is finished.

**Removing the sole maintainer.** There is no internal mechanism for this, and pretending
otherwise would be dishonest. If the sole maintainer is the problem — including for conduct — the
remedies available to contributors are escalation to GitHub Support and forking. The licence
([`LICENSE`](LICENSE)) permits a fork, and the project would rather say so than leave contributors
with no route.

## 6. Decision process

Most decisions are made in the open on pull requests and issues, and most are uncontroversial.

**Lazy consensus.** A proposal that draws no objection within a reasonable period is accepted.
This is how routine work proceeds and it should stay that way.

**Design-first changes.** The following require a written proposal in an issue or a design note
under `docs/`, and explicit maintainer agreement, *before* implementation:

- any change to the public contract: the C ABI, the C++ SDK headers, the CLI surface and its
  machine-readable output, or the Python package surface;
- any change to a format profile's meaning, or the addition of a new profile ID;
- any change to a default that alters output bytes for an input that previously succeeded;
- any change to the resource limits or the untrusted-input threat model;
- any change to the product boundary in [§1](#1-scope-and-product-boundary);
- any change to this document.

The reason for the pre-implementation rule is that these changes are cheap to reject on paper and
expensive to reject after the code exists — and a maintainer who is looking at a finished
implementation is under pressure to accept a design they would not have chosen.

**Disagreement.** Technical disagreement is resolved by argument on the merits: correctness
evidence, tests, benchmarks, and specification citations beat seniority. If it cannot be
resolved, the maintainers decide, and the decision, with its reasoning, is recorded in the issue.
An objection that was overridden is not deleted.

**Conflict of interest.** A role holder with a material interest in the outcome (employment,
funding, or a competing product) says so on the thread before participating in the decision.

## 7. Release approval

A release is a claim about what other people can rely on, so the gate is deliberately stricter
than for a merge.

A version may be tagged and published only when all of the following hold:

1. the blocker register in `docs/audit/production-blockers.md` has no open blocker for that
   version;
2. CI is green on the exact commit being tagged, including the sanitizer build and the
   stub/CPU-topology configuration;
3. the evidence bundle builds and verifies from the tag (`scripts/build_release_evidence.py`),
   and the tag matches the version recorded in the build system;
4. the release notes, `SUPPORT.md`, and the platform matrix describe what was actually built and
   tested — a backend that is compiled but not hardware-qualified is labelled experimental, and a
   compile-only CI job is never presented as qualification;
5. the release manager approves.

For a release to be declared **production-supported** — that is, for `v2.0.0` and any later
stable release — one additional condition applies and cannot be waived internally: the
independent external review in [§3](#3-bus-factor-and-the-independent-review-requirement) has been
completed and its verdict is public.

Release candidates (`-rc.N`) may be published without the external review, provided they are
labelled as unsupported development artifacts, which is what `SUPPORT.md` already says.

## 8. Emergency security exception

The normal process is too slow for an actively exploited vulnerability, so there is one exception,
and it is bounded.

The security responder may, without prior review and without a public issue, merge and release a
fix for a vulnerability that is being exploited or whose public disclosure is imminent. The
exception covers **only** the minimal change that removes the vulnerability. It does not license
refactoring, feature work, or dependency bumps carried along in the same commit, and it does not
suspend the licence, provenance, or evidence requirements of a release.

Every use of the exception incurs these obligations:

- the fix is pushed to a branch and merged as a pull request (which may be merged immediately),
  so the diff is reviewable after the fact;
- a security advisory is published, with the affected versions and a workaround where one exists;
- the change is described in `CHANGELOG.md` in the ordinary way;
- within fourteen days of the advisory, the fix is reviewed by someone other than its author —
  today this means an external reviewer, because there is no second maintainer — and any follow-up
  hardening is tracked as a normal issue;
- the exception's use is recorded in the release notes, so that a user can see which changes did
  not receive normal review.

The exception exists to protect users, not to move faster. Using it for anything other than an
active security emergency is a governance breach and grounds for removal under
[§5](#5-adding-and-removing-role-holders).

## 9. Package indexes and signing identities

The project's identity on distribution channels is the thing an attacker most wants, because a
malicious artifact published under a trusted name compromises every user who does not check it.

**Ownership.** The GitHub organization `sepahead`, the repository, its release namespace, and any
package-index namespace the project publishes under (for example a PyPI project for the Python
package, and any future desktop or registry namespace) are project assets, not personal
conveniences. They are held by the maintainer and administered by the release manager. Publishing
under any of them by anyone other than the release manager is prohibited, including with the
maintainer's informal permission.

**Current state, stated honestly.** As documented in [`docs/RELEASE.md`](docs/RELEASE.md), the
project today builds **unsigned** developer and release-candidate artifacts. No production signing
identity, notarization credential, or keyless-attestation configuration is provisioned yet, and
the provenance statement the evidence bundle produces is deterministic but unsigned. Consequently
no current artifact proves publisher identity, and no current artifact should be trusted as if it
did. Provisioning and protecting those identities is a `v2.0.0` release blocker.

**Rules once identities exist.** They are recorded here in advance so that they constrain the
person who provisions them:

- Signing keys, certificates, provisioning profiles, and index tokens live in protected CI
  environments or hardware-backed stores, never in the repository, never in a build log, and never
  on a machine that also runs untrusted adapter code.
- Publishing runs from a protected workflow on a tag, from a reviewed configuration; a token is
  never used from a developer's shell for a production publish.
- Every published artifact is covered by checksums, an SBOM, and an attestation tied to the tag
  and commit, and the verification command is printed in the release notes.
- Compromise, or suspected compromise, of any signing identity or index token is handled as a
  security incident under [§8](#8-emergency-security-exception): revoke first, then yank or mark
  the affected artifacts, then disclose.
- A namespace is never transferred to an individual, and never handed to a third party, without a
  public issue and the succession process in [§10](#10-inactivity-and-succession).

**The bus factor applies here with full force.** A single person holds the credentials that would
be needed to revoke a compromised release. If that person is unavailable during an incident,
users cannot be protected by the project and must protect themselves by pinning and verifying
what they already have. Reducing this exposure — by provisioning keyless, tag-scoped attestation
rather than long-lived personal keys, and by recording recovery contacts — is part of the release
blocker above.

## 10. Inactivity and succession

Silence is a failure mode, and a project that does not plan for it strands its users.

**Inactivity.** A role holder who is unresponsive for **90 days** — no merges, no reviews, no
triage, no response to a direct ping on an issue — is moved to *Emeritus* in `MAINTAINERS.md`, and
their write access and any publishing credentials are revoked. This is not a punishment, and the
entry says so; access that nobody is watching is access an attacker can use. A returning role
holder is restored by the normal nomination process, which for someone with a track record is
usually immediate.

**If the sole maintainer becomes unavailable.** The following is the project's plan, and users
should read it as the honest limit of what this project can promise:

1. After **90 days** of no maintainer activity, any contributor may open a public issue titled
   *"Maintainer inactivity"*. It is the project's tripwire, and opening it is not a hostile act.
2. If there is no response within a further **30 days**, the project is to be treated as
   unmaintained. `SUPPORT.md` and `README.md` should be updated by any means available to say so,
   and users should assume that no security fix is coming. A fork is the appropriate response, and
   the licence permits it.
3. The maintainer's stated intent is that stewardship of the repository and its namespaces passes
   to a competent successor rather than lapsing. There is, today, **no named successor and no
   escrowed credential**, so this intent is not enforceable and must not be relied on. Naming a
   successor and recording a credential-recovery path is tracked as governance work; until it is
   done, the correct assumption is that an unavailable maintainer means an unrecoverable project.
4. Archival, if it happens, is done in the open: the repository is marked archived, the reason and
   the last audited commit are stated in the README, and existing artifacts are left downloadable
   with a warning rather than deleted, because deleting them would break provenance for anyone who
   already pinned or cited them.

## 11. Code of conduct

Participation is governed by [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md). Enforcement is the
maintainer's responsibility, with the single-maintainer limitation on enforcement stated in that
document — including the case where the report concerns the maintainer.

## 12. Amending this document

Amendments follow the design-first path in [§6](#6-decision-process): a public pull request, open
for at least fourteen days, with the rationale in the description. The one restriction is that the
independent-external-review requirement in [§3](#3-bus-factor-and-the-independent-review-requirement)
may not be removed or weakened by the sole maintainer acting alone; removing it requires the
project to have more than one maintainer, at which point the requirement it encodes has been met
by other means.
