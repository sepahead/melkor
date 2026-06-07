#!/bin/bash
set -e

# ============================================================================
# LichtFeld-Studio Setup Script
# ============================================================================
# High-performance 3D Gaussian Splatting implementation using C++23 and CUDA
# 
# Requirements:
#   - Linux (Ubuntu 22.04+ recommended)
#   - NVIDIA GPU with CUDA 12.8+ support
#   - GCC 11+ (C++23 support)
#   - CMake 3.20+
#   - Ninja build system
#
# Note: This tool is Linux CUDA only. On macOS, use OpenSplat or gsplat-mps.
# ============================================================================

VERSION="1.0.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TOOLS_DIR="$PROJECT_DIR/tools"
LICHTFELD_DIR="$TOOLS_DIR/LichtFeld-Studio"

# ============================================================================
# Color Output
# ============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[✓]${NC} $1"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1" >&2; }
log_step()    { echo -e "\n${CYAN}=== $1 ===${NC}"; }

# ============================================================================
# Platform Check
# ============================================================================
PLATFORM=$(uname -s)
ARCH=$(uname -m)

if [[ "$PLATFORM" != "Linux" ]]; then
    log_error "LichtFeld-Studio requires Linux with NVIDIA CUDA."
    log_info "On macOS, use OpenSplat or gsplat-mps instead:"
    log_info "  ./scripts/setup_opensplat.sh"
    log_info "  ./scripts/setup_gsplat_mps.sh"
    exit 1
fi

echo -e "${CYAN}╔══════════════════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║           LichtFeld-Studio Setup for Linux (CUDA)                            ║${NC}"
echo -e "${CYAN}║       High-Performance 3D Gaussian Splatting (C++23 + CUDA 12.8+)           ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════════════════════╝${NC}"
echo ""

mkdir -p "$TOOLS_DIR"

# ============================================================================
# Prerequisites Check
# ============================================================================
log_step "Checking Prerequisites [1/6]"

# Check for NVIDIA GPU
if ! command -v nvidia-smi &> /dev/null; then
    log_error "nvidia-smi not found. NVIDIA drivers are required."
    log_info "Install NVIDIA drivers first:"
    log_info "  sudo apt-get install nvidia-driver-535  # or latest version"
    exit 1
fi

# Display GPU info
log_info "Detected NVIDIA GPU(s):"
nvidia-smi --query-gpu=index,name,driver_version,memory.total --format=csv,noheader | while read -r line; do
    echo "  GPU $line"
done
echo ""

# Check CUDA version
if command -v nvcc &> /dev/null; then
    CUDA_VERSION=$(nvcc --version | grep "release" | awk '{print $6}' | cut -d',' -f1)
    log_info "Found CUDA: $CUDA_VERSION"
    
    # Extract major.minor version
    CUDA_MAJOR=$(echo "$CUDA_VERSION" | cut -d'.' -f1 | tr -d 'V')
    CUDA_MINOR=$(echo "$CUDA_VERSION" | cut -d'.' -f2)
    
    if [[ "$CUDA_MAJOR" -lt 12 ]] || [[ "$CUDA_MAJOR" -eq 12 && "$CUDA_MINOR" -lt 8 ]]; then
        log_warn "LichtFeld-Studio recommends CUDA 12.8+. Found: $CUDA_VERSION"
        log_warn "You may encounter compatibility issues."
    fi
else
    log_error "nvcc (CUDA compiler) not found."
    log_info "Install CUDA toolkit:"
    log_info "  wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb"
    log_info "  sudo dpkg -i cuda-keyring_1.1-1_all.deb"
    log_info "  sudo apt-get update"
    log_info "  sudo apt-get install cuda-toolkit-12-8"
    exit 1
fi

# Check GCC version (need 11+ for C++23)
if command -v gcc &> /dev/null; then
    GCC_VERSION=$(gcc -dumpversion)
    GCC_MAJOR=$(echo "$GCC_VERSION" | cut -d'.' -f1)
    
    if [[ "$GCC_MAJOR" -lt 11 ]]; then
        log_error "GCC 11+ required for C++23 support. Found: GCC $GCC_VERSION"
        log_info "Install GCC 11+:"
        log_info "  sudo apt-get install gcc-11 g++-11"
        log_info "  sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 11"
        log_info "  sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-11 11"
        exit 1
    fi
    log_success "Found GCC $GCC_VERSION"
else
    log_error "GCC not found."
    log_info "Install GCC:"
    log_info "  sudo apt-get install build-essential"
    exit 1
fi

# Check CMake
if command -v cmake &> /dev/null; then
    CMAKE_VERSION=$(cmake --version | head -1 | awk '{print $3}')
    log_success "Found CMake $CMAKE_VERSION"
else
    log_info "Installing CMake..."
    sudo apt-get update
    sudo apt-get install -y cmake
fi

# Check/Install Ninja
if command -v ninja &> /dev/null; then
    NINJA_VERSION=$(ninja --version)
    log_success "Found Ninja $NINJA_VERSION"
else
    log_info "Installing Ninja..."
    sudo apt-get install -y ninja-build
fi

# Install other dependencies
log_info "Installing build dependencies..."
sudo apt-get install -y \
    build-essential \
    git \
    unzip \
    wget \
    libopencv-dev \
    libeigen3-dev \
    libglfw3-dev \
    libglew-dev \
    libglm-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev \
    2>/dev/null || true

log_success "Prerequisites check complete"

# ============================================================================
# Clone LichtFeld-Studio
# ============================================================================
log_step "Setting up LichtFeld-Studio [2/6]"

if [[ -d "$LICHTFELD_DIR" ]]; then
    log_info "LichtFeld-Studio already cloned, updating..."
    cd "$LICHTFELD_DIR"
    git fetch origin
    git pull origin main || git pull origin master || true
else
    log_info "Cloning LichtFeld-Studio..."
    git clone https://github.com/MrNeRF/LichtFeld-Studio.git "$LICHTFELD_DIR"
fi

cd "$LICHTFELD_DIR"
log_success "LichtFeld-Studio cloned"

# ============================================================================
# Download LibTorch
# ============================================================================
log_step "Setting up LibTorch (CUDA 12.8) [3/6]"

LIBTORCH_DIR="$LICHTFELD_DIR/external/libtorch"
LIBTORCH_ZIP="libtorch-cxx11-abi-shared-with-deps-2.7.0+cu128.zip"
LIBTORCH_URL="https://download.pytorch.org/libtorch/cu128/$LIBTORCH_ZIP"

if [[ -d "$LIBTORCH_DIR" ]]; then
    log_success "LibTorch already exists"
else
    mkdir -p "$LICHTFELD_DIR/external"
    cd "$LICHTFELD_DIR/external"
    
    if [[ ! -f "$LIBTORCH_ZIP" ]]; then
        log_info "Downloading LibTorch 2.7.0 (CUDA 12.8)..."
        log_info "This may take a few minutes (~2GB download)..."
        wget -q --show-progress "$LIBTORCH_URL" -O "$LIBTORCH_ZIP"
    fi
    
    log_info "Extracting LibTorch..."
    unzip -q "$LIBTORCH_ZIP" -d .
    rm -f "$LIBTORCH_ZIP"
    
    log_success "LibTorch installed"
fi

# ============================================================================
# Build LichtFeld-Studio
# ============================================================================
log_step "Building LichtFeld-Studio [4/6]"

cd "$LICHTFELD_DIR"

# Clean build if requested
if [[ "$1" == "--clean" ]]; then
    log_info "Cleaning previous build..."
    rm -rf build
fi

# Configure with CMake
log_info "Configuring build with CMake..."
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja

# Build
NUM_CORES=$(nproc)
log_info "Building with $NUM_CORES cores..."
log_info "This may take several minutes on first build..."
cmake --build build -- -j"$NUM_CORES"

if [[ ! -f "$LICHTFELD_DIR/build/LichtFeld-Studio" ]]; then
    log_error "Build failed - executable not found"
    exit 1
fi

log_success "Build complete"

# ============================================================================
# Create Wrapper Script
# ============================================================================
log_step "Creating Wrapper Scripts [5/6]"

# Main wrapper
cat > "$PROJECT_DIR/lichtfeld" << 'WRAPPER'
#!/bin/bash
# LichtFeld-Studio wrapper script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LICHTFELD_BIN="$SCRIPT_DIR/tools/LichtFeld-Studio/build/LichtFeld-Studio"

if [[ ! -x "$LICHTFELD_BIN" ]]; then
    echo "Error: LichtFeld-Studio not built. Run: ./scripts/setup_lichtfeld.sh"
    exit 1
fi

exec "$LICHTFELD_BIN" "$@"
WRAPPER
chmod +x "$PROJECT_DIR/lichtfeld"

log_success "Wrapper script created: ./lichtfeld"

# ============================================================================
# Verify Installation
# ============================================================================
log_step "Verifying Installation [6/6]"

# Check if binary runs
if "$PROJECT_DIR/lichtfeld" --help &>/dev/null || "$PROJECT_DIR/lichtfeld" -h &>/dev/null; then
    log_success "LichtFeld-Studio is working"
else
    # Try running without args (might show usage)
    log_info "Testing binary..."
    timeout 5 "$PROJECT_DIR/lichtfeld" 2>&1 | head -5 || true
    log_success "Binary is executable"
fi

# ============================================================================
# Success!
# ============================================================================
echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║           LichtFeld-Studio Installed Successfully! ✓                         ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════════════════════════╝${NC}"
echo ""
log_info "Usage:"
echo "  ./lichtfeld -d /path/to/colmap/project -o output/"
echo ""
log_info "Training on COLMAP project:"
echo "  ./lichtfeld -d ~/my_project -o ~/output --eval"
echo ""
log_info "Key Options:"
echo "  -d <path>     Input data directory (COLMAP project)"
echo "  -o <path>     Output directory"
echo "  --eval        Run evaluation after training"
echo "  --gui         Launch interactive viewer (requires display)"
echo ""
log_info "Features vs OpenSplat:"
echo "  ✓ Faster training (20 min for 60k steps @ 4K)"
echo "  ✓ Pose optimization (fixes camera calibration errors)"
echo "  ✓ MCMC densification (better Gaussian placement)"
echo "  ✓ Interactive viewer with editing"
echo "  ✓ No ML framework dependencies (custom CUDA)"
echo ""
log_info "Note: LichtFeld-Studio requires NVIDIA CUDA. For macOS, use:"
echo "  ./scripts/setup_opensplat.sh  (Metal backend)"
echo ""
