#pragma once

#include "melkor/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace melkor {

// Configuration for GLB to Gaussian conversion
struct GlbConversionConfig {
    // Default scale for splats when not specified
    float default_scale = 0.01f;

    // Default opacity (0-1)
    float default_opacity = 1.0f;

    // Whether to use vertex colors if available
    bool use_vertex_colors = true;

    // Default color if no vertex colors (RGB 0-1)
    float default_color[3] = {0.5f, 0.5f, 0.5f};

    // Coordinate system transformation
    // GLB uses right-handed Y-up, 3DGS PLY typically uses right-handed Z-up
    bool convert_coordinate_system = true;

    // Scale factor for positions
    float position_scale = 1.0f;
};

// Result of GLB loading
struct GlbLoadResult {
    bool success = false;
    std::string error_message;
    std::optional<SplatData> data;

    // Statistics
    size_t total_vertices = 0;
    size_t total_meshes = 0;
    size_t total_primitives = 0;
};

// GLB/GLTF file reader
class GlbReader {
public:
    GlbReader();
    ~GlbReader();

    // Load from file path
    GlbLoadResult loadFromFile(const std::string& filepath, const GlbConversionConfig& config = {});

    // Load from memory
    GlbLoadResult loadFromMemory(const uint8_t* data, size_t size,
                                 const GlbConversionConfig& config = {});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace melkor
