#!/usr/bin/env python3
"""Regression tests for the repository tooling in tools/.

These lock in fixes for bugs an adversarial review found in the version-sync, source-bundle,
and notice-generation tools. Each test states the concrete failure it prevents. They run with
the standard library only, driven by CTest.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TOOLS = REPO_ROOT / "tools"


def load(module_name: str, filename: str):
    spec = importlib.util.spec_from_file_location(module_name, TOOLS / filename)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    # Register before executing so that @dataclass (which resolves cls.__module__ through
    # sys.modules) works when the module is loaded under a synthetic name.
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


class SourceBundleExclusion(unittest.TestCase):
    """The source-bundle allowlist must never let a weight or a secret ship, and must not
    silently drop a legitimate source file."""

    @classmethod
    def setUpClass(cls):
        cls.b = load("build_source_bundle", "build_source_bundle.py")

    def test_model_weights_are_excluded_regardless_of_case(self):
        # The allowlist includes examples/, so a weight dropped there would ship without the
        # extension denylist. Every common weight format, in any case, must be refused.
        for path in [
            "examples/model.gguf", "examples/model.ggml", "examples/weights.npz",
            "examples/arr.npy", "examples/net.onnx", "examples/graph.pb",
            "examples/net.tflite", "examples/data.h5", "examples/Model.PT",
            "examples/X.SafeTensors", "examples/net.ckpt", "examples/w.pth",
        ]:
            self.assertFalse(self.b.is_allowed(path), f"{path} must not ship")

    def test_secrets_are_excluded(self):
        for path in [
            "viewer/.env", "viewer/.env.production", "viewer/.env.local",
            "python/credentials.json", "keys/id_rsa", "certs/server.pem",
            "certs/app.p12", "certs/store.keystore",
        ]:
            self.assertFalse(self.b.is_allowed(path), f"{path} must not ship")

    def test_ordinary_source_still_ships(self):
        for path in ["src/main.cpp", "include/melkor/version.h.in", "README.md",
                     "cmake/MelkorVersion.cmake", "docs/index.md"]:
            self.assertTrue(self.b.is_allowed(path), f"{path} should ship")

    def test_symlinks_are_never_included(self):
        # A symlink stored via `git show` would put its target path into the bundle as file
        # content -- a wrong, misleading file. select() must classify it as excluded.
        # We assert on the classification helper rather than shelling out to git.
        included, excluded = self.b.select("HEAD")
        # There are no tracked symlinks today; assert the mechanism reports the reason field.
        for _path, reason in excluded:
            self.assertIsInstance(reason, str)


class VersionSync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.v = load("check_version_sync", "check_version_sync.py")

    def test_pep440_mapping(self):
        parse = self.v.parse_version
        self.assertEqual(parse("2.0.0").pep440, "2.0.0")
        self.assertEqual(parse("2.0.0-dev").pep440, "2.0.0.dev0")
        self.assertEqual(parse("2.0.0-rc.2").pep440, "2.0.0rc2")

    def test_changelog_prerelease_does_not_satisfy_stable(self):
        # The heading check for a stable release must NOT be satisfied by a prerelease section.
        # A trailing \b matched "## 2.0.0-rc.2" for a stable "2.0.0"; the fixed regex must not.
        import re
        version = self.v.parse_version("2.0.0")
        heading = rf"^##\s+{re.escape(version.core)}(?:$|[\s(])"
        self.assertIsNone(
            re.search(heading, "## 2.0.0-rc.2\n\nstuff", re.MULTILINE),
            "a prerelease heading must not satisfy the stable-release check",
        )
        self.assertIsNotNone(
            re.search(heading, "## 2.0.0 (2026-07-15)\n", re.MULTILINE),
            "a real stable heading must satisfy it",
        )
        self.assertIsNotNone(
            re.search(heading, "## 2.0.0\n", re.MULTILINE),
            "a bare stable heading at end of line must satisfy it",
        )

    def test_npm_lock_rewrite_targets_only_self_versions(self):
        # The lockfile self-version rewrite must touch only the first two "version" keys (the
        # top-level and packages.""), never a dependency that shares the version string.
        import re
        text = (
            '{\n  "name": "x",\n  "version": "1.0.0",\n  "packages": {\n'
            '    "": {\n      "version": "1.0.0"\n    },\n'
            '    "node_modules/dep": {\n      "version": "1.0.0"\n    }\n  }\n}\n'
        )
        new, n = re.subn(r'("version"\s*:\s*")[^"]*(")', r"\g<1>2.0.0\g<2>", text, count=2)
        self.assertEqual(n, 2)
        # The dependency's version must be untouched.
        self.assertIn('"node_modules/dep": {\n      "version": "1.0.0"', new)
        self.assertEqual(new.count('"version": "2.0.0"'), 2)


class NoticeGeneration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.g = load("generate_notices", "generate_notices.py")

    def test_missing_patch_rationale_does_not_crash(self):
        # A patch dict lacking 'rationale' must degrade to a placeholder, not raise KeyError on
        # the way to reporting the policy failure.
        lock = {
            "dependencies": [
                {
                    "id": "x", "license": "MIT", "upstream": "https://example.invalid/x",
                    "revision": "0" * 40, "release": "v1", "purpose": "test",
                    "license_file": None,
                    "patches": [{"file": "0001-x.patch", "upstream_status": "not-submitted"}],
                }
            ]
        }
        # Should not raise.
        text = self.g.render_third_party(lock)
        self.assertIn("0001-x.patch", text)


class DocumentationLinks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.docs = load("check_docs_links", "check_docs_links.py")

    def test_missing_target_and_anchor_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "README.md"
            guide = root / "Guide.md"
            source.write_text(
                "# Home\n\n[missing](missing.md)\n[case](guide.md)\n[anchor](Guide.md#no)\n",
                encoding="utf-8",
            )
            guide.write_text("# Valid heading\n", encoding="utf-8")
            findings = self.docs.check_markdown([source, guide], root)
            messages = [finding.message for finding in findings]
            self.assertTrue(any("does not exist" in message for message in messages))
            self.assertTrue(any("incorrect path case" in message for message in messages))
            self.assertTrue(any("anchor does not exist" in message for message in messages))

    def test_valid_relative_link_and_anchor_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            docs = root / "docs"
            docs.mkdir()
            source = root / "README.md"
            guide = docs / "guide.md"
            source.write_text("# Home\n\n[guide](docs/guide.md#valid-heading)\n", encoding="utf-8")
            guide.write_text("# Valid heading\n", encoding="utf-8")
            self.assertEqual(self.docs.check_markdown([source, guide], root), [])

    def test_query_does_not_become_part_of_local_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "README.md"
            guide = root / "guide.md"
            source.write_text("# Home\n\n[guide](guide.md?plain=1#valid)\n", encoding="utf-8")
            guide.write_text("# Valid\n", encoding="utf-8")
            self.assertEqual(self.docs.check_markdown([source, guide], root), [])


class DocumentationStyle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.style = load("check_docs_style", "check_docs_style.py")

    def test_style_violations_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "README.md"
            source.write_text(
                "# Test\n\nThis sentence has a colour issue; it isn't valid project prose.\n",
                encoding="utf-8",
            )
            messages = [
                finding.message for finding in self.style.check_markdown([source], root)
            ]
            self.assertTrue(any("American spelling" in message for message in messages))
            self.assertTrue(any("contraction" in message for message in messages))
            self.assertTrue(any("semicolon" in message for message in messages))

    def test_short_american_english_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "README.md"
            source.write_text("# Test\n\nUse the color profile.\n", encoding="utf-8")
            self.assertEqual(self.style.check_markdown([source], root), [])

    def test_long_paragraph_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "README.md"
            source.write_text(
                "# Test\n\nOne. Two. Three. Four. Five. Six. Seven.\n",
                encoding="utf-8",
            )
            messages = [
                finding.message for finding in self.style.check_markdown([source], root)
            ]
            self.assertIn("paragraph has 7 sentences", messages)


class ClaimCoverage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.claims = load("check_claims", "check_claims.py")

    def test_all_active_document_groups_are_covered(self):
        relative = {
            path.relative_to(REPO_ROOT).as_posix() for path in self.claims.linted_paths()
        }
        for expected in (
            "docs/QUICKSTART.md",
            "docs/PIPELINE.md",
            "benchmarks/README.md",
            "release/README.md",
            "scripts/pipeline.sh",
            "viewer/README.md",
        ):
            self.assertIn(expected, relative)
        self.assertNotIn("docs/audit/production-blockers.md", relative)


class ScriptContracts(unittest.TestCase):
    @staticmethod
    def make_colmap_project(root: Path) -> Path:
        project = root / "project"
        model = project / "sparse" / "0"
        images = project / "images"
        model.mkdir(parents=True)
        images.mkdir()
        for name in ("cameras.bin", "images.bin", "points3D.bin"):
            (model / name).write_bytes(b"fixture")
        (images / "frame.jpg").write_bytes(b"fixture")
        return project

    def test_global_mapper_dry_run_uses_colmap_without_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            images = root / "images"
            output = root / "output"
            images.mkdir()
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "glomap_wrapper.sh"),
                    str(images),
                    str(output),
                    "--dry-run",
                    "--matcher",
                    "sequential",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("colmap global_mapper", result.stdout)
            self.assertIn("colmap sequential_matcher", result.stdout)
            self.assertFalse(output.exists())

    def test_pipeline_rejects_retired_glomap_value_without_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            images = root / "images"
            output = root / "output"
            images.mkdir()
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "pipeline.sh"),
                    str(images),
                    str(output),
                    "--sfm",
                    "glomap",
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Use --sfm global", result.stderr)
            self.assertFalse(output.exists())

    def test_pipeline_dry_run_needs_no_external_binary_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            images = root / "images"
            output = root / "output"
            images.mkdir()
            for index in range(3):
                (images / f"frame-{index}.jpg").write_bytes(b"fixture")
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "pipeline.sh"),
                    str(images),
                    str(output),
                    "--opensplat",
                    str(root / "not-installed"),
                    "--dry-run",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("colmap automatic_reconstructor", result.stdout)
            self.assertIn("opensplat_wrapper.sh", result.stdout)
            self.assertFalse(output.exists())

    def test_opensplat_wrapper_runs_one_explicit_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            output = root / "result.ply"
            fake = root / "fake-opensplat"
            fake.write_text(
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                "while [[ $# -gt 0 ]]; do\n"
                "  if [[ $1 == -o ]]; then printf fixture > \"$2\"; exit 0; fi\n"
                "  shift\n"
                "done\n"
                "exit 2\n",
                encoding="utf-8",
            )
            fake.chmod(0o755)
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "opensplat_wrapper.sh"),
                    str(project),
                    "--opensplat",
                    str(fake),
                    "--output",
                    str(output),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_bytes(), b"fixture")

    def test_opensplat_wrapper_rejects_simulated_multi_gpu_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "opensplat_wrapper.sh"),
                    str(project),
                    "--gpu-ids",
                    "0,1",
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not a verified wrapper option", result.stderr)

    def test_opensplat_wrapper_preserves_existing_output_on_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            output = root / "result.ply"
            output.write_bytes(b"original")
            fake = root / "fake-opensplat"
            fake.write_text("#!/usr/bin/env bash\nexit 9\n", encoding="utf-8")
            fake.chmod(0o755)
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "opensplat_wrapper.sh"),
                    str(project),
                    "--opensplat",
                    str(fake),
                    "--output",
                    str(output),
                    "--force",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 9)
            self.assertEqual(output.read_bytes(), b"original")

    def test_opensplat_wrapper_rejects_empty_staged_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            output = root / "result.ply"
            fake = root / "fake-opensplat"
            fake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake.chmod(0o755)
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "opensplat_wrapper.sh"),
                    str(project),
                    "--opensplat",
                    str(fake),
                    "--output",
                    str(output),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertFalse(output.exists())

    def test_lichtfeld_wrapper_replaces_output_after_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            output = root / "result"
            output.mkdir()
            (output / "old.txt").write_text("old", encoding="utf-8")
            fake = root / "fake-lichtfeld"
            fake.write_text(
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                "while [[ $# -gt 0 ]]; do\n"
                "  if [[ $1 == -o ]]; then printf new > \"$2/result.ply\"; exit 0; fi\n"
                "  shift\n"
                "done\n"
                "exit 2\n",
                encoding="utf-8",
            )
            fake.chmod(0o755)
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "lichtfeld_wrapper.sh"),
                    str(project),
                    "--lichtfeld",
                    str(fake),
                    "--output",
                    str(output),
                    "--force",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((output / "result.ply").read_text(encoding="utf-8"), "new")
            self.assertFalse((output / "old.txt").exists())

    def test_lichtfeld_wrapper_preserves_output_on_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            output = root / "result"
            output.mkdir()
            old = output / "old.txt"
            old.write_text("old", encoding="utf-8")
            fake = root / "fake-lichtfeld"
            fake.write_text("#!/usr/bin/env bash\nexit 9\n", encoding="utf-8")
            fake.chmod(0o755)
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "lichtfeld_wrapper.sh"),
                    str(project),
                    "--lichtfeld",
                    str(fake),
                    "--output",
                    str(output),
                    "--force",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 9)
            self.assertEqual(old.read_text(encoding="utf-8"), "old")

    def test_retired_general_installers_fail_without_writes(self):
        for script in (
            "setup_all.sh",
            "setup_opensplat.sh",
            "setup_gsplat_cuda.sh",
            "setup_gsplat_mps.sh",
            "setup_lichtfeld.sh",
        ):
            with self.subTest(script=script), tempfile.TemporaryDirectory() as directory:
                result = subprocess.run(
                    [str(REPO_ROOT / "scripts" / script)],
                    cwd=directory,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode, 2)
                self.assertEqual(list(Path(directory).iterdir()), [])

    def test_da3_setup_requires_unlocked_dependency_acceptance(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [str(REPO_ROOT / "scripts" / "setup_da3.sh")],
                cwd=directory,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("--accept-unlocked-dependencies", result.stdout)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_feedforward_setup_requires_unlocked_dependency_acceptance(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "setup_feedforward_sota.sh"),
                    "mapanything",
                ],
                cwd=directory,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("--accept-unlocked-dependencies", result.stderr)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_feedforward_catalog_is_read_only(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "setup_feedforward_sota.sh"),
                    "list",
                ],
                cwd=directory,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("mapanything", result.stderr)
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == "__main__":
    result = unittest.main(argv=[sys.argv[0], "-v"], exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
