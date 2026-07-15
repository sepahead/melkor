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

SplatData::EditTransaction SplatData::edit() const { return EditTransaction(*this); }

// ---------------------------------------------------------------------------
// SplatData::EditTransaction
// ---------------------------------------------------------------------------

SplatData::EditTransaction::EditTransaction(const SplatData& source)
    : positions_(source.positions_),
      scales_(source.scales_),
      rotations_(source.rotations_),
      opacities_(source.opacities_),
      sh_degree_(source.sh_.degree()),
      sh_data_(source.sh_.raw()) {}

SplatData::EditTransaction& SplatData::EditTransaction::set_positions(
    std::vector<Vec3f> positions) {
    positions_ = std::move(positions);
    return *this;
}

SplatData::EditTransaction& SplatData::EditTransaction::set_scales(
    std::vector<Vec3f> scales) {
    scales_ = std::move(scales);
    return *this;
}

SplatData::EditTransaction& SplatData::EditTransaction::set_rotations(
    std::vector<Quatf> rotations) {
    rotations_ = std::move(rotations);
    return *this;
}

SplatData::EditTransaction& SplatData::EditTransaction::set_opacities(
    std::vector<float> opacities) {
    opacities_ = std::move(opacities);
    return *this;
}

SplatData::EditTransaction& SplatData::EditTransaction::set_sh(ShBuffer sh) {
    sh_degree_ = sh.degree();
    sh_data_ = sh.raw();
    return *this;
}

Result<void> SplatData::EditTransaction::reserve(std::size_t count, Budget& budget) {
    auto sh_total_u64 = checked_sh_total_floats(count, sh_degree_);
    if (!sh_total_u64.has_value()) {
        return Result<void>::failure(sh_total_u64.error_code(), sh_total_u64.diagnostics());
    }
    auto sh_total = checked_size_cast(sh_total_u64.value(), "edit SH reserve length");
    if (!sh_total.has_value()) {
        return Result<void>::failure(sh_total.error_code(), sh_total.diagnostics());
    }

    auto sh_per_splat = checked_sh_total_floats(1, sh_degree_);
    if (!sh_per_splat.has_value()) {
        return Result<void>::failure(sh_per_splat.error_code(), sh_per_splat.diagnostics());
    }
    auto sh_bytes = checked_mul(sh_per_splat.value(), sizeof(float),
                                "edit SH bytes per splat");
    if (!sh_bytes.has_value()) {
        return Result<void>::failure(sh_bytes.error_code(), sh_bytes.diagnostics());
    }
    constexpr std::uint64_t kFixedBytesPerSplat =
        sizeof(Vec3f) + sizeof(Vec3f) + sizeof(Quatf) + sizeof(float);
    auto bytes_per_splat =
        checked_add(kFixedBytesPerSplat, sh_bytes.value(), "edit bytes per splat");
    if (!bytes_per_splat.has_value()) {
        return Result<void>::failure(bytes_per_splat.error_code(), bytes_per_splat.diagnostics());
    }

    // The edit may change SH degree after an earlier reserve. Track the total charge directly:
    // combining a previous capacity with a previous per-splat stride loses information when the
    // degree shrinks while the count grows, and can undercharge a later degree increase. We do not
    // release budget when degree/count shrinks because vector capacity remains allocated.
    auto target_bytes = checked_mul(count, bytes_per_splat.value(),
                                    "edit reserved memory");
    if (!target_bytes.has_value()) {
        return Result<void>::failure(target_bytes.error_code(), target_bytes.diagnostics());
    }
    const std::uint64_t additional_bytes =
        target_bytes.value() > budgeted_memory_bytes_
            ? target_bytes.value() - budgeted_memory_bytes_
            : 0;
    if (additional_bytes != 0) {
        auto charged =
            budget.consume(BudgetKind::memory_bytes, additional_bytes, "scene.edit.reserve");
        if (!charged.has_value()) return charged;
    }

    // Record the conservative charge before reserving. If an allocator fails partway through,
    // some vector capacities may already have grown; retaining the charge is safer and accurate.
    budgeted_memory_bytes_ = std::max(budgeted_memory_bytes_, target_bytes.value());
    try {
        positions_.reserve(count);
        scales_.reserve(count);
        rotations_.reserve(count);
        opacities_.reserve(count);
        sh_data_.reserve(sh_total.value());
    } catch (const std::bad_alloc&) {
        Diagnostic d("MK1510_EDIT_ALLOCATION_FAILED", Severity::error,
                     "unable to reserve memory for the scene edit");
        d.with_context("requested_splats", static_cast<std::uint64_t>(count));
        return Result<void>::failure(ErrorCode::resource_limit, std::move(d));
    } catch (const std::length_error&) {
        Diagnostic d("MK1510_EDIT_ALLOCATION_FAILED", Severity::error,
                     "scene edit reserve exceeds a container's representable size");
        d.with_context("requested_splats", static_cast<std::uint64_t>(count));
        return Result<void>::failure(ErrorCode::resource_limit, std::move(d));
    }
    return Result<void>::success();
}

Result<void> SplatData::EditTransaction::append(const SplatRecord& splat, Budget& budget) {
    const std::size_t n = positions_.size();
    if (scales_.size() != n || rotations_.size() != n || opacities_.size() != n) {
        Diagnostic d("MK1509_EDIT_LENGTH_MISMATCH", Severity::error,
                     "cannot append while edit arrays have inconsistent lengths");
        return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
    }
    auto expected_sh = checked_sh_total_floats(n, sh_degree_);
    if (!expected_sh.has_value()) {
        return Result<void>::failure(expected_sh.error_code(), expected_sh.diagnostics());
    }
    if (expected_sh.value() != sh_data_.size()) {
        Diagnostic d("MK1509_EDIT_LENGTH_MISMATCH", Severity::error,
                     "cannot append while edit SH storage has an inconsistent length");
        d.with_context("expected_floats", expected_sh.value());
        d.with_context("actual_floats", static_cast<std::uint64_t>(sh_data_.size()));
        return Result<void>::failure(ErrorCode::invalid_data, std::move(d));
    }

    // Validate the record through the exact same factories used at commit. This is intentionally
    // done before reserve/charging so malformed input cannot change even transaction capacity.
    auto one_sh = ShBuffer::create(sh_degree_, 1, splat.sh);
    if (!one_sh.has_value()) {
        return Result<void>::failure(one_sh.error_code(), one_sh.diagnostics());
    }
    SplatBufferInput one_input;
    one_input.positions.push_back(splat.position);
    one_input.scales.push_back(splat.scale);
    one_input.rotations.push_back(splat.rotation);
    one_input.opacities.push_back(splat.opacity);
    one_input.sh = std::move(one_sh.value());
    auto validated = SplatData::create(std::move(one_input));
    if (!validated.has_value()) {
        return Result<void>::failure(validated.error_code(), validated.diagnostics());
    }

    auto next_u64 = checked_add(n, 1, "edited splat count");
    if (!next_u64.has_value()) {
        return Result<void>::failure(next_u64.error_code(), next_u64.diagnostics());
    }
    auto next = checked_size_cast(next_u64.value(), "edited splat count");
    if (!next.has_value()) {
        return Result<void>::failure(next.error_code(), next.diagnostics());
    }
    auto reserved = reserve(next.value(), budget);
    if (!reserved.has_value()) return reserved;

    auto charged = budget.consume(BudgetKind::splats, 1, "scene.edit.append");
    if (!charged.has_value()) return charged;

    const SplatData& value = validated.value();
    positions_.push_back(value.positions()[0]);
    scales_.push_back(value.scales()[0]);
    rotations_.push_back(value.rotations()[0]);
    opacities_.push_back(value.opacities()[0]);
    sh_data_.insert(sh_data_.end(), value.sh().raw().begin(), value.sh().raw().end());
    return Result<void>::success();
}

Result<SplatData> SplatData::EditTransaction::commit() {
    if (committed_) {
        Diagnostic d("MK1511_EDIT_ALREADY_COMMITTED", Severity::error,
                     "scene edit transaction has already been committed");
        return Result<SplatData>::failure(ErrorCode::internal_error, std::move(d));
    }
    committed_ = true;

    auto sh = ShBuffer::create(sh_degree_, positions_.size(), std::move(sh_data_));
    if (!sh.has_value()) {
        return Result<SplatData>::failure(sh.error_code(), sh.diagnostics());
    }
    SplatBufferInput input;
    input.positions = std::move(positions_);
    input.scales = std::move(scales_);
    input.rotations = std::move(rotations_);
    input.opacities = std::move(opacities_);
    input.sh = std::move(sh.value());
    return SplatData::create(std::move(input));
}

}  // namespace melkor
