#!/usr/bin/env bash

# Setup script for Depth-Anything-3 (DA3) CUDA inference.
# One scene must remain a single joint multi-view inference call. The retired
# multi-GPU wrapper incorrectly split a scene's views across independent ranks;
# use upstream DA3-Streaming for long scenes instead.

set -euo pipefail

# Pin upstream for reproducible installs. Override deliberately when reviewing
# a newer revision; never silently `git pull` a moving branch.
DA3_REF="${MELKOR_DA3_REF:-41736238f5bced4debf3f2a12375d2466874866d}"
DA3_REPO="https://github.com/ByteDance-Seed/Depth-Anything-3.git"
PYTHON_BIN="${MELKOR_PYTHON:-python3}"
ACCEPT_NONCOMMERCIAL=0
ACCEPT_UNLOCKED=0
STAGING_DIR=""

cleanup() {
    if [[ -n "$STAGING_DIR" && -d "$STAGING_DIR" ]]; then
        rm -rf -- "$STAGING_DIR"
    fi
}
trap cleanup EXIT
for arg in "$@"; do
    case "$arg" in
        --accept-noncommercial) ACCEPT_NONCOMMERCIAL=1 ;;
        --accept-unlocked-dependencies) ACCEPT_UNLOCKED=1 ;;
        -h|--help)
            echo "Usage: $0 --accept-unlocked-dependencies [--accept-noncommercial]"
            echo "The unlocked flag acknowledges that Python dependencies lack a hash lock."
            echo "The flag is required before downloading CC-BY-NC DA3 1.1 weights."
            exit 0
            ;;
        *)
            echo "Error: unknown option: $arg"
            exit 1
            ;;
    esac
done
if [ "$ACCEPT_UNLOCKED" -ne 1 ]; then
    echo "Error: setup requires --accept-unlocked-dependencies."
    echo "The source and model revisions are pinned, but Python dependencies are not hash-locked."
    exit 2
fi
if [[ ! "$DA3_REF" =~ ^[0-9a-f]{40}$ ]]; then
    echo "Error: MELKOR_DA3_REF must be a full lowercase commit SHA."
    exit 2
fi

echo "=========================================="
echo "Depth-Anything-3 Setup (CUDA)"
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
if ! command -v "$PYTHON_BIN" &> /dev/null; then
    echo "Error: Python 3 is required but not installed."
    exit 1
fi

PYTHON_VERSION=$($PYTHON_BIN --version 2>&1 | awk '{print $2}')
echo "Found Python: $PYTHON_VERSION"
if ! "$PYTHON_BIN" -c 'import sys; raise SystemExit(not ((3, 10) <= sys.version_info[:2] <= (3, 13)))'; then
    echo "Error: Melkor's DA3 bridge supports Python 3.10 through 3.13; found $PYTHON_VERSION."
    echo "Set MELKOR_PYTHON to a supported interpreter (Python 3.11 or 3.12 recommended)."
    exit 1
fi

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
    if [ "$(git -C "$DA3_DIR/Depth-Anything-3" remote get-url origin)" != "$DA3_REPO" ]; then
        echo "Error: the existing DA3 checkout has an unexpected origin."
        exit 2
    fi
    echo "Selecting pinned Depth-Anything-3 revision $DA3_REF..."
    git -C "$DA3_DIR/Depth-Anything-3" fetch --depth 1 origin "$DA3_REF"
    git -C "$DA3_DIR/Depth-Anything-3" checkout --detach FETCH_HEAD
else
    echo "Cloning Depth-Anything-3 @ $DA3_REF..."
    mkdir -p "$DA3_DIR"
    git clone --filter=blob:none --no-checkout \
        "$DA3_REPO" \
        "$DA3_DIR/Depth-Anything-3"
    git -C "$DA3_DIR/Depth-Anything-3" fetch --depth 1 origin "$DA3_REF"
    git -C "$DA3_DIR/Depth-Anything-3" checkout --detach FETCH_HEAD
fi
if [ "$(git -C "$DA3_DIR/Depth-Anything-3" rev-parse HEAD)" != "$DA3_REF" ]; then
    echo "Error: the DA3 checkout does not match the requested commit."
    exit 2
fi
if [ -n "$(git -C "$DA3_DIR/Depth-Anything-3" status --porcelain --untracked-files=all)" ]; then
    echo "Error: the DA3 checkout contains local or generated files."
    echo "Use a clean checkout before setup."
    exit 2
fi

# Create virtual environment
if [ ! -d "$VENV_DIR" ]; then
    echo ""
    echo "Creating virtual environment..."
    "$PYTHON_BIN" -m venv "$VENV_DIR"
else
    echo "Using existing virtual environment"
fi

# Activate virtual environment
source "$VENV_DIR/bin/activate"

echo ""
echo "Installing Python dependencies..."
python -m pip install --upgrade pip

# Install PyTorch with CUDA. The CUDA toolkit reported by nvcc does not select
# a wheel: PyTorch wheels carry their own CUDA runtime and compatibility is
# determined primarily by the NVIDIA driver/GPU generation. Use the current
# stable PyPI wheel by default; operators of legacy Pascal/Volta systems can
# point at PyTorch's supported legacy index (currently cu126) explicitly.
echo ""
echo "Installing PyTorch with CUDA support..."
if [ -n "${MELKOR_TORCH_INDEX_URL:-}" ]; then
    echo "Using configured PyTorch index: $MELKOR_TORCH_INDEX_URL"
    python -m pip install torch torchvision --index-url "$MELKOR_TORCH_INDEX_URL"
else
    python -m pip install torch torchvision
fi

# Verify CUDA is available in PyTorch
python -c "import torch; print(f'PyTorch {torch.__version__}, CUDA runtime {torch.version.cuda}')"
if ! python -c "import torch; raise SystemExit(not torch.cuda.is_available())"; then
    echo "Error: installed PyTorch cannot access CUDA."
    echo "Choose a supported wheel at https://pytorch.org/get-started/locally/ and rerun."
    exit 1
fi
python -c "import torch; print(f'PyTorch CUDA device count: {torch.cuda.device_count()}')"

# Install DA3 dependencies
echo ""
echo "Installing Depth-Anything-3..."
cd "$DA3_DIR/Depth-Anything-3"
python -m pip install -e '.[gs]'

# Install additional dependencies for 3DGS conversion
echo ""
echo "Installing additional dependencies..."
python -m pip install \
    numpy \
    pillow \
    tqdm \
    einops \
    opencv-python \
    scipy \
    plyfile \
    trimesh
# Open3D is used only by optional visualization/export helpers. Its wheel is
# unavailable on some supported Python/platform combinations, so keep this
# failure isolated from required runtime dependency installation.
python -m pip install open3d || echo "Warning: optional Open3D could not be installed"

echo ""
echo "=========================================="
echo "Downloading model weights..."
echo "=========================================="

download_model() {
    local name="$1"
    local output_dir="$MODEL_DIR"
    local revision=""

    case "$name" in
        DA3-SMALL) revision="e08cab65ca0ec38e7826075418411ab90cab4da3" ;;
        DA3-BASE) revision="f4a6c9b3c95e41c82048423d3493a81ec3fa810e" ;;
        DA3-LARGE-1.1) revision="0e109ae307c5982f319a67cf6f9f99ccdc0ec97c" ;;
        DA3-GIANT-1.1) revision="72ee9f89ce4e50d704e9d55ee9c646ec8dc25a19" ;;
        DA3NESTED-GIANT-LARGE-1.1) revision="b2359bdf726fb44ef62acca04d629dcf158053e7" ;;
        DA3MONO-LARGE) revision="f465978e618db8cc79c83b8bbf24964857db1875" ;;
        DA3METRIC-LARGE) revision="4010e39f3634a45bc60553321fb49fb760bd594e" ;;
        *) echo "Error: no reviewed revision is registered for $name"; return 1 ;;
    esac

    case "$name" in
        DA3-LARGE-1.1|DA3-GIANT-1.1|DA3NESTED-GIANT-LARGE-1.1)
            if [ "$ACCEPT_NONCOMMERCIAL" -ne 1 ]; then
                echo "Error: $name weights are CC-BY-NC-4.0 (non-commercial)."
                echo "Review the model card, then rerun with --accept-noncommercial if eligible."
                return 1
            fi
            ;;
    esac
    
    mkdir -p "$output_dir"
    
    local target="$output_dir/$name"
    local marker="$target/.melkor-revision"
    if [ -d "$target" ]; then
        if [ -s "$target/config.json" ] &&
           [ -f "$marker" ] && [ "$(cat "$marker")" = "$revision" ] &&
           find "$target" -type f \( -name '*.safetensors' -o -name 'pytorch_model*.bin' \) -size +0c -print -quit | grep -q .; then
            echo "Model $name already exists at reviewed revision $revision."
            return 0
        fi
        echo "Error: $target is partial, unverified, or from a different revision."
        echo "Move it aside and rerun. The installer will not trust or overwrite it."
        return 1
    else
        echo "Downloading $name from Hugging Face at $revision..."
        STAGING_DIR="$(mktemp -d "$output_dir/.melkor-${name}.XXXXXX")"
        # Pass values as argv, not interpolated Python source. snapshot_download
        # verifies Hugging Face's content-addressed cache and supports resumable
        # transfers for these multi-GB repositories.
        python - "$name" "$STAGING_DIR" "$revision" <<'PY' || {
import sys
from huggingface_hub import snapshot_download

name, output_dir, revision = sys.argv[1:]
snapshot_download(
    repo_id=f"depth-anything/{name}",
    revision=revision,
    local_dir=output_dir,
)
print(f"Downloaded {name}")
PY
            rm -rf -- "$STAGING_DIR"
            STAGING_DIR=""
            echo "Error: Failed to download $name"
            return 1
        }
        if [ ! -s "$STAGING_DIR/config.json" ] ||
           ! find "$STAGING_DIR" -type f \( -name '*.safetensors' -o -name 'pytorch_model*.bin' \) -size +0c -print -quit | grep -q .; then
            rm -rf -- "$STAGING_DIR"
            STAGING_DIR=""
            echo "Error: downloaded snapshot for $name is incomplete"
            return 1
        fi
        printf '%s\n' "$revision" > "$STAGING_DIR/.melkor-revision"
        mv "$STAGING_DIR" "$target"
        STAGING_DIR=""
        echo "Downloaded $name to $target"
    fi
}

# Ask which models to download
echo ""
echo "=========================================="
echo "Which DA3 models would you like to download?"
echo "=========================================="
echo ""
echo "MAIN SERIES (Any-View Depth + Pose → camera-aware point splats):"
echo "  All main models predict jointly consistent depth and camera poses."
echo "  The learned Gaussian head is available only on GIANT and NESTED."
echo ""
echo "  1. DA3-BASE (~2GB)               - depth/pose; derived point splats"
echo "  2. DA3-LARGE-1.1 (~4GB)           - refreshed depth/pose; point splats (NC)"
echo "  3. DA3-SMALL (~1GB)               - depth/pose; point splats"
echo "  4. DA3-GIANT-1.1 (~8GB)           - refreshed learned Gaussian head (NC)"
echo ""
echo "  5. DA3NESTED-GIANT-LARGE-1.1 (~12GB) - refreshed GS + metric alignment (NC)"
echo "                                       Output: learned Gaussians + metric depth"
echo ""
echo "BUNDLES:"
echo "  6. All reconstruction models"
echo "  7. None (rerun this installer later)"
echo ""
echo "DA3MONO-LARGE and DA3METRIC-LARGE are depth-only checkpoints. Use the"
echo "official DA3 depth exporter; Melkor does not present them as splat models."
echo ""
read -p "Enter choice [1-7]: " choice

case $choice in
    1)
        download_model "DA3-BASE"
        ;;
    2)
        download_model "DA3-LARGE-1.1"
        ;;
    3)
        download_model "DA3-SMALL"
        ;;
    4)
        download_model "DA3-GIANT-1.1"
        ;;
    5)
        download_model "DA3NESTED-GIANT-LARGE-1.1"
        ;;
    6)
        if [ "$ACCEPT_NONCOMMERCIAL" -ne 1 ]; then
            echo "Error: the reconstruction bundle includes CC-BY-NC-4.0 weights."
            echo "Rerun with --accept-noncommercial if eligible."
            exit 1
        fi
        download_model "DA3-SMALL"
        download_model "DA3-BASE"
        download_model "DA3-LARGE-1.1"
        download_model "DA3-GIANT-1.1"
        download_model "DA3NESTED-GIANT-LARGE-1.1"
        ;;
    7)
        echo "Skipping model download. Rerun scripts/setup_da3.sh with the required"
        echo "dependency acceptance flag later so the"
        echo "snapshot is revision-pinned, validated, and marked atomically."
        ;;
    *)
        echo "Error: invalid choice."
        exit 1
        ;;
esac

# Create wrapper scripts
echo ""
echo "Creating wrapper scripts..."

# Main DA3 inference wrapper
cat > "$PROJECT_ROOT/da3-infer" << 'WRAPPER_EOF'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DA3_DIR="$SCRIPT_DIR/tools/da3"
if [ ! -f "$DA3_DIR/venv/bin/activate" ]; then
    echo "Error: DA3 environment is missing. Run scripts/setup_da3.sh --accept-unlocked-dependencies" >&2
    exit 1
fi
source "$DA3_DIR/venv/bin/activate"
exec python3 "$DA3_DIR/inference.py" "$@"
WRAPPER_EOF
chmod +x "$PROJECT_ROOT/da3-infer"

# Python wrapper for direct imports
cat > "$PROJECT_ROOT/da3-python" << 'WRAPPER_EOF'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DA3_DIR="$SCRIPT_DIR/tools/da3"
if [ ! -f "$DA3_DIR/venv/bin/activate" ]; then
    echo "Error: DA3 environment is missing. Run scripts/setup_da3.sh --accept-unlocked-dependencies" >&2
    exit 1
fi
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
echo "  # Long sequences / constrained VRAM: use upstream DA3-Streaming"
echo "  # (keeps each scene's joint coordinate frame intact)"
echo ""
echo "  # Compress the PLY to SPZ with melkor:"
echo "  ./da3-infer --input images/ --output output.ply"
echo "  ./build/melkor output.ply output.spz"
echo ""
echo "  # With specific model:"
echo "  ./da3-infer --model da3-large-1.1 --input images/ --output output.ply"
echo ""
echo "For more options, run: ./da3-infer --help"
echo ""
