#!/bin/bash
set -e

# ============================================================================
# Melkor Unified 3D Gaussian Splatting Pipeline
# ============================================================================
# Complete pipeline: Images (+ optional metadata) → COLMAP → 3DGS PLY/SPZ
# Supports: CUDA (Linux), CPU (all platforms), Metal/MPS (macOS Apple Silicon)
# ============================================================================

VERSION="1.0.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# ============================================================================
# Default Configuration
# ============================================================================
BACKEND="auto"           # auto, cuda, cpu, metal
QUALITY="medium"         # fast, medium, high
OUTPUT_FORMAT="ply"      # ply, spz, both
ITERATIONS=""            # Override iterations (auto-set based on quality)
TOOL="auto"              # auto, opensplat, gsplat, lichtfeld
SFM_TOOL="colmap"        # colmap, glomap
SKIP_COLMAP=false
VERBOSE=false
DRY_RUN=false

# Multi-GPU and memory options
GPU_IDS=""               # Comma-separated GPU IDs (e.g., "0,1,2")
GPU_SPLIT="single"       # single, data-parallel, memory-split
DOWNSCALE=1              # Image downscale factor (1, 2, 4, 8)
CUSTOM_IMAGES=""         # Custom path to images (if different from COLMAP)

# ============================================================================
# Color Output
# ============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# All logging goes to stderr so functions whose stdout is captured via $( )
# (e.g. validate_input, train_gaussians) only emit their result on stdout.
log_info()  { echo -e "${BLUE}[INFO]${NC} $1" >&2; }
log_success() { echo -e "${GREEN}[✓]${NC} $1" >&2; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $1" >&2; }
log_error() { echo -e "${RED}[ERROR]${NC} $1" >&2; }
log_step()  { echo -e "\n${CYAN}=== $1 ===${NC}" >&2; }

# ============================================================================
# Help
# ============================================================================
print_usage() {
    cat << EOF
${CYAN}╔══════════════════════════════════════════════════════════════════════════════╗
║           Melkor 3D Gaussian Splatting Pipeline v${VERSION}                       ║
╚══════════════════════════════════════════════════════════════════════════════╝${NC}

Usage: $0 <input_images> <output_dir> [options]

${GREEN}Arguments:${NC}
  input_images    Path to folder containing images (JPG/PNG)
                  OR path to existing COLMAP project (with sparse/ folder)
  output_dir      Output directory for results

${GREEN}Backend Options:${NC}
  --backend <type>    Compute backend to use:
                        auto   - Auto-detect best available (default)
                        cuda   - NVIDIA GPU (Linux, requires CUDA)
                        metal  - Apple Silicon GPU (macOS M1/M2/M3)
                        cpu    - CPU only (slowest, all platforms)

${GREEN}Quality Options:${NC}
  --quality <level>   Training quality preset:
                        fast   - 7,000 iterations (~5-10 min)
                        medium - 15,000 iterations (~15-30 min, default)
                        high   - 30,000 iterations (~30-60 min)
  --iterations <n>    Override iteration count directly

${GREEN}Output Options:${NC}
  --format <type>     Output format:
                        ply    - Standard PLY format (default)
                        spz    - Compressed SPZ format (smaller)
                        both   - Generate both formats

${GREEN}Tool Selection:${NC}
  --tool <name>       Training tool to use:
                        auto        - Auto-select based on backend (default)
                        opensplat   - C++/LibTorch (recommended for production)
                        gsplat      - Python/PyTorch MPS (macOS only)
                        gsplat-cuda - Python/PyTorch CUDA (Linux multi-GPU)
                        lichtfeld   - C++23/CUDA (fastest single-GPU, Linux only)

${GREEN}Pipeline Options:${NC}
  --skip-colmap       Skip COLMAP/GLOMAP, use existing sparse reconstruction
  --sfm <tool>        SfM tool for reconstruction:
                        colmap  - COLMAP incremental SfM (default)
                        glomap  - GLOMAP global SfM (10-100× faster)
  --colmap-quality    COLMAP/GLOMAP feature extraction quality: low, medium, high

${GREEN}GPU & Memory Options:${NC}
  --gpu-ids <ids>     Comma-separated GPU IDs for multi-GPU training
                      (e.g., --gpu-ids 0,1,2,3)
  --gpu-split <mode>  Multi-GPU split mode: single, data-parallel, memory-split
  --downscale <n>     Downscale images by factor (1, 2, 4, 8) - reduces memory
  --images <path>     Custom path to images (if different from COLMAP project)

${GREEN}Other Options:${NC}
  --verbose, -v       Verbose output
  --dry-run           Show what would be done without executing
  --setup             Run setup for all dependencies
  --version           Show version
  --help, -h          Show this help

${GREEN}Examples:${NC}
  # Auto-detect everything (recommended)
  $0 ~/Photos/my_scene ~/output/my_scene

  # Use CUDA on Linux with high quality
  $0 ~/Photos/scene output/ --backend cuda --quality high

  # Use Metal on macOS with SPZ output
  $0 ~/Photos/scene output/ --backend metal --format spz

  # Fast preview on CPU
  $0 ~/Photos/scene output/ --backend cpu --quality fast

  # Use existing COLMAP project
  $0 ~/colmap_project output/ --skip-colmap --quality high

  # Use GLOMAP for faster reconstruction (10-100× faster)
  $0 ~/Photos/scene output/ --sfm glomap --quality high

  # Multi-GPU training with custom image path
  $0 ~/colmap_project output/ --gpu-ids 0,1,2,3 --images ~/original_images

  # Memory-constrained training (downscale images)
  $0 ~/Photos/scene output/ --downscale 2 --quality medium

${GREEN}Supported Input Formats:${NC}
  Images: JPG, JPEG, PNG, HEIC, HEIF, WEBP, TIFF, TIF, BMP, GIF
          (need 20+ images with 60-80% overlap)
          Note: HEIC/HEIF images are auto-converted to JPEG on macOS
  COLMAP: Project folder with sparse/0/ reconstruction

EOF
}

# ============================================================================
# Setup Command
# ============================================================================
run_setup() {
    log_step "Running Full Setup"
    
    # Detect platform
    local platform=$(uname -s)
    local arch=$(uname -m)
    
    log_info "Platform: $platform ($arch)"
    
    # Install COLMAP
    log_step "Setting up COLMAP"
    if ! command -v colmap &> /dev/null; then
        if [[ "$platform" == "Darwin" ]]; then
            log_info "Installing COLMAP via Homebrew..."
            brew install colmap
        elif [[ "$platform" == "Linux" ]]; then
            log_info "Installing COLMAP via apt..."
            sudo apt-get update
            sudo apt-get install -y colmap
        else
            log_error "Please install COLMAP manually: https://colmap.github.io/install.html"
            exit 1
        fi
    else
        log_success "COLMAP already installed"
    fi
    
    # Setup based on platform
    if [[ "$platform" == "Darwin" && "$arch" == "arm64" ]]; then
        log_step "Setting up for Apple Silicon (Metal/MPS)"
        
        # Setup OpenSplat with Metal
        if [[ -f "$SCRIPT_DIR/setup_opensplat.sh" ]]; then
            chmod +x "$SCRIPT_DIR/setup_opensplat.sh"
            "$SCRIPT_DIR/setup_opensplat.sh"
        fi
        
        # Setup gsplat-mps (Metal backend)
        if [[ -f "$SCRIPT_DIR/setup_gsplat_mps.sh" ]]; then
            chmod +x "$SCRIPT_DIR/setup_gsplat_mps.sh"
            "$SCRIPT_DIR/setup_gsplat_mps.sh"
        fi
        
    elif [[ "$platform" == "Darwin" && "$arch" == "x86_64" ]]; then
        log_step "Setting up for macOS Intel (CPU-only)"
        log_warn "Metal MPS acceleration is not available on Intel Macs"
        log_info "Using CPU-only OpenSplat"
        setup_cpu
        
    elif [[ "$platform" == "Linux" ]]; then
        # Check for CUDA
        if command -v nvidia-smi &> /dev/null; then
            log_step "Setting up for Linux (CUDA)"
            setup_cuda
        else
            log_step "Setting up for Linux (CPU)"
            setup_cpu
        fi
    else
        log_step "Setting up for CPU-only"
        setup_cpu
    fi
    
    # Build melkor
    log_step "Building Melkor"
    cd "$PROJECT_DIR"
    mkdir -p build && cd build
    cmake .. && make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
    
    log_success "Setup complete!"
}

setup_cuda() {
    log_info "Setting up CUDA-based Gaussian Splatting..."
    
    # Check CUDA version
    if command -v nvcc &> /dev/null; then
        local cuda_version=$(nvcc --version | grep release | awk '{print $6}' | cut -d',' -f1)
        log_info "Found CUDA: $cuda_version"
    else
        log_warn "nvcc not found. Please install CUDA toolkit."
    fi
    
    # Clone and build OpenSplat with CUDA
    local OPENSPLAT_DIR="$PROJECT_DIR/tools/OpenSplat"
    if [[ ! -d "$OPENSPLAT_DIR" ]]; then
        git clone https://github.com/pierotofy/OpenSplat.git "$OPENSPLAT_DIR"
    fi
    
    cd "$OPENSPLAT_DIR"
    mkdir -p build && cd build
    
    # Download LibTorch for Linux CUDA
    local LIBTORCH_DIR="$PROJECT_DIR/tools/libtorch"
    if [[ ! -d "$LIBTORCH_DIR" ]]; then
        log_info "Downloading LibTorch for CUDA..."
        cd "$PROJECT_DIR/tools"
        curl -L "https://download.pytorch.org/libtorch/cu121/libtorch-cxx11-abi-shared-with-deps-2.2.0%2Bcu121.zip" -o libtorch.zip
        unzip -q libtorch.zip
        rm libtorch.zip
    fi
    
    cd "$OPENSPLAT_DIR/build"
    cmake -DCMAKE_PREFIX_PATH="$LIBTORCH_DIR" \
          -DGPU_RUNTIME=CUDA \
          -DCMAKE_BUILD_TYPE=Release \
          ..
    make -j$(nproc)
    
    # Create wrapper
    cat > "$PROJECT_DIR/opensplat" << 'WRAPPER'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/tools/OpenSplat/build/opensplat" "$@"
WRAPPER
    chmod +x "$PROJECT_DIR/opensplat"
    
    log_success "CUDA setup complete"
}

setup_cpu() {
    log_info "Setting up CPU-based Gaussian Splatting..."
    
    local OPENSPLAT_DIR="$PROJECT_DIR/tools/OpenSplat"
    if [[ ! -d "$OPENSPLAT_DIR" ]]; then
        git clone https://github.com/pierotofy/OpenSplat.git "$OPENSPLAT_DIR"
    fi
    
    # Download CPU LibTorch
    local LIBTORCH_DIR="$PROJECT_DIR/tools/libtorch"
    if [[ ! -d "$LIBTORCH_DIR" ]]; then
        log_info "Downloading LibTorch (CPU)..."
        cd "$PROJECT_DIR/tools"
        
        local platform=$(uname -s)
        if [[ "$platform" == "Darwin" ]]; then
            curl -L "https://download.pytorch.org/libtorch/cpu/libtorch-macos-arm64-2.2.0.zip" -o libtorch.zip
        else
            curl -L "https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-2.2.0%2Bcpu.zip" -o libtorch.zip
        fi
        unzip -q libtorch.zip
        rm libtorch.zip
    fi
    
    cd "$OPENSPLAT_DIR"
    mkdir -p build && cd build
    cmake -DCMAKE_PREFIX_PATH="$LIBTORCH_DIR" \
          -DGPU_RUNTIME=NONE \
          -DCMAKE_BUILD_TYPE=Release \
          ..
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
    
    cat > "$PROJECT_DIR/opensplat" << 'WRAPPER'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/tools/OpenSplat/build/opensplat" "$@"
WRAPPER
    chmod +x "$PROJECT_DIR/opensplat"
    
    log_success "CPU setup complete"
}

# ============================================================================
# Detect Backend
# ============================================================================
detect_backend() {
    local platform=$(uname -s)
    local arch=$(uname -m)
    
    if [[ "$platform" == "Darwin" && "$arch" == "arm64" ]]; then
        # Apple Silicon - use Metal/MPS
        echo "metal"
    elif [[ "$platform" == "Darwin" && "$arch" == "x86_64" ]]; then
        # Intel Mac - CPU only (Metal MPS not available)
        echo "cpu"
    elif [[ "$platform" == "Linux" ]] && command -v nvidia-smi &> /dev/null; then
        # Linux with NVIDIA GPU - use CUDA
        echo "cuda"
    else
        # Fallback to CPU
        echo "cpu"
    fi
}

# ============================================================================
# Detect Tool
# ============================================================================
detect_tool() {
    local backend=$1
    
    # On Linux with CUDA and multiple GPUs, prefer gsplat-cuda for native DDP
    if [[ "$backend" == "cuda" ]] && [[ -n "$GPU_IDS" ]]; then
        # Multi-GPU requested - prefer gsplat-cuda for native DDP
        if [[ -d "$PROJECT_DIR/tools/gsplat-cuda" ]]; then
            echo "gsplat-cuda"
            return
        fi
    fi
    
    # On Linux with CUDA single GPU, prefer LichtFeld-Studio (fastest)
    if [[ "$backend" == "cuda" ]] && [[ -x "$PROJECT_DIR/lichtfeld" ]]; then
        echo "lichtfeld"
    # Otherwise prefer OpenSplat as it's generally faster and cross-platform
    elif [[ -x "$PROJECT_DIR/opensplat" ]]; then
        echo "opensplat"
    elif [[ -d "$PROJECT_DIR/tools/gsplat-cuda" ]]; then
        echo "gsplat-cuda"
    elif [[ -d "$PROJECT_DIR/tools/gsplat-mps" ]]; then
        echo "gsplat"
    elif [[ -x "$PROJECT_DIR/lichtfeld" ]]; then
        echo "lichtfeld"
    else
        log_error "No training tool found. Run: $0 --setup"
        exit 1
    fi
}

# ============================================================================
# Get Iterations Based on Quality
# ============================================================================
get_iterations() {
    local quality=$1
    case $quality in
        fast)   echo 7000 ;;
        medium) echo 15000 ;;
        high)   echo 30000 ;;
        *)      echo 15000 ;;
    esac
}

# ============================================================================
# Validate Input
# ============================================================================
validate_input() {
    local input_path=$1
    
    if [[ ! -e "$input_path" ]]; then
        log_error "Input path does not exist: $input_path"
        exit 1
    fi
    
    # Check if it's a COLMAP project
    if [[ -d "$input_path/sparse" || -d "$input_path/sparse/0" ]]; then
        echo "colmap"
        return
    fi
    
    # Check if it's an image directory (support many formats)
    local image_count=$(find "$input_path" -type f \( \
        -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o \
        -iname "*.heic" -o -iname "*.heif" -o -iname "*.webp" -o \
        -iname "*.tiff" -o -iname "*.tif" -o -iname "*.bmp" -o -iname "*.gif" \
    \) 2>/dev/null | wc -l | tr -d ' ')
    if [[ $image_count -gt 0 ]]; then
        if [[ $image_count -lt 3 ]]; then
            log_error "Need at least 3 images for reconstruction. Found: $image_count"
            exit 1
        fi
        if [[ $image_count -lt 20 ]]; then
            log_warn "Only $image_count images found. Recommend 20+ images for good results."
        fi
        echo "images"
        return
    fi
    
    log_error "Invalid input: No images or COLMAP project found at $input_path"
    exit 1
}

# ============================================================================
# Run COLMAP
# ============================================================================
run_colmap() {
    local input_dir=$1
    local workspace_dir=$2
    local colmap_quality=${3:-medium}
    
    log_step "Running COLMAP Structure-from-Motion"
    
    if ! command -v colmap &> /dev/null; then
        log_error "COLMAP not installed. Run: $0 --setup"
        exit 1
    fi
    
    # Prepare workspace
    mkdir -p "$workspace_dir/images"
    
    # Copy and convert images
    log_info "Copying images to workspace..."
    
    # Copy standard formats
    cp "$input_dir"/*.{jpg,jpeg,png,JPG,JPEG,PNG} "$workspace_dir/images/" 2>/dev/null || true
    cp "$input_dir"/*.{tiff,tif,TIFF,TIF} "$workspace_dir/images/" 2>/dev/null || true
    cp "$input_dir"/*.{bmp,BMP,gif,GIF} "$workspace_dir/images/" 2>/dev/null || true
    cp "$input_dir"/*.{webp,WEBP} "$workspace_dir/images/" 2>/dev/null || true
    
    # Handle HEIC/HEIF (Apple format) - convert to JPEG
    local heic_count=$(find "$input_dir" -type f \( -iname "*.heic" -o -iname "*.heif" \) 2>/dev/null | wc -l | tr -d ' ')
    if [[ $heic_count -gt 0 ]]; then
        log_info "Found $heic_count HEIC/HEIF images, converting to JPEG..."
        
        # Check for sips (macOS) or ImageMagick
        if command -v sips &> /dev/null; then
            # macOS - use sips for HEIC conversion
            find "$input_dir" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) | while read -r heic_file; do
                local fname=$(basename "$heic_file")
                local output_name="${fname%.*}.jpg"
                sips -s format jpeg "$heic_file" --out "$workspace_dir/images/$output_name" 2>/dev/null || true
            done
        elif command -v convert &> /dev/null; then
            # ImageMagick
            find "$input_dir" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) | while read -r heic_file; do
                local fname=$(basename "$heic_file")
                local output_name="${fname%.*}.jpg"
                convert "$heic_file" "$workspace_dir/images/$output_name" 2>/dev/null || true
            done
        elif command -v heif-convert &> /dev/null; then
            # libheif
            find "$input_dir" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) | while read -r heic_file; do
                local fname=$(basename "$heic_file")
                local output_name="${fname%.*}.jpg"
                heif-convert "$heic_file" "$workspace_dir/images/$output_name" 2>/dev/null || true
            done
        else
            log_warn "HEIC conversion not available. Install ImageMagick: brew install imagemagick"
            log_warn "Skipping $heic_count HEIC/HEIF images"
        fi
    fi
    
    # Convert WebP to JPEG if needed (COLMAP may not support WebP natively)
    local webp_count=$(find "$workspace_dir/images" -type f -iname "*.webp" 2>/dev/null | wc -l | tr -d ' ')
    if [[ $webp_count -gt 0 ]] && command -v convert &> /dev/null; then
        log_info "Converting $webp_count WebP images to JPEG..."
        find "$workspace_dir/images" -maxdepth 1 -type f -iname "*.webp" | while read -r webp_file; do
            local fname=$(basename "$webp_file")
            local output_name="${fname%.*}.jpg"
            convert "$webp_file" "$workspace_dir/images/$output_name" 2>/dev/null && rm "$webp_file" || true
        done
    fi
    
    local image_count=$(find "$workspace_dir/images" -type f \( -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o -iname "*.tiff" -o -iname "*.tif" \) | wc -l | tr -d ' ')
    log_info "Processing $image_count images..."
    
    # Map quality to COLMAP setting
    local colmap_preset="medium"
    case $colmap_quality in
        fast|low)   colmap_preset="low" ;;
        medium)     colmap_preset="medium" ;;
        high)       colmap_preset="high" ;;
    esac
    
    # Detect GPU for COLMAP CUDA acceleration
    local use_gpu=0
    if [[ "$(uname -s)" == "Linux" ]] && command -v nvidia-smi &> /dev/null; then
        log_info "NVIDIA GPU detected - enabling CUDA acceleration for COLMAP"
        use_gpu=1
    fi
    
    # Run COLMAP automatic reconstructor
    # Note: COLMAP CLI works headlessly without X display
    log_info "This may take several minutes depending on image count..."

    local -a verbose_args=()
    [[ "$VERBOSE" == true ]] && verbose_args+=(--verbose 1)

    colmap automatic_reconstructor \
        --workspace_path "$workspace_dir" \
        --image_path "$workspace_dir/images" \
        --quality "$colmap_preset" \
        --single_camera 1 \
        --SiftExtraction.use_gpu "$use_gpu" \
        --SiftMatching.use_gpu "$use_gpu" \
        "${verbose_args[@]}"
    
    # Verify reconstruction
    if [[ ! -f "$workspace_dir/sparse/0/cameras.bin" ]]; then
        log_error "COLMAP reconstruction failed"
        log_info "Tips:"
        log_info "  - Ensure images have 60-80% overlap"
        log_info "  - Avoid blurry or motion-blurred images"
        log_info "  - Try with more images (30+ recommended)"
        exit 1
    fi
    
    log_success "COLMAP reconstruction complete"
}

# ============================================================================
# Run GLOMAP (Global SfM - 10-100× faster than COLMAP)
# ============================================================================
run_glomap() {
    local input_dir=$1
    local workspace_dir=$2
    local colmap_quality=${3:-medium}
    
    log_step "Running GLOMAP Global Structure-from-Motion"
    
    # Check for GLOMAP
    local glomap_bin=""
    local glomap_candidates=(
        "$PROJECT_DIR/glomap"
        "$PROJECT_DIR/tools/glomap/build/glomap/glomap"
        "$(which glomap 2>/dev/null || true)"
    )
    for candidate in "${glomap_candidates[@]}"; do
        if [[ -x "$candidate" ]]; then
            glomap_bin="$candidate"
            break
        fi
    done
    
    if [[ -z "$glomap_bin" ]]; then
        log_error "GLOMAP not found. Run: ./scripts/setup_glomap.sh"
        log_info "Falling back to COLMAP..."
        run_colmap "$input_dir" "$workspace_dir" "$colmap_quality"
        return
    fi
    
    if ! command -v colmap &> /dev/null; then
        log_error "COLMAP not installed (required for feature extraction). Run: $0 --setup"
        exit 1
    fi
    
    # Prepare workspace
    mkdir -p "$workspace_dir/images"
    
    # Copy and convert images (same as run_colmap)
    log_info "Copying images to workspace..."
    
    # Copy standard formats
    cp "$input_dir"/*.{jpg,jpeg,png,JPG,JPEG,PNG} "$workspace_dir/images/" 2>/dev/null || true
    cp "$input_dir"/*.{tiff,tif,TIFF,TIF} "$workspace_dir/images/" 2>/dev/null || true
    cp "$input_dir"/*.{bmp,BMP,gif,GIF} "$workspace_dir/images/" 2>/dev/null || true
    cp "$input_dir"/*.{webp,WEBP} "$workspace_dir/images/" 2>/dev/null || true
    
    # Handle HEIC/HEIF (Apple format) - convert to JPEG
    local heic_count=$(find "$input_dir" -type f \( -iname "*.heic" -o -iname "*.heif" \) 2>/dev/null | wc -l | tr -d ' ')
    if [[ $heic_count -gt 0 ]]; then
        log_info "Found $heic_count HEIC/HEIF images, converting to JPEG..."
        
        if command -v sips &> /dev/null; then
            find "$input_dir" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) | while read -r heic_file; do
                local fname=$(basename "$heic_file")
                local output_name="${fname%.*}.jpg"
                sips -s format jpeg "$heic_file" --out "$workspace_dir/images/$output_name" 2>/dev/null || true
            done
        elif command -v convert &> /dev/null; then
            find "$input_dir" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) | while read -r heic_file; do
                local fname=$(basename "$heic_file")
                local output_name="${fname%.*}.jpg"
                convert "$heic_file" "$workspace_dir/images/$output_name" 2>/dev/null || true
            done
        else
            log_warn "HEIC conversion not available. Install ImageMagick."
        fi
    fi
    
    local image_count=$(find "$workspace_dir/images" -type f \( -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" \) | wc -l | tr -d ' ')
    log_info "Processing $image_count images..."
    
    # Detect GPU for COLMAP CUDA acceleration
    local use_gpu=0
    if [[ "$(uname -s)" == "Linux" ]] && command -v nvidia-smi &> /dev/null; then
        log_info "NVIDIA GPU detected - enabling CUDA acceleration for COLMAP"
        use_gpu=1
    fi
    
    local -a verbose_args=()
    [[ "$VERBOSE" == true ]] && verbose_args+=(--verbose 1)

    # Step 1: Feature extraction (COLMAP)
    log_info "Step 1/3: Feature extraction (COLMAP)..."
    colmap feature_extractor \
        --image_path "$workspace_dir/images" \
        --database_path "$workspace_dir/database.db" \
        --SiftExtraction.use_gpu "$use_gpu" \
        "${verbose_args[@]}"
    
    # Step 2: Feature matching (COLMAP)
    log_info "Step 2/3: Feature matching (COLMAP)..."
    
    # Choose matcher based on image count
    if [[ $image_count -gt 500 ]]; then
        log_info "Large dataset detected, using sequential matcher..."
        colmap sequential_matcher \
            --database_path "$workspace_dir/database.db" \
            --SiftMatching.use_gpu "$use_gpu" \
            "${verbose_args[@]}"
    else
        colmap exhaustive_matcher \
            --database_path "$workspace_dir/database.db" \
            --SiftMatching.use_gpu "$use_gpu" \
            "${verbose_args[@]}"
    fi
    
    # Step 3: GLOMAP mapping (the fast part!)
    log_info "Step 3/3: Global SfM mapping (GLOMAP - this is the fast part!)..."
    mkdir -p "$workspace_dir/sparse"
    
    "$glomap_bin" mapper \
        --database_path "$workspace_dir/database.db" \
        --image_path "$workspace_dir/images" \
        --output_path "$workspace_dir/sparse"
    
    # Verify reconstruction - handle different output locations
    if [[ ! -f "$workspace_dir/sparse/0/cameras.bin" ]]; then
        # Check if files are directly in sparse/
        if [[ -f "$workspace_dir/sparse/cameras.bin" ]]; then
            mkdir -p "$workspace_dir/sparse/0"
            mv "$workspace_dir/sparse/cameras.bin" "$workspace_dir/sparse/0/"
            mv "$workspace_dir/sparse/images.bin" "$workspace_dir/sparse/0/"
            mv "$workspace_dir/sparse/points3D.bin" "$workspace_dir/sparse/0/"
        else
            log_error "GLOMAP reconstruction failed"
            log_info "Tips:"
            log_info "  - Ensure images have 60-80% overlap"
            log_info "  - Avoid blurry or motion-blurred images"
            log_info "  - Try with more images (30+ recommended)"
            log_info "  - Try falling back to COLMAP: --sfm colmap"
            exit 1
        fi
    fi
    
    log_success "GLOMAP reconstruction complete (much faster than COLMAP!)"
}

# ============================================================================
# Train Gaussians
# ============================================================================
train_gaussians() {
    local workspace_dir=$1
    local output_dir=$2
    local tool=$3
    local backend=$4
    local iterations=$5
    
    log_step "Training 3D Gaussian Splats"
    log_info "Tool: $tool | Backend: $backend | Iterations: $iterations"
    
    local output_ply="$output_dir/point_cloud.ply"
    
    case $tool in
        opensplat)
            # Check if we should use the wrapper script (for custom images, multi-GPU, etc.)
            local use_wrapper=false
            [[ -n "$GPU_IDS" ]] && use_wrapper=true
            [[ -n "$CUSTOM_IMAGES" ]] && use_wrapper=true
            [[ "$DOWNSCALE" != "1" ]] && use_wrapper=true
            
            local wrapper_script="$SCRIPT_DIR/opensplat_wrapper.sh"
            
            if [[ "$use_wrapper" == true ]]; then
                if [[ ! -x "$wrapper_script" ]]; then
                    log_error "Wrapper script not found or not executable: $wrapper_script"
                    log_info "Falling back to standard opensplat..."
                    use_wrapper=false
                fi
            fi
            
            if [[ "$use_wrapper" == true ]]; then
                log_info "Using OpenSplat wrapper for advanced features..."
                
                local -a wrapper_args=("$workspace_dir" -n "$iterations" -o "$output_ply")
                [[ -n "$GPU_IDS" ]] && wrapper_args+=(--gpu-ids "$GPU_IDS" --split "$GPU_SPLIT")
                [[ -n "$CUSTOM_IMAGES" ]] && wrapper_args+=(--images "$CUSTOM_IMAGES")
                [[ "$DOWNSCALE" != "1" ]] && wrapper_args+=(--downscale "$DOWNSCALE")
                [[ "$VERBOSE" == true ]] && wrapper_args+=(--verbose)
                # Pass the opensplat binary path if we know where it is
                [[ -x "$PROJECT_DIR/opensplat" ]] && wrapper_args+=(--opensplat "$PROJECT_DIR/opensplat")
                
                "$wrapper_script" "${wrapper_args[@]}" >&2
            else
                if [[ ! -x "$PROJECT_DIR/opensplat" ]]; then
                    log_error "OpenSplat not found. Run: $0 --setup"
                    exit 1
                fi
                
                log_info "Starting OpenSplat training..."
                local -a opensplat_args=("$workspace_dir" -n "$iterations" -o "$output_ply")
                [[ "$VERBOSE" == true ]] && opensplat_args+=(--verbose)
                "$PROJECT_DIR/opensplat" "${opensplat_args[@]}" >&2
            fi
            ;;
            
        gsplat)
            local gsplat_dir="$PROJECT_DIR/tools/gsplat-mps"
            if [[ ! -d "$gsplat_dir" ]]; then
                log_error "gsplat-mps not found. Run: $0 --setup"
                exit 1
            fi
            
            log_info "Starting gsplat training..."
            source "$gsplat_dir/venv/bin/activate"
            cd "$gsplat_dir"
            
            python examples/simple_trainer.py \
                --data_dir "$workspace_dir" \
                --result_dir "$output_dir" \
                --max_steps "$iterations" >&2

            deactivate
            
            # gsplat outputs to different location
            if [[ -f "$output_dir/splat.ply" ]]; then
                output_ply="$output_dir/splat.ply"
            fi
            ;;
            
        gsplat-cuda)
            local gsplat_dir="$PROJECT_DIR/tools/gsplat-cuda"
            if [[ ! -d "$gsplat_dir" ]]; then
                log_error "gsplat-cuda not found. Run: ./scripts/setup_gsplat_cuda.sh"
                exit 1
            fi
            
            log_info "Starting gsplat CUDA training..."
            
            # Check for multi-GPU
            if [[ -n "$GPU_IDS" ]]; then
                log_info "Using multi-GPU distributed training with GPUs: $GPU_IDS"
                
                # Check if distributed wrapper exists
                local dist_wrapper="$PROJECT_DIR/gsplat-cuda-train-distributed"
                if [[ ! -x "$dist_wrapper" ]]; then
                    log_error "Distributed training wrapper not found: $dist_wrapper"
                    log_info "Re-run: ./scripts/setup_gsplat_cuda.sh"
                    exit 1
                fi
                
                # Use the distributed training wrapper
                "$dist_wrapper" \
                    --gpus "$GPU_IDS" \
                    -- default \
                    --data_dir "$workspace_dir" \
                    --result_dir "$output_dir" \
                    --max_steps "$iterations" >&2
            else
                # Single GPU - save current directory
                local orig_dir="$PWD"
                
                # Check venv exists
                if [[ ! -f "$gsplat_dir/venv/bin/activate" ]]; then
                    log_error "gsplat-cuda virtual environment not found: $gsplat_dir/venv"
                    log_info "Re-run: ./scripts/setup_gsplat_cuda.sh"
                    exit 1
                fi
                
                source "$gsplat_dir/venv/bin/activate" || {
                    log_error "Failed to activate gsplat-cuda virtual environment"
                    exit 1
                }
                cd "$gsplat_dir"
                
                python examples/simple_trainer.py default \
                    --data_dir "$workspace_dir" \
                    --result_dir "$output_dir" \
                    --max_steps "$iterations" >&2
                
                deactivate 2>/dev/null || true
                cd "$orig_dir"
            fi
            
            # Find output PLY
            local found_ply
            found_ply=$(find "$output_dir" -name "*.ply" -type f 2>/dev/null | head -1)
            if [[ -n "$found_ply" ]]; then
                output_ply="$found_ply"
            fi
            ;;
            
        lichtfeld)
            # LichtFeld-Studio - Linux CUDA only
            if [[ "$(uname -s)" != "Linux" ]]; then
                log_error "LichtFeld-Studio requires Linux with NVIDIA CUDA."
                log_info "Use --tool opensplat on macOS."
                exit 1
            fi
            
            local wrapper_script="$SCRIPT_DIR/lichtfeld_wrapper.sh"
            
            if [[ -x "$wrapper_script" ]]; then
                log_info "Using LichtFeld-Studio wrapper..."
                
                local -a wrapper_args=("$workspace_dir" -o "$output_dir")
                [[ -n "$CUSTOM_IMAGES" ]] && wrapper_args+=(--images "$CUSTOM_IMAGES")
                [[ -n "$GPU_IDS" ]] && wrapper_args+=(--gpu "${GPU_IDS%%,*}")
                [[ "$VERBOSE" == true ]] && wrapper_args+=(--verbose)
                
                "$wrapper_script" "${wrapper_args[@]}" >&2
            else
                if [[ ! -x "$PROJECT_DIR/lichtfeld" ]]; then
                    log_error "LichtFeld-Studio not found. Run: ./scripts/setup_lichtfeld.sh"
                    exit 1
                fi
                
                log_info "Starting LichtFeld-Studio training..."
                [[ -n "$GPU_IDS" ]] && export CUDA_VISIBLE_DEVICES="${GPU_IDS%%,*}"
                
                "$PROJECT_DIR/lichtfeld" -d "$workspace_dir" -o "$output_dir" >&2
            fi
            
            # Find output PLY
            local found_ply
            found_ply=$(find "$output_dir" -name "*.ply" -type f 2>/dev/null | head -1)
            if [[ -n "$found_ply" ]]; then
                output_ply="$found_ply"
            fi
            ;;
            
        *)
            log_error "Unknown tool: $tool"
            exit 1
            ;;
    esac
    
    # Verify output
    if [[ ! -f "$output_ply" ]]; then
        # Try to find the output
        output_ply=$(find "$output_dir" -name "*.ply" -type f | head -1)
    fi
    
    if [[ -z "$output_ply" || ! -f "$output_ply" ]]; then
        log_error "Training failed - no PLY output found"
        exit 1
    fi
    
    log_success "Training complete: $output_ply"
    echo "$output_ply"
}

# ============================================================================
# Convert to SPZ
# ============================================================================
convert_to_spz() {
    local input_ply=$1
    local output_spz=$2
    
    log_step "Converting to SPZ format"
    
    if [[ -x "$PROJECT_DIR/build/melkor" ]]; then
        "$PROJECT_DIR/build/melkor" "$input_ply" "$output_spz"
        log_success "Created: $output_spz"
    else
        log_warn "Melkor not built. Skipping SPZ conversion."
        log_info "Build with: cd $PROJECT_DIR && mkdir -p build && cd build && cmake .. && make"
    fi
}

# ============================================================================
# Main Pipeline
# ============================================================================
main() {
    local INPUT_PATH=""
    local OUTPUT_DIR=""
    local COLMAP_QUALITY="medium"
    
    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            --backend)
                BACKEND="$2"; shift 2 ;;
            --quality)
                QUALITY="$2"; shift 2 ;;
            --iterations)
                ITERATIONS="$2"; shift 2 ;;
            --format)
                OUTPUT_FORMAT="$2"; shift 2 ;;
            --tool)
                TOOL="$2"; shift 2 ;;
            --skip-colmap)
                SKIP_COLMAP=true; shift ;;
            --sfm)
                SFM_TOOL="$2"; shift 2 ;;
            --colmap-quality)
                COLMAP_QUALITY="$2"; shift 2 ;;
            --gpu-ids)
                GPU_IDS="$2"; shift 2 ;;
            --gpu-split)
                GPU_SPLIT="$2"; shift 2 ;;
            --downscale)
                DOWNSCALE="$2"; shift 2 ;;
            --images)
                CUSTOM_IMAGES="$2"; shift 2 ;;
            --verbose|-v)
                VERBOSE=true; shift ;;
            --dry-run)
                DRY_RUN=true; shift ;;
            --setup)
                run_setup; exit 0 ;;
            --version)
                echo "Melkor Pipeline v$VERSION"; exit 0 ;;
            --help|-h)
                print_usage; exit 0 ;;
            -*)
                log_error "Unknown option: $1"
                print_usage; exit 1 ;;
            *)
                if [[ -z "$INPUT_PATH" ]]; then
                    INPUT_PATH="$1"
                elif [[ -z "$OUTPUT_DIR" ]]; then
                    OUTPUT_DIR="$1"
                else
                    log_error "Unexpected argument: $1"
                    exit 1
                fi
                shift ;;
        esac
    done
    
    # Validate arguments
    if [[ -z "$INPUT_PATH" || -z "$OUTPUT_DIR" ]]; then
        log_error "Missing required arguments"
        print_usage
        exit 1
    fi
    
    # Resolve paths
    INPUT_PATH=$(cd "$INPUT_PATH" 2>/dev/null && pwd || echo "$INPUT_PATH")
    mkdir -p "$OUTPUT_DIR"
    OUTPUT_DIR=$(cd "$OUTPUT_DIR" && pwd)
    
    # Auto-detect backend
    if [[ "$BACKEND" == "auto" ]]; then
        BACKEND=$(detect_backend)
    fi
    
    # Auto-detect tool
    if [[ "$TOOL" == "auto" ]]; then
        TOOL=$(detect_tool "$BACKEND")
    fi
    
    # Set iterations
    if [[ -z "$ITERATIONS" ]]; then
        ITERATIONS=$(get_iterations "$QUALITY")
    fi
    
    # Print configuration
    echo -e "${CYAN}╔══════════════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║           Melkor 3D Gaussian Splatting Pipeline                              ║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    log_info "Input:      $INPUT_PATH"
    log_info "Output:     $OUTPUT_DIR"
    log_info "Backend:    $BACKEND"
    log_info "SfM Tool:   $SFM_TOOL"
    log_info "Tool:       $TOOL"
    log_info "Quality:    $QUALITY ($ITERATIONS iterations)"
    log_info "Format:     $OUTPUT_FORMAT"
    [[ -n "$GPU_IDS" ]] && log_info "GPU IDs:    $GPU_IDS ($GPU_SPLIT)"
    [[ "$DOWNSCALE" != "1" ]] && log_info "Downscale:  ${DOWNSCALE}x"
    [[ -n "$CUSTOM_IMAGES" ]] && log_info "Images:     $CUSTOM_IMAGES"
    echo ""
    
    if [[ "$DRY_RUN" == true ]]; then
        log_warn "Dry run - no changes will be made"
        exit 0
    fi
    
    # Validate input (declare separately so the exit status of the command
    # substitution is not masked by 'local')
    local input_type
    input_type=$(validate_input "$INPUT_PATH")
    log_info "Input type: $input_type"
    
    # Workspace directory
    local WORKSPACE_DIR="$OUTPUT_DIR/workspace"
    mkdir -p "$WORKSPACE_DIR"
    
    # Step 1: SfM Reconstruction (COLMAP or GLOMAP)
    if [[ "$input_type" == "images" && "$SKIP_COLMAP" == false ]]; then
        if [[ "$SFM_TOOL" == "glomap" ]]; then
            run_glomap "$INPUT_PATH" "$WORKSPACE_DIR" "$COLMAP_QUALITY"
        else
            run_colmap "$INPUT_PATH" "$WORKSPACE_DIR" "$COLMAP_QUALITY"
        fi
    elif [[ "$input_type" == "colmap" ]]; then
        # Copy/link existing COLMAP project
        log_info "Using existing COLMAP project"
        if [[ "$INPUT_PATH" != "$WORKSPACE_DIR" ]]; then
            cp -r "$INPUT_PATH"/* "$WORKSPACE_DIR/" 2>/dev/null || true
        fi
    elif [[ "$SKIP_COLMAP" == true ]]; then
        log_info "Skipping COLMAP (--skip-colmap)"
        cp -r "$INPUT_PATH"/* "$WORKSPACE_DIR/" 2>/dev/null || true
    fi
    
    # Step 2: Train Gaussians (declare separately so a training failure is not
    # masked by 'local'; the function's stdout is exactly the output PLY path)
    local output_ply
    output_ply=$(train_gaussians "$WORKSPACE_DIR" "$OUTPUT_DIR" "$TOOL" "$BACKEND" "$ITERATIONS")
    
    # Step 3: Convert to SPZ if requested
    if [[ "$OUTPUT_FORMAT" == "spz" || "$OUTPUT_FORMAT" == "both" ]]; then
        local output_spz="${output_ply%.ply}.spz"
        convert_to_spz "$output_ply" "$output_spz"
    fi
    
    # Final summary
    echo ""
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║                         PIPELINE COMPLETE! ✓                                 ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    
    # List output files
    log_info "Output files:"
    find "$OUTPUT_DIR" -maxdepth 2 \( -name "*.ply" -o -name "*.spz" \) -exec ls -lh {} \;
    
    echo ""
    log_info "View your Gaussian Splat:"
    echo "  • Web: https://playcanvas.com/supersplat (drag & drop PLY)"
    echo "  • macOS: MetalSplatter (App Store)"
    echo ""
}

# Run main
main "$@"
