#!/usr/bin/env python3
"""Build and verify deterministic source-release evidence from an exact Git ref.

The output is intentionally unsigned.  It is suitable for release-candidate
review and as the input to a later signing/attestation step, but it is not an
authenticity proof by itself.
"""

from __future__ import annotations

import argparse
import datetime as dt
import gzip
import hashlib
import io
import json
import math
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import types
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import quote, urlsplit


INVENTORY_PATH = "release/components.json"
SOURCE_POLICY_PATH = "tools/build_source_bundle.py"
PUBLISH_TOOL_PATH = "tools/atomic_publish.py"
GENERATOR_VERSION = "1"
LFS_HEADER = b"version https://git-lfs.github.com/spec/v1\n"
LFS_OID_RE = re.compile(rb"^oid sha256:([0-9a-f]{64})$")
# The authoritative version is the single line in the root VERSION file. Evidence must
# read the same source the build reads; deriving it by re-parsing CMakeLists.txt would
# reintroduce the second version definition this project just removed.
SEMVER_NUMBER = rb"(?:0|[1-9][0-9]*)"
SEMVER_PRERELEASE_ID = rb"(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
SEMVER_BUILD_ID = rb"[0-9A-Za-z-]+"
VERSION_RE = re.compile(
    rb"[ \t]*("
    + SEMVER_NUMBER
    + rb"\."
    + SEMVER_NUMBER
    + rb"\."
    + SEMVER_NUMBER
    + rb"(?:-"
    + SEMVER_PRERELEASE_ID
    + rb"(?:\."
    + SEMVER_PRERELEASE_ID
    + rb")*)?(?:\+"
    + SEMVER_BUILD_ID
    + rb"(?:\."
    + SEMVER_BUILD_ID
    + rb")*)?)[ \t]*(?:\r?\n)?"
)
SPDX_ID_RE = re.compile(r"^SPDXRef-[A-Za-z0-9.-]+$")
LICENSE_REF_RE = re.compile(r"LicenseRef-[A-Za-z0-9.-]+")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SHA1_RE = re.compile(r"^[0-9a-f]{40}$")
GIT_OBJECT_ID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
MAX_SOURCE_FILES = 100_000
MAX_SOURCE_FILE_BYTES = 64 * 1024 * 1024
MAX_SOURCE_TOTAL_BYTES = 512 * 1024 * 1024
MAX_EVIDENCE_METADATA_BYTES = 512 * 1024 * 1024
MAX_EVIDENCE_ARCHIVE_BYTES = 640 * 1024 * 1024
MAX_CHECKSUM_FILE_BYTES = 1024 * 1024
MAX_SOURCE_PATH_BYTES = 240
MAX_EXPANDED_ARCHIVE_BYTES = 768 * 1024 * 1024
MAX_JSON_DEPTH = 64
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


class EvidenceError(RuntimeError):
    """A release-evidence contract violation."""


@dataclass(frozen=True)
class GitEntry:
    path: str
    mode: str
    oid: str
    data: bytes

    @property
    def is_symlink(self) -> bool:
        return self.mode == "120000"


@dataclass(frozen=True)
class FileRecord:
    entry: GitEntry
    sha1: str
    sha256: str


@dataclass(frozen=True)
class ArchiveVerification:
    sha1_by_path: dict[str, str]
    captured_files: dict[str, bytes]


def escape_diagnostic(text: str) -> str:
    """Escape terminal control characters in diagnostic text."""
    escaped: list[str] = []
    for char in text:
        if char == "\\":
            escaped.append("\\\\")
        elif char.isprintable():
            escaped.append(char)
        else:
            escaped.append(char.encode("unicode_escape").decode("ascii"))
    return "".join(escaped)


def canonical_json_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path, maximum: int | None = None) -> str:
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as handle:
        file_stat = os.fstat(handle.fileno())
        if not stat.S_ISREG(file_stat.st_mode):
            raise EvidenceError(f"evidence artifact is not a regular file: {path}")
        if maximum is not None and file_stat.st_size > maximum:
            raise EvidenceError(f"evidence artifact exceeds its size limit: {path.name}")
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            total += len(chunk)
            if maximum is not None and total > maximum:
                raise EvidenceError(f"evidence artifact exceeds its size limit: {path.name}")
            digest.update(chunk)
    return digest.hexdigest()


def read_bounded_file(path: Path, maximum: int, label: str) -> bytes:
    """Read one regular file with an exact byte limit."""
    try:
        with path.open("rb") as handle:
            file_stat = os.fstat(handle.fileno())
            if not stat.S_ISREG(file_stat.st_mode):
                raise EvidenceError(f"{label} is not a regular file: {path}")
            if file_stat.st_size > maximum:
                raise EvidenceError(f"{label} exceeds the {maximum}-byte limit")
            data = handle.read(maximum + 1)
            if len(data) > maximum or handle.read(1):
                raise EvidenceError(f"{label} exceeds the {maximum}-byte limit")
            final_stat = os.fstat(handle.fileno())
            if final_stat.st_size != file_stat.st_size or len(data) != final_stat.st_size:
                raise EvidenceError(f"{label} changed while it was read")
            return data
    except EvidenceError:
        raise
    except OSError as exc:
        raise EvidenceError(f"cannot read {path}") from exc


def run_git(repo: Path, *args: str) -> bytes:
    command = ["git", "-C", str(repo), *args]
    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", "replace").strip()
        raise EvidenceError(f"git command failed ({' '.join(args)}): {detail}")
    return completed.stdout


def safe_source_path(path: str) -> None:
    if not path or path.startswith("/") or path.endswith("/") or "//" in path or "\\" in path:
        raise EvidenceError(f"unsafe tracked path: {path!r}")
    if any(ord(char) < 32 or ord(char) == 127 for char in path):
        raise EvidenceError(f"tracked path contains control characters: {path!r}")
    if len(path.encode("utf-8")) > MAX_SOURCE_PATH_BYTES:
        raise EvidenceError(f"tracked path exceeds {MAX_SOURCE_PATH_BYTES} UTF-8 bytes: {path!r}")
    if unicodedata.normalize("NFC", path) != path:
        raise EvidenceError(f"tracked path is not Unicode NFC: {path!r}")
    parts = tuple(path.split("/"))
    if any(part in {"", ".", ".."} for part in parts):
        raise EvidenceError(f"unsafe tracked path: {path!r}")
    if parts[0].casefold() == ".git":
        raise EvidenceError("the source tree must not contain .git entries")
    for part in parts:
        if part.endswith((" ", ".")) or any(char in WINDOWS_INVALID_CHARS for char in part):
            raise EvidenceError(f"tracked path is not portable to Windows: {path!r}")
        if part.split(".", 1)[0].upper() in WINDOWS_RESERVED_NAMES:
            raise EvidenceError(f"tracked path uses a reserved Windows name: {path!r}")


def portable_path_key(path: str) -> str:
    """Return the cross-platform collision key for one safe path."""
    return unicodedata.normalize("NFC", path).casefold()


def require_nonempty_string(value: Any, label: str, maximum: int = 4096) -> str:
    """Return one bounded non-empty string."""
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise EvidenceError(f"{label} must be a non-empty string")
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise EvidenceError(f"{label} contains a control character")
    return value


def require_https_url(value: Any, label: str) -> str:
    """Return one absolute HTTPS URL without user information."""
    url = require_nonempty_string(value, label, 2048)
    if "\\" in url or any(char.isspace() for char in url):
        raise EvidenceError(f"{label} is not a safe HTTPS URL")
    try:
        parsed = urlsplit(url)
        port = parsed.port
    except ValueError as exc:
        raise EvidenceError(f"{label} is not a valid HTTPS URL") from exc
    if (
        parsed.scheme != "https"
        or not parsed.netloc
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or port is not None
    ):
        raise EvidenceError(f"{label} must be an absolute HTTPS URL without user information")
    return url


def parse_lfs_pointer(data: bytes) -> str | None:
    if not data.startswith(LFS_HEADER):
        return None
    lines = data.rstrip(b"\n").splitlines()
    if len(lines) < 3:
        return "malformed"
    oid_match = LFS_OID_RE.fullmatch(lines[1])
    if oid_match is None or re.fullmatch(rb"size [0-9]+", lines[2]) is None:
        return "malformed"
    return oid_match.group(1).decode("ascii")


def read_git_tree_metadata(repo: Path, commit: str) -> list[GitEntry]:
    """Read tracked paths and object IDs without reading blob contents."""
    listing = run_git(repo, "ls-tree", "-rz", "--full-tree", commit)
    entries: list[GitEntry] = []
    seen_paths: set[str] = set()
    portable_paths: dict[str, str] = {}
    for raw_record in listing.split(b"\0"):
        if not raw_record:
            continue
        try:
            raw_meta, raw_path = raw_record.split(b"\t", 1)
            raw_mode, raw_type, raw_oid = raw_meta.split(b" ", 2)
            path = raw_path.decode("utf-8", "strict")
        except (ValueError, UnicodeDecodeError) as exc:
            raise EvidenceError("Git tree contains an unsupported entry") from exc
        safe_source_path(path)
        if path in seen_paths:
            raise EvidenceError(f"duplicate tracked path: {path}")
        seen_paths.add(path)
        portable_key = portable_path_key(path)
        previous = portable_paths.get(portable_key)
        if previous is not None:
            raise EvidenceError(f"portable tracked path collision: {previous!r} and {path!r}")
        portable_paths[portable_key] = path
        if raw_type != b"blob":
            kind = raw_type.decode("ascii", "replace")
            raise EvidenceError(f"tracked entry {path} is {kind}, not a self-contained blob")
        mode = raw_mode.decode("ascii")
        if mode not in {"100644", "100755", "120000"}:
            raise EvidenceError(f"unsupported Git mode {mode} for {path}")
        try:
            oid = raw_oid.decode("ascii", "strict")
        except UnicodeDecodeError as exc:
            raise EvidenceError(f"Git object ID is invalid for {path}") from exc
        if not GIT_OBJECT_ID_RE.fullmatch(oid):
            raise EvidenceError(f"Git object ID is invalid for {path}")
        entries.append(GitEntry(path=path, mode=mode, oid=oid, data=b""))

    if not entries:
        raise EvidenceError("the selected Git tree is empty")
    if len(entries) > MAX_SOURCE_FILES:
        raise EvidenceError(f"the selected Git tree has more than {MAX_SOURCE_FILES} files")
    return sorted(entries, key=lambda entry: entry.path)


def read_git_blobs(repo: Path, metadata: list[GitEntry]) -> list[GitEntry]:
    """Read bounded blob contents for selected tree entries."""
    if not metadata:
        return []

    batch_input = b"".join(f"{entry.oid}\n".encode("ascii") for entry in metadata)
    sizes = subprocess.run(
        [
            "git",
            "-C",
            str(repo),
            "cat-file",
            "--batch-check=%(objectname) %(objecttype) %(objectsize)",
        ],
        input=batch_input,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if sizes.returncode != 0:
        detail = sizes.stderr.decode("utf-8", "replace").strip()
        raise EvidenceError(f"could not inspect Git blobs: {detail}")
    size_lines = sizes.stdout.splitlines()
    if len(size_lines) != len(metadata):
        raise EvidenceError("truncated git cat-file size response")
    total_size = 0
    for entry, raw_line in zip(metadata, size_lines, strict=True):
        fields = raw_line.split()
        if len(fields) != 3 or fields[0] != entry.oid.encode("ascii") or fields[1] != b"blob":
            raise EvidenceError(f"unexpected Git blob metadata for {entry.path}")
        try:
            size = int(fields[2])
        except ValueError as exc:
            raise EvidenceError(f"invalid Git blob size for {entry.path}") from exc
        if size < 0 or size > MAX_SOURCE_FILE_BYTES:
            raise EvidenceError(
                f"source file exceeds the {MAX_SOURCE_FILE_BYTES}-byte limit: {entry.path}"
            )
        total_size += size
        if total_size > MAX_SOURCE_TOTAL_BYTES:
            raise EvidenceError(f"selected source exceeds the {MAX_SOURCE_TOTAL_BYTES}-byte limit")

    completed = subprocess.run(
        ["git", "-C", str(repo), "cat-file", "--batch"],
        input=batch_input,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", "replace").strip()
        raise EvidenceError(f"could not read Git blobs: {detail}")

    entries: list[GitEntry] = []
    cursor = 0
    output = completed.stdout
    for entry in metadata:
        path = entry.path
        mode = entry.mode
        expected_oid = entry.oid
        header_end = output.find(b"\n", cursor)
        if header_end < 0:
            raise EvidenceError("truncated git cat-file response")
        header = output[cursor:header_end].split()
        if len(header) != 3 or header[1] != b"blob":
            raise EvidenceError(f"unexpected git cat-file response for {path}")
        oid = header[0].decode("ascii")
        size = int(header[2])
        data_start = header_end + 1
        data_end = data_start + size
        if data_end >= len(output) or output[data_end : data_end + 1] != b"\n":
            raise EvidenceError(f"truncated Git blob for {path}")
        if oid != expected_oid:
            raise EvidenceError(f"Git object mismatch for {path}")
        entries.append(GitEntry(path=path, mode=mode, oid=oid, data=output[data_start:data_end]))
        cursor = data_end + 1
    if cursor != len(output):
        raise EvidenceError("unexpected trailing git cat-file output")
    return entries


def read_git_tree(repo: Path, commit: str) -> list[GitEntry]:
    """Read a complete bounded Git tree."""
    return read_git_blobs(repo, read_git_tree_metadata(repo, commit))


def entry_map(entries: Iterable[GitEntry]) -> dict[str, GitEntry]:
    return {entry.path: entry for entry in entries}


def require_tracked_file(files: dict[str, GitEntry], path: str, label: str) -> GitEntry:
    safe_source_path(path)
    entry = files.get(path)
    if entry is None or entry.is_symlink or not entry.data:
        raise EvidenceError(f"{label} must be a non-empty tracked regular file: {path}")
    return entry


def matches_prefix(path: str, prefix: str) -> bool:
    normalized = prefix.rstrip("/")
    return path == normalized or path.startswith(normalized + "/")


def validate_inventory(inventory: Any, files: dict[str, GitEntry]) -> tuple[dict[str, Any], str]:
    if not isinstance(inventory, dict) or inventory.get("schema_version") != 1:
        raise EvidenceError("component inventory schema_version must be 1")

    generator_path = inventory.get("generator_path")
    if generator_path != "scripts/build_release_evidence.py":
        raise EvidenceError("component inventory has an invalid generator_path")
    require_tracked_file(files, generator_path, "generator_path")

    project = inventory.get("project")
    if not isinstance(project, dict):
        raise EvidenceError("component inventory requires a project object")
    required_project_fields = {
        "spdx_id",
        "name",
        "version_source",
        "license_declared",
        "download_location",
        "supplier",
        "evidence_builder",
    }
    missing = sorted(required_project_fields - project.keys())
    if missing:
        raise EvidenceError(f"project inventory fields are missing: {', '.join(missing)}")
    project_spdx_id = require_nonempty_string(project["spdx_id"], "project spdx_id")
    if not SPDX_ID_RE.fullmatch(project_spdx_id):
        raise EvidenceError("project spdx_id is invalid")
    project_name = require_nonempty_string(project["name"], "project name", 128)
    if not re.fullmatch(r"[a-z0-9][a-z0-9.-]*", project_name):
        raise EvidenceError("project name is not artifact-safe")
    require_nonempty_string(project["license_declared"], "project license_declared", 512)
    require_nonempty_string(project["supplier"], "project supplier", 512)
    require_https_url(project["download_location"], "project download_location")
    require_https_url(project["evidence_builder"], "project evidence_builder")
    if project["version_source"] != "VERSION":
        raise EvidenceError("project version_source must be VERSION")
    version_entry = require_tracked_file(files, "VERSION", "project version_source")
    version_match = VERSION_RE.fullmatch(version_entry.data)
    if version_match is None:
        raise EvidenceError(
            f"could not derive the Melkor version from {project['version_source']}; "
            f"it must contain exactly one SemVer line"
        )
    version = version_match.group(1).decode("ascii")

    dependency_manifests = inventory.get("dependency_manifests")
    if (
        not isinstance(dependency_manifests, list)
        or not dependency_manifests
        or not all(isinstance(path, str) for path in dependency_manifests)
    ):
        raise EvidenceError("dependency_manifests must be a non-empty list")
    if len(set(dependency_manifests)) != len(dependency_manifests):
        raise EvidenceError("dependency_manifests contains duplicates")
    for path in dependency_manifests:
        require_tracked_file(files, path, "dependency manifest")
    third_party_evidence = set(dependency_manifests)

    extracted = inventory.get("extracted_licenses", [])
    if not isinstance(extracted, list):
        raise EvidenceError("extracted_licenses must be a list")
    extracted_ids: set[str] = set()
    for license_info in extracted:
        if not isinstance(license_info, dict):
            raise EvidenceError("each extracted license must be an object")
        license_id = license_info.get("license_id")
        text_file = license_info.get("text_file")
        if not isinstance(license_id, str) or not LICENSE_REF_RE.fullmatch(license_id):
            raise EvidenceError("extracted license IDs must start with LicenseRef-")
        if license_id in extracted_ids:
            raise EvidenceError(f"duplicate extracted license ID: {license_id}")
        extracted_ids.add(license_id)
        if not isinstance(text_file, str):
            raise EvidenceError(f"{license_id} requires text_file")
        require_tracked_file(files, text_file, f"license text for {license_id}")
        third_party_evidence.add(text_file)
        require_nonempty_string(license_info.get("name"), f"license name for {license_id}")
        see_also = license_info.get("see_also", [])
        if not isinstance(see_also, list):
            raise EvidenceError(f"license see_also must be a list: {license_id}")
        for index, url in enumerate(see_also):
            require_https_url(url, f"license see_also[{index}] for {license_id}")

    components = inventory.get("components")
    if not isinstance(components, list) or not components:
        raise EvidenceError("components must be a non-empty list")
    component_ids: set[str] = {project_spdx_id}
    component_names: set[str] = set()
    tracked_paths = tuple(files)
    vendored_owners: dict[str, str] = {}
    artifact_owners: dict[str, str] = {}
    for component in components:
        if not isinstance(component, dict):
            raise EvidenceError("each component must be an object")
        required = {
            "spdx_id",
            "name",
            "version",
            "distribution",
            "license_declared",
            "license_files",
            "download_location",
        }
        missing = sorted(required - component.keys())
        if missing:
            raise EvidenceError(f"component fields are missing: {', '.join(missing)}")
        spdx_id = require_nonempty_string(component["spdx_id"], "component spdx_id")
        name = require_nonempty_string(component["name"], "component name", 256)
        if not SPDX_ID_RE.fullmatch(spdx_id):
            raise EvidenceError(f"invalid component spdx_id: {spdx_id}")
        if spdx_id in component_ids:
            raise EvidenceError(f"duplicate component spdx_id: {spdx_id}")
        if not name or name in component_names:
            raise EvidenceError(f"duplicate or empty component name: {name!r}")
        component_ids.add(spdx_id)
        component_names.add(name)

        require_nonempty_string(component["version"], f"component version for {name}", 256)
        require_nonempty_string(
            component["license_declared"], f"component license_declared for {name}", 512
        )
        require_https_url(component["download_location"], f"component download_location for {name}")
        if "supplier" in component:
            require_nonempty_string(component["supplier"], f"component supplier for {name}")

        distribution = component["distribution"]
        if distribution not in {"vendored", "external-runtime"}:
            raise EvidenceError(f"unsupported distribution for {name}: {distribution}")
        paths = component.get("paths", [])
        evidence_paths = component.get("evidence_paths", [])
        artifacts = component.get("artifacts", [])
        if not isinstance(paths, list) or not all(isinstance(item, str) for item in paths):
            raise EvidenceError(f"component paths must be strings: {name}")
        if not isinstance(evidence_paths, list) or not all(
            isinstance(item, str) for item in evidence_paths
        ):
            raise EvidenceError(f"component evidence_paths must be strings: {name}")
        if not isinstance(artifacts, list):
            raise EvidenceError(f"component artifacts must be a list: {name}")
        if len(set(paths)) != len(paths) or len(set(evidence_paths)) != len(evidence_paths):
            raise EvidenceError(f"component paths contain duplicates: {name}")
        if distribution == "vendored":
            if not paths:
                raise EvidenceError(f"vendored component has no paths: {name}")
            for prefix in paths:
                normalized_prefix = prefix.rstrip("/")
                safe_source_path(normalized_prefix)
                if not normalized_prefix.startswith("third_party/"):
                    raise EvidenceError(
                        f"vendored component path must be below third_party/: {name}: {prefix}"
                    )
                matched_paths = [path for path in tracked_paths if matches_prefix(path, prefix)]
                if not matched_paths:
                    raise EvidenceError(
                        f"vendored component path matches no tracked files: {name}: {prefix}"
                    )
                for path in matched_paths:
                    previous = vendored_owners.get(path)
                    if previous is not None and previous != name:
                        raise EvidenceError(
                            f"vendored file belongs to multiple components: "
                            f"{path}: {previous}, {name}"
                        )
                    vendored_owners[path] = name
        else:
            if paths:
                raise EvidenceError(f"external component cannot define source paths: {name}")
            if not evidence_paths or not artifacts:
                raise EvidenceError(
                    f"external component needs evidence_paths and artifacts: {name}"
                )
        for path in evidence_paths:
            require_tracked_file(files, path, f"component evidence for {name}")
            third_party_evidence.add(path)
        evidence_bytes = b"\n".join(files[path].data for path in evidence_paths)
        for artifact in artifacts:
            if not isinstance(artifact, dict):
                raise EvidenceError(f"component artifact must be an object: {name}")
            artifact_path = artifact.get("path")
            artifact_url = artifact.get("url")
            artifact_sha256 = artifact.get("sha256")
            if not isinstance(artifact_path, str):
                raise EvidenceError(f"component artifact path is invalid: {name}")
            safe_source_path(artifact_path)
            require_https_url(artifact_url, f"component artifact URL for {name}")
            if not isinstance(artifact_sha256, str) or not SHA256_RE.fullmatch(artifact_sha256):
                raise EvidenceError(f"component artifact digest is invalid: {name}")
            previous = artifact_owners.get(artifact_path)
            if previous is not None:
                raise EvidenceError(
                    f"component artifact path is duplicated: {artifact_path}: {previous}, {name}"
                )
            artifact_owners[artifact_path] = name
            if artifact_sha256.encode("ascii") not in evidence_bytes:
                raise EvidenceError(
                    f"component artifact digest is absent from its evidence: "
                    f"{name}: {artifact_path}"
                )
            if artifact_url.encode("utf-8") not in evidence_bytes:
                raise EvidenceError(
                    f"component artifact URL is absent from its evidence: "
                    f"{name}: {artifact_path}"
                )

        license_files = component["license_files"]
        if not isinstance(license_files, list) or not license_files:
            raise EvidenceError(f"component has no license_files: {name}")
        if not all(isinstance(path, str) for path in license_files):
            raise EvidenceError(f"component license paths must be strings: {name}")
        if len(set(license_files)) != len(license_files):
            raise EvidenceError(f"component license_files contains duplicates: {name}")
        for path in license_files:
            require_tracked_file(files, path, f"component license for {name}")
            third_party_evidence.add(path)

        declared = component["license_declared"]
        missing_refs = sorted(set(LICENSE_REF_RE.findall(declared)) - extracted_ids)
        if missing_refs:
            raise EvidenceError(
                f"component {name} references undefined licenses: {', '.join(missing_refs)}"
            )

    unowned_third_party = sorted(
        path
        for path in tracked_paths
        if path.startswith("third_party/")
        and path not in vendored_owners
        and path not in third_party_evidence
    )
    if unowned_third_party:
        raise EvidenceError(
            "third-party source has no component or evidence owner: "
            + ", ".join(unowned_third_party)
        )
    return inventory, version


def load_and_validate_inventory(
    entries: list[GitEntry],
) -> tuple[dict[str, Any], str, str]:
    files = entry_map(entries)
    inventory_entry = require_tracked_file(files, INVENTORY_PATH, "component inventory")
    try:
        inventory = strict_json_loads(inventory_entry.data.decode("utf-8"), "component inventory")
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceError("component inventory is not valid UTF-8 JSON") from exc
    validated, version = validate_inventory(inventory, files)
    return validated, version, sha256_bytes(inventory_entry.data)


def validate_executing_generator(entries: list[GitEntry], inventory: dict[str, Any]) -> None:
    """Bind the running tool to the generator recorded in the selected tree."""
    files = entry_map(entries)
    generator_path = str(inventory["generator_path"])
    selected = require_tracked_file(files, generator_path, "generator_path")
    try:
        executing = Path(__file__).resolve().read_bytes()
    except OSError as exc:
        raise EvidenceError("cannot read the executing release-evidence generator") from exc
    if executing != selected.data:
        raise EvidenceError(
            "executing release-evidence generator does not match the selected "
            f"Git tree: {generator_path}"
        )


def execute_verified_python(name: str, path: Path, source: bytes) -> Any:
    """Execute verified source bytes without consulting a bytecode cache."""
    module = types.ModuleType(name)
    module.__file__ = str(path)
    module.__package__ = ""
    previous = sys.modules.get(name)
    sys.modules[name] = module
    try:
        code = compile(source, str(path), "exec", dont_inherit=True)
        exec(code, module.__dict__)
    finally:
        if previous is None:
            sys.modules.pop(name, None)
        else:
            sys.modules[name] = previous
    return module


def load_publish_tool(entries: list[GitEntry]) -> Any:
    """Load the exact atomic publisher recorded in the selected tree."""
    selected = require_tracked_file(entry_map(entries), PUBLISH_TOOL_PATH, "publish tool")
    tool_path = Path(__file__).resolve().parent.parent / PUBLISH_TOOL_PATH
    try:
        executing = tool_path.read_bytes()
    except OSError as exc:
        raise EvidenceError("cannot read the atomic publish tool") from exc
    if executing != selected.data:
        raise EvidenceError(
            f"atomic publish tool does not match the selected Git tree: {PUBLISH_TOOL_PATH}"
        )

    try:
        tool = execute_verified_python("melkor_release_atomic_publish", tool_path, selected.data)
    except Exception as exc:
        raise EvidenceError("cannot load the atomic publish tool") from exc
    if not callable(getattr(tool, "publish", None)):
        raise EvidenceError("atomic publish tool does not provide publish")
    return tool


def load_source_policy(entries: list[GitEntry]) -> Any:
    """Load the source-bundle policy after its selected copy matches this checkout."""
    selected = require_tracked_file(entry_map(entries), SOURCE_POLICY_PATH, "source-bundle policy")
    policy_path = Path(__file__).resolve().parent.parent / SOURCE_POLICY_PATH
    try:
        executing = policy_path.read_bytes()
    except OSError as exc:
        raise EvidenceError("cannot read the source-bundle policy") from exc
    if executing != selected.data:
        raise EvidenceError(
            "executing source-bundle policy does not match the selected Git tree: "
            f"{SOURCE_POLICY_PATH}"
        )

    try:
        policy = execute_verified_python("melkor_release_source_policy", policy_path, selected.data)
    except Exception as exc:
        raise EvidenceError("cannot load the source-bundle policy") from exc
    if not callable(getattr(policy, "select_entries", None)):
        raise EvidenceError("source-bundle policy does not provide select_entries")
    return policy


def select_source_entries(
    entries: list[GitEntry], policy: Any
) -> tuple[list[GitEntry], list[tuple[str, str]]]:
    """Apply the approved source boundary to one selected Git tree."""
    try:
        included, excluded = policy.select_entries([(entry.mode, entry.path) for entry in entries])
    except Exception as exc:
        raise EvidenceError("source-bundle policy rejected the selected Git tree") from exc
    included_set = set(included)
    excluded_paths = {path for path, _reason in excluded}
    all_paths = {entry.path for entry in entries}
    if len(included_set) != len(included) or len(excluded_paths) != len(excluded):
        raise EvidenceError("source-bundle policy returned duplicate paths")
    if (included_set & excluded_paths) or (included_set | excluded_paths) != all_paths:
        raise EvidenceError("source-bundle policy did not classify each tracked path once")
    return [entry for entry in entries if entry.path in included_set], excluded


def build_file_records(entries: list[GitEntry]) -> list[FileRecord]:
    records: list[FileRecord] = []
    lfs_pointers: list[str] = []
    for entry in entries:
        if entry.is_symlink:
            raise EvidenceError(f"the source policy selected a symbolic link: {entry.path}")
        pointer_oid = parse_lfs_pointer(entry.data)
        if pointer_oid is not None:
            lfs_pointers.append(f"{entry.path} ({pointer_oid})")
        records.append(
            FileRecord(
                entry=entry,
                sha1=hashlib.sha1(entry.data, usedforsecurity=False).hexdigest(),
                sha256=sha256_bytes(entry.data),
            )
        )
    if lfs_pointers:
        detail = "\n  - ".join(lfs_pointers)
        raise EvidenceError(
            f"unresolved Git LFS pointer(s) would make the source archive incomplete:\n  - {detail}"
        )
    return records


def write_source_archive(
    destination: Path, records: list[FileRecord], prefix: str, commit_epoch: int
) -> None:
    with destination.open("wb") as raw_file:
        with gzip.GzipFile(
            filename="",
            mode="wb",
            compresslevel=9,
            fileobj=raw_file,
            mtime=0,
        ) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for record in records:
                    entry = record.entry
                    member = tarfile.TarInfo(f"{prefix}/{entry.path}")
                    member.uid = 0
                    member.gid = 0
                    member.uname = ""
                    member.gname = ""
                    member.mtime = commit_epoch
                    member.pax_headers = {}
                    if entry.is_symlink:
                        raise EvidenceError(
                            f"source archives cannot contain symbolic links: {entry.path}"
                        )
                    member.type = tarfile.REGTYPE
                    member.mode = 0o755 if entry.mode == "100755" else 0o644
                    member.size = len(entry.data)
                    archive.addfile(member, io.BytesIO(entry.data))


def package_verification_code(records: Iterable[FileRecord]) -> str:
    concatenated = "".join(sorted(record.sha1 for record in records)).encode("ascii")
    return hashlib.sha1(concatenated, usedforsecurity=False).hexdigest()


def package_verification_code_from_digests(digests: Iterable[str]) -> str:
    """Return the SPDX package verification code for SHA-1 file digests."""

    concatenated = "".join(sorted(digests)).encode("ascii")
    return hashlib.sha1(concatenated, usedforsecurity=False).hexdigest()


def file_spdx_id(path: str) -> str:
    return "SPDXRef-File-" + hashlib.sha256(path.encode("utf-8")).hexdigest()[:24]


def component_records(component: dict[str, Any], records: list[FileRecord]) -> list[FileRecord]:
    if component["distribution"] != "vendored":
        return []
    prefixes = component.get("paths", [])
    return [
        record
        for record in records
        if any(matches_prefix(record.entry.path, prefix) for prefix in prefixes)
    ]


def build_spdx_document(
    inventory: dict[str, Any],
    version: str,
    commit: str,
    created: str,
    inventory_sha256: str,
    records: list[FileRecord],
) -> dict[str, Any]:
    project = inventory["project"]
    project_id = project["spdx_id"]
    packages: list[dict[str, Any]] = [
        {
            "SPDXID": project_id,
            "copyrightText": "NOASSERTION",
            "downloadLocation": project["download_location"],
            "filesAnalyzed": True,
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": project["license_declared"],
            "licenseInfoFromFiles": ["NOASSERTION"],
            "name": project["name"],
            "packageVerificationCode": {
                "packageVerificationCodeValue": package_verification_code(records)
            },
            "primaryPackagePurpose": "APPLICATION",
            "supplier": project["supplier"],
            "versionInfo": version,
        }
    ]
    relationships: list[dict[str, str]] = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": project_id,
        }
    ]
    for record in records:
        relationships.append(
            {
                "spdxElementId": project_id,
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": file_spdx_id(record.entry.path),
            }
        )

    for component in inventory["components"]:
        matched = component_records(component, records)
        package: dict[str, Any] = {
            "SPDXID": component["spdx_id"],
            "copyrightText": "NOASSERTION",
            "downloadLocation": component["download_location"],
            "filesAnalyzed": bool(matched),
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": component["license_declared"],
            "name": component["name"],
            "primaryPackagePurpose": "LIBRARY",
            "supplier": component.get("supplier", "NOASSERTION"),
            "versionInfo": component["version"],
        }
        if matched:
            package["licenseInfoFromFiles"] = ["NOASSERTION"]
            package["packageVerificationCode"] = {
                "packageVerificationCodeValue": package_verification_code(matched)
            }
        elif component.get("artifacts"):
            package["comment"] = "External artifacts: " + ", ".join(
                f"{artifact['path']} (sha256:{artifact['sha256']})"
                for artifact in component["artifacts"]
            )
        packages.append(package)
        relationships.append(
            {
                "spdxElementId": project_id,
                "relationshipType": (
                    "CONTAINS" if component["distribution"] == "vendored" else "DEPENDS_ON"
                ),
                "relatedSpdxElement": component["spdx_id"],
            }
        )
        for record in matched:
            relationships.append(
                {
                    "spdxElementId": component["spdx_id"],
                    "relationshipType": "CONTAINS",
                    "relatedSpdxElement": file_spdx_id(record.entry.path),
                }
            )

    files = [
        {
            "SPDXID": file_spdx_id(record.entry.path),
            "checksums": [
                {"algorithm": "SHA1", "checksumValue": record.sha1},
                {"algorithm": "SHA256", "checksumValue": record.sha256},
            ],
            "copyrightText": "NOASSERTION",
            "fileName": f"./{record.entry.path}",
            "licenseConcluded": "NOASSERTION",
            "licenseInfoInFiles": ["NOASSERTION"],
        }
        for record in records
    ]
    extracted_licenses = []
    files_by_path = entry_map(record.entry for record in records)
    for license_info in inventory.get("extracted_licenses", []):
        item = {
            "extractedText": files_by_path[license_info["text_file"]].data.decode(
                "utf-8", "strict"
            ),
            "licenseId": license_info["license_id"],
            "name": license_info["name"],
        }
        if license_info.get("see_also"):
            item["seeAlsos"] = license_info["see_also"]
        extracted_licenses.append(item)

    namespace = (
        project["download_location"].rstrip("/")
        + "/release-evidence/"
        + quote(version, safe="")
        + "/"
        + commit
    )
    document: dict[str, Any] = {
        "SPDXID": "SPDXRef-DOCUMENT",
        "comment": (
            "Deterministic source SBOM generated from release/components.json "
            f"sha256:{inventory_sha256}. External runtime components are declared "
            "but not file-analyzed."
        ),
        "creationInfo": {
            "created": created,
            "creators": [f"Tool: melkor-release-evidence-{GENERATOR_VERSION}"],
            "licenseListVersion": "3.25",
        },
        "dataLicense": "CC0-1.0",
        "documentNamespace": namespace,
        "files": files,
        "name": f"{project['name']}-{version}-{commit[:12]}-source",
        "packages": packages,
        "relationships": relationships,
        "spdxVersion": "SPDX-2.3",
    }
    if extracted_licenses:
        document["hasExtractedLicensingInfos"] = extracted_licenses
    return document


def build_provenance(
    inventory: dict[str, Any],
    version: str,
    commit: str,
    ref: str,
    release_tag: str | None,
    created: str,
    commit_epoch: int,
    prefix: str,
    inventory_sha256: str,
    records: list[FileRecord],
    excluded_tracked_file_count: int,
    subjects: list[tuple[str, str]],
) -> dict[str, Any]:
    files = entry_map(record.entry for record in records)
    resolved_dependencies: list[dict[str, Any]] = [
        {
            "digest": {"gitCommit": commit},
            "uri": f"git+{inventory['project']['download_location']}@{commit}",
        }
    ]
    for path in sorted(inventory["dependency_manifests"]):
        resolved_dependencies.append(
            {
                "digest": {"sha256": sha256_bytes(files[path].data)},
                "uri": f"file:{path}",
            }
        )
    for component in inventory["components"]:
        for artifact in component.get("artifacts", []):
            resolved_dependencies.append(
                {
                    "digest": {"sha256": artifact["sha256"]},
                    "uri": artifact["url"],
                }
            )
    return {
        "_type": "https://in-toto.io/Statement/v1",
        "predicate": {
            "buildDefinition": {
                "buildType": ("https://github.com/sepahead/melkor/release-evidence/v1"),
                "externalParameters": {
                    "commit": commit,
                    "ref": ref,
                    "releaseTag": release_tag,
                    "version": version,
                },
                "internalParameters": {
                    "archivePrefix": prefix,
                    "componentIds": [component["spdx_id"] for component in inventory["components"]],
                    "componentInventorySha256": inventory_sha256,
                    "generatorSha256": sha256_bytes(files[inventory["generator_path"]].data),
                    "publishToolPath": PUBLISH_TOOL_PATH,
                    "publishToolSha256": sha256_bytes(files[PUBLISH_TOOL_PATH].data),
                    "excludedTrackedFileCount": excluded_tracked_file_count,
                    "lfsPointerPolicy": "reject",
                    "sourcePolicyPath": SOURCE_POLICY_PATH,
                    "sourcePolicySha256": sha256_bytes(files[SOURCE_POLICY_PATH].data),
                    "sourceCommitEpoch": commit_epoch,
                    "sourceCommitTimestamp": created,
                    "sourceFileCount": len(records),
                },
                "resolvedDependencies": resolved_dependencies,
            },
            "runDetails": {
                "builder": {"id": inventory["project"]["evidence_builder"]},
                "metadata": {
                    "invocationId": f"urn:git:{commit}",
                },
            },
        },
        "predicateType": "https://slsa.dev/provenance/v1",
        "subject": [
            {"digest": {"sha256": digest}, "name": name} for name, digest in sorted(subjects)
        ],
    }


def source_manifest_bytes(records: list[FileRecord]) -> bytes:
    return "".join(
        f"{record.sha256}  {record.entry.mode}  {record.entry.path}\n" for record in records
    ).encode("utf-8")


def write_checksums(directory: Path, names: Iterable[str]) -> None:
    lines = [f"{sha256_file(directory / name)}  {name}\n" for name in sorted(names)]
    (directory / "SHA256SUMS").write_text("".join(lines), encoding="utf-8")


def resolve_commit(repo: Path, ref: str) -> str:
    try:
        commit = (
            run_git(repo, "rev-parse", "--verify", "--end-of-options", f"{ref}^{{commit}}")
            .decode("ascii", "strict")
            .strip()
        )
    except UnicodeDecodeError as exc:
        raise EvidenceError("Git returned a non-ASCII commit object ID") from exc
    if not GIT_OBJECT_ID_RE.fullmatch(commit):
        raise EvidenceError("Git returned an invalid commit object ID")
    return commit


def validate_release_tag(repo: Path, tag: str, version: str, commit: str) -> None:
    if tag != f"v{version}":
        raise EvidenceError(f"release tag {tag!r} does not match source version v{version}")
    tag_ref = f"refs/tags/{tag}"
    object_type = run_git(repo, "cat-file", "-t", tag_ref).decode().strip()
    if object_type != "tag":
        raise EvidenceError(f"release tag must be annotated: {tag}")
    tagged_commit = resolve_commit(repo, tag_ref)
    if tagged_commit != commit:
        raise EvidenceError(
            f"release tag {tag} resolves to {tagged_commit}, not selected commit {commit}"
        )


def commit_metadata(repo: Path, commit: str) -> tuple[int, str]:
    epoch_text = run_git(repo, "show", "-s", "--format=%ct", commit).decode().strip()
    try:
        epoch = int(epoch_text)
    except ValueError as exc:
        raise EvidenceError("Git commit timestamp is invalid") from exc
    if epoch < 0:
        raise EvidenceError("Git commit timestamp is negative")
    try:
        created = (
            dt.datetime.fromtimestamp(epoch, tz=dt.timezone.utc)
            .replace(microsecond=0)
            .isoformat()
            .replace("+00:00", "Z")
        )
    except (OverflowError, OSError, ValueError) as exc:
        raise EvidenceError("Git commit timestamp is outside the supported range") from exc
    return epoch, created


def parse_checksums(path: Path) -> dict[str, str]:
    try:
        raw = read_bounded_file(path, MAX_CHECKSUM_FILE_BYTES, "SHA256SUMS")
        lines = raw.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise EvidenceError(f"cannot read {path}") from exc
    checksums: dict[str, str] = {}
    for line in lines:
        if len(line) < 67 or line[64:66] != "  ":
            raise EvidenceError("SHA256SUMS contains a malformed line")
        digest, name = line[:64], line[66:]
        if (
            not SHA256_RE.fullmatch(digest)
            or not re.fullmatch(r"[A-Za-z0-9._+-]+", name)
            or Path(name).name != name
        ):
            raise EvidenceError("SHA256SUMS contains an unsafe or invalid entry")
        if name == "SHA256SUMS" or name in checksums:
            raise EvidenceError(f"SHA256SUMS contains a duplicate entry: {name}")
        checksums[name] = digest
    if not checksums:
        raise EvidenceError("SHA256SUMS is empty")
    canonical = "".join(f"{checksums[name]}  {name}\n" for name in sorted(checksums)).encode(
        "utf-8"
    )
    if raw != canonical:
        raise EvidenceError("SHA256SUMS is not in canonical sorted form")
    return checksums


def parse_source_manifest(data: bytes) -> dict[str, tuple[str, str]]:
    if not data.endswith(b"\n"):
        raise EvidenceError("source-file manifest must end with one newline")
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise EvidenceError("source-file manifest is not UTF-8") from exc
    records: dict[str, tuple[str, str]] = {}
    portable_paths: dict[str, str] = {}
    for line in lines:
        if len(line) < 75 or line[64:66] != "  " or line[72:74] != "  ":
            raise EvidenceError("source-file manifest contains a malformed line")
        digest, mode, path = line[:64], line[66:72], line[74:]
        if not SHA256_RE.fullmatch(digest) or mode not in {"100644", "100755"}:
            raise EvidenceError("source-file manifest contains an invalid digest or Git mode")
        safe_source_path(path)
        if path in records:
            raise EvidenceError(f"source-file manifest contains a duplicate: {path}")
        portable_key = portable_path_key(path)
        previous = portable_paths.get(portable_key)
        if previous is not None:
            raise EvidenceError(
                f"source-file manifest has a portable path collision: {previous!r} and {path!r}"
            )
        portable_paths[portable_key] = path
        records[path] = (digest, mode)
        if len(records) > MAX_SOURCE_FILES:
            raise EvidenceError(f"source-file manifest has more than {MAX_SOURCE_FILES} files")
    if not records:
        raise EvidenceError("source-file manifest is empty")
    if list(records) != sorted(records):
        raise EvidenceError("source-file manifest paths are not sorted")
    return records


def verify_archive(
    archive_path: Path,
    expected_files: dict[str, tuple[str, str]],
    prefix: str,
    commit_epoch: int,
) -> ArchiveVerification:
    with archive_path.open("rb") as handle:
        header = handle.read(10)
    if header != b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff":
        raise EvidenceError("source archive does not use a deterministic gzip header")

    observed: dict[str, tuple[str, str]] = {}
    sha1_by_path: dict[str, str] = {}
    captured_files: dict[str, bytes] = {}
    member_order: list[str] = []
    total_bytes = 0
    try:
        with (
            archive_path.open("rb") as raw_file,
            tempfile.SpooledTemporaryFile(max_size=8 * 1024 * 1024) as expanded_file,
            tempfile.SpooledTemporaryFile(max_size=8 * 1024 * 1024) as canonical_file,
            tempfile.SpooledTemporaryFile(max_size=8 * 1024 * 1024) as canonical_gzip_file,
        ):
            raw_stat = os.fstat(raw_file.fileno())
            if not stat.S_ISREG(raw_stat.st_mode) or raw_stat.st_size > MAX_EVIDENCE_ARCHIVE_BYTES:
                raise EvidenceError("source archive is not a bounded regular file")
            expanded_size = 0
            with gzip.GzipFile(fileobj=raw_file, mode="rb") as compressed:
                while True:
                    chunk = compressed.read(1024 * 1024)
                    if not chunk:
                        break
                    expanded_size += len(chunk)
                    if expanded_size > MAX_EXPANDED_ARCHIVE_BYTES:
                        raise EvidenceError("expanded source archive exceeds its size limit")
                    expanded_file.write(chunk)
            expanded_file.seek(0)
            with (
                tarfile.open(fileobj=expanded_file, mode="r:") as archive,
                tarfile.open(
                    fileobj=canonical_file, mode="w", format=tarfile.PAX_FORMAT
                ) as canonical_archive,
            ):
                for member in archive:
                    if len(member_order) >= MAX_SOURCE_FILES:
                        raise EvidenceError(f"archive has more than {MAX_SOURCE_FILES} members")
                    required_prefix = prefix + "/"
                    if not member.name.startswith(required_prefix):
                        raise EvidenceError(f"archive member has the wrong prefix: {member.name}")
                    relative = member.name[len(required_prefix) :]
                    safe_source_path(relative)
                    if relative in observed:
                        raise EvidenceError(f"archive contains duplicate path: {relative}")
                    if relative not in expected_files:
                        raise EvidenceError(f"archive contains an unlisted path: {relative}")
                    if (
                        member.uid != 0
                        or member.gid != 0
                        or member.uname
                        or member.gname
                        or member.mtime != commit_epoch
                    ):
                        raise EvidenceError(
                            f"archive member has non-canonical metadata: {relative}"
                        )

                    canonical_member = tarfile.TarInfo(member.name)
                    canonical_member.uid = 0
                    canonical_member.gid = 0
                    canonical_member.uname = ""
                    canonical_member.gname = ""
                    canonical_member.mtime = commit_epoch
                    canonical_member.pax_headers = {}
                    if member.isreg():
                        if member.size < 0 or member.size > MAX_SOURCE_FILE_BYTES:
                            raise EvidenceError(
                                f"archive member exceeds its size limit: {relative}"
                            )
                        total_bytes += member.size
                        if total_bytes > MAX_SOURCE_TOTAL_BYTES:
                            raise EvidenceError("archive content exceeds its total size limit")
                        extracted = archive.extractfile(member)
                        if extracted is None:
                            raise EvidenceError(f"cannot read archive member: {relative}")
                        digest = hashlib.sha256()
                        sha1_digest = hashlib.sha1(usedforsecurity=False)
                        probe = bytearray()
                        capture = bytearray() if relative in {INVENTORY_PATH, "VERSION"} else None
                        extracted_size = 0
                        while True:
                            chunk = extracted.read(1024 * 1024)
                            if not chunk:
                                break
                            extracted_size += len(chunk)
                            if extracted_size > member.size:
                                raise EvidenceError(
                                    f"archive member is longer than declared: {relative}"
                                )
                            if len(probe) < 4096:
                                probe.extend(chunk[: 4096 - len(probe)])
                            digest.update(chunk)
                            sha1_digest.update(chunk)
                            if capture is not None:
                                capture.extend(chunk)
                        if extracted_size != member.size:
                            raise EvidenceError(
                                f"archive member is shorter than declared: {relative}"
                            )
                        data_digest = digest.hexdigest()
                        sha1_by_path[relative] = sha1_digest.hexdigest()
                        if capture is not None:
                            captured_files[relative] = bytes(capture)
                        mode = "100755" if member.mode == 0o755 else "100644"
                        if member.mode not in {0o644, 0o755}:
                            raise EvidenceError(
                                f"archive member has non-canonical mode: {relative}"
                            )
                        canonical_member.type = tarfile.REGTYPE
                        canonical_member.mode = member.mode
                        canonical_member.size = member.size
                        canonical_data = archive.extractfile(member)
                        if canonical_data is None:
                            raise EvidenceError(f"cannot read archive member: {relative}")
                        canonical_archive.addfile(canonical_member, canonical_data)
                    else:
                        raise EvidenceError(f"archive contains unsupported member: {relative}")
                    pointer = parse_lfs_pointer(bytes(probe))
                    if pointer is not None:
                        raise EvidenceError(f"archive contains unresolved LFS pointer: {relative}")
                    observed[relative] = (data_digest, mode)
                    member_order.append(relative)

            expanded_file.seek(0)
            canonical_file.seek(0)
            while True:
                expanded_chunk = expanded_file.read(1024 * 1024)
                canonical_chunk = canonical_file.read(1024 * 1024)
                if expanded_chunk != canonical_chunk:
                    raise EvidenceError("source archive is not a canonical tar stream")
                if not expanded_chunk:
                    break

            canonical_file.seek(0)
            with gzip.GzipFile(
                filename="",
                mode="wb",
                compresslevel=9,
                fileobj=canonical_gzip_file,
                mtime=0,
            ) as canonical_compressed:
                while True:
                    canonical_chunk = canonical_file.read(1024 * 1024)
                    if not canonical_chunk:
                        break
                    canonical_compressed.write(canonical_chunk)

            raw_file.seek(0)
            canonical_gzip_file.seek(0)
            while True:
                raw_chunk = raw_file.read(1024 * 1024)
                canonical_gzip_chunk = canonical_gzip_file.read(1024 * 1024)
                if raw_chunk != canonical_gzip_chunk:
                    raise EvidenceError("source archive is not a canonical gzip stream")
                if not raw_chunk:
                    break
    except (tarfile.TarError, OSError) as exc:
        raise EvidenceError(f"cannot inspect source archive: {archive_path}") from exc
    if member_order != sorted(member_order):
        raise EvidenceError("archive members are not sorted deterministically")
    if observed != expected_files:
        missing = sorted(set(expected_files) - set(observed))
        extra = sorted(set(observed) - set(expected_files))
        changed = sorted(
            path
            for path in set(observed) & set(expected_files)
            if observed[path] != expected_files[path]
        )
        raise EvidenceError(
            "archive does not match source-file manifest "
            f"(missing={missing}, extra={extra}, changed={changed})"
        )
    return ArchiveVerification(sha1_by_path, captured_files)


def strict_json_loads(text: str, label: str) -> Any:
    """Parse JSON and reject duplicate object names."""

    def object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        value: dict[str, Any] = {}
        for name, item in pairs:
            if name in value:
                raise EvidenceError(f"{label} contains a duplicate object name: {name}")
            value[name] = item
        return value

    def reject_constant(value: str) -> None:
        raise EvidenceError(f"{label} contains a non-JSON number: {value}")

    def finite_float(value: str) -> float:
        parsed = float(value)
        if not math.isfinite(parsed):
            raise EvidenceError(f"{label} contains a non-finite JSON number: {value}")
        return parsed

    try:
        value = json.loads(
            text,
            object_pairs_hook=object_without_duplicates,
            parse_constant=reject_constant,
            parse_float=finite_float,
        )
    except RecursionError as exc:
        raise EvidenceError(f"{label} exceeds the JSON nesting limit") from exc

    stack: list[tuple[Any, int]] = [(value, 1)]
    while stack:
        item, depth = stack.pop()
        if depth > MAX_JSON_DEPTH:
            raise EvidenceError(f"{label} exceeds the JSON nesting limit")
        if isinstance(item, dict):
            stack.extend((child, depth + 1) for child in item.values())
        elif isinstance(item, list):
            stack.extend((child, depth + 1) for child in item)
    return value


def load_json(path: Path, label: str, maximum: int = MAX_EVIDENCE_METADATA_BYTES) -> dict[str, Any]:
    try:
        raw = read_bounded_file(path, maximum, label)
        text = raw.decode("utf-8")
        value = strict_json_loads(text, label)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"{label} is not valid UTF-8 JSON: {path}") from exc
    if not isinstance(value, dict):
        raise EvidenceError(f"{label} must be a JSON object")
    if raw != canonical_json_bytes(value):
        raise EvidenceError(f"{label} is not canonical JSON")
    return value


def verify_evidence(directory: Path) -> None:
    if not directory.is_dir():
        raise EvidenceError(f"evidence directory does not exist: {directory}")
    directory_entries: list[Path] = []
    for path in directory.iterdir():
        if path.is_symlink() or not path.is_file():
            raise EvidenceError(
                f"evidence directory contains nested or non-regular entries: {[path.name]}"
            )
        directory_entries.append(path)
        if len(directory_entries) > 5:
            raise EvidenceError("evidence directory must contain exactly five files")
    checksums = parse_checksums(directory / "SHA256SUMS")
    actual_files = {path.name for path in directory_entries}
    expected_files = set(checksums) | {"SHA256SUMS"}
    if actual_files != expected_files:
        raise EvidenceError(
            "evidence directory has unlisted or missing files: "
            f"expected={sorted(expected_files)}, actual={sorted(actual_files)}"
        )

    def exactly_one(suffix: str) -> str:
        matches = sorted(name for name in checksums if name.endswith(suffix))
        if len(matches) != 1:
            raise EvidenceError(f"expected exactly one {suffix} artifact")
        return matches[0]

    archive_name = exactly_one(".tar.gz")
    source_manifest_name = exactly_one(".source-files.sha256")
    spdx_name = exactly_one(".spdx.json")
    provenance_name = exactly_one(".provenance.json")
    required_artifacts = {
        archive_name,
        source_manifest_name,
        spdx_name,
        provenance_name,
    }
    if set(checksums) != required_artifacts:
        raise EvidenceError("SHA256SUMS must list exactly four release artifacts")

    size_limits = {
        archive_name: MAX_EVIDENCE_ARCHIVE_BYTES,
        source_manifest_name: MAX_EVIDENCE_METADATA_BYTES,
        spdx_name: MAX_EVIDENCE_METADATA_BYTES,
        provenance_name: 16 * 1024 * 1024,
    }
    for name, expected in checksums.items():
        actual = sha256_file(directory / name, size_limits[name])
        if actual != expected:
            raise EvidenceError(
                f"checksum mismatch for {name}: expected {expected}, found {actual}"
            )

    provenance = load_json(directory / provenance_name, "provenance", size_limits[provenance_name])
    if provenance.get("_type") != "https://in-toto.io/Statement/v1":
        raise EvidenceError("provenance statement type is invalid")
    if provenance.get("predicateType") != "https://slsa.dev/provenance/v1":
        raise EvidenceError("provenance predicate type is invalid")
    try:
        build_definition = provenance["predicate"]["buildDefinition"]
        internal = build_definition["internalParameters"]
        external = build_definition["externalParameters"]
        prefix = internal["archivePrefix"]
        commit = external["commit"]
        commit_epoch = internal["sourceCommitEpoch"]
    except (KeyError, TypeError) as exc:
        raise EvidenceError("provenance statement is incomplete") from exc
    if (
        not isinstance(build_definition, dict)
        or not isinstance(internal, dict)
        or not isinstance(external, dict)
    ):
        raise EvidenceError("provenance build definition is invalid")
    if not isinstance(prefix, str) or not re.fullmatch(r"[A-Za-z0-9._+-]+", prefix):
        raise EvidenceError("provenance archivePrefix is unsafe")
    if not isinstance(commit, str) or not GIT_OBJECT_ID_RE.fullmatch(commit):
        raise EvidenceError("provenance commit is invalid")
    version = external.get("version")
    try:
        version_bytes = version.encode("ascii") if isinstance(version, str) else b""
    except UnicodeEncodeError:
        version_bytes = b""
    if VERSION_RE.fullmatch(version_bytes) is None:
        raise EvidenceError("provenance version is invalid")
    ref = external.get("ref")
    if (
        not isinstance(ref, str)
        or not ref
        or any(ord(char) < 32 or ord(char) == 127 for char in ref)
    ):
        raise EvidenceError("provenance ref is invalid")
    release_tag = external.get("releaseTag")
    if release_tag is not None and release_tag != f"v{version}":
        raise EvidenceError("provenance releaseTag does not match its version")
    component_id_list = internal.get("componentIds")
    if (
        not isinstance(component_id_list, list)
        or not component_id_list
        or not all(
            isinstance(item, str) and SPDX_ID_RE.fullmatch(item) for item in component_id_list
        )
    ):
        raise EvidenceError("provenance componentIds are invalid")
    component_ids = set(component_id_list)
    if len(component_ids) != len(component_id_list):
        raise EvidenceError("provenance componentIds contain duplicates")
    if internal.get("lfsPointerPolicy") != "reject":
        raise EvidenceError("provenance does not enforce the LFS pointer policy")
    if build_definition.get("buildType") != (
        "https://github.com/sepahead/melkor/release-evidence/v1"
    ):
        raise EvidenceError("provenance buildType is invalid")
    run_details = provenance.get("predicate", {}).get("runDetails")
    if not isinstance(run_details, dict) or not isinstance(run_details.get("builder"), dict):
        raise EvidenceError("provenance builder is invalid")
    require_https_url(run_details["builder"].get("id"), "provenance builder ID")
    if type(commit_epoch) is not int or commit_epoch < 0:
        raise EvidenceError("provenance sourceCommitEpoch is invalid")
    try:
        expected_created = (
            dt.datetime.fromtimestamp(commit_epoch, tz=dt.timezone.utc)
            .replace(microsecond=0)
            .isoformat()
            .replace("+00:00", "Z")
        )
    except (OverflowError, OSError, ValueError) as exc:
        raise EvidenceError("provenance sourceCommitEpoch is outside the supported range") from exc
    if internal.get("sourceCommitTimestamp") != expected_created:
        raise EvidenceError("provenance sourceCommitTimestamp does not match its epoch")
    expected_names = {
        f"{prefix}.tar.gz",
        f"{prefix}.source-files.sha256",
        f"{prefix}.spdx.json",
        f"{prefix}.provenance.json",
    }
    if required_artifacts != expected_names:
        raise EvidenceError("release artifact names do not match the provenance prefix")

    subjects: dict[str, str] = {}
    raw_subjects = provenance.get("subject")
    if not isinstance(raw_subjects, list):
        raise EvidenceError("provenance subjects must be a list")
    for subject in raw_subjects:
        try:
            name = subject["name"]
            digest = subject["digest"]["sha256"]
        except (KeyError, TypeError) as exc:
            raise EvidenceError("provenance contains an invalid subject") from exc
        if name in subjects or name not in checksums or not SHA256_RE.fullmatch(digest):
            raise EvidenceError(f"provenance subject is invalid: {name!r}")
        subjects[name] = digest
    expected_subjects = {archive_name, source_manifest_name, spdx_name}
    if set(subjects) != expected_subjects:
        raise EvidenceError("provenance subjects do not cover the release evidence")
    for name, digest in subjects.items():
        if digest != checksums[name]:
            raise EvidenceError(f"provenance digest does not match SHA256SUMS: {name}")

    spdx = load_json(directory / spdx_name, "SPDX SBOM", size_limits[spdx_name])
    if spdx.get("spdxVersion") != "SPDX-2.3" or spdx.get("SPDXID") != "SPDXRef-DOCUMENT":
        raise EvidenceError("SPDX document identity is invalid")
    namespace = require_https_url(spdx.get("documentNamespace"), "SPDX documentNamespace")
    if not namespace.endswith("/" + commit):
        raise EvidenceError("SPDX namespace is not bound to the provenance commit")
    creation_info = spdx.get("creationInfo")
    if not isinstance(creation_info, dict) or creation_info.get("created") != expected_created:
        raise EvidenceError("SPDX creation time does not match the source commit")
    raw_packages = spdx.get("packages")
    if not isinstance(raw_packages, list) or not raw_packages:
        raise EvidenceError("SPDX packages are invalid")
    packages: dict[str, dict[str, Any]] = {}
    for package in raw_packages:
        if not isinstance(package, dict):
            raise EvidenceError("SPDX package entry is invalid")
        package_id = package.get("SPDXID")
        if (
            not isinstance(package_id, str)
            or not SPDX_ID_RE.fullmatch(package_id)
            or package_id in packages
        ):
            raise EvidenceError(f"SPDX package ID is invalid: {package_id!r}")
        packages[package_id] = package

    raw_relationships = spdx.get("relationships")
    if not isinstance(raw_relationships, list):
        raise EvidenceError("SPDX relationships are invalid")
    relationships: set[tuple[str, str, str]] = set()
    for relationship in raw_relationships:
        if not isinstance(relationship, dict):
            raise EvidenceError("SPDX relationship entry is invalid")
        item = (
            relationship.get("spdxElementId"),
            relationship.get("relationshipType"),
            relationship.get("relatedSpdxElement"),
        )
        if not all(isinstance(value, str) and value for value in item):
            raise EvidenceError("SPDX relationship fields are invalid")
        typed_item = (item[0], item[1], item[2])
        if typed_item in relationships:
            raise EvidenceError("SPDX relationships contain a duplicate")
        relationships.add(typed_item)
    describes = [
        related
        for source, kind, related in relationships
        if source == "SPDXRef-DOCUMENT" and kind == "DESCRIBES"
    ]
    if len(describes) != 1 or describes[0] not in packages:
        raise EvidenceError("SPDX document must describe exactly one project package")
    project_id = describes[0]
    if project_id in component_ids or set(packages) != component_ids | {project_id}:
        raise EvidenceError("SPDX packages do not match the inventoried components")
    if packages[project_id].get("versionInfo") != version:
        raise EvidenceError("SPDX project version does not match the provenance version")
    if packages[project_id].get("filesAnalyzed") is not True:
        raise EvidenceError("SPDX project package must analyze its source files")
    for component_id in component_ids:
        component = packages[component_id]
        if not isinstance(component.get("filesAnalyzed"), bool):
            raise EvidenceError(f"SPDX filesAnalyzed is invalid for {component_id}")
        expected_kind = "CONTAINS" if component.get("filesAnalyzed") is True else "DEPENDS_ON"
        if (project_id, expected_kind, component_id) not in relationships:
            raise EvidenceError(f"SPDX project relationship is missing for {component_id}")

    raw_spdx_files = spdx.get("files")
    if not isinstance(raw_spdx_files, list):
        raise EvidenceError("SPDX source file inventory is invalid")
    spdx_files: dict[str, str] = {}
    spdx_sha1: dict[str, str] = {}
    for item in raw_spdx_files:
        if not isinstance(item, dict):
            raise EvidenceError("SPDX source file entry is invalid")
        file_name = item.get("fileName")
        if not isinstance(file_name, str) or not file_name.startswith("./"):
            raise EvidenceError("SPDX source file path is invalid")
        relative = file_name[2:]
        safe_source_path(relative)
        file_id = item.get("SPDXID")
        if file_id != file_spdx_id(relative):
            raise EvidenceError(f"SPDX source file ID is invalid: {relative}")
        raw_checksums = item.get("checksums")
        if not isinstance(raw_checksums, list):
            raise EvidenceError(f"SPDX source file checksums are invalid: {relative}")
        if not all(
            isinstance(checksum, dict) and set(checksum) == {"algorithm", "checksumValue"}
            for checksum in raw_checksums
        ):
            raise EvidenceError(f"SPDX source file checksums are invalid: {relative}")
        checksum_map = {
            checksum["algorithm"]: checksum["checksumValue"] for checksum in raw_checksums
        }
        if (
            relative in spdx_files
            or len(raw_checksums) != 2
            or set(checksum_map) != {"SHA1", "SHA256"}
            or not isinstance(checksum_map["SHA1"], str)
            or not SHA1_RE.fullmatch(checksum_map["SHA1"])
            or not isinstance(checksum_map["SHA256"], str)
            or not SHA256_RE.fullmatch(checksum_map["SHA256"])
        ):
            raise EvidenceError(f"SPDX source file digest is invalid: {relative}")
        spdx_files[relative] = checksum_map["SHA256"]
        spdx_sha1[relative] = checksum_map["SHA1"]

    source_manifest = parse_source_manifest(
        read_bounded_file(
            directory / source_manifest_name,
            size_limits[source_manifest_name],
            "source-file manifest",
        )
    )
    manifest_files = {path: digest for path, (digest, _mode) in source_manifest.items()}
    if spdx_files != manifest_files:
        raise EvidenceError("SPDX file inventory does not match the source-file manifest")
    archive_verification = verify_archive(
        directory / archive_name, source_manifest, prefix, commit_epoch
    )
    if spdx_sha1 != archive_verification.sha1_by_path:
        raise EvidenceError("SPDX SHA-1 inventory does not match the source archive")

    raw_inventory = archive_verification.captured_files.get(INVENTORY_PATH)
    raw_version = archive_verification.captured_files.get("VERSION")
    if raw_inventory is None or raw_version is None:
        raise EvidenceError("source archive is missing required release metadata")
    try:
        inventory = strict_json_loads(raw_inventory.decode("utf-8"), "component inventory")
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceError("archived component inventory is not valid UTF-8 JSON") from exc
    if not isinstance(inventory, dict) or inventory.get("schema_version") != 1:
        raise EvidenceError("archived component inventory is invalid")
    version_match = VERSION_RE.fullmatch(raw_version)
    if version_match is None or version_match.group(1).decode("ascii") != version:
        raise EvidenceError("archived VERSION does not match the provenance version")

    project_inventory = inventory.get("project")
    if not isinstance(project_inventory, dict) or project_inventory.get("spdx_id") != project_id:
        raise EvidenceError("SPDX project does not match the component inventory")
    if inventory.get("generator_path") != "scripts/build_release_evidence.py":
        raise EvidenceError("archived component inventory has an invalid generator path")
    if project_inventory.get("version_source") != "VERSION":
        raise EvidenceError("archived component inventory has an invalid version source")
    project_download = require_https_url(
        project_inventory.get("download_location"),
        "archived project download_location",
    )
    expected_namespace = (
        project_download.rstrip("/") + "/release-evidence/" + quote(version, safe="") + "/" + commit
    )
    if namespace != expected_namespace:
        raise EvidenceError("SPDX namespace does not match the component inventory")
    project_builder = require_https_url(
        project_inventory.get("evidence_builder"),
        "archived project evidence_builder",
    )
    if run_details["builder"].get("id") != project_builder:
        raise EvidenceError("provenance builder does not match the component inventory")
    run_metadata = run_details.get("metadata")
    if (
        not isinstance(run_metadata, dict)
        or run_metadata.get("invocationId") != f"urn:git:{commit}"
    ):
        raise EvidenceError("provenance invocation does not match the source commit")
    project_package = packages[project_id]
    expected_project_fields = {
        "name": project_inventory.get("name"),
        "versionInfo": version,
        "licenseDeclared": project_inventory.get("license_declared"),
        "downloadLocation": project_inventory.get("download_location"),
        "supplier": project_inventory.get("supplier"),
        "filesAnalyzed": True,
    }
    if any(project_package.get(key) != value for key, value in expected_project_fields.items()):
        raise EvidenceError("SPDX project package differs from the component inventory")
    project_code = project_package.get("packageVerificationCode")
    expected_project_code = package_verification_code_from_digests(spdx_sha1.values())
    if (
        not isinstance(project_code, dict)
        or project_code.get("packageVerificationCodeValue") != expected_project_code
    ):
        raise EvidenceError("SPDX project package verification code is invalid")

    inventory_components = inventory.get("components")
    if not isinstance(inventory_components, list):
        raise EvidenceError("archived component inventory has no component list")
    inventory_component_ids: set[str] = set()
    owned_paths: dict[str, str] = {}
    evidence_paths: set[str] = set()
    dependency_manifests = inventory.get("dependency_manifests")
    if (
        not isinstance(dependency_manifests, list)
        or not dependency_manifests
        or not all(isinstance(path, str) for path in dependency_manifests)
        or len(set(dependency_manifests)) != len(dependency_manifests)
    ):
        raise EvidenceError("archived dependency manifest list is invalid")
    evidence_paths.update(dependency_manifests)
    expected_dependencies: list[dict[str, Any]] = [
        {
            "digest": {"gitCommit": commit},
            "uri": f"git+{project_download}@{commit}",
        }
    ]
    for path in sorted(dependency_manifests):
        safe_source_path(path)
        manifest_record = source_manifest.get(path)
        if manifest_record is None:
            raise EvidenceError(f"dependency manifest is absent from the source archive: {path}")
        expected_dependencies.append(
            {
                "digest": {"sha256": manifest_record[0]},
                "uri": f"file:{path}",
            }
        )
    extracted_licenses = inventory.get("extracted_licenses", [])
    if not isinstance(extracted_licenses, list):
        raise EvidenceError("archived extracted license list is invalid")
    for license_info in extracted_licenses:
        if not isinstance(license_info, dict) or not isinstance(license_info.get("text_file"), str):
            raise EvidenceError("archived extracted license entry is invalid")
        evidence_paths.add(license_info["text_file"])

    expected_relationships = {
        ("SPDXRef-DOCUMENT", "DESCRIBES", project_id),
        *((project_id, "CONTAINS", file_spdx_id(path)) for path in source_manifest),
    }
    for component_inventory in inventory_components:
        if not isinstance(component_inventory, dict):
            raise EvidenceError("archived component entry is invalid")
        component_id = component_inventory.get("spdx_id")
        if (
            not isinstance(component_id, str)
            or component_id not in component_ids
            or component_id in inventory_component_ids
        ):
            raise EvidenceError("archived component ID is invalid or duplicated")
        inventory_component_ids.add(component_id)
        component_package = packages[component_id]
        expected_component_fields = {
            "name": component_inventory.get("name"),
            "versionInfo": component_inventory.get("version"),
            "licenseDeclared": component_inventory.get("license_declared"),
            "downloadLocation": component_inventory.get("download_location"),
            "supplier": component_inventory.get("supplier", "NOASSERTION"),
        }
        if any(
            component_package.get(key) != value for key, value in expected_component_fields.items()
        ):
            raise EvidenceError(f"SPDX package differs from inventory: {component_id}")

        artifacts = component_inventory.get("artifacts", [])
        if not isinstance(artifacts, list):
            raise EvidenceError(f"component artifact list is invalid: {component_id}")
        for artifact in artifacts:
            if not isinstance(artifact, dict):
                raise EvidenceError(f"component artifact is invalid: {component_id}")
            artifact_path = artifact.get("path")
            artifact_digest = artifact.get("sha256")
            if not isinstance(artifact_path, str):
                raise EvidenceError(f"component artifact path is invalid: {component_id}")
            safe_source_path(artifact_path)
            artifact_url = require_https_url(
                artifact.get("url"), f"component artifact URL for {component_id}"
            )
            if not isinstance(artifact_digest, str) or not SHA256_RE.fullmatch(artifact_digest):
                raise EvidenceError(f"component artifact digest is invalid: {component_id}")
            expected_dependencies.append(
                {
                    "digest": {"sha256": artifact_digest},
                    "uri": artifact_url,
                }
            )

        license_files = component_inventory.get("license_files")
        component_evidence = component_inventory.get("evidence_paths", [])
        if not isinstance(license_files, list) or not all(
            isinstance(path, str) for path in license_files
        ):
            raise EvidenceError(f"component license list is invalid: {component_id}")
        if not isinstance(component_evidence, list) or not all(
            isinstance(path, str) for path in component_evidence
        ):
            raise EvidenceError(f"component evidence list is invalid: {component_id}")
        evidence_paths.update(license_files)
        evidence_paths.update(component_evidence)

        distribution = component_inventory.get("distribution")
        paths = component_inventory.get("paths", [])
        if not isinstance(paths, list) or not all(isinstance(path, str) for path in paths):
            raise EvidenceError(f"component source paths are invalid: {component_id}")
        if distribution == "vendored":
            if not paths or component_package.get("filesAnalyzed") is not True:
                raise EvidenceError(f"vendored SPDX package is incomplete: {component_id}")
            matched_paths: set[str] = set()
            for path_prefix in paths:
                normalized_prefix = path_prefix.rstrip("/")
                safe_source_path(normalized_prefix)
                if not normalized_prefix.startswith("third_party/"):
                    raise EvidenceError(f"vendored path is outside third_party/: {component_id}")
                matches = {path for path in source_manifest if matches_prefix(path, path_prefix)}
                if not matches:
                    raise EvidenceError(f"vendored path has no source files: {component_id}")
                matched_paths.update(matches)
            for path in matched_paths:
                previous = owned_paths.get(path)
                if previous is not None:
                    raise EvidenceError(f"source file belongs to multiple SPDX packages: {path}")
                owned_paths[path] = component_id
                expected_relationships.add((component_id, "CONTAINS", file_spdx_id(path)))
            expected_relationships.add((project_id, "CONTAINS", component_id))
            component_code = component_package.get("packageVerificationCode")
            expected_component_code = package_verification_code_from_digests(
                spdx_sha1[path] for path in matched_paths
            )
            if (
                not isinstance(component_code, dict)
                or component_code.get("packageVerificationCodeValue") != expected_component_code
            ):
                raise EvidenceError(f"SPDX component verification code is invalid: {component_id}")
        elif distribution == "external-runtime":
            if (
                paths
                or not artifacts
                or not component_evidence
                or component_package.get("filesAnalyzed") is not False
            ):
                raise EvidenceError(f"external SPDX package is invalid: {component_id}")
            expected_relationships.add((project_id, "DEPENDS_ON", component_id))
        else:
            raise EvidenceError(f"component distribution is invalid: {component_id}")

    if inventory_component_ids != component_ids:
        raise EvidenceError("SPDX components do not match the archived inventory")
    expected_component_ids = [
        component.get("spdx_id")
        for component in inventory_components
        if isinstance(component, dict)
    ]
    if component_id_list != expected_component_ids:
        raise EvidenceError("provenance componentIds do not match the component inventory order")
    if build_definition.get("resolvedDependencies") != expected_dependencies:
        raise EvidenceError("provenance resolvedDependencies do not match the source inventory")
    for path in evidence_paths:
        safe_source_path(path)
        if path not in source_manifest:
            raise EvidenceError(f"component evidence is absent from the source archive: {path}")
    unowned_third_party = sorted(
        path
        for path in source_manifest
        if path.startswith("third_party/")
        and path not in owned_paths
        and path not in evidence_paths
    )
    if unowned_third_party:
        raise EvidenceError(
            "source archive has unowned third-party files: " + ", ".join(unowned_third_party)
        )
    for path in source_manifest:
        project_relationship = (project_id, "CONTAINS", file_spdx_id(path))
        if project_relationship not in relationships:
            raise EvidenceError(f"SPDX project does not contain source file: {path}")
    if relationships != expected_relationships:
        raise EvidenceError("SPDX relationships do not match the archived component inventory")
    source_file_count = internal.get("sourceFileCount")
    if type(source_file_count) is not int or source_file_count != len(source_manifest):
        raise EvidenceError("provenance sourceFileCount does not match the manifest")
    source_policy_path = internal.get("sourcePolicyPath")
    if source_policy_path != SOURCE_POLICY_PATH:
        raise EvidenceError("provenance sourcePolicyPath is invalid")
    source_policy = source_manifest.get(source_policy_path)
    if source_policy is None:
        raise EvidenceError("source-file manifest is missing the source-bundle policy")
    if internal.get("sourcePolicySha256") != source_policy[0]:
        raise EvidenceError("provenance sourcePolicySha256 does not match the manifest")
    inventory_record = source_manifest.get(INVENTORY_PATH)
    if inventory_record is None:
        raise EvidenceError("source-file manifest is missing the component inventory")
    if internal.get("componentInventorySha256") != inventory_record[0]:
        raise EvidenceError("provenance componentInventorySha256 does not match the manifest")
    generator_record = source_manifest.get("scripts/build_release_evidence.py")
    if generator_record is None:
        raise EvidenceError("source-file manifest is missing the evidence generator")
    if internal.get("generatorSha256") != generator_record[0]:
        raise EvidenceError("provenance generatorSha256 does not match the manifest")
    if internal.get("publishToolPath") != PUBLISH_TOOL_PATH:
        raise EvidenceError("provenance publishToolPath is invalid")
    publish_tool_record = source_manifest.get(PUBLISH_TOOL_PATH)
    if publish_tool_record is None:
        raise EvidenceError("source-file manifest is missing the atomic publish tool")
    if internal.get("publishToolSha256") != publish_tool_record[0]:
        raise EvidenceError("provenance publishToolSha256 does not match the manifest")
    excluded_count = internal.get("excludedTrackedFileCount")
    if type(excluded_count) is not int or excluded_count < 0:
        raise EvidenceError("provenance excludedTrackedFileCount is invalid")


def build_evidence(repo: Path, ref: str, output: Path, release_tag: str | None) -> Path:
    repo = repo.resolve()
    if run_git(repo, "rev-parse", "--is-inside-work-tree").strip() != b"true":
        raise EvidenceError(f"not a Git working tree: {repo}")
    commit = resolve_commit(repo, ref)
    tracked_metadata = read_git_tree_metadata(repo, commit)
    policy_metadata = [entry for entry in tracked_metadata if entry.path == SOURCE_POLICY_PATH]
    if len(policy_metadata) != 1:
        raise EvidenceError("the selected Git tree is missing the source-bundle policy")
    source_policy = load_source_policy(read_git_blobs(repo, policy_metadata))
    selected_metadata, excluded_entries = select_source_entries(tracked_metadata, source_policy)
    entries = read_git_blobs(repo, selected_metadata)
    inventory, version, inventory_sha256 = load_and_validate_inventory(entries)
    validate_executing_generator(entries, inventory)
    publish_tool = load_publish_tool(entries)
    records = build_file_records(entries)
    if release_tag is not None:
        validate_release_tag(repo, release_tag, version, commit)
        prefix = f"{inventory['project']['name']}-{version}"
    else:
        prefix = f"{inventory['project']['name']}-{version}-{commit[:12]}"
    commit_epoch, created = commit_metadata(repo, commit)

    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise EvidenceError(f"output path already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output = output.parent.resolve(strict=True) / output.name
    if output.exists() or output.is_symlink():
        raise EvidenceError(f"output path already exists: {output}")
    staging: Path | None = Path(tempfile.mkdtemp(prefix=f".{output.name}-", dir=str(output.parent)))
    try:
        archive_name = f"{prefix}.tar.gz"
        source_manifest_name = f"{prefix}.source-files.sha256"
        spdx_name = f"{prefix}.spdx.json"
        provenance_name = f"{prefix}.provenance.json"

        write_source_archive(staging / archive_name, records, prefix, commit_epoch)
        (staging / source_manifest_name).write_bytes(source_manifest_bytes(records))
        spdx = build_spdx_document(
            inventory,
            version,
            commit,
            created,
            inventory_sha256,
            records,
        )
        (staging / spdx_name).write_bytes(canonical_json_bytes(spdx))
        subjects = [
            (archive_name, sha256_file(staging / archive_name)),
            (source_manifest_name, sha256_file(staging / source_manifest_name)),
            (spdx_name, sha256_file(staging / spdx_name)),
        ]
        provenance = build_provenance(
            inventory,
            version,
            commit,
            ref,
            release_tag,
            created,
            commit_epoch,
            prefix,
            inventory_sha256,
            records,
            len(excluded_entries),
            subjects,
        )
        (staging / provenance_name).write_bytes(canonical_json_bytes(provenance))
        write_checksums(
            staging,
            [archive_name, source_manifest_name, spdx_name, provenance_name],
        )
        verify_evidence(staging)
        try:
            publish_tool.publish(staging, output, False)
        except FileExistsError as exc:
            raise EvidenceError(f"output path already exists: {output}") from exc
        except (OSError, ValueError) as exc:
            raise EvidenceError("release evidence could not be published atomically") from exc
        staging = None
    except Exception:
        if staging is not None:
            shutil.rmtree(staging, ignore_errors=True)
        raise
    print(f"release evidence: {escape_diagnostic(str(output))}")
    print(f"source commit: {commit}")
    print(f"source files: {len(records)}")
    return output


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    build = subparsers.add_parser("build", help="build evidence from an exact Git ref")
    build.add_argument("--repo", type=Path, default=Path.cwd())
    build.add_argument("--ref", default="HEAD")
    build.add_argument("--output", type=Path, required=True)
    build.add_argument(
        "--release-tag",
        help="enforce an annotated v<version> tag and use the final artifact name",
    )
    verify = subparsers.add_parser("verify", help="verify a generated evidence directory")
    verify.add_argument("evidence", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "build":
            build_evidence(args.repo, args.ref, args.output, args.release_tag)
        else:
            verify_evidence(args.evidence.resolve())
            print("release evidence verified: " + escape_diagnostic(str(args.evidence.resolve())))
    except (EvidenceError, OSError) as exc:
        print(f"Error: {escape_diagnostic(str(exc))}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
