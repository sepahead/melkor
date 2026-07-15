# Migrating the C++ scene API from v1 to v2

Melkor v2 replaces the mutable `GaussianCloud` SDK model with `SplatData`. The v1 model stored
training-domain values (log scale, logit opacity, WXYZ quaternions) and exposed mutable vectors;
the v2 model stores canonical values (positive linear scale, linear opacity in `[0,1]`, XYZW unit
quaternions) and validates every construction or committed edit.

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

Format adapters must perform log/linear and logit/probability conversion exactly once through
`melkor/math/activation.hpp`. Do not infer a domain from the numeric range.

## Read and edit

The bulk accessors are const. Replace v1 calls to mutable `cloud.data()` with an explicit edit
transaction:

```cpp
auto edit = scene.value().edit();
auto scales = scene.value().scales();
scales[0] = {2.0f, 2.0f, 2.0f};
edit.set_scales(std::move(scales));

auto changed = edit.commit();
if (!changed.has_value()) {
    // The original scene is unchanged.
}
```

For incremental construction, use `edit.reserve(count, budget)` and
`edit.append(record, budget)`. Both account before allocating; a budget or validation failure
leaves the transaction's logical contents unchanged. `commit()` is one-shot and revalidates all
parallel lengths and canonical domains atomically.

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
colour space, antialiasing flag, source format/profile/hash, and operations. The default
`provenance_to_json` output is reproducible: timestamps are `null`, and source paths are not part
of the schema.

## Backend types

The stable installed boundary remains `melkor/c/melkor.h`. Backend-specific C++ types and the
legacy mutable `GaussianCloud` are implementation details during the v2 backend migration and are
not supported SDK contracts.
