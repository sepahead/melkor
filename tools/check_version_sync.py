#!/usr/bin/env python3
"""Verify (or update) every version surface against the authoritative root VERSION file.

The root ``VERSION`` file is the only place a Melkor version is written by hand. Every
other surface — the CMake project version, the viewer's ``package.json``, the Tauri
config, the Cargo manifest, the Python distribution, the changelog heading, the citation
metadata — is derived from it.

Without an enforced check this decays immediately: someone bumps CMake, forgets
``package.json``, and the CLI, the desktop app, and the release asset now disagree about
what they are. That is exactly the P0-01 finding this tool exists to prevent from
recurring.

Usage::

    python3 tools/check_version_sync.py --check    # never writes; CI uses this
    python3 tools/check_version_sync.py --write    # rewrite derived surfaces

``--check`` must never modify a file. ``--write`` may only touch derived surfaces; it will
not touch ``VERSION`` itself, because the whole point is that a human decides the version
deliberately.

Version mapping across ecosystems
--------------------------------
The same release is spelled differently by different packaging ecosystems, and the mapping
must be deliberate rather than accidental:

===============  =================  ==================  ==============
VERSION          SemVer / npm       PEP 440 (Python)    Cargo
===============  =================  ==================  ==============
``2.0.0-dev``    ``2.0.0-dev``      ``2.0.0.dev0``      ``2.0.0-dev``
``2.0.0-rc.2``   ``2.0.0-rc.2``     ``2.0.0rc2``        ``2.0.0-rc.2``
``2.0.0``        ``2.0.0``          ``2.0.0``           ``2.0.0``
===============  =================  ==================  ==============

Python is the awkward one: PEP 440 does not accept ``-rc.2``, so it must be normalized, or
``pip`` will silently treat the distribution as a different version than the tag claims.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = REPO_ROOT / "VERSION"

# SemVer 2.0.0, restricted to the forms this project actually releases.
SEMVER_RE = re.compile(
    r"^(?P<major>0|[1-9]\d*)"
    r"\.(?P<minor>0|[1-9]\d*)"
    r"\.(?P<patch>0|[1-9]\d*)"
    r"(?:-(?P<prerelease>[0-9A-Za-z.-]+))?"
    r"(?:\+(?P<build>[0-9A-Za-z.-]+))?$"
)


class VersionError(Exception):
    """A version is malformed, or a surface cannot be represented."""


@dataclass(frozen=True)
class Version:
    """A parsed project version, with the per-ecosystem spellings it maps to."""

    raw: str
    major: int
    minor: int
    patch: int
    prerelease: str  # "" for a stable release

    @property
    def core(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"

    @property
    def is_prerelease(self) -> bool:
        return bool(self.prerelease)

    @property
    def semver(self) -> str:
        """npm, Cargo, and Tauri all accept the SemVer spelling unchanged."""
        return self.raw

    @property
    def pep440(self) -> str:
        """PEP 440 spelling for the Python distribution.

        PEP 440 has its own prerelease grammar and rejects ``-rc.2``. Getting this wrong
        does not fail loudly: pip accepts a *different* version than intended and the wheel
        silently disagrees with the git tag. So an unmappable prerelease is a hard error
        rather than a best-effort guess.
        """
        if not self.prerelease:
            return self.core

        # 2.0.0-dev / 2.0.0-dev.3  ->  2.0.0.dev0 / 2.0.0.dev3
        match = re.fullmatch(r"dev(?:\.(\d+))?", self.prerelease)
        if match:
            return f"{self.core}.dev{match.group(1) or '0'}"

        # 2.0.0-rc.2 / 2.0.0-rc2  ->  2.0.0rc2
        match = re.fullmatch(r"(a|b|rc|alpha|beta)\.?(\d+)", self.prerelease)
        if match:
            phase = {"alpha": "a", "beta": "b"}.get(match.group(1), match.group(1))
            return f"{self.core}{phase}{match.group(2)}"

        raise VersionError(
            f"prerelease {self.prerelease!r} has no defined PEP 440 mapping.\n"
            f"Use a form this project supports: 'dev', 'dev.N', 'a.N', 'b.N', or 'rc.N'.\n"
            f"Do not invent a mapping here — pip would accept a version that disagrees "
            f"with the git tag, and the wheel would then be untraceable to its source."
        )


def parse_version(text: str) -> Version:
    text = text.strip()
    match = SEMVER_RE.match(text)
    if not match:
        raise VersionError(
            f"invalid version {text!r}.\n"
            f"Expected SemVer, for example 2.0.0, 2.0.0-dev, or 2.0.0-rc.2."
        )
    return Version(
        raw=text,
        major=int(match.group("major")),
        minor=int(match.group("minor")),
        patch=int(match.group("patch")),
        prerelease=match.group("prerelease") or "",
    )


def read_authoritative_version() -> Version:
    if not VERSION_FILE.is_file():
        raise VersionError(f"missing authoritative version file: {VERSION_FILE}")

    raw = VERSION_FILE.read_text(encoding="utf-8")
    lines = [line for line in raw.splitlines() if line.strip()]
    if len(lines) != 1:
        raise VersionError(
            f"{VERSION_FILE} must contain exactly one non-empty line, found {len(lines)}."
        )
    return parse_version(lines[0])


# ---------------------------------------------------------------------------
# Surfaces
#
# A surface is one file that must agree with VERSION. Each knows how to read its own
# current value and how to rewrite itself. A surface that does not exist yet is skipped
# rather than failing, so this tool works throughout the migration instead of only after
# it: the Python package and CITATION.cff arrive in later work packages.
# ---------------------------------------------------------------------------


@dataclass
class Finding:
    surface: str
    path: Path
    expected: str
    actual: str | None  # None == the field is missing entirely

    @property
    def ok(self) -> bool:
        return self.actual == self.expected


def _regex_surface(
    name: str,
    relative_path: str,
    pattern: str,
    expected: str,
    *,
    template: str,
) -> tuple[Finding | None, callable | None]:
    """A surface whose version is one regex capture group in a text file.

    Returns the finding plus a closure that rewrites the file, or ``(None, None)`` when
    the file does not exist yet.
    """
    path = REPO_ROOT / relative_path
    if not path.is_file():
        return None, None

    text = path.read_text(encoding="utf-8")
    match = re.search(pattern, text, re.MULTILINE)
    actual = match.group(1) if match else None
    finding = Finding(name, path, expected, actual)

    def write() -> None:
        if match:
            start, end = match.span(1)
            path.write_text(text[:start] + expected + text[end:], encoding="utf-8")
        else:
            raise VersionError(
                f"{relative_path}: cannot write {name} — the expected pattern is absent.\n"
                f"Fix the file structure by hand; this tool will not guess where to insert "
                f"a version."
            )

    return finding, write


def _json_surface(
    name: str,
    relative_path: str,
    key: str,
    expected: str,
) -> tuple[Finding | None, callable | None]:
    """A surface whose version is a top-level JSON key.

    Rewritten with a targeted regex rather than a json.dump round-trip, because dumping
    would reformat the whole file and produce a diff nobody can review.
    """
    path = REPO_ROOT / relative_path
    if not path.is_file():
        return None, None

    data = json.loads(path.read_text(encoding="utf-8"))
    actual = data.get(key)
    finding = Finding(name, path, expected, actual if isinstance(actual, str) else None)

    def write() -> None:
        text = path.read_text(encoding="utf-8")
        pattern = rf'("{re.escape(key)}"\s*:\s*")([^"]*)(")'
        new_text, count = re.subn(pattern, rf"\g<1>{expected}\g<3>", text, count=1)
        if count != 1:
            raise VersionError(f"{relative_path}: could not locate the {key!r} key to rewrite.")
        path.write_text(new_text, encoding="utf-8")

    return finding, write


def _npm_lock_surface(expected: str) -> tuple[Finding | None, callable | None]:
    """``viewer/package-lock.json`` restates the project's own version in two places.

    A lockfile whose self-version disagrees with its ``package.json`` makes
    ``npm ci --lockfile-only`` churn, and it puts a wrong version into the viewer bundle.
    Both occurrences are checked, not just the first.
    """
    path = REPO_ROOT / "viewer/package-lock.json"
    if not path.is_file():
        return None, None

    data = json.loads(path.read_text(encoding="utf-8"))
    top = data.get("version")
    root_pkg = data.get("packages", {}).get("", {}).get("version")

    # Report drift unless *both* agree; surface whichever value is wrong.
    if top == expected and root_pkg == expected:
        actual: str | None = expected
    elif top != expected:
        actual = top if isinstance(top, str) else None
    else:
        actual = root_pkg if isinstance(root_pkg, str) else None

    finding = Finding("viewer package-lock", path, expected, actual)

    def write() -> None:
        text = path.read_text(encoding="utf-8")
        # Only the project's own self-declarations carry the project version; dependency
        # entries are nested deeper and are never equal to it by construction.
        for stale in {v for v in (top, root_pkg) if isinstance(v, str) and v != expected}:
            text = text.replace(f'"version": "{stale}"', f'"version": "{expected}"')
        path.write_text(text, encoding="utf-8")

    return finding, write


def _cargo_lock_surface(expected: str) -> tuple[Finding | None, callable | None]:
    """``viewer/src-tauri/Cargo.lock`` restates the ``melkor-viewer`` package version.

    Matched by package name so a dependency that happens to share the version string is
    never touched.
    """
    path = REPO_ROOT / "viewer/src-tauri/Cargo.lock"
    if not path.is_file():
        return None, None

    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        r'(\[\[package\]\]\nname = "melkor-viewer"\nversion = ")([^"]+)(")', re.MULTILINE
    )
    match = pattern.search(text)
    actual = match.group(2) if match else None
    finding = Finding("Tauri Cargo.lock", path, expected, actual)

    def write() -> None:
        if not match:
            raise VersionError(
                "viewer/src-tauri/Cargo.lock: no [[package]] entry named 'melkor-viewer'."
            )
        path.write_text(pattern.sub(rf"\g<1>{expected}\g<3>", text, count=1), encoding="utf-8")

    return finding, write


def collect_surfaces(version: Version) -> tuple[list[Finding], dict[str, callable]]:
    findings: list[Finding] = []
    writers: dict[str, callable] = {}

    def add(result: tuple[Finding | None, callable | None]) -> None:
        finding, writer = result
        if finding is not None:
            findings.append(finding)
            if writer is not None:
                writers[finding.surface] = writer

    # The viewer application and its desktop shell. All three take SemVer unchanged.
    add(_json_surface("viewer package.json", "viewer/package.json", "version", version.semver))
    add(
        _json_surface(
            "Tauri config", "viewer/src-tauri/tauri.conf.json", "version", version.semver
        )
    )
    add(
        _regex_surface(
            "Tauri Cargo.toml",
            "viewer/src-tauri/Cargo.toml",
            r'^version\s*=\s*"([^"]+)"',
            version.semver,
            template='version = "{}"',
        )
    )
    # The lockfiles restate the project's own version and drift just as easily.
    add(_npm_lock_surface(version.semver))
    add(_cargo_lock_surface(version.semver))

    # The Python distribution. PEP 440 spelling, not SemVer.
    #
    # Only checked once pyproject.toml actually declares a [project] table. Until the
    # Python package exists, the root pyproject.toml is Ruff configuration with no version
    # to keep in step, and demanding one would be a false failure.
    pyproject = REPO_ROOT / "pyproject.toml"
    if pyproject.is_file() and re.search(
        r"^\[project\]", pyproject.read_text(encoding="utf-8"), re.MULTILINE
    ):
        add(
            _regex_surface(
                "Python distribution",
                "pyproject.toml",
                r'^version\s*=\s*"([^"]+)"',
                version.pep440,
                template='version = "{}"',
            )
        )
    add(
        _regex_surface(
            "Python _version.py",
            "python/melkor3d/_version.py",
            r'^__version__\s*=\s*"([^"]+)"',
            version.pep440,
            template='__version__ = "{}"',
        )
    )

    # Citation metadata. Uses the SemVer spelling, matching the git tag.
    add(
        _regex_surface(
            "CITATION.cff",
            "CITATION.cff",
            r'^version:\s*"?([^"\n]+?)"?\s*$',
            version.semver,
            template="version: {}",
        )
    )

    return findings, writers


# ---------------------------------------------------------------------------
# Checks that are not simple string equality
# ---------------------------------------------------------------------------


def check_no_hardcoded_version_in_cmake() -> list[str]:
    """CMake must derive its version, never restate it.

    A literal ``project(melkor VERSION 2.0.0)`` is exactly how the surfaces drifted apart
    the first time, so it is rejected structurally rather than merely compared.
    """
    errors: list[str] = []
    path = REPO_ROOT / "CMakeLists.txt"
    text = path.read_text(encoding="utf-8")

    if re.search(r"project\s*\(\s*melkor\s+VERSION\s+[0-9]", text, re.IGNORECASE):
        errors.append(
            "CMakeLists.txt: project() states a literal version.\n"
            "  It must use the value parsed from VERSION:\n"
            "      project(melkor VERSION ${MELKOR_VERSION_CORE} LANGUAGES CXX)"
        )

    if re.search(r'set\s*\(\s*MELKOR_PRERELEASE\s+"', text):
        errors.append(
            "CMakeLists.txt: MELKOR_PRERELEASE is hand-set.\n"
            "  The prerelease field comes from VERSION via cmake/MelkorVersion.cmake."
        )

    if "cmake/MelkorVersion.cmake" not in text:
        errors.append(
            "CMakeLists.txt: does not include cmake/MelkorVersion.cmake before project()."
        )

    return errors


def check_changelog(version: Version) -> list[str]:
    """The changelog must carry an ``Unreleased`` section, and must head a stable release.

    A prerelease may sit under ``Unreleased``. A stable release may not: shipping 2.0.0
    with no 2.0.0 changelog section means users cannot see what changed, which is a release
    blocker, not a style nit.
    """
    errors: list[str] = []
    path = REPO_ROOT / "CHANGELOG.md"
    if not path.is_file():
        return ["CHANGELOG.md is missing."]

    text = path.read_text(encoding="utf-8")

    if not re.search(r"^##\s+Unreleased\s*$", text, re.MULTILINE):
        errors.append(
            "CHANGELOG.md: no '## Unreleased' section.\n"
            "  Every user-visible change lands there before a release collects it."
        )

    if not version.is_prerelease:
        heading = rf"^##\s+{re.escape(version.core)}\b"
        if not re.search(heading, text, re.MULTILINE):
            errors.append(
                f"CHANGELOG.md: no '## {version.core}' section, but VERSION is a stable "
                f"release.\n"
                f"  A stable release must document what changed before it is tagged."
            )

    return errors


def check_release_tag(version: Version, expected_tag: str) -> list[str]:
    """The release tag must be exactly ``v${VERSION}``.

    Release workflows pass ``--release-tag`` so a mistyped tag cannot silently publish a
    tree whose metadata says something else.
    """
    want = f"v{version.raw}"
    if expected_tag != want:
        return [
            f"Release tag {expected_tag!r} does not match VERSION.\n"
            f"  Expected {want!r}. Never tag a tree whose metadata disagrees with the tag."
        ]
    if version.is_prerelease and not re.fullmatch(r"(dev|a|b|rc)(\.\d+)?", version.prerelease):
        return [f"Prerelease {version.prerelease!r} is not a recognized release channel."]
    return []


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify or update every version surface against the root VERSION file."
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--check",
        action="store_true",
        help="report drift and exit non-zero. Never modifies a file. Used by CI.",
    )
    mode.add_argument(
        "--write",
        action="store_true",
        help="rewrite derived surfaces to match VERSION.",
    )
    parser.add_argument(
        "--release-tag",
        metavar="TAG",
        help="additionally assert the tag equals v${VERSION}. Used by release workflows.",
    )
    args = parser.parse_args()

    try:
        version = read_authoritative_version()
    except VersionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(f"VERSION            {version.raw}")
    print(f"  semver / npm     {version.semver}")
    print(f"  PEP 440          {version.pep440}")
    print(f"  prerelease       {'yes' if version.is_prerelease else 'no'}")
    print()

    try:
        findings, writers = collect_surfaces(version)
    except VersionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    structural_errors = check_no_hardcoded_version_in_cmake() + check_changelog(version)
    if args.release_tag:
        structural_errors += check_release_tag(version, args.release_tag)

    if args.write:
        wrote = 0
        for finding in findings:
            if finding.ok:
                continue
            writer = writers.get(finding.surface)
            if writer is None:
                continue
            try:
                writer()
            except VersionError as exc:
                print(f"error: {exc}", file=sys.stderr)
                return 2
            print(f"updated  {finding.surface}: {finding.actual} -> {finding.expected}")
            wrote += 1

        if wrote == 0:
            print("All derived surfaces already match VERSION.")

        # --write fixes derived strings. It cannot fix a structural problem, and it must
        # not pretend it did.
        if structural_errors:
            print("\nStructural problems remain; --write cannot fix these:\n", file=sys.stderr)
            for error in structural_errors:
                print(f"  {error}\n", file=sys.stderr)
            return 1
        return 0

    # --check
    drifted = [f for f in findings if not f.ok]

    for finding in findings:
        status = "ok  " if finding.ok else "DRIFT"
        shown = finding.actual if finding.actual is not None else "<missing>"
        print(f"  {status}  {finding.surface:<24} {shown}")

    if not findings:
        print("  (no derived version surfaces present yet)")

    if drifted or structural_errors:
        print("\nVersion synchronization failed.\n", file=sys.stderr)
        for finding in drifted:
            rel = finding.path.relative_to(REPO_ROOT)
            shown = finding.actual if finding.actual is not None else "<missing>"
            print(
                f"  {rel}: {finding.surface} is {shown!r}, expected {finding.expected!r}",
                file=sys.stderr,
            )
        for error in structural_errors:
            print(f"  {error}", file=sys.stderr)
        print(
            "\nFix by editing VERSION and running:\n"
            "    python3 tools/check_version_sync.py --write",
            file=sys.stderr,
        )
        return 1

    print("\nAll version surfaces agree with VERSION.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
