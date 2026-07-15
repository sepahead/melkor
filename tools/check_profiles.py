#!/usr/bin/env python3
"""Validate every format profile in profiles/ against the format-profile schema.

Profiles are descriptive data that the format adapters read to know a format's exact semantic
conventions. A malformed profile would make an adapter misread a file, so every profile is
validated against schemas/format-profile.schema.json in CI.
"""
from __future__ import annotations
import json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "schemas" / "format-profile.schema.json"


def main() -> int:
    try:
        import jsonschema
    except ImportError:
        # Without jsonschema, still check every profile is well-formed JSON with the core fields.
        jsonschema = None

    schema = json.loads(SCHEMA.read_text())
    profiles = sorted((ROOT / "profiles").rglob("*.json"))
    errors = 0
    for path in profiles:
        try:
            data = json.loads(path.read_text())
        except json.JSONDecodeError as exc:
            print(f"  INVALID JSON {path.relative_to(ROOT)}: {exc}", file=sys.stderr)
            errors += 1
            continue
        # Core fields every profile must carry.
        for field in ("schema_version", "profile_id", "container", "status"):
            if field not in data:
                print(f"  {path.relative_to(ROOT)}: missing required field '{field}'", file=sys.stderr)
                errors += 1
        if jsonschema is not None:
            try:
                jsonschema.validate(data, schema)
            except jsonschema.ValidationError as exc:
                print(f"  {path.relative_to(ROOT)}: {exc.message}", file=sys.stderr)
                errors += 1
        print(f"  ok  {path.relative_to(ROOT)}  ({data.get('profile_id')})")

    if errors:
        print(f"\n{errors} profile error(s).", file=sys.stderr)
        return 1
    print(f"\nAll {len(profiles)} format profiles validate.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
