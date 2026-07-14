#!/usr/bin/env python3
"""Build the audited source bundle from an explicit allowlist.

The dangerous way to build a source release is "archive everything tracked". It works right
up until someone commits a model checkpoint, a dataset, a local scratch directory, or a
research snapshot under terms incompatible with the core licence — and then the release
quietly ships it. The failure is silent, and it is discovered by the recipient.

So this bundles an **allowlist**: a path is included because it was named, not because it
happened to be present. Anything new is excluded until someone deliberately adds it.

The bundle is deterministic. Given the same commit it produces byte-identical output:
entries are sorted, uid/gid/uname/gname are zeroed, permissions are normalized to 0644/0755,
and every mtime is set from ``SOURCE_DATE_EPOCH``. That is what makes the release gate's
"build it twice and compare" check meaningful.

Usage::

    python3 tools/build_source_bundle.py --output dist/melkor-2.0.0-source.tar.zst
    python3 tools/build_source_bundle.py --check      # report what would be included
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import subprocess
import sys
import tarfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Every path that belongs in a source release. A directory includes its tracked contents.
#
# Adding a line here is a deliberate act with licence and size consequences. Read
# EXCLUDED_PATTERNS below before you add one.
ALLOWLIST = [
    # Build system
    "CMakeLists.txt",
    "CMakePresets.json",
    "VERSION",
    "cmake/",
    # Source
    "include/",
    "src/",
    "apps/",
    "python/",
    "pipeline/",
    "viewer/",
    "packaging/",
    # Data that defines behaviour
    "profiles/",
    "schemas/",
    "examples/",
    # Tests and fuzzing
    "tests/",
    "fuzz/",
    # Benchmarks: the manifests and runners, never the datasets
    "benchmarks/README.md",
    "benchmarks/manifests/",
    "benchmarks/scripts/",
    "benchmarks/schema/",
    # Documentation
    "docs/",
    "mkdocs.yml",
    "assets/",  # logos referenced by README.md; without these the bundle's README is broken
    # Tooling
    "tools/",
    # Pinned dependencies and their provenance
    "third_party/",
    # Release metadata
    "release/manifests/",
    "release/components.json",
    # Project metadata
    "pyproject.toml",
    "CITATION.cff",
    "LICENSE",
    "NOTICE",
    "THIRD_PARTY_LICENSES.md",
    "MAINTAINERS.md",
    "README.md",
    "CHANGELOG.md",
    "CONTRIBUTING.md",
    "CODE_OF_CONDUCT.md",
    "SECURITY.md",
    "SUPPORT.md",
    "GOVERNANCE.md",
    "ROADMAP.md",
    ".clang-format",
    ".clang-tidy",
    ".editorconfig",
]

# Defence in depth. Even if one of these somehow sits under an allowlisted directory, it does
# not ship. The allowlist alone should be sufficient; this exists because "should be" is not
# a property you want to bet a licence violation on.
EXCLUDED_PATTERNS = [
    # Build outputs and caches
    "build/",
    "build-",
    "dist/",
    "node_modules/",
    "__pycache__/",
    ".ruff_cache/",
    ".pytest_cache/",
    "target/",
    ".venv/",
    "venv/",
    # Model weights and large data. These are the ones that cause licence incidents.
    ".ckpt",
    ".pt",
    ".pth",
    ".safetensors",
    ".onnx",
    ".mlmodel",
    ".mlpackage",
    ".bin",
    ".weights",
    # Local scratch
    "test_data/",
    "tmp/",
    ".superstack/",
    ".claude/",
    # Secrets
    ".env",
    "id_rsa",
    ".pem",
    ".key",
]


def git_tracked_files(ref: str) -> list[str]:
    """Every file tracked at ``ref``. The bundle is built from git, not the working tree,
    so uncommitted local state can never leak into a release."""
    out = subprocess.run(
        ["git", "ls-tree", "-r", "--name-only", "--full-tree", ref],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return sorted(line for line in out.stdout.splitlines() if line)


def is_allowed(path: str) -> bool:
    for pattern in EXCLUDED_PATTERNS:
        if pattern.endswith("/"):
            if path.startswith(pattern) or f"/{pattern}" in f"/{path}":
                return False
        elif pattern.startswith("."):
            if path.endswith(pattern):
                return False
        elif pattern in path:
            return False

    for allowed in ALLOWLIST:
        if allowed.endswith("/"):
            if path.startswith(allowed):
                return True
        elif path == allowed:
            return True
    return False


def select(ref: str) -> tuple[list[str], list[str]]:
    tracked = git_tracked_files(ref)
    included = [p for p in tracked if is_allowed(p)]
    excluded = [p for p in tracked if not is_allowed(p)]
    return included, excluded


def file_bytes(ref: str, path: str) -> bytes:
    out = subprocess.run(
        ["git", "show", f"{ref}:{path}"],
        cwd=REPO_ROOT,
        capture_output=True,
        check=True,
    )
    return out.stdout


def build(ref: str, output: Path, source_date_epoch: int) -> str:
    included, _ = select(ref)
    output.parent.mkdir(parents=True, exist_ok=True)

    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for path in included:  # already sorted -> deterministic entry order
            data = file_bytes(ref, path)
            info = tarfile.TarInfo(name=path)
            info.size = len(data)
            # Everything below is what makes two builds of one commit byte-identical.
            info.mtime = source_date_epoch
            info.mode = 0o755 if path.endswith(".sh") or path.endswith(".py") else 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.type = tarfile.REGTYPE
            tar.addfile(info, io.BytesIO(data))

    payload = raw.getvalue()
    output.write_bytes(payload)
    return hashlib.sha256(payload).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ref", default="HEAD", help="git ref to bundle (default: HEAD)")
    parser.add_argument("--output", type=Path, help="output tar path")
    parser.add_argument(
        "--check",
        action="store_true",
        help="report what would be included and excluded, without writing",
    )
    args = parser.parse_args()

    included, excluded = select(args.ref)

    if args.check or not args.output:
        print(f"Source bundle from {args.ref}\n")
        print(f"  included: {len(included)} files")
        print(f"  excluded: {len(excluded)} files\n")

        if excluded:
            print("Excluded (not on the allowlist):")
            shown: dict[str, int] = {}
            for path in excluded:
                top = path.split("/")[0] if "/" in path else path
                shown[top] = shown.get(top, 0) + 1
            for top, count in sorted(shown.items(), key=lambda kv: -kv[1]):
                print(f"  {count:>5}  {top}")
            print(
                "\nIf something above belongs in a source release, add it to ALLOWLIST\n"
                "deliberately. Do not widen the allowlist to make a warning go away."
            )
        return 0

    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    if epoch == 0:
        print(
            "warning: SOURCE_DATE_EPOCH is unset; using 0 so the bundle stays reproducible.",
            file=sys.stderr,
        )

    digest = build(args.ref, args.output, epoch)
    print(f"wrote {args.output}")
    print(f"  files:  {len(included)}")
    print(f"  sha256: {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
