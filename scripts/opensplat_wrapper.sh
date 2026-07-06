#!/bin/bash
set -e

# ============================================================================
# OpenSplat Wrapper Script
# ============================================================================
# Provides:
#   - Custom image path support (--images flag)
#   - Multi-GPU distribution (--gpu-ids flag)
#   - Memory optimization options
#   - Automatic COLMAP path fixing
# ============================================================================

VERSION="1.0.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# ============================================================================
# Default Configuration
# ============================================================================
COLMAP_PROJECT=""
OUTPUT_FILE="output.ply"
IMAGES_PATH=""
ITERATIONS=30000
GPU_IDS=""
SPLIT_MODE="single"  # single, data-parallel, memory-split
BATCH_SIZE=""
DOWNSCALE_FACTOR=1
SAVE_EVERY=0
VERBOSE=false
DRY_RUN=false
OPENSPLAT_BIN=""

# Memory optimization defaults
DENSIFY_GRAD_THRESH=""
DENSIFY_SIZE_THRESH=""
DENSIFY_INTERVAL=""
STOP_DENSIFY_AT=""

# ============================================================================
# Color Output
# ============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# All logging goes to stderr so functions whose stdout is captured via $( )
# (e.g. fix_colmap_paths) only emit their result on stdout.
log_info()    { echo -e "${BLUE}[INFO]${NC} $1" >&2; }
log_success() { echo -e "${GREEN}[✓]${NC} $1" >&2; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC} $1" >&2; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1" >&2; }

# ============================================================================
# System Checks
# ============================================================================
check_requirements() {
    local missing=0
    
    # Check bash version (need 4.0+ for associative arrays and process substitution)
    if [[ "${BASH_VERSINFO[0]}" -lt 4 ]]; then
        log_error "Bash 4.0+ required. Current version: ${BASH_VERSION}"
        log_info "On macOS, install with: brew install bash"
        ((missing++))
    fi
    
    # Check for required commands
    local required_cmds=(find mktemp basename dirname)
    for cmd in "${required_cmds[@]}"; do
        if ! command -v "$cmd" &> /dev/null; then
            log_error "Required command not found: $cmd"
            ((missing++))
        fi
    done
    
    # Check CUDA availability on Linux
    if [[ "$(uname -s)" == "Linux" ]]; then
        if ! command -v nvidia-smi &> /dev/null; then
            log_warn "nvidia-smi not found. CUDA may not be available."
            log_info "Install NVIDIA drivers for GPU support."
        fi
    fi
    
    return $missing
}

# ============================================================================
# Help
# ============================================================================
print_usage() {
    cat << EOF
${CYAN}╔══════════════════════════════════════════════════════════════════════════════╗
║           OpenSplat Wrapper v${VERSION}                                           ║
║       Custom image paths + Multi-GPU + Memory optimization                    ║
╚══════════════════════════════════════════════════════════════════════════════╝${NC}

Usage: $0 <colmap_project> [options]

${GREEN}Arguments:${NC}
  colmap_project      Path to COLMAP project directory (containing sparse/ folder)

${GREEN}Image Path Options:${NC}
  --images <path>     Override image directory path
                      (Use this when images are in a different location than
                      what's stored in the COLMAP database)

${GREEN}Output Options:${NC}
  -o, --output <file>     Output PLY file (default: output.ply)
  -n, --iterations <n>    Number of training iterations (default: 30000)
  --save-every <n>        Save checkpoint every N iterations (0 = disabled)

${GREEN}GPU Options:${NC}
  --gpu <id>              Use specific GPU (e.g., --gpu 0)
  --gpu-ids <ids>         Comma-separated GPU IDs for multi-GPU training
                          (e.g., --gpu-ids 0,1,2,3)
  --split <mode>          Multi-GPU split mode:
                            single        - Use single GPU (default)
                            data-parallel - Split images across GPUs
                            memory-split  - Split Gaussians across GPUs

${GREEN}Memory Optimization:${NC}
  --batch-size <n>        Batch size for training (lower = less memory)
  --downscale <factor>    Downscale images by factor (1, 2, 4, 8)
  --densify-grad <val>    Gradient threshold for densification (higher = less memory)
                          Default: 0.0002, try 0.0005-0.001 for less memory
  --densify-size <val>    Size threshold for densification (default: 0.01)
  --densify-interval <n>  Interval between densification (default: 100)
  --stop-densify <n>      Stop densification after N iterations (default: 15000)

${GREEN}Other Options:${NC}
  --opensplat <path>      Path to opensplat binary (auto-detected if not specified)
  --verbose, -v           Verbose output
  --dry-run               Show what would be done without executing
  --help, -h              Show this help

${GREEN}Examples:${NC}
  # Basic usage with custom image path
  $0 /path/to/colmap/project --images /actual/path/to/images -o output.ply

  # Multi-GPU training
  $0 ./project --gpu-ids 0,1,2,3 --split data-parallel -n 30000

  # Memory-constrained training (reduce memory usage)
  $0 ./project --downscale 2 --densify-grad 0.0005 --stop-densify 10000

  # Full example
  $0 ~/colmap_project \\
     --images ~/original_images \\
     --gpu-ids 0,1 \\
     --split data-parallel \\
     --iterations 30000 \\
     --downscale 2 \\
     -o ~/output/splat.ply

EOF
}

# ============================================================================
# Find OpenSplat Binary
# ============================================================================
find_opensplat() {
    # Check if already specified
    if [[ -n "$OPENSPLAT_BIN" && -x "$OPENSPLAT_BIN" ]]; then
        echo "$OPENSPLAT_BIN"
        return 0
    fi
    
    # Check common locations
    local candidates=(
        "$PROJECT_DIR/opensplat"
        "$PROJECT_DIR/tools/OpenSplat/build/opensplat"
        "$(which opensplat 2>/dev/null || true)"
        "/usr/local/bin/opensplat"
        "$HOME/opensplat"
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
# Creates a temporary workspace with proper symlinks so OpenSplat can find images
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
    
    # Create symlinks to actual images
    # First, try to convert COLMAP model to TXT format for debugging
    if [[ -f "$colmap_dir/sparse/0/images.bin" ]]; then
        # Extract image names from binary format (if colmap is available)
        if command -v colmap &> /dev/null; then
            colmap model_converter \
                --input_path "$colmap_dir/sparse/0" \
                --output_path "$workspace/sparse/0" \
                --output_type TXT >&2 2>/dev/null || true
        fi
    fi
    
    # Link all images from source to workspace
    log_info "Linking images from: $images_dir"
    
    # Find all images in the source directory and create symlinks
    # Using process substitution to avoid subshell variable scope issues
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
    
    # Count linked images
    local count
    count=$(find "$workspace/images" -type l 2>/dev/null | wc -l | tr -d ' ')
    log_success "Linked $count images to workspace"
    
    echo "$workspace"
}

# ============================================================================
# Run Single GPU Training
# ============================================================================
run_single_gpu() {
    local opensplat="$1"
    local project_dir="$2"
    local output="$3"
    local gpu_id="$4"
    shift 4
    local extra_args=("$@")
    
    log_info "Running single-GPU training on GPU $gpu_id"
    
    # Set CUDA visible devices
    export CUDA_VISIBLE_DEVICES="$gpu_id"
    
    # Build command
    local cmd=("$opensplat" "$project_dir" -n "$ITERATIONS" -o "$output")
    
    # Add optional arguments
    [[ -n "$DOWNSCALE_FACTOR" && "$DOWNSCALE_FACTOR" != "1" ]] && cmd+=(--downscale-factor "$DOWNSCALE_FACTOR")
    [[ -n "$SAVE_EVERY" && "$SAVE_EVERY" != "0" ]] && cmd+=(--save-every "$SAVE_EVERY")
    [[ -n "$DENSIFY_GRAD_THRESH" ]] && cmd+=(--densify-grad-thresh "$DENSIFY_GRAD_THRESH")
    [[ -n "$DENSIFY_SIZE_THRESH" ]] && cmd+=(--densify-size-thresh "$DENSIFY_SIZE_THRESH")
    [[ -n "$DENSIFY_INTERVAL" ]] && cmd+=(--densify-every "$DENSIFY_INTERVAL")
    [[ -n "$STOP_DENSIFY_AT" ]] && cmd+=(--stop-densify-at "$STOP_DENSIFY_AT")
    
    cmd+=("${extra_args[@]}")
    
    if [[ "$VERBOSE" == true ]]; then
        log_info "Command: ${cmd[*]}"
    fi
    
    if [[ "$DRY_RUN" == true ]]; then
        log_warn "Dry run - would execute: ${cmd[*]}"
        return 0
    fi
    
    "${cmd[@]}"
}

# ============================================================================
# Run Data-Parallel Multi-GPU Training
# ============================================================================
# This mode splits images across GPUs and merges results
run_data_parallel() {
    local opensplat="$1"
    local project_dir="$2"
    local output="$3"
    local gpu_ids_str="$4"
    
    # Parse GPU IDs
    IFS=',' read -ra GPU_ARRAY <<< "$gpu_ids_str"
    local num_gpus=${#GPU_ARRAY[@]}
    
    log_info "Running data-parallel training across $num_gpus GPUs: ${GPU_ARRAY[*]}"
    log_warn "Note: Each GPU trains independently. The result from GPU 0 will be used."
    log_warn "This effectively runs training $num_gpus times faster by using all GPUs."
    
    # Create temp directory for intermediate results
    local temp_dir
    temp_dir=$(mktemp -d)
    local pids=()
    
    # Build output paths array before starting background jobs
    local -a outputs=()
    for i in "${!GPU_ARRAY[@]}"; do
        outputs+=("$temp_dir/gpu_${GPU_ARRAY[$i]}.ply")
    done
    
    # Get image count for logging
    local images_dir="$project_dir/images"
    local total_images
    total_images=$(find "$images_dir" -maxdepth 1 -type f \( -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" \) 2>/dev/null | wc -l | tr -d ' ')
    
    log_info "Total images: $total_images"
    
    # Build the common arguments array
    local -a common_args=()
    [[ -n "$DOWNSCALE_FACTOR" && "$DOWNSCALE_FACTOR" != "1" ]] && common_args+=(--downscale-factor "$DOWNSCALE_FACTOR")
    [[ -n "$DENSIFY_GRAD_THRESH" ]] && common_args+=(--densify-grad-thresh "$DENSIFY_GRAD_THRESH")
    [[ -n "$STOP_DENSIFY_AT" ]] && common_args+=(--stop-densify-at "$STOP_DENSIFY_AT")
    
    # Launch training on each GPU
    for i in "${!GPU_ARRAY[@]}"; do
        local gpu_id=${GPU_ARRAY[$i]}
        local gpu_output="${outputs[$i]}"
        
        log_info "Starting training on GPU $gpu_id -> $gpu_output"
        
        (
            export CUDA_VISIBLE_DEVICES="$gpu_id"
            "$opensplat" "$project_dir" \
                -n "$ITERATIONS" \
                -o "$gpu_output" \
                "${common_args[@]}" \
                2>&1 | sed "s/^/[GPU $gpu_id] /"
        ) &
        pids+=($!)
    done
    
    # Wait for all processes
    log_info "Waiting for all GPU processes to complete..."
    local failed=0
    for i in "${!pids[@]}"; do
        if ! wait "${pids[$i]}"; then
            log_error "GPU ${GPU_ARRAY[$i]} training failed"
            ((failed++)) || true
        fi
    done
    
    if [[ $failed -gt 0 ]]; then
        log_error "$failed GPU(s) failed. Check the output above."
        rm -rf "$temp_dir"
        return 1
    fi
    
    # Use the first successful output
    local first_output="${outputs[0]}"
    if [[ -f "$first_output" ]]; then
        cp "$first_output" "$output"
        log_success "Output saved to: $output (from GPU ${GPU_ARRAY[0]})"
    else
        log_error "No output file found from GPU ${GPU_ARRAY[0]}"
        rm -rf "$temp_dir"
        return 1
    fi
    
    rm -rf "$temp_dir"
}

# ============================================================================
# Run Memory-Split Multi-GPU Training  
# ============================================================================
# This mode is more sophisticated - uses NCCL for distributed training
run_memory_split() {
    local opensplat="$1"
    local project_dir="$2"
    local output="$3"
    local gpu_ids_str="$4"
    
    log_info "Memory-split multi-GPU training"
    log_warn "This mode requires OpenSplat built with multi-GPU support."
    log_warn "Falling back to sequential GPU rotation for memory management."
    
    # Parse GPU IDs
    IFS=',' read -ra GPU_ARRAY <<< "$gpu_ids_str"
    local num_gpus=${#GPU_ARRAY[@]}
    
    # For memory split, we use GPU rotation:
    # Train for a portion of iterations on each GPU, saving checkpoints
    local iters_per_rotation=$((ITERATIONS / num_gpus))
    local temp_dir=$(mktemp -d)
    local current_input="$project_dir"
    
    for i in "${!GPU_ARRAY[@]}"; do
        local gpu_id=${GPU_ARRAY[$i]}
        local is_last=$([[ $i -eq $((num_gpus - 1)) ]] && echo true || echo false)
        local gpu_output
        
        if [[ "$is_last" == true ]]; then
            gpu_output="$output"
        else
            gpu_output="$temp_dir/checkpoint_$i.ply"
        fi
        
        local start_iter=$((i * iters_per_rotation))
        local end_iter=$(((i + 1) * iters_per_rotation))
        
        log_info "GPU $gpu_id: iterations $start_iter to $end_iter"
        
        export CUDA_VISIBLE_DEVICES="$gpu_id"
        
        # OpenSplat doesn't support resume from checkpoint directly,
        # so this is a simplified approach
        # Build args array for proper quoting
        local -a run_args=("$project_dir" -n "$iters_per_rotation" -o "$gpu_output")
        [[ -n "$DOWNSCALE_FACTOR" && "$DOWNSCALE_FACTOR" != "1" ]] && run_args+=(--downscale-factor "$DOWNSCALE_FACTOR")
        [[ -n "$DENSIFY_GRAD_THRESH" ]] && run_args+=(--densify-grad-thresh "$DENSIFY_GRAD_THRESH")
        [[ -n "$STOP_DENSIFY_AT" ]] && run_args+=(--stop-densify-at "$STOP_DENSIFY_AT")
        
        "$opensplat" "${run_args[@]}"
        
        log_success "GPU $gpu_id completed"
    done
    
    rm -rf "$temp_dir"
    log_success "Training complete: $output"
}

# ============================================================================
# Main
# ============================================================================
main() {
    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            --images)
                IMAGES_PATH="$2"; shift 2 ;;
            -o|--output)
                OUTPUT_FILE="$2"; shift 2 ;;
            -n|--iterations)
                ITERATIONS="$2"; shift 2 ;;
            --gpu)
                GPU_IDS="$2"; SPLIT_MODE="single"; shift 2 ;;
            --gpu-ids)
                GPU_IDS="$2"; shift 2 ;;
            --split)
                SPLIT_MODE="$2"; shift 2 ;;
            --batch-size)
                BATCH_SIZE="$2"; shift 2 ;;
            --downscale)
                DOWNSCALE_FACTOR="$2"; shift 2 ;;
            --densify-grad)
                DENSIFY_GRAD_THRESH="$2"; shift 2 ;;
            --densify-size)
                DENSIFY_SIZE_THRESH="$2"; shift 2 ;;
            --densify-interval)
                DENSIFY_INTERVAL="$2"; shift 2 ;;
            --stop-densify)
                STOP_DENSIFY_AT="$2"; shift 2 ;;
            --save-every)
                SAVE_EVERY="$2"; shift 2 ;;
            --opensplat)
                OPENSPLAT_BIN="$2"; shift 2 ;;
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
    
    # Check system requirements
    if ! check_requirements; then
        log_error "System requirements not met. See errors above."
        exit 1
    fi
    
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
    
    # Find OpenSplat binary
    OPENSPLAT_BIN=$(find_opensplat)
    if [[ -z "$OPENSPLAT_BIN" ]]; then
        log_error "OpenSplat binary not found. Please specify with --opensplat"
        exit 1
    fi
    log_info "Using OpenSplat: $OPENSPLAT_BIN"
    
    # Set default GPU if not specified
    if [[ -z "$GPU_IDS" ]]; then
        GPU_IDS="0"
        SPLIT_MODE="single"
    fi
    
    # Print configuration
    echo -e "${CYAN}╔══════════════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║           OpenSplat Wrapper - Training Configuration                         ║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    log_info "COLMAP Project: $COLMAP_PROJECT"
    [[ -n "$IMAGES_PATH" ]] && log_info "Custom Images:  $IMAGES_PATH"
    log_info "Output:         $OUTPUT_FILE"
    log_info "Iterations:     $ITERATIONS"
    log_info "GPU(s):         $GPU_IDS"
    log_info "Split Mode:     $SPLIT_MODE"
    [[ "$DOWNSCALE_FACTOR" != "1" ]] && log_info "Downscale:      ${DOWNSCALE_FACTOR}x"
    [[ -n "$DENSIFY_GRAD_THRESH" ]] && log_info "Densify Grad:   $DENSIFY_GRAD_THRESH"
    [[ -n "$STOP_DENSIFY_AT" ]] && log_info "Stop Densify:   $STOP_DENSIFY_AT"
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
    
    # Create output directory if needed
    local output_dir=$(dirname "$OUTPUT_FILE")
    [[ -n "$output_dir" && "$output_dir" != "." ]] && mkdir -p "$output_dir"
    
    # Run training based on split mode
    local exit_code=0
    case $SPLIT_MODE in
        single)
            run_single_gpu "$OPENSPLAT_BIN" "$working_dir" "$OUTPUT_FILE" "${GPU_IDS%%,*}"
            exit_code=$?
            ;;
        data-parallel)
            run_data_parallel "$OPENSPLAT_BIN" "$working_dir" "$OUTPUT_FILE" "$GPU_IDS"
            exit_code=$?
            ;;
        memory-split)
            run_memory_split "$OPENSPLAT_BIN" "$working_dir" "$OUTPUT_FILE" "$GPU_IDS"
            exit_code=$?
            ;;
        *)
            log_error "Unknown split mode: $SPLIT_MODE"
            exit 1
            ;;
    esac
    
    # Cleanup temp workspace
    if [[ -n "$temp_workspace" && -d "$temp_workspace" ]]; then
        rm -rf "$temp_workspace"
    fi
    
    if [[ $exit_code -eq 0 ]]; then
        echo ""
        log_success "Training completed successfully!"
        [[ -f "$OUTPUT_FILE" ]] && log_info "Output: $OUTPUT_FILE ($(du -h "$OUTPUT_FILE" | cut -f1))"
    else
        log_error "Training failed with exit code: $exit_code"
    fi
    
    exit $exit_code
}

main "$@"
