#!/bin/bash
set -e

# Detect platform
PLATFORM=$(uname)

if [[ "$PLATFORM" == "Darwin" ]]; then
    echo "========================================"
    echo "OpenSplat Setup for Apple Silicon (Metal)"
    echo "========================================"
elif [[ "$PLATFORM" == "Linux" ]]; then
    echo "========================================"
    echo "OpenSplat Setup for Linux (CUDA/CPU)"
    echo "========================================"
else
    echo "========================================"
    echo "OpenSplat Setup"
    echo "========================================"
fi
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TOOLS_DIR="$PROJECT_DIR/tools"
OPENSPLAT_DIR="$TOOLS_DIR/OpenSplat"

mkdir -p "$TOOLS_DIR"

# Detect number of CPU cores
if [[ "$PLATFORM" == "Darwin" ]]; then
    NUM_CORES=$(sysctl -n hw.ncpu)
elif [[ "$PLATFORM" == "Linux" ]]; then
    NUM_CORES=$(nproc)
else
    NUM_CORES=4
fi

# Check for required tools
echo "[1/6] Checking prerequisites..."

if [[ "$PLATFORM" == "Darwin" ]]; then
    if ! command -v brew &> /dev/null; then
        echo "Error: Homebrew not found. Install from https://brew.sh"
        exit 1
    fi

    if ! command -v cmake &> /dev/null; then
        echo "Installing CMake..."
        brew install cmake
    fi

    if ! brew list libomp &> /dev/null; then
        echo "Installing libomp..."
        brew install libomp
        brew link libomp --force 2>/dev/null || true
    fi
elif [[ "$PLATFORM" == "Linux" ]]; then
    if ! command -v cmake &> /dev/null; then
        echo "Installing CMake..."
        sudo apt-get update
        sudo apt-get install -y cmake
    fi
    
    # Install build dependencies
    echo "Installing build dependencies..."
    sudo apt-get install -y build-essential git unzip libopencv-dev libeigen3-dev
fi

# Check Python
echo "[2/6] Checking Python..."
if command -v python3 &> /dev/null; then
    PYTHON_VERSION=$(python3 --version 2>&1 | cut -d' ' -f2)
    PYTHON_MAJOR=$(echo $PYTHON_VERSION | cut -d'.' -f1)
    PYTHON_MINOR=$(echo $PYTHON_VERSION | cut -d'.' -f2)

    if [ "$PYTHON_MAJOR" -lt 3 ] || ([ "$PYTHON_MAJOR" -eq 3 ] && [ "$PYTHON_MINOR" -lt 10 ]); then
        echo "Warning: Python 3.10+ recommended. Found: $PYTHON_VERSION"
    else
        echo "Found Python $PYTHON_VERSION ✓"
    fi
else
    echo "Warning: Python3 not found. Some features may not work."
fi

# Download LibTorch (or use custom path if LIBTORCH_DIR is set)
echo "[3/6] Setting up LibTorch..."

# Allow user to specify custom LibTorch location
if [ -n "$LIBTORCH_DIR" ] && [ -d "$LIBTORCH_DIR" ]; then
    echo "Using custom LibTorch from: $LIBTORCH_DIR ✓"
else
    LIBTORCH_DIR="$TOOLS_DIR/libtorch"
    
    if [ ! -d "$LIBTORCH_DIR" ]; then
        cd "$TOOLS_DIR"
        
        if [[ "$PLATFORM" == "Darwin" ]]; then
            echo "Downloading LibTorch for Apple Silicon..."
            LIBTORCH_URL="https://download.pytorch.org/libtorch/cpu/libtorch-macos-arm64-2.2.0.zip"
        elif [[ "$PLATFORM" == "Linux" ]]; then
            # Check if CUDA is available
            if command -v nvidia-smi &> /dev/null && command -v nvcc &> /dev/null; then
                echo "Downloading LibTorch for Linux (CUDA)..."
                LIBTORCH_URL="https://download.pytorch.org/libtorch/cu121/libtorch-cxx11-abi-shared-with-deps-2.2.0%2Bcu121.zip"
                GPU_RUNTIME="CUDA"
            else
                echo "Downloading LibTorch for Linux (CPU)..."
                LIBTORCH_URL="https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-2.2.0%2Bcpu.zip"
                GPU_RUNTIME="CPU"
            fi
        else
            echo "Error: Unsupported platform"
            exit 1
        fi
        
        curl -L "$LIBTORCH_URL" -o libtorch.zip
        unzip -q libtorch.zip
        rm libtorch.zip
        echo "LibTorch downloaded ✓"
    else
        echo "LibTorch already exists ✓"
    fi
fi

# Clone OpenSplat (or use custom path if OPENSPLAT_DIR is set)
echo "[4/6] Setting up OpenSplat..."

# Allow user to specify custom OpenSplat location
if [ -n "$OPENSPLAT_SRC" ] && [ -d "$OPENSPLAT_SRC" ]; then
    echo "Using custom OpenSplat from: $OPENSPLAT_SRC"
    OPENSPLAT_DIR="$OPENSPLAT_SRC"
elif [ ! -d "$OPENSPLAT_DIR" ]; then
    echo "Cloning OpenSplat..."
    git clone https://github.com/pierotofy/OpenSplat.git "$OPENSPLAT_DIR"
else
    echo "OpenSplat already cloned, updating..."
    cd "$OPENSPLAT_DIR" && git pull
fi

# Build OpenSplat
echo "[5/6] Building OpenSplat..."
cd "$OPENSPLAT_DIR"
mkdir -p build && cd build

if [[ "$PLATFORM" == "Darwin" ]]; then
    echo "Building with Metal (MPS) support..."
    cmake -DCMAKE_PREFIX_PATH="$LIBTORCH_DIR" \
          -DGPU_RUNTIME=MPS \
          -DCMAKE_BUILD_TYPE=Release \
          ..
elif [[ "$PLATFORM" == "Linux" ]]; then
    # Determine GPU runtime
    if command -v nvidia-smi &> /dev/null && command -v nvcc &> /dev/null; then
        echo "Building with CUDA support..."
        cmake -DCMAKE_PREFIX_PATH="$LIBTORCH_DIR" \
              -DGPU_RUNTIME=CUDA \
              -DCMAKE_BUILD_TYPE=Release \
              ..
    else
        echo "Building with CPU support (no CUDA detected)..."
        cmake -DCMAKE_PREFIX_PATH="$LIBTORCH_DIR" \
              -DGPU_RUNTIME=CPU \
              -DCMAKE_BUILD_TYPE=Release \
              ..
    fi
fi

make -j${NUM_CORES}

# Create convenience wrapper
echo "[6/6] Creating wrapper script..."
cat > "$PROJECT_DIR/opensplat" << 'WRAPPER'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/tools/OpenSplat/build/opensplat" "$@"
WRAPPER
chmod +x "$PROJECT_DIR/opensplat"

echo ""
echo "========================================"
echo "OpenSplat installed successfully! ✓"
echo "========================================"
echo ""
echo "Usage:"
echo "  ./opensplat /path/to/colmap/project"
echo ""
echo "The input should be a COLMAP project with:"
echo "  - images/ folder with your photos"
echo "  - sparse/0/ folder with cameras.bin, images.bin, points3D.bin"
echo ""
echo "To create a COLMAP project from images:"
if [[ "$PLATFORM" == "Darwin" ]]; then
    echo "  1. Install COLMAP: brew install colmap"
else
    echo "  1. Install COLMAP: sudo apt-get install colmap"
fi
echo "  2. Run: colmap automatic_reconstructor --workspace_path /your/project --image_path /your/images"
echo ""
echo "Output: point_cloud.ply (3D Gaussian Splat file)"
