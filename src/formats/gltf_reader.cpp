#include "melkor/format/gltf_reader.hpp"

#include "melkor/checked.hpp"
#include "melkor/format/glb_container.hpp"
#include "melkor/format/gltf_accessor.hpp"
#include "melkor/format/gltf_document.hpp"
#include "melkor/format/gltf_extensions.hpp"
#include "melkor/format/gltf_transform.hpp"
#include "melkor/math/covariance.hpp"
#include "melkor/math/sh_rotation.hpp"

#include "gltf_usage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace melkor::format::gltf {

namespace {

constexpr std::uint64_t kCanonicalBytesPerSplat =
    sizeof(Vec3f) * 2u + sizeof(Quatf) + sizeof(float);
constexpr std::size_t kControlInterval = 1024;

class ScopedBudgetRelease {
public:
    ScopedBudgetRelease() = default;
    ScopedBudgetRelease(Budget* budget, BudgetKind kind, std::uint64_t amount) noexcept
        : budget_(budget), kind_(kind), amount_(amount) {}

    ~ScopedBudgetRelease() {
        if (budget_ != nullptr && amount_ != 0)
            budget_->release(kind_, amount_);
    }

    ScopedBudgetRelease(const ScopedBudgetRelease&) = delete;
    ScopedBudgetRelease& operator=(const ScopedBudgetRelease&) = delete;

    void add(std::uint64_t amount) noexcept { amount_ += amount; }
    void retain() noexcept {
        budget_ = nullptr;
        amount_ = 0;
    }

private:
    Budget* budget_ = nullptr;
    BudgetKind kind_ = BudgetKind::memory_bytes;
    std::uint64_t amount_ = 0;
};

Result<PrimitiveRead> fail_with(ErrorCode error_code, const char* code, std::string message) {
    Diagnostic d(code, Severity::error, std::move(message));
    return Result<PrimitiveRead>::failure(error_code, std::move(d));
}

Result<PrimitiveRead> fail(const char* code, std::string message) {
    return fail_with(ErrorCode::invalid_data, code, std::move(message));
}

Result<PrimitiveRead> primitive_control_failure(const Result<void>& control) {
    return Result<PrimitiveRead>::failure(control.error_code(), control.diagnostics());
}

bool is_plain_f32(const AccessorDesc& accessor) {
    return accessor.component == ComponentType::f32 && !accessor.normalized;
}

bool is_rotation_encoding(const AccessorDesc& accessor) {
    return is_plain_f32(accessor) ||
           ((accessor.component == ComponentType::i8 || accessor.component == ComponentType::i16) &&
            accessor.normalized);
}

bool is_scale_encoding(const AccessorDesc& accessor) {
    return is_plain_f32(accessor) || accessor.component == ComponentType::u8 ||
           accessor.component == ComponentType::u16;
}

bool is_opacity_encoding(const AccessorDesc& accessor) {
    return is_plain_f32(accessor) ||
           ((accessor.component == ComponentType::u8 || accessor.component == ComponentType::u16) &&
            accessor.normalized);
}

// Looks up an attribute's accessor index, failing with a clear message if it is absent.
Result<std::uint64_t> attribute_index(const Document& doc, const PrimitiveDesc& prim,
                                      const std::string& semantic) {
    auto it = prim.attributes.find(semantic);
    if (it == prim.attributes.end()) {
        Diagnostic d("MK2150_GLTF_MISSING_ATTRIBUTE", Severity::error,
                     "splat primitive is missing the required attribute '" + semantic + "'");
        return Result<std::uint64_t>::failure(ErrorCode::invalid_data, std::move(d));
    }
    if (it->second >= doc.accessors.size()) {
        Diagnostic d("MK2157_GLTF_ACCESSOR_INDEX", Severity::error,
                     "splat attribute accessor index is outside the accessor array");
        d.with_context("semantic", semantic)
            .with_context("accessor", it->second)
            .with_context("accessor_count", static_cast<std::uint64_t>(doc.accessors.size()));
        return Result<std::uint64_t>::failure(ErrorCode::invalid_data, std::move(d));
    }
    return Result<std::uint64_t>::success(it->second);
}

std::string feature_description(const char* label, std::uint64_t count,
                                const std::vector<std::string>& samples) {
    std::string description = label;
    description += " (" + std::to_string(count) + "): ";
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i != 0)
            description += ", ";
        description += samples[i];
    }
    if (count > samples.size())
        description += ", ...";
    return description;
}

}  // namespace

Result<PrimitiveRead> read_primitive_local(const Document& doc, const PrimitiveDesc& prim,
                                           const std::vector<BufferSpan>& buffers,
                                           Budget& budget) try {
    OperationContext context = make_default_context(budget);
    return read_primitive_local(doc, prim, buffers, context);
} catch (const std::bad_alloc&) {
    return fail_with(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                     "memory allocation failed while reading a glTF primitive");
} catch (const std::length_error&) {
    return fail_with(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                     "a glTF primitive allocation exceeds the host size limit");
}

Result<PrimitiveRead> read_primitive_local(const Document& doc, const PrimitiveDesc& prim,
                                           const std::vector<BufferSpan>& buffers,
                                           const OperationContext& context) try {
    if (context.budget == nullptr) {
        return fail_with(ErrorCode::internal_error, "MK0310_NO_BUDGET",
                         "the glTF reader requires a resource budget");
    }
    if (auto valid = context.budget->limits().validate(); !valid.has_value()) {
        return Result<PrimitiveRead>::failure(valid.error_code(), valid.diagnostics());
    }
    auto started = context.checkpoint({"gltf.read", "primitive", 0, 0, "splats"});
    if (!started.has_value()) {
        return primitive_control_failure(started);
    }
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
    if (prim.gaussian->projection != khr::kProjectionPerspective) {
        return fail_with(ErrorCode::unsupported_feature, "MK2159_GLTF_UNSUPPORTED_PROFILE",
                         "unsupported KHR_gaussian_splatting projection '" +
                             prim.gaussian->projection + "'");
    }
    if (prim.gaussian->sorting_method != khr::kSortingCameraDistance) {
        return fail_with(ErrorCode::unsupported_feature, "MK2159_GLTF_UNSUPPORTED_PROFILE",
                         "unsupported KHR_gaussian_splatting sortingMethod '" +
                             prim.gaussian->sorting_method + "'");
    }
    auto color_space = khr::color_space_from_string(prim.gaussian->color_space);
    if (!color_space.has_value()) {
        return fail_with(ErrorCode::unsupported_feature, "MK2159_GLTF_UNSUPPORTED_PROFILE",
                         "unsupported KHR_gaussian_splatting colorSpace '" +
                             prim.gaussian->color_space + "'");
    }

    // Resolve the required attribute accessor indices.
    auto pos_i = attribute_index(doc, prim, khr::kAttrPosition);
    if (!pos_i.has_value())
        return Result<PrimitiveRead>::failure(pos_i.error_code(), pos_i.diagnostics());
    auto rot_i = attribute_index(doc, prim, khr::kAttrRotation);
    if (!rot_i.has_value())
        return Result<PrimitiveRead>::failure(rot_i.error_code(), rot_i.diagnostics());
    auto scl_i = attribute_index(doc, prim, khr::kAttrScale);
    if (!scl_i.has_value())
        return Result<PrimitiveRead>::failure(scl_i.error_code(), scl_i.diagnostics());
    auto opa_i = attribute_index(doc, prim, khr::kAttrOpacity);
    if (!opa_i.has_value())
        return Result<PrimitiveRead>::failure(opa_i.error_code(), opa_i.diagnostics());
    auto dc_i = attribute_index(doc, prim, khr::sh_attribute({0, 0}));
    if (!dc_i.has_value())
        return Result<PrimitiveRead>::failure(dc_i.error_code(), dc_i.diagnostics());

    // The number of splats is the POSITION accessor's count; every other attribute must match it.
    const std::uint64_t n64 = doc.accessors[static_cast<std::size_t>(pos_i.value())].count;

    // Validate each element shape and component encoding before decode.
    auto matches = [&](std::uint64_t idx, ElementType elem) -> bool {
        return doc.accessors[static_cast<std::size_t>(idx)].element == elem &&
               doc.accessors[static_cast<std::size_t>(idx)].count == n64;
    };
    const AccessorDesc& position = doc.accessors[static_cast<std::size_t>(pos_i.value())];
    const AccessorDesc& rotation = doc.accessors[static_cast<std::size_t>(rot_i.value())];
    const AccessorDesc& scale = doc.accessors[static_cast<std::size_t>(scl_i.value())];
    const AccessorDesc& opacity = doc.accessors[static_cast<std::size_t>(opa_i.value())];
    if (position.element != ElementType::vec3 || !is_plain_f32(position)) {
        return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE",
                    "POSITION must be a non-normalized FLOAT VEC3 accessor");
    }
    if (!matches(rot_i.value(), ElementType::vec4) || !is_rotation_encoding(rotation)) {
        return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE",
                    "ROTATION must use a permitted VEC4 encoding and match POSITION count");
    }
    if (!matches(scl_i.value(), ElementType::vec3) || !is_scale_encoding(scale)) {
        return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE",
                    "SCALE must use a permitted VEC3 encoding and match POSITION count");
    }
    if (!matches(opa_i.value(), ElementType::scalar) || !is_opacity_encoding(opacity)) {
        return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE",
                    "OPACITY must use a permitted SCALAR encoding and match POSITION count");
    }

    if (n64 == 0) {
        return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE", "splat accessors must have a nonzero count");
    }
    auto n_checked = checked_size_cast(n64, "glTF splat count");
    if (!n_checked.has_value()) {
        return Result<PrimitiveRead>::failure(n_checked.error_code(), n_checked.diagnostics());
    }
    const std::size_t n = n_checked.value();

    auto canonical_bytes =
        checked_mul(n64, kCanonicalBytesPerSplat, "glTF canonical geometry bytes");
    if (!canonical_bytes.has_value()) {
        return Result<PrimitiveRead>::failure(canonical_bytes.error_code(),
                                              canonical_bytes.diagnostics());
    }
    if (auto charged = context.consume(BudgetKind::memory_bytes, canonical_bytes.value(),
                                       "gltf.primitive.canonical_geometry");
        !charged.has_value()) {
        return Result<PrimitiveRead>::failure(charged.error_code(), charged.diagnostics());
    }
    ScopedBudgetRelease canonical_release(context.budget, BudgetKind::memory_bytes,
                                          canonical_bytes.value());

    // Decode the geometry attributes. KHR domains are already canonical (linear scale, linear
    // opacity, unit quaternion x,y,z,w), so these map straight into SplatBufferInput.
    auto positions = resolve_and_decode_accessor(doc, pos_i.value(), buffers, context);
    if (!positions.has_value()) {
        return Result<PrimitiveRead>::failure(positions.error_code(), positions.diagnostics());
    }
    auto rotations = resolve_and_decode_accessor(doc, rot_i.value(), buffers, context);
    if (!rotations.has_value()) {
        return Result<PrimitiveRead>::failure(rotations.error_code(), rotations.diagnostics());
    }
    auto scales = resolve_and_decode_accessor(doc, scl_i.value(), buffers, context);
    if (!scales.has_value()) {
        return Result<PrimitiveRead>::failure(scales.error_code(), scales.diagnostics());
    }
    auto opacities = resolve_and_decode_accessor(doc, opa_i.value(), buffers, context);
    if (!opacities.has_value()) {
        return Result<PrimitiveRead>::failure(opacities.error_code(), opacities.diagnostics());
    }

    if (positions.value().size() != n * 3 || rotations.value().size() != n * 4 ||
        scales.value().size() != n * 3 || opacities.value().size() != n) {
        return fail("MK2155_GLTF_ATTRIBUTE_DECODE",
                    "decoded attribute lengths are inconsistent with the splat count");
    }

    for (const auto& [semantic, accessor] : prim.attributes) {
        (void)accessor;
        auto address = khr::parse_sh_attribute(semantic);
        if (address.has_value() && address->degree > khr::kMaxProfileShDegree) {
            return fail_with(ErrorCode::unsupported_feature, "MK2159_GLTF_UNSUPPORTED_PROFILE",
                             "the KHR_gaussian_splatting profile supports SH degree 0 through 3");
        }
    }

    // Find the complete, contiguous SH pyramid.
    std::uint32_t degree = 0;
    for (std::uint32_t l = 1; l <= khr::kMaxProfileShDegree; ++l) {
        std::size_t present = 0;
        const std::size_t needed = khr::sh_coefficients_at_degree(l);
        for (std::uint32_t c = 0; c < needed; ++c) {
            if (prim.attributes.count(khr::sh_attribute({l, c})) != 0)
                ++present;
        }
        if (present == 0)
            break;  // this degree absent: stop, degree is l-1
        if (present != needed) {
            return fail("MK2156_GLTF_PARTIAL_SH_DEGREE",
                        "spherical-harmonic degree " + std::to_string(l) +
                            " is only partially present; a degree must be complete or absent");
        }
        degree = l;
    }
    // Reject a coefficient above a missing degree.
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

    // Validate all SH accessors before allocation.
    const std::size_t coeffs = khr::sh_total_coefficients(degree);
    for (std::size_t k = 0; k < coeffs; ++k) {
        const auto address = khr::sh_flat_to_address(k);
        if (!address.has_value()) {
            return fail_with(ErrorCode::internal_error, "MK2166_GLTF_INTERNAL",
                             "the SH coefficient index exceeds the canonical model");
        }
        auto idx = attribute_index(doc, prim, khr::sh_attribute(*address));
        if (!idx.has_value()) {
            return Result<PrimitiveRead>::failure(idx.error_code(), idx.diagnostics());
        }
        const AccessorDesc& accessor = doc.accessors[static_cast<std::size_t>(idx.value())];
        if (accessor.element != ElementType::vec3 || accessor.count != n64 ||
            !is_plain_f32(accessor)) {
            return fail("MK2154_GLTF_BAD_ATTRIBUTE_TYPE",
                        "each SH coefficient must be a non-normalized FLOAT VEC3 accessor that "
                        "matches POSITION count");
        }
    }

    auto sh_floats = checked_sh_total_floats(n64, degree);
    auto sh_bytes =
        sh_floats.has_value()
            ? checked_mul(sh_floats.value(), sizeof(float), "glTF SH bytes")
            : Result<std::uint64_t>::failure(sh_floats.error_code(), sh_floats.diagnostics());
    if (!sh_bytes.has_value()) {
        return Result<PrimitiveRead>::failure(sh_bytes.error_code(), sh_bytes.diagnostics());
    }
    auto retained_bytes =
        checked_add(canonical_bytes.value(), sh_bytes.value(), "glTF primitive retained bytes");
    if (!retained_bytes.has_value()) {
        return Result<PrimitiveRead>::failure(retained_bytes.error_code(),
                                              retained_bytes.diagnostics());
    }
    if (auto charged =
            context.consume(BudgetKind::memory_bytes, sh_bytes.value(), "gltf.primitive.sh");
        !charged.has_value()) {
        return Result<PrimitiveRead>::failure(charged.error_code(), charged.diagnostics());
    }
    ScopedBudgetRelease sh_release(context.budget, BudgetKind::memory_bytes, sh_bytes.value());
    auto sh_size = checked_size_cast(sh_floats.value(), "glTF SH float count");
    if (!sh_size.has_value()) {
        return Result<PrimitiveRead>::failure(sh_size.error_code(), sh_size.diagnostics());
    }

    std::vector<float> sh_data(sh_size.value(), 0.0f);
    for (std::size_t k = 0; k < coeffs; ++k) {
        const auto address = khr::sh_flat_to_address(k);
        if (!address.has_value()) {
            return fail_with(ErrorCode::internal_error, "MK2166_GLTF_INTERNAL",
                             "the SH coefficient index exceeds the canonical model");
        }
        auto idx = attribute_index(doc, prim, khr::sh_attribute(*address));
        if (!idx.has_value()) {
            return Result<PrimitiveRead>::failure(idx.error_code(), idx.diagnostics());
        }
        auto coef = resolve_and_decode_accessor(doc, idx.value(), buffers, context);
        if (!coef.has_value()) {
            return Result<PrimitiveRead>::failure(coef.error_code(), coef.diagnostics());
        }
        if (coef.value().size() != n * 3) {
            return fail("MK2155_GLTF_ATTRIBUTE_DECODE",
                        "an SH coefficient decoded to the wrong length");
        }
        for (std::size_t s = 0; s < n; ++s) {
            if (s % kControlInterval == 0) {
                auto control =
                    context.checkpoint({"gltf.read", "spherical_harmonics", s, n, "splats"});
                if (!control.has_value()) {
                    return primitive_control_failure(control);
                }
            }
            for (std::size_t c = 0; c < 3; ++c) {
                sh_data[s * (coeffs * 3) + k * 3 + c] = coef.value()[s * 3 + c];
            }
        }
    }
    auto sh = ShBuffer::create(degree, n, std::move(sh_data), context);
    if (!sh.has_value()) {
        return Result<PrimitiveRead>::failure(sh.error_code(), sh.diagnostics());
    }

    SplatBufferInput input;
    input.positions.reserve(n);
    input.scales.reserve(n);
    input.rotations.reserve(n);
    input.opacities.reserve(n);
    for (std::size_t s = 0; s < n; ++s) {
        if (s % kControlInterval == 0) {
            auto control = context.checkpoint({"gltf.read", "canonicalize", s, n, "splats"});
            if (!control.has_value()) {
                return primitive_control_failure(control);
            }
        }
        input.positions.push_back(Vec3f{positions.value()[s * 3 + 0], positions.value()[s * 3 + 1],
                                        positions.value()[s * 3 + 2]});
        input.scales.push_back(
            Vec3f{scales.value()[s * 3 + 0], scales.value()[s * 3 + 1], scales.value()[s * 3 + 2]});
        const Vec3f& decoded_scale = input.scales.back();
        if (!std::isfinite(decoded_scale.x) || !std::isfinite(decoded_scale.y) ||
            !std::isfinite(decoded_scale.z) || decoded_scale.x < 0.0f || decoded_scale.y < 0.0f ||
            decoded_scale.z < 0.0f) {
            return fail("MK2158_GLTF_SPLAT_INVALID",
                        "SCALE contains a negative or non-finite value");
        }
        // Quantized rotation values can need normalization after decode.
        math::Quat q{rotations.value()[s * 4 + 0], rotations.value()[s * 4 + 1],
                     rotations.value()[s * 4 + 2], rotations.value()[s * 4 + 3]};
        const double norm_squared = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        if (!std::isfinite(norm_squared) || norm_squared <= 1e-20) {
            return fail("MK2158_GLTF_SPLAT_INVALID",
                        "ROTATION contains a zero or non-finite value");
        }
        const double rotation_tolerance = rotation.component == ComponentType::i8 ? 0.02 : 1e-4;
        if (std::fabs(norm_squared - 1.0) > rotation_tolerance) {
            return fail("MK2158_GLTF_SPLAT_INVALID", "ROTATION values must be unit quaternions");
        }
        auto qn = math::normalize(q);
        if (!qn.has_value()) {
            return fail("MK2158_GLTF_SPLAT_INVALID", "ROTATION could not be normalized");
        }
        const math::Quat u = qn.value();
        input.rotations.push_back(Quatf{static_cast<float>(u.x), static_cast<float>(u.y),
                                        static_cast<float>(u.z), static_cast<float>(u.w)});
        const float decoded_opacity = opacities.value()[s];
        if (!std::isfinite(decoded_opacity) || decoded_opacity < 0.0f || decoded_opacity > 1.0f) {
            return fail("MK2158_GLTF_SPLAT_INVALID",
                        "OPACITY contains a non-finite or out-of-range value");
        }
        input.opacities.push_back(decoded_opacity);
    }
    input.sh = std::move(sh.value());

    auto splats = SplatData::create(std::move(input), context);
    if (!splats.has_value()) {
        return Result<PrimitiveRead>::failure(splats.error_code(), splats.diagnostics());
    }

    auto completed = context.checkpoint({"gltf.read", "primitive", n, n, "splats"});
    if (!completed.has_value()) {
        return primitive_control_failure(completed);
    }
    auto retained = context.budget->adopt_charge(BudgetKind::memory_bytes, retained_bytes.value(),
                                                 "gltf.primitive_result");
    if (!retained.has_value()) {
        return Result<PrimitiveRead>::failure(retained.error_code(), retained.diagnostics());
    }
    PrimitiveRead result(std::move(splats.value()), color_space.value(), degree,
                         std::move(retained).value());
    canonical_release.retain();
    sh_release.retain();
    return Result<PrimitiveRead>::success(std::move(result));
} catch (const std::bad_alloc&) {
    return fail_with(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                     "memory allocation failed while reading a glTF primitive");
} catch (const std::length_error&) {
    return fail_with(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                     "a glTF primitive allocation exceeds the host size limit");
}

namespace {

// Return true for an exact, positive, axis-aligned scale. This transform does not rotate spherical
// harmonics.
bool is_pure_positive_scale(const math::Mat3& l) {
    return l[1] == 0.0 && l[2] == 0.0 && l[3] == 0.0 && l[5] == 0.0 && l[6] == 0.0 && l[7] == 0.0 &&
           l[0] > 0.0 && l[4] > 0.0 && l[8] > 0.0;
}

// Return true for the exact identity map. This path preserves the original scale and rotation.
bool is_identity_linear(const math::Mat3& l) {
    return l[0] == 1.0 && l[4] == 1.0 && l[8] == 1.0 && l[1] == 0.0 && l[2] == 0.0 && l[3] == 0.0 &&
           l[5] == 0.0 && l[6] == 0.0 && l[7] == 0.0;
}

bool representable_float(double value) noexcept {
    constexpr double limit = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -limit || value > limit)
        return false;
    return value == 0.0 || static_cast<float>(value) != 0.0f;
}

// The KHR profile defines splat rendering only for a global transform that decomposes into a
// proper rotation and finite positive scale. Unit column vectors must therefore have determinant
// near positive one. Hadamard's inequality also makes this test reject material shear.
bool has_defined_khr_transform(const math::Mat3& linear) {
    math::Mat3 normalized{};
    for (std::size_t column = 0; column < 3; ++column) {
        const double x = linear[column];
        const double y = linear[3 + column];
        const double z = linear[6 + column];
        const double length = std::hypot(x, y, z);
        if (!std::isfinite(length) || length == 0.0)
            return false;
        normalized[column] = x / length;
        normalized[3 + column] = y / length;
        normalized[6 + column] = z / length;
    }
    const double determinant =
        normalized[0] * (normalized[4] * normalized[8] - normalized[5] * normalized[7]) -
        normalized[1] * (normalized[3] * normalized[8] - normalized[5] * normalized[6]) +
        normalized[2] * (normalized[3] * normalized[7] - normalized[4] * normalized[6]);
    return std::isfinite(determinant) && std::fabs(determinant - 1.0) <= 1e-4;
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
};

// Apply the node transform to one primitive. Rotate SH when the transform has a rotation.
Result<TransformedBatch> transform_primitive(const PrimitiveRead& pr, const NodeTransform& xform,
                                             const math::ShRotation* sh_rot,
                                             const OperationContext& context) {
    TransformedBatch batch;
    batch.degree = pr.source_sh_degree;
    const std::size_t coeffs = khr::sh_total_coefficients(pr.source_sh_degree);
    const std::size_t block = coeffs * 3;
    const auto& raw = pr.data.sh().raw();
    const bool identity = is_identity_linear(xform.linear);
    batch.positions.reserve(pr.data.size());
    batch.scales.reserve(pr.data.size());
    batch.rotations.reserve(pr.data.size());
    batch.opacities.reserve(pr.data.size());
    batch.sh.reserve(raw.size());
    for (std::size_t s = 0; s < pr.data.size(); ++s) {
        if (s % kControlInterval == 0) {
            auto control =
                context.checkpoint({"gltf.read", "transform", s, pr.data.size(), "splats"});
            if (!control.has_value()) {
                return Result<TransformedBatch>::failure(control.error_code(),
                                                         control.diagnostics());
            }
        }
        const Vec3f& p = pr.data.positions()[s];
        const Vec3f& sc = pr.data.scales()[s];
        const Quatf& q = pr.data.rotations()[s];

        Vec3f out_scale = sc;
        Quatf out_rot = q;
        if (!identity) {
            auto rs = math::affine_transform_gaussian(xform.linear, math::Quat{q.x, q.y, q.z, q.w},
                                                      math::Vec3{sc.x, sc.y, sc.z});
            if (!rs.has_value()) {
                return Result<TransformedBatch>::failure(rs.error_code(), rs.diagnostics());
            }
            if (!representable_float(rs.value().scale[0]) ||
                !representable_float(rs.value().scale[1]) ||
                !representable_float(rs.value().scale[2])) {
                Diagnostic diagnostic("MK2176_GLTF_TRANSFORM_OVERFLOW", Severity::error,
                                      "a transformed Gaussian scale exceeds the canonical range");
                return Result<TransformedBatch>::failure(ErrorCode::unsupported_feature,
                                                         std::move(diagnostic));
            }
            const std::size_t source_zero_scales = static_cast<std::size_t>(sc.x == 0.0f) +
                                                   static_cast<std::size_t>(sc.y == 0.0f) +
                                                   static_cast<std::size_t>(sc.z == 0.0f);
            const std::size_t output_zero_scales =
                static_cast<std::size_t>(rs.value().scale[0] == 0.0) +
                static_cast<std::size_t>(rs.value().scale[1] == 0.0) +
                static_cast<std::size_t>(rs.value().scale[2] == 0.0);
            if (source_zero_scales != output_zero_scales) {
                Diagnostic diagnostic(
                    "MK2176_GLTF_TRANSFORM_OVERFLOW", Severity::error,
                    "a Gaussian node transform changed the rank of the canonical covariance");
                return Result<TransformedBatch>::failure(ErrorCode::unsupported_feature,
                                                         std::move(diagnostic));
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
        if (!representable_float(mean[0]) || !representable_float(mean[1]) ||
            !representable_float(mean[2])) {
            Diagnostic diagnostic("MK2176_GLTF_TRANSFORM_OVERFLOW", Severity::error,
                                  "a transformed Gaussian position exceeds the canonical range");
            return Result<TransformedBatch>::failure(ErrorCode::unsupported_feature,
                                                     std::move(diagnostic));
        }
        batch.positions.push_back(Vec3f{static_cast<float>(mean[0]), static_cast<float>(mean[1]),
                                        static_cast<float>(mean[2])});
        batch.scales.push_back(out_scale);
        batch.rotations.push_back(out_rot);
        batch.opacities.push_back(pr.data.opacities()[s]);
        const std::size_t output_offset = batch.sh.size();
        batch.sh.insert(batch.sh.end(), raw.begin() + static_cast<std::ptrdiff_t>(s * block),
                        raw.begin() + static_cast<std::ptrdiff_t>((s + 1) * block));
        if (sh_rot != nullptr) {
            auto rotated = sh_rot->rotate_block(batch.sh.data() + output_offset, 3);
            if (!rotated.has_value()) {
                return Result<TransformedBatch>::failure(rotated.error_code(),
                                                         rotated.diagnostics());
            }
        }
    }
    return Result<TransformedBatch>::success(std::move(batch));
}

Result<SceneRead> scene_fail(ErrorCode code, const char* diag_code, std::string message) {
    Diagnostic d(diag_code, Severity::error, std::move(message));
    return Result<SceneRead>::failure(code, std::move(d));
}

Result<SceneRead> scene_control_failure(const Result<void>& control) {
    return Result<SceneRead>::failure(control.error_code(), control.diagnostics());
}

Result<SceneRead> read_gaussian_scene_impl(const Document& doc,
                                           const std::vector<BufferSpan>& buffers,
                                           const Limits& limits, const OperationContext& context) {
    auto started = context.checkpoint({"gltf.read", "scene", 0, doc.nodes.size(), "nodes"});
    if (!started.has_value()) {
        return scene_control_failure(started);
    }
    if (doc.scene_count != 0 && !doc.has_default_scene) {
        return scene_fail(ErrorCode::unsupported_feature, "MK2169_GLTF_DEFAULT_SCENE_REQUIRED",
                          "the asset has scenes but does not select a default scene");
    }
    auto structural_count = checked_add(doc.accessors.size(), doc.buffer_views.size(),
                                        "glTF accessor and buffer-view count");
    if (!structural_count.has_value()) {
        return Result<SceneRead>::failure(structural_count.error_code(),
                                          structural_count.diagnostics());
    }
    if (auto charged = context.observe(BudgetKind::accessors, structural_count.value(),
                                       "gltf.structural_objects");
        !charged.has_value()) {
        return Result<SceneRead>::failure(charged.error_code(), charged.diagnostics());
    }
    if (auto charged =
            context.observe(BudgetKind::gltf_nodes, doc.nodes.size(), "gltf.document.nodes");
        !charged.has_value()) {
        return Result<SceneRead>::failure(charged.error_code(), charged.diagnostics());
    }

    // Reject an unsupported required extension.
    auto ext = evaluate_extensions(doc.extensions_used, doc.extensions_required);
    if (!ext.unsupported_required.empty()) {
        std::string names;
        for (const auto& n : ext.unsupported_required) {
            if (!names.empty())
                names += ", ";
            names += n;
        }
        return scene_fail(ErrorCode::unsupported_feature, "MK2160_GLTF_UNSUPPORTED_REQUIRED",
                          "the asset requires glTF extensions Melkor does not implement: " + names);
    }

    LossReport losses;
    if (doc.source_features.provenance_count != 0) {
        LossItem item;
        item.code = loss_code::kProvenanceDropped;
        item.severity = LossSeverity::info;
        item.source_feature =
            feature_description("glTF provenance properties", doc.source_features.provenance_count,
                                doc.source_features.provenance_samples);
        item.target_constraint = "the canonical scene does not retain source tool metadata";
        item.remediation = "keep the source asset if exact tool provenance is required";
        MELKOR_TRY_AS(losses.add(std::move(item)), SceneRead);
    }
    if (doc.source_features.attribution_count != 0) {
        LossItem item;
        item.code = loss_code::kAttributionDropped;
        item.severity = LossSeverity::severe;
        item.source_feature = feature_description("glTF attribution properties",
                                                  doc.source_features.attribution_count,
                                                  doc.source_features.attribution_samples);
        item.target_constraint = "the canonical scene does not retain source attribution";
        item.remediation = "remove the attribution or approve LOSS_ATTRIBUTION_DROPPED";
        MELKOR_TRY_AS(losses.add(std::move(item)), SceneRead);
    }
    if (doc.source_features.property_count != 0) {
        LossItem item;
        item.code = loss_code::kUnknownPropertyDropped;
        item.severity = LossSeverity::severe;
        item.source_feature = feature_description(
            "glTF properties outside the canonical scene model", doc.source_features.property_count,
            doc.source_features.property_samples);
        item.target_constraint = "the canonical scene does not retain these glTF properties";
        item.remediation = "remove the properties or approve LOSS_UNKNOWN_PROPERTY_DROPPED";
        MELKOR_TRY_AS(losses.add(std::move(item)), SceneRead);
    }
    if (doc.source_features.extension_count != 0) {
        LossItem item;
        item.code = loss_code::kExtensionDropped;
        item.severity = LossSeverity::severe;
        item.source_feature = feature_description("unsupported glTF extension occurrences",
                                                  doc.source_features.extension_count,
                                                  doc.source_features.extension_samples);
        item.target_constraint = "the canonical scene does not retain these extensions";
        item.remediation = "remove the extensions or approve LOSS_EXTENSION_DROPPED";
        MELKOR_TRY_AS(losses.add(std::move(item)), SceneRead);
    }
    auto usage_result = collect_default_scene_usage(doc, context);
    if (!usage_result.has_value()) {
        return Result<SceneRead>::failure(usage_result.error_code(), usage_result.diagnostics());
    }
    DefaultSceneUsage usage = std::move(usage_result).value();

    struct Instance {
        const PrimitiveDesc* prim;
        NodeTransform transform;
    };
    struct Frame {
        std::uint64_t node;
        NodeTransform parent;
        std::uint64_t depth;
    };

    std::uint64_t instance_count = 0;
    for (std::size_t node_index = 0; node_index < doc.nodes.size(); ++node_index) {
        if (usage.nodes[node_index] == 0 || !doc.nodes[node_index].mesh.has_value())
            continue;
        const std::size_t mesh_index = static_cast<std::size_t>(*doc.nodes[node_index].mesh);
        for (const PrimitiveDesc& primitive : doc.meshes[mesh_index].primitives) {
            if (!primitive.gaussian.has_value())
                continue;
            auto next = checked_add(instance_count, 1, "glTF Gaussian instance count");
            if (!next.has_value())
                return Result<SceneRead>::failure(next.error_code(), next.diagnostics());
            instance_count = next.value();
        }
    }
    auto instance_bytes =
        checked_mul(instance_count, sizeof(Instance), "glTF Gaussian instance memory");
    auto instance_size = checked_size_cast(instance_count, "glTF Gaussian instance count");
    if (!instance_bytes.has_value() || !instance_size.has_value()) {
        return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                          "the glTF Gaussian instance table is too large");
    }

    std::uint64_t edge_count = 0;
    for (const NodeDesc& node : doc.nodes) {
        auto next = checked_add(edge_count, node.children.size(), "glTF node edge count");
        if (!next.has_value()) {
            return Result<SceneRead>::failure(next.error_code(), next.diagnostics());
        }
        edge_count = next.value();
    }
    if (edge_count > limits.max_gltf_nodes) {
        return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                          "the glTF node graph has too many child references");
    }

    auto node_state_bytes = checked_mul(doc.nodes.size(), sizeof(char), "glTF node state memory");
    auto mesh_state_bytes =
        checked_mul(doc.meshes.size(), sizeof(std::uint64_t), "glTF mesh instance memory");
    auto stack_entries =
        checked_add(edge_count, doc.scene_roots.size(), "glTF traversal stack entries");
    auto stack_bytes =
        stack_entries.has_value()
            ? checked_mul(stack_entries.value(), sizeof(Frame), "glTF traversal stack memory")
            : Result<std::uint64_t>::failure(stack_entries.error_code(),
                                             stack_entries.diagnostics());
    auto traversal_state_bytes =
        node_state_bytes.has_value() && mesh_state_bytes.has_value()
            ? checked_add(node_state_bytes.value(), mesh_state_bytes.value(),
                          "glTF traversal state memory")
            : Result<std::uint64_t>::failure(
                  node_state_bytes.has_value() ? mesh_state_bytes.error_code()
                                               : node_state_bytes.error_code(),
                  node_state_bytes.has_value() ? mesh_state_bytes.diagnostics()
                                               : node_state_bytes.diagnostics());
    auto traversal_bytes =
        traversal_state_bytes.has_value() && stack_bytes.has_value()
            ? checked_add(traversal_state_bytes.value(), stack_bytes.value(),
                          "glTF traversal memory")
            : Result<std::uint64_t>::failure(
                  traversal_state_bytes.has_value() ? stack_bytes.error_code()
                                                    : traversal_state_bytes.error_code(),
                  traversal_state_bytes.has_value() ? stack_bytes.diagnostics()
                                                    : traversal_state_bytes.diagnostics());
    if (!traversal_bytes.has_value()) {
        return Result<SceneRead>::failure(traversal_bytes.error_code(),
                                          traversal_bytes.diagnostics());
    }
    if (auto charged = context.consume(BudgetKind::memory_bytes, traversal_bytes.value(),
                                       "gltf.scene_traversal");
        !charged.has_value()) {
        return Result<SceneRead>::failure(charged.error_code(), charged.diagnostics());
    }
    ScopedBudgetRelease traversal_release(context.budget, BudgetKind::memory_bytes,
                                          traversal_bytes.value());

    if (auto charged = context.consume(BudgetKind::memory_bytes, instance_bytes.value(),
                                       "gltf.scene_instances");
        !charged.has_value()) {
        return Result<SceneRead>::failure(charged.error_code(), charged.diagnostics());
    }
    ScopedBudgetRelease instance_record_release(context.budget, BudgetKind::memory_bytes,
                                                instance_bytes.value());
    std::vector<Instance> instances;
    instances.reserve(instance_size.value());
    std::vector<char> visited(doc.nodes.size(), 0);
    std::vector<std::uint64_t> mesh_instance_counts(doc.meshes.size(), 0);
    std::vector<Frame> stack;
    stack.reserve(static_cast<std::size_t>(usage.node_count));
    for (std::uint64_t root : doc.scene_roots) {
        stack.push_back(Frame{root, identity_transform(), 1});
    }
    std::uint64_t declared_splats = 0;
    std::size_t visited_nodes = 0;
    bool expanded_instance = false;
    while (!stack.empty()) {
        if (visited_nodes % kControlInterval == 0) {
            auto control = context.checkpoint(
                {"gltf.read", "traverse", visited_nodes, doc.nodes.size(), "nodes"});
            if (!control.has_value()) {
                return scene_control_failure(control);
            }
        }
        Frame f = stack.back();
        stack.pop_back();
        if (f.node >= doc.nodes.size()) {
            return scene_fail(ErrorCode::invalid_data, "MK2168_GLTF_NODE_GRAPH",
                              "the scene references a node outside the node array");
        }
        if (visited[static_cast<std::size_t>(f.node)]) {
            return scene_fail(
                ErrorCode::invalid_data, "MK2168_GLTF_NODE_GRAPH",
                "the glTF node graph contains a cycle or a node with multiple parents");
        }
        if (f.depth > limits.max_scene_depth) {
            return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                              "the glTF node graph exceeds the scene-depth limit");
        }
        visited[static_cast<std::size_t>(f.node)] = 1;
        ++visited_nodes;
        const NodeDesc& node = doc.nodes[static_cast<std::size_t>(f.node)];
        auto local = local_node_transform(node);
        if (!local.has_value()) {
            return Result<SceneRead>::failure(local.error_code(), local.diagnostics());
        }
        const NodeTransform global = compose(f.parent, local.value());
        if (node.mesh.has_value()) {
            const std::size_t mesh_index = static_cast<std::size_t>(node.mesh.value());
            const MeshDesc& mesh = doc.meshes[mesh_index];
            const bool has_gaussian = std::any_of(
                mesh.primitives.begin(), mesh.primitives.end(),
                [](const PrimitiveDesc& primitive) { return primitive.gaussian.has_value(); });
            if (has_gaussian) {
                ++mesh_instance_counts[mesh_index];
                expanded_instance = expanded_instance || mesh_instance_counts[mesh_index] > 1;
            }
            if (has_gaussian && !has_defined_khr_transform(global.linear)) {
                return scene_fail(
                    ErrorCode::unsupported_feature, "MK2175_GLTF_UNDEFINED_TRANSFORM",
                    "a Gaussian node transform does not have a proper rotation and positive scale");
            }
            if (has_gaussian &&
                (!std::isfinite(global.translation[0]) || !std::isfinite(global.translation[1]) ||
                 !std::isfinite(global.translation[2]))) {
                return scene_fail(ErrorCode::invalid_data, "MK2176_GLTF_TRANSFORM_OVERFLOW",
                                  "a Gaussian node translation overflows during composition");
            }
            for (const auto& prim : mesh.primitives) {
                if (prim.gaussian.has_value()) {
                    std::uint64_t splats = 1;
                    auto position = prim.attributes.find(khr::kAttrPosition);
                    if (position != prim.attributes.end() &&
                        position->second < doc.accessors.size()) {
                        splats = std::max<std::uint64_t>(
                            1, doc.accessors[static_cast<std::size_t>(position->second)].count);
                    }
                    auto next =
                        checked_add(declared_splats, splats, "instantiated glTF splat count");
                    if (!next.has_value() || next.value() > limits.max_splats) {
                        return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                                          "the instantiated glTF scene exceeds the splat limit");
                    }
                    declared_splats = next.value();
                    instances.push_back(Instance{&prim, global});
                }
            }
        }
        for (std::uint64_t child : node.children) {
            stack.push_back(Frame{child, global, f.depth + 1});
        }
    }

    if (instances.empty()) {
        return scene_fail(
            ErrorCode::invalid_data, "MK2161_GLTF_NO_SPLATS",
            "no KHR_gaussian_splatting primitive is reachable from the default scene");
    }

    // The count includes every primitive instance. Observe it before primitive decoding allocates
    // data. The memory budget accounts for the temporary and retained copies separately.
    if (auto charged =
            context.observe(BudgetKind::splats, declared_splats, "gltf.instantiated_splats");
        !charged.has_value()) {
        return Result<SceneRead>::failure(charged.error_code(), charged.diagnostics());
    }

    const std::uint64_t omitted_nodes = doc.nodes.size() - usage.node_count;
    const std::uint64_t omitted_meshes = doc.meshes.size() - usage.mesh_count;
    const std::uint64_t omitted_accessors = doc.accessors.size() - usage.accessor_count;
    const std::uint64_t omitted_views = doc.buffer_views.size() - usage.buffer_view_count;
    const std::uint64_t omitted_buffers = doc.buffers.size() - usage.buffer_count;
    const std::uint64_t extra_scenes = doc.scene_count > 0 ? doc.scene_count - 1 : 0;
    if (doc.source_features.non_gaussian_primitive_count != 0 || extra_scenes != 0 ||
        doc.source_features.unknown_container_chunk_count != 0 ||
        doc.source_features.unused_container_bin_count != 0 || omitted_nodes != 0 ||
        omitted_meshes != 0 || omitted_accessors != 0 || omitted_views != 0 ||
        omitted_buffers != 0) {
        LossItem item;
        item.code = loss_code::kGltfContentDropped;
        item.severity = LossSeverity::severe;
        item.source_feature =
            std::to_string(doc.source_features.non_gaussian_primitive_count) +
            " non-Gaussian primitives, " + std::to_string(extra_scenes) + " non-default scenes, " +
            std::to_string(doc.source_features.unknown_container_chunk_count) +
            " unknown GLB chunks, " +
            std::to_string(doc.source_features.unused_container_bin_count) +
            " unused GLB BIN chunks, " + std::to_string(omitted_nodes) + " unused nodes, " +
            std::to_string(omitted_meshes) + " unused meshes, " +
            std::to_string(omitted_accessors) + " unused accessors, " +
            std::to_string(omitted_views) + " unused bufferViews, and " +
            std::to_string(omitted_buffers) + " unused buffers";
        item.target_constraint = "the canonical scene stores only used default-scene Gaussian data";
        item.remediation = "remove the extra content or approve LOSS_GLTF_CONTENT_DROPPED";
        MELKOR_TRY_AS(losses.add(std::move(item)), SceneRead);
    }

    // 3. Read and transform every primitive.
    ScopedBudgetRelease batch_memory_release(context.budget, BudgetKind::memory_bytes, 0);
    auto batch_record_bytes =
        checked_mul(instances.size(), sizeof(TransformedBatch), "glTF batch record memory");
    if (!batch_record_bytes.has_value()) {
        return Result<SceneRead>::failure(batch_record_bytes.error_code(),
                                          batch_record_bytes.diagnostics());
    }
    if (auto charged = context.consume(BudgetKind::memory_bytes, batch_record_bytes.value(),
                                       "gltf.transformed_batches");
        !charged.has_value()) {
        return Result<SceneRead>::failure(charged.error_code(), charged.diagnostics());
    }
    ScopedBudgetRelease batch_record_release(context.budget, BudgetKind::memory_bytes,
                                             batch_record_bytes.value());
    std::vector<TransformedBatch> batches;
    batches.reserve(instances.size());
    khr::ColorSpace first_cs = khr::ColorSpace::srgb_rec709_display;
    bool have_cs = false;
    for (std::size_t instance_index = 0; instance_index < instances.size(); ++instance_index) {
        auto control = context.checkpoint(
            {"gltf.read", "instances", instance_index, instances.size(), "instances"});
        if (!control.has_value()) {
            return scene_control_failure(control);
        }
        const auto& inst = instances[instance_index];
        auto pr = read_primitive_local(doc, *inst.prim, buffers, context);
        if (!pr.has_value()) {
            return Result<SceneRead>::failure(pr.error_code(), pr.diagnostics());
        }
        if (!have_cs) {
            first_cs = pr.value().color_space;
            have_cs = true;
        } else if (pr.value().color_space != first_cs) {
            return scene_fail(ErrorCode::unsupported_feature, "MK2179_GLTF_COLOR_SPACE_CONFLICT",
                              "Gaussian primitives use different color spaces");
        }

        // Use the rotation directly, or extract it from rotation and positive scale.
        std::optional<math::ShRotation> sh_rot;
        const math::ShRotation* sh_rot_ptr = nullptr;
        if (pr.value().source_sh_degree >= 1 && !is_identity_linear(inst.transform.linear) &&
            !is_pure_positive_scale(inst.transform.linear)) {
            const math::Mat3& linear = inst.transform.linear;
            // Use the linear part directly when it is already a proper rotation; otherwise recover
            // the rotation component from a scaled transform.
            Result<math::Mat3> rot_matrix = math::is_proper_rotation(linear)
                                                ? Result<math::Mat3>::success(linear)
                                                : math::rotation_from_linear(linear);
            if (!rot_matrix.has_value()) {
                return scene_fail(ErrorCode::unsupported_feature, "MK2175_GLTF_UNDEFINED_TRANSFORM",
                                  "a Gaussian node transform has no stable rotation component");
            }
            auto rotation =
                math::ShRotation::create(rot_matrix.value(), pr.value().source_sh_degree);
            if (!rotation.has_value()) {
                return Result<SceneRead>::failure(rotation.error_code(), rotation.diagnostics());
            }
            sh_rot = std::move(rotation.value());
            sh_rot_ptr = &sh_rot.value();
        }

        const std::uint64_t transformed_bytes = pr.value().retained_memory_bytes();
        if (auto charged = context.consume(BudgetKind::memory_bytes, transformed_bytes,
                                           "gltf.transformed_batch");
            !charged.has_value()) {
            return Result<SceneRead>::failure(charged.error_code(), charged.diagnostics());
        }
        ScopedBudgetRelease transformed_memory_release(context.budget, BudgetKind::memory_bytes,
                                                       transformed_bytes);

        auto transformed = transform_primitive(pr.value(), inst.transform, sh_rot_ptr, context);
        if (!transformed.has_value()) {
            return Result<SceneRead>::failure(transformed.error_code(), transformed.diagnostics());
        }
        auto batch = std::move(transformed.value());
        if (!batch.positions.empty()) {
            batches.push_back(std::move(batch));
            batch_memory_release.add(transformed_bytes);
            transformed_memory_release.retain();
        }
    }

    if (batches.empty()) {
        return scene_fail(ErrorCode::internal_error, "MK2177_GLTF_READ_INVARIANT",
                          "the glTF scene produced no transformed Gaussian batch");
    }

    std::uint32_t max_degree = 0;
    std::uint64_t total_u64 = 0;
    for (const auto& b : batches) {
        max_degree = std::max(max_degree, b.degree);
        auto next = checked_add(total_u64, b.positions.size(), "merged glTF splat count");
        if (!next.has_value()) {
            return Result<SceneRead>::failure(next.error_code(), next.diagnostics());
        }
        total_u64 = next.value();
    }
    auto total_checked = checked_size_cast(total_u64, "merged glTF splat count");
    if (!total_checked.has_value()) {
        return Result<SceneRead>::failure(total_checked.error_code(), total_checked.diagnostics());
    }
    const std::size_t total = total_checked.value();
    const std::size_t merged_coeffs = khr::sh_total_coefficients(max_degree);
    const std::size_t merged_block = merged_coeffs * 3;

    auto merged_fixed =
        checked_mul(total_u64, kCanonicalBytesPerSplat, "merged glTF geometry bytes");
    auto merged_sh_floats = checked_sh_total_floats(total_u64, max_degree);
    auto merged_sh_bytes =
        merged_sh_floats.has_value()
            ? checked_mul(merged_sh_floats.value(), sizeof(float), "merged glTF SH bytes")
            : Result<std::uint64_t>::failure(merged_sh_floats.error_code(),
                                             merged_sh_floats.diagnostics());
    auto merged_bytes =
        merged_fixed.has_value() && merged_sh_bytes.has_value()
            ? checked_add(merged_fixed.value(), merged_sh_bytes.value(), "merged glTF bytes")
            : Result<std::uint64_t>::failure(merged_fixed.has_value() ? merged_sh_bytes.error_code()
                                                                      : merged_fixed.error_code(),
                                             merged_fixed.has_value()
                                                 ? merged_sh_bytes.diagnostics()
                                                 : merged_fixed.diagnostics());
    if (!merged_bytes.has_value()) {
        return Result<SceneRead>::failure(merged_bytes.error_code(), merged_bytes.diagnostics());
    }
    if (auto charged =
            context.consume(BudgetKind::memory_bytes, merged_bytes.value(), "gltf.merged_scene");
        !charged.has_value()) {
        return Result<SceneRead>::failure(charged.error_code(), charged.diagnostics());
    }
    ScopedBudgetRelease merged_memory_release(context.budget, BudgetKind::memory_bytes,
                                              merged_bytes.value());
    auto merged_sh_size = checked_size_cast(merged_sh_floats.value(), "merged glTF SH float count");
    if (!merged_sh_size.has_value()) {
        return Result<SceneRead>::failure(merged_sh_size.error_code(),
                                          merged_sh_size.diagnostics());
    }

    SplatBufferInput input;
    input.positions.reserve(total);
    input.scales.reserve(total);
    input.rotations.reserve(total);
    input.opacities.reserve(total);
    std::vector<float> merged_sh(merged_sh_size.value(), 0.0f);

    std::size_t write = 0;
    for (const auto& b : batches) {
        const std::size_t src_block = khr::sh_total_coefficients(b.degree) * 3;
        for (std::size_t s = 0; s < b.positions.size(); ++s) {
            if (write % kControlInterval == 0) {
                auto merge_control =
                    context.checkpoint({"gltf.read", "merge", write, total, "splats"});
                if (!merge_control.has_value()) {
                    return scene_control_failure(merge_control);
                }
            }
            input.positions.push_back(b.positions[s]);
            input.scales.push_back(b.scales[s]);
            input.rotations.push_back(b.rotations[s]);
            input.opacities.push_back(b.opacities[s]);
            // Copy the splat's coefficients into the front of its max-degree block; the higher
            // coefficients stay zero-initialized.
            for (std::size_t i = 0; i < src_block; ++i) {
                merged_sh[write * merged_block + i] = b.sh[s * src_block + i];
            }
            ++write;
        }
    }

    auto sh = ShBuffer::create(max_degree, total, std::move(merged_sh), context);
    if (!sh.has_value()) {
        return Result<SceneRead>::failure(sh.error_code(), sh.diagnostics());
    }
    input.sh = std::move(sh.value());

    auto merged = SplatData::create(std::move(input), context);
    if (!merged.has_value()) {
        return Result<SceneRead>::failure(merged.error_code(), merged.diagnostics());
    }

    bool trivial_scene = visited_nodes == 1 && doc.scene_roots.size() == 1 &&
                         doc.nodes[static_cast<std::size_t>(doc.scene_roots[0])].children.empty() &&
                         instances.size() == 1 && is_identity_linear(instances[0].transform.linear);
    if (trivial_scene) {
        const math::Vec3& translation = instances[0].transform.translation;
        trivial_scene = std::fabs(translation[0]) < 1e-12 && std::fabs(translation[1]) < 1e-12 &&
                        std::fabs(translation[2]) < 1e-12;
    }
    if (!trivial_scene) {
        LossItem item;
        item.code = loss_code::kSceneGraphFlattened;
        item.severity = LossSeverity::info;
        item.source_feature =
            std::to_string(visited_nodes) + " used scene nodes with transforms or hierarchy";
        item.target_constraint = "the canonical model is a single flat splat cloud";
        item.affected_splats = total;
        item.remediation = "none needed because the reader applies each node transform";
        MELKOR_TRY_AS(losses.add(std::move(item)), SceneRead);
    }

    if (expanded_instance) {
        LossItem item;
        item.code = loss_code::kInstanceExpanded;
        item.severity = LossSeverity::info;
        item.source_feature = "one or more meshes instantiated by multiple nodes";
        item.target_constraint = "the canonical model stores expanded splats";
        item.affected_splats = total;
        item.remediation = "none needed because each instance keeps its transformed geometry";
        MELKOR_TRY_AS(losses.add(std::move(item)), SceneRead);
    }

    if (!ext.ignored_used.empty()) {
        std::string names;
        for (const std::string& name : ext.ignored_used) {
            if (!names.empty())
                names += ", ";
            names += name;
        }
        LossItem item;
        item.code = loss_code::kExtensionDeclarationDropped;
        item.severity = LossSeverity::info;
        item.source_feature = "optional extension declarations: " + names;
        item.target_constraint = "the writer emits only extension declarations that it uses";
        item.affected_splats = 0;
        item.remediation = "none needed because optional declarations do not change this output";
        MELKOR_TRY_AS(losses.add(std::move(item)), SceneRead);
    }

    auto completed = context.checkpoint({"gltf.read", "complete", total, total, "splats"});
    if (!completed.has_value()) {
        return scene_control_failure(completed);
    }
    auto retained = context.budget->adopt_charge(BudgetKind::memory_bytes, merged_bytes.value(),
                                                 "gltf.scene_result");
    if (!retained.has_value()) {
        return Result<SceneRead>::failure(retained.error_code(), retained.diagnostics());
    }
    merged_memory_release.retain();
    SceneRead out(std::move(merged.value()), std::move(losses), first_cs, max_degree,
                  std::move(retained).value());
    return Result<SceneRead>::success(std::move(out));
}

}  // namespace

Result<SceneRead> read_gaussian_scene(const Document& doc, const std::vector<BufferSpan>& buffers,
                                      const Limits& limits) try {
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    return read_gaussian_scene(doc, buffers, context);
} catch (const std::bad_alloc&) {
    return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                      "memory allocation failed while reading the glTF scene");
} catch (const std::length_error&) {
    return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                      "a glTF scene allocation exceeds the host size limit");
}

Result<SceneRead> read_gaussian_scene(const Document& doc, const std::vector<BufferSpan>& buffers,
                                      const OperationContext& context) try {
    if (context.budget == nullptr) {
        return scene_fail(ErrorCode::internal_error, "MK0310_NO_BUDGET",
                          "the glTF reader requires a resource budget");
    }
    const Limits& limits = context.budget->limits();
    if (auto valid = limits.validate(); !valid.has_value()) {
        return Result<SceneRead>::failure(valid.error_code(), valid.diagnostics());
    }
    return read_gaussian_scene_impl(doc, buffers, limits, context);
} catch (const std::bad_alloc&) {
    return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                      "memory allocation failed while reading the glTF scene");
} catch (const std::length_error&) {
    return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                      "a glTF scene allocation exceeds the host size limit");
}

Result<SceneRead> read_glb(const std::uint8_t* data, std::size_t size, const Limits& limits) try {
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    return read_glb(data, size, context);
} catch (const std::bad_alloc&) {
    return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                      "memory allocation failed while reading the GLB");
} catch (const std::length_error&) {
    return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                      "a GLB allocation exceeds the host size limit");
}

Result<SceneRead> read_glb(const std::uint8_t* data, std::size_t size,
                           const OperationContext& context) try {
    if (context.budget == nullptr) {
        return scene_fail(ErrorCode::internal_error, "MK0310_NO_BUDGET",
                          "the glTF reader requires a resource budget");
    }
    auto started = context.checkpoint({"gltf.read", "container", 0, size, "bytes"});
    if (!started.has_value()) {
        return scene_control_failure(started);
    }
    const Limits& limits = context.budget->limits();
    if (auto valid = limits.validate(); !valid.has_value()) {
        return Result<SceneRead>::failure(valid.error_code(), valid.diagnostics());
    }
    if (auto charged = context.consume(BudgetKind::input_bytes, size, "gltf.input");
        !charged.has_value()) {
        return Result<SceneRead>::failure(charged.error_code(), charged.diagnostics());
    }
    if (auto charged = context.consume(BudgetKind::memory_bytes, size, "gltf.source_buffer");
        !charged.has_value()) {
        return Result<SceneRead>::failure(charged.error_code(), charged.diagnostics());
    }
    ScopedBudgetRelease source_release(context.budget, BudgetKind::memory_bytes, size);
    auto framing = glb::parse_glb(data, size);
    if (!framing.has_value()) {
        return Result<SceneRead>::failure(framing.error_code(), framing.diagnostics());
    }
    const ByteRange& json = framing.value().json;
    auto json_size = checked_size_cast(json.length(), "glTF JSON length");
    if (!json_size.has_value()) {
        return Result<SceneRead>::failure(json_size.error_code(), json_size.diagnostics());
    }
    auto doc = parse_gltf_json(data + json.offset(), json_size.value(), context);
    if (!doc.has_value()) {
        return Result<SceneRead>::failure(doc.error_code(), doc.diagnostics());
    }
    doc.value().source_features.unknown_container_chunk_count = framing.value().unknown_chunk_count;
    auto parsed =
        context.checkpoint({"gltf.read", "document", json.length(), json.length(), "bytes"});
    if (!parsed.has_value()) {
        return scene_control_failure(parsed);
    }

    if (doc.value().buffers.size() != 1) {
        return scene_fail(ErrorCode::unsupported_feature, "MK2170_GLB_BUFFER_PROFILE",
                          "the canonical GLB profile requires one embedded buffer");
    }

    // The single embedded binary buffer is glTF buffer 0.
    std::vector<BufferSpan> buffers;
    if (framing.value().bin.has_value()) {
        // The branch proves that the optional value exists.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        const ByteRange& bin = framing.value().bin.value();
        const BufferDesc& buffer = doc.value().buffers[0];
        if (buffer.uri.has_value()) {
            return scene_fail(ErrorCode::invalid_data, "MK2171_GLB_BUFFER_URI",
                              "GLB buffer 0 cannot define a URI when the BIN chunk supplies it");
        }
        if (buffer.byte_length > bin.length()) {
            return scene_fail(ErrorCode::invalid_data, "MK2172_GLB_BUFFER_LENGTH",
                              "GLB buffer 0 is longer than the BIN chunk");
        }
        if (bin.length() - buffer.byte_length > 3) {
            return scene_fail(ErrorCode::invalid_data, "MK2172_GLB_BUFFER_LENGTH",
                              "the BIN chunk has more than three padding bytes");
        }
        auto bin_offset = checked_size_cast(bin.offset(), "GLB BIN offset");
        auto buffer_size = checked_size_cast(buffer.byte_length, "GLB buffer length");
        if (!bin_offset.has_value() || !buffer_size.has_value()) {
            return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                              "the GLB buffer size is not representable on this host");
        }
        for (std::uint64_t i = buffer.byte_length; i < bin.length(); ++i) {
            if (data[bin_offset.value() + static_cast<std::size_t>(i)] != 0) {
                return scene_fail(ErrorCode::invalid_data, "MK2173_GLB_BIN_PADDING",
                                  "the BIN chunk padding contains a nonzero byte");
            }
        }
        buffers.push_back(BufferSpan{data + bin_offset.value(), buffer_size.value()});
    } else {
        return scene_fail(ErrorCode::invalid_data, "MK2174_GLB_BIN_MISSING",
                          "the embedded glTF buffer requires a BIN chunk");
    }
    auto result = read_gaussian_scene_impl(doc.value(), buffers, limits, context);
    if (result.has_value())
        result.value().source_bytes = size;
    return result;
} catch (const std::bad_alloc&) {
    return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                      "memory allocation failed while reading the GLB");
} catch (const std::length_error&) {
    return scene_fail(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                      "a GLB allocation exceeds the host size limit");
}

}  // namespace melkor::format::gltf
