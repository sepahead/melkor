#!/usr/bin/env bash
set -euo pipefail

# Development pipeline for COLMAP plus one user-supplied OpenSplat binary.
# The planned manifest-driven adapter runner will replace this script.

VERSION="2.0.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

INPUT_PATH=""
OUTPUT_DIR=""
SFM_TOOL="colmap"
SKIP_COLMAP=false
COLMAP_QUALITY="medium"
MATCHER="exhaustive"
QUALITY="medium"
ITERATIONS=""
OUTPUT_FORMAT="ply"
IMAGES_PATH=""
GPU_ID=""
OPENSPLAT_BIN="${MELKOR_OPENSPLAT_BIN:-}"
MELKOR_BIN="${MELKOR_BIN:-}"
DRY_RUN=false
VERBOSE=false

log() { printf '[INFO] %s\n' "$*" >&2; }
fail() { printf '[ERROR] %s\n' "$*" >&2; exit 2; }

usage() {
    cat <<EOF
Melkor reconstruction pipeline ${VERSION}

Usage: $0 INPUT OUTPUT_DIR [options]

Options:
  --skip-colmap          Treat INPUT as an existing COLMAP project
  --sfm MODE             Use colmap or global (default: colmap)
  --colmap-quality LEVEL Use low, medium, or high (default: medium)
  --matcher TYPE         Global matcher: exhaustive, sequential, or vocab_tree
  --images PATH          Use PATH as the image source for an existing project
  --quality LEVEL        Use fast, medium, or high training iterations
  --iterations N         Replace the training iteration count
  --format TYPE          Write ply, spz, or both (default: ply)
  --gpu ID               Set one CUDA device for OpenSplat
  --opensplat PATH       Use this OpenSplat executable
  --melkor PATH          Use this Melkor executable for SPZ conversion
  --dry-run              Print the plan without changing files
  --verbose, -v          Print detailed stage information
  --help, -h             Show this help

This wrapper supports one verified trainer contract: OpenSplat. It does not
install external tools. It refuses an existing output directory.
EOF
}

need_value() {
    local option="$1"
    local count="$2"
    [[ "$count" -ge 2 ]] || fail "Missing value for $option"
}

print_command() {
    printf 'DRY RUN:'
    printf ' %q' "$@"
    printf '\n'
}

find_model_dir() {
    local project="$1"
    if [[ -s "$project/sparse/0/cameras.bin" && \
          -s "$project/sparse/0/images.bin" && \
          -s "$project/sparse/0/points3D.bin" ]]; then
        printf '%s\n' "$project/sparse/0"
        return
    fi
    if [[ -s "$project/sparse/cameras.bin" && \
          -s "$project/sparse/images.bin" && \
          -s "$project/sparse/points3D.bin" ]]; then
        printf '%s\n' "$project/sparse"
        return
    fi
    return 1
}

resolve_executable() {
    local configured="$1"
    local project_candidate="$2"
    local command_name="$3"
    if [[ -n "$configured" ]]; then
        [[ -x "$configured" ]] || return 1
        printf '%s/%s\n' "$(cd "$(dirname "$configured")" && pwd)" "$(basename "$configured")"
        return
    fi
    if [[ -x "$project_candidate" ]]; then
        printf '%s\n' "$project_candidate"
        return
    fi
    command -v "$command_name" 2>/dev/null
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-colmap)
            SKIP_COLMAP=true
            shift
            ;;
        --sfm)
            need_value "$1" "$#"
            SFM_TOOL="$2"
            shift 2
            ;;
        --colmap-quality)
            need_value "$1" "$#"
            COLMAP_QUALITY="$2"
            shift 2
            ;;
        --matcher)
            need_value "$1" "$#"
            MATCHER="$2"
            shift 2
            ;;
        --images)
            need_value "$1" "$#"
            IMAGES_PATH="$2"
            shift 2
            ;;
        --quality)
            need_value "$1" "$#"
            QUALITY="$2"
            shift 2
            ;;
        --iterations)
            need_value "$1" "$#"
            ITERATIONS="$2"
            shift 2
            ;;
        --format)
            need_value "$1" "$#"
            OUTPUT_FORMAT="$2"
            shift 2
            ;;
        --gpu)
            need_value "$1" "$#"
            GPU_ID="$2"
            shift 2
            ;;
        --opensplat)
            need_value "$1" "$#"
            OPENSPLAT_BIN="$2"
            shift 2
            ;;
        --melkor)
            need_value "$1" "$#"
            MELKOR_BIN="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --tool)
            need_value "$1" "$#"
            [[ "$2" == "auto" || "$2" == "opensplat" ]] || \
                fail "Only the OpenSplat trainer contract is available"
            shift 2
            ;;
        --backend)
            need_value "$1" "$#"
            [[ "$2" == "auto" ]] || fail "--backend is retired. Select the external binary explicitly."
            shift 2
            ;;
        --gpu-ids)
            need_value "$1" "$#"
            [[ "$2" =~ ^[0-9]+$ ]] || fail "The simulated multi-GPU path was removed. Use --gpu ID."
            GPU_ID="$2"
            shift 2
            ;;
        --gpu-split|--downscale)
            fail "$1 is retired because the wrapper did not provide a verified contract"
            ;;
        --setup)
            fail "Automatic external-tool installation is retired. See docs/adapters/index.md."
            ;;
        --version)
            printf 'Melkor Pipeline %s\n' "$VERSION"
            exit 0
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        -*)
            fail "Unknown option: $1"
            ;;
        *)
            if [[ -z "$INPUT_PATH" ]]; then
                INPUT_PATH="$1"
            elif [[ -z "$OUTPUT_DIR" ]]; then
                OUTPUT_DIR="$1"
            else
                fail "Unexpected argument: $1"
            fi
            shift
            ;;
    esac
done

[[ -n "$INPUT_PATH" ]] || fail "Missing INPUT"
[[ -n "$OUTPUT_DIR" ]] || fail "Missing OUTPUT_DIR"
[[ -d "$INPUT_PATH" ]] || fail "Input directory does not exist: $INPUT_PATH"

case "$SFM_TOOL" in
    colmap|global) ;;
    glomap) fail "The standalone GLOMAP path was removed. Use --sfm global." ;;
    *) fail "Unknown structure-from-motion mode: $SFM_TOOL" ;;
esac
case "$COLMAP_QUALITY" in low|medium|high) ;; *) fail "Invalid COLMAP quality: $COLMAP_QUALITY" ;; esac
case "$MATCHER" in exhaustive|sequential|vocab_tree) ;; *) fail "Invalid matcher: $MATCHER" ;; esac
case "$QUALITY" in fast|medium|high) ;; *) fail "Invalid training quality: $QUALITY" ;; esac
case "$OUTPUT_FORMAT" in ply|spz|both) ;; *) fail "Invalid output format: $OUTPUT_FORMAT" ;; esac
[[ -z "$GPU_ID" || "$GPU_ID" =~ ^[0-9]+$ ]] || fail "GPU ID must be a nonnegative integer"

if [[ -z "$ITERATIONS" ]]; then
    case "$QUALITY" in
        fast) ITERATIONS=7000 ;;
        medium) ITERATIONS=15000 ;;
        high) ITERATIONS=30000 ;;
    esac
fi
[[ "$ITERATIONS" =~ ^[1-9][0-9]*$ ]] || fail "Iterations must be a positive integer"

INPUT_PATH="$(cd "$INPUT_PATH" && pwd)"
if [[ -n "$IMAGES_PATH" ]]; then
    [[ -d "$IMAGES_PATH" ]] || fail "Image directory does not exist: $IMAGES_PATH"
    IMAGES_PATH="$(cd "$IMAGES_PATH" && pwd)"
fi
if [[ "$OUTPUT_DIR" != /* ]]; then
    OUTPUT_DIR="$PWD/$OUTPUT_DIR"
fi
[[ ! -e "$OUTPUT_DIR" ]] || fail "Output directory already exists: $OUTPUT_DIR"

if [[ "$SKIP_COLMAP" == true ]] || find_model_dir "$INPUT_PATH" >/dev/null 2>&1; then
    COLMAP_PROJECT="$INPUT_PATH"
    find_model_dir "$COLMAP_PROJECT" >/dev/null || fail "Input does not contain a complete COLMAP sparse model"
    if [[ -z "$IMAGES_PATH" ]]; then
        [[ -d "$COLMAP_PROJECT/images" ]] || fail "The COLMAP project has no images/. Use --images PATH."
        IMAGES_PATH="$COLMAP_PROJECT/images"
    fi
    RUN_SFM=false
else
    image_count="$(find "$INPUT_PATH" -maxdepth 1 -type f \
        \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' \
        -o -iname '*.tif' -o -iname '*.tiff' -o -iname '*.bmp' \) | wc -l | tr -d ' ')"
    [[ "$image_count" -ge 3 ]] || fail "At least three supported images are required"
    IMAGES_PATH="$INPUT_PATH"
    COLMAP_PROJECT="$OUTPUT_DIR/workspace"
    RUN_SFM=true
fi

if [[ "$DRY_RUN" == false ]]; then
    OPENSPLAT_BIN="$(resolve_executable "$OPENSPLAT_BIN" "$PROJECT_DIR/opensplat" opensplat)" || \
        fail "OpenSplat is unavailable. Use --opensplat PATH."
    if [[ "$OUTPUT_FORMAT" == "spz" || "$OUTPUT_FORMAT" == "both" ]]; then
        if [[ -z "$MELKOR_BIN" ]]; then
            for candidate in "$PROJECT_DIR/build/melkor" "$PROJECT_DIR/build/dev/melkor"; do
                if [[ -x "$candidate" ]]; then
                    MELKOR_BIN="$candidate"
                    break
                fi
            done
        fi
        [[ -n "$MELKOR_BIN" && -x "$MELKOR_BIN" ]] || \
            fail "SPZ output requires Melkor. Use --melkor PATH."
    fi
fi

log "Input: $INPUT_PATH"
log "Output: $OUTPUT_DIR"
log "SfM: $([[ "$RUN_SFM" == true ]] && printf '%s' "$SFM_TOOL" || printf 'existing COLMAP project')"
log "Trainer: OpenSplat, $ITERATIONS iterations"
log "Format: $OUTPUT_FORMAT"

if [[ "$DRY_RUN" == true ]]; then
    if [[ "$RUN_SFM" == true ]]; then
        if [[ "$SFM_TOOL" == "global" ]]; then
            "$SCRIPT_DIR/glomap_wrapper.sh" "$IMAGES_PATH" "$COLMAP_PROJECT" \
                --quality "$COLMAP_QUALITY" --matcher "$MATCHER" --dry-run
        else
            print_command colmap automatic_reconstructor \
                --workspace_path "$COLMAP_PROJECT" \
                --image_path "$IMAGES_PATH" \
                --quality "$COLMAP_QUALITY" \
                --single_camera 1
        fi
    fi
    trainer=("$SCRIPT_DIR/opensplat_wrapper.sh" "$COLMAP_PROJECT" \
        --images "$IMAGES_PATH" --output "$OUTPUT_DIR/point_cloud.ply" \
        --iterations "$ITERATIONS" --opensplat "${OPENSPLAT_BIN:-<opensplat>}" --dry-run)
    [[ -n "$GPU_ID" ]] && trainer+=(--gpu "$GPU_ID")
    print_command "${trainer[@]}"
    if [[ "$OUTPUT_FORMAT" == "spz" || "$OUTPUT_FORMAT" == "both" ]]; then
        print_command "${MELKOR_BIN:-<melkor>}" "$OUTPUT_DIR/point_cloud.ply" "$OUTPUT_DIR/point_cloud.spz"
    fi
    exit 0
fi

mkdir -p "$OUTPUT_DIR"
if [[ "$RUN_SFM" == true ]]; then
    if [[ "$SFM_TOOL" == "global" ]]; then
        global_args=("$IMAGES_PATH" "$COLMAP_PROJECT" --quality "$COLMAP_QUALITY" --matcher "$MATCHER")
        [[ "$VERBOSE" == true ]] && global_args+=(--verbose)
        "$SCRIPT_DIR/glomap_wrapper.sh" "${global_args[@]}"
    else
        command -v colmap >/dev/null 2>&1 || fail "COLMAP is unavailable"
        mkdir -p "$COLMAP_PROJECT"
        colmap automatic_reconstructor \
            --workspace_path "$COLMAP_PROJECT" \
            --image_path "$IMAGES_PATH" \
            --quality "$COLMAP_QUALITY" \
            --single_camera 1
        find_model_dir "$COLMAP_PROJECT" >/dev/null || fail "COLMAP did not create a complete sparse model"
    fi
fi

trainer=("$SCRIPT_DIR/opensplat_wrapper.sh" "$COLMAP_PROJECT" \
    --images "$IMAGES_PATH" --output "$OUTPUT_DIR/point_cloud.ply" \
    --iterations "$ITERATIONS" --opensplat "$OPENSPLAT_BIN")
[[ -n "$GPU_ID" ]] && trainer+=(--gpu "$GPU_ID")
[[ "$VERBOSE" == true ]] && trainer+=(--verbose)
"${trainer[@]}"

if [[ "$OUTPUT_FORMAT" == "spz" || "$OUTPUT_FORMAT" == "both" ]]; then
    "$MELKOR_BIN" "$OUTPUT_DIR/point_cloud.ply" "$OUTPUT_DIR/point_cloud.spz"
    [[ -s "$OUTPUT_DIR/point_cloud.spz" ]] || fail "Melkor did not create the SPZ output"
fi

log "Pipeline completed: $OUTPUT_DIR"
