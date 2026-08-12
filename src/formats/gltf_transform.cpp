#include "melkor/format/gltf_transform.hpp"

#include <cmath>

namespace melkor::format::gltf {

namespace {

// Row-major 3x3 times 3x3: C[r][c] = sum_k A[r][k] B[k][c].
math::Mat3 matmul(const math::Mat3& a, const math::Mat3& b) {
    math::Mat3 c{};
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 3; ++col) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += a[static_cast<std::size_t>(r) * 3 + k] *
                       b[static_cast<std::size_t>(k) * 3 + col];
            }
            c[static_cast<std::size_t>(r) * 3 + col] = sum;
        }
    }
    return c;
}

// Row-major 3x3 times a 3-vector.
math::Vec3 matvec(const math::Mat3& m, const math::Vec3& v) {
    math::Vec3 out{};
    for (int r = 0; r < 3; ++r) {
        out[static_cast<std::size_t>(r)] = m[static_cast<std::size_t>(r) * 3 + 0] * v[0] +
                                           m[static_cast<std::size_t>(r) * 3 + 1] * v[1] +
                                           m[static_cast<std::size_t>(r) * 3 + 2] * v[2];
    }
    return out;
}

}  // namespace

NodeTransform identity_transform() {
    return NodeTransform{};
}

Result<NodeTransform> local_node_transform(const NodeDesc& node) {
    NodeTransform t;
    if (node.matrix.has_value()) {
        const auto& m = node.matrix.value();  // column-major: m[col*4 + row]
        for (double value : m) {
            if (!std::isfinite(value)) {
                Diagnostic diagnostic("MK2140_GLTF_NODE_TRANSFORM", Severity::error,
                                      "node matrix contains a nonfinite value");
                return Result<NodeTransform>::failure(ErrorCode::invalid_data,
                                                      std::move(diagnostic));
            }
        }
        if (m[3] != 0.0 || m[7] != 0.0 || m[11] != 0.0 || m[15] != 1.0) {
            Diagnostic diagnostic("MK2140_GLTF_NODE_TRANSFORM", Severity::error,
                                  "node matrix is not an affine transform");
            return Result<NodeTransform>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                t.linear[static_cast<std::size_t>(r) * 3 + c] =
                    m[static_cast<std::size_t>(c) * 4 + r];
            }
        }
        t.translation = math::Vec3{m[12], m[13], m[14]};  // fourth column, rows 0..2
        return Result<NodeTransform>::success(t);
    }

    // T * R * S. Normalize the node quaternion before matrix conversion.
    math::Quat q{node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]};
    if (!math::is_unit(q)) {
        Diagnostic diagnostic("MK2140_GLTF_NODE_TRANSFORM", Severity::error,
                              "node rotation is not a unit quaternion");
        return Result<NodeTransform>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }
    for (double value : node.translation) {
        if (!std::isfinite(value)) {
            Diagnostic diagnostic("MK2140_GLTF_NODE_TRANSFORM", Severity::error,
                                  "node translation contains a nonfinite value");
            return Result<NodeTransform>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
    }
    for (double value : node.scale) {
        if (!std::isfinite(value)) {
            Diagnostic diagnostic("MK2140_GLTF_NODE_TRANSFORM", Severity::error,
                                  "node scale contains a nonfinite value");
            return Result<NodeTransform>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
    }
    auto unit = math::normalize(q);
    if (!unit.has_value()) {
        return Result<NodeTransform>::failure(unit.error_code(), unit.diagnostics());
    }
    const math::Mat3 r = math::to_matrix(unit.value());

    // linear = R * diag(scale): column c of R scaled by scale[c].
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            t.linear[static_cast<std::size_t>(row) * 3 + col] =
                r[static_cast<std::size_t>(row) * 3 + col] *
                node.scale[static_cast<std::size_t>(col)];
        }
    }
    t.translation = math::Vec3{node.translation[0], node.translation[1], node.translation[2]};
    return Result<NodeTransform>::success(t);
}

NodeTransform compose(const NodeTransform& parent, const NodeTransform& child) {
    NodeTransform out;
    out.linear = matmul(parent.linear, child.linear);
    const math::Vec3 rotated_child_t = matvec(parent.linear, child.translation);
    out.translation = math::Vec3{rotated_child_t[0] + parent.translation[0],
                                 rotated_child_t[1] + parent.translation[1],
                                 rotated_child_t[2] + parent.translation[2]};
    return out;
}

math::Vec3 apply_point(const NodeTransform& t, const math::Vec3& p) {
    const math::Vec3 linear = matvec(t.linear, p);
    return math::Vec3{linear[0] + t.translation[0], linear[1] + t.translation[1],
                      linear[2] + t.translation[2]};
}

}  // namespace melkor::format::gltf
