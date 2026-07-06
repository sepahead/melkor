#!/bin/bash
set -e

# ============================================================================
# GLOMAP Wrapper Script
# ============================================================================
# Provides:
#   - Complete workflow from images to COLMAP-compatible sparse reconstruction
#   - Uses COLMAP for feature extraction and matching
#   - Uses GLOMAP for fast global SfM (10-100× faster than COLMAP mapper)
#   - Multiple matcher options (exhaustive, sequential, vocab_tree)
# ============================================================================

VERSION="1.0.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# ============================================================================
# Default Configuration
# ============================================================================
INPUT_PATH=""
OUTPUT_DIR=""
MATCHER="exhaustive"        # exhaustive, sequential, vocab_tree
QUALITY="medium"            # low, medium, high
USE_GPU="auto"              # auto, 0, 1
VERBOSE=false
DRY_RUN=false
SKIP_FEATURES=false
SKIP_MATCHING=false

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
# Help
# ============================================================================
print_usage() {
    cat << EOF
${CYAN}╔══════════════════════════════════════════════════════════════════════════════╗
║           GLOMAP Wrapper v${VERSION}                                              ║
║       Global Structure-from-Motion (10-100× faster than COLMAP)               ║
╚══════════════════════════════════════════════════════════════════════════════╝${NC}

Usage: $0 <input_images> <output_dir> [options]

${GREEN}Arguments:${NC}
  input_images        Path to directory containing images
                      OR path to existing COLMAP database (with --skip-features --skip-matching)
  output_dir          Output directory for COLMAP-compatible sparse reconstruction

${GREEN}Workflow Options:${NC}
  --skip-features     Skip feature extraction (use existing database)
  --skip-matching     Skip feature matching (use existing database)

${GREEN}Matching Options:${NC}
  --matcher <type>    Feature matching strategy:
                        exhaustive  - All pairs (best for <500 images, default)
                        sequential  - Sequential pairs (for video/ordered images)
                        vocab_tree  - Vocabulary tree (for 1000+ images)

${GREEN}Quality Options:${NC}
  --quality <level>   Feature extraction quality: low, medium, high

${GREEN}GPU Options:${NC}
  --gpu <mode>        GPU usage for COLMAP feature extraction:
                        auto - Auto-detect (default)
                        0    - Disable GPU
                        1    - Enable GPU

${GREEN}Other Options:${NC}
  --verbose, -v       Verbose output
  --dry-run           Show commands without executing
  --help, -h          Show this help

${GREEN}Examples:${NC}
  # Basic usage (exhaustive matching)
  $0 ~/Photos/scene ~/output/

  # Large dataset (vocab tree matching)
  $0 ~/Photos/scene ~/output/ --matcher vocab_tree

  # Video sequence
  $0 ~/Photos/scene ~/output/ --matcher sequential

  # High quality features
  $0 ~/Photos/scene ~/output/ --quality high

  # Existing database (GLOMAP mapping only)
  $0 ~/project/ ~/output/ --skip-features --skip-matching

${GREEN}Output:${NC}
  The output directory will contain:
    - database.db     : COLMAP database with features and matches
    - images/         : Symlinked or copied images
    - sparse/0/       : GLOMAP reconstruction (cameras.bin, images.bin, points3D.bin)

  This output is 100% compatible with OpenSplat, LichtFeld, gsplat, etc.

EOF
}

# ============================================================================
# Find GLOMAP Binary
# ============================================================================
find_glomap() {
    # Check common locations
    local candidates=(
        "$PROJECT_DIR/glomap"
        "$PROJECT_DIR/tools/glomap/build/glomap/glomap"
        "$PROJECT_DIR/tools/glomap/build/glomap"
        "$(which glomap 2>/dev/null || true)"
        "/usr/local/bin/glomap"
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
# Prepare Images
# ============================================================================
prepare_images() {
    local input_dir="$1"
    local workspace_dir="$2"
    
    log_info "Preparing images..."
    
    mkdir -p "$workspace_dir/images"
    
    # Count images
    local image_count=0
    
    # Copy or link images
    while IFS= read -r img; do
        local img_basename
        img_basename=$(basename "$img")
        if [[ ! -e "$workspace_dir/images/$img_basename" ]]; then
            ln -sf "$img" "$workspace_dir/images/$img_basename" 2>/dev/null || \
            cp "$img" "$workspace_dir/images/$img_basename"
            ((image_count++))
        fi
    done < <(find "$input_dir" -maxdepth 1 -type f \( \
        -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o \
        -iname "*.JPG" -o -iname "*.JPEG" -o -iname "*.PNG" -o \
        -iname "*.tiff" -o -iname "*.tif" -o -iname "*.bmp" \
    \))
    
    # Handle HEIC conversion on macOS
    local heic_count
    heic_count=$(find "$input_dir" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) 2>/dev/null | wc -l | tr -d ' ')
    if [[ $heic_count -gt 0 ]]; then
        log_info "Found $heic_count HEIC/HEIF images, converting to JPEG..."
        
        if command -v sips &> /dev/null; then
            # macOS
            find "$input_dir" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) | while read -r heic_file; do
                local fname=$(basename "$heic_file")
                local output_name="${fname%.*}.jpg"
                sips -s format jpeg "$heic_file" --out "$workspace_dir/images/$output_name" 2>/dev/null || true
                ((image_count++))
            done
        elif command -v convert &> /dev/null; then
            # ImageMagick
            find "$input_dir" -maxdepth 1 -type f \( -iname "*.heic" -o -iname "*.heif" \) | while read -r heic_file; do
                local fname=$(basename "$heic_file")
                local output_name="${fname%.*}.jpg"
                convert "$heic_file" "$workspace_dir/images/$output_name" 2>/dev/null || true
                ((image_count++))
            done
        else
            log_warn "HEIC conversion not available. Install ImageMagick."
        fi
    fi
    
    # Final count
    local final_count
    final_count=$(find "$workspace_dir/images" -type f \( -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" \) | wc -l | tr -d ' ')
    
    if [[ $final_count -lt 3 ]]; then
        log_error "Need at least 3 images for reconstruction. Found: $final_count"
        exit 1
    fi
    
    if [[ $final_count -lt 20 ]]; then
        log_warn "Only $final_count images found. Recommend 20+ images for good results."
    fi
    
    log_success "Prepared $final_count images"
}

# ============================================================================
# Run COLMAP Feature Extraction
# ============================================================================
run_feature_extraction() {
    local workspace_dir="$1"
    local quality="$2"
    local use_gpu="$3"
    
    log_step "Feature Extraction (COLMAP)"
    
    if ! command -v colmap &> /dev/null; then
        log_error "COLMAP not found. Install with: brew install colmap (macOS) or apt install colmap (Linux)"
        exit 1
    fi
    
    # Determine GPU usage
    local gpu_flag=0
    if [[ "$use_gpu" == "auto" ]]; then
        if [[ "$(uname -s)" == "Linux" ]] && command -v nvidia-smi &> /dev/null; then
            gpu_flag=1
            log_info "Using CUDA GPU for feature extraction"
        else
            gpu_flag=0
            log_info "Using CPU for feature extraction"
        fi
    elif [[ "$use_gpu" == "1" ]]; then
        gpu_flag=1
    fi
    
    # Map quality to COLMAP options
    local sift_options=""
    case $quality in
        low)
            sift_options="--SiftExtraction.max_num_features 4096"
            ;;
        medium)
            sift_options="--SiftExtraction.max_num_features 8192"
            ;;
        high)
            sift_options="--SiftExtraction.max_num_features 16384"
            ;;
    esac
    
    log_info "Running COLMAP feature extraction..."
    
    if [[ "$DRY_RUN" == true ]]; then
        log_warn "Dry run - would execute:"
        echo "colmap feature_extractor --image_path $workspace_dir/images --database_path $workspace_dir/database.db --SiftExtraction.use_gpu $gpu_flag $sift_options"
        return 0
    fi
    
    colmap feature_extractor \
        --image_path "$workspace_dir/images" \
        --database_path "$workspace_dir/database.db" \
        --SiftExtraction.use_gpu "$gpu_flag" \
        $sift_options \
        ${VERBOSE:+--verbose 1}
    
    log_success "Feature extraction complete"
}

# ============================================================================
# Run COLMAP Feature Matching
# ============================================================================
run_feature_matching() {
    local workspace_dir="$1"
    local matcher="$2"
    local use_gpu="$3"
    
    log_step "Feature Matching (COLMAP)"
    
    # Determine GPU usage
    local gpu_flag=0
    if [[ "$use_gpu" == "auto" ]]; then
        if [[ "$(uname -s)" == "Linux" ]] && command -v nvidia-smi &> /dev/null; then
            gpu_flag=1
        fi
    elif [[ "$use_gpu" == "1" ]]; then
        gpu_flag=1
    fi
    
    log_info "Running COLMAP $matcher matcher..."
    
    if [[ "$DRY_RUN" == true ]]; then
        log_warn "Dry run - would execute:"
        echo "colmap ${matcher}_matcher --database_path $workspace_dir/database.db --SiftMatching.use_gpu $gpu_flag"
        return 0
    fi
    
    case $matcher in
        exhaustive)
            colmap exhaustive_matcher \
                --database_path "$workspace_dir/database.db" \
                --SiftMatching.use_gpu "$gpu_flag" \
                ${VERBOSE:+--verbose 1}
            ;;
        sequential)
            colmap sequential_matcher \
                --database_path "$workspace_dir/database.db" \
                --SiftMatching.use_gpu "$gpu_flag" \
                ${VERBOSE:+--verbose 1}
            ;;
        vocab_tree)
            # Check for vocabulary tree
            local vocab_tree=""
            local vocab_paths=(
                "$HOME/.colmap/vocab_tree_flickr100K_words256K.bin"
                "/usr/share/colmap/vocab_tree_flickr100K_words256K.bin"
            )
            for path in "${vocab_paths[@]}"; do
                if [[ -f "$path" ]]; then
                    vocab_tree="$path"
                    break
                fi
            done
            
            if [[ -z "$vocab_tree" ]]; then
                log_warn "Vocabulary tree not found, downloading..."
                mkdir -p "$HOME/.colmap"
                curl -L "https://demuc.de/colmap/vocab_tree_flickr100K_words256K.bin" -o "$HOME/.colmap/vocab_tree_flickr100K_words256K.bin"
                vocab_tree="$HOME/.colmap/vocab_tree_flickr100K_words256K.bin"
            fi
            
            colmap vocab_tree_matcher \
                --database_path "$workspace_dir/database.db" \
                --VocabTreeMatching.vocab_tree_path "$vocab_tree" \
                --SiftMatching.use_gpu "$gpu_flag" \
                ${VERBOSE:+--verbose 1}
            ;;
        *)
            log_error "Unknown matcher: $matcher"
            exit 1
            ;;
    esac
    
    log_success "Feature matching complete"
}

# ============================================================================
# Run GLOMAP Mapping
# ============================================================================
run_glomap_mapping() {
    local workspace_dir="$1"
    
    log_step "Global SfM Mapping (GLOMAP)"
    
    # Find GLOMAP binary
    local glomap_bin
    glomap_bin=$(find_glomap) || true
    
    if [[ -z "$glomap_bin" ]]; then
        log_error "GLOMAP not found. Run: ./scripts/setup_glomap.sh"
        exit 1
    fi
    
    log_info "Using GLOMAP: $glomap_bin"
    
    mkdir -p "$workspace_dir/sparse"
    
    log_info "Running GLOMAP mapper (this is the fast part!)..."
    
    if [[ "$DRY_RUN" == true ]]; then
        log_warn "Dry run - would execute:"
        echo "$glomap_bin mapper --database_path $workspace_dir/database.db --image_path $workspace_dir/images --output_path $workspace_dir/sparse"
        return 0
    fi
    
    "$glomap_bin" mapper \
        --database_path "$workspace_dir/database.db" \
        --image_path "$workspace_dir/images" \
        --output_path "$workspace_dir/sparse"
    
    # Verify output
    if [[ ! -f "$workspace_dir/sparse/0/cameras.bin" ]] && [[ ! -f "$workspace_dir/sparse/cameras.bin" ]]; then
        # Check if files are directly in sparse/
        if [[ -f "$workspace_dir/sparse/cameras.bin" ]]; then
            # Create 0/ subdirectory for compatibility
            mkdir -p "$workspace_dir/sparse/0"
            mv "$workspace_dir/sparse/cameras.bin" "$workspace_dir/sparse/0/"
            mv "$workspace_dir/sparse/images.bin" "$workspace_dir/sparse/0/"
            mv "$workspace_dir/sparse/points3D.bin" "$workspace_dir/sparse/0/"
        else
            log_error "GLOMAP reconstruction failed - no output files"
            log_info "Tips:"
            log_info "  - Ensure images have 60-80% overlap"
            log_info "  - Avoid blurry images"
            log_info "  - Try different matcher (--matcher sequential)"
            exit 1
        fi
    fi
    
    log_success "GLOMAP reconstruction complete"
}

# ============================================================================
# Main
# ============================================================================
main() {
    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            --matcher)
                MATCHER="$2"; shift 2 ;;
            --quality)
                QUALITY="$2"; shift 2 ;;
            --gpu)
                USE_GPU="$2"; shift 2 ;;
            --skip-features)
                SKIP_FEATURES=true; shift ;;
            --skip-matching)
                SKIP_MATCHING=true; shift ;;
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
    
    if [[ ! -e "$INPUT_PATH" ]]; then
        log_error "Input path does not exist: $INPUT_PATH"
        exit 1
    fi
    
    # Resolve paths
    INPUT_PATH=$(cd "$INPUT_PATH" 2>/dev/null && pwd || echo "$INPUT_PATH")
    mkdir -p "$OUTPUT_DIR"
    OUTPUT_DIR=$(cd "$OUTPUT_DIR" && pwd)
    
    # Print configuration
    echo -e "${CYAN}╔══════════════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║           GLOMAP - Global Structure-from-Motion                              ║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    log_info "Input:      $INPUT_PATH"
    log_info "Output:     $OUTPUT_DIR"
    log_info "Matcher:    $MATCHER"
    log_info "Quality:    $QUALITY"
    [[ "$SKIP_FEATURES" == true ]] && log_info "Skip features: Yes"
    [[ "$SKIP_MATCHING" == true ]] && log_info "Skip matching: Yes"
    echo ""
    
    if [[ "$DRY_RUN" == true ]]; then
        log_warn "Dry run - no changes will be made"
    fi
    
    # Determine workspace
    local WORKSPACE_DIR="$OUTPUT_DIR"
    
    # Step 1: Prepare images (if not skipping features)
    if [[ "$SKIP_FEATURES" == false ]]; then
        prepare_images "$INPUT_PATH" "$WORKSPACE_DIR"
    else
        # Use existing database
        if [[ ! -f "$INPUT_PATH/database.db" ]]; then
            log_error "No database.db found in $INPUT_PATH"
            exit 1
        fi
        
        # Copy database to workspace if needed
        if [[ "$INPUT_PATH" != "$WORKSPACE_DIR" ]]; then
            cp "$INPUT_PATH/database.db" "$WORKSPACE_DIR/"
            [[ -d "$INPUT_PATH/images" ]] && ln -sf "$INPUT_PATH/images" "$WORKSPACE_DIR/images"
        fi
    fi
    
    # Step 2: Feature extraction (COLMAP)
    if [[ "$SKIP_FEATURES" == false ]]; then
        run_feature_extraction "$WORKSPACE_DIR" "$QUALITY" "$USE_GPU"
    else
        log_info "Skipping feature extraction (--skip-features)"
    fi
    
    # Step 3: Feature matching (COLMAP)
    if [[ "$SKIP_MATCHING" == false ]]; then
        run_feature_matching "$WORKSPACE_DIR" "$MATCHER" "$USE_GPU"
    else
        log_info "Skipping feature matching (--skip-matching)"
    fi
    
    # Step 4: GLOMAP mapping (the fast part!)
    run_glomap_mapping "$WORKSPACE_DIR"
    
    # Summary
    echo ""
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║                         GLOMAP COMPLETE! ✓                                   ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    log_info "Output directory: $OUTPUT_DIR"
    
    # List output files
    if [[ -d "$OUTPUT_DIR/sparse/0" ]]; then
        log_info "Sparse reconstruction:"
        ls -lh "$OUTPUT_DIR/sparse/0/"*.bin 2>/dev/null || true
    elif [[ -d "$OUTPUT_DIR/sparse" ]]; then
        log_info "Sparse reconstruction:"
        ls -lh "$OUTPUT_DIR/sparse/"*.bin 2>/dev/null || true
    fi
    
    echo ""
    log_info "Next steps:"
    echo "  # Train 3D Gaussian Splats with OpenSplat:"
    echo "  ./opensplat $OUTPUT_DIR -o output.ply"
    echo ""
    echo "  # Or with LichtFeld-Studio (Linux CUDA):"
    echo "  ./lichtfeld -d $OUTPUT_DIR -o output/"
    echo ""
    echo "  # Or via the pipeline:"
    echo "  ./scripts/pipeline.sh $OUTPUT_DIR output/ --skip-colmap"
    echo ""
}

main "$@"
