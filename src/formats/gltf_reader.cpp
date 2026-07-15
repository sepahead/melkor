#include "melkor/format/gltf_reader.hpp"

#include "melkor/format/gltf_accessor.hpp"

#include <string>

namespace melkor::format::gltf {

namespace {

Result<PrimitiveRead> fail(const char* code, std::string message) {
    Diagnostic d(code, Severity::error, std::move(message));
    return Result<PrimitiveRead>::failure(ErrorCode::invalid_data, std::move(d));
}

// Looks up an attribute's accessor index, failing with a clear message if it is absent.
Result<std::uint64_t> attribute_index(const PrimitiveDesc& prim, const std::string& semantic) {
    auto it = prim.attributes.find(semantic);
    if (it == prim.attributes.end()) {
        Diagnostic d("MK2150_GLTF_MISSING_ATTRIBUTE", Severity::error,
                     "splat primitive is missing the required attribute '" + semantic + "'");
        return Result<std::uint64_t>::failure(ErrorCode::invalid_data, std::move(d));
    }
    return Result<std::uint64_t>::success(it->second);
}

}  // namespace

Result<PrimitiveRead> read_primitive_local(const Document& doc, const PrimitiveDesc& prim,
                                           const std::vector<BufferSpan>& buffers) {
    if (!prim.gaussian.has_value()) {
        return fail("MK2151_GLTF_NOT_A_SPLAT",
                    "primitive does not carry the KHR_gaussian_splatting extension");
    }
    if (prim.mode != khr::kPrimitiveModePoints) {
        return fail("MK2152_GLTF_NOT_POINTS",
                    "a KHR_gaussian_splatting primitive must use POINTS mode (0), not mode " +
                        std::to_string(prim.mode));
    }
    if (prim.gaussian->kernel != khr::kKernelEllipse) {
        return fail("MK2153_GLTF_UNSUPPORTED_KERNEL",
                    "unsupported KHR_gaussian_splatting kernel '" + prim.gaussian->kernel +
                        "' (Melkor implements the 'ellipse' kernel)");
    }

    // Resolve the required attribute accessor indices.
    auto pos_i = attribute_index(prim, khr::kAttrPosition);
    if (!pos_i.has_value()) return fail(pos_i.diagnostics()[0].code.c_str(), pos_i.diagnostics()[0].message);
    auto rot_i = attribute_index(prim, khr::kAttrRotation);
    if (!rot_i.has_value()) return fail(rot_i.diagnostics()[0].code.c_str(), rot_i.diagnostics()[0].message);
    auto scl_i = attribute_index(prim, khr::kAttrScale);
    if (!scl_i.has_value()) return fail(scl_i.diagnostics()[0].code.c_str(), scl_i.diagnostics()[0].message);
    auto opa_i = attribute_index(prim, khr::kAttrOpacity);
    if (!opa_i.has_value()) return fail(opa_i.diagnostics()[0].code.c_str(), opa_i.diagnostics()[0].message);
    auto dc_i = attribute_index(prim, khr::sh_attribute({0, 0}));
    if (!dc_i.has_value()) return fail(dc_i.diagnostics()[0].code.c_str(), dc_i.diagnostics()[0].message);

    // The number of splats is the POSITION accessor's count; every other attribute must match it.
    const std::uint64_t n64 = doc.accessors[static_cast<std::size_t>(pos_i.value())].count;

    // Validate the element types are what KHR mandates before trusting the decoded shapes.
    auto matches = [&](std::uint64_t idx, ElementType elem) -> bool {
        return doc.accessors[static_cast<std::size_t>(idx)].element == elem &&
               doc.accessors[static_cast<std::size_t>(idx)].count == n64;
    };
    if (doc.accessors[static_cast<std::size_t>(pos_i.value())].element != ElementType::vec3) {
        return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE", "POSITION must be a VEC3 accessor");
    }
    if (!matches(rot_i.value(), ElementType::vec4)) {
        return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE",
                    "ROTATION must be a VEC4 accessor with the same count as POSITION");
    }
    if (!matches(scl_i.value(), ElementType::vec3)) {
        return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE",
                    "SCALE must be a VEC3 accessor with the same count as POSITION");
    }
    if (!matches(opa_i.value(), ElementType::scalar)) {
        return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE",
                    "OPACITY must be a SCALAR accessor with the same count as POSITION");
    }

    const std::size_t n = static_cast<std::size_t>(n64);

    // Decode the geometry attributes. KHR domains are already canonical (linear scale, linear
    // opacity, unit quaternion x,y,z,w), so these map straight into SplatBufferInput.
    auto positions = resolve_and_decode_accessor(doc, pos_i.value(), buffers);
    if (!positions.has_value()) return fail("MK2155_GLTF_ATTRIBUTE_DECODE", positions.diagnostics().empty() ? "failed to decode POSITION" : positions.diagnostics()[0].message);
    auto rotations = resolve_and_decode_accessor(doc, rot_i.value(), buffers);
    if (!rotations.has_value()) return fail("MK2155_GLTF_ATTRIBUTE_DECODE", rotations.diagnostics().empty() ? "failed to decode ROTATION" : rotations.diagnostics()[0].message);
    auto scales = resolve_and_decode_accessor(doc, scl_i.value(), buffers);
    if (!scales.has_value()) return fail("MK2155_GLTF_ATTRIBUTE_DECODE", scales.diagnostics().empty() ? "failed to decode SCALE" : scales.diagnostics()[0].message);
    auto opacities = resolve_and_decode_accessor(doc, opa_i.value(), buffers);
    if (!opacities.has_value()) return fail("MK2155_GLTF_ATTRIBUTE_DECODE", opacities.diagnostics().empty() ? "failed to decode OPACITY" : opacities.diagnostics()[0].message);

    if (positions.value().size() != n * 3 || rotations.value().size() != n * 4 ||
        scales.value().size() != n * 3 || opacities.value().size() != n) {
        return fail("MK2155_GLTF_ATTRIBUTE_DECODE",
                    "decoded attribute lengths are inconsistent with the splat count");
    }

    // Determine the SH degree present: scan from degree 0 upward while every coefficient of the
    // degree is present. A degree that is partially present is an error (KHR forbids partial
    // degrees), and the profile ceiling is degree 3.
    std::uint32_t degree = 0;
    for (std::uint32_t l = 1; l <= khr::kMaxProfileShDegree; ++l) {
        std::size_t present = 0;
        const std::size_t needed = khr::sh_coefficients_at_degree(l);
        for (std::uint32_t c = 0; c < needed; ++c) {
            if (prim.attributes.count(khr::sh_attribute({l, c})) != 0) ++present;
        }
        if (present == 0) break;              // this degree absent: stop, degree is l-1
        if (present != needed) {
            return fail("MK2156_GLTF_PARTIAL_SH_DEGREE",
                        "spherical-harmonic degree " + std::to_string(l) +
                            " is only partially present; a degree must be complete or absent");
        }
        degree = l;
    }

    // Assemble the SH buffer in the scene model's splat-major block layout. For flat coefficient k
    // (which maps to a KHR (degree, coef) address in the same m-order), the accessor holds all
    // splats' RGB for that coefficient; scatter it into each splat's block.
    const std::size_t coeffs = khr::sh_total_coefficients(degree);
    std::vector<float> sh_data(n * coeffs * 3, 0.0f);
    for (std::size_t k = 0; k < coeffs; ++k) {
        const khr::ShAddress addr = khr::sh_flat_to_address(k);
        auto idx = attribute_index(prim, khr::sh_attribute(addr));
        if (!idx.has_value()) {
            // Guaranteed present by the degree scan above, but re-check for isolation safety.
            return fail("MK2150_GLTF_MISSING_ATTRIBUTE", idx.diagnostics()[0].message);
        }
        if (doc.accessors[static_cast<std::size_t>(idx.value())].element != ElementType::vec3 ||
            doc.accessors[static_cast<std::size_t>(idx.value())].count != n64) {
            return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE",
                        "an SH coefficient accessor must be VEC3 with the same count as POSITION");
        }
        auto coef = resolve_and_decode_accessor(doc, idx.value(), buffers);
        if (!coef.has_value()) {
            return fail("MK2155_GLTF_ATTRIBUTE_DECODE",
                        coef.diagnostics().empty() ? "failed to decode an SH coefficient"
                                                   : coef.diagnostics()[0].message);
        }
        if (coef.value().size() != n * 3) {
            return fail("MK2155_GLTF_ATTRIBUTE_DECODE", "an SH coefficient decoded to the wrong length");
        }
        for (std::size_t s = 0; s < n; ++s) {
            for (std::size_t c = 0; c < 3; ++c) {
                sh_data[s * (coeffs * 3) + k * 3 + c] = coef.value()[s * 3 + c];
            }
        }
    }

    auto sh = ShBuffer::create(degree, n, std::move(sh_data));
    if (!sh.has_value()) {
        return fail(sh.diagnostics().empty() ? "MK2157_GLTF_SH_INVALID" : sh.diagnostics()[0].code.c_str(),
                    sh.diagnostics().empty() ? "invalid SH buffer" : sh.diagnostics()[0].message);
    }

    SplatBufferInput input;
    input.positions.reserve(n);
    input.scales.reserve(n);
    input.rotations.reserve(n);
    input.opacities.reserve(n);
    for (std::size_t s = 0; s < n; ++s) {
        input.positions.push_back(Vec3f{positions.value()[s * 3 + 0], positions.value()[s * 3 + 1],
                                        positions.value()[s * 3 + 2]});
        input.scales.push_back(
            Vec3f{scales.value()[s * 3 + 0], scales.value()[s * 3 + 1], scales.value()[s * 3 + 2]});
        input.rotations.push_back(Quatf{rotations.value()[s * 4 + 0], rotations.value()[s * 4 + 1],
                                        rotations.value()[s * 4 + 2], rotations.value()[s * 4 + 3]});
        input.opacities.push_back(opacities.value()[s]);
    }
    input.sh = std::move(sh.value());

    auto splats = SplatData::create(std::move(input));
    if (!splats.has_value()) {
        return fail(splats.diagnostics().empty() ? "MK2158_GLTF_SPLAT_INVALID"
                                                 : splats.diagnostics()[0].code.c_str(),
                    splats.diagnostics().empty() ? "the decoded splats failed validation"
                                                 : splats.diagnostics()[0].message);
    }

    PrimitiveRead result{std::move(splats.value()), khr::ColorSpace::srgb_rec709_display, false,
                         degree};
    auto cs = khr::color_space_from_string(prim.gaussian->color_space);
    if (cs.has_value()) {
        result.color_space = cs.value();
    } else {
        result.color_space = khr::ColorSpace::srgb_rec709_display;
        result.color_space_assumed = true;
    }
    return Result<PrimitiveRead>::success(std::move(result));
}

}  // namespace melkor::format::gltf
