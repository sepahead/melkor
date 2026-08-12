#!/usr/bin/env python3
"""Build the audited source bundle from an explicit allowlist.

The dangerous way to build a source release is "archive everything tracked". It works right
up until someone commits a model checkpoint, a dataset, a local scratch directory, or a
research snapshot under terms incompatible with the core license — and then the release
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
import importlib.util
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import unicodedata
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MAX_SOURCE_FILES = 100_000
MAX_SOURCE_FILE_BYTES = 64 * 1024 * 1024
MAX_SOURCE_TOTAL_BYTES = 512 * 1024 * 1024
MAX_SOURCE_DATE_EPOCH = (1 << 32) - 1
MAX_SOURCE_PATH_BYTES = 240
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

# Every path that belongs in a source release. A directory includes its tracked contents.
#
# Adding a line here is a deliberate act with license and size consequences. Read
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
    "viewer/",
    # Data that defines behavior
    "profiles/",
    "schemas/",
    # Tests and fuzzing
    "tests/",
    "fuzz/",
    # Benchmark definitions and dataset metadata. Dataset bytes are never stored in Git.
    "benchmarks/README.md",
    "benchmarks/datasets/manifest.json",
    "benchmarks/schema/",
    # Documentation
    "docs/",
    "assets/",  # logos referenced by README.md; without these the bundle's README is broken
    # Tooling
    "tools/",
    # Keep each script explicit. The project documentation and release checks use these entry
    # points. A new script stays excluded until this list receives a review.
    "scripts/build_release_evidence.py",
    "scripts/glomap_wrapper.sh",
    "scripts/lichtfeld_wrapper.sh",
    "scripts/opensplat_wrapper.sh",
    "scripts/pipeline.sh",
    "scripts/setup_all.sh",
    "scripts/setup_da3.sh",
    "scripts/setup_deps.sh",
    "scripts/setup_feedforward.sh",
    "scripts/setup_feedforward_sota.sh",
    "scripts/setup_glomap.sh",
    "scripts/setup_gsplat_cuda.sh",
    "scripts/setup_gsplat_mps.sh",
    "scripts/setup_lichtfeld.sh",
    "scripts/setup_opensplat.sh",
    "scripts/setup_streaming.sh",
    "scripts/test_sdk_install.sh",
    "scripts/train_from_images.sh",
    # Pinned dependencies and their provenance
    "third_party/",
    # Release metadata
    "release/README.md",
    "release/components.json",
    # Project metadata
    "pyproject.toml",
    "CITATION.cff",
    "AGENTS.md",
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
    ".github/CODEOWNERS",
    ".clang-format",
    ".clang-tidy",
    ".editorconfig",
]

# Defense in depth. Even if one of these somehow sits under an allowlisted directory, it does
# not ship. The allowlist alone should be sufficient; this exists because "should be" is not
# a property you want to bet a license violation on.
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
# Directory component prefixes that must never ship.
EXCLUDED_DIR_PREFIXES = [p for p in EXCLUDED_PATTERNS if not p.endswith("/")]

# File extensions that must never ship, matched case-insensitively against the full suffix.
#
# Model weights are the ones that cause license incidents -- a research-only checkpoint bundled
# into an MIT release. The list is deliberately broad and matched without regard to case, so
# `.PT` and `.SafeTensors` are caught as surely as `.pt` and `.safetensors`.
EXCLUDED_EXTENSIONS = [
    # PyTorch / generic tensor formats
    ".ckpt",
    ".pt",
    ".pth",
    ".safetensors",
    ".t7",
    ".pkl",
    # ONNX / TF / other frameworks
    ".onnx",
    ".pb",
    ".tflite",
    ".caffemodel",
    ".params",
    # Apple CoreML
    ".mlmodel",
    ".mlpackage",
    # NumPy / HDF5 arrays and blobs
    ".npy",
    ".npz",
    ".h5",
    ".hdf5",
    ".bin",
    ".weights",
    ".gguf",
    ".ggml",
    # Keys and certificates
    ".pem",
    ".key",
    ".p12",
    ".pfx",
    ".keystore",
    ".jks",
]

# Basename patterns for secret files, matched case-insensitively against the FILE NAME only
# (never the whole path), so a legitimate source file deeper in the tree is not caught by a
# substring collision. `.env` and every `.env.*` variant, and any name containing "credential"
# or "secret", are refused.
EXCLUDED_BASENAME_PREFIXES = ["id_rsa", ".env"]
EXCLUDED_BASENAME_CONTAINS = ["credential", "secret"]


def escape_diagnostic(text: str) -> str:
    """Escape terminal controls in user-controlled diagnostic text."""
    escaped: list[str] = []
    for char in text:
        if char == "\\":
            escaped.append("\\\\")
        elif char.isprintable():
            escaped.append(char)
        else:
            escaped.append(char.encode("unicode_escape").decode("ascii"))
    return "".join(escaped)


def resolve_commit(ref: str, repo: Path = REPO_ROOT) -> str:
    """Resolve one ref to an immutable commit object ID."""
    resolved = subprocess.run(
        [
            "git",
            "rev-parse",
            "--verify",
            "--quiet",
            "--end-of-options",
            f"{ref}^{{commit}}",
        ],
        cwd=repo,
        capture_output=True,
        check=False,
    )
    try:
        commit = resolved.stdout.decode("ascii", "strict").strip()
    except UnicodeDecodeError as exc:
        raise SystemExit("error: Git returned a non-ASCII commit object ID") from exc
    if (
        resolved.returncode != 0
        or len(commit) not in {40, 64}
        or any(char not in "0123456789abcdefABCDEF" for char in commit)
    ):
        raise SystemExit("error: Git ref does not resolve to a commit: " + escape_diagnostic(ref))
    return commit.lower()


def _git_tracked_entries_at_commit(commit: str, repo: Path = REPO_ROOT) -> list[tuple[str, str]]:
    """Return every tracked file at one resolved commit."""
    out = subprocess.run(
        ["git", "ls-tree", "-rz", "--full-tree", commit],
        cwd=repo,
        capture_output=True,
        check=True,
    )
    entries: list[tuple[str, str]] = []
    for record in out.stdout.split(b"\0"):
        if not record:
            continue
        try:
            meta, raw_path = record.split(b"\t", 1)
            mode, object_type, _oid = meta.split(b" ", 2)
            path = raw_path.decode("utf-8", "strict")
        except (UnicodeDecodeError, ValueError) as exc:
            raise SystemExit("error: Git tree contains an unsupported entry") from exc
        if object_type != b"blob":
            raise SystemExit(
                "error: tracked entry is not a self-contained blob: " + escape_diagnostic(path)
            )
        mode = mode.decode("ascii")
        if mode not in {"100644", "100755", "120000"}:
            raise SystemExit(f"error: unsupported Git mode {mode} for {escape_diagnostic(path)}")
        entries.append((mode, path))
        if len(entries) > MAX_SOURCE_FILES:
            raise SystemExit(f"error: Git tree has more than {MAX_SOURCE_FILES} files")
    return sorted(entries, key=lambda entry: entry[1])


def git_tracked_entries(ref: str, repo: Path = REPO_ROOT) -> list[tuple[str, str]]:
    """Every tracked file at ``ref`` as (mode, path).

    Built from git, not the working tree, so uncommitted local state can never leak into a
    release. The mode is carried so symlinks (120000) can be recognized: ``git show`` of a
    symlink returns its *target path* as bytes, and storing that as a regular file would put a
    wrong, misleading file in the bundle.
    """
    commit = resolve_commit(ref, repo)
    return _git_tracked_entries_at_commit(commit, repo)


def unsafe_path_reason(path: str) -> str | None:
    """Return a reason when an archive path is unsafe or ambiguous."""
    if not path or path.startswith("/") or path.endswith("/") or "//" in path or "\\" in path:
        return "unsafe source path"
    if any(ord(char) < 32 or ord(char) == 127 for char in path):
        return "source path contains a control character"
    if len(path.encode("utf-8")) > MAX_SOURCE_PATH_BYTES:
        return f"source path exceeds {MAX_SOURCE_PATH_BYTES} UTF-8 bytes"
    if unicodedata.normalize("NFC", path) != path:
        return "source path is not Unicode NFC"
    raw_parts = path.split("/")
    if any(part in {"", ".", ".."} for part in raw_parts):
        return "unsafe source path component"
    if raw_parts[0].casefold() == ".git":
        return "reserved .git path"
    for part in raw_parts:
        if part.endswith((" ", ".")) or any(char in WINDOWS_INVALID_CHARS for char in part):
            return "source path is not portable to Windows"
        if part.split(".", 1)[0].upper() in WINDOWS_RESERVED_NAMES:
            return "source path uses a reserved Windows name"
    return None


def portable_path_key(path: str) -> str:
    """Return the cross-platform collision key for one safe path."""
    return unicodedata.normalize("NFC", path).casefold()


def exclusion_reason(path: str) -> str | None:
    """Why ``path`` must not ship, or None if the denylist does not object.

    Returns a reason string so the report can say *why* something was dropped, which matters
    when the thing dropped is a weight or a secret rather than a build artifact.
    """
    unsafe_reason = unsafe_path_reason(path)
    if unsafe_reason is not None:
        return unsafe_reason

    lower = path.lower()
    basename = path.rsplit("/", 1)[-1].lower()
    components = lower.split("/")

    for directory in EXCLUDED_DIRS:
        if directory.lower() in components:
            return f"excluded directory '{directory}/'"

    for prefix in EXCLUDED_DIR_PREFIXES:
        if any(component.startswith(prefix.lower()) for component in components[:-1]):
            return f"excluded directory prefix '{prefix}'"

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
    # The denylist is defense in depth over the allowlist: even a path under an allowlisted
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


def select_entries(
    entries: list[tuple[str, str]],
) -> tuple[list[str], list[tuple[str, str]]]:
    """Return the approved paths and each excluded path with its reason.

    The source bundle and release evidence use this function. This keeps one source boundary.
    Symlinks are excluded because this builder cannot preserve them safely.
    """
    included: list[str] = []
    excluded: list[tuple[str, str]] = []
    portable_paths: dict[str, str] = {}
    for mode, path in entries:
        if mode == "120000":
            excluded.append((path, "symlink (target would be stored as file content)"))
            continue
        if is_allowed(path):
            key = portable_path_key(path)
            previous = portable_paths.get(key)
            if previous is not None:
                raise ValueError(f"portable source path collision: {previous!r} and {path!r}")
            portable_paths[key] = path
            included.append(path)
        else:
            reason = exclusion_reason(path) or "not on the allowlist"
            excluded.append((path, reason))
    return included, excluded


def _select_commit(commit: str, repo: Path = REPO_ROOT) -> tuple[list[str], list[tuple[str, str]]]:
    """Return the approved paths from one resolved commit."""
    try:
        return select_entries(_git_tracked_entries_at_commit(commit, repo))
    except ValueError as exc:
        raise SystemExit(f"error: {exc}") from exc


def select(ref: str, repo: Path = REPO_ROOT) -> tuple[list[str], list[tuple[str, str]]]:
    """Resolve a ref and return its approved paths."""
    return _select_commit(resolve_commit(ref, repo), repo)


def _file_bytes_at_commit(commit: str, path: str, repo: Path = REPO_ROOT) -> bytes:
    object_name = f"{commit}:{path}"
    size_result = subprocess.run(
        ["git", "cat-file", "-s", object_name],
        cwd=repo,
        capture_output=True,
        check=False,
    )
    try:
        size = int(size_result.stdout.decode("ascii", "strict").strip(), 10)
    except (UnicodeDecodeError, ValueError) as exc:
        raise SystemExit(
            "error: cannot read the Git blob size for " + escape_diagnostic(path)
        ) from exc
    if size_result.returncode != 0 or size < 0:
        raise SystemExit("error: cannot read the Git blob for " + escape_diagnostic(path))
    if size > MAX_SOURCE_FILE_BYTES:
        raise SystemExit(
            f"error: source file exceeds the {MAX_SOURCE_FILE_BYTES}-byte limit: "
            + escape_diagnostic(path)
        )
    out = subprocess.run(
        ["git", "show", object_name],
        cwd=repo,
        capture_output=True,
        check=False,
    )
    if out.returncode != 0 or len(out.stdout) != size:
        raise SystemExit("error: cannot read the Git blob for " + escape_diagnostic(path))
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

        # An empty filename and mtime 0 produce one platform-neutral gzip header.
        compressed = io.BytesIO()
        with gzip.GzipFile(
            filename="", mode="wb", compresslevel=9, fileobj=compressed, mtime=0
        ) as handle:
            handle.write(tar_bytes)
        return compressed.getvalue()
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


def _build_commit(
    commit: str,
    included: list[str],
    output: Path,
    source_date_epoch: int,
    repo: Path = REPO_ROOT,
) -> str:
    if (
        isinstance(source_date_epoch, bool)
        or not isinstance(source_date_epoch, int)
        or source_date_epoch < 0
        or source_date_epoch > MAX_SOURCE_DATE_EPOCH
    ):
        raise SystemExit(f"error: SOURCE_DATE_EPOCH must be from 0 through {MAX_SOURCE_DATE_EPOCH}")
    if len(included) > MAX_SOURCE_FILES:
        raise SystemExit(f"error: source bundle has more than {MAX_SOURCE_FILES} files")
    if output.exists() or output.is_symlink():
        raise SystemExit("error: output path already exists: " + escape_diagnostic(str(output)))
    output.parent.mkdir(parents=True, exist_ok=True)
    output = output.parent.resolve(strict=True) / output.name
    modes_by_path = {path: mode for mode, path in _git_tracked_entries_at_commit(commit, repo)}

    raw = io.BytesIO()
    total_size = 0
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for path in included:  # already sorted -> deterministic entry order
            mode = modes_by_path.get(path)
            if mode not in {"100644", "100755"}:
                raise SystemExit(
                    "error: selected path is not a tracked regular file: " + escape_diagnostic(path)
                )
            data = _file_bytes_at_commit(commit, path, repo)
            total_size += len(data)
            if total_size > MAX_SOURCE_TOTAL_BYTES:
                raise SystemExit(
                    f"error: selected source exceeds the {MAX_SOURCE_TOTAL_BYTES}-byte limit"
                )
            info = tarfile.TarInfo(name=path)
            info.size = len(data)
            # Everything below is what makes two builds of one commit byte-identical.
            info.mtime = source_date_epoch
            info.mode = 0o755 if mode == "100755" else 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.type = tarfile.REGTYPE
            tar.addfile(info, io.BytesIO(data))

    payload = _compress(raw.getvalue(), output)
    temporary: Path | None = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
        )
        temporary = Path(temporary_name)
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        temporary.chmod(0o644)
        try:
            publisher_path = Path(__file__).resolve().with_name("atomic_publish.py")
            spec = importlib.util.spec_from_file_location(
                "melkor_source_bundle_atomic_publish", publisher_path
            )
            if spec is None or spec.loader is None:
                raise OSError("cannot load the atomic publish tool")
            publisher = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(publisher)
            publish = getattr(publisher, "publish", None)
            if not callable(publish):
                raise OSError("the atomic publish tool does not provide publish")
            publish(temporary, output, False)
        except FileExistsError as exc:
            raise SystemExit(
                "error: output path already exists: " + escape_diagnostic(str(output))
            ) from exc
        except (OSError, ValueError) as exc:
            raise SystemExit(
                "error: source bundle could not be published without replacing an existing file"
            ) from exc
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    # Hash the FINAL file the user receives, not the uncompressed intermediate, so the digest
    # verifies the artifact that actually ships.
    return hashlib.sha256(payload).hexdigest()


def build(
    ref: str,
    output: Path,
    source_date_epoch: int,
    repo: Path = REPO_ROOT,
) -> str:
    """Resolve one ref and build its source bundle."""
    commit = resolve_commit(ref, repo)
    included, _ = _select_commit(commit, repo)
    return _build_commit(commit, included, output, source_date_epoch, repo)


def _print_check_report(ref: str, included: list[str], excluded: list[tuple[str, str]]) -> None:
    print(f"Source bundle from {escape_diagnostic(ref)}\n")
    print(f"  included: {len(included)} files")
    print(f"  excluded: {len(excluded)} files\n")

    # List sensitive exclusions by name so that the review cannot miss them.
    notable = [
        (path, reason)
        for path, reason in excluded
        if any(
            term in reason
            for term in (
                "weight",
                "secret",
                "symlink",
                "key",
                "unsafe",
                "control character",
            )
        )
    ]
    if notable:
        print("Excluded for safety (verify none of these should have shipped elsewhere):")
        for path, reason in notable:
            print(f"  {escape_diagnostic(path)}  -- {escape_diagnostic(reason)}")
        print()

    if excluded:
        print("Excluded, grouped by top-level directory:")
        shown: dict[str, int] = {}
        for path, _reason in excluded:
            top = path.split("/")[0] if "/" in path else path
            shown[top] = shown.get(top, 0) + 1
        for top, count in sorted(shown.items(), key=lambda item: -item[1]):
            print(f"  {count:>5}  {escape_diagnostic(top)}")
        print(
            "\nIf something above belongs in a source release, add it to ALLOWLIST\n"
            "deliberately. Do not widen the allowlist to make a warning go away."
        )


def main(argv: list[str] | None = None, repo: Path = REPO_ROOT) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ref", default="HEAD", help="git ref to bundle (default: HEAD)")
    parser.add_argument("--output", type=Path, help="output tar path")
    parser.add_argument(
        "--check",
        action="store_true",
        help="report what would be included and excluded, without writing",
    )
    args = parser.parse_args(argv)

    commit = resolve_commit(args.ref, repo)
    included, excluded = _select_commit(commit, repo)

    if args.check or not args.output:
        _print_check_report(args.ref, included, excluded)
        return 0

    epoch_environment = os.environ.get("SOURCE_DATE_EPOCH")
    epoch_text = "0" if epoch_environment is None else epoch_environment
    try:
        epoch = int(epoch_text, 10)
    except ValueError as exc:
        raise SystemExit("error: SOURCE_DATE_EPOCH must be a nonnegative integer") from exc
    if epoch < 0 or epoch > MAX_SOURCE_DATE_EPOCH:
        raise SystemExit(f"error: SOURCE_DATE_EPOCH must be from 0 through {MAX_SOURCE_DATE_EPOCH}")
    if epoch_environment is None:
        print(
            "warning: SOURCE_DATE_EPOCH is unset; using 0 so the bundle stays reproducible.",
            file=sys.stderr,
        )

    digest = _build_commit(commit, included, args.output, epoch, repo)
    print(f"wrote {escape_diagnostic(str(args.output))}")
    print(f"  files:  {len(included)}")
    print(f"  sha256: {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
