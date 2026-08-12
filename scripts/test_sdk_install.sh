#!/usr/bin/env bash
# Test the installed SDK from two standalone projects.
# The test also moves the prefix and tests it again.
#
# Usage:  scripts/test_sdk_install.sh [extra cmake args...]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

build="$work/build"
prefix="$work/prefix"
version="$(tr -d '\r\n' < "$repo_root/VERSION")"
consumer_find_version=""
if [[ "$version" != *-* ]]; then
    consumer_find_version="${version%%.*}"
fi

echo "== configuring and building Melkor =="
cmake -S "$repo_root" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DBUILD_TESTING=OFF \
    "$@"
cmake --build "$build" --parallel

echo "== installing the SDK to $prefix =="
cmake --install "$build"

package_version_file="$prefix/lib/cmake/Melkor/MelkorConfigVersion.cmake"
if [[ ! -f "$package_version_file" ]]; then
    package_version_file="$prefix/lib64/cmake/Melkor/MelkorConfigVersion.cmake"
fi
version_probe="$work/check-package-version.cmake"
printf '%s\n' \
    "include(\"$package_version_file\")" \
    "if(NOT PACKAGE_VERSION STREQUAL \"$version\")" \
    '  message(FATAL_ERROR "The installed package reports an incorrect version")' \
    'endif()' > "$version_probe"
cmake -P "$version_probe"

# Reject a vendored SPZ header in the installed SDK.
if find "$prefix/include" -name 'load-spz.h' -o -name 'splat-types.h' | grep -q .; then
    echo "FAIL: vendored spz headers leaked into the installed SDK include tree" >&2
    exit 1
fi
if find "$prefix" -type f \( -name 'libspz*.dylib' -o -name 'libspz*.so*' \) | grep -q .; then
    echo "FAIL: the vendored SPZ library leaked into the installed SDK" >&2
    exit 1
fi

# The supported SDK contains only the C ABI header and its version constants.
if [[ ! -f "$prefix/include/melkor/c/melkor.h" ]] ||
   [[ ! -f "$prefix/include/melkor/version.h" ]]; then
    echo "FAIL: a required C SDK header is missing" >&2
    exit 1
fi

# The package must include its machine contracts and dependency license texts.
for required in \
    share/melkor/profiles/gltf/khr-gaussian-splatting-rc-63770cc.json \
    share/melkor/profiles/ply/da3-gaussian-v1.json \
    share/melkor/profiles/ply/graphdeco-3dgs-v1.json \
    share/melkor/profiles/ply/melkor-canonical-v1.json \
    share/melkor/profiles/spz/spz-v1-v3.json \
    share/melkor/schemas/format-profile.schema.json \
    share/melkor/schemas/inspect-v1.schema.json \
    share/melkor/schemas/loss-report-v1.schema.json \
    share/melkor/licenses/spz-LICENSE \
    share/melkor/licenses/nlohmann-json-LICENSE; do
    if [[ ! -s "$prefix/$required" ]]; then
        echo "FAIL: the installed SDK is missing $required" >&2
        exit 1
    fi
done

unexpected_headers="$(find "$prefix/include/melkor" -type f \
    ! -path "$prefix/include/melkor/c/melkor.h" \
    ! -path "$prefix/include/melkor/version.h" -print)"
if [[ -n "$unexpected_headers" ]]; then
    echo "FAIL: private headers leaked into the installed SDK:" >&2
    echo "$unexpected_headers" >&2
    exit 1
fi

# The C ABI header also supports older C++ translation units.
"${CXX:-c++}" -std=c++98 -Wall -Wextra -Wpedantic -Werror -fsyntax-only \
    -I "$prefix/include" "$repo_root/tests/install/header-cpp98.cpp"
"${CC:-cc}" -std=c99 -Wall -Wextra -Wpedantic -Werror -fsyntax-only \
    -I "$prefix/include" "$repo_root/tests/install/header-version-c.c"
"${CXX:-c++}" -std=c++98 -Wall -Wextra -Wpedantic -Werror -fsyntax-only \
    -I "$prefix/include" "$repo_root/tests/install/header-version-cpp98.cpp"

check_symbols() {
    local pfx="$1"
    local library
    library="$(find "$pfx" -type f \( -name 'libmelkor*.dylib' -o -name 'libmelkor*.so*' \) \
        -print | head -n 1)"
    if [[ -z "$library" ]]; then
        echo "FAIL: the installed shared library is missing" >&2
        return 1
    fi

    local raw_symbols
    local exported_symbols
    if [[ "$(uname -s)" == "Darwin" ]]; then
        raw_symbols="$(nm -gU "$library")"
        exported_symbols="$(printf '%s\n' "$raw_symbols" | awk '{print $3}' | \
            sed 's/^_//' | sort -u)"
        if otool -L "$library" | grep -F 'libspz' >/dev/null; then
            echo "FAIL: libmelkor has a dynamic dependency on vendored SPZ" >&2
            return 1
        fi
    else
        raw_symbols="$(nm -D --defined-only "$library")"
        exported_symbols="$(printf '%s\n' "$raw_symbols" | awk '{print $3}' | \
            sed 's/@.*//' | sort -u)"
        if readelf -d "$library" | grep -F 'libspz' >/dev/null; then
            echo "FAIL: libmelkor has a dynamic dependency on vendored SPZ" >&2
            return 1
        fi
    fi

    local expected_symbols
    expected_symbols="$(printf '%s\n' \
        melkor_get_version \
        melkor_inspect_ply_file \
        melkor_status_string | sort)"
    if [[ "$exported_symbols" != "$expected_symbols" ]]; then
        echo "FAIL: the installed exported symbol set is incorrect" >&2
        echo "Expected:" >&2
        echo "$expected_symbols" >&2
        echo "Found:" >&2
        echo "$exported_symbols" >&2
        return 1
    fi
}

check_symbols "$prefix"

if [[ "$version" == *-* ]]; then
    prerelease_check="$work/prerelease-version-check"
    if cmake -S "$repo_root/tests/install/consumer-c" -B "$prerelease_check" \
        -DCMAKE_PREFIX_PATH="$prefix" -DMELKOR_CONSUMER_FIND_VERSION="${version%%.*}" \
        >"$work/prerelease-version-check.log" 2>&1; then
        echo "FAIL: a prerelease SDK satisfied a stable version request" >&2
        exit 1
    fi
fi

consume() {
    local lang="$1" src="$2" pfx="$3"
    local cbuild="$work/consumer-$lang"
    echo "== consuming the SDK from a standalone $lang project =="
    if [[ -n "$consumer_find_version" ]]; then
        cmake -S "$src" -B "$cbuild" -DCMAKE_PREFIX_PATH="$pfx" \
            -DMELKOR_CONSUMER_FIND_VERSION="$consumer_find_version"
    else
        cmake -S "$src" -B "$cbuild" -DCMAKE_PREFIX_PATH="$pfx"
    fi
    cmake --build "$cbuild" --parallel
    ctest --test-dir "$cbuild" --output-on-failure
}

consume c "$repo_root/tests/install/consumer-c" "$prefix"
consume cpp "$repo_root/tests/install/consumer-cpp" "$prefix"

echo "== relocating the install and consuming again =="
moved="$work/prefix-moved"
mv "$prefix" "$moved"
check_symbols "$moved"
reloc="$work/consumer-c-reloc"
if [[ -n "$consumer_find_version" ]]; then
    cmake -S "$repo_root/tests/install/consumer-c" -B "$reloc" -DCMAKE_PREFIX_PATH="$moved" \
        -DMELKOR_CONSUMER_FIND_VERSION="$consumer_find_version"
else
    cmake -S "$repo_root/tests/install/consumer-c" -B "$reloc" -DCMAKE_PREFIX_PATH="$moved"
fi
cmake --build "$reloc" --parallel
ctest --test-dir "$reloc" --output-on-failure

echo "PASS: the installed SDK is found, linked, run, and relocatable."
