#include "melkor/math/quaternion.hpp"

#include <algorithm>
#include <cmath>

namespace melkor::math {
namespace {

bool finite(const Quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

double dot3(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double determinant(const Mat3& m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

bool proper_rotation(const Mat3& m) {
    constexpr double kTolerance = 1e-6;
    const Vec3 columns[3] = {
        {m[0], m[3], m[6]},
        {m[1], m[4], m[7]},
        {m[2], m[5], m[8]},
    };
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dot3(columns[i], columns[i]) - 1.0) > kTolerance)
            return false;
        for (int j = i + 1; j < 3; ++j) {
            if (std::fabs(dot3(columns[i], columns[j])) > kTolerance)
                return false;
        }
    }
    return std::fabs(determinant(m) - 1.0) <= kTolerance;
}

bool should_negate(const Quat& q) {
    if (q.w != 0.0)
        return q.w < 0.0;
    if (q.z != 0.0)
        return q.z < 0.0;
    if (q.y != 0.0)
        return q.y < 0.0;
    return q.x < 0.0;
}

void clear_negative_zero(Quat& q) {
    if (q.x == 0.0)
        q.x = 0.0;
    if (q.y == 0.0)
        q.y = 0.0;
    if (q.z == 0.0)
        q.z = 0.0;
    if (q.w == 0.0)
        q.w = 0.0;
}

}  // namespace

double norm(const Quat& q) {
    return std::hypot(std::hypot(q.x, q.y), std::hypot(q.z, q.w));
}

Result<Quat> normalize(const Quat& q) {
    if (!finite(q)) {
        Diagnostic d("MK1201_NONFINITE_QUATERNION", Severity::error, "quaternion is not finite");
        return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
    }
    const double magnitude =
        std::max({std::fabs(q.x), std::fabs(q.y), std::fabs(q.z), std::fabs(q.w)});
    if (magnitude == 0.0) {
        Diagnostic d("MK1202_ZERO_QUATERNION", Severity::error,
                     "quaternion norm is below the rejection tolerance");
        d.with_context("norm", 0.0);
        return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
    }
    const Quat scaled{q.x / magnitude, q.y / magnitude, q.z / magnitude, q.w / magnitude};
    const double scaled_norm = std::sqrt(scaled.x * scaled.x + scaled.y * scaled.y +
                                         scaled.z * scaled.z + scaled.w * scaled.w);
    const double n = magnitude * scaled_norm;
    if (n < tol::kQuatRejectNorm) {
        // A near-zero quaternion has no direction. Promoting it to identity would invent an
        // orientation the data never had, so this fails; an explicit repair step may substitute
        // identity, but that is the caller's recorded decision, not a silent default.
        Diagnostic d("MK1202_ZERO_QUATERNION", Severity::error,
                     "quaternion norm is below the rejection tolerance");
        d.with_context("norm", n);
        return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
    }
    const double inv = 1.0 / scaled_norm;
    return Result<Quat>::success(
        Quat{scaled.x * inv, scaled.y * inv, scaled.z * inv, scaled.w * inv});
}

bool is_unit(const Quat& q) {
    return finite(q) && std::fabs(norm(q) - 1.0) <= tol::kQuatRenormalize;
}

Mat3 to_matrix(const Quat& q) {
    // Standard quaternion-to-rotation-matrix, assuming q is (near) unit. Row-major.
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    const double xx = x * x, yy = y * y, zz = z * z;
    const double xy = x * y, xz = x * z, yz = y * z;
    const double wx = w * x, wy = w * y, wz = w * z;
    return Mat3{
        1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz),       2.0 * (xz + wy),
        2.0 * (xy + wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx),
        2.0 * (xz - wy),       2.0 * (yz + wx),       1.0 - 2.0 * (xx + yy),
    };
}

Result<Quat> from_matrix(const Mat3& m) {
    for (double v : m) {
        if (!std::isfinite(v)) {
            Diagnostic d("MK1203_NONFINITE_MATRIX", Severity::error,
                         "rotation matrix is not finite");
            return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
        }
    }
    if (!proper_rotation(m)) {
        Diagnostic d("MK1206_MATRIX_NOT_ROTATION", Severity::error,
                     "matrix is not a proper rotation");
        return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
    }

    // Branch on the largest diagonal term so the divisor is never near zero. The naive
    // `w = sqrt(1 + trace)/2` loses all precision as the trace approaches -1 (a 180-degree
    // rotation), where w -> 0; picking the largest component to solve for first avoids that.
    const double m00 = m[0], m01 = m[1], m02 = m[2];
    const double m10 = m[3], m11 = m[4], m12 = m[5];
    const double m20 = m[6], m21 = m[7], m22 = m[8];
    const double trace = m00 + m11 + m22;

    Quat q;
    if (trace > 0.0) {
        double s = std::sqrt(trace + 1.0) * 2.0;  // s = 4w
        q.w = 0.25 * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;  // s = 4x
        q.w = (m21 - m12) / s;
        q.x = 0.25 * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;  // s = 4y
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25 * s;
        q.z = (m12 + m21) / s;
    } else {
        double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;  // s = 4z
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25 * s;
    }

    auto normalized = normalize(q);
    if (!normalized.has_value()) {
        return normalized;
    }
    Quat r = normalized.value();
    // Canonical sign: w >= 0. q and -q are the same rotation, so this makes serialization
    // deterministic without changing the rotation.
    if (should_negate(r)) {
        r.x = -r.x;
        r.y = -r.y;
        r.z = -r.z;
        r.w = -r.w;
    }
    clear_negative_zero(r);
    return Result<Quat>::success(r);
}

Result<Quat> from_frame(const Vec3& ax, const Vec3& ay, const Vec3& az) {
    // Reject a frame that is not orthonormal within tolerance: a non-orthonormal frame does not
    // correspond to a rotation, and forcing a quaternion out of it would silently produce a
    // non-rotation. The tolerance is loose enough for a frame built from normalized cross
    // products but tight enough to catch a genuinely skewed frame.
    constexpr double kOrtho = 1e-6;
    const bool unit = std::fabs(dot3(ax, ax) - 1.0) < kOrtho &&
                      std::fabs(dot3(ay, ay) - 1.0) < kOrtho &&
                      std::fabs(dot3(az, az) - 1.0) < kOrtho;
    const bool orthogonal = std::fabs(dot3(ax, ay)) < kOrtho && std::fabs(dot3(ax, az)) < kOrtho &&
                            std::fabs(dot3(ay, az)) < kOrtho;
    if (!unit || !orthogonal) {
        Diagnostic d("MK1204_NON_ORTHONORMAL_FRAME", Severity::error,
                     "frame axes are not orthonormal");
        return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
    }

    // Orthonormal is not enough: a left-handed (reflection) frame is orthonormal but has
    // determinant -1 and is not a proper rotation. from_matrix would silently misconvert it, so
    // reject it. For an orthonormal frame the scalar triple product (ax x ay) . az is exactly the
    // determinant, +1 for right-handed and -1 for left-handed.
    const Vec3 axay{ax[1] * ay[2] - ax[2] * ay[1], ax[2] * ay[0] - ax[0] * ay[2],
                    ax[0] * ay[1] - ax[1] * ay[0]};
    if (dot3(axay, az) < 0.0) {
        Diagnostic d("MK1205_LEFT_HANDED_FRAME", Severity::error,
                     "frame is left-handed (a reflection), not a proper rotation");
        return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
    }

    // Columns of the rotation matrix are the frame axes.
    Mat3 m{
        ax[0], ay[0], az[0], ax[1], ay[1], az[1], ax[2], ay[2], az[2],
    };
    return from_matrix(m);
}

double angular_distance(const Quat& a, const Quat& b) {
    // |dot| collapses the q/-q ambiguity: the same rotation reports zero distance. Clamp to
    // [0,1] before acos so floating error at the boundary does not produce a NaN.
    double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    if (d > 1.0)
        d = 1.0;
    return 2.0 * std::acos(d);
}

Quat multiply(const Quat& a, const Quat& b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

}  // namespace melkor::math
