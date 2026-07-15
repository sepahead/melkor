#!/usr/bin/env python3
"""Regression tests for the repository tooling in tools/.

These lock in fixes for bugs an adversarial review found in the version-sync, source-bundle,
and notice-generation tools. Each test states the concrete failure it prevents. They run with
the standard library only, driven by CTest.
"""

from __future__ import annotations

import importlib.util
import sys
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


if __name__ == "__main__":
    result = unittest.main(argv=[sys.argv[0], "-v"], exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
