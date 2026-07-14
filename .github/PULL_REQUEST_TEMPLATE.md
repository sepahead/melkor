<!--
Fill in every section. Where a section does not apply, write "N/A" and one clause saying why —
an empty section is indistinguishable from an unconsidered one.

Melkor has a single maintainer (see GOVERNANCE.md). Your pull request is the only artifact a
reviewer has, and in most cases the reviewer is also the person who will have to debug it in a
year. Detail here is not bureaucracy; it is the substitute for the second reviewer this project
does not have.
-->

## Objective and linked issue

<!-- What problem this solves, and for whom. Link the issue: "Closes #123" / "Refs #123".
     If there is no issue and this is more than a typo fix, say why it went straight to a PR. -->

## User-visible contract change

<!-- What a user can now do, or can no longer do. Cover the CLI surface and its machine-readable
     output, the C ABI and C++ headers, the Python surface, file formats and profile IDs, exit
     codes, and diagnostics. If output bytes change for an input that previously succeeded, say so
     explicitly — that is a contract change even when no signature moved.
     Write "None" if the observable behaviour is identical. -->

## Files and architecture affected

<!-- The main files/subsystems touched and why the change belongs there. Note any new dependency,
     any new third-party code, and any change to the build or link topology. -->

## Compatibility and migration

<!-- Backward compatibility for existing assets, commands, and callers. Any deprecation, and the
     migration path. A format profile's meaning must never change silently under the same profile
     ID — if the meaning changed, a new ID and a migration note are required. -->

## Security and resource impact

<!-- Does this touch untrusted input (GLB/PLY/SPZ readers), path handling, subprocess invocation,
     downloads, or the viewer's server? Which resource limits apply, and are they still enforced
     on the new path? A parser change without a bounds argument is not reviewable. -->

## Tests and evidence

<!-- What you ran, and what it proved. Paste the relevant output. New behaviour needs both a
     positive and a negative test; a parser change needs a malformed-input test. -->

- [ ] `ctest` passes in the default configuration
- [ ] `ctest` passes with `-DMELKOR_USE_METAL=OFF` (stub/CPU topology, same link shape as Linux CPU)
- [ ] Backend-semantic changes applied to Metal, CUDA, and CPU in this same PR
- [ ] Sanitizer build run for parser or memory-handling changes (ASan + UBSan)
- [ ] No new compiler warnings (`-Wall -Wextra -Wpedantic` on first-party code)
- [ ] Python changes pass `ruff check`; viewer changes pass `bun run test` in `viewer/`

## Interoperability and benchmark evidence

<!-- If this changes how an asset is read or written: which producer/consumer tools and versions
     you checked against, and what the round-trip showed.
     If this claims a performance change: the hardware, the input, the command, and the before/after
     numbers with run count. A performance claim without a reproducible command is an anecdote and
     will be treated as one. -->

## Documentation and release notes

<!-- Which docs changed, and the CHANGELOG.md entry for any user-visible change. If this alters what
     the project claims to support, SUPPORT.md and the platform matrix must move with it. -->

## Rollback plan

<!-- How to undo this if it turns out to be wrong in a release: is a plain revert sufficient, or does
     it leave migrated data, a published artifact, or a persisted profile ID behind? If a clean revert
     is not possible, say what the recovery path is. -->

---

## Ten-lens review

Confirm each lens, or state the exception. The point of the list is to force the failure modes that
are invisible from inside the diff to be considered at least once.

- [ ] **1. Correctness and data integrity** — The change is right for valid input, and it does not
      silently corrupt, drop, or reinterpret data. Numerical behaviour (normalization, coordinate
      frame, quaternion order, SH basis/order, opacity and colour domains) is preserved or the change
      is intentional and documented. Failure returns an error rather than plausible-looking output.
- [ ] **2. Security and adversarial input** — Untrusted input is validated before use: indices,
      strides, counts, offsets, and sizes are bounds-checked; arithmetic cannot overflow into a
      short allocation. No new path injection, command injection, or unverified download. Errors do
      not leak paths or environment detail into machine-readable output.
- [ ] **3. Performance and resources** — Memory, CPU, and disk use are bounded and proportionate to
      declared limits. No unbounded allocation driven by a field an attacker controls. Any
      regression is measured and justified, not assumed to be negligible.
- [ ] **4. Portability and distribution** — Builds on the supported platform matrix, not only on the
      author's machine. No reliance on undefined behaviour, unspecified evaluation order, endianness,
      or a specific compiler's tolerance. Packaging and install paths still work.
- [ ] **5. API, CLI, UX, and accessibility** — Names, flags, defaults, and exit codes are consistent
      with the existing surface. Diagnostics say what failed, where, and what to do next. Output
      remains parseable for machine consumers and legible for human ones; the viewer remains
      keyboard-navigable and does not rely on colour alone to convey state.
- [ ] **6. Interoperability and ecosystem** — Files Melkor writes are still readable by the tools that
      matter, and files those tools write are still readable by Melkor. Behaviour follows the pinned
      specification rather than one implementation's quirk; where a producer is non-conformant, the
      workaround is explicit and cited.
- [ ] **7. Scientific validity and claims** — Any claim about quality, accuracy, or method is
      supported by evidence in the PR and is stated with its conditions. No implication that Melkor
      performs reconstruction or training it does not perform, and no borrowing of an external
      adapter's results as if they were Melkor's own.
- [ ] **8. Reproducibility and supply chain** — Output is deterministic where it is supposed to be
      (given seed and input). Dependencies are pinned and licence-recorded; vendored code is not
      modified without a recorded patch. Release evidence still builds and verifies from a clean tree.
- [ ] **9. Licensing, privacy, and governance** — Every added file's licence is compatible with the
      core's and is recorded (`NOTICE`, `THIRD_PARTY_LICENSES.md`, `third_party/manifest.lock.json`).
      No copyleft or research-only source enters the redistributable core. No personal data, path, or
      credential is committed, logged, or emitted in a report.
- [ ] **10. Maintainability and operations** — The next person can understand this from the code and
      the comments. Non-obvious decisions carry a *why*, not a restatement of the *what*. Failure is
      diagnosable from the logs a user would actually have.

## Reviewer notes

<!-- The parts you are least sure about, and where you most want scrutiny. Naming your own weak spot
     is the most useful thing you can write here. -->
