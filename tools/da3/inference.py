#!/usr/bin/env python3
"""
Depth-Anything-3 Single-GPU Inference Script
Converts images to 3D Gaussian Splats using DA3's depth-ray representation.

Usage:
    python inference.py --input images/ --output output.ply
    python inference.py --input image.jpg --output output.ply --model da3-large-1.1
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import stat
import sys
import tempfile
import time
import warnings
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, List, Optional, Tuple

import numpy as np
import torch
from PIL import Image
from tqdm import tqdm

# Default paths
DEFAULT_MODEL_DIR = os.path.expanduser("~/.melkor/models/da3")
DEFAULT_MODEL = "DA3-BASE"

# Spherical-harmonics band-0 constant for the 3DGS color convention.
# Value: 1 / (2 * sqrt(pi)).
SH_C0 = 0.28209479177387814
SUPPORTED_SH_COEFFICIENT_COUNTS = {1, 4, 9, 16, 25}
SUPPORTED_IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".webp", ".tiff", ".tif", ".bmp"}
MAX_INPUT_VIEWS = 256
MAX_INPUT_FILE_BYTES = 512 * 1024 * 1024
MAX_TOTAL_INPUT_BYTES = 8 * 1024 * 1024 * 1024
MAX_IMAGE_PIXELS = 100_000_000
MAX_TOTAL_IMAGE_PIXELS = 1_000_000_000
COPY_CHUNK_BYTES = 1024 * 1024


def get_image_files(input_path: str) -> List[Path]:
    """Return one bounded, sorted list of supported image files."""
    input_path = Path(input_path)

    if input_path.is_file():
        if input_path.suffix.lower() not in SUPPORTED_IMAGE_SUFFIXES:
            raise ValueError(f"unsupported input image extension: {input_path.suffix or 'none'}")
        return [input_path]

    if input_path.is_dir():
        files: List[Path] = []
        for path in input_path.iterdir():
            if not path.is_file() or path.suffix.lower() not in SUPPORTED_IMAGE_SUFFIXES:
                continue
            files.append(path)
            if len(files) > MAX_INPUT_VIEWS:
                raise ValueError(
                    f"input contains more than {MAX_INPUT_VIEWS} images. "
                    "Use DA3-Streaming for longer scenes"
                )
        return sorted(files)

    raise ValueError(f"Input path does not exist: {input_path}")


def _copy_and_probe_image(source_path: Path, snapshot_path: Path) -> Tuple[int, int, int]:
    """Copy one bound image file and validate the exact copied bytes."""
    if source_path.is_symlink():
        raise ValueError(f"input image must not be a symbolic link: {source_path}")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(source_path, flags)
        with os.fdopen(descriptor, "rb") as source, snapshot_path.open("xb") as snapshot:
            initial = os.fstat(source.fileno())
            if not stat.S_ISREG(initial.st_mode):
                raise ValueError(f"input image is not a regular file: {source_path}")
            if initial.st_size <= 0 or initial.st_size > MAX_INPUT_FILE_BYTES:
                raise ValueError(
                    f"input image must contain 1 through {MAX_INPUT_FILE_BYTES} bytes: "
                    f"{source_path}"
                )

            remaining = initial.st_size
            while remaining:
                chunk = source.read(min(COPY_CHUNK_BYTES, remaining))
                if not chunk:
                    raise ValueError(f"input image changed during snapshot: {source_path}")
                snapshot.write(chunk)
                remaining -= len(chunk)
            if source.read(1):
                raise ValueError(f"input image changed during snapshot: {source_path}")

            final = os.fstat(source.fileno())
            if (
                final.st_dev != initial.st_dev
                or final.st_ino != initial.st_ino
                or final.st_size != initial.st_size
                or final.st_mtime_ns != initial.st_mtime_ns
                or final.st_ctime_ns != initial.st_ctime_ns
            ):
                raise ValueError(f"input image changed during snapshot: {source_path}")
    except OSError as error:
        raise ValueError(f"cannot snapshot input image: {source_path}") from error

    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", Image.DecompressionBombWarning)
            with Image.open(snapshot_path) as image:
                width, height = image.size
                if width <= 0 or height <= 0:
                    raise ValueError(f"input image has invalid dimensions: {source_path}")
                pixels = width * height
                if pixels > MAX_IMAGE_PIXELS:
                    raise ValueError(
                        f"input image exceeds the {MAX_IMAGE_PIXELS}-pixel limit: {source_path}"
                    )
                image.verify()
    except (OSError, SyntaxError, Image.DecompressionBombError) as error:
        raise ValueError(f"cannot validate input image: {source_path}") from error
    return width, height, initial.st_size


@contextmanager
def snapshot_image_files(
    paths: List[Path],
) -> Iterator[Tuple[List[Path], List[Tuple[int, int]]]]:
    """Yield private validated copies of all input images."""
    total_bytes = 0
    total_pixels = 0
    dimensions: List[Tuple[int, int]] = []
    with tempfile.TemporaryDirectory(prefix="melkor-da3-input-") as directory:
        snapshot_root = Path(directory)
        snapshots: List[Path] = []
        for index, path in enumerate(paths):
            suffix = path.suffix.lower()
            snapshot = snapshot_root / f"view-{index:05d}{suffix}"
            width, height, byte_count = _copy_and_probe_image(path, snapshot)
            total_bytes += byte_count
            if total_bytes > MAX_TOTAL_INPUT_BYTES:
                raise ValueError(
                    f"input images exceed the {MAX_TOTAL_INPUT_BYTES}-byte total limit"
                )
            total_pixels += width * height
            if total_pixels > MAX_TOTAL_IMAGE_PIXELS:
                raise ValueError(
                    f"input images exceed the {MAX_TOTAL_IMAGE_PIXELS}-pixel total limit"
                )
            snapshots.append(snapshot)
            dimensions.append((width, height))
        yield snapshots, dimensions


def probe_image_files(paths: List[Path]) -> List[Tuple[int, int]]:
    """Validate image files and return their dimensions."""
    with snapshot_image_files(paths) as (_snapshots, dimensions):
        return dimensions


def validate_model_snapshot(model_path: Path, expected_revision: Optional[str]) -> None:
    """Validate one local model tree and its revision marker."""
    if expected_revision is None:
        raise RuntimeError("the selected model has no reviewed revision")
    if model_path.is_symlink() or not model_path.is_dir():
        raise FileNotFoundError(
            f"revision-marked local model not found at {model_path}; run scripts/setup_da3.sh"
        )

    marker_path = model_path / ".melkor-revision"
    config_path = model_path / "config.json"
    if marker_path.is_symlink() or not marker_path.is_file():
        raise RuntimeError(f"model snapshot has no regular revision marker: {model_path}")
    if config_path.is_symlink() or not config_path.is_file() or config_path.stat().st_size == 0:
        raise RuntimeError(f"model snapshot has no regular configuration file: {model_path}")

    actual_revision = marker_path.read_text(encoding="utf-8").strip()
    if actual_revision != expected_revision:
        raise RuntimeError(
            f"model snapshot at {model_path} has revision marker "
            f"{actual_revision or 'empty'}; expected {expected_revision}"
        )

    has_weights = False
    try:
        for root, directories, files in os.walk(model_path, followlinks=False):
            root_path = Path(root)
            for name in directories:
                entry = root_path / name
                mode = entry.lstat().st_mode
                if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
                    raise RuntimeError(f"model snapshot contains an unsafe entry: {entry}")
            for name in files:
                entry = root_path / name
                info = entry.lstat()
                if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
                    raise RuntimeError(f"model snapshot contains an unsafe entry: {entry}")
                if (
                    name.endswith(".safetensors") or name.startswith("pytorch_model")
                ) and (name.endswith(".safetensors") or name.endswith(".bin")):
                    has_weights |= info.st_size > 0
    except OSError as error:
        raise RuntimeError(f"cannot validate model snapshot: {model_path}") from error
    if not has_weights:
        raise RuntimeError(f"model snapshot has no nonempty weight file: {model_path}")


def load_image(
    path: Path, size: Optional[Tuple[int, int]] = None
) -> Tuple[torch.Tensor, np.ndarray]:
    """Load and preprocess one validated image."""
    with Image.open(path) as source:
        img = source.convert("RGB")

        if size is not None:
            img = img.resize(size, Image.LANCZOS)

        # Convert to tensor and normalize.
        img_np = np.array(img, dtype=np.float32) / 255.0
    img_tensor = torch.from_numpy(img_np).permute(2, 0, 1)  # HWC -> CHW

    return img_tensor, img_np


class DA3GaussianGenerator:
    """Generate 3D Gaussian Splats from images using Depth-Anything-3."""

    # Class-level flag to show fallback warning only once
    _fallback_warned = False

    # Model name normalization map
    MODEL_NAME_MAP = {
        "da3-small": "DA3-SMALL",
        "da3-base": "DA3-BASE",
        "da3-large": "DA3-LARGE-1.1",
        "da3-large-1.1": "DA3-LARGE-1.1",
        "da3-giant": "DA3-GIANT-1.1",
        "da3-giant-1.1": "DA3-GIANT-1.1",
        "da3mono-large": "DA3MONO-LARGE",
        "da3metric-large": "DA3METRIC-LARGE",
        "da3nested-giant-large": "DA3NESTED-GIANT-LARGE-1.1",
        "da3nested-giant-large-1.1": "DA3NESTED-GIANT-LARGE-1.1",
    }

    # Models that support multi-view input
    MULTI_VIEW_MODELS = {
        "DA3-SMALL",
        "DA3-BASE",
        "DA3-LARGE-1.1",
        "DA3-GIANT-1.1",
        "DA3NESTED-GIANT-LARGE-1.1",
    }

    # Models that output metric depth (in meters)
    METRIC_MODELS = {"DA3METRIC-LARGE", "DA3NESTED-GIANT-LARGE-1.1"}

    # Models optimized for monocular (single-view) input
    MONOCULAR_MODELS = {"DA3MONO-LARGE", "DA3METRIC-LARGE"}

    # Upstream exposes the learned Gaussian head only for these checkpoints.
    # Passing infer_gs=True to the smaller depth-only models is not supported.
    GAUSSIAN_MODELS = {"DA3-GIANT-1.1", "DA3NESTED-GIANT-LARGE-1.1"}

    # Must match scripts/setup_da3.sh. Inference only consumes a local,
    # revision-marked snapshot; it never falls back to a mutable Hub branch.
    MODEL_REVISIONS = {
        "DA3-SMALL": "e08cab65ca0ec38e7826075418411ab90cab4da3",
        "DA3-BASE": "f4a6c9b3c95e41c82048423d3493a81ec3fa810e",
        "DA3-LARGE-1.1": "0e109ae307c5982f319a67cf6f9f99ccdc0ec97c",
        "DA3-GIANT-1.1": "72ee9f89ce4e50d704e9d55ee9c646ec8dc25a19",
        "DA3NESTED-GIANT-LARGE-1.1": "b2359bdf726fb44ef62acca04d629dcf158053e7",
    }

    def __init__(
        self,
        model_name: str = DEFAULT_MODEL,
        model_dir: str = DEFAULT_MODEL_DIR,
        device: str = "cuda",
        dtype: torch.dtype = torch.float16,
        allow_fallback_depth: bool = False,
    ):
        self.device = device
        self.dtype = dtype
        # Normalize model name to uppercase canonical form
        self.model_name = self.MODEL_NAME_MAP.get(model_name.lower(), model_name.upper())
        self.model_dir = Path(model_dir)
        self.model = None
        # Last Prediction returned by inference(); used by
        # gaussians_from_prediction() to read the model's directly-estimated
        # Gaussian parameters (means/scales/rotations/SH/opacity) instead of
        # re-deriving them from depth x ray. Set in predict_depth_rays().
        self.last_prediction = None
        # Per-view world-space camera origins for the depth fallback. DA3 depth
        # is camera-Z depth, so reconstructing world points requires both the
        # camera origin and the unnormalized K^-1 pixel vector.
        self.last_ray_origins: List[np.ndarray] = []
        self.last_confidences: List[Optional[np.ndarray]] = []
        self.last_sky_masks: List[Optional[np.ndarray]] = []
        # When the DA3 model is unavailable, the intensity-based depth fallback
        # is gated behind this explicit opt-in. The fallback output is
        # preview-only and must never flow into a reconstruction pipeline
        # unnoticed, so the default is False.
        self.allow_fallback_depth = allow_fallback_depth

        # Determine model capabilities
        self.supports_multi_view = self.model_name in self.MULTI_VIEW_MODELS
        self.outputs_metric_depth = self.model_name in self.METRIC_MODELS
        self.is_monocular = self.model_name in self.MONOCULAR_MODELS

    def load_model(self):
        """Load the DA3 model."""
        print(f"Loading {self.model_name}...")
        print(f"  Multi-view support: {self.supports_multi_view}")
        print(f"  Metric depth output: {self.outputs_metric_depth}")
        print(f"  Monocular optimized: {self.is_monocular}")

        try:
            # Try to load from local path first
            model_path = self.model_dir / self.model_name

            expected_revision = self.MODEL_REVISIONS.get(self.model_name)
            validate_model_snapshot(model_path, expected_revision)
            from depth_anything_3.api import DepthAnything3

            self.model = DepthAnything3.from_pretrained(str(model_path))

            # Let upstream choose BF16/FP16 autocast on capable CUDA devices.
            # Casting the entire checkpoint to FP16 here overrides that policy
            # and loses BF16's wider exponent range. --fp32 remains an explicit
            # diagnostic/CPU path.
            if self.dtype == torch.float32:
                self.model = self.model.to(device=self.device, dtype=torch.float32)
            else:
                self.model = self.model.to(device=self.device)
            self.model.eval()
            print(f"Model loaded on {self.device}")

        except Exception as e:
            self.model = None
            if not self.allow_fallback_depth:
                raise RuntimeError(
                    "DA3 model loading failed and preview fallback is disabled"
                ) from e
            print(f"Warning: DA3 model unavailable ({e}).")
            print("Using explicit preview-only intensity depth fallback.")

    def predict_depth_rays(
        self, images: Optional[List[torch.Tensor]], image_paths: List[Path]
    ) -> Tuple[List[np.ndarray], List[np.ndarray], List[np.ndarray]]:
        """Predict depth and ray maps for images.

        DA3 is a multi-view model: feeding all images in a single inference()
        call lets it jointly estimate consistent geometry, poses, and per-pixel
        rays across views. Iterating per-image destroys this and falls back to
        monocular depth, which is why this method now issues ONE batched call.

        Returns:
            depths: List of depth maps, one per input image, each (H, W)
            rays: List of ray direction maps, each (H, W, 3)
            colors: List of RGB color arrays, each (H, W, 3)
        """
        depths: List[np.ndarray] = []
        rays: List[np.ndarray] = []
        colors: List[np.ndarray] = []
        self.last_ray_origins = []
        self.last_confidences = []
        self.last_sky_masks = []

        if not image_paths or (images is not None and len(images) != len(image_paths)):
            raise ValueError("images and image paths must contain the same nonzero view count")

        if self.is_monocular:
            raise RuntimeError(
                f"{self.model_name} is a depth-only checkpoint without multi-view "
                "camera poses/intrinsics, so it cannot produce a world-space splat "
                "scene. Use the upstream DA3 depth exporter, or choose DA3-BASE, "
                "DA3-SMALL, DA3-LARGE-1.1, DA3-GIANT-1.1, or "
                "DA3NESTED-GIANT-LARGE-1.1."
            )

        # Warn (not fail) on dimension mismatch; DA3 resizes internally but the
        # caller should ideally pre-normalize inputs.
        if images:
            first_shape = images[0].shape
            mismatched = [
                (i, image_paths[i], img.shape)
                for i, img in enumerate(images)
                if img.shape != first_shape
            ]
            if mismatched:
                print(f"\nWarning: {len(mismatched)} images have different dimensions:")
                for _idx, path, shape in mismatched[:5]:
                    print(f"  {path.name}: {shape} (expected {first_shape})")
                if len(mismatched) > 5:
                    print(f"  ... and {len(mismatched) - 5} more")
                print("DA3 will resize internally; results may be suboptimal.\n")

        if self.model is not None:
            # Single multi-view call: inference() takes the list of image paths
            # (or arrays) and returns a Prediction whose arrays are indexed by
            # view, i.e. depth has shape (N, H, W). The learned Gaussian branch
            # is requested only for the two checkpoints that upstream documents
            # as supporting it; other models use camera-aware depth unprojection.
            try:
                with torch.no_grad():
                    prediction = self.model.inference(
                        [str(p) for p in image_paths],
                        infer_gs=self.model_name in self.GAUSSIAN_MODELS,
                    )
                # Stash the Prediction so callers can use the model's directly
                # estimated Gaussians (means, scales, rotations, SH, opacity)
                # instead of re-deriving them from depth x ray. See
                # gaussians_from_prediction().
                self.last_prediction = prediction
                depth_stack = np.asarray(prediction.depth)  # (N, H, W)
                if depth_stack.ndim == 2:
                    # Single view came back unbatched; reintroduce the view axis.
                    depth_stack = depth_stack[None]
            except Exception as e:
                self.last_prediction = None
                if not self.allow_fallback_depth:
                    raise RuntimeError("DA3 multi-view inference failed") from e
                print(f"Warning: DA3 multi-view inference failed: {e}")
                depth_stack = None
        else:
            self.last_prediction = None
            depth_stack = None

        # Per-image fallback only if the model is missing or the batched call
        # failed. This keeps the old code path alive for degraded environments
        # but is explicitly NOT the primary route.
        if depth_stack is None:
            print("Falling back to per-image depth estimation.")
            with torch.no_grad():
                for view_index in tqdm(range(len(image_paths)), desc="Predicting depth"):
                    img_tensor = (
                        images[view_index]
                        if images is not None
                        else load_image(image_paths[view_index])[0]
                    )
                    img_batch = img_tensor.unsqueeze(0).to(self.device, self.dtype)
                    depth = self._fallback_depth(img_batch)
                    depths.append(depth)
                    rays.append(self._compute_rays(*depth.shape))
                    self.last_ray_origins.append(np.zeros(3, dtype=np.float32))
                    self.last_confidences.append(None)
                    self.last_sky_masks.append(None)
                    colors.append(img_tensor.permute(1, 2, 0).numpy())
            return depths, rays, colors

        # Primary path: slice the batched prediction per view.
        if depth_stack.ndim != 3:
            raise RuntimeError("DA3 returned a depth stack with an invalid layout")
        n_views = depth_stack.shape[0]
        if n_views != len(image_paths):
            raise RuntimeError(
                "DA3 returned a depth stack that does not match the input view count"
            )
        # Learned Gaussians already contain geometry and color data.
        # Do not build unused camera, ray, mask, or image arrays.
        if getattr(prediction, "gaussians", None) is not None:
            return [np.asarray(depth_stack[index]) for index in range(n_views)], [], []
        ext_np = (
            np.asarray(prediction.extrinsics)
            if getattr(prediction, "extrinsics", None) is not None
            else None
        )
        ixt_np = (
            np.asarray(prediction.intrinsics)
            if getattr(prediction, "intrinsics", None) is not None
            else None
        )
        pred_conf = getattr(prediction, "conf", None)
        conf_np = np.asarray(pred_conf) if pred_conf is not None else None
        if conf_np is not None and conf_np.ndim == 2 and n_views == 1:
            conf_np = conf_np[None]
        if conf_np is not None and (conf_np.ndim != 3 or conf_np.shape[0] != n_views):
            raise RuntimeError("DA3 returned confidence data with an invalid view layout")
        pred_sky = getattr(prediction, "sky", None)
        sky_np = np.asarray(pred_sky) if pred_sky is not None else None
        if sky_np is not None and sky_np.ndim == 2 and n_views == 1:
            sky_np = sky_np[None]
        if sky_np is not None and (sky_np.ndim != 3 or sky_np.shape[0] != n_views):
            raise RuntimeError("DA3 returned sky data with an invalid view layout")
        processed_images = getattr(prediction, "processed_images", None)
        processed_np = np.asarray(processed_images) if processed_images is not None else None
        if processed_np is not None and (
            processed_np.ndim != 4
            or processed_np.shape[0] != n_views
            or processed_np.shape[-1] != 3
        ):
            raise RuntimeError("DA3 returned processed images with an invalid view layout")
        if (ext_np is None or ixt_np is None) and not self.allow_fallback_depth:
            raise RuntimeError(
                "DA3 returned depth without camera extrinsics/intrinsics. Refusing "
                "to invent a 60-degree camera and origin because that would not be "
                "a valid world-space reconstruction."
            )
        for view_idx in range(n_views):
            depth = depth_stack[view_idx]
            depths.append(depth)

            h, w = depth.shape
            origin, ray = self._ray_geometry_for_view(view_idx, h, w, ext_np, ixt_np)
            self.last_ray_origins.append(origin)
            rays.append(ray)
            self.last_confidences.append(
                np.asarray(conf_np[view_idx]) if conf_np is not None else None
            )
            self.last_sky_masks.append(np.asarray(sky_np[view_idx]) if sky_np is not None else None)

            # Recover the (possibly resized) color image. The model's processed
            # images are the ground truth for what the network actually saw.
            if processed_np is not None:
                processed = np.asarray(processed_np[view_idx], dtype=np.float32)
                if (
                    processed.shape[:2] != (h, w)
                    or not np.isfinite(processed).all()
                    or np.any((processed < 0.0) | (processed > 255.0))
                ):
                    raise RuntimeError(
                        "DA3 returned a processed image that does not match its depth map"
                    )
                colors.append(processed / 255.0)
            else:
                source_tensor = (
                    images[view_idx] if images is not None else load_image(image_paths[view_idx])[0]
                )
                original = source_tensor.permute(1, 2, 0).numpy()
                if original.shape[:2] != (h, w):
                    resized = Image.fromarray(
                        np.rint(np.clip(original, 0.0, 1.0) * 255.0).astype(np.uint8),
                        mode="RGB",
                    ).resize((w, h), Image.LANCZOS)
                    original = np.asarray(resized, dtype=np.float32) / 255.0
                colors.append(original)

        return depths, rays, colors

    def _ray_geometry_for_view(
        self,
        view_idx: int,
        h: int,
        w: int,
        extrinsics: "np.ndarray | None",
        intrinsics: "np.ndarray | None",
    ) -> Tuple[np.ndarray, np.ndarray]:
        """Build a camera origin and per-pixel world-space depth vectors.

        When DA3 provides camera extrinsics (w2c, (4,4)) and intrinsics (3,3)
        we back-project K^-1 [u,v,1] and rotate it into world space. The vector
        is deliberately NOT normalized: Prediction.depth is camera-Z depth,
        not Euclidean ray distance. World points are therefore
        ``camera_origin + depth * depth_vector``. If camera parameters are
        unavailable, retain the old origin-zero/FOV approximation for the
        explicitly opted-in preview fallback.
        """
        if extrinsics is not None and intrinsics is not None:
            try:
                ext = extrinsics[view_idx] if extrinsics.ndim >= 3 else extrinsics
                ixt = intrinsics[view_idx] if intrinsics.ndim >= 3 else intrinsics
                if ext.shape not in ((3, 4), (4, 4)) or ixt.shape != (3, 3):
                    raise ValueError(
                        f"expected extrinsics (3,4)/(4,4) and intrinsics (3,3), "
                        f"got {ext.shape} and {ixt.shape}"
                    )
                if not np.isfinite(ext).all() or not np.isfinite(ixt).all():
                    raise ValueError("camera arrays contain NaN/Inf")
                w2c = np.eye(4, dtype=np.float64)
                w2c[: ext.shape[0], : ext.shape[1]] = ext
                if not np.allclose(w2c[3], [0.0, 0.0, 0.0, 1.0], rtol=0.0, atol=1e-6):
                    raise ValueError("camera extrinsics are not affine")
                rotation = w2c[:3, :3]
                if not np.allclose(
                    rotation @ rotation.T, np.eye(3), rtol=1e-3, atol=1e-3
                ) or not np.isclose(np.linalg.det(rotation), 1.0, rtol=1e-3, atol=1e-3):
                    raise ValueError("camera extrinsics do not contain a proper rotation")
                c2w = np.linalg.inv(w2c)
                if float(ixt[0, 0]) <= 0.0 or float(ixt[1, 1]) <= 0.0:
                    raise ValueError("camera intrinsics have nonpositive focal lengths")
                inverse_intrinsics = np.linalg.inv(ixt.astype(np.float64))
                u = np.arange(w, dtype=np.float64)
                v = np.arange(h, dtype=np.float64)
                uu, vv = np.meshgrid(u, v)
                # Camera-space vector for one unit of camera-Z depth.
                pixels = np.stack([uu, vv, np.ones_like(uu)], axis=-1)
                dir_cam = pixels @ inverse_intrinsics.T
                camera_z = dir_cam[..., 2:3]
                if not np.isfinite(dir_cam).all() or np.any(np.abs(camera_z) < 1e-12):
                    raise ValueError("camera intrinsics produce invalid depth vectors")
                dir_cam /= camera_z
                r_c2w = c2w[:3, :3].astype(np.float64)
                dir_world = dir_cam @ r_c2w.T
                origin = c2w[:3, 3].astype(np.float32)
                return origin, dir_world.astype(np.float32)
            except Exception as e:
                if not self.allow_fallback_depth:
                    raise RuntimeError(
                        "DA3 returned malformed camera parameters; refusing to invent "
                        "preview geometry for a world-space reconstruction."
                    ) from e
                print(
                    f"Warning: ray derivation from camera params failed ({e}); using FOV fallback."
                )
        return np.zeros(3, dtype=np.float32), self._compute_rays(h, w)

    def _ray_map_for_view(
        self,
        view_idx: int,
        h: int,
        w: int,
        extrinsics: "np.ndarray | None",
        intrinsics: "np.ndarray | None",
    ) -> np.ndarray:
        """Compatibility wrapper returning only the per-pixel depth vectors."""
        return self._ray_geometry_for_view(view_idx, h, w, extrinsics, intrinsics)[1]

    def _fallback_depth(self, img_batch: torch.Tensor) -> np.ndarray:
        """Preview-only intensity-based depth estimation.

        This assumes darker pixels are farther away, which is frequently wrong
        (shadows, dark surfaces, lighting). The output is NOT suitable for 3D
        reconstruction and is gated behind `allow_fallback_depth` to prevent it
        from silently flowing into a pipeline. Install the DA3 model for real
        depth: ./scripts/setup_da3.sh
        """
        if not self.allow_fallback_depth:
            raise RuntimeError(
                "DA3 model unavailable and intensity-based fallback is disabled. "
                "The fallback produces preview-only output unsuitable for "
                "reconstruction. Re-run with --allow-fallback-depth to override, "
                "or install the model: ./scripts/setup_da3.sh"
            )
        if not DA3GaussianGenerator._fallback_warned:
            DA3GaussianGenerator._fallback_warned = True
            print("\n" + "=" * 70)
            print("WARNING: Using FALLBACK depth estimation (intensity-based)")
            print("This produces PREVIEW-ONLY results unsuitable for 3D")
            print("reconstruction. Install the DA3 model: ./scripts/setup_da3.sh")
            print("=" * 70 + "\n")

        # Convert to grayscale
        gray = 0.299 * img_batch[:, 0] + 0.587 * img_batch[:, 1] + 0.114 * img_batch[:, 2]

        # Use intensity as rough depth proxy (darker = further)
        depth = 1.0 - gray.squeeze().cpu().numpy()

        # Normalize to reasonable depth range
        depth = depth * 5.0 + 0.5  # Range ~0.5 to 5.5

        return depth

    def _compute_rays(self, h: int, w: int, fov: float = 60.0) -> np.ndarray:
        """Compute ray directions for a pinhole camera model."""
        # Compute focal length from FOV
        fx = w / (2 * np.tan(np.radians(fov) / 2))
        fy = fx  # Assume square pixels
        cx, cy = w / 2, h / 2

        # Create pixel grid
        u = np.arange(w)
        v = np.arange(h)
        u, v = np.meshgrid(u, v)

        # Compute ray directions
        x = (u - cx) / fx
        y = (v - cy) / fy
        z = np.ones_like(x)

        # Normalize
        rays = np.stack([x, y, z], axis=-1)
        rays = rays / np.linalg.norm(rays, axis=-1, keepdims=True)

        return rays

    def depth_rays_to_gaussians(
        self,
        depths: List[np.ndarray],
        rays: List[np.ndarray],
        colors: List[np.ndarray],
        scale_factor: float = 0.01,
        min_depth: float = 0.1,
        max_depth: float = 100.0,
        subsample: int = 1,
        confidence_percentile: float = 40.0,
    ) -> dict:
        """Convert depth-ray predictions to 3D Gaussian splats.

        Args:
            depths: List of depth maps
            rays: List of ray direction maps
            colors: List of RGB color arrays
            scale_factor: Base scale for Gaussians
            min_depth: Minimum valid depth
            max_depth: Maximum valid depth
            subsample: Pixel subsampling factor (1 = all pixels)

        Returns:
            Dictionary with Gaussian parameters
        """
        all_positions = []
        all_colors = []
        all_scales = []
        all_rotations = []
        all_opacities = []

        if len(depths) != len(rays) or len(depths) != len(colors):
            raise ValueError("depth, ray, and color lists must have the same view count")
        if not np.isfinite(scale_factor) or scale_factor <= 0.0:
            raise ValueError("scale_factor must be finite and positive")
        if (
            not np.isfinite(min_depth)
            or not np.isfinite(max_depth)
            or min_depth < 0.0
            or max_depth <= min_depth
        ):
            raise ValueError("depth bounds must be finite, with 0 <= min < max")
        if subsample < 1:
            raise ValueError("subsample must be at least 1")
        if not np.isfinite(confidence_percentile) or not 0.0 <= confidence_percentile <= 100.0:
            raise ValueError("confidence_percentile must be in [0, 100]")

        for view_idx, (depth, ray, color) in enumerate(zip(depths, rays, colors, strict=True)):
            depth = np.asarray(depth)
            ray = np.asarray(ray)
            color = np.asarray(color)
            if depth.ndim != 2:
                raise ValueError("each depth map must have shape (height, width)")
            h, w = depth.shape
            if ray.shape != (h, w, 3) or color.shape != (h, w, 3):
                raise ValueError("each ray and color map must match its depth map")
            confidence = (
                np.asarray(self.last_confidences[view_idx], dtype=np.float32)
                if view_idx < len(self.last_confidences)
                and self.last_confidences[view_idx] is not None
                else None
            )
            sky = (
                np.asarray(self.last_sky_masks[view_idx])
                if view_idx < len(self.last_sky_masks) and self.last_sky_masks[view_idx] is not None
                else None
            )
            if confidence is not None and confidence.shape != depth.shape:
                raise ValueError("each confidence map must match its depth map")
            if sky is not None and sky.shape != depth.shape:
                raise ValueError("each sky map must match its depth map")

            # Subsample if requested
            if subsample > 1:
                depth = depth[::subsample, ::subsample]
                ray = ray[::subsample, ::subsample]
                color = color[::subsample, ::subsample]
                if confidence is not None:
                    confidence = confidence[::subsample, ::subsample]
                if sky is not None:
                    sky = sky[::subsample, ::subsample]
                h, w = depth.shape

            # Create mask for valid depths
            valid_mask = (depth > min_depth) & (depth < max_depth) & np.isfinite(depth)
            if sky is not None:
                if not np.isfinite(sky).all():
                    raise ValueError("sky data contains a non-finite value")
                valid_mask &= sky < 0.5
            if confidence is not None:
                finite_conf = confidence[valid_mask & np.isfinite(confidence)]
                if not finite_conf.size:
                    raise ValueError("confidence data contains no finite value for a valid depth")
                threshold = np.percentile(finite_conf, confidence_percentile)
                valid_mask &= np.isfinite(confidence) & (confidence >= threshold)

            # Camera-aware world unprojection. In the primary path
            # prediction.gaussians is used directly; this fallback still needs
            # to respect each view's camera translation.
            origin = (
                self.last_ray_origins[view_idx]
                if view_idx < len(self.last_ray_origins)
                else np.zeros(3, dtype=np.float32)
            )
            positions = origin.reshape(1, 1, 3) + depth[..., np.newaxis] * ray

            # Flatten and filter
            positions_flat = positions[valid_mask]
            colors_flat = color[valid_mask]
            depths_flat = depth[valid_mask]

            if len(positions_flat) == 0:
                continue
            if not np.isfinite(positions_flat).all():
                raise ValueError("ray geometry produced a non-finite position")
            if not np.isfinite(colors_flat).all() or np.any(
                (colors_flat < 0.0) | (colors_flat > 1.0)
            ):
                raise ValueError("retained colors must be finite values in [0, 1]")

            # Compute adaptive scales based on depth and local density
            # Further points should have larger splats
            scales = scale_factor * (depths_flat / np.median(depths_flat))
            scales = np.clip(scales, scale_factor * 0.1, scale_factor * 10.0)

            # Create isotropic scales (same in all directions)
            scales_3d = np.stack([scales, scales, scales], axis=-1)

            # Identity rotations (quaternion: w, x, y, z)
            rotations = np.zeros((len(positions_flat), 4))
            rotations[:, 0] = 1.0  # w = 1, others = 0 (identity)

            # Opacity (higher for confident depths)
            opacities = np.ones(len(positions_flat)) * 0.9

            all_positions.append(positions_flat)
            all_colors.append(colors_flat)
            all_scales.append(scales_3d)
            all_rotations.append(rotations)
            all_opacities.append(opacities)

        # Concatenate all views
        if not all_positions:
            print("Warning: No valid Gaussians generated. Check depth range and input images.")
            print(f"  - min_depth: {min_depth}, max_depth: {max_depth}")
            print("  - Try adjusting --min-depth and --max-depth parameters.")
            return {
                "positions": np.zeros((0, 3)),
                "colors": np.zeros((0, 3)),
                "scales": np.zeros((0, 3)),
                "rotations": np.zeros((0, 4)),
                "opacities": np.zeros((0,)),
            }

        positions = np.concatenate(all_positions, axis=0)
        colors = np.concatenate(all_colors, axis=0)
        scales = np.concatenate(all_scales, axis=0)
        rotations = np.concatenate(all_rotations, axis=0)
        opacities = np.concatenate(all_opacities, axis=0)

        return {
            "positions": positions,
            "colors": colors,
            "scales": scales,
            "rotations": rotations,
            "opacities": opacities,
        }

    def gaussians_from_prediction(self, subsample: int = 1) -> Optional[dict]:
        """Extract the model's directly-estimated Gaussians as a dict.

        When DA3 runs with infer_gs=True, the model emits a full Gaussian
        Splatting cloud (means, scales, rotations, SH, opacity) in world space.
        This is strictly higher fidelity than re-deriving geometry from
        depth x ray (the depth_rays_to_gaussians path), which throws away the
        model's estimated scales, rotations, SH, and opacity. This method is
        the preferred source of Gaussians when available.

        Returns None if no Prediction is cached (e.g. the model was unavailable
        and the depth fallback ran), so callers can fall back to the
        depth x ray path.

        The field transforms match ByteDance's export_ply() in
        utils/gsply_helpers.py so the output is byte-compatible with the DA3
        reference exporter:
          - means            -> positions (world)
          - harmonics        -> complete degree 0-4 SH data
          - scales           -> scales (already linear; save_ply logs them)
          - rotations        -> rotations (wxyz, already world space)
          - opacities        -> opacities (already [0,1]; save_ply logits them)
        """
        if subsample < 1:
            raise ValueError("subsample must be at least 1")
        pred = getattr(self, "last_prediction", None)
        if pred is None or getattr(pred, "gaussians", None) is None:
            return None
        g = pred.gaussians
        try:
            depth = np.asarray(pred.depth)
            if depth.ndim == 2:
                depth = depth[None]
            if depth.ndim != 3:
                raise ValueError(f"expected prediction.depth (V,H,W), got {depth.shape}")
            views, height, width = depth.shape
            means = g.means.detach().cpu().reshape(-1, 3).contiguous().numpy()
            # harmonics: (batch, splats, channels, coefficients).
            harmonics = g.harmonics.detach().cpu().contiguous().numpy()
            if harmonics.ndim != 4 or harmonics.shape[2] != 3:
                raise ValueError(
                    f"expected Gaussian SH shape (batch, splats, 3, coefficients), "
                    f"got {harmonics.shape}"
                )
            harmonics = harmonics.reshape(-1, 3, harmonics.shape[-1])
            if harmonics.shape[2] not in SUPPORTED_SH_COEFFICIENT_COUNTS:
                raise ValueError(
                    "Gaussian SH data must contain 1, 4, 9, 16, or 25 coefficients per channel"
                )
            scales = g.scales.detach().cpu().reshape(-1, 3).contiguous().numpy()
            # rotations are wxyz per the Gaussians dataclass.
            rots = g.rotations.detach().cpu().reshape(-1, 4).contiguous().numpy()
            opac = g.opacities.detach().cpu().reshape(-1).contiguous().numpy()
            expected = views * height * width
            if not all(
                array.shape[0] == expected for array in (means, harmonics, scales, rots, opac)
            ):
                raise ValueError(
                    f"Gaussian grid has inconsistent size; expected {expected} "
                    f"from depth but got {means.shape[0]}"
                )
        except Exception as e:
            raise RuntimeError(
                "DA3 returned Gaussian data that the adapter could not preserve"
            ) from e

        if means.shape[0] == 0:
            raise RuntimeError("DA3 returned an empty Gaussian grid")

        # Use upstream's border trim. Preserve valid far-depth Gaussians and
        # complete SH data instead of applying the exporter's lossy defaults.
        # Keep the (V,H,W) layout until after 2-D pixel subsampling.
        mask = np.ones(depth.shape, dtype=bool)
        trim_h = int(8 / 256 * height)
        trim_w = int(8 / 256 * width)
        if trim_h > 0:
            mask[:, :trim_h, :] = False
            mask[:, -trim_h:, :] = False
        if trim_w > 0:
            mask[:, :, :trim_w] = False
            mask[:, :, -trim_w:] = False
        if np.any(~np.isfinite(depth[mask])) or np.any(depth[mask] <= 0.0):
            raise RuntimeError("DA3 returned invalid depth for a retained Gaussian")
        if subsample > 1:
            sampled = np.zeros_like(mask)
            sampled[:, ::subsample, ::subsample] = True
            mask &= sampled
        selected = mask.reshape(-1)
        means, harmonics, scales, rots, opac = (
            array[selected] for array in (means, harmonics, scales, rots, opac)
        )
        if means.shape[0] == 0:
            raise RuntimeError("all model-direct Gaussians failed the depth filter")

        # DC is already in SH space; invert to [0,1] rgb so save_ply's
        # rgb->SH-DC conversion round-trips cleanly.
        rgb = harmonics[:, :, 0] * SH_C0 + 0.5

        return validated_gaussian_arrays(
            {
                "positions": means,
                "colors": rgb,
                "scales": scales,
                "rotations": rots,
                "opacities": opac,
                "sh_coefficients": harmonics,
            }
        )


def validated_gaussian_arrays(gaussians: dict) -> dict[str, np.ndarray]:
    """Return finite float32 Gaussian arrays with matching shapes."""
    required_shapes = {
        "positions": 3,
        "colors": 3,
        "scales": 3,
        "rotations": 4,
    }
    arrays: dict[str, np.ndarray] = {}
    count: Optional[int] = None
    try:
        for name, width in required_shapes.items():
            source = np.asarray(gaussians[name])
            if source.ndim != 2 or source.shape[1] != width:
                raise ValueError(f"{name} must have shape (splats, {width})")
            if not np.isfinite(source).all():
                raise ValueError(f"{name} contains a non-finite value")
            with np.errstate(over="ignore", invalid="ignore"):
                converted = source.astype(np.float32)
            if not np.isfinite(converted).all():
                raise ValueError(f"{name} exceeds the float32 range")
            count = converted.shape[0] if count is None else count
            if converted.shape[0] != count:
                raise ValueError("Gaussian arrays have different splat counts")
            arrays[name] = converted

        opacity_source = np.asarray(gaussians["opacities"])
        if opacity_source.ndim != 1 or opacity_source.shape[0] != count:
            raise ValueError("opacities must have shape (splats,)")
        if not np.isfinite(opacity_source).all():
            raise ValueError("opacities contains a non-finite value")
        with np.errstate(over="ignore", invalid="ignore"):
            arrays["opacities"] = opacity_source.astype(np.float32)
        if not np.isfinite(arrays["opacities"]).all():
            raise ValueError("opacities exceeds the float32 range")

        sh_source = gaussians.get("sh_coefficients")
        if sh_source is not None:
            sh = np.asarray(sh_source)
            if (
                sh.ndim != 3
                or sh.shape[:2] != (count, 3)
                or sh.shape[2] not in SUPPORTED_SH_COEFFICIENT_COUNTS
            ):
                raise ValueError("SH data must have shape (splats, 3, 1|4|9|16|25)")
            if not np.isfinite(sh).all():
                raise ValueError("SH data contains a non-finite value")
            with np.errstate(over="ignore", invalid="ignore"):
                arrays["sh_coefficients"] = sh.astype(np.float32)
            if not np.isfinite(arrays["sh_coefficients"]).all():
                raise ValueError("SH data exceeds the float32 range")
    except (KeyError, TypeError) as exc:
        raise ValueError("Gaussian data does not contain valid numeric arrays") from exc

    if count == 0:
        raise ValueError("cannot write an empty Gaussian output")
    if np.any(arrays["scales"] <= 0.0):
        raise ValueError("Gaussian scales must be positive")
    if np.any((arrays["opacities"] < 0.0) | (arrays["opacities"] > 1.0)):
        raise ValueError("Gaussian opacities must be in [0, 1]")
    if "sh_coefficients" not in arrays and np.any(
        (arrays["colors"] < 0.0) | (arrays["colors"] > 1.0)
    ):
        raise ValueError("Gaussian colors must be in [0, 1]")
    rotation_norms = np.linalg.norm(arrays["rotations"].astype(np.float64), axis=1)
    if np.any(~np.isfinite(rotation_norms)) or np.any(rotation_norms < 1e-12):
        raise ValueError("Gaussian rotations must be nonzero finite quaternions")
    arrays["rotations"] = (
        arrays["rotations"].astype(np.float64) / rotation_norms[:, np.newaxis]
    ).astype(np.float32)
    return arrays


def save_ply(gaussians: dict, output_path: str):
    """Save Gaussians to PLY format compatible with 3DGS viewers.

    Uses efficient numpy structured array writes instead of per-element writes.
    """
    arrays = validated_gaussian_arrays(gaussians)
    positions = arrays["positions"]
    colors = arrays["colors"]
    scales = arrays["scales"]
    rotations = arrays["rotations"]
    opacities = arrays["opacities"]

    n = len(positions)

    sh_coefficients = arrays.get("sh_coefficients")
    if sh_coefficients is None:
        sh_dc = ((colors - 0.5) / SH_C0).astype(np.float32)
        sh_coefficients = sh_dc[:, :, None]
    else:
        sh_coefficients = np.asarray(sh_coefficients, dtype=np.float32)
        if (
            sh_coefficients.ndim != 3
            or sh_coefficients.shape[:2] != (n, 3)
            or sh_coefficients.shape[2] not in SUPPORTED_SH_COEFFICIENT_COUNTS
        ):
            raise ValueError("SH data must have shape (splats, 3, 1|4|9|16|25)")
        sh_dc = sh_coefficients[:, :, 0]
    sh_rest = sh_coefficients[:, :, 1:].reshape(n, -1)

    # Convert scales to log space
    scales_log = np.log(scales).astype(np.float32)
    if not np.isfinite(scales_log).all():
        raise ValueError("Gaussian scales cannot be encoded as finite float32 logarithms")

    # Convert opacities to logit space
    if np.any((opacities <= 0.0) | (opacities >= 1.0)):
        raise ValueError("PLY opacity must be strictly between 0 and 1 for finite logits")
    opacities_logit = np.log(opacities / (1 - opacities)).astype(np.float32)

    # Zero normals declare that this point layout has no vertex-normal semantics.
    normals = np.zeros((n, 3), dtype=np.float32)

    # Build structured array for efficient binary write
    fields = [
        ("x", "<f4"),
        ("y", "<f4"),
        ("z", "<f4"),
        ("nx", "<f4"),
        ("ny", "<f4"),
        ("nz", "<f4"),
        ("f_dc_0", "<f4"),
        ("f_dc_1", "<f4"),
        ("f_dc_2", "<f4"),
    ]
    fields.extend((f"f_rest_{index}", "<f4") for index in range(sh_rest.shape[1]))
    fields.extend(
        [
            ("opacity", "<f4"),
            ("scale_0", "<f4"),
            ("scale_1", "<f4"),
            ("scale_2", "<f4"),
            ("rot_0", "<f4"),
            ("rot_1", "<f4"),
            ("rot_2", "<f4"),
            ("rot_3", "<f4"),
        ]
    )
    vertex_data = np.zeros(n, dtype=fields)

    vertex_data["x"] = positions[:, 0]
    vertex_data["y"] = positions[:, 1]
    vertex_data["z"] = positions[:, 2]
    vertex_data["nx"] = normals[:, 0]
    vertex_data["ny"] = normals[:, 1]
    vertex_data["nz"] = normals[:, 2]
    vertex_data["f_dc_0"] = sh_dc[:, 0]
    vertex_data["f_dc_1"] = sh_dc[:, 1]
    vertex_data["f_dc_2"] = sh_dc[:, 2]
    for index in range(sh_rest.shape[1]):
        vertex_data[f"f_rest_{index}"] = sh_rest[:, index]
    vertex_data["opacity"] = opacities_logit
    vertex_data["scale_0"] = scales_log[:, 0]
    vertex_data["scale_1"] = scales_log[:, 1]
    vertex_data["scale_2"] = scales_log[:, 2]
    vertex_data["rot_0"] = rotations[:, 0]
    vertex_data["rot_1"] = rotations[:, 1]
    vertex_data["rot_2"] = rotations[:, 2]
    vertex_data["rot_3"] = rotations[:, 3]

    # Write PLY header and data
    rest_header = "".join(f"property float f_rest_{index}\n" for index in range(sh_rest.shape[1]))
    sh_degree = int(round(np.sqrt(sh_coefficients.shape[2]))) - 1
    header = f"""ply
format binary_little_endian 1.0
comment melkor_profile da3-gaussian-v1
comment melkor_coordinate_system ply-rdf
comment melkor_quaternion_order wxyz
comment melkor_scale_domain log
comment melkor_opacity_domain logit
comment melkor_sh_basis real_condon_shortley
comment melkor_sh_degree {sh_degree}
element vertex {n}
property float x
property float y
property float z
property float nx
property float ny
property float nz
property float f_dc_0
property float f_dc_1
property float f_dc_2
{rest_header}\
property float opacity
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
end_header
"""

    with open(output_path, "wb") as f:
        f.write(header.encode("utf-8"))
        vertex_data.tofile(f)

    print(f"Saved {n} Gaussians to {output_path}")


def save_json(gaussians: dict, output_path: str):
    """Save Gaussians to JSON format for debugging."""
    gaussians = validated_gaussian_arrays(gaussians)
    included = min(len(gaussians["positions"]), 1000)
    output = {
        "num_gaussians": len(gaussians["positions"]),
        "included_gaussians": included,
        "truncated": included < len(gaussians["positions"]),
        "sh_coefficient_count": int(
            gaussians.get("sh_coefficients", np.empty((0, 0, 1))).shape[-1]
        ),
        "gaussians": [],
    }

    for i in range(included):  # Keep the debugging representation bounded.
        item = {
            "position": gaussians["positions"][i].tolist(),
            "color": gaussians["colors"][i].tolist(),
            "scale": gaussians["scales"][i].tolist(),
            "rotation": gaussians["rotations"][i].tolist(),
            "opacity": float(gaussians["opacities"][i]),
        }
        if "sh_coefficients" in gaussians:
            item["sh_coefficients"] = gaussians["sh_coefficients"][i].tolist()
        output["gaussians"].append(item)

    with open(output_path, "w", encoding="utf-8", newline="") as f:
        json.dump(output, f, allow_nan=False, indent=2)
        f.write("\n")

    print(
        f"Saved JSON preview with {included}/{output['num_gaussians']} Gaussians to {output_path}"
    )


def save_npz(gaussians: dict, output_path: str):
    """Save Gaussians to NPZ format (compressed numpy arrays)."""
    gaussians = validated_gaussian_arrays(gaussians)
    arrays = dict(
        positions=gaussians["positions"],
        colors=gaussians["colors"],
        scales=gaussians["scales"],
        rotations=gaussians["rotations"],
        opacities=gaussians["opacities"],
    )
    if "sh_coefficients" in gaussians:
        arrays["sh_coefficients"] = gaussians["sh_coefficients"]
    np.savez_compressed(output_path, **arrays)
    print(f"Saved {len(gaussians['positions'])} Gaussians to {output_path}")


def save_glb(gaussians: dict, output_path: str):
    """Save Gaussians as GLB point cloud (requires trimesh)."""
    gaussians = validated_gaussian_arrays(gaussians)
    print(
        "Warning: GLB output is a colored point-cloud preview. "
        "It does not preserve Gaussian scale, rotation, opacity, or SH data.",
        file=sys.stderr,
    )
    try:
        import trimesh

        # Create point cloud
        cloud = trimesh.PointCloud(
            vertices=gaussians["positions"],
            colors=np.clip(gaussians["colors"] * 255, 0, 255).astype(np.uint8),
        )

        # Export as GLB
        cloud.export(output_path, file_type="glb")
        print(f"Saved {len(gaussians['positions'])} points to {output_path}")
    except ImportError as exc:
        raise RuntimeError(
            "GLB output requires trimesh; install it or request .ply explicitly"
        ) from exc


def load_atomic_publisher():
    """Load the repository's shared atomic publication function."""
    publisher_path = Path(__file__).resolve().parents[1] / "atomic_publish.py"
    spec = importlib.util.spec_from_file_location("melkor_da3_atomic_publish", publisher_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load the atomic publish tool")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as error:
        raise RuntimeError("cannot load the atomic publish tool") from error
    publish = getattr(module, "publish", None)
    if not callable(publish):
        raise RuntimeError("the atomic publish tool does not provide publish")
    return publish


def save_output_atomic(
    gaussians: dict,
    output_path: Path,
    force: bool,
    allow_lossy_preview: bool = False,
) -> None:
    """Write one output through a same-directory temporary file."""
    output_path = Path(os.path.abspath(output_path))
    parent = output_path.parent
    if not parent.is_dir():
        raise RuntimeError(f"output directory does not exist: {parent}")
    parent = parent.resolve(strict=True)
    output_path = parent / output_path.name
    if output_path.is_symlink():
        raise RuntimeError("refusing a symbolic-link output path")
    if output_path.exists() and not stat.S_ISREG(output_path.stat().st_mode):
        raise RuntimeError("an existing output must be a regular file")
    if output_path.exists() and not force:
        raise FileExistsError(f"output exists; use --force to replace it: {output_path}")

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output_path.stem}.", suffix=output_path.suffix, dir=parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        suffix = output_path.suffix.lower()
        if suffix == ".json":
            save_json(gaussians, str(temporary))
        elif suffix == ".npz":
            save_npz(gaussians, str(temporary))
        elif suffix == ".glb":
            if not allow_lossy_preview:
                raise RuntimeError("GLB preview output requires --allow-lossy-preview")
            save_glb(gaussians, str(temporary))
        elif suffix == ".ply":
            save_ply(gaussians, str(temporary))
        else:
            raise RuntimeError(f"unsupported output suffix: {suffix}")

        flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        artifact_descriptor = os.open(temporary, flags)
        try:
            if not stat.S_ISREG(os.fstat(artifact_descriptor).st_mode):
                raise RuntimeError("the staged output is not a regular file")
            os.fsync(artifact_descriptor)
        finally:
            os.close(artifact_descriptor)
        temporary.chmod(0o644)
        load_atomic_publisher()(temporary, output_path, force)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(
        description="Depth-Anything-3 to 3D Gaussian Splats",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Single image
  python inference.py --input photo.jpg --output scene.ply
  
  # Directory of images
  python inference.py --input images/ --output scene.ply
  
  # With specific model
  python inference.py --model da3-large-1.1 --input images/ --output scene.ply
  
  # Adjust Gaussian scale
  python inference.py --input images/ --output scene.ply --scale 0.005
  
  # Subsample for faster processing
  python inference.py --input images/ --output scene.ply --subsample 2
""",
    )

    parser.add_argument("--input", "-i", required=True, help="Input image or directory of images")
    parser.add_argument(
        "--output", "-o", required=True, help="Output file (.ply, .json, .npz, or .glb)"
    )
    parser.add_argument(
        "--model",
        "-m",
        default=DEFAULT_MODEL,
        choices=[
            "DA3-SMALL",
            "DA3-BASE",
            "DA3-LARGE-1.1",
            "DA3-GIANT-1.1",
            "DA3NESTED-GIANT-LARGE-1.1",
            "da3-small",
            "da3-base",
            "da3-large",
            "da3-large-1.1",
            "da3-giant",
            "da3-giant-1.1",
            "da3nested-giant-large",
            "da3nested-giant-large-1.1",
        ],
        help=f"DA3 model to use (default: {DEFAULT_MODEL})",
    )
    parser.add_argument(
        "--model-dir", default=DEFAULT_MODEL_DIR, help="Directory containing model weights"
    )
    parser.add_argument(
        "--device", default="cuda", choices=["cuda", "cpu"], help="Device to use (cuda, cpu)"
    )
    parser.add_argument(
        "--scale", type=float, default=0.01, help="Base scale for Gaussians (default: 0.01)"
    )
    parser.add_argument(
        "--subsample",
        type=int,
        default=1,
        help="Pixel subsampling factor (default: 1, use all pixels)",
    )
    parser.add_argument(
        "--confidence-percentile",
        type=float,
        default=40.0,
        help="Discard depth pixels below this confidence percentile "
        "(default: 40; depth-derived splats only)",
    )
    parser.add_argument(
        "--min-depth", type=float, default=0.1, help="Minimum valid depth (default: 0.1)"
    )
    parser.add_argument(
        "--max-depth", type=float, default=100.0, help="Maximum valid depth (default: 100.0)"
    )
    parser.add_argument("--fp32", action="store_true", help="Use FP32 instead of FP16")
    parser.add_argument(
        "--allow-fallback-depth",
        action="store_true",
        help="Permit the intensity-based depth fallback when the DA3 "
        "model is unavailable. The fallback produces PREVIEW-ONLY "
        "output that is NOT suitable for reconstruction; it is "
        "disabled by default to prevent silent low-quality results.",
    )
    parser.add_argument(
        "--allow-lossy-preview",
        action="store_true",
        help="Permit GLB point-cloud output that drops Gaussian attributes",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Replace an existing regular output file",
    )

    args = parser.parse_args()

    if not np.isfinite(args.scale) or args.scale <= 0:
        parser.error("--scale must be a finite value > 0")
    if args.subsample < 1:
        parser.error("--subsample must be >= 1")
    if not np.isfinite(args.confidence_percentile) or not 0 <= args.confidence_percentile <= 100:
        parser.error("--confidence-percentile must be finite and in [0, 100]")
    if (
        not np.isfinite(args.min_depth)
        or not np.isfinite(args.max_depth)
        or args.min_depth < 0
        or args.max_depth <= args.min_depth
    ):
        parser.error("--min-depth and --max-depth must be finite, with 0 <= min < max")

    output_path = Path(args.output)
    supported_suffixes = {".ply", ".json", ".npz", ".glb"}
    if output_path.suffix.lower() not in supported_suffixes:
        parser.error(
            "unsupported output extension; choose .ply, .json, .npz, or .glb. "
            "For SPZ, write .ply first and use the melkor convert command."
        )
    if output_path.suffix.lower() == ".glb" and not args.allow_lossy_preview:
        parser.error(".glb drops Gaussian attributes; add --allow-lossy-preview to approve it")
    if output_path.is_symlink():
        parser.error("the output path must not be a symbolic link")
    if output_path.exists() and not args.force:
        parser.error("the output already exists; use --force to replace it")

    # Check CUDA availability
    if args.device == "cuda" and not torch.cuda.is_available():
        print("Warning: CUDA not available, falling back to CPU")
        args.device = "cpu"
        args.fp32 = True

    if args.device == "cuda":
        print(f"Using GPU: {torch.cuda.get_device_name(0)}")
        print(f"GPU Memory: {torch.cuda.get_device_properties(0).total_memory / 1e9:.1f} GB")

    # Get image files
    image_files = get_image_files(args.input)
    print(f"Found {len(image_files)} image(s)")

    if len(image_files) == 0:
        print("Error: No images found")
        sys.exit(1)
    with snapshot_image_files(image_files) as (validated_images, dimensions):
        if len(set(dimensions)) > 1:
            print("Warning: input images have different dimensions.")
            print("DA3 will resize them internally.")

        # Initialize generator
        dtype = torch.float32 if args.fp32 else torch.float16
        generator = DA3GaussianGenerator(
            model_name=args.model,
            model_dir=args.model_dir,
            device=args.device,
            dtype=dtype,
            allow_fallback_depth=args.allow_fallback_depth,
        )
        generator.load_model()

        # Predict depth and rays from the validated snapshots.
        start_time = time.time()
        depths, rays, colors = generator.predict_depth_rays(None, validated_images)
        depth_time = time.time() - start_time
    per_image_time = depth_time / len(image_files)
    print(f"Depth prediction: {depth_time:.2f}s ({per_image_time:.2f}s per image)")

    # Convert to Gaussians. Prefer the model's directly-estimated cloud
    # (means/scales/rotations/SH/opacity in world space) when infer_gs ran
    # successfully; this is strictly higher fidelity than re-deriving geometry
    # from depth x ray. Fall back to the depth path only when the direct
    # Gaussians are unavailable (model absent, or infer_gs didn't populate).
    start_time = time.time()
    gaussians = generator.gaussians_from_prediction(subsample=args.subsample)
    if gaussians is not None:
        print("Using model-direct Gaussians (infer_gs branch).")
    else:
        gaussians = generator.depth_rays_to_gaussians(
            depths,
            rays,
            colors,
            scale_factor=args.scale,
            min_depth=args.min_depth,
            max_depth=args.max_depth,
            subsample=args.subsample,
            confidence_percentile=args.confidence_percentile,
        )
    convert_time = time.time() - start_time
    print(f"Gaussian conversion: {convert_time:.2f}s")
    print(f"Generated {len(gaussians['positions'])} Gaussians")
    if len(gaussians["positions"]) == 0:
        raise RuntimeError("DA3 produced no valid Gaussians; refusing to write an empty output")

    save_output_atomic(
        gaussians,
        output_path,
        args.force,
        allow_lossy_preview=args.allow_lossy_preview,
    )

    print(f"\nTotal time: {depth_time + convert_time:.2f}s")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(1) from None
