#!/usr/bin/env python3
"""Contract tests for `melkor convert` (the canonical GLB KHR_gaussian_splatting path)."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 120


def run(*argv: str) -> "subprocess.CompletedProcess[str]":
    return subprocess.run(list(argv), capture_output=True, text=True, timeout=TIMEOUT_SECONDS)


def main() -> None:
    if sys.flags.optimize:
        raise SystemExit("refusing to run under PYTHONOPTIMIZE: asserts would be stripped")
    if len(sys.argv) < 2:
        raise SystemExit("usage: test_convert_cli.py /path/to/melkor")
    binary = Path(sys.argv[1])
    seed = Path(__file__).resolve().parent.parent / "fuzz" / "corpus" / "gltf_khr" / \
        "minimal_degree1.glb"
    assert seed.is_file(), f"missing seed corpus GLB: {seed}"

    with tempfile.TemporaryDirectory(prefix="melkor-convert-") as directory:
        root = Path(directory)
        out_glb = root / "out.glb"

        # Round-trip: read the KHR GLB into the canonical model and write it back out.
        result = run(str(binary), "convert", str(seed), str(out_glb))
        assert result.returncode == 0, result.stderr
        assert out_glb.is_file(), "convert did not produce an output file"

        # The output must read back as the same splat cloud through inspect.
        inspected = run(str(binary), "inspect", str(out_glb), "--json")
        assert inspected.returncode == 0, inspected.stderr
        report = json.loads(inspected.stdout)
        assert report["source"]["kind"] == "splat_data", report["source"]
        assert report["cloud"]["splats"] == 3, report["cloud"]
        assert report["cloud"]["sh_degree"] == 1, report["cloud"]

        # The historical positional GLB path is only a mesh-vertex sampler. It must not treat a
        # KHR splat primitive as a mesh and silently discard scale, rotation, opacity, and SH.
        positional = run(str(binary), str(seed), str(root / "positional.ply"))
        assert positional.returncode != 0, positional.stdout
        assert "KHR_gaussian_splatting" in positional.stderr, positional.stderr
        assert not (root / "positional.ply").exists()

        # Cross-format conversion is not yet supported and must be refused cleanly (exit 2).
        cross = run(str(binary), "convert", str(seed), str(root / "out.ply"))
        assert cross.returncode == 2, cross.stderr

        # The command writes a GLB container. It must not put GLB bytes below a .gltf name.
        mislabeled_output = root / "out.gltf"
        wrong_output = run(str(binary), "convert", str(seed), str(mislabeled_output))
        assert wrong_output.returncode == 2, wrong_output.stderr
        assert not mislabeled_output.exists()

        # A JSON .gltf input needs URI resolution that this GLB-only command does not provide.
        mislabeled_input = run(str(binary), "convert", str(root / "in.gltf"), str(out_glb))
        assert mislabeled_input.returncode == 2, mislabeled_input.stderr

        # A missing input is a clean failure, not a crash.
        missing = run(str(binary), "convert", str(root / "nope.glb"), str(out_glb))
        # A positive exit code excludes a signal death (negative returncode from subprocess).
        assert missing.returncode > 0, missing.returncode
        assert missing.stderr.strip(), "missing-input failure must explain itself on stderr"

        # Wrong argument count shows usage (exit 2).
        usage = run(str(binary), "convert", str(seed))
        assert usage.returncode == 2, usage.stderr

    print("Convert CLI contract tests passed")


if __name__ == "__main__":
    main()
