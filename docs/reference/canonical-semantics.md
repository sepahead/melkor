# Canonical semantics

Melkor has one public and format-interchange representation of a Gaussian splat: `SplatData`.
Every canonical format path exchanges it. Formats with read and write paths convert at
their respective boundaries. This page defines that contract.

The math oracle (`include/melkor/math/`) and scene model
(`include/melkor/scene.hpp`) implement this single source of truth. Thus, a conversion is a
defined operation, not a guess.

Deferred compute-provider, densifier, and pre-A2 mesh-initialization implementations still contain
a private compatibility representation in the source tree. It is excluded from the installed SDK
and must not cross into inspection, format, or ordinary CLI data flow. Its removal belongs to
WP10/WP12/WP14. Its existence does not permit another bridge.

## Coordinate frame

The canonical frame is the glTF world convention: **right-handed, +X left, +Y up, +Z forward,
meters.** A format that stores another frame declares it explicitly. Conversion uses the
coordinate-frame registry (`math/coordinate_frame.hpp`). This registry stores an exact orthogonal
basis-to-canonical matrix for each frame. A label alone, such as "Y-up" or "OpenGL," is ambiguous.
Melkor rejects it because an ambiguous frame can mirror or rotate a complete scene.

## Scalars and their domains

| Quantity | Storage | Domain | Notes |
|---|---|---|---|
| Position | float32, meters | finite | Canonical frame. |
| Scale | float32, per-axis | **linear**, strictly positive | Training-domain log scale is decoded once at the profile boundary, never inside an algorithm. |
| Opacity | float32 | **linear**, `[0, 1]` | Training-domain logit is decoded once at the boundary. |
| Rotation | float32 quaternion `x,y,z,w` | unit within tolerance | Identity is `(0,0,0,1)`. `q` and `-q` are the same rotation. |

A "double activation" applies a sigmoid to linear opacity or `exp` to linear scale. It produces a
plausible but incorrect value. A range check cannot find this error because small log scales and
small linear scales overlap. One module (`math/activation.hpp`) contains conversions with explicit
names. This design helps prevent an accidental double activation.

## Validated construction and editing

`SplatData::create` is the only populated construction path. It validates parallel-array lengths,
finite positions, strictly positive scale, opacity range, unit rotation, SH degree, and exact SH
storage shape before returning a value. Bulk access is const. `SplatData::edit()` creates an
isolated transaction.

`commit()` rebuilds through the same validator. It returns a complete new
value or leaves the source unchanged. Budgeted `reserve` and `append` account before allocation
and never expose a partial logical append.

`SplatMetadata`, `SplatPrimitive`, and `Provenance` (`include/melkor/provenance.hpp`) make frame,
domain, color, SH, antialiasing, source profile/hash, and operations explicit. Reproducible JSON
uses null timestamps and has no source-path field. The full hierarchy-preserving scene graph is
still WP06 work. The metadata API does not claim that current flat adapters preserve hierarchy.

## Format boundaries

- **Graphdeco-style PLY:** scale is stored as log scale, opacity as logit, quaternion as WXYZ, and
  higher SH properties are channel-major. The adapter applies `exp`/`sigmoid` once, reorders to
  XYZW, and transposes to canonical coefficient/RGB interleave on read. Write does the exact
  inverse. Canonical opacity endpoints cannot be finite logits, so write clamps them with
  `MK1210_PLY_OPACITY_ENDPOINT_CLAMPED`. Omitting SH or accepting a non-unit stored quaternion is
  reported, never silent.
- **SPZ v1–v3:** the vendored codec exposes log scale and logit opacity, XYZW rotation, and
  coefficient/RGB-interleaved SH. The Melkor adapter converts activation domains once and keeps the
  other layouts direct. Endpoint clamp, SH truncation, and non-unit rotation use stable `MK132x`
  diagnostics. The current writer cannot encode degree 4 and fails or explicitly reports a lower
  requested degree. SPZ v4 remains P0-09.
- **Legacy mesh GLB/glTF:** this is geometry initialization, not a Gaussian round trip. Positions,
  transformed normals, optional vertex color, and explicit/default linear scale and opacity are
  assembled directly into `SplatData`. A KHR Gaussian primitive is rejected by this path so it
  cannot silently lose rotation, scale, opacity, or SH. Use `melkor convert` for GLB→GLB until the
  WP06 cross-format planner exists.

## Covariance and transforms

A Gaussian's shape is the covariance `Σ = R diag(s²) Rᵀ`. For an affine node transform `A`, the
mean moves as `Aμ + t` **and** the covariance transforms as `Σ' = A Σ Aᵀ`. Moving only the mean
silently corrupts each anisotropic Gaussian when the other properties do not change.
`math/covariance.hpp` transforms the covariance, orientation, and scale. It correctly handles
rotation, nonuniform scale, shear, and reflection. It decomposes the result into positive scale
and proper rotation.

## Spherical harmonics

- Real spherical-harmonic basis, Condon–Shortley phase.
- Degree **0–4** in the canonical scene. The pinned glTF `KHR_gaussian_splatting` RC profile
  supports through degree 3. A degree-4 source into it is `LOSS_SH_DEGREE_TRUNCATED`, an approved
  loss, never a silent truncation.
- Degree `d` stores exactly `(d+1)²` RGB coefficient vectors per splat, with all lower degrees
  complete.
- The degree-0 (DC) coefficient relates to linear RGB by the pinned relation
  `rgb = SH_C0·sh_dc + 0.5` (`math/color.hpp`). This is not a gamma conversion, and it is never
  applied twice.

## Color space

sRGB↔linear is a transfer-function conversion. Linear-RGB↔SH-DC is the coefficient relation above.
They are different operations. Incorrect order or duplicate application can make splats dark or
washed out. Both operations are in `math/color.hpp`.

## Antialiasing

Antialiasing belongs in `SplatMetadata`, not in the numeric `SplatData` arrays. The current SPZ
decoder reports it in `SpzDecodeMetadata`. The legacy positional CLI has no metadata-carrying
conversion planner. Thus, it does not prove cross-format preservation. WP06/WP08 must carry it
through `SplatPrimitive`. A target that cannot represent it must report
`LOSS_ANTIALIASING_METADATA_DROPPED` rather than discard it silently.
