// glTF KHR_gaussian_splatting writer.
//
// Serializes a canonical `SplatData` into a GLB carrying one `KHR_gaussian_splatting` POINTS
// primitive. It is the inverse of the reader. It preserves linear scale, linear opacity, XYZW
// quaternions, and SH coefficients. It transposes splat-major SH blocks into one float accessor per
// coefficient. KHR requires that coefficient-major layout.
//
// The pinned RC profile supports SH degree 0-3. A degree-4 source is written at degree 3 with a
// reported `LOSS_SH_DEGREE_TRUNCATED`; the writer returns that loss in the report rather than
// silently dropping the coefficients, so the caller applies the loss policy before treating the
// output as faithful.

#ifndef MELKOR_FORMAT_GLTF_WRITER_HPP
#define MELKOR_FORMAT_GLTF_WRITER_HPP

#include "melkor/budget.hpp"
#include "melkor/error.hpp"
#include "melkor/format/gltf_khr.hpp"
#include "melkor/format/loss.hpp"
#include "melkor/limits.hpp"
#include "melkor/scene.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace melkor::format::gltf {

struct GlbWriteResult {
private:
    // Later members are destroyed first. Release the charge after the bytes are destroyed.
    Budget::Charge retained_memory_;

public:
    GlbWriteResult() = default;
    GlbWriteResult(const GlbWriteResult&) = delete;
    GlbWriteResult& operator=(const GlbWriteResult&) = delete;
    GlbWriteResult(GlbWriteResult&&) noexcept = default;
    GlbWriteResult& operator=(GlbWriteResult&& other) noexcept {
        if (this == &other)
            return *this;
        bytes = std::move(other.bytes);
        losses = std::move(other.losses);
        retained_memory_ = std::move(other.retained_memory_);
        return *this;
    }

    std::uint64_t retained_memory_bytes() const noexcept { return retained_memory_.amount(); }
    void set_retained_memory(Budget::Charge charge) noexcept {
        retained_memory_ = std::move(charge);
    }
    Budget::Charge take_retained_memory() noexcept { return std::move(retained_memory_); }

    std::vector<std::uint8_t> bytes;  // the complete GLB
    LossReport losses;                // e.g. LOSS_SH_DEGREE_TRUNCATED for a degree-4 source
};

// Write `data` as one KHR_gaussian_splatting primitive.
// The node uses the identity transform.
// The limits apply to the splat count, structure, working memory, and output size.
Result<GlbWriteResult>
write_glb(const SplatData& data, khr::ColorSpace color_space,
          const Limits& limits = Limits::for_profile(LimitsProfile::desktop));

// Use one context across all pipeline stages.
Result<GlbWriteResult> write_glb(const SplatData& data, khr::ColorSpace color_space,
                                 const OperationContext& context);

}  // namespace melkor::format::gltf

#endif  // MELKOR_FORMAT_GLTF_WRITER_HPP
