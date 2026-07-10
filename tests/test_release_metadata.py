#!/usr/bin/env python3
"""Keep user-visible release metadata synchronized across build systems."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def require_match(pattern: str, text: str, source: str) -> str:
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"could not read version from {source}")
    return match.group(1)


def main() -> int:
    cmake_base = require_match(
        r"^project\(melkor VERSION ([0-9]+\.[0-9]+\.[0-9]+)",
        (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
        "CMakeLists.txt",
    )
    prerelease = require_match(
        r'^set\(MELKOR_PRERELEASE "([0-9A-Za-z.-]+)"\)',
        (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
        "CMakeLists.txt",
    )
    cmake = f"{cmake_base}-{prerelease}"
    package = json.loads((ROOT / "viewer/package.json").read_text(encoding="utf-8"))[
        "version"
    ]
    tauri = json.loads(
        (ROOT / "viewer/src-tauri/tauri.conf.json").read_text(encoding="utf-8")
    )["version"]
    cargo = require_match(
        r'^version\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+-[0-9A-Za-z.-]+)"',
        (ROOT / "viewer/src-tauri/Cargo.toml").read_text(encoding="utf-8"),
        "viewer/src-tauri/Cargo.toml",
    )
    versions = {"CMake": cmake, "npm": package, "Tauri": tauri, "Cargo": cargo}
    assert len(set(versions.values())) == 1, f"release version drift: {versions}"

    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    assert f"## {cmake} " in changelog, f"CHANGELOG has no {cmake} release section"
    print(f"release metadata synchronized at {cmake}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
