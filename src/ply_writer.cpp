#include "melkor/ply_writer.hpp"

#include "melkor/budget.hpp"
#include "melkor/checked.hpp"
#include "melkor/io/atomic_writer.hpp"
#include "melkor/limits.hpp"
#include "melkor/math/activation.hpp"
#include "melkor/math/coordinate_frame.hpp"
#include "melkor/math/quaternion.hpp"
#include "melkor/math/sh_rotation.hpp"

#include "io/input_file.hpp"
#include "safe_text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <locale>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace melkor {
namespace {

void writeFloatLittleEndian(std::ostream& stream, float value) {
    static_assert(sizeof(float) == sizeof(uint32_t), "PLY requires IEEE-754 32-bit floats");
    static_assert(std::numeric_limits<float>::is_iec559,
                  "PLY requires IEEE-754 floating-point semantics");
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const char bytes[4] = {
        static_cast<char>(bits & 0xffu),
        static_cast<char>((bits >> 8u) & 0xffu),
        static_cast<char>((bits >> 16u) & 0xffu),
        static_cast<char>((bits >> 24u) & 0xffu),
    };
    stream.write(bytes, sizeof(bytes));
}

std::size_t writeAsciiFloat(std::ostream& stream, float value) {
    char text[64];
    std::size_t length = 0;
    if (!text::formatClassicFloat(value, text, sizeof(text), length)) {
        stream.setstate(std::ios::failbit);
        return 0;
    }
    stream.write(text, static_cast<std::streamsize>(length));
    return length;
}

std::string sanitizeComment(const std::string& comment, std::size_t& replaced_bytes) {
    std::string sanitized = comment;
    replaced_bytes = 0;
    for (char& byte : sanitized) {
        const unsigned char value = static_cast<unsigned char>(byte);
        if (value < 0x20u || value > 0x7eu) {
            byte = ' ';
            ++replaced_bytes;
        }
    }
    return sanitized;
}

// The graphdeco PLY profile stores opacity in the unbounded training domain. Canonical endpoint
// probabilities have infinite logits, so a writer must either reject them or make the adjustment
// explicit. We preserve the useful 0/1 scene values with the same finite epsilon historically used
// by the format adapters, while surfacing one structured warning for the write.
constexpr float kPlyOpacityEpsilon = 1.0e-6f;
constexpr float kTrainingScaleFloor = std::numeric_limits<float>::min();
constexpr std::uint64_t kPlyHeaderWorkingMultiplier = 12;
constexpr std::uint64_t kPlyHeaderWorkingBase = std::uint64_t{16} * 1024;
constexpr std::uint64_t kPlyWriterHeaderBase = std::uint64_t{8} * 1024;
constexpr std::uint64_t kPlyWriterHeaderCopies = 3;

Result<float> encodePlyOpacity(float opacity, bool& clamped) {
    float encodable = opacity;
    if (encodable <= 0.0f) {
        encodable = kPlyOpacityEpsilon;
        clamped = true;
    } else if (encodable >= 1.0f) {
        encodable = 1.0f - kPlyOpacityEpsilon;
        clamped = true;
    }
    return math::logit_from_probability(encodable);
}

float trainingScale(float scale) noexcept {
    return scale == 0.0f ? kTrainingScaleFloor : scale;
}

bool isValidPlyFormat(PlyFormat format) noexcept {
    return format == PlyFormat::Binary || format == PlyFormat::Ascii;
}

bool usesGraphdecoLayout(FormatProfileId profile) noexcept {
    return profile == FormatProfileId::ply_graphdeco_3dgs_v1 ||
           profile == FormatProfileId::ply_da3_gaussian_v1;
}

const char* plyProfileMarker(FormatProfileId profile) noexcept {
    switch (profile) {
    case FormatProfileId::ply_melkor_canonical_v1:
        return "melkor-canonical-v1";
    case FormatProfileId::ply_graphdeco_3dgs_v1:
        return "graphdeco-3dgs-v1";
    case FormatProfileId::ply_da3_gaussian_v1:
        return "da3-gaussian-v1";
    default:
        return "unknown";
    }
}

bool isFloatRepresentable(double value) noexcept {
    constexpr double limit = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -limit || value > limit)
        return false;
    const float stored = static_cast<float>(value);
    return value == 0.0 || stored != 0.0f;
}

std::string firstDiagnosticMessage(const std::vector<Diagnostic>& diagnostics,
                                   const char* fallback) {
    return diagnostics.empty() ? fallback
                               : diagnostics.front().code + ": " + diagnostics.front().message;
}

void failPlyWrite(PlyWriteResult& result, ErrorCode code, std::string message) {
    result.success = false;
    result.set_retained_memory({});
    result.failure_code = code == ErrorCode::ok ? ErrorCode::internal_error : code;
    result.error_message = std::move(message);
    result.diagnostics.emplace_back("MK1216_PLY_WRITE_FAILED", Severity::error,
                                    result.error_message.empty() ? "the PLY writer failed"
                                                                 : result.error_message);
}

void failPlyWrite(PlyWriteResult& result, ErrorCode code,
                  const std::vector<Diagnostic>& diagnostics, const char* fallback) {
    result.success = false;
    result.set_retained_memory({});
    result.failure_code = code == ErrorCode::ok ? ErrorCode::internal_error : code;
    result.error_message = firstDiagnosticMessage(diagnostics, fallback);
    if (diagnostics.empty()) {
        result.diagnostics.emplace_back("MK1216_PLY_WRITE_FAILED", Severity::error,
                                        result.error_message);
    } else {
        result.diagnostics.insert(result.diagnostics.end(), diagnostics.begin(), diagnostics.end());
    }
}

template <class T>
void failPlyWrite(PlyWriteResult& result, const Result<T>& failure, const char* fallback) {
    failPlyWrite(result, failure.error_code(), failure.diagnostics(), fallback);
}

PlyWriteResult plyWriteFailure(ErrorCode code, std::string message) {
    PlyWriteResult result;
    failPlyWrite(result, code, std::move(message));
    return result;
}

bool checkPlyWrite(PlyWriteResult& result, const OperationContext& context, const char* phase,
                   std::uint64_t completed, std::uint64_t total) {
    auto control = context.checkpoint({"ply.write", phase, completed, total, "splats"});
    if (control.has_value())
        return true;
    failPlyWrite(result, control, "PLY write stopped");
    return false;
}

PlyReader::ReadResult plyReadControlFailure(const Result<void>& control) {
    return {false,
            firstDiagnosticMessage(control.diagnostics(), "PLY read stopped"),
            {},
            {},
            control.error_code(),
            {},
            control.diagnostics()};
}

template <class T>
PlyReader::ReadResult plyReadFailure(const Result<T>& failure, const char* fallback,
                                     LossReport losses = {}) {
    std::vector<Diagnostic> diagnostics = failure.diagnostics();
    return {false,
            firstDiagnosticMessage(diagnostics, fallback),
            {},
            {},
            failure.error_code(),
            std::move(losses),
            std::move(diagnostics)};
}

class ScopedBudgetRelease {
public:
    ScopedBudgetRelease(Budget* budget, BudgetKind kind, std::uint64_t amount) noexcept
        : budget_(budget), kind_(kind), amount_(amount) {}

    ~ScopedBudgetRelease() {
        if (budget_ != nullptr)
            budget_->release(kind_, amount_);
    }

    ScopedBudgetRelease(const ScopedBudgetRelease&) = delete;
    ScopedBudgetRelease& operator=(const ScopedBudgetRelease&) = delete;

    ScopedBudgetRelease(ScopedBudgetRelease&& other) noexcept
        : budget_(other.budget_), kind_(other.kind_), amount_(other.amount_) {
        other.budget_ = nullptr;
    }

    ScopedBudgetRelease& operator=(ScopedBudgetRelease&&) = delete;

    void retain(std::uint64_t amount) noexcept {
        if (budget_ == nullptr)
            return;
        if (amount < amount_)
            budget_->release(kind_, amount_ - amount);
        budget_ = nullptr;
    }

private:
    Budget* budget_;
    BudgetKind kind_;
    std::uint64_t amount_;
};

bool estimatePlyOutputBytes(const SplatData& data, const PlyWriteConfig& config,
                            std::uint64_t& bytes) {
    const FormatProfile& profile = format_profile(config.profile);
    if (!profile.supports_write_container(FormatId::ply))
        return false;
    const std::uint32_t requested_degree =
        config.sh_degree < 0 ? profile.max_sh_degree : static_cast<std::uint32_t>(config.sh_degree);
    const std::uint32_t output_degree =
        config.include_sh_rest
            ? std::min({data.sh().degree(), static_cast<std::uint32_t>(profile.max_sh_degree),
                        requested_degree})
            : 0;
    const std::uint64_t coefficients =
        static_cast<std::uint64_t>(output_degree + 1) * (output_degree + 1);
    const std::uint64_t scalar_count = config.profile == FormatProfileId::ply_melkor_canonical_v1
                                           ? 11 + coefficients * 3
                                           : 17 + (coefficients - 1) * 3;
    const std::uint64_t record_bytes =
        config.format == PlyFormat::Binary ? scalar_count * sizeof(float) : scalar_count * 33 + 1;
    const std::uint64_t header_bytes = 8192 + config.comment.size();
    if (data.size() > (std::numeric_limits<std::uint64_t>::max() - header_bytes) / record_bytes)
        return false;
    bytes = header_bytes + static_cast<std::uint64_t>(data.size()) * record_bytes;
    return true;
}

bool has_identity_basis(const math::CoordinateFrame& frame) {
    return frame.to_canonical == math::canonical_frame().to_canonical;
}

bool is_canonical_frame(const math::CoordinateFrame& frame) {
    return frame.unit_to_meter == 1.0 && has_identity_basis(frame);
}

math::Mat3 transpose_scaled(const math::Mat3& matrix, double scale) {
    return math::Mat3{matrix[0] * scale, matrix[3] * scale, matrix[6] * scale,
                      matrix[1] * scale, matrix[4] * scale, matrix[7] * scale,
                      matrix[2] * scale, matrix[5] * scale, matrix[8] * scale};
}

std::string canonicalShProperty(std::uint32_t coefficient, std::size_t channel) {
    std::uint32_t degree = 0;
    while ((degree + 1) * (degree + 1) <= coefficient)
        ++degree;
    const std::uint32_t band_index = coefficient - degree * degree;
    const char channel_name[3] = {'r', 'g', 'b'};
    return "sh_" + std::to_string(degree) + "_" + std::to_string(band_index) + "_" +
           channel_name[channel];
}

bool appendLoss(PlyWriteResult& result, LossItem item) {
    auto added = result.losses.add(std::move(item));
    if (added.has_value())
        return true;
    failPlyWrite(result, added, "Invalid PLY loss report");
    return false;
}

struct EncodedPlySplat {
    Vec3f position;
    Vec3f scale;
    Quatf rotation;
    float opacity = 0.0f;
    std::array<float, 75> sh{};
};

Result<EncodedPlySplat> encodePlySplat(const SplatData& data, std::size_t splat,
                                       std::uint32_t output_degree,
                                       const math::CoordinateFrame& target_frame,
                                       const math::ShRotation* sh_rotation) {
    EncodedPlySplat encoded;
    const Vec3f& position = data.positions()[splat];
    const Vec3f& scale = data.scales()[splat];
    const Quatf& rotation = data.rotations()[splat];

    if (is_canonical_frame(target_frame)) {
        encoded.position = position;
        encoded.scale = scale;
        encoded.rotation = rotation;
    } else {
        const math::Vec3 source_position{position.x, position.y, position.z};
        const math::Vec3 target_position =
            math::position_from_canonical(target_frame, source_position);
        auto target_rotation = math::rotation_from_canonical(
            target_frame, math::Quat{rotation.x, rotation.y, rotation.z, rotation.w});
        if (!target_rotation.has_value()) {
            return Result<EncodedPlySplat>::failure(target_rotation.error_code(),
                                                    target_rotation.diagnostics());
        }
        const math::Vec3 target_scale{
            static_cast<double>(scale.x) / target_frame.unit_to_meter,
            static_cast<double>(scale.y) / target_frame.unit_to_meter,
            static_cast<double>(scale.z) / target_frame.unit_to_meter,
        };
        if (!std::all_of(target_position.begin(), target_position.end(), isFloatRepresentable) ||
            !std::all_of(target_scale.begin(), target_scale.end(), isFloatRepresentable)) {
            Diagnostic diagnostic("MK1214_PLY_TRANSFORM_RANGE", Severity::error,
                                  "a transformed PLY splat exceeds the target float range");
            return Result<EncodedPlySplat>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
        encoded.position =
            Vec3f{static_cast<float>(target_position[0]), static_cast<float>(target_position[1]),
                  static_cast<float>(target_position[2])};
        encoded.scale =
            Vec3f{static_cast<float>(target_scale[0]), static_cast<float>(target_scale[1]),
                  static_cast<float>(target_scale[2])};
        encoded.rotation = Quatf{static_cast<float>(target_rotation.value().x),
                                 static_cast<float>(target_rotation.value().y),
                                 static_cast<float>(target_rotation.value().z),
                                 static_cast<float>(target_rotation.value().w)};
    }
    encoded.opacity = data.opacities()[splat];

    const std::size_t source_coefficients = data.sh().coefficients();
    const std::size_t output_coefficients =
        static_cast<std::size_t>(output_degree + 1) * (output_degree + 1);
    const std::size_t source_offset = splat * source_coefficients * 3;
    std::copy_n(data.sh().raw().data() + source_offset, output_coefficients * 3, encoded.sh.data());
    if (sh_rotation != nullptr) {
        auto rotated = sh_rotation->rotate_block(encoded.sh.data(), 3);
        if (!rotated.has_value()) {
            return Result<EncodedPlySplat>::failure(rotated.error_code(), rotated.diagnostics());
        }
    }
    return Result<EncodedPlySplat>::success(encoded);
}

}  // namespace

PlyWriter::PlyWriter() = default;

PlyWriteResult PlyWriter::writeToFile(const std::string& filepath, const SplatData& data,
                                      const PlyWriteConfig& config) try {
    Budget budget(config.limits);
    OperationContext context = make_default_context(budget);
    return writeToFile(filepath, data, config, context);
} catch (const std::bad_alloc&) {
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded available memory");
} catch (const std::length_error&) {
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded a container size limit");
}

PlyWriteResult PlyWriter::writeToFile(const std::string& filepath, const SplatData& data,
                                      const PlyWriteConfig& requested,
                                      const OperationContext& context) try {
    // Route through the atomic writer. Opening the destination directly with std::ofstream
    // truncates it on open, so a failure partway through a write left the user with a
    // half-written file where their good one used to be (P0-08).
    //
    // The bytes stream into an exclusively-created temporary in the same directory; the
    // destination is replaced atomically only after the write fully succeeds. Streaming --
    // rather than buffering the result and writing it in one go -- matters here: a
    // 25-million-splat PLY is several gigabytes, and trading a data-loss bug for an
    // out-of-memory bug would not be a fix.
    if (context.budget == nullptr) {
        return plyWriteFailure(ErrorCode::internal_error,
                               "The PLY writer requires a resource budget");
    }
    PlyWriteConfig config = requested;
    config.limits = context.budget->limits();
    if (!isValidPlyFormat(config.format)) {
        return plyWriteFailure(ErrorCode::invalid_argument, "The PLY encoding is invalid");
    }
    if (auto valid = config.limits.validate(); !valid.has_value()) {
        PlyWriteResult failure;
        failPlyWrite(failure, valid, "Invalid resource limits");
        return failure;
    }
    PlyWriteResult control_result;
    if (!checkPlyWrite(control_result, context, "prepare", 0, data.size()))
        return control_result;

    melkor::io::WriteOptions options;
    options.overwrite = config.overwrite;

    auto writer = melkor::io::AtomicWriter::create(filepath, options, context);
    if (!writer.has_value()) {
        PlyWriteResult failure;
        failPlyWrite(failure, writer, "Failed to open the PLY output file");
        return failure;
    }

    PlyWriteResult result;
    {
        melkor::io::AtomicOutputStream stream(*writer.value());
        result = writeToStream(stream, data, config, context);

        stream.flush();

        // An ostream swallows write failures by design. Committing after one would install a
        // silently truncated file, so the stream's own error state is checked explicitly
        // rather than trusting that writeToStream noticed.
        if (stream.failed()) {
            PlyWriteResult failure;
            failPlyWrite(failure,
                         stream.error_code() == ErrorCode::ok ? ErrorCode::io_error
                                                              : stream.error_code(),
                         stream.diagnostics(), "Failed writing PLY data");
            return failure;
        }
    }

    if (!result.success) {
        // The destination is untouched; the temporary is removed by the destructor.
        return result;
    }

    auto committed = writer.value()->commit();
    if (!committed.has_value()) {
        PlyWriteResult failure;
        failPlyWrite(failure, committed, "Failed to commit the PLY file");
        return failure;
    }

    result.bytes_written = writer.value()->bytes_written();
    return result;
} catch (const std::bad_alloc&) {
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded available memory");
} catch (const std::length_error&) {
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded a container size limit");
} catch (const std::filesystem::filesystem_error&) {
    return plyWriteFailure(ErrorCode::io_error, "Failed to access the PLY output file");
} catch (const std::ios_base::failure&) {
    return plyWriteFailure(ErrorCode::io_error, "The PLY output stream failed");
} catch (const std::exception& error) {
    return plyWriteFailure(ErrorCode::internal_error,
                           std::string("Unexpected PLY write failure: ") + error.what());
}

PlyWriteResult PlyWriter::writeToStream(std::ostream& stream, const SplatData& data,
                                        const PlyWriteConfig& config) try {
    Budget budget(config.limits);
    OperationContext context = make_default_context(budget);
    return writeToStream(stream, data, config, context);
} catch (const std::bad_alloc&) {
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded available memory");
} catch (const std::length_error&) {
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded a container size limit");
}

PlyWriteResult PlyWriter::writeToStream(std::ostream& stream, const SplatData& data,
                                        const PlyWriteConfig& requested,
                                        const OperationContext& context) try {
    PlyWriteResult result;
    if (context.budget == nullptr) {
        failPlyWrite(result, ErrorCode::internal_error,
                     "The PLY writer requires a resource budget");
        return result;
    }
    PlyWriteConfig config = requested;
    config.limits = context.budget->limits();
    result.profile = config.profile;
    if (!isValidPlyFormat(config.format)) {
        failPlyWrite(result, ErrorCode::invalid_argument, "The PLY encoding is invalid");
        return result;
    }
    if (!checkPlyWrite(result, context, "prepare", 0, data.size()))
        return result;
    if (auto valid = config.limits.validate(); !valid.has_value()) {
        failPlyWrite(result, valid, "Invalid resource limits");
        return result;
    }
    if (!stream.good()) {
        failPlyWrite(result, ErrorCode::io_error, "The PLY output stream is not writable");
        return result;
    }
    const FormatProfile& profile = format_profile(config.profile);
    if (!profile.supports_write_container(FormatId::ply)) {
        failPlyWrite(result, ErrorCode::unsupported_feature,
                     "The selected profile does not support PLY output");
        return result;
    }
    if (!config.color_space.has_value() || !is_valid(*config.color_space)) {
        failPlyWrite(result, ErrorCode::invalid_argument,
                     "PLY output requires a valid color space");
        return result;
    }
    if (!config.comment.empty() && config.comment.rfind("melkor_", 0) == 0) {
        failPlyWrite(result, ErrorCode::invalid_argument,
                     "A user PLY comment must not use the reserved melkor_ prefix");
        return result;
    }
    if (config.comment.size() > config.limits.max_metadata_string_bytes) {
        failPlyWrite(result, ErrorCode::resource_limit,
                     "PLY comment exceeds the metadata string limit");
        return result;
    }
    if (config.sh_degree < -1 || config.sh_degree > 4) {
        failPlyWrite(result, ErrorCode::invalid_argument,
                     "The requested PLY SH degree must be -1 or between 0 and 4");
        return result;
    }
    if (auto charged = context.observe(BudgetKind::splats, data.size(), "ply.write.splats");
        !charged.has_value()) {
        failPlyWrite(result, charged, "PLY splat count exceeds the limit");
        return result;
    }
    if (auto valid = data.validate(context); !valid.has_value()) {
        failPlyWrite(result, valid, "Invalid canonical splat data");
        return result;
    }

    const std::uint32_t degree = data.sh().degree();
    const std::uint32_t requested_degree =
        config.sh_degree < 0 ? profile.max_sh_degree : static_cast<std::uint32_t>(config.sh_degree);
    const std::uint32_t output_degree =
        config.include_sh_rest
            ? std::min(
                  {degree, static_cast<std::uint32_t>(profile.max_sh_degree), requested_degree})
            : 0;
    const std::size_t output_coefficients =
        static_cast<std::size_t>(output_degree + 1) * (output_degree + 1);
    const int sh_rest_count = static_cast<int>((output_coefficients - 1) * 3);

    if ((!config.include_sh_rest || requested_degree == 0) && degree > 0) {
        LossItem item;
        item.code = loss_code::kShCoefficientsDropped;
        item.severity = LossSeverity::severe;
        item.source_feature = "spherical harmonics degree " + std::to_string(degree);
        item.target_constraint = "the requested PLY output contains degree 0 only";
        item.affected_splats = data.size();
        item.remediation = "remove the option or approve LOSS_SH_COEFFICIENTS_DROPPED";
        if (!appendLoss(result, std::move(item)))
            return result;
    } else if (output_degree < degree) {
        LossItem item;
        item.code = loss_code::kShDegreeTruncated;
        item.severity = LossSeverity::severe;
        item.source_feature = "spherical harmonics degree " + std::to_string(degree);
        item.target_constraint =
            "the requested PLY output supports degree 0-" + std::to_string(output_degree);
        item.affected_splats = data.size();
        item.remediation = "select a profile with the source degree or approve "
                           "LOSS_SH_DEGREE_TRUNCATED";
        if (!appendLoss(result, std::move(item)))
            return result;
    }

    math::CoordinateFrame target_frame = math::canonical_frame();
    if (config.profile == FormatProfileId::ply_melkor_canonical_v1) {
        if (config.target_frame_id.has_value() && *config.target_frame_id != "gltf-luf") {
            failPlyWrite(result, ErrorCode::invalid_argument,
                         "The canonical PLY profile requires the gltf-luf frame");
            return result;
        }
    } else if (config.profile == FormatProfileId::ply_da3_gaussian_v1) {
        if (config.target_frame_id.has_value() && *config.target_frame_id != "ply-rdf") {
            failPlyWrite(result, ErrorCode::invalid_argument,
                         "The DA3 PLY profile requires the ply-rdf frame");
            return result;
        }
        target_frame = math::frame_by_id("ply-rdf").value();
    } else if (config.profile == FormatProfileId::ply_graphdeco_3dgs_v1) {
        if (!config.target_frame_id.has_value()) {
            failPlyWrite(result, ErrorCode::invalid_argument,
                         "Training-layout PLY output requires an explicit target frame");
            return result;
        }
        auto frame = math::frame_by_id(*config.target_frame_id);
        if (!frame.has_value()) {
            failPlyWrite(result, frame, "Invalid PLY target frame");
            return result;
        }
        target_frame = std::move(frame.value());
    } else {
        failPlyWrite(result, ErrorCode::unsupported_feature,
                     "Melkor does not write generic point-cloud PLY data");
        return result;
    }
    if (target_frame.includes_reflection) {
        failPlyWrite(result, ErrorCode::unsupported_feature,
                     "PLY output does not support a reflecting target frame");
        return result;
    }

    std::optional<math::ShRotation> sh_rotation;
    if (!has_identity_basis(target_frame) && output_degree > 0) {
        const math::Mat3 canonical_to_target = transpose_scaled(target_frame.to_canonical, 1.0);
        auto rotation = math::ShRotation::create(canonical_to_target, output_degree);
        if (!rotation.has_value()) {
            failPlyWrite(result, rotation, "Could not rotate PLY spherical harmonics");
            return result;
        }
        sh_rotation.emplace(std::move(rotation.value()));
    }

    // Preflight each conversion before the writer emits a header.
    std::size_t endpoint_count = 0;
    std::size_t zero_scale_count = 0;
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i % 1024 == 0 && !checkPlyWrite(result, context, "validate", i, data.size()))
            return result;
        auto encoded = encodePlySplat(data, i, output_degree, target_frame,
                                      sh_rotation ? &*sh_rotation : nullptr);
        if (!encoded.has_value()) {
            failPlyWrite(result, encoded, "Could not transform a PLY splat");
            return result;
        }
        if (usesGraphdecoLayout(config.profile)) {
            bool clamped = false;
            auto opacity = encodePlyOpacity(encoded.value().opacity, clamped);
            if (!opacity.has_value()) {
                failPlyWrite(result, opacity, "Could not encode PLY opacity");
                return result;
            }
            if (clamped)
                ++endpoint_count;
            const Vec3f scale = encoded.value().scale;
            if (scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f)
                ++zero_scale_count;
            for (float component : {scale.x, scale.y, scale.z}) {
                auto encoded_scale = math::log_scale_from_linear(trainingScale(component));
                if (!encoded_scale.has_value()) {
                    failPlyWrite(result, encoded_scale, "Could not encode PLY scale");
                    return result;
                }
            }
        }
    }
    if (endpoint_count != 0) {
        LossItem item;
        item.code = loss_code::kOpacityClamped;
        item.severity = LossSeverity::warning;
        item.source_feature = "canonical opacity endpoints";
        item.target_constraint = "training-layout PLY stores finite opacity logits";
        item.affected_splats = endpoint_count;
        item.remediation = "use the canonical PLY profile to preserve exact endpoints";
        if (!appendLoss(result, std::move(item)))
            return result;
        Diagnostic diagnostic("MK1210_PLY_OPACITY_ENDPOINT_CLAMPED", Severity::warning,
                              "Canonical opacity endpoints were clamped for finite PLY logits");
        diagnostic.with_context("splat_count", static_cast<std::uint64_t>(endpoint_count))
            .with_context("epsilon", static_cast<double>(kPlyOpacityEpsilon));
        result.diagnostics.push_back(std::move(diagnostic));
    }
    if (zero_scale_count != 0) {
        LossItem item;
        item.code = loss_code::kScaleClamped;
        item.severity = LossSeverity::severe;
        item.source_feature = "canonical zero scale";
        item.target_constraint = "training-layout PLY stores each natural-log scale";
        item.affected_splats = zero_scale_count;
        item.remediation = "use canonical PLY or glTF to preserve zero scales";
        if (!appendLoss(result, std::move(item)))
            return result;
    }
    if (auto policy = result.losses.check_policy(config.approved_loss_codes); !policy.has_value()) {
        failPlyWrite(result, policy, "PLY loss policy failed");
        return result;
    }

    {
        auto header_bound =
            checked_add(kPlyWriterHeaderBase, config.comment.size(), "PLY writer header bound");
        if (!header_bound.has_value()) {
            failPlyWrite(result, header_bound, "PLY header size overflows resource accounting");
            return result;
        }
        auto header_working = checked_mul(header_bound.value(), kPlyWriterHeaderCopies,
                                          "PLY writer header working memory");
        if (!header_working.has_value()) {
            failPlyWrite(result, header_working,
                         "PLY header working memory overflows resource accounting");
            return result;
        }
        auto header_reservation = context.budget->reserve(
            BudgetKind::memory_bytes, header_working.value(), "ply.write.header");
        if (!header_reservation.has_value()) {
            failPlyWrite(result, header_reservation, "PLY header exceeds the memory limit");
            return result;
        }
        Budget::Charge header_memory = std::move(header_reservation).value();

        std::ostringstream header;
        header.imbue(std::locale::classic());
        header << "ply\n";
        if (config.format == PlyFormat::Binary) {
            header << "format binary_little_endian 1.0\n";
        } else {
            header << "format ascii 1.0\n";
        }

        header << "comment melkor_profile " << plyProfileMarker(config.profile) << "\n";
        header << "comment melkor_coordinate_system " << target_frame.id << "\n";
        header << "comment melkor_length_unit meter\n";
        header << "comment melkor_color_space " << to_string(*config.color_space) << "\n";
        header << "comment melkor_quaternion_order "
               << (config.profile == FormatProfileId::ply_melkor_canonical_v1 ? "xyzw" : "wxyz")
               << "\n";
        header << "comment melkor_scale_domain "
               << (config.profile == FormatProfileId::ply_melkor_canonical_v1 ? "linear" : "log")
               << "\n";
        header << "comment melkor_opacity_domain "
               << (config.profile == FormatProfileId::ply_melkor_canonical_v1 ? "linear" : "logit")
               << "\n";
        header << "comment melkor_sh_basis real_condon_shortley\n";
        header << "comment melkor_sh_degree " << output_degree << "\n";
        if (config.antialiased.has_value()) {
            header << "comment melkor_antialiased " << (*config.antialiased ? 1 : 0) << "\n";
        }

        if (!config.comment.empty()) {
            std::size_t replaced_bytes = 0;
            const std::string comment = sanitizeComment(config.comment, replaced_bytes);
            header << "comment " << comment << "\n";
            if (replaced_bytes != 0) {
                Diagnostic diagnostic(
                    "MK1213_PLY_COMMENT_SANITIZED", Severity::warning,
                    "Bytes outside printable ASCII in the PLY comment were replaced with spaces");
                diagnostic.with_context("replaced_bytes",
                                        static_cast<std::uint64_t>(replaced_bytes));
                result.diagnostics.push_back(std::move(diagnostic));
            }
        }

        header << "element vertex " << data.size() << "\n";
        header << "property float x\n";
        header << "property float y\n";
        header << "property float z\n";
        if (config.profile == FormatProfileId::ply_melkor_canonical_v1) {
            header << "property float scale_x\n";
            header << "property float scale_y\n";
            header << "property float scale_z\n";
            header << "property float rotation_x\n";
            header << "property float rotation_y\n";
            header << "property float rotation_z\n";
            header << "property float rotation_w\n";
            header << "property float opacity\n";
            for (std::uint32_t coefficient = 0; coefficient < output_coefficients; ++coefficient) {
                for (std::size_t channel = 0; channel < 3; ++channel)
                    header << "property float " << canonicalShProperty(coefficient, channel)
                           << "\n";
            }
        } else {
            header << "property float nx\n";
            header << "property float ny\n";
            header << "property float nz\n";
            header << "property float f_dc_0\n";
            header << "property float f_dc_1\n";
            header << "property float f_dc_2\n";
            for (int i = 0; i < sh_rest_count; ++i)
                header << "property float f_rest_" << i << "\n";
            header << "property float opacity\n";
            header << "property float scale_0\n";
            header << "property float scale_1\n";
            header << "property float scale_2\n";
            header << "property float rot_0\n";
            header << "property float rot_1\n";
            header << "property float rot_2\n";
            header << "property float rot_3\n";
        }

        header << "end_header\n";

        std::string header_str = header.str();
        if (header_str.size() > config.limits.max_ply_header_bytes) {
            failPlyWrite(result, ErrorCode::resource_limit,
                         "PLY header exceeds the selected header limit");
            return result;
        }
        stream.write(header_str.c_str(), static_cast<std::streamsize>(header_str.size()));
        result.bytes_written = header_str.size();
    }
    if (!stream.good()) {
        failPlyWrite(result, ErrorCode::io_error, "Stream error during write");
        return result;
    }

    if (config.format == PlyFormat::Binary) {
        for (std::size_t splat = 0; splat < data.size(); ++splat) {
            if (splat % 1024 == 0) {
                if (!stream.good()) {
                    failPlyWrite(result, ErrorCode::io_error, "Stream error during write");
                    return result;
                }
                if (!checkPlyWrite(result, context, "vertices", splat, data.size()))
                    return result;
            }
            auto encoded = encodePlySplat(data, splat, output_degree, target_frame,
                                          sh_rotation ? &*sh_rotation : nullptr);
            if (!encoded.has_value()) {
                failPlyWrite(result, encoded, "Could not transform a PLY splat");
                return result;
            }
            const Vec3f& position = encoded.value().position;
            const Vec3f& scale = encoded.value().scale;
            const Quatf& rotation = encoded.value().rotation;
            writeFloatLittleEndian(stream, position.x);
            writeFloatLittleEndian(stream, position.y);
            writeFloatLittleEndian(stream, position.z);
            if (config.profile == FormatProfileId::ply_melkor_canonical_v1) {
                writeFloatLittleEndian(stream, scale.x);
                writeFloatLittleEndian(stream, scale.y);
                writeFloatLittleEndian(stream, scale.z);
                writeFloatLittleEndian(stream, rotation.x);
                writeFloatLittleEndian(stream, rotation.y);
                writeFloatLittleEndian(stream, rotation.z);
                writeFloatLittleEndian(stream, rotation.w);
                writeFloatLittleEndian(stream, encoded.value().opacity);
                for (std::size_t i = 0; i < output_coefficients * 3; ++i)
                    writeFloatLittleEndian(stream, encoded.value().sh[i]);
                result.bytes_written += (11 + output_coefficients * 3) * sizeof(float);
            } else {
                writeFloatLittleEndian(stream, 0.0f);
                writeFloatLittleEndian(stream, 0.0f);
                writeFloatLittleEndian(stream, 0.0f);
                writeFloatLittleEndian(stream, encoded.value().sh[0]);
                writeFloatLittleEndian(stream, encoded.value().sh[1]);
                writeFloatLittleEndian(stream, encoded.value().sh[2]);
                const std::size_t higher_per_channel = output_coefficients - 1;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    for (std::size_t higher = 0; higher < higher_per_channel; ++higher) {
                        writeFloatLittleEndian(stream,
                                               encoded.value().sh[(higher + 1) * 3 + channel]);
                    }
                }
                bool ignored_clamp = false;
                writeFloatLittleEndian(
                    stream, encodePlyOpacity(encoded.value().opacity, ignored_clamp).value());
                writeFloatLittleEndian(stream,
                                       math::log_scale_from_linear(trainingScale(scale.x)).value());
                writeFloatLittleEndian(stream,
                                       math::log_scale_from_linear(trainingScale(scale.y)).value());
                writeFloatLittleEndian(stream,
                                       math::log_scale_from_linear(trainingScale(scale.z)).value());
                writeFloatLittleEndian(stream, rotation.w);
                writeFloatLittleEndian(stream, rotation.x);
                writeFloatLittleEndian(stream, rotation.y);
                writeFloatLittleEndian(stream, rotation.z);
                result.bytes_written += (17 + sh_rest_count) * sizeof(float);
            }
        }
    } else {
        const auto write_text = [&](std::string_view text) {
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            result.bytes_written += static_cast<std::uint64_t>(text.size());
        };
        const auto write_value = [&](float value) {
            result.bytes_written += static_cast<std::uint64_t>(writeAsciiFloat(stream, value));
        };
        for (std::size_t splat = 0; splat < data.size(); ++splat) {
            if (splat % 1024 == 0) {
                if (!stream.good()) {
                    failPlyWrite(result, ErrorCode::io_error, "Stream error during write");
                    return result;
                }
                if (!checkPlyWrite(result, context, "vertices", splat, data.size()))
                    return result;
            }
            auto encoded = encodePlySplat(data, splat, output_degree, target_frame,
                                          sh_rotation ? &*sh_rotation : nullptr);
            if (!encoded.has_value()) {
                failPlyWrite(result, encoded, "Could not transform a PLY splat");
                return result;
            }
            const Vec3f& position = encoded.value().position;
            const Vec3f& scale = encoded.value().scale;
            const Quatf& rotation = encoded.value().rotation;
            write_value(position.x);
            write_text(" ");
            write_value(position.y);
            write_text(" ");
            write_value(position.z);
            write_text(" ");
            if (config.profile == FormatProfileId::ply_melkor_canonical_v1) {
                for (float value : {scale.x, scale.y, scale.z, rotation.x, rotation.y, rotation.z,
                                    rotation.w, encoded.value().opacity}) {
                    write_value(value);
                    write_text(" ");
                }
                for (std::size_t i = 0; i < output_coefficients * 3; ++i) {
                    write_value(encoded.value().sh[i]);
                    write_text(i + 1 == output_coefficients * 3 ? "\n" : " ");
                }
            } else {
                write_text("0 0 0 ");
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    write_value(encoded.value().sh[channel]);
                    write_text(" ");
                }
                const std::size_t higher_per_channel = output_coefficients - 1;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    for (std::size_t higher = 0; higher < higher_per_channel; ++higher) {
                        write_value(encoded.value().sh[(higher + 1) * 3 + channel]);
                        write_text(" ");
                    }
                }
                bool ignored_clamp = false;
                write_value(encodePlyOpacity(encoded.value().opacity, ignored_clamp).value());
                write_text(" ");
                write_value(math::log_scale_from_linear(trainingScale(scale.x)).value());
                write_text(" ");
                write_value(math::log_scale_from_linear(trainingScale(scale.y)).value());
                write_text(" ");
                write_value(math::log_scale_from_linear(trainingScale(scale.z)).value());
                write_text(" ");
                write_value(rotation.w);
                write_text(" ");
                write_value(rotation.x);
                write_text(" ");
                write_value(rotation.y);
                write_text(" ");
                write_value(rotation.z);
                write_text("\n");
            }
        }
    }

    if (!stream.good()) {
        failPlyWrite(result, ErrorCode::io_error, "Stream error during write");
        return result;
    }

    if (!checkPlyWrite(result, context, "complete", data.size(), data.size()))
        return result;

    result.success = true;
    return result;
} catch (const std::bad_alloc&) {
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded available memory");
} catch (const std::length_error&) {
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded a container size limit");
} catch (const std::ios_base::failure&) {
    return plyWriteFailure(ErrorCode::io_error, "The PLY output stream failed");
} catch (const std::exception& error) {
    return plyWriteFailure(ErrorCode::internal_error,
                           std::string("Unexpected PLY write failure: ") + error.what());
}

PlyWriteResult PlyWriter::writeToBuffer(std::vector<uint8_t>& buffer, const SplatData& data,
                                        const PlyWriteConfig& config) try {
    Budget budget(config.limits);
    OperationContext context = make_default_context(budget);
    return writeToBuffer(buffer, data, config, context);
} catch (const std::bad_alloc&) {
    std::vector<uint8_t>().swap(buffer);
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded available memory");
} catch (const std::length_error&) {
    std::vector<uint8_t>().swap(buffer);
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded a container size limit");
}

PlyWriteResult PlyWriter::writeToBuffer(std::vector<uint8_t>& buffer, const SplatData& data,
                                        const PlyWriteConfig& requested,
                                        const OperationContext& context) try {
    std::vector<uint8_t>().swap(buffer);
    if (context.budget == nullptr) {
        return plyWriteFailure(ErrorCode::internal_error,
                               "The PLY writer requires a resource budget");
    }
    PlyWriteConfig config = requested;
    config.limits = context.budget->limits();
    if (!isValidPlyFormat(config.format)) {
        return plyWriteFailure(ErrorCode::invalid_argument, "The PLY encoding is invalid");
    }
    if (auto valid = config.limits.validate(); !valid.has_value()) {
        return plyWriteFailure(
            ErrorCode::invalid_argument,
            firstDiagnosticMessage(valid.diagnostics(), "Invalid resource limits"));
    }
    const FormatProfile& profile = format_profile(config.profile);
    if (!profile.supports_write_container(FormatId::ply)) {
        return plyWriteFailure(ErrorCode::unsupported_feature,
                               "The selected profile does not support PLY output");
    }
    std::uint64_t output_bound = 0;
    if (!estimatePlyOutputBytes(data, config, output_bound)) {
        return plyWriteFailure(ErrorCode::resource_limit,
                               "PLY buffer size overflows resource accounting");
    }
    // stringbuf can retain spare capacity while str() and the output vector hold copies.
    auto working_bytes = checked_mul(output_bound, 4, "PLY output buffer working memory");
    if (!working_bytes.has_value()) {
        PlyWriteResult failure;
        failPlyWrite(failure, working_bytes, "PLY buffer size overflows");
        return failure;
    }
    auto reserved = context.budget->reserve(BudgetKind::memory_bytes, working_bytes.value(),
                                            "ply.write.buffer_working_set");
    if (!reserved.has_value()) {
        PlyWriteResult failure;
        failPlyWrite(failure, reserved, "PLY buffer exceeds the memory limit");
        return failure;
    }
    Budget::Charge reservation = std::move(reserved).value();
    PlyWriteResult result;
    {
        std::ostringstream stream(std::ios::binary);
        result = writeToStream(stream, data, config, context);
        if (result.success) {
            std::string encoded = stream.str();
            buffer.assign(encoded.begin(), encoded.end());
            result.bytes_written = buffer.size();
        }
    }

    if (result.success && buffer.capacity() > working_bytes.value()) {
        std::vector<uint8_t>().swap(buffer);
        return plyWriteFailure(ErrorCode::internal_error,
                               "The PLY buffer exceeded its preflight memory bound");
    }

    if (result.success) {
        reservation.shrink_to(buffer.capacity());
        result.set_retained_memory(std::move(reservation));
    }

    return result;
} catch (const std::bad_alloc&) {
    std::vector<uint8_t>().swap(buffer);
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded available memory");
} catch (const std::length_error&) {
    std::vector<uint8_t>().swap(buffer);
    return plyWriteFailure(ErrorCode::resource_limit, "PLY write exceeded a container size limit");
} catch (const std::exception& error) {
    std::vector<uint8_t>().swap(buffer);
    return plyWriteFailure(ErrorCode::internal_error,
                           std::string("Unexpected PLY write failure: ") + error.what());
}

// PLY Reader implementation
PlyReader::PlyReader() = default;

PlyReader::ReadResult PlyReader::readFromFile(const std::filesystem::path& filepath,
                                              const Limits& limits) try {
    PlyReadConfig config;
    config.limits = limits;
    return readFromFile(filepath, config);
} catch (const std::bad_alloc&) {
    return {false, "PLY file exceeds available memory", {}, {}, ErrorCode::resource_limit};
} catch (const std::length_error&) {
    return {false, "PLY file exceeds a container size limit", {}, {}, ErrorCode::resource_limit};
}

PlyReader::ReadResult PlyReader::readFromFile(const std::filesystem::path& filepath,
                                              const PlyReadConfig& config) try {
    Budget budget(config.limits);
    OperationContext context = make_default_context(budget);
    return readFromFile(filepath, config, context);
} catch (const std::bad_alloc&) {
    return {false, "PLY file exceeds available memory", {}, {}, ErrorCode::resource_limit};
} catch (const std::length_error&) {
    return {false, "PLY file exceeds a container size limit", {}, {}, ErrorCode::resource_limit};
}

PlyReader::ReadResult PlyReader::readFromFile(const std::filesystem::path& filepath,
                                              const OperationContext& context) try {
    PlyReadConfig config;
    if (context.budget != nullptr)
        config.limits = context.budget->limits();
    return readFromFile(filepath, config, context);
} catch (const std::bad_alloc&) {
    return {false, "PLY file exceeds available memory", {}, {}, ErrorCode::resource_limit};
} catch (const std::length_error&) {
    return {false, "PLY file exceeds a container size limit", {}, {}, ErrorCode::resource_limit};
} catch (const std::filesystem::filesystem_error&) {
    return {false, "Failed to access PLY file", {}, {}, ErrorCode::io_error};
}

PlyReader::ReadResult PlyReader::readFromFile(const std::filesystem::path& filepath,
                                              const PlyReadConfig& requested,
                                              const OperationContext& context) try {
    if (context.budget == nullptr) {
        return {
            false, "The PLY reader requires a resource budget", {}, {}, ErrorCode::internal_error};
    }
    auto control = context.checkpoint({"ply.read", "prepare", 0, 0, "bytes"});
    if (!control.has_value())
        return plyReadFailure(control, "PLY read stopped");
    PlyReadConfig config = requested;
    config.limits = context.budget->limits();
    const Limits& limits = config.limits;
    if (auto valid = limits.validate(); !valid.has_value())
        return plyReadFailure(valid, "Invalid resource limits");
    auto opened = io::InputFile::open(filepath, context);
    if (!opened.has_value())
        return plyReadFailure(opened, "Failed to open PLY file");
    io::InputFile file = std::move(opened).value();

    const std::uint64_t file_size = file.size();
    if (file_size == 0) {
        return {false, "Invalid or empty PLY file", {}, {}};
    }
    if (file_size > std::numeric_limits<size_t>::max()) {
        return {false,
                "PLY file exceeds the host address-space limit",
                {},
                {},
                ErrorCode::resource_limit};
    }
    const size_t size = static_cast<size_t>(file_size);

    // Charge the complete file before any read can allocate a same-sized buffer.
    if (auto charged = context.consume(BudgetKind::input_bytes, size, "ply.file");
        !charged.has_value()) {
        return plyReadFailure(charged, "PLY file exceeds the input-size limit");
    }
    // Screen the header against the same limit readFromBuffer enforces, not a hardcoded 1 MiB, so
    // a file with a valid within-policy header (e.g. up to the desktop profile's 4 MiB) is not
    // false-rejected here.
    const size_t header_cap =
        limits.max_ply_header_bytes != 0
            ? static_cast<size_t>(std::min<std::uint64_t>(limits.max_ply_header_bytes,
                                                          std::numeric_limits<size_t>::max()))
            : size;
    const size_t prefix_size = std::min(size, header_cap);
    std::optional<ScopedBudgetRelease> file_buffer_charge;
    std::vector<uint8_t> buffer;
    {
        auto prefix_charge =
            context.consume(BudgetKind::memory_bytes, prefix_size, "ply.file_header_prefix");
        if (!prefix_charge.has_value()) {
            return plyReadFailure(prefix_charge, "PLY header prefix exceeds the memory limit");
        }
        ScopedBudgetRelease prefix_release(context.budget, BudgetKind::memory_bytes, prefix_size);
        std::vector<uint8_t> prefix(prefix_size);
        auto prefix_read =
            file.read_exact(0, prefix.data(), prefix.size(), context, "ply.read", "header_file");
        if (!prefix_read.has_value())
            return plyReadFailure(prefix_read, "Failed to read PLY header");
        const std::string_view prefix_view(reinterpret_cast<const char*>(prefix.data()),
                                           prefix.size());
        if (!(prefix_view.rfind("ply\n", 0) == 0 || prefix_view.rfind("ply\r\n", 0) == 0)) {
            return {false, "Invalid PLY: missing ply magic line", {}, {}};
        }
        const bool has_header_end = prefix_view.find("\nend_header\n") != std::string_view::npos ||
                                    prefix_view.find("\nend_header\r\n") != std::string_view::npos;
        if (!has_header_end && size > prefix_size) {
            return {false,
                    "Invalid PLY: header exceeds the configured size limit",
                    {},
                    {},
                    ErrorCode::resource_limit};
        }
        if (auto charged = context.consume(BudgetKind::memory_bytes, size, "ply.file_buffer");
            !charged.has_value()) {
            return plyReadFailure(charged, "PLY file buffer exceeds the memory limit");
        }
        file_buffer_charge.emplace(context.budget, BudgetKind::memory_bytes, size);
        try {
            buffer.resize(size);
        } catch (const std::bad_alloc&) {
            return {false, "PLY file exceeds available memory", {}, {}, ErrorCode::resource_limit};
        }
        std::copy(prefix.begin(), prefix.end(), buffer.begin());
    }

    auto payload_read = file.read_exact(prefix_size, buffer.data() + prefix_size,
                                        size - prefix_size, context, "ply.read", "file");
    if (!payload_read.has_value())
        return plyReadFailure(payload_read, "Failed to read complete PLY file");

    return readFromBufferImpl(buffer.data(), buffer.size(), context, config, true);
} catch (const std::bad_alloc&) {
    return {false, "PLY file exceeds available memory", {}, {}, ErrorCode::resource_limit};
} catch (const std::length_error&) {
    return {false, "PLY file exceeds a container size limit", {}, {}, ErrorCode::resource_limit};
} catch (const std::filesystem::filesystem_error&) {
    return {false, "Failed to access PLY file", {}, {}, ErrorCode::io_error};
}

PlyReader::ReadResult PlyReader::readFromBuffer(const uint8_t* data, size_t size,
                                                const Limits& limits) try {
    PlyReadConfig config;
    config.limits = limits;
    return readFromBuffer(data, size, config);
} catch (const std::bad_alloc&) {
    return {false, "PLY buffer exceeds available memory", {}, {}, ErrorCode::resource_limit};
} catch (const std::length_error&) {
    return {false, "PLY buffer exceeds a container size limit", {}, {}, ErrorCode::resource_limit};
}

PlyReader::ReadResult PlyReader::readFromBuffer(const uint8_t* data, size_t size,
                                                const PlyReadConfig& config) try {
    Budget budget(config.limits);
    OperationContext context = make_default_context(budget);
    return readFromBuffer(data, size, config, context);
} catch (const std::bad_alloc&) {
    return {false, "PLY buffer exceeds available memory", {}, {}, ErrorCode::resource_limit};
} catch (const std::length_error&) {
    return {false, "PLY buffer exceeds a container size limit", {}, {}, ErrorCode::resource_limit};
}

PlyReader::ReadResult PlyReader::readFromBuffer(const uint8_t* data, size_t size,
                                                const OperationContext& context) try {
    PlyReadConfig config;
    if (context.budget != nullptr)
        config.limits = context.budget->limits();
    return readFromBuffer(data, size, config, context);
} catch (const std::bad_alloc&) {
    return {false, "PLY buffer exceeds available memory", {}, {}, ErrorCode::resource_limit};
} catch (const std::length_error&) {
    return {false, "PLY buffer exceeds a container size limit", {}, {}, ErrorCode::resource_limit};
}

PlyReader::ReadResult PlyReader::readFromBuffer(const uint8_t* data, size_t size,
                                                const PlyReadConfig& requested,
                                                const OperationContext& context) try {
    PlyReadConfig config = requested;
    if (context.budget != nullptr)
        config.limits = context.budget->limits();
    return readFromBufferImpl(data, size, context, config, false);
} catch (const std::bad_alloc&) {
    return {false, "PLY buffer exceeds available memory", {}, {}, ErrorCode::resource_limit};
} catch (const std::length_error&) {
    return {false, "PLY buffer exceeds a container size limit", {}, {}, ErrorCode::resource_limit};
}

PlyReader::ReadResult PlyReader::readFromBufferImpl(const uint8_t* data, size_t size,
                                                    const OperationContext& context,
                                                    const PlyReadConfig& config,
                                                    bool input_is_charged) try {
    if (context.budget == nullptr) {
        return {
            false, "The PLY reader requires a resource budget", {}, {}, ErrorCode::internal_error};
    }
    auto control = context.checkpoint({"ply.read", "buffer", 0, size, "bytes"});
    if (!control.has_value())
        return plyReadFailure(control, "PLY read stopped");
    const Limits& limits = context.budget->limits();
    if (auto valid = limits.validate(); !valid.has_value())
        return plyReadFailure(valid, "Invalid resource limits");
    if (data == nullptr || size == 0) {
        return {false, "Invalid PLY: input buffer is null or empty", {}, {}};
    }
    std::optional<ScopedBudgetRelease> input_release;
    if (!input_is_charged) {
        if (auto charged = context.consume(BudgetKind::input_bytes, size, "ply.input");
            !charged.has_value()) {
            return plyReadFailure(charged, "PLY input exceeds the input-size limit");
        }
        if (auto charged = context.consume(BudgetKind::memory_bytes, size, "ply.input_buffer");
            !charged.has_value()) {
            return plyReadFailure(charged, "PLY input buffer exceeds the memory limit");
        }
        input_release.emplace(context.budget, BudgetKind::memory_bytes, size);
    }
    ReadResult result;
    result.success = true;
    result.metadata.source_bytes = size;

    // Canonical scalar type table. PLY 1.0 allows both the classic names
    // (char/uchar/short/ushort/int/uint/float/double) and the sized aliases
    // (int8/uint8/int16/uint16/int32/uint32/float32/float64); both spellings
    // must resolve to the same size and decoding, otherwise stride computation
    // and binary decoding silently corrupt valid files.
    enum class PropKind { F32, F64, I8, U8, I16, U16, I32, U32, Unknown };
    struct PropType {
        PropKind kind;
        size_t size;
    };
    auto prop_type = [](const std::string& t) -> PropType {
        if (t == "float" || t == "float32")
            return {PropKind::F32, 4};
        if (t == "double" || t == "float64")
            return {PropKind::F64, 8};
        if (t == "uchar" || t == "uint8")
            return {PropKind::U8, 1};
        if (t == "char" || t == "int8")
            return {PropKind::I8, 1};
        if (t == "ushort" || t == "uint16")
            return {PropKind::U16, 2};
        if (t == "short" || t == "int16")
            return {PropKind::I16, 2};
        if (t == "uint" || t == "uint32")
            return {PropKind::U32, 4};
        if (t == "int" || t == "int32")
            return {PropKind::I32, 4};
        return {PropKind::Unknown, 4};  // unknown name: assume a 4-byte float
    };

    // Parse the header line by line. The header ends at the first line whose
    // content is exactly "end_header" (with an optional trailing CR for CRLF
    // files); the data section starts immediately after that line's newline.
    // Scanning whole lines (instead of a raw substring find) means CRLF
    // headers are accepted and a comment that merely contains "end_header"
    // cannot truncate the header early.
    //
    // The reader is header-driven: we map each property name to its index
    // within the vertex record, so PLYs authored by different tools (e.g. those
    // using red/green/blue instead of f_dc_*, or omitting normals) are parsed
    // correctly instead of silently misaligned.
    struct Property {
        std::string name;
        PropType type;
        size_t offset;  // byte offset within one binary vertex record
    };
    std::vector<Property> vertex_props;
    size_t vertex_count = 0;
    bool is_binary = false;
    bool is_big_endian = false;
    bool in_vertex = false;
    bool saw_format = false;
    bool saw_vertex = false;
    size_t current_element_count = 0;
    std::unordered_set<std::string> element_names;
    std::unordered_set<std::string> property_names;
    std::optional<std::string> marker_profile;
    std::optional<std::string> marker_frame;
    std::optional<std::string> marker_color_space;
    std::optional<std::string> marker_length_unit;
    std::optional<std::string> marker_quaternion_order;
    std::optional<std::string> marker_scale_domain;
    std::optional<std::string> marker_opacity_domain;
    std::optional<std::string> marker_sh_basis;
    std::optional<std::uint32_t> marker_sh_degree;
    std::optional<bool> marker_antialiased;
    std::uint64_t metadata_bytes = 0;
    std::size_t dropped_free_form_metadata_entries = 0;
    std::size_t dropped_empty_schema_entries = 0;

    const auto account_metadata = [&](std::string_view value) {
        const std::uint64_t bytes = value.size();
        if (bytes > limits.max_metadata_string_bytes ||
            metadata_bytes > limits.max_metadata_total_bytes - bytes) {
            return false;
        }
        metadata_bytes += bytes;
        return true;
    };

    std::string_view view(reinterpret_cast<const char*>(data), size);

    // Find the exact header boundary without allocating a line. A single long line must not
    // allocate before the header limit rejects it.
    size_t bounded_header_bytes = 0;
    {
        const size_t header_limit = static_cast<size_t>(std::min<std::uint64_t>(
            limits.max_ply_header_bytes, std::numeric_limits<size_t>::max()));
        const size_t scan_end = std::min(size, header_limit);
        size_t line_start = 0;
        std::size_t line_count = 0;
        while (line_start < scan_end) {
            if (line_count % 1024 == 0) {
                auto header_control =
                    context.checkpoint({"ply.read", "header", line_start, scan_end, "bytes"});
                if (!header_control.has_value())
                    return plyReadControlFailure(header_control);
            }
            ++line_count;
            const size_t relative_nl = view.substr(line_start, scan_end - line_start).find('\n');
            if (relative_nl == std::string_view::npos)
                break;
            const size_t nl = line_start + relative_nl;
            const size_t line_end = nl + 1;
            std::string_view line = view.substr(line_start, nl - line_start);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line == "end_header") {
                bounded_header_bytes = line_end;
                break;
            }
            line_start = line_end;
        }
        if (bounded_header_bytes == 0) {
            if (size > header_limit) {
                return {false,
                        "Invalid PLY: header exceeds the configured size limit",
                        {},
                        {},
                        ErrorCode::resource_limit};
            }
            return {false, "Invalid PLY: no end_header found", {}, {}};
        }
    }
    auto header_scaled = checked_mul(bounded_header_bytes, kPlyHeaderWorkingMultiplier,
                                     "PLY header parser working memory");
    auto header_working = header_scaled.has_value()
                              ? checked_add(header_scaled.value(), kPlyHeaderWorkingBase,
                                            "PLY header parser working memory")
                              : Result<std::uint64_t>::failure(header_scaled.error_code(),
                                                               header_scaled.diagnostics());
    if (!header_working.has_value())
        return plyReadFailure(header_working, "PLY header working-memory size overflows");
    if (auto charged =
            context.consume(BudgetKind::memory_bytes, header_working.value(), "ply.header_parse");
        !charged.has_value()) {
        return {false,
                charged.diagnostics().empty() ? "PLY header exceeds the memory limit"
                                              : charged.diagnostics()[0].message,
                {},
                {},
                charged.error_code()};
    }
    ScopedBudgetRelease header_release(context.budget, BudgetKind::memory_bytes,
                                       header_working.value());

    size_t header_bytes = 0;
    {
        bool found_end = false;
        size_t line_start = 0;
        size_t line_number = 0;
        while (line_start < size) {
            if (line_number % 1024 == 0) {
                auto header_control = context.checkpoint(
                    {"ply.read", "header", line_start, bounded_header_bytes, "bytes"});
                if (!header_control.has_value())
                    return plyReadControlFailure(header_control);
            }
            size_t nl = view.find('\n', line_start);
            if (nl == std::string_view::npos)
                break;  // header lines must end in '\n'
            if (nl + 1 > bounded_header_bytes) {
                return {false, "Invalid PLY: inconsistent header boundary", {}, {}};
            }
            std::string_view line_view = view.substr(line_start, nl - line_start);
            if (!line_view.empty() && line_view.back() == '\r')
                line_view.remove_suffix(1);
            const bool valid_header_text =
                std::all_of(line_view.begin(), line_view.end(), [](unsigned char byte) {
                    return byte == '\t' || (byte >= 0x20u && byte <= 0x7eu);
                });
            if (!valid_header_text)
                return {false, "Invalid PLY: header contains a non-ASCII or control byte", {}, {}};
            if (line_view.size() > limits.max_metadata_string_bytes) {
                return {false,
                        "PLY header line exceeds the metadata string limit",
                        {},
                        {},
                        ErrorCode::resource_limit};
            }
            std::string line(line_view);
            line_start = nl + 1;
            if (line_number++ == 0) {
                if (line != "ply") {
                    return {false, "Invalid PLY: missing ply magic line", {}, {}};
                }
                continue;
            }
            if (line == "end_header") {
                header_bytes = line_start;
                found_end = true;
                break;
            }
            if (!saw_format && line.rfind("format ", 0) != 0) {
                return {
                    false, "Invalid PLY: format declaration must follow the magic line", {}, {}};
            }
            if (line.rfind("format ", 0) == 0) {
                if (saw_format) {
                    return {false, "Invalid PLY: duplicate format declaration", {}, {}};
                }
                saw_format = true;
                if (line == "format ascii 1.0") {
                    is_binary = false;
                    is_big_endian = false;
                } else if (line == "format binary_little_endian 1.0") {
                    is_binary = true;
                    is_big_endian = false;
                } else if (line == "format binary_big_endian 1.0") {
                    is_binary = true;
                    is_big_endian = true;
                } else {
                    return {false, "Invalid PLY: unsupported format declaration", {}, {}};
                }
            } else if (line.rfind("element ", 0) == 0) {
                std::istringstream declaration(line);
                declaration.imbue(std::locale::classic());
                std::string keyword;
                std::string element_name;
                std::string count_token;
                std::string extra;
                declaration >> keyword >> element_name >> count_token >> extra;
                if (keyword != "element" || element_name.empty() || count_token.empty() ||
                    !extra.empty()) {
                    return {false, "Invalid PLY: malformed element declaration", {}, {}};
                }
                if (!element_names.insert(element_name).second) {
                    return {false, "Invalid PLY: duplicate element declaration", {}, {}};
                }
                if (!account_metadata(element_name)) {
                    return {false,
                            "PLY element names exceed the metadata limit",
                            {},
                            {},
                            ErrorCode::resource_limit};
                }

                size_t element_count = 0;
                const char* begin = count_token.data();
                const char* end = begin + count_token.size();
                const auto parsed = std::from_chars(begin, end, element_count);
                if (parsed.ec != std::errc{} || parsed.ptr != end) {
                    return {false, "Invalid PLY: malformed element count in header", {}, {}};
                }

                current_element_count = element_count;
                in_vertex = element_name == "vertex";
                property_names.clear();
                if (in_vertex) {
                    saw_vertex = true;
                    vertex_count = element_count;
                } else if (element_count != 0) {
                    return {false,
                            "Unsupported PLY: Gaussian decoding requires vertex-only data",
                            {},
                            {},
                            ErrorCode::unsupported_feature};
                } else {
                    ++dropped_empty_schema_entries;
                }
            } else if (line.rfind("property ", 0) == 0 && in_vertex) {
                std::istringstream property(line);
                property.imbue(std::locale::classic());
                std::string keyword;
                std::string type_name;
                std::string property_name;
                std::string extra;
                property >> keyword >> type_name;
                if (type_name == "list") {
                    std::string count_type_name;
                    std::string item_type_name;
                    property >> count_type_name >> item_type_name >> property_name >> extra;
                    if (keyword != "property" || count_type_name.empty() ||
                        item_type_name.empty() || property_name.empty() || !extra.empty()) {
                        return {false, "Invalid PLY: malformed vertex list property", {}, {}};
                    }
                    return {false,
                            "Unsupported PLY: vertex list properties have variable record sizes",
                            {},
                            {},
                            ErrorCode::unsupported_feature};
                }
                property >> property_name >> extra;
                if (keyword != "property" || type_name.empty() || property_name.empty() ||
                    !extra.empty()) {
                    return {false, "Invalid PLY: malformed vertex property", {}, {}};
                }
                const PropType type = prop_type(type_name);
                if (type.kind == PropKind::Unknown) {
                    return {false,
                            "Invalid PLY: unsupported scalar property type " + type_name,
                            {},
                            {}};
                }
                if (!property_names.insert(property_name).second) {
                    return {
                        false, "Invalid PLY: duplicate vertex property " + property_name, {}, {}};
                }
                if (!account_metadata(property_name)) {
                    return {false,
                            "PLY property names exceed the metadata limit",
                            {},
                            {},
                            ErrorCode::resource_limit};
                }
                vertex_props.push_back({property_name, type, 0});
            } else if (line.rfind("property ", 0) == 0) {
                if (element_names.empty() || current_element_count != 0) {
                    return {false, "Invalid PLY: property has no supported element", {}, {}};
                }
                std::istringstream property(line);
                property.imbue(std::locale::classic());
                std::string keyword;
                std::string type_name;
                std::string property_name;
                std::string extra;
                property >> keyword >> type_name;
                if (type_name == "list") {
                    std::string count_type_name;
                    std::string item_type_name;
                    property >> count_type_name >> item_type_name >> property_name >> extra;
                    const PropKind count_kind = prop_type(count_type_name).kind;
                    const bool integer_count =
                        count_kind == PropKind::I8 || count_kind == PropKind::U8 ||
                        count_kind == PropKind::I16 || count_kind == PropKind::U16 ||
                        count_kind == PropKind::I32 || count_kind == PropKind::U32;
                    if (!integer_count || prop_type(item_type_name).kind == PropKind::Unknown) {
                        return {false, "Invalid PLY: unsupported list property type", {}, {}};
                    }
                } else {
                    property >> property_name >> extra;
                    if (prop_type(type_name).kind == PropKind::Unknown) {
                        return {false, "Invalid PLY: unsupported scalar property type", {}, {}};
                    }
                }
                if (keyword != "property" || type_name.empty() || property_name.empty() ||
                    !extra.empty()) {
                    return {false, "Invalid PLY: malformed property declaration", {}, {}};
                }
                if (!property_names.insert(property_name).second) {
                    return {false, "Invalid PLY: duplicate property declaration", {}, {}};
                }
                if (!account_metadata(property_name)) {
                    return {false,
                            "PLY property names exceed the metadata limit",
                            {},
                            {},
                            ErrorCode::resource_limit};
                }
                ++dropped_empty_schema_entries;
            } else if (line.rfind("comment", 0) == 0) {
                if (line != "comment" && line.rfind("comment ", 0) != 0) {
                    return {false, "Invalid PLY: malformed comment declaration", {}, {}};
                }
                const std::string content = line == "comment" ? std::string{} : line.substr(8);
                if (!account_metadata(content)) {
                    return {false,
                            "PLY comments exceed the metadata limit",
                            {},
                            {},
                            ErrorCode::resource_limit};
                }
                if (content.rfind("melkor_", 0) == 0) {
                    std::istringstream marker(content);
                    marker.imbue(std::locale::classic());
                    std::string name;
                    std::string value;
                    std::string extra;
                    marker >> name >> value >> extra;
                    if (name.empty() || value.empty() || !extra.empty()) {
                        return {false, "Invalid PLY: malformed Melkor marker", {}, {}};
                    }
                    const auto set_string_marker =
                        [&](std::optional<std::string>& destination) -> bool {
                        if (destination.has_value())
                            return false;
                        destination = value;
                        return true;
                    };
                    bool marker_ok = false;
                    if (name == "melkor_profile")
                        marker_ok = set_string_marker(marker_profile);
                    else if (name == "melkor_coordinate_system")
                        marker_ok = set_string_marker(marker_frame);
                    else if (name == "melkor_color_space")
                        marker_ok = set_string_marker(marker_color_space);
                    else if (name == "melkor_length_unit")
                        marker_ok = set_string_marker(marker_length_unit);
                    else if (name == "melkor_quaternion_order")
                        marker_ok = set_string_marker(marker_quaternion_order);
                    else if (name == "melkor_scale_domain")
                        marker_ok = set_string_marker(marker_scale_domain);
                    else if (name == "melkor_opacity_domain")
                        marker_ok = set_string_marker(marker_opacity_domain);
                    else if (name == "melkor_sh_basis")
                        marker_ok = set_string_marker(marker_sh_basis);
                    else if (name == "melkor_sh_degree") {
                        if (!marker_sh_degree.has_value()) {
                            std::uint32_t parsed_degree = 0;
                            const auto parsed = std::from_chars(
                                value.data(), value.data() + value.size(), parsed_degree);
                            marker_ok = parsed.ec == std::errc{} &&
                                        parsed.ptr == value.data() + value.size() &&
                                        parsed_degree <= 4;
                            if (marker_ok)
                                marker_sh_degree = parsed_degree;
                        }
                    } else if (name == "melkor_antialiased") {
                        if (!marker_antialiased.has_value() && (value == "0" || value == "1")) {
                            marker_antialiased = value == "1";
                            marker_ok = true;
                        }
                    } else {
                        return {false,
                                "Unsupported PLY: unknown Melkor marker",
                                {},
                                {},
                                ErrorCode::unsupported_feature};
                    }
                    if (!marker_ok) {
                        return {false, "Invalid PLY: duplicate or invalid Melkor marker", {}, {}};
                    }
                } else if (content.empty()) {
                    ++dropped_empty_schema_entries;
                } else {
                    ++dropped_free_form_metadata_entries;
                }
            } else if (line == "obj_info" || line.rfind("obj_info ", 0) == 0) {
                const std::string_view content(line.data() + 8, line.size() - 8);
                if (!account_metadata(content)) {
                    return {false,
                            "PLY object information exceeds the metadata limit",
                            {},
                            {},
                            ErrorCode::resource_limit};
                }
                const bool has_content = std::any_of(content.begin(), content.end(), [](char byte) {
                    return byte != ' ' && byte != '\t';
                });
                if (has_content)
                    ++dropped_free_form_metadata_entries;
                else
                    ++dropped_empty_schema_entries;
                continue;
            } else {
                return {false, "Invalid PLY: unknown header directive", {}, {}};
            }
        }
        if (!found_end) {
            return {false, "Invalid PLY: no end_header found", {}, {}};
        }
        if (!saw_format) {
            return {false, "Invalid PLY: missing format declaration", {}, {}};
        }
        if (!saw_vertex) {
            return {false, "Invalid PLY: no vertex element found", {}, {}};
        }
    }

    result.metadata.declared_vertices = vertex_count;
    result.metadata.encoding = is_big_endian ? Metadata::Encoding::BinaryBigEndian
                               : is_binary   ? Metadata::Encoding::BinaryLittleEndian
                                             : Metadata::Encoding::Ascii;

    if (vertex_props.empty()) {
        return {false, "Invalid PLY: no vertex properties found", {}, {}};
    }

    auto find_idx = [&](const std::string& name) -> int {
        for (size_t i = 0; i < vertex_props.size(); ++i) {
            if (vertex_props[i].name == name)
                return static_cast<int>(i);
        }
        return -1;
    };

    const int ix = find_idx("x");
    const int iy = find_idx("y");
    const int iz = find_idx("z");
    if (ix < 0 || iy < 0 || iz < 0) {
        return {false, "Invalid PLY: missing x/y/z position properties", {}, {}};
    }

    const int ifdc0 = find_idx("f_dc_0");
    const int ifdc1 = find_idx("f_dc_1");
    const int ifdc2 = find_idx("f_dc_2");
    const int iopacity = find_idx("opacity");
    const int graph_scale[3] = {find_idx("scale_0"), find_idx("scale_1"), find_idx("scale_2")};
    const int graph_rotation[4] = {find_idx("rot_0"), find_idx("rot_1"), find_idx("rot_2"),
                                   find_idx("rot_3")};
    const int graph_normal[3] = {find_idx("nx"), find_idx("ny"), find_idx("nz")};
    const int canonical_scale[3] = {find_idx("scale_x"), find_idx("scale_y"), find_idx("scale_z")};
    const int canonical_rotation[4] = {find_idx("rotation_x"), find_idx("rotation_y"),
                                       find_idx("rotation_z"), find_idx("rotation_w")};

    const bool graph_fixed = ifdc0 >= 0 && ifdc1 >= 0 && ifdc2 >= 0 && iopacity >= 0 &&
                             std::all_of(std::begin(graph_scale), std::end(graph_scale),
                                         [](int value) { return value >= 0; }) &&
                             std::all_of(std::begin(graph_rotation), std::end(graph_rotation),
                                         [](int value) { return value >= 0; });
    const bool canonical_fixed =
        iopacity >= 0 &&
        std::all_of(std::begin(canonical_scale), std::end(canonical_scale),
                    [](int value) { return value >= 0; }) &&
        std::all_of(std::begin(canonical_rotation), std::end(canonical_rotation),
                    [](int value) { return value >= 0; });
    const bool any_graph_field = ifdc0 >= 0 || ifdc1 >= 0 || ifdc2 >= 0 || graph_scale[0] >= 0 ||
                                 graph_scale[1] >= 0 || graph_scale[2] >= 0 ||
                                 graph_rotation[0] >= 0 || graph_rotation[1] >= 0 ||
                                 graph_rotation[2] >= 0 || graph_rotation[3] >= 0;
    const bool any_canonical_field =
        canonical_scale[0] >= 0 || canonical_scale[1] >= 0 || canonical_scale[2] >= 0 ||
        canonical_rotation[0] >= 0 || canonical_rotation[1] >= 0 || canonical_rotation[2] >= 0 ||
        canonical_rotation[3] >= 0 ||
        std::any_of(vertex_props.begin(), vertex_props.end(),
                    [](const Property& property) { return property.name.rfind("sh_", 0) == 0; });

    std::optional<FormatProfileId> marked_profile;
    if (marker_profile.has_value()) {
        if (*marker_profile == "melkor-canonical-v1" ||
            *marker_profile == "ply:melkor-canonical-v1") {
            marked_profile = FormatProfileId::ply_melkor_canonical_v1;
        } else if (*marker_profile == "graphdeco-3dgs-v1" ||
                   *marker_profile == "ply:graphdeco-3dgs-v1") {
            marked_profile = FormatProfileId::ply_graphdeco_3dgs_v1;
        } else if (*marker_profile == "da3-gaussian-v1" ||
                   *marker_profile == "ply:da3-gaussian-v1") {
            marked_profile = FormatProfileId::ply_da3_gaussian_v1;
        } else {
            return {false,
                    "Unsupported PLY: unknown profile marker",
                    {},
                    {},
                    ErrorCode::unsupported_feature};
        }
    }
    if (config.profile.has_value()) {
        const FormatProfile& requested_profile = format_profile(*config.profile);
        if (!requested_profile.supports_read_container(FormatId::ply)) {
            return {false,
                    "The selected profile does not support PLY input",
                    {},
                    {},
                    ErrorCode::invalid_argument};
        }
        if (marked_profile.has_value() && *marked_profile != *config.profile) {
            return {false,
                    "PLY profile marker conflicts with the selected input profile",
                    {},
                    {},
                    ErrorCode::invalid_argument};
        }
    }

    FormatProfileId selected_profile = FormatProfileId::unknown;
    if (config.profile.has_value())
        selected_profile = *config.profile;
    else if (marked_profile.has_value())
        selected_profile = *marked_profile;
    else if (canonical_fixed && any_canonical_field)
        selected_profile = FormatProfileId::ply_melkor_canonical_v1;
    else if (graph_fixed)
        selected_profile = FormatProfileId::ply_graphdeco_3dgs_v1;

    if (any_graph_field && any_canonical_field) {
        return {
            false, "Invalid PLY: training-layout and canonical properties cannot be mixed", {}, {}};
    }
    if (selected_profile == FormatProfileId::unknown) {
        if (any_graph_field || any_canonical_field) {
            return {false, "Invalid PLY: Gaussian profile fields are incomplete", {}, {}};
        }
        return {false,
                "Unsupported PLY: the vertex properties do not define a supported Gaussian "
                "profile",
                {},
                {},
                ErrorCode::unsupported_feature};
    }

    if (selected_profile == FormatProfileId::ply_melkor_canonical_v1 && !canonical_fixed) {
        return {false, "Invalid PLY: canonical profile fields are incomplete", {}, {}};
    }
    if (usesGraphdecoLayout(selected_profile) && !graph_fixed) {
        return {false, "Invalid PLY: training-layout profile fields are incomplete", {}, {}};
    }
    result.metadata.profile = selected_profile;
    result.metadata.has_profile_marker = marker_profile.has_value();
    result.metadata.has_position = true;

    const auto marker_must_equal = [&](const std::optional<std::string>& marker,
                                       const char* expected,
                                       const char* name) -> std::optional<std::string> {
        if (marker.has_value() && *marker != expected)
            return std::string("PLY ") + name + " marker conflicts with its profile";
        return std::nullopt;
    };
    if (marker_length_unit.has_value() && *marker_length_unit != "meter") {
        return {false,
                "Unsupported PLY: only meter length markers are supported",
                {},
                {},
                ErrorCode::unsupported_feature};
    }
    const bool canonical_profile = selected_profile == FormatProfileId::ply_melkor_canonical_v1;
    const bool da3_profile = selected_profile == FormatProfileId::ply_da3_gaussian_v1;
    if (auto mismatch = marker_must_equal(marker_quaternion_order,
                                          canonical_profile ? "xyzw" : "wxyz", "quaternion-order");
        mismatch.has_value()) {
        return {false, *mismatch, {}, {}};
    }
    if (auto mismatch = marker_must_equal(marker_scale_domain, canonical_profile ? "linear" : "log",
                                          "scale-domain");
        mismatch.has_value()) {
        return {false, *mismatch, {}, {}};
    }
    if (auto mismatch = marker_must_equal(marker_opacity_domain,
                                          canonical_profile ? "linear" : "logit", "opacity-domain");
        mismatch.has_value()) {
        return {false, *mismatch, {}, {}};
    }
    if (auto mismatch = marker_must_equal(marker_sh_basis, "real_condon_shortley", "SH-basis");
        mismatch.has_value()) {
        return {false, *mismatch, {}, {}};
    }

    std::optional<std::string> source_frame_id = marker_frame;
    if (config.source_frame_id.has_value()) {
        if (source_frame_id.has_value() && *source_frame_id != *config.source_frame_id) {
            return {false,
                    "PLY coordinate marker conflicts with the selected source frame",
                    {},
                    {},
                    ErrorCode::invalid_argument};
        }
        source_frame_id = config.source_frame_id;
    }
    const char* fixed_source_frame = canonical_profile ? "gltf-luf"
                                     : da3_profile     ? "ply-rdf"
                                                       : nullptr;
    if (fixed_source_frame != nullptr && source_frame_id.has_value() &&
        *source_frame_id != fixed_source_frame) {
        return {false,
                canonical_profile ? "The canonical PLY profile requires the gltf-luf source frame"
                                  : "The DA3 PLY profile requires the ply-rdf source frame",
                {},
                {},
                ErrorCode::invalid_argument};
    }
    if (!source_frame_id.has_value() && fixed_source_frame != nullptr)
        source_frame_id = fixed_source_frame;
    if (!source_frame_id.has_value()) {
        return {false,
                "PLY input requires an explicit source frame or a Melkor coordinate marker",
                {},
                {},
                ErrorCode::invalid_argument};
    }
    auto source_frame = math::frame_by_id(*source_frame_id);
    if (!source_frame.has_value())
        return plyReadFailure(source_frame, "Invalid PLY source frame");
    std::optional<double> source_unit_to_meter;
    if (marker_length_unit.has_value() || canonical_profile)
        source_unit_to_meter = 1.0;
    if (config.source_unit_to_meter.has_value()) {
        if (!std::isfinite(*config.source_unit_to_meter) || *config.source_unit_to_meter <= 0.0) {
            return {false,
                    "The PLY source unit scale must be finite and positive",
                    {},
                    {},
                    ErrorCode::invalid_argument};
        }
        if (source_unit_to_meter.has_value() &&
            *source_unit_to_meter != *config.source_unit_to_meter) {
            return {false,
                    "PLY length-unit marker conflicts with the selected source unit",
                    {},
                    {},
                    ErrorCode::invalid_argument};
        }
        source_unit_to_meter = config.source_unit_to_meter;
    }
    if (!source_unit_to_meter.has_value()) {
        return {false,
                "PLY input requires an explicit length-unit scale or a Melkor unit marker",
                {},
                {},
                ErrorCode::invalid_argument};
    }
    source_frame.value().unit_to_meter = *source_unit_to_meter;
    if (source_frame.value().includes_reflection) {
        return {false,
                "PLY input does not support a reflecting source frame",
                {},
                {},
                ErrorCode::unsupported_feature};
    }
    result.metadata.source_frame_id = source_frame_id;
    result.metadata.source_unit_to_meter = source_unit_to_meter;
    result.metadata.has_coordinate_marker = marker_frame.has_value();
    result.metadata.has_length_unit_marker = marker_length_unit.has_value();

    std::optional<ColorSpace> source_color_space;
    if (marker_color_space.has_value()) {
        source_color_space = color_space_from_string(*marker_color_space);
        if (!source_color_space.has_value()) {
            return {false,
                    "Unsupported PLY: unknown color-space marker",
                    {},
                    {},
                    ErrorCode::unsupported_feature};
        }
    }
    if (config.source_color_space.has_value()) {
        if (!is_valid(*config.source_color_space)) {
            return {false,
                    "The PLY source color space is invalid",
                    {},
                    {},
                    ErrorCode::invalid_argument};
        }
        if (source_color_space.has_value() && *source_color_space != *config.source_color_space) {
            return {false,
                    "PLY color marker conflicts with the selected source color space",
                    {},
                    {},
                    ErrorCode::invalid_argument};
        }
        source_color_space = config.source_color_space;
    }
    if (!source_color_space.has_value()) {
        return {false,
                "PLY input requires an explicit color space or a Melkor color marker",
                {},
                {},
                ErrorCode::invalid_argument};
    }
    result.metadata.color_space = source_color_space;
    result.metadata.has_color_space_marker = marker_color_space.has_value();
    result.metadata.antialiased = marker_antialiased;

    int is0 = canonical_profile ? canonical_scale[0] : graph_scale[0];
    int is1 = canonical_profile ? canonical_scale[1] : graph_scale[1];
    int is2 = canonical_profile ? canonical_scale[2] : graph_scale[2];
    int ir0 = canonical_profile ? canonical_rotation[0] : graph_rotation[0];
    int ir1 = canonical_profile ? canonical_rotation[1] : graph_rotation[1];
    int ir2 = canonical_profile ? canonical_rotation[2] : graph_rotation[2];
    int ir3 = canonical_profile ? canonical_rotation[3] : graph_rotation[3];
    result.metadata.has_opacity = iopacity >= 0;
    result.metadata.has_scale = is0 >= 0 && is1 >= 0 && is2 >= 0;
    result.metadata.has_rotation = ir0 >= 0 && ir1 >= 0 && ir2 >= 0 && ir3 >= 0;

    std::vector<int> sh_rest_idx;
    std::vector<int> canonical_sh_idx;
    std::uint32_t sh_degree = 0;
    if (usesGraphdecoLayout(selected_profile)) {
        const std::size_t normal_count =
            static_cast<std::size_t>(std::count_if(std::begin(graph_normal), std::end(graph_normal),
                                                   [](int index) { return index >= 0; }));
        if (normal_count != 0 && normal_count != 3) {
            return {
                false, "Invalid PLY: training-layout normals must contain nx, ny, and nz", {}, {}};
        }
        std::vector<std::pair<size_t, int>> indexed_sh_rest;
        for (size_t property_index = 0; property_index < vertex_props.size(); ++property_index) {
            const auto& property = vertex_props[property_index];
            if (property.name.rfind("f_rest_", 0) != 0)
                continue;
            const std::string suffix = property.name.substr(7);
            size_t coefficient = 0;
            const auto parsed =
                std::from_chars(suffix.data(), suffix.data() + suffix.size(), coefficient);
            if (suffix.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != suffix.data() + suffix.size() || coefficient > 71 ||
                suffix != std::to_string(coefficient)) {
                return {false, "Invalid PLY: malformed f_rest coefficient name", {}, {}};
            }
            indexed_sh_rest.emplace_back(coefficient, static_cast<int>(property_index));
        }
        if (!indexed_sh_rest.empty()) {
            std::sort(indexed_sh_rest.begin(), indexed_sh_rest.end());
            const size_t coefficient_count = indexed_sh_rest.size();
            if (coefficient_count != 9 && coefficient_count != 24 && coefficient_count != 45 &&
                coefficient_count != 72) {
                return {false,
                        "Invalid PLY: f_rest count does not match SH degree 1, 2, 3, or 4",
                        {},
                        {}};
            }
            for (size_t coefficient = 0; coefficient < coefficient_count; ++coefficient) {
                if (indexed_sh_rest[coefficient].first != coefficient) {
                    return {false,
                            "Invalid PLY: f_rest coefficients must be contiguous from zero",
                            {},
                            {}};
                }
                sh_rest_idx.push_back(indexed_sh_rest[coefficient].second);
            }
            sh_degree = coefficient_count == 9    ? 1
                        : coefficient_count == 24 ? 2
                        : coefficient_count == 45 ? 3
                                                  : 4;
            if (sh_degree > format_profile(selected_profile).max_sh_degree) {
                return {false,
                        "Invalid PLY: SH degree exceeds the selected profile",
                        {},
                        {},
                        ErrorCode::invalid_data};
            }
        }
        result.metadata.has_sh_dc = true;
    } else if (canonical_profile) {
        std::array<int, 75> indices{};
        indices.fill(-1);
        std::uint32_t maximum_degree = 0;
        std::size_t named_count = 0;
        for (size_t property_index = 0; property_index < vertex_props.size(); ++property_index) {
            const std::string& name = vertex_props[property_index].name;
            if (name.rfind("sh_", 0) != 0)
                continue;
            const std::size_t first = name.find('_', 3);
            const std::size_t second =
                first == std::string::npos ? std::string::npos : name.find('_', first + 1);
            if (first == std::string::npos || second == std::string::npos ||
                second + 2 != name.size()) {
                return {false, "Invalid PLY: malformed canonical SH property name", {}, {}};
            }
            std::uint32_t property_degree = 0;
            std::uint32_t band_index = 0;
            const auto parsed_degree =
                std::from_chars(name.data() + 3, name.data() + first, property_degree);
            const auto parsed_index =
                std::from_chars(name.data() + first + 1, name.data() + second, band_index);
            const char channel = name.back();
            const int channel_index = channel == 'r'   ? 0
                                      : channel == 'g' ? 1
                                      : channel == 'b' ? 2
                                                       : -1;
            if (parsed_degree.ec != std::errc{} || parsed_degree.ptr != name.data() + first ||
                parsed_index.ec != std::errc{} || parsed_index.ptr != name.data() + second ||
                property_degree > 4 || band_index > 2 * property_degree || channel_index < 0) {
                return {false, "Invalid PLY: malformed canonical SH property name", {}, {}};
            }
            const std::size_t coefficient =
                static_cast<std::size_t>(property_degree) * property_degree + band_index;
            if (name != canonicalShProperty(static_cast<std::uint32_t>(coefficient),
                                            static_cast<std::size_t>(channel_index))) {
                return {false, "Invalid PLY: canonical SH property name is not canonical", {}, {}};
            }
            const std::size_t slot = coefficient * 3 + static_cast<std::size_t>(channel_index);
            if (indices[slot] >= 0) {
                return {false, "Invalid PLY: duplicate canonical SH coefficient", {}, {}};
            }
            indices[slot] = static_cast<int>(property_index);
            maximum_degree = std::max(maximum_degree, property_degree);
            ++named_count;
        }
        const std::size_t expected =
            static_cast<std::size_t>(maximum_degree + 1) * (maximum_degree + 1) * 3;
        if (named_count == 0 || named_count != expected ||
            std::any_of(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(expected),
                        [](int index) { return index < 0; })) {
            return {false, "Invalid PLY: canonical SH degrees must be complete", {}, {}};
        }
        canonical_sh_idx.assign(indices.begin(),
                                indices.begin() + static_cast<std::ptrdiff_t>(expected));
        sh_degree = maximum_degree;
        result.metadata.has_sh_dc = true;
    }
    result.metadata.has_sh_rest = sh_degree > 0;
    result.metadata.sh_degree = sh_degree;
    if (marker_sh_degree.has_value() && *marker_sh_degree != sh_degree) {
        return {false, "PLY SH-degree marker conflicts with the property set", {}, {}};
    }

    const auto property_is_known = [&](const std::string& name) {
        if (name == "x" || name == "y" || name == "z")
            return true;
        if (canonical_profile) {
            return name == "scale_x" || name == "scale_y" || name == "scale_z" ||
                   name == "rotation_x" || name == "rotation_y" || name == "rotation_z" ||
                   name == "rotation_w" || name == "opacity" || name.rfind("sh_", 0) == 0;
        }
        if (usesGraphdecoLayout(selected_profile)) {
            return name == "nx" || name == "ny" || name == "nz" || name == "f_dc_0" ||
                   name == "f_dc_1" || name == "f_dc_2" || name == "opacity" || name == "scale_0" ||
                   name == "scale_1" || name == "scale_2" || name == "rot_0" || name == "rot_1" ||
                   name == "rot_2" || name == "rot_3" || name.rfind("f_rest_", 0) == 0;
        }
        return false;
    };
    for (const Property& property : vertex_props) {
        if (property_is_known(property.name) && property.type.kind != PropKind::F32) {
            return {false, "Invalid PLY: Gaussian profile properties must use float32", {}, {}};
        }
    }
    for (const Property& property : vertex_props) {
        if (!property_is_known(property.name))
            ++result.metadata.unknown_property_count;
    }
    if (result.metadata.unknown_property_count != 0) {
        LossItem item;
        item.code = loss_code::kUnknownPropertyDropped;
        item.severity = LossSeverity::severe;
        item.source_feature = std::to_string(result.metadata.unknown_property_count) +
                              " PLY vertex properties outside the selected profile";
        item.target_constraint = "the canonical Gaussian model does not retain these properties";
        item.affected_splats = vertex_count;
        item.remediation = "remove the properties or approve LOSS_UNKNOWN_PROPERTY_DROPPED";
        auto added = result.losses.add(std::move(item));
        if (!added.has_value())
            return plyReadFailure(added, "Invalid PLY loss report", std::move(result.losses));
    }
    if (dropped_free_form_metadata_entries != 0) {
        LossItem item;
        item.code = loss_code::kMetadataDropped;
        item.severity = LossSeverity::severe;
        item.source_feature = std::to_string(dropped_free_form_metadata_entries) +
                              " PLY comment or object-information entries";
        item.target_constraint = "the canonical Gaussian model does not retain free-form metadata";
        item.affected_splats = 0;
        item.remediation = "keep the source PLY or approve LOSS_METADATA_DROPPED";
        auto added = result.losses.add(std::move(item));
        if (!added.has_value())
            return plyReadFailure(added, "Invalid PLY loss report", std::move(result.losses));
    }
    if (dropped_empty_schema_entries != 0) {
        LossItem item;
        item.code = loss_code::kMetadataDropped;
        item.severity = LossSeverity::info;
        item.source_feature = std::to_string(dropped_empty_schema_entries) +
                              " empty PLY element or property declarations";
        item.target_constraint = "the canonical Gaussian model does not retain empty schemas";
        item.affected_splats = 0;
        item.remediation = "keep the source PLY when the empty schema is required";
        auto added = result.losses.add(std::move(item));
        if (!added.has_value())
            return plyReadFailure(added, "Invalid PLY loss report", std::move(result.losses));
    }

    // Record stride and per-property byte offsets from the canonical type
    // sizes (used consistently for validation and binary decoding).
    size_t stride = 0;
    for (auto& p : vertex_props) {
        p.offset = stride;
        stride += p.type.size;
    }

    // Validate the declared vertex count against what the remaining buffer
    // could possibly hold before reserving, so a malformed count fails cleanly
    // instead of causing a huge allocation or an out-of-bounds read. All
    // arithmetic is done as 64-bit division to avoid count*stride overflow.
    const size_t remaining = size - header_bytes;
    if (is_binary) {
        if (stride == 0 || vertex_count > remaining / stride) {
            return {false, "Invalid PLY: data section too small for declared vertex count", {}, {}};
        }
        if (vertex_count * stride != remaining) {
            return {false, "Invalid PLY: binary data has trailing or unclaimed bytes", {}, {}};
        }
    } else {
        // ASCII data size is not stride-predictable, but each value needs at
        // least one character plus a separator, so one vertex record cannot
        // occupy fewer than 2*props - 1 bytes. Cap the count accordingly.
        const size_t min_record = 2 * vertex_props.size() - 1;
        if (vertex_count > remaining / min_record + 1) {
            return {false, "Invalid PLY: declared vertex count exceeds data size", {}, {}};
        }
    }

    // Charge the reconstructed canonical arrays before reserving them, so a well-formed
    // header declaring an enormous vertex count is refused by policy. Per-splat memory includes the
    // position, scale, rotation, opacity, and the complete SH block. Both resource-accounting and
    // host-address-space products are checked explicitly before allocation.
    if (auto charged = context.observe(BudgetKind::splats, vertex_count, "ply.vertices");
        !charged.has_value()) {
        return {false,
                charged.diagnostics().empty() ? "PLY vertex count exceeds the splat limit"
                                              : charged.diagnostics()[0].message,
                {},
                {},
                ErrorCode::resource_limit};
    }
    const std::uint64_t coefficients =
        static_cast<std::uint64_t>(sh_degree + 1) * static_cast<std::uint64_t>(sh_degree + 1);
    const std::uint64_t per_splat_bytes =
        2 * sizeof(Vec3f) + sizeof(Quatf) + sizeof(float) + coefficients * 3 * sizeof(float);
    if (static_cast<std::uint64_t>(vertex_count) >
        std::numeric_limits<std::uint64_t>::max() / per_splat_bytes) {
        return {false,
                "PLY canonical allocation size overflows resource accounting",
                {},
                {},
                ErrorCode::resource_limit};
    }
    const std::uint64_t canonical_bytes =
        static_cast<std::uint64_t>(vertex_count) * per_splat_bytes;
    auto canonical_reservation =
        context.budget->reserve(BudgetKind::memory_bytes, canonical_bytes, "ply.canonical_data");
    if (!canonical_reservation.has_value()) {
        return {false,
                canonical_reservation.diagnostics().empty()
                    ? "PLY cloud exceeds the memory limit"
                    : canonical_reservation.diagnostics()[0].message,
                {},
                {},
                ErrorCode::resource_limit};
    }
    Budget::Charge canonical_charge = std::move(canonical_reservation).value();

    SplatBufferInput canonical_input;
    canonical_input.positions.resize(vertex_count);
    canonical_input.scales.resize(vertex_count);
    canonical_input.rotations.resize(vertex_count);
    canonical_input.opacities.resize(vertex_count);
    if (vertex_count >
        std::numeric_limits<std::size_t>::max() / (static_cast<std::size_t>(coefficients) * 3)) {
        return {false,
                "PLY SH allocation size overflows the host address space",
                {},
                {},
                ErrorCode::resource_limit};
    }
    std::vector<float> sh_values(vertex_count * static_cast<std::size_t>(coefficients) * 3, 0.0f);
    const uint8_t* vertex_data = data + header_bytes;

    // True when the file's byte order differs from the host's; the host order
    // is probed at runtime so the swap stays generic instead of assuming a
    // little-endian machine.
    const bool host_is_little = [] {
        const uint16_t probe = 1;
        uint8_t first;
        std::memcpy(&first, &probe, 1);
        return first == 1;
    }();
    const bool needs_swap = (is_big_endian == host_is_little);

    // Read a single property value (at the given property index) for the
    // current vertex record starting at `base`, normalized to float. Multi-byte
    // values are byte-swapped as needed for the file's declared endianness.
    auto read_prop = [&](const uint8_t* base, int prop_idx) -> float {
        const auto& p = vertex_props[static_cast<size_t>(prop_idx)];
        uint8_t raw[8];
        std::memcpy(raw, base + p.offset, p.type.size);
        if (needs_swap && p.type.size > 1) {
            std::reverse(raw, raw + p.type.size);
        }
        switch (p.type.kind) {
        case PropKind::F64: {
            double d;
            std::memcpy(&d, raw, 8);
            return static_cast<float>(d);
        }
        case PropKind::I8:
            return static_cast<float>(static_cast<int8_t>(raw[0]));
        case PropKind::U8:
            return static_cast<float>(raw[0]);
        case PropKind::I16: {
            int16_t s;
            std::memcpy(&s, raw, 2);
            return static_cast<float>(s);
        }
        case PropKind::U16: {
            uint16_t u;
            std::memcpy(&u, raw, 2);
            return static_cast<float>(u);
        }
        case PropKind::I32: {
            int32_t v;
            std::memcpy(&v, raw, 4);
            return static_cast<float>(v);
        }
        case PropKind::U32: {
            uint32_t u;
            std::memcpy(&u, raw, 4);
            return static_cast<float>(u);
        }
        case PropKind::F32:
        case PropKind::Unknown:
        default: {
            float f;
            std::memcpy(&f, raw, 4);
            return f;
        }
        }
    };

    auto parse_ascii_value = [](std::string_view token, PropKind kind, bool retain,
                                float& output) -> bool {
        if (token.empty())
            return false;
        if (token.front() == '+') {
            token.remove_prefix(1);
            if (token.empty())
                return false;
        }
        const char* begin = token.data();
        const char* end = begin + token.size();
        switch (kind) {
        case PropKind::F32: {
            float value = 0.0f;
            if (!text::parseClassicFloat(token, value))
                return false;
            output = value;
            return true;
        }
        case PropKind::F64: {
            double value = 0.0;
            if (!text::parseClassicDouble(token, value))
                return false;
            if (!retain) {
                output = 0.0f;
                return true;
            }
            if (std::isfinite(value) &&
                (std::abs(value) > std::numeric_limits<float>::max() ||
                 (value != 0.0 && std::abs(value) < std::numeric_limits<float>::denorm_min()))) {
                return false;
            }
            output = static_cast<float>(value);
            return true;
        }
        case PropKind::U8:
        case PropKind::U16:
        case PropKind::U32: {
            if (token.front() == '-')
                return false;
            std::uint64_t value = 0;
            const auto parsed = std::from_chars(begin, end, value, 10);
            const std::uint64_t maximum = kind == PropKind::U8 ? std::numeric_limits<uint8_t>::max()
                                          : kind == PropKind::U16
                                              ? std::numeric_limits<uint16_t>::max()
                                              : std::numeric_limits<uint32_t>::max();
            if (parsed.ec != std::errc{} || parsed.ptr != end || value > maximum) {
                return false;
            }
            output = static_cast<float>(value);
            return true;
        }
        case PropKind::I8:
        case PropKind::I16:
        case PropKind::I32: {
            std::int64_t value = 0;
            const auto parsed = std::from_chars(begin, end, value, 10);
            const std::int64_t minimum = kind == PropKind::I8 ? std::numeric_limits<int8_t>::min()
                                         : kind == PropKind::I16
                                             ? std::numeric_limits<int16_t>::min()
                                             : std::numeric_limits<int32_t>::min();
            const std::int64_t maximum = kind == PropKind::I8 ? std::numeric_limits<int8_t>::max()
                                         : kind == PropKind::I16
                                             ? std::numeric_limits<int16_t>::max()
                                             : std::numeric_limits<int32_t>::max();
            if (parsed.ec != std::errc{} || parsed.ptr != end || value < minimum ||
                value > maximum) {
                return false;
            }
            output = static_cast<float>(value);
            return true;
        }
        case PropKind::Unknown:
        default:
            return false;
        }
    };

    std::optional<math::ShRotation> source_sh_rotation;
    if (!has_identity_basis(source_frame.value()) && sh_degree > 0) {
        auto rotation = math::ShRotation::create(source_frame.value().to_canonical, sh_degree);
        if (!rotation.has_value())
            return plyReadFailure(rotation, "Could not rotate PLY spherical harmonics",
                                  std::move(result.losses));
        source_sh_rotation.emplace(std::move(rotation.value()));
    }

    std::size_t normalized_quaternion_count = 0;
    std::size_t dropped_normal_count = 0;
    auto decode_splat = [&](std::size_t i, auto&& value) -> Result<void> {
        const math::Vec3 source_position{value(ix), value(iy), value(iz)};
        const math::Vec3 transformed_position =
            math::position_to_canonical(source_frame.value(), source_position);
        if (!std::all_of(transformed_position.begin(), transformed_position.end(),
                         isFloatRepresentable)) {
            Diagnostic diagnostic("MK1214_PLY_TRANSFORM_RANGE", Severity::error,
                                  "A transformed PLY position exceeds the canonical float range");
            diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i));
            return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
        canonical_input.positions[i] = {
            static_cast<float>(transformed_position[0]),
            static_cast<float>(transformed_position[1]),
            static_cast<float>(transformed_position[2]),
        };

        const std::size_t coefficient_count = static_cast<std::size_t>(coefficients);
        const std::size_t sh_base = i * coefficient_count * 3;
        if (canonical_profile) {
            for (std::size_t sh_index = 0; sh_index < canonical_sh_idx.size(); ++sh_index)
                sh_values[sh_base + sh_index] = value(canonical_sh_idx[sh_index]);
        } else if (usesGraphdecoLayout(selected_profile)) {
            sh_values[sh_base] = value(ifdc0);
            sh_values[sh_base + 1] = value(ifdc1);
            sh_values[sh_base + 2] = value(ifdc2);
            const std::size_t higher_per_channel = coefficient_count - 1;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                for (std::size_t higher = 0; higher < higher_per_channel; ++higher) {
                    const std::size_t ply_index = channel * higher_per_channel + higher;
                    sh_values[sh_base + (higher + 1) * 3 + channel] = value(sh_rest_idx[ply_index]);
                }
            }
        }

        if (canonical_profile) {
            canonical_input.opacities[i] = value(iopacity);
        } else if (usesGraphdecoLayout(selected_profile)) {
            auto opacity = math::sigmoid_from_logit(value(iopacity));
            if (!opacity.has_value()) {
                auto diagnostics = opacity.diagnostics();
                for (auto& diagnostic : diagnostics) {
                    diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i));
                }
                return Result<void>::failure(opacity.error_code(), std::move(diagnostics));
            }
            canonical_input.opacities[i] = opacity.value();
        }

        Vec3f source_scale;
        if (canonical_profile) {
            source_scale = {value(is0), value(is1), value(is2)};
        } else if (usesGraphdecoLayout(selected_profile)) {
            const int scale_indices[3] = {is0, is1, is2};
            float* scale_components[3] = {&source_scale.x, &source_scale.y, &source_scale.z};
            for (std::size_t component = 0; component < 3; ++component) {
                auto decoded = math::linear_scale_from_log(value(scale_indices[component]));
                if (!decoded.has_value()) {
                    auto diagnostics = decoded.diagnostics();
                    for (auto& diagnostic : diagnostics) {
                        diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i))
                            .with_context("component", static_cast<std::uint64_t>(component));
                    }
                    return Result<void>::failure(decoded.error_code(), std::move(diagnostics));
                }
                *scale_components[component] = decoded.value();
            }
        }

        math::Quat source_rotation{0.0, 0.0, 0.0, 1.0};
        if (canonical_profile) {
            source_rotation = {value(ir0), value(ir1), value(ir2), value(ir3)};
        } else if (usesGraphdecoLayout(selected_profile)) {
            source_rotation = {value(ir1), value(ir2), value(ir3), value(ir0)};
        }
        const bool normalize_graphdeco_rotation =
            usesGraphdecoLayout(selected_profile) && !math::is_unit(source_rotation);
        if (canonical_profile && !math::is_unit(source_rotation)) {
            Diagnostic diagnostic("MK1212_PLY_NON_UNIT_ROTATION", Severity::error,
                                  "Canonical PLY rotation is not unit within the tolerance");
            diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i))
                .with_context("norm", math::norm(source_rotation));
            return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
        auto normalized = math::normalize(source_rotation);
        if (!normalized.has_value()) {
            auto diagnostics = normalized.diagnostics();
            for (auto& diagnostic : diagnostics)
                diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i));
            return Result<void>::failure(normalized.error_code(), std::move(diagnostics));
        }
        if (normalize_graphdeco_rotation)
            ++normalized_quaternion_count;

        if (usesGraphdecoLayout(selected_profile)) {
            const bool has_nonzero_normal =
                std::any_of(std::begin(graph_normal), std::end(graph_normal),
                            [&](int index) { return index >= 0 && value(index) != 0.0f; });
            if (has_nonzero_normal)
                ++dropped_normal_count;
        }
        math::Quat transformed_rotation = normalized.value();
        if (!has_identity_basis(source_frame.value())) {
            auto transformed =
                math::rotation_to_canonical(source_frame.value(), normalized.value());
            if (!transformed.has_value()) {
                auto diagnostics = transformed.diagnostics();
                for (auto& diagnostic : diagnostics)
                    diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i));
                return Result<void>::failure(transformed.error_code(), std::move(diagnostics));
            }
            transformed_rotation = transformed.value();
        }
        const math::Vec3 transformed_scale{
            static_cast<double>(source_scale.x) * source_frame.value().unit_to_meter,
            static_cast<double>(source_scale.y) * source_frame.value().unit_to_meter,
            static_cast<double>(source_scale.z) * source_frame.value().unit_to_meter,
        };
        if (!std::all_of(transformed_scale.begin(), transformed_scale.end(),
                         isFloatRepresentable) ||
            !isFloatRepresentable(transformed_rotation.x) ||
            !isFloatRepresentable(transformed_rotation.y) ||
            !isFloatRepresentable(transformed_rotation.z) ||
            !isFloatRepresentable(transformed_rotation.w)) {
            Diagnostic diagnostic("MK1214_PLY_TRANSFORM_RANGE", Severity::error,
                                  "A transformed PLY shape exceeds the canonical float range");
            diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i));
            return Result<void>::failure(ErrorCode::invalid_data, std::move(diagnostic));
        }
        canonical_input.scales[i] = {
            static_cast<float>(transformed_scale[0]),
            static_cast<float>(transformed_scale[1]),
            static_cast<float>(transformed_scale[2]),
        };
        canonical_input.rotations[i] = {
            static_cast<float>(transformed_rotation.x),
            static_cast<float>(transformed_rotation.y),
            static_cast<float>(transformed_rotation.z),
            static_cast<float>(transformed_rotation.w),
        };
        if (source_sh_rotation.has_value()) {
            auto rotated = source_sh_rotation->rotate_block(sh_values.data() + sh_base, 3);
            if (!rotated.has_value()) {
                auto diagnostics = rotated.diagnostics();
                for (auto& diagnostic : diagnostics)
                    diagnostic.with_context("splat_index", static_cast<std::uint64_t>(i));
                return Result<void>::failure(rotated.error_code(), std::move(diagnostics));
            }
        }
        return Result<void>::success();
    };

    if (is_binary) {
        for (size_t i = 0; i < vertex_count; ++i) {
            if (i % 1024 == 0) {
                auto vertex_control =
                    context.checkpoint({"ply.read", "vertices", i, vertex_count, "splats"});
                if (!vertex_control.has_value())
                    return plyReadControlFailure(vertex_control);
            }
            const uint8_t* base = vertex_data + i * stride;
            auto decoded = decode_splat(i, [&](int property) { return read_prop(base, property); });
            if (!decoded.has_value())
                return plyReadFailure(decoded, "Invalid PLY vertex", std::move(result.losses));
        }
    } else {
        auto value_bytes =
            checked_array_bytes(vertex_props.size(), sizeof(float), "PLY ASCII value buffer");
        if (!value_bytes.has_value())
            return plyReadFailure(value_bytes, "PLY ASCII value buffer size overflow",
                                  std::move(result.losses));
        if (auto charged =
                context.consume(BudgetKind::memory_bytes, value_bytes.value(), "ply.ascii_values");
            !charged.has_value()) {
            return plyReadFailure(charged, "PLY ASCII values exceed the memory limit",
                                  std::move(result.losses));
        }
        ScopedBudgetRelease ascii_release(context.budget, BudgetKind::memory_bytes,
                                          value_bytes.value());
        std::vector<float> vals(vertex_props.size());
        const std::string_view payload(reinterpret_cast<const char*>(vertex_data),
                                       size - header_bytes);
        std::size_t payload_offset = 0;
        const auto is_space = [](char byte) {
            return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' || byte == '\v' ||
                   byte == '\f';
        };
        for (size_t i = 0; i < vertex_count; ++i) {
            if (i % 1024 == 0) {
                auto vertex_control =
                    context.checkpoint({"ply.read", "vertices", i, vertex_count, "splats"});
                if (!vertex_control.has_value())
                    return plyReadControlFailure(vertex_control);
            }
            if (payload_offset >= payload.size()) {
                return {false,
                        "Invalid PLY: missing ASCII record for vertex " + std::to_string(i),
                        {},
                        {}};
            }

            const std::size_t newline = payload.find('\n', payload_offset);
            const std::size_t record_end =
                newline == std::string_view::npos ? payload.size() : newline;
            std::string_view record = payload.substr(payload_offset, record_end - payload_offset);
            if (!record.empty() && record.back() == '\r')
                record.remove_suffix(1);
            payload_offset = newline == std::string_view::npos ? payload.size() : newline + 1;

            std::size_t token_offset = 0;
            for (size_t j = 0; j < vertex_props.size(); ++j) {
                while (token_offset < record.size() && is_space(record[token_offset]))
                    ++token_offset;
                const std::size_t token_start = token_offset;
                while (token_offset < record.size() && !is_space(record[token_offset]))
                    ++token_offset;
                const std::string_view token =
                    record.substr(token_start, token_offset - token_start);
                if (!parse_ascii_value(token, vertex_props[j].type.kind,
                                       property_is_known(vertex_props[j].name), vals[j])) {
                    return {false,
                            "Invalid PLY: malformed scalar at vertex " + std::to_string(i),
                            {},
                            {}};
                }
            }
            while (token_offset < record.size() && is_space(record[token_offset]))
                ++token_offset;
            if (token_offset != record.size()) {
                return {false, "Invalid PLY: extra scalar at vertex " + std::to_string(i), {}, {}};
            }
            auto decoded = decode_splat(
                i, [&](int property) { return vals[static_cast<std::size_t>(property)]; });
            if (!decoded.has_value())
                return plyReadFailure(decoded, "Invalid PLY vertex", std::move(result.losses));
        }
        while (payload_offset < payload.size() && is_space(payload[payload_offset]))
            ++payload_offset;
        if (payload_offset != payload.size()) {
            return {false, "Invalid PLY: ASCII data has trailing records", {}, {}};
        }
    }

    if (dropped_normal_count != 0) {
        LossItem item;
        item.code = loss_code::kVertexNormalsDropped;
        item.severity = LossSeverity::severe;
        item.source_feature = "nonzero training-layout PLY vertex normals";
        item.target_constraint = "the canonical Gaussian model does not store vertex normals";
        item.affected_splats = dropped_normal_count;
        item.remediation = "remove the normals or approve LOSS_VERTEX_NORMALS_DROPPED";
        auto added = result.losses.add(std::move(item));
        if (!added.has_value())
            return plyReadFailure(added, "Invalid PLY loss report", std::move(result.losses));
    }
    if (normalized_quaternion_count != 0) {
        LossItem item;
        item.code = loss_code::kQuaternionNormalized;
        item.severity = LossSeverity::info;
        item.source_feature = "nonunit training-layout PLY quaternions";
        item.target_constraint = "the canonical Gaussian model stores unit quaternions";
        item.affected_splats = normalized_quaternion_count;
        item.remediation = "normalize source quaternions to avoid this representation change";
        auto added = result.losses.add(std::move(item));
        if (!added.has_value())
            return plyReadFailure(added, "Invalid PLY loss report", std::move(result.losses));
    }

    auto sh = ShBuffer::create(sh_degree, vertex_count, std::move(sh_values), context);
    if (!sh.has_value())
        return plyReadFailure(sh, "Invalid PLY spherical harmonics", std::move(result.losses));
    canonical_input.sh = std::move(sh).value();
    auto canonical = SplatData::create(std::move(canonical_input), context);
    if (!canonical.has_value())
        return plyReadFailure(canonical, "PLY data violates canonical scene invariants",
                              std::move(result.losses));
    result.data.emplace(std::move(canonical).value());
    auto completed =
        context.checkpoint({"ply.read", "complete", vertex_count, vertex_count, "splats"});
    if (!completed.has_value())
        return plyReadControlFailure(completed);
    result.set_retained_memory(std::move(canonical_charge));
    return result;
} catch (const std::bad_alloc&) {
    return {false, "PLY buffer exceeds available memory", {}, {}, ErrorCode::resource_limit};
} catch (const std::length_error&) {
    return {false, "PLY buffer exceeds a container size limit", {}, {}, ErrorCode::resource_limit};
}

}  // namespace melkor
