# Canonical semantics

Melkor has one public and format-interchange representation of a Gaussian splat: `SplatData`.
Every supported adapter entry point exchanges it; formats with both read and write paths convert at
their respective boundaries. This page is that contract.
It is the single source of truth the math oracle (`include/melkor/math/`) and scene model
(`include/melkor/scene.hpp`) implement, and it is what makes a conversion a defined operation
rather than a guess.

Deferred compute-provider, densifier, and pre-A2 mesh-initialisation implementations still contain
a private compatibility representation in the source tree. It is excluded from the installed SDK
and must not cross into inspection, format, or ordinary CLI data flow. Its removal belongs to
WP10/WP12/WP14; its existence is not permission to add another bridge.

## Coordinate frame

The canonical frame is the glTF world convention: **right-handed, +X left, +Y up, +Z forward,
metres.** A format that stores another frame declares it explicitly; conversion goes through the
coordinate-frame registry (`math/coordinate_frame.hpp`), which carries an exact orthogonal
basis-to-canonical matrix per frame. A frame given only as a label ("Y-up", "OpenGL") is
ambiguous and is rejected — an ambiguous frame silently mirrors or rotates a whole scene.

## Scalars and their domains

| Quantity | Storage | Domain | Notes |
|---|---|---|---|
| Position | float32, metres | finite | Canonical frame. |
| Scale | float32, per-axis | **linear**, strictly positive | Training-domain log scale is decoded once at the profile boundary, never inside an algorithm. |
| Opacity | float32 | **linear**, `[0, 1]` | Training-domain logit is decoded once at the boundary. |
| Rotation | float32 quaternion `x,y,z,w` | unit within tolerance | Identity is `(0,0,0,1)`. `q` and `-q` are the same rotation. |

Applying a sigmoid to an already-linear opacity, or an `exp` to an already-linear scale — "double
activation" — produces a plausible but wrong value that no range check catches, because small
log-scales and small linear scales overlap numerically. The conversions live in one module
(`math/activation.hpp`) with explicit names so this cannot happen by accident.

## Validated construction and editing

`SplatData::create` is the only populated construction path. It validates parallel-array lengths,
finite positions, strictly positive scale, opacity range, unit rotation, SH degree, and exact SH
storage shape before returning a value. Bulk access is const. `SplatData::edit()` creates an
isolated transaction; `commit()` rebuilds through the same validator and either returns a complete
new value or leaves the source unchanged. Budgeted `reserve` and `append` account before allocation
and never expose a partial logical append.

`SplatMetadata`, `SplatPrimitive`, and `Provenance` (`include/melkor/provenance.hpp`) make frame,
domain, colour, SH, antialiasing, source profile/hash, and operations explicit. Reproducible JSON
uses null timestamps and has no source-path field. The full hierarchy-preserving scene graph is
still WP06 work; the metadata API does not claim that current flat adapters preserve hierarchy.

## Format boundaries

- **Graphdeco-style PLY:** scale is stored as log scale, opacity as logit, quaternion as WXYZ, and
  higher SH properties are channel-major. The adapter applies `exp`/`sigmoid` once, reorders to
  XYZW, and transposes to canonical coefficient/RGB interleave on read; write performs the exact
  inverse. Canonical opacity endpoints cannot be finite logits, so write clamps them with
  `MK1210_PLY_OPACITY_ENDPOINT_CLAMPED`. Omitting SH or accepting a non-unit stored quaternion is
  reported, never silent.
- **SPZ v1–v3:** the vendored codec exposes log scale and logit opacity, XYZW rotation, and
  coefficient/RGB-interleaved SH. The Melkor adapter converts activation domains once and keeps the
  other layouts direct. Endpoint clamp, SH truncation, and non-unit rotation use stable `MK132x`
  diagnostics. The current writer cannot encode degree 4 and fails or explicitly reports a lower
  requested degree; SPZ v4 remains P0-09.
- **Legacy mesh GLB/glTF:** this is geometry initialisation, not a Gaussian round-trip. Positions,
  transformed normals, optional vertex colour, and explicit/default linear scale and opacity are
  assembled directly into `SplatData`. A KHR Gaussian primitive is rejected by this path so it
  cannot silently lose rotation, scale, opacity, or SH; use `melkor convert` for GLB→GLB until the
  WP06 cross-format planner lands.

## Covariance and transforms

A Gaussian's shape is the covariance `Σ = R diag(s²) Rᵀ`. Under an affine node transform `A`, the
mean moves as `Aμ + t` **and** the covariance transforms as `Σ' = A Σ Aᵀ`. Moving only the mean
while leaving the covariance, orientation, and scale untouched silently corrupts every anisotropic
Gaussian; the transform is done through `math/covariance.hpp`, which handles rotation, non-uniform
scale, shear, and reflection correctly and decomposes the result back to a positive-scale,
proper-rotation form.

## Spherical harmonics

- Real spherical-harmonic basis, Condon–Shortley phase.
- Degree **0–4** in the canonical scene. The pinned glTF `KHR_gaussian_splatting` RC profile
  supports through degree 3; a degree-4 source into it is `LOSS_SH_DEGREE_TRUNCATED`, an approved
  loss, never a silent truncation.
- Degree `d` stores exactly `(d+1)²` RGB coefficient vectors per splat, with all lower degrees
  complete.
- The degree-0 (DC) coefficient relates to linear RGB by the pinned relation
  `rgb = SH_C0·sh_dc + 0.5` (`math/color.hpp`). This is not a gamma conversion, and it is never
  applied twice.

## Colour space

sRGB↔linear is a transfer-function (gamma) conversion; linear-RGB↔SH-DC is the coefficient
relation above. They are distinct, and conflating them — or applying either twice — is a common
way splats render dark or washed out. Both live in `math/color.hpp`.

## Antialiasing

Antialiasing belongs in `SplatMetadata`, not in the numeric `SplatData` arrays. The current SPZ
decoder reports it in `SpzDecodeMetadata`, but the legacy positional CLI has no metadata-carrying
conversion planner and therefore does not yet prove cross-format preservation. WP06/WP08 must carry
it through `SplatPrimitive`; a target that cannot represent it must report
`LOSS_ANTIALIASING_METADATA_DROPPED` rather than discard it silently.
