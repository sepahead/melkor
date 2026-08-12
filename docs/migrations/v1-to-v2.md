# Migrating the C++ scene API from v1 to v2

Melkor v2 replaces the mutable `GaussianCloud` model with `SplatData`. The v1 model stored
training-domain values and exposed mutable vectors. These values included log scale, logit
opacity, and WXYZ quaternions. The v2 model stores nonnegative linear scale, linear opacity, and
XYZW unit quaternions. Linear opacity stays in `[0,1]`. The model validates each construction.

## Construct a scene

Instead of default-constructing and mutating `GaussianSplat` fields, assemble the structure-of-
arrays input and pass it through the validating factory:

```cpp
melkor::SplatBufferInput input;
input.positions = {{0.0f, 0.0f, 0.0f}};
input.scales = {{1.0f, 1.0f, 1.0f}};       // linear, never log scale
input.rotations = {{0.0f, 0.0f, 0.0f, 1.0f}}; // XYZW
input.opacities = {0.5f};                  // linear, never a logit
input.sh = melkor::ShBuffer::black(1).value();

auto scene = melkor::SplatData::create(std::move(input));
if (!scene.has_value()) {
    // Inspect scene.diagnostics(); do not use a partially valid scene.
}
```

Format adapters must do log/linear and logit/probability conversion exactly once through
`melkor/math/activation.hpp`. Do not infer a domain from the numeric range.

## Replace scene data

The bulk accessors are const. Replace v1 calls to mutable `cloud.data()` with a new validated
value:

```cpp
melkor::SplatBufferInput replacement;
replacement.positions = scene.value().positions();
replacement.scales = scene.value().scales();
replacement.rotations = scene.value().rotations();
replacement.opacities = scene.value().opacities();
replacement.sh = scene.value().sh();
replacement.scales[0] = {2.0f, 2.0f, 2.0f};

auto changed = melkor::SplatData::create(std::move(replacement));
if (!changed.has_value()) {
    // The original scene is unchanged.
}
```

For incremental construction, collect each field in `SplatBufferInput`. Check resource limits
before each allocation. Call `SplatData::create` after all fields are complete. A failed factory
call does not change the original value.

## Spherical harmonics

`ShBuffer` stores one contiguous splat-major block. For splat `s`, coefficient `k`, and channel
`c`, the index is:

```text
s * (((degree + 1)^2) * 3) + k * 3 + c
```

Use `ShBuffer::create(degree, splat_count, data)` instead of a mutable SH-degree setter. Degrees
0 through 4 are accepted and lower-degree bands must be complete.

## Metadata and provenance

Wrap canonical data with `SplatPrimitive::create` to record the exact coordinate frame, SH basis,
color space, antialiasing flag, source format/profile/hash, and operations. The default
`provenance_to_json` output is reproducible. Timestamps are `null`, and source paths are not part
of the schema.

## Installed SDK boundary

The stable installed boundary is `melkor/c/melkor.h`.
The project does not install `SplatData`, `GaussianCloud`, or another C++ model API.
