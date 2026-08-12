#!/usr/bin/env python3
"""Validate each format profile against the format-profile schema.

A profile defines the exact semantic rules for one format. CI rejects a profile that does not
conform to ``schemas/format-profile.schema.json``.
"""

from __future__ import annotations

import json
import math
import os
import re
import stat
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "schemas" / "format-profile.schema.json"
RUNTIME_PROFILES = ROOT / "src" / "formats" / "profile.cpp"
MAX_JSON_BYTES = 4 * 1024 * 1024


def strict_json(path: Path) -> object:
    """Read bounded UTF-8 JSON and reject duplicate object names."""

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

    if path.is_symlink():
        raise ValueError("file must not be a symbolic link")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as handle:
        initial = os.fstat(handle.fileno())
        if not stat.S_ISREG(initial.st_mode) or initial.st_size > MAX_JSON_BYTES:
            raise ValueError(f"file must be a regular file of at most {MAX_JSON_BYTES} bytes")
        raw = handle.read(MAX_JSON_BYTES + 1)
        final = os.fstat(handle.fileno())
        if (
            len(raw) != initial.st_size
            or final.st_dev != initial.st_dev
            or final.st_ino != initial.st_ino
            or final.st_size != initial.st_size
            or final.st_mtime_ns != initial.st_mtime_ns
            or final.st_ctime_ns != initial.st_ctime_ns
        ):
            raise ValueError("file changed while it was read")
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=object_without_duplicates,
            parse_constant=reject_constant,
            parse_float=finite_float,
        )
    except RecursionError as exc:
        raise ValueError("JSON nesting is too deep") from exc


def load_jsonschema():
    """Load the required schema validator."""

    try:
        import jsonschema
    except ModuleNotFoundError as exc:
        if exc.name != "jsonschema":
            raise
        print(
            "ERROR: jsonschema is required. Install jsonschema==4.26.0.",
            file=sys.stderr,
        )
        return None
    return jsonschema


def make_validator(jsonschema, schema: object):
    """Create a validator for the schema draft that the schema selects."""

    validator_class = jsonschema.validators.validator_for(schema)
    validator_class.check_schema(schema)
    return validator_class(schema)


def profile_errors(validator, data: object) -> list:
    """Return profile errors in a stable order."""

    return sorted(
        validator.iter_errors(data),
        key=lambda error: (
            tuple(str(part) for part in error.absolute_path),
            error.message,
        ),
    )


def error_location(error) -> str:
    """Return a short JSON path for one validation error."""

    location = "$"
    for part in error.absolute_path:
        if isinstance(part, int):
            location += f"[{part}]"
        else:
            location += f".{part}"
    return location


def duplicate_profile_ids(profiles: list[tuple[Path, object]]) -> list[tuple[str, Path, Path]]:
    """Return duplicate profile IDs in path order."""

    first_paths: dict[str, Path] = {}
    duplicates = []
    for path, data in profiles:
        if not isinstance(data, dict) or not isinstance(data.get("profile_id"), str):
            continue
        profile_id = data["profile_id"]
        if profile_id in first_paths:
            duplicates.append((profile_id, first_paths[profile_id], path))
        else:
            first_paths[profile_id] = path
    return duplicates


def runtime_profiles(source: str) -> dict[str, dict[str, object]]:
    """Read profile capabilities from the constexpr runtime table."""

    pattern = re.compile(
        r"\{\s*FormatProfileId::[a-z0-9_]+,\s*\"([^\"]+)\",\s*"
        r"((?:container_bit\(FormatId::[a-z]+\)"
        r"(?:\s*\|\s*container_bit\(FormatId::[a-z]+\))*)),\s*"
        r"((?:container_bit\(FormatId::[a-z]+\)"
        r"(?:\s*\|\s*container_bit\(FormatId::[a-z]+\))*)),\s*"
        r"(\d+)\s*,?\s*\}",
        re.DOTALL,
    )
    container_pattern = re.compile(r"container_bit\(FormatId::([a-z]+)\)")
    profiles: dict[str, dict[str, object]] = {}
    for profile_id, read_expression, write_expression, degree_text in pattern.findall(source):
        if profile_id in profiles:
            raise ValueError(f"duplicate runtime profile ID {profile_id!r}")
        profiles[profile_id] = {
            "max_sh_degree": int(degree_text),
            "read_containers": sorted(container_pattern.findall(read_expression)),
            "write_containers": sorted(container_pattern.findall(write_expression)),
        }
    if not profiles:
        raise ValueError("the runtime profile table contains no readable entries")
    return profiles


def runtime_profile_degrees(source: str) -> dict[str, int]:
    """Read profile IDs and SH limits for compatibility with tool callers."""

    return {
        profile_id: int(values["max_sh_degree"])
        for profile_id, values in runtime_profiles(source).items()
    }


def semantic_errors(data: object) -> list[str]:
    """Return semantic profile errors that JSON Schema cannot express clearly."""
    if not isinstance(data, dict):
        return []
    errors: list[str] = []
    sh = data.get("sh")
    if isinstance(sh, dict):
        degree = sh.get("max_degree")
        counts = sh.get("coefficients_per_degree")
        if isinstance(degree, int) and isinstance(counts, list):
            expected = [2 * value + 1 for value in range(degree + 1)]
            if counts != expected:
                errors.append(
                    f"$.sh.coefficients_per_degree must equal {expected} for degree {degree}"
                )
    if data.get("opacity_domain") == "linear":
        opacity_range = data.get("opacity_range")
        if not (
            isinstance(opacity_range, list)
            and len(opacity_range) == 2
            and all(
                isinstance(value, (int, float)) and not isinstance(value, bool)
                for value in opacity_range
            )
            and 0.0 <= opacity_range[0] <= opacity_range[1] <= 1.0
        ):
            errors.append("$.opacity_range must be an ordered subset of [0, 1]")
    read_containers = data.get("read_containers")
    write_containers = data.get("write_containers")
    primary_container = data.get("container")
    if isinstance(read_containers, list) and isinstance(primary_container, str):
        if primary_container not in read_containers:
            errors.append("$.container must occur in $.read_containers")
    if isinstance(read_containers, list) and isinstance(write_containers, list):
        unsupported_writes = sorted(set(write_containers) - set(read_containers))
        if unsupported_writes:
            errors.append(
                "$.write_containers must be a subset of $.read_containers: "
                + ", ".join(unsupported_writes)
            )
    markers = data.get("header_markers")
    if isinstance(markers, dict):
        profile_id = data.get("profile_id")
        if isinstance(profile_id, str):
            expected_marker = profile_id.removeprefix("ply:")
            if markers.get("melkor_profile") != expected_marker:
                errors.append(f"$.header_markers.melkor_profile must equal {expected_marker!r}")
        for marker_name, profile_name in (
            ("melkor_quaternion_order", "quaternion_order"),
            ("melkor_scale_domain", "scale_domain"),
            ("melkor_opacity_domain", "opacity_domain"),
        ):
            marker_value = markers.get(marker_name)
            profile_value = data.get(profile_name)
            if marker_value is not None and marker_value != profile_value:
                errors.append(f"$.header_markers.{marker_name} must equal $.{profile_name}")
        degree = sh.get("max_degree") if isinstance(sh, dict) else None
        if isinstance(degree, int) and markers.get("melkor_sh_degree") != f"0-{degree}":
            errors.append(f"$.header_markers.melkor_sh_degree must equal '0-{degree}'")
    return errors


def read_runtime_source(path: Path) -> str:
    """Read the runtime profile table through the bounded JSON reader limit."""
    if path.is_symlink():
        raise ValueError("runtime profile source must not be a symbolic link")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as handle:
        initial = os.fstat(handle.fileno())
        if not stat.S_ISREG(initial.st_mode) or initial.st_size > MAX_JSON_BYTES:
            raise ValueError("runtime profile source is not a bounded regular file")
        raw = handle.read(MAX_JSON_BYTES + 1)
        final = os.fstat(handle.fileno())
        if (
            len(raw) != initial.st_size
            or final.st_dev != initial.st_dev
            or final.st_ino != initial.st_ino
            or final.st_size != initial.st_size
            or final.st_mtime_ns != initial.st_mtime_ns
            or final.st_ctime_ns != initial.st_ctime_ns
        ):
            raise ValueError("runtime profile source changed while it was read")
    return raw.decode("utf-8")


def main() -> int:
    jsonschema = load_jsonschema()
    if jsonschema is None:
        return 2

    try:
        schema = strict_json(SCHEMA)
        validator = make_validator(jsonschema, schema)
    except (
        OSError,
        UnicodeDecodeError,
        ValueError,
        json.JSONDecodeError,
        jsonschema.SchemaError,
    ) as exc:
        print(f"ERROR: cannot load {SCHEMA.relative_to(ROOT)}: {exc}", file=sys.stderr)
        return 2

    profiles = sorted((ROOT / "profiles").rglob("*.json"))
    if not profiles:
        print("ERROR: profiles/ contains no JSON profiles.", file=sys.stderr)
        return 2

    errors = 0
    valid_profiles = []
    for path in profiles:
        try:
            data = strict_json(path)
        except (OSError, UnicodeDecodeError, ValueError, json.JSONDecodeError) as exc:
            print(f"  INVALID JSON {path.relative_to(ROOT)}: {exc}", file=sys.stderr)
            errors += 1
            continue

        path_errors = profile_errors(validator, data)
        semantic_path_errors = semantic_errors(data)
        if path_errors or semantic_path_errors:
            errors += len(path_errors) + len(semantic_path_errors)
            for error in path_errors:
                print(
                    f"  {path.relative_to(ROOT)} {error_location(error)}: {error.message}",
                    file=sys.stderr,
                )
            for message in semantic_path_errors:
                print(f"  {path.relative_to(ROOT)} {message}", file=sys.stderr)
            continue

        profile_id = data.get("profile_id") if isinstance(data, dict) else None
        valid_profiles.append((path, data))
        print(f"  ok  {path.relative_to(ROOT)}  ({profile_id})")

    for profile_id, first, duplicate in duplicate_profile_ids(valid_profiles):
        print(
            f"  {duplicate.relative_to(ROOT)} $.profile_id: duplicate profile ID "
            f"'{profile_id}' first used by {first.relative_to(ROOT)}",
            file=sys.stderr,
        )
        errors += 1

    try:
        runtime = runtime_profiles(read_runtime_source(RUNTIME_PROFILES))
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        print(f"ERROR: cannot load {RUNTIME_PROFILES.relative_to(ROOT)}: {exc}", file=sys.stderr)
        return 2
    documented = {
        data["profile_id"]: {
            "max_sh_degree": data["sh"]["max_degree"],
            "read_containers": sorted(data["read_containers"]),
            "write_containers": sorted(data["write_containers"]),
        }
        for _, data in valid_profiles
        if isinstance(data, dict)
    }
    if runtime != documented:
        print(
            "  profiles/: documented runtime capabilities differ from "
            f"{RUNTIME_PROFILES.relative_to(ROOT)}",
            file=sys.stderr,
        )
        errors += 1

    if errors:
        print(f"\n{errors} profile errors.", file=sys.stderr)
        return 1
    print(f"\nAll {len(profiles)} format profiles validate.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
