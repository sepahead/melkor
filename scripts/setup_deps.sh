#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
THIRD_PARTY_DIR="$PROJECT_DIR/third_party"

# Pinned refs for reproducible fetches. Override with the matching env var to
# pick up a newer release, but never fetch from a moving branch by default --
# that makes builds non-reproducible and can silently break the vendored API.
TINYGLTF_REF="${MELKOR_TINYGLTF_REF:-v2.9.4}"
STB_REF="${MELKOR_STB_REF:-57e75b9}"
SPZ_REF="${MELKOR_SPZ_REF:-v0.3.0}"

echo "Setting up dependencies in $THIRD_PARTY_DIR"
echo "  tinygltf ref: $TINYGLTF_REF"
echo "  stb ref:      $STB_REF"
echo "  spz ref:      $SPZ_REF"

mkdir -p "$THIRD_PARTY_DIR"

# tinygltf (header-only). The repo vendors a known-good snapshot; we only
# re-fetch if the header is genuinely absent, so a normal checkout is left
# untouched and builds remain reproducible.
TINYGLTF_DIR="$THIRD_PARTY_DIR/tinygltf"
if [ ! -f "$TINYGLTF_DIR/tiny_gltf.h" ]; then
    echo "Downloading tinygltf @ $TINYGLTF_REF..."
    mkdir -p "$TINYGLTF_DIR"
    curl -fsSL "https://raw.githubusercontent.com/syoyo/tinygltf/${TINYGLTF_REF}/tiny_gltf.h" -o "$TINYGLTF_DIR/tiny_gltf.h"
    curl -fsSL "https://raw.githubusercontent.com/syoyo/tinygltf/${TINYGLTF_REF}/json.hpp" -o "$TINYGLTF_DIR/json.hpp"
    echo "tinygltf downloaded."
else
    echo "tinygltf already vendored (leaving untouched)."
fi

# stb_image (header-only, required by tinygltf). Same rule: don't clobber.
STB_DIR="$THIRD_PARTY_DIR/stb"
if [ ! -f "$STB_DIR/stb_image.h" ]; then
    echo "Downloading stb_image @ $STB_REF..."
    mkdir -p "$STB_DIR"
    curl -fsSL "https://raw.githubusercontent.com/nothings/stb/${STB_REF}/stb_image.h" -o "$STB_DIR/stb_image.h"
    echo "stb_image downloaded."
else
    echo "stb_image already vendored (leaving untouched)."
fi

# Clone nianticlabs/spz (optional, for SPZ support). Pinned ref for the same
# reproducibility reason; the repo already ships a snapshot.
SPZ_DIR="$THIRD_PARTY_DIR/spz"
if [ ! -d "$SPZ_DIR" ]; then
    echo "Cloning nianticlabs/spz @ $SPZ_REF..."
    git clone --depth 1 --branch "$SPZ_REF" https://github.com/nianticlabs/spz.git "$SPZ_DIR" 2>/dev/null || {
        echo "Warning: Could not clone spz repository at ref '$SPZ_REF'."
        echo "SPZ support will be disabled. The repo ships a vendored snapshot;"
        echo "restore third_party/spz from git instead of re-fetching."
    }
else
    echo "spz already vendored (leaving untouched)."
fi

# Detect number of CPU cores (cross-platform)
if [[ "$(uname)" == "Darwin" ]]; then
    NUM_CORES=$(sysctl -n hw.ncpu)
elif [[ "$(uname)" == "Linux" ]]; then
    NUM_CORES=$(nproc)
else
    NUM_CORES=4  # Fallback
fi

echo ""
echo "Dependencies setup complete!"
echo ""
echo "To build:"
echo "  mkdir build && cd build"
echo "  cmake .."
echo "  make -j${NUM_CORES}"
