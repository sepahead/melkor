#!/usr/bin/env python3
"""Release metadata must agree with the authoritative VERSION file.

This test used to re-derive the version by regexing ``CMakeLists.txt`` and comparing it
against ``package.json``, the Tauri config, and ``Cargo.toml``. That made it a *second*
implementation of version synchronization, which is the same class of bug it was meant to
catch: two places that must agree, and nothing forcing them to.

It now delegates to ``tools/check_version_sync.py``, which is the single implementation. It
covers strictly more than the old test did — the lockfiles, the PEP 440 mapping for the
Python distribution, the changelog, and a structural check that CMake never restates a
literal version.

Kept as a separate entry point because CTest registers it as ``release_metadata_tests`` and
the release-candidate workflow invokes it by path.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "check_version_sync.py"), "--check"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )

    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)

    if result.returncode != 0:
        print("\nrelease metadata is not synchronized with VERSION", file=sys.stderr)
        return 1

    print("release metadata synchronized with VERSION")
    return 0


if __name__ == "__main__":
    sys.exit(main())
