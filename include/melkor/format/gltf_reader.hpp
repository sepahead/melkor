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
// silently corrupt color, so it is done here in one place and pinned by tests.

#ifndef MELKOR_FORMAT_GLTF_READER_HPP
#define MELKOR_FORMAT_GLTF_READER_HPP

#include "melkor/budget.hpp"
#include "melkor/error.hpp"
#include "melkor/format/gltf_document.hpp"
#include "melkor/format/gltf_khr.hpp"
#include "melkor/format/gltf_resolve.hpp"
#include "melkor/format/loss.hpp"
#include "melkor/limits.hpp"
#include "melkor/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace melkor::format::gltf {

// Select the expected file encoding. Use automatic only when no container decision exists.
enum class FileEncoding : std::uint8_t {
    automatic = 0,
    json = 1,
    binary_glb = 2,
};

// The result contains the local-space splats, the declared color space, and the source SH degree.
struct PrimitiveRead {
private:
    // Later members are destroyed first. Release the charge after the data is destroyed.
    Budget::Charge retained_memory_;

public:
    PrimitiveRead(SplatData data_value, khr::ColorSpace color_space_value,
                  std::uint32_t source_degree_value, Budget::Charge retained_memory) noexcept
        : retained_memory_(std::move(retained_memory)), data(std::move(data_value)),
          color_space(color_space_value), source_sh_degree(source_degree_value) {}

    PrimitiveRead(const PrimitiveRead&) = delete;
    PrimitiveRead& operator=(const PrimitiveRead&) = delete;
    PrimitiveRead(PrimitiveRead&&) noexcept = default;
    PrimitiveRead& operator=(PrimitiveRead&& other) noexcept {
        if (this == &other)
            return *this;
        data = std::move(other.data);
        color_space = other.color_space;
        source_sh_degree = other.source_sh_degree;
        retained_memory_ = std::move(other.retained_memory_);
        return *this;
    }

    std::uint64_t retained_memory_bytes() const noexcept { return retained_memory_.amount(); }

    SplatData data;
    khr::ColorSpace color_space = khr::ColorSpace::srgb_rec709_display;
    std::uint32_t source_sh_degree = 0;
};

// Reads one KHR_gaussian_splatting primitive into a local-space SplatData. Validates that the
// primitive is POINTS mode with the `ellipse` kernel, that the required attributes (POSITION,
// ROTATION, SCALE, OPACITY, and the degree-0 SH coefficient) are present with the expected element
// types and a consistent splat count, and that the spherical-harmonic degrees present are complete
// and contiguous. Fails cleanly on any structural violation; the resulting SplatData is fully
// validated by SplatData::create (finite, nonnegative scale, [0,1] opacity, unit quaternion).
//
// `prim` must be a primitive whose `gaussian` extension is set; `buffers` supplies the bytes of
// each glTF buffer (the GLB BIN chunk for buffer 0). `budget` bounds resource use: the splat count
// and the spherical-harmonic allocation are charged against it before they are allocated, so a
// primitive declaring an enormous count fails before it can exhaust memory.
Result<PrimitiveRead> read_primitive_local(const Document& doc, const PrimitiveDesc& prim,
                                           const std::vector<BufferSpan>& buffers, Budget& budget);

// Use one context across the complete read pipeline.
Result<PrimitiveRead> read_primitive_local(const Document& doc, const PrimitiveDesc& prim,
                                           const std::vector<BufferSpan>& buffers,
                                           const OperationContext& context);

// The result of reading a whole glTF scene into one merged splat cloud: the world-space splats and
// the loss report describing what the conversion could not fully preserve.
struct SceneRead {
private:
    // Later members are destroyed first. Release the charge after the data is destroyed.
    Budget::Charge retained_memory_;

public:
    SceneRead(SplatData data_value, LossReport loss_value, khr::ColorSpace color_space_value,
              std::uint32_t degree_value, Budget::Charge retained_memory) noexcept
        : retained_memory_(std::move(retained_memory)), data(std::move(data_value)),
          losses(std::move(loss_value)), color_space(color_space_value), sh_degree(degree_value) {}

    SceneRead(const SceneRead&) = delete;
    SceneRead& operator=(const SceneRead&) = delete;
    SceneRead(SceneRead&&) noexcept = default;
    SceneRead& operator=(SceneRead&& other) noexcept {
        if (this == &other)
            return *this;
        data = std::move(other.data);
        losses = std::move(other.losses);
        color_space = other.color_space;
        sh_degree = other.sh_degree;
        retained_memory_ = std::move(other.retained_memory_);
        return *this;
    }

    std::uint64_t retained_memory_bytes() const noexcept { return retained_memory_.amount(); }
    Budget::Charge take_retained_memory() noexcept { return std::move(retained_memory_); }

    SplatData data;
    LossReport losses;
    khr::ColorSpace color_space = khr::ColorSpace::srgb_rec709_display;
    std::uint32_t sh_degree = 0;
    std::uint64_t source_bytes = 0;
};

// Reads every KHR_gaussian_splatting primitive reachable from the default scene, applies each
// instantiating node's global transform to the geometry (mean via `μ' = Mμ + t`, covariance via
// `Σ' = M Σ Mᵀ` through the math oracle), and merges the results into one world-space SplatData.
//
// The reader rejects cycles and nodes with multiple parents. It also enforces the configured scene
// depth. Primitives of differing SH degree are padded to the maximum with zeros.
//
// The reader reports each item that it cannot preserve. It rejects an unsupported required
// extension. It rotates degree 0-3 SH for each proper node rotation. It rejects transforms whose
// KHR rendering is undefined. It reports color-space conflicts and hierarchy flattening. The
// caller applies the loss policy. The reader rejects a color space outside the selected profile.
//
// `limits` bounds resource use. Because a mesh shared by many nodes is instantiated once per node,
// a small file can otherwise describe an enormous splat cloud; the walk charges each node and each
// primitive's splats/memory against the limits and fails fast when they are exceeded.
Result<SceneRead>
read_gaussian_scene(const Document& doc, const std::vector<BufferSpan>& buffers,
                    const Limits& limits = Limits::for_profile(LimitsProfile::desktop));

Result<SceneRead> read_gaussian_scene(const Document& doc, const std::vector<BufferSpan>& buffers,
                                      const OperationContext& context);

// Reads a complete GLB file: validates the container framing, parses the JSON chunk into a
// document, feeds the binary chunk to the scene reader as glTF buffer 0, and returns the merged
// splats and loss report. This is the top-level entry point for a `.glb` on disk or in memory.
//
// Only the embedded binary buffer (buffer 0) is available; a bufferView that references any other
// buffer -- an external or data-URI buffer -- resolves to a clean error, since this reader does not
// fetch external resources. `limits` bounds resource use (see read_gaussian_scene).
Result<SceneRead> read_glb(const std::uint8_t* data, std::size_t size,
                           const Limits& limits = Limits::for_profile(LimitsProfile::desktop));

Result<SceneRead> read_glb(const std::uint8_t* data, std::size_t size,
                           const OperationContext& context);

// Read a GLB or JSON glTF asset from a local file. This overload resolves only data URIs and
// relative local buffer URIs. It rejects network, absolute, escaping, and symbolic-link paths.
Result<SceneRead> read_file(const std::filesystem::path& path,
                            const Limits& limits = Limits::for_profile(LimitsProfile::desktop));

Result<SceneRead> read_file(const std::filesystem::path& path, const OperationContext& context);

Result<SceneRead> read_file(const std::filesystem::path& path, FileEncoding encoding,
                            const Limits& limits = Limits::for_profile(LimitsProfile::desktop));

Result<SceneRead> read_file(const std::filesystem::path& path, FileEncoding encoding,
                            const OperationContext& context);

}  // namespace melkor::format::gltf

#endif  // MELKOR_FORMAT_GLTF_READER_HPP
