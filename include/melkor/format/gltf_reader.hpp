// glTF KHR_gaussian_splatting reader.
//
// This assembles the modules below it -- the JSON document model, accessor resolution and
// decoding, the KHR layout core, and the scene model -- into Gaussian splats. This header exposes
// the per-primitive step: reading one `KHR_gaussian_splatting` primitive's attributes into a
// canonical `SplatData` in the primitive's own local space (no node transform applied yet). The
// scene-graph walk that composes node transforms and merges primitives is layered on top of this.
//
// KHR stores splats in exactly Melkor's canonical domains already -- linear scale, linear opacity
// in [0,1], a unit quaternion in x,y,z,w order, and the SH DC term as a coefficient -- so this step
// is a structural remap, not a semantic conversion. The one non-trivial remap is the spherical
// harmonics: KHR stores one accessor per coefficient (each accessor is all splats' RGB for that one
// coefficient), which is coefficient-major across splats, and it must be transposed into the scene
// model's splat-major per-splat blocks. That transpose is the easiest place in the whole reader to
// silently corrupt colour, so it is done here in one place and pinned by tests.

#ifndef MELKOR_FORMAT_GLTF_READER_HPP
#define MELKOR_FORMAT_GLTF_READER_HPP

#include "melkor/error.hpp"
#include "melkor/format/gltf_document.hpp"
#include "melkor/format/gltf_khr.hpp"
#include "melkor/format/gltf_resolve.hpp"
#include "melkor/scene.hpp"

#include <cstdint>
#include <vector>

namespace melkor::format::gltf {

// The result of reading one splat primitive: the local-space splats, the declared colour space, and
// the SH degree that was actually present in the source. `color_space_assumed` is true when the
// primitive's `colorSpace` string was not one Melkor recognises and sRGB was assumed -- the caller
// turns that into a LOSS_COLOR_SPACE_ASSUMED entry rather than the reader silently guessing.
struct PrimitiveRead {
    SplatData data;
    khr::ColorSpace color_space = khr::ColorSpace::srgb_rec709_display;
    bool color_space_assumed = false;
    std::uint32_t source_sh_degree = 0;
};

// Reads one KHR_gaussian_splatting primitive into a local-space SplatData. Validates that the
// primitive is POINTS mode with the `ellipse` kernel, that the required attributes (POSITION,
// ROTATION, SCALE, OPACITY, and the degree-0 SH coefficient) are present with the expected element
// types and a consistent splat count, and that the spherical-harmonic degrees present are complete
// and contiguous. Fails cleanly on any structural violation; the resulting SplatData is fully
// validated by SplatData::create (finite, positive scale, [0,1] opacity, unit quaternion).
//
// `prim` must be a primitive whose `gaussian` extension is set; `buffers` supplies the bytes of each
// glTF buffer (the GLB BIN chunk for buffer 0).
Result<PrimitiveRead> read_primitive_local(const Document& doc, const PrimitiveDesc& prim,
                                           const std::vector<BufferSpan>& buffers);

}  // namespace melkor::format::gltf

#endif  // MELKOR_FORMAT_GLTF_READER_HPP
