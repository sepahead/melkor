# Contributing to Melkor

Melkor accepts changes that preserve its narrow format and safety contract.
Read [GOVERNANCE.md](GOVERNANCE.md) before you change the product boundary.

## Development setup

```bash
./scripts/setup_deps.sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

Use the `spz-off` preset to test the optional adapter boundary:

```bash
cmake --preset spz-off
cmake --build --preset spz-off --parallel
ctest --preset spz-off
```

See [Quick Start](docs/QUICKSTART.md) for the Python test environment and install steps.

## Architecture rules

Apply these rules to every core change:

- Treat every asset byte and count as untrusted.
- Check arithmetic before allocation or range access.
- Charge one operation budget before each controlled allocation.
- Check cancellation and the deadline in long loops.
- Preserve the canonical `SplatData` invariants.
- Use an exact format profile.
- Reject an ambiguous source semantic.
- Report each representational loss before writing output.
- Require an exact approval code for each severe loss.
- Route file output through `AtomicWriter`.
- Keep the installed surface limited to the C ABI.

Do not add a fallback that changes data semantics.
Do not turn a warning into an implicit default for ambiguous input.

## Format changes

A format change must include:

- The exact specification or producer revision
- A versioned profile change when semantics change
- Positive, boundary, and malformed fixtures
- Resource-limit tests
- Round-trip or differential evidence when it applies
- Loss-policy changes for each unrepresentable feature
- Documentation and changelog updates

Do not change an existing profile identifier to mean something new.
Add a new identifier and a migration note.

## Error handling

Return `Result<T>` from a fallible core operation.
Use a stable `MK####_*` diagnostic code for a new failure condition.

Do not parse English error text for control flow.
Do not let an exception cross the C ABI.
Do not change a CLI exit-code class without a compatibility review.

## Code style

- Use C++17 and four-space indentation.
- Run the repository `clang-format` version on changed C++ files.
- Keep first-party C++ warnings clean under `MELKOR_WERROR=ON`.
- Run Ruff on Python files.
- Use two-space indentation in shell scripts.
- Run `bash -n` and ShellCheck on shell changes.
- Follow `AGENTS.md` for project-owned technical prose.

Keep tests deterministic.
Print a seed when a randomized property test fails.
Use a focused regression for each fixed defect.

The `dev` preset writes `build/dev/compile_commands.json`.
Run the optional static analysis with `run-clang-tidy -p build/dev`.

## Viewer changes

The viewer is a separate JavaScript and Rust workspace.
It must stay local-only by default.

Run these checks after a viewer change:

```bash
cd viewer
npm ci --ignore-scripts
npm audit --audit-level=high
bun run test -- --project=chromium

cd src-tauri
rustup run 1.90.0 cargo fmt --all -- --check
rustup run 1.90.0 cargo clippy --locked --all-targets --all-features -- -D warnings
rustup run 1.90.0 cargo test --locked --all-targets --all-features
```

Do not add Tauri IPC permission without a documented need and a security review.

## Repository checks

Run the policy tools from the repository root:

```bash
ruff check . --no-unsafe-fixes
python3 tools/verify_third_party.py --check
python3 tools/generate_notices.py --check
python3 tools/check_version_sync.py --check
python3 tools/build_source_bundle.py --check
python3 tools/check_profiles.py
python3 tools/check_claims.py
python3 tools/check_docs_links.py
python3 tools/check_docs_style.py
python3 tests/test_tools.py
```

## Pull request checklist

- [ ] The strict `dev` build and complete CTest set pass.
- [ ] The `spz-off` build and tests pass.
- [ ] New parser paths have hostile-input and resource-limit tests.
- [ ] Public behavior has stable diagnostics and documentation.
- [ ] Format losses are complete and tested.
- [ ] The installed C ABI remains source-compatible or changes through the ABI policy.
- [ ] The viewer checks pass when viewer files change.
- [ ] Repository policy tools pass.
- [ ] `CHANGELOG.md` records each user-visible change.
- [ ] `git diff --check` passes.

## Report an issue

Include the source version, commit SHA, build command, operating system, exact command, and complete output.
Include `melkor inspect INPUT --json` when an asset triggers the issue.

Share a minimal asset only when you have redistribution rights.
Use [private vulnerability reporting](SECURITY.md#report-a-vulnerability-privately) for a security issue.
