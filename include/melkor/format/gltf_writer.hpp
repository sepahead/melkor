// glTF KHR_gaussian_splatting writer.
//
// Serialises a canonical `SplatData` into a GLB carrying one `KHR_gaussian_splatting` POINTS
// primitive. It is the inverse of the reader: the same canonical domains (linear scale/opacity, unit
// quaternion x,y,z,w, SH DC as a coefficient) go straight onto the wire, and the spherical harmonics
// are transposed the other way -- from the scene model's splat-major per-splat blocks back into one
// float accessor per coefficient (coefficient-major across splats), which is the layout KHR mandates.
//
// The pinned RC profile supports SH degree 0-3. A degree-4 source is written at degree 3 with a
// reported `LOSS_SH_DEGREE_TRUNCATED`; the writer returns that loss in the report rather than
// silently dropping the coefficients, so the caller applies the loss policy before treating the
// output as faithful.

#ifndef MELKOR_FORMAT_GLTF_WRITER_HPP
#define MELKOR_FORMAT_GLTF_WRITER_HPP

#include "melkor/error.hpp"
#include "melkor/format/gltf_khr.hpp"
#include "melkor/format/loss.hpp"
#include "melkor/scene.hpp"

#include <cstdint>
#include <vector>

namespace melkor::format::gltf {

struct GlbWriteResult {
    std::vector<std::uint8_t> bytes;  // the complete GLB
    LossReport losses;                // e.g. LOSS_SH_DEGREE_TRUNCATED for a degree-4 source
};

// Writes `data` as a GLB with one KHR_gaussian_splatting primitive in `color_space`. The single
// node is the identity, so the splats are written in the world frame they already occupy. Fails
// only on an internal size overflow (a scene too large for the GLB 32-bit length field); a lossy
// but valid write succeeds and reports the loss.
Result<GlbWriteResult> write_glb(const SplatData& data, khr::ColorSpace color_space);

}  // namespace melkor::format::gltf

#endif  // MELKOR_FORMAT_GLTF_WRITER_HPP
