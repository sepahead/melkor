#!/bin/bash
set -e
set -o pipefail

echo "========================================"
echo "gsplat CUDA Setup for Linux Multi-GPU"
echo "========================================"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TOOLS_DIR="$PROJECT_DIR/tools"
GSPLAT_DIR="$TOOLS_DIR/gsplat-cuda"

mkdir -p "$TOOLS_DIR"

# Check platform
if [[ "$(uname)" != "Linux" ]]; then
    echo "Error: This script is for Linux only."
    echo "For macOS, use: ./scripts/setup_gsplat_mps.sh"
    exit 1
fi

# Check for CUDA
echo "[1/6] Checking CUDA installation..."
if ! command -v nvidia-smi &> /dev/null; then
    echo "Error: nvidia-smi not found. Please install NVIDIA drivers."
    exit 1
fi

if ! command -v nvcc &> /dev/null; then
    echo "Error: nvcc not found. Please install CUDA Toolkit."
    echo "Install with:"
    echo "  wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb"
    echo "  sudo dpkg -i cuda-keyring_1.1-1_all.deb"
    echo "  sudo apt-get update && sudo apt-get install -y cuda-toolkit-12-3"
    exit 1
fi

CUDA_VERSION=$(nvcc --version | grep release | sed 's/.*release \([0-9]*\.[0-9]*\).*/\1/')
if [ -z "$CUDA_VERSION" ]; then
    echo "Error: Could not parse CUDA version from nvcc output"
    exit 1
fi
echo "Found CUDA $CUDA_VERSION ✓"

# Show GPU info
echo ""
echo "Available GPUs:"
nvidia-smi --query-gpu=index,name,memory.total --format=csv,noheader
GPU_COUNT=$(nvidia-smi --list-gpus 2>/dev/null | wc -l)
if [ -z "$GPU_COUNT" ] || [ "$GPU_COUNT" -eq 0 ]; then
    GPU_COUNT=1
fi
echo ""
echo "Detected $GPU_COUNT GPU(s)"
echo ""

# Check Python version
echo "[2/6] Checking Python..."
if ! command -v python3 &> /dev/null; then
    echo "Error: Python3 not found. Install with: sudo apt-get install python3 python3-pip python3-venv"
    exit 1
fi

PYTHON_VERSION=$(python3 --version 2>&1 | cut -d' ' -f2)
PYTHON_MAJOR=$(echo $PYTHON_VERSION | cut -d'.' -f1)
PYTHON_MINOR=$(echo $PYTHON_VERSION | cut -d'.' -f2)

if [ "$PYTHON_MAJOR" -lt 3 ] || ([ "$PYTHON_MAJOR" -eq 3 ] && [ "$PYTHON_MINOR" -lt 10 ]); then
    echo "Error: Python 3.10+ required. Found: $PYTHON_VERSION"
    echo "Install with: sudo apt-get install python3.11 python3.11-venv"
    exit 1
fi
echo "Found Python $PYTHON_VERSION ✓"

# Clone gsplat
echo "[3/6] Cloning gsplat..."
if [ ! -d "$GSPLAT_DIR" ]; then
    git clone --recursive https://github.com/nerfstudio-project/gsplat.git "$GSPLAT_DIR"
else
    echo "gsplat already cloned, updating..."
    cd "$GSPLAT_DIR" && git pull && git submodule update --init --recursive
fi

# Create virtual environment
echo "[4/6] Setting up Python virtual environment..."
cd "$GSPLAT_DIR"

if [ ! -d "venv" ]; then
    python3 -m venv venv
fi

source venv/bin/activate || {
    echo "Error: Failed to activate virtual environment"
    exit 1
}

# Install PyTorch with CUDA
echo "[5/6] Installing dependencies (this may take several minutes)..."
pip install --upgrade pip

# Determine PyTorch CUDA version based on system CUDA
CUDA_MAJOR=$(echo $CUDA_VERSION | cut -d'.' -f1)
if [ "$CUDA_MAJOR" -ge 12 ]; then
    echo "Installing PyTorch with CUDA 12.1 support..."
    pip install torch torchvision --index-url https://download.pytorch.org/whl/cu121 || {
        echo "Error: Failed to install PyTorch with CUDA 12.1"
        exit 1
    }
else
    echo "Installing PyTorch with CUDA 11.8 support..."
    pip install torch torchvision --index-url https://download.pytorch.org/whl/cu118 || {
        echo "Error: Failed to install PyTorch with CUDA 11.8"
        exit 1
    }
fi

# Install gsplat from source (builds CUDA kernels)
echo "Building gsplat CUDA kernels (this may take 5-10 minutes)..."
pip install -e ".[dev]" || {
    echo "Error: Failed to build gsplat CUDA kernels"
    echo "Check that your CUDA toolkit version matches PyTorch CUDA version"
    exit 1
}

# Verify installation
echo "Verifying gsplat installation..."
python -c "import gsplat; print(f'gsplat version: {gsplat.__version__ if hasattr(gsplat, \"__version__\") else \"installed\"}')" || {
    echo "Error: gsplat import failed. Installation may be incomplete."
    exit 1
}

# Install training requirements
pip install opencv-python plyfile tqdm lpips tensorboard nerfview
pip install -r examples/requirements.txt 2>/dev/null || echo "Note: Some optional requirements may not be available"

# Create wrapper scripts
echo "[6/6] Creating wrapper scripts..."

# Training wrapper with multi-GPU support
cat > "$PROJECT_DIR/gsplat-cuda-train" << 'WRAPPER'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GSPLAT_DIR="$SCRIPT_DIR/tools/gsplat-cuda"
cd "$GSPLAT_DIR"
source venv/bin/activate

# Default to all available GPUs if not specified
if [ -z "$CUDA_VISIBLE_DEVICES" ]; then
    GPU_COUNT=$(nvidia-smi --query-gpu=count --format=csv,noheader | head -1)
    if [ "$GPU_COUNT" -gt 1 ]; then
        echo "Multiple GPUs detected. Use CUDA_VISIBLE_DEVICES or --gpu-ids to select GPUs."
        echo "Example: CUDA_VISIBLE_DEVICES=0,1,2,3 $0 $@"
    fi
fi

python examples/simple_trainer.py "$@"
WRAPPER
chmod +x "$PROJECT_DIR/gsplat-cuda-train"

# Multi-GPU distributed training wrapper
cat > "$PROJECT_DIR/gsplat-cuda-train-distributed" << 'WRAPPER'
#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GSPLAT_DIR="$SCRIPT_DIR/tools/gsplat-cuda"

print_usage() {
    echo "gsplat Multi-GPU Distributed Training"
    echo ""
    echo "Usage: $0 [options] -- [gsplat args]"
    echo ""
    echo "Options:"
    echo "  --gpus <ids>       Comma-separated GPU IDs (e.g., 0,1,2,3)"
    echo "  --nproc <n>        Number of processes (default: number of GPUs)"
    echo "  --port <port>      Master port (default: 29500)"
    echo "  -h, --help         Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 --gpus 0,1,2,3 -- default --data_dir ~/data/scene"
    echo "  $0 --gpus 0,1 --port 29501 -- mcmc --data_dir ~/data/scene"
}

# Defaults
GPU_IDS=""
NPROC=""
MASTER_PORT=29500
GSPLAT_ARGS=()

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --gpus)
            GPU_IDS="$2"; shift 2 ;;
        --nproc)
            NPROC="$2"; shift 2 ;;
        --port)
            MASTER_PORT="$2"; shift 2 ;;
        -h|--help)
            print_usage; exit 0 ;;
        --)
            shift; GSPLAT_ARGS=("$@"); break ;;
        *)
            GSPLAT_ARGS+=("$1"); shift ;;
    esac
done

# Determine GPU configuration
if [ -z "$GPU_IDS" ]; then
    GPU_COUNT=$(nvidia-smi --query-gpu=count --format=csv,noheader | head -1)
    GPU_IDS=$(seq -s, 0 $((GPU_COUNT - 1)))
    echo "Using all $GPU_COUNT GPUs: $GPU_IDS"
fi

IFS=',' read -ra GPU_ARRAY <<< "$GPU_IDS"
NUM_GPUS=${#GPU_ARRAY[@]}

if [ -z "$NPROC" ]; then
    NPROC=$NUM_GPUS
fi

export CUDA_VISIBLE_DEVICES="$GPU_IDS"
export MASTER_ADDR=localhost
export MASTER_PORT=$MASTER_PORT

cd "$GSPLAT_DIR"
source venv/bin/activate

echo "Starting distributed training:"
echo "  GPUs: $GPU_IDS ($NUM_GPUS total)"
echo "  Processes: $NPROC"
echo "  Master: $MASTER_ADDR:$MASTER_PORT"
echo ""

# Use torchrun for distributed training
torchrun --nproc_per_node=$NPROC \
    --master_addr=$MASTER_ADDR \
    --master_port=$MASTER_PORT \
    examples/simple_trainer.py "${GSPLAT_ARGS[@]}"
WRAPPER
chmod +x "$PROJECT_DIR/gsplat-cuda-train-distributed"

# Python environment wrapper
cat > "$PROJECT_DIR/gsplat-cuda-python" << 'WRAPPER'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GSPLAT_DIR="$SCRIPT_DIR/tools/gsplat-cuda"
source "$GSPLAT_DIR/venv/bin/activate"
python "$@"
WRAPPER
chmod +x "$PROJECT_DIR/gsplat-cuda-python"

deactivate

echo ""
echo "========================================"
echo "gsplat CUDA installed successfully! ✓"
echo "========================================"
echo ""
echo "Detected $GPU_COUNT GPU(s) for multi-GPU training"
echo ""
echo "Usage:"
echo ""
echo "  Single-GPU training:"
echo "    ./gsplat-cuda-train default --data_dir /path/to/colmap_project --result_dir ./output"
echo ""
echo "  Multi-GPU distributed training:"
echo "    ./gsplat-cuda-train-distributed --gpus 0,1,2,3 -- default --data_dir /path/to/colmap_project --result_dir ./output"
echo ""
echo "  Specific GPUs:"
echo "    CUDA_VISIBLE_DEVICES=0,1 ./gsplat-cuda-train default --data_dir /path/to/colmap_project --result_dir ./output"
echo ""
echo "Training strategies:"
echo "  default  - Standard 3D Gaussian Splatting"
echo "  mcmc     - MCMC-based densification (often better quality)"
echo ""
echo "Documentation: docs/GSPLAT_CUDA.md"
echo ""
