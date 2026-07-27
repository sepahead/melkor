#!/usr/bin/env bash
set -euo pipefail

# The file name remains for command compatibility.
# The wrapper uses COLMAP global_mapper, not the retired GLOMAP program.

VERSION="2.0.0"
INPUT_PATH=""
OUTPUT_DIR=""
MATCHER="exhaustive"
QUALITY="medium"
USE_GPU="auto"
VOCAB_TREE=""
SKIP_FEATURES=false
SKIP_MATCHING=false
DRY_RUN=false
VERBOSE=false

log() { printf '[INFO] %s\n' "$*" >&2; }
warn() { printf '[WARN] %s\n' "$*" >&2; }
fail() { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

print_usage() {
    cat <<EOF
COLMAP global mapper wrapper ${VERSION}

Usage: $0 INPUT_IMAGES OUTPUT_DIR [options]

Options:
  --matcher TYPE       exhaustive, sequential, or vocab_tree
  --vocab-tree PATH    Required data file for vocab_tree matching
  --quality LEVEL      low, medium, or high
  --gpu MODE           auto, 0, or 1
  --skip-features      Use an existing database.db
  --skip-matching      Use existing matches in database.db
  --dry-run            Print commands without changing files
  --verbose, -v        Print the selected configuration
  --help, -h           Show this help

This wrapper requires a COLMAP build that provides global_mapper.
It does not install or select a COLMAP version.
EOF
}

require_value() {
    local option="$1"
    local count="$2"
    [[ "$count" -ge 2 ]] || fail "Missing value for $option"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --matcher)
            require_value "$1" "$#"
            MATCHER="$2"
            shift 2
            ;;
        --vocab-tree)
            require_value "$1" "$#"
            VOCAB_TREE="$2"
            shift 2
            ;;
        --quality)
            require_value "$1" "$#"
            QUALITY="$2"
            shift 2
            ;;
        --gpu)
            require_value "$1" "$#"
            USE_GPU="$2"
            shift 2
            ;;
        --skip-features)
            SKIP_FEATURES=true
            shift
            ;;
        --skip-matching)
            SKIP_MATCHING=true
            shift
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --help|-h)
            print_usage
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

[[ -n "$INPUT_PATH" ]] || fail "Missing INPUT_IMAGES"
[[ -n "$OUTPUT_DIR" ]] || fail "Missing OUTPUT_DIR"
[[ -d "$INPUT_PATH" ]] || fail "Input directory does not exist: $INPUT_PATH"

case "$MATCHER" in
    exhaustive|sequential|vocab_tree) ;;
    *) fail "Invalid matcher: $MATCHER" ;;
esac
case "$QUALITY" in
    low|medium|high) ;;
    *) fail "Invalid quality: $QUALITY" ;;
esac
case "$USE_GPU" in
    auto|0|1) ;;
    *) fail "Invalid GPU mode: $USE_GPU" ;;
esac
if [[ "$MATCHER" == "vocab_tree" ]]; then
    [[ -n "$VOCAB_TREE" ]] || fail "--vocab-tree is required for vocab_tree matching"
    [[ -f "$VOCAB_TREE" ]] || fail "Vocabulary tree does not exist: $VOCAB_TREE"
fi
if [[ "$SKIP_MATCHING" == true && "$SKIP_FEATURES" == false ]]; then
    fail "--skip-matching requires --skip-features and an existing database"
fi
if [[ "$SKIP_FEATURES" == true ]]; then
    [[ -f "$INPUT_PATH/database.db" ]] || fail "Input does not contain database.db"
    [[ -d "$INPUT_PATH/images" ]] || fail "Input does not contain images/"
fi

INPUT_PATH="$(cd "$INPUT_PATH" && pwd)"
if [[ "$OUTPUT_DIR" != /* ]]; then
    OUTPUT_DIR="$PWD/$OUTPUT_DIR"
fi
[[ ! -e "$OUTPUT_DIR" ]] || fail "Output directory already exists: $OUTPUT_DIR"

GPU_FLAG="$USE_GPU"
if [[ "$GPU_FLAG" == "auto" ]]; then
    GPU_FLAG=0
    if [[ "$(uname -s)" == "Linux" ]] && command -v nvidia-smi >/dev/null 2>&1; then
        GPU_FLAG=1
    fi
fi

case "$QUALITY" in
    low) MAX_FEATURES=4096 ;;
    medium) MAX_FEATURES=8192 ;;
    high) MAX_FEATURES=16384 ;;
esac

run_command() {
    if [[ "$DRY_RUN" == true ]]; then
        printf 'DRY RUN:'
        printf ' %q' "$@"
        printf '\n'
        return
    fi
    "$@"
}

if [[ "$VERBOSE" == true ]]; then
    log "Input: $INPUT_PATH"
    log "Output: $OUTPUT_DIR"
    log "Matcher: $MATCHER"
    log "Quality: $QUALITY"
    log "GPU mode: $GPU_FLAG"
fi

if [[ "$DRY_RUN" == true ]]; then
    if [[ "$SKIP_FEATURES" == false ]]; then
        run_command colmap feature_extractor \
            --image_path "$OUTPUT_DIR/images" \
            --database_path "$OUTPUT_DIR/database.db" \
            --SiftExtraction.use_gpu "$GPU_FLAG" \
            --SiftExtraction.max_num_features "$MAX_FEATURES"
    fi
    if [[ "$SKIP_MATCHING" == false ]]; then
        matcher_args=(
            "${MATCHER}_matcher"
            --database_path "$OUTPUT_DIR/database.db"
            --SiftMatching.use_gpu "$GPU_FLAG"
        )
        if [[ "$MATCHER" == "vocab_tree" ]]; then
            matcher_args+=(--VocabTreeMatching.vocab_tree_path "$VOCAB_TREE")
        fi
        run_command colmap "${matcher_args[@]}"
    fi
    run_command colmap global_mapper \
        --database_path "$OUTPUT_DIR/database.db" \
        --image_path "$OUTPUT_DIR/images" \
        --output_path "$OUTPUT_DIR/sparse"
    exit 0
fi

command -v colmap >/dev/null 2>&1 || fail "COLMAP is not installed"
colmap global_mapper --help >/dev/null 2>&1 || \
    fail "This COLMAP build does not provide global_mapper"

mkdir -p "$OUTPUT_DIR"

if [[ "$SKIP_FEATURES" == true ]]; then
    cp "$INPUT_PATH/database.db" "$OUTPUT_DIR/database.db"
    ln -s "$INPUT_PATH/images" "$OUTPUT_DIR/images"
else
    mkdir -p "$OUTPUT_DIR/images"
    image_count=0
    while IFS= read -r -d '' image; do
        name="$(basename "$image")"
        destination="$OUTPUT_DIR/images/$name"
        [[ ! -e "$destination" ]] || fail "Duplicate output image name: $name"
        ln -s "$image" "$destination"
        image_count=$((image_count + 1))
    done < <(find "$INPUT_PATH" -maxdepth 1 -type f \
        \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' \
        -o -iname '*.tif' -o -iname '*.tiff' -o -iname '*.bmp' \) -print0)
    [[ "$image_count" -ge 3 ]] || fail "At least three supported images are required"

    log "Extracting features from $image_count images"
    run_command colmap feature_extractor \
        --image_path "$OUTPUT_DIR/images" \
        --database_path "$OUTPUT_DIR/database.db" \
        --SiftExtraction.use_gpu "$GPU_FLAG" \
        --SiftExtraction.max_num_features "$MAX_FEATURES"
fi

if [[ "$SKIP_MATCHING" == false ]]; then
    matcher_args=(
        "${MATCHER}_matcher"
        --database_path "$OUTPUT_DIR/database.db"
        --SiftMatching.use_gpu "$GPU_FLAG"
    )
    if [[ "$MATCHER" == "vocab_tree" ]]; then
        matcher_args+=(--VocabTreeMatching.vocab_tree_path "$VOCAB_TREE")
    fi
    log "Matching features with $MATCHER"
    run_command colmap "${matcher_args[@]}"
fi

mkdir -p "$OUTPUT_DIR/sparse"
log "Running COLMAP global_mapper"
run_command colmap global_mapper \
    --database_path "$OUTPUT_DIR/database.db" \
    --image_path "$OUTPUT_DIR/images" \
    --output_path "$OUTPUT_DIR/sparse"

model_dir="$OUTPUT_DIR/sparse/0"
if [[ ! -d "$model_dir" ]] && [[ -f "$OUTPUT_DIR/sparse/cameras.bin" ]]; then
    model_dir="$OUTPUT_DIR/sparse"
fi
for file in cameras.bin images.bin points3D.bin; do
    [[ -s "$model_dir/$file" ]] || fail "global_mapper did not create $file"
done

log "Global mapping completed: $model_dir"
