# Streaming and temporal playback

This document describes the current Melkor viewer behavior.
It does not make performance claims for external reconstruction methods.

## Static asset loading

The viewer uses the digest-pinned Spark 2.1.0 runtime.
`viewer/fetch-assets.sh` records the runtime URL and SHA-256 value.

The viewer accepts these local asset types:

- PLY
- SPZ
- SPLAT
- KSPLAT
- SOG or ZIP

The viewer applies these local limits:

- Files are at most 256 MiB.
- Scenes contain at most 5,000,000 splats.
- PLY input uses binary records.
- PLY headers are at most 64 KiB.
- SOG and ZIP content expands to at most 512 MiB.
- SOG metadata is at most 64 KiB.
- Each SOG image contains at most 5,000,000 pixels.
- All decoded SOG RGBA images use at most 256 MiB.

Browser or device memory can impose a lower practical limit.

The first bundled scene can render while its download continues.
During a later scene change, the viewer keeps the current scene visible.
It commits the replacement only after initialization succeeds.

The Playwright test named `first load streams progressively; scene switch does not` checks this behavior.

## Format size

PLY, SPZ, and SOG use different encodings.
Their relative size depends on the source asset and encoder settings.

Melkor does not publish one compression ratio for all assets.
Measure the exact files that you plan to serve.

Inspect a PLY or SPZ file before viewer use:

```bash
./build/dev/melkor inspect scene.ply --strict
./build/dev/melkor inspect scene.spz --strict
```

## RAD and level of detail

The pinned Spark runtime contains RAD and level-of-detail features.
Melkor does not currently provide RAD authoring or local RAD opening.

Do not list RAD as a supported Melkor input format.
Treat RAD and hierarchy authoring as future integration work.

## Temporal manifest

The viewer represents a temporal scene with a JSON manifest.
The manifest has an `fps` value and a `frames` array.

Example:

```json
{
  "fps": 12,
  "frames": [
    "time_00000.ply",
    "time_00001.ply"
  ]
}
```

The viewer uses 12 frames per second when `fps` is absent.
It accepts finite rates greater than zero and no more than 240.

Each frame path must meet these rules:

- The value is a non-empty string.
- The value is a relative path.
- The value contains no backslash.
- The value contains no `..` path part.

The first frame must initialize before the viewer commits the temporal scene.
If it fails, the current scene stays active.

## Buffered frame window

The player loads a bounded window around the active frame.
It requests two frames behind and six frames ahead.
It retains a two-frame eviction margin on each side.

The player evicts frames outside the retained window.
One loaded frame is visible at a time.

The player retries a failed frame no more than three times.
The retry delay increases after each failure.
After the last failure, the player stops and shows a recovery action.

The sequence wraps from the last frame to the first frame.
The prefetch window also wraps at this boundary.

## Create a synthetic sequence

Generate the project-owned temporal test fixture:

```bash
node viewer/make-4d-demo.js
```

The command writes per-frame PLY files and a manifest below `viewer/public/splats/4d/`.

Fetch all viewer fixtures to create the normal local demo set:

```bash
cd viewer
./fetch-assets.sh
```

The full fetch downloads digest-checked external test assets.
Use `--runtime-only` when you do not need those fixtures.

## Pack PLY frames as SPZ

`viewer/pack-4d.js` sorts numbered PLY frames and writes a temporal manifest.
Use `--spz` to convert each frame with a Melkor executable.

```bash
node viewer/pack-4d.js /path/to/frames \
  --spz \
  --fps 24 \
  --allow-loss LOSS_COLOR_SPACE_METADATA_DROPPED \
  --allow-loss LOSS_COORDINATE_METADATA_DROPPED \
  --out viewer/public/splats/4d/my-scene
```

SPZ conversion is lossy.
These approvals permit the SPZ container to omit the source color-space and coordinate metadata.
The packer stages the full sequence before it replaces an output directory.
It stores each Melkor loss report in `loss-reports.ndjson`.
Inspect representative output frames before you publish the sequence.

## Test temporal playback

Run the Chromium viewer suite:

```bash
cd viewer
bun run test -- --project=chromium
```

The suite checks these temporal behaviors:

- Static-scene preservation after a failed temporal load
- Latest-seek-wins ordering
- Bounded retries for missing frames
- PLY frame playback
- SPZ frame playback
- Sequence wrapping and buffering

## External streaming tools

`scripts/setup_streaming.sh list` prints the external research catalog.
The command does not clone or install software.

Each listed tool has separate platform, dataset, license, and model requirements.
Install only a reviewed revision in an isolated environment.
Review [the external adapter catalog](adapters/index.md) before use.

Do not treat an external project's output as a supported Melkor format without inspection.
Convert only documented PLY or SPZ output through the native format paths.

## Related documents

- [Viewer guide](../viewer/README.md)
- [Pipeline](PIPELINE.md)
- [Asset inspection](INSPECT.md)
- [Resource limits](reference/resource-limits.md)
