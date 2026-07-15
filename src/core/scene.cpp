#include "melkor/scene.hpp"

#include "melkor/checked.hpp"
#include "melkor/math/quaternion.hpp"

#include <cmath>

namespace melkor {
namespace {

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

}  // namespace

// ---------------------------------------------------------------------------
// ShBuffer
// ---------------------------------------------------------------------------

Result<ShBuffer> ShBuffer::create(std::uint32_t degree, std::size_t splat_count,
                                  std::vector<float> data) {
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
    return Result<ShBuffer>::success(std::move(buffer));
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
    return create(0, splat_count, std::vector<float>(count.value(), 0.0f));
}

std::size_t ShBuffer::coefficients() const noexcept {
    const std::size_t n = degree_ + 1;
    return n * n;
}

float ShBuffer::dc(std::size_t splat, int channel) const {
    // Coefficient-major layout: [coeff][channel][splat] is not how we store it; we store
    // per-splat blocks of (coefficients * 3). The DC term is coefficient 0, so it is the first
    // three floats of each splat's block.
    const std::size_t block = coefficients() * 3;
    return data_[splat * block + static_cast<std::size_t>(channel)];
}

// ---------------------------------------------------------------------------
// SplatData
// ---------------------------------------------------------------------------

Result<SplatData> SplatData::create(SplatBufferInput input) {
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

    if (auto r = require_length(input.scales.size(), "scales"); !r.has_value()) return Result<SplatData>::failure(r.error_code(), r.diagnostics());
    if (auto r = require_length(input.rotations.size(), "rotations"); !r.has_value()) return Result<SplatData>::failure(r.error_code(), r.diagnostics());
    if (auto r = require_length(input.opacities.size(), "opacities"); !r.has_value()) return Result<SplatData>::failure(r.error_code(), r.diagnostics());
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
        if (!finite(input.positions[i])) {
            return Result<SplatData>::failure(
                ErrorCode::invalid_data,
                splat_error("MK1504_NONFINITE_POSITION", "position is not finite", i, "position"));
        }
        const Vec3f& s = input.scales[i];
        if (!finite(s) || s.x <= 0.0f || s.y <= 0.0f || s.z <= 0.0f) {
            return Result<SplatData>::failure(
                ErrorCode::invalid_data,
                splat_error("MK1505_NONPOSITIVE_SCALE",
                            "scale must be finite and strictly positive on every axis", i, "scale"));
        }
        const float o = input.opacities[i];
        if (!std::isfinite(o) || o < 0.0f || o > 1.0f) {
            return Result<SplatData>::failure(
                ErrorCode::invalid_data,
                splat_error("MK1506_OPACITY_OUT_OF_RANGE", "opacity must be finite in [0, 1]", i,
                            "opacity"));
        }
        // The rotation must be a unit quaternion within tolerance. This reuses the math oracle's
        // definition so the scene model and the transforms agree on what "unit" means.
        const Quatf& q = input.rotations[i];
        math::Quat mq{q.x, q.y, q.z, q.w};
        if (!math::is_unit(mq)) {
            return Result<SplatData>::failure(
                ErrorCode::invalid_data,
                splat_error("MK1507_NON_UNIT_ROTATION",
                            "rotation must be a unit quaternion within tolerance", i, "rotation"));
        }
    }

    SplatData data;
    data.positions_ = std::move(input.positions);
    data.scales_ = std::move(input.scales);
    data.rotations_ = std::move(input.rotations);
    data.opacities_ = std::move(input.opacities);
    data.sh_ = std::move(input.sh);
    return Result<SplatData>::success(std::move(data));
}

Result<void> SplatData::validate() const {
    // Rebuild-and-revalidate would move the data; instead re-run the same domain checks in place.
    // A SplatData created through create() always passes; this exists for values that crossed an
    // ABI or were deserialised without going through create().
    const std::size_t n = positions_.size();
    if (scales_.size() != n || rotations_.size() != n || opacities_.size() != n ||
        sh_.splat_count() != n) {
        Diagnostic d("MK1503_LENGTH_MISMATCH", Severity::error,
                     "SplatData arrays have inconsistent lengths");
        return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!finite(positions_[i]) || !finite(scales_[i]) ||
            scales_[i].x <= 0.0f || scales_[i].y <= 0.0f || scales_[i].z <= 0.0f ||
            !std::isfinite(opacities_[i]) || opacities_[i] < 0.0f || opacities_[i] > 1.0f) {
            return Result<void>::failure(
                ErrorCode::invalid_data,
                splat_error("MK1508_INVALID_SPLAT", "splat violates a canonical invariant", i,
                            "splat"));
        }
        // The same unit-quaternion invariant create() enforces: a value that crossed an ABI or was
        // deserialised could carry a non-unit rotation, which must not report as valid.
        const Quatf& q = rotations_[i];
        if (!math::is_unit(math::Quat{q.x, q.y, q.z, q.w})) {
            return Result<void>::failure(
                ErrorCode::invalid_data,
                splat_error("MK1507_NON_UNIT_ROTATION",
                            "rotation must be a unit quaternion within tolerance", i, "rotation"));
        }
    }
    // Spherical harmonics must be finite (create() enforces this; a deserialised buffer might not).
    const std::vector<float>& sh = sh_.raw();
    for (std::size_t k = 0; k < sh.size(); ++k) {
        if (!std::isfinite(sh[k])) {
            Diagnostic d("MK1502_SH_NONFINITE", Severity::error,
                         "spherical-harmonic coefficient is not finite");
            d.with_context("coefficient_index", static_cast<std::uint64_t>(k));
            return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
        }
    }
    return Result<void>::success();
}

}  // namespace melkor
