#!/usr/bin/env bash
set -euo pipefail

# Run one user-supplied OpenSplat binary against a validated COLMAP project.
# This wrapper does not install OpenSplat or simulate unsupported multi-GPU modes.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
[[ -f "$REPO_DIR/VERSION" ]] || { printf '[ERROR] VERSION is missing\n' >&2; exit 2; }
VERSION="$(<"$REPO_DIR/VERSION")"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$ ]] || {
    printf '[ERROR] VERSION is invalid\n' >&2
    exit 2
}
PROJECT_DIR=""
IMAGES_DIR=""
OUTPUT_FILE="output.ply"
ITERATIONS="30000"
GPU_ID=""
OPENSPLAT_BIN="${MELKOR_OPENSPLAT_BIN:-}"
DRY_RUN=false
VERBOSE=false
FORCE=false
EXTRA_ARGS=()
HAS_EXTRA_ARGS=false
TEMP_DIR=""
TEMP_OUTPUT_DIR=""

log() { printf '[INFO] %s\n' "$*" >&2; }
fail() { printf '[ERROR] %s\n' "$*" >&2; exit 2; }

cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "$TEMP_DIR" && -d "$TEMP_DIR" ]]; then
        rm -rf -- "$TEMP_DIR"
    fi
    if [[ -n "$TEMP_OUTPUT_DIR" && -d "$TEMP_OUTPUT_DIR" ]]; then
        rm -rf -- "$TEMP_OUTPUT_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

usage() {
    cat <<EOF
OpenSplat wrapper ${VERSION}

Usage: $0 COLMAP_PROJECT [options] [-- OPENSPLAT_ARGS...]

Options:
  --images PATH         Use images from PATH in a temporary workspace
  -o, --output PATH     Write the PLY file to PATH (default: output.ply)
  -n, --iterations N    Run N training iterations (default: 30000)
  --gpu ID              Set CUDA_VISIBLE_DEVICES for this process
  --opensplat PATH      Use this OpenSplat executable
  --force               Permit replacement of an existing output file
  --dry-run             Print the command without changing files
  --verbose, -v         Print the selected paths
  --help, -h            Show this help

The wrapper supports one OpenSplat process. It rejects the retired simulated
multi-GPU options. Pass version-specific OpenSplat options after --.
EOF
}

need_value() {
    local option="$1"
    local count="$2"
    [[ "$count" -ge 2 ]] || fail "Missing value for $option"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --images)
            need_value "$1" "$#"
            IMAGES_DIR="$2"
            shift 2
            ;;
        -o|--output)
            need_value "$1" "$#"
            OUTPUT_FILE="$2"
            shift 2
            ;;
        -n|--iterations)
            need_value "$1" "$#"
            ITERATIONS="$2"
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
        --force)
            FORCE=true
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
        --gpu-ids|--split|--downscale|--densify-grad|--densify-size|--densify-interval|--stop-densify|--save-every)
            fail "$1 is not a verified wrapper option. Pass a supported upstream option after --."
            ;;
        --)
            shift
            if [[ $# -gt 0 ]]; then
                EXTRA_ARGS=("$@")
                HAS_EXTRA_ARGS=true
            fi
            break
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        -*)
            fail "Unknown option: $1"
            ;;
        *)
            if [[ -z "$PROJECT_DIR" ]]; then
                PROJECT_DIR="$1"
            else
                fail "Unexpected argument: $1"
            fi
            shift
            ;;
    esac
done

if [[ "$HAS_EXTRA_ARGS" == true ]]; then
    for argument in "${EXTRA_ARGS[@]}"; do
        case "$argument" in
            -o|-o?*|--output|--output=*)
                fail "Extra arguments must not replace the staged output path"
                ;;
        esac
    done
fi

[[ -n "$PROJECT_DIR" ]] || fail "Missing COLMAP_PROJECT"
[[ -d "$PROJECT_DIR" ]] || fail "COLMAP project does not exist: $PROJECT_DIR"
[[ "$ITERATIONS" =~ ^[1-9][0-9]*$ ]] || fail "Iterations must be a positive integer"
[[ -z "$GPU_ID" || "$GPU_ID" =~ ^[0-9]+$ ]] || fail "GPU ID must be a nonnegative integer"

PROJECT_DIR="$(cd "$PROJECT_DIR" && pwd)"
if [[ -n "$IMAGES_DIR" ]]; then
    [[ -d "$IMAGES_DIR" ]] || fail "Image directory does not exist: $IMAGES_DIR"
    IMAGES_DIR="$(cd "$IMAGES_DIR" && pwd)"
elif [[ ! -d "$PROJECT_DIR/images" ]]; then
    fail "The COLMAP project does not contain images/. Use --images PATH."
fi

MODEL_DIR="$PROJECT_DIR/sparse/0"
if [[ ! -d "$MODEL_DIR" ]]; then
    MODEL_DIR="$PROJECT_DIR/sparse"
fi
for file in cameras.bin images.bin points3D.bin; do
    [[ -s "$MODEL_DIR/$file" ]] || fail "The COLMAP model does not contain $file"
done

if [[ -z "$OPENSPLAT_BIN" ]]; then
    for candidate in "$(dirname "$(dirname "${BASH_SOURCE[0]}")")/opensplat" "$(command -v opensplat 2>/dev/null || true)"; do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            OPENSPLAT_BIN="$candidate"
            break
        fi
    done
fi
[[ -n "$OPENSPLAT_BIN" && -x "$OPENSPLAT_BIN" ]] || \
    fail "OpenSplat is unavailable. Set MELKOR_OPENSPLAT_BIN or use --opensplat PATH."
OPENSPLAT_BIN="$(cd "$(dirname "$OPENSPLAT_BIN")" && pwd)/$(basename "$OPENSPLAT_BIN")"

if [[ -d "$OUTPUT_FILE" ]]; then
    fail "The output path is a directory: $OUTPUT_FILE"
fi
if [[ (-e "$OUTPUT_FILE" || -L "$OUTPUT_FILE") && ! -f "$OUTPUT_FILE" && ! -L "$OUTPUT_FILE" ]]; then
    fail "The output path is not a regular file: $OUTPUT_FILE"
fi
if [[ (-e "$OUTPUT_FILE" || -L "$OUTPUT_FILE") && "$FORCE" == false ]]; then
    fail "Output already exists. Use --force to permit replacement: $OUTPUT_FILE"
fi

WORK_DIR="$PROJECT_DIR"
if [[ -n "$IMAGES_DIR" ]]; then
    if [[ "$DRY_RUN" == true ]]; then
        WORK_DIR="<temporary-colmap-project>"
    else
        TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/melkor-opensplat.XXXXXX")"
        mkdir -p "$TEMP_DIR/sparse/0" "$TEMP_DIR/images"
        cp -a "$MODEL_DIR/." "$TEMP_DIR/sparse/0/"
        if [[ -f "$PROJECT_DIR/database.db" ]]; then
            cp "$PROJECT_DIR/database.db" "$TEMP_DIR/database.db"
        fi

        image_count=0
        while IFS= read -r -d '' image; do
            name="$(basename "$image")"
            [[ ! -e "$TEMP_DIR/images/$name" ]] || fail "Duplicate image name: $name"
            ln -s "$image" "$TEMP_DIR/images/$name"
            image_count=$((image_count + 1))
        done < <(find "$IMAGES_DIR" -maxdepth 1 -type f \
            \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' \
            -o -iname '*.tif' -o -iname '*.tiff' -o -iname '*.bmp' \) -print0)
        [[ "$image_count" -gt 0 ]] || fail "The image directory has no supported images"
        WORK_DIR="$TEMP_DIR"
    fi
fi

if [[ "$VERBOSE" == true ]]; then
    log "COLMAP project: $PROJECT_DIR"
    [[ -n "$IMAGES_DIR" ]] && log "Image source: $IMAGES_DIR"
    log "Output: $OUTPUT_FILE"
    log "OpenSplat: $OPENSPLAT_BIN"
fi

if [[ "$DRY_RUN" == true ]]; then
    command=("$OPENSPLAT_BIN" "$WORK_DIR" -n "$ITERATIONS" -o "<temporary-output>.ply")
    printf 'DRY RUN:'
    if [[ -n "$GPU_ID" ]]; then
        printf ' CUDA_VISIBLE_DEVICES=%q' "$GPU_ID"
    fi
    printf ' %q' "${command[@]}"
    if [[ "$HAS_EXTRA_ARGS" == true ]]; then
        printf ' -- <%d upstream arguments withheld>' "${#EXTRA_ARGS[@]}"
    fi
    printf '\n'
    exit 0
fi

output_parent="$(dirname "$OUTPUT_FILE")"
mkdir -p "$output_parent"
output_parent="$(cd "$output_parent" && pwd)"
output_name="$(basename "$OUTPUT_FILE")"
[[ "$output_name" != "." && "$output_name" != ".." ]] || fail "Invalid output filename"
case "$output_name" in
    *.[Pp][Ll][Yy]) ;;
    *) fail "The output filename must end in .ply" ;;
esac
OUTPUT_FILE="$output_parent/$output_name"
case "$OUTPUT_FILE" in
    "$PROJECT_DIR"|"$PROJECT_DIR"/*)
        fail "The output file must stay outside the COLMAP project"
        ;;
esac
if [[ -n "$IMAGES_DIR" ]]; then
    case "$OUTPUT_FILE" in
        "$IMAGES_DIR"|"$IMAGES_DIR"/*)
            fail "The output file must stay outside the image directory"
            ;;
    esac
fi
TEMP_OUTPUT_DIR="$(mktemp -d "$output_parent/.melkor-opensplat.XXXXXX")"
staged_output="$TEMP_OUTPUT_DIR/result.ply"
command=("$OPENSPLAT_BIN" "$WORK_DIR" -n "$ITERATIONS" -o "$staged_output")
if [[ "$HAS_EXTRA_ARGS" == true ]]; then
    command+=("${EXTRA_ARGS[@]}")
fi
if [[ -n "$GPU_ID" ]]; then
    export CUDA_VISIBLE_DEVICES="$GPU_ID"
fi
"${command[@]}"
[[ -f "$staged_output" && ! -L "$staged_output" && -s "$staged_output" ]] || \
    fail "OpenSplat did not create a nonempty regular output file"
if [[ -d "$OUTPUT_FILE" ]] || \
   [[ (-e "$OUTPUT_FILE" || -L "$OUTPUT_FILE") && ! -f "$OUTPUT_FILE" && ! -L "$OUTPUT_FILE" ]]; then
    fail "The output path changed to an unsupported object during processing"
fi
if [[ (-e "$OUTPUT_FILE" || -L "$OUTPUT_FILE") && "$FORCE" == false ]]; then
    fail "Output appeared during processing: $OUTPUT_FILE"
fi
publish_args=("$staged_output" "$OUTPUT_FILE")
[[ "$FORCE" == true ]] && publish_args+=(--replace)
python3 "$REPO_DIR/tools/atomic_publish.py" "${publish_args[@]}" || \
    fail "The output file could not be published atomically"
log "OpenSplat completed: $OUTPUT_FILE"
