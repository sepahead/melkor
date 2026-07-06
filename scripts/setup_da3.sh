#!/bin/bash

# Setup script for Depth-Anything-3 (DA3) with Multi-GPU CUDA support
# This installs DA3 from ByteDance for feedforward 3D Gaussian Splatting

set -e

echo "=========================================="
echo "Depth-Anything-3 Multi-GPU Setup (CUDA)"
echo "=========================================="
echo ""

# Check for CUDA
if ! command -v nvcc &> /dev/null; then
    echo "Error: CUDA toolkit not found. DA3 requires NVIDIA CUDA."
    echo "Install CUDA from: https://developer.nvidia.com/cuda-downloads"
    exit 1
fi

CUDA_VERSION=$(nvcc --version | grep "release" | awk '{print $6}' | cut -d',' -f1)
echo "Found CUDA: $CUDA_VERSION"

# Check for Python
if ! command -v python3 &> /dev/null; then
    echo "Error: Python 3 is required but not installed."
    exit 1
fi

PYTHON_VERSION=$(python3 --version 2>&1 | awk '{print $2}')
echo "Found Python: $PYTHON_VERSION"

# Check for NVIDIA GPU
if ! command -v nvidia-smi &> /dev/null; then
    echo "Error: nvidia-smi not found. Please install NVIDIA drivers."
    exit 1
fi

GPU_COUNT=$(nvidia-smi -L | wc -l)
echo "Found $GPU_COUNT GPU(s)"
nvidia-smi -L
echo ""

# Setup directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
TOOLS_DIR="$PROJECT_ROOT/tools"
DA3_DIR="$TOOLS_DIR/da3"
VENV_DIR="$DA3_DIR/venv"
MODEL_DIR="$HOME/.melkor/models/da3"

mkdir -p "$TOOLS_DIR"
mkdir -p "$MODEL_DIR"

echo "Installing to: $DA3_DIR"
echo ""

# Clone or update Depth-Anything-3
if [ -d "$DA3_DIR/Depth-Anything-3" ]; then
    echo "Updating existing Depth-Anything-3 repository..."
    cd "$DA3_DIR/Depth-Anything-3"
    git pull || echo "Warning: Could not update repository"
else
    echo "Cloning Depth-Anything-3 repository..."
    mkdir -p "$DA3_DIR"
    cd "$DA3_DIR"
    git clone https://github.com/ByteDance-Seed/Depth-Anything-3.git
fi

# Create virtual environment
if [ ! -d "$VENV_DIR" ]; then
    echo ""
    echo "Creating virtual environment..."
    python3 -m venv "$VENV_DIR"
else
    echo "Using existing virtual environment"
fi

# Activate virtual environment
source "$VENV_DIR/bin/activate"

echo ""
echo "Installing Python dependencies..."
pip install --upgrade pip

# Install PyTorch with CUDA
echo ""
echo "Installing PyTorch with CUDA support..."
# Detect CUDA version and install appropriate PyTorch
CUDA_MAJOR=$(echo $CUDA_VERSION | cut -d'.' -f1 | tr -d 'V')
CUDA_MINOR=$(echo $CUDA_VERSION | cut -d'.' -f2)

echo "Detected CUDA version: $CUDA_MAJOR.$CUDA_MINOR"

# PyTorch supports specific CUDA versions - pick the closest compatible one
# As of 2024, PyTorch supports: cu118, cu121, cu124
if [ "$CUDA_MAJOR" -ge 12 ]; then
    if [ "$CUDA_MINOR" -ge 4 ]; then
        echo "Using PyTorch with CUDA 12.4 support"
        pip install torch torchvision --index-url https://download.pytorch.org/whl/cu124
    elif [ "$CUDA_MINOR" -ge 1 ]; then
        echo "Using PyTorch with CUDA 12.1 support"
        pip install torch torchvision --index-url https://download.pytorch.org/whl/cu121
    else
        echo "CUDA 12.0 detected - using CUDA 12.1 compatible PyTorch"
        pip install torch torchvision --index-url https://download.pytorch.org/whl/cu121
    fi
elif [ "$CUDA_MAJOR" -eq 11 ] && [ "$CUDA_MINOR" -ge 8 ]; then
    echo "Using PyTorch with CUDA 11.8 support"
    pip install torch torchvision --index-url https://download.pytorch.org/whl/cu118
elif [ "$CUDA_MAJOR" -eq 11 ]; then
    echo "Warning: CUDA 11.x (<11.8) detected. Trying CUDA 11.8 wheels (may have compatibility issues)"
    pip install torch torchvision --index-url https://download.pytorch.org/whl/cu118
else
    echo "Warning: CUDA version $CUDA_VERSION may not be fully supported."
    echo "Attempting to install with automatic CUDA detection..."
    pip install torch torchvision
fi

# Verify CUDA is available in PyTorch
python3 -c "import torch; print(f'PyTorch CUDA available: {torch.cuda.is_available()}')"
python3 -c "import torch; print(f'PyTorch CUDA device count: {torch.cuda.device_count()}')"

# Install DA3 dependencies
echo ""
echo "Installing Depth-Anything-3..."
cd "$DA3_DIR/Depth-Anything-3"
pip install -e .

# Install additional dependencies for multi-GPU and 3DGS conversion
echo ""
echo "Installing additional dependencies..."
pip install \
    numpy \
    pillow \
    tqdm \
    einops \
    opencv-python \
    scipy \
    plyfile \
    trimesh \
    open3d || echo "Warning: Some optional dependencies failed to install"

# Link to our inference scripts (they live in tools/da3/)
echo ""
echo "Setting up inference scripts..."
# The inference scripts are already in $PROJECT_ROOT/tools/da3/
# Create symlinks if running from a different location
if [ ! -f "$DA3_DIR/inference.py" ] && [ -f "$PROJECT_ROOT/tools/da3/inference.py" ]; then
    ln -sf "$PROJECT_ROOT/tools/da3/inference.py" "$DA3_DIR/inference.py" 2>/dev/null || \
    cp "$PROJECT_ROOT/tools/da3/inference.py" "$DA3_DIR/inference.py" 2>/dev/null || true
fi
if [ ! -f "$DA3_DIR/multigpu_inference.py" ] && [ -f "$PROJECT_ROOT/tools/da3/multigpu_inference.py" ]; then
    ln -sf "$PROJECT_ROOT/tools/da3/multigpu_inference.py" "$DA3_DIR/multigpu_inference.py" 2>/dev/null || \
    cp "$PROJECT_ROOT/tools/da3/multigpu_inference.py" "$DA3_DIR/multigpu_inference.py" 2>/dev/null || true
fi

echo ""
echo "=========================================="
echo "Downloading model weights..."
echo "=========================================="

download_model() {
    local name=$1
    local url=$2
    local output_dir="$MODEL_DIR"
    
    mkdir -p "$output_dir"
    
    if [ -d "$output_dir/$name" ] || [ -f "$output_dir/$name" ]; then
        echo "Model $name already exists."
    else
        echo "Downloading $name from Hugging Face..."
        # Use huggingface-cli if available, otherwise curl
        if command -v huggingface-cli &> /dev/null; then
            huggingface-cli download "depth-anything/$name" --local-dir "$output_dir/$name"
        else
            pip install huggingface_hub
            python3 -c "
from huggingface_hub import snapshot_download
snapshot_download(repo_id='depth-anything/$name', local_dir='$output_dir/$name')
print('Downloaded $name')
" || {
                echo "Warning: Failed to download $name"
                return 1
            }
        fi
        echo "Downloaded $name to $output_dir/$name"
    fi
}

# Ask which models to download
echo ""
echo "=========================================="
echo "Which DA3 models would you like to download?"
echo "=========================================="
echo ""
echo "MAIN SERIES (Any-View Depth + Ray → Full 3D Gaussian Splatting):"
echo "  These models predict depth AND ray directions from multiple viewpoints,"
echo "  enabling full 3D reconstruction without COLMAP camera poses."
echo ""
echo "  1. DA3-BASE (recommended, ~2GB)  - 97M params, balanced speed/quality"
echo "  2. DA3-LARGE (~4GB)               - 335M params, higher quality"
echo "  3. DA3-SMALL (~1GB)               - 24M params, fastest inference"
echo "  4. DA3-GIANT (~8GB)               - 1.15B params, BEST quality (flagship)"
echo ""
echo "SPECIALIZED SERIES (Task-Specific):"
echo "  These models are optimized for specific use cases."
echo ""
echo "  5. DA3MONO-LARGE (~4GB)           - Single-view relative depth only"
echo "                                       Best for: depth visualization, AR effects"
echo "                                       Output: Normalized depth map (0-1)"
echo ""
echo "  6. DA3METRIC-LARGE (~4GB)         - Single-view METRIC depth (in meters)"
echo "                                       Best for: robotics, navigation, measurement"
echo "                                       Output: Absolute depth in real-world units"
echo ""
echo "  7. DA3NESTED-GIANT-LARGE (~12GB)  - Combines DA3-GIANT + METRIC alignment"
echo "                                       Best for: Production-quality 3D with real scale"
echo "                                       Output: Full 3DGS + metric depth"
echo ""
echo "BUNDLES:"
echo "  8. All main series (SMALL + BASE + LARGE + GIANT)"
echo "  9. None (download later)"
echo ""
read -p "Enter choice [1-9]: " choice

case $choice in
    1)
        download_model "DA3-BASE" "https://huggingface.co/depth-anything/DA3-BASE"
        ;;
    2)
        download_model "DA3-LARGE" "https://huggingface.co/depth-anything/DA3-LARGE"
        ;;
    3)
        download_model "DA3-SMALL" "https://huggingface.co/depth-anything/DA3-SMALL"
        ;;
    4)
        download_model "DA3-GIANT" "https://huggingface.co/depth-anything/DA3-GIANT"
        ;;
    5)
        download_model "DA3MONO-LARGE" "https://huggingface.co/depth-anything/DA3MONO-LARGE"
        ;;
    6)
        download_model "DA3METRIC-LARGE" "https://huggingface.co/depth-anything/DA3METRIC-LARGE"
        ;;
    7)
        download_model "DA3NESTED-GIANT-LARGE" "https://huggingface.co/depth-anything/DA3NESTED-GIANT-LARGE"
        ;;
    8)
        download_model "DA3-SMALL" "https://huggingface.co/depth-anything/DA3-SMALL"
        download_model "DA3-BASE" "https://huggingface.co/depth-anything/DA3-BASE"
        download_model "DA3-LARGE" "https://huggingface.co/depth-anything/DA3-LARGE"
        download_model "DA3-GIANT" "https://huggingface.co/depth-anything/DA3-GIANT"
        ;;
    9)
        echo "Skipping model download. You can download later with:"
        echo "  python3 -c \"from huggingface_hub import snapshot_download; snapshot_download('depth-anything/DA3-BASE', local_dir='$MODEL_DIR/DA3-BASE')\""
        ;;
    *)
        echo "Invalid choice. Skipping model download."
        ;;
esac

# Create wrapper scripts
echo ""
echo "Creating wrapper scripts..."

# Main DA3 inference wrapper
cat > "$PROJECT_ROOT/da3-infer" << 'WRAPPER_EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DA3_DIR="$SCRIPT_DIR/tools/da3"
source "$DA3_DIR/venv/bin/activate"
exec python3 "$DA3_DIR/inference.py" "$@"
WRAPPER_EOF
chmod +x "$PROJECT_ROOT/da3-infer"

# Multi-GPU DA3 inference wrapper
cat > "$PROJECT_ROOT/da3-infer-multigpu" << 'WRAPPER_EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DA3_DIR="$SCRIPT_DIR/tools/da3"

# Parse arguments for GPU count
NUM_GPUS=$(nvidia-smi -L | wc -l)
for arg in "$@"; do
    if [[ $arg == --gpus=* ]]; then
        NUM_GPUS="${arg#*=}"
    fi
done

source "$DA3_DIR/venv/bin/activate"
exec torchrun --nproc_per_node=$NUM_GPUS "$DA3_DIR/multigpu_inference.py" "$@"
WRAPPER_EOF
chmod +x "$PROJECT_ROOT/da3-infer-multigpu"

# Python wrapper for direct imports
cat > "$PROJECT_ROOT/da3-python" << 'WRAPPER_EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DA3_DIR="$SCRIPT_DIR/tools/da3"
source "$DA3_DIR/venv/bin/activate"
exec python3 "$@"
WRAPPER_EOF
chmod +x "$PROJECT_ROOT/da3-python"

echo ""
echo "=========================================="
echo "Setup Complete!"
echo "=========================================="
echo ""
echo "Installed components:"
echo "  - Depth-Anything-3: $DA3_DIR/Depth-Anything-3"
echo "  - Virtual environment: $VENV_DIR"
echo "  - Model directory: $MODEL_DIR"
echo ""
echo "Wrapper scripts created:"
echo "  - ./da3-infer          : Single-GPU inference"
echo "  - ./da3-infer-multigpu : Multi-GPU distributed inference"
echo "  - ./da3-python         : Python with DA3 environment"
echo ""
echo "Usage examples:"
echo ""
echo "  # Single image to 3DGS (single GPU):"
echo "  ./da3-infer --input image.jpg --output output.ply"
echo ""
echo "  # Multiple images to 3DGS (single GPU):"
echo "  ./da3-infer --input images/ --output output.ply"
echo ""
echo "  # Multi-GPU inference (all GPUs):"
echo "  ./da3-infer-multigpu --input images/ --output output.ply"
echo ""
echo "  # Multi-GPU with specific GPU count:"
echo "  ./da3-infer-multigpu --gpus=4 --input images/ --output output.ply"
echo ""
echo "  # Export to SPZ format:"
echo "  ./da3-infer --input images/ --output output.spz"
echo ""
echo "  # With specific model:"
echo "  ./da3-infer --model DA3-LARGE --input images/ --output output.ply"
echo ""
echo "For more options, run: ./da3-infer --help"
echo ""
