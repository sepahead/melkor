// SPZ Encoder/Decoder Implementation
// Bridges melkor::GaussianCloud to spz::GaussianCloud

#include "melkor/spz_encoder.hpp"

#ifdef MELKOR_HAS_SPZ

#include "melkor/cloud_inspector.hpp"
#include "load-spz.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>

namespace melkor {

// ============================================================================
// Helper functions to convert between melkor and spz types
// ============================================================================

static size_t shRestCountForDegree(int degree) {
    switch (degree) {
        case 0: return 0;
        case 1: return 9;
        case 2: return 24;
        case 3: return 45;
        default: return 0;
    }
}

static std::string validateEncodeInput(const GaussianCloud& cloud,
                                       const SpzEncodeConfig& config) {
    if (cloud.empty()) return "Cannot encode empty cloud";
    if (config.sh_degree < 0 || config.sh_degree > 3) {
        return "Cannot encode SPZ: requested SH degree must be between 0 and 3";
    }
    if (cloud.shDegree() < 0 || cloud.shDegree() > 3) {
        return "Cannot encode SPZ: cloud SH degree must be between 0 and 3";
    }
    const int effective_degree = std::min(config.sh_degree, cloud.shDegree());
    const size_t sh_components = shRestCountForDegree(effective_degree);
    const size_t largest_signed_multiplier = std::max<size_t>({9, 4, sh_components});
    const size_t signed_allocation_limit =
        static_cast<size_t>(std::numeric_limits<int32_t>::max()) /
        largest_signed_multiplier;
    if (cloud.size() > signed_allocation_limit) {
        return "Cannot encode SPZ: point count overflows an upstream signed allocation";
    }
    // The canonical decoder rejects larger clouds, even when an individual
    // encoder-side allocation expression would still fit in int32.
    constexpr size_t kMaxSpzPoints = 10'000'000;
    if (cloud.size() > kMaxSpzPoints) {
        return "Cannot encode SPZ: cloud exceeds the 10-million-point format limit";
    }

    const CloudInspection inspection = inspectCloud(cloud);
    if (!inspection.valid) {
        const auto issue = std::find_if(
            inspection.issues.begin(), inspection.issues.end(),
            [](const InspectionIssue& candidate) {
                return candidate.severity == InspectionSeverity::Error;
            });
        std::string message = "Cannot encode invalid cloud";
        if (issue != inspection.issues.end()) {
            message += " [" + issue->code + "]: " + issue->message;
            if (issue->has_index) {
                message += " First splat: " + std::to_string(issue->first_index) + ".";
            }
        }
        return message;
    }

    // SPZ stores positions as signed 24-bit fixed point with 12 fractional
    // bits. Values outside this interval would either overflow the upstream
    // float-to-int cast or silently wrap when only its low 24 bits are stored.
    constexpr double kPositionScale = 4096.0;
    constexpr double kMinFixedPosition = -8388608.0;
    constexpr double kMaxFixedPosition = 8388607.0;
    const float min_int_float = static_cast<float>(std::numeric_limits<int32_t>::min());
    const float max_int_float = static_cast<float>(std::numeric_limits<int32_t>::max());
    for (size_t index = 0; index < cloud.size(); ++index) {
        const GaussianSplat& splat = cloud[index];
        for (const float position : {splat.x, splat.y, splat.z}) {
            const double fixed = std::round(static_cast<double>(position) * kPositionScale);
            if (fixed < kMinFixedPosition || fixed > kMaxFixedPosition) {
                return "Cannot encode SPZ: position at splat " + std::to_string(index) +
                       " exceeds the signed 24-bit fixed-point range";
            }
        }
        for (const float coefficient : splat.sh_rest) {
            // Mirror the upstream float expression, but test its range before
            // the cast. float(INT32_MAX) rounds to 2^31, so that upper bound is
            // exclusive; INT32_MIN is exactly representable.
            const float quantized = std::round(coefficient * 128.0f) + 128.0f;
            if (!std::isfinite(quantized) || quantized < min_int_float ||
                quantized >= max_int_float) {
                return "Cannot encode SPZ: SH coefficient at splat " +
                       std::to_string(index) + " exceeds the safe quantization range";
            }
        }
    }
    return {};
}

static spz::GaussianCloud toSpzCloud(const GaussianCloud& cloud, int sh_degree) {
    spz::GaussianCloud spz_cloud;
    spz_cloud.numPoints = static_cast<int32_t>(cloud.size());
    // Clamp the encoded SH degree to both the config and what the cloud actually
    // carries, so requesting a lower degree truncates SH rest (smaller file)
    // and requesting a higher degree than present doesn't pad with garbage.
    int effective_degree = std::min(sh_degree, cloud.shDegree());
    spz_cloud.shDegree = effective_degree;
    spz_cloud.antialiased = false;
    
    // Reserve space
    spz_cloud.positions.reserve(cloud.size() * 3);
    spz_cloud.scales.reserve(cloud.size() * 3);
    spz_cloud.rotations.reserve(cloud.size() * 4);
    spz_cloud.alphas.reserve(cloud.size());
    spz_cloud.colors.reserve(cloud.size() * 3);
    
    // Number of SH-rest coefficients per splat for the effective degree.
    int sh_rest_count = 0;
    switch (effective_degree) {
        case 1: sh_rest_count = 9; break;
        case 2: sh_rest_count = 24; break;
        case 3: sh_rest_count = 45; break;
        default: sh_rest_count = 0; break;
    }
    
    // Convert each splat
    for (size_t i = 0; i < cloud.size(); ++i) {
        const auto& splat = cloud[i];
        
        // Position (x, y, z)
        spz_cloud.positions.push_back(splat.x);
        spz_cloud.positions.push_back(splat.y);
        spz_cloud.positions.push_back(splat.z);
        
        // Scale (log space)
        spz_cloud.scales.push_back(splat.scale_0);
        spz_cloud.scales.push_back(splat.scale_1);
        spz_cloud.scales.push_back(splat.scale_2);
        
        // Rotation quaternion. SPZ's GaussianCloud stores quaternions as
        // (x, y, z, w) -- scalar last (see spz splat-types.h: "xyzw quaternion"
        // and UnpackedGaussian.rotation = {x, y, z, w}; packQuaternionSmallestThree
        // only ever flips the x/y/z components, never w). Melkor stores rotations
        // as (rot_0=w, rot_1=x, rot_2=y, rot_3=z), so we must reorder here.
        float w = splat.rot_0;
        float x = splat.rot_1;
        float y = splat.rot_2;
        float z = splat.rot_3;
        // The upstream normalizer squares float components directly and can
        // overflow for a finite, non-unit quaternion. Melkor's implementation
        // scales by the largest component first, so normalize safely here.
        utils::normalizeQuaternion(w, x, y, z);
        spz_cloud.rotations.push_back(x);
        spz_cloud.rotations.push_back(y);
        spz_cloud.rotations.push_back(z);
        spz_cloud.rotations.push_back(w);
        
        // Alpha (logit space)
        spz_cloud.alphas.push_back(splat.opacity);
        
        // Color (SH DC coefficients)
        spz_cloud.colors.push_back(splat.f_dc_0);
        spz_cloud.colors.push_back(splat.f_dc_1);
        spz_cloud.colors.push_back(splat.f_dc_2);
        
        // SH rest. Melkor stores sh_rest in the 3DGS PLY convention
        // (channel-major: all R coefficients, then all G, then all B), while
        // SPZ interleaves the channel as the fastest axis (c0r c0g c0b c1r
        // ... — see splat-types.h). Transpose while copying so external SPZ
        // consumers read each coefficient in the channel it came from.
        const int num_coeffs = sh_rest_count / 3;
        const int source_num_coeffs =
            static_cast<int>(shRestCountForDegree(cloud.shDegree()) / 3);
        for (int j = 0; j < num_coeffs; ++j) {
            for (int ch = 0; ch < 3; ++ch) {
                const int src = ch * source_num_coeffs + j;
                spz_cloud.sh.push_back(splat.sh_rest[static_cast<size_t>(src)]);
            }
        }
    }
    
    return spz_cloud;
}

static std::string validateSpzCloudLayout(const spz::GaussianCloud& cloud) {
    if (cloud.numPoints < 0) return "SPZ declares a negative point count";
    if (cloud.shDegree < 0 || cloud.shDegree > 3) return "SPZ declares an unsupported SH degree";
    const size_t count = static_cast<size_t>(cloud.numPoints);
    const auto shorterThan = [&](size_t actual, size_t components) {
        return components != 0 &&
            (count > std::numeric_limits<size_t>::max() / components ||
             actual < count * components);
    };
    if (shorterThan(cloud.positions.size(), 3) || shorterThan(cloud.scales.size(), 3) ||
        shorterThan(cloud.rotations.size(), 4) || cloud.alphas.size() < count ||
        shorterThan(cloud.colors.size(), 3) ||
        shorterThan(cloud.sh.size(), shRestCountForDegree(cloud.shDegree))) {
        return "SPZ decoded arrays are shorter than the declared point count";
    }
    return {};
}

static GaussianCloud fromSpzCloud(const spz::GaussianCloud& spz_cloud) {
    GaussianCloud cloud;
    cloud.setShDegree(spz_cloud.shDegree);
    cloud.reserve(spz_cloud.numPoints);
    
    // Layout is validated before this conversion. Never silently truncate a
    // declared cloud: that would make a damaged SPZ appear valid.
    const size_t np = static_cast<size_t>(spz_cloud.numPoints);
    
    for (size_t i = 0; i < np; ++i) {
        GaussianSplat splat;
        
        // Position
        splat.x = spz_cloud.positions[i * 3 + 0];
        splat.y = spz_cloud.positions[i * 3 + 1];
        splat.z = spz_cloud.positions[i * 3 + 2];
        
        // Scale
        splat.scale_0 = spz_cloud.scales[i * 3 + 0];
        splat.scale_1 = spz_cloud.scales[i * 3 + 1];
        splat.scale_2 = spz_cloud.scales[i * 3 + 2];
        
        // Rotation. SPZ provides quaternions as (x, y, z, w) -- scalar last;
        // convert to Melkor's (w, x, y, z) layout. See toSpzCloud for details.
        splat.rot_0 = spz_cloud.rotations[i * 4 + 3];  // w
        splat.rot_1 = spz_cloud.rotations[i * 4 + 0];  // x
        splat.rot_2 = spz_cloud.rotations[i * 4 + 1];  // y
        splat.rot_3 = spz_cloud.rotations[i * 4 + 2];  // z
        
        // Alpha
        splat.opacity = spz_cloud.alphas[i];
        
        // Color
        splat.f_dc_0 = spz_cloud.colors[i * 3 + 0];
        splat.f_dc_1 = spz_cloud.colors[i * 3 + 1];
        splat.f_dc_2 = spz_cloud.colors[i * 3 + 2];
        
        // Higher order SH. SPZ stores the channel as the fastest axis
        // (c0r c0g c0b ...); transpose back into Melkor's channel-major
        // sh_rest layout (all R, then all G, then all B) — the inverse of
        // the transpose in toSpzCloud.
        if (spz_cloud.shDegree > 0 && !spz_cloud.sh.empty()) {
            size_t sh_per_point = 0;
            switch (spz_cloud.shDegree) {
                case 1: sh_per_point = 9; break;
                case 2: sh_per_point = 24; break;
                case 3: sh_per_point = 45; break;
            }
            const size_t num_coeffs = sh_per_point / 3;
            const size_t sh_start = i * sh_per_point;
            splat.sh_rest.assign(sh_per_point, 0.0f);
            for (size_t j = 0; j < num_coeffs; ++j) {
                for (size_t ch = 0; ch < 3; ++ch) {
                    const size_t src = sh_start + j * 3 + ch;
                    if (src < spz_cloud.sh.size()) {
                        splat.sh_rest[ch * num_coeffs + j] = spz_cloud.sh[src];
                    }
                }
            }
        }
        
        cloud.addSplat(std::move(splat));
    }
    
    return cloud;
}

// ============================================================================
// SpzEncoder Implementation
// ============================================================================

SpzEncoder::SpzEncoder() = default;
SpzEncoder::~SpzEncoder() = default;

SpzEncodeResult SpzEncoder::encodeToFile(const std::string& filepath,
                                          const GaussianCloud& cloud,
                                          const SpzEncodeConfig& config) {
    std::vector<uint8_t> buffer;
    SpzEncodeResult encoded = encodeToBuffer(buffer, cloud, config);
    if (!encoded.success) return encoded;

    SpzEncodeResult result;
    bool output_opened = false;
    try {
        // Encode fully before opening the destination. Validation/allocation
        // failures therefore preserve an existing file; an I/O failure after
        // truncation removes the partial replacement below.
        std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            result.error_message = "Failed to open SPZ file for writing";
            return result;
        }
        output_opened = true;
        file.write(reinterpret_cast<const char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        file.close();
        if (!file.good()) {
            std::remove(filepath.c_str());
            result.error_message = "Failed to write complete SPZ file";
            return result;
        }
        result.success = true;
        result.bytes_written = buffer.size();
    } catch (const std::bad_alloc&) {
        if (output_opened) std::remove(filepath.c_str());
        result.error_message = "SPZ file output exceeded available memory";
    } catch (const std::length_error&) {
        if (output_opened) std::remove(filepath.c_str());
        result.error_message = "SPZ file output exceeded a container size limit";
    } catch (const std::exception& error) {
        if (output_opened) std::remove(filepath.c_str());
        result.error_message = std::string("Failed to write SPZ: ") + error.what();
    }

    return result;
}

SpzEncodeResult SpzEncoder::encodeToBuffer(std::vector<uint8_t>& buffer,
                                            const GaussianCloud& cloud,
                                            const SpzEncodeConfig& config) {
    SpzEncodeResult result;
    buffer.clear();

    if (const std::string validation_error = validateEncodeInput(cloud, config);
        !validation_error.empty()) {
        result.error_message = validation_error;
        return result;
    }

    try {
        // Convert to SPZ cloud
        spz::GaussianCloud spz_cloud = toSpzCloud(cloud, config.sh_degree);

        // Set up pack options
        spz::PackOptions options;
        options.from = spz::CoordinateSystem::RDF;  // PLY coordinate system

        // Save to buffer
        if (spz::saveSpz(spz_cloud, options, &buffer)) {
            result.success = true;
            result.bytes_written = buffer.size();
        } else {
            buffer.clear();
            result.error_message = "Failed to encode SPZ data";
        }
    } catch (const std::bad_alloc&) {
        buffer.clear();
        result.error_message = "SPZ encoding exceeded available memory";
    } catch (const std::length_error&) {
        buffer.clear();
        result.error_message = "SPZ encoding exceeded a container size limit";
    } catch (const std::exception& error) {
        buffer.clear();
        result.error_message = std::string("Failed to encode SPZ: ") + error.what();
    }

    return result;
}

// ============================================================================
// SpzDecoder Implementation
// ============================================================================

SpzDecoder::SpzDecoder() = default;
SpzDecoder::~SpzDecoder() = default;

SpzDecoder::DecodeResult SpzDecoder::decodeFromFile(const std::string& filepath) {
    DecodeResult result;
    std::ifstream stream(filepath, std::ios::binary | std::ios::ate);
    if (!stream) {
        result.error_message = "Failed to open SPZ file";
        return result;
    }
    const std::streamoff end = stream.tellg();
    if (end <= 0 || static_cast<uintmax_t>(end) >
                        static_cast<uintmax_t>(std::numeric_limits<int32_t>::max())) {
        result.error_message = "SPZ file is empty or exceeds the 2 GiB decoder limit";
        return result;
    }
    std::vector<uint8_t> data;
    try {
        data.resize(static_cast<size_t>(end));
    } catch (const std::bad_alloc&) {
        result.error_message = "SPZ file exceeds available memory";
        return result;
    }
    stream.seekg(0, std::ios::beg);
    if (!stream.read(reinterpret_cast<char*>(data.data()), end)) {
        result.error_message = "Failed to read complete SPZ file";
        return result;
    }
    return decodeFromBuffer(data.data(), data.size());
}

SpzDecoder::DecodeResult SpzDecoder::decodeFromBuffer(const uint8_t* data, size_t size) {
    DecodeResult result;
    if (data == nullptr || size == 0 ||
        size > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        result.error_message = "SPZ input buffer is null, empty, or exceeds the 2 GiB decoder limit";
        return result;
    }
    const int32_t version = spz::getSpzVersion(data, static_cast<int32_t>(size));
    if (version != 0 && (version < 1 || version > 3)) {
        result.error_message = "Unsupported SPZ version v" + std::to_string(version) +
                               "; this build supports SPZ v1-v3";
        return result;
    }
    
    // Set up unpack options
    spz::UnpackOptions options;
    options.to = spz::CoordinateSystem::RDF;  // PLY coordinate system
    
    try {
        spz::GaussianCloud spz_cloud = spz::loadSpz(data, static_cast<int32_t>(size), options);
        
        if (spz_cloud.numPoints == 0) {
            result.error_message = "SPZ data contains no points";
            return result;
        }

        result.metadata.declared_points = static_cast<size_t>(spz_cloud.numPoints);
        result.metadata.sh_degree = spz_cloud.shDegree;
        result.metadata.antialiased = spz_cloud.antialiased;
        if (const std::string layout_error = validateSpzCloudLayout(spz_cloud);
            !layout_error.empty()) {
            result.error_message = layout_error;
            return result;
        }
        
        result.cloud = fromSpzCloud(spz_cloud);
        result.metadata.decoded_points = result.cloud.size();
        result.success = true;
    } catch (const std::exception& e) {
        result.error_message = std::string("Failed to decode SPZ: ") + e.what();
    }
    
    return result;
}

} // namespace melkor

#endif // MELKOR_HAS_SPZ
