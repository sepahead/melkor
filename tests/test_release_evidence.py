#!/usr/bin/env python3
"""Focused tests for the deterministic release-evidence contract."""

from __future__ import annotations

import hashlib
import gzip
import io
import json
import importlib.util
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "scripts" / "build_release_evidence.py"
SOURCE_POLICY = ROOT / "tools" / "build_source_bundle.py"
PUBLISH_TOOL = ROOT / "tools" / "atomic_publish.py"


def load_evidence_module():
    spec = importlib.util.spec_from_file_location("melkor_release_evidence_tool", TOOL)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load release-evidence module")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class RepositoryInventoryTests(unittest.TestCase):
    def test_repository_component_inventory_is_complete(self) -> None:
        module = load_evidence_module()
        inventory_path = ROOT / "release" / "components.json"
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
        required_paths = {
            "release/components.json",
            module.SOURCE_POLICY_PATH,
            module.PUBLISH_TOOL_PATH,
            inventory["generator_path"],
            inventory["project"]["version_source"],
            *inventory["dependency_manifests"],
        }
        for license_info in inventory.get("extracted_licenses", []):
            required_paths.add(license_info["text_file"])
        for component in inventory["components"]:
            required_paths.update(component["license_files"])
            required_paths.update(component.get("evidence_paths", []))
            for prefix in component.get("paths", []):
                candidate = ROOT / prefix
                if candidate.is_file():
                    required_paths.add(prefix)
                else:
                    first_file = next(
                        (path for path in sorted(candidate.rglob("*")) if path.is_file()),
                        None,
                    )
                    self.assertIsNotNone(first_file, f"empty component path: {prefix}")
                    required_paths.add(first_file.relative_to(ROOT).as_posix())

        entries = []
        for relative in sorted(required_paths):
            path = ROOT / relative
            self.assertTrue(path.is_file(), f"missing inventory evidence: {relative}")
            data = path.read_bytes()
            entries.append(
                module.GitEntry(
                    path=relative,
                    mode="100755" if os.access(path, os.X_OK) else "100644",
                    oid="0" * 40,
                    data=data,
                )
            )
        validated, version = module.validate_inventory(inventory, module.entry_map(entries))
        self.assertIs(validated, inventory)

        # Compare against the authoritative VERSION file, not a literal. Hard-coding the
        # version here would make this test one more surface that has to be remembered on
        # every bump — the exact failure mode the single version source exists to remove.
        expected = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        self.assertEqual(version, expected)

    def test_verified_python_executes_selected_bytes_only(self) -> None:
        module = load_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "policy.py"
            path.write_text("IDENTITY = 'unverified-file'\n", encoding="utf-8")
            loaded = module.execute_verified_python(
                "melkor_selected_source_test",
                path,
                b"IDENTITY = 'selected-tree'\n",
            )
        self.assertEqual(loaded.IDENTITY, "selected-tree")
        self.assertNotIn("melkor_selected_source_test", sys.modules)


class ReleaseEvidenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.repo = Path(self.tempdir.name) / "repo"
        self.repo.mkdir()
        self.git("init", "--quiet")
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release-test@example.invalid")
        # Keep fixture commits hermetic when the invoking developer requires
        # signed commits globally; the ephemeral test repository has no key.
        self.git("config", "commit.gpgsign", "false")
        # The authoritative version is a single SemVer line in a VERSION file, matching
        # the real project. Evidence reads the same source the build reads.
        self.write("VERSION", "1.2.3\n")
        self.write("CMakeLists.txt", "project(melkor VERSION ${MELKOR_VERSION_CORE})\n")
        self.write("LICENSE", "Synthetic project license\n")
        self.write("third_party/widget/LICENSE", "Synthetic widget license\n")
        self.write("third_party/widget/widget.cpp", "int widget() { return 7; }\n")
        self.write("third_party/runtime.lock", "runtime==4.5.6\n")
        self.write("docs/external-license.txt", "Synthetic external license\n")
        self.write(
            "viewer/fetch-assets.sh",
            "#!/bin/sh\n"
            "# https://example.invalid/runtime.bin\n"
            "# sha256:"
            + "a" * 64
            + "\nexit 0\n",
            executable=True,
        )
        self.write(
            "scripts/build_release_evidence.py",
            TOOL.read_text(encoding="utf-8"),
            executable=True,
        )
        self.write(
            "tools/build_source_bundle.py",
            SOURCE_POLICY.read_text(encoding="utf-8"),
        )
        self.write(
            "tools/atomic_publish.py",
            PUBLISH_TOOL.read_text(encoding="utf-8"),
        )
        self.write_inventory(self.valid_inventory())
        self.commit("initial evidence fixture")

    def git(self, *args: str, env: dict[str, str] | None = None) -> str:
        completed = subprocess.run(
            ["git", "-C", str(self.repo), *args],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        return completed.stdout.strip()

    def write(self, relative: str, contents: str, *, executable: bool = False) -> None:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
        if executable:
            path.chmod(0o755)

    def valid_inventory(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "generator_path": "scripts/build_release_evidence.py",
            "project": {
                "spdx_id": "SPDXRef-Package-melkor",
                "name": "melkor",
                "version_source": "VERSION",
                "license_declared": "MIT",
                "download_location": "https://example.invalid/melkor",
                "supplier": "Organization: Melkor Test",
                "evidence_builder": "https://example.invalid/evidence-builder/v1",
            },
            "dependency_manifests": ["third_party/runtime.lock"],
            "extracted_licenses": [
                {
                    "license_id": "LicenseRef-Synthetic-Widget",
                    "name": "Synthetic Widget License",
                    "text_file": "third_party/widget/LICENSE",
                }
            ],
            "components": [
                {
                    "spdx_id": "SPDXRef-Package-widget",
                    "name": "widget",
                    "version": "7.0",
                    "distribution": "vendored",
                    "paths": ["third_party/widget"],
                    "license_declared": "LicenseRef-Synthetic-Widget",
                    "license_files": ["third_party/widget/LICENSE"],
                    "download_location": "https://example.invalid/widget",
                },
                {
                    "spdx_id": "SPDXRef-Package-external-runtime",
                    "name": "external-runtime",
                    "version": "4.5.6",
                    "distribution": "external-runtime",
                    "artifacts": [
                        {
                            "path": "vendor/runtime.bin",
                            "url": "https://example.invalid/runtime.bin",
                            "sha256": "a" * 64,
                        }
                    ],
                    "evidence_paths": [
                        "viewer/fetch-assets.sh",
                        "docs/external-license.txt",
                    ],
                    "license_declared": "MIT",
                    "license_files": ["docs/external-license.txt"],
                    "download_location": "https://example.invalid/runtime",
                },
            ],
        }

    def write_inventory(self, inventory: dict[str, object]) -> None:
        self.write(
            "release/components.json",
            json.dumps(inventory, indent=2, sort_keys=True) + "\n",
        )

    def commit(self, message: str) -> str:
        self.git("add", "--all")
        self.git(
            "update-index",
            "--chmod=+x",
            "scripts/build_release_evidence.py",
            "viewer/fetch-assets.sh",
        )
        sequence = len(self.git("rev-list", "--all").splitlines()) + 1
        timestamp = f"2026-01-{sequence:02d}T00:00:00Z"
        env = dict(os.environ)
        env["GIT_AUTHOR_DATE"] = timestamp
        env["GIT_COMMITTER_DATE"] = timestamp
        self.git("commit", "--quiet", "-m", message, env=env)
        return self.git("rev-parse", "HEAD")

    def run_tool(
        self, *args: object, env: dict[str, str] | None = None
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOL), *(str(arg) for arg in args)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
            env=env,
        )

    def build(
        self,
        output: Path,
        *extra: object,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        return self.run_tool(
            "build",
            "--repo",
            self.repo,
            "--ref",
            "HEAD",
            "--output",
            output,
            *extra,
            env=env,
        )

    def test_build_is_deterministic_and_self_verifying(self) -> None:
        output_a = Path(self.tempdir.name) / "evidence-a"
        output_b = Path(self.tempdir.name) / "evidence-b"
        first = self.build(output_a)
        alternate_environment = dict(os.environ)
        alternate_environment["LC_ALL"] = "C"
        alternate_environment["TZ"] = "Pacific/Honolulu"
        second = self.build(output_b, env=alternate_environment)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)

        names_a = sorted(path.name for path in output_a.iterdir())
        names_b = sorted(path.name for path in output_b.iterdir())
        self.assertEqual(names_a, names_b)
        self.assertEqual(len(names_a), 5)
        for name in names_a:
            self.assertEqual((output_a / name).read_bytes(), (output_b / name).read_bytes())

        verified = self.run_tool("verify", output_a)
        self.assertEqual(verified.returncode, 0, verified.stderr)
        archive = next(output_a.glob("*.tar.gz"))
        self.assertEqual(archive.read_bytes()[4:8], b"\0\0\0\0")
        with tarfile.open(archive, "r:gz") as source:
            members = source.getmembers()
            generator = next(
                member
                for member in members
                if member.name.endswith("/scripts/build_release_evidence.py")
            )
            self.assertEqual(generator.mode, 0o755)

        spdx = json.loads(next(output_a.glob("*.spdx.json")).read_text())
        self.assertEqual(spdx["spdxVersion"], "SPDX-2.3")
        package_ids = {package["SPDXID"] for package in spdx["packages"]}
        self.assertIn("SPDXRef-Package-widget", package_ids)
        self.assertIn("SPDXRef-Package-external-runtime", package_ids)
        provenance = json.loads(next(output_a.glob("*.provenance.json")).read_text())
        self.assertEqual(provenance["predicateType"], "https://slsa.dev/provenance/v1")
        self.assertEqual(len(provenance["subject"]), 3)
        generator_digest = hashlib.sha256(TOOL.read_bytes()).hexdigest()
        self.assertEqual(
            provenance["predicate"]["buildDefinition"]["internalParameters"]["generatorSha256"],
            generator_digest,
        )
        internal = provenance["predicate"]["buildDefinition"]["internalParameters"]
        self.assertEqual(internal["sourcePolicyPath"], "tools/build_source_bundle.py")
        self.assertEqual(
            internal["sourcePolicySha256"], hashlib.sha256(SOURCE_POLICY.read_bytes()).hexdigest()
        )
        self.assertEqual(internal["publishToolPath"], "tools/atomic_publish.py")
        self.assertEqual(
            internal["publishToolSha256"], hashlib.sha256(PUBLISH_TOOL.read_bytes()).hexdigest()
        )

    def test_all_source_surfaces_apply_the_approved_boundary(self) -> None:
        allowed = "src/fixture.cpp"
        denied = {"src/private-model.PT", "private/internal.cpp"}
        self.write(allowed, "approved source\n")
        self.write("src/private-model.PT", "restricted model\n")
        self.write("private/internal.cpp", "int private_api();\n")
        self.commit("add source-boundary fixtures")

        output = Path(self.tempdir.name) / "bounded-evidence"
        result = self.build(output)
        self.assertEqual(result.returncode, 0, result.stderr)

        archive = next(output.glob("*.tar.gz"))
        with tarfile.open(archive, "r:gz") as source:
            archive_paths = {
                member.name.split("/", 1)[1] for member in source.getmembers() if member.isfile()
            }
        module = load_evidence_module()
        manifest_paths = set(
            module.parse_source_manifest(next(output.glob("*.source-files.sha256")).read_bytes())
        )
        spdx = json.loads(next(output.glob("*.spdx.json")).read_text())
        spdx_paths = {item["fileName"].removeprefix("./") for item in spdx["files"]}

        self.assertEqual(archive_paths, manifest_paths)
        self.assertEqual(archive_paths, spdx_paths)
        self.assertIn(allowed, archive_paths)
        self.assertTrue(denied.isdisjoint(archive_paths))
        provenance = json.loads(next(output.glob("*.provenance.json")).read_text())
        internal = provenance["predicate"]["buildDefinition"]["internalParameters"]
        self.assertEqual(internal["excludedTrackedFileCount"], len(denied))

    def test_rejects_generator_that_differs_from_selected_tree(self) -> None:
        self.write(
            "scripts/build_release_evidence.py",
            "#!/usr/bin/env python3\n# unrelated generator\n",
            executable=True,
        )
        self.commit("replace the selected generator")
        result = self.build(Path(self.tempdir.name) / "wrong-generator")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("generator does not match the selected Git tree", result.stderr)

    def test_rejects_policy_that_differs_from_selected_tree(self) -> None:
        self.write(
            "tools/build_source_bundle.py",
            "def select_entries(entries):\n    return ([], entries)\n",
        )
        self.commit("replace the selected source policy")
        result = self.build(Path(self.tempdir.name) / "wrong-policy")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("policy does not match the selected Git tree", result.stderr)

    def test_rejects_publisher_that_differs_from_selected_tree(self) -> None:
        self.write("tools/atomic_publish.py", "def publish(*_args):\n    return None\n")
        self.commit("replace the selected atomic publisher")
        result = self.build(Path(self.tempdir.name) / "wrong-publisher")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("publish tool does not match the selected Git tree", result.stderr)

    def test_rejects_unresolved_lfs_pointer(self) -> None:
        self.write(
            "third_party/widget/widget.cpp",
            "version https://git-lfs.github.com/spec/v1\n"
            "oid sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
            "size 1234\n",
        )
        self.commit("replace source with pointer")
        result = self.build(Path(self.tempdir.name) / "lfs-evidence")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unresolved Git LFS pointer", result.stderr)
        self.assertFalse((Path(self.tempdir.name) / "lfs-evidence").exists())

    def test_rejects_incomplete_component_inventory(self) -> None:
        inventory = self.valid_inventory()
        components = inventory["components"]
        assert isinstance(components, list)
        components[0]["license_files"] = ["third_party/widget/MISSING"]
        self.write_inventory(inventory)
        self.commit("break component license evidence")
        result = self.build(Path(self.tempdir.name) / "invalid-inventory")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("component license", result.stderr)

    def test_inventory_rejects_ambiguous_https_urls(self) -> None:
        module = load_evidence_module()
        entries = module.read_git_tree(self.repo, self.git("rev-parse", "HEAD"))
        inventory = self.valid_inventory()
        inventory["project"]["download_location"] = "https://user@example.invalid/repo"

        with self.assertRaisesRegex(module.EvidenceError, "without user information"):
            module.validate_inventory(inventory, module.entry_map(entries))

    def test_inventory_requires_the_external_url_in_its_evidence(self) -> None:
        module = load_evidence_module()
        entries = module.read_git_tree(self.repo, self.git("rev-parse", "HEAD"))
        inventory = self.valid_inventory()
        inventory["components"][1]["artifacts"][0]["url"] = (
            "https://example.invalid/unbound-runtime.bin"
        )

        with self.assertRaisesRegex(module.EvidenceError, "URL is absent from its evidence"):
            module.validate_inventory(inventory, module.entry_map(entries))

    def test_inventory_rejects_nonstring_dependency_paths(self) -> None:
        module = load_evidence_module()
        entries = module.read_git_tree(self.repo, self.git("rev-parse", "HEAD"))
        inventory = self.valid_inventory()
        inventory["dependency_manifests"] = [{}]

        with self.assertRaisesRegex(module.EvidenceError, "non-empty list"):
            module.validate_inventory(inventory, module.entry_map(entries))

    def test_git_object_ids_have_an_exact_supported_width(self) -> None:
        module = load_evidence_module()
        tree = b"100644 blob " + (b"a" * 41) + b"\tVERSION\0"
        with mock.patch.object(module, "run_git", return_value=tree):
            with self.assertRaisesRegex(module.EvidenceError, "object ID is invalid"):
                module.read_git_tree_metadata(self.repo, "0" * 40)

    def test_inventory_rejects_overlapping_vendored_components(self) -> None:
        module = load_evidence_module()
        entries = module.read_git_tree(self.repo, self.git("rev-parse", "HEAD"))
        inventory = self.valid_inventory()
        duplicate = dict(inventory["components"][0])
        duplicate["spdx_id"] = "SPDXRef-Package-widget-copy"
        duplicate["name"] = "widget-copy"
        inventory["components"].append(duplicate)

        with self.assertRaisesRegex(module.EvidenceError, "multiple components"):
            module.validate_inventory(inventory, module.entry_map(entries))

    def test_build_rejects_unowned_third_party_source(self) -> None:
        self.write("third_party/unclaimed/code.cpp", "int unclaimed();\n")
        self.commit("add unclaimed third-party source")

        result = self.build(Path(self.tempdir.name) / "unowned-third-party")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no component or evidence owner", result.stderr)

    def test_source_manifest_rejects_portable_path_collisions(self) -> None:
        module = load_evidence_module()
        digest = "0" * 64
        manifest = (f"{digest}  100644  src/Cloud.cpp\n{digest}  100644  src/cloud.cpp\n").encode()

        with self.assertRaisesRegex(module.EvidenceError, "portable path collision"):
            module.parse_source_manifest(manifest)

    def test_source_paths_reject_all_windows_device_name_forms(self) -> None:
        module = load_evidence_module()

        for path in (
            "src/CONIN$.txt",
            "src/CONOUT$.txt",
            "src/CLOCK$.txt",
            "src/COM¹.log",
            "src/LPT³",
        ):
            with self.subTest(path=path):
                with self.assertRaisesRegex(module.EvidenceError, "reserved Windows name"):
                    module.safe_source_path(path)

    def test_source_manifest_rejects_symbolic_links(self) -> None:
        module = load_evidence_module()
        digest = "0" * 64
        manifest = f"{digest}  120000  docs/link\n".encode()

        with self.assertRaisesRegex(module.EvidenceError, "invalid digest or Git mode"):
            module.parse_source_manifest(manifest)

    def test_rejects_duplicate_inventory_names(self) -> None:
        inventory = json.dumps(self.valid_inventory(), indent=2, sort_keys=True)
        inventory = inventory.replace(
            '"schema_version": 1',
            '"schema_version": 1,\n  "schema_version": 1',
            1,
        )
        self.write("release/components.json", inventory + "\n")
        self.commit("add a duplicate inventory name")

        result = self.build(Path(self.tempdir.name) / "duplicate-inventory")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate object name: schema_version", result.stderr)

    def test_version_source_contains_only_one_version_line(self) -> None:
        self.write("VERSION", "1.2.3\nignored\n")
        self.commit("add invalid version source content")

        result = self.build(Path(self.tempdir.name) / "invalid-version")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly one SemVer line", result.stderr)

    def test_excluded_blob_contents_are_not_loaded(self) -> None:
        self.write("private/large.txt", "x" * 200_000)
        commit = self.commit("add an excluded large file")
        module = load_evidence_module()
        metadata = module.read_git_tree_metadata(self.repo, commit)
        policy_metadata = [entry for entry in metadata if entry.path == module.SOURCE_POLICY_PATH]
        policy = module.load_source_policy(module.read_git_blobs(self.repo, policy_metadata))
        selected, excluded = module.select_source_entries(metadata, policy)

        with mock.patch.object(module, "MAX_SOURCE_FILE_BYTES", 100_000):
            entries = module.read_git_blobs(self.repo, selected)

        self.assertNotIn("private/large.txt", {entry.path for entry in entries})
        self.assertIn("private/large.txt", {path for path, _reason in excluded})

    def test_build_accepts_a_linked_git_worktree(self) -> None:
        linked = Path(self.tempdir.name) / "linked-worktree"
        self.git("branch", "linked-worktree")
        self.git("worktree", "add", "--quiet", str(linked), "linked-worktree")
        self.assertTrue((linked / ".git").is_file())
        output = Path(self.tempdir.name) / "worktree-evidence"

        result = self.run_tool(
            "build",
            "--repo",
            linked,
            "--ref",
            "HEAD",
            "--output",
            output,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output.is_dir())

    def test_release_tag_contract_requires_matching_annotated_tag(self) -> None:
        env = dict(os.environ)
        env["GIT_COMMITTER_DATE"] = "2026-02-01T00:00:00Z"
        self.git("tag", "-a", "v1.2.3", "-m", "release 1.2.3", env=env)
        output = Path(self.tempdir.name) / "tagged-evidence"
        result = self.run_tool(
            "build",
            "--repo",
            self.repo,
            "--ref",
            "v1.2.3",
            "--release-tag",
            "v1.2.3",
            "--output",
            output,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((output / "melkor-1.2.3.tar.gz").is_file())

        mismatch = self.run_tool(
            "build",
            "--repo",
            self.repo,
            "--ref",
            "v1.2.3",
            "--release-tag",
            "v9.9.9",
            "--output",
            Path(self.tempdir.name) / "bad-tag",
        )
        self.assertNotEqual(mismatch.returncode, 0)
        self.assertIn("does not match source version", mismatch.stderr)

    def test_build_accepts_semver_prerelease_and_build_metadata(self) -> None:
        self.write("VERSION", "1.2.3-rc.1+build.7\n")
        self.commit("use complete SemVer syntax")
        output = Path(self.tempdir.name) / "semver-evidence"

        result = self.build(output)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(any("1.2.3-rc.1+build.7" in path.name for path in output.iterdir()))
        verified = self.run_tool("verify", output)
        self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_verify_rejects_missing_vendored_component_relationship(self) -> None:
        output = Path(self.tempdir.name) / "component-relationship-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        module = load_evidence_module()

        spdx_path = next(output.glob("*.spdx.json"))
        spdx = json.loads(spdx_path.read_text())
        removed = next(
            item
            for item in spdx["relationships"]
            if item["spdxElementId"] == "SPDXRef-Package-widget"
            and item["relationshipType"] == "CONTAINS"
            and item["relatedSpdxElement"].startswith("SPDXRef-File-")
        )
        spdx["relationships"].remove(removed)
        spdx_path.write_bytes(module.canonical_json_bytes(spdx))
        provenance_path = next(output.glob("*.provenance.json"))
        provenance = json.loads(provenance_path.read_text())
        for subject in provenance["subject"]:
            if subject["name"] == spdx_path.name:
                subject["digest"]["sha256"] = module.sha256_file(spdx_path)
        provenance_path.write_bytes(module.canonical_json_bytes(provenance))
        module.write_checksums(
            output,
            [path.name for path in output.iterdir() if path.name != "SHA256SUMS"],
        )

        verified = self.run_tool("verify", output)

        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("relationships do not match", verified.stderr)

    def test_verify_detects_tampering(self) -> None:
        output = Path(self.tempdir.name) / "tamper-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        spdx = next(output.glob("*.spdx.json"))
        spdx.write_bytes(spdx.read_bytes() + b"\n")
        verified = self.run_tool("verify", output)
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("checksum mismatch", verified.stderr)

    def test_verify_rejects_rehashed_noncanonical_metadata(self) -> None:
        output = Path(self.tempdir.name) / "noncanonical-metadata-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        module = load_evidence_module()

        provenance_path = next(output.glob("*.provenance.json"))
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
        provenance_path.write_text(json.dumps(provenance) + "\n", encoding="utf-8")
        module.write_checksums(
            output,
            [path.name for path in output.iterdir() if path.name != "SHA256SUMS"],
        )

        verified = self.run_tool("verify", output)

        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("provenance is not canonical JSON", verified.stderr)

    def test_verify_rejects_reordered_checksum_entries(self) -> None:
        output = Path(self.tempdir.name) / "reordered-checksum-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)

        checksums_path = output / "SHA256SUMS"
        lines = checksums_path.read_text(encoding="utf-8").splitlines()
        checksums_path.write_text("\n".join(reversed(lines)) + "\n", encoding="utf-8")

        verified = self.run_tool("verify", output)

        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("canonical sorted form", verified.stderr)

    def test_verify_rejects_rehashed_dependency_omission(self) -> None:
        output = Path(self.tempdir.name) / "missing-dependency-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        module = load_evidence_module()

        provenance_path = next(output.glob("*.provenance.json"))
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
        dependencies = provenance["predicate"]["buildDefinition"]["resolvedDependencies"]
        dependencies.pop()
        provenance_path.write_bytes(module.canonical_json_bytes(provenance))
        module.write_checksums(
            output,
            [path.name for path in output.iterdir() if path.name != "SHA256SUMS"],
        )

        verified = self.run_tool("verify", output)

        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("resolvedDependencies do not match", verified.stderr)

    def test_json_parser_rejects_nonstandard_numbers_and_deep_nesting(self) -> None:
        module = load_evidence_module()

        with self.assertRaisesRegex(module.EvidenceError, "non-JSON number"):
            module.strict_json_loads('{"value": NaN}', "fixture")
        with self.assertRaisesRegex(module.EvidenceError, "non-finite JSON number"):
            module.strict_json_loads('{"value": 1e999}', "fixture")
        nested = "[" * (module.MAX_JSON_DEPTH + 1) + "0" + "]" * (module.MAX_JSON_DEPTH + 1)
        with self.assertRaisesRegex(module.EvidenceError, "JSON nesting limit"):
            module.strict_json_loads(nested, "fixture")

    def test_verify_rejects_a_rehashed_incomplete_sbom(self) -> None:
        output = Path(self.tempdir.name) / "incomplete-sbom-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        module = load_evidence_module()

        spdx_path = next(output.glob("*.spdx.json"))
        spdx = json.loads(spdx_path.read_text())
        spdx["files"].pop()
        spdx_path.write_bytes(module.canonical_json_bytes(spdx))

        provenance_path = next(output.glob("*.provenance.json"))
        provenance = json.loads(provenance_path.read_text())
        for subject in provenance["subject"]:
            if subject["name"] == spdx_path.name:
                subject["digest"]["sha256"] = module.sha256_file(spdx_path)
        provenance_path.write_bytes(module.canonical_json_bytes(provenance))
        artifact_names = [path.name for path in output.iterdir() if path.name != "SHA256SUMS"]
        module.write_checksums(output, artifact_names)

        verified = self.run_tool("verify", output)
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("SPDX file inventory does not match", verified.stderr)

    def test_verify_rejects_a_rehashed_missing_project_relationship(self) -> None:
        output = Path(self.tempdir.name) / "incomplete-relationships-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        module = load_evidence_module()

        spdx_path = next(output.glob("*.spdx.json"))
        spdx = json.loads(spdx_path.read_text())
        project_id = next(
            item["relatedSpdxElement"]
            for item in spdx["relationships"]
            if item["spdxElementId"] == "SPDXRef-DOCUMENT"
            and item["relationshipType"] == "DESCRIBES"
        )
        removed = next(
            item
            for item in spdx["relationships"]
            if item["spdxElementId"] == project_id
            and item["relationshipType"] == "CONTAINS"
            and item["relatedSpdxElement"].startswith("SPDXRef-File-")
        )
        spdx["relationships"].remove(removed)
        spdx_path.write_bytes(module.canonical_json_bytes(spdx))

        provenance_path = next(output.glob("*.provenance.json"))
        provenance = json.loads(provenance_path.read_text())
        for subject in provenance["subject"]:
            if subject["name"] == spdx_path.name:
                subject["digest"]["sha256"] = module.sha256_file(spdx_path)
        provenance_path.write_bytes(module.canonical_json_bytes(provenance))
        artifact_names = [path.name for path in output.iterdir() if path.name != "SHA256SUMS"]
        module.write_checksums(output, artifact_names)

        verified = self.run_tool("verify", output)
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("does not contain source file", verified.stderr)

    def test_verify_rejects_rehashed_noncanonical_pax_data(self) -> None:
        output = Path(self.tempdir.name) / "noncanonical-archive-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        module = load_evidence_module()

        archive_path = next(output.glob("*.tar.gz"))
        members: list[tuple[tarfile.TarInfo, bytes | None]] = []
        with tarfile.open(archive_path, "r:gz") as source:
            for member in source.getmembers():
                extracted = source.extractfile(member) if member.isfile() else None
                members.append((member, extracted.read() if extracted is not None else None))

        expanded = io.BytesIO()
        with tarfile.open(fileobj=expanded, mode="w", format=tarfile.PAX_FORMAT) as archive:
            for index, (member, data) in enumerate(members):
                replacement = tarfile.TarInfo(member.name)
                replacement.mode = member.mode
                replacement.uid = member.uid
                replacement.gid = member.gid
                replacement.mtime = member.mtime
                replacement.uname = member.uname
                replacement.gname = member.gname
                replacement.type = member.type
                replacement.size = member.size
                replacement.linkname = member.linkname
                replacement.pax_headers = {"comment": "hidden"} if index == 0 else {}
                archive.addfile(replacement, io.BytesIO(data) if data is not None else None)

        compressed = io.BytesIO()
        with gzip.GzipFile(
            filename="", mode="wb", compresslevel=9, fileobj=compressed, mtime=0
        ) as handle:
            handle.write(expanded.getvalue())
        archive_path.write_bytes(compressed.getvalue())

        provenance_path = next(output.glob("*.provenance.json"))
        provenance = json.loads(provenance_path.read_text())
        for subject in provenance["subject"]:
            if subject["name"] == archive_path.name:
                subject["digest"]["sha256"] = module.sha256_file(archive_path)
        provenance_path.write_bytes(module.canonical_json_bytes(provenance))
        artifact_names = [path.name for path in output.iterdir() if path.name != "SHA256SUMS"]
        module.write_checksums(output, artifact_names)

        verified = self.run_tool("verify", output)
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("canonical tar stream", verified.stderr)

    def test_verify_rejects_a_rehashed_concatenated_gzip_stream(self) -> None:
        output = Path(self.tempdir.name) / "concatenated-gzip-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        module = load_evidence_module()

        archive_path = next(output.glob("*.tar.gz"))
        extra_member = io.BytesIO()
        with gzip.GzipFile(filename="", mode="wb", compresslevel=9, fileobj=extra_member, mtime=0):
            pass
        archive_path.write_bytes(archive_path.read_bytes() + extra_member.getvalue())

        provenance_path = next(output.glob("*.provenance.json"))
        provenance = json.loads(provenance_path.read_text())
        for subject in provenance["subject"]:
            if subject["name"] == archive_path.name:
                subject["digest"]["sha256"] = module.sha256_file(archive_path)
        provenance_path.write_bytes(module.canonical_json_bytes(provenance))
        artifact_names = [path.name for path in output.iterdir() if path.name != "SHA256SUMS"]
        module.write_checksums(output, artifact_names)

        verified = self.run_tool("verify", output)
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("canonical gzip stream", verified.stderr)

    def test_verify_rejects_unlisted_nested_content(self) -> None:
        output = Path(self.tempdir.name) / "nested-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        unexpected = output / "unlisted"
        unexpected.mkdir()
        (unexpected / "payload.txt").write_text("not checksummed\n", encoding="utf-8")
        verified = self.run_tool("verify", output)
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("nested or non-regular", verified.stderr)

    def test_verify_rejects_symlink_entries(self) -> None:
        output = Path(self.tempdir.name) / "symlink-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        target = next(output.glob("*.spdx.json"))
        try:
            (output / "unlisted-link").symlink_to(target.name)
        except OSError as exc:
            self.skipTest(f"symlinks unavailable: {exc}")
        verified = self.run_tool("verify", output)
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("nested or non-regular", verified.stderr)


if __name__ == "__main__":
    unittest.main()
