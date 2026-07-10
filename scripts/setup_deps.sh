#!/usr/bin/env bash

# Verify the dependency snapshots committed to third_party/. A source checkout
# is the supply-chain boundary: this script never repairs missing files by
# downloading live content from tags or branches.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

if command -v sha256sum >/dev/null 2>&1; then
    SHA=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then
    SHA=(shasum -a 256)
else
    echo "Error: sha256sum or shasum is required" >&2
    exit 1
fi

entries=(
  "third_party/tinygltf/tiny_gltf.h|b63dc0ae9e4b1ae5af288bb2b9b69ba06bc65bf9fd880795a7ba3cabe96df072"
  "third_party/tinygltf/json.hpp|c9ac7589260f36ea7016d4d51a6c95809803298c7caec9f55830a0214c5f9140"
  "third_party/stb/stb_image.h|594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3"
  "third_party/spz/CMakeLists.txt|697e3674703a8468d1757926dfa4125a7d3072d5dea5fbc0e446155da1648e17"
  "third_party/spz/LICENSE|d2b852220d1043148f523c4154e8bf7d9c5e419aede33b4c94115cfc457e4714"
  "third_party/spz/src/cc/load-spz.cc|a7a603564df969bd88dd30433216637a3854d68c03c83f6a2c9852ddaa98d4b4"
  "third_party/spz/src/cc/load-spz.h|eaf7c72e209ea481bcdd7367e50add9e2883484ec94bd6ee5c8017e006e06d0b"
  "third_party/spz/src/cc/splat-c-types.cc|5a82cfc6d7667f3393b6c65086bc7d94337f751689d2cef38928a928a6627783"
  "third_party/spz/src/cc/splat-c-types.h|0e4a6301339f7c6de55324bc6deb83484de0873b55fdc9be60cf0b292ce0f007"
  "third_party/spz/src/cc/splat-types.cc|529c92e9980291ea5f2433e6086ff9a5130d8d24b5af572829e8cfa30e96e4a4"
  "third_party/spz/src/cc/splat-types.h|93a01e5e4edb6709ad001a9e688003c846c97b3e4631a176143779744299e2d8"
)

echo "Verifying vendored dependency snapshots..."
for entry in "${entries[@]}"; do
    path="${entry%%|*}"
    expected="${entry##*|}"
    if [ ! -f "$path" ]; then
        echo "Error: missing $path; restore it from the reviewed source checkout" >&2
        exit 1
    fi
    actual="$("${SHA[@]}" "$path" | awk '{print $1}')"
    if [ "$actual" != "$expected" ]; then
        echo "Error: digest mismatch for $path" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        echo "Review an upstream update and change source + digest together." >&2
        exit 1
    fi
    echo "ok  $path"
done

if [[ "$(uname)" == "Darwin" ]]; then
    NUM_CORES="$(sysctl -n hw.ncpu)"
elif [[ "$(uname)" == "Linux" ]]; then
    NUM_CORES="$(nproc)"
else
    NUM_CORES=4
fi

echo "Vendored dependencies verified."
echo "Build with: cmake -B build && cmake --build build -j${NUM_CORES}"
