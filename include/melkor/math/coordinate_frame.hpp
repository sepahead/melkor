// Coordinate-frame registry.
//
// A coordinate frame is not just a label. "OpenGL coordinates" or "Y-up" without a full axis,
// handedness, and unit definition is ambiguous, and an ambiguous frame silently mirrors or
// rotates a whole scene. Every frame here carries an exact basis-to-canonical matrix, its
// handedness, and its length unit, so a conversion is a defined operation rather than a guess.
//
// The canonical frame is the glTF world convention: right-handed, +X left, +Y up, +Z forward,
// meters. Everything internal is expressed in it. Adapters convert at their boundary.
//
// A frame conversion applies to a Gaussian's mean AND, through the same linear basis, to its
// covariance (via covariance.hpp's affine transform) and its directional spherical harmonics.
// Transforming only the mean is the P0-17 bug; the registry exists so a caller reaches for the
// whole, correct conversion.

#ifndef MELKOR_MATH_COORDINATE_FRAME_HPP
#define MELKOR_MATH_COORDINATE_FRAME_HPP

#include "melkor/error.hpp"
#include "melkor/math/quaternion.hpp"

#include <string>

namespace melkor::math {

// A named coordinate frame with an exact definition.
struct CoordinateFrame {
    std::string id;

    // The 3x3 linear map that takes a vector expressed in THIS frame to the canonical frame.
    // For the canonical frame this is the identity. It is a pure basis change (rotation and/or
    // reflection). The unit_to_meter member carries the length scale.
    Mat3 to_canonical{1, 0, 0, 0, 1, 0, 0, 0, 1};

    // Multiply a length in this frame's unit by this value to get meters.
    double unit_to_meter = 1.0;

    // True when to_canonical includes a reflection (negative determinant). A reflection needs a
    // separately tested SH transform and must not be applied silently, so it is flagged here.
    bool includes_reflection = false;
};

// The canonical frame uses the identity basis and meters.
CoordinateFrame canonical_frame();

// Builds a frame from an explicit basis matrix and unit. The matrix must be orthogonal (a basis
// change, not an arbitrary transform); a non-orthogonal matrix is rejected, because it would not
// be a coordinate frame at all. `includes_reflection` is derived from the determinant sign.
Result<CoordinateFrame> frame_from_basis(std::string id, const Mat3& to_canonical,
                                         double unit_to_meter);

// Looks up a registered frame by its stable ID. The registry contains gltf-luf, ply-rdf, and
// spz-rub. A PLY adapter must not select ply-rdf without an explicit user choice or file marker.
Result<CoordinateFrame> frame_by_id(const std::string& id);

// Transforms a position from `from` into the canonical frame: applies the basis change, then the
// unit scale. This is the position half of a full frame conversion; covariance and SH use the
// same `from.to_canonical` through the covariance and SH modules.
Vec3 position_to_canonical(const CoordinateFrame& from, const Vec3& position);

// Transforms a canonical position into `to`. This function is the inverse of
// position_to_canonical for a valid frame.
Vec3 position_from_canonical(const CoordinateFrame& to, const Vec3& position);

// Transforms an orientation between a proper frame and the canonical frame. These functions keep
// the scale-axis order. They reject a reflection because a quaternion cannot represent one.
Result<Quat> rotation_to_canonical(const CoordinateFrame& from, const Quat& rotation);
Result<Quat> rotation_from_canonical(const CoordinateFrame& to, const Quat& rotation);

}  // namespace melkor::math

#endif  // MELKOR_MATH_COORDINATE_FRAME_HPP
