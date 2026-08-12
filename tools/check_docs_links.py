#!/usr/bin/env python3
"""Check local links and anchors in each repository Markdown file."""

from __future__ import annotations

import argparse
import os
import re
import stat
import subprocess
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import unquote, urlsplit


REPO_ROOT = Path(__file__).resolve().parent.parent
MAX_MARKDOWN_BYTES = 4 * 1024 * 1024
INLINE_LINK = re.compile(r"!?\[[^\]]*\]\((?P<target><[^>]+>|[^\s)]+)")
REFERENCE_LINK = re.compile(r"^\s*\[[^\]]+\]:\s*(?P<target><[^>]+>|\S+)")
HTML_LINK = re.compile(r"(?:href|src)=[\"'](?P<target>[^\"']+)[\"']", re.IGNORECASE)
HEADING = re.compile(r"^\s{0,3}#{1,6}\s+(?P<text>.+?)\s*#*\s*$")
SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    message: str


def read_markdown(path: Path) -> str:
    """Read one bounded regular UTF-8 Markdown file."""
    if path.is_symlink():
        raise ValueError(f"Markdown input must not be a symbolic link: {path}")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as handle:
        initial = os.fstat(handle.fileno())
        if not stat.S_ISREG(initial.st_mode) or initial.st_size > MAX_MARKDOWN_BYTES:
            raise ValueError(f"Markdown input must be a bounded regular file: {path}")
        raw = handle.read(MAX_MARKDOWN_BYTES + 1)
        final = os.fstat(handle.fileno())
        if (
            len(raw) != initial.st_size
            or final.st_dev != initial.st_dev
            or final.st_ino != initial.st_ino
            or final.st_size != initial.st_size
            or final.st_mtime_ns != initial.st_mtime_ns
            or final.st_ctime_ns != initial.st_ctime_ns
        ):
            raise ValueError(f"Markdown input changed while it was read: {path}")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError(f"Markdown input is not UTF-8: {path}") from error


def tracked_markdown(root: Path = REPO_ROOT) -> list[Path]:
    """Return each tracked or proposed Markdown file below ``root``."""
    result = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", "*.md"],
        cwd=root,
        check=True,
        capture_output=True,
    )
    try:
        paths = [root / name.decode("utf-8", "strict") for name in result.stdout.split(b"\0") if name]
    except UnicodeDecodeError as error:
        raise ValueError("Git returned a non-UTF-8 Markdown path") from error
    links = [path for path in paths if path.is_symlink()]
    if links:
        raise ValueError(f"Markdown files must not be symbolic links: {links[0]}")
    return [path for path in paths if path.is_file()]


def github_slug(text: str) -> str:
    """Return the GitHub-style base anchor for one heading."""
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r"[`*_~]", "", text)
    text = re.sub(r"!?\[([^\]]+)\]\([^)]*\)", r"\1", text)
    text = unicodedata.normalize("NFKC", text).strip().lower()
    text = "".join(char for char in text if char.isalnum() or char in " _-")
    return re.sub(r"\s", "-", text)


def anchors(path: Path) -> set[str]:
    """Return each heading anchor in one Markdown file."""
    found: set[str] = set()
    duplicates: dict[str, int] = {}
    in_fence = False
    for line in read_markdown(path).splitlines():
        if line.lstrip().startswith(("```", "~~~")):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = HEADING.match(line)
        if not match:
            continue
        base = github_slug(match.group("text"))
        count = duplicates.get(base, 0)
        found.add(base if count == 0 else f"{base}-{count}")
        duplicates[base] = count + 1
    return found


def path_case(path: Path, root: Path) -> str:
    """Classify a path as exact, incorrect case, or missing."""
    try:
        relative = path.relative_to(root)
    except ValueError:
        return "missing"
    current = root
    exact = True
    for part in relative.parts:
        try:
            names = [entry.name for entry in current.iterdir()]
        except OSError:
            return "missing"
        if part in names:
            selected = part
        else:
            matches = [name for name in names if name.casefold() == part.casefold()]
            if len(matches) != 1:
                return "missing"
            selected = matches[0]
            exact = False
        current /= selected
    return "exact" if exact else "incorrect"


def targets(path: Path) -> list[tuple[int, str]]:
    """Return local and remote link targets outside fenced code blocks."""
    result: list[tuple[int, str]] = []
    in_fence = False
    for number, line in enumerate(read_markdown(path).splitlines(), start=1):
        if line.lstrip().startswith(("```", "~~~")):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for pattern in (INLINE_LINK, REFERENCE_LINK, HTML_LINK):
            for match in pattern.finditer(line):
                result.append((number, match.group("target").strip("<>")))
    return result


def check_markdown(paths: list[Path], root: Path = REPO_ROOT) -> list[Finding]:
    """Check local targets and Markdown anchors."""
    findings: list[Finding] = []
    anchor_cache: dict[Path, set[str]] = {}
    for source in paths:
        for line, raw_target in targets(source):
            if not raw_target or SCHEME.match(raw_target) or raw_target.startswith(("//", "{")):
                continue
            try:
                parsed = urlsplit(raw_target)
            except ValueError:
                findings.append(Finding(source, line, f"link target is invalid: {raw_target}"))
                continue
            path_text = unquote(parsed.path)
            fragment = unquote(parsed.fragment)
            if not path_text:
                destination = source
            elif path_text.startswith("/"):
                destination = root / path_text.lstrip("/")
            else:
                destination = source.parent / path_text
            destination = Path(os.path.normpath(destination.absolute()))
            try:
                destination.resolve(strict=False).relative_to(root.resolve())
            except ValueError:
                findings.append(Finding(source, line, f"local link escapes the repository: {raw_target}"))
                continue
            case = path_case(destination, root)
            if case == "incorrect":
                findings.append(Finding(source, line, f"local link has incorrect path case: {raw_target}"))
                continue
            if case == "missing" or not destination.exists():
                findings.append(Finding(source, line, f"local link target does not exist: {raw_target}"))
                continue
            if fragment and destination.suffix.lower() == ".md":
                expected = unicodedata.normalize("NFKC", fragment).lower()
                available = anchor_cache.setdefault(destination, anchors(destination))
                if expected not in available:
                    findings.append(Finding(source, line, f"Markdown anchor does not exist: {raw_target}"))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    try:
        paths = tracked_markdown()
        findings = check_markdown(paths)
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        print(f"error: documentation link check failed: {error}", file=sys.stderr)
        return 2
    for finding in findings:
        relative = finding.path.relative_to(REPO_ROOT)
        print(f"{relative}:{finding.line}: {finding.message}")
    if findings:
        print(f"\n{len(findings)} documentation link error(s).", file=sys.stderr)
        return 1
    print(f"All local links and anchors pass in {len(paths)} repository Markdown files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
