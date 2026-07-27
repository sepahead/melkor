<!--
Fill in every section. Where a section does not apply, write "N/A" and one clause saying why —
an empty section is indistinguishable from an unconsidered one.

Melkor has a single maintainer (see GOVERNANCE.md). Your pull request is the reviewer's primary
artifact. The reviewer can also maintain this code later. Give enough detail for both tasks.
-->

## Objective and linked issue

<!-- What problem this solves, and for whom. Link the issue: "Closes #123" / "Refs #123".
     If there is no issue and this is more than a typo fix, say why it went straight to a PR. -->

## User-visible contract change

<!-- What a user can now do, or can no longer do. Cover the CLI surface and its machine-readable
     output, the C ABI and C++ headers, the Python surface, file formats and profile IDs, exit
     codes, and diagnostics. If output bytes change for an input that previously succeeded, say so
     explicitly — that is a contract change even when no signature moved.
     Write "None" if the observable behavior is identical. -->

## Files and architecture affected

<!-- The main files/subsystems touched and why the change belongs there. Note any new dependency,
     any new third-party code, and any change to the build or link topology. -->

## Compatibility and migration

<!-- Backward compatibility for existing assets, commands, and callers. Any deprecation, and the
     migration path. A format profile's meaning must not change under the same profile ID.
     If the meaning changed, add a new ID and a migration note. -->

## Security and resource impact

<!-- Does this touch untrusted input (GLB/PLY/SPZ readers), path handling, subprocess invocation,
     downloads, or the viewer's server? Which resource limits apply, and are they still enforced
     on the new path? A parser change without a bounds argument is not reviewable. -->

## Tests and evidence

<!-- What you ran, and what it proved. Paste the relevant output. New behavior needs both a
     positive and a negative test. A parser change needs a malformed-input test. -->

- [ ] `ctest` passes in the default configuration
- [ ] `ctest` passes with `-DMELKOR_USE_METAL=OFF` (CPU-only topology)
- [ ] Backend-semantic changes applied to Metal, CUDA, and CPU in this same PR
- [ ] Sanitizer build run for parser or memory-handling changes (ASan + UBSan)
- [ ] No new compiler warnings (`-Wall -Wextra -Wpedantic` on first-party code)
- [ ] Python changes pass `ruff check`. Viewer changes pass `bun run test` in `viewer/`.

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
      silently corrupt, drop, or reinterpret data. Numerical behavior (normalization, coordinate
      frame, quaternion order, SH basis/order, opacity and color domains) is preserved or the change
      is intentional and documented. Failure returns an error rather than plausible-looking output.
- [ ] **2. Security and adversarial input** — Untrusted input is validated before use. Indices,
      strides, counts, offsets, and sizes are bounds-checked. Arithmetic cannot overflow into a
      short allocation. Add no path injection, command injection, or unverified download. Errors do
      not leak paths or environment detail into machine-readable output.
- [ ] **3. Performance and resources** — Memory, CPU, and disk use are bounded and proportionate to
      declared limits. No unbounded allocation driven by a field an attacker controls. Any
      regression is measured and justified, not assumed to be negligible.
- [ ] **4. Portability and distribution** — Builds on the supported platform matrix, not only on the
      author's machine. No reliance on undefined behavior, unspecified evaluation order, endianness,
      or a specific compiler's tolerance. Packaging and install paths still work.
- [ ] **5. API, CLI, UX, and accessibility** — Names, flags, defaults, and exit codes are consistent
      with the existing surface. Diagnostics say what failed, where, and what to do next. Output
      remains parseable for machine consumers and legible for human ones. The viewer remains
      keyboard-navigable and does not rely on color alone to convey state.
- [ ] **6. Interoperability and ecosystem** — Files Melkor writes are still readable by the tools that
      matter, and files those tools write are still readable by Melkor. Behavior follows the pinned
      specification rather than one implementation's quirk. If a producer is nonconformant, the
      workaround is explicit and cited.
- [ ] **7. Scientific validity and claims** — Evidence in the PR supports each quality, accuracy,
      or method claim. State the conditions. Do not claim that Melkor performs external training.
      Do not present an external adapter's result as a Melkor result.
- [ ] **8. Reproducibility and supply chain** — Output is deterministic where it is supposed to be
      for the same seed and input. Dependencies are pinned and license-recorded. Vendored code is not
      modified without a recorded patch. Release evidence still builds and verifies from a clean tree.
- [ ] **9. Licensing, privacy, and governance** — Every added file's license is compatible with the
      core's and is recorded (`NOTICE`, `THIRD_PARTY_LICENSES.md`, `third_party/manifest.lock.json`).
      No copyleft or research-only source enters the redistributable core. No personal data, path, or
      credential is committed, logged, or emitted in a report.
- [ ] **10. Maintainability and operations** — The next person can understand this from the code and
      the comments. Non-obvious decisions carry a *why*, not a restatement of the *what*. Failure is
      diagnosable from the logs a user would actually have.

## Reviewer notes

<!-- The parts you are least sure about, and where you most want scrutiny. Naming your own weak spot
     is the most useful thing you can write here. -->
