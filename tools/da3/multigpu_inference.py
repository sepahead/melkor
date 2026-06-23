#!/usr/bin/env python3
"""
Depth-Anything-3 Multi-GPU Distributed Inference Script
Converts images to 3D Gaussian Splats using DA3's depth-ray representation.

This script uses PyTorch Distributed Data Parallel (DDP) for multi-GPU inference.
Images are distributed across GPUs for parallel processing.

Usage:
    # Using wrapper script (recommended)
    ./da3-infer-multigpu --input images/ --output output.ply
    
    # Using torchrun directly
    torchrun --nproc_per_node=4 multigpu_inference.py --input images/ --output output.ply
    
    # With specific GPUs
    CUDA_VISIBLE_DEVICES=0,1,2,3 torchrun --nproc_per_node=4 multigpu_inference.py ...
"""

import argparse
import os
import sys
import tempfile
import time
from datetime import timedelta
from pathlib import Path
from typing import List, Optional, Tuple

import numpy as np
import torch
import torch.distributed as dist
from PIL import Image
from tqdm import tqdm

# Distributed timeout settings
DISTRIBUTED_TIMEOUT_MINUTES = 30
NCCL_TIMEOUT = timedelta(minutes=DISTRIBUTED_TIMEOUT_MINUTES)

# Default paths
DEFAULT_MODEL_DIR = os.path.expanduser("~/.melkor/models/da3")
DEFAULT_MODEL = "DA3-BASE"


def setup_distributed():
    """Initialize distributed training with proper timeout handling."""
    if 'RANK' in os.environ:
        # Launched with torchrun
        rank = int(os.environ['RANK'])
        world_size = int(os.environ['WORLD_SIZE'])
        local_rank = int(os.environ['LOCAL_RANK'])
    else:
        # Single process fallback
        rank = 0
        world_size = 1
        local_rank = 0
    
    if world_size > 1:
        try:
            dist.init_process_group(
                backend='nccl',
                init_method='env://',
                world_size=world_size,
                rank=rank,
                timeout=NCCL_TIMEOUT
            )
        except Exception as e:
            print(f"[Rank {rank}] Failed to initialize distributed process group: {e}")
            print(f"[Rank {rank}] Hint: Check that all GPUs are accessible and NCCL can communicate.")
            print(f"[Rank {rank}] Try setting NCCL_DEBUG=INFO for more details.")
            raise
    
    torch.cuda.set_device(local_rank)
    
    return rank, world_size, local_rank


def cleanup_distributed():
    """Clean up distributed training."""
    if dist.is_initialized():
        dist.destroy_process_group()


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


def load_image(path: Path, size: Optional[Tuple[int, int]] = None) -> Tuple[torch.Tensor, np.ndarray]:
    """Load and preprocess an image."""
    img = Image.open(path).convert('RGB')
    
    if size is not None:
        img = img.resize(size, Image.LANCZOS)
    
    # Convert to tensor and normalize
    img_np = np.array(img).astype(np.float32) / 255.0
    img_tensor = torch.from_numpy(img_np).permute(2, 0, 1)  # HWC -> CHW
    
    return img_tensor, img_np


class DA3MultiGPUGenerator:
    """Generate 3D Gaussian Splats from images using Depth-Anything-3 with multi-GPU support."""
    
    # Class-level flag to show fallback warning only once (across all instances)
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
        rank: int = 0,
        world_size: int = 1,
        local_rank: int = 0,
        dtype: torch.dtype = torch.float16
    ):
        self.rank = rank
        self.world_size = world_size
        self.local_rank = local_rank
        self.device = f"cuda:{local_rank}"
        self.dtype = dtype
        # Normalize model name to uppercase canonical form
        self.model_name = self.MODEL_NAME_MAP.get(model_name.lower(), model_name.upper())
        self.model_dir = Path(model_dir)
        self.model = None
        
        # Determine model capabilities
        self.supports_multi_view = self.model_name in self.MULTI_VIEW_MODELS
        self.outputs_metric_depth = self.model_name in self.METRIC_MODELS
        self.is_monocular = self.model_name in self.MONOCULAR_MODELS
        
    def log(self, msg: str):
        """Log message only from rank 0."""
        if self.rank == 0:
            print(msg)
        
    def load_model(self):
        """Load the DA3 model on this GPU."""
        if self.rank == 0:
            print(f"Loading {self.model_name} on {self.world_size} GPU(s)...")
            print(f"  Multi-view support: {self.supports_multi_view}")
            print(f"  Metric depth output: {self.outputs_metric_depth}")
            print(f"  Monocular optimized: {self.is_monocular}")
            
            # Warn if using monocular model with multi-GPU (data parallelism won't help much)
            if self.is_monocular and self.world_size > 1:
                print(f"  Note: {self.model_name} is monocular-optimized. Multi-GPU provides")
                print("        parallel processing of multiple single images, not multi-view fusion.")
        
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
                if self.rank == 0:
                    print(f"Model not found locally, loading from HuggingFace: {hf_model_id}")
                self.model = DepthAnything3.from_pretrained(hf_model_id)
            
            self.model = self.model.to(device=self.device, dtype=self.dtype)
            self.model.eval()
            
            if self.rank == 0:
                print(f"Model loaded on {self.world_size} GPU(s)")
            
        except ImportError as e:
            if self.rank == 0:
                print(f"Warning: depth_anything_3 not found ({e}). Using fallback depth estimation.")
            self.model = None
        except Exception as e:
            if self.rank == 0:
                print(f"Warning: Could not load DA3 model: {e}")
                print("Using fallback depth estimation.")
            self.model = None
    
    def distribute_work(self, image_files: List[Path]) -> List[Path]:
        """Distribute image files across GPUs.
        
        Each GPU processes a subset of images.
        """
        # Split images across ranks
        n_images = len(image_files)
        images_per_rank = n_images // self.world_size
        remainder = n_images % self.world_size
        
        # Calculate start and end indices for this rank
        start = self.rank * images_per_rank + min(self.rank, remainder)
        end = start + images_per_rank + (1 if self.rank < remainder else 0)
        
        local_files = image_files[start:end]
        
        if self.rank == 0:
            print(f"Distributing {n_images} images across {self.world_size} GPUs")
            for r in range(self.world_size):
                r_start = r * images_per_rank + min(r, remainder)
                r_end = r_start + images_per_rank + (1 if r < remainder else 0)
                print(f"  GPU {r}: {r_end - r_start} images")
        
        return local_files
    
    def predict_depth_rays_local(
        self,
        images: List[torch.Tensor],
        image_paths: List[Path]
    ) -> Tuple[List[np.ndarray], List[np.ndarray], List[np.ndarray]]:
        """Predict depth and ray maps for local images on this GPU."""
        depths = []
        rays = []
        colors = []
        
        # Validate image dimensions - all images should have the same shape
        if images:
            first_shape = images[0].shape
            for i, img in enumerate(images):
                if img.shape != first_shape:
                    if self.rank == 0:
                        print(f"Warning: Image {image_paths[i]} has shape {img.shape}, "
                              f"expected {first_shape}. Results may be inconsistent.")
        
        desc = f"GPU {self.rank}: Predicting depth" if self.world_size > 1 else "Predicting depth"
        disable_tqdm = self.rank != 0  # Only show progress bar on rank 0
        
        with torch.no_grad():
            for img_tensor, img_path in tqdm(zip(images, image_paths, strict=False),
                                              total=len(images),
                                              desc=desc,
                                              disable=disable_tqdm):
                img_batch = img_tensor.unsqueeze(0).to(self.device, self.dtype)
                
                if self.model is not None:
                    # Use DA3 model
                    try:
                        output = self.model.inference([str(img_path)])
                        depth = output.depth[0].cpu().numpy()
                        ray = output.ray[0].cpu().numpy() if hasattr(output, 'ray') else None
                    except Exception as e:
                        print(f"Warning [GPU {self.rank}]: DA3 inference failed for {img_path}: {e}")
                        depth = self._fallback_depth(img_batch)
                        ray = None
                else:
                    # Fallback depth estimation
                    depth = self._fallback_depth(img_batch)
                    ray = None
                
                # Generate rays if not provided by model
                if ray is None:
                    h, w = depth.shape
                    ray = self._compute_rays(h, w)
                
                depths.append(depth)
                rays.append(ray)
                
                # Get color from original image
                img_np = img_tensor.permute(1, 2, 0).numpy()  # CHW -> HWC
                colors.append(img_np)
        
        return depths, rays, colors
    
    def _fallback_depth(self, img_batch: torch.Tensor) -> np.ndarray:
        """Simple fallback depth estimation using image intensity.
        
        WARNING: This is a very crude approximation and should NOT be used
        for production 3D reconstruction. It assumes darker pixels are farther,
        which is often incorrect. Install the DA3 model for proper depth estimation.
        """
        if self.rank == 0 and not DA3MultiGPUGenerator._fallback_warned:
            DA3MultiGPUGenerator._fallback_warned = True
            print("\n" + "="*70)
            print("WARNING: Using FALLBACK depth estimation (intensity-based)")
            print("This produces POOR quality results. For proper 3D reconstruction,")
            print("install the DA3 model: ./scripts/setup_da3.sh")
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
        """Convert depth-ray predictions to 3D Gaussian splats."""
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
            if self.rank == 0:
                print("Warning: No valid Gaussians generated on this GPU. Check depth range and input images.")
                print(f"  - min_depth: {min_depth}, max_depth: {max_depth}")
                print("  - Try adjusting --min-depth and --max-depth parameters.")
            return {
                'positions': np.zeros((0, 3)),
                'colors': np.zeros((0, 3)),
                'scales': np.zeros((0, 3)),
                'rotations': np.zeros((0, 4)),
                'opacities': np.zeros((0,))
            }
        
        return {
            'positions': np.concatenate(all_positions, axis=0),
            'colors': np.concatenate(all_colors, axis=0),
            'scales': np.concatenate(all_scales, axis=0),
            'rotations': np.concatenate(all_rotations, axis=0),
            'opacities': np.concatenate(all_opacities, axis=0)
        }


def gather_gaussians(local_gaussians: dict, rank: int, world_size: int) -> Optional[dict]:
    """Gather Gaussians from all GPUs to rank 0.
    
    Uses tensor-based gathering with torch.distributed for proper synchronization.
    Falls back to file-based gathering for very large arrays.
    """
    if world_size == 1:
        return local_gaussians
    
    device = f"cuda:{rank}"
    
    # Get local counts
    local_count = len(local_gaussians['positions'])
    local_count_tensor = torch.tensor([local_count], dtype=torch.long, device=device)
    
    # Gather counts from all ranks to determine sizes
    all_counts = [torch.zeros(1, dtype=torch.long, device=device) for _ in range(world_size)]
    dist.all_gather(all_counts, local_count_tensor)
    all_counts = [c.item() for c in all_counts]
    total_count = sum(all_counts)
    
    if rank == 0:
        print(f"Gathering {total_count} total Gaussians from {world_size} GPUs")
        for r, c in enumerate(all_counts):
            print(f"  GPU {r}: {c} Gaussians")
    
    # Handle edge case: all GPUs produced 0 Gaussians
    if total_count == 0:
        if rank == 0:
            return {
                'positions': np.zeros((0, 3)),
                'colors': np.zeros((0, 3)),
                'scales': np.zeros((0, 3)),
                'rotations': np.zeros((0, 4)),
                'opacities': np.zeros((0,))
            }
        return None
    
    # For very large arrays (>10M Gaussians), use file-based approach to avoid OOM
    # Rationale: 10M Gaussians × 17 floats × 4 bytes ≈ 680MB per GPU for gathering
    # File-based approach avoids doubling GPU memory usage during all_gather
    if total_count > 10_000_000:
        return _gather_gaussians_file_based(local_gaussians, rank, world_size)
    
    # Prepare local tensors
    positions_t = torch.from_numpy(local_gaussians['positions'].astype(np.float32)).to(device)
    colors_t = torch.from_numpy(local_gaussians['colors'].astype(np.float32)).to(device)
    scales_t = torch.from_numpy(local_gaussians['scales'].astype(np.float32)).to(device)
    rotations_t = torch.from_numpy(local_gaussians['rotations'].astype(np.float32)).to(device)
    opacities_t = torch.from_numpy(local_gaussians['opacities'].astype(np.float32)).to(device)
    
    # Pad tensors to max size for gather (all_gather requires same size)
    max_count = max(all_counts)
    
    def pad_tensor(t, target_rows, cols):
        """Pad tensor to target_rows. Handles both 1D and 2D tensors."""
        if len(t) == target_rows:
            return t
        # For 1D tensors (like opacities), cols should be 0 or 1
        if len(t.shape) == 1 or cols <= 1:
            padded = torch.zeros(target_rows, dtype=t.dtype, device=device)
            padded[:len(t)] = t.flatten() if len(t.shape) > 1 else t
        else:
            padded = torch.zeros((target_rows, cols), dtype=t.dtype, device=device)
            padded[:len(t)] = t
        return padded
    
    positions_padded = pad_tensor(positions_t, max_count, 3)
    colors_padded = pad_tensor(colors_t, max_count, 3)
    scales_padded = pad_tensor(scales_t, max_count, 3)
    rotations_padded = pad_tensor(rotations_t, max_count, 4)
    opacities_padded = pad_tensor(opacities_t, max_count, 0)  # 1D tensor
    
    # Gather all tensors
    def gather_tensor(t):
        gathered = [torch.zeros_like(t) for _ in range(world_size)]
        dist.all_gather(gathered, t)
        return gathered
    
    all_positions = gather_tensor(positions_padded)
    all_colors = gather_tensor(colors_padded)
    all_scales = gather_tensor(scales_padded)
    all_rotations = gather_tensor(rotations_padded)
    all_opacities = gather_tensor(opacities_padded)
    
    # Synchronize to ensure all gathers are complete
    dist.barrier()
    
    # Only rank 0 needs to process the full result
    if rank == 0:
        # Unpad and concatenate
        result_positions = []
        result_colors = []
        result_scales = []
        result_rotations = []
        result_opacities = []
        
        for r in range(world_size):
            count = all_counts[r]
            if count > 0:
                result_positions.append(all_positions[r][:count].cpu().numpy())
                result_colors.append(all_colors[r][:count].cpu().numpy())
                result_scales.append(all_scales[r][:count].cpu().numpy())
                result_rotations.append(all_rotations[r][:count].cpu().numpy())
                result_opacities.append(all_opacities[r][:count].cpu().numpy())
        
        return {
            'positions': np.concatenate(result_positions, axis=0) if result_positions else np.zeros((0, 3)),
            'colors': np.concatenate(result_colors, axis=0) if result_colors else np.zeros((0, 3)),
            'scales': np.concatenate(result_scales, axis=0) if result_scales else np.zeros((0, 3)),
            'rotations': np.concatenate(result_rotations, axis=0) if result_rotations else np.zeros((0, 4)),
            'opacities': np.concatenate(result_opacities, axis=0) if result_opacities else np.zeros((0,))
        }
    else:
        return None


def _gather_gaussians_file_based(local_gaussians: dict, rank: int, world_size: int) -> Optional[dict]:
    """File-based gathering for very large arrays (>10M Gaussians).
    
    Uses explicit file sync and cleanup on all ranks.
    """
    # Create temp directory for inter-process communication
    temp_dir = Path(tempfile.gettempdir()) / "da3_multigpu"
    temp_dir.mkdir(exist_ok=True)
    local_file = temp_dir / f"gaussians_rank_{rank}.npz"
    
    try:
        # Save local results to temp file
        np.savez_compressed(
            local_file,
            positions=local_gaussians['positions'],
            colors=local_gaussians['colors'],
            scales=local_gaussians['scales'],
            rotations=local_gaussians['rotations'],
            opacities=local_gaussians['opacities']
        )
        
        # Explicit file sync to ensure writes are flushed (Unix-only, but this is CUDA/Linux anyway)
        os.sync()
        
        # Synchronize all processes after file writes
        dist.barrier()
        
        # Small delay to ensure filesystem consistency across NFS/shared storage
        time.sleep(0.5)
        
        # Rank 0 gathers all results
        if rank == 0:
            all_gaussians = {
                'positions': [],
                'colors': [],
                'scales': [],
                'rotations': [],
                'opacities': []
            }
            
            for r in range(world_size):
                r_file = temp_dir / f"gaussians_rank_{r}.npz"
                # Retry loading with exponential backoff in case of NFS delays
                for attempt in range(5):
                    try:
                        data = np.load(r_file)
                        all_gaussians['positions'].append(data['positions'])
                        all_gaussians['colors'].append(data['colors'])
                        all_gaussians['scales'].append(data['scales'])
                        all_gaussians['rotations'].append(data['rotations'])
                        all_gaussians['opacities'].append(data['opacities'])
                        break
                    except Exception as e:
                        if attempt < 4:
                            time.sleep(0.5 * (2 ** attempt))
                        else:
                            raise RuntimeError(f"Failed to load Gaussians from rank {r}: {e}") from e
            
            result = {
                'positions': np.concatenate(all_gaussians['positions'], axis=0),
                'colors': np.concatenate(all_gaussians['colors'], axis=0),
                'scales': np.concatenate(all_gaussians['scales'], axis=0),
                'rotations': np.concatenate(all_gaussians['rotations'], axis=0),
                'opacities': np.concatenate(all_gaussians['opacities'], axis=0)
            }
        else:
            result = None
        
        # Barrier before cleanup to ensure rank 0 has read all files
        dist.barrier()
        
        return result
        
    finally:
        # All ranks clean up their own temp files
        if local_file.exists():
            try:
                local_file.unlink()
            except Exception:
                pass  # Best effort cleanup


def save_ply(gaussians: dict, output_path: str):
    """Save Gaussians to PLY format compatible with 3DGS viewers.
    
    Uses efficient numpy buffer writes instead of per-element writes.
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
comment Generated by Melkor DA3 Multi-GPU
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
    SH_C0 = 0.28209479177387814
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
    # PLY format: x,y,z, nx,ny,nz, f_dc_0,f_dc_1,f_dc_2, opacity, scale_0,scale_1,scale_2, rot_0,rot_1,rot_2,rot_3
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
    
    # Write PLY header
    header = f"""ply
format binary_little_endian 1.0
comment Generated by Melkor DA3 Multi-GPU
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


def main():
    parser = argparse.ArgumentParser(
        description='Depth-Anything-3 Multi-GPU Inference',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Multi-GPU inference (all GPUs)
  torchrun --nproc_per_node=4 multigpu_inference.py --input images/ --output scene.ply
  
  # Specific GPUs
  CUDA_VISIBLE_DEVICES=0,1,2,3 torchrun --nproc_per_node=4 multigpu_inference.py ...
  
  # Using wrapper script
  ./da3-infer-multigpu --input images/ --output scene.ply
  ./da3-infer-multigpu --gpus=4 --input images/ --output scene.ply
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
    parser.add_argument('--scale', type=float, default=0.01,
                       help='Base scale for Gaussians (default: 0.01)')
    parser.add_argument('--subsample', type=int, default=1,
                       help='Pixel subsampling factor (default: 1, use all pixels)')
    parser.add_argument('--min-depth', type=float, default=0.1,
                       help='Minimum valid depth (default: 0.1)')
    parser.add_argument('--max-depth', type=float, default=100.0,
                       help='Maximum valid depth (default: 100.0)')
    parser.add_argument('--fp32', action='store_true',
                       help='Use FP32 instead of FP16')
    parser.add_argument('--gpus', type=str, default=None,
                       help='Number of GPUs (ignored, for compatibility with wrapper)')
    parser.add_argument('--verbose', '-v', action='store_true',
                       help='Verbose output')
    
    args = parser.parse_args()
    
    # Setup distributed
    rank, world_size, local_rank = setup_distributed()
    
    try:
        if rank == 0:
            print(f"\n{'='*50}")
            print("Depth-Anything-3 Multi-GPU Inference")
            print(f"{'='*50}")
            print(f"GPUs: {world_size}")
            for i in range(torch.cuda.device_count()):
                print(f"  GPU {i}: {torch.cuda.get_device_name(i)}")
            print()
        
        # Get image files (all ranks need this for distribution)
        image_files = get_image_files(args.input)
        
        if rank == 0:
            print(f"Found {len(image_files)} image(s)")
            if len(image_files) == 0:
                print("Error: No images found")
                sys.exit(1)
        
        # Initialize generator
        dtype = torch.float32 if args.fp32 else torch.float16
        generator = DA3MultiGPUGenerator(
            model_name=args.model,
            model_dir=args.model_dir,
            rank=rank,
            world_size=world_size,
            local_rank=local_rank,
            dtype=dtype
        )
        generator.load_model()
        
        # Distribute work across GPUs
        local_files = generator.distribute_work(image_files)
        
        # Load local images
        if rank == 0:
            print("Loading images...")
        
        local_images = []
        for path in local_files:
            img_tensor, _ = load_image(path)
            local_images.append(img_tensor)
        
        # Synchronize before timing
        if world_size > 1:
            dist.barrier()
        
        # Predict depth and rays on local images
        start_time = time.time()
        depths, rays, colors = generator.predict_depth_rays_local(local_images, local_files)
        
        # Synchronize after prediction
        if world_size > 1:
            dist.barrier()
        
        depth_time = time.time() - start_time
        if rank == 0:
            per_image_time = depth_time / len(image_files) if len(image_files) > 0 else 0
            print(f"Depth prediction: {depth_time:.2f}s ({per_image_time:.2f}s per image effective)")
        
        # Convert to Gaussians
        start_time = time.time()
        local_gaussians = generator.depth_rays_to_gaussians(
            depths, rays, colors,
            scale_factor=args.scale,
            min_depth=args.min_depth,
            max_depth=args.max_depth,
            subsample=args.subsample
        )
        convert_time = time.time() - start_time
        
        if rank == 0:
            print(f"Gaussian conversion: {convert_time:.2f}s")
        
        # Gather results from all GPUs
        if rank == 0:
            print("Gathering results from all GPUs...")
        
        all_gaussians = gather_gaussians(local_gaussians, rank, world_size)
        
        # Save output (only rank 0)
        if rank == 0 and all_gaussians is not None:
            print(f"Generated {len(all_gaussians['positions'])} Gaussians")
            
            output_path = Path(args.output)
            output_path.parent.mkdir(parents=True, exist_ok=True)
            
            save_ply(all_gaussians, str(output_path))
            
            print(f"\nTotal time: {depth_time + convert_time:.2f}s")
            print(f"Speedup: ~{world_size}x vs single GPU")
        
    finally:
        cleanup_distributed()


if __name__ == '__main__':
    main()
