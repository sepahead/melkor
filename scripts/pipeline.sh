#!/usr/bin/env bash
set -euo pipefail

# Development pipeline for COLMAP plus one user-supplied OpenSplat binary.
# The planned manifest-driven adapter runner will replace this script.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
[[ -f "$PROJECT_DIR/VERSION" ]] || { printf '[ERROR] VERSION is missing\n' >&2; exit 2; }
VERSION="$(<"$PROJECT_DIR/VERSION")"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$ ]] || {
    printf '[ERROR] VERSION is invalid\n' >&2
    exit 2
}

INPUT_PATH=""
OUTPUT_DIR=""
SFM_TOOL="colmap"
SKIP_COLMAP=false
COLMAP_QUALITY="medium"
MATCHER="exhaustive"
VOCAB_TREE=""
QUALITY="medium"
ITERATIONS=""
OUTPUT_FORMAT="ply"
IMAGES_PATH=""
GPU_ID=""
OPENSPLAT_BIN="${MELKOR_OPENSPLAT_BIN:-}"
MELKOR_BIN="${MELKOR_BIN:-}"
SOURCE_FRAME=""
SOURCE_UNIT_TO_METER=""
SOURCE_COLOR_SPACE=""
OUTPUT_ANTIALIASED=""
ALLOW_LOSSES=()
DRY_RUN=false
VERBOSE=false
STAGING_DIR=""
FINAL_OUTPUT_DIR=""

log() { printf '[INFO] %s\n' "$*" >&2; }
fail() { printf '[ERROR] %s\n' "$*" >&2; exit 2; }

cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "$STAGING_DIR" && -d "$STAGING_DIR" ]]; then
        rm -rf -- "$STAGING_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

usage() {
    cat <<EOF
Melkor reconstruction pipeline ${VERSION}

Usage: $0 INPUT OUTPUT_DIR [options]

Options:
  --skip-colmap          Treat INPUT as an existing COLMAP project
  --sfm MODE             Use colmap or global (default: colmap)
  --colmap-quality LEVEL Use low, medium, or high (default: medium)
  --matcher TYPE         Global matcher: exhaustive, sequential, or vocab_tree
  --vocab-tree PATH      Set the data file for global vocab_tree matching
  --images PATH          Use PATH as the image source for an existing project
  --quality LEVEL        Use fast, medium, or high training iterations
  --iterations N         Replace the training iteration count
  --format TYPE          Write ply, spz, or both (default: ply)
  --gpu ID               Set one CUDA device for OpenSplat
  --opensplat PATH       Use this OpenSplat executable
  --melkor PATH          Use this Melkor executable for SPZ conversion
  --source-frame FRAME   Set the verified PLY frame for SPZ conversion
  --source-unit-to-meter VALUE
                         Set positive meters per PLY unit for SPZ conversion
  --source-color-space SPACE
                         Set srgb_rec709_display or lin_rec709_display
  --output-antialiased BOOL
                         Set true or false for the SPZ antialiasing flag
  --allow-loss CODE      Approve one Melkor loss. Repeat as needed.
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
        --vocab-tree)
            need_value "$1" "$#"
            VOCAB_TREE="$2"
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
        --source-frame)
            need_value "$1" "$#"
            SOURCE_FRAME="$2"
            shift 2
            ;;
        --source-unit-to-meter)
            need_value "$1" "$#"
            SOURCE_UNIT_TO_METER="$2"
            shift 2
            ;;
        --source-color-space)
            need_value "$1" "$#"
            SOURCE_COLOR_SPACE="$2"
            shift 2
            ;;
        --output-antialiased)
            need_value "$1" "$#"
            OUTPUT_ANTIALIASED="$2"
            shift 2
            ;;
        --allow-loss)
            need_value "$1" "$#"
            ALLOW_LOSSES+=("$2")
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

if [[ "$OUTPUT_FORMAT" == "spz" || "$OUTPUT_FORMAT" == "both" ]]; then
    case "$SOURCE_FRAME" in
        gltf-luf|ply-rdf|spz-rub) ;;
        "") fail "SPZ output requires --source-frame" ;;
        *) fail "Invalid source frame: $SOURCE_FRAME" ;;
    esac
    [[ "$SOURCE_UNIT_TO_METER" =~ ^([1-9][0-9]*([.][0-9]*)?|0[.][0-9]*[1-9][0-9]*)$ ]] || \
        fail "SPZ output requires a positive --source-unit-to-meter value"
    case "$SOURCE_COLOR_SPACE" in
        srgb_rec709_display|lin_rec709_display) ;;
        "") fail "SPZ output requires --source-color-space" ;;
        *) fail "Invalid source color space: $SOURCE_COLOR_SPACE" ;;
    esac
    case "$OUTPUT_ANTIALIASED" in
        true|false) ;;
        "") fail "SPZ output requires --output-antialiased" ;;
        *) fail "--output-antialiased must be true or false" ;;
    esac
    for required_loss in \
        LOSS_COLOR_SPACE_METADATA_DROPPED \
        LOSS_COORDINATE_METADATA_DROPPED; do
        approved=false
        for code in "${ALLOW_LOSSES[@]}"; do
            [[ "$code" == "$required_loss" ]] && approved=true
        done
        [[ "$approved" == true ]] || fail "SPZ output requires --allow-loss $required_loss"
    done
else
    if [[ -n "$SOURCE_FRAME" || -n "$SOURCE_UNIT_TO_METER" ||
          -n "$SOURCE_COLOR_SPACE" || -n "$OUTPUT_ANTIALIASED" ||
          ${#ALLOW_LOSSES[@]} -gt 0 ]]; then
        fail "SPZ conversion options require --format spz or --format both"
    fi
fi

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
output_parent="$(dirname "$OUTPUT_DIR")"
output_name="$(basename "$OUTPUT_DIR")"
[[ "$output_name" != "." && "$output_name" != ".." && "$output_name" != "/" ]] || \
    fail "Invalid output directory name"
if [[ "$DRY_RUN" == true ]]; then
    [[ "$output_parent" == /* ]] || output_parent="$PWD/$output_parent"
else
    mkdir -p "$output_parent"
    output_parent="$(cd "$output_parent" && pwd)"
fi
FINAL_OUTPUT_DIR="$output_parent/$output_name"
[[ ! -e "$FINAL_OUTPUT_DIR" && ! -L "$FINAL_OUTPUT_DIR" ]] || \
    fail "Output directory already exists: $FINAL_OUTPUT_DIR"
OUTPUT_DIR="$FINAL_OUTPUT_DIR"

paths_overlap() {
    local first="$1"
    local second="$2"
    [[ "$first" == "$second" || "$first" == "$second/"* || "$second" == "$first/"* ]]
}

if [[ "$SKIP_COLMAP" == true ]] || find_model_dir "$INPUT_PATH" >/dev/null 2>&1; then
    COLMAP_PROJECT="$INPUT_PATH"
    find_model_dir "$COLMAP_PROJECT" >/dev/null || fail "Input does not contain a complete COLMAP sparse model"
    if [[ -z "$IMAGES_PATH" ]]; then
        [[ -d "$COLMAP_PROJECT/images" ]] || fail "The COLMAP project has no images/. Use --images PATH."
        IMAGES_PATH="$COLMAP_PROJECT/images"
    fi
    RUN_SFM=false
else
    image_count=0
    while IFS= read -r -d '' _image; do
        image_count=$((image_count + 1))
    done < <(find "$INPUT_PATH" -maxdepth 1 -type f \
        \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' \
        -o -iname '*.tif' -o -iname '*.tiff' -o -iname '*.bmp' \) -print0)
    [[ "$image_count" -ge 3 ]] || fail "At least three supported images are required"
    IMAGES_PATH="$INPUT_PATH"
    COLMAP_PROJECT="$OUTPUT_DIR/workspace"
    RUN_SFM=true
fi

if paths_overlap "$INPUT_PATH" "$FINAL_OUTPUT_DIR"; then
    fail "The input and output directories must not overlap"
fi
if paths_overlap "$IMAGES_PATH" "$FINAL_OUTPUT_DIR"; then
    fail "The image and output directories must not overlap"
fi

if [[ "$RUN_SFM" == false && ( "$MATCHER" != "exhaustive" || -n "$VOCAB_TREE" ) ]]; then
    fail "Matcher options require a new global SfM run"
fi
if [[ "$RUN_SFM" == true && "$SFM_TOOL" != "global" && "$MATCHER" != "exhaustive" ]]; then
    fail "--matcher applies only to --sfm global"
fi
if [[ "$MATCHER" == "vocab_tree" ]]; then
    [[ -n "$VOCAB_TREE" ]] || fail "vocab_tree matching requires --vocab-tree PATH"
    [[ -f "$VOCAB_TREE" ]] || fail "Vocabulary tree does not exist: $VOCAB_TREE"
    VOCAB_TREE="$(cd "$(dirname "$VOCAB_TREE")" && pwd)/$(basename "$VOCAB_TREE")"
elif [[ -n "$VOCAB_TREE" ]]; then
    fail "--vocab-tree requires --matcher vocab_tree"
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
log "Output: $FINAL_OUTPUT_DIR"
log "SfM: $([[ "$RUN_SFM" == true ]] && printf '%s' "$SFM_TOOL" || printf 'existing COLMAP project')"
log "Trainer: OpenSplat, $ITERATIONS iterations"
log "Format: $OUTPUT_FORMAT"

if [[ "$DRY_RUN" == true ]]; then
    if [[ "$RUN_SFM" == true ]]; then
        if [[ "$SFM_TOOL" == "global" ]]; then
            global_args=("$IMAGES_PATH" "$COLMAP_PROJECT" --quality "$COLMAP_QUALITY" \
                --matcher "$MATCHER" --dry-run)
            [[ -n "$VOCAB_TREE" ]] && global_args+=(--vocab-tree "$VOCAB_TREE")
            "$SCRIPT_DIR/glomap_wrapper.sh" "${global_args[@]}"
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
        conversion=("${MELKOR_BIN:-<melkor>}" convert \
            "$OUTPUT_DIR/point_cloud.ply" "$OUTPUT_DIR/point_cloud.spz" \
            --input-profile ply:graphdeco-3dgs-v1 \
            --source-frame "$SOURCE_FRAME" \
            --source-unit-to-meter "$SOURCE_UNIT_TO_METER" \
            --source-color-space "$SOURCE_COLOR_SPACE" \
            --output-antialiased "$OUTPUT_ANTIALIASED")
        for code in "${ALLOW_LOSSES[@]}"; do conversion+=(--allow-loss "$code"); done
        print_command "${conversion[@]}"
        printf 'DRY RUN: loss report -> %q\n' "$OUTPUT_DIR/point_cloud.loss-report.json"
    fi
    exit 0
fi

STAGING_DIR="$(mktemp -d "$output_parent/.melkor-pipeline.XXXXXX")"
OUTPUT_DIR="$STAGING_DIR"
if [[ "$RUN_SFM" == true ]]; then
    COLMAP_PROJECT="$OUTPUT_DIR/workspace"
fi
if [[ "$RUN_SFM" == true ]]; then
    if [[ "$SFM_TOOL" == "global" ]]; then
        global_args=("$IMAGES_PATH" "$COLMAP_PROJECT" --quality "$COLMAP_QUALITY" --matcher "$MATCHER")
        [[ -n "$VOCAB_TREE" ]] && global_args+=(--vocab-tree "$VOCAB_TREE")
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
    conversion=("$MELKOR_BIN" convert \
        "$OUTPUT_DIR/point_cloud.ply" "$OUTPUT_DIR/point_cloud.spz" \
        --input-profile ply:graphdeco-3dgs-v1 \
        --source-frame "$SOURCE_FRAME" \
        --source-unit-to-meter "$SOURCE_UNIT_TO_METER" \
        --source-color-space "$SOURCE_COLOR_SPACE" \
        --output-antialiased "$OUTPUT_ANTIALIASED")
    for code in "${ALLOW_LOSSES[@]}"; do conversion+=(--allow-loss "$code"); done
    loss_report="$OUTPUT_DIR/point_cloud.loss-report.json"
    "${conversion[@]}" > "$loss_report"
    [[ -s "$OUTPUT_DIR/point_cloud.spz" ]] || fail "Melkor did not create the SPZ output"
    [[ -s "$loss_report" ]] || fail "Melkor did not create the loss report"
    if [[ "$OUTPUT_FORMAT" == "spz" ]]; then
        rm -- "$OUTPUT_DIR/point_cloud.ply"
    fi
fi

[[ ! -e "$FINAL_OUTPUT_DIR" && ! -L "$FINAL_OUTPUT_DIR" ]] || \
    fail "Output directory appeared during processing: $FINAL_OUTPUT_DIR"
python3 "$PROJECT_DIR/tools/atomic_publish.py" "$STAGING_DIR" "$FINAL_OUTPUT_DIR" || \
    fail "The output directory could not be published without replacement"
STAGING_DIR=""
log "Pipeline completed: $FINAL_OUTPUT_DIR"
