#pragma once

// Include tiny_gltf.h before this internal header. TinyGLTF's implementation
// mode pulls in unguarded stb implementation headers, so including it twice in
// the same translation unit is not safe.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

namespace melkor::gltf_scene {

using Matrix = std::array<double, 16>;  // glTF/OpenGL column-major

struct MeshInstance {
    size_t mesh_index = 0;
    Matrix world{};
};

struct TraversalResult {
    std::vector<MeshInstance> instances;
    std::string error;

    [[nodiscard]] bool success() const { return error.empty(); }
};

inline bool fileHasGlbMagic(const std::string& filepath) {
    std::ifstream stream(filepath, std::ios::binary);
    unsigned char magic[4]{};
    stream.read(reinterpret_cast<char*>(magic), sizeof(magic));
    return stream.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
           magic[0] == 0x67 && magic[1] == 0x6c && magic[2] == 0x54 && magic[3] == 0x46;
}

inline std::string validateModelContract(const tinygltf::Model& model) {
    if (model.asset.version != "2.0") {
        return "Unsupported glTF asset version '" + model.asset.version +
               "' (Melkor supports glTF 2.0)";
    }
    if (!model.asset.minVersion.empty() && model.asset.minVersion != "2.0") {
        return "Unsupported glTF minimum version '" + model.asset.minVersion +
               "' (Melkor supports glTF 2.0)";
    }
    if (std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(),
                  "KHR_gaussian_splatting") != model.extensionsUsed.end()) {
        return "Unsupported glTF extension: KHR_gaussian_splatting";
    }
    if (!model.extensionsRequired.empty()) {
        return "Unsupported required glTF extension: " + model.extensionsRequired.front();
    }
    return {};
}

inline Matrix identity() {
    return {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
}

inline Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 4; ++row) {
            for (size_t k = 0; k < 4; ++k) {
                out[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
            }
        }
    }
    return out;
}

inline bool nodeMatrix(const tinygltf::Node& node, Matrix& out) {
    if (!node.matrix.empty()) {
        if (node.matrix.size() != 16 ||
            !std::all_of(node.matrix.begin(), node.matrix.end(),
                         [](double value) { return std::isfinite(value); })) {
            return false;
        }
        std::copy(node.matrix.begin(), node.matrix.end(), out.begin());
        return true;
    }

    const auto component = [](const std::vector<double>& values, size_t index, double fallback) {
        return index < values.size() ? values[index] : fallback;
    };
    if ((!node.translation.empty() && node.translation.size() != 3) ||
        (!node.rotation.empty() && node.rotation.size() != 4) ||
        (!node.scale.empty() && node.scale.size() != 3)) {
        return false;
    }

    const double tx = component(node.translation, 0, 0.0);
    const double ty = component(node.translation, 1, 0.0);
    const double tz = component(node.translation, 2, 0.0);
    const double sx = component(node.scale, 0, 1.0);
    const double sy = component(node.scale, 1, 1.0);
    const double sz = component(node.scale, 2, 1.0);
    double x = component(node.rotation, 0, 0.0);
    double y = component(node.rotation, 1, 0.0);
    double z = component(node.rotation, 2, 0.0);
    double w = component(node.rotation, 3, 1.0);
    if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz) || !std::isfinite(sx) ||
        !std::isfinite(sy) || !std::isfinite(sz) || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || !std::isfinite(w)) {
        return false;
    }
    const double max_q = std::max({std::abs(x), std::abs(y), std::abs(z), std::abs(w)});
    if (max_q > 0.0) {
        x /= max_q;
        y /= max_q;
        z /= max_q;
        w /= max_q;
        const double length = std::sqrt(x * x + y * y + z * z + w * w);
        x /= length;
        y /= length;
        z /= length;
        w /= length;
    } else {
        x = y = z = 0.0;
        w = 1.0;
    }

    const double xx = x * x, yy = y * y, zz = z * z;
    const double xy = x * y, xz = x * z, yz = y * z;
    const double wx = w * x, wy = w * y, wz = w * z;
    out = {
        (1.0 - 2.0 * (yy + zz)) * sx,
        (2.0 * (xy + wz)) * sx,
        (2.0 * (xz - wy)) * sx,
        0.0,
        (2.0 * (xy - wz)) * sy,
        (1.0 - 2.0 * (xx + zz)) * sy,
        (2.0 * (yz + wx)) * sy,
        0.0,
        (2.0 * (xz + wy)) * sz,
        (2.0 * (yz - wx)) * sz,
        (1.0 - 2.0 * (xx + yy)) * sz,
        0.0,
        tx,
        ty,
        tz,
        1.0,
    };
    return true;
}

inline bool transformPoint(const Matrix& matrix, const float in[3], float out[3]) {
    const double x = in[0], y = in[1], z = in[2];
    const double ox = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
    const double oy = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
    const double oz = matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14];
    const double ow = matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
    if (!std::isfinite(ox) || !std::isfinite(oy) || !std::isfinite(oz) || !std::isfinite(ow) ||
        std::abs(ow) < 1e-15) {
        return false;
    }
    out[0] = static_cast<float>(ox / ow);
    out[1] = static_cast<float>(oy / ow);
    out[2] = static_cast<float>(oz / ow);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

inline bool transformNormal(const Matrix& m, const float in[3], float out[3]) {
    const double a00 = m[0], a01 = m[4], a02 = m[8];
    const double a10 = m[1], a11 = m[5], a12 = m[9];
    const double a20 = m[2], a21 = m[6], a22 = m[10];
    const double c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22,
                 c02 = a10 * a21 - a11 * a20;
    const double c10 = a02 * a21 - a01 * a22, c11 = a00 * a22 - a02 * a20,
                 c12 = a01 * a20 - a00 * a21;
    const double c20 = a01 * a12 - a02 * a11, c21 = a02 * a10 - a00 * a12,
                 c22 = a00 * a11 - a01 * a10;
    const double det = a00 * c00 + a01 * c01 + a02 * c02;
    if (!std::isfinite(det) || std::abs(det) < 1e-15)
        return false;
    double x = (c00 * in[0] + c01 * in[1] + c02 * in[2]) / det;
    double y = (c10 * in[0] + c11 * in[1] + c12 * in[2]) / det;
    double z = (c20 * in[0] + c21 * in[1] + c22 * in[2]) / det;
    const double max_n = std::max({std::abs(x), std::abs(y), std::abs(z)});
    if (!std::isfinite(max_n) || max_n <= 0.0)
        return false;
    x /= max_n;
    y /= max_n;
    z /= max_n;
    const double length = std::sqrt(x * x + y * y + z * z);
    out[0] = static_cast<float>(x / length);
    out[1] = static_cast<float>(y / length);
    out[2] = static_cast<float>(z / length);
    return true;
}

inline TraversalResult activeMeshInstances(const tinygltf::Model& model) {
    TraversalResult result;
    if (model.nodes.empty() && model.scenes.empty()) {
        for (size_t i = 0; i < model.meshes.size(); ++i)
            result.instances.push_back({i, identity()});
        return result;
    }

    // Validate the complete node graph before selecting a scene. TinyGLTF's
    // model containers may still contain invalid indices, malformed transforms,
    // and cycles. Silently skipping any of those would turn a malformed asset
    // into a lossy-but-successful conversion.
    std::vector<size_t> indegree(model.nodes.size(), 0);
    std::vector<Matrix> local_matrices(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const auto& node = model.nodes[i];
        if (node.mesh < -1 ||
            (node.mesh >= 0 && static_cast<size_t>(node.mesh) >= model.meshes.size())) {
            result.error = "Node " + std::to_string(i) + " references an invalid mesh";
            return result;
        }
        if (!nodeMatrix(node, local_matrices[i])) {
            result.error = "Node " + std::to_string(i) + " has an invalid transform";
            return result;
        }
        for (int child : node.children) {
            if (child < 0 || static_cast<size_t>(child) >= model.nodes.size()) {
                result.error = "Node " + std::to_string(i) +
                               " references an invalid child node";
                return result;
            }
            const size_t child_index = static_cast<size_t>(child);
            if (++indegree[child_index] > 1) {
                result.error = "Node " + std::to_string(child_index) +
                               " has more than one parent";
                return result;
            }
        }
    }

    // Kahn's algorithm proves that every component is acyclic without using
    // recursion, so a deeply nested untrusted document cannot exhaust the C++
    // call stack during validation.
    std::vector<size_t> graph_queue;
    graph_queue.reserve(model.nodes.size());
    for (size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0)
            graph_queue.push_back(i);
    }
    std::vector<size_t> remaining_indegree = indegree;
    size_t processed = 0;
    for (size_t cursor = 0; cursor < graph_queue.size(); ++cursor) {
        const size_t node_index = graph_queue[cursor];
        ++processed;
        for (int child : model.nodes[node_index].children) {
            const size_t child_index = static_cast<size_t>(child);
            if (--remaining_indegree[child_index] == 0)
                graph_queue.push_back(child_index);
        }
    }
    if (processed != model.nodes.size()) {
        result.error = "glTF node graph contains a cycle";
        return result;
    }

    std::vector<int> roots;
    int scene_index = model.defaultScene;
    if (!model.scenes.empty()) {
        if (scene_index == -1)
            scene_index = 0;
        if (scene_index < 0 || static_cast<size_t>(scene_index) >= model.scenes.size()) {
            result.error = "glTF default scene index is invalid";
            return result;
        }
        roots = model.scenes[static_cast<size_t>(scene_index)].nodes;
        std::vector<bool> selected_root(model.nodes.size(), false);
        for (int root : roots) {
            if (root < 0 || static_cast<size_t>(root) >= model.nodes.size()) {
                result.error = "Active glTF scene references an invalid root node";
                return result;
            }
            const size_t root_index = static_cast<size_t>(root);
            if (indegree[root_index] != 0 || selected_root[root_index]) {
                result.error = "Active glTF scene contains a duplicate or non-root node";
                return result;
            }
            selected_root[root_index] = true;
        }
    } else {
        if (scene_index != -1) {
            result.error = "glTF default scene is set but no scenes are defined";
            return result;
        }
        for (size_t i = 0; i < indegree.size(); ++i) {
            if (indegree[i] == 0)
                roots.push_back(static_cast<int>(i));
        }
    }

    struct PendingNode {
        size_t index;
        Matrix parent;
    };
    std::vector<PendingNode> pending;
    pending.reserve(model.nodes.size());
    for (auto root = roots.rbegin(); root != roots.rend(); ++root)
        pending.push_back({static_cast<size_t>(*root), identity()});

    while (!pending.empty()) {
        PendingNode current = pending.back();
        pending.pop_back();
        const auto& node = model.nodes[current.index];
        const Matrix world = multiply(current.parent, local_matrices[current.index]);
        if (!std::all_of(world.begin(), world.end(),
                         [](double value) { return std::isfinite(value); })) {
            result.error = "Node " + std::to_string(current.index) +
                           " produces a non-finite world transform";
            result.instances.clear();
            return result;
        }
        if (node.mesh >= 0)
            result.instances.push_back({static_cast<size_t>(node.mesh), world});
        for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
            pending.push_back({static_cast<size_t>(*child), world});
        }
    }
    return result;
}

}  // namespace melkor::gltf_scene
