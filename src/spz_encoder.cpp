// SPZ Encoder/Decoder Implementation
// Bridges canonical melkor::SplatData to spz::GaussianCloud.

#include "melkor/spz_encoder.hpp"

#ifdef MELKOR_HAS_SPZ

#include "melkor/budget.hpp"
#include "melkor/checked.hpp"
#include "melkor/io/atomic_writer.hpp"
#include "melkor/limits.hpp"
#include "melkor/math/activation.hpp"
#include "melkor/math/quaternion.hpp"

#include "io/input_file.hpp"

#include "load-spz.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <zlib.h>

namespace melkor {

// ============================================================================
// Helper functions to convert between melkor and spz types
// ============================================================================

constexpr float kSpzOpacityEpsilon = 1.0e-6f;
constexpr float kSpzMinimumScale = 4.5399929762484854e-5f;  // exp(-10)
constexpr float kSpzColorScale = 0.15f;
constexpr float kSpzSafeClampedDcMagnitude = 4.0f;
constexpr std::uint32_t kSpzMagic = 0x5053474e;
constexpr std::uint32_t kMaxUpstreamSpzPoints = 10'000'000;
constexpr std::size_t kSpzHeaderBytes = 16;
constexpr std::uint8_t kSpzAntialiasedFlag = 0x01;
constexpr std::uint64_t kSpzCodecOverheadBytes = 128U << 10;
constexpr std::size_t kControlInterval = 1024;

class ScopedBudgetRelease {
public:
    ScopedBudgetRelease() = default;
    ScopedBudgetRelease(Budget* budget, BudgetKind kind, std::uint64_t amount) noexcept
        : budget_(budget), kind_(kind), amount_(amount) {}

    ~ScopedBudgetRelease() {
        if (budget_ != nullptr)
            budget_->release(kind_, amount_);
    }

    ScopedBudgetRelease(const ScopedBudgetRelease&) = delete;
    ScopedBudgetRelease& operator=(const ScopedBudgetRelease&) = delete;

    ScopedBudgetRelease(ScopedBudgetRelease&& other) noexcept
        : budget_(std::exchange(other.budget_, nullptr)), kind_(other.kind_),
          amount_(std::exchange(other.amount_, 0)) {}

    ScopedBudgetRelease& operator=(ScopedBudgetRelease&&) = delete;

    void retain(std::uint64_t retained) noexcept {
        amount_ = retained >= amount_ ? 0 : amount_ - retained;
    }

private:
    Budget* budget_ = nullptr;
    BudgetKind kind_ = BudgetKind::memory_bytes;
    std::uint64_t amount_ = 0;
};

std::string firstDiagnosticMessage(const std::vector<Diagnostic>& diagnostics,
                                   const char* fallback) {
    return diagnostics.empty() ? std::string(fallback) : diagnostics.front().message;
}

void failEncode(SpzEncodeResult& result, ErrorCode code, std::string message,
                std::vector<Diagnostic> diagnostics = {}) {
    result.success = false;
    result.bytes_written = 0;
    result.set_retained_memory({});
    result.failure_code = code;
    result.error_message = std::move(message);
    if (!diagnostics.empty())
        result.diagnostics = std::move(diagnostics);
}

void failDecode(SpzDecoder::DecodeResult& result, ErrorCode code, std::string message,
                std::vector<Diagnostic> diagnostics = {}) {
    result.success = false;
    result.data.reset();
    result.metadata.decoded_points = 0;
    result.set_retained_memory({});
    result.failure_code = code;
    result.error_message = std::move(message);
    result.diagnostics = std::move(diagnostics);
}

Result<void> validateDecodeConfig(const SpzDecodeConfig& config) {
    if (!config.source_unit_to_meter.has_value()) {
        return Result<void>::failure(
            ErrorCode::invalid_argument,
            Diagnostic("MK1331_SPZ_UNIT_REQUIRED", Severity::error,
                       "SPZ input requires an explicit length-unit scale"));
    }
    if (!std::isfinite(*config.source_unit_to_meter) || *config.source_unit_to_meter <= 0.0) {
        return Result<void>::failure(
            ErrorCode::invalid_argument,
            Diagnostic("MK1332_SPZ_UNIT_INVALID", Severity::error,
                       "The SPZ length-unit scale must be finite and positive"));
    }
    if (config.source_color_space.has_value() && !is_valid(*config.source_color_space)) {
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     Diagnostic("MK1309_SPZ_COLOR_SPACE_INVALID", Severity::error,
                                                "The SPZ source color space is invalid"));
    }
    return Result<void>::success();
}

static size_t shRestCountForDegree(int degree);

struct SpzPackedHeader {
    std::uint32_t version = 0;
    std::uint32_t num_points = 0;
    std::uint8_t sh_degree = 0;
    std::uint8_t fractional_bits = 0;
    std::uint8_t flags = 0;
    std::uint8_t reserved = 0;
};

struct SpzDecodePlan {
    std::uint64_t packed_bytes = 0;
    std::uint64_t cloud_bytes = 0;
    std::uint64_t working_memory_bytes = 0;
};

struct SpzEncodePlan {
    std::uint64_t packed_bytes = 0;
    std::uint64_t output_bound = 0;
    std::uint64_t working_memory_bytes = 0;
};

static std::uint32_t readU32LittleEndian(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

static Result<SpzPackedHeader> probeSpzHeader(const std::uint8_t* data, std::size_t probe_size,
                                              bool input_was_capped) {
    std::uint8_t bytes[kSpzHeaderBytes] = {};
    z_stream stream = {};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    stream.avail_in = static_cast<uInt>(probe_size);
    stream.next_out = reinterpret_cast<Bytef*>(bytes);
    stream.avail_out = static_cast<uInt>(sizeof(bytes));

    const int initialized = inflateInit2(&stream, 16 + MAX_WBITS);
    if (initialized != Z_OK) {
        const ErrorCode code =
            initialized == Z_MEM_ERROR ? ErrorCode::resource_limit : ErrorCode::internal_error;
        Diagnostic diagnostic("MK1323_SPZ_HEADER_PROBE_FAILED", Severity::error,
                              "Could not initialize the bounded SPZ header probe");
        return Result<SpzPackedHeader>::failure(code, std::move(diagnostic));
    }

    int status = Z_OK;
    while (stream.avail_out != 0) {
        const uInt input_before = stream.avail_in;
        const uInt output_before = stream.avail_out;
        status = inflate(&stream, Z_NO_FLUSH);
        if (status == Z_STREAM_END || status != Z_OK)
            break;
        if (input_before == stream.avail_in && output_before == stream.avail_out) {
            status = Z_BUF_ERROR;
            break;
        }
    }
    const std::size_t produced = sizeof(bytes) - stream.avail_out;
    inflateEnd(&stream);

    if (produced != sizeof(bytes)) {
        const ErrorCode code =
            input_was_capped ? ErrorCode::resource_limit : ErrorCode::invalid_data;
        Diagnostic diagnostic(
            input_was_capped ? "MK1325_SPZ_HEADER_PROBE_LIMIT" : "MK1324_SPZ_HEADER_INVALID",
            Severity::error,
            input_was_capped ? "SPZ gzip header exceeds the bounded probe input limit"
                             : "SPZ input does not contain a complete gzip-packed header");
        diagnostic.with_context("zlib_status", static_cast<std::int64_t>(status));
        diagnostic.with_context("header_bytes", static_cast<std::uint64_t>(produced));
        diagnostic.with_context("probe_input_bytes", static_cast<std::uint64_t>(probe_size));
        return Result<SpzPackedHeader>::failure(code, std::move(diagnostic));
    }

    if (readU32LittleEndian(bytes) != kSpzMagic) {
        Diagnostic diagnostic("MK1324_SPZ_HEADER_INVALID", Severity::error,
                              "SPZ input has an invalid packed-header magic value");
        return Result<SpzPackedHeader>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }

    SpzPackedHeader header;
    header.version = readU32LittleEndian(bytes + 4);
    header.num_points = readU32LittleEndian(bytes + 8);
    header.sh_degree = bytes[12];
    header.fractional_bits = bytes[13];
    header.flags = bytes[14];
    header.reserved = bytes[15];
    return Result<SpzPackedHeader>::success(header);
}

static Result<void> validateSpzHeader(const SpzPackedHeader& header) {
    if (header.version < 1 || header.version > 3) {
        Diagnostic diagnostic("MK1327_SPZ_VERSION_UNSUPPORTED", Severity::error,
                              "The SPZ version is not supported");
        diagnostic.with_context("version", static_cast<std::uint64_t>(header.version));
        return Result<void>::failure(ErrorCode::unsupported_feature, std::move(diagnostic));
    }
    if (header.sh_degree > 3) {
        Diagnostic diagnostic("MK1328_SPZ_SH_DEGREE_UNSUPPORTED", Severity::error,
                              "The SPZ SH degree is not supported");
        diagnostic.with_context("degree", static_cast<std::uint64_t>(header.sh_degree));
        return Result<void>::failure(ErrorCode::unsupported_feature, std::move(diagnostic));
    }
    if (header.version != 1 && header.fractional_bits > 23) {
        Diagnostic diagnostic("MK1329_SPZ_FRACTIONAL_BITS_INVALID", Severity::error,
                              "The SPZ fixed-point fractional bit count is invalid");
        diagnostic.with_context("fractional_bits",
                                static_cast<std::uint64_t>(header.fractional_bits));
        return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }
    if ((header.flags & static_cast<std::uint8_t>(~kSpzAntialiasedFlag)) != 0) {
        Diagnostic diagnostic("MK1333_SPZ_FLAGS_INVALID", Severity::error,
                              "The SPZ header contains an unknown flag");
        diagnostic.with_context("flags", static_cast<std::uint64_t>(header.flags));
        return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }
    if (header.reserved != 0) {
        Diagnostic diagnostic("MK1334_SPZ_RESERVED_BYTE_INVALID", Severity::error,
                              "The SPZ reserved header byte is not zero");
        diagnostic.with_context("reserved", static_cast<std::uint64_t>(header.reserved));
        return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }
    if (header.num_points == 0) {
        return Result<void>::failure(
            ErrorCode::invalid_data,
            Diagnostic("MK1335_SPZ_EMPTY", Severity::error, "The SPZ data contains no points"));
    }
    return Result<void>::success();
}

static Result<SpzDecodePlan> makeSpzDecodePlan(const SpzPackedHeader& header) {
    const std::uint64_t position_bytes = header.version == 1 ? 6 : 9;
    const std::uint64_t rotation_bytes = header.version >= 3 ? 4 : 3;
    const std::uint64_t sh_bytes = shRestCountForDegree(header.sh_degree);
    const std::uint64_t packed_bytes_per_splat =
        position_bytes + 1 + 3 + 3 + rotation_bytes + sh_bytes;

    auto packed_payload =
        checked_array_bytes(header.num_points, packed_bytes_per_splat, "SPZ packed payload size");
    if (!packed_payload.has_value()) {
        return Result<SpzDecodePlan>::failure(packed_payload.error_code(),
                                              packed_payload.diagnostics());
    }
    auto packed_bytes =
        checked_add(kSpzHeaderBytes, packed_payload.value(), "SPZ packed stream size");
    if (!packed_bytes.has_value()) {
        return Result<SpzDecodePlan>::failure(packed_bytes.error_code(),
                                              packed_bytes.diagnostics());
    }

    const std::uint64_t cloud_floats_per_splat = 14 + sh_bytes;
    auto cloud_floats =
        checked_array_bytes(header.num_points, cloud_floats_per_splat, "SPZ decoded float count");
    if (!cloud_floats.has_value()) {
        return Result<SpzDecodePlan>::failure(cloud_floats.error_code(),
                                              cloud_floats.diagnostics());
    }
    auto cloud_bytes =
        checked_array_bytes(cloud_floats.value(), sizeof(float), "SPZ decoded cloud size");
    if (!cloud_bytes.has_value()) {
        return Result<SpzDecodePlan>::failure(cloud_bytes.error_code(), cloud_bytes.diagnostics());
    }

    SpzDecodePlan plan;
    plan.packed_bytes = packed_bytes.value();
    plan.cloud_bytes = cloud_bytes.value();
    auto payload_and_cloud =
        checked_add(plan.packed_bytes, plan.cloud_bytes, "SPZ decode payload and cloud");
    if (!payload_and_cloud.has_value()) {
        return Result<SpzDecodePlan>::failure(payload_and_cloud.error_code(),
                                              payload_and_cloud.diagnostics());
    }
    auto working_memory =
        checked_add(payload_and_cloud.value(), kSpzCodecOverheadBytes, "SPZ decode working memory");
    if (!working_memory.has_value()) {
        return Result<SpzDecodePlan>::failure(working_memory.error_code(),
                                              working_memory.diagnostics());
    }
    plan.working_memory_bytes = working_memory.value();
    return Result<SpzDecodePlan>::success(plan);
}

static size_t shRestCountForDegree(int degree) {
    switch (degree) {
    case 0:
        return 0;
    case 1:
        return 9;
    case 2:
        return 24;
    case 3:
        return 45;
    default:
        return 0;
    }
}

static int effectiveDegree(const SplatData& data, const SpzEncodeConfig& config) {
    const int source_degree = static_cast<int>(data.sh().degree());
    const int requested_degree = config.sh_degree < 0 ? 3 : config.sh_degree;
    return std::min({source_degree, requested_degree, 3});
}

static Result<void> encodeValidationFailure(ErrorCode code, const char* diagnostic_code,
                                            std::string message) {
    return Result<void>::failure(code,
                                 Diagnostic(diagnostic_code, Severity::error, std::move(message)));
}

static Result<void> validateEncodeInput(const SplatData& data, const SpzEncodeConfig& config,
                                        const OperationContext& context) {
    if (data.empty()) {
        return encodeValidationFailure(ErrorCode::invalid_data, "MK1301_SPZ_EMPTY",
                                       "Cannot encode empty splat data");
    }
    if (config.sh_degree < -1 || config.sh_degree > 3) {
        return encodeValidationFailure(ErrorCode::invalid_argument, "MK1302_SPZ_SH_DEGREE_INVALID",
                                       "The requested SPZ SH degree must be -1 or between 0 and 3");
    }
    if (!config.color_space.has_value()) {
        return encodeValidationFailure(
            ErrorCode::invalid_argument, "MK1308_SPZ_COLOR_SPACE_REQUIRED",
            "SPZ output requires the source color space because SPZ cannot store it");
    }
    if (!is_valid(*config.color_space)) {
        return encodeValidationFailure(ErrorCode::invalid_argument,
                                       "MK1309_SPZ_COLOR_SPACE_INVALID",
                                       "The SPZ source color space is invalid");
    }
    if (!config.antialiased.has_value()) {
        return encodeValidationFailure(ErrorCode::invalid_argument,
                                       "MK1310_SPZ_ANTIALIASING_REQUIRED",
                                       "SPZ output requires an explicit antialiasing value");
    }
    const int effective_degree = effectiveDegree(data, config);
    const size_t sh_components = shRestCountForDegree(effective_degree);
    const size_t largest_signed_multiplier = std::max<size_t>({9, 4, sh_components});
    const size_t signed_allocation_limit =
        static_cast<size_t>(std::numeric_limits<int32_t>::max()) / largest_signed_multiplier;
    if (data.size() > signed_allocation_limit) {
        return encodeValidationFailure(
            ErrorCode::resource_limit, "MK1304_SPZ_POINT_COUNT_LIMIT",
            "The SPZ point count exceeds an upstream signed allocation limit");
    }
    // The canonical decoder rejects larger clouds, even when an individual
    // encoder-side allocation expression would still fit in int32.
    if (data.size() > kMaxUpstreamSpzPoints) {
        return encodeValidationFailure(ErrorCode::resource_limit, "MK1304_SPZ_POINT_COUNT_LIMIT",
                                       "SPZ v1 through v3 support at most 10 million points");
    }

    auto valid = data.validate(context);
    if (!valid.has_value()) {
        return valid;
    }

    // SPZ stores positions as signed 24-bit fixed point with 12 fractional
    // bits. Validate the value after the LUF-to-RUB axis flip. The interval is
    // asymmetric, so a check in the source frame can accept an invalid value.
    constexpr float kPositionScale = 4096.0f;
    constexpr float kMinFixedPosition = -8388608.0f;
    constexpr float kMaxFixedPosition = 8388607.0f;
    const float min_int_float = static_cast<float>(std::numeric_limits<int32_t>::min());
    const float max_int_float = static_cast<float>(std::numeric_limits<int32_t>::max());
    const auto converter =
        spz::coordinateConverter(spz::CoordinateSystem::LUF, spz::CoordinateSystem::RUB);
    for (size_t index = 0; index < data.size(); ++index) {
        if (index % kControlInterval == 0) {
            auto control =
                context.checkpoint({"spz.write", "validate", index, data.size(), "splats"});
            if (!control.has_value())
                return control;
        }
        const Vec3f position = data.positions()[index];
        const float position_components[3] = {position.x, position.y, position.z};
        for (std::size_t component = 0; component < 3; ++component) {
            const float fixed = std::round(converter.flipP[component] *
                                           position_components[component] * kPositionScale);
            if (!std::isfinite(fixed) || fixed < kMinFixedPosition || fixed > kMaxFixedPosition) {
                Diagnostic diagnostic("MK1305_SPZ_POSITION_RANGE", Severity::error,
                                      "A position exceeds the SPZ signed 24-bit fixed-point range");
                diagnostic.with_context("splat_index", static_cast<std::uint64_t>(index))
                    .with_context("component", static_cast<std::uint64_t>(component));
                return Result<void>::failure(ErrorCode::unsupported_feature, std::move(diagnostic));
            }
        }
        const std::size_t source_coefficients = data.sh().coefficients();
        const std::size_t encoded_coefficients =
            static_cast<std::size_t>(effective_degree + 1) * (effective_degree + 1);
        const std::size_t sh_base = index * source_coefficients * 3;
        for (std::size_t coefficient = 1; coefficient < encoded_coefficients; ++coefficient) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                // Mirror the upstream float expression, but test its range before
                // the cast. float(INT32_MAX) rounds to 2^31, so that upper bound is
                // exclusive; INT32_MIN is exactly representable.
                const float value = data.sh().raw()[sh_base + coefficient * 3 + channel] *
                                    converter.flipSh[coefficient - 1];
                const float quantized = std::round(value * 128.0f) + 128.0f;
                if (!std::isfinite(quantized) || quantized < min_int_float ||
                    quantized >= max_int_float) {
                    Diagnostic diagnostic(
                        "MK1306_SPZ_SH_RANGE", Severity::error,
                        "An SH coefficient exceeds the safe SPZ quantization range");
                    diagnostic.with_context("splat_index", static_cast<std::uint64_t>(index))
                        .with_context("coefficient", static_cast<std::uint64_t>(coefficient))
                        .with_context("channel", static_cast<std::uint64_t>(channel));
                    return Result<void>::failure(ErrorCode::unsupported_feature,
                                                 std::move(diagnostic));
                }
            }
        }
    }
    return context.checkpoint({"spz.write", "validate", data.size(), data.size(), "splats"});
}

static bool spzByteEncodingClamps(float encoded) {
    const float rounded = std::round(encoded);
    return rounded < 0.0f || rounded > 255.0f;
}

static float safeSpzDcForUpstream(float coefficient) noexcept {
    const float encoded = coefficient * (kSpzColorScale * 255.0f) + (0.5f * 255.0f);
    if (std::isfinite(encoded))
        return coefficient;

    // The loss report already requires approval for this clamp. Give the upstream converter a
    // finite value that maps to the same endpoint. This prevents an out-of-range integer cast.
    return std::copysign(kSpzSafeClampedDcMagnitude, coefficient);
}

static bool spzShEncodingClamps(float value, int bucket_size) {
    const float rounded = std::round(value * 128.0f) + 128.0f;
    const auto integer = static_cast<std::int64_t>(rounded);
    const std::int64_t bucketed = ((integer + bucket_size / 2) / bucket_size) * bucket_size;
    return bucketed < 0 || bucketed > 255;
}

static Result<LossReport> makeSpzLossReport(const SplatData& data, const SpzEncodeConfig& config,
                                            const OperationContext& context) {
    LossReport losses;

    LossItem color;
    color.code = loss_code::kColorSpaceMetadataDropped;
    color.severity = LossSeverity::severe;
    // validateEncodeInput proves that this value exists and is valid.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    color.source_feature = std::string("color space ") + to_string(*config.color_space);
    color.target_constraint = "SPZ v1 through v3 do not store a color-space identifier";
    color.affected_splats = data.size();
    color.remediation = "keep the color space in a sidecar or select a self-describing target";
    if (auto added = losses.add(std::move(color)); !added.has_value()) {
        return Result<LossReport>::failure(added.error_code(), added.diagnostics());
    }

    LossItem unit;
    unit.code = loss_code::kCoordinateMetadataDropped;
    unit.severity = LossSeverity::severe;
    unit.source_feature = "meter length-unit metadata";
    unit.target_constraint = "SPZ v1 through v3 do not store a length-unit identifier";
    unit.affected_splats = data.size();
    unit.remediation = "keep the unit scale in a sidecar or select a self-describing target";
    if (auto added = losses.add(std::move(unit)); !added.has_value()) {
        return Result<LossReport>::failure(added.error_code(), added.diagnostics());
    }

    LossItem quantization;
    quantization.code = loss_code::kQuantizationApplied;
    quantization.severity = LossSeverity::warning;
    quantization.source_feature = "canonical float32 Gaussian attributes";
    quantization.target_constraint = "SPZ v1 through v3 use quantized attribute storage";
    quantization.affected_splats = data.size();
    quantization.remediation =
        "select canonical PLY or glTF when exact float32 values are required";
    if (auto added = losses.add(std::move(quantization)); !added.has_value()) {
        return Result<LossReport>::failure(added.error_code(), added.diagnostics());
    }

    const int encoded_degree = effectiveDegree(data, config);
    if (encoded_degree < static_cast<int>(data.sh().degree())) {
        LossItem truncation;
        truncation.code = loss_code::kShDegreeTruncated;
        truncation.severity = LossSeverity::severe;
        truncation.source_feature =
            "spherical harmonics degree " + std::to_string(data.sh().degree());
        truncation.target_constraint = "SPZ v1 through v3 support spherical harmonics degree 0-3";
        truncation.affected_splats = data.size();
        truncation.remediation = "select an output profile that supports the source degree";
        if (auto added = losses.add(std::move(truncation)); !added.has_value()) {
            return Result<LossReport>::failure(added.error_code(), added.diagnostics());
        }
    }

    std::size_t scale_clamped_splats = 0;
    std::size_t color_clamped_splats = 0;
    std::size_t sh_clamped_splats = 0;
    const std::size_t encoded_coefficients =
        static_cast<std::size_t>(encoded_degree + 1) * (encoded_degree + 1);
    const std::size_t source_coefficients = data.sh().coefficients();
    const auto converter =
        spz::coordinateConverter(spz::CoordinateSystem::LUF, spz::CoordinateSystem::RUB);
    for (std::size_t splat = 0; splat < data.size(); ++splat) {
        if (splat % kControlInterval == 0) {
            auto control =
                context.checkpoint({"spz.write", "loss_scan", splat, data.size(), "splats"});
            if (!control.has_value()) {
                return Result<LossReport>::failure(control.error_code(), control.diagnostics());
            }
        }
        bool scale_clamped = false;
        const Vec3f scale = data.scales()[splat];
        for (float component : {scale.x, scale.y, scale.z}) {
            const float encoded = (std::log(component) + 10.0f) * 16.0f;
            scale_clamped = scale_clamped || spzByteEncodingClamps(encoded);
        }
        scale_clamped_splats += scale_clamped ? 1 : 0;

        const std::size_t base = splat * source_coefficients * 3;
        bool color_clamped = false;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float encoded =
                data.sh().raw()[base + channel] * (kSpzColorScale * 255.0f) + (0.5f * 255.0f);
            color_clamped = color_clamped || spzByteEncodingClamps(encoded);
        }
        color_clamped_splats += color_clamped ? 1 : 0;

        bool sh_clamped = false;
        for (std::size_t coefficient = 1; coefficient < encoded_coefficients; ++coefficient) {
            const int bucket_size = coefficient <= 3 ? 8 : 16;
            const float flip = converter.flipSh[coefficient - 1];
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float value = data.sh().raw()[base + coefficient * 3 + channel] * flip;
                sh_clamped = sh_clamped || spzShEncodingClamps(value, bucket_size);
            }
        }
        sh_clamped_splats += sh_clamped ? 1 : 0;
    }
    if (auto control =
            context.checkpoint({"spz.write", "loss_scan", data.size(), data.size(), "splats"});
        !control.has_value()) {
        return Result<LossReport>::failure(control.error_code(), control.diagnostics());
    }

    if (scale_clamped_splats != 0) {
        LossItem scale;
        scale.code = loss_code::kScaleClamped;
        scale.severity = LossSeverity::severe;
        scale.source_feature = "canonical scales outside the SPZ logarithmic byte range";
        scale.target_constraint = "SPZ v1 through v3 store each logarithmic scale in one byte";
        scale.affected_splats = scale_clamped_splats;
        scale.remediation = "rescale the asset or select a target with float32 scales";
        if (auto added = losses.add(std::move(scale)); !added.has_value()) {
            return Result<LossReport>::failure(added.error_code(), added.diagnostics());
        }
    }
    if (color_clamped_splats != 0) {
        LossItem color_clamp;
        color_clamp.code = loss_code::kColorClamped;
        color_clamp.severity = LossSeverity::severe;
        color_clamp.source_feature = "SH degree-zero coefficients outside the SPZ byte range";
        color_clamp.target_constraint = "SPZ v1 through v3 store each base-color value in one byte";
        color_clamp.affected_splats = color_clamped_splats;
        color_clamp.remediation = "select canonical PLY or glTF to keep the source coefficients";
        if (auto added = losses.add(std::move(color_clamp)); !added.has_value()) {
            return Result<LossReport>::failure(added.error_code(), added.diagnostics());
        }
    }
    if (sh_clamped_splats != 0) {
        LossItem sh_clamp;
        sh_clamp.code = loss_code::kShCoefficientsClamped;
        sh_clamp.severity = LossSeverity::severe;
        sh_clamp.source_feature = "higher SH coefficients outside the SPZ byte range";
        sh_clamp.target_constraint = "SPZ v1 through v3 store each higher SH value in one byte";
        sh_clamp.affected_splats = sh_clamped_splats;
        sh_clamp.remediation = "select canonical PLY or glTF to keep the source coefficients";
        if (auto added = losses.add(std::move(sh_clamp)); !added.has_value()) {
            return Result<LossReport>::failure(added.error_code(), added.diagnostics());
        }
    }

    return Result<LossReport>::success(std::move(losses));
}

static Result<SpzEncodePlan> makeSpzEncodePlan(const SplatData& data,
                                               const SpzEncodeConfig& config) {
    const std::uint64_t count = data.size();
    const std::uint64_t sh_bytes = shRestCountForDegree(effectiveDegree(data, config));

    auto cloud_floats = checked_array_bytes(count, 14 + sh_bytes, "SPZ encode float count");
    if (!cloud_floats.has_value()) {
        return Result<SpzEncodePlan>::failure(cloud_floats.error_code(),
                                              cloud_floats.diagnostics());
    }
    auto cloud_bytes =
        checked_array_bytes(cloud_floats.value(), sizeof(float), "SPZ encode cloud bytes");
    if (!cloud_bytes.has_value()) {
        return Result<SpzEncodePlan>::failure(cloud_bytes.error_code(), cloud_bytes.diagnostics());
    }

    auto packed_payload = checked_array_bytes(count, 20 + sh_bytes, "SPZ encode packed payload");
    if (!packed_payload.has_value()) {
        return Result<SpzEncodePlan>::failure(packed_payload.error_code(),
                                              packed_payload.diagnostics());
    }
    auto packed_bytes =
        checked_add(kSpzHeaderBytes, packed_payload.value(), "SPZ encode packed stream");
    if (!packed_bytes.has_value()) {
        return Result<SpzEncodePlan>::failure(packed_bytes.error_code(),
                                              packed_bytes.diagnostics());
    }

    // The gzip result cannot exceed twice the packed stream plus a small container allowance.
    // This is conservative for zlib and remains independent of its platform integer types.
    auto doubled_output = checked_mul(packed_bytes.value(), 2, "SPZ output bound");
    if (!doubled_output.has_value()) {
        return Result<SpzEncodePlan>::failure(doubled_output.error_code(),
                                              doubled_output.diagnostics());
    }
    auto output_bound = checked_add(doubled_output.value(), 1024, "SPZ output bound");
    if (!output_bound.has_value()) {
        return Result<SpzEncodePlan>::failure(output_bound.error_code(),
                                              output_bound.diagnostics());
    }

    // The upstream v3 writer materializes a packed object, a string stream, a string, and the
    // gzip result. Charge their conservative peak before it allocates any of them.
    auto serialized_copies = checked_mul(packed_bytes.value(), 3, "SPZ serialized copies");
    auto serialization_peak =
        serialized_copies.has_value()
            ? checked_add(cloud_bytes.value(), serialized_copies.value(), "SPZ serialization peak")
            : Result<std::uint64_t>::failure(serialized_copies.error_code(),
                                             serialized_copies.diagnostics());
    auto compressed_buffers =
        checked_add(packed_bytes.value(), output_bound.value(), "SPZ compression buffers");
    auto compression_peak =
        compressed_buffers.has_value()
            ? checked_add(cloud_bytes.value(), compressed_buffers.value(), "SPZ compression peak")
            : Result<std::uint64_t>::failure(compressed_buffers.error_code(),
                                             compressed_buffers.diagnostics());
    if (!serialization_peak.has_value() || !compression_peak.has_value()) {
        const auto& failed =
            !serialization_peak.has_value() ? serialization_peak : compression_peak;
        return Result<SpzEncodePlan>::failure(failed.error_code(), failed.diagnostics());
    }
    auto working = checked_add(std::max(serialization_peak.value(), compression_peak.value()),
                               kSpzCodecOverheadBytes, "SPZ encode working memory");
    if (!working.has_value()) {
        return Result<SpzEncodePlan>::failure(working.error_code(), working.diagnostics());
    }

    return Result<SpzEncodePlan>::success(
        {packed_bytes.value(), output_bound.value(), working.value()});
}

static Result<spz::GaussianCloud> toSpzCloud(const SplatData& data, const SpzEncodeConfig& config,
                                             const OperationContext& context) {
    spz::GaussianCloud spz_cloud;
    spz_cloud.numPoints = static_cast<int32_t>(data.size());
    const int effective_degree = effectiveDegree(data, config);
    spz_cloud.shDegree = effective_degree;
    // validateEncodeInput proves that this value exists.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    spz_cloud.antialiased = *config.antialiased;

    // Reserve space
    spz_cloud.positions.reserve(data.size() * 3);
    spz_cloud.scales.reserve(data.size() * 3);
    spz_cloud.rotations.reserve(data.size() * 4);
    spz_cloud.alphas.reserve(data.size());
    spz_cloud.colors.reserve(data.size() * 3);

    // Number of SH-rest coefficients per splat for the effective degree.
    int sh_rest_count = 0;
    switch (effective_degree) {
    case 1:
        sh_rest_count = 9;
        break;
    case 2:
        sh_rest_count = 24;
        break;
    case 3:
        sh_rest_count = 45;
        break;
    default:
        sh_rest_count = 0;
        break;
    }

    for (size_t i = 0; i < data.size(); ++i) {
        if (i % kControlInterval == 0) {
            auto control =
                context.checkpoint({"spz.write", "canonicalize", i, data.size(), "splats"});
            if (!control.has_value()) {
                return Result<spz::GaussianCloud>::failure(control.error_code(),
                                                           control.diagnostics());
            }
        }
        const Vec3f position = data.positions()[i];
        const Vec3f scale = data.scales()[i];
        const Quatf rotation = data.rotations()[i];

        // Position (x, y, z)
        spz_cloud.positions.push_back(position.x);
        spz_cloud.positions.push_back(position.y);
        spz_cloud.positions.push_back(position.z);

        spz_cloud.scales.push_back(
            math::log_scale_from_linear(std::max(scale.x, kSpzMinimumScale)).value());
        spz_cloud.scales.push_back(
            math::log_scale_from_linear(std::max(scale.y, kSpzMinimumScale)).value());
        spz_cloud.scales.push_back(
            math::log_scale_from_linear(std::max(scale.z, kSpzMinimumScale)).value());

        // Both SPZ and canonical SplatData are xyzw.
        spz_cloud.rotations.push_back(rotation.x);
        spz_cloud.rotations.push_back(rotation.y);
        spz_cloud.rotations.push_back(rotation.z);
        spz_cloud.rotations.push_back(rotation.w);

        float opacity = data.opacities()[i];
        if (opacity <= 0.0f)
            opacity = kSpzOpacityEpsilon;
        if (opacity >= 1.0f)
            opacity = 1.0f - kSpzOpacityEpsilon;
        spz_cloud.alphas.push_back(math::logit_from_probability(opacity).value());

        // Color (SH DC coefficients)
        const auto dc = data.sh().dc(i);
        if (!dc.has_value()) {
            Diagnostic diagnostic("MK1326_SPZ_SH_LAYOUT", Severity::error,
                                  "Validated SH data has no DC coefficient for a splat");
            diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i));
            return Result<spz::GaussianCloud>::failure(ErrorCode::internal_error,
                                                       std::move(diagnostic));
        }
        spz_cloud.colors.push_back(safeSpzDcForUpstream(dc->x));
        spz_cloud.colors.push_back(safeSpzDcForUpstream(dc->y));
        spz_cloud.colors.push_back(safeSpzDcForUpstream(dc->z));

        // SPZ and canonical storage both interleave RGB as the fastest axis for each coefficient.
        const int num_coeffs = sh_rest_count / 3;
        const std::size_t source_coefficients = data.sh().coefficients();
        const std::size_t sh_base = i * source_coefficients * 3;
        for (int j = 0; j < num_coeffs; ++j) {
            for (int ch = 0; ch < 3; ++ch) {
                spz_cloud.sh.push_back(
                    data.sh().raw()[sh_base + static_cast<std::size_t>(j + 1) * 3 +
                                    static_cast<std::size_t>(ch)]);
            }
        }
    }

    auto control =
        context.checkpoint({"spz.write", "canonicalize", data.size(), data.size(), "splats"});
    if (!control.has_value()) {
        return Result<spz::GaussianCloud>::failure(control.error_code(), control.diagnostics());
    }

    return Result<spz::GaussianCloud>::success(std::move(spz_cloud));
}

static Result<std::vector<std::uint8_t>> inflateSpzPayload(const std::uint8_t* compressed,
                                                           std::size_t compressed_size,
                                                           std::size_t expected_size,
                                                           const OperationContext& context) {
    std::vector<std::uint8_t> output(expected_size);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed));
    stream.avail_in = static_cast<uInt>(compressed_size);
    const int initialized = inflateInit2(&stream, 16 + MAX_WBITS);
    if (initialized != Z_OK) {
        const ErrorCode code =
            initialized == Z_MEM_ERROR ? ErrorCode::resource_limit : ErrorCode::internal_error;
        return Result<std::vector<std::uint8_t>>::failure(
            code, Diagnostic("MK1336_SPZ_INFLATE_INIT", Severity::error,
                             "Could not initialize the bounded SPZ decoder"));
    }
    struct InflateGuard {
        z_stream* stream;
        ~InflateGuard() { inflateEnd(stream); }
    } guard{&stream};

    constexpr std::size_t kInflateChunkBytes = std::size_t{64} * 1024U;
    std::size_t produced_total = 0;
    std::uint8_t overflow_byte = 0;
    for (;;) {
        const bool output_is_full = produced_total == expected_size;
        const std::size_t available =
            output_is_full ? 1 : std::min(kInflateChunkBytes, expected_size - produced_total);
        stream.next_out = output_is_full ? &overflow_byte : output.data() + produced_total;
        stream.avail_out = static_cast<uInt>(available);
        const uInt input_before = stream.avail_in;
        const uInt output_before = stream.avail_out;
        const int status = inflate(&stream, Z_NO_FLUSH);
        const std::size_t produced = output_before - stream.avail_out;
        if (output_is_full && produced != 0) {
            return Result<std::vector<std::uint8_t>>::failure(
                ErrorCode::invalid_data,
                Diagnostic("MK1337_SPZ_INFLATE_INVALID", Severity::error,
                           "The SPZ gzip stream expands beyond its declared size"));
        }
        if (!output_is_full)
            produced_total += produced;

        auto control =
            context.checkpoint({"spz.read", "inflate", produced_total, expected_size, "bytes"});
        if (!control.has_value()) {
            return Result<std::vector<std::uint8_t>>::failure(control.error_code(),
                                                              control.diagnostics());
        }
        if (status == Z_STREAM_END) {
            if (produced_total != expected_size || stream.avail_in != 0) {
                Diagnostic diagnostic("MK1337_SPZ_INFLATE_INVALID", Severity::error,
                                      "The SPZ gzip stream has an invalid size or trailing data");
                diagnostic.with_context("expected_bytes", static_cast<std::uint64_t>(expected_size))
                    .with_context("decoded_bytes", static_cast<std::uint64_t>(produced_total))
                    .with_context("trailing_bytes", static_cast<std::uint64_t>(stream.avail_in));
                return Result<std::vector<std::uint8_t>>::failure(ErrorCode::invalid_data,
                                                                  std::move(diagnostic));
            }
            return Result<std::vector<std::uint8_t>>::success(std::move(output));
        }
        if (status != Z_OK ||
            (input_before == stream.avail_in && output_before == stream.avail_out)) {
            Diagnostic diagnostic("MK1337_SPZ_INFLATE_INVALID", Severity::error,
                                  "The SPZ gzip stream is corrupt or incomplete");
            diagnostic.with_context("zlib_status", static_cast<std::int64_t>(status));
            return Result<std::vector<std::uint8_t>>::failure(ErrorCode::invalid_data,
                                                              std::move(diagnostic));
        }
    }
}

static Result<Quatf> decodeSpzRotation(const std::uint8_t* encoded, bool smallest_three,
                                       std::size_t splat) {
    math::Quat rotation{};
    double decoded_squared_norm = 1.0;
    if (smallest_three) {
        std::uint32_t bits = readU32LittleEndian(encoded);
        const std::uint32_t largest = bits >> 30U;
        constexpr std::uint32_t kMagnitudeMask = (1U << 9U) - 1U;
        constexpr double kSqrtHalf = 0.70710678118654752440;
        double sum_squares = 0.0;
        double* components[4] = {&rotation.x, &rotation.y, &rotation.z, &rotation.w};
        for (int component = 3; component >= 0; --component) {
            if (static_cast<std::uint32_t>(component) == largest)
                continue;
            const std::uint32_t magnitude = bits & kMagnitudeMask;
            const bool negative = ((bits >> 9U) & 1U) != 0;
            bits >>= 10U;
            double value =
                kSqrtHalf * static_cast<double>(magnitude) / static_cast<double>(kMagnitudeMask);
            if (negative)
                value = -value;
            *components[component] = value;
            sum_squares += value * value;
        }
        if (sum_squares > 1.0) {
            Diagnostic diagnostic("MK1338_SPZ_ROTATION_INVALID", Severity::error,
                                  "The SPZ smallest-three quaternion is invalid");
            diagnostic.with_context("splat_index", static_cast<std::uint64_t>(splat));
            return Result<Quatf>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
        *components[largest] = std::sqrt(1.0 - sum_squares);
    } else {
        rotation.x = static_cast<double>(encoded[0]) / 127.5 - 1.0;
        rotation.y = static_cast<double>(encoded[1]) / 127.5 - 1.0;
        rotation.z = static_cast<double>(encoded[2]) / 127.5 - 1.0;
        const double sum_squares =
            rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z;
        decoded_squared_norm = sum_squares;
        rotation.w = std::sqrt(std::max(0.0, 1.0 - sum_squares));

        // SPZ v1-v2 stores three components with an 8-bit uniform quantizer.
        // Rounding can move a valid unit quaternion slightly outside the unit sphere.
        constexpr double kQuantizationError = 1.0 / 255.0;
        constexpr double kSqrtThree = 1.73205080756887729353;
        constexpr double kMaximumSquaredNorm = 1.0 + 2.0 * kSqrtThree * kQuantizationError +
                                               3.0 * kQuantizationError * kQuantizationError;
        if (decoded_squared_norm > kMaximumSquaredNorm) {
            Diagnostic diagnostic("MK1320_SPZ_NON_UNIT_ROTATION", Severity::error,
                                  "The SPZ v1-v2 rotation exceeds its quantization bound");
            diagnostic.with_context("splat_index", static_cast<std::uint64_t>(splat))
                .with_context("squared_norm", decoded_squared_norm)
                .with_context("maximum_squared_norm", kMaximumSquaredNorm);
            return Result<Quatf>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
    }

    // SPZ stores RUB. Canonical data uses LUF. This proper basis change flips X and Z.
    rotation.x = -rotation.x;
    rotation.z = -rotation.z;
    if (smallest_three && !math::is_unit(rotation)) {
        Diagnostic diagnostic("MK1320_SPZ_NON_UNIT_ROTATION", Severity::error,
                              "SPZ rotation is not unit within the canonical tolerance");
        diagnostic.with_context("splat_index", static_cast<std::uint64_t>(splat))
            .with_context("norm", math::norm(rotation));
        return Result<Quatf>::failure(ErrorCode::invalid_data, std::move(diagnostic));
    }
    auto normalized = math::normalize(rotation);
    if (!normalized.has_value())
        return Result<Quatf>::failure(normalized.error_code(), normalized.diagnostics());

    Quatf result{static_cast<float>(normalized.value().x), static_cast<float>(normalized.value().y),
                 static_cast<float>(normalized.value().z),
                 static_cast<float>(normalized.value().w)};
    return Result<Quatf>::success(result);
}

static Result<SplatData> decodeSpzPayload(const std::vector<std::uint8_t>& payload,
                                          const SpzPackedHeader& header, double unit_to_meter,
                                          const OperationContext& context) {
    if (payload.size() < kSpzHeaderBytes || readU32LittleEndian(payload.data()) != kSpzMagic ||
        readU32LittleEndian(payload.data() + 4) != header.version ||
        readU32LittleEndian(payload.data() + 8) != header.num_points ||
        payload[12] != header.sh_degree || payload[13] != header.fractional_bits ||
        payload[14] != header.flags || payload[15] != header.reserved) {
        return Result<SplatData>::failure(
            ErrorCode::invalid_data,
            Diagnostic("MK1339_SPZ_HEADER_CHANGED", Severity::error,
                       "The decoded SPZ header does not match the bounded probe"));
    }

    const std::size_t count = header.num_points;
    const std::uint32_t degree = header.sh_degree;
    const std::size_t coefficients = static_cast<std::size_t>(degree + 1) * (degree + 1);
    const std::size_t position_bytes = header.version == 1 ? 6 : 9;
    const std::size_t rotation_bytes = header.version >= 3 ? 4 : 3;
    const std::size_t sh_bytes = shRestCountForDegree(header.sh_degree);
    const std::uint8_t* positions = payload.data() + kSpzHeaderBytes;
    const std::uint8_t* alphas = positions + count * position_bytes;
    const std::uint8_t* colors = alphas + count;
    const std::uint8_t* scales = colors + count * 3;
    const std::uint8_t* rotations = scales + count * 3;
    const std::uint8_t* sh = rotations + count * rotation_bytes;
    if (sh + count * sh_bytes != payload.data() + payload.size()) {
        return Result<SplatData>::failure(
            ErrorCode::internal_error,
            Diagnostic("MK1340_SPZ_LAYOUT_INVARIANT", Severity::error,
                       "The planned SPZ payload layout is inconsistent"));
    }

    SplatBufferInput input;
    input.positions.resize(count);
    input.scales.resize(count);
    input.rotations.resize(count);
    input.opacities.resize(count);
    std::vector<float> sh_values(count * coefficients * 3, 0.0f);
    const auto converter =
        spz::coordinateConverter(spz::CoordinateSystem::RUB, spz::CoordinateSystem::LUF);

    for (std::size_t i = 0; i < count; ++i) {
        if (i % kControlInterval == 0) {
            auto control = context.checkpoint({"spz.read", "canonicalize", i, count, "splats"});
            if (!control.has_value())
                return Result<SplatData>::failure(control.error_code(), control.diagnostics());
        }

        Vec3f* position = &input.positions[i];
        float* position_components[3] = {&position->x, &position->y, &position->z};
        for (std::size_t component = 0; component < 3; ++component) {
            double source = 0.0;
            if (header.version == 1) {
                const std::uint8_t* encoded = positions + i * position_bytes + component * 2;
                const std::uint16_t half = static_cast<std::uint16_t>(encoded[0]) |
                                           (static_cast<std::uint16_t>(encoded[1]) << 8U);
                source = spz::halfToFloat(half);
            } else {
                const std::uint8_t* encoded = positions + i * position_bytes + component * 3;
                const std::uint32_t raw = static_cast<std::uint32_t>(encoded[0]) |
                                          (static_cast<std::uint32_t>(encoded[1]) << 8U) |
                                          (static_cast<std::uint32_t>(encoded[2]) << 16U);
                const std::int32_t fixed =
                    raw >= 0x800000U
                        ? static_cast<std::int32_t>(static_cast<std::int64_t>(raw) - 0x1000000LL)
                        : static_cast<std::int32_t>(raw);
                source = std::ldexp(static_cast<double>(fixed), -header.fractional_bits);
            }
            source *= converter.flipP[component];
            const double meters = source * unit_to_meter;
            const float stored = static_cast<float>(meters);
            if (!std::isfinite(stored) || (source != 0.0 && stored == 0.0f)) {
                Diagnostic diagnostic("MK1330_SPZ_UNIT_CONVERSION_RANGE", Severity::error,
                                      "An SPZ position exceeds the canonical float32 range");
                diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i))
                    .with_context("component", static_cast<std::uint64_t>(component));
                return Result<SplatData>::failure(ErrorCode::invalid_data, std::move(diagnostic));
            }
            *position_components[component] = stored;
        }

        float* scale_components[3] = {&input.scales[i].x, &input.scales[i].y, &input.scales[i].z};
        for (std::size_t component = 0; component < 3; ++component) {
            const float log_scale = static_cast<float>(scales[i * 3 + component]) / 16.0f - 10.0f;
            auto scale = math::linear_scale_from_log(log_scale);
            if (!scale.has_value()) {
                auto diagnostics = scale.diagnostics();
                for (auto& diagnostic : diagnostics) {
                    diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i))
                        .with_context("component", static_cast<std::uint64_t>(component));
                }
                return Result<SplatData>::failure(scale.error_code(), std::move(diagnostics));
            }
            const double meters = static_cast<double>(scale.value()) * unit_to_meter;
            const float stored = static_cast<float>(meters);
            if (!std::isfinite(stored) || stored <= 0.0f) {
                Diagnostic diagnostic("MK1330_SPZ_UNIT_CONVERSION_RANGE", Severity::error,
                                      "An SPZ scale exceeds the canonical float32 range");
                diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i))
                    .with_context("component", static_cast<std::uint64_t>(component));
                return Result<SplatData>::failure(ErrorCode::invalid_data, std::move(diagnostic));
            }
            *scale_components[component] = stored;
        }

        auto rotation = decodeSpzRotation(rotations + i * rotation_bytes, header.version >= 3, i);
        if (!rotation.has_value())
            return Result<SplatData>::failure(rotation.error_code(), rotation.diagnostics());
        input.rotations[i] = rotation.value();

        input.opacities[i] = static_cast<float>(alphas[i]) / 255.0f;

        const std::size_t sh_base = i * coefficients * 3;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            sh_values[sh_base + channel] =
                ((static_cast<float>(colors[i * 3 + channel]) / 255.0f) - 0.5f) / 0.15f;
        }
        const std::size_t source_sh_base = i * sh_bytes;
        for (std::size_t coefficient = 1; coefficient < coefficients; ++coefficient) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const std::size_t encoded = source_sh_base + (coefficient - 1) * 3 + channel;
                sh_values[sh_base + coefficient * 3 + channel] =
                    converter.flipSh[coefficient - 1] *
                    ((static_cast<float>(sh[encoded]) - 128.0f) / 128.0f);
            }
        }
    }

    auto sh_buffer = ShBuffer::create(degree, count, std::move(sh_values), context);
    if (!sh_buffer.has_value())
        return Result<SplatData>::failure(sh_buffer.error_code(), sh_buffer.diagnostics());
    input.sh = std::move(sh_buffer).value();
    auto data = SplatData::create(std::move(input), context);
    if (!data.has_value())
        return data;
    auto control = context.checkpoint({"spz.read", "canonicalize", count, count, "splats"});
    if (!control.has_value())
        return Result<SplatData>::failure(control.error_code(), control.diagnostics());
    return data;
}

// ============================================================================
// SpzEncoder Implementation
// ============================================================================

SpzEncodeResult SpzEncoder::encodeToFile(const std::filesystem::path& filepath,
                                         const SplatData& data, const SpzEncodeConfig& config) try {
    Budget budget(config.limits);
    OperationContext context = make_default_context(budget);
    return encodeToFile(filepath, data, config, context);
} catch (const std::bad_alloc&) {
    SpzEncodeResult result;
    failEncode(result, ErrorCode::resource_limit, "SPZ encoding exceeded available memory");
    return result;
} catch (const std::length_error&) {
    SpzEncodeResult result;
    failEncode(result, ErrorCode::resource_limit, "SPZ encoding exceeded a container size limit");
    return result;
}

SpzEncodeResult SpzEncoder::encodeToFile(const std::filesystem::path& filepath,
                                         const SplatData& data, const SpzEncodeConfig& requested,
                                         const OperationContext& context) try {
    SpzEncodeResult result;
    if (context.budget == nullptr) {
        failEncode(result, ErrorCode::internal_error, "The SPZ writer requires a resource budget");
        return result;
    }

    SpzEncodeConfig config = requested;
    config.limits = context.budget->limits();
    std::vector<uint8_t> buffer;
    result = encodeToBufferImpl(buffer, data, config, context);
    if (!result.success)
        return result;

    Budget::Charge output_memory = result.take_retained_memory();
    io::WriteOptions options;
    options.overwrite = config.overwrite;

    auto writer = io::AtomicWriter::create(filepath, options, context);
    if (!writer.has_value()) {
        failEncode(
            result, writer.error_code(),
            firstDiagnosticMessage(writer.diagnostics(), "Failed to open the SPZ output file"),
            writer.diagnostics());
        return result;
    }

    auto written = writer.value()->write(buffer.data(), buffer.size());
    if (!written.has_value()) {
        failEncode(
            result, written.error_code(),
            firstDiagnosticMessage(written.diagnostics(), "Failed to write the complete SPZ file"),
            written.diagnostics());
        return result;
    }

    auto committed = writer.value()->commit();
    if (!committed.has_value()) {
        failEncode(result, committed.error_code(),
                   firstDiagnosticMessage(committed.diagnostics(), "Failed to commit the SPZ file"),
                   committed.diagnostics());
        return result;
    }

    result.bytes_written = writer.value()->bytes_written();
    return result;
} catch (const std::bad_alloc&) {
    SpzEncodeResult result;
    failEncode(result, ErrorCode::resource_limit, "SPZ encoding exceeded available memory");
    return result;
} catch (const std::length_error&) {
    SpzEncodeResult result;
    failEncode(result, ErrorCode::resource_limit, "SPZ encoding exceeded a container size limit");
    return result;
} catch (const std::filesystem::filesystem_error&) {
    SpzEncodeResult result;
    failEncode(result, ErrorCode::io_error, "Failed to access the SPZ output file");
    return result;
} catch (const std::exception& error) {
    SpzEncodeResult result;
    failEncode(result, ErrorCode::internal_error,
               std::string("Unexpected SPZ encoding failure: ") + error.what());
    return result;
}

SpzEncodeResult SpzEncoder::encodeToBuffer(std::vector<uint8_t>& buffer, const SplatData& data,
                                           const SpzEncodeConfig& config) try {
    Budget budget(config.limits);
    OperationContext context = make_default_context(budget);
    return encodeToBuffer(buffer, data, config, context);
} catch (const std::bad_alloc&) {
    buffer.clear();
    SpzEncodeResult result;
    failEncode(result, ErrorCode::resource_limit, "SPZ encoding exceeded available memory");
    return result;
} catch (const std::length_error&) {
    buffer.clear();
    SpzEncodeResult result;
    failEncode(result, ErrorCode::resource_limit, "SPZ encoding exceeded a container size limit");
    return result;
}

SpzEncodeResult SpzEncoder::encodeToBuffer(std::vector<uint8_t>& buffer, const SplatData& data,
                                           const SpzEncodeConfig& requested,
                                           const OperationContext& context) try {
    SpzEncodeConfig config = requested;
    if (context.budget != nullptr)
        config.limits = context.budget->limits();
    return encodeToBufferImpl(buffer, data, config, context);
} catch (const std::bad_alloc&) {
    std::vector<uint8_t>().swap(buffer);
    SpzEncodeResult result;
    failEncode(result, ErrorCode::resource_limit, "SPZ encoding exceeded available memory");
    return result;
} catch (const std::length_error&) {
    std::vector<uint8_t>().swap(buffer);
    SpzEncodeResult result;
    failEncode(result, ErrorCode::resource_limit, "SPZ encoding exceeded a container size limit");
    return result;
} catch (const std::exception& error) {
    std::vector<uint8_t>().swap(buffer);
    SpzEncodeResult result;
    failEncode(result, ErrorCode::internal_error,
               std::string("Unexpected SPZ encoding failure: ") + error.what());
    return result;
}

SpzEncodeResult SpzEncoder::encodeToBufferImpl(std::vector<uint8_t>& buffer, const SplatData& data,
                                               const SpzEncodeConfig& config,
                                               const OperationContext& context) {
    SpzEncodeResult result;
    std::vector<uint8_t>().swap(buffer);

    if (context.budget == nullptr) {
        failEncode(result, ErrorCode::internal_error, "The SPZ writer requires a resource budget");
        return result;
    }
    auto valid_limits = context.budget->limits().validate();
    if (!valid_limits.has_value()) {
        failEncode(result, ErrorCode::invalid_argument,
                   firstDiagnosticMessage(valid_limits.diagnostics(), "Invalid resource limits"),
                   valid_limits.diagnostics());
        return result;
    }
    auto control = context.checkpoint({"spz.write", "prepare", 0, data.size(), "splats"});
    if (!control.has_value()) {
        failEncode(result, control.error_code(),
                   firstDiagnosticMessage(control.diagnostics(), "SPZ encoding stopped"),
                   control.diagnostics());
        return result;
    }
    auto observed = context.observe(BudgetKind::splats, data.size(), "spz.write.splats");
    if (!observed.has_value()) {
        failEncode(result, observed.error_code(),
                   firstDiagnosticMessage(observed.diagnostics(),
                                          "SPZ splat count exceeds the selected limit"),
                   observed.diagnostics());
        return result;
    }

    auto input_valid = validateEncodeInput(data, config, context);
    if (!input_valid.has_value()) {
        failEncode(result, input_valid.error_code(),
                   firstDiagnosticMessage(input_valid.diagnostics(), "Cannot encode the SPZ input"),
                   input_valid.diagnostics());
        return result;
    }
    auto losses = makeSpzLossReport(data, config, context);
    if (!losses.has_value()) {
        failEncode(
            result, losses.error_code(),
            firstDiagnosticMessage(losses.diagnostics(), "Could not create the SPZ loss report"),
            losses.diagnostics());
        return result;
    }
    result.losses = std::move(losses).value();
    if (auto policy = result.losses.check_policy(config.approved_loss_codes); !policy.has_value()) {
        failEncode(result, policy.error_code(),
                   firstDiagnosticMessage(policy.diagnostics(), "SPZ loss policy failed"),
                   policy.diagnostics());
        return result;
    }
    auto plan = makeSpzEncodePlan(data, config);
    if (!plan.has_value()) {
        failEncode(result, plan.error_code(),
                   firstDiagnosticMessage(plan.diagnostics(), "SPZ resource planning failed"),
                   plan.diagnostics());
        return result;
    }
    auto memory = context.budget->reserve(
        BudgetKind::memory_bytes, plan.value().working_memory_bytes, "spz.write.working_set");
    if (!memory.has_value()) {
        failEncode(
            result, memory.error_code(),
            firstDiagnosticMessage(memory.diagnostics(), "SPZ encoding exceeds the memory limit"),
            memory.diagnostics());
        return result;
    }
    Budget::Charge working_memory = std::move(memory).value();

    const int encoded_degree = effectiveDegree(data, config);
    if (encoded_degree < static_cast<int>(data.sh().degree())) {
        Diagnostic diagnostic("MK1322_SPZ_SH_TRUNCATED", Severity::warning,
                              "Higher spherical-harmonic coefficients were omitted for SPZ output");
        diagnostic.with_context("source_degree", static_cast<std::uint64_t>(data.sh().degree()))
            .with_context("encoded_degree", static_cast<std::uint64_t>(encoded_degree));
        result.diagnostics.push_back(std::move(diagnostic));
    }
    try {
        auto converted = toSpzCloud(data, config, context);
        if (!converted.has_value()) {
            failEncode(
                result, converted.error_code(),
                firstDiagnosticMessage(converted.diagnostics(), "Failed to build the SPZ cloud"),
                converted.diagnostics());
            return result;
        }
        spz::GaussianCloud spz_cloud = std::move(converted).value();

        spz::PackOptions options;
        options.from = spz::CoordinateSystem::LUF;

        control =
            context.checkpoint({"spz.write", "compress", 0, plan.value().packed_bytes, "bytes"});
        if (!control.has_value()) {
            failEncode(result, control.error_code(),
                       firstDiagnosticMessage(control.diagnostics(), "SPZ encoding stopped"),
                       control.diagnostics());
            return result;
        }
        if (!spz::saveSpz(spz_cloud, options, &buffer)) {
            failEncode(result, ErrorCode::invalid_data, "Failed to encode SPZ data");
            std::vector<uint8_t>().swap(buffer);
            return result;
        }
        if (buffer.capacity() > plan.value().output_bound) {
            failEncode(result, ErrorCode::internal_error,
                       "The SPZ codec exceeded its preflight output bound");
            std::vector<uint8_t>().swap(buffer);
            return result;
        }
        control = context.checkpoint({"spz.write", "compress", plan.value().packed_bytes,
                                      plan.value().packed_bytes, "bytes"});
        if (!control.has_value()) {
            failEncode(result, control.error_code(),
                       firstDiagnosticMessage(control.diagnostics(), "SPZ encoding stopped"),
                       control.diagnostics());
            std::vector<uint8_t>().swap(buffer);
            return result;
        }
        working_memory.shrink_to(buffer.capacity());
        result.set_retained_memory(std::move(working_memory));
        result.success = true;
        result.bytes_written = buffer.size();
    } catch (const std::bad_alloc&) {
        std::vector<uint8_t>().swap(buffer);
        failEncode(result, ErrorCode::resource_limit, "SPZ encoding exceeded available memory");
    } catch (const std::length_error&) {
        std::vector<uint8_t>().swap(buffer);
        failEncode(result, ErrorCode::resource_limit,
                   "SPZ encoding exceeded a container size limit");
    } catch (const std::exception& error) {
        std::vector<uint8_t>().swap(buffer);
        failEncode(result, ErrorCode::internal_error,
                   std::string("Failed to encode SPZ: ") + error.what());
    }

    return result;
}

// ============================================================================
// SpzDecoder Implementation
// ============================================================================

SpzDecoder::DecodeResult SpzDecoder::decodeFromFile(const std::filesystem::path& filepath,
                                                    const SpzDecodeConfig& config) try {
    Budget budget(config.limits);
    OperationContext context = make_default_context(budget);
    return decodeFromFile(filepath, config, context);
} catch (const std::bad_alloc&) {
    DecodeResult result;
    failDecode(result, ErrorCode::resource_limit, "SPZ file exceeds available memory");
    return result;
} catch (const std::length_error&) {
    DecodeResult result;
    failDecode(result, ErrorCode::resource_limit, "SPZ file exceeds a container size limit");
    return result;
} catch (const std::filesystem::filesystem_error&) {
    DecodeResult result;
    failDecode(result, ErrorCode::io_error, "Failed to access the SPZ file");
    return result;
}

SpzDecoder::DecodeResult SpzDecoder::decodeFromFile(const std::filesystem::path& filepath,
                                                    const SpzDecodeConfig& requested,
                                                    const OperationContext& context) try {
    DecodeResult result;
    if (context.budget == nullptr) {
        failDecode(result, ErrorCode::internal_error, "The SPZ reader requires a resource budget");
        return result;
    }
    SpzDecodeConfig config = requested;
    config.limits = context.budget->limits();
    if (auto valid_config = validateDecodeConfig(config); !valid_config.has_value()) {
        failDecode(result, valid_config.error_code(),
                   firstDiagnosticMessage(valid_config.diagnostics(),
                                          "The SPZ decode configuration is invalid"),
                   valid_config.diagnostics());
        return result;
    }
    const Limits& limits = context.budget->limits();
    auto valid = limits.validate();
    if (!valid.has_value()) {
        failDecode(result, ErrorCode::invalid_argument,
                   firstDiagnosticMessage(valid.diagnostics(), "Invalid resource limits"),
                   valid.diagnostics());
        return result;
    }
    auto control = context.checkpoint({"spz.read", "prepare", 0, 0, "bytes"});
    if (!control.has_value()) {
        failDecode(result, control.error_code(),
                   firstDiagnosticMessage(control.diagnostics(), "SPZ reading stopped"),
                   control.diagnostics());
        return result;
    }
    auto opened = io::InputFile::open(filepath, context);
    if (!opened.has_value()) {
        failDecode(result, opened.error_code(),
                   firstDiagnosticMessage(opened.diagnostics(), "Failed to open the SPZ file"),
                   opened.diagnostics());
        return result;
    }
    io::InputFile file = std::move(opened).value();
    const std::uint64_t file_size = file.size();
    if (file_size == 0) {
        failDecode(result, ErrorCode::invalid_data, "The SPZ file is empty");
        return result;
    }
    if (file_size > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
        file_size > std::numeric_limits<std::size_t>::max()) {
        failDecode(result, ErrorCode::resource_limit,
                   "The SPZ file exceeds the 2 GiB decoder limit");
        return result;
    }
    const std::size_t size = static_cast<std::size_t>(file_size);
    if (auto charged = context.consume(BudgetKind::input_bytes, size, "spz.file");
        !charged.has_value()) {
        failDecode(
            result, charged.error_code(),
            firstDiagnosticMessage(charged.diagnostics(), "SPZ file exceeds the input-size limit"),
            charged.diagnostics());
        return result;
    }

    const std::uint64_t probe_limit =
        std::min<std::uint64_t>(limits.max_metadata_total_bytes, std::numeric_limits<uInt>::max());
    const std::size_t prefix_size =
        static_cast<std::size_t>(std::min<std::uint64_t>(size, probe_limit));
    SpzDecodePlan preflight_plan;
    std::vector<uint8_t> prefix;
    std::optional<ScopedBudgetRelease> prefix_release;
    {
        if (auto charged =
                context.consume(BudgetKind::memory_bytes, prefix_size, "spz.file_header_prefix");
            !charged.has_value()) {
            failDecode(result, charged.error_code(),
                       firstDiagnosticMessage(charged.diagnostics(),
                                              "SPZ header prefix exceeds the memory limit"),
                       charged.diagnostics());
            return result;
        }
        prefix_release.emplace(context.budget, BudgetKind::memory_bytes, prefix_size);
        prefix.resize(prefix_size);
        auto prefix_read =
            file.read_exact(0, prefix.data(), prefix.size(), context, "spz.read", "header_file");
        if (!prefix_read.has_value()) {
            failDecode(result, prefix_read.error_code(),
                       firstDiagnosticMessage(prefix_read.diagnostics(),
                                              "Failed to read the SPZ header prefix"),
                       prefix_read.diagnostics());
            return result;
        }
        if (auto charged = context.consume(BudgetKind::memory_bytes, kSpzCodecOverheadBytes,
                                           "spz.file_header_probe");
            !charged.has_value()) {
            failDecode(result, charged.error_code(),
                       firstDiagnosticMessage(charged.diagnostics(),
                                              "SPZ header probe exceeds the memory limit"),
                       charged.diagnostics());
            return result;
        }
        ScopedBudgetRelease probe_release(context.budget, BudgetKind::memory_bytes,
                                          kSpzCodecOverheadBytes);
        auto probed = probeSpzHeader(prefix.data(), prefix.size(), prefix.size() != size);
        if (!probed.has_value()) {
            failDecode(result, probed.error_code(),
                       firstDiagnosticMessage(probed.diagnostics(),
                                              "SPZ input has an invalid packed header"),
                       probed.diagnostics());
            return result;
        }
        if (auto header_valid = validateSpzHeader(probed.value()); !header_valid.has_value()) {
            failDecode(
                result, header_valid.error_code(),
                firstDiagnosticMessage(header_valid.diagnostics(), "The SPZ header is invalid"),
                header_valid.diagnostics());
            return result;
        }
        if (probed.value().num_points > limits.max_splats ||
            probed.value().num_points > kMaxUpstreamSpzPoints) {
            failDecode(result, ErrorCode::resource_limit,
                       "The SPZ point count exceeds the selected limit");
            return result;
        }
        auto planned = makeSpzDecodePlan(probed.value());
        if (!planned.has_value()) {
            failDecode(result, planned.error_code(),
                       firstDiagnosticMessage(planned.diagnostics(),
                                              "SPZ decoded-size calculation failed"),
                       planned.diagnostics());
            return result;
        }
        preflight_plan = planned.value();
        if (preflight_plan.packed_bytes > context.budget->remaining(BudgetKind::decoded_bytes)) {
            failDecode(result, ErrorCode::resource_limit,
                       "The SPZ packed data exceeds the decoded-byte limit");
            return result;
        }
        if (auto ratio = context.budget->check_decompression_ratio(
                size, preflight_plan.packed_bytes, "spz.file_inflate");
            !ratio.has_value()) {
            failDecode(result, ratio.error_code(),
                       firstDiagnosticMessage(ratio.diagnostics(),
                                              "SPZ data exceeds the decompression-ratio limit"),
                       ratio.diagnostics());
            return result;
        }
    }

    auto required_memory =
        checked_add(size, preflight_plan.working_memory_bytes, "SPZ file decode memory");
    auto available_after_prefix =
        checked_add(context.budget->remaining(BudgetKind::memory_bytes), prefix_size,
                    "SPZ memory available after the header prefix is released");
    if (!required_memory.has_value() || !available_after_prefix.has_value() ||
        required_memory.value() > available_after_prefix.value()) {
        failDecode(result, ErrorCode::resource_limit,
                   "The SPZ file and decoded data exceed the memory limit");
        return result;
    }
    if (auto charged = context.consume(BudgetKind::memory_bytes, size, "spz.file_buffer");
        !charged.has_value()) {
        failDecode(result, charged.error_code(),
                   firstDiagnosticMessage(charged.diagnostics(),
                                          "SPZ file buffer exceeds the memory limit"),
                   charged.diagnostics());
        return result;
    }
    ScopedBudgetRelease file_release(context.budget, BudgetKind::memory_bytes, size);
    std::vector<uint8_t> data;
    data.resize(size);
    std::copy(prefix.begin(), prefix.end(), data.begin());
    std::vector<uint8_t>().swap(prefix);
    prefix_release.reset();
    auto payload_read = file.read_exact(prefix_size, data.data() + prefix_size, size - prefix_size,
                                        context, "spz.read", "file");
    if (!payload_read.has_value()) {
        failDecode(result, payload_read.error_code(),
                   firstDiagnosticMessage(payload_read.diagnostics(),
                                          "Failed to read the complete SPZ file"),
                   payload_read.diagnostics());
        return result;
    }
    return decodeFromBufferImpl(data.data(), data.size(), config, context, true);
} catch (const std::bad_alloc&) {
    DecodeResult result;
    failDecode(result, ErrorCode::resource_limit, "SPZ file exceeds available memory");
    return result;
} catch (const std::length_error&) {
    DecodeResult result;
    failDecode(result, ErrorCode::resource_limit, "SPZ file exceeds a container size limit");
    return result;
} catch (const std::filesystem::filesystem_error&) {
    DecodeResult result;
    failDecode(result, ErrorCode::io_error, "Failed to access the SPZ file");
    return result;
}

SpzDecoder::DecodeResult SpzDecoder::decodeFromBuffer(const uint8_t* data, size_t size,
                                                      const SpzDecodeConfig& config) try {
    Budget budget(config.limits);
    OperationContext context = make_default_context(budget);
    return decodeFromBuffer(data, size, config, context);
} catch (const std::bad_alloc&) {
    DecodeResult result;
    failDecode(result, ErrorCode::resource_limit, "SPZ buffer exceeds available memory");
    return result;
} catch (const std::length_error&) {
    DecodeResult result;
    failDecode(result, ErrorCode::resource_limit, "SPZ buffer exceeds a container size limit");
    return result;
}

SpzDecoder::DecodeResult SpzDecoder::decodeFromBuffer(const uint8_t* data, size_t size,
                                                      const SpzDecodeConfig& requested,
                                                      const OperationContext& context) {
    SpzDecodeConfig config = requested;
    if (context.budget != nullptr)
        config.limits = context.budget->limits();
    return decodeFromBufferImpl(data, size, config, context, false);
}

SpzDecoder::DecodeResult SpzDecoder::decodeFromBufferImpl(const uint8_t* data, size_t size,
                                                          const SpzDecodeConfig& config,
                                                          const OperationContext& context,
                                                          bool input_is_charged) try {
    DecodeResult result;
    if (context.budget == nullptr) {
        failDecode(result, ErrorCode::internal_error, "The SPZ reader requires a resource budget");
        return result;
    }
    if (auto valid_config = validateDecodeConfig(config); !valid_config.has_value()) {
        failDecode(result, valid_config.error_code(),
                   firstDiagnosticMessage(valid_config.diagnostics(),
                                          "The SPZ decode configuration is invalid"),
                   valid_config.diagnostics());
        return result;
    }
    result.metadata.color_space = config.source_color_space;
    result.metadata.source_bytes = size;
    const Limits& limits = context.budget->limits();
    auto valid = limits.validate();
    if (!valid.has_value()) {
        failDecode(result, ErrorCode::invalid_argument,
                   firstDiagnosticMessage(valid.diagnostics(), "Invalid resource limits"),
                   valid.diagnostics());
        return result;
    }
    if (data == nullptr || size == 0 ||
        size > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        const ErrorCode code = size > static_cast<size_t>(std::numeric_limits<int32_t>::max())
                                   ? ErrorCode::resource_limit
                                   : ErrorCode::invalid_argument;
        failDecode(result, code, "SPZ input is null, empty, or exceeds the 2 GiB decoder limit");
        return result;
    }
    auto control = context.checkpoint({"spz.read", "buffer", 0, size, "bytes"});
    if (!control.has_value()) {
        failDecode(result, control.error_code(),
                   firstDiagnosticMessage(control.diagnostics(), "SPZ reading stopped"),
                   control.diagnostics());
        return result;
    }
    std::optional<ScopedBudgetRelease> input_release;
    if (!input_is_charged) {
        auto input = context.consume(BudgetKind::input_bytes, size, "spz.input");
        if (!input.has_value()) {
            failDecode(result, input.error_code(),
                       firstDiagnosticMessage(input.diagnostics(),
                                              "SPZ input exceeds the input-size limit"),
                       input.diagnostics());
            return result;
        }
        auto memory = context.consume(BudgetKind::memory_bytes, size, "spz.input_buffer");
        if (!memory.has_value()) {
            failDecode(result, memory.error_code(),
                       firstDiagnosticMessage(memory.diagnostics(),
                                              "SPZ input buffer exceeds the memory limit"),
                       memory.diagnostics());
            return result;
        }
        input_release.emplace(context.budget, BudgetKind::memory_bytes, size);
    }
    SpzPackedHeader header;
    {
        if (auto charged = context.consume(BudgetKind::memory_bytes, kSpzCodecOverheadBytes,
                                           "spz.header_probe");
            !charged.has_value()) {
            failDecode(result, charged.error_code(),
                       firstDiagnosticMessage(charged.diagnostics(),
                                              "SPZ header probe exceeds the memory limit"),
                       charged.diagnostics());
            return result;
        }
        ScopedBudgetRelease probe_release(context.budget, BudgetKind::memory_bytes,
                                          kSpzCodecOverheadBytes);
        const std::uint64_t probe_limit = std::min<std::uint64_t>(limits.max_metadata_total_bytes,
                                                                  std::numeric_limits<uInt>::max());
        const std::size_t probe_size =
            static_cast<std::size_t>(std::min<std::uint64_t>(size, probe_limit));
        auto probed = probeSpzHeader(data, probe_size, probe_size != size);
        if (!probed.has_value()) {
            failDecode(result, probed.error_code(),
                       firstDiagnosticMessage(probed.diagnostics(),
                                              "SPZ input has an invalid packed header"),
                       probed.diagnostics());
            return result;
        }
        header = probed.value();
    }
    if (auto header_valid = validateSpzHeader(header); !header_valid.has_value()) {
        failDecode(result, header_valid.error_code(),
                   firstDiagnosticMessage(header_valid.diagnostics(), "The SPZ header is invalid"),
                   header_valid.diagnostics());
        return result;
    }

    result.metadata.declared_points = header.num_points;
    result.metadata.sh_degree = header.sh_degree;
    result.metadata.antialiased = (header.flags & kSpzAntialiasedFlag) != 0;
    if (auto charged = context.observe(BudgetKind::splats, header.num_points, "spz.read.splats");
        !charged.has_value()) {
        failDecode(result, charged.error_code(),
                   firstDiagnosticMessage(charged.diagnostics(),
                                          "SPZ point count exceeds the splat limit"),
                   charged.diagnostics());
        return result;
    }
    if (header.num_points > kMaxUpstreamSpzPoints) {
        failDecode(result, ErrorCode::resource_limit, "SPZ point count exceeds the decoder limit");
        return result;
    }

    auto planned = makeSpzDecodePlan(header);
    if (!planned.has_value()) {
        failDecode(
            result, planned.error_code(),
            firstDiagnosticMessage(planned.diagnostics(), "SPZ decoded-size calculation failed"),
            planned.diagnostics());
        return result;
    }
    const SpzDecodePlan& plan = planned.value();
    if (auto charged = context.consume(BudgetKind::decoded_bytes, plan.packed_bytes, "spz.inflate");
        !charged.has_value()) {
        failDecode(result, charged.error_code(),
                   firstDiagnosticMessage(charged.diagnostics(),
                                          "SPZ packed data exceeds the decoded-byte limit"),
                   charged.diagnostics());
        return result;
    }
    if (auto ratio =
            context.budget->check_decompression_ratio(size, plan.packed_bytes, "spz.inflate");
        !ratio.has_value()) {
        failDecode(result, ratio.error_code(),
                   firstDiagnosticMessage(ratio.diagnostics(),
                                          "SPZ data exceeds the decompression-ratio limit"),
                   ratio.diagnostics());
        return result;
    }
    auto working_reservation = context.budget->reserve(
        BudgetKind::memory_bytes, plan.working_memory_bytes, "spz.decode_working_set");
    if (!working_reservation.has_value()) {
        failDecode(result, working_reservation.error_code(),
                   firstDiagnosticMessage(working_reservation.diagnostics(),
                                          "SPZ decode exceeds the memory limit"),
                   working_reservation.diagnostics());
        return result;
    }
    Budget::Charge working_charge = std::move(working_reservation).value();

    try {
        control = context.checkpoint({"spz.read", "inflate", 0, plan.packed_bytes, "bytes"});
        if (!control.has_value()) {
            failDecode(result, control.error_code(),
                       firstDiagnosticMessage(control.diagnostics(), "SPZ reading stopped"),
                       control.diagnostics());
            return result;
        }
        auto packed_size = checked_size_cast(plan.packed_bytes, "SPZ packed payload size");
        if (!packed_size.has_value()) {
            failDecode(result, packed_size.error_code(),
                       firstDiagnosticMessage(packed_size.diagnostics(),
                                              "SPZ packed data exceeds the host size limit"),
                       packed_size.diagnostics());
            return result;
        }
        {
            auto inflated = inflateSpzPayload(data, size, packed_size.value(), context);
            if (!inflated.has_value()) {
                failDecode(result, inflated.error_code(),
                           firstDiagnosticMessage(inflated.diagnostics(),
                                                  "Failed to inflate the SPZ data"),
                           inflated.diagnostics());
                return result;
            }
            // validateDecodeConfig proves that this fallback cannot be selected.
            const double source_unit_to_meter = config.source_unit_to_meter.value_or(0.0);
            auto canonical =
                decodeSpzPayload(inflated.value(), header, source_unit_to_meter, context);
            if (!canonical.has_value()) {
                failDecode(result, canonical.error_code(),
                           firstDiagnosticMessage(canonical.diagnostics(),
                                                  "SPZ data violates canonical scene invariants"),
                           canonical.diagnostics());
                return result;
            }
            result.data.emplace(std::move(canonical).value());
        }
        result.metadata.decoded_points = result.data->size();
        working_charge.shrink_to(plan.cloud_bytes);
        result.set_retained_memory(std::move(working_charge));
        result.success = true;
    } catch (const std::bad_alloc&) {
        failDecode(result, ErrorCode::resource_limit, "SPZ decode exceeded available memory");
    } catch (const std::length_error&) {
        failDecode(result, ErrorCode::resource_limit, "SPZ decode exceeded a container size limit");
    } catch (const std::exception& e) {
        failDecode(result, ErrorCode::internal_error,
                   std::string("Failed to decode SPZ: ") + e.what());
    }

    return result;
} catch (const std::bad_alloc&) {
    DecodeResult result;
    failDecode(result, ErrorCode::resource_limit, "SPZ buffer exceeds available memory");
    return result;
} catch (const std::length_error&) {
    DecodeResult result;
    failDecode(result, ErrorCode::resource_limit, "SPZ buffer exceeds a container size limit");
    return result;
}

}  // namespace melkor

#endif  // MELKOR_HAS_SPZ
