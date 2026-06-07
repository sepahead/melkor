#!/bin/bash
set -e

# Detect platform
PLATFORM=$(uname)

if [[ "$PLATFORM" == "Darwin" ]]; then
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  Melkor Complete Gaussian Splatting Setup for Apple Silicon  ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
elif [[ "$PLATFORM" == "Linux" ]]; then
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║    Melkor Complete Gaussian Splatting Setup for Linux/CUDA   ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
else
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║         Melkor Complete Gaussian Splatting Setup             ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
fi
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Install COLMAP first (needed for both tools)
echo "=== Installing COLMAP (Structure from Motion) ==="
if ! command -v colmap &> /dev/null; then
    if [[ "$PLATFORM" == "Darwin" ]]; then
        echo "Installing COLMAP via Homebrew..."
        brew install colmap
    elif [[ "$PLATFORM" == "Linux" ]]; then
        echo "Installing COLMAP via apt..."
        echo "Note: This requires sudo privileges"
        sudo apt-get update
        sudo apt-get install -y colmap libopencv-dev libeigen3-dev
    else
        echo "Error: Unsupported platform. Please install COLMAP manually."
        echo "Visit: https://colmap.github.io/install.html"
        exit 1
    fi
else
    echo "COLMAP already installed ✓"
fi
echo ""

# Setup OpenSplat
echo "=== Setting up OpenSplat ==="
chmod +x "$SCRIPT_DIR/setup_opensplat.sh"
"$SCRIPT_DIR/setup_opensplat.sh"
echo ""

# Setup gsplat-mps (macOS only) or LichtFeld-Studio (Linux CUDA only)
if [[ "$PLATFORM" == "Darwin" ]]; then
    echo "=== Setting up gsplat-MPS ==="
    chmod +x "$SCRIPT_DIR/setup_gsplat_mps.sh"
    "$SCRIPT_DIR/setup_gsplat_mps.sh"
    echo ""
elif [[ "$PLATFORM" == "Linux" ]] && command -v nvidia-smi &> /dev/null; then
    echo "=== Setting up LichtFeld-Studio (High-Performance CUDA) ==="
    if [[ -f "$SCRIPT_DIR/setup_lichtfeld.sh" ]]; then
        chmod +x "$SCRIPT_DIR/setup_lichtfeld.sh"
        "$SCRIPT_DIR/setup_lichtfeld.sh" || {
            echo ""
            echo "Warning: LichtFeld-Studio setup failed."
            echo "This may require CUDA 12.8+ and GCC 11+."
            echo "OpenSplat is still available as a fallback."
            echo ""
        }
    fi
    echo ""
else
    echo "=== Skipping gsplat-MPS (macOS Metal only) ==="
    echo "=== Skipping LichtFeld-Studio (Linux CUDA only) ==="
    echo ""
fi

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║           ALL TOOLS INSTALLED SUCCESSFULLY! ✓                 ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

if [[ "$PLATFORM" == "Darwin" ]]; then
    echo "You now have two production-grade Gaussian splatting tools:"
    echo ""
    echo "1. OpenSplat (C++/Metal - Fastest)"
    echo "   Usage: ./opensplat /path/to/colmap/project"
    echo "   Best for: Production, large scenes, fastest training"
    echo ""
    echo "2. gsplat-MPS (Python/Metal - Most Flexible)"
    echo "   Usage: ./gsplat-train"
    echo "   Best for: Research, custom training loops, experimentation"
elif [[ "$PLATFORM" == "Linux" ]]; then
    echo "You now have the following Gaussian splatting tools:"
    echo ""
    if command -v nvidia-smi &> /dev/null; then
        echo "1. LichtFeld-Studio (C++23/CUDA - Fastest)"
        echo "   Usage: ./lichtfeld -d /path/to/colmap/project -o output/"
        echo "   Best for: Maximum speed, pose optimization, advanced features"
        echo "   Note: Requires CUDA 12.8+"
        echo ""
        echo "2. OpenSplat (C++/CUDA)"
        echo "   Usage: ./opensplat /path/to/colmap/project"
        echo "   Best for: Production, broad compatibility"
    else
        echo "1. OpenSplat (C++/CPU)"
        echo "   Usage: ./opensplat /path/to/colmap/project"
        echo "   GPU: No NVIDIA GPU detected - using CPU mode"
        echo "   Best for: CPU-only environments"
    fi
fi

echo ""
echo "=== Quick Start Workflow ==="
echo ""
echo "Step 1: Prepare your images"
echo "  mkdir -p my_project/images"
echo "  cp /path/to/your/photos/*.jpg my_project/images/"
echo ""
echo "Step 2: Run COLMAP to extract camera poses"
echo "  colmap automatic_reconstructor \\"
echo "    --workspace_path my_project \\"
echo "    --image_path my_project/images"
echo ""
echo "Step 3: Train Gaussian Splats:"
echo "  ./opensplat my_project          # OpenSplat training"

if [[ "$PLATFORM" == "Darwin" ]]; then
    echo "  # OR"
    echo "  ./gsplat-train --data my_project  # Python training (Metal)"
fi

echo ""
echo "Step 4: View results"
echo "  Output: my_project/point_cloud.ply"
echo "  View in: SuperSplat (https://playcanvas.com/supersplat)"
echo ""
