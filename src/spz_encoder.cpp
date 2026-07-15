// SPZ Encoder/Decoder Implementation
// Bridges canonical melkor::SplatData to spz::GaussianCloud.

#include "melkor/spz_encoder.hpp"

#ifdef MELKOR_HAS_SPZ

#include "melkor/budget.hpp"
#include "melkor/cloud_inspector.hpp"
#include "melkor/io/atomic_writer.hpp"
#include "melkor/limits.hpp"
#include "melkor/math/activation.hpp"
#include "melkor/math/quaternion.hpp"
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

constexpr float kSpzOpacityEpsilon = 1.0e-6f;

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
    return config.sh_degree < 0 ? source_degree : std::min(config.sh_degree, source_degree);
}

static std::string validateEncodeInput(const SplatData& data, const SpzEncodeConfig& config) {
    if (data.empty())
        return "Cannot encode empty splat data";
    if (config.sh_degree < -1 || config.sh_degree > 3) {
        return "Cannot encode SPZ: requested SH degree must be -1 or between 0 and 3";
    }
    if (data.sh().degree() > 3) {
        return "Cannot encode SPZ v1-v3: source SH degree exceeds 3";
    }
    const int effective_degree = effectiveDegree(data, config);
    const size_t sh_components = shRestCountForDegree(effective_degree);
    const size_t largest_signed_multiplier = std::max<size_t>({9, 4, sh_components});
    const size_t signed_allocation_limit =
        static_cast<size_t>(std::numeric_limits<int32_t>::max()) / largest_signed_multiplier;
    if (data.size() > signed_allocation_limit) {
        return "Cannot encode SPZ: point count overflows an upstream signed allocation";
    }
    // The canonical decoder rejects larger clouds, even when an individual
    // encoder-side allocation expression would still fit in int32.
    constexpr size_t kMaxSpzPoints = 10'000'000;
    if (data.size() > kMaxSpzPoints) {
        return "Cannot encode SPZ: data exceeds the 10-million-point format limit";
    }

    const CloudInspection inspection = inspectCloud(data);
    if (!inspection.valid) {
        const auto issue = std::find_if(inspection.issues.begin(), inspection.issues.end(),
                                        [](const InspectionIssue& candidate) {
                                            return candidate.severity == InspectionSeverity::Error;
                                        });
        std::string message = "Cannot encode invalid canonical splat data";
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
    for (size_t index = 0; index < data.size(); ++index) {
        const Vec3f position_value = data.positions()[index];
        for (const float position : {position_value.x, position_value.y, position_value.z}) {
            const double fixed = std::round(static_cast<double>(position) * kPositionScale);
            if (fixed < kMinFixedPosition || fixed > kMaxFixedPosition) {
                return "Cannot encode SPZ: position at splat " + std::to_string(index) +
                       " exceeds the signed 24-bit fixed-point range";
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
                const float quantized =
                    std::round(data.sh().raw()[sh_base + coefficient * 3 + channel] * 128.0f) +
                    128.0f;
                if (!std::isfinite(quantized) || quantized < min_int_float ||
                    quantized >= max_int_float) {
                    return "Cannot encode SPZ: SH coefficient at splat " + std::to_string(index) +
                           " exceeds the safe quantization range";
                }
            }
        }

        for (const float scale :
             {data.scales()[index].x, data.scales()[index].y, data.scales()[index].z}) {
            if (!math::log_scale_from_linear(scale).has_value()) {
                return "Cannot encode SPZ: scale is outside the log-domain boundary";
            }
        }
    }
    return {};
}

static spz::GaussianCloud toSpzCloud(const SplatData& data, int sh_degree) {
    spz::GaussianCloud spz_cloud;
    spz_cloud.numPoints = static_cast<int32_t>(data.size());
    const int effective_degree = sh_degree < 0
                                     ? static_cast<int>(data.sh().degree())
                                     : std::min(sh_degree, static_cast<int>(data.sh().degree()));
    spz_cloud.shDegree = effective_degree;
    spz_cloud.antialiased = false;

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
        const Vec3f position = data.positions()[i];
        const Vec3f scale = data.scales()[i];
        const Quatf rotation = data.rotations()[i];

        // Position (x, y, z)
        spz_cloud.positions.push_back(position.x);
        spz_cloud.positions.push_back(position.y);
        spz_cloud.positions.push_back(position.z);

        spz_cloud.scales.push_back(math::log_scale_from_linear(scale.x).value());
        spz_cloud.scales.push_back(math::log_scale_from_linear(scale.y).value());
        spz_cloud.scales.push_back(math::log_scale_from_linear(scale.z).value());

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
        spz_cloud.colors.push_back(data.sh().dc(i, 0));
        spz_cloud.colors.push_back(data.sh().dc(i, 1));
        spz_cloud.colors.push_back(data.sh().dc(i, 2));

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

    return spz_cloud;
}

static std::string validateSpzCloudLayout(const spz::GaussianCloud& cloud) {
    if (cloud.numPoints < 0)
        return "SPZ declares a negative point count";
    if (cloud.shDegree < 0 || cloud.shDegree > 3)
        return "SPZ declares an unsupported SH degree";
    const size_t count = static_cast<size_t>(cloud.numPoints);
    const auto shorterThan = [&](size_t actual, size_t components) {
        return components != 0 && (count > std::numeric_limits<size_t>::max() / components ||
                                   actual < count * components);
    };
    if (shorterThan(cloud.positions.size(), 3) || shorterThan(cloud.scales.size(), 3) ||
        shorterThan(cloud.rotations.size(), 4) || cloud.alphas.size() < count ||
        shorterThan(cloud.colors.size(), 3) ||
        shorterThan(cloud.sh.size(), shRestCountForDegree(cloud.shDegree))) {
        return "SPZ decoded arrays are shorter than the declared point count";
    }
    // Reject a non-finite decoded value. Crafted input can drive the decoder to a NaN/inf -- e.g. a
    // v3 "smallest-three" quaternion whose stored components sum to more than 1 makes the recovered
    // component sqrt of a negative -- and the layout check above would otherwise pass it as a valid
    // decode. Guarding at the boundary catches every such source, not one function.
    const auto any_non_finite = [](const std::vector<float>& v) {
        for (float x : v) {
            if (!std::isfinite(x))
                return true;
        }
        return false;
    };
    if (any_non_finite(cloud.positions) || any_non_finite(cloud.scales) ||
        any_non_finite(cloud.rotations) || any_non_finite(cloud.alphas) ||
        any_non_finite(cloud.colors) || any_non_finite(cloud.sh)) {
        return "SPZ decoded a non-finite value (NaN or infinity), likely from corrupt or crafted "
               "input";
    }
    return {};
}

static Result<SplatData> fromSpzCloud(const spz::GaussianCloud& spz_cloud) {
    const std::size_t count = static_cast<std::size_t>(spz_cloud.numPoints);
    const std::uint32_t degree = static_cast<std::uint32_t>(spz_cloud.shDegree);
    const std::size_t coefficients = static_cast<std::size_t>(degree + 1) * (degree + 1);

    SplatBufferInput input;
    input.positions.resize(count);
    input.scales.resize(count);
    input.rotations.resize(count);
    input.opacities.resize(count);
    std::vector<float> sh_values(count * coefficients * 3, 0.0f);

    for (std::size_t i = 0; i < count; ++i) {
        input.positions[i] = {spz_cloud.positions[i * 3], spz_cloud.positions[i * 3 + 1],
                              spz_cloud.positions[i * 3 + 2]};

        float* scale_components[3] = {&input.scales[i].x, &input.scales[i].y, &input.scales[i].z};
        for (std::size_t component = 0; component < 3; ++component) {
            auto scale = math::linear_scale_from_log(spz_cloud.scales[i * 3 + component]);
            if (!scale.has_value()) {
                auto diagnostics = scale.diagnostics();
                for (auto& diagnostic : diagnostics) {
                    diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i))
                        .with_context("component", static_cast<std::uint64_t>(component));
                }
                return Result<SplatData>::failure(scale.error_code(), std::move(diagnostics));
            }
            *scale_components[component] = scale.value();
        }

        const math::Quat source_rotation{spz_cloud.rotations[i * 4], spz_cloud.rotations[i * 4 + 1],
                                         spz_cloud.rotations[i * 4 + 2],
                                         spz_cloud.rotations[i * 4 + 3]};
        if (!math::is_unit(source_rotation)) {
            Diagnostic diagnostic("MK1320_SPZ_NON_UNIT_ROTATION", Severity::error,
                                  "SPZ rotation is not unit within the canonical tolerance");
            diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i))
                .with_context("norm", math::norm(source_rotation));
            return Result<SplatData>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
        auto normalized = math::normalize(source_rotation);
        if (!normalized.has_value()) {
            return Result<SplatData>::failure(normalized.error_code(), normalized.diagnostics());
        }
        const auto rotation = normalized.value();
        input.rotations[i] = {static_cast<float>(rotation.x), static_cast<float>(rotation.y),
                              static_cast<float>(rotation.z), static_cast<float>(rotation.w)};

        auto opacity = math::sigmoid_from_logit(spz_cloud.alphas[i]);
        if (!opacity.has_value()) {
            auto diagnostics = opacity.diagnostics();
            for (auto& diagnostic : diagnostics) {
                diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i));
            }
            return Result<SplatData>::failure(opacity.error_code(), std::move(diagnostics));
        }
        input.opacities[i] = opacity.value();

        const std::size_t sh_base = i * coefficients * 3;
        sh_values[sh_base] = spz_cloud.colors[i * 3];
        sh_values[sh_base + 1] = spz_cloud.colors[i * 3 + 1];
        sh_values[sh_base + 2] = spz_cloud.colors[i * 3 + 2];
        const std::size_t source_sh_base = i * (coefficients - 1) * 3;
        for (std::size_t coefficient = 1; coefficient < coefficients; ++coefficient) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                sh_values[sh_base + coefficient * 3 + channel] =
                    spz_cloud.sh[source_sh_base + (coefficient - 1) * 3 + channel];
            }
        }
    }

    auto sh = ShBuffer::create(degree, count, std::move(sh_values));
    if (!sh.has_value()) {
        return Result<SplatData>::failure(sh.error_code(), sh.diagnostics());
    }
    input.sh = std::move(sh).value();
    return SplatData::create(std::move(input));
}

// ============================================================================
// SpzEncoder Implementation
// ============================================================================

SpzEncoder::SpzEncoder() = default;
SpzEncoder::~SpzEncoder() = default;

SpzEncodeResult SpzEncoder::encodeToFile(const std::string& filepath, const SplatData& data,
                                         const SpzEncodeConfig& config) {
    std::vector<uint8_t> buffer;
    SpzEncodeResult encoded = encodeToBuffer(buffer, data, config);
    if (!encoded.success)
        return encoded;

    SpzEncodeResult result;
    result.diagnostics = std::move(encoded.diagnostics);

    // Route through the atomic writer. The previous implementation opened the destination
    // with std::ios::trunc -- destroying the user's existing file before a single byte of the
    // new one was written -- and then called std::remove(filepath) from each error handler.
    // An encode that failed partway through therefore truncated the good file and then
    // deleted it, leaving the user with neither the new asset nor the old one. That is
    // release blocker P0-08, and it is the reason AtomicWriter exists.
    //
    // Now: the bytes go to an exclusively-created temporary in the same directory, and the
    // destination is replaced atomically only after the write has fully succeeded. Any
    // failure leaves the pre-existing file byte-for-byte intact.
    melkor::Budget budget(melkor::Limits::for_profile(melkor::LimitsProfile::desktop));
    melkor::OperationContext context = melkor::make_default_context(budget);

    melkor::io::WriteOptions options;
    // The legacy entry point has no --force plumbing yet; CLI v2 (WP15) makes overwrite an
    // explicit user decision. Preserving the historical "replace the output" behaviour here
    // keeps this a pure data-safety fix rather than a silent contract change.
    options.overwrite = true;

    auto writer = melkor::io::AtomicWriter::create(filepath, options, context);
    if (!writer.has_value()) {
        result.error_message = writer.diagnostics().empty() ? "Failed to open SPZ file for writing"
                                                            : writer.diagnostics()[0].message;
        return result;
    }

    auto written = writer.value()->write(buffer.data(), buffer.size());
    if (!written.has_value()) {
        // The destination is untouched. The temporary is removed by the destructor.
        result.error_message = written.diagnostics().empty() ? "Failed to write complete SPZ file"
                                                             : written.diagnostics()[0].message;
        return result;
    }

    auto committed = writer.value()->commit();
    if (!committed.has_value()) {
        result.error_message = committed.diagnostics().empty() ? "Failed to commit SPZ file"
                                                               : committed.diagnostics()[0].message;
        return result;
    }

    result.success = true;
    result.bytes_written = buffer.size();
    return result;
}

SpzEncodeResult SpzEncoder::encodeToBuffer(std::vector<uint8_t>& buffer, const SplatData& data,
                                           const SpzEncodeConfig& config) {
    SpzEncodeResult result;
    buffer.clear();

    if (const std::string validation_error = validateEncodeInput(data, config);
        !validation_error.empty()) {
        result.error_message = validation_error;
        return result;
    }

    const int encoded_degree = effectiveDegree(data, config);
    if (encoded_degree < static_cast<int>(data.sh().degree())) {
        Diagnostic diagnostic("MK1322_SPZ_SH_TRUNCATED", Severity::warning,
                              "Higher spherical-harmonic coefficients were omitted for SPZ output");
        diagnostic.with_context("source_degree", static_cast<std::uint64_t>(data.sh().degree()))
            .with_context("encoded_degree", static_cast<std::uint64_t>(encoded_degree));
        result.diagnostics.push_back(std::move(diagnostic));
    }
    std::size_t endpoint_count = 0;
    for (float opacity : data.opacities()) {
        if (opacity <= 0.0f || opacity >= 1.0f)
            ++endpoint_count;
    }
    if (endpoint_count != 0) {
        Diagnostic diagnostic(
            "MK1321_SPZ_OPACITY_ENDPOINT_CLAMPED", Severity::warning,
            "Canonical opacity endpoints were clamped to finite values for the SPZ logit domain");
        diagnostic.with_context("splat_count", static_cast<std::uint64_t>(endpoint_count))
            .with_context("epsilon", static_cast<double>(kSpzOpacityEpsilon));
        result.diagnostics.push_back(std::move(diagnostic));
    }

    try {
        // Convert to SPZ cloud
        spz::GaussianCloud spz_cloud = toSpzCloud(data, config.sh_degree);

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

SpzDecoder::DecodeResult SpzDecoder::decodeFromFile(const std::string& filepath,
                                                    const Limits& limits) {
    DecodeResult result;
    std::ifstream stream(filepath, std::ios::binary | std::ios::ate);
    if (!stream) {
        result.error_message = "Failed to open SPZ file";
        return result;
    }
    const std::streamoff end = stream.tellg();
    if (end <= 0 ||
        static_cast<uintmax_t>(end) > static_cast<uintmax_t>(std::numeric_limits<int32_t>::max())) {
        result.error_message = "SPZ file is empty or exceeds the 2 GiB decoder limit";
        return result;
    }
    Budget budget(limits);
    if (auto charged =
            budget.consume(BudgetKind::input_bytes, static_cast<std::uint64_t>(end), "spz.file");
        !charged.has_value()) {
        result.error_message = charged.diagnostics().empty()
                                   ? "SPZ file exceeds the input-size limit"
                                   : charged.diagnostics()[0].message;
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
    return decodeFromBuffer(data.data(), data.size(), limits);
}

SpzDecoder::DecodeResult SpzDecoder::decodeFromBuffer(const uint8_t* data, size_t size,
                                                      const Limits& limits) {
    DecodeResult result;
    if (data == nullptr || size == 0 ||
        size > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        result.error_message =
            "SPZ input buffer is null, empty, or exceeds the 2 GiB decoder limit";
        return result;
    }
    // Charge the compressed input against the budget before the decode. This bounds how large an
    // SPZ stream will even be attempted; the decoded allocation itself happens inside vendored
    // upstream (after a whole-stream inflate, capped by upstream at 10M points) and a tighter
    // per-file bound needs a header peek, tracked with the SPZ v4 upgrade (P0-09).
    Budget budget(limits);
    if (auto charged =
            budget.consume(BudgetKind::input_bytes, static_cast<std::uint64_t>(size), "spz.input");
        !charged.has_value()) {
        result.error_message = charged.diagnostics().empty()
                                   ? "SPZ input exceeds the input-size limit"
                                   : charged.diagnostics()[0].message;
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

        const std::size_t count = static_cast<std::size_t>(spz_cloud.numPoints);
        if (auto charged = budget.consume(BudgetKind::splats, count, "spz.points");
            !charged.has_value()) {
            result.error_message = charged.diagnostics().empty()
                                       ? "SPZ point count exceeds the splat limit"
                                       : charged.diagnostics()[0].message;
            return result;
        }
        const std::uint64_t coefficient_count = static_cast<std::uint64_t>(spz_cloud.shDegree + 1) *
                                                static_cast<std::uint64_t>(spz_cloud.shDegree + 1);
        const std::uint64_t per_splat_bytes = 2 * sizeof(Vec3f) + sizeof(Quatf) + sizeof(float) +
                                              coefficient_count * 3 * sizeof(float);
        if (auto charged = budget.consume(BudgetKind::memory_bytes,
                                          static_cast<std::uint64_t>(count) * per_splat_bytes,
                                          "spz.canonical_data");
            !charged.has_value()) {
            result.error_message = charged.diagnostics().empty()
                                       ? "SPZ canonical data exceeds the memory limit"
                                       : charged.diagnostics()[0].message;
            return result;
        }
        auto canonical = fromSpzCloud(spz_cloud);
        if (!canonical.has_value()) {
            result.error_message = canonical.diagnostics().empty()
                                       ? "SPZ data violates canonical scene invariants"
                                       : canonical.diagnostics()[0].message;
            return result;
        }
        result.data.emplace(std::move(canonical).value());
        result.metadata.decoded_points = result.data->size();
        result.success = true;
    } catch (const std::exception& e) {
        result.error_message = std::string("Failed to decode SPZ: ") + e.what();
    }

    return result;
}

}  // namespace melkor

#endif  // MELKOR_HAS_SPZ
