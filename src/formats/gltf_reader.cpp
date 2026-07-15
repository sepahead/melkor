#include "melkor/format/gltf_reader.hpp"

#include "melkor/format/glb_container.hpp"
#include "melkor/format/gltf_accessor.hpp"
#include "melkor/format/gltf_document.hpp"
#include "melkor/format/gltf_extensions.hpp"
#include "melkor/format/gltf_transform.hpp"
#include "melkor/math/covariance.hpp"
#include "melkor/math/sh_rotation.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

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
                                           const std::vector<BufferSpan>& buffers, Budget& budget) {
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

    // Charge the splat count before allocating anything sized by it, so a primitive declaring an
    // enormous count (or a mesh instantiated by many nodes) is refused before exhausting memory.
    if (auto charged = budget.consume(BudgetKind::splats, n64, "gltf.primitive.splats");
        !charged.has_value()) {
        return Result<PrimitiveRead>::failure(charged.error_code(), charged.diagnostics());
    }

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
    // The scan stops at the first absent degree. Any SH coefficient present for a *higher* degree is
    // a gap in the pyramid, which the KHR spec forbids ("either all coefficients for a given degree
    // and all lower degrees MUST be defined or none"). Reject it rather than silently dropping the
    // higher-degree colour, which would be a hidden loss.
    for (std::uint32_t l = degree + 1; l <= khr::kMaxProfileShDegree; ++l) {
        for (std::uint32_t c = 0; c < khr::sh_coefficients_at_degree(l); ++c) {
            if (prim.attributes.count(khr::sh_attribute({l, c})) != 0) {
                return fail("MK2156_GLTF_PARTIAL_SH_DEGREE",
                            "spherical-harmonic coefficients are present for degree " +
                                std::to_string(l) +
                                " but a lower degree is absent; the SH pyramid must be contiguous");
            }
        }
    }

    // Assemble the SH buffer in the scene model's splat-major block layout. For flat coefficient k
    // (which maps to a KHR (degree, coef) address in the same m-order), the accessor holds all
    // splats' RGB for that coefficient; scatter it into each splat's block.
    const std::size_t coeffs = khr::sh_total_coefficients(degree);
    // Charge the SH allocation before making it. n is already bounded by the splats charge above
    // (n <= max_splats) and coeffs <= 16, so this product cannot overflow.
    const std::uint64_t sh_bytes = static_cast<std::uint64_t>(n) * coeffs * 3u * sizeof(float);
    if (auto charged = budget.consume(BudgetKind::memory_bytes, sh_bytes, "gltf.primitive.sh");
        !charged.has_value()) {
        return Result<PrimitiveRead>::failure(charged.error_code(), charged.diagnostics());
    }
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
        // Renormalize the decoded quaternion, mirroring the node-quaternion path. KHR permits
        // ROTATION in normalized signed-byte/short encodings whose quantization error (up to ~1/127)
        // exceeds SplatData's unit-quaternion tolerance; without this a spec-valid asset would be
        // rejected. A genuinely degenerate (near-zero) quaternion still fails, reported by
        // SplatData::create.
        math::Quat q{rotations.value()[s * 4 + 0], rotations.value()[s * 4 + 1],
                     rotations.value()[s * 4 + 2], rotations.value()[s * 4 + 3]};
        auto qn = math::normalize(q);
        const math::Quat u = qn.has_value() ? qn.value() : q;
        input.rotations.push_back(Quatf{static_cast<float>(u.x), static_cast<float>(u.y),
                                        static_cast<float>(u.z), static_cast<float>(u.w)});
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

    // An unrecognised colorSpace string is assumed sRGB (and reported), but the raw string is kept
    // so the scene reader can still tell two different unrecognised spaces apart.
    auto cs = khr::color_space_from_string(prim.gaussian->color_space);
    const khr::ColorSpace color_space =
        cs.has_value() ? cs.value() : khr::ColorSpace::srgb_rec709_display;
    PrimitiveRead result{std::move(splats.value()), color_space, !cs.has_value(),
                         prim.gaussian->color_space, degree};
    return Result<PrimitiveRead>::success(std::move(result));
}

namespace {

// Whether a linear map is a pure, axis-aligned positive scale (no rotation or reflection). If so,
// spherical harmonics need no rotation under this transform; otherwise a rotation is present and
// degree>=1 SH would need a Wigner-D rotation we do not yet apply. The tolerance is relative to the
// largest element so it scales with the transform's magnitude.
bool is_pure_positive_scale(const math::Mat3& l) {
    double maxabs = 0.0;
    for (double e : l) maxabs = std::max(maxabs, std::fabs(e));
    const double eps = 1e-6 * std::max(1.0, maxabs);
    const bool off_zero = std::fabs(l[1]) < eps && std::fabs(l[2]) < eps && std::fabs(l[3]) < eps &&
                          std::fabs(l[5]) < eps && std::fabs(l[6]) < eps && std::fabs(l[7]) < eps;
    const bool diag_pos = l[0] > eps && l[4] > eps && l[8] > eps;
    return off_zero && diag_pos;
}

// Whether a linear map is the identity within tolerance. When a node contributes no linear part
// (only translation, or nothing), the covariance is unchanged, so the splat's rotation and scale
// are kept exactly rather than round-tripped through an eigendecomposition -- which would preserve
// the covariance but re-express it with permuted axes or a sign-flipped quaternion, needlessly
// perturbing the common case of splats already authored in world space.
bool is_identity_linear(const math::Mat3& l) {
    const double eps = 1e-9;
    return std::fabs(l[0] - 1.0) < eps && std::fabs(l[4] - 1.0) < eps && std::fabs(l[8] - 1.0) < eps &&
           std::fabs(l[1]) < eps && std::fabs(l[2]) < eps && std::fabs(l[3]) < eps &&
           std::fabs(l[5]) < eps && std::fabs(l[6]) < eps && std::fabs(l[7]) < eps;
}

// A primitive's splats after its node transform has been applied: geometry in world space, SH still
// in the source frame, tagged with the source degree so the merge can pad to a common degree.
struct TransformedBatch {
    std::vector<Vec3f> positions;
    std::vector<Vec3f> scales;
    std::vector<Quatf> rotations;
    std::vector<float> opacities;
    std::vector<float> sh;  // splat-major blocks at `degree`
    std::uint32_t degree = 0;
    std::uint64_t dropped = 0;  // splats a singular node transform collapsed
};

// Applies a node transform to one primitive's local splats. When `sh_rot` is non-null, each splat's
// spherical harmonics are rotated by it (the node's rotation); otherwise the SH are carried through
// unchanged (correct for an identity or pure-scale node, or reported as an un-applied loss by the
// caller for a rotation combined with scale/shear).
TransformedBatch transform_primitive(const PrimitiveRead& pr, const NodeTransform& xform,
                                     const math::ShRotation* sh_rot) {
    TransformedBatch batch;
    batch.degree = pr.source_sh_degree;
    const std::size_t coeffs = khr::sh_total_coefficients(pr.source_sh_degree);
    const std::size_t block = coeffs * 3;
    const auto& raw = pr.data.sh().raw();
    const bool identity = is_identity_linear(xform.linear);
    for (std::size_t s = 0; s < pr.data.size(); ++s) {
        const Vec3f& p = pr.data.positions()[s];
        const Vec3f& sc = pr.data.scales()[s];
        const Quatf& q = pr.data.rotations()[s];

        Vec3f out_scale = sc;
        Quatf out_rot = q;
        if (!identity) {
            auto rs = math::affine_transform_gaussian(
                xform.linear, math::Quat{q.x, q.y, q.z, q.w}, math::Vec3{sc.x, sc.y, sc.z});
            if (!rs.has_value()) {
                ++batch.dropped;  // a singular transform collapses this Gaussian; drop it honestly
                continue;
            }
            out_scale = Vec3f{static_cast<float>(rs.value().scale[0]),
                              static_cast<float>(rs.value().scale[1]),
                              static_cast<float>(rs.value().scale[2])};
            out_rot = Quatf{static_cast<float>(rs.value().rotation.x),
                            static_cast<float>(rs.value().rotation.y),
                            static_cast<float>(rs.value().rotation.z),
                            static_cast<float>(rs.value().rotation.w)};
        }
        const math::Vec3 mean = apply_point(xform, math::Vec3{p.x, p.y, p.z});
        batch.positions.push_back(Vec3f{static_cast<float>(mean[0]), static_cast<float>(mean[1]),
                                        static_cast<float>(mean[2])});
        batch.scales.push_back(out_scale);
        batch.rotations.push_back(out_rot);
        batch.opacities.push_back(pr.data.opacities()[s]);
        // Carry the SH block through, rotating it by the node rotation when one applies.
        std::vector<float> sh_block(raw.begin() + static_cast<std::ptrdiff_t>(s * block),
                                    raw.begin() + static_cast<std::ptrdiff_t>((s + 1) * block));
        if (sh_rot != nullptr) {
            sh_rot->rotate_block(sh_block.data(), 3);
        }
        batch.sh.insert(batch.sh.end(), sh_block.begin(), sh_block.end());
    }
    return batch;
}

Result<SceneRead> scene_fail(ErrorCode code, const char* diag_code, std::string message) {
    Diagnostic d(diag_code, Severity::error, std::move(message));
    return Result<SceneRead>::failure(code, std::move(d));
}

}  // namespace

Result<SceneRead> read_gaussian_scene(const Document& doc, const std::vector<BufferSpan>& buffers,
                                      const Limits& limits) {
    Budget budget(limits);

    // 1. Extension gate: an unsupported *required* extension makes the asset unreadable.
    auto ext = evaluate_extensions(doc.extensions_used, doc.extensions_required);
    if (!ext.unsupported_required.empty()) {
        std::string names;
        for (const auto& n : ext.unsupported_required) {
            if (!names.empty()) names += ", ";
            names += n;
        }
        return scene_fail(ErrorCode::unsupported_feature, "MK2160_GLTF_UNSUPPORTED_REQUIRED",
                          "the asset requires glTF extensions Melkor does not implement: " + names);
    }

    // 2. Walk the default scene, collecting each splat primitive with the global transform of the
    // node that instantiates it. Iterative with a visited-set: a cyclic or shared-node graph
    // terminates instead of looping.
    struct Instance {
        const PrimitiveDesc* prim;
        NodeTransform transform;
    };
    std::vector<Instance> instances;
    std::vector<char> visited(doc.nodes.size(), 0);
    struct Frame {
        std::uint64_t node;
        NodeTransform parent;
    };
    std::vector<Frame> stack;
    for (std::uint64_t root : doc.scene_roots) stack.push_back(Frame{root, identity_transform()});
    while (!stack.empty()) {
        Frame f = stack.back();
        stack.pop_back();
        if (f.node >= doc.nodes.size() || visited[static_cast<std::size_t>(f.node)]) continue;
        visited[static_cast<std::size_t>(f.node)] = 1;
        const NodeDesc& node = doc.nodes[static_cast<std::size_t>(f.node)];
        const NodeTransform global = compose(f.parent, local_node_transform(node));
        if (node.mesh.has_value()) {
            const MeshDesc& mesh = doc.meshes[static_cast<std::size_t>(node.mesh.value())];
            for (const auto& prim : mesh.primitives) {
                if (prim.gaussian.has_value()) {
                    // Charge each instantiation. A mesh shared by many nodes multiplies here, so
                    // bounding the instance count stops a small file from describing an unbounded
                    // scene before any splats are even read.
                    if (auto charged = budget.consume(BudgetKind::gltf_nodes, 1, "gltf.instance");
                        !charged.has_value()) {
                        return scene_fail(charged.error_code(),
                                          charged.diagnostics().empty()
                                              ? "MK2167_GLTF_BUDGET"
                                              : charged.diagnostics()[0].code.c_str(),
                                          charged.diagnostics().empty()
                                              ? "glTF scene exceeds the node budget"
                                              : charged.diagnostics()[0].message);
                    }
                    instances.push_back(Instance{&prim, global});
                }
            }
        }
        for (std::uint64_t child : node.children) {
            if (child < doc.nodes.size() && !visited[static_cast<std::size_t>(child)]) {
                stack.push_back(Frame{child, global});
            }
        }
    }

    if (instances.empty()) {
        return scene_fail(ErrorCode::invalid_data, "MK2161_GLTF_NO_SPLATS",
                          "no KHR_gaussian_splatting primitive is reachable from the default scene");
    }

    // 3. Read and transform every primitive; accumulate losses.
    LossReport losses;
    std::vector<TransformedBatch> batches;
    khr::ColorSpace first_cs = khr::ColorSpace::srgb_rec709_display;
    std::string first_cs_raw;
    bool have_cs = false;
    bool mixed_cs = false;
    bool any_assumed = false;
    std::uint64_t total_dropped = 0;

    for (const auto& inst : instances) {
        auto pr = read_primitive_local(doc, *inst.prim, buffers, budget);
        if (!pr.has_value()) {
            return scene_fail(pr.error_code(),
                              pr.diagnostics().empty() ? "MK2162_GLTF_PRIMITIVE_READ"
                                                       : pr.diagnostics()[0].code.c_str(),
                              pr.diagnostics().empty() ? "failed to read a splat primitive"
                                                       : pr.diagnostics()[0].message);
        }
        if (!have_cs) {
            first_cs = pr.value().color_space;
            first_cs_raw = pr.value().color_space_raw;
            have_cs = true;
        } else if (pr.value().color_space_raw != first_cs_raw) {
            // Compare the *declared* strings, so two different unrecognised spaces (both coerced to
            // the sRGB enum) are still detected as a conflict, not silently merged.
            mixed_cs = true;
        }
        if (pr.value().color_space_assumed) any_assumed = true;

        // Decide how the node's transform rotates the spherical harmonics. A pure rotation is used
        // directly; a rotation combined with scale/shear has its rotation component extracted by
        // polar decomposition; an identity or pure scale leaves SH untouched (correct). Only a
        // reflection or singular map -- where no proper rotation is defined -- reports a loss.
        std::optional<math::ShRotation> sh_rot;
        const math::ShRotation* sh_rot_ptr = nullptr;
        if (pr.value().source_sh_degree >= 1 && !is_identity_linear(inst.transform.linear) &&
            !is_pure_positive_scale(inst.transform.linear)) {
            const math::Mat3& linear = inst.transform.linear;
            // Use the linear part directly when it is already a proper rotation; otherwise recover
            // the rotation component from a scaled/sheared transform.
            Result<math::Mat3> rot_matrix =
                math::is_proper_rotation(linear)
                    ? Result<math::Mat3>::success(linear)
                    : math::rotation_from_linear(linear);
            bool applied = false;
            if (rot_matrix.has_value()) {
                auto r = math::ShRotation::create(rot_matrix.value(), pr.value().source_sh_degree);
                if (r.has_value()) {
                    sh_rot = std::move(r.value());
                    sh_rot_ptr = &sh_rot.value();
                    applied = true;
                }
            }
            if (!applied) {
                LossItem item;
                item.code = loss_code::kShRotationNotApplied;
                item.severity = LossSeverity::severe;
                item.source_feature =
                    "degree>=1 spherical harmonics under a node transform with no proper-rotation "
                    "component (a reflection or a degenerate/singular transform)";
                item.target_constraint =
                    "SH rotation is defined only for a proper rotation; a reflecting or collapsing "
                    "transform has none";
                item.affected_splats = pr.value().data.size();
                item.remediation =
                    "remove the reflection or degenerate scale from the node transform, or approve "
                    "LOSS_SH_ROTATION_NOT_APPLIED to accept colour in the source frame";
                losses.add(std::move(item));
            }
        }

        auto batch = transform_primitive(pr.value(), inst.transform, sh_rot_ptr);
        total_dropped += batch.dropped;
        if (!batch.positions.empty()) batches.push_back(std::move(batch));
    }

    if (batches.empty()) {
        return scene_fail(ErrorCode::invalid_data, "MK2161_GLTF_NO_SPLATS",
                          "every splat was dropped by a singular node transform");
    }

    // 4. Merge, padding each batch's SH up to the maximum degree present.
    std::uint32_t max_degree = 0;
    std::size_t total = 0;
    for (const auto& b : batches) {
        max_degree = std::max(max_degree, b.degree);
        total += b.positions.size();
    }
    const std::size_t merged_coeffs = khr::sh_total_coefficients(max_degree);
    const std::size_t merged_block = merged_coeffs * 3;

    SplatBufferInput input;
    input.positions.reserve(total);
    input.scales.reserve(total);
    input.rotations.reserve(total);
    input.opacities.reserve(total);
    std::vector<float> merged_sh(total * merged_block, 0.0f);

    std::size_t write = 0;
    for (const auto& b : batches) {
        const std::size_t src_block = khr::sh_total_coefficients(b.degree) * 3;
        for (std::size_t s = 0; s < b.positions.size(); ++s) {
            input.positions.push_back(b.positions[s]);
            input.scales.push_back(b.scales[s]);
            input.rotations.push_back(b.rotations[s]);
            input.opacities.push_back(b.opacities[s]);
            // Copy the splat's coefficients into the front of its max-degree block; the higher
            // coefficients stay zero-initialised.
            for (std::size_t i = 0; i < src_block; ++i) {
                merged_sh[write * merged_block + i] = b.sh[s * src_block + i];
            }
            ++write;
        }
    }

    auto sh = ShBuffer::create(max_degree, total, std::move(merged_sh));
    if (!sh.has_value()) {
        return scene_fail(ErrorCode::invalid_data,
                          sh.diagnostics().empty() ? "MK2163_GLTF_SH_MERGE"
                                                   : sh.diagnostics()[0].code.c_str(),
                          sh.diagnostics().empty() ? "failed to merge spherical harmonics"
                                                   : sh.diagnostics()[0].message);
    }
    input.sh = std::move(sh.value());

    auto merged = SplatData::create(std::move(input));
    if (!merged.has_value()) {
        return scene_fail(ErrorCode::invalid_data,
                          merged.diagnostics().empty() ? "MK2164_GLTF_MERGE"
                                                       : merged.diagnostics()[0].code.c_str(),
                          merged.diagnostics().empty() ? "the merged splats failed validation"
                                                       : merged.diagnostics()[0].message);
    }

    // 5. Remaining losses.
    if (batches.size() > 1) {
        LossItem item;
        item.code = loss_code::kSceneGraphFlattened;
        item.severity = LossSeverity::info;
        item.source_feature = std::to_string(batches.size()) + " splat primitives across the scene graph";
        item.target_constraint = "the canonical model is a single flat splat cloud";
        item.affected_splats = total;
        item.remediation = "none needed; the geometry is preserved, only the node hierarchy is not";
        losses.add(std::move(item));
    }
    if (mixed_cs || any_assumed) {
        LossItem item;
        item.code = loss_code::kColorSpaceAssumed;
        item.severity = mixed_cs ? LossSeverity::severe : LossSeverity::warning;
        item.source_feature = mixed_cs ? "primitives declaring different colour spaces"
                                       : "a colorSpace string Melkor does not recognise";
        item.target_constraint = "the canonical model carries one colour space; sRGB was assumed";
        item.affected_splats = total;
        item.remediation = mixed_cs
                               ? "re-export the asset with a single colour space"
                               : "approve LOSS_COLOR_SPACE_ASSUMED to accept the sRGB assumption";
        losses.add(std::move(item));
    }
    if (total_dropped > 0) {
        LossItem item;
        item.code = loss_code::kInvalidSplatDropped;
        item.severity = LossSeverity::warning;
        item.source_feature = "splats under a singular (zero-scale) node transform";
        item.target_constraint = "a collapsed Gaussian has no valid covariance";
        item.affected_splats = total_dropped;
        item.remediation = "remove the degenerate node scale before conversion";
        losses.add(std::move(item));
    }

    SceneRead out{std::move(merged.value()), std::move(losses), first_cs, max_degree};
    return Result<SceneRead>::success(std::move(out));
}

Result<SceneRead> read_glb(const std::uint8_t* data, std::size_t size, const Limits& limits) {
    auto framing = glb::parse_glb(data, size);
    if (!framing.has_value()) {
        return scene_fail(framing.error_code(),
                          framing.diagnostics().empty() ? "MK2165_GLB_INVALID"
                                                        : framing.diagnostics()[0].code.c_str(),
                          framing.diagnostics().empty() ? "invalid GLB container"
                                                        : framing.diagnostics()[0].message);
    }
    const ByteRange& json = framing.value().json;
    auto doc = parse_gltf_json(data + json.offset, static_cast<std::size_t>(json.length));
    if (!doc.has_value()) {
        return scene_fail(doc.error_code(),
                          doc.diagnostics().empty() ? "MK2166_GLB_JSON"
                                                    : doc.diagnostics()[0].code.c_str(),
                          doc.diagnostics().empty() ? "invalid glTF JSON in GLB"
                                                    : doc.diagnostics()[0].message);
    }

    // The single embedded binary buffer is glTF buffer 0. If there is no BIN chunk, buffer 0 has no
    // bytes; a bufferView referencing it then resolves to a clean "buffer unavailable" error.
    std::vector<BufferSpan> buffers;
    if (framing.value().bin.has_value()) {
        const ByteRange& bin = framing.value().bin.value();
        buffers.push_back(BufferSpan{data + bin.offset, static_cast<std::size_t>(bin.length)});
    } else {
        buffers.push_back(BufferSpan{nullptr, 0});
    }
    return read_gaussian_scene(doc.value(), buffers, limits);
}

}  // namespace melkor::format::gltf
