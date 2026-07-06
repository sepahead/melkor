# Security Policy

## Supported versions

Security fixes are applied to the `main` branch. There are no maintained
release branches; users should track `main`.

## Reporting a vulnerability

Please report vulnerabilities privately via
[GitHub Security Advisories](https://github.com/sepahead/melkor/security/advisories/new)
("Report a vulnerability"). Do **not** open a public issue for anything
exploitable.

Include what you can of: affected file/function, a minimal reproducing
input, the platform and backend (`melkor --info`), and the impact you
believe it has. You should receive an acknowledgment within a few days.

## Scope and threat model

Melkor parses untrusted input by design. The following are in scope and
treated as security bugs, not ordinary crashes:

- Memory unsafety (out-of-bounds reads/writes, wild pointers) in the GLB,
  PLY, or SPZ readers, or in any code reachable from a crafted input file
- Command or path injection through the pipeline scripts' handling of
  user-supplied paths and filenames
- The viewer's static file server escaping its root directory
- Model-weight downloads accepting spoofed or tampered payloads

Out of scope: vulnerabilities exclusively in vendored third-party trees
(`third_party/`, `tools/OpenSplat/`, `ml-sharp/`, `DA3coreml/`) — report
those upstream — and denial-of-service via absurdly large but well-formed
inputs.

## Hardening notes for integrators

- Treat all splat/mesh files from untrusted sources as attacker-controlled;
  run conversions with the least privilege your platform allows.
- The viewer's dev server (`viewer/serve.js`) binds 127.0.0.1 and is not
  intended for exposure to untrusted networks.
- CI runs an AddressSanitizer + UndefinedBehaviorSanitizer build on every
  push; run a sanitizer build locally when touching parser code.
