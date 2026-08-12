#!/usr/bin/env python3
"""Verify or update each version surface from the root VERSION file.

The root ``VERSION`` file is the only manual version source. This tool checks each
package, application, lockfile, and citation surface. It also checks release structure.

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
---------------------------------
Each package system uses a different version syntax. This tool applies the following map:

===============  =================  ==================  ==============
VERSION          SemVer / npm       PEP 440 (Python)    Cargo
===============  =================  ==================  ==============
``2.0.0-dev``    ``2.0.0-dev``      ``2.0.0.dev0``      ``2.0.0-dev``
``2.0.0-rc.2``   ``2.0.0-rc.2``     ``2.0.0rc2``        ``2.0.0-rc.2``
``2.0.0``        ``2.0.0``          ``2.0.0``           ``2.0.0``
===============  =================  ==================  ==============

PEP 440 does not accept ``-rc.2``. The tool changes this form to ``rc2``.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import stat
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

REPO_ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = REPO_ROOT / "VERSION"
MAX_TEXT_BYTES = 4 * 1024 * 1024

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


Writer = Callable[[], None]


def read_text(path: Path) -> str:
    """Read a bounded regular UTF-8 file."""

    if path.is_symlink():
        raise VersionError(f"{path}: version surfaces must not be symbolic links.")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
        with os.fdopen(descriptor, "rb") as handle:
            initial = os.fstat(handle.fileno())
            if not stat.S_ISREG(initial.st_mode):
                raise VersionError(f"{path}: the version surface must be a regular file.")
            if initial.st_size > MAX_TEXT_BYTES:
                raise VersionError(f"{path}: the version surface exceeds {MAX_TEXT_BYTES} bytes.")
            raw = handle.read(MAX_TEXT_BYTES + 1)
            final = os.fstat(handle.fileno())
            if (
                len(raw) != initial.st_size
                or final.st_dev != initial.st_dev
                or final.st_ino != initial.st_ino
                or final.st_size != initial.st_size
                or final.st_mtime_ns != initial.st_mtime_ns
                or final.st_ctime_ns != initial.st_ctime_ns
            ):
                raise VersionError(f"{path}: the version surface changed while it was read.")
    except OSError as exc:
        raise VersionError(f"{path}: cannot read the version surface: {exc}") from exc
    if len(raw) > MAX_TEXT_BYTES:
        raise VersionError(f"{path}: the version surface exceeds {MAX_TEXT_BYTES} bytes.")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise VersionError(f"{path}: the version surface is not valid UTF-8: {exc}") from exc


def parse_json_object(text: str, path: Path) -> dict[str, Any]:
    """Parse one JSON object and reject duplicate object names."""

    def object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        value: dict[str, Any] = {}
        for name, item in pairs:
            if name in value:
                raise VersionError(f"{path}: duplicate JSON object name {name!r}.")
            value[name] = item
        return value

    def reject_constant(value: str) -> None:
        raise VersionError(f"{path}: non-JSON number {value!r}.")

    def finite_float(value: str) -> float:
        parsed = float(value)
        if not math.isfinite(parsed):
            raise VersionError(f"{path}: non-finite JSON number {value!r}.")
        return parsed

    try:
        value = json.loads(
            text,
            object_pairs_hook=object_without_duplicates,
            parse_constant=reject_constant,
            parse_float=finite_float,
        )
    except json.JSONDecodeError as exc:
        raise VersionError(f"{path}: cannot read valid JSON: {exc}") from exc
    except RecursionError as exc:
        raise VersionError(f"{path}: JSON nesting is too deep.") from exc
    if not isinstance(value, dict):
        raise VersionError(f"{path}: the JSON root must be an object.")
    return value


def read_json_object(path: Path) -> dict[str, Any]:
    """Read a bounded JSON object and reject duplicate object names."""

    return parse_json_object(read_text(path), path)


def replace_top_level_json_string(path: Path, key: str, expected: str) -> str:
    """Replace one top-level JSON string without changing other formatting."""

    text = read_text(path)
    escaped_key = re.escape(json.dumps(key))
    pattern = re.compile(
        rf'({escaped_key}\s*:\s*")((?:\\.|[^"\\])*)(")'
    )
    replacement_value = json.dumps(expected, ensure_ascii=False)[1:-1]
    candidates: list[str] = []
    for match in pattern.finditer(text):
        candidate = text[: match.start(2)] + replacement_value + text[match.end(2) :]
        try:
            parsed = parse_json_object(candidate, path)
        except VersionError:
            continue
        if parsed.get(key) == expected:
            candidates.append(candidate)
    if len(candidates) != 1:
        raise VersionError(
            f"{path}: cannot locate exactly one top-level string field {key!r}."
        )
    return candidates[0]


def atomic_write_text(path: Path, text: str) -> None:
    """Replace one text file only after the complete write succeeds."""
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        temporary.chmod(path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    except OSError as exc:
        raise VersionError(f"{path}: cannot replace the version surface: {exc}") from exc
    finally:
        temporary.unlink(missing_ok=True)


@dataclass(frozen=True)
class Version:
    """A parsed project version, with the per-ecosystem spellings it maps to."""

    raw: str
    major: int
    minor: int
    patch: int
    prerelease: str  # "" for a stable release
    build: str  # "" when the version has no build metadata

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
            mapped = self.core

        else:
            # 2.0.0-dev / 2.0.0-dev.3  ->  2.0.0.dev0 / 2.0.0.dev3
            match = re.fullmatch(r"dev(?:\.(\d+))?", self.prerelease)
            if match:
                mapped = f"{self.core}.dev{match.group(1) or '0'}"

            else:
                # 2.0.0-rc.2 / 2.0.0-rc2  ->  2.0.0rc2
                match = re.fullmatch(r"(a|b|rc|alpha|beta)\.?(\d+)", self.prerelease)
                if match:
                    phase = {"alpha": "a", "beta": "b"}.get(
                        match.group(1), match.group(1)
                    )
                    mapped = f"{self.core}{phase}{match.group(2)}"
                else:
                    raise VersionError(
                        f"prerelease {self.prerelease!r} has no defined PEP 440 mapping.\n"
                        f"Use a form this project supports: 'dev', 'dev.N', 'a.N', 'b.N', or "
                        f"'rc.N'.\nDo not invent a mapping here. The wheel must map to its "
                        f"source tag."
                    )

        if self.build:
            local = self.build.lower().replace("-", ".")
            mapped += f"+{local}"
        return mapped


def parse_version(text: str) -> Version:
    text = text.strip()
    match = SEMVER_RE.match(text)
    if not match:
        raise VersionError(
            f"invalid version {text!r}.\n"
            f"Expected SemVer, for example 2.0.0, 2.0.0-dev, or 2.0.0-rc.2."
        )
    prerelease = match.group("prerelease") or ""
    build = match.group("build") or ""
    for label, value in (("prerelease", prerelease), ("build metadata", build)):
        if value and any(not identifier for identifier in value.split(".")):
            raise VersionError(f"invalid version {text!r}: {label} has an empty identifier.")
    for identifier in prerelease.split(".") if prerelease else ():
        if identifier.isdigit() and len(identifier) > 1 and identifier.startswith("0"):
            raise VersionError(
                f"invalid version {text!r}: numeric prerelease identifier {identifier!r} "
                "has a leading zero."
            )
    components = tuple(int(match.group(name)) for name in ("major", "minor", "patch"))
    if any(component > 0xFFFFFFFF for component in components):
        raise VersionError(f"invalid version {text!r}: a numeric core field exceeds uint32.")
    return Version(
        raw=text,
        major=components[0],
        minor=components[1],
        patch=components[2],
        prerelease=prerelease,
        build=build,
    )


def read_authoritative_version() -> Version:
    if not VERSION_FILE.is_file():
        raise VersionError(f"missing authoritative version file: {VERSION_FILE}")

    raw = read_text(VERSION_FILE)
    lines = raw.splitlines()
    if len(lines) != 1 or not lines[0].strip() or raw != f"{lines[0]}\n":
        raise VersionError(
            f"{VERSION_FILE} must contain one non-empty line with an LF terminator."
        )
    if lines[0] != lines[0].strip():
        raise VersionError(f"{VERSION_FILE} must not contain surrounding white space.")
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
    required: bool = True,
) -> tuple[Finding | None, Writer | None]:
    """A surface whose version is one regex capture group in a text file.

    Returns the finding plus a closure that rewrites the file, or ``(None, None)`` when
    the file does not exist yet.
    """
    path = REPO_ROOT / relative_path
    if not path.is_file():
        if required:
            return Finding(name, path, expected, None), None
        return None, None

    text = read_text(path)
    match = re.search(pattern, text, re.MULTILINE)
    actual = match.group(1) if match else None
    finding = Finding(name, path, expected, actual)

    def write() -> None:
        if match:
            start, end = match.span(1)
            atomic_write_text(path, text[:start] + expected + text[end:])
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
    *,
    required: bool = True,
) -> tuple[Finding | None, Writer | None]:
    """A surface whose version is a top-level JSON key.

    Rewritten with a targeted regex rather than a json.dump round-trip, because dumping
    would reformat the whole file and produce a diff nobody can review.
    """
    path = REPO_ROOT / relative_path
    if not path.is_file():
        if required:
            return Finding(name, path, expected, None), None
        return None, None

    data = read_json_object(path)
    actual = data.get(key)
    finding = Finding(name, path, expected, actual if isinstance(actual, str) else None)

    def write() -> None:
        atomic_write_text(path, replace_top_level_json_string(path, key, expected))

    return finding, write


def _npm_lock_surface(expected: str) -> tuple[Finding | None, Writer | None]:
    """``viewer/package-lock.json`` restates the project's own version in two places.

    A lockfile whose self-version disagrees with its ``package.json`` makes
    ``npm ci --lockfile-only`` churn, and it puts a wrong version into the viewer bundle.
    Both occurrences are checked, not just the first.
    """
    path = REPO_ROOT / "viewer/package-lock.json"
    if not path.is_file():
        return Finding("viewer package-lock", path, expected, None), None

    data = read_json_object(path)
    top = data.get("version")
    packages = data.get("packages")
    if not isinstance(packages, dict):
        raise VersionError("viewer/package-lock.json: 'packages' must be an object.")
    root = packages.get("")
    if not isinstance(root, dict):
        raise VersionError("viewer/package-lock.json: packages[''] must be an object.")
    root_pkg = root.get("version")

    # Report drift unless *both* agree; surface whichever value is wrong.
    if top == expected and root_pkg == expected:
        actual: str | None = expected
    elif top != expected:
        actual = top if isinstance(top, str) else None
    else:
        actual = root_pkg if isinstance(root_pkg, str) else None

    finding = Finding("viewer package-lock", path, expected, actual)

    def write() -> None:
        text = read_text(path)
        top_pattern = re.compile(r'^(  "version"\s*:\s*")[^"]*(")', re.MULTILINE)
        new_text, top_count = top_pattern.subn(
            rf"\g<1>{expected}\g<2>", text, count=1
        )
        root_pattern = re.compile(
            r'(^  "packages"\s*:\s*\{\s*\n'
            r'    ""\s*:\s*\{.*?^      "version"\s*:\s*")[^"]*(")',
            re.MULTILINE | re.DOTALL,
        )
        new_text, root_count = root_pattern.subn(
            rf"\g<1>{expected}\g<2>", new_text, count=1
        )
        if top_count != 1 or root_count != 1:
            raise VersionError(
                "viewer/package-lock.json: cannot locate both project version fields."
            )
        atomic_write_text(path, new_text)

    return finding, write


def _cargo_lock_surface(expected: str) -> tuple[Finding | None, Writer | None]:
    """``viewer/src-tauri/Cargo.lock`` restates the ``melkor-viewer`` package version.

    Matched by package name so a dependency that happens to share the version string is
    never touched.
    """
    path = REPO_ROOT / "viewer/src-tauri/Cargo.lock"
    if not path.is_file():
        return Finding("Tauri Cargo.lock", path, expected, None), None

    text = read_text(path)
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
        atomic_write_text(
            path, pattern.sub(rf"\g<1>{expected}\g<3>", text, count=1)
        )

    return finding, write


def collect_surfaces(version: Version) -> tuple[list[Finding], dict[str, Writer]]:
    findings: list[Finding] = []
    writers: dict[str, Writer] = {}

    def add(result: tuple[Finding | None, Writer | None]) -> None:
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
        r"^\[project\]", read_text(pyproject), re.MULTILINE
    ):
        add(
            _regex_surface(
                "Python distribution",
                "pyproject.toml",
                r'^version\s*=\s*"([^"]+)"',
                version.pep440,
            )
        )
    add(
        _regex_surface(
            "Python _version.py",
            "python/melkor3d/_version.py",
            r'^__version__\s*=\s*"([^"]+)"',
            version.pep440,
            required=False,
        )
    )

    # Citation metadata. Uses the SemVer spelling, matching the git tag.
    add(
        _regex_surface(
            "CITATION.cff",
            "CITATION.cff",
            r'^version:\s*"?([^"\n]+?)"?\s*$',
            version.semver,
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
    text = read_text(path)

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


def check_no_hardcoded_version_in_scripts() -> list[str]:
    """Project wrappers must read VERSION instead of restating a release number."""

    errors: list[str] = []
    for relative in (
        "scripts/glomap_wrapper.sh",
        "scripts/lichtfeld_wrapper.sh",
        "scripts/opensplat_wrapper.sh",
        "scripts/pipeline.sh",
    ):
        path = REPO_ROOT / relative
        text = read_text(path)
        if re.search(r'^VERSION=["\'][0-9]', text, re.MULTILINE):
            errors.append(f"{relative}: states a literal project version.")
        if 'VERSION=\"$(<\"$' not in text or "/VERSION\")\"" not in text:
            errors.append(f"{relative}: does not read the root VERSION file.")
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

    text = read_text(path)

    if not re.search(r"^##\s+Unreleased\s*$", text, re.MULTILINE):
        errors.append(
            "CHANGELOG.md: no '## Unreleased' section.\n"
            "  Every user-visible change lands there before a release collects it."
        )

    if not version.is_prerelease:
        # The heading must be the exact stable version, not a prerelease of it. A trailing `\b`
        # is a word boundary, and there IS one between the "0" of "2.0.0" and the "-" of
        # "2.0.0-rc.2", so `^##\s+2\.0\.0\b` matched a changelog that only had a "## 2.0.0-rc.2"
        # section -- letting a stable release ship with no stable changelog entry. The next
        # character must be whitespace, a paren, or end-of-line, never "-" or ".".
        heading = rf"^##\s+{re.escape(version.core)}(?:$|[\s(])"
        if not re.search(heading, text, re.MULTILINE):
            errors.append(
                f"CHANGELOG.md: no '## {version.core}' section, but VERSION is a stable "
                f"release.\n"
                f"  A stable release must document what changed before it is tagged.\n"
                f"  (A '## {version.core}-rc.N' prerelease section does not satisfy this.)"
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
        pep440 = version.pep440
    except VersionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(f"VERSION            {version.raw}")
    print(f"  semver / npm     {version.semver}")
    print(f"  PEP 440          {pep440}")
    print(f"  prerelease       {'yes' if version.is_prerelease else 'no'}")
    print()

    try:
        findings, writers = collect_surfaces(version)
    except VersionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    structural_errors = (
        check_no_hardcoded_version_in_cmake()
        + check_no_hardcoded_version_in_scripts()
        + check_changelog(version)
    )
    if args.release_tag is not None:
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
