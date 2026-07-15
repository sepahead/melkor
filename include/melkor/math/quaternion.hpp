// Canonical quaternion contract.
//
// A quaternion in Melkor is stored `(x, y, z, w)`, unit length within a documented tolerance,
// with identity `(0, 0, 0, 1)`. It represents an active rotation of a vector in the canonical
// right-handed frame. Every format adapter converts *to* this contract at its boundary and
// *from* it when writing; no adapter reorders quaternion components on its own, because both
// component orders can produce unit quaternions and guessing from the values is impossible.
//
// This is part of the correctness fix for P0-17: a scene transform that rotates the mean but
// leaves the orientation quaternion (and the covariance built from it) untouched silently
// corrupts every anisotropic Gaussian. Orientation, scale, and covariance transform together,
// through this module, or not at all.
//
// All storage is float32; composition and decomposition use float64 intermediates where it
// materially improves stability, then convert back.

#ifndef MELKOR_MATH_QUATERNION_HPP
#define MELKOR_MATH_QUATERNION_HPP

#include "melkor/error.hpp"

#include <array>
#include <cstdint>

namespace melkor::math {

// A 3x3 matrix in row-major order, double precision. Used for rotation and covariance.
using Mat3 = std::array<double, 9>;   // [r0c0 r0c1 r0c2  r1c0 ...]
using Vec3 = std::array<double, 3>;

// Canonical quaternion, component order x, y, z, w.
struct Quat {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;  // identity by default: a default-constructed quaternion is a valid rotation
};

// Tolerances. Named constants with a single definition, referenced by the tests, so the
// numbers a reader sees are the numbers the tests enforce.
namespace tol {
// Below this norm a quaternion carries no usable direction and is rejected outright.
constexpr double kQuatRejectNorm = 1e-12;
// Within this of unit length, a quaternion is renormalised silently (an info-level event).
constexpr double kQuatRenormalize = 1e-3;
// Minimum positive scale, in metres. A scale at or below this is degenerate.
constexpr double kMinScale = 1e-12;
}  // namespace tol

// The identity rotation.
constexpr Quat identity_quat() { return Quat{0.0, 0.0, 0.0, 1.0}; }

double norm(const Quat& q);

// Normalises to unit length. Fails if the norm is below kQuatRejectNorm: such a quaternion has
// no meaningful direction and must not be silently promoted to identity, because that would
// invent an orientation the data never had. Repair to identity is a separate, explicit choice.
Result<Quat> normalize(const Quat& q);

// True when the quaternion is finite and unit length within kQuatRenormalize.
bool is_unit(const Quat& q);

// Converts a unit quaternion to its rotation matrix. Precondition: q is finite; the caller
// should have normalised it. The matrix is orthonormal within floating tolerance.
Mat3 to_matrix(const Quat& q);

// Recovers a quaternion from a rotation matrix, using the numerically stable branch selection
// that avoids catastrophic cancellation near a 180-degree rotation (the naive `w = sqrt(1 +
// trace)/2` formula loses all precision when the trace approaches -1). Sign is canonicalised so
// that w >= 0, which makes serialisation deterministic; q and -q are the same rotation.
Result<Quat> from_matrix(const Mat3& m);

// Builds a quaternion from an orthonormal right-handed frame given as three column axes. Used
// by mesh initialisation to turn a tangent frame into an orientation. The axes must be
// orthonormal within tolerance; otherwise this fails rather than producing a non-rotation.
Result<Quat> from_frame(const Vec3& axis_x, const Vec3& axis_y, const Vec3& axis_z);

// The rotation angle, in radians, between two rotations. Uses the absolute value of the dot
// product so that q and -q -- the same rotation -- report zero distance. This is the correct
// way to compare quaternions in a test: never component-by-component, because the components are
// not unique.
double angular_distance(const Quat& a, const Quat& b);

// Composition: the rotation that applies `b` then `a` (matrix product A*B). Order matters and is
// documented here so callers do not have to reverse-engineer it.
Quat multiply(const Quat& a, const Quat& b);

}  // namespace melkor::math

#endif  // MELKOR_MATH_QUATERNION_HPP
