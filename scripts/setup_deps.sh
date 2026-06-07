#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
THIRD_PARTY_DIR="$PROJECT_DIR/third_party"

echo "Setting up dependencies in $THIRD_PARTY_DIR"

mkdir -p "$THIRD_PARTY_DIR"

# Download tinygltf (header-only)
TINYGLTF_DIR="$THIRD_PARTY_DIR/tinygltf"
if [ ! -f "$TINYGLTF_DIR/tiny_gltf.h" ]; then
    echo "Downloading tinygltf..."
    mkdir -p "$TINYGLTF_DIR"
    curl -sL "https://raw.githubusercontent.com/syoyo/tinygltf/release/tiny_gltf.h" -o "$TINYGLTF_DIR/tiny_gltf.h"
    curl -sL "https://raw.githubusercontent.com/syoyo/tinygltf/release/json.hpp" -o "$TINYGLTF_DIR/json.hpp"
    echo "tinygltf downloaded."
else
    echo "tinygltf already exists."
fi

# Download stb_image (header-only, required by tinygltf)
STB_DIR="$THIRD_PARTY_DIR/stb"
if [ ! -f "$STB_DIR/stb_image.h" ]; then
    echo "Downloading stb_image..."
    mkdir -p "$STB_DIR"
    curl -sL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" -o "$STB_DIR/stb_image.h"
    echo "stb_image downloaded."
else
    echo "stb_image already exists."
fi

# Clone nianticlabs/spz (optional, for SPZ support)
SPZ_DIR="$THIRD_PARTY_DIR/spz"
if [ ! -d "$SPZ_DIR" ]; then
    echo "Cloning nianticlabs/spz..."
    git clone --depth 1 https://github.com/nianticlabs/spz.git "$SPZ_DIR" 2>/dev/null || {
        echo "Warning: Could not clone spz repository."
        echo "SPZ support will be disabled."
        echo "You can manually clone it later: git clone https://github.com/nianticlabs/spz.git $SPZ_DIR"
    }
else
    echo "spz already exists."
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
