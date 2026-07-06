#!/bin/bash
set -e

# Complete end-to-end Gaussian Splatting training from images
# Usage: ./scripts/train_from_images.sh /path/to/images /path/to/output

if [ $# -lt 2 ]; then
    echo "Usage: $0 <image_folder> <output_folder> [--tool opensplat|gsplat]"
    echo ""
    echo "Example:"
    echo "  $0 ~/Photos/my_scene ~/output/my_scene"
    echo "  $0 ~/Photos/my_scene ~/output/my_scene --tool gsplat"
    echo ""
    echo "Options:"
    echo "  --tool opensplat  Use OpenSplat (default, fastest, cross-platform)"
    echo "  --tool gsplat     Use gsplat-mps (macOS only, requires Metal)"
    echo "  --quality high    High quality (30k iterations, slower)"
    echo "  --quality fast    Fast preview (7k iterations, quick)"
    exit 1
fi

IMAGE_FOLDER="$1"
OUTPUT_FOLDER="$2"
shift 2

TOOL="opensplat"
QUALITY="high"

while [[ $# -gt 0 ]]; do
    case $1 in
        --tool)
            TOOL="$2"
            shift 2
            ;;
        --quality)
            QUALITY="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║        Gaussian Splatting Training Pipeline                   ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "Input images: $IMAGE_FOLDER"
echo "Output folder: $OUTPUT_FOLDER"
echo "Tool: $TOOL"
echo "Quality: $QUALITY"
echo ""

# Check inputs
if [ ! -d "$IMAGE_FOLDER" ]; then
    echo "Error: Image folder not found: $IMAGE_FOLDER"
    exit 1
fi

# Resolve output folder to an absolute path early: later steps (e.g. the
# gsplat branch) cd into tool directories, which would break relative paths.
mkdir -p "$OUTPUT_FOLDER"
OUTPUT_FOLDER="$(cd "$OUTPUT_FOLDER" && pwd)"

# Count top-level images only, matching the copy step below (which does not
# descend into subdirectories).
IMAGE_COUNT=$(find "$IMAGE_FOLDER" -maxdepth 1 -type f \( \
    -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o \
    -iname "*.heic" -o -iname "*.heif" -o -iname "*.webp" -o \
    -iname "*.tiff" -o -iname "*.tif" -o -iname "*.bmp" -o -iname "*.gif" \
\) | wc -l | tr -d ' ')
RECURSIVE_IMAGE_COUNT=$(find "$IMAGE_FOLDER" -type f \( \
    -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o \
    -iname "*.heic" -o -iname "*.heif" -o -iname "*.webp" -o \
    -iname "*.tiff" -o -iname "*.tif" -o -iname "*.bmp" -o -iname "*.gif" \
\) | wc -l | tr -d ' ')
if [ "$RECURSIVE_IMAGE_COUNT" -gt "$IMAGE_COUNT" ]; then
    echo "Warning: $((RECURSIVE_IMAGE_COUNT - IMAGE_COUNT)) image(s) found in nested subdirectories of $IMAGE_FOLDER will be IGNORED."
    echo "Only images directly inside the folder are used. Move them to the top level to include them."
fi
if [ "$IMAGE_COUNT" -lt 3 ]; then
    echo "Error: Need at least 3 images for reconstruction. Found: $IMAGE_COUNT"
    exit 1
fi
echo "Found $IMAGE_COUNT images ✓"
echo ""

# Create output folder structure
mkdir -p "$OUTPUT_FOLDER/images"

# Copy images to workspace (COLMAP expects specific structure)
echo "[1/4] Preparing workspace..."

# Copy standard image formats
cp "$IMAGE_FOLDER"/*.{jpg,jpeg,png,JPG,JPEG,PNG} "$OUTPUT_FOLDER/images/" 2>/dev/null || true
cp "$IMAGE_FOLDER"/*.{tiff,tif,TIFF,TIF} "$OUTPUT_FOLDER/images/" 2>/dev/null || true
cp "$IMAGE_FOLDER"/*.{bmp,BMP,gif,GIF} "$OUTPUT_FOLDER/images/" 2>/dev/null || true
cp "$IMAGE_FOLDER"/*.{webp,WEBP} "$OUTPUT_FOLDER/images/" 2>/dev/null || true

# Handle HEIC/HEIF (Apple format) - convert to JPEG
HEIC_COUNT=$(find "$IMAGE_FOLDER" -type f \( -iname "*.heic" -o -iname "*.heif" \) 2>/dev/null | wc -l | tr -d ' ')
if [ "$HEIC_COUNT" -gt 0 ]; then
    echo "Found $HEIC_COUNT HEIC/HEIF images, converting to JPEG..."
    
    if command -v sips &> /dev/null; then
        # macOS - use sips for HEIC conversion
        find "$IMAGE_FOLDER" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) | while read -r heic_file; do
            fname=$(basename "$heic_file")
            output_name="${fname%.*}.jpg"
            sips -s format jpeg "$heic_file" --out "$OUTPUT_FOLDER/images/$output_name" 2>/dev/null || true
        done
    elif command -v convert &> /dev/null; then
        # ImageMagick
        find "$IMAGE_FOLDER" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) | while read -r heic_file; do
            fname=$(basename "$heic_file")
            output_name="${fname%.*}.jpg"
            convert "$heic_file" "$OUTPUT_FOLDER/images/$output_name" 2>/dev/null || true
        done
    elif command -v heif-convert &> /dev/null; then
        # libheif
        find "$IMAGE_FOLDER" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) | while read -r heic_file; do
            fname=$(basename "$heic_file")
            output_name="${fname%.*}.jpg"
            heif-convert "$heic_file" "$OUTPUT_FOLDER/images/$output_name" 2>/dev/null || true
        done
    else
        if [[ "$(uname)" == "Darwin" ]]; then
            echo "Warning: HEIC conversion not available. Install ImageMagick: brew install imagemagick"
        else
            echo "Warning: HEIC conversion not available. Install ImageMagick: sudo apt-get install imagemagick"
        fi
    fi
fi

# Convert WebP to JPEG if needed
WEBP_COUNT=$(find "$OUTPUT_FOLDER/images" -type f -iname "*.webp" 2>/dev/null | wc -l | tr -d ' ')
if [ "$WEBP_COUNT" -gt 0 ] && command -v convert &> /dev/null; then
    echo "Converting $WEBP_COUNT WebP images to JPEG..."
    find "$OUTPUT_FOLDER/images" -maxdepth 1 -type f -iname "*.webp" | while read -r webp_file; do
        fname=$(basename "$webp_file")
        output_name="${fname%.*}.jpg"
        convert "$webp_file" "$OUTPUT_FOLDER/images/$output_name" 2>/dev/null && rm "$webp_file" || true
    done
fi

COPIED_COUNT=$(find "$OUTPUT_FOLDER/images" -type f \( -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o -iname "*.tiff" -o -iname "*.tif" \) | wc -l | tr -d ' ')
echo "$COPIED_COUNT images ready ✓"
echo ""

# Run COLMAP for camera pose estimation
echo "[2/4] Running COLMAP structure-from-motion..."
echo "This extracts camera positions from your images."
echo "Time depends on image count and resolution (~1-10 minutes)"
echo ""

if ! command -v colmap &> /dev/null; then
    if [[ "$(uname)" == "Darwin" ]]; then
        echo "Error: COLMAP not installed. Run: brew install colmap"
    else
        echo "Error: COLMAP not installed. Run: sudo apt-get install colmap"
    fi
    exit 1
fi

# COLMAP automatic reconstruction
# Detect if NVIDIA GPU is available for CUDA acceleration
USE_GPU=0
if [[ "$(uname)" == "Linux" ]] && command -v nvidia-smi &> /dev/null; then
    echo "NVIDIA GPU detected - enabling CUDA acceleration for COLMAP"
    USE_GPU=1
fi

# Build COLMAP command with GPU settings
# Note: COLMAP CLI works headlessly without X display
colmap automatic_reconstructor \
    --workspace_path "$OUTPUT_FOLDER" \
    --image_path "$OUTPUT_FOLDER/images" \
    --quality "$([[ "$QUALITY" == "fast" ]] && echo "low" || echo "high")" \
    --single_camera 1 \
    --SiftExtraction.use_gpu "$USE_GPU" \
    --SiftMatching.use_gpu "$USE_GPU"

if [ ! -f "$OUTPUT_FOLDER/sparse/0/cameras.bin" ]; then
    echo "Error: COLMAP reconstruction failed."
    echo "Tips:"
    echo "  - Ensure images have sufficient overlap (60-80%)"
    echo "  - Avoid blurry or motion-blurred images"
    echo "  - Need 30+ images for best results"
    exit 1
fi
echo "COLMAP reconstruction complete ✓"
echo ""

# Train Gaussian Splats
echo "[3/4] Training 3D Gaussian Splats..."
echo "Using $TOOL with $QUALITY quality settings"
echo ""

if [ "$TOOL" == "opensplat" ]; then
    if [ ! -f "$PROJECT_DIR/opensplat" ]; then
        echo "Error: OpenSplat not installed. Run: ./scripts/setup_opensplat.sh"
        exit 1
    fi
    
    # OpenSplat training
    ITERATIONS=$([[ "$QUALITY" == "fast" ]] && echo "7000" || echo "30000")
    
    "$PROJECT_DIR/opensplat" "$OUTPUT_FOLDER" \
        -n "$ITERATIONS" \
        -o "$OUTPUT_FOLDER/splat.ply"
        
elif [ "$TOOL" == "gsplat" ]; then
    if [[ "$(uname)" != "Darwin" ]]; then
        echo "Error: gsplat-mps is only available on macOS (requires Metal)."
        echo "Use --tool opensplat instead on Linux."
        exit 1
    fi
    
    if [ ! -f "$PROJECT_DIR/gsplat-python" ]; then
        echo "Error: gsplat-mps not installed. Run: ./scripts/setup_gsplat_mps.sh"
        exit 1
    fi
    
    # gsplat-mps training (using simple trainer)
    GSPLAT_DIR="$PROJECT_DIR/tools/gsplat-mps"
    ITERATIONS=$([[ "$QUALITY" == "fast" ]] && echo "7000" || echo "30000")
    
    source "$GSPLAT_DIR/venv/bin/activate"
    cd "$GSPLAT_DIR"
    
    python examples/simple_trainer.py \
        --data_dir "$OUTPUT_FOLDER" \
        --result_dir "$OUTPUT_FOLDER" \
        --max_steps "$ITERATIONS"
        
    deactivate
else
    echo "Error: Unknown tool: $TOOL"
    exit 1
fi

echo "Training complete ✓"
echo ""

# Find output file
echo "[4/4] Finalizing..."
OUTPUT_PLY=""
if [ -f "$OUTPUT_FOLDER/splat.ply" ]; then
    OUTPUT_PLY="$OUTPUT_FOLDER/splat.ply"
elif [ -f "$OUTPUT_FOLDER/point_cloud.ply" ]; then
    OUTPUT_PLY="$OUTPUT_FOLDER/point_cloud.ply"
fi

if [ -z "$OUTPUT_PLY" ]; then
    echo "Warning: Output PLY not found in expected location."
    echo "Check $OUTPUT_FOLDER for output files."
else
    FILE_SIZE=$(du -h "$OUTPUT_PLY" | cut -f1)
    echo ""
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║              TRAINING COMPLETE! ✓                             ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo ""
    echo "Output: $OUTPUT_PLY ($FILE_SIZE)"
    echo ""
    echo "View your Gaussian Splat:"
    echo "  1. SuperSplat (Web): https://playcanvas.com/supersplat"
    echo "     - Drag and drop your PLY file"
    echo ""
    echo "  2. Convert to SPZ for smaller files:"
    echo "     ./build/melkor $OUTPUT_PLY ${OUTPUT_PLY%.ply}.spz"
    echo ""
    echo "  3. MetalSplatter (macOS app):"
    echo "     - Available on Mac App Store"
fi
