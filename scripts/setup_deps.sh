#!/usr/bin/env bash

# Verify the dependency snapshots committed to third_party/.
# A source checkout is the supply-chain boundary.
# This script does not download or repair dependencies.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: python3 is required" >&2
    exit 1
fi

python3 tools/verify_third_party.py --check

if [[ "$(uname)" == "Darwin" ]]; then
    NUM_CORES="$(sysctl -n hw.ncpu)"
elif [[ "$(uname)" == "Linux" ]]; then
    NUM_CORES="$(nproc)"
else
    NUM_CORES=4
fi

echo "Vendored dependencies verified."
echo "Build with: cmake -B build && cmake --build build -j${NUM_CORES}"
