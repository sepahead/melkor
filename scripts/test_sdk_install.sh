#!/usr/bin/env bash
# Clean-room SDK install-and-consume test.
#
# Proves release blocker P0-04 is closed: that `cmake --install` produces a package a separate
# project can find with find_package(Melkor) and link against. It configures and builds Melkor,
# installs it to a throwaway prefix, then configures TWO standalone consumer projects (one C,
# one C++) against that prefix -- exactly as a downstream user would -- builds them, and runs
# them. It also moves the install and reconfigures, so relocation is covered.
#
# A consumer that built inside the main tree would prove nothing, because it would see the
# source headers directly. This deliberately does not.
#
# Usage:  scripts/test_sdk_install.sh [extra cmake args...]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

build="$work/build"
prefix="$work/prefix"

echo "== configuring and building Melkor =="
cmake -S "$repo_root" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    "$@"
cmake --build "$build" --parallel

echo "== installing the SDK to $prefix =="
cmake --install "$build"

# The SDK must not leak a vendored dependency's surface into the install.
if find "$prefix/include" -name 'load-spz.h' -o -name 'splat-types.h' | grep -q .; then
    echo "FAIL: vendored spz headers leaked into the installed SDK include tree" >&2
    exit 1
fi

consume() {
    local lang="$1" src="$2" pfx="$3"
    local cbuild="$work/consumer-$lang"
    echo "== consuming the SDK from a standalone $lang project =="
    cmake -S "$src" -B "$cbuild" -DCMAKE_PREFIX_PATH="$pfx"
    cmake --build "$cbuild" --parallel
    ctest --test-dir "$cbuild" --output-on-failure
}

consume c "$repo_root/tests/install/consumer-c" "$prefix"
consume cpp "$repo_root/tests/install/consumer-cpp" "$prefix"

echo "== relocating the install and consuming again =="
moved="$work/prefix-moved"
mv "$prefix" "$moved"
reloc="$work/consumer-c-reloc"
cmake -S "$repo_root/tests/install/consumer-c" -B "$reloc" -DCMAKE_PREFIX_PATH="$moved"
cmake --build "$reloc" --parallel
ctest --test-dir "$reloc" --output-on-failure

echo "PASS: the installed SDK is found, linked, run, and relocatable."
