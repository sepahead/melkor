#include "melkor/math/covariance.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace melkor::math {
namespace {

bool all_finite(const Mat3& m) {
    for (double v : m) {
        if (!std::isfinite(v))
            return false;
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

double max_abs(const Mat3& m) {
    double maximum = 0.0;
    for (double value : m)
        maximum = std::max(maximum, std::fabs(value));
    return maximum;
}

Mat3 divided_by(const Mat3& m, double divisor) {
    Mat3 result{};
    for (std::size_t i = 0; i < m.size(); ++i)
        result[i] = m[i] / divisor;
    return result;
}

}  // namespace

Result<Mat3> covariance_from_rotation_scale(const Quat& rotation, const Vec3& scale) {
    if (!is_unit(rotation)) {
        Diagnostic d("MK1410_NON_UNIT_ROTATION", Severity::error,
                     "rotation must be a unit quaternion");
        return Result<Mat3>::failure(ErrorCode::invalid_data, std::move(d));
    }
    for (double s : scale) {
        if (!std::isfinite(s) || s < 0.0) {
            Diagnostic d("MK1411_NEGATIVE_SCALE", Severity::error,
                         "covariance scales must be finite and nonnegative");
            d.with_context("scale", s);
            return Result<Mat3>::failure(ErrorCode::invalid_data, std::move(d));
        }
    }

    // Σ = R diag(s²) Rᵀ. Build R diag(s²) by scaling R's columns, then multiply by Rᵀ.
    // Normalize an accepted near-unit input before matrix conversion. This keeps scale separate
    // from rotation and makes the result symmetric under the documented input tolerance.
    auto normalized_rotation = normalize(rotation);
    if (!normalized_rotation.has_value()) {
        return Result<Mat3>::failure(normalized_rotation.error_code(),
                                     normalized_rotation.diagnostics());
    }
    const Mat3 r = to_matrix(normalized_rotation.value());
    const std::array<double, 3> s2{scale[0] * scale[0], scale[1] * scale[1], scale[2] * scale[2]};
    if (!std::isfinite(s2[0]) || !std::isfinite(s2[1]) || !std::isfinite(s2[2])) {
        Diagnostic d("MK1416_SCALE_SQUARE_OVERFLOW", Severity::error,
                     "a covariance scale square is not finite");
        return Result<Mat3>::failure(ErrorCode::invalid_data, std::move(d));
    }
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
        Diagnostic d("MK1412_NONFINITE_COVARIANCE", Severity::error, "matrix is not finite");
        return Result<Eigen3>::failure(ErrorCode::invalid_data, std::move(d));
    }

    const double matrix_scale = max_abs(m);
    if (matrix_scale == 0.0) {
        Eigen3 zero;
        zero.values = Vec3{0.0, 0.0, 0.0};
        zero.vectors = Mat3{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
        return Result<Eigen3>::success(zero);
    }

    // Scale first. This prevents overflow in the convergence and rotation calculations.
    const Mat3 scaled = divided_by(m, matrix_scale);

    // Symmetrize a matrix that differs only through round-off.
    double a[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            a[i][j] = 0.5 * (scaled[i * 3 + j] + scaled[j * 3 + i]);
        }
    }

    // Jacobi eigenvalue iteration. For a 3x3 symmetric matrix this converges in a handful of
    // sweeps; the fixed cap is far more than needed and keeps the routine bounded and
    // deterministic (no data-dependent iteration count).
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    for (int sweep = 0; sweep < 50; ++sweep) {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        const double diag = a[0][0] * a[0][0] + a[1][1] * a[1][1] + a[2][2] * a[2][2];
        // Converged when the off-diagonal energy is negligible *relative to* the matrix magnitude.
        // The Frobenius norm (diag + 2*off) is invariant under the Givens rotations, so this is
        // scale-invariant; an absolute threshold would wrongly declare a small-magnitude covariance
        // already diagonal and skip every rotation, returning the raw diagonal as eigenvalues.
        if (off <= 1e-30 * (diag + 2.0 * off)) {
            break;  // off-diagonal is at machine precision relative to the matrix
        }
        // Zero each off-diagonal (p,q) in turn with a Givens rotation.
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(a[p][q]) < 1e-300) {
                    continue;
                }
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t =
                    (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::hypot(theta, 1.0));
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
    // Use a stable insertion sort. Equal eigenvalues keep their original axis order.
    // This rule makes the derived quaternion deterministic across standard libraries.
    for (std::size_t index = 1; index < pairs.size(); ++index) {
        auto value = pairs[index];
        std::size_t position = index;
        while (position > 0 && value.first > pairs[position - 1].first) {
            pairs[position] = pairs[position - 1];
            --position;
        }
        pairs[position] = value;
    }

    Eigen3 result;
    for (int i = 0; i < 3; ++i) {
        result.values[i] = pairs[i].first * matrix_scale;
        if (!std::isfinite(result.values[i])) {
            Diagnostic d("MK1419_EIGENVALUE_OVERFLOW", Severity::error,
                         "matrix eigenvalue does not fit in finite double precision");
            return Result<Eigen3>::failure(ErrorCode::invalid_data, std::move(d));
        }
        result.vectors[0 * 3 + i] = pairs[i].second[0];
        result.vectors[1 * 3 + i] = pairs[i].second[1];
        result.vectors[2 * 3 + i] = pairs[i].second[2];
    }
    return Result<Eigen3>::success(result);
}

Result<RotationScale> rotation_scale_from_covariance(const Mat3& sigma) {
    if (!all_finite(sigma)) {
        Diagnostic d("MK1412_NONFINITE_COVARIANCE", Severity::error,
                     "covariance matrix is not finite");
        return Result<RotationScale>::failure(ErrorCode::invalid_data, std::move(d));
    }
    const double magnitude = max_abs(sigma);
    const double symmetry_tolerance = 1e-10 * magnitude;
    if (std::fabs(sigma[1] - sigma[3]) > symmetry_tolerance ||
        std::fabs(sigma[2] - sigma[6]) > symmetry_tolerance ||
        std::fabs(sigma[5] - sigma[7]) > symmetry_tolerance) {
        Diagnostic d("MK1415_NONSYMMETRIC_COVARIANCE", Severity::error,
                     "covariance matrix is not symmetric");
        return Result<RotationScale>::failure(ErrorCode::invalid_data, std::move(d));
    }
    auto eigen = symmetric_eigen(sigma);
    if (!eigen.has_value()) {
        return Result<RotationScale>::failure(eigen.error_code(), eigen.diagnostics());
    }
    const Eigen3& e = eigen.value();

    // Eigenvalues are the squared scales. A small negative from round-off clamps to zero.
    // A substantial negative means the input was not a valid positive-semidefinite
    // covariance, which is an error rather than something to sweep under the rug.
    Vec3 scale{};
    const double eigen_scale =
        std::max({std::fabs(e.values[0]), std::fabs(e.values[1]), std::fabs(e.values[2])});
    const double tolerance = -1e-12 * eigen_scale;
    for (int i = 0; i < 3; ++i) {
        double lambda = e.values[i];
        if (lambda < tolerance) {
            Diagnostic d("MK1413_NOT_POSITIVE_SEMIDEFINITE", Severity::error,
                         "covariance has a substantially negative eigenvalue; it is not a valid "
                         "covariance");
            d.with_context("eigenvalue", lambda);
            return Result<RotationScale>::failure(ErrorCode::invalid_data, std::move(d));
        }
        if (lambda < 0.0) {
            lambda = 0.0;
        }
        scale[i] = std::sqrt(lambda);
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
        Diagnostic d("MK1414_NONFINITE_TRANSFORM", Severity::error,
                     "affine linear part is not finite");
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
    if (!all_finite(transformed)) {
        Diagnostic d("MK1417_COVARIANCE_TRANSFORM_OVERFLOW", Severity::error,
                     "transformed covariance is not finite");
        return Result<RotationScale>::failure(ErrorCode::invalid_data, std::move(d));
    }
    return rotation_scale_from_covariance(transformed);
}

Result<Mat3> rotation_from_linear(const Mat3& m) {
    if (!all_finite(m)) {
        Diagnostic d("MK1414_NONFINITE_TRANSFORM", Severity::error, "linear map is not finite");
        return Result<Mat3>::failure(ErrorCode::invalid_data, std::move(d));
    }
    const double linear_scale = max_abs(m);
    if (linear_scale == 0.0) {
        Diagnostic d("MK1418_NO_ROTATION_COMPONENT", Severity::error,
                     "linear map is singular; it has no rotation component");
        return Result<Mat3>::failure(ErrorCode::invalid_data, std::move(d));
    }
    const Mat3 normalized = divided_by(m, linear_scale);
    // A reflection (negative determinant) has no proper-rotation component. This is a sign test,
    // not a magnitude test: the determinant scales as the cube of the map's overall scale, so an
    // absolute threshold would wrongly reject a valid rotation combined with a small uniform scale.
    if (determinant(normalized) <= 0.0) {
        Diagnostic d("MK1418_NO_ROTATION_COMPONENT", Severity::error,
                     "linear map is a reflection or singular; it has no proper-rotation component");
        return Result<Mat3>::failure(ErrorCode::invalid_data, std::move(d));
    }

    // Eigendecompose MᵀM = V diag(λ) Vᵀ (symmetric positive-definite for a non-singular M), then
    // (MᵀM)^(-1/2) = V diag(1/√λ) Vᵀ and R = M (MᵀM)^(-1/2).
    const Mat3 mtm = matmul(transpose(normalized), normalized);
    auto eig = symmetric_eigen(mtm);
    if (!eig.has_value()) {
        return Result<Mat3>::failure(eig.error_code(), eig.diagnostics());
    }
    // Near-singularity is judged by the condition number, not an absolute floor, so the check is
    // invariant to the map's overall scale. `values` are the squared singular values, descending.
    if (eig.value().values[2] <= 1e-24 * eig.value().values[0]) {
        Diagnostic d("MK1418_NO_ROTATION_COMPONENT", Severity::error,
                     "linear map is numerically singular; no stable rotation component");
        return Result<Mat3>::failure(ErrorCode::invalid_data, std::move(d));
    }

    const Mat3& v = eig.value().vectors;
    // vs = V diag(1/√λ): scale column j of V by 1/√λ_j.
    Mat3 vs{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            vs[static_cast<std::size_t>(row) * 3 + col] =
                v[static_cast<std::size_t>(row) * 3 + col] / std::sqrt(eig.value().values[col]);
        }
    }
    const Mat3 pinv = matmul(vs, transpose(v));  // (MᵀM)^(-1/2), symmetric
    const Mat3 r = matmul(normalized, pinv);

    // Guard: the result must be a proper rotation (orthonormal, det +1). If numerical trouble left
    // it otherwise, refuse rather than return a subtly-wrong "rotation".
    const Mat3 rtr = matmul(transpose(r), r);
    const double orth = std::fabs(rtr[0] - 1.0) + std::fabs(rtr[4] - 1.0) +
                        std::fabs(rtr[8] - 1.0) + std::fabs(rtr[1]) + std::fabs(rtr[2]) +
                        std::fabs(rtr[5]);
    if (orth > 1e-6 || std::fabs(determinant(r) - 1.0) > 1e-6) {
        Diagnostic d("MK1418_NO_ROTATION_COMPONENT", Severity::error,
                     "polar decomposition did not yield a proper rotation");
        return Result<Mat3>::failure(ErrorCode::invalid_data, std::move(d));
    }
    return Result<Mat3>::success(r);
}

}  // namespace melkor::math
