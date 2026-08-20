# CLI reference

The `melkor` executable inspects and converts Gaussian-splat assets.
It has no trainer, renderer, network loader, or runtime plugin system.

No production release is currently supported.
This reference describes the `2.0.0-dev` source tree.

## Commands

```text
melkor inspect INPUT [options]
melkor convert INPUT OUTPUT [options]
melkor --version
melkor --help
```

`melkor version` and `melkor help` are equivalent command aliases.
They accept no additional arguments.

Run `melkor inspect --help` or `melkor convert --help` for command-specific help.
Use `--` before a path that starts with a hyphen.

## Inspect

`inspect` validates one asset without writing output.

```text
melkor inspect INPUT [options]
```

| Option | Value | Purpose |
|---|---|---|
| `--input-format` | `ply`, `spz`, `gltf`, or `glb` | Select the container explicitly. |
| `--input-profile` | Exact profile ID | Select the stored semantic contract. |
| `--source-frame` | `gltf-luf`, `ply-rdf`, or `spz-rub` | Supply a missing PLY source frame. |
| `--source-unit-to-meter` | Positive number | Supply a missing PLY or SPZ unit. |
| `--source-color-space` | `srgb_rec709_display` or `lin_rec709_display` | Supply a missing PLY or SPZ color space. |
| `--json` | None | Write one `melkor.inspect.v1` JSON report. |
| `--strict` | None | Fail on warnings or non-informational losses. |
| `--limits-profile` | `web`, `desktop`, or `server` | Select a bounded resource profile. |
| `-h`, `--help` | None | Show command help. |

PLY and SPZ can omit required source semantics.
Supply missing values only from trusted producer information.

SPZ fixes its source frame to `spz-rub` and rejects a frame override.
glTF and GLB define their frame, unit, and color space.
They reject all source semantic overrides.

```bash
melkor inspect trained.ply \
  --input-profile ply:graphdeco-3dgs-v1 \
  --source-frame gltf-luf \
  --source-unit-to-meter 1 \
  --source-color-space lin_rec709_display
```

Do not copy these example values without checking the producer.
Read [Asset inspection](INSPECT.md) for JSON fields, diagnostics, and exit codes.

## Convert

`convert` reads one asset through the canonical model and writes one target asset.

```text
melkor convert INPUT OUTPUT [options]
```

### Input options

| Option | Value | Purpose |
|---|---|---|
| `--input-format` | `ply`, `spz`, `gltf`, or `glb` | Select the input container explicitly. |
| `--input-profile` | Exact profile ID | Select the stored input semantics. |
| `--source-frame` | `gltf-luf`, `ply-rdf`, or `spz-rub` | Supply a missing PLY source frame. |
| `--source-unit-to-meter` | Positive number | Supply a missing PLY or SPZ unit. |
| `--source-color-space` | `srgb_rec709_display` or `lin_rec709_display` | Supply a missing PLY or SPZ color space. |

### Output options

| Option | Value | Purpose |
|---|---|---|
| `--output-format` | `ply`, `spz`, or `glb` | Select the output container explicitly. |
| `--output-profile` | Exact profile ID | Select the stored output semantics. |
| `--target-frame` | `gltf-luf`, `ply-rdf`, or `spz-rub` | Select the PLY target frame. |
| `--output-antialiased` | `true` or `false` | Set PLY or SPZ antialiasing metadata. |
| `--max-sh-degree` | Integer from `0` through `4` | Set the maximum PLY or SPZ SH degree. |
| `--ascii` | None | Write ASCII PLY instead of binary PLY. |
| `--force` | None | Replace an existing regular output file. |

### Policy options

| Option | Value | Purpose |
|---|---|---|
| `--allow-loss` | Stable loss code | Approve one severe loss. Repeat for different codes. |
| `--limits-profile` | `web`, `desktop`, or `server` | Select a bounded resource profile. |
| `-h`, `--help` | None | Show command help. |

The command rejects a repeated option.
Each `--allow-loss` code must be unique.
There is no blanket loss approval flag.

ASCII output and target-frame selection apply only to PLY.
GLB rejects ASCII, target-frame, antialiasing, and SH-degree output options.
SPZ needs `--output-antialiased` when the input has no antialiasing value.

## Format profiles

| Profile | Read container | Write container | Important limit |
|---|---|---|---|
| `ply:melkor-canonical-v1` | PLY | PLY | Carries required semantic markers. |
| `ply:graphdeco-3dgs-v1` | PLY | PLY | Uses Graphdeco storage domains and field order. |
| `ply:da3-gaussian-v1` | PLY | PLY | Preserves complete SH data through degree 4. |
| `spz:spz-v1-v3` | SPZ versions 1 through 3 | SPZ version 3 | Omits some source semantics. |
| `khr-gaussian-splatting-rc-63770cc` | glTF or GLB | GLB | Uses the pinned release-candidate subset. |

The profile ID defines field meaning.
Read [Canonical semantics](reference/canonical-semantics.md) before you add an out-of-band semantic value.

## Format selection

Melkor normally combines the path suffix, content probe, and explicit profile rules.
Input bytes must agree with an explicit input format.
Without an input override, recognized bytes and a recognized suffix must agree.
An explicit output format must agree with a recognized output suffix.
Use a format option for a suffixless path:

```bash
melkor inspect asset --input-format glb
melkor convert input.glb output --output-format ply
```

An explicit format does not bypass content validation.
An explicit profile does not approve missing or incompatible semantics.

## Loss reports

Each successful conversion writes one version 1 loss-report JSON document to standard output.
The command writes human-readable diagnostics to standard error.
The report has a top-level `schema_version` value of `1`.

Use the loss report only when the command returns exit status zero.
A severe loss blocks output installation without its exact approval code.
A fatal loss cannot receive approval.

```bash
melkor convert scene.ply scene.spz \
  --allow-loss LOSS_COLOR_SPACE_METADATA_DROPPED \
  --allow-loss LOSS_COORDINATE_METADATA_DROPPED
```

Review [Loss policy](reference/loss-policy.md) before you approve a code.
Validate the report against
[`schemas/loss-report-v1.schema.json`](../schemas/loss-report-v1.schema.json).

## Output safety

The writer stages output in the destination directory.
It completes the format writer and loss-policy checks before installation.

The default policy refuses an existing destination.
Use `--force` only after you verify the target path.
The CLI rejects a symbolic-link destination.

A failure before installation preserves the existing destination.
A durability failure can occur after installation and returns a failure status.

## Resource limits and cancellation

The default resource profile is `desktop`.
There is no unlimited CLI profile.

Press `Ctrl+C` to request cooperative cancellation.
Canceled work returns exit code `130` and does not install incomplete output.

Read [Resource limits](reference/resource-limits.md) for exact ceilings and accounting rules.

## Automation rules

Apply these rules in scripts:

- Check the process exit status before you use a report.
- Parse `schema` for inspection reports and `schema_version` for loss reports.
- Use stable diagnostic codes and structured fields.
- Do not parse English diagnostic text for control flow.
- Keep each loss report with its converted asset.
- Record the Melkor version, exact command, and input digest.
- Select each missing semantic from verified producer information.

Example:

```bash
if report="$(melkor inspect scene.glb --json --strict)"; then
  printf '%s\n' "$report" | jq -e \
    '.schema == "melkor.inspect.v1" and .valid == true'
else
  status=$?
  printf '%s\n' "$report" >&2
  exit "$status"
fi
```
