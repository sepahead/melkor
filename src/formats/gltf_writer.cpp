#include "melkor/format/gltf_writer.hpp"

#include "melkor/budget.hpp"
#include "melkor/checked.hpp"
#include "melkor/format/glb_container.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

namespace melkor::format::gltf {

namespace {

using json = nlohmann::json;

constexpr std::size_t kFixedAccessorCount = 4;
constexpr std::uint32_t kArrayBufferTarget = 34962;
constexpr std::size_t kMaxWriterAccessorCount =
    kFixedAccessorCount + (static_cast<std::size_t>(khr::kMaxProfileShDegree) + 1) *
                              (static_cast<std::size_t>(khr::kMaxProfileShDegree) + 1);
constexpr std::uint64_t kJsonBaseBytes = std::uint64_t{8} * 1024;
constexpr std::uint64_t kJsonBytesPerStructuralObject = 1024U;
constexpr std::uint64_t kJsonWorkingSetMultiplier = 16U;

static_assert(sizeof(float) == 4, "the glTF writer requires 32-bit float values");
static_assert(std::numeric_limits<float>::is_iec559,
              "the glTF writer requires IEEE-754 floating-point semantics");

struct AttributeBlock {
    std::uint64_t byte_offset = 0;
    std::uint64_t byte_length = 0;
    std::size_t components = 0;
};

struct WritePlan {
    std::array<AttributeBlock, kMaxWriterAccessorCount> blocks{};
    std::size_t block_count = 0;
    std::size_t source_coefficients = 0;
    std::size_t source_stride = 0;
    std::size_t output_coefficients = 0;
    std::uint64_t binary_bytes = 0;
};

class ScopedBudgetRelease {
public:
    ScopedBudgetRelease(Budget* budget, BudgetKind kind, std::uint64_t amount) noexcept
        : budget_(budget), kind_(kind), amount_(amount) {}

    ~ScopedBudgetRelease() {
        if (budget_ != nullptr && amount_ != 0)
            budget_->release(kind_, amount_);
    }

    ScopedBudgetRelease(const ScopedBudgetRelease&) = delete;
    ScopedBudgetRelease& operator=(const ScopedBudgetRelease&) = delete;

    void retain() noexcept { amount_ = 0; }

private:
    Budget* budget_ = nullptr;
    BudgetKind kind_ = BudgetKind::memory_bytes;
    std::uint64_t amount_ = 0;
};

Result<GlbWriteResult> writer_failure(ErrorCode error_code, const char* code, std::string message) {
    Diagnostic diagnostic(code, Severity::error, std::move(message));
    return Result<GlbWriteResult>::failure(error_code, std::move(diagnostic));
}

Result<GlbWriteResult> writer_checkpoint(const OperationContext& context, const char* phase,
                                         std::uint64_t completed, std::uint64_t total) {
    auto control = context.checkpoint({"gltf.write", phase, completed, total, "splats"});
    if (!control.has_value()) {
        return Result<GlbWriteResult>::failure(control.error_code(), control.diagnostics());
    }
    return Result<GlbWriteResult>::success({});
}

Result<WritePlan> make_write_plan(std::uint64_t splat_count, std::uint32_t source_degree,
                                  std::uint32_t output_degree) {
    auto source_coefficients = checked_sh_coefficient_count(source_degree);
    if (!source_coefficients.has_value()) {
        return Result<WritePlan>::failure(source_coefficients.error_code(),
                                          source_coefficients.diagnostics());
    }
    auto output_coefficients = checked_sh_coefficient_count(output_degree);
    if (!output_coefficients.has_value()) {
        return Result<WritePlan>::failure(output_coefficients.error_code(),
                                          output_coefficients.diagnostics());
    }
    auto source_count = checked_size_cast(source_coefficients.value(), "source SH coefficients");
    auto output_count = checked_size_cast(output_coefficients.value(), "output SH coefficients");
    if (!source_count.has_value()) {
        return Result<WritePlan>::failure(source_count.error_code(), source_count.diagnostics());
    }
    if (!output_count.has_value()) {
        return Result<WritePlan>::failure(output_count.error_code(), output_count.diagnostics());
    }
    auto source_stride_floats = checked_mul(source_coefficients.value(), 3, "source SH stride");
    if (!source_stride_floats.has_value()) {
        return Result<WritePlan>::failure(source_stride_floats.error_code(),
                                          source_stride_floats.diagnostics());
    }
    auto source_stride = checked_size_cast(source_stride_floats.value(), "source SH stride");
    if (!source_stride.has_value()) {
        return Result<WritePlan>::failure(source_stride.error_code(), source_stride.diagnostics());
    }

    WritePlan plan;
    plan.source_coefficients = source_count.value();
    plan.source_stride = source_stride.value();
    plan.output_coefficients = output_count.value();

    const auto add_block = [&](std::size_t components) -> Result<void> {
        auto floats = checked_mul(splat_count, components, "glTF attribute float count");
        if (!floats.has_value()) {
            return Result<void>::failure(floats.error_code(), floats.diagnostics());
        }
        auto bytes = checked_mul(floats.value(), sizeof(float), "glTF attribute byte length");
        if (!bytes.has_value()) {
            return Result<void>::failure(bytes.error_code(), bytes.diagnostics());
        }
        auto end = checked_add(plan.binary_bytes, bytes.value(), "glTF binary buffer size");
        if (!end.has_value()) {
            return Result<void>::failure(end.error_code(), end.diagnostics());
        }
        if (plan.block_count >= plan.blocks.size()) {
            Diagnostic diagnostic("MK2211_GLTF_WRITE_INVARIANT", Severity::error,
                                  "the glTF write plan has too many attribute blocks");
            return Result<void>::failure(ErrorCode::internal_error, std::move(diagnostic));
        }
        plan.blocks[plan.block_count++] =
            AttributeBlock{plan.binary_bytes, bytes.value(), components};
        plan.binary_bytes = end.value();
        return Result<void>::success();
    };

    for (const std::size_t components : {3U, 4U, 3U, 1U}) {
        auto added = add_block(components);
        if (!added.has_value()) {
            return Result<WritePlan>::failure(added.error_code(), added.diagnostics());
        }
    }
    for (std::size_t coefficient = 0; coefficient < plan.output_coefficients; ++coefficient) {
        auto added = add_block(3);
        if (!added.has_value()) {
            return Result<WritePlan>::failure(added.error_code(), added.diagnostics());
        }
    }
    return Result<WritePlan>::success(plan);
}

// Add one accessor and its matching buffer view.
struct Builder {
    json accessors = json::array();
    json buffer_views = json::array();

    std::size_t add(const AttributeBlock& block, std::uint64_t count) {
        const char* type =
            block.components == 1 ? "SCALAR" : (block.components == 3 ? "VEC3" : "VEC4");
        buffer_views.push_back({{"buffer", 0},
                                {"byteOffset", block.byte_offset},
                                {"byteLength", block.byte_length},
                                {"target", kArrayBufferTarget}});
        json accessor = {{"bufferView", buffer_views.size() - 1},
                         {"componentType", 5126},
                         {"type", type},
                         {"count", count}};
        accessors.push_back(std::move(accessor));
        return accessors.size() - 1;
    }
};

class BinaryWriter {
public:
    BinaryWriter(std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool append(float value) noexcept {
        if (position_ > size_ || size_ - position_ < sizeof(float)) {
            return false;
        }
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        data_[position_++] = static_cast<std::uint8_t>(bits & 0xFFU);
        data_[position_++] = static_cast<std::uint8_t>((bits >> 8U) & 0xFFU);
        data_[position_++] = static_cast<std::uint8_t>((bits >> 16U) & 0xFFU);
        data_[position_++] = static_cast<std::uint8_t>((bits >> 24U) & 0xFFU);
        return true;
    }

    bool complete() const noexcept { return position_ == size_; }

private:
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
};

}  // namespace

Result<GlbWriteResult> write_glb(const SplatData& data, khr::ColorSpace color_space,
                                 const Limits& limits) try {
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    return write_glb(data, color_space, context);
} catch (const std::bad_alloc&) {
    return writer_failure(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                          "memory allocation failed while writing the GLB");
} catch (const std::length_error&) {
    return writer_failure(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                          "a GLB writer allocation exceeds the host size limit");
}

Result<GlbWriteResult> write_glb(const SplatData& data, khr::ColorSpace color_space,
                                 const OperationContext& context) try {
    if (context.budget == nullptr) {
        return writer_failure(ErrorCode::internal_error, "MK0310_NO_BUDGET",
                              "the glTF writer requires a resource budget");
    }
    auto started = writer_checkpoint(context, "prepare", 0, data.size());
    if (!started.has_value()) {
        return started;
    }
    const Limits& limits = context.budget->limits();
    if (auto valid = limits.validate(); !valid.has_value()) {
        return Result<GlbWriteResult>::failure(valid.error_code(), valid.diagnostics());
    }
    if (auto valid = data.validate(context); !valid.has_value()) {
        return Result<GlbWriteResult>::failure(valid.error_code(), valid.diagnostics());
    }
    if (color_space != khr::ColorSpace::srgb_rec709_display &&
        color_space != khr::ColorSpace::lin_rec709_display) {
        return writer_failure(ErrorCode::invalid_argument, "MK2212_GLTF_COLOR_SPACE",
                              "the glTF writer received an invalid color space");
    }

    const std::size_t n = data.size();
    if (n == 0) {
        return writer_failure(ErrorCode::invalid_data, "MK2210_GLTF_NO_SPLATS",
                              "cannot write a KHR_gaussian_splatting GLB with zero splats");
    }
    const std::uint64_t splat_count = static_cast<std::uint64_t>(n);
    if (auto charged = context.observe(BudgetKind::splats, splat_count, "gltf.write.splats");
        !charged.has_value()) {
        return Result<GlbWriteResult>::failure(charged.error_code(), charged.diagnostics());
    }

    const std::uint32_t source_degree = data.sh().degree();
    const std::uint32_t write_degree =
        source_degree > khr::kMaxProfileShDegree ? khr::kMaxProfileShDegree : source_degree;
    auto plan_result = make_write_plan(splat_count, source_degree, write_degree);
    if (!plan_result.has_value()) {
        return Result<GlbWriteResult>::failure(plan_result.error_code(), plan_result.diagnostics());
    }
    const WritePlan& plan = plan_result.value();
    auto planned_binary_size = checked_size_cast(plan.binary_bytes, "glTF binary buffer size");
    if (!planned_binary_size.has_value()) {
        return Result<GlbWriteResult>::failure(planned_binary_size.error_code(),
                                               planned_binary_size.diagnostics());
    }

    auto structural_count = checked_mul(plan.block_count, 2, "glTF structural object count");
    if (!structural_count.has_value()) {
        return Result<GlbWriteResult>::failure(structural_count.error_code(),
                                               structural_count.diagnostics());
    }
    if (auto charged = context.observe(BudgetKind::accessors, structural_count.value(),
                                       "gltf.write.structural_objects");
        !charged.has_value()) {
        return Result<GlbWriteResult>::failure(charged.error_code(), charged.diagnostics());
    }
    if (auto charged = context.observe(BudgetKind::gltf_nodes, 1, "gltf.write.nodes");
        !charged.has_value()) {
        return Result<GlbWriteResult>::failure(charged.error_code(), charged.diagnostics());
    }

    auto json_variable_bytes = checked_mul(structural_count.value(), kJsonBytesPerStructuralObject,
                                           "glTF writer JSON size bound");
    auto json_bound = json_variable_bytes.has_value()
                          ? checked_add(kJsonBaseBytes, json_variable_bytes.value(),
                                        "glTF writer JSON size bound")
                          : Result<std::uint64_t>::failure(json_variable_bytes.error_code(),
                                                           json_variable_bytes.diagnostics());
    auto json_working_bytes =
        json_bound.has_value()
            ? checked_mul(json_bound.value(), kJsonWorkingSetMultiplier,
                          "glTF writer JSON working memory")
            : Result<std::uint64_t>::failure(json_bound.error_code(), json_bound.diagnostics());
    if (!json_working_bytes.has_value()) {
        return Result<GlbWriteResult>::failure(json_working_bytes.error_code(),
                                               json_working_bytes.diagnostics());
    }

    if (auto charged =
            context.consume(BudgetKind::memory_bytes, plan.binary_bytes, "gltf.write.output");
        !charged.has_value()) {
        return Result<GlbWriteResult>::failure(charged.error_code(), charged.diagnostics());
    }
    ScopedBudgetRelease binary_release(context.budget, BudgetKind::memory_bytes, plan.binary_bytes);
    if (auto charged = context.consume(BudgetKind::memory_bytes, json_working_bytes.value(),
                                       "gltf.write.json");
        !charged.has_value()) {
        return Result<GlbWriteResult>::failure(charged.error_code(), charged.diagnostics());
    }
    ScopedBudgetRelease json_release(context.budget, BudgetKind::memory_bytes,
                                     json_working_bytes.value());

    LossReport losses;
    if (source_degree > khr::kMaxProfileShDegree) {
        LossItem item;
        item.code = loss_code::kShDegreeTruncated;
        item.severity = LossSeverity::severe;
        item.source_feature = "spherical harmonics degree " + std::to_string(source_degree);
        item.target_constraint = "the KHR_gaussian_splatting RC profile supports degree 0-3";
        item.affected_splats = n;
        item.remediation =
            "approve LOSS_SH_DEGREE_TRUNCATED to accept degree-3 output, or target a "
            "format that carries degree 4";
        MELKOR_TRY_AS(losses.add(std::move(item)), GlbWriteResult);
    }

    float min_pos[3] = {std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity()};
    float max_pos[3] = {-std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()};
    for (std::size_t splat = 0; splat < n; ++splat) {
        if (splat % 4096 == 0) {
            auto control = writer_checkpoint(context, "bounds", splat, n);
            if (!control.has_value()) {
                return control;
            }
        }
        const Vec3f& position = data.positions()[splat];
        const float xyz[3] = {position.x, position.y, position.z};
        for (std::size_t component = 0; component < 3; ++component) {
            if (xyz[component] < min_pos[component]) {
                min_pos[component] = xyz[component];
            }
            if (xyz[component] > max_pos[component]) {
                max_pos[component] = xyz[component];
            }
        }
    }

    Builder builder;
    const std::size_t position_index = builder.add(plan.blocks[0], splat_count);
    builder.accessors[position_index]["min"] = {min_pos[0], min_pos[1], min_pos[2]};
    builder.accessors[position_index]["max"] = {max_pos[0], max_pos[1], max_pos[2]};
    const std::size_t rotation_index = builder.add(plan.blocks[1], splat_count);
    const std::size_t scale_index = builder.add(plan.blocks[2], splat_count);
    const std::size_t opacity_index = builder.add(plan.blocks[3], splat_count);

    json attributes = {{khr::kAttrPosition, position_index},
                       {khr::kAttrRotation, rotation_index},
                       {khr::kAttrScale, scale_index},
                       {khr::kAttrOpacity, opacity_index}};
    for (std::size_t coefficient = 0; coefficient < plan.output_coefficients; ++coefficient) {
        const std::size_t index =
            builder.add(plan.blocks[kFixedAccessorCount + coefficient], splat_count);
        const auto address = khr::sh_flat_to_address(coefficient);
        if (!address.has_value()) {
            return writer_failure(ErrorCode::internal_error, "MK2211_GLTF_WRITE_INVARIANT",
                                  "the SH coefficient index exceeds the canonical model");
        }
        attributes[khr::sh_attribute(*address)] = index;
    }

    json primitive = {
        {"mode", khr::kPrimitiveModePoints},
        {"attributes", attributes},
        {"extensions",
         {{khr::kExtensionName,
           {{"kernel", khr::kKernelEllipse}, {"colorSpace", khr::to_string(color_space)}}}}}};
    json document = {{"asset", {{"version", "2.0"}, {"generator", "melkor"}}},
                     {"extensionsUsed", json::array({khr::kExtensionName})},
                     {"buffers", json::array({{{"byteLength", plan.binary_bytes}}})},
                     {"bufferViews", builder.buffer_views},
                     {"accessors", builder.accessors},
                     {"meshes", json::array({{{"primitives", json::array({primitive})}}})},
                     {"nodes", json::array({{{"mesh", 0}}})},
                     {"scenes", json::array({{{"nodes", json::array({0})}}})},
                     {"scene", 0}};

    const std::string json_text = document.dump();
    if (json_text.size() > json_bound.value()) {
        return writer_failure(ErrorCode::internal_error, "MK2211_GLTF_WRITE_INVARIANT",
                              "the serialized glTF JSON exceeds its preflight bound");
    }
    auto total_size = glb::encoded_glb_size(json_text.size(), planned_binary_size.value());
    if (!total_size.has_value()) {
        return Result<GlbWriteResult>::failure(total_size.error_code(), total_size.diagnostics());
    }
    auto framing_bytes =
        checked_sub(total_size.value(), plan.binary_bytes, "GLB framing and JSON byte count");
    if (!framing_bytes.has_value()) {
        return Result<GlbWriteResult>::failure(framing_bytes.error_code(),
                                               framing_bytes.diagnostics());
    }
    if (auto charged =
            context.consume(BudgetKind::memory_bytes, framing_bytes.value(), "gltf.write.output");
        !charged.has_value()) {
        return Result<GlbWriteResult>::failure(charged.error_code(), charged.diagnostics());
    }
    ScopedBudgetRelease framing_release(context.budget, BudgetKind::memory_bytes,
                                        framing_bytes.value());

    auto built = glb::build_glb_buffer(json_text, planned_binary_size.value());
    if (!built.has_value()) {
        return Result<GlbWriteResult>::failure(built.error_code(), built.diagnostics());
    }
    auto binary_range =
        checked_range(built.value().bin_payload.offset(), built.value().bin_payload.length(),
                      built.value().bytes.size(), "GLB BIN payload");
    if (!binary_range.has_value() || built.value().bin_payload.length() != plan.binary_bytes) {
        return writer_failure(ErrorCode::internal_error, "MK2211_GLTF_WRITE_INVARIANT",
                              "the GLB builder returned an incorrect BIN range");
    }
    auto binary_offset = checked_size_cast(binary_range.value().offset(), "GLB BIN offset");
    auto binary_size = checked_size_cast(binary_range.value().length(), "GLB BIN length");
    if (!binary_offset.has_value() || !binary_size.has_value()) {
        return writer_failure(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                              "the GLB BIN range exceeds the host address space");
    }

    BinaryWriter binary(built.value().bytes.data() + binary_offset.value(), binary_size.value());
    for (std::size_t splat = 0; splat < n; ++splat) {
        if (splat % 4096 == 0) {
            auto control = writer_checkpoint(context, "positions", splat, n);
            if (!control.has_value()) {
                return control;
            }
        }
        const Vec3f& position = data.positions()[splat];
        if (!binary.append(position.x) || !binary.append(position.y) ||
            !binary.append(position.z)) {
            return writer_failure(ErrorCode::internal_error, "MK2211_GLTF_WRITE_INVARIANT",
                                  "the POSITION block exceeds the planned BIN range");
        }
    }
    for (std::size_t splat = 0; splat < n; ++splat) {
        if (splat % 4096 == 0) {
            auto control = writer_checkpoint(context, "rotations", splat, n);
            if (!control.has_value()) {
                return control;
            }
        }
        const Quatf& rotation = data.rotations()[splat];
        if (!binary.append(rotation.x) || !binary.append(rotation.y) ||
            !binary.append(rotation.z) || !binary.append(rotation.w)) {
            return writer_failure(ErrorCode::internal_error, "MK2211_GLTF_WRITE_INVARIANT",
                                  "the ROTATION block exceeds the planned BIN range");
        }
    }
    for (std::size_t splat = 0; splat < n; ++splat) {
        if (splat % 4096 == 0) {
            auto control = writer_checkpoint(context, "scales", splat, n);
            if (!control.has_value()) {
                return control;
            }
        }
        const Vec3f& scale = data.scales()[splat];
        if (!binary.append(scale.x) || !binary.append(scale.y) || !binary.append(scale.z)) {
            return writer_failure(ErrorCode::internal_error, "MK2211_GLTF_WRITE_INVARIANT",
                                  "the SCALE block exceeds the planned BIN range");
        }
    }
    for (std::size_t splat = 0; splat < n; ++splat) {
        if (splat % 4096 == 0) {
            auto control = writer_checkpoint(context, "opacities", splat, n);
            if (!control.has_value()) {
                return control;
            }
        }
        const float opacity = data.opacities()[splat];
        if (!binary.append(opacity)) {
            return writer_failure(ErrorCode::internal_error, "MK2211_GLTF_WRITE_INVARIANT",
                                  "the OPACITY block exceeds the planned BIN range");
        }
    }

    const auto& raw_sh = data.sh().raw();
    for (std::size_t coefficient = 0; coefficient < plan.output_coefficients; ++coefficient) {
        for (std::size_t splat = 0; splat < n; ++splat) {
            if (splat % 4096 == 0) {
                auto control = writer_checkpoint(context, "spherical_harmonics", splat, n);
                if (!control.has_value()) {
                    return control;
                }
            }
            const std::size_t base = splat * plan.source_stride + coefficient * 3;
            if (!binary.append(raw_sh[base]) || !binary.append(raw_sh[base + 1]) ||
                !binary.append(raw_sh[base + 2])) {
                return writer_failure(ErrorCode::internal_error, "MK2211_GLTF_WRITE_INVARIANT",
                                      "an SH block exceeds the planned BIN range");
            }
        }
    }
    if (!binary.complete()) {
        return writer_failure(ErrorCode::internal_error, "MK2211_GLTF_WRITE_INVARIANT",
                              "the GLB BIN payload is shorter than its write plan");
    }

    auto completed = writer_checkpoint(context, "complete", n, n);
    if (!completed.has_value()) {
        return completed;
    }

    GlbWriteResult result;
    result.bytes = std::move(built.value().bytes);
    result.losses = std::move(losses);
    auto retained = context.budget->adopt_charge(BudgetKind::memory_bytes, total_size.value(),
                                                 "gltf.write_result");
    if (!retained.has_value()) {
        return Result<GlbWriteResult>::failure(retained.error_code(), retained.diagnostics());
    }
    result.set_retained_memory(std::move(retained).value());
    binary_release.retain();
    framing_release.retain();
    return Result<GlbWriteResult>::success(std::move(result));
} catch (const std::bad_alloc&) {
    return writer_failure(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                          "memory allocation failed while writing the GLB");
} catch (const std::length_error&) {
    return writer_failure(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                          "a GLB writer allocation exceeds the host size limit");
} catch (const nlohmann::json::exception&) {
    return writer_failure(ErrorCode::internal_error, "MK2211_GLTF_WRITE_INVARIANT",
                          "the glTF JSON writer failed");
}

}  // namespace melkor::format::gltf
