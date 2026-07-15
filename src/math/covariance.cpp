#include "melkor/math/covariance.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace melkor::math {
namespace {

bool all_finite(const Mat3& m) {
    for (double v : m) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

// C = A * B, row-major 3x3.
Mat3 matmul(const Mat3& a, const Mat3& b) {
    Mat3 c{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += a[i * 3 + k] * b[k * 3 + j];
            }
            c[i * 3 + j] = sum;
        }
    }
    return c;
}

Mat3 transpose(const Mat3& m) {
    return Mat3{m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
}

double determinant(const Mat3& m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

}  // namespace

Result<Mat3> covariance_from_rotation_scale(const Quat& rotation, const Vec3& scale) {
    if (!is_unit(rotation)) {
        Diagnostic d("MK1301_NON_UNIT_ROTATION", Severity::error,
                     "rotation must be a unit quaternion");
        return Result<Mat3>::failure(ErrorCode::invalid_data, std::move(d));
    }
    for (double s : scale) {
        if (!std::isfinite(s) || s <= 0.0) {
            Diagnostic d("MK1302_NONPOSITIVE_SCALE", Severity::error,
                         "covariance scales must be finite and positive");
            d.with_context("scale", s);
            return Result<Mat3>::failure(ErrorCode::invalid_data, std::move(d));
        }
    }

    // Σ = R diag(s²) Rᵀ. Build R diag(s²) by scaling R's columns, then multiply by Rᵀ.
    const Mat3 r = to_matrix(rotation);
    const std::array<double, 3> s2{scale[0] * scale[0], scale[1] * scale[1], scale[2] * scale[2]};
    Mat3 rs{};  // R * diag(s²): column j of R scaled by s²[j]
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rs[i * 3 + j] = r[i * 3 + j] * s2[j];
        }
    }
    return Result<Mat3>::success(matmul(rs, transpose(r)));
}

Result<Eigen3> symmetric_eigen(const Mat3& m) {
    if (!all_finite(m)) {
        Diagnostic d("MK1303_NONFINITE_COVARIANCE", Severity::error, "matrix is not finite");
        return Result<Eigen3>::failure(ErrorCode::invalid_data, std::move(d));
    }

    // Symmetrise, so a matrix that is symmetric only up to round-off is handled cleanly.
    double a[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            a[i][j] = 0.5 * (m[i * 3 + j] + m[j * 3 + i]);
        }
    }

    // Jacobi eigenvalue iteration. For a 3x3 symmetric matrix this converges in a handful of
    // sweeps; the fixed cap is far more than needed and keeps the routine bounded and
    // deterministic (no data-dependent iteration count).
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    for (int sweep = 0; sweep < 50; ++sweep) {
        double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-30) {
            break;  // off-diagonal is numerically zero
        }
        // Zero each off-diagonal (p,q) in turn with a Givens rotation.
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(a[p][q]) < 1e-300) {
                    continue;
                }
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t =
                    (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;

                // Apply the rotation to A (both sides) and accumulate into V.
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p];
                    const double akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k];
                    const double aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p];
                    const double vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }

    // Collect (eigenvalue, eigenvector-column) pairs and sort descending, so the decomposition
    // is stable run to run rather than depending on iteration order.
    std::array<std::pair<double, std::array<double, 3>>, 3> pairs;
    for (int i = 0; i < 3; ++i) {
        pairs[i] = {a[i][i], {v[0][i], v[1][i], v[2][i]}};
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& l, const auto& r) { return l.first > r.first; });

    Eigen3 result;
    for (int i = 0; i < 3; ++i) {
        result.values[i] = pairs[i].first;
        result.vectors[0 * 3 + i] = pairs[i].second[0];
        result.vectors[1 * 3 + i] = pairs[i].second[1];
        result.vectors[2 * 3 + i] = pairs[i].second[2];
    }
    return Result<Eigen3>::success(result);
}

Result<RotationScale> rotation_scale_from_covariance(const Mat3& sigma) {
    auto eigen = symmetric_eigen(sigma);
    if (!eigen.has_value()) {
        return Result<RotationScale>::failure(eigen.error_code(), eigen.diagnostics());
    }
    const Eigen3& e = eigen.value();

    // Eigenvalues are the squared scales. A small negative from round-off clamps to the minimum
    // scale; a substantial negative means the input was not a valid (positive-semidefinite)
    // covariance, which is an error rather than something to sweep under the rug.
    Vec3 scale{};
    const double tolerance = -1e-9 * std::max({std::fabs(e.values[0]), std::fabs(e.values[2]), 1.0});
    for (int i = 0; i < 3; ++i) {
        double lambda = e.values[i];
        if (lambda < tolerance) {
            Diagnostic d("MK1304_NOT_POSITIVE_SEMIDEFINITE", Severity::error,
                         "covariance has a substantially negative eigenvalue; it is not a valid "
                         "covariance");
            d.with_context("eigenvalue", lambda);
            return Result<RotationScale>::failure(ErrorCode::invalid_data, std::move(d));
        }
        if (lambda < 0.0) {
            lambda = 0.0;
        }
        scale[i] = std::max(std::sqrt(lambda), tol::kMinScale);
    }

    // The eigenvector basis may be left-handed (a reflection). A quaternion can only encode a
    // proper rotation, so fold the reflection away by flipping the last column. This preserves
    // Σ exactly, because the covariance is invariant to the sign of an eigenvector.
    Mat3 basis = e.vectors;
    if (determinant(basis) < 0.0) {
        basis[0 * 3 + 2] = -basis[0 * 3 + 2];
        basis[1 * 3 + 2] = -basis[1 * 3 + 2];
        basis[2 * 3 + 2] = -basis[2 * 3 + 2];
    }

    auto rotation = from_matrix(basis);
    if (!rotation.has_value()) {
        return Result<RotationScale>::failure(rotation.error_code(), rotation.diagnostics());
    }

    return Result<RotationScale>::success(RotationScale{rotation.value(), scale});
}

Result<RotationScale> affine_transform_gaussian(const Mat3& linear, const Quat& rotation,
                                               const Vec3& scale) {
    if (!all_finite(linear)) {
        Diagnostic d("MK1305_NONFINITE_TRANSFORM", Severity::error,
                     "affine linear part is not finite");
        return Result<RotationScale>::failure(ErrorCode::invalid_data, std::move(d));
    }
    // A (near-)singular transform collapses the Gaussian to a lower dimension, which has no valid
    // positive-scale decomposition. Reject it rather than emitting a degenerate scale.
    if (std::fabs(determinant(linear)) < 1e-18) {
        Diagnostic d("MK1306_SINGULAR_TRANSFORM", Severity::error,
                     "affine transform is singular; the Gaussian would collapse");
        return Result<RotationScale>::failure(ErrorCode::invalid_data, std::move(d));
    }

    auto sigma = covariance_from_rotation_scale(rotation, scale);
    if (!sigma.has_value()) {
        return Result<RotationScale>::failure(sigma.error_code(), sigma.diagnostics());
    }

    // Σ' = A Σ Aᵀ. This is the whole point: the shape transforms through the covariance, not by
    // multiplying the quaternion (valid only for a pure rotation) or scaling the components
    // (valid only for an axis-aligned scale). This is correct for rotation, non-uniform scale,
    // shear, and reflection alike.
    const Mat3 transformed = matmul(matmul(linear, sigma.value()), transpose(linear));
    return rotation_scale_from_covariance(transformed);
}

}  // namespace melkor::math
