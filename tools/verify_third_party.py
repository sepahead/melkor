#!/usr/bin/env python3
"""Verify the vendored third-party sources against third_party/manifest.lock.json.

Why a content digest rather than an archive digest
--------------------------------------------------
The obvious design is to pin the SHA-256 of the upstream release tarball. It does not
actually work for GitHub-generated archives: the ``/archive/<sha>.tar.gz`` endpoint
recompresses on the fly, and GitHub has changed that compression before, silently
invalidating pinned hashes across the ecosystem. A hash that can change while the source
does not is worse than no hash, because it trains people to update it without looking.

So this tool pins two things that genuinely cannot drift:

1. ``revision`` — the upstream commit SHA. Git content-addresses its objects, so a commit
   SHA *is* a cryptographic commitment to a source tree.
2. ``vendored.content_sha256`` — a digest computed over the vendored files themselves:
   sorted relative path, length, and bytes. It is independent of archive format,
   compression, timestamps, and file ordering, and it is what CI verifies against the tree
   that actually gets compiled.

The archive URL and its digest are still recorded for offline dependency caches, but the
revision is authoritative and this tool does not require the archive to be present.

Local patches
-------------
A vendored dependency may legitimately carry local patches, but they must be *visible*.
Every patch is a numbered file under ``third_party/patches/<id>/`` with a rationale, and the
lock records its digest. An undocumented fork — upstream code silently edited in place —
is rejected: it is indistinguishable from a supply-chain compromise, and it makes upgrading
upstream a guessing game.

Usage::

    python3 tools/verify_third_party.py --check      # CI
    python3 tools/verify_third_party.py --print-digests
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LOCK_PATH = REPO_ROOT / "third_party" / "manifest.lock.json"

SUPPORTED_SCHEMA_VERSION = 1


def content_digest(root: Path, relative_files: list[str]) -> str:
    """Deterministic digest over a set of files.

    Independent of archive format, compression, mtimes, and directory iteration order. The
    length is folded in so that concatenation of two files cannot collide with a single
    file holding their concatenation.
    """
    digest = hashlib.sha256()
    for relative in sorted(relative_files):
        data = (root / relative).read_bytes()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(len(data)).encode("ascii"))
        digest.update(b"\0")
        digest.update(data)
    return digest.hexdigest()


def tracked_files(path: Path) -> list[str]:
    """Every file under ``path``, as paths relative to it."""
    if path.is_file():
        return [path.name]
    return sorted(
        str(p.relative_to(path))
        for p in path.rglob("*")
        if p.is_file() and ".git" not in p.parts
    )


def file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_lock() -> dict:
    if not LOCK_PATH.is_file():
        raise SystemExit(f"missing dependency lock: {LOCK_PATH}")

    lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    schema = lock.get("schema_version")
    if schema != SUPPORTED_SCHEMA_VERSION:
        raise SystemExit(
            f"{LOCK_PATH}: schema_version {schema!r} is not supported "
            f"(this tool understands {SUPPORTED_SCHEMA_VERSION})"
        )
    return lock


def check(lock: dict) -> list[str]:
    errors: list[str] = []

    for dep in lock["dependencies"]:
        dep_id = dep["id"]

        # ---- Identity must be immutable ------------------------------------------------
        revision = dep.get("revision")
        if not isinstance(revision, str) or len(revision) != 40:
            errors.append(
                f"{dep_id}: revision must be a full 40-character commit SHA, got "
                f"{revision!r}. A tag or branch name is mutable and cannot pin a build."
            )

        # ---- The vendored tree must match the recorded digest ---------------------------
        vendored = dep.get("vendored")
        if vendored is None:
            continue  # A declared-but-not-vendored dependency has nothing local to verify.

        path = REPO_ROOT / vendored["path"]
        if not path.exists():
            errors.append(f"{dep_id}: vendored path does not exist: {vendored['path']}")
            continue

        files = tracked_files(path)
        if not files:
            errors.append(f"{dep_id}: vendored path {vendored['path']} contains no files")
            continue

        actual = content_digest(path, files)
        expected = vendored["content_sha256"]
        if actual != expected:
            errors.append(
                f"{dep_id}: vendored source does not match the lock.\n"
                f"    path     {vendored['path']}\n"
                f"    expected {expected}\n"
                f"    actual   {actual}\n"
                f"  The vendored tree was modified without updating the lock. If the change\n"
                f"  is intentional, add it as a numbered patch under\n"
                f"  third_party/patches/{dep_id}/ and refresh the digest with --print-digests."
            )

        if vendored.get("file_count") != len(files):
            errors.append(
                f"{dep_id}: expected {vendored.get('file_count')} vendored files, "
                f"found {len(files)}"
            )

        # ---- Licence text must actually be present -------------------------------------
        license_file = dep.get("license_file")
        if license_file and not (REPO_ROOT / license_file).is_file():
            errors.append(f"{dep_id}: license_file is missing: {license_file}")

        # ---- Local patches must be declared and unmodified ------------------------------
        declared = dep.get("patches", [])
        patch_dir = REPO_ROOT / "third_party" / "patches" / dep_id
        on_disk = sorted(p.name for p in patch_dir.glob("*.patch")) if patch_dir.is_dir() else []
        declared_names = [p["file"] for p in declared]

        for name in on_disk:
            if name not in declared_names:
                errors.append(
                    f"{dep_id}: patch {name} exists on disk but is not declared in the lock.\n"
                    f"  An undeclared patch is an invisible fork."
                )

        for patch in declared:
            patch_path = patch_dir / patch["file"]
            if not patch_path.is_file():
                errors.append(f"{dep_id}: declared patch is missing: {patch['file']}")
                continue
            actual_patch = file_digest(patch_path)
            if actual_patch != patch["sha256"]:
                errors.append(
                    f"{dep_id}: patch {patch['file']} does not match its recorded digest.\n"
                    f"    expected {patch['sha256']}\n"
                    f"    actual   {actual_patch}"
                )
            if not patch.get("rationale"):
                errors.append(
                    f"{dep_id}: patch {patch['file']} has no rationale.\n"
                    f"  A future maintainer must be able to tell whether it is still needed."
                )

    return errors


def print_digests(lock: dict) -> None:
    """Emit the digests the lock should carry, for use after a deliberate change."""
    for dep in lock["dependencies"]:
        vendored = dep.get("vendored")
        if vendored is None:
            continue
        path = REPO_ROOT / vendored["path"]
        if not path.exists():
            print(f"{dep['id']}: (vendored path absent)")
            continue
        files = tracked_files(path)
        print(f"{dep['id']}:")
        print(f'  "content_sha256": "{content_digest(path, files)}",')
        print(f'  "file_count": {len(files)}')

        patch_dir = REPO_ROOT / "third_party" / "patches" / dep["id"]
        if patch_dir.is_dir():
            for patch in sorted(patch_dir.glob("*.patch")):
                print(f'  patch {patch.name}: "{file_digest(patch)}"')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="verify; exit non-zero on drift")
    mode.add_argument(
        "--print-digests",
        action="store_true",
        help="print current digests for updating the lock after a deliberate change",
    )
    args = parser.parse_args()

    lock = load_lock()

    if args.print_digests:
        print_digests(lock)
        return 0

    errors = check(lock)
    if errors:
        print("Third-party dependency verification failed.\n", file=sys.stderr)
        for error in errors:
            print(f"  {error}\n", file=sys.stderr)
        return 1

    count = len(lock["dependencies"])
    print(f"All {count} third-party dependencies match third_party/manifest.lock.json.")
    for dep in lock["dependencies"]:
        patches = len(dep.get("patches", []))
        suffix = f", {patches} declared patch{'es' if patches != 1 else ''}" if patches else ""
        print(f"  {dep['id']:<10} {dep['revision'][:12]}  {dep['license']}{suffix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
