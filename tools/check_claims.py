#!/usr/bin/env python3
"""Reject unsupported public superlative and performance claims.

A risky claim must cite upstream evidence, link benchmark evidence, or include a justified
``claim-ok:`` marker. The default scan covers active first-party public text surfaces.

Use ``python3 tools/check_claims.py [FILE ...]``.
"""

from __future__ import annotations

import argparse
import os
import re
import stat
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MAX_PUBLIC_TEXT_BYTES = 4 * 1024 * 1024

# These root files define the public project contract.
LINTED_FILES = [
    "README.md",
    "ROADMAP.md",
    "SUPPORT.md",
    "SECURITY.md",
    "CONTRIBUTING.md",
    "GOVERNANCE.md",
    "MAINTAINERS.md",
]
# Check all first-party guides, references, release notes, and viewer documents.
LINTED_GLOBS = [
    "benchmarks/**/*.md",
    "docs/**/*.md",
    "fuzz/corpus/**/*.md",
    "release/**/*.md",
    "scripts/*.sh",
    "src/main.cpp",
    "viewer/*.html",
    "viewer/*.js",
    "viewer/*.md",
]

# Audits and history must preserve the original finding text.
EXCLUDED_SUBSTRINGS = ["docs/audit/", "docs/history/", "docs/reviews/"]

# Banned phrases, as case-insensitive regexes with word boundaries where sensible.
BANNED = [
    r"\bSOTA\b",
    r"state[\s-]of[\s-]the[\s-]art",
    r"\d+\s*[-–]\s*\d+\s*[x×]\b",  # "10-100x", "10–100×"
    r"\b\d+\s*[x×]\s+faster\b",
    r"\bfastest\b",
    r"\bbest[\s-]in[\s-]class\b",
    r"\bproduction[\s-]grade\b",
    r"\bproduction[\s-]quality\b",
    r"\blossless\b",
    r"\buniversal(?:ly)?\b",
    r"\ball formats\b",
    r"\bblazing(?:ly)?\b",
    r"\bworld[\s-]class\b",
]

# Cues that turn a banned phrase into a permitted, attributed one.
ATTRIBUTION_CUES = [
    "reported by", "upstream", "the authors", "authors'", "author's", "per ", "according to",
    "benchmark", "measured", "claim", "as documented by", "documented in", "see benchmarks",
]

CLAIM_OK = re.compile(r"claim-ok:\s*\S")


def linted_paths() -> list[Path]:
    paths: list[Path] = []
    for rel in LINTED_FILES:
        p = REPO_ROOT / rel
        if p.is_file():
            paths.append(p)
    for pattern in LINTED_GLOBS:
        paths.extend(sorted(REPO_ROOT.glob(pattern)))
    # De-dupe, drop excluded.
    seen: set[Path] = set()
    result: list[Path] = []
    for p in paths:
        rel = str(p.relative_to(REPO_ROOT))
        if any(sub in rel for sub in EXCLUDED_SUBSTRINGS):
            continue
        if p not in seen:
            seen.add(p)
            result.append(p)
    return result


def read_public_text(path: Path) -> str:
    """Read one bounded regular UTF-8 public text file."""
    if path.is_symlink():
        raise ValueError(f"public text input must not be a symbolic link: {path}")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as handle:
        initial = os.fstat(handle.fileno())
        if not stat.S_ISREG(initial.st_mode) or initial.st_size > MAX_PUBLIC_TEXT_BYTES:
            raise ValueError(f"public text input must be a bounded regular file: {path}")
        raw = handle.read(MAX_PUBLIC_TEXT_BYTES + 1)
        final = os.fstat(handle.fileno())
        if (
            len(raw) != initial.st_size
            or final.st_dev != initial.st_dev
            or final.st_ino != initial.st_ino
            or final.st_size != initial.st_size
            or final.st_mtime_ns != initial.st_mtime_ns
            or final.st_ctime_ns != initial.st_ctime_ns
        ):
            raise ValueError(f"public text input changed while it was read: {path}")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError(f"public text input is not UTF-8: {path}") from error


def line_is_allowed(line: str, match_start: int | None = None) -> bool:
    lower = line.lower()
    if CLAIM_OK.search(line):
        return True
    if match_start is not None:
        prefix = re.sub(r"[*_`]", "", lower[:match_start])
        if re.search(r"\b(?:not|never)\s+(?:\S+\s+)?$", prefix):
            return True
    return any(cue in lower for cue in ATTRIBUTION_CUES)


def scan(path: Path) -> list[tuple[int, str, str]]:
    findings: list[tuple[int, str, str]] = []
    for lineno, line in enumerate(read_public_text(path).splitlines(), start=1):
        for pattern in BANNED:
            m = re.search(pattern, line, re.IGNORECASE)
            if m and not line_is_allowed(line, m.start()):
                findings.append((lineno, m.group(0), line.strip()))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true", help="list the linted files and exit")
    parser.add_argument("files", nargs="*", type=Path, help="check only these files")
    args = parser.parse_args()

    paths = []
    for path in args.files:
        candidate = path if path.is_absolute() else REPO_ROOT / path
        if candidate.is_symlink():
            parser.error(f"file must not be a symbolic link: {path}")
        if not candidate.is_file():
            parser.error(f"file does not exist: {path}")
        paths.append(candidate.absolute())
    if not paths:
        paths = linted_paths()

    if args.list:
        print("Claim lint covers:")
        for p in paths:
            try:
                display_path = p.relative_to(REPO_ROOT)
            except ValueError:
                display_path = p
            print(f"  {display_path}")
        return 0

    total = 0
    for path in paths:
        try:
            findings = scan(path)
        except (OSError, ValueError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
        for lineno, phrase, line in findings:
            total += 1
            try:
                rel = path.relative_to(REPO_ROOT)
            except ValueError:
                rel = path
            print(f"{rel}:{lineno}: unqualified claim {phrase!r}")
            print(f"    {line}")

    if total:
        print(
            f"\n{total} unqualified claim(s) found.\n"
            "Remove each claim or attribute it to an upstream source on the same line.\n"
            "You can also link benchmark evidence or add a justified <!-- claim-ok: reason --> marker.",
            file=sys.stderr,
        )
        return 1

    print(f"No unqualified claims in {len(paths)} public surface(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
