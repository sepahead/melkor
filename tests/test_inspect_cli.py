#!/usr/bin/env python3
"""Contract tests for the deterministic `melkor inspect` command."""

from __future__ import annotations

import gzip
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import jsonschema

TIMEOUT_SECONDS = 120
REPO_ROOT = Path(__file__).resolve().parent.parent


def run(binary: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), "inspect", *arguments],
        check=False,
        capture_output=True,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )


def load_report(
    result: subprocess.CompletedProcess[str],
    validator: jsonschema.Draft202012Validator,
) -> dict:
    report = json.loads(result.stdout)
    validator.validate(report)
    return report


def canonical_ply(
    *,
    scale: str = "0.01",
    extra_property: bool = False,
    vertex_count: int = 1,
) -> str:
    header = [
        "ply",
        "format ascii 1.0",
        "comment melkor_profile melkor-canonical-v1",
        "comment melkor_coordinate_system gltf-luf",
        "comment melkor_length_unit meter",
        "comment melkor_quaternion_order xyzw",
        "comment melkor_scale_domain linear",
        "comment melkor_opacity_domain linear",
        "comment melkor_color_space srgb_rec709_display",
        "comment melkor_sh_basis real_condon_shortley",
        "comment melkor_sh_degree 0",
        f"element vertex {vertex_count}",
        "property float x",
        "property float y",
        "property float z",
        "property float scale_x",
        "property float scale_y",
        "property float scale_z",
        "property float rotation_x",
        "property float rotation_y",
        "property float rotation_z",
        "property float rotation_w",
        "property float opacity",
        "property float sh_0_0_r",
        "property float sh_0_0_g",
        "property float sh_0_0_b",
    ]
    if extra_property:
        header.append("property float custom_extra")
    header.append("end_header")
    if vertex_count:
        row = f"-1 2 3 {scale} {scale} {scale} 0 0 0 1 0.5 0.1 0.2 0.3"
        if extra_property:
            row += " 7"
        header.append(row)
    return "\n".join(header) + "\n"


def main() -> int:
    if sys.flags.optimize:
        raise SystemExit("refusing to run under PYTHONOPTIMIZE: asserts would be stripped")
    if len(sys.argv) != 3 or sys.argv[2] not in {"ON", "OFF"}:
        raise SystemExit("usage: test_inspect_cli.py /path/to/melkor <ON|OFF>")

    binary = Path(sys.argv[1]).resolve()
    spz_enabled = sys.argv[2] == "ON"
    schema = json.loads(
        (REPO_ROOT / "schemas" / "inspect-v1.schema.json").read_text(encoding="utf-8")
    )
    jsonschema.Draft202012Validator.check_schema(schema)
    validator = jsonschema.Draft202012Validator(schema)

    with tempfile.TemporaryDirectory(prefix="melkor-inspect-") as directory:
        root = Path(directory)
        canonical = root / "canonical.ply"
        canonical.write_text(canonical_ply(), encoding="utf-8")
        source_bytes = canonical.read_bytes()
        source_mtime = canonical.stat().st_mtime_ns

        first = run(binary, str(canonical), "--json")
        second = run(binary, str(canonical), "--json")
        assert first.returncode == 0, first.stderr
        assert first.stderr == ""
        assert first.stdout == second.stdout
        report = load_report(first, validator)
        assert report["schema"] == "melkor.inspect.v1"
        assert report["valid"] is True
        assert report["source"] == {
            "path": canonical.name,
            "format": "ply",
            "profile": "ply:melkor-canonical-v1",
            "bytes": len(source_bytes),
        }
        assert str(root) not in first.stdout
        assert report["cloud"]["splats"] == 1
        assert report["cloud"]["sh_degree"] == 0
        assert report["cloud"]["bounds"] == {
            "min": [-1.0, 2.0, 3.0],
            "max": [-1.0, 2.0, 3.0],
        }
        assert report["container"] == {
            "encoding": "ascii",
            "declared_splats": 1,
            "antialiased": None,
        }
        assert report["losses"] == []
        assert report["validation"] == {
            "error_code": None,
            "errors": 0,
            "warnings": 0,
            "issues": [],
        }
        assert canonical.read_bytes() == source_bytes
        assert canonical.stat().st_mtime_ns == source_mtime

        human = run(binary, str(canonical))
        assert human.returncode == 0, human.stderr
        assert "Valid: yes" in human.stdout
        assert "Profile: ply:melkor-canonical-v1" in human.stdout

        with canonical.open("rb") as read_only_stdout:
            report_failure = subprocess.run(
                [str(binary), "inspect", str(canonical), "--json"],
                check=False,
                stdout=read_only_stdout,
                stderr=subprocess.PIPE,
                text=True,
                timeout=TIMEOUT_SECONDS,
            )
        assert report_failure.returncode == 5, report_failure.stderr
        assert "MK1904_REPORT_WRITE_FAILED" in report_failure.stderr

        lossy = root / "unknown-property.ply"
        lossy.write_text(canonical_ply(extra_property=True), encoding="utf-8")
        lossy_result = run(binary, str(lossy), "--json")
        assert lossy_result.returncode == 0, lossy_result.stderr
        lossy_report = load_report(lossy_result, validator)
        assert lossy_report["valid"] is True
        assert lossy_report["validation"]["errors"] == 0
        assert [item["code"] for item in lossy_report["losses"]] == [
            "LOSS_UNKNOWN_PROPERTY_DROPPED"
        ]
        assert lossy_report["losses"][0]["severity"] == "severe"
        lossy_strict = run(binary, str(lossy), "--json", "--strict")
        assert lossy_strict.returncode == 1, lossy_strict.stderr
        assert load_report(lossy_strict, validator) == lossy_report

        subnormal = root / "subnormal.ply"
        subnormal.write_text(canonical_ply(scale="1e-20"), encoding="utf-8")
        permissive = run(binary, str(subnormal), "--json")
        strict = run(binary, str(subnormal), "--json", "--strict")
        assert permissive.returncode == 0, permissive.stderr
        assert strict.returncode == 1, strict.stderr
        warning_report = load_report(strict, validator)
        assert warning_report["valid"] is True
        assert warning_report["validation"]["warnings"] == 1
        warning = warning_report["validation"]["issues"][0]
        assert warning == {
            "severity": "warning",
            "code": "scale_covariance_subnormal",
            "message": "Squared linear scale becomes subnormal in 32-bit covariance.",
            "count": 1,
            "first_index": 0,
            "path": None,
            "byte_offset": None,
            "context": {},
        }

        empty = root / "empty.ply"
        empty.write_text(canonical_ply(vertex_count=0), encoding="utf-8")
        empty_result = run(binary, str(empty), "--json")
        assert empty_result.returncode == 3, empty_result.stderr
        empty_report = load_report(empty_result, validator)
        assert empty_report["valid"] is False
        assert empty_report["cloud"]["splats"] == 0
        assert empty_report["cloud"]["bounds"] is None
        assert empty_report["validation"]["issues"][0]["code"] == "empty_cloud"

        invalid = root / "invalid-scale.ply"
        invalid.write_text(canonical_ply(scale="-1"), encoding="utf-8")
        invalid_result = run(binary, str(invalid), "--json")
        assert invalid_result.returncode == 3, invalid_result.stderr
        invalid_report = load_report(invalid_result, validator)
        assert invalid_report["valid"] is False
        assert invalid_report["cloud"] is None
        assert invalid_report["validation"]["error_code"] == "invalid_data"
        invalid_issue = invalid_report["validation"]["issues"][0]
        assert invalid_issue["code"] == "MK1505_NEGATIVE_SCALE"
        assert invalid_issue["path"] == "invalid-scale.ply"
        assert invalid_issue["context"] == {"field": "scale", "splat_index": 0}

        malformed = root / "malformed.ply"
        malformed.write_text(
            "ply\nformat ascii 1.0\nelement vertex 1\nproperty float16 x\nend_header\n0\n",
            encoding="utf-8",
        )
        malformed_result = run(binary, str(malformed), "--json")
        assert malformed_result.returncode == 3, malformed_result.stderr
        assert load_report(malformed_result, validator)["valid"] is False

        override_path = root / "malformed.data"
        override_path.write_bytes(malformed.read_bytes())
        override_result = run(binary, str(override_path), "--json", "--input-format", "ply")
        assert override_result.returncode == 3, override_result.stderr
        override_report = load_report(override_result, validator)
        assert override_report["source"]["format"] == "ply"

        missing = run(binary, str(root / "missing.ply"), "--json")
        assert missing.returncode == 5, missing.stderr
        missing_report = load_report(missing, validator)
        assert missing_report["source"]["path"] == "missing.ply"
        assert missing_report["validation"]["issues"][0]["code"] == "MK2301_INPUT_OPEN"

        unsupported = root / "asset.txt"
        unsupported.write_text("not an asset", encoding="utf-8")
        unsupported_result = run(binary, str(unsupported), "--json")
        assert unsupported_result.returncode == 4, unsupported_result.stderr
        unsupported_report = load_report(unsupported_result, validator)
        assert unsupported_report["valid"] is False
        assert unsupported_report["validation"]["error_code"] == "unsupported_feature"

        seed = REPO_ROOT / "fuzz" / "corpus" / "gltf_khr" / "minimal_degree1.glb"
        glb = run(binary, str(seed), "--json", "--limits-profile", "web")
        assert glb.returncode == 0, glb.stderr
        glb_report = load_report(glb, validator)
        assert glb_report["source"]["format"] == "glb"
        assert glb_report["source"]["profile"] == ("khr-gaussian-splatting-rc-63770cc")
        assert glb_report["cloud"]["splats"] == 3
        assert glb_report["cloud"]["sh_degree"] == 1
        assert {item["code"] for item in glb_report["losses"]} == {"LOSS_PROVENANCE_DROPPED"}

        override = run(
            binary,
            str(seed),
            "--json",
            "--source-color-space",
            "srgb_rec709_display",
        )
        assert override.returncode == 2, override.stderr
        override_report = load_report(override, validator)
        assert override_report["validation"]["issues"][0]["code"] == (
            "MK1712_GLTF_SEMANTIC_OVERRIDE"
        )

        wrong_profile = run(
            binary,
            str(seed),
            "--json",
            "--input-profile",
            "ply:melkor-canonical-v1",
        )
        assert wrong_profile.returncode == 2, wrong_profile.stderr
        wrong_profile_report = load_report(wrong_profile, validator)
        assert wrong_profile_report["validation"]["issues"][0]["code"] == (
            "MK1734_PROFILE_CONTAINER_MISMATCH"
        )

        future_spz = root / "future.spz"
        future_spz.write_bytes(
            gzip.compress(struct.pack("<IIIBBBB", 0x5053474E, 4, 1, 0, 12, 0, 0))
        )
        spz_arguments = (
            str(future_spz),
            "--json",
            "--source-unit-to-meter",
            "1",
            "--source-color-space",
            "srgb_rec709_display",
        )
        future_result = run(binary, *spz_arguments)
        assert future_result.returncode == 4, future_result.stderr
        future_report = load_report(future_result, validator)
        future_code = future_report["validation"]["issues"][0]["code"]
        if spz_enabled:
            assert future_code == "MK1327_SPZ_VERSION_UNSUPPORTED"
        else:
            assert future_code == "MK1711_SPZ_READ_FAILED"

        missing_semantics = run(binary, str(future_spz), "--json")
        assert missing_semantics.returncode == 2, missing_semantics.stderr
        assert (
            load_report(missing_semantics, validator)["validation"]["issues"][0]["code"]
            == "MK1709_SPZ_UNIT_REQUIRED"
        )

        symlink = root / "linked.ply"
        try:
            symlink.symlink_to(canonical)
        except OSError as error:
            print(f"SKIP: could not create a symbolic link: {error}")
        else:
            symlink_result = run(binary, str(symlink), "--json")
            assert symlink_result.returncode == 5, symlink_result.stderr
            symlink_report = load_report(symlink_result, validator)
            assert symlink_report["validation"]["issues"][0]["code"] == "MK2302_INPUT_TYPE"

        controlled_component = (
            "line-\u2028\u2029.ply" if os.name == "nt" else "line\ncontrol\x1b\u2028\u2029.ply"
        )
        controlled_name = root / controlled_component
        controlled_name.write_text(canonical_ply(), encoding="utf-8")
        controlled = run(binary, str(controlled_name))
        assert controlled.returncode == 0, controlled.stderr
        if os.name != "nt":
            assert "\\n" in controlled.stdout and "\\x1b" in controlled.stdout
        assert "\\u2028" in controlled.stdout and "\\u2029" in controlled.stdout
        assert "\x1b" not in controlled.stdout
        assert "\u2028" not in controlled.stdout and "\u2029" not in controlled.stdout

        if os.name != "nt":
            invalid_byte_name = Path(os.fsdecode(os.fsencode(str(root)) + b"/invalid-\xff.ply"))
            try:
                invalid_byte_name.write_text(canonical_ply(), encoding="utf-8")
            except OSError:
                print("SKIP: the file system rejects non-UTF-8 file names")
            else:
                invalid_byte = run(binary, str(invalid_byte_name))
                assert invalid_byte.returncode == 0, invalid_byte.stderr
                assert "\\xff" in invalid_byte.stdout

    print("Inspect CLI contract tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
