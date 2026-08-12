#!/usr/bin/env python3
"""Test the model-direct Gaussians extraction path in inference.py.

These tests do NOT require DA3 weights, GPU, or torch. They build a synthetic
Gaussians-shaped object and validate that DA3GaussianGenerator.gaussians_from_prediction()
correctly maps the model's output to the dict format that save_ply() consumes,
with field transforms that match ByteDance's export_ply() in
Depth Anything 3's src/depth_anything_3/utils/gsply_helpers.py.
DA3 is an external adapter and is no longer vendored; see docs/adapters/index.md.

Critical correctness property: after save_ply's rgb->SH-DC conversion, the
final PLY f_dc must equal the model's original DC band (no double-conversion
drift).

Run: python3 tests/test_gaussians_from_prediction.py
Exit code 0 = pass, nonzero = fail.
"""

from __future__ import annotations

import importlib.util
import re
import struct
import sys
import types
from pathlib import Path
from types import SimpleNamespace

if sys.flags.optimize:
    raise SystemExit("refusing to run under PYTHONOPTIMIZE: asserts would be stripped")

try:
    import numpy as np
except ImportError:
    raise SystemExit("numpy is required for this test: pip install numpy") from None

REPO_ROOT = Path(__file__).resolve().parent.parent


class _TensorShim:
    """Minimal tensor shim matching the Gaussians dataclass access pattern.

    The real Gaussians fields are torch tensors; inference.py calls
    `.detach().cpu().reshape(...).contiguous().numpy()` on them. This shim
    mirrors that chain over numpy so we can test the extraction logic without
    importing torch.
    """

    def __init__(self, arr: np.ndarray) -> None:
        self._arr = np.asarray(arr)

    def detach(self) -> "_TensorShim":
        return _TensorShim(self._arr)

    def cpu(self) -> "_TensorShim":
        return _TensorShim(self._arr)

    def reshape(self, *shape: int) -> "_TensorShim":
        return _TensorShim(self._arr.reshape(*shape))

    def contiguous(self) -> "_TensorShim":
        return _TensorShim(np.ascontiguousarray(self._arr))

    def numpy(self) -> np.ndarray:
        return np.asarray(self._arr)

    def __getitem__(self, idx):
        r = self._arr[idx]
        return _TensorShim(r) if isinstance(r, np.ndarray) else r


def _load_inference_module():
    """Load tools/da3/inference.py with torch/PIL/tqdm stubbed out."""
    # Stub torch (heavy, GPU-dependent) with just what the module top-level needs.
    fake_torch = types.ModuleType("torch")
    fake_torch.no_grad = lambda: type(
        "ctx", (), {"__enter__": lambda s: None, "__exit__": lambda *a: None}
    )()
    fake_torch.cuda = SimpleNamespace(is_available=lambda: False, is_bf16_supported=lambda: False)
    fake_torch.float16 = "fp16"
    fake_torch.float32 = "fp32"
    sys.modules["torch"] = fake_torch
    for mod in ("PIL", "PIL.Image", "tqdm"):
        sys.modules.setdefault(mod, types.ModuleType(mod))
    sys.modules["tqdm"].tqdm = lambda x, **k: x  # type: ignore[attr-defined]

    spec = importlib.util.spec_from_file_location(
        "da3_infer", REPO_ROOT / "tools" / "da3" / "inference.py"
    )
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _make_generator(module):
    """Construct a generator without running __init__ (no model load needed)."""
    gen = module.DA3GaussianGenerator.__new__(module.DA3GaussianGenerator)
    gen.last_prediction = None
    gen.last_ray_origins = []
    gen.last_confidences = []
    gen.last_sky_masks = []
    gen.allow_fallback_depth = False
    gen.is_monocular = False
    gen.model_name = "DA3-GIANT-1.1"
    gen.model = None
    gen.device = "cpu"
    gen.dtype = module.torch.float32
    return gen


def _read_ply_vertex(path: str, vertex_index: int) -> dict[str, float]:
    """Read one scalar-only binary PLY vertex."""
    with open(path, "rb") as f:
        data = f.read()
    hdr_end = data.index(b"end_header\n") + len(b"end_header\n")
    header = data[:hdr_end].decode("ascii")
    properties = [
        line.split()[2] for line in header.splitlines() if line.startswith("property float ")
    ]
    offset = hdr_end + vertex_index * len(properties) * 4
    values = struct.unpack_from(f"<{len(properties)}f", data, offset)
    return dict(zip(properties, values, strict=True))


def main() -> int:
    m = _load_inference_module()
    gen = _make_generator(m)
    SH_C0 = m.SH_C0

    # Case 1+2: graceful None when no prediction / no gaussians.
    assert gen.gaussians_from_prediction() is None
    gen.last_prediction = SimpleNamespace(gaussians=None, depth=None)
    assert gen.gaussians_from_prediction() is None
    print("case1+2 (None handling): PASS")

    # Case 3: full gaussians. Known values chosen so each transform is testable.
    dc0 = np.array([0.0, 0.0, 0.0], dtype=np.float32)  # -> rgb 0.5
    dc1 = np.array([0.5 / SH_C0] * 3, dtype=np.float32)  # -> rgb 1.0
    harmonics_array = np.zeros((1, 2, 3, 4), dtype=np.float32)
    harmonics_array[0, :, :, 0] = np.stack([dc0, dc1], axis=0)
    harmonics_array[0, 0, :, 1:] = np.array([[1, 2, 3], [4, 5, 6], [7, 8, 9]], dtype=np.float32)
    harmonics = _TensorShim(harmonics_array)
    g = SimpleNamespace(
        means=_TensorShim(np.array([[[1, 2, 0], [3, 4, 5]]], dtype=np.float32)),
        scales=_TensorShim(np.array([[[0.1, 0.2, 0.3], [0.4, 0.5, 0.6]]], dtype=np.float32)),
        rotations=_TensorShim(np.array([[[1, 0, 0, 0], [0.5, 0.5, 0.5, 0.5]]], dtype=np.float32)),
        harmonics=harmonics,
        opacities=_TensorShim(np.array([[0.9, 0.5]], dtype=np.float32)),
    )
    gen.last_prediction = SimpleNamespace(
        gaussians=g,
        depth=np.ones((1, 1, 2), dtype=np.float32),
    )
    out = gen.gaussians_from_prediction()
    assert out is not None, "expected dict"

    assert np.allclose(out["positions"], [[1, 2, 0], [3, 4, 5]])
    print("case3 positions: PASS")

    # DC inverted to rgb (so save_ply's rgb->SH-DC round-trips).
    assert np.allclose(out["colors"][0], [0.5, 0.5, 0.5], atol=1e-5)
    assert np.allclose(out["colors"][1], [1.0, 1.0, 1.0], atol=1e-5)
    print("case3 colors (DC->rgb invert): PASS")

    # Scales passed through linear (save_ply applies log()).
    assert np.allclose(out["scales"], [[0.1, 0.2, 0.3], [0.4, 0.5, 0.6]])
    print("case3 scales (linear passthrough): PASS")

    # Rotations wxyz preserved (already world space).
    assert np.allclose(out["rotations"], [[1, 0, 0, 0], [0.5, 0.5, 0.5, 0.5]])
    print("case3 rotations (wxyz preserved): PASS")

    # Opacities passed through (save_ply applies logit()).
    assert np.allclose(out["opacities"], [0.9, 0.5])
    print("case3 opacities (passthrough): PASS")
    assert np.array_equal(out["sh_coefficients"], harmonics_array.reshape(2, 3, 4))
    print("case3 complete SH data: PASS")

    # Case 4: direct GS subsampling operates on the 2-D pixel grid, matching
    # depth-derived splats rather than taking an arbitrary flat stride.
    sub = gen.gaussians_from_prediction(subsample=2)
    assert sub is not None
    assert len(sub["positions"]) == 1
    print("case4 subsample slice: PASS")

    # Case 5: end-to-end round-trip through save_ply. The final PLY f_dc must
    # equal the model's original DC band -- this is the property that proves
    # the DC->rgb->DC chain does not drift.
    import tempfile, os

    fd, ply_path = tempfile.mkstemp(suffix=".ply")
    os.close(fd)
    try:
        m.save_ply(out, ply_path)
        header = Path(ply_path).read_bytes().split(b"end_header\n", 1)[0].decode("ascii")
        assert "comment melkor_profile da3-gaussian-v1\n" in header
        assert "comment melkor_coordinate_system ply-rdf\n" in header
        assert "comment melkor_sh_degree 1\n" in header
        vertex0 = _read_ply_vertex(ply_path, 0)
        vertex1 = _read_ply_vertex(ply_path, 1)
        f0 = tuple(vertex0[f"f_dc_{channel}"] for channel in range(3))
        f1 = tuple(vertex1[f"f_dc_{channel}"] for channel in range(3))
        assert all(abs(x) < 1e-4 for x in f0), f"point0 DC drift: {f0}"
        assert abs(f1[0] - (0.5 / SH_C0)) < 1e-3, f"point1 DC drift: {f1}"
        assert [vertex0[f"f_rest_{index}"] for index in range(9)] == list(range(1, 10))
        assert vertex0["nx"] == vertex0["ny"] == vertex0["nz"] == 0.0
        print(
            f"case5 (save_ply complete SH preserved): PASS  "
            f"p0={tuple(round(v, 4) for v in f0)} p1[0]={f1[0]:.4f} "
            f"expected={0.5 / SH_C0:.4f}"
        )
    finally:
        os.unlink(ply_path)

    # Case 5b: the pinned DA3 Gaussian head emits degree-4 SH data. Preserve
    # all 25 coefficients instead of rejecting or truncating the final band.
    degree_four_harmonics = np.arange(75, dtype=np.float32).reshape(1, 1, 3, 25)
    degree_four = SimpleNamespace(
        means=_TensorShim(np.array([[[1, 2, 3]]], dtype=np.float32)),
        scales=_TensorShim(np.array([[[0.1, 0.2, 0.3]]], dtype=np.float32)),
        rotations=_TensorShim(np.array([[[1, 0, 0, 0]]], dtype=np.float32)),
        harmonics=_TensorShim(degree_four_harmonics),
        opacities=_TensorShim(np.array([[0.5]], dtype=np.float32)),
    )
    gen.last_prediction = SimpleNamespace(
        gaussians=degree_four,
        depth=np.ones((1, 1, 1), dtype=np.float32),
    )
    degree_four_out = gen.gaussians_from_prediction()
    assert degree_four_out is not None
    fd, degree_four_path = tempfile.mkstemp(suffix=".ply")
    os.close(fd)
    try:
        m.save_ply(degree_four_out, degree_four_path)
        degree_four_header = (
            Path(degree_four_path).read_bytes().split(b"end_header\n", 1)[0].decode("ascii")
        )
        assert "comment melkor_sh_degree 4\n" in degree_four_header
        assert "property float f_rest_71\n" in degree_four_header
        vertex = _read_ply_vertex(degree_four_path, 0)
        assert vertex["f_rest_71"] == degree_four_harmonics.reshape(1, 3, 25)[0, 2, 24]
    finally:
        os.unlink(degree_four_path)
    print("case5b (degree-4 SH preserved): PASS")

    # Case 6: the command-facing writer refuses replacement unless requested.
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "scene.ply"
        m.save_output_atomic(out, output, False)
        first = output.read_bytes()
        try:
            m.save_output_atomic(out, output, False)
            raise AssertionError("existing output was replaced without --force")
        except FileExistsError:
            pass
        assert output.read_bytes() == first
        m.save_output_atomic(out, output, True)
        try:
            m.save_output_atomic(out, Path(directory) / "preview.glb", False)
            raise AssertionError("lossy GLB preview did not require approval")
        except RuntimeError as error:
            assert "--allow-lossy-preview" in str(error)
    print("case6 (atomic no-overwrite output): PASS")

    # Case 7: depth fallback uses the camera origin and camera-Z convention.
    # A w2c translation of -10 on X means the camera center is world X=10.
    # With K=I, pixel u=1 has the unnormalized depth vector (1,0,1), so Z-depth
    # 2 reconstructs points (10,0,2) and (12,0,2), not origin-centered points
    # or normalized-ray distances.
    w2c = np.eye(4, dtype=np.float32)
    w2c[0, 3] = -10.0
    intrinsics = np.eye(3, dtype=np.float32)
    origin, depth_vectors = gen._ray_geometry_for_view(0, 1, 2, w2c[None], intrinsics[None])
    assert np.allclose(origin, [10, 0, 0])
    assert np.allclose(depth_vectors[0, 0], [0, 0, 1])
    assert np.allclose(depth_vectors[0, 1], [1, 0, 1])
    gen.last_ray_origins = [origin]
    fallback = gen.depth_rays_to_gaussians(
        [np.array([[2.0, 2.0]], dtype=np.float32)],
        [depth_vectors],
        [np.zeros((1, 2, 3), dtype=np.float32)],
        min_depth=0.1,
        max_depth=10.0,
    )
    assert np.allclose(fallback["positions"], [[10, 0, 2], [12, 0, 2]])
    print("case7 (camera-aware depth unprojection): PASS")

    # Case 8: malformed camera arrays fail closed unless preview fallback was
    # explicitly enabled; reconstruction must never invent a camera silently.
    bad_intrinsics = np.eye(3, dtype=np.float32)
    bad_intrinsics[0, 0] = 0.0
    try:
        gen._ray_geometry_for_view(0, 1, 1, w2c[None], bad_intrinsics[None])
        raise AssertionError("malformed intrinsics were accepted")
    except RuntimeError:
        pass
    gen.allow_fallback_depth = True
    preview_origin, preview_rays = gen._ray_geometry_for_view(
        0, 1, 1, w2c[None], bad_intrinsics[None]
    )
    assert np.allclose(preview_origin, [0, 0, 0]) and preview_rays.shape == (1, 1, 3)
    print("case8 (malformed cameras fail closed): PASS")

    # Case 9: the installer and runtime must agree on immutable checkpoint
    # revisions. A drift here would either bypass review or make a valid setup
    # unusable because inference rejects its marker.
    setup = (REPO_ROOT / "scripts" / "setup_da3.sh").read_text(encoding="utf-8")
    setup_revisions = dict(re.findall(r"\s+(DA3[A-Z0-9.-]+)\) revision=\"([0-9a-f]{40})\"", setup))
    assert m.DA3GaussianGenerator.MODEL_REVISIONS == {
        name: setup_revisions[name] for name in m.DA3GaussianGenerator.MODEL_REVISIONS
    }
    print("case9 (checkpoint revision contract): PASS")

    # Case 10: each output path rejects values that cannot represent a valid
    # Gaussian. Float64 overflow must also fail without a runtime warning.
    valid = {
        "positions": np.zeros((1, 3), dtype=np.float64),
        "colors": np.full((1, 3), 0.5, dtype=np.float64),
        "scales": np.ones((1, 3), dtype=np.float64),
        "rotations": np.array([[1.0, 0.0, 0.0, 0.0]], dtype=np.float64),
        "opacities": np.array([0.5], dtype=np.float64),
    }
    invalid_cases = {
        "non-finite position": ("positions", np.array([[np.nan, 0.0, 0.0]])),
        "float32 overflow": ("positions", np.array([[1.0e100, 0.0, 0.0]])),
        "zero scale": ("scales", np.array([[0.0, 1.0, 1.0]])),
        "negative scale": ("scales", np.array([[-1.0, 1.0, 1.0]])),
        "zero quaternion": ("rotations", np.zeros((1, 4))),
        "opacity below range": ("opacities", np.array([-0.1])),
        "opacity above range": ("opacities", np.array([1.1])),
    }
    for label, (field, value) in invalid_cases.items():
        candidate = {name: np.array(array, copy=True) for name, array in valid.items()}
        candidate[field] = value
        try:
            m.validated_gaussian_arrays(candidate)
            raise AssertionError(f"accepted {label}")
        except ValueError:
            pass
    for endpoint in (0.0, 1.0):
        candidate = {name: np.array(array, copy=True) for name, array in valid.items()}
        candidate["opacities"][:] = endpoint
        fd, endpoint_path = tempfile.mkstemp(suffix=".ply")
        os.close(fd)
        try:
            try:
                m.save_ply(candidate, endpoint_path)
                raise AssertionError(f"PLY accepted opacity endpoint {endpoint}")
            except ValueError:
                pass
        finally:
            os.unlink(endpoint_path)
    print("case10 (invalid Gaussian values rejected): PASS")

    # Case 11: keep upstream's border trim without lossy depth pruning.
    far_depth_gaussians = SimpleNamespace(
        means=_TensorShim(np.array([[[1, 0, 0], [2, 0, 0]]], dtype=np.float32)),
        scales=_TensorShim(np.ones((1, 2, 3), dtype=np.float32)),
        rotations=_TensorShim(np.array([[[1, 0, 0, 0], [1, 0, 0, 0]]], dtype=np.float32)),
        harmonics=_TensorShim(np.zeros((1, 2, 3, 1), dtype=np.float32)),
        opacities=_TensorShim(np.full((1, 2), 0.5, dtype=np.float32)),
    )
    gen.last_prediction = SimpleNamespace(
        gaussians=far_depth_gaussians,
        depth=np.array([[[1.0, 1000.0]]], dtype=np.float32),
    )
    far_depth_output = gen.gaussians_from_prediction()
    assert far_depth_output is not None and len(far_depth_output["positions"]) == 2
    gen.last_prediction.depth[0, 0, 1] = np.nan
    try:
        gen.gaussians_from_prediction()
        raise AssertionError("invalid retained depth was accepted")
    except RuntimeError:
        pass
    print("case11 (valid far-depth Gaussian preserved): PASS")

    # Case 12: discovery accepts mixed-case suffixes and stops at its view cap.
    with tempfile.TemporaryDirectory() as directory:
        image_directory = Path(directory)
        (image_directory / "b.JpG").write_bytes(b"x")
        (image_directory / "a.PNG").write_bytes(b"x")
        (image_directory / "ignored.txt").write_bytes(b"x")
        assert [path.name for path in m.get_image_files(directory)] == ["a.PNG", "b.JpG"]
        for index in range(m.MAX_INPUT_VIEWS - 1):
            (image_directory / f"view-{index:03}.webp").write_bytes(b"x")
        try:
            m.get_image_files(directory)
            raise AssertionError("image discovery exceeded its view cap")
        except ValueError as error:
            assert "DA3-Streaming" in str(error)
    print("case12 (bounded image discovery): PASS")

    # Case 13: image preflight enforces decoded pixel limits.
    class _FakeImage:
        size = (2, 3)

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def verify(self):
            return None

    m.Image.DecompressionBombWarning = RuntimeWarning
    m.Image.DecompressionBombError = RuntimeError
    m.Image.open = lambda _handle: _FakeImage()
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "source.png"
        source.write_bytes(b"valid fixture")
        assert m.probe_image_files([source]) == [(2, 3)]
        with m.snapshot_image_files([source]) as (snapshots, dimensions):
            expected_bytes = source.read_bytes()
            source.write_bytes(b"changed after the snapshot")
            assert dimensions == [(2, 3)]
            assert snapshots[0].read_bytes() == expected_bytes
        original_pixel_limit = m.MAX_IMAGE_PIXELS
        m.MAX_IMAGE_PIXELS = 5
        try:
            m.probe_image_files([source])
            raise AssertionError("image preflight exceeded its pixel cap")
        except ValueError as error:
            assert "pixel limit" in str(error)
        finally:
            m.MAX_IMAGE_PIXELS = original_pixel_limit
    print("case13 (bounded image preflight): PASS")

    # Case 14: the learned branch does not build unused ray or image arrays.
    direct_prediction = SimpleNamespace(
        depth=np.ones((1, 2, 2), dtype=np.float32),
        gaussians=SimpleNamespace(),
        extrinsics=object(),
        intrinsics=object(),
        conf=object(),
        sky=object(),
        processed_images=object(),
    )
    calls = []
    gen.model = SimpleNamespace(
        inference=lambda paths, infer_gs: calls.append((paths, infer_gs)) or direct_prediction
    )
    depths, rays, colors = gen.predict_depth_rays(None, [Path("unused-source.png")])
    assert len(depths) == 1 and rays == [] and colors == []
    assert calls == [(["unused-source.png"], True)]
    assert gen.last_ray_origins == []
    print("case14 (learned path avoids unused geometry): PASS")

    # Case 15: inference failure cannot enter preview fallback without approval.
    def fail_inference(_paths, _infer_gs):
        raise ArithmeticError("synthetic inference failure")

    gen.model = SimpleNamespace(inference=fail_inference)
    gen.allow_fallback_depth = False
    try:
        gen.predict_depth_rays(None, [Path("unused-source.png")])
        raise AssertionError("inference failure entered preview fallback")
    except RuntimeError as error:
        assert "multi-view inference failed" in str(error)
    print("case15 (inference failure fails closed): PASS")

    # Case 16: output validation normalizes finite nonzero quaternions.
    scaled_rotation = {name: np.array(array, copy=True) for name, array in valid.items()}
    scaled_rotation["rotations"] = np.array([[2.0, 0.0, 0.0, 0.0]])
    normalized = m.validated_gaussian_arrays(scaled_rotation)
    assert np.array_equal(normalized["rotations"], [[1.0, 0.0, 0.0, 0.0]])
    try:
        gen.depth_rays_to_gaussians(
            [np.ones((1, 1), dtype=np.float32)],
            [np.zeros((1, 1, 2), dtype=np.float32)],
            [np.zeros((1, 1, 3), dtype=np.float32)],
        )
        raise AssertionError("depth conversion accepted a malformed ray map")
    except ValueError:
        pass
    print("case16 (normalized rotation and shape checks): PASS")

    # Case 17: local model validation rejects symbolic links and stale markers.
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        snapshot = Path(directory) / "DA3-TEST"
        snapshot.mkdir()
        (snapshot / "config.json").write_text("{}\n", encoding="utf-8")
        (snapshot / "model.safetensors").write_bytes(b"weight")
        (snapshot / ".melkor-revision").write_text("a" * 40 + "\n", encoding="utf-8")
        m.validate_model_snapshot(snapshot, "a" * 40)
        try:
            m.validate_model_snapshot(snapshot, "b" * 40)
            raise AssertionError("model validation accepted another revision")
        except RuntimeError:
            pass
        try:
            (snapshot / "unsafe-link").symlink_to("config.json")
        except OSError:
            pass
        else:
            try:
                m.validate_model_snapshot(snapshot, "a" * 40)
                raise AssertionError("model validation accepted a symbolic link")
            except RuntimeError:
                pass
    print("case17 (model snapshot contract): PASS")

    print("\nALL gaussians_from_prediction TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
