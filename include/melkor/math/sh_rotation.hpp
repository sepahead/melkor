// Rotation of real spherical-harmonic coefficients.
//
// When a scene node rotates a Gaussian splat, its view-dependent colour -- stored as real SH
// coefficients -- must rotate with it, or the specular highlights point the wrong way. This is the
// transform the glTF spec notes "is usually performed using Wigner-D matrices". Melkor builds the
// per-band rotation operator directly from its own SH basis, so the rotation is exact for the
// coefficient convention the rest of the toolkit uses (the 3DGS/KHR real basis, Condon-Shortley
// phase, coefficients ordered m = -l..+l within each degree) rather than for some other library's.
//
// Scope: degrees 0-3, the ceiling of the pinned glTF `KHR_gaussian_splatting` RC profile. Degree 4
// (which SPZ v4 carries) is not yet supported and is rejected rather than silently mishandled.
//
// Correctness is not asserted, it is constructed and checked: each band matrix is the least-squares
// operator that reproduces `Y_l^m(R^{-1} d)` in the span of `{Y_l^m'(d)}`, and the tests verify it
// is orthogonal, inverts and composes correctly, matches a direct evaluation of the rotated basis,
// and rotates a directional lobe to the expected place.

#ifndef MELKOR_MATH_SH_ROTATION_HPP
#define MELKOR_MATH_SH_ROTATION_HPP

#include "melkor/error.hpp"
#include "melkor/math/quaternion.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace melkor::math {

// True when `m` is a proper rotation: orthonormal columns and determinant +1, within `tol`. SH
// rotation is only defined for a rotation; a scaled, sheared, or reflecting linear map is not one.
bool is_proper_rotation(const Mat3& m, double tol = 1e-6);

class ShRotation {
  public:
    // The highest SH degree this operator supports.
    static constexpr std::uint32_t kMaxDegree = 3;

    // Builds the rotation operator for degrees 0..degree under the proper rotation `rotation`.
    // Fails if `degree > kMaxDegree` or `rotation` is not a proper rotation.
    static Result<ShRotation> create(const Mat3& rotation, std::uint32_t degree);

    std::uint32_t degree() const noexcept { return degree_; }

    // Rotates one splat's SH coefficient block in place. The block holds `(degree+1)^2` coefficients,
    // each with `channels` contiguous values (3 for RGB), laid out coefficient-major then channel --
    // the scene model's per-splat layout: value(coeff k, channel c) = block[k*channels + c]. The DC
    // term (coefficient 0) is rotation-invariant and is left untouched.
    void rotate_block(float* block, std::size_t channels) const;

    // The (2l+1)x(2l+1) band matrix for degree l, row-major (M[m*(2l+1) + m']). Exposed for tests.
    const std::vector<double>& band(std::uint32_t l) const;

  private:
    std::uint32_t degree_ = 0;
    std::array<std::vector<double>, kMaxDegree + 1> bands_;
};

}  // namespace melkor::math

#endif  // MELKOR_MATH_SH_ROTATION_HPP
