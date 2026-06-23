#!/usr/bin/env python3
"""
Depth-Anything-3 Single-GPU Inference Script
Converts images to 3D Gaussian Splats using DA3's depth-ray representation.

Usage:
    python inference.py --input images/ --output output.ply
    python inference.py --input image.jpg --output output.ply --model DA3-LARGE
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple

import numpy as np
import torch
from PIL import Image
from tqdm import tqdm

# Default paths
DEFAULT_MODEL_DIR = os.path.expanduser("~/.melkor/models/da3")
DEFAULT_MODEL = "DA3-BASE"

# Spherical-harmonics band-0 constant. Single source of truth for the 3DGS
# color convention used by both the C++ core (melkor::utils::SH_C0) and these
# Python tools. Value: 1 / (2 * sqrt(pi)). Keep in sync with
# include/melkor/gaussian_data.hpp.
SH_C0 = 0.28209479177387814


def get_image_files(input_path: str) -> List[Path]:
    """Get list of image files from input path."""
    input_path = Path(input_path)
    
    if input_path.is_file():
        return [input_path]
    
    if input_path.is_dir():
        extensions = {'.jpg', '.jpeg', '.png', '.webp', '.tiff', '.tif', '.bmp'}
        files = []
        for ext in extensions:
            files.extend(input_path.glob(f'*{ext}'))
            files.extend(input_path.glob(f'*{ext.upper()}'))
        return sorted(files)
    
    raise ValueError(f"Input path does not exist: {input_path}")


def load_image(path: Path, size: Optional[Tuple[int, int]] = None) -> torch.Tensor:
    """Load and preprocess an image."""
    img = Image.open(path).convert('RGB')
    
    if size is not None:
        img = img.resize(size, Image.LANCZOS)
    
    # Convert to tensor and normalize
    img_np = np.array(img).astype(np.float32) / 255.0
    img_tensor = torch.from_numpy(img_np).permute(2, 0, 1)  # HWC -> CHW
    
    return img_tensor, img_np


class DA3GaussianGenerator:
    """Generate 3D Gaussian Splats from images using Depth-Anything-3."""
    
    # Class-level flag to show fallback warning only once
    _fallback_warned = False
    
    # Model name normalization map
    MODEL_NAME_MAP = {
        'da3-small': 'DA3-SMALL',
        'da3-base': 'DA3-BASE',
        'da3-large': 'DA3-LARGE',
        'da3-giant': 'DA3-GIANT',
        'da3mono-large': 'DA3MONO-LARGE',
        'da3metric-large': 'DA3METRIC-LARGE',
        'da3nested-giant-large': 'DA3NESTED-GIANT-LARGE',
    }
    
    # Models that support multi-view input
    MULTI_VIEW_MODELS = {'DA3-SMALL', 'DA3-BASE', 'DA3-LARGE', 'DA3-GIANT', 'DA3NESTED-GIANT-LARGE'}
    
    # Models that output metric depth (in meters)
    METRIC_MODELS = {'DA3METRIC-LARGE', 'DA3NESTED-GIANT-LARGE'}
    
    # Models optimized for monocular (single-view) input
    MONOCULAR_MODELS = {'DA3MONO-LARGE', 'DA3METRIC-LARGE'}
    
    def __init__(
        self,
        model_name: str = DEFAULT_MODEL,
        model_dir: str = DEFAULT_MODEL_DIR,
        device: str = "cuda",
        dtype: torch.dtype = torch.float16,
        allow_fallback_depth: bool = False
    ):
        self.device = device
        self.dtype = dtype
        # Normalize model name to uppercase canonical form
        self.model_name = self.MODEL_NAME_MAP.get(model_name.lower(), model_name.upper())
        self.model_dir = Path(model_dir)
        self.model = None
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
            
            if model_path.exists():
                from depth_anything_3.api import DepthAnything3
                self.model = DepthAnything3.from_pretrained(str(model_path))
            else:
                # Try loading from HuggingFace
                from depth_anything_3.api import DepthAnything3
                # Handle special model name formats for HuggingFace
                hf_model_id = f"depth-anything/{self.model_name}"
                print(f"Model not found locally, loading from HuggingFace: {hf_model_id}")
                self.model = DepthAnything3.from_pretrained(hf_model_id)
            
            self.model = self.model.to(device=self.device, dtype=self.dtype)
            self.model.eval()
            print(f"Model loaded on {self.device}")
            
        except ImportError as e:
            print(f"Warning: depth_anything_3 not found ({e}). Using fallback depth estimation.")
            self.model = None
        except Exception as e:
            print(f"Warning: Could not load DA3 model: {e}")
            print("Using fallback depth estimation.")
            self.model = None
    
    def predict_depth_rays(
        self,
        images: List[torch.Tensor],
        image_paths: List[Path]
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
            # view, i.e. depth has shape (N, H, W). infer_gs=True enables the
            # Gaussian branch; it also forces depth/extrinsics/intrinsics to be
            # populated consistently across views.
            try:
                with torch.no_grad():
                    prediction = self.model.inference(
                        [str(p) for p in image_paths],
                        infer_gs=True,
                    )
                depth_stack = np.asarray(prediction.depth)  # (N, H, W)
                if depth_stack.ndim == 2:
                    # Single view came back unbatched; reintroduce the view axis.
                    depth_stack = depth_stack[None]
            except Exception as e:
                print(f"Warning: DA3 multi-view inference failed: {e}")
                depth_stack = None
        else:
            depth_stack = None

        # Per-image fallback only if the model is missing or the batched call
        # failed. This keeps the old code path alive for degraded environments
        # but is explicitly NOT the primary route.
        if depth_stack is None:
            print("Falling back to per-image depth estimation.")
            with torch.no_grad():
                for img_tensor, _img_path in tqdm(
                    zip(images, image_paths, strict=False),
                    total=len(images),
                    desc="Predicting depth",
                ):
                    img_batch = img_tensor.unsqueeze(0).to(self.device, self.dtype)
                    depth = self._fallback_depth(img_batch)
                    depths.append(depth)
                    rays.append(self._compute_rays(*depth.shape))
                    colors.append(img_tensor.permute(1, 2, 0).numpy())
            return depths, rays, colors

        # Primary path: slice the batched prediction per view.
        n_views = depth_stack.shape[0]
        ext_np = np.asarray(prediction.extrinsics) if getattr(prediction, "extrinsics", None) is not None else None
        ixt_np = np.asarray(prediction.intrinsics) if getattr(prediction, "intrinsics", None) is not None else None
        for view_idx in range(n_views):
            depth = depth_stack[view_idx]
            depths.append(depth)

            h, w = depth.shape
            ray = self._ray_map_for_view(view_idx, h, w, ext_np, ixt_np)
            rays.append(ray)

            # Recover the (possibly resized) color image. The model's processed
            # images are the ground truth for what the network actually saw.
            if (
                getattr(prediction, "processed_images", None) is not None
                and view_idx < len(prediction.processed_images)
            ):
                colors.append(
                    np.asarray(prediction.processed_images[view_idx], dtype=np.float32) / 255.0
                )
            else:
                colors.append(images[view_idx].permute(1, 2, 0).numpy())

        return depths, rays, colors

    def _ray_map_for_view(
        self,
        view_idx: int,
        h: int,
        w: int,
        extrinsics: "np.ndarray | None",
        intrinsics: "np.ndarray | None",
    ) -> np.ndarray:
        """Build a per-pixel world-space ray direction map for one view.

        When DA3 provides camera extrinsics (w2c, (4,4)) and intrinsics (3,3)
        we back out the ray directions from the pinhole model and rotate them
        into world space -- this is the geometrically correct ray and replaces
        the old FOV-guessed _compute_rays fallback. If either is missing we
        keep the FOV-based approximation so single-image/metric-only models
        still produce output.
        """
        if extrinsics is not None and intrinsics is not None:
            try:
                ext = extrinsics[view_idx] if extrinsics.ndim >= 3 else extrinsics
                ixt = intrinsics[view_idx] if intrinsics.ndim >= 3 else intrinsics
                w2c = np.eye(4, dtype=np.float64)
                w2c[: ext.shape[0], : ext.shape[1]] = ext
                c2w = np.linalg.inv(w2c)
                fx, fy = float(ixt[0, 0]), float(ixt[1, 1])
                cx, cy = float(ixt[0, 2]), float(ixt[1, 2])
                u = np.arange(w, dtype=np.float64)
                v = np.arange(h, dtype=np.float64)
                uu, vv = np.meshgrid(u, v)
                # Camera-space ray (before normalization).
                dir_cam = np.stack(
                    [(uu - cx) / fx, (vv - cy) / fy, np.ones_like(uu)], axis=-1
                )
                dir_cam /= np.linalg.norm(dir_cam, axis=-1, keepdims=True)
                # Rotate into world space (extrinsics translate too, but ray
                # directions are translation-invariant).
                r_c2w = c2w[:3, :3].astype(np.float64)
                dir_world = dir_cam @ r_c2w.T
                return dir_world.astype(np.float32)
            except Exception as e:
                print(f"Warning: ray derivation from camera params failed ({e}); using FOV fallback.")
        return self._compute_rays(h, w)

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
            print("\n" + "="*70)
            print("WARNING: Using FALLBACK depth estimation (intensity-based)")
            print("This produces PREVIEW-ONLY results unsuitable for 3D")
            print("reconstruction. Install the DA3 model: ./scripts/setup_da3.sh")
            print("="*70 + "\n")

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
        subsample: int = 1
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
        
        for depth, ray, color in zip(depths, rays, colors, strict=True):
            h, w = depth.shape
            
            # Subsample if requested
            if subsample > 1:
                depth = depth[::subsample, ::subsample]
                ray = ray[::subsample, ::subsample]
                color = color[::subsample, ::subsample]
                h, w = depth.shape
            
            # Create mask for valid depths
            valid_mask = (depth > min_depth) & (depth < max_depth) & np.isfinite(depth)
            
            # Compute 3D positions: P = depth * ray
            positions = depth[..., np.newaxis] * ray
            
            # Flatten and filter
            positions_flat = positions[valid_mask]
            colors_flat = color[valid_mask]
            depths_flat = depth[valid_mask]
            
            if len(positions_flat) == 0:
                continue
            
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
                'positions': np.zeros((0, 3)),
                'colors': np.zeros((0, 3)),
                'scales': np.zeros((0, 3)),
                'rotations': np.zeros((0, 4)),
                'opacities': np.zeros((0,))
            }
        
        positions = np.concatenate(all_positions, axis=0)
        colors = np.concatenate(all_colors, axis=0)
        scales = np.concatenate(all_scales, axis=0)
        rotations = np.concatenate(all_rotations, axis=0)
        opacities = np.concatenate(all_opacities, axis=0)
        
        return {
            'positions': positions,
            'colors': colors,
            'scales': scales,
            'rotations': rotations,
            'opacities': opacities
        }
    
    def fuse_multi_view(
        self,
        gaussians_list: List[dict],
        voxel_size: float = 0.05
    ) -> dict:
        """Fuse Gaussians from multiple views using voxel downsampling.
        
        This removes duplicate points from overlapping regions.
        """
        if len(gaussians_list) == 1:
            return gaussians_list[0]
        
        # Concatenate all Gaussians
        all_gaussians = {
            'positions': np.concatenate([g['positions'] for g in gaussians_list], axis=0),
            'colors': np.concatenate([g['colors'] for g in gaussians_list], axis=0),
            'scales': np.concatenate([g['scales'] for g in gaussians_list], axis=0),
            'rotations': np.concatenate([g['rotations'] for g in gaussians_list], axis=0),
            'opacities': np.concatenate([g['opacities'] for g in gaussians_list], axis=0)
        }
        
        # Voxel downsampling to remove duplicates
        try:
            import open3d as o3d
            
            pcd = o3d.geometry.PointCloud()
            pcd.points = o3d.utility.Vector3dVector(all_gaussians['positions'])
            pcd.colors = o3d.utility.Vector3dVector(all_gaussians['colors'])
            
            # Downsample
            downsampled = pcd.voxel_down_sample(voxel_size)
            
            # Get indices of kept points (approximate by nearest neighbor)
            tree = o3d.geometry.KDTreeFlann(downsampled)
            keep_indices = []
            for i, pt in enumerate(np.asarray(pcd.points)):
                _, idx, _ = tree.search_knn_vector_3d(pt, 1)
                if idx[0] not in keep_indices:
                    keep_indices.append(i)
            
            keep_indices = np.array(keep_indices[:len(downsampled.points)])
            
            return {
                'positions': all_gaussians['positions'][keep_indices],
                'colors': all_gaussians['colors'][keep_indices],
                'scales': all_gaussians['scales'][keep_indices],
                'rotations': all_gaussians['rotations'][keep_indices],
                'opacities': all_gaussians['opacities'][keep_indices]
            }
            
        except ImportError:
            print("Warning: open3d not available, skipping voxel fusion")
            return all_gaussians


def save_ply(gaussians: dict, output_path: str):
    """Save Gaussians to PLY format compatible with 3DGS viewers.
    
    Uses efficient numpy structured array writes instead of per-element writes.
    """
    positions = gaussians['positions'].astype(np.float32)
    colors = gaussians['colors'].astype(np.float32)
    scales = gaussians['scales'].astype(np.float32)
    rotations = gaussians['rotations'].astype(np.float32)
    opacities = gaussians['opacities'].astype(np.float32)
    
    n = len(positions)
    
    # Handle empty Gaussians edge case
    if n == 0:
        header = """ply
format binary_little_endian 1.0
comment Generated by Melkor DA3
element vertex 0
property float x
property float y
property float z
end_header
"""
        with open(output_path, 'wb') as f:
            f.write(header.encode('utf-8'))
        print(f"Saved 0 Gaussians to {output_path}")
        return
    
    # Convert colors to SH DC coefficients using the centralized SH_C0 constant
    # (kept in sync with include/melkor/gaussian_data.hpp).
    sh_dc = ((colors - 0.5) / SH_C0).astype(np.float32)
    
    # Convert scales to log space
    scales_log = np.log(np.maximum(scales, 1e-7)).astype(np.float32)
    
    # Convert opacities to logit space
    opacities_clamped = np.clip(opacities, 1e-4, 1 - 1e-4)
    opacities_logit = np.log(opacities_clamped / (1 - opacities_clamped)).astype(np.float32)
    
    # Create dummy normals (pointing up)
    normals = np.zeros((n, 3), dtype=np.float32)
    normals[:, 2] = 1.0
    
    # Build structured array for efficient binary write
    vertex_data = np.zeros(n, dtype=[
        ('x', '<f4'), ('y', '<f4'), ('z', '<f4'),
        ('nx', '<f4'), ('ny', '<f4'), ('nz', '<f4'),
        ('f_dc_0', '<f4'), ('f_dc_1', '<f4'), ('f_dc_2', '<f4'),
        ('opacity', '<f4'),
        ('scale_0', '<f4'), ('scale_1', '<f4'), ('scale_2', '<f4'),
        ('rot_0', '<f4'), ('rot_1', '<f4'), ('rot_2', '<f4'), ('rot_3', '<f4')
    ])
    
    vertex_data['x'] = positions[:, 0]
    vertex_data['y'] = positions[:, 1]
    vertex_data['z'] = positions[:, 2]
    vertex_data['nx'] = normals[:, 0]
    vertex_data['ny'] = normals[:, 1]
    vertex_data['nz'] = normals[:, 2]
    vertex_data['f_dc_0'] = sh_dc[:, 0]
    vertex_data['f_dc_1'] = sh_dc[:, 1]
    vertex_data['f_dc_2'] = sh_dc[:, 2]
    vertex_data['opacity'] = opacities_logit
    vertex_data['scale_0'] = scales_log[:, 0]
    vertex_data['scale_1'] = scales_log[:, 1]
    vertex_data['scale_2'] = scales_log[:, 2]
    vertex_data['rot_0'] = rotations[:, 0]
    vertex_data['rot_1'] = rotations[:, 1]
    vertex_data['rot_2'] = rotations[:, 2]
    vertex_data['rot_3'] = rotations[:, 3]
    
    # Write PLY header and data
    header = f"""ply
format binary_little_endian 1.0
comment Generated by Melkor DA3
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
    
    with open(output_path, 'wb') as f:
        f.write(header.encode('utf-8'))
        vertex_data.tofile(f)
    
    print(f"Saved {n} Gaussians to {output_path}")


def save_json(gaussians: dict, output_path: str):
    """Save Gaussians to JSON format for debugging."""
    output = {
        'num_gaussians': len(gaussians['positions']),
        'gaussians': []
    }
    
    for i in range(min(len(gaussians['positions']), 1000)):  # Limit for JSON size
        output['gaussians'].append({
            'position': gaussians['positions'][i].tolist(),
            'color': gaussians['colors'][i].tolist(),
            'scale': gaussians['scales'][i].tolist(),
            'rotation': gaussians['rotations'][i].tolist(),
            'opacity': float(gaussians['opacities'][i])
        })
    
    with open(output_path, 'w') as f:
        json.dump(output, f, indent=2)
    
    print(f"Saved {output['num_gaussians']} Gaussians to {output_path}")


def save_npz(gaussians: dict, output_path: str):
    """Save Gaussians to NPZ format (compressed numpy arrays)."""
    np.savez_compressed(
        output_path,
        positions=gaussians['positions'],
        colors=gaussians['colors'],
        scales=gaussians['scales'],
        rotations=gaussians['rotations'],
        opacities=gaussians['opacities']
    )
    print(f"Saved {len(gaussians['positions'])} Gaussians to {output_path}")


def save_glb(gaussians: dict, output_path: str):
    """Save Gaussians as GLB point cloud (requires trimesh)."""
    try:
        import trimesh
        
        # Create point cloud
        cloud = trimesh.PointCloud(
            vertices=gaussians['positions'],
            colors=np.clip(gaussians['colors'] * 255, 0, 255).astype(np.uint8)
        )
        
        # Export as GLB
        cloud.export(output_path, file_type='glb')
        print(f"Saved {len(gaussians['positions'])} points to {output_path}")
    except ImportError:
        print("Error: trimesh not installed. Install with: pip install trimesh")
        # Fallback to PLY
        ply_path = output_path.replace('.glb', '.ply')
        save_ply(gaussians, ply_path)
        print(f"Saved as PLY instead: {ply_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Depth-Anything-3 to 3D Gaussian Splats',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Single image
  python inference.py --input photo.jpg --output scene.ply
  
  # Directory of images
  python inference.py --input images/ --output scene.ply
  
  # With specific model
  python inference.py --model DA3-LARGE --input images/ --output scene.ply
  
  # Adjust Gaussian scale
  python inference.py --input images/ --output scene.ply --scale 0.005
  
  # Subsample for faster processing
  python inference.py --input images/ --output scene.ply --subsample 2
"""
    )
    
    parser.add_argument('--input', '-i', required=True,
                       help='Input image or directory of images')
    parser.add_argument('--output', '-o', required=True,
                       help='Output file (.ply or .json)')
    parser.add_argument('--model', '-m', default=DEFAULT_MODEL,
                       choices=['DA3-SMALL', 'DA3-BASE', 'DA3-LARGE', 'DA3-GIANT', 
                                'DA3MONO-LARGE', 'DA3METRIC-LARGE', 'DA3NESTED-GIANT-LARGE',
                                'da3-small', 'da3-base', 'da3-large', 'da3-giant',
                                'da3mono-large', 'da3metric-large', 'da3nested-giant-large'],
                       help=f'DA3 model to use (default: {DEFAULT_MODEL})')
    parser.add_argument('--model-dir', default=DEFAULT_MODEL_DIR,
                       help='Directory containing model weights')
    parser.add_argument('--device', default='cuda',
                       help='Device to use (cuda, cpu)')
    parser.add_argument('--scale', type=float, default=0.01,
                       help='Base scale for Gaussians (default: 0.01)')
    parser.add_argument('--subsample', type=int, default=1,
                       help='Pixel subsampling factor (default: 1, use all pixels)')
    parser.add_argument('--min-depth', type=float, default=0.1,
                       help='Minimum valid depth (default: 0.1)')
    parser.add_argument('--max-depth', type=float, default=100.0,
                       help='Maximum valid depth (default: 100.0)')
    parser.add_argument('--voxel-size', type=float, default=0.05,
                       help='Voxel size for multi-view fusion (default: 0.05)')
    parser.add_argument('--export-format', default='ply',
                       choices=['ply', 'glb', 'npz'],
                       help='Output format (default: ply)')
    parser.add_argument('--fp32', action='store_true',
                       help='Use FP32 instead of FP16')
    parser.add_argument('--verbose', '-v', action='store_true',
                       help='Verbose output')
    parser.add_argument('--allow-fallback-depth', action='store_true',
                       help='Permit the intensity-based depth fallback when the DA3 '
                            'model is unavailable. The fallback produces PREVIEW-ONLY '
                            'output that is NOT suitable for reconstruction; it is '
                            'disabled by default to prevent silent low-quality results.')

    args = parser.parse_args()
    
    # Check CUDA availability
    if args.device == 'cuda' and not torch.cuda.is_available():
        print("Warning: CUDA not available, falling back to CPU")
        args.device = 'cpu'
    
    if args.device == 'cuda':
        print(f"Using GPU: {torch.cuda.get_device_name(0)}")
        print(f"GPU Memory: {torch.cuda.get_device_properties(0).total_memory / 1e9:.1f} GB")
    
    # Get image files
    image_files = get_image_files(args.input)
    print(f"Found {len(image_files)} image(s)")
    
    if len(image_files) == 0:
        print("Error: No images found")
        sys.exit(1)
    
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
    
    # Load images
    print("Loading images...")
    images = []
    for path in tqdm(image_files, desc="Loading"):
        img_tensor, _ = load_image(path)
        images.append(img_tensor)
    
    # Predict depth and rays
    start_time = time.time()
    depths, rays, colors = generator.predict_depth_rays(images, image_files)
    depth_time = time.time() - start_time
    per_image_time = depth_time / len(images) if len(images) > 0 else 0
    print(f"Depth prediction: {depth_time:.2f}s ({per_image_time:.2f}s per image)")
    
    # Convert to Gaussians
    start_time = time.time()
    gaussians = generator.depth_rays_to_gaussians(
        depths, rays, colors,
        scale_factor=args.scale,
        min_depth=args.min_depth,
        max_depth=args.max_depth,
        subsample=args.subsample
    )
    convert_time = time.time() - start_time
    print(f"Gaussian conversion: {convert_time:.2f}s")
    print(f"Generated {len(gaussians['positions'])} Gaussians")
    
    # Save output
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    suffix = output_path.suffix.lower()
    if suffix == '.json':
        save_json(gaussians, str(output_path))
    elif suffix == '.npz':
        save_npz(gaussians, str(output_path))
    elif suffix == '.glb':
        save_glb(gaussians, str(output_path))
    else:
        save_ply(gaussians, str(output_path))
    
    print(f"\nTotal time: {depth_time + convert_time:.2f}s")


if __name__ == '__main__':
    main()
