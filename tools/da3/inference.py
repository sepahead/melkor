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
        dtype: torch.dtype = torch.float16
    ):
        self.device = device
        self.dtype = dtype
        # Normalize model name to uppercase canonical form
        self.model_name = self.MODEL_NAME_MAP.get(model_name.lower(), model_name.upper())
        self.model_dir = Path(model_dir)
        self.model = None
        
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
        
        Returns:
            depths: List of depth maps (H, W)
            rays: List of ray direction maps (H, W, 3)
            colors: List of RGB color arrays (H, W, 3)
        """
        depths = []
        rays = []
        colors = []
        
        # Validate image dimensions - warn if images have different sizes
        if images:
            first_shape = images[0].shape
            mismatched = []
            for i, img in enumerate(images):
                if img.shape != first_shape:
                    mismatched.append((i, image_paths[i], img.shape))
            if mismatched:
                print(f"\nWarning: {len(mismatched)} images have different dimensions:")
                for idx, path, shape in mismatched[:5]:  # Show first 5
                    print(f"  {path.name}: {shape} (expected {first_shape})")
                if len(mismatched) > 5:
                    print(f"  ... and {len(mismatched) - 5} more")
                print("Results may be inconsistent. Consider resizing images to uniform dimensions.\n")
        
        with torch.no_grad():
            for img_tensor, img_path in tqdm(zip(images, image_paths), 
                                              total=len(images),
                                              desc="Predicting depth"):
                img_batch = img_tensor.unsqueeze(0).to(self.device, self.dtype)
                
                if self.model is not None:
                    # Use DA3 model
                    try:
                        output = self.model.inference([str(img_path)])
                        depth = output.depth[0].cpu().numpy()
                        ray = output.ray[0].cpu().numpy() if hasattr(output, 'ray') else None
                    except Exception as e:
                        print(f"Warning: DA3 inference failed for {img_path}: {e}")
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
        if not DA3GaussianGenerator._fallback_warned:
            DA3GaussianGenerator._fallback_warned = True
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
        
        for depth, ray, color in zip(depths, rays, colors):
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
            print(f"  - Try adjusting --min-depth and --max-depth parameters.")
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
    
    # Convert colors to SH DC coefficients
    # SH_DC = (RGB - 0.5) / C0 where C0 = 0.28209479177387814
    C0 = 0.28209479177387814
    sh_dc = ((colors - 0.5) / C0).astype(np.float32)
    
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
                       help=f'Directory containing model weights')
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
        dtype=dtype
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
