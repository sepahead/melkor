# Support

## Current support status

**No production release is currently supported.**

Melkor is in v2 hardening. Until `v2.0.0` is published:

- `main` is development software. It may build, pass tests, and still change its public contract
  without notice.
- `v2.0.0-rc.1` is a release candidate. It is not a supported production release.
- The `1.x` releases remain downloadable, but they are not the supported production line and they
  do not receive fixes.

If you need a stable, supported Melkor, wait for `v2.0.0`. Watch
[Releases](https://github.com/sepahead/melkor/releases).

This is deliberate. The project would rather tell you plainly that nothing is supported yet than
imply that a release candidate carries a support promise it cannot honour.

## Support policy after v2.0.0

Once `v2.0.0` is published, the following applies.

**Supported line.** The latest patch release of the current stable minor receives correctness and
security fixes. Only the latest patch within a minor line receives fixes, except that a critical
security issue may be backported.

**Support window.** `2.0.x` is supported until the later of:

- twelve months after `v2.0.0`; or
- ninety days after the next supported minor release.

**Not supported.** `main` and release candidates are accepted for bug reports but carry no
production support promise. Versions earlier than `2.0` are not supported, though a coordinated
security fix remains at maintainer discretion. Older unsupported versions stay downloadable with
a warning; they are not deleted, because deleting them would break provenance for anyone who
already cited or pinned them.

**Format profiles.** Support for a format profile or specification revision may be revised in a
minor release, but only through an explicit new profile ID and a migration note. An existing
profile ID never silently changes meaning.

**Platform matrix.** The supported operating system, Python, compiler, and backend matrix is
versioned per release. The release page and `melkor backends --output-mode json` are
authoritative. A backend is not supported on the strength of a compile-only CI job.

**Effort.** Support is best-effort open source unless a separate commercial contract exists.
There is no service-level agreement.

## Getting help

| You want to | Use |
|---|---|
| Ask a question or discuss an idea | [GitHub Discussions](https://github.com/sepahead/melkor/discussions) |
| Report a reproducible bug | [GitHub Issues](https://github.com/sepahead/melkor/issues) |
| Report a format incompatibility | GitHub Issues, using the format-compatibility form |
| Report a performance problem | GitHub Issues, using the performance form, with a benchmark manifest |
| Report a security vulnerability | **Not** a public issue — see [SECURITY.md](SECURITY.md) |

## What to include in a bug report

A report we can act on contains:

- the output of `melkor version --output-mode json`;
- the output of `melkor doctor --output-mode json`, redacted if it contains anything sensitive;
- how you installed Melkor (released archive, wheel, or source build);
- your operating system and architecture;
- the exact command or API call, and its output;
- what you expected and what happened instead;
- an inspection report (`melkor inspect --input <asset> --level structure --output-mode json`)
  where the problem involves an asset;
- a minimal asset that reproduces it, **only if you have the right to share it**.

If you cannot share the asset, say so. An inspection report is often enough, and it is designed
to be safe to paste: by default it reports a basename rather than a full path and it does not
include environment variables, usernames, or home directories.

Do not attach an asset you do not have permission to redistribute, and do not attach anything
containing personal data.

## Security reports

Never open a public issue for a suspected vulnerability. Follow [SECURITY.md](SECURITY.md) and
use GitHub private vulnerability reporting.

Resource exhaustion from large or maliciously constructed input **is in scope**. If you can make
Melkor consume unbounded memory, CPU, or disk from a file that passes its declared limits, that
is a security report, not a performance complaint.
