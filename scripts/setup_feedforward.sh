#!/bin/bash

# Setup script for Melkor feedforward model dependencies
# This installs Python dependencies and downloads model weights

set -e

echo "=================================="
echo "Melkor Feedforward Model Setup"
echo "=================================="
echo ""

# Check Python
if ! command -v python3 &> /dev/null; then
    echo "Error: Python 3 is required but not installed."
    echo "Install it with: brew install python3"
    exit 1
fi

PYTHON_VERSION=$(python3 --version 2>&1 | awk '{print $2}')
echo "Found Python: $PYTHON_VERSION"

# Create virtual environment
VENV_DIR="$HOME/.melkor/venv"
MODEL_DIR="$HOME/.melkor/models"

mkdir -p "$MODEL_DIR"

if [ ! -d "$VENV_DIR" ]; then
    echo "Creating virtual environment at $VENV_DIR..."
    python3 -m venv "$VENV_DIR"
else
    echo "Using existing virtual environment at $VENV_DIR"
fi

# Activate virtual environment
source "$VENV_DIR/bin/activate"

# Install dependencies
echo ""
echo "Installing Python dependencies..."
pip install --upgrade pip

# Core dependencies
pip install numpy pillow tqdm

# PyTorch for Apple Silicon
echo ""
echo "Installing PyTorch for Apple Silicon..."
pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu

# Optional: MLX for Apple Silicon optimization
echo ""
echo "Installing MLX (optional, for Apple Silicon optimization)..."
pip install mlx || echo "MLX installation failed (optional)"

# Model-specific dependencies
echo ""
echo "Installing model-specific dependencies..."

# For splatter-image
pip install einops timm kornia || true

# For mvsplat
pip install lpips || true

echo ""
echo "=================================="
echo "Downloading model weights..."
echo "=================================="

download_model() {
    local name=$1
    local url=$2
    local output_dir="$MODEL_DIR/$name"
    local filename=$(basename "$url")
    
    mkdir -p "$output_dir"
    
    if [ -f "$output_dir/$filename" ]; then
        echo "Model $name already downloaded."
    else
        echo "Downloading $name..."
        curl -L -o "$output_dir/$filename" "$url" || {
            echo "Warning: Failed to download $name"
            return 1
        }
        echo "Downloaded $name to $output_dir/$filename"
    fi
}

# Ask which models to download
echo ""
echo "Which models would you like to download?"
echo "1. splatter-image (500MB) - Single-view 3D reconstruction"
echo "2. mvsplat (800MB) - Multi-view 3D reconstruction"
echo "3. Both"
echo "4. None (skip download)"
read -p "Enter choice [1-4]: " choice

case $choice in
    1)
        download_model "splatter-image" "https://huggingface.co/szymanowiczs/splatter-image/resolve/main/model_latest.pth"
        ;;
    2)
        download_model "mvsplat" "https://huggingface.co/donydchen/mvsplat/resolve/main/re10k.ckpt"
        ;;
    3)
        download_model "splatter-image" "https://huggingface.co/szymanowiczs/splatter-image/resolve/main/model_latest.pth"
        download_model "mvsplat" "https://huggingface.co/donydchen/mvsplat/resolve/main/re10k.ckpt"
        ;;
    4)
        echo "Skipping model download."
        ;;
    *)
        echo "Invalid choice. Skipping model download."
        ;;
esac

echo ""
echo "=================================="
echo "Setup complete!"
echo "=================================="
echo ""
echo "Virtual environment: $VENV_DIR"
echo "Model directory: $MODEL_DIR"
echo ""
echo "Usage:"
echo "  ./melkor input.glb output.ply --feedforward --model splatter-image"
echo ""
echo "To activate the virtual environment manually:"
echo "  source $VENV_DIR/bin/activate"
echo ""
