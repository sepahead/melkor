# Governance

This document describes Melkor's decision process and roles. It also defines the requirements for
a supported release. The document includes the weaknesses of the current project arrangement.

Melkor has one maintainer. This condition is a real limitation. [Section
3](#3-bus-factor-and-the-independent-review-requirement) explains it without
claiming a review process that does not exist.

## 1. Scope and product boundary

Governance decisions are bounded by this product definition.
Melkor is a Gaussian-splat asset interoperability core.
It inspects and converts supported PLY, SPZ, glTF, and GLB assets.
Each native operation validates input before it returns data or installs output.

Melkor is **not** a reconstruction or training system. It contains no learned
scene-reconstruction model, and it does not vendor or redistribute one.

Reconstruction,
training, depth estimation, and feedforward inference are separate programs with their own
licenses, hardware requirements, and quality characteristics.
Current shell wrappers can invoke user-supplied external programs for development work.
They do not provide a pinned production adapter contract.

The project plans manifest-based adapters that define how to obtain, verify, invoke, and
validate an external tool. An adapter describes a tool. It does not become that tool.
The external tool's behavior is not Melkor's native contract.
See [`docs/adapters/index.md`](docs/adapters/index.md) and the scope section of
[`ROADMAP.md`](ROADMAP.md).

This boundary keeps copyleft or research-only model code outside the permissively licensed core.
It also keeps the support scope manageable. The maintainer will decline a proposal that erodes
this boundary. Examples include vendored trainers, native learned models, and unreviewed model
downloads. Technical merit does not change this decision. A change to the boundary is a
governance change and follows [§6](#6-decision-process).

## 2. Roles

Roles are described by responsibility, not by seniority. One person can hold several roles.
Today, one person holds all roles. Current holders are listed in
[`MAINTAINERS.md`](MAINTAINERS.md).

### Contributor

Anyone who opens an issue, a discussion, or a pull request. Contributors need no permissions and
make no commitments. Contributions are accepted under the repository's license
([`LICENSE`](LICENSE)) and are subject to [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).

### Reviewer

A reviewer has no write access. A reviewer reads pull requests in a declared area of competence.
Examples include a file format, the viewer, the C ABI, and the packaging pipeline. The
reviewer gives a substantive verdict against the pull-request checklist.

A reviewer's approval is advisory and cannot merge a change. The project expects a maintainer to
explain publicly why the maintainer overrode a reviewer's objection.

The reviewer role recognizes competence before the project grants write access. It also provides
independent scrutiny without a second maintainer.

### Maintainer

A maintainer has write access to the repository. A maintainer may merge pull requests, apply
labels and milestones, cut branches, and change repository settings. A maintainer is expected
to:

- review changes against the twenty-lens checklist and the correctness rules in
  [`CONTRIBUTING.md`](CONTRIBUTING.md).
- refuse changes that widen the public contract without documentation, tests, and a changelog
  entry.
- refuse changes that bypass canonical semantics, format profiles, or loss reporting.
- keep the blocker register in [`docs/audit/production-blockers.md`](docs/audit/production-blockers.md)
  honest, including by adding blockers that are inconvenient.

A maintainer may not merge their own change without an independent review **when that change is
in a release-critical area** (see [§7](#7-release-approval)). Today that rule is not satisfiable
internally, which is exactly the problem described in
[§3](#3-bus-factor-and-the-independent-review-requirement).

### Release manager

The release manager decides the release content and creates its tag. The manager also produces
the evidence bundle and publishes the artifacts. The evidence includes a source archive,
checksums, an SBOM, and provenance. See [`release/README.md`](release/README.md) and
[`docs/RELEASE.md`](docs/RELEASE.md).

The release manager is accountable for the release notes, support statement, and platform matrix.
This responsibility covers all release claims, not only the artifacts.

The release manager is the only role permitted to publish under the project's package-index and
signing identities ([§9](#9-package-indexes-and-signing-identities)).

### Security responder

The security responder receives reports through GitHub private vulnerability reporting. The
responder triages them against the threat model in [`SECURITY.md`](SECURITY.md). The responder
coordinates an embargoed fix, requests a CVE when necessary, and publishes the advisory.
The security responder may use the emergency exception in [§8](#8-emergency-security-exception).

The response targets and same-thread follow-up guidance are in [`SECURITY.md`](SECURITY.md).
These targets apply only while the security responder is available.
Reporters remain free to disclose on their own timeline.

## 3. Bus factor and the independent review requirement

**Melkor has one maintainer. The bus factor is one.** If that person becomes unavailable, no one
else can merge a fix or publish a release. No one else can revoke a signing identity or respond
to a vulnerability report.

This condition is the project's largest governance risk. This
document does not mitigate it. Users can use this information to assess Melkor as a dependency.

Two consequences follow, and they are binding:

1. **Self-merge is the normal path today, and it is not represented as review.** A change
   authored and merged by the same person has been reviewed by one person. Project
   communication, release notes, and audit documents must not describe this as peer review. They
   must not claim that a two-person review process exists.

2. **No release may be declared production-supported until at least one independent external
   reviewer has reviewed the release candidate and recorded a public verdict.** The reviewer
   cannot be the maintainer. "Independent" means not the change author and not under the
   maintainer's direction. At a minimum, the review must cover:

   - The untrusted-input parsers
   - The resource limits
   - The release evidence and its verification
   - The accuracy of support and platform claims

   The release notes link to the reviewer's verdict, including an unfavorable verdict. This review
   is a `v2.0.0` release blocker. The maintainer cannot waive it. A control is ineffective when
   its only subject can waive it.

The project prefers to reduce the bus factor by adding reviewers first. See
[§5](#5-adding-and-removing-role-holders). It does not grant write access quickly to new
contributors. Write access to this project is a security boundary, not a reward.

## 4. Ownership map

Ownership of a path means that the owner must approve changes to it. It does not mean that the
owner wrote it. The machine-readable form is [`.github/CODEOWNERS`](.github/CODEOWNERS), which is the
authority when the two disagree.

| Area | Why it is owned | Owner |
|---|---|---|
| CI/release workflows, `dependabot.yml` | Executes with repository credentials. A change here can exfiltrate secrets or forge an artifact. | Maintainer |
| `SECURITY.md`, threat model | Defines what the project promises to treat as a vulnerability | Security responder |
| `third_party/`, dependency manifests, license evidence | Changes the redistributable boundary and the license obligations of every downstream user | Maintainer |
| Release tooling (`scripts/build_release_evidence.py`, `release/`, `tools/`) | Produces the evidence a consumer uses to decide whether to trust an artifact | Release manager |
| Untrusted-input parsers (`src/io/`, GLB/PLY/SPZ readers) | The primary attack surface | Maintainer |
| Governance files (this file, `MAINTAINERS.md`, `CODE_OF_CONDUCT.md`, `CODEOWNERS`) | Self-amendment must not be silent | Maintainer |

Every one of these currently resolves to the same person. That concentration is stated in
`.github/CODEOWNERS` deliberately: recording the real bottleneck is more useful than distributing
ownership across handles that do not exist.

## 5. Adding and removing role holders

**Adding a reviewer.** A maintainer nominates a candidate in a public issue. The issue names the
area of competence and cites supporting work. Reviews, reports, and merged changes are valid
evidence. The nomination stays open for public objection for at least seven days. The project
records the role in `MAINTAINERS.md`.

**Adding a maintainer.** Normally, the project selects a candidate who has served as a reviewer.
The candidate has shown sustained, competent judgment, including rejection of an unsuitable
change. The public nomination stays open for at least fourteen days. Existing maintainers must
agree to it. Before the project grants write access, the candidate must:

- Read `SECURITY.md`
- Read this document and the release process
- Accept the signing-material rules in
  [§9](#9-package-indexes-and-signing-identities)

**Stepping down.** Any role holder may resign at any time by opening a pull request that moves
their entry to the *Emeritus* section of `MAINTAINERS.md`. No justification is required.

**Removal.** The project can remove a role holder for:

- A serious or repeated breach of the code of conduct
- A breach of security or signing rules
- Sustained unresponsiveness under [§10](#10-inactivity-and-succession)

The project proposes removal in public. An unfixed vulnerability or personal-safety issue permits
private removal. In that case, the project explains the action afterward without sensitive
details. The project revokes access when removal takes effect.

**Removing the sole maintainer.** There is no internal mechanism for this, and pretending
otherwise would be dishonest. If the sole maintainer is the problem — including for conduct — the
remedies available to contributors are escalation to GitHub Support and forking. The license
([`LICENSE`](LICENSE)) permits a fork, and the project would rather say so than leave contributors
with no route.

## 6. Decision process

Most decisions are made in the open on pull requests and issues, and most are uncontroversial.

**Lazy consensus.** A proposal that draws no objection within a reasonable period is accepted.
This is how routine work proceeds. The project intends to continue this process.

**Design-first changes.** The changes below require a written proposal in an issue or a design note
under `docs/`, and explicit maintainer agreement, *before* implementation:

- any change to the public contract: the C ABI, CLI, JSON schemas, or profile identifiers.
- any change to a format profile's meaning, or the addition of a new profile ID.
- any change to a default that alters output bytes for an input that previously succeeded.
- any change to the resource limits or the untrusted-input threat model.
- any change to the product boundary in [§1](#1-scope-and-product-boundary).
- any change to this document.

The pre-implementation rule makes unsuitable proposals less costly to reject. A completed
implementation can pressure a maintainer to accept an unsuitable design.

**Disagreement.** Resolve technical disagreements with correctness evidence, tests, benchmarks,
and specification citations. Seniority does not override this evidence. If no agreement is
possible, the maintainers decide. They record the decision and its rationale in the issue.
An objection that was overridden is not deleted.

**Conflict of interest.** A role holder must disclose a material interest before participating in
a decision. Material interests include employment, funding, and competing products.

## 7. Release approval

A release is a claim about what other people can rely on, so the gate is deliberately stricter
than for a merge.

A version may be tagged and published only when all conditions below hold:

1. the blocker register in `docs/audit/production-blockers.md` has no open blocker for that
   version.
2. CI is green on the exact commit being tagged, including sanitizers and the no-SPZ build.
3. the evidence bundle builds and verifies from the tag (`scripts/build_release_evidence.py`),
   and the tag matches the version recorded in the build system.
4. the release notes and support matrix describe what was built and tested.
   A compile-only CI job is never presented as platform qualification.
5. the release manager approves.

Each **production-supported** release has one additional requirement. This requirement applies to
`v2.0.0` and later stable releases. The independent external review in
[§3](#3-bus-factor-and-the-independent-review-requirement) must be complete and public. The
project cannot waive this requirement internally.

Release candidates (`-rc.N`) may be published without the external review, provided they are
labeled as unsupported development artifacts, which is what `SUPPORT.md` already says.

## 8. Emergency security exception

The normal process is too slow for an actively exploited vulnerability, so there is one exception,
and it is bounded.

The security responder can merge and release an emergency fix without prior review or a public
issue. The vulnerability must be actively exploited or close to public disclosure. The exception
covers **only** the minimum change that removes the vulnerability. It excludes refactoring,
feature work, and unrelated dependency updates. It does not suspend license, provenance, or
release-evidence requirements.

Every use of the exception incurs these obligations:

- the fix is pushed to a branch and merged as a pull request (which may be merged immediately),
  so the diff is reviewable after the fact.
- a security advisory is published, with the affected versions and a workaround where one exists.
- the change is described in `CHANGELOG.md` in the ordinary way.
- within fourteen days of the advisory, the fix is reviewed by someone other than its author —
  today this means an external reviewer, because there is no second maintainer — and any follow-up
  hardening is tracked as a normal issue.
- the exception's use is recorded in the release notes, so that a user can see which changes did
  not receive normal review.

The exception exists to protect users, not to move faster. Using it for anything other than an
active security emergency is a governance breach and grounds for removal under
[§5](#5-adding-and-removing-role-holders).

## 9. Package indexes and signing identities

An attacker can target the project's identity on distribution channels. A malicious artifact
under a trusted name can compromise each user who does not verify it.

**Ownership.** These project assets are not personal conveniences:

- The GitHub organization `sepahead`
- The repository and its release namespace
- Each package-index, desktop, or registry namespace

The maintainer holds these assets, and the release manager administers them. Only the release
manager can publish under them. The maintainer's informal permission does not override this rule.

**Current state, stated honestly.** As documented in [`docs/RELEASE.md`](docs/RELEASE.md), the
project today builds **unsigned** developer and release-candidate artifacts. The project has no
production signing identity, notarization credential, or keyless-attestation configuration. The
evidence bundle produces deterministic but unsigned provenance.

Thus, no current artifact proves
publisher identity. Do not trust a current artifact as though it proves that identity.
Provisioning and protecting these identities is a `v2.0.0` release blocker.

**Rules once identities exist.** They are recorded here in advance so that they constrain the
person who provisions them:

- Signing keys, certificates, provisioning profiles, and index tokens live in protected CI
  environments or hardware-backed stores, never in the repository, never in a build log, and never
  on a machine that also runs untrusted adapter code.
- Publishing runs from a protected workflow on a tag and uses a reviewed configuration. Never
  use a developer-shell token for a production publish.
- Every published artifact is covered by checksums, an SBOM, and an attestation tied to the tag
  and commit, and the verification command is printed in the release notes.
- Treat a suspected or confirmed signing-identity compromise as a security incident under
  [§8](#8-emergency-security-exception). First, revoke the identity. Then, remove or mark the
  affected artifacts. Finally, disclose the incident.
- A namespace is never transferred to an individual, and never handed to a third party, without a
  public issue and the succession process in [§10](#10-inactivity-and-succession).

**The bus factor applies here with full force.** A single person holds the credentials that would
be needed to revoke a compromised release. If that person is unavailable during an incident,
the project cannot protect users. Users must pin and verify their existing artifacts. The release
blocker above requires keyless, tag-scoped attestation and recorded recovery contacts. This design
reduces reliance on long-lived personal keys.

## 10. Inactivity and succession

Silence is a failure mode, and a project that does not plan for it strands its users.

**Inactivity.** The project moves an unresponsive role holder to *Emeritus* in `MAINTAINERS.md`
after **90 days**. Inactivity means no merges, reviews, triage, or response to a direct issue
mention. The project also revokes the person's write access and publishing credentials.

This action is not a
punishment. Unmonitored access creates a security risk. A returning role holder uses the normal
nomination process.

**If the sole maintainer becomes unavailable.** The steps below are the project's plan. They also
define the limit of the project's commitments.

1. After **90 days** of no maintainer activity, any contributor may open a public issue titled
   *"Maintainer inactivity"*. It is the project's tripwire, and opening it is not a hostile act.
2. If there is no response within a further **30 days**, the project is to be treated as
   unmaintained. Available contributors should update `SUPPORT.md` and `README.md`. Users should
   assume that no security fix is coming. A fork is the appropriate response, and the license
   permits it.
3. The maintainer's stated intent is that stewardship of the repository and its namespaces passes
   to a competent successor rather than lapsing. There is, today, **no named successor and no
   escrowed credential**, so this intent is not enforceable and must not be relied on. Naming a
   successor and recording a credential-recovery path is tracked as governance work. Until it is
   done, assume that an unavailable maintainer makes the project unrecoverable.
4. Archival, if it happens, is done in the open: the repository is marked archived, the reason and
   the last audited commit are stated in the README. Existing artifacts stay downloadable with a
   warning. Deletion would break provenance for users who pinned or cited them.

## 11. Code of conduct

Participation is governed by [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md). Enforcement is the
maintainer's responsibility, with the single-maintainer limitation on enforcement stated in that
document — including the case where the report concerns the maintainer.

## 12. Amending this document

Amendments follow the design-first path in [§6](#6-decision-process). Use a public pull request
that stays open for at least fourteen days. Put the rationale in the description.

The sole maintainer cannot remove or weaken the independent external review requirement in
[§3](#3-bus-factor-and-the-independent-review-requirement). Removal requires more than one
maintainer. At that point, a different control satisfies the requirement.
