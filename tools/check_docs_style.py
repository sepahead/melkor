#!/usr/bin/env python3
"""Check first-party Markdown against the repository writing rules."""

from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

EXCLUDED_PREFIXES = (
    "docs/audit/",
    "docs/history/",
    "docs/reviews/",
    "third_party/",
)
EXCLUDED_FILES = {
    "AGENTS.md",
    "CHANGELOG.md",
    "CLAUDE.md",
    "CODE_OF_CONDUCT.md",
    "THIRD_PARTY_LICENSES.md",
    "viewer/THIRD_PARTY_NOTICES.md",
}

BRITISH_SPELLINGS = re.compile(
    r"\b(?:behaviour|behaviours|catalogue|centre|colour|colours|honour|honoured|"
    r"characterise|characterised|defence|grey|initialise|initialised|initialisation|"
    r"licence|licences|neighbour|neighbours|"
    r"neighbourhood|normalise|normalised|normalisation|optimise|optimised|"
    r"optimisation|prioritise|prioritised|recognise|recognised|unrecognised)\b",
    re.IGNORECASE,
)
CONTRACTIONS = re.compile(
    r"\b(?:can't|won't|isn't|aren't|wasn't|weren't|doesn't|don't|didn't|hasn't|"
    r"haven't|hadn't|shouldn't|wouldn't|couldn't|mustn't|it's|that's|there's|"
    r"here's|we're|you're|they're|I've|we've|you've|they've|I'll|we'll|you'll|"
    r"they'll|I'd|we'd|you'd|they'd)\b",
    re.IGNORECASE,
)
LINK_OR_CODE = re.compile(r"`[^`]*`|\[[^]]*\]\([^)]*\)|<https?://[^>]+>|https?://\S+")
WORDS = re.compile(r"\b[A-Za-z0-9][A-Za-z0-9'_.+/-]*\b")
LIST_ITEM = re.compile(r"^\s*(?:[-*+] |\d+\. )(.*)")


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    message: str


def tracked_markdown(root: Path = REPO_ROOT) -> list[Path]:
    output = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", "*.md"],
        cwd=root,
    ).decode("utf-8")
    result = []
    for relative in output.split("\0"):
        if not relative or relative in EXCLUDED_FILES:
            continue
        if any(relative.startswith(prefix) for prefix in EXCLUDED_PREFIXES):
            continue
        result.append(root / relative)
    return result


def clean_prose(text: str) -> str:
    return LINK_OR_CODE.sub(" TERM ", text)


def sentence_word_counts(text: str) -> list[int]:
    sentences = re.split(r"(?<=[.!?])(?:[\"'”’)*_`]+)?\s+", clean_prose(text))
    return [len(WORDS.findall(sentence)) for sentence in sentences if sentence.strip()]


def check_markdown(paths: list[Path], root: Path = REPO_ROOT) -> list[Finding]:
    findings: list[Finding] = []
    for path in paths:
        in_fence = False
        paragraph: list[str] = []
        paragraph_line = 0

        def flush_paragraph(line_number: int, current_path: Path = path) -> None:
            nonlocal paragraph
            if not paragraph:
                return
            counts = sentence_word_counts(" ".join(line.strip() for line in paragraph))
            if len(counts) > 6:
                findings.append(
                    Finding(
                        current_path,
                        line_number,
                        f"paragraph has {len(counts)} sentences",
                    )
                )
            for count in counts:
                if count > 25:
                    findings.append(
                        Finding(
                            current_path,
                            line_number,
                            f"descriptive sentence has {count} words",
                        )
                    )
            paragraph = []

        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("```"):
                flush_paragraph(paragraph_line)
                in_fence = not in_fence
                continue
            if in_fence:
                continue

            prose = clean_prose(line)
            if match := BRITISH_SPELLINGS.search(prose):
                findings.append(Finding(path, lineno, f"use American spelling for {match.group(0)!r}"))
            if match := CONTRACTIONS.search(prose):
                findings.append(Finding(path, lineno, f"replace contraction {match.group(0)!r}"))
            if ";" in prose:
                findings.append(Finding(path, lineno, "replace the semicolon in prose"))

            if not stripped or stripped == ">" or stripped.startswith(("#", "<", "---")):
                flush_paragraph(paragraph_line)
                continue

            if stripped.startswith("|"):
                flush_paragraph(paragraph_line)
                if not re.match(r"^\|?\s*:?-+", stripped):
                    for cell in stripped.strip("|").split("|"):
                        for count in sentence_word_counts(cell):
                            if count > 25:
                                findings.append(
                                    Finding(path, lineno, f"table sentence has {count} words")
                                )
                continue

            if match := LIST_ITEM.match(line):
                flush_paragraph(paragraph_line)
                for count in sentence_word_counts(match.group(1)):
                    if count > 20:
                        findings.append(
                            Finding(path, lineno, f"list sentence has {count} words")
                        )
                continue

            if not paragraph:
                paragraph_line = lineno
            paragraph.append(line)

        flush_paragraph(paragraph_line)

    return findings


def main() -> int:
    findings = check_markdown(tracked_markdown())
    for finding in findings:
        relative = finding.path.relative_to(REPO_ROOT)
        print(f"{relative}:{finding.line}: {finding.message}", file=sys.stderr)
    if findings:
        print(f"Documentation style check found {len(findings)} issue(s).", file=sys.stderr)
        return 1
    print(f"Documentation style passes in {len(tracked_markdown())} first-party Markdown files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
