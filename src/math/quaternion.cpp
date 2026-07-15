#include "melkor/math/quaternion.hpp"

#include <cmath>

namespace melkor::math {
namespace {

bool finite(const Quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

double dot3(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

}  // namespace

double norm(const Quat& q) {
    return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

Result<Quat> normalize(const Quat& q) {
    if (!finite(q)) {
        Diagnostic d("MK1201_NONFINITE_QUATERNION", Severity::error, "quaternion is not finite");
        return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
    }
    const double n = norm(q);
    if (n < tol::kQuatRejectNorm) {
        // A near-zero quaternion has no direction. Promoting it to identity would invent an
        // orientation the data never had, so this fails; an explicit repair step may substitute
        // identity, but that is the caller's recorded decision, not a silent default.
        Diagnostic d("MK1202_ZERO_QUATERNION", Severity::error,
                     "quaternion norm is below the rejection tolerance");
        d.with_context("norm", n);
        return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
    }
    const double inv = 1.0 / n;
    return Result<Quat>::success(Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv});
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
            Diagnostic d("MK1203_NONFINITE_MATRIX", Severity::error, "rotation matrix is not finite");
            return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
        }
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
    // Canonical sign: w >= 0. q and -q are the same rotation, so this makes serialisation
    // deterministic without changing the rotation.
    if (r.w < 0.0) {
        r.x = -r.x;
        r.y = -r.y;
        r.z = -r.z;
        r.w = -r.w;
    }
    return Result<Quat>::success(r);
}

Result<Quat> from_frame(const Vec3& ax, const Vec3& ay, const Vec3& az) {
    // Reject a frame that is not orthonormal within tolerance: a non-orthonormal frame does not
    // correspond to a rotation, and forcing a quaternion out of it would silently produce a
    // non-rotation. The tolerance is loose enough for a frame built from normalised cross
    // products but tight enough to catch a genuinely skewed frame.
    constexpr double kOrtho = 1e-3;
    const bool unit = std::fabs(dot3(ax, ax) - 1.0) < kOrtho &&
                      std::fabs(dot3(ay, ay) - 1.0) < kOrtho &&
                      std::fabs(dot3(az, az) - 1.0) < kOrtho;
    const bool orthogonal = std::fabs(dot3(ax, ay)) < kOrtho &&
                            std::fabs(dot3(ax, az)) < kOrtho &&
                            std::fabs(dot3(ay, az)) < kOrtho;
    if (!unit || !orthogonal) {
        Diagnostic d("MK1204_NON_ORTHONORMAL_FRAME", Severity::error,
                     "frame axes are not orthonormal");
        return Result<Quat>::failure(ErrorCode::invalid_data, std::move(d));
    }

    // Columns of the rotation matrix are the frame axes.
    Mat3 m{
        ax[0], ay[0], az[0],
        ax[1], ay[1], az[1],
        ax[2], ay[2], az[2],
    };
    return from_matrix(m);
}

double angular_distance(const Quat& a, const Quat& b) {
    // |dot| collapses the q/-q ambiguity: the same rotation reports zero distance. Clamp to
    // [0,1] before acos so floating error at the boundary does not produce a NaN.
    double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    if (d > 1.0) d = 1.0;
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
