#!/usr/bin/env python3
"""Lint public-facing prose for unqualified superlative and performance claims.

The rule the blueprint sets: a quantitative or superlative claim -- "SOTA", "10-100x faster",
"fastest", "lossless", "production-grade" -- may appear in Melkor's public surfaces only when it
is either (a) attributed to an upstream source rather than stated as a Melkor-reproduced fact, or
(b) backed by a benchmark record. A bare "state of the art" in a README is marketing, and it is
exactly the kind of claim that ages into a lie.

This does not judge whether a claim is true. It enforces that a claim carries its evidence or its
attribution on the same line, so a reader can tell the difference between "we measured this" and
"we are hoping you do not check".

Escape hatches, both explicit and visible in the diff:
  - A line may carry an inline marker  <!-- claim-ok: why -->  with a real justification.
  - A banned phrase is allowed on a line that also carries an attribution cue
    ("reported by", "upstream", "the authors", "per ", "according to", a benchmark link, ...),
    because that is the blueprint's sanctioned form for an upstream figure.

Usage::

    python3 tools/check_claims.py            # lint the default surfaces
    python3 tools/check_claims.py --list     # show which files are linted
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# The surfaces a prospective user reads to decide whether to trust the project. These are the
# ones a marketing claim does the most damage in.
#
# The detailed pipeline wrapper docs (docs/PIPELINE.md, docs/GLOMAP_WRAPPER.md, ...) are NOT here
# yet: they describe the deprecated standalone-GLOMAP flow and are scheduled for replacement by
# the pinned adapter protocol (P0-14 / WP18). They will be brought under this lint when they are
# rewritten. Excluding them is recorded, not silent.
LINTED_FILES = [
    "README.md",
    "ROADMAP.md",
    "SUPPORT.md",
    "SECURITY.md",
    "CONTRIBUTING.md",
    "docs/index.md",
    "docs/quickstart.md",
]
LINTED_GLOBS = [
    "docs/reference/*.md",
    "docs/formats/*.md",
    "docs/security/*.md",
]

# Paths that legitimately contain the banned words: audits quote findings, this tool names the
# words it bans, and the changelog records history.
EXCLUDED_SUBSTRINGS = ["docs/audit/", "docs/history/", "docs/reviews/", "tools/check_claims.py"]

# Banned phrases, as case-insensitive regexes with word boundaries where sensible.
BANNED = [
    r"\bSOTA\b",
    r"state[\s-]of[\s-]the[\s-]art",
    r"\d+\s*[-–]\s*\d+\s*[x×]\b",  # "10-100x", "10–100×"
    r"\b\d+\s*[x×]\s+faster\b",
    r"\bfastest\b",
    r"\bbest[\s-]in[\s-]class\b",
    r"\bproduction[\s-]grade\b",
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


def line_is_allowed(line: str) -> bool:
    lower = line.lower()
    if CLAIM_OK.search(line):
        return True
    return any(cue in lower for cue in ATTRIBUTION_CUES)


def scan(path: Path) -> list[tuple[int, str, str]]:
    findings: list[tuple[int, str, str]] = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        for pattern in BANNED:
            m = re.search(pattern, line, re.IGNORECASE)
            if m and not line_is_allowed(line):
                findings.append((lineno, m.group(0), line.strip()))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true", help="list the linted files and exit")
    args = parser.parse_args()

    paths = linted_paths()

    if args.list:
        print("Claim lint covers:")
        for p in paths:
            print(f"  {p.relative_to(REPO_ROOT)}")
        return 0

    total = 0
    for path in paths:
        findings = scan(path)
        for lineno, phrase, line in findings:
            total += 1
            rel = path.relative_to(REPO_ROOT)
            print(f"{rel}:{lineno}: unqualified claim {phrase!r}")
            print(f"    {line}")

    if total:
        print(
            f"\n{total} unqualified claim(s) found.\n"
            "Each must be removed, attributed to its upstream source on the same line "
            "(e.g. 'reported by the GLOMAP authors'), backed by a benchmark link, or marked "
            "with an inline  <!-- claim-ok: reason -->  that justifies it.",
            file=sys.stderr,
        )
        return 1

    print(f"No unqualified claims in {len(paths)} public surface(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
