#ifndef MELKOR_SPZ_ENCODER_HPP
#define MELKOR_SPZ_ENCODER_HPP

#include "melkor/budget.hpp"
#include "melkor/color_space.hpp"
#include "melkor/format/loss.hpp"
#include "melkor/format/profile.hpp"
#include "melkor/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace melkor {

struct SpzDecodeConfig {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);

    // SPZ does not store a length unit. Semantic decoding requires this scale.
    std::optional<double> source_unit_to_meter;

    // SPZ does not store a color space. Supply its out-of-band value when known.
    // The decoder leaves the result metadata empty when this value is absent.
    std::optional<ColorSpace> source_color_space;
};

struct SpzDecodeMetadata {
    std::uint64_t source_bytes = 0;
    size_t declared_points = 0;
    size_t decoded_points = 0;
    int sh_degree = 0;
    bool antialiased = false;
    FormatProfileId profile = FormatProfileId::spz_v1_v3;
    std::optional<ColorSpace> color_space;
};

struct SpzEncodeConfig {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    bool overwrite = false;

    // Use -1 to write the highest degree that SPZ v1 through v3 can store.
    int sh_degree = -1;

    std::optional<bool> antialiased;

    // SPZ stores coefficients but does not store their color-space identity.
    // The writer requires this value and reports its removal as a severe loss.
    std::optional<ColorSpace> color_space;

    // The writer emits no bytes when it finds an unapproved severe loss.
    std::vector<std::string> approved_loss_codes;
};

struct SpzEncodeResult {
private:
    // Keep the controlled output allocation charged until this result releases it.
    Budget::Charge retained_memory_;

public:
    SpzEncodeResult() = default;
    SpzEncodeResult(const SpzEncodeResult&) = delete;
    SpzEncodeResult& operator=(const SpzEncodeResult&) = delete;
    SpzEncodeResult(SpzEncodeResult&&) noexcept = default;
    SpzEncodeResult& operator=(SpzEncodeResult&&) noexcept = default;

    bool success = false;
    std::string error_message;
    std::uint64_t bytes_written = 0;
    std::vector<Diagnostic> diagnostics;
    ErrorCode failure_code = ErrorCode::invalid_data;
    FormatProfileId profile = FormatProfileId::spz_v1_v3;
    LossReport losses;

    // Keep this result alive while the controlled output buffer uses its allocation.
    std::uint64_t retained_memory_bytes() const noexcept { return retained_memory_.amount(); }
    Budget::Charge take_retained_memory() noexcept { return std::move(retained_memory_); }
    void set_retained_memory(Budget::Charge charge) noexcept {
        retained_memory_ = std::move(charge);
    }

    ErrorCode error_code() const noexcept { return success ? ErrorCode::ok : failure_code; }
};

class SpzEncoder {
public:
#ifdef MELKOR_HAS_SPZ
    SpzEncoder() = default;
    ~SpzEncoder() = default;

    SpzEncodeResult encodeToFile(const std::filesystem::path& filepath, const SplatData& data,
                                 const SpzEncodeConfig& config = {});
    SpzEncodeResult encodeToFile(const std::filesystem::path& filepath, const SplatData& data,
                                 const SpzEncodeConfig& config, const OperationContext& context);

    SpzEncodeResult encodeToBuffer(std::vector<uint8_t>& buffer, const SplatData& data,
                                   const SpzEncodeConfig& config = {});
    SpzEncodeResult encodeToBuffer(std::vector<uint8_t>& buffer, const SplatData& data,
                                   const SpzEncodeConfig& config, const OperationContext& context);

private:
    SpzEncodeResult encodeToBufferImpl(std::vector<uint8_t>& buffer, const SplatData& data,
                                       const SpzEncodeConfig& config,
                                       const OperationContext& context);
#else
    SpzEncoder() = default;
    ~SpzEncoder() = default;

    SpzEncodeResult encodeToFile(const std::filesystem::path&, const SplatData&,
                                 const SpzEncodeConfig& = {}) {
        return unavailable();
    }
    SpzEncodeResult encodeToFile(const std::filesystem::path&, const SplatData&,
                                 const SpzEncodeConfig&, const OperationContext&) {
        return unavailable();
    }
    SpzEncodeResult encodeToBuffer(std::vector<uint8_t>& buffer, const SplatData&,
                                   const SpzEncodeConfig& = {}) {
        buffer.clear();
        return unavailable();
    }
    SpzEncodeResult encodeToBuffer(std::vector<uint8_t>& buffer, const SplatData&,
                                   const SpzEncodeConfig&, const OperationContext&) {
        buffer.clear();
        return unavailable();
    }

private:
    static SpzEncodeResult unavailable() {
        SpzEncodeResult result;
        result.error_message = "SPZ support is not compiled";
        result.failure_code = ErrorCode::unsupported_feature;
        return result;
    }
#endif
};

class SpzDecoder {
public:
#ifdef MELKOR_HAS_SPZ
    SpzDecoder() = default;
    ~SpzDecoder() = default;
#else
    SpzDecoder() = default;
    ~SpzDecoder() = default;
#endif

    using Metadata = SpzDecodeMetadata;

    struct DecodeResult {
    private:
#ifdef MELKOR_HAS_SPZ
        // Later members are destroyed first. Release this charge after `data` is destroyed.
        Budget::Charge retained_memory_;
#endif

    public:
        DecodeResult() = default;
        DecodeResult(const DecodeResult&) = delete;
        DecodeResult& operator=(const DecodeResult&) = delete;
        DecodeResult(DecodeResult&&) noexcept = default;
        DecodeResult& operator=(DecodeResult&& other) noexcept {
            if (this == &other)
                return *this;
            success = other.success;
            error_message = std::move(other.error_message);
            data = std::move(other.data);
            metadata = other.metadata;
            diagnostics = std::move(other.diagnostics);
            failure_code = other.failure_code;
#ifdef MELKOR_HAS_SPZ
            retained_memory_ = std::move(other.retained_memory_);
#endif
            return *this;
        }

        std::uint64_t retained_memory_bytes() const noexcept {
#ifdef MELKOR_HAS_SPZ
            return retained_memory_.amount();
#else
            return 0;
#endif
        }
        Budget::Charge take_retained_memory() noexcept {
#ifdef MELKOR_HAS_SPZ
            return std::move(retained_memory_);
#else
            return {};
#endif
        }
        void set_retained_memory(Budget::Charge charge) noexcept {
#ifdef MELKOR_HAS_SPZ
            retained_memory_ = std::move(charge);
#else
            static_cast<void>(charge);
#endif
        }

        bool success = false;
        std::string error_message;
        std::optional<SplatData> data;
        Metadata metadata;
        std::vector<Diagnostic> diagnostics;
        ErrorCode failure_code = ErrorCode::invalid_data;

        ErrorCode error_code() const noexcept { return success ? ErrorCode::ok : failure_code; }
    };

#ifdef MELKOR_HAS_SPZ
    DecodeResult decodeFromFile(const std::filesystem::path& filepath,
                                const SpzDecodeConfig& config = {});
    DecodeResult decodeFromFile(const std::filesystem::path& filepath,
                                const SpzDecodeConfig& config, const OperationContext& context);

    DecodeResult decodeFromBuffer(const uint8_t* data, size_t size,
                                  const SpzDecodeConfig& config = {});
    DecodeResult decodeFromBuffer(const uint8_t* data, size_t size, const SpzDecodeConfig& config,
                                  const OperationContext& context);

private:
    DecodeResult decodeFromBufferImpl(const uint8_t* data, size_t size,
                                      const SpzDecodeConfig& config,
                                      const OperationContext& context, bool input_is_charged);
#else
    DecodeResult decodeFromFile(const std::filesystem::path&, const SpzDecodeConfig& = {}) {
        return unavailable();
    }
    DecodeResult decodeFromFile(const std::filesystem::path&, const SpzDecodeConfig&,
                                const OperationContext&) {
        return unavailable();
    }
    DecodeResult decodeFromBuffer(const uint8_t*, size_t, const SpzDecodeConfig& = {}) {
        return unavailable();
    }
    DecodeResult decodeFromBuffer(const uint8_t*, size_t, const SpzDecodeConfig&,
                                  const OperationContext&) {
        return unavailable();
    }

private:
    static DecodeResult unavailable() {
        DecodeResult result;
        result.error_message = "SPZ support is not compiled";
        result.failure_code = ErrorCode::unsupported_feature;
        return result;
    }
#endif
};

inline bool isSpzAvailable() noexcept {
#ifdef MELKOR_HAS_SPZ
    return true;
#else
    return false;
#endif
}

}  // namespace melkor

#endif  // MELKOR_SPZ_ENCODER_HPP
