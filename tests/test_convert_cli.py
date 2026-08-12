#!/usr/bin/env python3
"""Contract tests for the explicit `melkor convert` command."""

from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from jsonschema import Draft202012Validator

TIMEOUT_SECONDS = 120
REPO_ROOT = Path(__file__).resolve().parent.parent


def run(binary: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), "convert", *arguments],
        check=False,
        capture_output=True,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )


def load_report(
    result: subprocess.CompletedProcess[str],
    validator: Draft202012Validator,
) -> dict:
    report = json.loads(result.stdout)
    validator.validate(report)
    return report


def unpack_glb(source: Path, json_path: Path, binary_path: Path) -> None:
    data = source.read_bytes()
    magic, version, declared_length = struct.unpack_from("<III", data)
    assert magic == 0x46546C67 and version == 2 and declared_length == len(data)

    offset = 12
    chunks: dict[int, bytes] = {}
    while offset < len(data):
        length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        chunks[chunk_type] = data[offset : offset + length]
        offset += length
    document = json.loads(chunks[0x4E4F534A].rstrip(b" \t\r\n\0"))
    binary = chunks[0x004E4942]
    document["buffers"][0]["uri"] = binary_path.name
    json_path.write_text(json.dumps(document, separators=(",", ":")), encoding="utf-8")
    binary_path.write_bytes(binary)


def main() -> int:
    if sys.flags.optimize:
        raise SystemExit("refusing to run under PYTHONOPTIMIZE: asserts would be stripped")
    if len(sys.argv) != 3 or sys.argv[2] not in {"ON", "OFF"}:
        raise SystemExit("usage: test_convert_cli.py /path/to/melkor <ON|OFF>")

    binary = Path(sys.argv[1]).resolve()
    spz_enabled = sys.argv[2] == "ON"
    seed = REPO_ROOT / "fuzz" / "corpus" / "gltf_khr" / "minimal_degree1.glb"
    schema = json.loads(
        (REPO_ROOT / "schemas" / "loss-report-v1.schema.json").read_text(
            encoding="utf-8"
        )
    )
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    assert seed.is_file(), f"missing seed GLB: {seed}"

    output_commands = (
        ["--version"],
        ["--help"],
        ["convert", "--help"],
        ["inspect", "--help"],
    )
    for arguments in output_commands:
        with seed.open("rb") as read_only_stdout:
            failed_help = subprocess.run(
                [str(binary), *arguments],
                check=False,
                stdout=read_only_stdout,
                stderr=subprocess.PIPE,
                text=True,
                timeout=TIMEOUT_SECONDS,
            )
        assert failed_help.returncode == 5, failed_help.stderr
        assert "MK1806_OUTPUT_WRITE_FAILED" in failed_help.stderr

    with tempfile.TemporaryDirectory(prefix="melkor-convert-") as directory:
        root = Path(directory)
        output_glb = root / "roundtrip.glb"

        converted = run(binary, str(seed), str(output_glb))
        assert converted.returncode == 0, converted.stderr
        assert converted.stderr == ""
        assert output_glb.read_bytes().startswith(b"glTF")
        report = load_report(converted, validator)
        assert report["input"] == {
            "format": "glb",
            "profile": "khr-gaussian-splatting-rc-63770cc",
        }
        assert report["output"] == {
            "format": "glb",
            "profile": "khr-gaussian-splatting-rc-63770cc",
        }
        assert report["approved_codes"] == []
        assert {item["code"] for item in report["items"]} == {
            "LOSS_PROVENANCE_DROPPED"
        }

        original = output_glb.read_bytes()
        refused = run(binary, str(seed), str(output_glb))
        assert refused.returncode == 5, refused.stderr
        assert "MK0505_OUTPUT_EXISTS" in refused.stderr
        assert "--force" in refused.stderr
        assert output_glb.read_bytes() == original

        forced = run(binary, str(seed), str(output_glb), "--force")
        assert forced.returncode == 0, forced.stderr
        assert output_glb.read_bytes() == original

        report_failure_output = root / "report-failure.glb"
        with seed.open("rb") as read_only_stdout:
            report_failure = subprocess.run(
                [
                    str(binary),
                    "convert",
                    str(seed),
                    str(report_failure_output),
                ],
                check=False,
                stdout=read_only_stdout,
                stderr=subprocess.PIPE,
                text=True,
                timeout=TIMEOUT_SECONDS,
            )
        assert report_failure.returncode == 5, report_failure.stderr
        assert "MK1726_REPORT_WRITE_FAILED" in report_failure.stderr
        assert not report_failure_output.exists()

        if os.name == "posix":
            broken_pipe_output = root / "broken-pipe.glb"
            read_fd, write_fd = os.pipe()
            os.close(read_fd)
            try:
                broken_pipe = subprocess.run(
                    [
                        str(binary),
                        "convert",
                        str(seed),
                        str(broken_pipe_output),
                    ],
                    check=False,
                    stdout=write_fd,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=TIMEOUT_SECONDS,
                )
            finally:
                os.close(write_fd)
            assert broken_pipe.returncode == 5, broken_pipe.stderr
            assert "MK1726_REPORT_WRITE_FAILED" in broken_pipe.stderr
            assert not broken_pipe_output.exists()
            assert list(root.glob(".melkor-*.tmp")) == []

        preserved_output = root / "report-failure-existing.glb"
        preserved_output.write_bytes(b"existing output")
        with seed.open("rb") as read_only_stdout:
            overwrite_report_failure = subprocess.run(
                [
                    str(binary),
                    "convert",
                    str(seed),
                    str(preserved_output),
                    "--force",
                ],
                check=False,
                stdout=read_only_stdout,
                stderr=subprocess.PIPE,
                text=True,
                timeout=TIMEOUT_SECONDS,
            )
        assert overwrite_report_failure.returncode == 5, (
            overwrite_report_failure.stderr
        )
        assert "MK1726_REPORT_WRITE_FAILED" in overwrite_report_failure.stderr
        assert preserved_output.read_bytes() == b"existing output"

        canonical_ply = root / "canonical.ply"
        to_ply = run(binary, str(seed), str(canonical_ply))
        assert to_ply.returncode == 0, to_ply.stderr
        assert load_report(to_ply, validator)["output"]["profile"] == (
            "ply:melkor-canonical-v1"
        )
        inspected_ply = subprocess.run(
            [str(binary), "inspect", str(canonical_ply), "--json"],
            check=False,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
        )
        assert inspected_ply.returncode == 0, inspected_ply.stderr
        ply_report = json.loads(inspected_ply.stdout)
        assert ply_report["valid"] is True
        assert ply_report["cloud"]["splats"] == 3
        assert ply_report["cloud"]["sh_degree"] == 1

        ply_roundtrip = root / "canonical-roundtrip.glb"
        from_ply = run(binary, str(canonical_ply), str(ply_roundtrip))
        assert from_ply.returncode == 0, from_ply.stderr
        assert load_report(from_ply, validator)["items"] == []

        external_gltf = root / "external.gltf"
        external_bin = root / "external.bin"
        unpack_glb(seed, external_gltf, external_bin)
        external_output = root / "external.glb"
        external = run(binary, str(external_gltf), str(external_output))
        assert external.returncode == 0, external.stderr
        assert load_report(external, validator)["input"]["format"] == "gltf"

        same_file = run(binary, str(canonical_ply), str(canonical_ply), "--force")
        assert same_file.returncode == 2, same_file.stderr
        assert "MK1716_INPUT_OUTPUT_SAME" in same_file.stderr

        wrong_profile = run(
            binary,
            str(seed),
            str(root / "wrong-profile.glb"),
            "--output-profile",
            "ply:melkor-canonical-v1",
        )
        assert wrong_profile.returncode == 2, wrong_profile.stderr
        assert "MK1734_PROFILE_CONTAINER_MISMATCH" in wrong_profile.stderr

        source_override = run(
            binary,
            str(seed),
            str(root / "override.glb"),
            "--source-unit-to-meter",
            "1",
        )
        assert source_override.returncode == 2, source_override.stderr
        assert "MK1712_GLTF_SEMANTIC_OVERRIDE" in source_override.stderr

        json_output = root / "unsupported.gltf"
        unsupported_output = run(binary, str(seed), str(json_output))
        assert unsupported_output.returncode == 4, unsupported_output.stderr
        assert "MK1718_GLTF_JSON_WRITE_UNSUPPORTED" in unsupported_output.stderr
        assert not json_output.exists()

        suffix_conflict = run(
            binary,
            str(seed),
            str(root / "conflict.ply"),
            "--output-format",
            "glb",
        )
        assert suffix_conflict.returncode == 2, suffix_conflict.stderr
        assert "MK1728_OUTPUT_SUFFIX_CONFLICT" in suffix_conflict.stderr

        missing = run(binary, str(root / "missing.glb"), str(root / "missing-out.glb"))
        assert missing.returncode == 5, missing.stderr
        assert "MK2301_INPUT_OPEN" in missing.stderr
        assert not (root / "missing-out.glb").exists()

        malformed = root / "malformed.glb"
        malformed.write_bytes(b"not a GLB")
        invalid = run(binary, str(malformed), str(root / "invalid.glb"))
        assert invalid.returncode == 3, invalid.stderr
        assert "MK2101_GLB_TRUNCATED_HEADER" in invalid.stderr
        assert not (root / "invalid.glb").exists()

        controlled_missing = root / "missing\n\x1b[31m.glb"
        controlled = run(
            binary,
            str(controlled_missing),
            str(root / "controlled-output.glb"),
        )
        assert controlled.returncode == 5, controlled.stderr
        assert "\\n" in controlled.stderr and "\\x1b" in controlled.stderr
        assert "\x1b" not in controlled.stderr

        if spz_enabled:
            output_spz = root / "output.spz"
            missing_antialias = run(binary, str(seed), str(output_spz))
            assert missing_antialias.returncode == 2, missing_antialias.stderr
            assert "MK1720_SPZ_ANTIALIASING_REQUIRED" in missing_antialias.stderr

            blocked_color = run(
                binary,
                str(seed),
                str(output_spz),
                "--output-antialiased",
                "false",
            )
            assert blocked_color.returncode == 4, blocked_color.stderr
            assert "--allow-loss LOSS_COLOR_SPACE_METADATA_DROPPED" in (
                blocked_color.stderr
            )
            assert not output_spz.exists()

            blocked_unit = run(
                binary,
                str(seed),
                str(output_spz),
                "--output-antialiased",
                "false",
                "--allow-loss",
                "LOSS_COLOR_SPACE_METADATA_DROPPED",
            )
            assert blocked_unit.returncode == 4, blocked_unit.stderr
            assert "--allow-loss LOSS_COORDINATE_METADATA_DROPPED" in (
                blocked_unit.stderr
            )
            assert not output_spz.exists()

            approved = run(
                binary,
                str(seed),
                str(output_spz),
                "--output-antialiased",
                "false",
                "--allow-loss",
                "LOSS_COORDINATE_METADATA_DROPPED",
                "--allow-loss",
                "LOSS_COLOR_SPACE_METADATA_DROPPED",
            )
            assert approved.returncode == 0, approved.stderr
            approved_report = load_report(approved, validator)
            assert approved_report["approved_codes"] == [
                "LOSS_COLOR_SPACE_METADATA_DROPPED",
                "LOSS_COORDINATE_METADATA_DROPPED",
            ]
            assert {item["code"] for item in approved_report["items"]} >= {
                "LOSS_COLOR_SPACE_METADATA_DROPPED",
                "LOSS_COORDINATE_METADATA_DROPPED",
                "LOSS_QUANTIZATION_APPLIED",
            }
            assert output_spz.read_bytes().startswith(b"\x1f\x8b")

    print("Convert CLI contract tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
