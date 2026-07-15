#!/usr/bin/env bash
set -euo pipefail

bin="${1:?melkor executable path required}"

expect_fail() {
    local expected="$1"
    shift
    local output
    if output="$("$bin" "$@" 2>&1)"; then
        echo "FAIL: command unexpectedly succeeded: $*" >&2
        exit 1
    fi
    if [[ "$output" != *"$expected"* ]]; then
        echo "FAIL: expected '$expected' from: $*" >&2
        echo "$output" >&2
        exit 1
    fi
}

expect_fail "Invalid finite number" input.glb output.ply --opacity nan
expect_fail "Invalid finite number" input.glb output.ply --scale 1oops
expect_fail "Invalid integer" input.glb output.ply --knn 4oops
expect_fail "Missing value" input.glb output.ply --opacity
expect_fail "Unknown option" input.glb output.ply --opactiy 0.5
expect_fail "Unexpected extra positional" input.glb output.ply extra.ply
expect_fail "Conflicting conversion mode" input.glb output.ply --basic --enhanced
expect_fail "previous implementation did not" --fit
expect_fail "Native --feedforward has been retired" --feedforward

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/melkor-cli-validation.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT
nan_ply="$tmp_dir/nonfinite-position.ply"
nan_output="$tmp_dir/nonfinite-position.spz"
printf '%s\n' \
    'ply' \
    'format ascii 1.0' \
    'element vertex 1' \
    'property float x' \
    'property float y' \
    'property float z' \
    'property float f_dc_0' \
    'property float f_dc_1' \
    'property float f_dc_2' \
    'property float opacity' \
    'property float scale_0' \
    'property float scale_1' \
    'property float scale_2' \
    'property float rot_0' \
    'property float rot_1' \
    'property float rot_2' \
    'property float rot_3' \
    'end_header' \
    'nan 0 0 0 0 0 0 -2 -2 -2 1 0 0 0' > "$nan_ply"
expect_fail "MK1504_NONFINITE_POSITION" "$nan_ply" "$nan_output" --no-gpu
if [[ -e "$nan_output" ]]; then
    echo "FAIL: rejected non-finite input left an output artifact" >&2
    exit 1
fi

default_gltf="$tmp_dir/default-opacity.gltf"
default_spz="$tmp_dir/default-opacity.spz"
default_ply="$tmp_dir/default-opacity-roundtrip.ply"
printf '%s\n' \
    '{"asset":{"version":"2.0"},"buffers":[{"uri":"data:application/octet-stream;base64,AACAPwAAAEAAAEBA","byteLength":12}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":12}],"accessors":[{"bufferView":0,"componentType":5126,"count":1,"type":"VEC3"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0}' > "$default_gltf"
"$bin" "$default_gltf" "$default_spz" --no-gpu >/dev/null
default_report="$("$bin" inspect "$default_spz" --json)"
if [[ "$default_report" != *'"valid":true'* ]]; then
    echo "FAIL: default GLTF opacity did not survive SPZ inspection" >&2
    echo "$default_report" >&2
    exit 1
fi
"$bin" "$default_spz" "$default_ply" --no-gpu >/dev/null
if [[ ! -s "$default_ply" ]]; then
    echo "FAIL: default-opacity GLTF -> SPZ -> PLY roundtrip produced no output" >&2
    exit 1
fi

models="$("$bin" --list-models)"
[[ "$models" == *"da3-base"* ]]
[[ "$models" == *"noncommercial"* ]]

echo "CLI validation tests passed"
