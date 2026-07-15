#include "melkor/math/sh_rotation.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace melkor::math {

namespace {

constexpr double kPi = 3.14159265358979323846;

// 3DGS/KHR real-SH normalization constants (the same basis Melkor's DC<->RGB relation uses).
constexpr double kC0 = 0.28209479177387814;
constexpr double kC1 = 0.4886025119029199;
constexpr double kC2[5] = {1.0925484305920792, -1.0925484305920792, 0.31539156525252005,
                           -1.0925484305920792, 0.5462742152960396};
constexpr double kC3[7] = {-0.5900435899266435, 2.890611442640554,  -0.4570457994644658,
                           0.3731763325901154,  -0.4570457994644658, 1.445305721320277,
                           -0.5900435899266435};

// Evaluates the real SH basis (degrees 0-3, 16 coefficients) at a unit direction, in the exact
// convention the 3DGS forward pass uses: eval(d, coeffs) == sum_k coeffs[k] * basis(d)[k]. The
// m-ordering within each band is -l..+l.
std::array<double, 16> sh_basis(const Vec3& d) {
    const double x = d[0], y = d[1], z = d[2];
    const double xx = x * x, yy = y * y, zz = z * z;
    std::array<double, 16> b{};
    b[0] = kC0;

    b[1] = -kC1 * y;
    b[2] = kC1 * z;
    b[3] = -kC1 * x;

    b[4] = kC2[0] * x * y;
    b[5] = kC2[1] * y * z;
    b[6] = kC2[2] * (2.0 * zz - xx - yy);
    b[7] = kC2[3] * x * z;
    b[8] = kC2[4] * (xx - yy);

    b[9] = kC3[0] * y * (3.0 * xx - yy);
    b[10] = kC3[1] * x * y * z;
    b[11] = kC3[2] * y * (4.0 * zz - xx - yy);
    b[12] = kC3[3] * z * (2.0 * zz - 3.0 * xx - 3.0 * yy);
    b[13] = kC3[4] * x * (4.0 * zz - xx - yy);
    b[14] = kC3[5] * z * (xx - yy);
    b[15] = kC3[6] * x * (xx - 3.0 * yy);
    return b;
}

Vec3 mat_vec(const Mat3& m, const Vec3& v) {
    return Vec3{m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

// A deterministic set of well-spread unit directions (Fibonacci sphere). Overdetermines each band's
// (2l+1) unknowns so the least-squares fit is well-conditioned and effectively exact.
std::vector<Vec3> sample_directions(std::size_t n) {
    std::vector<Vec3> dirs;
    dirs.reserve(n);
    const double golden = kPi * (3.0 - std::sqrt(5.0));  // golden angle
    for (std::size_t i = 0; i < n; ++i) {
        const double z = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(n);
        const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double phi = static_cast<double>(i) * golden;
        dirs.push_back(Vec3{r * std::cos(phi), r * std::sin(phi), z});
    }
    return dirs;
}

// Solves the K x K linear system A x = B for x, with B (and x) carrying `cols` right-hand sides,
// by Gauss-Jordan elimination with partial pivoting. A and B are row-major, K rows. Returns false
// if A is singular. On success, `b` holds the solution (K x cols, row-major).
bool solve_linear(std::vector<double>& a, std::vector<double>& b, std::size_t k, std::size_t cols) {
    for (std::size_t col = 0; col < k; ++col) {
        // Partial pivot.
        std::size_t pivot = col;
        double best = std::fabs(a[col * k + col]);
        for (std::size_t r = col + 1; r < k; ++r) {
            const double v = std::fabs(a[r * k + col]);
            if (v > best) {
                best = v;
                pivot = r;
            }
        }
        if (best < 1e-14) return false;
        if (pivot != col) {
            for (std::size_t c = 0; c < k; ++c) std::swap(a[col * k + c], a[pivot * k + c]);
            for (std::size_t c = 0; c < cols; ++c) std::swap(b[col * cols + c], b[pivot * cols + c]);
        }
        const double diag = a[col * k + col];
        for (std::size_t c = 0; c < k; ++c) a[col * k + c] /= diag;
        for (std::size_t c = 0; c < cols; ++c) b[col * cols + c] /= diag;
        for (std::size_t r = 0; r < k; ++r) {
            if (r == col) continue;
            const double factor = a[r * k + col];
            if (factor == 0.0) continue;
            for (std::size_t c = 0; c < k; ++c) a[r * k + c] -= factor * a[col * k + c];
            for (std::size_t c = 0; c < cols; ++c) b[r * cols + c] -= factor * b[col * cols + c];
        }
    }
    return true;
}

// Builds the band-l rotation matrix M (K x K, K = 2l+1, row-major) such that, for any coefficient
// vector c, the rotated coefficients c' = M c reproduce the source radiance viewed in the rotated
// frame: sum_m c'_m Y_l^m(d) == sum_m c_m Y_l^m(R^{-1} d). Solving A M = B with A[i][m]=Y_l^m(d_i)
// and B[i][m]=Y_l^m(R^{-1} d_i) gives M via the normal equations (A^T A) M = A^T B.
std::vector<double> band_matrix(const Mat3& rotation, std::uint32_t l,
                                const std::vector<Vec3>& dirs) {
    const std::size_t k = 2u * l + 1u;
    const std::size_t base = l * l;  // first flat coefficient index of band l
    // R is orthonormal, so R^{-1} = R^T (row-major transpose).
    const Mat3 inv{rotation[0], rotation[3], rotation[6], rotation[1], rotation[4],
                   rotation[7], rotation[2], rotation[5], rotation[8]};

    // A (N x k) and B (N x k).
    const std::size_t n = dirs.size();
    std::vector<double> A(n * k), B(n * k);
    for (std::size_t i = 0; i < n; ++i) {
        const auto ad = sh_basis(dirs[i]);
        const auto bd = sh_basis(mat_vec(inv, dirs[i]));
        for (std::size_t m = 0; m < k; ++m) {
            A[i * k + m] = ad[base + m];
            B[i * k + m] = bd[base + m];
        }
    }

    // Normal equations: AtA (k x k) = A^T A, AtB (k x k) = A^T B.
    std::vector<double> AtA(k * k, 0.0), AtB(k * k, 0.0);
    for (std::size_t r = 0; r < k; ++r) {
        for (std::size_t c = 0; c < k; ++c) {
            double sa = 0.0, sb = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                sa += A[i * k + r] * A[i * k + c];
                sb += A[i * k + r] * B[i * k + c];
            }
            AtA[r * k + c] = sa;
            AtB[r * k + c] = sb;
        }
    }
    solve_linear(AtA, AtB, k, k);  // AtB now holds M (k x k)
    return AtB;
}

}  // namespace

bool is_proper_rotation(const Mat3& m, double tol) {
    // Columns orthonormal.
    auto col = [&](int c) { return Vec3{m[c], m[3 + c], m[6 + c]}; };
    auto dot = [](const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const Vec3 c0 = col(0), c1 = col(1), c2 = col(2);
    if (std::fabs(dot(c0, c0) - 1.0) > tol || std::fabs(dot(c1, c1) - 1.0) > tol ||
        std::fabs(dot(c2, c2) - 1.0) > tol) {
        return false;
    }
    if (std::fabs(dot(c0, c1)) > tol || std::fabs(dot(c0, c2)) > tol || std::fabs(dot(c1, c2)) > tol) {
        return false;
    }
    // Determinant +1 (proper, not a reflection).
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    return std::fabs(det - 1.0) <= tol;
}

Result<ShRotation> ShRotation::create(const Mat3& rotation, std::uint32_t degree) {
    if (degree > kMaxDegree) {
        Diagnostic d("MK1701_SH_ROTATION_DEGREE", Severity::error,
                     "SH rotation supports degrees 0-3; degree " + std::to_string(degree) +
                         " is not yet implemented");
        return Result<ShRotation>::failure(ErrorCode::unsupported_feature, std::move(d));
    }
    if (!is_proper_rotation(rotation)) {
        Diagnostic d("MK1702_SH_ROTATION_NOT_ROTATION", Severity::error,
                     "SH rotation requires a proper rotation (orthonormal, determinant +1)");
        return Result<ShRotation>::failure(ErrorCode::invalid_argument, std::move(d));
    }

    ShRotation out;
    out.degree_ = degree;
    const auto dirs = sample_directions(64);
    out.bands_[0] = std::vector<double>{1.0};  // degree 0 is rotation-invariant
    for (std::uint32_t l = 1; l <= degree; ++l) {
        out.bands_[l] = band_matrix(rotation, l, dirs);
    }
    return Result<ShRotation>::success(std::move(out));
}

void ShRotation::rotate_block(float* block, std::size_t channels) const {
    // Degree 0 (coefficient 0) is invariant; start at band 1.
    for (std::uint32_t l = 1; l <= degree_; ++l) {
        const std::size_t k = 2u * l + 1u;
        const std::size_t base = static_cast<std::size_t>(l) * l;  // first coefficient of band l
        const std::vector<double>& m = bands_[l];
        for (std::size_t ch = 0; ch < channels; ++ch) {
            // Gather this band's coefficients for this channel.
            double in[7];
            for (std::size_t j = 0; j < k; ++j) {
                in[j] = static_cast<double>(block[(base + j) * channels + ch]);
            }
            for (std::size_t row = 0; row < k; ++row) {
                double acc = 0.0;
                for (std::size_t colm = 0; colm < k; ++colm) {
                    acc += m[row * k + colm] * in[colm];
                }
                block[(base + row) * channels + ch] = static_cast<float>(acc);
            }
        }
    }
}

const std::vector<double>& ShRotation::band(std::uint32_t l) const { return bands_[l]; }

}  // namespace melkor::math
