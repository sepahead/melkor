#pragma once

#include "melkor/gaussian_data.hpp"
#include "melkor/limits.hpp"
#include <string>
#include <vector>

namespace melkor {

struct SpzDecodeMetadata {
    size_t declared_points = 0;
    size_t decoded_points = 0;
    int sh_degree = 0;
    bool antialiased = false;
};

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

    using Metadata = SpzDecodeMetadata;
    
    struct DecodeResult {
        bool success = false;
        std::string error_message;
        GaussianCloud cloud;
        Metadata metadata;
    };
    
    // Decode from file. `limits` bounds the compressed input size charged against a Budget before
    // the decode. (The decoded allocation happens inside vendored upstream after a whole-stream
    // inflate; bounding that fully needs a header peek and is tracked with the SPZ v4 upgrade.)
    DecodeResult decodeFromFile(const std::string& filepath,
                                const Limits& limits = Limits::for_profile(LimitsProfile::desktop));

    // Decode from memory buffer (see decodeFromFile for `limits`).
    DecodeResult decodeFromBuffer(const uint8_t* data, size_t size,
                                  const Limits& limits = Limits::for_profile(LimitsProfile::desktop));
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

    using Metadata = SpzDecodeMetadata;
    
    struct DecodeResult {
        bool success = false;
        std::string error_message = "SPZ support not compiled";
        GaussianCloud cloud;
        Metadata metadata;
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
