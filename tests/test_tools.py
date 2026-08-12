#!/usr/bin/env python3
"""Regression tests for the repository tooling in tools/."""

from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parent.parent
TOOLS = REPO_ROOT / "tools"


class DiagnosticContracts(unittest.TestCase):
    def test_source_diagnostic_codes_are_valid_and_numbers_are_unique(self):
        literal_pattern = re.compile(r'"(MK[^:"\\ ]*)')
        code_pattern = re.compile(r"MK[0-9]{4}_[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)*")
        names_by_number: dict[str, set[str]] = {}

        for root in (REPO_ROOT / "include", REPO_ROOT / "src"):
            for path in sorted(candidate for candidate in root.rglob("*") if candidate.is_file()):
                text = path.read_text(encoding="utf-8")
                for code in literal_pattern.findall(text):
                    self.assertRegex(code, rf"^{code_pattern.pattern}$", path.as_posix())
                    names_by_number.setdefault(code[:6], set()).add(code)

        collisions = {
            number: sorted(names) for number, names in names_by_number.items() if len(names) > 1
        }
        self.assertEqual(collisions, {})


class AtomicPublication(unittest.TestCase):
    tool = TOOLS / "atomic_publish.py"

    def run_tool(self, *arguments: Path | str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(self.tool), *(str(argument) for argument in arguments)],
            capture_output=True,
            text=True,
        )

    def test_directory_publication_is_no_replace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "staged"
            destination = root / "output"
            source.mkdir()
            destination.mkdir()
            (source / "new.txt").write_text("new", encoding="utf-8")
            (destination / "old.txt").write_text("old", encoding="utf-8")

            result = self.run_tool(source, destination)

            self.assertEqual(result.returncode, 3, result.stderr)
            self.assertEqual((source / "new.txt").read_text(encoding="utf-8"), "new")
            self.assertEqual((destination / "old.txt").read_text(encoding="utf-8"), "old")

    def test_file_replacement_is_atomic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "staged.ply"
            destination = root / "output.ply"
            source.write_bytes(b"new")
            destination.write_bytes(b"old")

            result = self.run_tool(source, destination, "--replace")

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(source.exists())
            self.assertEqual(destination.read_bytes(), b"new")

    @unittest.skipUnless(sys.platform == "win32", "Windows uses same-directory handle rename")
    def test_windows_rejects_a_different_destination_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_parent = root / "source"
            destination_parent = root / "destination"
            source_parent.mkdir()
            destination_parent.mkdir()
            source = source_parent / "staged.ply"
            destination = destination_parent / "output.ply"
            source.write_bytes(b"new")

            result = self.run_tool(source, destination)

            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("must use one directory", result.stderr)
            self.assertEqual(source.read_bytes(), b"new")
            self.assertFalse(destination.exists())

    @unittest.skipIf(sys.platform == "win32", "Windows symbolic-link creation needs privileges")
    def test_symbolic_link_paths_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            real_parent = root / "real"
            real_parent.mkdir()
            source = real_parent / "staged"
            source.write_bytes(b"new")
            linked_parent = root / "linked"
            linked_parent.symlink_to(real_parent, target_is_directory=True)

            result = self.run_tool(linked_parent / source.name, root / "output")

            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertTrue(source.exists())
            self.assertFalse((root / "output").exists())

    def test_two_publishers_cannot_merge_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "output"
            sources = [root / "staged-a", root / "staged-b"]
            for index, source in enumerate(sources):
                source.mkdir()
                (source / "value.txt").write_text(str(index), encoding="utf-8")

            processes = [
                subprocess.Popen(
                    [sys.executable, str(self.tool), str(source), str(destination)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                for source in sources
            ]
            results = [process.communicate() + (process.returncode,) for process in processes]

            self.assertEqual(sum(returncode == 0 for _, _, returncode in results), 1, results)
            self.assertIn((destination / "value.txt").read_text(encoding="utf-8"), {"0", "1"})
            self.assertFalse((destination / "staged-a").exists())
            self.assertFalse((destination / "staged-b").exists())


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

    @staticmethod
    def init_repository(repo: Path, extra_paths: tuple[str, ...] = ()) -> str:
        subprocess.run(["git", "init", "--quiet", repo], check=True)
        subprocess.run(
            ["git", "-C", repo, "config", "user.name", "Source Bundle Test"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", repo, "config", "user.email", "test@example.invalid"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", repo, "config", "commit.gpgsign", "false"],
            check=True,
        )
        (repo / "README.md").write_text("# Test\n", encoding="utf-8")
        for relative in extra_paths:
            path = repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("excluded\n", encoding="utf-8")
        subprocess.run(["git", "-C", repo, "add", "--all"], check=True)
        subprocess.run(
            ["git", "-C", repo, "commit", "--quiet", "-m", "fixture"],
            check=True,
        )
        return subprocess.run(
            ["git", "-C", repo, "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    def test_model_weights_are_excluded_regardless_of_case(self):
        # The allowlist includes viewer/, so a weight dropped there would ship without the
        # extension denylist. Every common weight format, in any case, must be refused.
        for path in [
            "viewer/fixtures/model.gguf",
            "viewer/fixtures/model.ggml",
            "viewer/fixtures/weights.npz",
            "viewer/fixtures/arr.npy",
            "viewer/fixtures/net.onnx",
            "viewer/fixtures/graph.pb",
            "viewer/fixtures/net.tflite",
            "viewer/fixtures/data.h5",
            "viewer/fixtures/Model.PT",
            "viewer/fixtures/X.SafeTensors",
            "viewer/fixtures/net.ckpt",
            "viewer/fixtures/w.pth",
        ]:
            self.assertFalse(self.b.is_allowed(path), f"{path} must not ship")

    def test_secrets_are_excluded(self):
        for path in [
            "viewer/.env",
            "viewer/.env.production",
            "viewer/.env.local",
            "python/credentials.json",
            "keys/id_rsa",
            "certs/server.pem",
            "certs/app.p12",
            "certs/store.keystore",
        ]:
            self.assertFalse(self.b.is_allowed(path), f"{path} must not ship")

    def test_unsafe_paths_are_excluded(self):
        for path in [
            "src/line\nbreak.cpp",
            "src/control\x1f.cpp",
            "src\\windows.cpp",
            "/src/absolute.cpp",
            "src/./ambiguous.cpp",
            "src/../escape.cpp",
            ".GIT/config",
            "src/CON.txt",
            "src/CONIN$.txt",
            "src/CONOUT$.txt",
            "src/CLOCK$.txt",
            "src/COM¹.log",
            "src/LPT³",
            "src/trailing.",
            "src/not:native.cpp",
            "src/e\u0301.cpp",
        ]:
            self.assertFalse(self.b.is_allowed(path), f"{path!r} must not ship")

    def test_portable_source_path_collisions_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "portable source path collision"):
            self.b.select_entries(
                [
                    ("100644", "src/Cloud.cpp"),
                    ("100644", "src/cloud.cpp"),
                ]
            )

    def test_ordinary_source_still_ships(self):
        for path in [
            "src/main.cpp",
            "include/melkor/version.h.in",
            "README.md",
            "AGENTS.md",
            "cmake/MelkorVersion.cmake",
            "benchmarks/datasets/manifest.json",
            "docs/index.md",
            ".github/CODEOWNERS",
        ]:
            self.assertTrue(self.b.is_allowed(path), f"{path} should ship")

    def test_each_allowlist_entry_selects_a_regular_repository_file(self):
        top_level = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
        )
        if (
            top_level.returncode != 0
            or Path(top_level.stdout.strip()).resolve() != REPO_ROOT.resolve()
        ):
            self.skipTest("This policy check requires the repository Git index.")

        result = subprocess.run(
            ["git", "ls-files", "--stage", "-z"],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
        )
        entries: list[tuple[str, str]] = []
        for record in result.stdout.split(b"\0"):
            if not record:
                continue
            metadata, raw_path = record.split(b"\t", 1)
            mode = metadata.split(b" ", 1)[0].decode("ascii")
            path = raw_path.decode("utf-8")
            entries.append((mode, path))

        unused: list[str] = []
        for allowed in self.b.ALLOWLIST:
            matches = [
                path
                for mode, path in entries
                if mode in {"100644", "100755"}
                and self.b.is_allowed(path)
                and (path.startswith(allowed) if allowed.endswith("/") else path == allowed)
            ]
            if not matches:
                unused.append(allowed)

        self.assertEqual(unused, [], "remove source allowlist entries that select no files")

    def test_generated_build_directories_are_excluded(self):
        for path in [
            "src/build-debug/object.o",
            "viewer/build-release/index.html",
            "build-local/output.txt",
        ]:
            self.assertFalse(self.b.is_allowed(path), f"{path} must not ship")

        self.assertTrue(self.b.is_allowed("docs/build-guide.md"))

    def test_documented_and_release_scripts_ship(self):
        required = [
            "release/README.md",
            "benchmarks/datasets/manifest.json",
            "scripts/build_release_evidence.py",
            "scripts/pipeline.sh",
            "scripts/setup_deps.sh",
            "scripts/test_sdk_install.sh",
            "scripts/train_from_images.sh",
        ]
        for path in required:
            self.assertTrue(self.b.is_allowed(path), f"{path} should ship")

        self.assertFalse(self.b.is_allowed("scripts/unreviewed-entry-point.sh"))

    def test_symlinks_are_never_included(self):
        # A symlink stored via `git show` would put its target path into the bundle as file
        # content -- a wrong, misleading file. select() must classify it as excluded.
        # We assert on the classification helper rather than shelling out to git.
        included, excluded = self.b.select_entries(
            [
                ("100644", "src/main.cpp"),
                ("120000", "docs/link.md"),
            ]
        )
        self.assertEqual(included, ["src/main.cpp"])
        self.assertEqual(
            excluded,
            [("docs/link.md", "symlink (target would be stored as file content)")],
        )

    def test_option_like_ref_resolves_as_ref_data(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            head = self.init_repository(repo)
            subprocess.run(
                ["git", "-C", repo, "update-ref", "refs/tags/--help", head],
                check=True,
            )

            self.assertEqual(self.b.resolve_commit("--help", repo), head)
            self.assertIn(("100644", "README.md"), self.b.git_tracked_entries("--help", repo))

    @unittest.skipIf(os.name == "nt", "Windows forbids control bytes in file names")
    def test_diagnostics_escape_control_characters(self):
        relative = "src/line\nbreak\x1b[31m.txt"
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            self.init_repository(repo, (relative,))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(self.b.main(["--check"], repo), 0)

            report = output.getvalue()
            self.assertIn(self.b.escape_diagnostic(relative), report)
            self.assertNotIn(relative, report)
            self.assertNotIn("\x1b", report)

            bad_ref = "--missing\n\x1b[31m"
            with self.assertRaises(SystemExit) as raised:
                self.b.resolve_commit(bad_ref, repo)
            message = str(raised.exception)
            self.assertIn(self.b.escape_diagnostic(bad_ref), message)
            self.assertNotIn(bad_ref, message)
            self.assertNotIn("\x1b", message)

    def test_archive_preserves_git_executable_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "repo"
            repo.mkdir()
            self.init_repository(repo, ("tools/plain.py", "src/runner"))
            subprocess.run(
                ["git", "-C", repo, "update-index", "--chmod=+x", "src/runner"],
                check=True,
            )
            subprocess.run(
                ["git", "-C", repo, "update-index", "--chmod=-x", "tools/plain.py"],
                check=True,
            )
            subprocess.run(
                ["git", "-C", repo, "commit", "--quiet", "-m", "set modes"],
                check=True,
            )
            head = subprocess.run(
                ["git", "-C", repo, "rev-parse", "HEAD"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()
            output = Path(directory) / "source.tar"

            self.b.build(head, output, 0, repo)

            with tarfile.open(output, "r:") as archive:
                self.assertEqual(archive.getmember("tools/plain.py").mode, 0o644)
                self.assertEqual(archive.getmember("src/runner").mode, 0o755)

    def test_gzip_archive_has_a_platform_neutral_header(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "repo"
            repo.mkdir()
            head = self.init_repository(repo)
            output = Path(directory) / "source.tar.gz"

            self.b.build(head, output, 0, repo)

            self.assertEqual(
                output.read_bytes()[:10],
                b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff",
            )
            with tarfile.open(output, "r:gz") as archive:
                self.assertEqual(archive.getnames(), ["README.md"])

    def test_source_size_limit_fails_before_writing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "repo"
            repo.mkdir()
            head = self.init_repository(repo)
            output = Path(directory) / "source.tar"

            with mock.patch.object(self.b, "MAX_SOURCE_TOTAL_BYTES", 1):
                with self.assertRaisesRegex(SystemExit, "selected source exceeds"):
                    self.b.build(head, output, 0, repo)

            self.assertFalse(output.exists())

    def test_source_bundle_does_not_replace_an_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "repo"
            repo.mkdir()
            head = self.init_repository(repo)
            output = Path(directory) / "source.tar"
            output.write_bytes(b"keep me")

            with self.assertRaisesRegex(SystemExit, "output path already exists"):
                self.b.build(head, output, 0, repo)

            self.assertEqual(output.read_bytes(), b"keep me")

    def test_source_bundle_uses_the_atomic_publisher(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "repo"
            repo.mkdir()
            head = self.init_repository(repo)
            output = Path(directory) / "source.tar"

            with mock.patch.object(
                self.b.importlib.util,
                "spec_from_file_location",
                wraps=self.b.importlib.util.spec_from_file_location,
            ) as loader:
                self.b.build(head, output, 0, repo)

            loader.assert_called_once()
            self.assertEqual(loader.call_args.args[1].name, "atomic_publish.py")
            self.assertTrue(output.is_file())

    def test_epoch_outside_the_gzip_range_fails_before_writing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "repo"
            repo.mkdir()
            head = self.init_repository(repo)
            output = Path(directory) / "source.tar.gz"

            with self.assertRaisesRegex(SystemExit, "SOURCE_DATE_EPOCH"):
                self.b.build(head, output, self.b.MAX_SOURCE_DATE_EPOCH + 1, repo)

            self.assertFalse(output.exists())

    def test_invalid_source_date_epoch_fails_without_replacing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "repo"
            repo.mkdir()
            self.init_repository(repo)
            output = Path(directory) / "source.tar"
            output.write_bytes(b"keep me")

            with mock.patch.dict("os.environ", {"SOURCE_DATE_EPOCH": "invalid"}, clear=False):
                with self.assertRaises(SystemExit):
                    self.b.main(["--output", str(output)], repo)

            self.assertEqual(output.read_bytes(), b"keep me")

    def test_explicit_zero_source_date_epoch_does_not_report_it_as_unset(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "repo"
            repo.mkdir()
            self.init_repository(repo)
            output = Path(directory) / "source.tar"
            error = io.StringIO()

            with mock.patch.dict("os.environ", {"SOURCE_DATE_EPOCH": "0"}, clear=False):
                with contextlib.redirect_stderr(error):
                    self.assertEqual(self.b.main(["--output", str(output)], repo), 0)

            self.assertNotIn("is unset", error.getvalue())


class CiResultPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ci = load("check_ci_results", "check_ci_results.py")

    def results(self, value: str = "success"):
        return {job: {"result": value} for job in self.ci.REQUIRED_JOBS}

    def test_pull_request_requires_every_job_to_succeed(self):
        self.assertEqual(self.ci.check_results(self.results(), "pull_request"), [])

    def test_required_jobs_include_the_windows_build(self):
        self.assertIn("build-windows", self.ci.REQUIRED_JOBS)

    def test_push_requires_only_dependency_review_to_be_skipped(self):
        results = self.results()
        results["dependency-review"]["result"] = "skipped"
        self.assertEqual(self.ci.check_results(results, "push"), [])

    def test_pull_request_rejects_skipped_dependency_review(self):
        results = self.results()
        results["dependency-review"]["result"] = "skipped"
        self.assertEqual(
            self.ci.check_results(results, "pull_request"),
            ["dependency-review=skipped"],
        )

    def test_push_rejects_a_skipped_build(self):
        results = self.results()
        results["dependency-review"]["result"] = "skipped"
        results["build-linux"]["result"] = "skipped"
        self.assertEqual(
            self.ci.check_results(results, "push"),
            ["build-linux=skipped"],
        )

    def test_failure_and_cancellation_fail_the_gate(self):
        results = self.results()
        results["build-linux"]["result"] = "failure"
        results["build-macos"]["result"] = "cancelled"
        self.assertEqual(
            self.ci.check_results(results, "pull_request"),
            ["build-macos=cancelled", "build-linux=failure"],
        )

    def test_missing_and_unexpected_jobs_fail_the_gate(self):
        results = self.results()
        del results["fuzz-smoke"]
        results["unreviewed-job"] = {"result": "success"}
        self.assertEqual(
            self.ci.check_results(results, "pull_request"),
            ["fuzz-smoke=missing", "unreviewed-job=unexpected"],
        )

    def test_job_results_require_strict_json(self):
        for raw in (b'{"job":NaN}', b'{"job":1e999}', b'{"job":1,"job":2}'):
            with self.subTest(raw=raw):
                with self.assertRaises(ValueError):
                    self.ci.parse_results(raw)


class DiagramClaims(unittest.TestCase):
    def test_all_svg_assets_have_accessible_names(self):
        namespace = "{http://www.w3.org/2000/svg}"
        svgs = sorted((REPO_ROOT / "assets").rglob("*.svg"))
        self.assertTrue(svgs)

        for path in svgs:
            with self.subTest(path=path.relative_to(REPO_ROOT)):
                root = ET.parse(path).getroot()
                self.assertEqual(root.tag, namespace + "svg")
                self.assertEqual(root.get("role"), "img")
                labels = root.get("aria-labelledby", "").split()
                self.assertEqual(len(labels), 2)
                identifiers = [element.get("id") for element in root.iter() if element.get("id")]
                self.assertEqual(len(identifiers), len(set(identifiers)))
                elements = {
                    element.get("id"): element for element in root.iter() if element.get("id")
                }
                self.assertEqual(elements[labels[0]].tag, namespace + "title")
                self.assertEqual(elements[labels[1]].tag, namespace + "desc")
                for label in labels:
                    self.assertTrue("".join(elements[label].itertext()).strip())

    def test_loss_policy_diagram_matches_sources(self):
        import re

        header = (REPO_ROOT / "include" / "melkor" / "format" / "loss.hpp").read_text(
            encoding="utf-8"
        )
        errors = (REPO_ROOT / "src" / "core" / "error.cpp").read_text(encoding="utf-8")
        diagram = (REPO_ROOT / "assets" / "diagrams" / "loss-policy.svg").read_text(
            encoding="utf-8"
        )
        codes = re.findall(r'=\s*"(LOSS_[A-Z_]+)";', header)
        self.assertIn(f"{len(codes)} STABLE LOSS CODES", diagram)
        for code in codes:
            self.assertIn(f">{code.removeprefix('LOSS_')}<", diagram)

        unsupported = re.search(r"case ErrorCode::unsupported_feature:\s*return\s+(\d+);", errors)
        self.assertIsNotNone(unsupported)
        self.assertIn(f"stop &#183; exit {unsupported.group(1)}", diagram)

    def test_verification_counts_match_cmake(self):
        import re

        cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        diagram = (REPO_ROOT / "assets" / "diagrams" / "verification.svg").read_text(
            encoding="utf-8"
        )
        explicit_tests = re.findall(r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_]+)", cmake)
        match = re.search(r"set\(MELKOR_FUZZ_TARGETS\s+([^\)]+)\)", cmake, re.DOTALL)
        self.assertIsNotNone(match)
        fuzz_targets = match.group(1).split()
        total = len(explicit_tests) + len(fuzz_targets)

        self.assertIn(f"{total} REGISTERED TESTS", diagram)
        self.assertIn(f"{len(fuzz_targets)} FUZZ TARGETS", diagram)
        for target in fuzz_targets:
            self.assertIn(f">{target}<", diagram)

        bars = [int(value) for value in re.findall(r'class="val"[^>]*>(\d+)<', diagram)]
        self.assertEqual(sum(bars), total)

    def test_verification_exit_codes_match_source(self):
        import re

        source = (REPO_ROOT / "src" / "core" / "error.cpp").read_text(encoding="utf-8")
        inspect_contract = (REPO_ROOT / "docs" / "INSPECT.md").read_text(encoding="utf-8")
        diagram = (REPO_ROOT / "assets" / "diagrams" / "verification.svg").read_text(
            encoding="utf-8"
        )
        function = source.split("int exit_code_for", maxsplit=1)[1].split(
            "const char* to_string(Severity", maxsplit=1
        )[0]
        source_codes = {
            int(code)
            for code in re.findall(
                r"case ErrorCode::[a-z_]+:.*?return\s+(\d+);", function, re.DOTALL
            )
        }
        contract = diagram.split("EXIT-CODE CONTRACT", maxsplit=1)[1].split(
            '<text class="sub"', maxsplit=1
        )[0]
        diagram_codes = {
            int(code) for code in re.findall(r"<text[^>]*>(\d+)\s+[^<]+</text>", contract)
        }
        documented_codes = {
            int(code) for code in re.findall(r"^\| `(\d+)` \|", inspect_contract, re.MULTILINE)
        }
        self.assertTrue(source_codes.issubset(documented_codes))
        self.assertEqual(diagram_codes, documented_codes)

    def test_diagram_version_and_profiles_match_sources(self):
        version = (REPO_ROOT / "VERSION").read_text(encoding="utf-8").strip()
        architecture = (REPO_ROOT / "assets" / "diagrams" / "architecture.svg").read_text(
            encoding="utf-8"
        )
        matrix = (REPO_ROOT / "assets" / "diagrams" / "format-matrix.svg").read_text(
            encoding="utf-8"
        )

        self.assertIn(f"version {version}", architecture)
        for path in sorted((REPO_ROOT / "profiles").glob("*/*.json")):
            profile = json.loads(path.read_text(encoding="utf-8"))
            self.assertIn(f">{profile['profile_id']}<", matrix)
            self.assertIn(f">{profile['status']}<", matrix)


class LossReportSchema(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import jsonschema

        schema = json.loads(
            (REPO_ROOT / "schemas" / "loss-report-v1.schema.json").read_text(encoding="utf-8")
        )
        jsonschema.Draft202012Validator.check_schema(schema)
        cls.validator = jsonschema.Draft202012Validator(schema)

    def test_complete_zero_loss_report_is_valid(self):
        self.validator.validate(
            {
                "schema_version": 1,
                "input": {"format": "glb", "profile": "KHR_gaussian_splatting"},
                "output": {"format": "glb", "profile": "KHR_gaussian_splatting"},
                "items": [],
                "approved_codes": [],
            }
        )

    def test_incomplete_report_is_rejected(self):
        import jsonschema

        with self.assertRaises(jsonschema.ValidationError):
            self.validator.validate({"schema_version": 1, "items": []})

    def test_codes_and_minimum_severities_match_the_runtime_registry(self):
        import jsonschema
        import re

        header = (REPO_ROOT / "include" / "melkor" / "format" / "loss.hpp").read_text(
            encoding="utf-8"
        )
        source = (REPO_ROOT / "src" / "formats" / "loss.cpp").read_text(encoding="utf-8")
        definitions = dict(
            re.findall(
                r'inline constexpr const char\* (k[A-Za-z0-9]+) = "(LOSS_[A-Z0-9_]+)";',
                header,
            )
        )
        runtime = {
            definitions[name]: severity
            for name, severity in re.findall(
                r"\{loss_code::(k[A-Za-z0-9]+), LossSeverity::(info|warning|severe|fatal)\}",
                source,
            )
        }
        schema_codes = set(self.validator.schema["$defs"]["lossCode"]["enum"])
        self.assertEqual(schema_codes, set(runtime))

        inspect_schema = json.loads(
            (REPO_ROOT / "schemas" / "inspect-v1.schema.json").read_text(encoding="utf-8")
        )
        self.assertEqual(set(inspect_schema["$defs"]["lossCode"]["enum"]), set(runtime))
        inspect_validator = jsonschema.Draft202012Validator(inspect_schema)

        order = ["info", "warning", "severe", "fatal"]
        for code, minimum in runtime.items():
            report = {
                "schema_version": 1,
                "input": {"format": "glb", "profile": "input"},
                "output": {"format": "glb", "profile": "output"},
                "items": [
                    {
                        "code": code,
                        "severity": minimum,
                        "source_feature": "source",
                        "target_constraint": "target",
                        "affected_splats": 0,
                        "remediation": "none",
                    }
                ],
                "approved_codes": [],
            }
            self.validator.validate(report)
            minimum_index = order.index(minimum)
            if minimum_index != 0:
                report["items"][0]["severity"] = order[minimum_index - 1]
                with self.assertRaises(jsonschema.ValidationError):
                    self.validator.validate(report)

                inspect_report = InspectReportSchema.report_document()
                inspect_report["losses"] = report["items"]
                with self.assertRaises(jsonschema.ValidationError):
                    inspect_validator.validate(inspect_report)


class InspectReportSchema(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import jsonschema

        schema = json.loads(
            (REPO_ROOT / "schemas" / "inspect-v1.schema.json").read_text(encoding="utf-8")
        )
        jsonschema.Draft202012Validator.check_schema(schema)
        cls.validator = jsonschema.Draft202012Validator(schema)

    @staticmethod
    def report_document():
        return {
            "schema": "melkor.inspect.v1",
            "valid": True,
            "source": {"path": "input.ply", "format": "ply", "profile": "p", "bytes": 1},
            "cloud": {
                "splats": 1,
                "sh_degree": 0,
                "color_space": "lin_rec709_display",
                "coordinate_frame": "gltf-luf",
                "unit_to_meter": 1.0,
                "bounds": None,
                "fields": {
                    "position": "explicit",
                    "color": "explicit_sh",
                    "opacity": "explicit",
                    "scale": "explicit",
                    "rotation": "explicit",
                    "sh_rest": "absent",
                },
            },
            "container": {"encoding": "ascii", "declared_splats": 1, "antialiased": None},
            "losses": [],
            "validation": {"error_code": None, "errors": 0, "warnings": 0, "issues": []},
        }

    def report(self):
        return self.report_document()

    def test_valid_report_requires_cloud_without_errors(self):
        import jsonschema

        report = self.report()
        self.validator.validate(report)
        report["cloud"] = None
        with self.assertRaises(jsonschema.ValidationError):
            self.validator.validate(report)

    def test_invalid_report_requires_an_error_class_and_error(self):
        import jsonschema

        report = self.report()
        report["valid"] = False
        with self.assertRaises(jsonschema.ValidationError):
            self.validator.validate(report)
        report["validation"] = {
            "error_code": "invalid_data",
            "errors": 1,
            "warnings": 0,
            "issues": [
                {
                    "severity": "error",
                    "code": "MK1505_NEGATIVE_SCALE",
                    "message": "The scale is negative.",
                    "count": 1,
                    "first_index": 0,
                    "path": "input.ply",
                    "byte_offset": None,
                    "context": {},
                }
            ],
        }
        self.validator.validate(report)


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
        text = (
            '{\n  "name": "x",\n  "version": "1.0.0",\n  "packages": {\n'
            '    "": {\n      "name": "x",\n      "version": "1.0.0"\n    },\n'
            '    "node_modules/dep": {\n      "version": "1.0.0"\n    }\n  }\n}\n'
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "viewer" / "package-lock.json"
            path.parent.mkdir()
            path.write_text(text, encoding="utf-8")
            with mock.patch.object(self.v, "REPO_ROOT", root):
                finding, writer = self.v._npm_lock_surface("2.0.0")
                self.assertFalse(finding.ok)
                self.assertIsNotNone(writer)
                writer()

            updated = path.read_text(encoding="utf-8")
            self.assertEqual(updated.count('"version": "2.0.0"'), 2)
            self.assertIn('"node_modules/dep": {\n      "version": "1.0.0"', updated)

    def test_json_rewrite_targets_the_top_level_field(self):
        text = '{\n  "metadata": {"version": "nested"},\n  "version": "1.0.0"\n}\n'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "surface.json"
            path.write_text(text, encoding="utf-8")
            updated = self.v.replace_top_level_json_string(path, "version", "2.0.0")

        self.assertIn('"metadata": {"version": "nested"}', updated)
        self.assertIn('  "version": "2.0.0"', updated)

    def test_npm_lock_rejects_nonobject_packages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "viewer" / "package-lock.json"
            path.parent.mkdir()
            path.write_text('{"version":"1.0.0","packages":[]}', encoding="utf-8")
            with mock.patch.object(self.v, "REPO_ROOT", root):
                with self.assertRaisesRegex(self.v.VersionError, "must be an object"):
                    self.v._npm_lock_surface("2.0.0")

    def test_version_surfaces_reject_symbolic_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target.json"
            target.write_text('{"version":"1.0.0"}', encoding="utf-8")
            link = root / "surface.json"
            try:
                link.symlink_to(target.name)
            except OSError as exc:
                self.skipTest(f"symbolic links unavailable: {exc}")
            with self.assertRaisesRegex(self.v.VersionError, "symbolic links"):
                self.v.read_json_object(link)

    def test_version_surfaces_reject_non_json_numbers(self):
        for text in ('{"version":NaN}', '{"version":1e999}'):
            with (
                self.subTest(text=text),
                self.assertRaisesRegex(self.v.VersionError, "JSON number"),
            ):
                self.v.parse_json_object(text, Path("surface.json"))

    def test_version_file_requires_canonical_line_form(self):
        for content in ("2.0.0-dev", " 2.0.0-dev\n", "2.0.0-dev\r\n"):
            with self.subTest(content=content), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "VERSION"
                path.write_text(content, encoding="utf-8", newline="")
                with mock.patch.object(self.v, "VERSION_FILE", path):
                    with self.assertRaises(self.v.VersionError):
                        self.v.read_authoritative_version()

    def test_unmapped_version_reports_an_error_without_a_traceback(self):
        version = self.v.parse_version("2.0.0-preview")
        with (
            mock.patch.object(self.v, "read_authoritative_version", return_value=version),
            mock.patch.object(sys, "argv", ["check_version_sync.py", "--check"]),
            mock.patch("sys.stderr", new_callable=io.StringIO) as stderr,
        ):
            self.assertEqual(self.v.main(), 2)
        self.assertIn("no defined PEP 440 mapping", stderr.getvalue())
        self.assertNotIn("Traceback", stderr.getvalue())

    def test_shell_wrappers_derive_the_project_version(self):
        self.assertEqual(self.v.check_no_hardcoded_version_in_scripts(), [])


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
                    "id": "x",
                    "license": "MIT",
                    "upstream": "https://example.invalid/x",
                    "revision": "0" * 40,
                    "release": "v1",
                    "purpose": "test",
                    "license_file": None,
                    "patches": [{"file": "0001-x.patch", "upstream_status": "not-submitted"}],
                }
            ]
        }
        # Should not raise.
        text = self.g.render_third_party(lock)
        self.assertIn("0001-x.patch", text)

    def test_notice_inputs_reject_non_json_numbers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock.json"
            for text in ('{"value":Infinity}', '{"value":1e999}'):
                with self.subTest(text=text):
                    path.write_text(text, encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, "JSON number"):
                        self.g.strict_json(path)

    def test_notice_inputs_reject_symbolic_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target.json"
            target.write_text("{}\n", encoding="utf-8")
            link = root / "lock.json"
            try:
                link.symlink_to(target.name)
            except OSError as exc:
                self.skipTest(f"symlinks unavailable: {exc}")
            with self.assertRaisesRegex(ValueError, "symbolic link"):
                self.g.strict_json(link)

    def test_notice_reports_optional_build_conditions(self):
        lock = {
            "dependencies": [
                {
                    "id": "optional-codec",
                    "license": "MIT",
                    "upstream": "https://example.invalid/codec",
                    "revision": "0" * 40,
                    "release": "v1",
                    "purpose": "test",
                    "license_file": None,
                    "patches": [],
                    "build_condition": "MELKOR_CODEC_ENABLED",
                }
            ]
        }
        text = self.g.render_notice(lock)
        self.assertIn("Build condition: MELKOR_CODEC_ENABLED", text)


class ThirdPartyVerification(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.v = load("verify_third_party", "verify_third_party.py")

    def test_git_checkout_never_falls_back_to_untracked_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vendored = root / "third_party" / "widget"
            vendored.mkdir(parents=True)
            (vendored / "tracked.cpp").write_text("tracked\n", encoding="utf-8")
            subprocess.run(["git", "init", "--quiet", root], check=True)
            subprocess.run(["git", "-C", root, "add", "--all"], check=True)
            (vendored / "untracked.cpp").write_text("untracked\n", encoding="utf-8")

            with mock.patch.object(self.v, "REPO_ROOT", root):
                files = self.v.tracked_files(vendored)

            self.assertEqual(files, ["tracked.cpp"])

    def test_content_digest_rejects_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target.txt"
            target.write_text("target\n", encoding="utf-8")
            link = root / "link.txt"
            try:
                link.symlink_to(target.name)
            except OSError as exc:
                self.skipTest(f"symlinks unavailable: {exc}")

            with self.assertRaisesRegex(ValueError, "must not be a symlink"):
                self.v.content_digest(root, ["link.txt"])

    def test_manifest_paths_cannot_escape_the_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(self.v, "REPO_ROOT", root):
                with self.assertRaisesRegex(ValueError, "unsafe repository path"):
                    self.v.safe_repo_path("../outside", prefix="third_party")

    def test_manifest_paths_reject_symbolic_link_components(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            real = root / "real"
            real.mkdir()
            third_party = root / "third_party"
            try:
                third_party.symlink_to(real, target_is_directory=True)
            except OSError as exc:
                self.skipTest(f"symlinks unavailable: {exc}")
            with mock.patch.object(self.v, "REPO_ROOT", root):
                with self.assertRaisesRegex(ValueError, "symbolic-link component"):
                    self.v.safe_repo_path("third_party/source", prefix="third_party")

    def test_manifest_paths_reject_nonportable_names(self):
        for path in (
            "third_party/CON.txt",
            "third_party/source./file.cpp",
            "third_party/a:file.cpp",
            "third_party/e\u0301/file.cpp",
        ):
            with self.subTest(path=path):
                with self.assertRaises(ValueError):
                    self.v.safe_repo_path(path, prefix="third_party")

    def test_content_digest_rejects_portable_path_collisions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Source.cpp").write_text("one\n", encoding="utf-8")
            (root / "source.cpp").write_text("two\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "portable path collision"):
                self.v.content_digest(root, ["Source.cpp", "source.cpp"])

    def test_specification_license_notice_must_be_declared(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = root / "third_party" / "specs" / "example" / "abc"
            base.mkdir(parents=True)
            readme = base / "README.md"
            readme.write_text("specification\n", encoding="utf-8")
            subprocess.run(["git", "init", "--quiet", root], check=True)
            subprocess.run(["git", "-C", root, "add", "--all"], check=True)
            lock_path = root / "third_party" / "specifications.lock.json"
            lock_path.write_text("{}\n", encoding="utf-8")
            lock = {
                "schema_version": 1,
                "specifications": [
                    {
                        "id": "example",
                        "upstream": "https://example.invalid/spec",
                        "path_in_upstream": "spec",
                        "commit": "0" * 40,
                        "vendored_path": "third_party/specs/example/abc",
                        "files": [
                            {
                                "path": "README.md",
                                "sha256": self.v.file_digest(readme),
                            }
                        ],
                        "license_notice_file": (
                            "third_party/specs/example/abc/MISSING-LICENSE.txt"
                        ),
                    }
                ],
            }

            with (
                mock.patch.object(self.v, "REPO_ROOT", root),
                mock.patch.object(self.v, "SPEC_LOCK_PATH", lock_path),
                mock.patch.object(self.v, "strict_json", return_value=lock),
            ):
                errors = self.v.check_specifications()

            self.assertTrue(any("must identify a declared" in error for error in errors))

    def test_dependency_locks_reject_non_json_numbers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock.json"
            for text in ('{"value":-Infinity}', '{"value":1e999}'):
                with self.subTest(text=text):
                    path.write_text(text, encoding="utf-8")
                    with self.assertRaisesRegex(SystemExit, "JSON number"):
                        self.v.strict_json(path)

    def test_dependency_locks_reject_symbolic_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target.json"
            target.write_text("{}\n", encoding="utf-8")
            link = root / "lock.json"
            try:
                link.symlink_to(target.name)
            except OSError as exc:
                self.skipTest(f"symlinks unavailable: {exc}")
            with self.assertRaisesRegex(SystemExit, "symbolic link"):
                self.v.strict_json(link)

    def test_dependency_locks_reject_unknown_fields_and_url_state(self):
        errors = []
        self.v.reject_unknown_keys({"known": 1, "typo": 2}, {"known"}, "record", errors)
        self.assertEqual(errors, ["record: unknown field: typo"])
        for url in (
            "https://example.invalid/source?token=secret",
            "https://example.invalid/source#fragment",
            "https://user@example.invalid/source",
        ):
            with self.subTest(url=url), self.assertRaises(ValueError):
                self.v.validate_https_url(url, "source")

    def test_declared_patch_reconstructs_the_upstream_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            (source / "value.txt").write_text("patched\n", encoding="utf-8")
            patch = root / "0001-change.patch"
            patch.write_text(
                "--- a/value.txt\n+++ b/value.txt\n@@ -1 +1 @@\n-upstream\n+patched\n",
                encoding="utf-8",
            )
            upstream = root / "upstream"
            upstream.mkdir()
            (upstream / "value.txt").write_text("upstream\n", encoding="utf-8")
            expected = self.v.content_digest(upstream, ["value.txt"])

            actual, count = self.v.reconstruct_upstream_digest(source, ["value.txt"], [patch])

            self.assertEqual(actual, expected)
            self.assertEqual(count, 1)

    def test_declared_patch_must_reverse_cleanly(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            (source / "value.txt").write_text("different\n", encoding="utf-8")
            patch = root / "0001-change.patch"
            patch.write_text(
                "--- a/value.txt\n+++ b/value.txt\n@@ -1 +1 @@\n-upstream\n+patched\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "does not reverse cleanly"):
                self.v.reconstruct_upstream_digest(source, ["value.txt"], [patch])


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

    def test_vendored_markdown_is_not_project_documentation(self):
        self.assertFalse(self.docs.is_project_markdown("third_party/widget/README.md"))
        self.assertFalse(self.docs.is_project_markdown("viewer/vendor/README.md"))
        self.assertTrue(self.docs.is_project_markdown("docs/README.md"))

    def test_query_does_not_become_part_of_local_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "README.md"
            guide = root / "guide.md"
            source.write_text("# Home\n\n[guide](guide.md?plain=1#valid)\n", encoding="utf-8")
            guide.write_text("# Valid\n", encoding="utf-8")
            self.assertEqual(self.docs.check_markdown([source, guide], root), [])

    def test_markdown_link_input_rejects_symbolic_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target.md"
            target.write_text("# Target\n", encoding="utf-8")
            link = root / "link.md"
            try:
                link.symlink_to(target.name)
            except OSError as exc:
                self.skipTest(f"symlinks unavailable: {exc}")
            with self.assertRaisesRegex(ValueError, "symbolic link"):
                self.docs.read_markdown(link)


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
            messages = [finding.message for finding in self.style.check_markdown([source], root)]
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
            messages = [finding.message for finding in self.style.check_markdown([source], root)]
            self.assertIn("paragraph has 7 sentences", messages)

    def test_markdown_style_input_rejects_symbolic_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target.md"
            target.write_text("# Target\n", encoding="utf-8")
            link = root / "link.md"
            try:
                link.symlink_to(target.name)
            except OSError as exc:
                self.skipTest(f"symlinks unavailable: {exc}")
            with self.assertRaisesRegex(ValueError, "symbolic link"):
                self.style.read_markdown(link)


class ClaimCoverage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.claims = load("check_claims", "check_claims.py")

    def test_all_active_document_groups_are_covered(self):
        relative = {path.relative_to(REPO_ROOT).as_posix() for path in self.claims.linted_paths()}
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

    def test_claim_input_rejects_symbolic_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target.md"
            target.write_text("Plain text.\n", encoding="utf-8")
            link = root / "link.md"
            try:
                link.symlink_to(target.name)
            except OSError as error:
                self.skipTest(f"symlinks unavailable: {error}")
            with self.assertRaisesRegex(ValueError, "symbolic link"):
                self.claims.scan(link)

    def test_claim_input_rejects_oversized_files(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "large.md"
            source.write_text("0123456789", encoding="utf-8")
            original_limit = self.claims.MAX_PUBLIC_TEXT_BYTES
            self.claims.MAX_PUBLIC_TEXT_BYTES = 4
            try:
                with self.assertRaisesRegex(ValueError, "bounded regular file"):
                    self.claims.scan(source)
            finally:
                self.claims.MAX_PUBLIC_TEXT_BYTES = original_limit


class ProfileValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profiles = load("check_profiles", "check_profiles.py")
        cls.jsonschema = cls.profiles.load_jsonschema()
        if cls.jsonschema is None:
            raise RuntimeError("jsonschema is required for profile validation tests")
        schema = json.loads(
            (REPO_ROOT / "schemas" / "format-profile.schema.json").read_text(encoding="utf-8")
        )
        cls.validator = cls.profiles.make_validator(cls.jsonschema, schema)
        cls.valid_profile = {
            "schema_version": 1,
            "profile_id": "test:profile-v1",
            "container": "ply",
            "read_containers": ["ply"],
            "write_containers": ["ply"],
            "status": "experimental",
            "description": "A test profile.",
            "coordinate_system": "gltf-luf",
            "length_unit": "meter",
            "quaternion_order": "xyzw",
            "quaternion_normalized": True,
            "scale_domain": "linear",
            "scale_constraint": "non-negative",
            "opacity_domain": "linear",
            "opacity_range": [0.0, 1.0],
            "color_space": "lin_rec709_display",
            "sh": {
                "basis": "real",
                "max_degree": 0,
                "order": "degree-major-m-ascending",
                "phase": "condon-shortley",
                "coefficients_per_degree": [1],
            },
            "required_properties": ["POSITION"],
        }

    def errors_for(self, changes):
        profile = copy.deepcopy(self.valid_profile)
        profile.update(changes)
        return self.profiles.profile_errors(self.validator, profile)

    def test_invalid_type_is_rejected(self):
        errors = self.errors_for({"description": 7})
        self.assertTrue(any(error.validator == "type" for error in errors))

    def test_invalid_enum_is_rejected(self):
        errors = self.errors_for({"container": "obj"})
        self.assertTrue(any(error.validator == "enum" for error in errors))

    def test_unknown_structural_field_is_rejected(self):
        errors = self.errors_for({"unexpected_field": True})
        self.assertTrue(any(error.validator == "additionalProperties" for error in errors))

    def test_unknown_sh_field_is_rejected(self):
        errors = self.errors_for({"sh": {"basis": "real", "unexpected_field": True}})
        self.assertTrue(any(error.validator == "additionalProperties" for error in errors))

    def test_format_property_extensions_are_permitted(self):
        errors = self.errors_for({"properties": {"vendor_extension": {"opaque_value": True}}})
        self.assertEqual(errors, [])

    def test_missing_semantic_contract_is_rejected(self):
        profile = copy.deepcopy(self.valid_profile)
        del profile["coordinate_system"]
        errors = self.profiles.profile_errors(self.validator, profile)
        self.assertTrue(any(error.validator == "required" for error in errors))

    def test_color_contract_is_unambiguous(self):
        errors = self.errors_for({"color_spaces": ["lin_rec709_display"]})
        self.assertTrue(any(error.validator == "oneOf" for error in errors))

    def test_duplicate_profile_ids_are_rejected(self):
        duplicates = self.profiles.duplicate_profile_ids(
            [
                (Path("a.json"), {"profile_id": "test:profile-v1"}),
                (Path("b.json"), {"profile_id": "test:profile-v1"}),
            ]
        )
        self.assertEqual(
            duplicates,
            [("test:profile-v1", Path("a.json"), Path("b.json"))],
        )

    def test_runtime_profile_table_parser_is_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "no readable entries"):
            self.profiles.runtime_profiles("unrelated source")

    def test_runtime_profile_table_parser_reads_all_capabilities(self):
        source = """
        constexpr std::array<FormatProfile, 1> kProfiles{{
            {FormatProfileId::sample, "sample:v1",
             container_bit(FormatId::gltf) | container_bit(FormatId::glb),
             container_bit(FormatId::glb), 3},
        }};
        """
        self.assertEqual(
            self.profiles.runtime_profiles(source),
            {
                "sample:v1": {
                    "max_sh_degree": 3,
                    "read_containers": ["glb", "gltf"],
                    "write_containers": ["glb"],
                }
            },
        )

    def test_write_containers_must_be_readable(self):
        profile = copy.deepcopy(self.valid_profile)
        profile["write_containers"] = ["glb"]
        self.assertIn(
            "$.write_containers must be a subset of $.read_containers: glb",
            self.profiles.semantic_errors(profile),
        )

    def test_duplicate_json_names_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            path.write_text('{"profile_id":"a","profile_id":"b"}\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate object name"):
                self.profiles.strict_json(path)

    def test_non_json_numbers_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            path.write_text('{"value":NaN}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "non-JSON number"):
                self.profiles.strict_json(path)

    def test_overflowed_json_numbers_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            path.write_text('{"value":1e999}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "non-finite JSON number"):
                self.profiles.strict_json(path)

    def test_header_markers_must_match_profile_domains(self):
        profile = copy.deepcopy(self.valid_profile)
        profile["profile_id"] = "ply:test-profile-v1"
        profile["header_markers"] = {
            "melkor_profile": "test-profile-v1",
            "melkor_quaternion_order": "wxyz",
            "melkor_scale_domain": "linear",
            "melkor_opacity_domain": "linear",
            "melkor_sh_degree": "0-0",
        }
        self.assertIn(
            "$.header_markers.melkor_quaternion_order must equal $.quaternion_order",
            self.profiles.semantic_errors(profile),
        )

    def test_sh_coefficient_counts_must_match_the_degree(self):
        profile = copy.deepcopy(self.valid_profile)
        profile["sh"]["max_degree"] = 2
        profile["sh"]["coefficients_per_degree"] = [1, 3, 7]
        self.assertEqual(
            self.profiles.semantic_errors(profile),
            ["$.sh.coefficients_per_degree must equal [1, 3, 5] for degree 2"],
        )

    def test_all_repository_profiles_validate(self):
        result = subprocess.run(
            [sys.executable, str(TOOLS / "check_profiles.py")],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_jsonschema_fails_closed(self):
        result = subprocess.run(
            [sys.executable, "-S", str(TOOLS / "check_profiles.py")],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("jsonschema is required", result.stderr)


@unittest.skipIf(os.name == "nt", "Bash contract tests require a POSIX host")
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

    def test_viewer_fixture_conversion_matches_spark_ply_contract(self):
        script = (REPO_ROOT / "viewer" / "fetch-assets.sh").read_text(encoding="utf-8")
        conversion = script[script.index('"$MELKOR_BIN" convert') :]
        conversion = conversion[: conversion.index(">/dev/null")]
        self.assertIn("--output-profile ply:graphdeco-3dgs-v1", conversion)
        self.assertIn("--target-frame spz-rub", conversion)

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

    def test_pipeline_image_count_handles_newline_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            images = root / "images"
            output = root / "output"
            images.mkdir()
            (images / "one\ntwo\nthree.jpg").write_bytes(b"fixture")
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "pipeline.sh"),
                    str(images),
                    str(output),
                    "--dry-run",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("At least three supported images", result.stderr)
            self.assertFalse(output.exists())

    def test_pipeline_rejects_output_inside_the_input_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            images = root / "images"
            images.mkdir()
            for index in range(3):
                (images / f"frame-{index}.jpg").write_bytes(b"fixture")
            output = images / "result"
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "pipeline.sh"),
                    str(images),
                    str(output),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("must not overlap", result.stderr)
            self.assertFalse(output.exists())

    def test_pipeline_spz_requires_verified_semantics_before_writes(self):
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
                    "--format",
                    "spz",
                    "--dry-run",
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--source-frame", result.stderr)
            self.assertFalse(output.exists())

    def test_pipeline_spz_dry_run_uses_explicit_convert_contract(self):
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
                    "--format",
                    "spz",
                    "--source-frame",
                    "ply-rdf",
                    "--source-unit-to-meter",
                    "1",
                    "--source-color-space",
                    "lin_rec709_display",
                    "--output-antialiased",
                    "false",
                    "--allow-loss",
                    "LOSS_COLOR_SPACE_METADATA_DROPPED",
                    "--allow-loss",
                    "LOSS_COORDINATE_METADATA_DROPPED",
                    "--dry-run",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(" convert ", result.stdout)
            self.assertIn("ply:graphdeco-3dgs-v1", result.stdout)
            self.assertIn("LOSS_COORDINATE_METADATA_DROPPED", result.stdout)
            self.assertFalse(output.exists())

    def test_pipeline_publishes_only_after_all_stages_succeed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            output = root / "output"
            fake = root / "fake-opensplat"
            fake.write_text(
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                "while [[ $# -gt 0 ]]; do\n"
                '  if [[ $1 == -o ]]; then printf fixture > "$2"; exit 0; fi\n'
                "  shift\n"
                "done\n"
                "exit 2\n",
                encoding="utf-8",
            )
            fake.chmod(0o755)
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "pipeline.sh"),
                    str(project),
                    str(output),
                    "--skip-colmap",
                    "--opensplat",
                    str(fake),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((output / "point_cloud.ply").read_bytes(), b"fixture")
            self.assertEqual(list(root.glob(".melkor-pipeline.*")), [])

    def test_pipeline_removes_partial_output_after_stage_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            output = root / "output"
            fake = root / "fake-opensplat"
            fake.write_text("#!/usr/bin/env bash\nexit 9\n", encoding="utf-8")
            fake.chmod(0o755)
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "pipeline.sh"),
                    str(project),
                    str(output),
                    "--skip-colmap",
                    "--opensplat",
                    str(fake),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 9)
            self.assertFalse(output.exists())
            self.assertEqual(list(root.glob(".melkor-pipeline.*")), [])

    def test_pipeline_spz_mode_publishes_only_spz(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            output = root / "output"
            opensplat = root / "fake-opensplat"
            opensplat.write_text(
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                "while [[ $# -gt 0 ]]; do\n"
                '  if [[ $1 == -o ]]; then printf ply > "$2"; exit 0; fi\n'
                "  shift\n"
                "done\n"
                "exit 2\n",
                encoding="utf-8",
            )
            opensplat.chmod(0o755)
            melkor = root / "fake-melkor"
            melkor.write_text(
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                "[[ $1 == convert ]]\n"
                'printf spz > "$3"\n'
                "printf '{}\\n'\n",
                encoding="utf-8",
            )
            melkor.chmod(0o755)
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "pipeline.sh"),
                    str(project),
                    str(output),
                    "--skip-colmap",
                    "--format",
                    "spz",
                    "--opensplat",
                    str(opensplat),
                    "--melkor",
                    str(melkor),
                    "--source-frame",
                    "ply-rdf",
                    "--source-unit-to-meter",
                    "1",
                    "--source-color-space",
                    "lin_rec709_display",
                    "--output-antialiased",
                    "false",
                    "--allow-loss",
                    "LOSS_COLOR_SPACE_METADATA_DROPPED",
                    "--allow-loss",
                    "LOSS_COORDINATE_METADATA_DROPPED",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((output / "point_cloud.spz").read_bytes(), b"spz")
            self.assertFalse((output / "point_cloud.ply").exists())
            self.assertEqual((output / "point_cloud.loss-report.json").read_text(), "{}\n")

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
                '  if [[ $1 == -o ]]; then printf fixture > "$2"; exit 0; fi\n'
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

    def test_tool_wrappers_protect_the_staged_output_argument(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            fake = root / "fake-tool"
            fake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake.chmod(0o755)
            for script, option in (
                ("opensplat_wrapper.sh", "--opensplat"),
                ("lichtfeld_wrapper.sh", "--lichtfeld"),
            ):
                with self.subTest(script=script):
                    result = subprocess.run(
                        [
                            str(REPO_ROOT / "scripts" / script),
                            str(project),
                            option,
                            str(fake),
                            "--",
                            "-o",
                            str(root / "escaped-output"),
                        ],
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("staged output path", result.stderr)
                    self.assertFalse((root / "escaped-output").exists())

    def test_tool_wrapper_dry_runs_withhold_upstream_secrets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            fake = root / "fake-tool"
            fake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake.chmod(0o755)
            secret = "melkor-wrapper-secret"
            for script, option in (
                ("opensplat_wrapper.sh", "--opensplat"),
                ("lichtfeld_wrapper.sh", "--lichtfeld"),
            ):
                with self.subTest(script=script):
                    result = subprocess.run(
                        [
                            str(REPO_ROOT / "scripts" / script),
                            str(project),
                            option,
                            str(fake),
                            "--dry-run",
                            "--",
                            "--access-token",
                            secret,
                        ],
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("upstream arguments withheld", result.stdout)
                    self.assertNotIn(secret, result.stdout + result.stderr)

    def test_lichtfeld_output_must_stay_outside_the_input_project(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            fake = root / "fake-tool"
            fake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake.chmod(0o755)
            output = project / "new-output"
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "lichtfeld_wrapper.sh"),
                    str(project),
                    "--lichtfeld",
                    str(fake),
                    "--output",
                    str(output),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("outside the COLMAP project", result.stderr)
            self.assertFalse(output.exists())

    def test_wrappers_reject_wrong_output_object_types(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            fake = root / "fake-tool"
            fake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake.chmod(0o755)

            output_directory = root / "ply-is-a-directory"
            output_directory.mkdir()
            opensplat = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "opensplat_wrapper.sh"),
                    str(project),
                    "--opensplat",
                    str(fake),
                    "--output",
                    str(output_directory),
                    "--force",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(opensplat.returncode, 2)
            self.assertIn("is a directory", opensplat.stderr)

            output_file = root / "directory-is-a-file"
            output_file.write_text("keep", encoding="utf-8")
            lichtfeld = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "lichtfeld_wrapper.sh"),
                    str(project),
                    "--lichtfeld",
                    str(fake),
                    "--output",
                    str(output_file),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(lichtfeld.returncode, 2)
            self.assertIn("not a directory", lichtfeld.stderr)
            self.assertEqual(output_file.read_text(encoding="utf-8"), "keep")

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

    def test_lichtfeld_wrapper_rejects_existing_output(self):
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
                '  if [[ $1 == -o ]]; then printf new > "$2/result.ply"; exit 0; fi\n'
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
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("already exists", result.stderr)
            self.assertEqual((output / "old.txt").read_text(encoding="utf-8"), "old")
            self.assertFalse((output / "result.ply").exists())

    def test_lichtfeld_wrapper_rejects_symbolic_link_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self.make_colmap_project(root)
            output = root / "result"
            target = root / "outside.txt"
            target.write_text("outside", encoding="utf-8")
            fake = root / "fake-lichtfeld"
            fake.write_text(
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                "while [[ $# -gt 0 ]]; do\n"
                '  if [[ $1 == -o ]]; then ln -s "' + str(target) + '" "$2/link"; exit 0; fi\n'
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
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("symbolic link or special file", result.stderr)
            self.assertFalse(output.exists())
            self.assertEqual(target.read_text(encoding="utf-8"), "outside")

    def test_lichtfeld_wrapper_rejects_force_without_changes(self):
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
            self.assertEqual(result.returncode, 2)
            self.assertIn("cannot be replaced atomically", result.stderr)
            self.assertEqual(old.read_text(encoding="utf-8"), "old")

    def test_retired_general_installers_fail_without_writes(self):
        for script in (
            "setup_all.sh",
            "setup_feedforward.sh",
            "setup_glomap.sh",
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

    def test_da3_setup_rejects_an_unreviewed_source_override(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = os.environ.copy()
            environment["MELKOR_DA3_REF"] = "0" * 40
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "setup_da3.sh"),
                    "--accept-unlocked-dependencies",
                ],
                cwd=directory,
                env=environment,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("cannot change the supported DA3 profile", result.stdout)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_da3_setup_rejects_a_secret_package_index(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = os.environ.copy()
            secret = "melkor-test-secret"
            environment["MELKOR_TORCH_INDEX_URL"] = f"https://user:{secret}@example.invalid/simple"
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "setup_da3.sh"),
                    "--accept-unlocked-dependencies",
                ],
                cwd=directory,
                env=environment,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            output = result.stdout + result.stderr
            self.assertIn("without credentials", output)
            self.assertNotIn(secret, output)
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

    def test_feedforward_setup_rejects_multiple_selections(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [
                    str(REPO_ROOT / "scripts" / "setup_feedforward_sota.sh"),
                    "mapanything",
                    "moge2",
                ],
                cwd=directory,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("Specify one model or group", result.stderr)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_feedforward_setup_requires_restricted_terms_before_writes(self):
        for selection, required_flag in (
            ("vggt", "--accept-noncommercial"),
            ("amb3r", "--accept-unlicensed"),
            ("all", "--accept-noncommercial"),
        ):
            with self.subTest(selection=selection), tempfile.TemporaryDirectory() as directory:
                result = subprocess.run(
                    [
                        str(REPO_ROOT / "scripts" / "setup_feedforward_sota.sh"),
                        selection,
                        "--accept-unlocked-dependencies",
                    ],
                    cwd=directory,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn(required_flag, result.stderr)
                self.assertIn("No selected tool was installed", result.stderr)
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
