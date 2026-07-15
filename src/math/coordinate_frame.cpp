#include "melkor/math/coordinate_frame.hpp"

#include <cmath>

namespace melkor::math {
namespace {

double det3(const Mat3& m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

// Is m orthogonal, i.e. m mᵀ == I within tolerance? A coordinate frame's basis change must be
// orthogonal; anything else is not a frame.
bool is_orthogonal(const Mat3& m) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k) {
                dot += m[i * 3 + k] * m[j * 3 + k];
            }
            const double expected = (i == j) ? 1.0 : 0.0;
            if (std::fabs(dot - expected) > 1e-6) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

CoordinateFrame canonical_frame() {
    CoordinateFrame frame;
    frame.id = "gltf-luf";
    frame.to_canonical = Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
    frame.unit_to_meter = 1.0;
    frame.includes_reflection = false;
    return frame;
}

Result<CoordinateFrame> frame_from_basis(std::string id, const Mat3& to_canonical,
                                        double unit_to_meter) {
    for (double v : to_canonical) {
        if (!std::isfinite(v)) {
            Diagnostic d("MK1401_NONFINITE_FRAME", Severity::error,
                         "coordinate-frame basis is not finite");
            return Result<CoordinateFrame>::failure(ErrorCode::invalid_data, std::move(d));
        }
    }
    if (!std::isfinite(unit_to_meter) || unit_to_meter <= 0.0) {
        Diagnostic d("MK1402_BAD_UNIT_SCALE", Severity::error,
                     "coordinate-frame unit scale must be finite and positive");
        return Result<CoordinateFrame>::failure(ErrorCode::invalid_data, std::move(d));
    }
    if (!is_orthogonal(to_canonical)) {
        Diagnostic d("MK1403_NON_ORTHOGONAL_FRAME", Severity::error,
                     "coordinate-frame basis is not orthogonal; it is not a valid frame");
        return Result<CoordinateFrame>::failure(ErrorCode::invalid_data, std::move(d));
    }

    CoordinateFrame frame;
    frame.id = std::move(id);
    frame.to_canonical = to_canonical;
    frame.unit_to_meter = unit_to_meter;
    // A negative determinant means the basis change mirrors space. Flagged, not applied blindly:
    // a reflected frame needs the separately tested SH reflection, and the covariance transform
    // already handles the mean/shape correctly.
    frame.includes_reflection = det3(to_canonical) < 0.0;
    return Result<CoordinateFrame>::success(frame);
}

Result<CoordinateFrame> frame_by_id(const std::string& id) {
    if (id == "gltf-luf") {
        return Result<CoordinateFrame>::success(canonical_frame());
    }
    // Only the canonical frame is registered in the core. Specific upstream frames -- SPZ's
    // legacy default, COLMAP's camera and world frames -- are defined by their adapters against
    // the exact upstream specification, not guessed at here, because an inexact frame silently
    // mirrors or rotates every scene that passes through it.
    Diagnostic d("MK1404_UNKNOWN_FRAME", Severity::error, "unknown coordinate frame");
    d.with_context("id", id);
    d.with_context("note", std::string("Only 'gltf-luf' is registered in the core; other frames "
                                       "are supplied by their format adapters."));
    return Result<CoordinateFrame>::failure(ErrorCode::unsupported_feature, std::move(d));
}

Vec3 position_to_canonical(const CoordinateFrame& from, const Vec3& position) {
    // Basis change, then unit scale.
    Vec3 out{};
    for (int i = 0; i < 3; ++i) {
        double sum = 0.0;
        for (int j = 0; j < 3; ++j) {
            sum += from.to_canonical[i * 3 + j] * position[j];
        }
        out[i] = sum * from.unit_to_meter;
    }
    return out;
}

}  // namespace melkor::math
