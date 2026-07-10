#include "melkor/enhanced_converter.hpp"
#include "melkor/glb_reader.hpp"
#include "melkor/spatial_grid.hpp"

#ifdef MELKOR_HAS_CUDA
#include "melkor/cuda_compute.hpp"
#endif
#include <array>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>

// For GLB loading — guard against definitions from the CMake target
#ifndef TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#endif
#ifndef TINYGLTF_NO_EXTERNAL_IMAGE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#endif
#include "tiny_gltf.h"
#include "gltf_scene_utils.hpp"
#include "safe_gltf_fs.hpp"

namespace melkor {

namespace {

// Analytical eigendecomposition of a real symmetric 3x3 matrix
// (Smith, "The Eigenvalues and Eigenvectors of 3x3 Matrices", 2002).
// Out: eigenvalues w0<=w1<=w2 (ascending), and corresponding unit eigenvectors.
// Used for PCA normal estimation: the normal is the eigenvector for the
// smallest eigenvalue (direction of least variance in the neighborhood).
void eigenSymmetric3x3(
    float a11, float a12, float a13,
    float a22, float a23, float a33,
    float& w0, float& w1, float& w2,  // eigenvalues, ascending
    float v0[3], float v1[3], float v2[3]) {  // eigenvectors (unit)
    float p1 = a12 * a12 + a13 * a13 + a23 * a23;
    if (p1 < 1e-18f) {
        // Matrix is already diagonal.
        w0 = a11; w1 = a22; w2 = a33;
        // Order them ascending with a stable sort of {0,1,2}.
        struct E { float w; int i; };
        E e[3] = {{a11, 0}, {a22, 1}, {a33, 2}};
        std::sort(e, e + 3, [](const E& x, const E& y) { return x.w < y.w; });
        w0 = e[0].w; w1 = e[1].w; w2 = e[2].w;
        for (int k = 0; k < 3; ++k) {
            float* v = (k == 0) ? v0 : (k == 1) ? v1 : v2;
            v[0] = (e[k].i == 0) ? 1.0f : 0.0f;
            v[1] = (e[k].i == 1) ? 1.0f : 0.0f;
            v[2] = (e[k].i == 2) ? 1.0f : 0.0f;
        }
        return;
    }
    float q = (a11 + a22 + a33) / 3.0f;
    float p2 = (a11 - q) * (a11 - q) + (a22 - q) * (a22 - q) +
               (a33 - q) * (a33 - q) + 2.0f * p1;
    float p = std::sqrt(p2 / 6.0f);
    // B = (1/p) (A - q I)
    float b11 = (a11 - q) / p, b12 = a12 / p, b13 = a13 / p;
    float b22 = (a22 - q) / p, b23 = a23 / p;
    float b33 = (a33 - q) / p;
    // det(B)
    float detB = b11 * (b22 * b33 - b23 * b23) -
                 b12 * (b12 * b33 - b23 * b13) +
                 b13 * (b12 * b23 - b22 * b13);
    float r = std::clamp(detB / 2.0f, -1.0f, 1.0f);
    float phi = std::acos(r) / 3.0f;
    // Eigenvalues of A, ordered chi1 >= chi2 >= chi3.
    float chi1 = q + 2.0f * p * std::cos(phi);
    float chi3 = q + 2.0f * p * std::cos(phi + 2.0943951023931953f /* 2pi/3 */);
    float chi2 = 3.0f * q - chi1 - chi3;
    // Ascending.
    w0 = chi3; w1 = chi2; w2 = chi1;

    // Eigenvector for eigenvalue w: solve (A - w I) v = 0 via the cross product
    // of two rows of (A - w I). Pick the two rows with the largest-magnitude
    // entries to maximize numerical stability.
    auto eigvec = [&](float w) -> std::array<float, 3> {
        float m00 = a11 - w, m01 = a12, m02 = a13;
        float m10 = a12, m11 = a22 - w, m12 = a23;
        float m20 = a13, m21 = a23, m22 = a33 - w;
        // Row norms.
        float r0 = m00 * m00 + m01 * m01 + m02 * m02;
        float r1 = m10 * m10 + m11 * m11 + m12 * m12;
        float r2 = m20 * m20 + m21 * m21 + m22 * m22;
        // Cross product of the two smallest-norm rows (least trust -> discard).
        float ax, ay, az, bx, by, bz;
        if (r0 <= r1 && r0 <= r2) { ax = m10; ay = m11; az = m12; bx = m20; by = m21; bz = m22; }
        else if (r1 <= r0 && r1 <= r2) { ax = m00; ay = m01; az = m02; bx = m20; by = m21; bz = m22; }
        else { ax = m00; ay = m01; az = m02; bx = m10; by = m11; bz = m12; }
        float cx = ay * bz - az * by;
        float cy = az * bx - ax * bz;
        float cz = ax * by - ay * bx;
        float len = std::sqrt(cx * cx + cy * cy + cz * cz);
        if (len < 1e-12f) {
            // Degenerate; fall back to an axis.
            return {1.0f, 0.0f, 0.0f};
        }
        return {cx / len, cy / len, cz / len};
    };
    auto e0 = eigvec(w0); auto e1 = eigvec(w1); auto e2 = eigvec(w2);
    v0[0] = e0[0]; v0[1] = e0[1]; v0[2] = e0[2];
    v1[0] = e1[0]; v1[1] = e1[1]; v1[2] = e1[2];
    v2[0] = e2[0]; v2[1] = e2[1]; v2[2] = e2[2];
}

// Return only the eigenvector for the smallest eigenvalue of the symmetric
// covariance matrix (the surface normal under the local-plane PCA model).
void smallestEigenvector3x3(
    float sxx, float sxy, float sxz, float syy, float syz, float szz,
    float& nx, float& ny, float& nz) {
    float w0, w1, w2;
    float v0[3], v1[3], v2[3];
    eigenSymmetric3x3(sxx, sxy, sxz, syy, syz, szz, w0, w1, w2, v0, v1, v2);
    nx = v0[0]; ny = v0[1]; nz = v0[2];
}

// A validated, alignment-agnostic view over one glTF accessor. glTF files are
// untrusted input: every index, byte range, stride, and multiplication must be
// checked before a pointer is formed. Keeping this decoder here also prevents
// the enhanced path from silently becoming less safe than GlbReader.
struct AccessorView {
    const uint8_t* data = nullptr;
    size_t stride = 0;
    size_t count = 0;
    size_t component_size = 0;
    int component_type = 0;
    int type = 0;
    int components = 0;
    bool normalized = false;
};

bool resolveAccessor(const tinygltf::Model& model, int accessor_index,
                     AccessorView& out) {
    if (accessor_index < 0 ||
        static_cast<size_t>(accessor_index) >= model.accessors.size()) {
        return false;
    }
    const auto& accessor = model.accessors[static_cast<size_t>(accessor_index)];
    // A sparse accessor can have a normal base bufferView. Decoding that view
    // without applying the sparse values is silent data loss, so both glTF
    // conversion paths fail closed until sparse overlays are supported.
    if (accessor.sparse.isSparse) {
        return false;
    }
    if (accessor.bufferView < 0 ||
        static_cast<size_t>(accessor.bufferView) >= model.bufferViews.size()) {
        // Sparse/implicit accessors are legal glTF, but are not supported by
        // this converter. Reject them cleanly instead of dereferencing -1.
        return false;
    }
    const auto& view = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
    if (view.buffer < 0 || static_cast<size_t>(view.buffer) >= model.buffers.size()) {
        return false;
    }
    const auto& buffer = model.buffers[static_cast<size_t>(view.buffer)];
    if (view.byteOffset > buffer.data.size() ||
        view.byteLength > buffer.data.size() - view.byteOffset) {
        return false;
    }

    const int component_size_i = tinygltf::GetComponentSizeInBytes(
        static_cast<uint32_t>(accessor.componentType));
    const int components = tinygltf::GetNumComponentsInType(
        static_cast<uint32_t>(accessor.type));
    const int stride_i = accessor.ByteStride(view);
    if (component_size_i <= 0 || components <= 0 || stride_i <= 0) {
        return false;
    }
    const size_t component_size = static_cast<size_t>(component_size_i);
    const size_t component_count = static_cast<size_t>(components);
    if (component_count > std::numeric_limits<size_t>::max() / component_size) {
        return false;
    }
    const size_t element_size = component_count * component_size;
    const size_t stride = static_cast<size_t>(stride_i);
    if (stride < element_size || accessor.byteOffset > view.byteLength) {
        return false;
    }

    const size_t available = view.byteLength - accessor.byteOffset;
    if (accessor.count > 0 &&
        (element_size > available ||
         accessor.count - 1 > (available - element_size) / stride)) {
        return false;
    }

    out.data = buffer.data.data() + view.byteOffset + accessor.byteOffset;
    out.stride = stride;
    out.count = accessor.count;
    out.component_size = component_size;
    out.component_type = accessor.componentType;
    out.type = accessor.type;
    out.components = components;
    out.normalized = accessor.normalized;
    return true;
}

template <typename T>
T readUnaligned(const uint8_t* data) {
    T value{};
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool readFloatVec3(const AccessorView& view, size_t index, float out[3]) {
    if (view.component_type != TINYGLTF_COMPONENT_TYPE_FLOAT ||
        view.type != TINYGLTF_TYPE_VEC3 || index >= view.count) {
        return false;
    }
    const uint8_t* p = view.data + index * view.stride;
    for (int c = 0; c < 3; ++c) {
        out[c] = readUnaligned<float>(p + static_cast<size_t>(c) * sizeof(float));
        if (!std::isfinite(out[c])) return false;
    }
    return true;
}

bool readColor(const AccessorView& view, size_t index, float out[3]) {
    if ((view.type != TINYGLTF_TYPE_VEC3 && view.type != TINYGLTF_TYPE_VEC4) ||
        index >= view.count) {
        return false;
    }
    const uint8_t* p = view.data + index * view.stride;
    for (int c = 0; c < 3; ++c) {
        const uint8_t* component = p + static_cast<size_t>(c) * view.component_size;
        switch (view.component_type) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                out[c] = static_cast<float>(*component) / 255.0f;
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                out[c] = static_cast<float>(readUnaligned<uint16_t>(component)) / 65535.0f;
                break;
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                out[c] = readUnaligned<float>(component);
                break;
            default:
                return false;
        }
        if (!std::isfinite(out[c])) return false;
        out[c] = std::clamp(out[c], 0.0f, 1.0f);
    }
    return true;
}

}  // namespace

// Simple spatial hash for k-NN queries
class SpatialHash {
public:
    SpatialHash(const std::vector<float>& positions, double cell_size)
        : inv_cell_size_(1.0 / cell_size) {
        const size_t num_points = positions.size() / 3;
        if (num_points > 0) {
            origin_[0] = static_cast<double>(positions[0]);
            origin_[1] = static_cast<double>(positions[1]);
            origin_[2] = static_cast<double>(positions[2]);
            for (size_t i = 1; i < num_points; ++i) {
                origin_[0] = std::min(origin_[0], static_cast<double>(positions[i * 3 + 0]));
                origin_[1] = std::min(origin_[1], static_cast<double>(positions[i * 3 + 1]));
                origin_[2] = std::min(origin_[2], static_cast<double>(positions[i * 3 + 2]));
            }
        }
        for (size_t i = 0; i < num_points; ++i) {
            const int64_t cx = cellCoordinate(positions[i * 3 + 0], origin_[0]);
            const int64_t cy = cellCoordinate(positions[i * 3 + 1], origin_[1]);
            const int64_t cz = cellCoordinate(positions[i * 3 + 2], origin_[2]);
            const uint64_t key = hashCell(cx, cy, cz);
            cells_[key].push_back(i);
        }
    }
    
    std::vector<size_t> queryNeighbors(float x, float y, float z, int radius = 1) const {
        std::vector<size_t> result;
        const int64_t cx = cellCoordinate(x, origin_[0]);
        const int64_t cy = cellCoordinate(y, origin_[1]);
        const int64_t cz = cellCoordinate(z, origin_[2]);
        
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    const uint64_t key = hashCell(
                        cx + static_cast<int64_t>(dx),
                        cy + static_cast<int64_t>(dy),
                        cz + static_cast<int64_t>(dz));
                    auto it = cells_.find(key);
                    if (it != cells_.end()) {
                        result.insert(result.end(), it->second.begin(), it->second.end());
                    }
                }
            }
        }
        return result;
    }
    
private:
    int64_t cellCoordinate(float value, double origin) const {
        const double coordinate =
            std::floor((static_cast<double>(value) - origin) * inv_cell_size_);
        // Valid finite inputs naturally stay far inside this range. Retain a
        // margin for neighbor-radius addition so public helper inputs cannot
        // trigger float-to-integer or signed-addition undefined behavior.
        constexpr int64_t margin = 1024;
        constexpr int64_t minimum = std::numeric_limits<int64_t>::min() + margin;
        constexpr int64_t maximum = std::numeric_limits<int64_t>::max() - margin;
        if (!std::isfinite(coordinate)) return coordinate < 0.0 ? minimum : maximum;
        if (coordinate <= static_cast<double>(minimum)) return minimum;
        if (coordinate >= static_cast<double>(maximum)) return maximum;
        return static_cast<int64_t>(coordinate);
    }

    static uint64_t hashCell(int64_t x, int64_t y, int64_t z) {
        // Simple spatial hash
        constexpr uint64_t mask = 0x1fffff;
        return ((static_cast<uint64_t>(x) & mask) << 42) |
               ((static_cast<uint64_t>(y) & mask) << 21) |
               (static_cast<uint64_t>(z) & mask);
    }
    
    double inv_cell_size_;
    double origin_[3] = {0.0, 0.0, 0.0};
    std::unordered_map<uint64_t, std::vector<size_t>> cells_;
};

#ifdef MELKOR_HAS_CUDA
// Process-wide CUDA context for the converter's k-NN path. The converter
// API predates ComputeProvider and only carries a Metal context pointer,
// which is always null on CUDA builds — so the CUDA grid k-NN is reached
// through this lazily initialized context instead. Returns per-point mean
// k-NN distances, or empty when no CUDA device is usable.
static std::vector<float> cudaGridKnnMeanDists(
    const std::vector<float>& positions, int k) {
    static cuda::CudaContext ctx;
    static const bool ok = cuda::CudaContext::isAvailable() && ctx.initialize();
    if (!ok) return {};
    auto g = grid::buildGrid(positions);
    if (!g.valid) return {};
    cuda::GaussianProcessor processor(ctx);
    auto stats = processor.knnStatsGrid(positions, g.entries, g.cell_starts,
                                        g.cell_counts, g.origin.data(),
                                        g.cell_size, g.dims.data(), k);
    const size_t n = positions.size() / 3;
    if (stats.size() != n * 4) return {};
    std::vector<float> dists(n);
    for (size_t i = 0; i < n; ++i) {
        dists[i] = stats[i * 4];
    }
    return dists;
}
#endif

class EnhancedConverter::Impl {
public:
    metal::MetalContext* metal_ctx_ = nullptr;
    tinygltf::TinyGLTF loader_;
    gltf_fs::Context fs_context_;
    
    Impl() { configureFilesystem(); }
    explicit Impl(metal::MetalContext* ctx) : metal_ctx_(ctx) { configureFilesystem(); }

    void configureFilesystem() {
        std::string ignored;
        loader_.SetFsCallbacks(gltf_fs::callbacks(fs_context_), &ignored);
    }

    bool setInputRoot(const std::string& filepath, std::string& error) {
        return gltf_fs::setRootForInput(fs_context_, filepath, error);
    }
    
    EnhancedConversionResult convert(
        const std::vector<float>& positions,
        const std::vector<float>& normals,
        const std::vector<float>& colors,
        const std::vector<uint32_t>& indices [[maybe_unused]],
        const EnhancedConversionConfig& config) {
        
        EnhancedConversionResult result;
        result.original_vertices = positions.size() / 3;

        const auto finitePositive = [](float value) {
            return std::isfinite(value) && value > 0.0f;
        };
        if (positions.size() % 3 != 0 || normals.size() % 3 != 0 || colors.size() % 3 != 0 ||
            config.knn_neighbors < 1 || config.knn_neighbors > 1024 ||
            !finitePositive(config.scale_factor) || !finitePositive(config.min_scale) ||
            !finitePositive(config.max_scale) || config.max_scale < config.min_scale ||
            !finitePositive(config.normal_scale_ratio) ||
            !finitePositive(config.position_scale) ||
            !std::isfinite(config.default_opacity) || config.default_opacity <= 0.0f ||
            config.default_opacity > 1.0f ||
            !std::isfinite(config.default_color[0]) ||
            !std::isfinite(config.default_color[1]) ||
            !std::isfinite(config.default_color[2]) ||
            config.default_color[0] < 0.0f || config.default_color[0] > 1.0f ||
            config.default_color[1] < 0.0f || config.default_color[1] > 1.0f ||
            config.default_color[2] < 0.0f || config.default_color[2] > 1.0f ||
            !std::all_of(positions.begin(), positions.end(), [&](float value) {
                return std::isfinite(value) && std::isfinite(value * config.position_scale);
            }) ||
            !std::all_of(normals.begin(), normals.end(), [](float value) { return std::isfinite(value); }) ||
            !std::all_of(colors.begin(), colors.end(), [](float value) { return std::isfinite(value); })) {
            result.error_message = "Invalid enhanced conversion input or configuration";
            return result;
        }
        
        if (positions.empty()) {
            result.error_message = "No positions provided";
            return result;
        }
        
        // Step 1: Compute adaptive scales using k-NN.
        // Metal brute-force k-NN for small clouds, Metal grid k-NN for large
        // ones, CPU spatial hash when no GPU is available.
        std::vector<float> adaptive_scales;
        size_t num_points = positions.size() / 3;
        if (config.use_gpu && metal_ctx_ && num_points > 0) {
            metal::GaussianProcessor processor(*metal_ctx_);
            std::vector<float> knn_dists;
            if (num_points < 10000) {
                knn_dists = processor.computeKnnDistancesMetal(
                    positions, config.knn_neighbors);
            } else {
                auto g = grid::buildGrid(positions);
                if (g.valid) {
                    auto stats = processor.knnStatsGrid(
                        positions, g.entries, g.cell_starts, g.cell_counts,
                        g.origin.data(), g.cell_size, g.dims.data(),
                        config.knn_neighbors);
                    if (stats.size() == num_points * 4) {
                        knn_dists.resize(num_points);
                        for (size_t i = 0; i < num_points; ++i) {
                            knn_dists[i] = stats[i * 4];
                        }
                    }
                }
            }
            if (knn_dists.size() == num_points) {
                // Scale: half the average k-NN distance (matching CPU path)
                adaptive_scales.resize(num_points);
                for (size_t i = 0; i < num_points; ++i) {
                    adaptive_scales[i] = knn_dists[i] * 0.5f;
                }
            }
        }
#ifdef MELKOR_HAS_CUDA
        if (config.use_gpu && adaptive_scales.empty() && num_points > 0) {
            auto knn_dists = cudaGridKnnMeanDists(positions, config.knn_neighbors);
            if (knn_dists.size() == num_points) {
                adaptive_scales.resize(num_points);
                for (size_t i = 0; i < num_points; ++i) {
                    adaptive_scales[i] = knn_dists[i] * 0.5f;
                }
            }
        }
#endif
        if (adaptive_scales.empty()) {
            adaptive_scales = computeAdaptiveScales(
                positions, config.knn_neighbors, config);
        }
        
        // Step 2: Compute or use provided normals
        std::vector<float> final_normals = normals;
        if (final_normals.empty()) {
            final_normals = enhanced::estimateNormals(positions, config.knn_neighbors);
        }

        // Attribute alignment: multi-primitive GLBs can carry colors/normals
        // for only some primitives, leaving these arrays shorter than
        // positions — indexing them per point would read out of bounds (on
        // both the CPU and GPU paths). Pad short arrays to full length:
        // colors with the default color, normals with zeros (a zero normal
        // falls back to the default orientation downstream).
        std::vector<float> final_colors =
            config.use_vertex_colors ? colors : std::vector<float>{};
        if (!final_colors.empty() && final_colors.size() < num_points * 3) {
            while (final_colors.size() < num_points * 3) {
                final_colors.push_back(config.default_color[0]);
                final_colors.push_back(config.default_color[1]);
                final_colors.push_back(config.default_color[2]);
            }
        }
        if (!final_normals.empty() && final_normals.size() < num_points * 3) {
            final_normals.resize(num_points * 3, 0.0f);
        }

        // Step 3: Convert each point to a Gaussian splat.
        // Use Metal GPU acceleration when available for the per-point loop.
        result.cloud.reserve(num_points * 2);

        if (config.use_gpu && metal_ctx_ && num_points > 0) {
            // Metal-accelerated per-point conversion
            metal::GaussianProcessor processor(*metal_ctx_);
            metal::GaussianProcessor::EnhancedConvertConfig mtl_cfg;
            mtl_cfg.scale_factor = config.scale_factor;
            mtl_cfg.min_scale = config.min_scale;
            mtl_cfg.max_scale = config.max_scale;
            mtl_cfg.normal_scale_ratio = config.normal_scale_ratio;
            mtl_cfg.default_opacity = config.default_opacity;
            mtl_cfg.position_scale = config.position_scale;
            mtl_cfg.convert_coordinate_system = config.convert_coordinate_system;
            mtl_cfg.use_surface_alignment = config.use_surface_alignment;
            mtl_cfg.default_color[0] = config.default_color[0];
            mtl_cfg.default_color[1] = config.default_color[1];
            mtl_cfg.default_color[2] = config.default_color[2];

            auto packed = processor.enhancedConvert(
                positions, final_normals, final_colors, adaptive_scales, mtl_cfg);

            if (!packed.empty()) {
                // Convert PackedGaussian back to GaussianSplat
                float total_scale = 0.0f;
                for (size_t i = 0; i < num_points; ++i) {
                    GaussianSplat splat;
                    splat.x = packed[i].position[0];
                    splat.y = packed[i].position[1];
                    splat.z = packed[i].position[2];
                    splat.opacity = packed[i].position[3];
                    splat.f_dc_0 = packed[i].color[0];
                    splat.f_dc_1 = packed[i].color[1];
                    splat.f_dc_2 = packed[i].color[2];
                    splat.scale_0 = packed[i].scale[0];
                    splat.scale_1 = packed[i].scale[1];
                    splat.scale_2 = packed[i].scale[2];
                    splat.rot_0 = packed[i].rotation[0];
                    splat.rot_1 = packed[i].rotation[1];
                    splat.rot_2 = packed[i].rotation[2];
                    splat.rot_3 = packed[i].rotation[3];
                    total_scale += std::exp(packed[i].scale[0]);
                    result.cloud.addSplat(std::move(splat));
                }
                result.output_splats = result.cloud.size();
                result.avg_scale = total_scale / static_cast<float>(num_points);
                result.success = true;
                return result;
            }
            // Fall through to CPU path if Metal failed
        }

        // CPU per-point conversion (fallback)
        float total_scale = 0.0f;
        for (size_t i = 0; i < num_points; ++i) {
            GaussianSplat splat;
            
            // Position (with coordinate conversion if needed)
            float px = positions[i*3+0] * config.position_scale;
            float py = positions[i*3+1] * config.position_scale;
            float pz = positions[i*3+2] * config.position_scale;
            
            if (config.convert_coordinate_system) {
                // Y-up to Z-up
                splat.x = px;
                splat.y = -pz;
                splat.z = py;
            } else {
                splat.x = px;
                splat.y = py;
                splat.z = pz;
            }
            
            // Color -> SH DC coefficients
            float r, g, b;
            if (!final_colors.empty()) {
                r = final_colors[i*3+0];
                g = final_colors[i*3+1];
                b = final_colors[i*3+2];
            } else {
                r = config.default_color[0];
                g = config.default_color[1];
                b = config.default_color[2];
            }
            splat.f_dc_0 = utils::rgbToShDc(r);
            splat.f_dc_1 = utils::rgbToShDc(g);
            splat.f_dc_2 = utils::rgbToShDc(b);
            
            // Opacity (in logit space)
            splat.opacity = utils::logit(config.default_opacity);
            
            // Adaptive anisotropic scale
            float base_scale = adaptive_scales[i] * config.scale_factor;
            base_scale = std::clamp(base_scale, config.min_scale, config.max_scale);
            
            float tangent_scale = base_scale;
            float normal_scale = base_scale * config.normal_scale_ratio;
            
            if (config.use_surface_alignment && !final_normals.empty()) {
                // Anisotropic scale: flatter along normal
                splat.scale_0 = std::log(tangent_scale);
                splat.scale_1 = std::log(tangent_scale);
                splat.scale_2 = std::log(normal_scale);
            } else {
                // Isotropic scale
                float log_scale = std::log(base_scale);
                splat.scale_0 = log_scale;
                splat.scale_1 = log_scale;
                splat.scale_2 = log_scale;
            }
            
            total_scale += base_scale;
            
            // Rotation from surface normal
            if (config.use_surface_alignment && !final_normals.empty()) {
                float nx = final_normals[i*3+0];
                float ny = final_normals[i*3+1];
                float nz = final_normals[i*3+2];
                
                if (config.convert_coordinate_system) {
                    float tmp = ny;
                    ny = -nz;
                    nz = tmp;
                }
                
                // Compute quaternion that rotates Z-axis to normal
                computeQuaternionFromNormal(nx, ny, nz,
                    splat.rot_0, splat.rot_1, splat.rot_2, splat.rot_3);
            } else {
                // Identity quaternion
                splat.rot_0 = 1.0f;
                splat.rot_1 = 0.0f;
                splat.rot_2 = 0.0f;
                splat.rot_3 = 0.0f;
            }
            
            result.cloud.addSplat(std::move(splat));
        }
        
        result.output_splats = result.cloud.size();
        result.avg_scale = total_scale / static_cast<float>(num_points);
        result.success = true;
        
        return result;
    }
    
    std::vector<float> computeAdaptiveScales(
        const std::vector<float>& positions,
        int k,
        const EnhancedConversionConfig& config) {
        
        size_t num_points = positions.size() / 3;
        std::vector<float> scales(num_points);
        
        // Compute bounding box for cell size estimation
        float min_x = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        float min_y = min_x, max_y = max_x, min_z = min_x, max_z = max_x;
        
        for (size_t i = 0; i < num_points; ++i) {
            min_x = std::min(min_x, positions[i*3+0]);
            max_x = std::max(max_x, positions[i*3+0]);
            min_y = std::min(min_y, positions[i*3+1]);
            max_y = std::max(max_y, positions[i*3+1]);
            min_z = std::min(min_z, positions[i*3+2]);
            max_z = std::max(max_z, positions[i*3+2]);
        }
        
        float extent = std::max({max_x - min_x, max_y - min_y, max_z - min_z});
        // Degenerate extent (single point or all points coincident): a zero
        // cell size would produce inf/NaN cell coordinates and UB in the
        // float->int cast inside SpatialHash. Every point gets the minimum
        // scale instead.
        if (extent < 1e-9f) {
            std::fill(scales.begin(), scales.end(), config.min_scale);
            return scales;
        }
        float cell_size = extent / std::cbrt(static_cast<float>(num_points)) * 2.0f;

        // Build spatial hash
        SpatialHash hash(positions, cell_size);
        
        // For each point, find k nearest neighbors and compute average distance
        for (size_t i = 0; i < num_points; ++i) {
            float px = positions[i*3+0];
            float py = positions[i*3+1];
            float pz = positions[i*3+2];
            
            // Query neighbors, expanding the search radius until we have at
            // least k+1 candidates (matching the pattern in estimateNormals).
            auto candidates = hash.queryNeighbors(px, py, pz, 2);
            int radius = 2;
            while (static_cast<int>(candidates.size()) < k + 1 && radius < 16) {
                candidates = hash.queryNeighbors(px, py, pz, ++radius);
            }
            
            // Compute distances to all candidates (excluding self)
            std::vector<float> distances;
            distances.reserve(candidates.size());
            for (size_t j : candidates) {
                if (j == i) continue;
                float dx = positions[j*3+0] - px;
                float dy = positions[j*3+1] - py;
                float dz = positions[j*3+2] - pz;
                distances.push_back(std::sqrt(dx*dx + dy*dy + dz*dz));
            }
            
            if (distances.empty()) {
                scales[i] = config.min_scale;
                continue;
            }
            
            // Sort and take k nearest
            std::sort(distances.begin(), distances.end());
            int actual_k = std::min(k, static_cast<int>(distances.size()));
            
            // Average of k nearest distances
            float avg_dist = 0.0f;
            for (int j = 0; j < actual_k; ++j) {
                avg_dist += distances[j];
            }
            avg_dist /= static_cast<float>(actual_k);
            
            // Scale should cover approximately half the distance to nearest neighbor
            scales[i] = avg_dist * 0.5f;
        }
        
        return scales;
    }
    
    void computeQuaternionFromNormal(float nx, float ny, float nz,
                                     float& w, float& x, float& y, float& z) {
        // Normalize normal
        float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (len > 0.0f) {
            nx /= len;
            ny /= len;
            nz /= len;
        } else {
            // Default to Z-up
            nx = 0.0f;
            ny = 0.0f;
            nz = 1.0f;
        }
        
        // Compute quaternion that rotates Z-axis (0,0,1) to normal (nx,ny,nz)
        float dot = nz;  // dot(Z-axis, normal)
        
        if (dot > 0.9999f) {
            // Normal is already Z-up
            w = 1.0f;
            x = 0.0f;
            y = 0.0f;
            z = 0.0f;
        } else if (dot < -0.9999f) {
            // Normal is Z-down, rotate 180° around X
            w = 0.0f;
            x = 1.0f;
            y = 0.0f;
            z = 0.0f;
        } else {
            // General case: rotation axis = cross(Z, normal)
            float ax = -ny;  // cross(Z, normal).x = 0*nz - 1*ny = -ny
            float ay = nx;   // cross(Z, normal).y = 1*nx - 0*nz = nx
            float az = 0.0f; // cross(Z, normal).z = 0*ny - 0*nx = 0
            
            // Half-angle formula
            float s = std::sqrt((1.0f + dot) * 2.0f);
            float inv_s = 1.0f / s;
            
            w = s * 0.5f;
            x = ax * inv_s;
            y = ay * inv_s;
            z = az * inv_s;
        }
        
        // Normalize quaternion
        utils::normalizeQuaternion(w, x, y, z);
    }
};

EnhancedConverter::EnhancedConverter() : impl_(std::make_unique<Impl>()) {}
EnhancedConverter::EnhancedConverter(metal::MetalContext* metal_ctx)
    : impl_(std::make_unique<Impl>(metal_ctx)) {}
EnhancedConverter::~EnhancedConverter() = default;

EnhancedConversionResult EnhancedConverter::convertFromFile(
    const std::string& filepath,
    const EnhancedConversionConfig& config) {
    
    EnhancedConversionResult result;
    if (!impl_->setInputRoot(filepath, result.error_message)) return result;
    
    tinygltf::Model model;
    std::string err, warn;
    bool success;
    
    if (gltf_scene::fileHasGlbMagic(filepath)) {
        success = impl_->loader_.LoadBinaryFromFile(&model, &err, &warn, filepath);
    } else {
        success = impl_->loader_.LoadASCIIFromFile(&model, &err, &warn, filepath);
    }
    
    if (!success) {
        result.error_message = err;
        return result;
    }

    if (const std::string contract_error = gltf_scene::validateModelContract(model);
        !contract_error.empty()) {
        result.error_message = contract_error;
        return result;
    }
    
    // Extract mesh data from GLB. Unlike TinyGLTF's high-level parsing, its
    // model containers do not make direct vector indexing safe: malformed
    // accessor/buffer references may still be present. Resolve every accessor
    // through the checked view above and use memcpy-based scalar reads so an
    // unaligned (but otherwise valid) byte offset cannot trigger UB.
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> colors;
    std::vector<uint32_t> indices;
    bool any_normals = false;
    bool any_colors = false;
    
    const auto traversal = gltf_scene::activeMeshInstances(model);
    if (!traversal.success()) {
        result.error_message = traversal.error;
        return result;
    }
    const auto& instances = traversal.instances;
    for (const auto& instance : instances) {
        const auto& mesh = model.meshes[instance.mesh_index];
        for (const auto& primitive : mesh.primitives) {
            auto pos_it = primitive.attributes.find("POSITION");
            if (pos_it == primitive.attributes.end()) {
                result.error_message = "Primitive is missing its POSITION attribute";
                return result;
            }

            AccessorView pos_view;
            if (!resolveAccessor(model, pos_it->second, pos_view) ||
                pos_view.component_type != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                pos_view.type != TINYGLTF_TYPE_VEC3 || pos_view.count == 0) {
                result.error_message = "Primitive has an invalid POSITION accessor";
                return result;
            }
            if (pos_view.count > std::numeric_limits<size_t>::max() / 3 ||
                positions.size() >
                    std::numeric_limits<size_t>::max() - pos_view.count * 3) {
                result.error_message = "POSITION accessor is too large";
                return result;
            }

            // Decode a primitive into temporary storage first. If one position
            // is NaN/Inf, reject the document rather than silently producing a
            // lossy conversion from only its remaining primitives.
            std::vector<float> primitive_positions;
            primitive_positions.reserve(pos_view.count * 3);
            bool positions_valid = true;
            for (size_t i = 0; i < pos_view.count; ++i) {
                float local[3];
                float value[3];
                if (!readFloatVec3(pos_view, i, local) ||
                    !gltf_scene::transformPoint(instance.world, local, value)) {
                    positions_valid = false;
                    break;
                }
                primitive_positions.insert(primitive_positions.end(), value, value + 3);
            }
            if (!positions_valid) {
                result.error_message =
                    "Primitive contains a non-finite or non-transformable POSITION";
                return result;
            }

            size_t base_idx = positions.size() / 3;
            const size_t count = pos_view.count;
            positions.insert(positions.end(), primitive_positions.begin(),
                             primitive_positions.end());

            // Keep one placeholder normal/color per appended position. If no
            // primitive supplies an attribute, the arrays are cleared below so
            // the converter estimates normals / applies the configured color.
            const size_t normal_start = normals.size();
            normals.resize(normal_start + count * 3, 0.0f);
            auto norm_it = primitive.attributes.find("NORMAL");
            if (norm_it != primitive.attributes.end()) {
                AccessorView normal_view;
                if (!resolveAccessor(model, norm_it->second, normal_view) ||
                    normal_view.count != count ||
                    normal_view.component_type != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                    normal_view.type != TINYGLTF_TYPE_VEC3) {
                    result.error_message = "Primitive has an invalid NORMAL accessor";
                    return result;
                }
                for (size_t i = 0; i < count; ++i) {
                    float local[3];
                    float value[3];
                    if (!readFloatVec3(normal_view, i, local) ||
                        !gltf_scene::transformNormal(instance.world, local, value)) {
                        result.error_message =
                            "Primitive contains a non-finite or non-transformable NORMAL";
                        return result;
                    }
                    std::copy(value, value + 3,
                              normals.begin() + static_cast<ptrdiff_t>(normal_start + i * 3));
                }
                any_normals = true;
            }

            const size_t color_start = colors.size();
            for (size_t i = 0; i < count; ++i) {
                colors.push_back(config.default_color[0]);
                colors.push_back(config.default_color[1]);
                colors.push_back(config.default_color[2]);
            }
            auto color_it = primitive.attributes.find("COLOR_0");
            if (color_it != primitive.attributes.end()) {
                AccessorView color_view;
                const bool supported_component =
                    resolveAccessor(model, color_it->second, color_view) &&
                    (color_view.component_type == TINYGLTF_COMPONENT_TYPE_FLOAT ||
                     color_view.component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
                     color_view.component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
                const bool supported_type = color_view.type == TINYGLTF_TYPE_VEC3 ||
                                            color_view.type == TINYGLTF_TYPE_VEC4;
                const bool valid_normalization =
                    color_view.component_type == TINYGLTF_COMPONENT_TYPE_FLOAT ||
                    color_view.normalized;
                if (!supported_component || !supported_type || !valid_normalization ||
                    color_view.count != count) {
                    result.error_message = "Primitive has an invalid COLOR_0 accessor";
                    return result;
                }
                for (size_t i = 0; i < count; ++i) {
                    float value[3];
                    if (!readColor(color_view, i, value)) {
                        result.error_message = "Primitive contains a non-finite COLOR_0 value";
                        return result;
                    }
                    std::copy(value, value + 3,
                              colors.begin() + static_cast<ptrdiff_t>(color_start + i * 3));
                }
                any_colors = true;
            }

            // Indices are not currently consumed by the point-based converter,
            // but decode valid ones so the API remains ready for triangle-aware
            // processing without preserving the old unchecked reads.
            if (primitive.indices < -1) {
                result.error_message = "Primitive has an invalid indices accessor";
                return result;
            }
            if (primitive.indices >= 0) {
                AccessorView index_view;
                if (!resolveAccessor(model, primitive.indices, index_view) ||
                    index_view.type != TINYGLTF_TYPE_SCALAR ||
                    (index_view.component_type != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                     index_view.component_type != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
                     index_view.component_type != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)) {
                    result.error_message = "Primitive has an invalid indices accessor";
                    return result;
                }
                for (size_t i = 0; i < index_view.count; ++i) {
                    const uint8_t* p = index_view.data + i * index_view.stride;
                    uint32_t idx = 0;
                    if (index_view.component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        idx = *p;
                    } else if (index_view.component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        idx = readUnaligned<uint16_t>(p);
                    } else {
                        idx = readUnaligned<uint32_t>(p);
                    }
                    if (idx >= count ||
                        base_idx > std::numeric_limits<uint32_t>::max() - idx) {
                        result.error_message = "Primitive contains an out-of-range index";
                        return result;
                    }
                    indices.push_back(static_cast<uint32_t>(base_idx + idx));
                }
            }
        }
    }

    if (!any_normals) normals.clear();
    if (!any_colors) colors.clear();

    return impl_->convert(positions, normals, colors, indices, config);
}

EnhancedConversionResult EnhancedConverter::convertFromMesh(
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<float>& colors,
    const std::vector<uint32_t>& indices,
    const EnhancedConversionConfig& config) {
    return impl_->convert(positions, normals, colors, indices, config);
}

namespace enhanced {

std::vector<float> computeKnnDistances(
    const std::vector<float>& positions,
    int k,
    metal::MetalContext* metal_ctx) {

    size_t num_points = positions.size() / 3;
    std::vector<float> distances(num_points, 0.0f);
    if (num_points == 0) return distances;

#ifdef MELKOR_HAS_CUDA
    // CUDA builds have no Metal context; try the CUDA grid k-NN first.
    {
        auto cuda_dists = cudaGridKnnMeanDists(positions, k);
        if (cuda_dists.size() == num_points) return cuda_dists;
    }
#endif

    // Use Metal when a GPU is available: brute force for small clouds,
    // grid-accelerated for large ones.
    if (metal_ctx && num_points < 10000) {
        metal::GaussianProcessor processor(*metal_ctx);
        auto result = processor.computeKnnDistancesMetal(positions, k);
        if (result.size() == num_points) return result;
    } else if (metal_ctx) {
        auto g = grid::buildGrid(positions);
        if (g.valid) {
            metal::GaussianProcessor processor(*metal_ctx);
            auto stats = processor.knnStatsGrid(
                positions, g.entries, g.cell_starts, g.cell_counts,
                g.origin.data(), g.cell_size, g.dims.data(), k);
            if (stats.size() == num_points * 4) {
                for (size_t i = 0; i < num_points; ++i) {
                    distances[i] = stats[i * 4];
                }
                return distances;
            }
        }
    }

    // CPU: O(n^2) brute force for small clouds; spatial hash for larger ones.
    if (num_points < 10000) {
        for (size_t i = 0; i < num_points; ++i) {
            std::vector<float> dists;
            dists.reserve(num_points - 1);
            
            for (size_t j = 0; j < num_points; ++j) {
                if (i == j) continue;
                float dx = positions[j*3+0] - positions[i*3+0];
                float dy = positions[j*3+1] - positions[i*3+1];
                float dz = positions[j*3+2] - positions[i*3+2];
                dists.push_back(std::sqrt(dx*dx + dy*dy + dz*dz));
            }
            
            std::sort(dists.begin(), dists.end());
            int actual_k = std::min(k, static_cast<int>(dists.size()));
            if (actual_k == 0) continue;  // single-point cloud: avoid 0/0 NaN
            for (int j = 0; j < actual_k; ++j) {
                distances[i] += dists[j];
            }
            distances[i] /= static_cast<float>(actual_k);
        }
    } else {
        // Spatial hash k-NN for large clouds (same pattern as estimateNormals).
        float min_x = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        float min_y = min_x, max_y = max_x, min_z = min_x, max_z = max_x;
        for (size_t i = 0; i < num_points; ++i) {
            min_x = std::min(min_x, positions[i*3+0]);
            max_x = std::max(max_x, positions[i*3+0]);
            min_y = std::min(min_y, positions[i*3+1]);
            max_y = std::max(max_y, positions[i*3+1]);
            min_z = std::min(min_z, positions[i*3+2]);
            max_z = std::max(max_z, positions[i*3+2]);
        }
        float extent = std::max({max_x - min_x, max_y - min_y, max_z - min_z});
        if (extent < 1e-9f) return distances;
        float cell_size = extent / std::cbrt(static_cast<float>(num_points)) * 2.0f;
        SpatialHash hash(positions, cell_size);

        for (size_t i = 0; i < num_points; ++i) {
            float px = positions[i*3+0];
            float py = positions[i*3+1];
            float pz = positions[i*3+2];

            auto candidates = hash.queryNeighbors(px, py, pz, 2);
            int radius = 2;
            while (static_cast<int>(candidates.size()) < k + 1 && radius < 16) {
                candidates = hash.queryNeighbors(px, py, pz, ++radius);
            }

            std::vector<float> dists;
            dists.reserve(candidates.size());
            for (size_t j : candidates) {
                if (j == i) continue;
                float dx = positions[j*3+0] - px;
                float dy = positions[j*3+1] - py;
                float dz = positions[j*3+2] - pz;
                dists.push_back(std::sqrt(dx*dx + dy*dy + dz*dz));
            }
            if (dists.empty()) continue;

            std::sort(dists.begin(), dists.end());
            int actual_k = std::min(k, static_cast<int>(dists.size()));
            for (int j = 0; j < actual_k; ++j) {
                distances[i] += dists[j];
            }
            distances[i] /= static_cast<float>(actual_k);
        }
    }
    
    return distances;
}

void computeSurfaceFrame(
    const float normal[3],
    float tangent[3],
    float bitangent[3]) {
    
    // Find a vector not parallel to normal
    float ref[3] = {1.0f, 0.0f, 0.0f};
    if (std::abs(normal[0]) > 0.9f) {
        ref[0] = 0.0f;
        ref[1] = 1.0f;
    }
    
    // tangent = cross(normal, ref)
    tangent[0] = normal[1] * ref[2] - normal[2] * ref[1];
    tangent[1] = normal[2] * ref[0] - normal[0] * ref[2];
    tangent[2] = normal[0] * ref[1] - normal[1] * ref[0];
    
    // Normalize tangent
    float len = std::sqrt(tangent[0]*tangent[0] + tangent[1]*tangent[1] + tangent[2]*tangent[2]);
    if (len > 0.0f) {
        tangent[0] /= len;
        tangent[1] /= len;
        tangent[2] /= len;
    }
    
    // bitangent = cross(normal, tangent)
    bitangent[0] = normal[1] * tangent[2] - normal[2] * tangent[1];
    bitangent[1] = normal[2] * tangent[0] - normal[0] * tangent[2];
    bitangent[2] = normal[0] * tangent[1] - normal[1] * tangent[0];
}

void surfaceFrameToQuaternion(
    const float normal[3],
    const float tangent[3],
    float quat[4]) {
    
    // Build rotation matrix from surface frame
    // Column 0: tangent, Column 1: bitangent, Column 2: normal
    float bitangent[3];
    bitangent[0] = normal[1] * tangent[2] - normal[2] * tangent[1];
    bitangent[1] = normal[2] * tangent[0] - normal[0] * tangent[2];
    bitangent[2] = normal[0] * tangent[1] - normal[1] * tangent[0];
    
    // Convert rotation matrix to quaternion
    float m00 = tangent[0], m01 = bitangent[0], m02 = normal[0];
    float m10 = tangent[1], m11 = bitangent[1], m12 = normal[1];
    float m20 = tangent[2], m21 = bitangent[2], m22 = normal[2];
    
    float trace = m00 + m11 + m22;
    
    if (trace > 0) {
        float s = 0.5f / std::sqrt(trace + 1.0f);
        quat[0] = 0.25f / s;  // w
        quat[1] = (m21 - m12) * s;  // x
        quat[2] = (m02 - m20) * s;  // y
        quat[3] = (m10 - m01) * s;  // z
    } else if (m00 > m11 && m00 > m22) {
        float s = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        quat[0] = (m21 - m12) / s;
        quat[1] = 0.25f * s;
        quat[2] = (m01 + m10) / s;
        quat[3] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        quat[0] = (m02 - m20) / s;
        quat[1] = (m01 + m10) / s;
        quat[2] = 0.25f * s;
        quat[3] = (m12 + m21) / s;
    } else {
        float s = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        quat[0] = (m10 - m01) / s;
        quat[1] = (m02 + m20) / s;
        quat[2] = (m12 + m21) / s;
        quat[3] = 0.25f * s;
    }
    
    // Normalize
    float len = std::sqrt(quat[0]*quat[0] + quat[1]*quat[1] + quat[2]*quat[2] + quat[3]*quat[3]);
    if (len > 0.0f) {
        quat[0] /= len;
        quat[1] /= len;
        quat[2] /= len;
        quat[3] /= len;
    }
}

std::vector<float> estimateNormals(
    const std::vector<float>& positions,
    int k_neighbors) {

    size_t num_points = positions.size() / 3;
    std::vector<float> normals(num_points * 3);

    // Default to Z-up everywhere; overwritten where a real normal is computed.
    for (size_t i = 0; i < num_points; ++i) {
        normals[i * 3 + 0] = 0.0f;
        normals[i * 3 + 1] = 0.0f;
        normals[i * 3 + 2] = 1.0f;
    }

    if (num_points == 0) {
        return normals;
    }

    // Build a spatial hash so k-NN is ~O(1) per point instead of O(n).
    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_y = min_x, max_y = max_x, min_z = min_x, max_z = max_x;
    for (size_t i = 0; i < num_points; ++i) {
        min_x = std::min(min_x, positions[i * 3 + 0]);
        max_x = std::max(max_x, positions[i * 3 + 0]);
        min_y = std::min(min_y, positions[i * 3 + 1]);
        max_y = std::max(max_y, positions[i * 3 + 1]);
        min_z = std::min(min_z, positions[i * 3 + 2]);
        max_z = std::max(max_z, positions[i * 3 + 2]);
    }
    float extent = std::max({max_x - min_x, max_y - min_y, max_z - min_z});
    if (extent < 1e-9f) {
        // Degenerate cloud (all points coincident): keep Z-up defaults.
        return normals;
    }
    float cell_size = extent / std::cbrt(static_cast<float>(num_points)) * 2.0f;
    SpatialHash hash(positions, cell_size);

    int k = std::max(3, k_neighbors);

    for (size_t i = 0; i < num_points; ++i) {
        float px = positions[i * 3 + 0];
        float py = positions[i * 3 + 1];
        float pz = positions[i * 3 + 2];

        // Gather candidate neighbors, expanding the search radius until we have
        // at least k of them (capped to avoid pathological scans on clusters).
        auto candidates = hash.queryNeighbors(px, py, pz, 2);
        int radius = 2;
        while (static_cast<int>(candidates.size()) < k && radius < 16) {
            candidates = hash.queryNeighbors(px, py, pz, ++radius);
        }

        // Compute squared distances and keep the k nearest (excluding self).
        std::vector<std::pair<float, size_t>> nbrs;
        nbrs.reserve(candidates.size());
        for (size_t j : candidates) {
            if (j == i) continue;
            float dx = positions[j * 3 + 0] - px;
            float dy = positions[j * 3 + 1] - py;
            float dz = positions[j * 3 + 2] - pz;
            nbrs.emplace_back(dx * dx + dy * dy + dz * dz, j);
        }
        if (nbrs.empty()) {
            continue;  // keep Z-up default for this isolated point
        }
        std::partial_sort(nbrs.begin(),
                          nbrs.begin() + std::min<int>(k, static_cast<int>(nbrs.size())),
                          nbrs.end());

        int knn = std::min<int>(k, static_cast<int>(nbrs.size()));
        if (knn < 3) {
            // Not enough neighbors to define a plane reliably.
            continue;
        }

        // Centroid of the neighborhood (including the query point).
        float cx = px, cy = py, cz = pz;
        for (int n = 0; n < knn; ++n) {
            size_t j = nbrs[n].second;
            cx += positions[j * 3 + 0];
            cy += positions[j * 3 + 1];
            cz += positions[j * 3 + 2];
        }
        float inv_count = 1.0f / static_cast<float>(knn + 1);
        cx *= inv_count;
        cy *= inv_count;
        cz *= inv_count;

        // Covariance matrix entries (symmetric).
        float sxx = 0, sxy = 0, sxz = 0, syy = 0, syz = 0, szz = 0;
        auto accumulate = [&](size_t j) {
            float dx = positions[j * 3 + 0] - cx;
            float dy = positions[j * 3 + 1] - cy;
            float dz = positions[j * 3 + 2] - cz;
            sxx += dx * dx; sxy += dx * dy; sxz += dx * dz;
            syy += dy * dy; syz += dy * dz;
            szz += dz * dz;
        };
        accumulate(i);
        for (int n = 0; n < knn; ++n) {
            accumulate(nbrs[n].second);
        }
        sxx *= inv_count; sxy *= inv_count; sxz *= inv_count;
        syy *= inv_count; syz *= inv_count; szz *= inv_count;

        // Smallest-eigenvalue eigenvector of the symmetric 3x3 covariance via
        // the analytical method. The normal is the direction of least variance,
        // i.e. the eigenvector for the smallest eigenvalue.
        float nx, ny, nz;
        smallestEigenvector3x3(sxx, sxy, sxz, syy, syz, szz, nx, ny, nz);

        // Orient the normal consistently: point it toward the "outside" of the
        // cloud, approximated by the vector from the centroid to the query
        // point. Without a sensor viewpoint this is the best local heuristic.
        float ox = px - cx, oy = py - cy, oz = pz - cz;
        if (nx * ox + ny * oy + nz * oz < 0.0f) {
            nx = -nx;
            ny = -ny;
            nz = -nz;
        }

        normals[i * 3 + 0] = nx;
        normals[i * 3 + 1] = ny;
        normals[i * 3 + 2] = nz;
    }

    return normals;
}

} // namespace enhanced
} // namespace melkor
