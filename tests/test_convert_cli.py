#!/usr/bin/env python3
"""Contract tests for `melkor convert` (the canonical GLB KHR_gaussian_splatting path)."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
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
        result = subprocess.run([str(binary), "convert", str(seed), str(out_glb)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert out_glb.is_file(), "convert did not produce an output file"

        # The output must read back as the same splat cloud through inspect.
        inspected = subprocess.run([str(binary), "inspect", str(out_glb), "--json"],
                                   capture_output=True, text=True)
        assert inspected.returncode == 0, inspected.stderr
        report = json.loads(inspected.stdout)
        assert report["source"]["kind"] == "gaussian_cloud", report["source"]
        assert report["cloud"]["splats"] == 3, report["cloud"]
        assert report["cloud"]["sh_degree"] == 1, report["cloud"]

        # Cross-format conversion is not yet supported and must be refused cleanly (exit 2).
        cross = subprocess.run([str(binary), "convert", str(seed), str(root / "out.ply")],
                               capture_output=True, text=True)
        assert cross.returncode == 2, cross.stderr

        # A missing input is a clean failure, not a crash.
        missing = subprocess.run([str(binary), "convert", str(root / "nope.glb"), str(out_glb)],
                                 capture_output=True, text=True)
        assert missing.returncode != 0

        # Wrong argument count shows usage (exit 2).
        usage = subprocess.run([str(binary), "convert", str(seed)], capture_output=True, text=True)
        assert usage.returncode == 2, usage.stderr

    print("Convert CLI contract tests passed")


if __name__ == "__main__":
    main()
