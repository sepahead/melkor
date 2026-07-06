#pragma once

#include "melkor/gaussian_data.hpp"
#include <string>
#include <vector>

namespace melkor {

#ifdef MELKOR_HAS_SPZ

// SPZ encoding configuration
struct SpzEncodeConfig {
    // Spherical harmonics degree to encode (0-3)
    int sh_degree = 0;

    // Note: position quantization precision (fractional bits) is fixed by
    // the spz container format writer (12 bits) and is not configurable —
    // spz::PackOptions exposes no such knob, so no field is offered here.
};

// SPZ encoding result
struct SpzEncodeResult {
    bool success = false;
    std::string error_message;
    size_t bytes_written = 0;
};

// SPZ encoder using nianticlabs/spz library
class SpzEncoder {
public:
    SpzEncoder();
    ~SpzEncoder();
    
    // Encode to file
    SpzEncodeResult encodeToFile(const std::string& filepath,
                                 const GaussianCloud& cloud,
                                 const SpzEncodeConfig& config = {});
    
    // Encode to memory buffer
    SpzEncodeResult encodeToBuffer(std::vector<uint8_t>& buffer,
                                   const GaussianCloud& cloud,
                                   const SpzEncodeConfig& config = {});
};

// SPZ decoder
class SpzDecoder {
public:
    SpzDecoder();
    ~SpzDecoder();
    
    struct DecodeResult {
        bool success = false;
        std::string error_message;
        GaussianCloud cloud;
    };
    
    // Decode from file
    DecodeResult decodeFromFile(const std::string& filepath);
    
    // Decode from memory buffer
    DecodeResult decodeFromBuffer(const uint8_t* data, size_t size);
};

#else

// Stub implementations when SPZ is not available
class SpzEncoder {
public:
    SpzEncoder() = default;
    ~SpzEncoder() = default;
    
    struct SpzEncodeResult {
        bool success = false;
        std::string error_message = "SPZ support not compiled";
        size_t bytes_written = 0;
    };
    
    template<typename... Args>
    SpzEncodeResult encodeToFile(Args&&...) { return {}; }
    
    template<typename... Args>
    SpzEncodeResult encodeToBuffer(Args&&...) { return {}; }
};

class SpzDecoder {
public:
    SpzDecoder() = default;
    ~SpzDecoder() = default;
    
    struct DecodeResult {
        bool success = false;
        std::string error_message = "SPZ support not compiled";
        GaussianCloud cloud;
    };
    
    template<typename... Args>
    DecodeResult decodeFromFile(Args&&...) { return {}; }
    
    template<typename... Args>
    DecodeResult decodeFromBuffer(Args&&...) { return {}; }
};

#endif // MELKOR_HAS_SPZ

// Check if SPZ support is available at runtime
inline bool isSpzAvailable() {
#ifdef MELKOR_HAS_SPZ
    return true;
#else
    return false;
#endif
}

} // namespace melkor
