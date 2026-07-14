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
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable
from urllib.parse import quote


INVENTORY_PATH = "release/components.json"
GENERATOR_VERSION = "1"
LFS_HEADER = b"version https://git-lfs.github.com/spec/v1\n"
LFS_OID_RE = re.compile(rb"^oid sha256:([0-9a-f]{64})$")
# The authoritative version is the single line in the root VERSION file. Evidence must
# read the same source the build reads; deriving it by re-parsing CMakeLists.txt would
# reintroduce the second version definition this project just removed.
VERSION_RE = re.compile(
    rb"^\s*([0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?)\s*$", re.MULTILINE
)
SPDX_ID_RE = re.compile(r"^SPDXRef-[A-Za-z0-9.-]+$")
LICENSE_REF_RE = re.compile(r"LicenseRef-[A-Za-z0-9.-]+")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


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


def canonical_json_bytes(value: Any) -> bytes:
    return (
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


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
    if not path or path.startswith("/") or "\\" in path:
        raise EvidenceError(f"unsafe tracked path: {path!r}")
    if any(ord(char) < 32 or ord(char) == 127 for char in path):
        raise EvidenceError(f"tracked path contains control characters: {path!r}")
    parts = PurePosixPath(path).parts
    if any(part in {"", ".", ".."} for part in parts):
        raise EvidenceError(f"unsafe tracked path: {path!r}")
    if parts[0] == ".git":
        raise EvidenceError("the source tree must not contain .git entries")


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


def read_git_tree(repo: Path, commit: str) -> list[GitEntry]:
    listing = run_git(repo, "ls-tree", "-rz", "--full-tree", commit)
    metadata: list[tuple[str, str, str]] = []
    seen_paths: set[str] = set()
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
        if raw_type != b"blob":
            kind = raw_type.decode("ascii", "replace")
            raise EvidenceError(
                f"tracked entry {path} is {kind}, not a self-contained blob"
            )
        mode = raw_mode.decode("ascii")
        if mode not in {"100644", "100755", "120000"}:
            raise EvidenceError(f"unsupported Git mode {mode} for {path}")
        metadata.append((path, mode, raw_oid.decode("ascii")))

    if not metadata:
        raise EvidenceError("the selected Git tree is empty")

    batch_input = b"".join(f"{oid}\n".encode("ascii") for _, _, oid in metadata)
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
    for path, mode, expected_oid in metadata:
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
    return sorted(entries, key=lambda entry: entry.path)


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


def validate_inventory(
    inventory: Any, files: dict[str, GitEntry]
) -> tuple[dict[str, Any], str]:
    if not isinstance(inventory, dict) or inventory.get("schema_version") != 1:
        raise EvidenceError("component inventory schema_version must be 1")

    generator_path = inventory.get("generator_path")
    if not isinstance(generator_path, str):
        raise EvidenceError("component inventory requires generator_path")
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
    if not SPDX_ID_RE.fullmatch(str(project["spdx_id"])):
        raise EvidenceError("project spdx_id is invalid")
    if not re.fullmatch(r"[a-z0-9][a-z0-9.-]*", str(project["name"])):
        raise EvidenceError("project name is not artifact-safe")
    version_entry = require_tracked_file(
        files, str(project["version_source"]), "project version_source"
    )
    version_match = VERSION_RE.search(version_entry.data)
    if version_match is None:
        raise EvidenceError(
            f"could not derive the Melkor version from {project['version_source']}; "
            f"it must contain exactly one SemVer line"
        )
    version = version_match.group(1).decode("ascii")

    dependency_manifests = inventory.get("dependency_manifests")
    if not isinstance(dependency_manifests, list) or not dependency_manifests:
        raise EvidenceError("dependency_manifests must be a non-empty list")
    if len(set(dependency_manifests)) != len(dependency_manifests):
        raise EvidenceError("dependency_manifests contains duplicates")
    for path in dependency_manifests:
        if not isinstance(path, str):
            raise EvidenceError("dependency manifest paths must be strings")
        require_tracked_file(files, path, "dependency manifest")

    extracted = inventory.get("extracted_licenses", [])
    if not isinstance(extracted, list):
        raise EvidenceError("extracted_licenses must be a list")
    extracted_ids: set[str] = set()
    for license_info in extracted:
        if not isinstance(license_info, dict):
            raise EvidenceError("each extracted license must be an object")
        license_id = license_info.get("license_id")
        text_file = license_info.get("text_file")
        if not isinstance(license_id, str) or not license_id.startswith("LicenseRef-"):
            raise EvidenceError("extracted license IDs must start with LicenseRef-")
        if license_id in extracted_ids:
            raise EvidenceError(f"duplicate extracted license ID: {license_id}")
        extracted_ids.add(license_id)
        if not isinstance(text_file, str):
            raise EvidenceError(f"{license_id} requires text_file")
        require_tracked_file(files, text_file, f"license text for {license_id}")

    components = inventory.get("components")
    if not isinstance(components, list) or not components:
        raise EvidenceError("components must be a non-empty list")
    component_ids: set[str] = {str(project["spdx_id"])}
    component_names: set[str] = set()
    tracked_paths = tuple(files)
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
            raise EvidenceError(
                f"component fields are missing: {', '.join(missing)}"
            )
        spdx_id = str(component["spdx_id"])
        name = str(component["name"])
        if not SPDX_ID_RE.fullmatch(spdx_id):
            raise EvidenceError(f"invalid component spdx_id: {spdx_id}")
        if spdx_id in component_ids:
            raise EvidenceError(f"duplicate component spdx_id: {spdx_id}")
        if not name or name in component_names:
            raise EvidenceError(f"duplicate or empty component name: {name!r}")
        component_ids.add(spdx_id)
        component_names.add(name)

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
        if distribution == "vendored":
            if not paths:
                raise EvidenceError(f"vendored component has no paths: {name}")
            for prefix in paths:
                safe_source_path(prefix.rstrip("/"))
                if not any(matches_prefix(path, prefix) for path in tracked_paths):
                    raise EvidenceError(
                        f"vendored component path matches no tracked files: {name}: {prefix}"
                    )
        elif not evidence_paths or not artifacts:
            raise EvidenceError(
                f"external component needs evidence_paths and artifacts: {name}"
            )
        for path in evidence_paths:
            require_tracked_file(files, path, f"component evidence for {name}")
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
            if not isinstance(artifact_url, str) or not artifact_url.startswith(
                "https://"
            ):
                raise EvidenceError(f"component artifact URL is invalid: {name}")
            if not isinstance(artifact_sha256, str) or not SHA256_RE.fullmatch(
                artifact_sha256
            ):
                raise EvidenceError(f"component artifact digest is invalid: {name}")
            if artifact_sha256.encode("ascii") not in evidence_bytes:
                raise EvidenceError(
                    f"component artifact digest is absent from its evidence: "
                    f"{name}: {artifact_path}"
                )

        license_files = component["license_files"]
        if not isinstance(license_files, list) or not license_files:
            raise EvidenceError(f"component has no license_files: {name}")
        for path in license_files:
            if not isinstance(path, str):
                raise EvidenceError(f"component license paths must be strings: {name}")
            require_tracked_file(files, path, f"component license for {name}")

        declared = str(component["license_declared"])
        missing_refs = sorted(set(LICENSE_REF_RE.findall(declared)) - extracted_ids)
        if missing_refs:
            raise EvidenceError(
                f"component {name} references undefined licenses: {', '.join(missing_refs)}"
            )
        if not str(component["version"]) or not str(component["download_location"]):
            raise EvidenceError(f"component metadata is incomplete: {name}")

    return inventory, version


def load_and_validate_inventory(
    entries: list[GitEntry],
) -> tuple[dict[str, Any], str, str]:
    files = entry_map(entries)
    inventory_entry = require_tracked_file(files, INVENTORY_PATH, "component inventory")
    try:
        inventory = json.loads(inventory_entry.data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceError("component inventory is not valid UTF-8 JSON") from exc
    validated, version = validate_inventory(inventory, files)
    return validated, version, sha256_bytes(inventory_entry.data)


def validate_executing_generator(
    entries: list[GitEntry], inventory: dict[str, Any]
) -> None:
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


def build_file_records(entries: list[GitEntry]) -> list[FileRecord]:
    records: list[FileRecord] = []
    lfs_pointers: list[str] = []
    for entry in entries:
        pointer_oid = parse_lfs_pointer(entry.data)
        if pointer_oid is not None:
            lfs_pointers.append(f"{entry.path} ({pointer_oid})")
        records.append(
            FileRecord(
                entry=entry,
                sha1=hashlib.sha1(entry.data).hexdigest(),  # SPDX requires SHA-1 support.
                sha256=sha256_bytes(entry.data),
            )
        )
    if lfs_pointers:
        detail = "\n  - ".join(lfs_pointers)
        raise EvidenceError(
            "unresolved Git LFS pointer(s) would make the source archive incomplete:\n"
            f"  - {detail}"
        )
    return records


def validate_symlink(entry: GitEntry) -> str:
    try:
        target = entry.data.decode("utf-8", "strict")
    except UnicodeDecodeError as exc:
        raise EvidenceError(f"symlink target is not UTF-8: {entry.path}") from exc
    if not target or target.startswith("/") or "\x00" in target:
        raise EvidenceError(f"unsafe symlink target for {entry.path}: {target!r}")
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(entry.path), target))
    if resolved == ".." or resolved.startswith("../"):
        raise EvidenceError(f"symlink escapes the source archive: {entry.path} -> {target}")
    return target


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
            with tarfile.open(
                fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT
            ) as archive:
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
                        member.type = tarfile.SYMTYPE
                        member.mode = 0o777
                        member.size = 0
                        member.linkname = validate_symlink(entry)
                        archive.addfile(member)
                    else:
                        member.type = tarfile.REGTYPE
                        member.mode = 0o755 if entry.mode == "100755" else 0o644
                        member.size = len(entry.data)
                        archive.addfile(member, io.BytesIO(entry.data))


def package_verification_code(records: Iterable[FileRecord]) -> str:
    concatenated = "".join(sorted(record.sha1 for record in records)).encode("ascii")
    return hashlib.sha1(concatenated).hexdigest()


def file_spdx_id(path: str) -> str:
    return "SPDXRef-File-" + hashlib.sha256(path.encode("utf-8")).hexdigest()[:24]


def component_records(
    component: dict[str, Any], records: list[FileRecord]
) -> list[FileRecord]:
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
                    "CONTAINS"
                    if component["distribution"] == "vendored"
                    else "DEPENDS_ON"
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
                "buildType": (
                    "https://github.com/sepahead/melkor/release-evidence/v1"
                ),
                "externalParameters": {
                    "commit": commit,
                    "ref": ref,
                    "releaseTag": release_tag,
                    "version": version,
                },
                "internalParameters": {
                    "archivePrefix": prefix,
                    "componentIds": [
                        component["spdx_id"] for component in inventory["components"]
                    ],
                    "componentInventorySha256": inventory_sha256,
                    "generatorSha256": sha256_bytes(
                        files[inventory["generator_path"]].data
                    ),
                    "lfsPointerPolicy": "reject",
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
            {"digest": {"sha256": digest}, "name": name}
            for name, digest in sorted(subjects)
        ],
    }


def source_manifest_bytes(records: list[FileRecord]) -> bytes:
    return "".join(
        f"{record.sha256}  {record.entry.mode}  {record.entry.path}\n"
        for record in records
    ).encode("utf-8")


def write_checksums(directory: Path, names: Iterable[str]) -> None:
    lines = [f"{sha256_file(directory / name)}  {name}\n" for name in sorted(names)]
    (directory / "SHA256SUMS").write_text("".join(lines), encoding="utf-8")


def resolve_commit(repo: Path, ref: str) -> str:
    return (
        run_git(repo, "rev-parse", "--verify", "--end-of-options", f"{ref}^{{commit}}")
        .decode()
        .strip()
    )


def validate_release_tag(repo: Path, tag: str, version: str, commit: str) -> None:
    if tag != f"v{version}":
        raise EvidenceError(
            f"release tag {tag!r} does not match source version v{version}"
        )
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
    created = (
        dt.datetime.fromtimestamp(epoch, tz=dt.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )
    return epoch, created


def parse_checksums(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise EvidenceError(f"cannot read {path}") from exc
    checksums: dict[str, str] = {}
    for line in lines:
        if len(line) < 67 or line[64:66] != "  ":
            raise EvidenceError("SHA256SUMS contains a malformed line")
        digest, name = line[:64], line[66:]
        if not SHA256_RE.fullmatch(digest) or not name or Path(name).name != name:
            raise EvidenceError("SHA256SUMS contains an unsafe or invalid entry")
        if name == "SHA256SUMS" or name in checksums:
            raise EvidenceError(f"SHA256SUMS contains a duplicate entry: {name}")
        checksums[name] = digest
    if not checksums:
        raise EvidenceError("SHA256SUMS is empty")
    return checksums


def parse_source_manifest(data: bytes) -> dict[str, tuple[str, str]]:
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise EvidenceError("source-file manifest is not UTF-8") from exc
    records: dict[str, tuple[str, str]] = {}
    for line in lines:
        if len(line) < 75 or line[64:66] != "  " or line[72:74] != "  ":
            raise EvidenceError("source-file manifest contains a malformed line")
        digest, mode, path = line[:64], line[66:72], line[74:]
        if not SHA256_RE.fullmatch(digest) or mode not in {
            "100644",
            "100755",
            "120000",
        }:
            raise EvidenceError(
                "source-file manifest contains an invalid digest or Git mode"
            )
        safe_source_path(path)
        if path in records:
            raise EvidenceError(f"source-file manifest contains a duplicate: {path}")
        records[path] = (digest, mode)
    if not records:
        raise EvidenceError("source-file manifest is empty")
    return records


def verify_archive(
    archive_path: Path,
    expected_files: dict[str, tuple[str, str]],
    prefix: str,
    commit_epoch: int,
) -> None:
    with archive_path.open("rb") as handle:
        header = handle.read(10)
    if len(header) != 10 or header[:2] != b"\x1f\x8b" or header[4:8] != b"\0\0\0\0":
        raise EvidenceError("source archive does not use a deterministic gzip header")

    observed: dict[str, tuple[str, str]] = {}
    member_order: list[str] = []
    try:
        with tarfile.open(archive_path, mode="r:gz") as archive:
            for member in archive.getmembers():
                required_prefix = prefix + "/"
                if not member.name.startswith(required_prefix):
                    raise EvidenceError(f"archive member has the wrong prefix: {member.name}")
                relative = member.name[len(required_prefix) :]
                safe_source_path(relative)
                if relative in observed:
                    raise EvidenceError(f"archive contains duplicate path: {relative}")
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
                if member.isreg():
                    extracted = archive.extractfile(member)
                    if extracted is None:
                        raise EvidenceError(f"cannot read archive member: {relative}")
                    data = extracted.read()
                    mode = "100755" if member.mode == 0o755 else "100644"
                    if member.mode not in {0o644, 0o755}:
                        raise EvidenceError(
                            f"archive member has non-canonical mode: {relative}"
                        )
                elif member.issym():
                    data = member.linkname.encode("utf-8")
                    validate_symlink(
                        GitEntry(path=relative, mode="120000", oid="", data=data)
                    )
                    if member.mode != 0o777:
                        raise EvidenceError(
                            f"archive symlink has non-canonical mode: {relative}"
                        )
                    mode = "120000"
                else:
                    raise EvidenceError(f"archive contains unsupported member: {relative}")
                pointer = parse_lfs_pointer(data)
                if pointer is not None:
                    raise EvidenceError(f"archive contains unresolved LFS pointer: {relative}")
                observed[relative] = (sha256_bytes(data), mode)
                member_order.append(relative)
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


def load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"{label} is not valid UTF-8 JSON: {path}") from exc
    if not isinstance(value, dict):
        raise EvidenceError(f"{label} must be a JSON object")
    return value


def verify_evidence(directory: Path) -> None:
    if not directory.is_dir():
        raise EvidenceError(f"evidence directory does not exist: {directory}")
    directory_entries = list(directory.iterdir())
    unsupported = sorted(
        path.name
        for path in directory_entries
        if path.is_symlink() or not path.is_file()
    )
    if unsupported:
        raise EvidenceError(
            "evidence directory contains nested or non-regular entries: "
            f"{unsupported}"
        )
    checksums = parse_checksums(directory / "SHA256SUMS")
    actual_files = {path.name for path in directory_entries}
    expected_files = set(checksums) | {"SHA256SUMS"}
    if actual_files != expected_files:
        raise EvidenceError(
            "evidence directory has unlisted or missing files: "
            f"expected={sorted(expected_files)}, actual={sorted(actual_files)}"
        )
    for name, expected in checksums.items():
        actual = sha256_file(directory / name)
        if actual != expected:
            raise EvidenceError(
                f"checksum mismatch for {name}: expected {expected}, found {actual}"
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

    provenance = load_json(directory / provenance_name, "provenance")
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
        component_ids = set(internal["componentIds"])
        commit_epoch = internal["sourceCommitEpoch"]
    except (KeyError, TypeError) as exc:
        raise EvidenceError("provenance statement is incomplete") from exc
    if not isinstance(prefix, str) or not re.fullmatch(r"[A-Za-z0-9._-]+", prefix):
        raise EvidenceError("provenance archivePrefix is unsafe")
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40,64}", commit):
        raise EvidenceError("provenance commit is invalid")
    if internal.get("lfsPointerPolicy") != "reject":
        raise EvidenceError("provenance does not enforce the LFS pointer policy")
    if not isinstance(commit_epoch, int) or commit_epoch < 0:
        raise EvidenceError("provenance sourceCommitEpoch is invalid")

    subjects: dict[str, str] = {}
    for subject in provenance.get("subject", []):
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

    spdx = load_json(directory / spdx_name, "SPDX SBOM")
    if spdx.get("spdxVersion") != "SPDX-2.3" or spdx.get("SPDXID") != "SPDXRef-DOCUMENT":
        raise EvidenceError("SPDX document identity is invalid")
    if not str(spdx.get("documentNamespace", "")).endswith("/" + commit):
        raise EvidenceError("SPDX namespace is not bound to the provenance commit")
    package_ids = {
        package.get("SPDXID")
        for package in spdx.get("packages", [])
        if isinstance(package, dict)
    }
    if not component_ids.issubset(package_ids):
        raise EvidenceError("SPDX SBOM is missing inventoried components")

    source_manifest = parse_source_manifest(
        (directory / source_manifest_name).read_bytes()
    )
    if internal.get("sourceFileCount") != len(source_manifest):
        raise EvidenceError("provenance sourceFileCount does not match the manifest")
    verify_archive(
        directory / archive_name, source_manifest, prefix, commit_epoch
    )


def build_evidence(
    repo: Path, ref: str, output: Path, release_tag: str | None
) -> Path:
    repo = repo.resolve()
    if not (repo / ".git").exists():
        raise EvidenceError(f"not a Git working tree: {repo}")
    commit = resolve_commit(repo, ref)
    entries = read_git_tree(repo, commit)
    inventory, version, inventory_sha256 = load_and_validate_inventory(entries)
    validate_executing_generator(entries, inventory)
    records = build_file_records(entries)
    if release_tag is not None:
        validate_release_tag(repo, release_tag, version, commit)
        prefix = f"{inventory['project']['name']}-{version}"
    else:
        prefix = f"{inventory['project']['name']}-{version}-{commit[:12]}"
    commit_epoch, created = commit_metadata(repo, commit)

    output = output.resolve()
    if output.exists():
        raise EvidenceError(f"output path already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(prefix=f".{output.name}-", dir=str(output.parent))
    )
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
            subjects,
        )
        (staging / provenance_name).write_bytes(canonical_json_bytes(provenance))
        write_checksums(
            staging,
            [archive_name, source_manifest_name, spdx_name, provenance_name],
        )
        verify_evidence(staging)
        os.replace(staging, output)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    print(f"release evidence: {output}")
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
            print(f"release evidence verified: {args.evidence.resolve()}")
    except EvidenceError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
