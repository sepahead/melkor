#ifndef MELKOR_FORMATS_GLTF_USAGE_HPP
#define MELKOR_FORMATS_GLTF_USAGE_HPP

#include "melkor/budget.hpp"
#include "melkor/error.hpp"
#include "melkor/format/gltf_document.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace melkor::format::gltf {

// This plan identifies the document objects that the default Gaussian scene uses.
// The memory charge owns the marker tables while the plan exists.
struct DefaultSceneUsage {
    DefaultSceneUsage() = default;
    DefaultSceneUsage(const DefaultSceneUsage&) = delete;
    DefaultSceneUsage& operator=(const DefaultSceneUsage&) = delete;
    DefaultSceneUsage(DefaultSceneUsage&&) noexcept = default;
    DefaultSceneUsage& operator=(DefaultSceneUsage&& other) noexcept {
        if (this == &other)
            return *this;

        // Release the old marker storage before its budget charge.
        nodes = std::move(other.nodes);
        meshes = std::move(other.meshes);
        accessors = std::move(other.accessors);
        buffer_views = std::move(other.buffer_views);
        buffers = std::move(other.buffers);
        node_count = other.node_count;
        mesh_count = other.mesh_count;
        accessor_count = other.accessor_count;
        buffer_view_count = other.buffer_view_count;
        buffer_count = other.buffer_count;
        memory_charge = std::move(other.memory_charge);
        return *this;
    }

    Budget::Charge memory_charge;
    std::vector<std::uint8_t> nodes;
    std::vector<std::uint8_t> meshes;
    std::vector<std::uint8_t> accessors;
    std::vector<std::uint8_t> buffer_views;
    std::vector<std::uint8_t> buffers;
    std::uint64_t node_count = 0;
    std::uint64_t mesh_count = 0;
    std::uint64_t accessor_count = 0;
    std::uint64_t buffer_view_count = 0;
    std::uint64_t buffer_count = 0;
};

// Walk the default scene and mark only data that the Gaussian reader consumes.
Result<DefaultSceneUsage> collect_default_scene_usage(const Document& document,
                                                      const OperationContext& context);

}  // namespace melkor::format::gltf

#endif  // MELKOR_FORMATS_GLTF_USAGE_HPP
