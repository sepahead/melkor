# Maintainers

This file records who currently holds each role, what each role is accountable for, and how the
roles are filled. The rules that govern the roles — how they are granted, removed, and what they
may approve — are in [`GOVERNANCE.md`](GOVERNANCE.md).

Contact is through public project channels only: issues, pull requests,
[Discussions](https://github.com/sepahead/melkor/discussions), and — for anything sensitive —
[GitHub private vulnerability reporting](https://github.com/sepahead/melkor/security/advisories/new).
No personal contact details are published here, and none should be added; a maintainer's inbox is
not a support channel and is not an audit trail.

## Current maintainers

| Name | GitHub | Roles | Areas |
|---|---|---|---|
| Sepehr Mahmoudian | [@sepahead](https://github.com/sepahead) | Maintainer, Reviewer, Release manager, Security responder | All: core library, compute backends (Metal / CUDA / CPU), format readers and writers, CLI, viewer, packaging and release evidence, adapters, security |

**This is the complete list. There is one person on it.**

One person holds every role, so every review is a self-review, every release is approved by its
own author, and every private security report is read by the same individual who would have to fix
it. Melkor's bus factor is one. This is the project's most significant governance risk, and its
consequences — including the requirement for an independent external reviewer before any release
may be called production-supported — are set out in
[`GOVERNANCE.md` §3](GOVERNANCE.md#3-bus-factor-and-the-independent-review-requirement). Nothing in
this project's documentation, release notes, or audit records may describe its review process as if
a second person were involved.

## Emeritus

None. Nobody has held a role and stepped down.

## Reviewers

None.

A reviewer has no write access. A reviewer reads pull requests in a declared area — a compute
backend, a file format, the viewer, the packaging pipeline — and gives a substantive verdict
against the ten-lens checklist in the pull-request template. The role exists because independent
scrutiny is the thing this project is missing, and it can be supplied without granting write
access to a codebase that parses untrusted input and publishes distributable artifacts.

**The project is actively looking for reviewers**, particularly in:

- **Untrusted-input parsing** — the GLB, PLY, and SPZ readers, the resource limits, and the
  sanitizer builds. This is the primary attack surface.
- **Compute-backend parity** — whether the Metal, CUDA, and CPU implementations of an operation
  really agree, on hardware the maintainer does not own (CUDA in particular is compiled in CI but
  not hardware-qualified).
- **Packaging, provenance, and signing** — whether the release evidence proves what the release
  notes claim it proves.
- **Format conformance** — whether Melkor's reading and writing of a profile match the
  specification and the behaviour of the tools that actually produce those files.

If you want to review, say so on an issue and start by reviewing something. The role is recorded
after the work, not before it; see [`GOVERNANCE.md` §5](GOVERNANCE.md#5-adding-and-removing-role-holders).

## What each role is accountable for

The full definitions are in [`GOVERNANCE.md` §2](GOVERNANCE.md#2-roles). In summary:

### Maintainer

Write access. Merges pull requests and owns the repository configuration. Accountable for:
enforcing the correctness rules in [`CONTRIBUTING.md`](CONTRIBUTING.md); refusing changes that
widen the public contract without documentation, tests, and a changelog entry; refusing
backend-semantic changes that land in only one of Metal, CUDA, and CPU; and keeping
[`docs/audit/production-blockers.md`](docs/audit/production-blockers.md) honest, including when a
blocker is inconvenient to admit.

### Reviewer

No write access. Accountable for the quality and honesty of their verdict, and for saying "I did
not check that" rather than approving an area they did not read. A reviewer's objection may be
overridden by a maintainer, but only in public and with a stated reason.

### Release manager

Decides release content, tags, produces and verifies the evidence bundle
([`release/README.md`](release/README.md), [`docs/RELEASE.md`](docs/RELEASE.md)), and publishes.
Accountable not only for the artifacts but for the **claims** made about them: the release notes,
the support statement in [`SUPPORT.md`](SUPPORT.md), and the platform matrix. A backend that is
compiled but not hardware-qualified must be labelled experimental. The release manager is the only
role permitted to publish under the project's package-index and signing identities
([`GOVERNANCE.md` §9](GOVERNANCE.md#9-package-indexes-and-signing-identities)).

### Security responder

Receives private vulnerability reports, triages them against the threat model in
[`SECURITY.md`](SECURITY.md), coordinates fixes under embargo, and publishes advisories. May
invoke the emergency exception in
[`GOVERNANCE.md` §8](GOVERNANCE.md#8-emergency-security-exception), and is then obliged to get the
change reviewed after the fact and to record in the release notes that it bypassed normal review.

With one person in this role, the acknowledgement target in `SECURITY.md` holds only while that
person is available. Reporters should escalate an unacknowledged report as `SECURITY.md` describes,
and remain free to disclose on their own timeline.

## Ownership of code paths

Path ownership is declared in [`.github/CODEOWNERS`](.github/CODEOWNERS), which is authoritative
for review requirements. Today every entry in it resolves to @sepahead. That is recorded as a fact
rather than spread across invented handles, because a CODEOWNERS file that names people who do not
exist is worse than one that admits a bottleneck.

## Becoming a maintainer

See [`GOVERNANCE.md` §5](GOVERNANCE.md#5-adding-and-removing-role-holders). In short: review first,
in public, over time. Write access to a project that parses untrusted input and signs distributable
artifacts is a security boundary, and it is granted on demonstrated judgement — including the
judgement to reject a change that should not land — not on volume of contribution.
