# Canonical semantics

Melkor has one internal representation of a Gaussian splat, and every format adapter converts to
and from it at its boundary. This page is that contract. It is the single source of truth the
math oracle (`include/melkor/math/`) and the scene model (`include/melkor/scene.hpp`) implement,
and it is what makes a conversion a defined operation rather than a guess.

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

Antialiasing is metadata, preserved through conversion, not an ignored boolean. Dropping it when a
target cannot represent it is `LOSS_ANTIALIASING_METADATA_DROPPED`.
