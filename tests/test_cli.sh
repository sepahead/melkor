#!/usr/bin/env bash
set -euo pipefail

bin="${1:?melkor executable path required}"
spz_enabled="${2:-ON}"
if [[ "$spz_enabled" != "ON" && "$spz_enabled" != "OFF" ]]; then
    echo "FAIL: SPZ feature state must be ON or OFF" >&2
    exit 1
fi

expect_exit() {
    local expected_status="$1"
    local expected_text="$2"
    shift 2
    local output
    local status
    set +e
    output="$("$bin" "$@" 2>&1)"
    status=$?
    set -e
    if [[ "$status" -ne "$expected_status" ]]; then
        echo "FAIL: expected exit $expected_status, received $status: $*" >&2
        echo "$output" >&2
        exit 1
    fi
    if [[ "$output" != *"$expected_text"* ]]; then
        echo "FAIL: expected '$expected_text': $*" >&2
        echo "$output" >&2
        exit 1
    fi
}

"$bin" --version | grep -Eq '^melkor [0-9]+\.[0-9]+\.[0-9]+'
"$bin" --help | grep -F 'convert INPUT OUTPUT' >/dev/null
"$bin" convert --help | grep -F 'spz:spz-v1-v3' >/dev/null
"$bin" inspect --help | grep -F 'melkor.inspect.v1' >/dev/null

expect_exit 2 'MK1802_USAGE'
expect_exit 2 'unknown command' --info
expect_exit 2 'unknown command' input.glb output.ply
expect_exit 2 'help command accepts no arguments' --help extra
expect_exit 2 'version command accepts no arguments' --version extra

expect_exit 2 'convert requires one input and one output' convert input.glb
expect_exit 2 'unknown option' convert input.glb output.glb --opactiy 0.5
expect_exit 2 'requires a value' convert input.glb output.glb --input-profile
expect_exit 2 'option specified more than once' \
    convert input.glb output.glb --ascii --ascii
expect_exit 2 'positive finite value' \
    convert input.spz output.ply --source-unit-to-meter nan
expect_exit 2 'positive finite value' \
    convert input.spz output.ply --source-unit-to-meter ' 1'
expect_exit 2 'must be true or false' \
    convert input.glb output.spz --output-antialiased yes
expect_exit 2 'must be from 0 through 4' \
    convert input.glb output.ply --max-sh-degree 5
expect_exit 2 'unknown limits profile' \
    convert input.glb output.glb --limits-profile custom
expect_exit 2 'unknown loss code' \
    convert input.glb output.glb --allow-loss LOSS_NOT_REAL
expect_exit 2 'each --allow-loss code must be unique' \
    convert input.glb output.glb \
    --allow-loss LOSS_PROVENANCE_DROPPED \
    --allow-loss LOSS_PROVENANCE_DROPPED

expect_exit 2 'inspect requires one input' inspect
expect_exit 2 'unknown option' inspect input.ply --jsoon
expect_exit 2 'option specified more than once' inspect input.ply --json --json
expect_exit 2 'positive finite value' \
    inspect input.spz --source-unit-to-meter 0
expect_exit 2 'positive finite value' \
    inspect input.spz --source-unit-to-meter '+1'

if [[ "$spz_enabled" == "OFF" ]]; then
    tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/melkor-cli.XXXXXX")"
    trap 'rm -rf "$tmp_dir"' EXIT
    printf '\x1f\x8b\x08\x00' > "$tmp_dir/input.spz"
    expect_exit 4 'SPZ support is not compiled' \
        convert "$tmp_dir/input.spz" "$tmp_dir/output.ply" \
        --source-unit-to-meter 1 \
        --source-color-space srgb_rec709_display
    if [[ -e "$tmp_dir/output.ply" ]]; then
        echo "FAIL: disabled SPZ support created an output" >&2
        exit 1
    fi
fi

echo "CLI grammar tests passed"
