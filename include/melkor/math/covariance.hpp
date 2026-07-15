// Gaussian covariance: construction, affine transform, and decomposition.
//
// A 3D Gaussian's shape is the covariance Σ = R diag(s²) Rᵀ, where R is the orientation and s
// the per-axis linear scales. When a scene node applies an affine transform A to the Gaussian,
// the mean moves as `μ' = Aμ + t` AND the covariance transforms as `Σ' = A Σ Aᵀ`. Leaving the
// covariance (or the orientation and scale it is built from) untouched while moving only the
// mean is release blocker P0-17: it silently corrupts every anisotropic Gaussian under any
// rotation, non-uniform scale, or reflection.
//
// This module is the one place that transform is done correctly, so a format adapter or backend
// can never reinvent it wrongly.

#ifndef MELKOR_MATH_COVARIANCE_HPP
#define MELKOR_MATH_COVARIANCE_HPP

#include "melkor/error.hpp"
#include "melkor/math/quaternion.hpp"

namespace melkor::math {

// A rotation (as a canonical quaternion) plus three positive linear scales. This is the
// storage form; covariance is the derived form.
struct RotationScale {
    Quat rotation;
    Vec3 scale{1.0, 1.0, 1.0};  // per-axis linear standard-deviation-like scale, all > 0
};

// Builds the symmetric covariance Σ = R diag(s²) Rᵀ from a rotation and positive scales.
// Precondition: rotation is unit, scales are finite and positive. The result is symmetric by
// construction.
Result<Mat3> covariance_from_rotation_scale(const Quat& rotation, const Vec3& scale);

// Applies a general affine linear map A to a Gaussian's shape: Σ' = A Σ Aᵀ, then recovers the
// orientation and positive scales of the transformed Gaussian.
//
// This handles rotation, non-uniform scale, shear, and reflection correctly, because it works
// on the covariance rather than trying to compose the transform onto the quaternion directly
// (which is only valid for a pure rotation). The returned rotation/scale reproduce Σ' within
// tolerance.
//
// `linear` is the 3x3 linear part of the node transform (the translation is applied to the mean
// separately). A near-singular or non-finite `linear` fails rather than producing garbage.
Result<RotationScale> affine_transform_gaussian(const Mat3& linear, const Quat& rotation,
                                                const Vec3& scale);

// Decomposes a symmetric positive-semidefinite covariance back into a rotation and positive
// scales via a symmetric eigendecomposition. Eigenvalues are the squared scales; eigenvectors
// are the rotation columns.
//
// Determinism and robustness:
//   - Eigenvalues (and their paired eigenvectors) are sorted descending, so the decomposition
//     is stable run to run.
//   - The eigenvector basis is forced right-handed (a reflection is folded into the rotation by
//     flipping one axis), because a quaternion can only represent a proper rotation.
//   - A tiny negative eigenvalue from round-off is clamped to the minimum scale under a
//     published tolerance; a substantial negative eigenvalue means the input was not a valid
//     covariance and is an error.
Result<RotationScale> rotation_scale_from_covariance(const Mat3& sigma);

// Symmetric 3x3 eigendecomposition, exposed for testing and reuse. Returns eigenvalues in
// descending order and the corresponding orthonormal eigenvectors as matrix columns. The input
// is symmetrised as (M + Mᵀ)/2 before solving. Deterministic.
struct Eigen3 {
    Vec3 values;   // descending
    Mat3 vectors;  // column i is the eigenvector for values[i]
};
Result<Eigen3> symmetric_eigen(const Mat3& m);

// Extracts the rotation component of a general linear map via the polar decomposition M = R P,
// where R is a proper rotation and P is symmetric positive-definite. This is the rotation the glTF
// spec says spherical harmonics follow under a node transform, and it recovers R correctly whether
// the source factors the transform as R·S or S·R. Computed as R = M (MᵀM)^(-1/2) from the
// eigendecomposition of MᵀM. Fails when M is singular or has a negative determinant (a reflection),
// for which no proper rotation is defined.
Result<Mat3> rotation_from_linear(const Mat3& m);

}  // namespace melkor::math

#endif  // MELKOR_MATH_COVARIANCE_HPP
