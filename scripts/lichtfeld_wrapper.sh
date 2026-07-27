#!/usr/bin/env bash
set -euo pipefail

# Run a user-supplied LichtFeld-Studio binary without inventing upstream flags.

VERSION="2.0.0"
PROJECT_DIR=""
IMAGES_DIR=""
OUTPUT_DIR="output"
GPU_ID=""
LICHTFELD_BIN="${MELKOR_LICHTFELD_BIN:-}"
DRY_RUN=false
VERBOSE=false
FORCE=false
EXTRA_ARGS=()
HAS_EXTRA_ARGS=false
TEMP_DIR=""
TEMP_OUTPUT_DIR=""
BACKUP_DIR=""

log() { printf '[INFO] %s\n' "$*" >&2; }
fail() { printf '[ERROR] %s\n' "$*" >&2; exit 2; }

cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "$TEMP_DIR" && -d "$TEMP_DIR" ]]; then
        rm -rf -- "$TEMP_DIR"
    fi
    if [[ -n "$BACKUP_DIR" && -d "$BACKUP_DIR" ]]; then
        if [[ ! -e "$OUTPUT_DIR" && -e "$BACKUP_DIR/original" ]]; then
            mv -- "$BACKUP_DIR/original" "$OUTPUT_DIR"
        fi
        rm -rf -- "$BACKUP_DIR"
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
LichtFeld-Studio wrapper ${VERSION}

Usage: $0 COLMAP_PROJECT [options] [-- LICHTFELD_ARGS...]

Options:
  --images PATH         Use images from PATH in a temporary workspace
  -o, --output PATH     Use PATH as the output directory (default: output)
  --gpu ID              Set CUDA_VISIBLE_DEVICES for this process
  --lichtfeld PATH      Use this LichtFeld-Studio executable
  --force               Permit use of an existing output directory
  --dry-run             Print the command without changing files
  --verbose, -v         Print the selected paths
  --help, -h            Show this help

Pass version-specific LichtFeld-Studio options after --. The wrapper does not
translate options because Melkor does not pin an upstream command contract.
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
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --gpu)
            need_value "$1" "$#"
            GPU_ID="$2"
            shift 2
            ;;
        --lichtfeld)
            need_value "$1" "$#"
            LICHTFELD_BIN="$2"
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
        -n|--iterations|--pose-opt|--no-mcmc|--eval|--gui)
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

[[ -n "$PROJECT_DIR" ]] || fail "Missing COLMAP_PROJECT"
[[ -d "$PROJECT_DIR" ]] || fail "COLMAP project does not exist: $PROJECT_DIR"
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

if [[ -z "$LICHTFELD_BIN" ]]; then
    for candidate in "$(dirname "$(dirname "${BASH_SOURCE[0]}")")/lichtfeld" \
        "$(command -v lichtfeld 2>/dev/null || true)" \
        "$(command -v LichtFeld-Studio 2>/dev/null || true)"; do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            LICHTFELD_BIN="$candidate"
            break
        fi
    done
fi
[[ -n "$LICHTFELD_BIN" && -x "$LICHTFELD_BIN" ]] || \
    fail "LichtFeld-Studio is unavailable. Set MELKOR_LICHTFELD_BIN or use --lichtfeld PATH."
LICHTFELD_BIN="$(cd "$(dirname "$LICHTFELD_BIN")" && pwd)/$(basename "$LICHTFELD_BIN")"

if [[ -e "$OUTPUT_DIR" && "$FORCE" == false ]]; then
    fail "Output directory already exists. Use --force to permit its use: $OUTPUT_DIR"
fi

WORK_DIR="$PROJECT_DIR"
if [[ -n "$IMAGES_DIR" ]]; then
    if [[ "$DRY_RUN" == true ]]; then
        WORK_DIR="<temporary-colmap-project>"
    else
        TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/melkor-lichtfeld.XXXXXX")"
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
    log "Output: $OUTPUT_DIR"
    log "LichtFeld-Studio: $LICHTFELD_BIN"
fi

if [[ "$DRY_RUN" == true ]]; then
    command=("$LICHTFELD_BIN" -d "$WORK_DIR" -o "<temporary-output-directory>")
    if [[ "$HAS_EXTRA_ARGS" == true ]]; then
        command+=("${EXTRA_ARGS[@]}")
    fi
    printf 'DRY RUN:'
    if [[ -n "$GPU_ID" ]]; then
        printf ' CUDA_VISIBLE_DEVICES=%q' "$GPU_ID"
    fi
    printf ' %q' "${command[@]}"
    printf '\n'
    exit 0
fi

output_parent="$(dirname "$OUTPUT_DIR")"
mkdir -p "$output_parent"
output_parent="$(cd "$output_parent" && pwd)"
output_name="$(basename "$OUTPUT_DIR")"
OUTPUT_DIR="$output_parent/$output_name"
TEMP_OUTPUT_DIR="$(mktemp -d "$output_parent/.melkor-lichtfeld.XXXXXX")"
command=("$LICHTFELD_BIN" -d "$WORK_DIR" -o "$TEMP_OUTPUT_DIR")
if [[ "$HAS_EXTRA_ARGS" == true ]]; then
    command+=("${EXTRA_ARGS[@]}")
fi
if [[ -n "$GPU_ID" ]]; then
    export CUDA_VISIBLE_DEVICES="$GPU_ID"
fi
"${command[@]}"
find "$TEMP_OUTPUT_DIR" -mindepth 1 -print -quit | grep -q . || \
    fail "LichtFeld-Studio did not create output files"
if [[ -e "$OUTPUT_DIR" ]]; then
    BACKUP_DIR="$(mktemp -d "$output_parent/.melkor-lichtfeld-backup.XXXXXX")"
    mv -- "$OUTPUT_DIR" "$BACKUP_DIR/original"
fi
mv -- "$TEMP_OUTPUT_DIR" "$OUTPUT_DIR"
TEMP_OUTPUT_DIR=""
if [[ -n "$BACKUP_DIR" ]]; then
    rm -rf -- "$BACKUP_DIR"
    BACKUP_DIR=""
fi
log "LichtFeld-Studio completed: $OUTPUT_DIR"
