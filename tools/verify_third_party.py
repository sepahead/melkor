#!/usr/bin/env python3
"""Verify the vendored third-party sources against third_party/manifest.lock.json.

Trust boundary
--------------
The reviewed Melkor source commit is the offline trust boundary for vendored source.
``revision`` records the upstream review target. This offline tool does not fetch that
commit, so it does not independently prove the relationship to upstream.

``vendored.content_sha256`` covers sorted paths, lengths, and bytes. CI verifies this digest
against the files that the build uses. ``vendored.upstream_content_sha256`` covers the local
tree after declared patches are reversed. These checks detect unrecorded local drift.

GitHub-generated archives can change compression without a source change. The lock records
an archive digest only when maintainers have a stable artifact. The verifier never downloads
or repairs a dependency.

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
import datetime
import hashlib
import json
import math
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unicodedata
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import urlsplit

REPO_ROOT = Path(__file__).resolve().parent.parent
LOCK_PATH = REPO_ROOT / "third_party" / "manifest.lock.json"

SUPPORTED_SCHEMA_VERSION = 1
MAX_VENDORED_FILE_BYTES = 64 * 1024 * 1024
MAX_VENDORED_TOTAL_BYTES = 512 * 1024 * 1024
MAX_VENDORED_FILES = 100_000
MAX_LOCK_BYTES = 4 * 1024 * 1024
MAX_PATH_BYTES = 240
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
WINDOWS_RESERVED_NAMES = {
    "CON",
    "CONIN$",
    "CONOUT$",
    "PRN",
    "AUX",
    "NUL",
    "CLOCK$",
    *(f"COM{index}" for index in range(1, 10)),
    *(f"LPT{index}" for index in range(1, 10)),
    *(f"COM{index}" for index in "¹²³"),
    *(f"LPT{index}" for index in "¹²³"),
}
WINDOWS_INVALID_CHARS = frozenset('<>:"|?*')


def strict_json(path: Path) -> Any:
    """Read UTF-8 JSON and reject duplicate object names."""

    def object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        value: dict[str, Any] = {}
        for name, item in pairs:
            if name in value:
                raise ValueError(f"duplicate object name {name!r}")
            value[name] = item
        return value

    def reject_constant(value: str) -> None:
        raise ValueError(f"non-JSON number {value!r}")

    def finite_float(value: str) -> float:
        parsed = float(value)
        if not math.isfinite(parsed):
            raise ValueError(f"non-finite JSON number {value!r}")
        return parsed

    if path.is_symlink():
        raise SystemExit(f"invalid JSON in {path}: the file must not be a symbolic link")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
        with os.fdopen(descriptor, "rb") as handle:
            metadata = os.fstat(handle.fileno())
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > MAX_LOCK_BYTES:
                raise ValueError("the lock must be a bounded regular file")
            raw = handle.read(MAX_LOCK_BYTES + 1)
            if len(raw) > MAX_LOCK_BYTES or len(raw) != metadata.st_size:
                raise ValueError("the lock changed or exceeds its size limit")
            final = os.fstat(handle.fileno())
            if (
                final.st_dev != metadata.st_dev
                or final.st_ino != metadata.st_ino
                or final.st_size != metadata.st_size
                or final.st_mtime_ns != metadata.st_mtime_ns
                or final.st_ctime_ns != metadata.st_ctime_ns
            ):
                raise ValueError("the lock changed while it was read")
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=object_without_duplicates,
            parse_constant=reject_constant,
            parse_float=finite_float,
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as exc:
        raise SystemExit(f"invalid JSON in {path}: {exc}") from exc


def safe_repo_path(value: str, *, prefix: str | None = None) -> Path:
    """Return a safe repository path from a manifest value."""
    try:
        safe_relative_path(value)
    except ValueError as exc:
        raise ValueError(f"unsafe repository path: {value!r}: {exc}") from exc
    parts = PurePosixPath(value).parts
    if prefix is not None and not (value == prefix or value.startswith(prefix + "/")):
        raise ValueError(f"repository path must stay under {prefix}/: {value!r}")
    candidate = REPO_ROOT.joinpath(*parts)
    current = REPO_ROOT
    for part in parts:
        current /= part
        if current.is_symlink():
            raise ValueError(f"repository path uses a symbolic-link component: {value!r}")
    try:
        candidate.resolve(strict=False).relative_to(REPO_ROOT.resolve())
    except ValueError as exc:
        raise ValueError(f"repository path escapes the checkout: {value!r}") from exc
    return candidate


def safe_relative_path(value: str) -> str:
    """Return one portable relative path."""
    if not value or value.startswith("/") or value.endswith("/") or "//" in value or "\\" in value:
        raise ValueError(f"unsafe relative path: {value!r}")
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise ValueError(f"relative path contains a control character: {value!r}")
    if len(value.encode("utf-8")) > MAX_PATH_BYTES:
        raise ValueError(f"relative path exceeds {MAX_PATH_BYTES} UTF-8 bytes: {value!r}")
    if unicodedata.normalize("NFC", value) != value:
        raise ValueError(f"relative path is not Unicode NFC: {value!r}")
    parts = tuple(value.split("/"))
    if any(part in {"", ".", ".."} for part in parts):
        raise ValueError(f"unsafe relative path: {value!r}")
    if parts[0].casefold() == ".git":
        raise ValueError(f"relative path uses the reserved .git name: {value!r}")
    for part in parts:
        if part.endswith((" ", ".")) or any(char in WINDOWS_INVALID_CHARS for char in part):
            raise ValueError(f"relative path is not portable to Windows: {value!r}")
        if part.split(".", 1)[0].upper() in WINDOWS_RESERVED_NAMES:
            raise ValueError(f"relative path uses a reserved Windows name: {value!r}")
    return value


def validate_portable_path_set(paths: list[str], label: str) -> None:
    """Reject names that collide on common case-insensitive file systems."""
    seen: dict[str, str] = {}
    for path in paths:
        safe_relative_path(path)
        key = unicodedata.normalize("NFC", path).casefold()
        previous = seen.get(key)
        if previous is not None and previous != path:
            raise ValueError(f"{label} has a portable path collision: {previous!r} and {path!r}")
        seen[key] = path


def validate_https_url(value: Any, label: str) -> str:
    """Validate one absolute HTTPS URL without user information."""
    if not isinstance(value, str) or not value or len(value) > 2048:
        raise ValueError(f"{label} must be a non-empty HTTPS URL")
    if "\\" in value or any(char.isspace() for char in value):
        raise ValueError(f"{label} is not a safe HTTPS URL")
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError as exc:
        raise ValueError(f"{label} is not a valid HTTPS URL") from exc
    if (
        parsed.scheme != "https"
        or not parsed.netloc
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or port is not None
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError(f"{label} must be an absolute HTTPS URL without user information")
    return value


def reject_unknown_keys(
    value: dict[str, Any], allowed: set[str], label: str, errors: list[str]
) -> None:
    """Report each unsupported object name."""
    for name in sorted(set(value) - allowed):
        errors.append(f"{label}: unknown field: {name}")


def validate_iso_date(value: Any, label: str, errors: list[str]) -> None:
    """Validate one exact ISO calendar date."""
    if not isinstance(value, str):
        errors.append(f"{label} must be an ISO date")
        return
    try:
        parsed = datetime.date.fromisoformat(value)
    except ValueError:
        errors.append(f"{label} must be an ISO date")
        return
    if parsed.isoformat() != value:
        errors.append(f"{label} must be an ISO date")


def update_regular_file_digest(digest: Any, path: Path, relative: str | None = None) -> int:
    """Hash one handle-bound regular file and return its byte size."""
    if path.is_symlink():
        raise ValueError(f"digest input must not be a symlink: {path}")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise ValueError(f"cannot open digest input: {path}") from exc
    try:
        with os.fdopen(descriptor, "rb") as handle:
            descriptor = -1
            initial = os.fstat(handle.fileno())
            if not stat.S_ISREG(initial.st_mode):
                raise ValueError(f"digest input must be a regular file: {path}")
            if initial.st_size > MAX_VENDORED_FILE_BYTES:
                raise ValueError(
                    f"vendored file exceeds the {MAX_VENDORED_FILE_BYTES}-byte limit: {path}"
                )
            if relative is not None:
                digest.update(relative.encode("utf-8"))
                digest.update(b"\0")
                digest.update(str(initial.st_size).encode("ascii"))
                digest.update(b"\0")
            read_size = 0
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                read_size += len(chunk)
                if read_size > MAX_VENDORED_FILE_BYTES:
                    raise ValueError(
                        f"vendored file exceeds the {MAX_VENDORED_FILE_BYTES}-byte limit: {path}"
                    )
                digest.update(chunk)
            final = os.fstat(handle.fileno())
            if (
                read_size != initial.st_size
                or final.st_size != initial.st_size
                or final.st_dev != initial.st_dev
                or final.st_ino != initial.st_ino
                or final.st_mtime_ns != initial.st_mtime_ns
                or final.st_ctime_ns != initial.st_ctime_ns
            ):
                raise ValueError(f"digest input changed while it was read: {path}")
            return read_size
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def content_digest(root: Path, relative_files: list[str]) -> str:
    """Deterministic digest over a set of files.

    Independent of archive format, compression, mtimes, and directory iteration order. The
    length is folded in so that concatenation of two files cannot collide with a single
    file holding their concatenation.
    """
    if root.is_symlink():
        raise ValueError(f"vendored root must not be a symlink: {root}")
    validate_portable_path_set(relative_files, "vendored content")
    base = root.parent if root.is_file() else root
    digest = hashlib.sha256()
    total_size = 0
    for relative in sorted(relative_files):
        safe_relative_path(relative)
        candidate = base / relative
        total_size += update_regular_file_digest(digest, candidate, relative)
        if total_size > MAX_VENDORED_TOTAL_BYTES:
            raise ValueError(f"vendored content exceeds the {MAX_VENDORED_TOTAL_BYTES}-byte limit")
    return digest.hexdigest()


def tracked_files(path: Path) -> list[str]:
    """The git-tracked files under ``path``, as paths relative to it.

    This deliberately consults git rather than walking the disk. The digest is meant to cover
    the *vendored source we commit*, not whatever happens to be lying in the directory: a
    developer who builds a dependency in-tree, or drops a scratch file, would otherwise change
    the digest and get a spurious verification failure -- or, worse, have an untracked file
    silently folded into the "vendored source" identity.

    Falls back to a filtered disk walk only when git is unavailable or the path is not in a
    repository, so the tool still works from an extracted source tarball.
    """
    try:
        relative_root = path.relative_to(REPO_ROOT).as_posix()
        out = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "ls-files", "-z", "--", relative_root],
            capture_output=True,
            check=True,
        )
        try:
            rel_to_repo = [raw.decode("utf-8", "strict") for raw in out.stdout.split(b"\0") if raw]
        except UnicodeDecodeError as exc:
            raise ValueError("Git returned a non-UTF-8 tracked path") from exc
        prefix = relative_root.rstrip("/") + "/"
        relative_files = sorted(
            path.name if item == relative_root else item[len(prefix) :]
            for item in rel_to_repo
            if item == relative_root or item.startswith(prefix)
        )
        if len(relative_files) > MAX_VENDORED_FILES:
            raise ValueError(f"vendored tree has more than {MAX_VENDORED_FILES} files")
        for relative in relative_files:
            safe_relative_path(relative)
        validate_portable_path_set(relative_files, "vendored tree")
        return relative_files
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass  # Not a git checkout (e.g. an extracted tarball); fall back to a filtered walk.

    # Fallback: walk the disk, excluding version-control and common build/cache artefacts so an
    # in-tree build does not corrupt the digest.
    if path.is_symlink():
        raise ValueError(f"vendored root must not be a symlink: {path}")
    if path.is_file():
        return [safe_relative_path(path.name)]
    _skip = {".git", "build", "_build", "__pycache__", ".cache", "node_modules"}
    relative_files: list[str] = []
    for candidate in path.rglob("*"):
        relative_path = candidate.relative_to(path)
        if _skip & set(relative_path.parts):
            continue
        if candidate.is_symlink():
            raise ValueError(f"vendored entry must not be a symlink: {candidate}")
        if candidate.is_file():
            relative = relative_path.as_posix()
            safe_relative_path(relative)
            relative_files.append(relative)
            if len(relative_files) > MAX_VENDORED_FILES:
                raise ValueError(f"vendored tree has more than {MAX_VENDORED_FILES} files")
    relative_files.sort()
    validate_portable_path_set(relative_files, "vendored tree")
    return relative_files


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    update_regular_file_digest(digest, path)
    return digest.hexdigest()


def regular_tree_files(root: Path) -> list[str]:
    """Return each bounded regular file below one temporary source tree."""

    files: list[str] = []
    for candidate in root.rglob("*"):
        if candidate.is_symlink():
            raise ValueError(f"reconstructed upstream entry is a symlink: {candidate}")
        if candidate.is_dir():
            continue
        if not candidate.is_file():
            raise ValueError(f"reconstructed upstream entry is not regular: {candidate}")
        relative = safe_relative_path(candidate.relative_to(root).as_posix())
        files.append(relative)
        if len(files) > MAX_VENDORED_FILES:
            raise ValueError(
                f"reconstructed upstream tree has more than {MAX_VENDORED_FILES} files"
            )
    files.sort()
    validate_portable_path_set(files, "reconstructed upstream tree")
    return files


def reconstruct_upstream_digest(
    root: Path, relative_files: list[str], patch_paths: list[Path]
) -> tuple[str, int]:
    """Reverse each declared patch and hash the reconstructed upstream subset."""

    with tempfile.TemporaryDirectory(prefix="melkor-upstream-") as directory:
        temporary_root = Path(directory) / "source"
        temporary_root.mkdir()
        for relative in relative_files:
            safe_relative_path(relative)
            source = root.joinpath(*PurePosixPath(relative).parts)
            destination = temporary_root.joinpath(*PurePosixPath(relative).parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if source.is_symlink() or not source.is_file():
                raise ValueError(f"vendored patch input is not a regular file: {source}")
            try:
                shutil.copyfile(source, destination, follow_symlinks=False)
            except OSError as exc:
                raise ValueError(f"cannot copy vendored patch input: {source}") from exc

        for patch_path in reversed(patch_paths):
            for check_only in (True, False):
                command = ["git", "apply", "--reverse", "--whitespace=nowarn"]
                if check_only:
                    command.append("--check")
                command.append(str(patch_path.resolve()))
                try:
                    completed = subprocess.run(
                        command,
                        cwd=temporary_root,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        check=False,
                    )
                except FileNotFoundError as exc:
                    raise ValueError("git is required to verify declared source patches") from exc
                if completed.returncode != 0:
                    detail = completed.stderr.decode("utf-8", "replace").strip()
                    raise ValueError(
                        f"declared patch does not reverse cleanly: {patch_path.name}: {detail}"
                    )

        upstream_files = regular_tree_files(temporary_root)
        if not upstream_files:
            raise ValueError("the reconstructed upstream source tree is empty")
        return content_digest(temporary_root, upstream_files), len(upstream_files)


def load_lock() -> dict:
    if not LOCK_PATH.is_file():
        raise SystemExit(f"missing dependency lock: {LOCK_PATH}")

    lock = strict_json(LOCK_PATH)
    if not isinstance(lock, dict):
        raise SystemExit(f"{LOCK_PATH}: root must be an object")
    schema = lock.get("schema_version")
    if schema != SUPPORTED_SCHEMA_VERSION:
        raise SystemExit(
            f"{LOCK_PATH}: schema_version {schema!r} is not supported "
            f"(this tool understands {SUPPORTED_SCHEMA_VERSION})"
        )
    return lock


def validate_lock(lock: dict) -> list[str]:
    """Validate the dependency lock before file access."""
    errors: list[str] = []
    reject_unknown_keys(lock, {"schema_version", "_comment", "dependencies"}, "lock", errors)
    comment = lock.get("_comment")
    if comment is not None and (
        not isinstance(comment, list) or not all(isinstance(line, str) for line in comment)
    ):
        errors.append("lock: _comment must be a string list")
    dependencies = lock.get("dependencies")
    if not isinstance(dependencies, list) or not dependencies:
        return ["dependency lock requires a non-empty dependencies list"]

    seen_ids: set[str] = set()
    vendored_roots: list[tuple[str, str]] = []
    for index, dep in enumerate(dependencies):
        label = f"dependency {index}"
        if not isinstance(dep, dict):
            errors.append(f"{label} must be an object")
            continue
        reject_unknown_keys(
            dep,
            {
                "id",
                "kind",
                "purpose",
                "upstream",
                "revision",
                "release",
                "archive",
                "license",
                "license_file",
                "vendored",
                "patches",
                "build_condition",
                "format_versions",
                "upgrade_target",
                "used_by",
                "update_policy",
                "security_contact",
                "review",
            },
            label,
            errors,
        )
        dep_id = dep.get("id")
        if not isinstance(dep_id, str) or not ID_RE.fullmatch(dep_id):
            errors.append(f"{label} has an invalid id")
            continue
        label = dep_id
        if dep_id in seen_ids:
            errors.append(f"duplicate dependency id: {dep_id}")
        seen_ids.add(dep_id)

        if dep.get("kind") != "source":
            errors.append(f"{label}: kind must be 'source'")

        for name in ("purpose", "upstream", "license", "license_file"):
            if not isinstance(dep.get(name), str) or not dep[name]:
                errors.append(f"{label}: {name} must be a non-empty string")
        try:
            validate_https_url(dep.get("upstream"), f"{label}: upstream")
        except ValueError as exc:
            errors.append(str(exc))
        if not isinstance(dep.get("revision"), str) or not REVISION_RE.fullmatch(dep["revision"]):
            errors.append(f"{label}: revision must be a lowercase 40-character commit SHA")
        release = dep.get("release")
        if release is not None and (not isinstance(release, str) or not release):
            errors.append(f"{label}: release must be a non-empty string or null")
        archive = dep.get("archive")
        if not isinstance(archive, dict):
            errors.append(f"{label}: archive must be an object")
        else:
            reject_unknown_keys(archive, {"url", "sha256", "note"}, f"{label}: archive", errors)
            try:
                validate_https_url(archive.get("url"), f"{label}: archive.url")
            except ValueError as exc:
                errors.append(str(exc))
            archive_digest = archive.get("sha256")
            if archive_digest is not None and (
                not isinstance(archive_digest, str) or not SHA256_RE.fullmatch(archive_digest)
            ):
                errors.append(f"{label}: archive.sha256 must be a SHA-256 digest or null")
            if archive_digest is None and (
                not isinstance(archive.get("note"), str) or not archive["note"].strip()
            ):
                errors.append(f"{label}: archive.note must explain an absent digest")
        license_file = dep.get("license_file")
        if isinstance(license_file, str):
            try:
                safe_repo_path(license_file, prefix="third_party")
            except ValueError as exc:
                errors.append(f"{label}: {exc}")

        vendored = dep.get("vendored")
        if not isinstance(vendored, dict):
            errors.append(f"{label}: vendored must be an object")
        else:
            reject_unknown_keys(
                vendored,
                {
                    "path",
                    "content_sha256",
                    "file_count",
                    "upstream_content_sha256",
                    "upstream_file_count",
                },
                f"{label}: vendored",
                errors,
            )
            vendored_path = vendored.get("path")
            if not isinstance(vendored_path, str):
                errors.append(f"{label}: vendored.path must be a string")
            else:
                try:
                    safe_repo_path(vendored_path, prefix="third_party")
                except ValueError as exc:
                    errors.append(f"{label}: {exc}")
                else:
                    normalized = vendored_path.rstrip("/")
                    if normalized == "third_party":
                        errors.append(f"{label}: vendored.path must identify one dependency")
                    for previous_id, previous in vendored_roots:
                        if (
                            normalized == previous
                            or normalized.startswith(previous + "/")
                            or previous.startswith(normalized + "/")
                        ):
                            errors.append(
                                f"{label}: vendored path overlaps {previous_id}: {normalized}"
                            )
                    vendored_roots.append((dep_id, normalized))
                    if isinstance(license_file, str) and not (
                        license_file == normalized or license_file.startswith(normalized + "/")
                    ):
                        errors.append(f"{label}: license_file must stay in the vendored path")
            if not isinstance(vendored.get("content_sha256"), str) or not SHA256_RE.fullmatch(
                vendored["content_sha256"]
            ):
                errors.append(f"{label}: vendored.content_sha256 is invalid")
            if not isinstance(
                vendored.get("upstream_content_sha256"), str
            ) or not SHA256_RE.fullmatch(vendored["upstream_content_sha256"]):
                errors.append(f"{label}: vendored.upstream_content_sha256 is invalid")
            file_count = vendored.get("file_count")
            if not isinstance(file_count, int) or isinstance(file_count, bool) or file_count < 1:
                errors.append(f"{label}: vendored.file_count must be a positive integer")
            elif file_count > MAX_VENDORED_FILES:
                errors.append(f"{label}: vendored.file_count exceeds {MAX_VENDORED_FILES}")
            upstream_file_count = vendored.get("upstream_file_count")
            if (
                not isinstance(upstream_file_count, int)
                or isinstance(upstream_file_count, bool)
                or upstream_file_count < 1
            ):
                errors.append(f"{label}: vendored.upstream_file_count must be positive")
            elif upstream_file_count > MAX_VENDORED_FILES:
                errors.append(f"{label}: vendored.upstream_file_count exceeds {MAX_VENDORED_FILES}")

        patches = dep.get("patches", [])
        if not isinstance(patches, list):
            errors.append(f"{label}: patches must be a list")
            continue
        patch_names: set[str] = set()
        for patch in patches:
            if not isinstance(patch, dict):
                errors.append(f"{label}: each patch must be an object")
                continue
            reject_unknown_keys(
                patch,
                {"file", "sha256", "rationale", "upstream_status"},
                f"{label}: patch",
                errors,
            )
            name = patch.get("file")
            try:
                if not isinstance(name, str):
                    raise ValueError
                safe_relative_path(name)
                if PurePosixPath(name).name != name or not name.endswith(".patch"):
                    raise ValueError
            except ValueError:
                errors.append(f"{label}: patch file name is invalid")
                continue
            if name in patch_names:
                errors.append(f"{label}: duplicate patch file: {name}")
            patch_names.add(name)
            if not isinstance(patch.get("sha256"), str) or not SHA256_RE.fullmatch(patch["sha256"]):
                errors.append(f"{label}: patch {name} has an invalid digest")
            if not isinstance(patch.get("rationale"), str) or not patch["rationale"].strip():
                errors.append(f"{label}: patch {name} has no rationale")
            elif len(patch["rationale"]) > 1000:
                errors.append(f"{label}: patch {name} rationale is too long")
            if patch.get("upstream_status") not in {
                "not-submitted",
                "submitted",
                "accepted",
                "rejected",
            }:
                errors.append(f"{label}: patch {name} has an invalid upstream_status")

        used_by = dep.get("used_by")
        if (
            not isinstance(used_by, list)
            or not used_by
            or not all(isinstance(item, str) and item for item in used_by)
            or len(set(used_by)) != len(used_by)
        ):
            errors.append(f"{label}: used_by must be a non-empty unique string list")
        build_condition = dep.get("build_condition")
        if not isinstance(build_condition, str) or not re.fullmatch(
            r"(?:always|MELKOR_[A-Z0-9_]+)", build_condition
        ):
            errors.append(f"{label}: build_condition is invalid")

        format_versions = dep.get("format_versions")
        if format_versions is not None:
            if not isinstance(format_versions, dict):
                errors.append(f"{label}: format_versions must be an object")
            else:
                reject_unknown_keys(
                    format_versions,
                    {"read", "write", "_comment"},
                    f"{label}: format_versions",
                    errors,
                )
                for operation in ("read", "write"):
                    versions = format_versions.get(operation)
                    if (
                        not isinstance(versions, list)
                        or not versions
                        or any(
                            not isinstance(version, int)
                            or isinstance(version, bool)
                            or not 0 < version < 256
                            for version in versions
                        )
                        or len(set(versions)) != len(versions)
                    ):
                        errors.append(
                            f"{label}: format_versions.{operation} must be a unique byte list"
                        )
                format_comment = format_versions.get("_comment")
                if format_comment is not None and not isinstance(format_comment, str):
                    errors.append(f"{label}: format_versions._comment must be a string")

        upgrade = dep.get("upgrade_target")
        if upgrade is not None:
            if not isinstance(upgrade, dict):
                errors.append(f"{label}: upgrade_target must be an object")
            else:
                reject_unknown_keys(
                    upgrade,
                    {"revision", "release", "blocker", "work_package", "reason"},
                    f"{label}: upgrade_target",
                    errors,
                )
                if not isinstance(upgrade.get("revision"), str) or not REVISION_RE.fullmatch(
                    upgrade["revision"]
                ):
                    errors.append(f"{label}: upgrade_target.revision is invalid")
                for name in ("release", "blocker", "work_package", "reason"):
                    if not isinstance(upgrade.get(name), str) or not upgrade[name].strip():
                        errors.append(f"{label}: upgrade_target.{name} must be a string")

        if dep.get("update_policy") != "manual-reviewed":
            errors.append(f"{label}: update_policy must be 'manual-reviewed'")
        if not isinstance(dep.get("security_contact"), str) or not dep["security_contact"].strip():
            errors.append(f"{label}: security_contact must be a string")
        review = dep.get("review")
        if not isinstance(review, dict):
            errors.append(f"{label}: review must be an object")
        else:
            reject_unknown_keys(review, {"date", "owner", "next_review"}, f"{label}: review", errors)
            validate_iso_date(review.get("date"), f"{label}: review.date", errors)
            validate_iso_date(review.get("next_review"), f"{label}: review.next_review", errors)
            if not isinstance(review.get("owner"), str) or not review["owner"].strip():
                errors.append(f"{label}: review.owner must be a string")
    return errors


SPEC_LOCK_PATH = REPO_ROOT / "third_party" / "specifications.lock.json"


def check_specifications() -> list[str]:
    """Verify vendored specification files against third_party/specifications.lock.json.

    A pinned spec (like the Khronos KHR_gaussian_splatting release candidate) is pinned by the
    exact upstream commit and the SHA-256 of each vendored file, so that an editorial or semantic
    change upstream cannot silently alter Melkor's behavior. This checks the files still match.
    """
    errors: list[str] = []
    if not SPEC_LOCK_PATH.is_file():
        return [f"missing specification lock: {SPEC_LOCK_PATH}"]

    lock = strict_json(SPEC_LOCK_PATH)
    if not isinstance(lock, dict) or lock.get("schema_version") != 1:
        return ["specification lock schema_version must be 1"]
    reject_unknown_keys(
        lock,
        {"schema_version", "_comment", "specifications"},
        "specification lock",
        errors,
    )
    specifications = lock.get("specifications")
    if not isinstance(specifications, list) or not specifications:
        return ["specification lock requires a non-empty specifications list"]

    seen_ids: set[str] = set()
    for index, spec in enumerate(specifications):
        if not isinstance(spec, dict):
            errors.append(f"specification {index} must be an object")
            continue
        reject_unknown_keys(
            spec,
            {
                "id",
                "upstream",
                "path_in_upstream",
                "commit",
                "commit_date",
                "status",
                "status_note",
                "vendored_path",
                "files",
                "license",
                "license_notice_file",
                "semantic_decisions",
                "owner",
                "next_review",
                "known_open_upstream",
            },
            f"specification {index}",
            errors,
        )
        spec_id = spec.get("id")
        if not isinstance(spec_id, str) or not ID_RE.fullmatch(spec_id):
            errors.append(f"specification {index} has an invalid id")
            continue
        if spec_id in seen_ids:
            errors.append(f"duplicate specification id: {spec_id}")
        seen_ids.add(spec_id)
        commit = spec.get("commit")
        if not isinstance(commit, str) or not REVISION_RE.fullmatch(commit):
            errors.append(f"{spec_id}: commit must be a lowercase 40-character SHA")
        if "commit_date" in spec:
            validate_iso_date(spec.get("commit_date"), f"{spec_id}: commit_date", errors)
        if "next_review" in spec:
            validate_iso_date(spec.get("next_review"), f"{spec_id}: next_review", errors)
        for name in ("status", "status_note", "license", "owner"):
            if name in spec and (not isinstance(spec[name], str) or not spec[name].strip()):
                errors.append(f"{spec_id}: {name} must be a string")
        if "semantic_decisions" in spec and not isinstance(spec["semantic_decisions"], dict):
            errors.append(f"{spec_id}: semantic_decisions must be an object")
        known_open = spec.get("known_open_upstream")
        if known_open is not None and (
            not isinstance(known_open, list)
            or not all(isinstance(item, str) and item.strip() for item in known_open)
        ):
            errors.append(f"{spec_id}: known_open_upstream must be a string list")
        try:
            validate_https_url(spec.get("upstream"), f"{spec_id}: upstream")
        except ValueError as exc:
            errors.append(str(exc))
        path_in_upstream = spec.get("path_in_upstream")
        if not isinstance(path_in_upstream, str):
            errors.append(f"{spec_id}: path_in_upstream must be a string")
        else:
            try:
                safe_relative_path(path_in_upstream)
            except ValueError as exc:
                errors.append(f"{spec_id}: {exc}")
        vendored_path = spec.get("vendored_path")
        if not isinstance(vendored_path, str):
            errors.append(f"{spec_id}: vendored_path must be a string")
            continue
        try:
            base = safe_repo_path(vendored_path, prefix="third_party/specs")
        except ValueError as exc:
            errors.append(f"{spec_id}: {exc}")
            continue
        files = spec.get("files")
        if not isinstance(files, list) or not files:
            errors.append(f"{spec_id}: files must be a non-empty list")
            continue
        if len(files) > MAX_VENDORED_FILES:
            errors.append(f"{spec_id}: files exceeds {MAX_VENDORED_FILES} entries")
            continue
        declared: dict[str, str] = {}
        for entry in files:
            if not isinstance(entry, dict):
                errors.append(f"{spec_id}: each file must be an object")
                continue
            reject_unknown_keys(entry, {"path", "sha256"}, f"{spec_id}: file", errors)
            relative = entry.get("path")
            expected = entry.get("sha256")
            try:
                if not isinstance(relative, str):
                    raise ValueError("path is not a string")
                safe_relative_path(relative)
            except ValueError:
                errors.append(f"{spec_id}: specification file path is unsafe")
                continue
            if relative in declared:
                errors.append(f"{spec_id}: duplicate specification file: {relative}")
                continue
            if not isinstance(expected, str) or not SHA256_RE.fullmatch(expected):
                errors.append(f"{spec_id}: invalid digest for {relative}")
                continue
            declared[relative] = expected

        try:
            validate_portable_path_set(list(declared), f"{spec_id} file inventory")
        except ValueError as exc:
            errors.append(str(exc))

        license_notice = spec.get("license_notice_file")
        if not isinstance(license_notice, str):
            errors.append(f"{spec_id}: license_notice_file must be a string")
        else:
            try:
                notice_path = safe_repo_path(license_notice, prefix=vendored_path)
            except ValueError as exc:
                errors.append(f"{spec_id}: {exc}")
            else:
                notice_relative = notice_path.relative_to(base).as_posix()
                if notice_relative not in declared:
                    errors.append(
                        f"{spec_id}: license_notice_file must identify a declared specification file"
                    )
                elif notice_path.is_symlink() or not notice_path.is_file():
                    errors.append(f"{spec_id}: license_notice_file is not a regular file")

        try:
            actual_files = set(tracked_files(base))
        except ValueError as exc:
            errors.append(f"{spec_id}: {exc}")
            continue
        if actual_files != set(declared):
            missing = sorted(set(declared) - actual_files)
            extra = sorted(actual_files - set(declared))
            errors.append(
                f"{spec_id}: specification file inventory differs from the lock "
                f"(missing={missing}, extra={extra})"
            )
        for relative, expected in declared.items():
            path = base.joinpath(*PurePosixPath(relative).parts)
            if not path.is_file() or path.is_symlink():
                errors.append(f"{spec_id}: vendored spec file missing: {relative}")
                continue
            try:
                actual = file_digest(path)
            except ValueError as exc:
                errors.append(f"{spec_id}: {exc}")
                continue
            if actual != expected:
                errors.append(
                    f"{spec_id}: {relative} does not match the specifications lock.\n"
                    f"    expected {expected}\n    actual   {actual}\n"
                    "  The pinned spec file changed. Follow the update procedure. "
                    "Create a new profile ID and refresh the lock."
                )
    return errors


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

        path = safe_repo_path(vendored["path"], prefix="third_party")
        if not path.exists():
            errors.append(f"{dep_id}: vendored path does not exist: {vendored['path']}")
            continue

        try:
            files = tracked_files(path)
        except ValueError as exc:
            errors.append(f"{dep_id}: {exc}")
            continue
        if not files:
            errors.append(f"{dep_id}: vendored path {vendored['path']} contains no files")
            continue

        try:
            actual = content_digest(path, files)
        except ValueError as exc:
            errors.append(f"{dep_id}: {exc}")
            continue
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

        # ---- License text must actually be present -------------------------------------
        license_file = dep.get("license_file")
        if license_file:
            license_path = safe_repo_path(license_file, prefix="third_party")
            if license_path.is_symlink() or not license_path.is_file():
                errors.append(f"{dep_id}: license_file is missing or unsafe: {license_file}")

        # ---- Local patches must be declared and unmodified ------------------------------
        declared = dep.get("patches", [])
        patch_dir = REPO_ROOT / "third_party" / "patches" / dep_id
        declared_names = [p["file"] for p in declared]
        try:
            tracked_patch_files = tracked_files(patch_dir) if patch_dir.is_dir() else []
        except ValueError as exc:
            errors.append(f"{dep_id}: {exc}")
            tracked_patch_files = []
        extra_patch_files = sorted(set(tracked_patch_files) - set(declared_names))
        missing_patch_files = sorted(set(declared_names) - set(tracked_patch_files))
        patches_valid = not missing_patch_files and not extra_patch_files
        if extra_patch_files:
            errors.append(
                f"{dep_id}: undeclared files exist in the patch directory: {extra_patch_files}"
            )
        if missing_patch_files:
            errors.append(f"{dep_id}: declared patches are not tracked: {missing_patch_files}")

        for patch in declared:
            patch_path = patch_dir / patch["file"]
            if patch_path.is_symlink() or not patch_path.is_file():
                errors.append(f"{dep_id}: declared patch is missing: {patch['file']}")
                patches_valid = False
                continue
            try:
                actual_patch = file_digest(patch_path)
            except ValueError as exc:
                errors.append(f"{dep_id}: {exc}")
                patches_valid = False
                continue
            if actual_patch != patch["sha256"]:
                patches_valid = False
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

        if patches_valid:
            patch_paths = [patch_dir / patch["file"] for patch in declared]
            try:
                upstream_digest, upstream_file_count = reconstruct_upstream_digest(
                    path, files, patch_paths
                )
            except ValueError as exc:
                errors.append(f"{dep_id}: {exc}")
            else:
                if upstream_digest != vendored.get("upstream_content_sha256"):
                    errors.append(
                        f"{dep_id}: declared patches do not reconstruct the pinned upstream "
                        "content.\n"
                        f"    expected {vendored.get('upstream_content_sha256')}\n"
                        f"    actual   {upstream_digest}"
                    )
                if upstream_file_count != vendored.get("upstream_file_count"):
                    errors.append(
                        f"{dep_id}: expected {vendored.get('upstream_file_count')} upstream "
                        f"files, reconstructed {upstream_file_count}"
                    )

    return errors


def print_digests(lock: dict) -> None:
    """Emit the digests the lock should carry, for use after a deliberate change."""
    for dep in lock["dependencies"]:
        vendored = dep.get("vendored")
        if vendored is None:
            continue
        path = safe_repo_path(vendored["path"], prefix="third_party")
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
        declared_patches = [patch_dir / patch["file"] for patch in dep.get("patches", [])]
        upstream_digest, upstream_file_count = reconstruct_upstream_digest(
            path, files, declared_patches
        )
        print(f'  "upstream_content_sha256": "{upstream_digest}",')
        print(f'  "upstream_file_count": {upstream_file_count}')


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
    validation_errors = validate_lock(lock)
    if validation_errors:
        print("Third-party dependency lock is invalid.\n", file=sys.stderr)
        for error in validation_errors:
            print(f"  {error}\n", file=sys.stderr)
        return 1

    if args.print_digests:
        print_digests(lock)
        return 0

    errors = check(lock)
    errors += check_specifications()
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
