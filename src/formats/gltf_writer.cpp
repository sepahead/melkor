#include "melkor/format/gltf_writer.hpp"

#include "melkor/format/glb_container.hpp"

#include "json.hpp"

#include <cstring>
#include <limits>
#include <string>

namespace melkor::format::gltf {

namespace {

using json = nlohmann::json;

void append_f32_le(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    out.push_back(static_cast<std::uint8_t>(bits & 0xFF));
    out.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFF));
}

// Records an accessor + its bufferView for a freshly-appended, tightly-packed block of `count`
// elements of `comps` float components starting at `byte_offset`. Returns the accessor index.
struct Builder {
    json accessors = json::array();
    json buffer_views = json::array();

    std::size_t add(std::size_t byte_offset, std::size_t comps, std::size_t count) {
        const char* type = comps == 1 ? "SCALAR" : (comps == 3 ? "VEC3" : "VEC4");
        buffer_views.push_back({{"buffer", 0},
                                {"byteOffset", byte_offset},
                                {"byteLength", comps * 4 * count}});
        json accessor = {{"bufferView", buffer_views.size() - 1},
                         {"componentType", 5126},
                         {"type", type},
                         {"count", count}};
        accessors.push_back(accessor);
        return accessors.size() - 1;
    }
};

}  // namespace

Result<GlbWriteResult> write_glb(const SplatData& data, khr::ColorSpace color_space) {
    const std::size_t n = data.size();
    LossReport losses;

    // The KHR RC profile stops at degree 3; a higher-degree source is truncated with a reported
    // loss rather than silently dropping the top coefficients.
    const std::uint32_t source_degree = data.sh().degree();
    std::uint32_t write_degree = source_degree;
    if (write_degree > khr::kMaxProfileShDegree) {
        write_degree = khr::kMaxProfileShDegree;
        LossItem item;
        item.code = loss_code::kShDegreeTruncated;
        item.severity = LossSeverity::severe;
        item.source_feature = "spherical harmonics degree " + std::to_string(source_degree);
        item.target_constraint = "the KHR_gaussian_splatting RC profile supports degree 0-3";
        item.affected_splats = n;
        item.remediation = "approve LOSS_SH_DEGREE_TRUNCATED to accept degree-3 output, or target a "
                           "format that carries degree 4";
        losses.add(std::move(item));
    }

    const std::size_t src_coeffs = khr::sh_total_coefficients(source_degree);
    const std::size_t out_coeffs = khr::sh_total_coefficients(write_degree);
    const auto& raw = data.sh().raw();

    // Assemble the binary buffer, one tightly-packed block per attribute. Every block is float, so
    // every offset is 4-byte aligned as glTF requires.
    std::vector<std::uint8_t> bin;
    Builder b;

    // POSITION, with the min/max the base glTF spec requires for a POSITION accessor.
    float min_pos[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity()};
    float max_pos[3] = {-std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()};
    const std::size_t pos_offset = bin.size();
    for (std::size_t s = 0; s < n; ++s) {
        const Vec3f& p = data.positions()[s];
        const float xyz[3] = {p.x, p.y, p.z};
        for (int c = 0; c < 3; ++c) {
            append_f32_le(bin, xyz[c]);
            if (xyz[c] < min_pos[c]) min_pos[c] = xyz[c];
            if (xyz[c] > max_pos[c]) max_pos[c] = xyz[c];
        }
    }
    const std::size_t pos_idx = b.add(pos_offset, 3, n);
    if (n > 0) {
        b.accessors[pos_idx]["min"] = {min_pos[0], min_pos[1], min_pos[2]};
        b.accessors[pos_idx]["max"] = {max_pos[0], max_pos[1], max_pos[2]};
    }

    const std::size_t rot_offset = bin.size();
    for (std::size_t s = 0; s < n; ++s) {
        const Quatf& q = data.rotations()[s];
        append_f32_le(bin, q.x);
        append_f32_le(bin, q.y);
        append_f32_le(bin, q.z);
        append_f32_le(bin, q.w);
    }
    const std::size_t rot_idx = b.add(rot_offset, 4, n);

    const std::size_t scale_offset = bin.size();
    for (std::size_t s = 0; s < n; ++s) {
        const Vec3f& sc = data.scales()[s];
        append_f32_le(bin, sc.x);
        append_f32_le(bin, sc.y);
        append_f32_le(bin, sc.z);
    }
    const std::size_t scale_idx = b.add(scale_offset, 3, n);

    const std::size_t opacity_offset = bin.size();
    for (std::size_t s = 0; s < n; ++s) append_f32_le(bin, data.opacities()[s]);
    const std::size_t opacity_idx = b.add(opacity_offset, 1, n);

    // Spherical harmonics: transpose from splat-major blocks to one accessor per coefficient.
    json attributes = {{khr::kAttrPosition, pos_idx},
                       {khr::kAttrRotation, rot_idx},
                       {khr::kAttrScale, scale_idx},
                       {khr::kAttrOpacity, opacity_idx}};
    for (std::size_t k = 0; k < out_coeffs; ++k) {
        const std::size_t coef_offset = bin.size();
        for (std::size_t s = 0; s < n; ++s) {
            for (std::size_t c = 0; c < 3; ++c) {
                append_f32_le(bin, raw[s * (src_coeffs * 3) + k * 3 + c]);
            }
        }
        const std::size_t idx = b.add(coef_offset, 3, n);
        attributes[khr::sh_attribute(khr::sh_flat_to_address(k))] = idx;
    }

    // Assemble the glTF JSON document.
    json primitive = {{"mode", khr::kPrimitiveModePoints},
                      {"attributes", attributes},
                      {"extensions",
                       {{khr::kExtensionName,
                         {{"kernel", khr::kKernelEllipse},
                          {"colorSpace", khr::to_string(color_space)}}}}}};
    json doc = {{"asset", {{"version", "2.0"}, {"generator", "melkor"}}},
                {"extensionsUsed", json::array({khr::kExtensionName})},
                {"buffers", json::array({{{"byteLength", bin.size()}}})},
                {"bufferViews", b.buffer_views},
                {"accessors", b.accessors},
                {"meshes", json::array({{{"primitives", json::array({primitive})}}})},
                {"nodes", json::array({{{"mesh", 0}}})},
                {"scenes", json::array({{{"nodes", json::array({0})}}})},
                {"scene", 0}};

    const std::string json_text = doc.dump();
    auto glb = glb::build_glb(json_text, bin.empty() ? nullptr : bin.data(), bin.size());
    if (!glb.has_value()) {
        return Result<GlbWriteResult>::failure(glb.error_code(), glb.diagnostics());
    }

    GlbWriteResult result;
    result.bytes = std::move(glb.value());
    result.losses = std::move(losses);
    return Result<GlbWriteResult>::success(std::move(result));
}

}  // namespace melkor::format::gltf
