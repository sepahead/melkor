#!/usr/bin/env python3
"""Focused tests for the deterministic release-evidence contract."""

from __future__ import annotations

import hashlib
import json
import importlib.util
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "scripts" / "build_release_evidence.py"


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
                        (
                            path
                            for path in sorted(candidate.rglob("*"))
                            if path.is_file()
                        ),
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
        validated, version = module.validate_inventory(
            inventory, module.entry_map(entries)
        )
        self.assertIs(validated, inventory)

        # Compare against the authoritative VERSION file, not a literal. Hard-coding the
        # version here would make this test one more surface that has to be remembered on
        # every bump — the exact failure mode the single version source exists to remove.
        expected = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        self.assertEqual(version, expected)


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
        self.write("deps/widget/LICENSE", "Synthetic widget license\n")
        self.write("deps/widget/widget.cpp", "int widget() { return 7; }\n")
        self.write("locks/runtime.lock", "runtime==4.5.6\n")
        self.write("docs/external-license.txt", "Synthetic external license\n")
        self.write(
            "scripts/fetch-runtime.sh",
            "#!/bin/sh\n# sha256:"
            + "a" * 64
            + "\nexit 0\n",
            executable=True,
        )
        self.write(
            "scripts/build_release_evidence.py",
            TOOL.read_text(encoding="utf-8"),
            executable=True,
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
            "dependency_manifests": ["locks/runtime.lock"],
            "extracted_licenses": [
                {
                    "license_id": "LicenseRef-Synthetic-Widget",
                    "name": "Synthetic Widget License",
                    "text_file": "deps/widget/LICENSE",
                }
            ],
            "components": [
                {
                    "spdx_id": "SPDXRef-Package-widget",
                    "name": "widget",
                    "version": "7.0",
                    "distribution": "vendored",
                    "paths": ["deps/widget"],
                    "license_declared": "LicenseRef-Synthetic-Widget",
                    "license_files": ["deps/widget/LICENSE"],
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
                        "scripts/fetch-runtime.sh",
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
            "scripts/fetch-runtime.sh",
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
            provenance["predicate"]["buildDefinition"]["internalParameters"][
                "generatorSha256"
            ],
            generator_digest,
        )

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

    def test_rejects_unresolved_lfs_pointer(self) -> None:
        self.write(
            "deps/widget/widget.cpp",
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
        components[0]["license_files"] = ["deps/widget/MISSING"]
        self.write_inventory(inventory)
        self.commit("break component license evidence")
        result = self.build(Path(self.tempdir.name) / "invalid-inventory")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("component license", result.stderr)

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

    def test_verify_detects_tampering(self) -> None:
        output = Path(self.tempdir.name) / "tamper-evidence"
        built = self.build(output)
        self.assertEqual(built.returncode, 0, built.stderr)
        spdx = next(output.glob("*.spdx.json"))
        spdx.write_bytes(spdx.read_bytes() + b"\n")
        verified = self.run_tool("verify", output)
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("checksum mismatch", verified.stderr)

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
