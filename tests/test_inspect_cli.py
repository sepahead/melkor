#!/usr/bin/env python3
"""Contract tests for the deterministic, backend-free `melkor inspect` command."""

from __future__ import annotations

import json
import base64
import gzip
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def run(binary: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), "inspect", *args],
        check=False,
        capture_output=True,
        text=True,
    )


def run_command(binary: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), *args],
        check=False,
        capture_output=True,
        text=True,
    )


def write_canonical(path: Path) -> None:
    path.write_text(
        """ply
format ascii 1.0
element vertex 1
property float x
property float y
property float z
property float f_dc_0
property float f_dc_1
property float f_dc_2
property float opacity
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
end_header
-1 2 3 0.1 0.2 0.3 2 -4.6051702 -4.6051702 -4.6051702 1 0 0 0
""",
        encoding="utf-8",
    )


def write_rgb_points(path: Path) -> None:
    path.write_text(
        """ply
format ascii 1.0
element vertex 1
property float x
property float y
property float z
property uchar red
property uchar green
property uchar blue
end_header
0 0 0 255 128 0
""",
        encoding="utf-8",
    )


def mesh_gltf(*, version: str = "2.0", min_version: str | None = None,
              required: list[str] | None = None, used: list[str] | None = None) -> dict:
    asset = {"version": version}
    if min_version is not None:
        asset["minVersion"] = min_version
    document = {
        "asset": asset,
        "buffers": [{
            "uri": "data:application/octet-stream;base64," +
                   base64.b64encode(struct.pack("<fff", 1.0, 2.0, 3.0)).decode("ascii"),
            "byteLength": 12,
        }],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 12}],
        "accessors": [{
            "bufferView": 0, "componentType": 5126, "count": 1, "type": "VEC3",
        }],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "nodes": [{"mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    if required is not None:
        document["extensionsRequired"] = required
    if used is not None:
        document["extensionsUsed"] = used
    return document


def write_glb(path: Path) -> None:
    document = mesh_gltf()
    document["buffers"] = [{"byteLength": 12}]
    json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * ((-len(json_chunk)) % 4)
    binary_chunk = struct.pack("<fff", 1.0, 2.0, 3.0)
    length = 12 + 8 + len(json_chunk) + 8 + len(binary_chunk)
    path.write_bytes(
        struct.pack("<III", 0x46546C67, 2, length)
        + struct.pack("<II", len(json_chunk), 0x4E4F534A)
        + json_chunk
        + struct.pack("<II", len(binary_chunk), 0x004E4942)
        + binary_chunk
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_inspect_cli.py /path/to/melkor")
    binary = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="melkor-inspect-") as directory:
        root = Path(directory)
        canonical = root / "canonical.ply"
        write_canonical(canonical)
        before_bytes = canonical.read_bytes()
        before_mtime = canonical.stat().st_mtime_ns

        first = run(binary, str(canonical), "--json")
        second = run(binary, str(canonical), "--json")
        assert first.returncode == 0, first.stderr
        assert second.returncode == 0, second.stderr
        assert first.stdout == second.stdout, "inspect JSON must be byte-deterministic"
        assert "Backend" not in first.stdout and "acceleration" not in first.stdout
        report = json.loads(first.stdout)
        assert report["schema"] == "melkor.inspect.v1"
        assert report["valid"] is True
        assert report["source"]["format"] == "ply"
        assert report["source"]["bytes"] == len(before_bytes)
        assert report["container"]["encoding"] == "ascii"
        assert report["container"]["declared_splats"] == 1
        assert report["cloud"]["splats"] == 1
        assert report["cloud"]["bounds"] == {"min": [-1, 2, 3], "max": [-1, 2, 3]}
        assert report["validation"] == {"errors": 0, "warnings": 0, "issues": []}
        assert canonical.read_bytes() == before_bytes
        assert canonical.stat().st_mtime_ns == before_mtime

        strict_valid = run(binary, str(canonical), "--json", "--strict")
        assert strict_valid.returncode == 0, strict_valid.stderr

        uppercase_glb = root / "scene.GLB"
        write_glb(uppercase_glb)
        glb_result = run(binary, str(uppercase_glb), "--json")
        assert glb_result.returncode == 0, glb_result.stderr
        glb_report = json.loads(glb_result.stdout)
        assert glb_report["source"]["format"] == "glb"
        assert glb_report["source"]["kind"] == "mesh_vertices"
        assert glb_report["cloud"]["splats"] == 1

        invalid_color = mesh_gltf()
        invalid_color["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"] = 99
        invalid_normal = mesh_gltf()
        invalid_normal["meshes"][0]["primitives"][0]["attributes"]["NORMAL"] = 99
        cyclic_nodes = mesh_gltf()
        cyclic_nodes["nodes"] = [
            {"mesh": 0, "children": [1]},
            {"children": [0]},
        ]
        invalid_child = mesh_gltf()
        invalid_child["nodes"][0]["children"] = [99]
        invalid_mesh = mesh_gltf()
        invalid_mesh["nodes"][0]["mesh"] = 99

        invalid_gltf_contracts = {
            "version-3.gltf": mesh_gltf(version="3.0"),
            "min-version-2-1.gltf": mesh_gltf(min_version="2.1"),
            "required-unknown.gltf": mesh_gltf(required=["EXT_never"]),
            "gaussian-extension.gltf": mesh_gltf(
                used=["KHR_gaussian_splatting"],
                required=["KHR_gaussian_splatting"],
            ),
            "invalid-color.gltf": invalid_color,
            "invalid-normal.gltf": invalid_normal,
            "cyclic-nodes.gltf": cyclic_nodes,
            "invalid-child.gltf": invalid_child,
            "invalid-mesh.gltf": invalid_mesh,
        }
        for name, document in invalid_gltf_contracts.items():
            gltf_path = root / name
            gltf_path.write_text(json.dumps(document), encoding="utf-8")
            contract_result = run(binary, str(gltf_path), "--json")
            assert contract_result.returncode == 1, name
            contract_report = json.loads(contract_result.stdout)
            assert contract_report["valid"] is False, name
            assert contract_report["validation"]["issues"][0]["code"] == "read_error"

        # TinyGLTF preserves sparse metadata even when an accessor has a base
        # bufferView. Both converters must reject rather than read only the
        # zero-valued base and silently discard the (1, 2, 3) sparse override.
        sparse_bytes = (
            struct.pack("<fff", 0.0, 0.0, 0.0)
            + bytes([0])
            + bytes(3)
            + struct.pack("<fff", 1.0, 2.0, 3.0)
        )
        sparse_document = mesh_gltf()
        sparse_document["buffers"] = [{
            "uri": "data:application/octet-stream;base64,"
            + base64.b64encode(sparse_bytes).decode("ascii"),
            "byteLength": len(sparse_bytes),
        }]
        sparse_document["bufferViews"] = [
            {"buffer": 0, "byteOffset": 0, "byteLength": 12},
            {"buffer": 0, "byteOffset": 12, "byteLength": 1},
            {"buffer": 0, "byteOffset": 16, "byteLength": 12},
        ]
        sparse_document["accessors"] = [{
            "bufferView": 0,
            "componentType": 5126,
            "count": 1,
            "type": "VEC3",
            "sparse": {
                "count": 1,
                "indices": {"bufferView": 1, "componentType": 5121},
                "values": {"bufferView": 2},
            },
        }]
        sparse_path = root / "sparse-position.gltf"
        sparse_path.write_text(json.dumps(sparse_document), encoding="utf-8")

        sparse_inspect = run(binary, str(sparse_path), "--json")
        assert sparse_inspect.returncode == 1
        assert json.loads(sparse_inspect.stdout)["valid"] is False
        for mode in ("--basic", "--enhanced"):
            sparse_output = root / f"sparse-{mode[2:]}.ply"
            sparse_convert = run_command(
                binary, str(sparse_path), str(sparse_output), mode, "--no-gpu"
            )
            assert sparse_convert.returncode == 1, mode
            assert not sparse_output.exists(), mode

        attacker = root / "attacker"
        attacker.mkdir()
        secret = root / "secret.bin"
        secret.write_bytes(struct.pack("<fff", 123.5, 456.25, -7.75))
        traversal_document = mesh_gltf()
        traversal_document["buffers"] = [{"uri": "../secret.bin", "byteLength": 12}]
        traversal_path = attacker / "traversal.gltf"
        traversal_path.write_text(json.dumps(traversal_document), encoding="utf-8")
        traversal_result = run(binary, str(traversal_path), "--json")
        assert traversal_result.returncode == 1
        traversal_report = json.loads(traversal_result.stdout)
        assert traversal_report["valid"] is False
        assert "123.5" not in traversal_result.stdout

        symlink_path = attacker / "linked.bin"
        os.symlink(secret, symlink_path)
        symlink_document = mesh_gltf()
        symlink_document["buffers"] = [{"uri": "linked.bin", "byteLength": 12}]
        symlink_gltf = attacker / "symlink.gltf"
        symlink_gltf.write_text(json.dumps(symlink_document), encoding="utf-8")
        symlink_result = run(binary, str(symlink_gltf), "--json")
        assert symlink_result.returncode == 1
        assert json.loads(symlink_result.stdout)["valid"] is False

        partial_document = mesh_gltf()
        partial_document["meshes"][0]["primitives"].append(
            {"attributes": {"POSITION": 99}}
        )
        partial_path = root / "partial.gltf"
        partial_path.write_text(json.dumps(partial_document), encoding="utf-8")
        partial_result = run(binary, str(partial_path), "--json")
        assert partial_result.returncode == 1
        assert json.loads(partial_result.stdout)["valid"] is False

        nonfinite_document = mesh_gltf()
        nonfinite_bytes = struct.pack("<ffffff", 1.0, 2.0, 3.0, float("nan"), 4.0, 5.0)
        nonfinite_document["buffers"] = [{
            "uri": "data:application/octet-stream;base64," +
                   base64.b64encode(nonfinite_bytes).decode("ascii"),
            "byteLength": len(nonfinite_bytes),
        }]
        nonfinite_document["bufferViews"][0]["byteLength"] = len(nonfinite_bytes)
        nonfinite_document["accessors"][0]["count"] = 2
        nonfinite_path = root / "nonfinite-position.gltf"
        nonfinite_path.write_text(json.dumps(nonfinite_document), encoding="utf-8")
        nonfinite_result = run(binary, str(nonfinite_path), "--json")
        assert nonfinite_result.returncode == 1
        assert json.loads(nonfinite_result.stdout)["valid"] is False

        empty = root / "empty.ply"
        empty.write_text(
            "ply\nformat ascii 1.0\nelement vertex 0\nend_header\n",
            encoding="utf-8",
        )
        empty_result = run(binary, str(empty), "--json")
        assert empty_result.returncode == 1
        empty_report = json.loads(empty_result.stdout)
        assert empty_report["valid"] is False
        assert empty_report["cloud"]["bounds"] is None
        assert empty_report["cloud"]["fields"]["position"] == "missing"
        assert empty_report["validation"]["issues"][0]["code"] == "empty_cloud"

        points = root / "points.ply"
        write_rgb_points(points)
        permissive = run(binary, str(points), "--json")
        strict = run(binary, str(points), "--json", "--strict")
        assert permissive.returncode == 0, permissive.stderr
        assert strict.returncode == 1
        point_report = json.loads(permissive.stdout)
        assert point_report["valid"] is True
        assert point_report["cloud"]["fields"]["color"] == "converted_rgb"
        warning_codes = {issue["code"] for issue in point_report["validation"]["issues"]}
        assert warning_codes == {
            "rgb_color_converted",
            "defaulted_opacity",
            "defaulted_scale",
            "defaulted_rotation",
        }

        missing = run(binary, str(root / "missing.ply"), "--json")
        assert missing.returncode == 1
        missing_report = json.loads(missing.stdout)
        assert missing_report["valid"] is False
        assert missing_report["cloud"] is None
        assert missing_report["validation"]["issues"][0]["code"] == "input_not_found"

        invalid_utf8 = root / "invalid-utf8.gltf"
        invalid_utf8.write_bytes(b'{"asset":{"version":"2.0"},"\xff":1}')
        invalid_utf8_result = run(binary, str(invalid_utf8), "--json")
        assert invalid_utf8_result.returncode == 1
        invalid_utf8_report = json.loads(invalid_utf8_result.stdout)
        assert invalid_utf8_report["valid"] is False
        assert invalid_utf8_report["validation"]["issues"][0]["code"] == "read_error"

        unsupported_spz = root / "future.spz"
        unsupported_spz.write_bytes(
            gzip.compress(struct.pack("<IIIBBBB", 0x5053474E, 4, 1, 0, 12, 0, 0))
        )
        unsupported_spz_result = run(binary, str(unsupported_spz), "--json")
        assert unsupported_spz_result.returncode == 1
        unsupported_spz_report = json.loads(unsupported_spz_result.stdout)
        assert unsupported_spz_report["validation"]["issues"][0]["code"] == (
            "unsupported_spz_version"
        )

        invalid_fractional_bits = root / "invalid-fractional-bits.spz"
        invalid_fractional_bits.write_bytes(
            gzip.compress(
                struct.pack("<IIIBBBB", 0x5053474E, 3, 1, 0, 255, 0, 0)
                + bytes(20)
            )
        )
        invalid_fractional_inspect = run(
            binary, str(invalid_fractional_bits), "--json"
        )
        assert invalid_fractional_inspect.returncode == 1
        invalid_fractional_report = json.loads(invalid_fractional_inspect.stdout)
        assert invalid_fractional_report["valid"] is False
        assert invalid_fractional_report["validation"]["issues"][0]["code"] == (
            "read_error"
        )
        invalid_fractional_output = root / "invalid-fractional-bits.ply"
        invalid_fractional_convert = run_command(
            binary,
            str(invalid_fractional_bits),
            str(invalid_fractional_output),
            "--no-gpu",
        )
        assert invalid_fractional_convert.returncode == 1
        assert not invalid_fractional_output.exists()

        controlled_name = root / "line\ncontrol\x1b.ply"
        write_canonical(controlled_name)
        controlled_result = run(binary, str(controlled_name))
        assert controlled_result.returncode == 0
        assert "\\n" in controlled_result.stdout
        assert "\\x1b" in controlled_result.stdout
        assert "\x1b" not in controlled_result.stdout

        invalid_byte_name = Path(os.fsdecode(
            os.fsencode(str(root)) + b"/invalid-\xff.ply"
        ))
        try:
            write_canonical(invalid_byte_name)
        except OSError:
            # APFS rejects non-UTF-8 path components; Linux filesystems permit
            # them and exercise the filename branch below.
            pass
        else:
            invalid_byte_result = run(binary, str(invalid_byte_name))
            assert invalid_byte_result.returncode == 0
            assert "\\xff" in invalid_byte_result.stdout

        c1_name = root / "c1-\u009b.ply"
        write_canonical(c1_name)
        c1_result = run(binary, str(c1_name))
        assert c1_result.returncode == 0
        assert "\\u009b" in c1_result.stdout
        assert "\u009b" not in c1_result.stdout

        hostile_extension = root / "format.ply\n\x1b[31m"
        write_canonical(hostile_extension)
        hostile_extension_result = run(binary, str(hostile_extension))
        assert hostile_extension_result.returncode == 1
        assert "\\n" in hostile_extension_result.stdout
        assert "\\x1b" in hostile_extension_result.stdout
        assert "\x1b" not in hostile_extension_result.stdout

        missing_controlled = root / "missing\n\x1b[31m.gltf"
        missing_controlled_result = run_command(
            binary, str(missing_controlled), str(root / "unused.ply"), "--no-gpu"
        )
        assert missing_controlled_result.returncode == 1
        assert "\\n" in missing_controlled_result.stderr
        assert "\\x1b" in missing_controlled_result.stderr
        assert "\x1b" not in missing_controlled_result.stderr

        hostile_reader_error = root / "hostile-reader-error.ply"
        hostile_reader_error.write_bytes(
            b"ply\nformat ascii 1.0\nelement vertex 1\n"
            b"property float\xff x\nproperty float y\nproperty float z\n"
            b"end_header\n0 0 0\n"
        )
        hostile_reader_result = run_command(
            binary, str(hostile_reader_error), str(root / "unused.ply"), "--no-gpu"
        )
        assert hostile_reader_result.returncode == 1
        assert "\\xff" in hostile_reader_result.stderr

        bad_type = root / "bad-type.ply"
        bad_type.write_text(
            "ply\nformat ascii 1.0\nelement vertex 1\nproperty float16 x\n"
            "property float y\nproperty float z\nend_header\n0 0 0\n",
            encoding="utf-8",
        )
        rejected = run(binary, str(bad_type), "--json")
        assert rejected.returncode == 1
        assert json.loads(rejected.stdout)["validation"]["issues"][0]["code"] == "read_error"

        malformed_headers = {
            "missing-magic.ply": (
                "not-ply\nformat ascii 1.0\nelement vertex 0\nend_header\n"
            ),
            "unknown-format.ply": (
                "ply\nformat binary_native_endian 1.0\nelement vertex 0\nend_header\n"
            ),
            "negative-count.ply": (
                "ply\nformat ascii 1.0\nelement vertex -1\nend_header\n"
            ),
            "malformed-face-count.ply": (
                "ply\nformat ascii 1.0\nelement face nope\n"
                "element vertex 0\nend_header\n"
            ),
            "duplicate-face-element.ply": (
                "ply\nformat ascii 1.0\nelement face 0\nelement face 0\n"
                "element vertex 0\nend_header\n"
            ),
            "duplicate-vertex-element.ply": (
                "ply\nformat ascii 1.0\nelement vertex 0\nelement vertex 0\n"
                "end_header\n"
            ),
            "duplicate-position-property.ply": (
                "ply\nformat ascii 1.0\nelement vertex 1\n"
                "property float x\nproperty float x\nproperty float y\n"
                "property float z\nend_header\n0 0 0 0\n"
            ),
            "vertex-list.ply": (
                "ply\nformat ascii 1.0\nelement vertex 1\n"
                "property list uchar float weights\nend_header\n0\n"
            ),
        }
        for name, contents in malformed_headers.items():
            malformed = root / name
            malformed.write_text(contents, encoding="utf-8")
            failed = run(binary, str(malformed), "--json")
            assert failed.returncode == 1, name
            malformed_report = json.loads(failed.stdout)
            assert malformed_report["valid"] is False, name
            assert malformed_report["validation"]["issues"][0]["code"] == "read_error"

        malformed_ascii_records = {
            "face-before-vertex.ply": (
                "ply\nformat ascii 1.0\nelement face 1\n"
                "property list uchar int vertex_indices\n"
                "element vertex 1\nproperty float x\nproperty float y\n"
                "property float z\nend_header\n3 0 1 2\n0 0 0\n"
            ),
            "vertex-row-spill.ply": (
                "ply\nformat ascii 1.0\nelement vertex 2\n"
                "property float x\nproperty float y\nproperty float z\n"
                "end_header\n0 0\n0 1 1 1\n"
            ),
            "vertex-extra-field.ply": (
                "ply\nformat ascii 1.0\nelement vertex 1\n"
                "property float x\nproperty float y\nproperty float z\n"
                "end_header\n0 0 0 99\n"
            ),
        }
        for name, contents in malformed_ascii_records.items():
            malformed_record = root / name
            malformed_record.write_text(contents, encoding="utf-8")
            malformed_record_result = run(binary, str(malformed_record), "--json")
            assert malformed_record_result.returncode == 1, name
            assert json.loads(malformed_record_result.stdout)["valid"] is False, name

        repeated_sh = root / "duplicate-sh.ply"
        repeated_sh.write_text(
            "ply\nformat ascii 1.0\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\n"
            + "property float f_rest_0\n" * 9
            + "end_header\n0 0 0 "
            + "0 " * 9
            + "\n",
            encoding="utf-8",
        )
        repeated_result = run(binary, str(repeated_sh), "--json")
        assert repeated_result.returncode == 1
        repeated_report = json.loads(repeated_result.stdout)
        assert repeated_report["validation"]["issues"][0]["code"] == "read_error"

        for index, bad_color in enumerate(("300", "-1", "1.5")):
            invalid_rgb = root / f"invalid-rgb-{index}.ply"
            invalid_rgb.write_text(
                "ply\nformat ascii 1.0\nelement vertex 1\n"
                "property float x\nproperty float y\nproperty float z\n"
                "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                f"end_header\n0 0 0 {bad_color} 0 0\n",
                encoding="utf-8",
            )
            invalid_rgb_result = run(binary, str(invalid_rgb), "--json")
            assert invalid_rgb_result.returncode == 1
            assert json.loads(invalid_rgb_result.stdout)["valid"] is False

        usage = run(binary, "--json")
        assert usage.returncode == 2
        assert usage.stdout == ""

    print("Inspect CLI contract tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
