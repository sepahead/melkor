#!/usr/bin/env python3
"""Test the model-direct Gaussians extraction path in inference.py.

These tests do NOT require DA3 weights, GPU, or torch. They build a synthetic
Gaussians-shaped object and validate that DA3GaussianGenerator.gaussians_from_prediction()
correctly maps the model's output to the dict format that save_ply() consumes,
with field transforms that match ByteDance's export_ply() in
DA3coreml/src/depth_anything_3/utils/gsply_helpers.py.

Critical correctness property: after save_ply's rgb->SH-DC conversion, the
final PLY f_dc must equal the model's original DC band (no double-conversion
drift).

Run: python3 tests/test_gaussians_from_prediction.py
Exit code 0 = pass, nonzero = fail.
"""
from __future__ import annotations

import importlib.util
import struct
import sys
import types
from pathlib import Path
from types import SimpleNamespace

try:
    import numpy as np
except ImportError:
    # numpy is the only hard runtime dependency. If it's missing, skip
    # gracefully (exit 0) rather than erroring, so this test never breaks a
    # minimal build environment that didn't install Python scientific deps.
    print("SKIP: numpy not installed")
    sys.exit(0)

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
    fake_torch.cuda = SimpleNamespace(
        is_available=lambda: False, is_bf16_supported=lambda: False
    )
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
    return gen


def _read_ply_fdc(path: str, vertex_index: int) -> tuple[float, float, float]:
    """Read f_dc_0/1/2 for a vertex from a melkor-written binary PLY."""
    with open(path, "rb") as f:
        data = f.read()
    hdr_end = data.index(b"end_header\n") + len(b"end_header\n")
    offset = hdr_end + vertex_index * 17 * 4  # 17 floats per vertex
    vals = struct.unpack_from("<17f", data, offset)
    return vals[6], vals[7], vals[8]  # x,y,z,nx,ny,nz,f_dc_0,f_dc_1,f_dc_2,...


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
    dc0 = np.array([[0.0, 0.0, 0.0]], dtype=np.float32)          # -> rgb 0.5
    dc1 = np.array([[0.5 / SH_C0] * 3], dtype=np.float32)        # -> rgb 1.0
    harmonics = _TensorShim(np.stack([dc0, dc1], axis=0)[None, ..., None])  # (1,2,3,1)
    g = SimpleNamespace(
        means=_TensorShim(np.array([[[1, 2, 0], [3, 4, 5]]], dtype=np.float32)),
        scales=_TensorShim(np.array([[[0.1, 0.2, 0.3], [0.4, 0.5, 0.6]]], dtype=np.float32)),
        rotations=_TensorShim(
            np.array([[[1, 0, 0, 0], [0.5, 0.5, 0.5, 0.5]]], dtype=np.float32)
        ),
        harmonics=harmonics,
        opacities=_TensorShim(np.array([[0.9, 0.5]], dtype=np.float32)),
    )
    gen.last_prediction = SimpleNamespace(gaussians=g, depth=None)
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

    # Case 4: subsample strided slice on the dict (as main() does).
    sel = slice(None, None, 2)
    sub = {k: v[sel] for k, v in out.items()}
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
        f0 = _read_ply_fdc(ply_path, 0)
        f1 = _read_ply_fdc(ply_path, 1)
        assert all(abs(x) < 1e-4 for x in f0), f"point0 DC drift: {f0}"
        assert abs(f1[0] - (0.5 / SH_C0)) < 1e-3, f"point1 DC drift: {f1}"
        print(
            f"case5 (save_ply round-trip DC preserved): PASS  "
            f"p0={tuple(round(v,4) for v in f0)} p1[0]={f1[0]:.4f} "
            f"expected={0.5/SH_C0:.4f}"
        )
    finally:
        os.unlink(ply_path)

    print("\nALL gaussians_from_prediction TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
