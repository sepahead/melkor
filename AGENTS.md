# Melkor agent contract

Melkor is a bounded C++17 toolkit that inspects, converts, and locally views Gaussian-splat assets.
This file is the operating contract for maintainers and coding agents.
[README.md](README.md) owns the scope and format contract.
[CONTRIBUTING.md](CONTRIBUTING.md) owns the architecture rules and the pull request checklist.
[GOVERNANCE.md](GOVERNANCE.md) owns roles, decisions, and release approval.

## Authority and workflow

The owner authorizes agents to commit, push, and merge to `main`.
`main` has no branch protection. The local gate is the only check before a push.
Sign every commit. The repository configuration signs with the owner's SSH key.
Do not add AI attribution or co-author trailers.
Preserve unrelated work and another contributor's active scope.

Self-merge is the normal path, and it is not peer review.
Do not describe agent or maintainer review as peer, independent, or two-person review.
[GOVERNANCE.md section 3](GOVERNANCE.md#3-bus-factor-and-the-independent-review-requirement) makes this rule binding.

A design-first change needs a written proposal and explicit maintainer agreement before implementation.
[GOVERNANCE.md section 6](GOVERNANCE.md#6-decision-process) lists these changes.
Examples are the C ABI, CLI, JSON schemas, profile identifiers, output-byte defaults, and resource limits.
Put the proposal in an issue or in a design note under `docs/`.
An explicit owner instruction is the maintainer agreement. Record it in the proposal.

Releases, tags, package-index publication, signing identities, and repository settings are release-manager actions.
Do not do them without an explicit owner instruction for that action.

## Read before changing

Read the documents that own the changed surface:

| Surface | Owning documents |
|---|---|
| Native core, formats, and loss | [Contributing](CONTRIBUTING.md), [Canonical semantics](docs/reference/canonical-semantics.md), [Loss policy](docs/reference/loss-policy.md) |
| CLI and inspection | [CLI reference](docs/CLI.md), [Asset inspection](docs/INSPECT.md) |
| C SDK | [README](README.md#c-sdk), the public C ABI header under `include/melkor/` |
| Hostile input and resources | [Resource limits](docs/reference/resource-limits.md), [Threat model](docs/security/threat-model.md) |
| Viewer | [Viewer guide](viewer/README.md), [Asset provenance](viewer/ASSET_PROVENANCE.md) |
| Release and evidence | [Release process](docs/RELEASE.md), [Release metadata](release/README.md), [Governance](GOVERNANCE.md) |
| Removed v1 surfaces | [Migration guide](docs/migrations/v1-to-v2.md) |
| Current development state | [Production blockers](docs/audit/production-blockers.md), [v2 progress](docs/audit/v2-progress.md) |

## Product boundaries

- Treat every asset byte and count as untrusted.
- Do not add a fallback that changes data semantics.
- Report each representational loss before output. Require the exact approval code for each severe loss.
- Route file output through `AtomicWriter`.
- Keep the installed surface limited to the stable C ABI and version constants.
- Keep the native core free of renderers, GPU backends, trainers, learned models, and network loaders.
- Keep trainer code, model weights, and their licenses outside the MIT core.

The full rule set is in [CONTRIBUTING.md](CONTRIBUTING.md#architecture-rules).

Melkor `2.0.0-dev` has no supported production release.
Do not claim production support, a signed binary, a package, or a desktop application.
A production-support claim needs the independent external review in GOVERNANCE.md section 3.

## Artifact classes

Classify every artifact before you change it.

- **Generated notices.** `python3 tools/generate_notices.py --write` writes `NOTICE` and `THIRD_PARTY_LICENSES.md`.
  Check them with `--check`.
- **Version-derived surfaces.** `VERSION` is the source.
  `python3 tools/check_version_sync.py --write` rewrites the derived surfaces.
- **Pinned dependency snapshots.** `third_party/manifest.lock.json` and `third_party/specifications.lock.json` pin them.
  `python3 tools/verify_third_party.py --check` verifies them.
  After a deliberate change, `--print-digests` prints the new lock values.
- **Pinned viewer assets.** `viewer/fetch-assets.sh` pins each artifact by SHA-256.
  [Asset provenance](viewer/ASSET_PROVENANCE.md) records their sources and licenses.
- **Fuzz seed corpus.** [The corpus guide](fuzz/corpus/README.md) records seed provenance and corpus rules.
- **Point-in-time records.** Files in `docs/audit/` other than the two current-state files keep their old paths, commands, and counts.
  Do not update them to the current contract.

## Gates

Run the native gate in a Python 3.11 virtual environment with the pinned `jsonschema` and `numpy`:

```bash
./scripts/setup_deps.sh
cmake --preset dev -DPython3_EXECUTABLE="$VIRTUAL_ENV/bin/python"
cmake --build --preset dev --parallel
ctest --preset dev
cmake --preset spz-off
cmake --build --preset spz-off --parallel
ctest --preset spz-off
```

Then run every command in [Repository checks](CONTRIBUTING.md#repository-checks) and `git diff --check`.
Run the [viewer checks](CONTRIBUTING.md#viewer-changes) when viewer files change.
CI also runs sanitizers, Linux and Windows builds, SDK install tests, Tauri checks, fuzz smoke, and secret scans.
A local run is not the complete CI gate. Name the checks that you ran.

## Technical writing

Use [ASD-STE100 Simplified Technical English, Issue 9](https://www.asd-ste100.org/assets/files/ASD-STE100_ISSUE9.pdf)
for project-owned technical documentation. These instructions apply to Markdown,
user messages, release notes, code comments, and other explanatory text.

Apply these rules:

- Use American English spelling.
- Use short, common words when they preserve the technical meaning.
- Use one term for each item or concept. Do not use a synonym only for variety.
- Treat established software names, API names, file formats, commands, flags, and
  domain terms as technical terms. Do not rename them.
- Give each sentence one topic. Give information in a logical order.
- Use no more than 25 words in a descriptive sentence.
- Use no more than 20 words in a procedural sentence.
- Use the active voice. Use the passive voice only when the actor is unknown or
  less important than the action.
- Write instructions in the imperative form. Put one instruction in each step.
- Put a condition before the action when the reader must know the condition first.
- Use simple present, simple past, or simple future tense. Avoid complex verb
  constructions when a simple tense has the same meaning.
- Do not use contractions or semicolons.
- Use a vertical list for complex information or three or more related items.
- Keep each paragraph about one topic. Use no more than six sentences in a
  paragraph.
- Use articles and demonstrative adjectives when they make a noun unambiguous.
- Write safety information as a direct command or condition. Then explain the
  possible harm or damage.

Accuracy has priority over vocabulary restriction. Do not change a requirement,
behavior, command, identifier, quotation, or legal meaning only to satisfy a
language rule.

Do not rewrite these items as project prose:

- Code, commands, paths, identifiers, protocol fields, and literal user-interface
  text
- Direct quotations and historical records
- License text, codes of conduct derived from an external template, generated
  notices, and vendored documentation

When you edit documentation, verify commands, links, version statements, and
capability claims against the repository. Preserve Markdown structure and valid
link targets.
