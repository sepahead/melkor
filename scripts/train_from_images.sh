#!/usr/bin/env bash
set -euo pipefail

# Keep the old entry point as a narrow compatibility forwarder.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -lt 2 ]]; then
    printf 'Usage: %s IMAGE_DIR OUTPUT_DIR [--quality fast|medium|high]\n' "$0" >&2
    exit 2
fi

INPUT_PATH="$1"
OUTPUT_DIR="$2"
shift 2

ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tool)
            [[ $# -ge 2 ]] || { printf 'Error: missing value for --tool\n' >&2; exit 2; }
            [[ "$2" == "opensplat" ]] || {
                printf 'Error: only the OpenSplat pipeline contract is available\n' >&2
                exit 2
            }
            shift 2
            ;;
        --quality)
            [[ $# -ge 2 ]] || { printf 'Error: missing value for --quality\n' >&2; exit 2; }
            ARGS+=(--quality "$2")
            shift 2
            ;;
        --help|-h)
            printf 'Usage: %s IMAGE_DIR OUTPUT_DIR [--quality fast|medium|high]\n' "$0"
            exit 0
            ;;
        *)
            printf 'Error: unknown option: %s\n' "$1" >&2
            exit 2
            ;;
    esac
done

exec "$SCRIPT_DIR/pipeline.sh" "$INPUT_PATH" "$OUTPUT_DIR" "${ARGS[@]}"
