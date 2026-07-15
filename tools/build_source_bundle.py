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
    # Local scratch
    "test_data/",
    "tmp/",
    ".superstack/",
    ".claude/",
]

# Directory components that must never ship (matched case-insensitively as a whole component).
EXCLUDED_DIRS = [d.rstrip("/") for d in EXCLUDED_PATTERNS if d.endswith("/")]

# File extensions that must never ship, matched case-insensitively against the full suffix.
#
# Model weights are the ones that cause licence incidents -- a research-only checkpoint bundled
# into an MIT release. The list is deliberately broad and matched without regard to case, so
# `.PT` and `.SafeTensors` are caught as surely as `.pt` and `.safetensors`.
EXCLUDED_EXTENSIONS = [
    # PyTorch / generic tensor formats
    ".ckpt", ".pt", ".pth", ".safetensors", ".t7", ".pkl",
    # ONNX / TF / other frameworks
    ".onnx", ".pb", ".tflite", ".caffemodel", ".params",
    # Apple CoreML
    ".mlmodel", ".mlpackage",
    # NumPy / HDF5 arrays and blobs
    ".npy", ".npz", ".h5", ".hdf5", ".bin", ".weights", ".gguf", ".ggml",
    # Keys and certificates
    ".pem", ".key", ".p12", ".pfx", ".keystore", ".jks",
]

# Basename patterns for secret files, matched case-insensitively against the FILE NAME only
# (never the whole path), so a legitimate source file deeper in the tree is not caught by a
# substring collision. `.env` and every `.env.*` variant, and any name containing "credential"
# or "secret", are refused.
EXCLUDED_BASENAME_PREFIXES = ["id_rsa", ".env"]
EXCLUDED_BASENAME_CONTAINS = ["credential", "secret"]


def git_tracked_entries(ref: str) -> list[tuple[str, str]]:
    """Every tracked file at ``ref`` as (mode, path).

    Built from git, not the working tree, so uncommitted local state can never leak into a
    release. The mode is carried so symlinks (120000) can be recognised: ``git show`` of a
    symlink returns its *target path* as bytes, and storing that as a regular file would put a
    wrong, misleading file in the bundle.
    """
    out = subprocess.run(
        ["git", "ls-tree", "-r", "--full-tree", ref],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    entries: list[tuple[str, str]] = []
    for line in out.stdout.splitlines():
        if not line.strip():
            continue
        # Format: "<mode> <type> <sha>\t<path>"
        meta, _, path = line.partition("\t")
        mode = meta.split()[0]
        entries.append((mode, path))
    return sorted(entries, key=lambda e: e[1])


def exclusion_reason(path: str) -> str | None:
    """Why ``path`` must not ship, or None if the denylist does not object.

    Returns a reason string so the report can say *why* something was dropped, which matters
    when the thing dropped is a weight or a secret rather than a build artifact.
    """
    lower = path.lower()
    basename = path.rsplit("/", 1)[-1].lower()
    components = lower.split("/")

    for directory in EXCLUDED_DIRS:
        if directory.lower() in components:
            return f"excluded directory '{directory}/'"

    for ext in EXCLUDED_EXTENSIONS:
        if lower.endswith(ext):
            return f"excluded extension '{ext}' (weight/key/cert)"

    for prefix in EXCLUDED_BASENAME_PREFIXES:
        if basename == prefix or basename.startswith(prefix + "."):
            return f"secret-like filename '{basename}'"

    for needle in EXCLUDED_BASENAME_CONTAINS:
        if needle in basename:
            return f"secret-like filename '{basename}'"

    return None


def is_allowed(path: str) -> bool:
    # The denylist is defence in depth over the allowlist: even a path under an allowlisted
    # directory is dropped if it looks like a weight or a secret.
    if exclusion_reason(path) is not None:
        return False

    for allowed in ALLOWLIST:
        if allowed.endswith("/"):
            if path.startswith(allowed):
                return True
        elif path == allowed:
            return True
    return False


def select(ref: str) -> tuple[list[str], list[tuple[str, str]]]:
    """Returns (included_paths, excluded_with_reason).

    Symlinks are never included: `git show` would store their target path as file content,
    silently producing a wrong file. They are reported as excluded with a reason.
    """
    included: list[str] = []
    excluded: list[tuple[str, str]] = []
    for mode, path in git_tracked_entries(ref):
        if mode == "120000":
            excluded.append((path, "symlink (target would be stored as file content)"))
            continue
        if is_allowed(path):
            included.append(path)
        else:
            reason = exclusion_reason(path) or "not on the allowlist"
            excluded.append((path, reason))
    return included, excluded


def file_bytes(ref: str, path: str) -> bytes:
    out = subprocess.run(
        ["git", "show", f"{ref}:{path}"],
        cwd=REPO_ROOT,
        capture_output=True,
        check=True,
    )
    return out.stdout


def _compress(tar_bytes: bytes, output: Path) -> bytes:
    """Compress the tar payload to match the OUTPUT filename, deterministically.

    The output must not lie about its contents. A previous version always wrote an uncompressed
    tar but happily accepted a ``.tar.zst`` name, so the file's extension claimed a compression
    that was never applied. The suffix now decides the encoding, and an unsupported one is a
    hard error rather than a silent uncompressed write.
    """
    name = output.name.lower()
    if name.endswith(".tar"):
        return tar_bytes
    if name.endswith(".tar.gz") or name.endswith(".tgz"):
        import gzip
        # mtime=0 and a fixed level keep the gzip container byte-identical across runs.
        return gzip.compress(tar_bytes, compresslevel=9, mtime=0)
    if name.endswith(".tar.zst"):
        # Python's stdlib has no zstd, so shell out. zstd at a fixed level is deterministic for
        # a fixed input and a fixed zstd version; the release process pins the tool version.
        try:
            proc = subprocess.run(
                ["zstd", "-q", "-19", "--no-progress", "-c"],
                input=tar_bytes,
                capture_output=True,
                check=True,
            )
        except FileNotFoundError as exc:
            raise SystemExit(
                "error: output ends in .tar.zst but the 'zstd' tool is not installed.\n"
                "Install zstd, or choose a .tar or .tar.gz output name."
            ) from exc
        return proc.stdout
    raise SystemExit(
        f"error: unsupported output extension for {output.name!r}.\n"
        "Use .tar, .tar.gz, or .tar.zst so the file's name matches its contents."
    )


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

    payload = _compress(raw.getvalue(), output)
    output.write_bytes(payload)
    # Hash the FINAL file the user receives, not the uncompressed intermediate, so the digest
    # verifies the artifact that actually ships.
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

        # Any weight, key, secret, or symlink that was dropped is surfaced explicitly and by
        # name, not folded into a directory count. A silently dropped weight reads as "we
        # covered everything" when we did not, and a weight that was NOT dropped is a licence
        # incident -- so both directions of this must be visible.
        notable = [
            (p, r) for p, r in excluded
            if "weight" in r or "secret" in r or "symlink" in r or "key" in r
        ]
        if notable:
            print("Excluded for safety (verify none of these should have shipped elsewhere):")
            for path, reason in notable:
                print(f"  {path}  -- {reason}")
            print()

        if excluded:
            print("Excluded, grouped by top-level directory:")
            shown: dict[str, int] = {}
            for path, _reason in excluded:
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
