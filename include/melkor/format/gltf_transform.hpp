// glTF node transforms.
//
// A glTF node places a mesh in space with either a 4x4 column-major `matrix` or a
// translation/rotation/scale triple, and a node inherits its parent's transform, so a splat's
// effective placement is the product of transforms from the scene root down to the node that
// instantiates its mesh. This module turns a `NodeDesc` into an affine `{linear 3x3, translation}`
// and composes the parent chain -- the exact `M` (the upper-left 3x3 of the node's global matrix)
// and translation the `KHR_gaussian_splatting` reader feeds into the covariance transform
// `Σ' = M Σ Mᵀ` and the mean update `μ' = M μ + t`.
//
// It is pure and reuses the math oracle's quaternion handling, so the node transform and the
// covariance transform agree by construction. The intricate part -- turning `M` and a splat's own
// rotation/scale back into a canonical positive-scale/proper-rotation Gaussian -- is done by
// math/covariance.hpp, not here; this module only produces `M` and `t`.

#ifndef MELKOR_FORMAT_GLTF_TRANSFORM_HPP
#define MELKOR_FORMAT_GLTF_TRANSFORM_HPP

#include "melkor/format/gltf_document.hpp"
#include "melkor/math/quaternion.hpp"

namespace melkor::format::gltf {

// An affine transform as a linear part (row-major 3x3, matching math::Mat3) and a translation.
struct NodeTransform {
    math::Mat3 linear{{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};
    math::Vec3 translation{{0.0, 0.0, 0.0}};
};

// The identity transform.
NodeTransform identity_transform();

// The local transform of a single node. If the node carries a `matrix`, its upper-left 3x3
// (converted from glTF's column-major storage to row-major) is the linear part and its fourth
// column is the translation. Otherwise the transform is T·R·S: the linear part is R·diag(scale)
// with R built from the node's (normalized) quaternion, and the translation is the node's
// translation. A degenerate (zero-norm) node quaternion falls back to identity rotation.
NodeTransform local_node_transform(const NodeDesc& node);

// Affine composition: the transform that applies `child` first and then `parent`, i.e. the global
// transform of a child node given its parent's global transform. linear = parent.linear·child.linear;
// translation = parent.linear·child.translation + parent.translation.
NodeTransform compose(const NodeTransform& parent, const NodeTransform& child);

// Applies a transform to a point: linear·p + translation.
math::Vec3 apply_point(const NodeTransform& t, const math::Vec3& p);

}  // namespace melkor::format::gltf

#endif  // MELKOR_FORMAT_GLTF_TRANSFORM_HPP
