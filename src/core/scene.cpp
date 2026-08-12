#include "melkor/scene.hpp"

#include "melkor/budget.hpp"
#include "melkor/checked.hpp"
#include "melkor/math/quaternion.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>

namespace melkor {
namespace {

constexpr std::size_t kControlInterval = 4096;

bool finite(const Vec3f& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Diagnostic splat_error(const char* code, const std::string& message, std::size_t index,
                       const char* field) {
    Diagnostic d(code, Severity::error, message);
    d.with_context("splat_index", static_cast<std::uint64_t>(index));
    d.with_context("field", std::string(field));
    return d;
}

Result<void> validation_checkpoint(const OperationContext* context, const char* phase,
                                   std::uint64_t completed, std::uint64_t total, const char* unit) {
    if (context == nullptr)
        return Result<void>::success();
    return context->checkpoint({"scene.validate", phase, completed, total, unit});
}

}  // namespace

// ---------------------------------------------------------------------------
// ShBuffer
// ---------------------------------------------------------------------------

ShBuffer::ShBuffer(ShBuffer&& other) noexcept
    : degree_(other.degree_), splat_count_(other.splat_count_), data_(std::move(other.data_)) {
    other.degree_ = 0;
    other.splat_count_ = 0;
    other.data_.clear();
}

ShBuffer& ShBuffer::operator=(ShBuffer&& other) noexcept {
    if (this == &other)
        return *this;
    degree_ = other.degree_;
    splat_count_ = other.splat_count_;
    data_ = std::move(other.data_);
    other.degree_ = 0;
    other.splat_count_ = 0;
    other.data_.clear();
    return *this;
}

Result<ShBuffer> ShBuffer::create_impl(std::uint32_t degree, std::size_t splat_count,
                                       std::vector<float> data, const OperationContext* context) {
    // Expected length = splat_count * (degree+1)^2 * 3, computed with checked arithmetic because
    // splat_count and degree can both come from a file.
    auto total = checked_sh_total_floats(splat_count, degree);
    if (!total.has_value()) {
        return Result<ShBuffer>::failure(total.error_code(), total.diagnostics());
    }

    auto expected = checked_size_cast(total.value(), "spherical-harmonic float count");
    if (!expected.has_value()) {
        return Result<ShBuffer>::failure(expected.error_code(), expected.diagnostics());
    }

    if (data.size() != expected.value()) {
        Diagnostic d("MK1501_SH_LENGTH_MISMATCH", Severity::error,
                     "spherical-harmonic data length does not match splat count and degree");
        d.with_context("degree", static_cast<std::uint64_t>(degree));
        d.with_context("splat_count", static_cast<std::uint64_t>(splat_count));
        d.with_context("expected_floats", static_cast<std::uint64_t>(expected.value()));
        d.with_context("actual_floats", static_cast<std::uint64_t>(data.size()));
        return Result<ShBuffer>::failure(ErrorCode::invalid_data, std::move(d));
    }

    // Every SH value must be finite. A NaN in a coefficient renders as a black or garbage splat
    // and propagates through any rotation, so it is rejected at the boundary.
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i % kControlInterval == 0) {
            auto control = validation_checkpoint(context, "spherical_harmonics", i, data.size(),
                                                 "coefficients");
            if (!control.has_value())
                return Result<ShBuffer>::failure(control.error_code(), control.diagnostics());
        }
        if (!std::isfinite(data[i])) {
            Diagnostic d("MK1502_SH_NONFINITE", Severity::error,
                         "spherical-harmonic coefficient is not finite");
            d.with_context("coefficient_index", static_cast<std::uint64_t>(i));
            return Result<ShBuffer>::failure(ErrorCode::invalid_data, std::move(d));
        }
    }

    ShBuffer buffer;
    buffer.degree_ = degree;
    buffer.splat_count_ = splat_count;
    buffer.data_ = std::move(data);
    auto completed = validation_checkpoint(context, "spherical_harmonics", buffer.data_.size(),
                                           buffer.data_.size(), "coefficients");
    if (!completed.has_value())
        return Result<ShBuffer>::failure(completed.error_code(), completed.diagnostics());
    return Result<ShBuffer>::success(std::move(buffer));
}

Result<ShBuffer> ShBuffer::create(std::uint32_t degree, std::size_t splat_count,
                                  std::vector<float> data) {
    return create_impl(degree, splat_count, std::move(data), nullptr);
}

Result<ShBuffer> ShBuffer::create(std::uint32_t degree, std::size_t splat_count,
                                  std::vector<float> data, const OperationContext& context) {
    return create_impl(degree, splat_count, std::move(data), &context);
}

Result<ShBuffer> ShBuffer::black(std::size_t splat_count) {
    // Degree 0, all coefficients zero: the safe default appearance.
    auto total = checked_sh_total_floats(splat_count, 0);
    if (!total.has_value()) {
        return Result<ShBuffer>::failure(total.error_code(), total.diagnostics());
    }
    auto count = checked_size_cast(total.value(), "spherical-harmonic float count");
    if (!count.has_value()) {
        return Result<ShBuffer>::failure(count.error_code(), count.diagnostics());
    }
    try {
        return create(0, splat_count, std::vector<float>(count.value(), 0.0f));
    } catch (const std::bad_alloc&) {
        Diagnostic diagnostic("MK1508_SH_ALLOCATION_FAILED", Severity::error,
                              "memory allocation failed for the spherical-harmonic buffer");
        diagnostic.with_context("float_count", static_cast<std::uint64_t>(count.value()));
        return Result<ShBuffer>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    } catch (const std::length_error&) {
        Diagnostic diagnostic("MK1508_SH_ALLOCATION_FAILED", Severity::error,
                              "the spherical-harmonic buffer exceeds the container limit");
        diagnostic.with_context("float_count", static_cast<std::uint64_t>(count.value()));
        return Result<ShBuffer>::failure(ErrorCode::resource_limit, std::move(diagnostic));
    }
}

std::size_t ShBuffer::coefficients() const noexcept {
    const std::size_t n = degree_ + 1;
    return n * n;
}

std::optional<Vec3f> ShBuffer::dc(std::size_t splat) const noexcept {
    if (splat >= splat_count_)
        return std::nullopt;

    // The storage has one block for each splat. The DC term is the first RGB triplet.
    const std::size_t block = coefficients() * 3;
    const std::size_t base = splat * block;
    if (base > data_.size() || data_.size() - base < 3)
        return std::nullopt;
    return Vec3f{data_[base], data_[base + 1], data_[base + 2]};
}

Result<void> ShBuffer::validate_impl(const OperationContext* context) const {
    auto total = checked_sh_total_floats(splat_count_, degree_);
    if (!total.has_value()) {
        return Result<void>::failure(total.error_code(), total.diagnostics());
    }
    auto expected = checked_size_cast(total.value(), "spherical-harmonic float count");
    if (!expected.has_value()) {
        return Result<void>::failure(expected.error_code(), expected.diagnostics());
    }
    if (data_.size() != expected.value()) {
        Diagnostic diagnostic(
            "MK1501_SH_LENGTH_MISMATCH", Severity::error,
            "spherical-harmonic data length does not match splat count and degree");
        diagnostic.with_context("degree", static_cast<std::uint64_t>(degree_));
        diagnostic.with_context("splat_count", static_cast<std::uint64_t>(splat_count_));
        diagnostic.with_context("expected_floats", static_cast<std::uint64_t>(expected.value()));
        diagnostic.with_context("actual_floats", static_cast<std::uint64_t>(data_.size()));
        return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }
    for (std::size_t index = 0; index < data_.size(); ++index) {
        if (index % kControlInterval == 0) {
            auto control = validation_checkpoint(context, "spherical_harmonics", index,
                                                 data_.size(), "coefficients");
            if (!control.has_value())
                return control;
        }
        if (!std::isfinite(data_[index])) {
            Diagnostic diagnostic("MK1502_SH_NONFINITE", Severity::error,
                                  "spherical-harmonic coefficient is not finite");
            diagnostic.with_context("coefficient_index", static_cast<std::uint64_t>(index));
            return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
    }
    return validation_checkpoint(context, "spherical_harmonics", data_.size(), data_.size(),
                                 "coefficients");
}

Result<void> ShBuffer::validate() const {
    return validate_impl(nullptr);
}

Result<void> ShBuffer::validate(const OperationContext& context) const {
    return validate_impl(&context);
}

// ---------------------------------------------------------------------------
// SplatData
// ---------------------------------------------------------------------------

SplatData::SplatData(SplatData&& other) noexcept
    : positions_(std::move(other.positions_)), scales_(std::move(other.scales_)),
      rotations_(std::move(other.rotations_)), opacities_(std::move(other.opacities_)),
      sh_(std::move(other.sh_)) {
    other.positions_.clear();
    other.scales_.clear();
    other.rotations_.clear();
    other.opacities_.clear();
}

SplatData& SplatData::operator=(SplatData&& other) noexcept {
    if (this == &other)
        return *this;
    positions_ = std::move(other.positions_);
    scales_ = std::move(other.scales_);
    rotations_ = std::move(other.rotations_);
    opacities_ = std::move(other.opacities_);
    sh_ = std::move(other.sh_);
    other.positions_.clear();
    other.scales_.clear();
    other.rotations_.clear();
    other.opacities_.clear();
    return *this;
}

Result<SplatData> SplatData::create_impl(SplatBufferInput input, const OperationContext* context) {
    const std::size_t n = input.positions.size();

    // Every parallel array must have the same length. A length mismatch is exactly the class of
    // bug the old mutable data() allowed -- one array resized out of step with the others.
    auto require_length = [&](std::size_t actual, const char* field) -> Result<void> {
        if (actual != n) {
            Diagnostic d("MK1503_LENGTH_MISMATCH", Severity::error,
                         "per-splat array length does not match the position count");
            d.with_context("field", std::string(field));
            d.with_context("expected", static_cast<std::uint64_t>(n));
            d.with_context("actual", static_cast<std::uint64_t>(actual));
            return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
        }
        return Result<void>::success();
    };

    if (auto r = require_length(input.scales.size(), "scales"); !r.has_value())
        return Result<SplatData>::failure(r.error_code(), r.diagnostics());
    if (auto r = require_length(input.rotations.size(), "rotations"); !r.has_value())
        return Result<SplatData>::failure(r.error_code(), r.diagnostics());
    if (auto r = require_length(input.opacities.size(), "opacities"); !r.has_value())
        return Result<SplatData>::failure(r.error_code(), r.diagnostics());
    if (input.sh.splat_count() != n) {
        Diagnostic d("MK1503_LENGTH_MISMATCH", Severity::error,
                     "spherical-harmonic splat count does not match the position count");
        d.with_context("field", std::string("sh"));
        d.with_context("expected", static_cast<std::uint64_t>(n));
        d.with_context("actual", static_cast<std::uint64_t>(input.sh.splat_count()));
        return Result<SplatData>::failure(ErrorCode::invalid_data, std::move(d));
    }

    // Per-splat domain validation. Each check names the splat and the field, so a diagnostic
    // points at the exact bad value rather than saying "something is wrong".
    for (std::size_t i = 0; i < n; ++i) {
        if (i % kControlInterval == 0) {
            auto control = validation_checkpoint(context, "splats", i, n, "splats");
            if (!control.has_value())
                return Result<SplatData>::failure(control.error_code(), control.diagnostics());
        }
        if (!finite(input.positions[i])) {
            return Result<SplatData>::failure(
                ErrorCode::invalid_data,
                splat_error("MK1504_NONFINITE_POSITION", "position is not finite", i, "position"));
        }
        const Vec3f& s = input.scales[i];
        if (!finite(s) || s.x < 0.0f || s.y < 0.0f || s.z < 0.0f) {
            return Result<SplatData>::failure(
                ErrorCode::invalid_data,
                splat_error("MK1505_NEGATIVE_SCALE",
                            "scale must be finite and nonnegative on every axis", i, "scale"));
        }
        const float o = input.opacities[i];
        if (!std::isfinite(o) || o < 0.0f || o > 1.0f) {
            return Result<SplatData>::failure(ErrorCode::invalid_data,
                                              splat_error("MK1506_OPACITY_OUT_OF_RANGE",
                                                          "opacity must be finite in [0, 1]", i,
                                                          "opacity"));
        }
        // Accept small input error, but store an exact unit quaternion. A near-unit value can
        // otherwise change the scale when code converts it to a rotation matrix.
        Quatf& q = input.rotations[i];
        const math::Quat mq{q.x, q.y, q.z, q.w};
        if (!math::is_unit(mq)) {
            return Result<SplatData>::failure(
                ErrorCode::invalid_data,
                splat_error("MK1507_NON_UNIT_ROTATION",
                            "rotation must be a unit quaternion within tolerance", i, "rotation"));
        }
        auto normalized = math::normalize(mq);
        if (!normalized.has_value()) {
            return Result<SplatData>::failure(normalized.error_code(), normalized.diagnostics());
        }
        q = Quatf{
            static_cast<float>(normalized.value().x), static_cast<float>(normalized.value().y),
            static_cast<float>(normalized.value().z), static_cast<float>(normalized.value().w)};
    }

    SplatData data;
    data.positions_ = std::move(input.positions);
    data.scales_ = std::move(input.scales);
    data.rotations_ = std::move(input.rotations);
    data.opacities_ = std::move(input.opacities);
    data.sh_ = std::move(input.sh);
    auto completed = validation_checkpoint(context, "splats", n, n, "splats");
    if (!completed.has_value())
        return Result<SplatData>::failure(completed.error_code(), completed.diagnostics());
    return Result<SplatData>::success(std::move(data));
}

Result<SplatData> SplatData::create(SplatBufferInput input) {
    return create_impl(std::move(input), nullptr);
}

Result<SplatData> SplatData::create(SplatBufferInput input, const OperationContext& context) {
    return create_impl(std::move(input), &context);
}

Result<void> SplatData::validate_impl(const OperationContext* context) const {
    // Rebuild-and-revalidate would move the data; instead re-run the same domain checks in place.
    // A SplatData created through create() always passes; this exists for values that crossed an
    // ABI or were deserialized without going through create().
    const std::size_t n = positions_.size();
    if (scales_.size() != n || rotations_.size() != n || opacities_.size() != n ||
        sh_.splat_count() != n) {
        Diagnostic d("MK1503_LENGTH_MISMATCH", Severity::error,
                     "SplatData arrays have inconsistent lengths");
        return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (i % kControlInterval == 0) {
            auto control = validation_checkpoint(context, "splats", i, n, "splats");
            if (!control.has_value())
                return control;
        }
        if (!finite(positions_[i]) || !finite(scales_[i]) || scales_[i].x < 0.0f ||
            scales_[i].y < 0.0f || scales_[i].z < 0.0f || !std::isfinite(opacities_[i]) ||
            opacities_[i] < 0.0f || opacities_[i] > 1.0f) {
            return Result<void>::failure(ErrorCode::invalid_data,
                                         splat_error("MK1509_INVALID_SPLAT",
                                                     "splat violates a canonical invariant", i,
                                                     "splat"));
        }
        // The same unit-quaternion invariant create() enforces: a value that crossed an ABI or was
        // deserialized could carry a non-unit rotation, which must not report as valid.
        const Quatf& q = rotations_[i];
        if (!math::is_unit(math::Quat{q.x, q.y, q.z, q.w})) {
            return Result<void>::failure(
                ErrorCode::invalid_data,
                splat_error("MK1507_NON_UNIT_ROTATION",
                            "rotation must be a unit quaternion within tolerance", i, "rotation"));
        }
    }
    auto completed = validation_checkpoint(context, "splats", n, n, "splats");
    if (!completed.has_value())
        return completed;
    return context == nullptr ? sh_.validate() : sh_.validate(*context);
}

Result<void> SplatData::validate() const {
    return validate_impl(nullptr);
}

Result<void> SplatData::validate(const OperationContext& context) const {
    return validate_impl(&context);
}

}  // namespace melkor
