# Inspecting assets

`melkor inspect` validates an asset without writing output or initializing a
Metal, CUDA, or CPU compute provider:

```bash
melkor inspect scene.ply
melkor inspect scene.spz --json
melkor inspect model.glb --json --strict
```

Supported inputs are PLY, SPZ v1-v3, GLB, and glTF 2.0. SPZ v4 is reported as
`unsupported_spz_version` until Melkor's pinned SPZ decoder supports it.
`KHR_gaussian_splatting` and other required glTF extensions fail closed rather
than being misrepresented as ordinary mesh vertices.

## Exit codes

| Code | Meaning |
|---:|---|
| `0` | The decoded asset has no validation errors. |
| `1` | The asset is invalid, or `--strict` promoted one or more warnings. |
| `2` | The inspect command line is invalid. |

Warnings describe deliberate reader conversions or defaults, such as mapping
byte RGB to spherical-harmonics DC or supplying a missing identity rotation.
Without `--strict`, a warning-only report remains valid and exits zero.

## JSON contract

`--json` emits exactly one deterministic UTF-8 JSON document with schema
`melkor.inspect.v1`. It includes:

- source path, normalized format, kind, and byte size;
- decoded count, SH degree, field provenance, and finite bounds;
- container encoding, declared count, and SPZ antialiasing metadata;
- stable issue severity/code/message/count/first-index records.

Malformed UTF-8 from an input parser is escaped, so failure output remains
parseable JSON. Human output escapes terminal control bytes in paths and parser
messages.

The command never normalizes, rewrites, or changes the source timestamp. It
rejects partial glTF decodes, unsupported versions/required extensions,
external-buffer traversal and symlink escapes, malformed PLY scalar/SH layouts,
non-finite fields, unusable quaternions, and scale values whose float32
covariance would overflow or round to zero.

## Automation example

```bash
report="$(melkor inspect scene.ply --json --strict)" || {
  printf '%s\n' "$report" >&2
  exit 1
}
printf '%s\n' "$report" | jq -e '.schema == "melkor.inspect.v1" and .valid'
```

The issue order and number rendering are stable for an unchanged input and
Melkor version. Consumers should key on `schema` and issue `code`, not parse the
human-readable message.
