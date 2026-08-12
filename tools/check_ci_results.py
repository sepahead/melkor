#!/usr/bin/env python3
"""Check the required GitHub Actions job results."""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections.abc import Mapping

REQUIRED_JOBS = (
    "secrets-scan",
    "dependency-review",
    "build-macos",
    "sanitize-macos",
    "build-linux",
    "build-windows",
    "lint-python",
    "viewer-tests",
    "tauri-check",
    "fuzz-smoke",
)
MAX_RESULTS_BYTES = 1024 * 1024


def parse_results(raw: bytes) -> object:
    """Parse bounded, strict UTF-8 JSON job results."""

    if len(raw) > MAX_RESULTS_BYTES:
        raise ValueError("the job results exceed 1 MiB")

    def object_without_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
        value: dict[str, object] = {}
        for name, item in pairs:
            if name in value:
                raise ValueError(f"duplicate object name {name!r}")
            value[name] = item
        return value

    def reject_constant(value: str) -> None:
        raise ValueError(f"non-JSON number {value!r}")

    def finite_float(value: str) -> float:
        parsed = float(value)
        if not math.isfinite(parsed):
            raise ValueError(f"non-finite JSON number {value!r}")
        return parsed

    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=object_without_duplicates,
            parse_constant=reject_constant,
            parse_float=finite_float,
        )
    except RecursionError as exc:
        raise ValueError("JSON nesting is too deep") from exc


def check_results(results: object, event: str) -> list[str]:
    """Return one error for each invalid or missing job result."""
    if not isinstance(results, Mapping):
        return ["the job results must be a JSON object"]

    errors: list[str] = []
    expected = set(REQUIRED_JOBS)
    actual = set(results)

    for job in sorted(expected - actual):
        errors.append(f"{job}=missing")
    for job in sorted(actual - expected):
        errors.append(f"{job}=unexpected")

    for job in REQUIRED_JOBS:
        info = results.get(job)
        if info is None:
            continue
        if not isinstance(info, Mapping):
            errors.append(f"{job}=invalid")
            continue

        result = info.get("result")
        allowed = result == "success"
        if event == "push" and job == "dependency-review":
            allowed = result == "skipped"
        if not allowed:
            errors.append(f"{job}={result}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", required=True, choices=("pull_request", "push"))
    args = parser.parse_args()

    try:
        results = parse_results(sys.stdin.buffer.read(MAX_RESULTS_BYTES + 1))
    except (json.JSONDecodeError, UnicodeDecodeError, ValueError) as error:
        print(f"CI Gate FAILED: invalid JSON: {error}", file=sys.stderr)
        return 1

    if isinstance(results, Mapping):
        for job in sorted(results):
            info = results[job]
            result = info.get("result") if isinstance(info, Mapping) else "invalid"
            print(f"  {job:<24} {result}")

    errors = check_results(results, args.event)
    if errors:
        print("CI Gate FAILED: " + ", ".join(errors), file=sys.stderr)
        return 1

    print("CI Gate passed: all required jobs have the required result.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
