<!-- Complete each section. If a section does not apply, write "N/A" and give the reason. -->

## Objective and linked issue

<!-- Define the user problem. Link the issue, or explain why no issue exists. -->

## User-visible contract change

<!--
Describe each changed command, API, profile, exit code, diagnostic, and output.
State "None" if observable behavior does not change.
-->

## Files and architecture affected

<!-- Identify the changed subsystems. Explain each new dependency or build-topology change. -->

## Compatibility and migration

<!--
Describe compatibility for existing callers, commands, and assets.
If a format profile changes meaning, add a new profile ID.
-->

## Security and resource impact

<!--
Identify each untrusted-input or path-handling change.
Name the applicable memory, CPU, disk, file, and count limits.
-->

## Tests and evidence

<!-- List the commands that ran. Explain the behavior that each command verified. -->

- [ ] `cmake --build --preset dev` and `ctest --preset dev` pass
- [ ] `cmake --build --preset spz-off` and `ctest --preset spz-off` pass
- [ ] C ABI changes pass `scripts/test_sdk_install.sh`
- [ ] Parser and memory changes pass ASan and UBSan
- [ ] First-party code has no new compiler warnings
- [ ] Python changes pass Ruff and their tests
- [ ] Viewer changes pass Playwright
- [ ] Tauri changes pass rustfmt, Clippy, Cargo checks, and the staged build

## Interoperability and benchmark evidence

<!--
For a format change, identify the tested producer and consumer versions.
For a performance claim, give the hardware, input, command, run count, and results.
-->

## Documentation and release notes

<!-- List the changed documentation. Add a changelog entry for each user-visible change. -->

## Rollback plan

<!-- Explain how to reverse the change. Identify all data or artifacts that a revert cannot restore. -->

## Twenty-lens review

Confirm each lens or describe the exception.

- [ ] **1. Functional correctness**: Valid input produces the specified result. Invalid input returns a stable error.
- [ ] **2. Data integrity**: The change does not silently drop, corrupt, or reinterpret data.
- [ ] **3. Format semantics**: Profiles define field domains, order, units, frame, and color space.
- [ ] **4. Numerical behavior**: Arithmetic handles overflow, non-finite values, singular values, and tolerance boundaries.
- [ ] **5. Input security**: Parsers validate counts, offsets, sizes, indices, and strings before use.
- [ ] **6. Resource control**: Memory, CPU, disk, and expanded data stay within one operation budget.
- [ ] **7. Filesystem safety**: Path operations resist traversal, link attacks, replacement races, and partial writes.
- [ ] **8. Control flow**: Cancellation, deadlines, progress, and concurrent state changes remain correct.
- [ ] **9. Performance**: Measurements support each performance claim. The change avoids unnecessary copies and scans.
- [ ] **10. Portability**: Supported platforms avoid undefined behavior and platform-specific assumptions.
- [ ] **11. API and ABI**: Public types, layouts, symbols, ownership, errors, and version rules remain compatible.
- [ ] **12. CLI and accessibility**: Commands, diagnostics, keyboard use, focus, motion, and contrast remain clear.
- [ ] **13. Interoperability**: Tests cover the pinned specification and relevant producer and consumer tools.
- [ ] **14. Scientific claims**: Reproducible evidence supports each accuracy, quality, or method claim.
- [ ] **15. Test strength**: Tests cover success, failure, boundaries, malformed input, and regression behavior.
- [ ] **16. Reproducibility**: Dependencies, inputs, versions, seeds, and output metadata remain deterministic.
- [ ] **17. Supply chain**: Every dependency has a fixed identity, verified bytes, license record, and reviewable patch history.
- [ ] **18. Privacy and governance**: Reports expose no secrets or personal paths. The change follows project authority rules.
- [ ] **19. Maintainability**: Code has one clear owner for each concept. Comments explain non-obvious decisions.
- [ ] **20. Release operations**: Packaging, installation, provenance, rollback, and support claims match the shipped artifact.

## Reviewer notes

<!-- Identify uncertain decisions and the parts that need the closest review. -->
