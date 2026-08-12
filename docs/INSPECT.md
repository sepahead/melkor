# Inspecting assets

`melkor inspect` validates one Gaussian-splat asset without writing output:

```bash
melkor inspect scene.ply
melkor inspect scene.spz --json
melkor inspect scene.glb --json --strict
melkor inspect scene.gltf --json --strict
```

The command supports PLY, SPZ versions 1 through 3, glTF, and GLB.
The glTF and GLB paths use the pinned `KHR_gaussian_splatting` release-candidate profile.

The glTF reader decodes one bounded Gaussian subset.
It reports optional geometry, materials, animation, skins, cameras, morph targets, and extension content as severe source-read losses.
Use `--strict` when these losses must fail inspection.
Conversion blocks each severe loss unless the command approves its exact loss code.
The reader rejects malformed fields and unsupported required extensions.

Local `.gltf` files can use adjacent buffer files.
Each buffer must stay below the source file directory.
Buffers can also use bounded base64 `data:` URIs.
Network URLs are not supported.

## Source semantics

A self-describing Melkor PLY, glTF, or GLB file carries its required semantics.
Graphdeco PLY and SPZ omit some values.

Supply missing values from trusted producer information:

```bash
melkor inspect trained.ply \
  --input-profile ply:graphdeco-3dgs-v1 \
  --source-frame gltf-luf \
  --source-unit-to-meter 1 \
  --source-color-space lin_rec709_display

melkor inspect scene.spz \
  --source-unit-to-meter 1 \
  --source-color-space lin_rec709_display
```

Melkor rejects an ambiguous value.
It does not select a semantic value from a filename or common convention.

## Exit codes

| Code | Meaning |
|---:|---|
| `0` | The decoded asset has no blocking issue. |
| `1` | `--strict` found one or more warnings. |
| `2` | The command line is invalid. |
| `3` | The input data is invalid. |
| `4` | The format, profile, version, or feature is unsupported. |
| `5` | An input operation failed. |
| `6` | The selected resource profile rejected the operation. |
| `7` | The requested backend is unavailable. Current commands do not return this code. |
| `8` | An unexpected internal failure occurred. |
| `130` | The user canceled the operation with `SIGINT`. |

Exit code `7` remains reserved for compatibility.

## JSON contract

`--json` emits one deterministic UTF-8 document.
The document follows [`melkor.inspect.v1`](../schemas/inspect-v1.schema.json).

The report contains:

- The source basename, container, profile, and verified byte count
- The canonical splat count, SH degree, color space, and bounds
- Field provenance
- Container encoding, declared count, and antialiasing metadata
- Source-read losses
- Stable diagnostic codes and structured context

The report never includes an absolute source path.
Diagnostic text escapes terminal control bytes.

Use `schema`, `validation.error_code`, and issue `code` for automation.
Do not parse the English message.

## Automation example

```bash
report="$(melkor inspect scene.glb --json --strict)" || {
  status=$?
  printf '%s\n' "$report" >&2
  exit "$status"
}
printf '%s\n' "$report" | jq -e \
  '.schema == "melkor.inspect.v1" and .valid == true'
```

The report order and numeric representation are stable for one input and Melkor version.

## Validation scope

Inspection rejects these conditions:

- Invalid container framing or JSON structure
- Unsupported required versions, profiles, or extensions
- Malformed semantic fields
- External-buffer traversal and symlink escapes
- Count, byte, memory, decompression, or time-limit violations
- Partial or inconsistent SH layouts
- Non-finite positions, scales, opacity, rotation, or SH values
- Unusable quaternions
- Invalid scene graphs or transforms
- Scales outside the finite float32 storage range

Inspection can return warnings for a valid source conversion.
These warnings include a scale that loses range in a downstream float32 covariance.
Use `--strict` when a warning must fail the check.

Inspection can also report a severe source-read loss for valid glTF content outside the canonical model.
Use `--strict` when any non-informational loss must fail the check.
