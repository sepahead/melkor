# Maintainers

This file records who currently holds each role, what each role is accountable for, and how the
roles are filled. The rules that govern the roles — how they are granted, removed, and what they
may approve — are in [`GOVERNANCE.md`](GOVERNANCE.md).

Contact is through public project channels only: issues, pull requests,
[Discussions](https://github.com/sepahead/melkor/discussions), and — for anything sensitive —
[GitHub private vulnerability reporting](https://github.com/sepahead/melkor/security/advisories/new).
Do not add personal contact details. A maintainer's inbox is not a support channel or an audit
trail.

## Current maintainers

| Name | GitHub | Roles | Areas |
|---|---|---|---|
| Sepehr Mahmoudian | [@sepahead](https://github.com/sepahead) | Maintainer, Reviewer, Release manager, Security responder | All: core library, formats, C ABI, CLI, viewer, release evidence, adapters, and security |

**This is the complete list. There is one person on it.**

One person holds every role. Thus, every review is a self-review, and each author approves their
own release. The same person reads and fixes every private security report. Melkor's bus factor is
one. This condition is the project's most significant governance risk.

The consequences include an independent external review requirement for each
production-supported release. The complete requirements are in
[`GOVERNANCE.md` §3](GOVERNANCE.md#3-bus-factor-and-the-independent-review-requirement). Nothing in
this project's documentation, release notes, or audit records may describe its review process as if
a second person were involved.

## Emeritus

None. Nobody has held a role and stepped down.

## Reviewers

None.

A reviewer has no write access. A reviewer reads pull requests in a declared area and gives a
substantive verdict. Declared areas include file formats, the C ABI, the viewer, and the
packaging pipeline. Each review uses the twenty-lens checklist in the pull-request template.
This role gives the project independent scrutiny without repository write access.

**The project is actively looking for reviewers**, particularly in:

- **Untrusted-input parsing** — the GLB, PLY, and SPZ readers, the resource limits, and the
  sanitizer builds. This is the primary attack surface.
- **Format semantics** — whether each adapter preserves canonical values and reports every loss.
- **Packaging, provenance, and signing** — whether the release evidence proves what the release
  notes claim it proves.
- **Format conformance** — whether Melkor's reading and writing of a profile match the
  specification and the behavior of the tools that produce those files.

If you want to review, say so on an issue and start with a review. The project records the role
after the work. See [`GOVERNANCE.md` §5](GOVERNANCE.md#5-adding-and-removing-role-holders).

## What each role is accountable for

The full definitions are in [`GOVERNANCE.md` §2](GOVERNANCE.md#2-roles). In summary:

### Maintainer

Write access. Merges pull requests and owns the repository configuration. A maintainer is
accountable for:

- Enforcing the correctness rules in [`CONTRIBUTING.md`](CONTRIBUTING.md)
- Refusing public-contract changes without documentation, tests, and a changelog entry
- Refusing format changes that bypass canonical semantics or the loss policy
- Keeping
  [`docs/audit/production-blockers.md`](docs/audit/production-blockers.md)
  accurate, even when a blocker is inconvenient

### Reviewer

No write access. Reviewers are accountable for the quality and honesty of their verdict. They must
state, "I did not check that," for areas they did not read. A maintainer can override an objection
only in public and with a stated reason.

### Release manager

Decides release content, tags, produces and verifies the evidence bundle
([`release/README.md`](release/README.md), [`docs/RELEASE.md`](docs/RELEASE.md)), and publishes.
Accountable not only for the artifacts but for the **claims** made about them: the release notes,
the support statement in [`SUPPORT.md`](SUPPORT.md), and the platform matrix. A compile-only
platform is not qualified. The release manager is the only
role permitted to publish under the project's package-index and signing identities
([`GOVERNANCE.md` §9](GOVERNANCE.md#9-package-indexes-and-signing-identities)).

### Security responder

Receives private vulnerability reports, triages them against the threat model in
[`SECURITY.md`](SECURITY.md), coordinates fixes under embargo, and publishes advisories. May
invoke the emergency exception in
[`GOVERNANCE.md` §8](GOVERNANCE.md#8-emergency-security-exception). The responder must then get a
review and record the bypass in the release notes.

With one person in this role, the acknowledgment target in `SECURITY.md` holds only while that
person is available.
The detailed response targets and same-thread follow-up guidance are in `SECURITY.md`.
Reporters remain free to disclose on their own timeline.

## Ownership of code paths

Path ownership is declared in [`.github/CODEOWNERS`](.github/CODEOWNERS). This file is
authoritative for review requirements. Today, every entry resolves to @sepahead. This fact is more
accurate than invented handles. It also makes the bottleneck clear.

## Becoming a maintainer

See [`GOVERNANCE.md` §5](GOVERNANCE.md#5-adding-and-removing-role-holders). First, review in public
over time. Write access to this project is a security boundary. The project grants it for
demonstrated judgment, including the judgment to reject unsuitable changes. Contribution volume
alone is not sufficient.
