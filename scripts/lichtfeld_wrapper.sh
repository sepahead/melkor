#!/bin/bash
set -e

# ============================================================================
# LichtFeld-Studio Wrapper Script
# ============================================================================
# Provides:
#   - Custom image path support (--images flag)
#   - Easy interface to LichtFeld-Studio features
#   - Pose optimization options
#   - Output format handling
# ============================================================================

VERSION="1.0.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# ============================================================================
# Default Configuration
# ============================================================================
COLMAP_PROJECT=""
OUTPUT_DIR="output"
IMAGES_PATH=""
ITERATIONS=30000
GPU_ID="0"
POSE_OPT="none"         # none, direct, mlp
ENABLE_MCMC=true
ENABLE_EVAL=false
ENABLE_GUI=false
VERBOSE=false
DRY_RUN=false
LICHTFELD_BIN=""

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

# ============================================================================
# Help
# ============================================================================
print_usage() {
    cat << EOF
${CYAN}╔══════════════════════════════════════════════════════════════════════════════╗
║           LichtFeld-Studio Wrapper v${VERSION}                                    ║
║       High-Performance 3D Gaussian Splatting (C++23 + CUDA)                   ║
╚══════════════════════════════════════════════════════════════════════════════╝${NC}

Usage: $0 <colmap_project> [options]

${GREEN}Arguments:${NC}
  colmap_project      Path to COLMAP project directory (containing sparse/ folder)

${GREEN}Image Path Options:${NC}
  --images <path>     Override image directory path
                      (Use when images are in a different location)

${GREEN}Output Options:${NC}
  -o, --output <dir>      Output directory (default: output)
  -n, --iterations <n>    Number of training iterations (default: 30000)

${GREEN}GPU Options:${NC}
  --gpu <id>              GPU to use (default: 0)

${GREEN}Training Options:${NC}
  --pose-opt <mode>       Pose optimization mode:
                            none   - No pose optimization (default)
                            direct - Direct pose offset optimization
                            mlp    - MLP-based pose optimization (better but slower)
  --no-mcmc               Disable MCMC densification
  --eval                  Run evaluation after training
  --gui                   Launch interactive viewer (requires display)

${GREEN}Other Options:${NC}
  --lichtfeld <path>      Path to LichtFeld-Studio binary
  --verbose, -v           Verbose output
  --dry-run               Show what would be done without executing
  --help, -h              Show this help

${GREEN}Examples:${NC}
  # Basic training
  $0 ~/colmap_project -o output/

  # With custom image path
  $0 ~/colmap_project --images ~/original_images -o output/

  # With pose optimization (fixes camera errors)
  $0 ~/colmap_project --pose-opt mlp -o output/

  # Full example
  $0 ~/colmap_project \\
     --images ~/images \\
     --pose-opt direct \\
     --iterations 60000 \\
     --eval \\
     -o ~/output/

EOF
}

# ============================================================================
# Find LichtFeld-Studio Binary
# ============================================================================
find_lichtfeld() {
    # Check if already specified
    if [[ -n "$LICHTFELD_BIN" && -x "$LICHTFELD_BIN" ]]; then
        echo "$LICHTFELD_BIN"
        return 0
    fi
    
    # Check common locations
    local candidates=(
        "$PROJECT_DIR/lichtfeld"
        "$PROJECT_DIR/tools/LichtFeld-Studio/build/LichtFeld-Studio"
        "$(which lichtfeld 2>/dev/null || true)"
        "$(which LichtFeld-Studio 2>/dev/null || true)"
    )
    
    for candidate in "${candidates[@]}"; do
        if [[ -x "$candidate" ]]; then
            echo "$candidate"
            return 0
        fi
    done
    
    return 1
}

# ============================================================================
# Fix COLMAP Image Paths
# ============================================================================
fix_colmap_paths() {
    local colmap_dir="$1"
    local images_dir="$2"
    local workspace="$3"
    
    log_info "Setting up workspace with custom image paths..."
    
    # Create workspace structure
    mkdir -p "$workspace/sparse/0"
    mkdir -p "$workspace/images"
    
    # Copy COLMAP database files
    if [[ -d "$colmap_dir/sparse/0" ]]; then
        cp -a "$colmap_dir/sparse/0/"* "$workspace/sparse/0/" 2>/dev/null || true
    elif [[ -d "$colmap_dir/sparse" ]]; then
        cp -a "$colmap_dir/sparse/"* "$workspace/sparse/" 2>/dev/null || true
    fi
    
    # Copy database.db if exists
    if [[ -f "$colmap_dir/database.db" ]]; then
        cp "$colmap_dir/database.db" "$workspace/"
    fi
    
    # Link all images from source to workspace
    log_info "Linking images from: $images_dir"
    
    local link_count=0
    while IFS= read -r img; do
        local img_basename
        img_basename=$(basename "$img")
        if [[ ! -e "$workspace/images/$img_basename" ]]; then
            ln -sf "$img" "$workspace/images/$img_basename"
            ((link_count++))
        fi
    done < <(find "$images_dir" -maxdepth 1 -type f \( \
        -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o \
        -iname "*.JPG" -o -iname "*.JPEG" -o -iname "*.PNG" \
    \))
    
    log_success "Linked $link_count images to workspace"
    echo "$workspace"
}

# ============================================================================
# Main
# ============================================================================
main() {
    # Check platform
    if [[ "$(uname -s)" != "Linux" ]]; then
        log_error "LichtFeld-Studio requires Linux with NVIDIA CUDA."
        log_info "On macOS, use: ./scripts/opensplat_wrapper.sh"
        exit 1
    fi
    
    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            --images)
                IMAGES_PATH="$2"; shift 2 ;;
            -o|--output)
                OUTPUT_DIR="$2"; shift 2 ;;
            -n|--iterations)
                ITERATIONS="$2"; shift 2 ;;
            --gpu)
                GPU_ID="$2"; shift 2 ;;
            --pose-opt)
                POSE_OPT="$2"; shift 2 ;;
            --no-mcmc)
                ENABLE_MCMC=false; shift ;;
            --eval)
                ENABLE_EVAL=true; shift ;;
            --gui)
                ENABLE_GUI=true; shift ;;
            --lichtfeld)
                LICHTFELD_BIN="$2"; shift 2 ;;
            --verbose|-v)
                VERBOSE=true; shift ;;
            --dry-run)
                DRY_RUN=true; shift ;;
            --help|-h)
                print_usage; exit 0 ;;
            -*)
                log_error "Unknown option: $1"
                print_usage; exit 1 ;;
            *)
                if [[ -z "$COLMAP_PROJECT" ]]; then
                    COLMAP_PROJECT="$1"
                else
                    log_error "Unexpected argument: $1"
                    exit 1
                fi
                shift ;;
        esac
    done
    
    # Validate arguments
    if [[ -z "$COLMAP_PROJECT" ]]; then
        log_error "Missing COLMAP project path"
        print_usage
        exit 1
    fi
    
    if [[ ! -d "$COLMAP_PROJECT" ]]; then
        log_error "COLMAP project directory not found: $COLMAP_PROJECT"
        exit 1
    fi
    
    # Resolve paths
    COLMAP_PROJECT=$(cd "$COLMAP_PROJECT" && pwd)
    
    # Find LichtFeld-Studio binary
    LICHTFELD_BIN=$(find_lichtfeld) || true
    if [[ -z "$LICHTFELD_BIN" ]]; then
        log_error "LichtFeld-Studio binary not found."
        log_info "Run: ./scripts/setup_lichtfeld.sh"
        exit 1
    fi
    log_info "Using LichtFeld-Studio: $LICHTFELD_BIN"
    
    # Print configuration
    echo -e "${CYAN}╔══════════════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║           LichtFeld-Studio - Training Configuration                          ║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    log_info "COLMAP Project: $COLMAP_PROJECT"
    [[ -n "$IMAGES_PATH" ]] && log_info "Custom Images:  $IMAGES_PATH"
    log_info "Output:         $OUTPUT_DIR"
    log_info "Iterations:     $ITERATIONS"
    log_info "GPU:            $GPU_ID"
    log_info "Pose Opt:       $POSE_OPT"
    log_info "MCMC:           $ENABLE_MCMC"
    [[ "$ENABLE_EVAL" == true ]] && log_info "Evaluation:     enabled"
    [[ "$ENABLE_GUI" == true ]] && log_info "GUI:            enabled"
    echo ""
    
    # Setup workspace with custom image paths if needed
    local working_dir="$COLMAP_PROJECT"
    local temp_workspace=""
    
    if [[ -n "$IMAGES_PATH" ]]; then
        if [[ ! -d "$IMAGES_PATH" ]]; then
            log_error "Images directory not found: $IMAGES_PATH"
            exit 1
        fi
        IMAGES_PATH=$(cd "$IMAGES_PATH" && pwd)
        
        temp_workspace=$(mktemp -d)
        working_dir=$(fix_colmap_paths "$COLMAP_PROJECT" "$IMAGES_PATH" "$temp_workspace")
    fi
    
    # Create output directory
    mkdir -p "$OUTPUT_DIR"
    OUTPUT_DIR=$(cd "$OUTPUT_DIR" && pwd)
    
    # Set GPU
    export CUDA_VISIBLE_DEVICES="$GPU_ID"
    
    # Build command
    local -a cmd=("$LICHTFELD_BIN")
    cmd+=(-d "$working_dir")
    cmd+=(-o "$OUTPUT_DIR")
    
    # Add iterations (if LichtFeld-Studio supports it)
    # Note: Check actual CLI args of LichtFeld-Studio
    
    # Add pose optimization
    case $POSE_OPT in
        direct)
            cmd+=(--pose-opt-direct)
            ;;
        mlp)
            cmd+=(--pose-opt-mlp)
            ;;
    esac
    
    # Add evaluation
    [[ "$ENABLE_EVAL" == true ]] && cmd+=(--eval)
    
    # Add GUI
    [[ "$ENABLE_GUI" == true ]] && cmd+=(--gui)
    
    if [[ "$VERBOSE" == true ]]; then
        log_info "Command: ${cmd[*]}"
    fi
    
    if [[ "$DRY_RUN" == true ]]; then
        log_warn "Dry run - would execute: ${cmd[*]}"
        [[ -n "$temp_workspace" ]] && rm -rf "$temp_workspace"
        return 0
    fi
    
    # Run training
    log_info "Starting LichtFeld-Studio training..."
    "${cmd[@]}"
    local exit_code=$?
    
    # Cleanup temp workspace
    if [[ -n "$temp_workspace" && -d "$temp_workspace" ]]; then
        rm -rf "$temp_workspace"
    fi
    
    if [[ $exit_code -eq 0 ]]; then
        echo ""
        log_success "Training completed successfully!"
        log_info "Output: $OUTPUT_DIR"
        
        # List output files
        find "$OUTPUT_DIR" -maxdepth 2 \( -name "*.ply" -o -name "*.splat" \) -exec ls -lh {} \; 2>/dev/null || true
    else
        log_error "Training failed with exit code: $exit_code"
    fi
    
    exit $exit_code
}

main "$@"
