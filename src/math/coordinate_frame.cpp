#include "melkor/math/coordinate_frame.hpp"

#include <cstddef>
#include <cmath>
#include <utility>

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

Mat3 transpose(const Mat3& matrix) {
    return Mat3{matrix[0], matrix[3], matrix[6], matrix[1], matrix[4],
                matrix[7], matrix[2], matrix[5], matrix[8]};
}

Mat3 multiply_matrix(const Mat3& left, const Mat3& right) {
    Mat3 product{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            for (std::size_t inner = 0; inner < 3; ++inner) {
                product[row * 3 + column] += left[row * 3 + inner] * right[inner * 3 + column];
            }
        }
    }
    return product;
}

Result<Quat> convert_rotation(const CoordinateFrame& frame, const Quat& rotation,
                              bool to_canonical) {
    if (frame.includes_reflection) {
        Diagnostic diagnostic("MK1406_REFLECTING_ROTATION", Severity::error,
                              "a quaternion cannot represent a reflecting frame change");
        diagnostic.with_context("id", frame.id);
        return Result<Quat>::failure(ErrorCode::unsupported_feature, std::move(diagnostic));
    }
    auto normalized = normalize(rotation);
    if (!normalized.has_value())
        return normalized;
    const Mat3 basis = to_canonical ? frame.to_canonical : transpose(frame.to_canonical);
    return from_matrix(multiply_matrix(basis, to_matrix(normalized.value())));
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
    if (id.empty()) {
        Diagnostic d("MK1405_EMPTY_FRAME_ID", Severity::error, "coordinate-frame ID is empty");
        return Result<CoordinateFrame>::failure(ErrorCode::invalid_argument, std::move(d));
    }
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
    // Niantic SPZ defines RUB as Right-Up-Back. Conversion to glTF LUF flips X and Z.
    if (id == "spz-rub") {
        return frame_from_basis(id, Mat3{-1, 0, 0, 0, 1, 0, 0, 0, -1}, 1.0);
    }
    // Niantic SPZ describes common PLY data as RDF. Conversion to glTF LUF flips X and Y.
    // A PLY file does not declare this frame. The caller must select it explicitly.
    if (id == "ply-rdf") {
        return frame_from_basis(id, Mat3{-1, 0, 0, 0, -1, 0, 0, 0, 1}, 1.0);
    }
    Diagnostic d("MK1404_UNKNOWN_FRAME", Severity::error, "unknown coordinate frame");
    d.with_context("id", id);
    d.with_context("supported", std::string("gltf-luf, ply-rdf, spz-rub"));
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

Vec3 position_from_canonical(const CoordinateFrame& to, const Vec3& position) {
    const Mat3 inverse_basis = transpose(to.to_canonical);
    Vec3 out{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column)
            out[row] += inverse_basis[row * 3 + column] * position[column];
        out[row] /= to.unit_to_meter;
    }
    return out;
}

Result<Quat> rotation_to_canonical(const CoordinateFrame& from, const Quat& rotation) {
    return convert_rotation(from, rotation, true);
}

Result<Quat> rotation_from_canonical(const CoordinateFrame& to, const Quat& rotation) {
    return convert_rotation(to, rotation, false);
}

}  // namespace melkor::math
