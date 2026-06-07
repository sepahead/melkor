#!/bin/bash
set -e

echo "========================================"
echo "gsplat-MPS Setup for Apple Silicon"
echo "========================================"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TOOLS_DIR="$PROJECT_DIR/tools"
GSPLAT_DIR="$TOOLS_DIR/gsplat-mps"

mkdir -p "$TOOLS_DIR"

# Check Python version
echo "[1/5] Checking Python..."
PYTHON_VERSION=$(python3 --version 2>&1 | cut -d' ' -f2)
PYTHON_MAJOR=$(echo $PYTHON_VERSION | cut -d'.' -f1)
PYTHON_MINOR=$(echo $PYTHON_VERSION | cut -d'.' -f2)

if [ "$PYTHON_MAJOR" -lt 3 ] || ([ "$PYTHON_MAJOR" -eq 3 ] && [ "$PYTHON_MINOR" -lt 10 ]); then
    echo "Error: Python 3.10+ required. Found: $PYTHON_VERSION"
    echo "Install with: brew install python@3.11"
    exit 1
fi
echo "Found Python $PYTHON_VERSION ✓"

# Clone gsplat-mps
echo "[2/5] Cloning gsplat-mps..."
if [ ! -d "$GSPLAT_DIR" ]; then
    git clone --recursive https://github.com/iffyloop/gsplat-mps.git "$GSPLAT_DIR"
else
    echo "gsplat-mps already cloned, updating..."
    cd "$GSPLAT_DIR" && git pull && git submodule update --init --recursive
fi

# Create virtual environment
echo "[3/5] Setting up Python virtual environment..."
cd "$GSPLAT_DIR"

if [ ! -d "venv" ]; then
    python3 -m venv venv
fi

source venv/bin/activate

# Install dependencies
echo "[4/5] Installing dependencies (this may take a few minutes)..."
pip install --upgrade pip
pip install torch torchvision
pip install -e ".[dev]"
pip install -r examples/requirements.txt 2>/dev/null || echo "Note: Some optional requirements may not be available"

# Create wrapper scripts
echo "[5/5] Creating wrapper scripts..."

# Training wrapper
cat > "$PROJECT_DIR/gsplat-train" << 'WRAPPER'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GSPLAT_DIR="$SCRIPT_DIR/tools/gsplat-mps"
cd "$GSPLAT_DIR"
source venv/bin/activate
python examples/simple_trainer.py "$@"
WRAPPER
chmod +x "$PROJECT_DIR/gsplat-train"

# Python environment wrapper for custom scripts
cat > "$PROJECT_DIR/gsplat-python" << 'WRAPPER'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GSPLAT_DIR="$SCRIPT_DIR/tools/gsplat-mps"
source "$GSPLAT_DIR/venv/bin/activate"
python "$@"
WRAPPER
chmod +x "$PROJECT_DIR/gsplat-python"

deactivate

echo ""
echo "========================================"
echo "gsplat-MPS installed successfully! ✓"
echo "========================================"
echo ""
echo "Usage:"
echo "  ./gsplat-train                    # Run the example trainer"
echo "  ./gsplat-python your_script.py    # Run custom scripts with gsplat"
echo ""
echo "The gsplat-mps library provides:"
echo "  - Differentiable Gaussian rasterization on Metal"
echo "  - Python API for training 3D Gaussian splats"
echo "  - Example trainer for quick experiments"
echo ""
echo "For custom training, see:"
echo "  $GSPLAT_DIR/examples/"
