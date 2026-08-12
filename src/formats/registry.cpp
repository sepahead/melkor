#include "melkor/format/registry.hpp"

#include "melkor/format/gltf_reader.hpp"
#include "melkor/format/gltf_writer.hpp"
#include "melkor/format/probe.hpp"
#include "melkor/io/atomic_writer.hpp"
#include "melkor/ply_writer.hpp"
#include "melkor/spz_encoder.hpp"

#include "../io/input_file.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace melkor {
namespace {

namespace fs = std::filesystem;

template <class T> Result<T> fail(ErrorCode error, const char* code, std::string message) {
    return Result<T>::failure(error, Diagnostic(code, Severity::error, std::move(message)));
}

template <class T>
Result<T> fail_for_path(ErrorCode error, const char* code, std::string message,
                        const fs::path& path, const OperationContext& context) {
    Diagnostic diagnostic(code, Severity::error, std::move(message));
    diagnostic.with_path(redact_path(path.u8string(), context.path_policy, context.path_root));
    return Result<T>::failure(error, std::move(diagnostic));
}

std::string lowercase_suffix(const fs::path& path) {
    std::string suffix = path.extension().u8string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a')
                                            : static_cast<char>(value);
    });
    return suffix;
}

std::optional<FormatId> format_from_suffix(const fs::path& path) {
    const std::string suffix = lowercase_suffix(path);
    if (suffix == ".ply")
        return FormatId::ply;
    if (suffix == ".spz")
        return FormatId::spz;
    if (suffix == ".gltf")
        return FormatId::gltf;
    if (suffix == ".glb")
        return FormatId::glb;
    return std::nullopt;
}

Result<FormatId> probe_file(const fs::path& path, const std::optional<FormatId>& requested,
                            const OperationContext& context) {
    std::array<std::uint8_t, 64> prefix{};
    auto opened = io::InputFile::open(path, context);
    if (!opened.has_value())
        return Result<FormatId>::failure(opened.error_code(), opened.diagnostics());
    io::InputFile file = std::move(opened).value();
    const std::size_t read =
        static_cast<std::size_t>(std::min<std::uint64_t>(prefix.size(), file.size()));
    auto prefix_read =
        file.read_exact(0, prefix.data(), read, context, "format.probe", "input_prefix");
    if (!prefix_read.has_value())
        return Result<FormatId>::failure(prefix_read.error_code(), prefix_read.diagnostics());
    const ContainerProbe probe = probe_container(prefix.data(), read);
    const std::optional<FormatId> suffix = format_from_suffix(path);
    FormatId selected = requested.value_or(probe.format);
    if (selected == FormatId::unknown && suffix.has_value())
        selected = *suffix;
    if (selected == FormatId::unknown) {
        return fail_for_path<FormatId>(ErrorCode::unsupported_feature, "MK1702_FORMAT_UNKNOWN",
                                       "the input container could not be identified", path,
                                       context);
    }
    if (probe.format != FormatId::unknown && requested.has_value() && probe.format != *requested) {
        return fail_for_path<FormatId>(
            ErrorCode::invalid_argument, "MK1703_FORMAT_OVERRIDE_CONFLICT",
            "the selected input format conflicts with the file bytes", path, context);
    }
    if (probe.format != FormatId::unknown && !requested.has_value() && suffix.has_value() &&
        probe.format != *suffix) {
        return fail_for_path<FormatId>(ErrorCode::invalid_data, "MK1704_FORMAT_SUFFIX_MISMATCH",
                                       "the input suffix does not match the file bytes", path,
                                       context);
    }
    return Result<FormatId>::success(selected);
}

Result<void> validate_profile(FormatProfileId id, FormatId format, bool write) {
    const FormatProfile& profile = format_profile(id);
    const bool supports_container =
        write ? profile.supports_write_container(format) : profile.supports_read_container(format);
    if (profile.id == FormatProfileId::unknown || !supports_container) {
        Diagnostic diagnostic("MK1734_PROFILE_CONTAINER_MISMATCH", Severity::error,
                              "the selected profile does not apply to this container");
        diagnostic.with_context("profile", std::string(profile.profile_id));
        diagnostic.with_context("format", std::string(to_string(format)));
        return Result<void>::failure(ErrorCode::invalid_argument, std::move(diagnostic));
    }
    return Result<void>::success();
}

Result<AssetReadResult> make_asset(SplatData data, ColorSpace color_space,
                                   std::optional<bool> antialiased, FormatId format,
                                   FormatProfileId profile, AssetSourceMetadata source,
                                   LossReport losses, Budget::Charge retained_memory,
                                   const OperationContext& context) {
    const FormatProfile& descriptor = format_profile(profile);
    if (descriptor.id == FormatProfileId::unknown) {
        return fail<AssetReadResult>(ErrorCode::internal_error, "MK1706_PROFILE_MISSING",
                                     "the reader returned an unknown source profile");
    }
    SplatMetadata metadata;
    metadata.color_space = color_space;
    metadata.sh_degree = static_cast<std::uint8_t>(data.sh().degree());
    metadata.antialiased = antialiased;
    Provenance provenance;
    provenance.source_format = to_string(format);
    provenance.source_profile = std::string(descriptor.profile_id);
    auto primitive = SplatPrimitive::create(std::move(metadata), std::move(data),
                                            std::move(provenance), context);
    if (!primitive.has_value()) {
        return Result<AssetReadResult>::failure(primitive.error_code(), primitive.diagnostics());
    }
    return Result<AssetReadResult>::success(
        AssetReadResult(std::move(primitive.value()), std::move(losses), format, profile,
                        std::move(source), std::move(retained_memory)));
}

const char* ply_encoding_name(PlyReader::Metadata::Encoding encoding) noexcept {
    switch (encoding) {
    case PlyReader::Metadata::Encoding::Ascii:
        return "ascii";
    case PlyReader::Metadata::Encoding::BinaryLittleEndian:
        return "binary_little_endian";
    case PlyReader::Metadata::Encoding::BinaryBigEndian:
        return "binary_big_endian";
    case PlyReader::Metadata::Encoding::Unknown:
        return "unknown";
    }
    return "unknown";
}

Result<AssetReadResult> read_ply(const fs::path& path, const AssetReadOptions& options,
                                 const OperationContext& context) {
    if (options.profile.has_value()) {
        auto valid = validate_profile(*options.profile, FormatId::ply, false);
        if (!valid.has_value())
            return Result<AssetReadResult>::failure(valid.error_code(), valid.diagnostics());
    }
    PlyReadConfig config;
    config.limits = context.budget->limits();
    config.profile = options.profile;
    config.source_frame_id = options.source_frame_id;
    config.source_unit_to_meter = options.source_unit_to_meter;
    config.source_color_space = options.source_color_space;
    PlyReader reader;
    auto result = reader.readFromFile(path, config, context);
    if (!result.success || !result.data.has_value()) {
        if (!result.diagnostics.empty()) {
            return Result<AssetReadResult>::failure(result.error_code(), result.diagnostics);
        }
        return fail<AssetReadResult>(result.error_code(), "MK1707_PLY_READ_FAILED",
                                     result.error_message.empty()
                                         ? "the PLY reader returned no canonical data"
                                         : result.error_message);
    }
    Budget::Charge retained = result.take_retained_memory();
    if (!result.metadata.color_space.has_value()) {
        return fail<AssetReadResult>(ErrorCode::internal_error, "MK1708_COLOR_SPACE_MISSING",
                                     "the PLY reader returned no canonical color space");
    }
    // The checks above prove that both optional values exist.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    SplatData data = std::move(result.data).value();
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const ColorSpace color_space = result.metadata.color_space.value();
    AssetSourceMetadata source;
    source.encoding = ply_encoding_name(result.metadata.encoding);
    source.source_bytes = result.metadata.source_bytes;
    source.declared_splats = result.metadata.declared_vertices;
    return make_asset(std::move(data), color_space, result.metadata.antialiased, FormatId::ply,
                      result.metadata.profile, std::move(source), std::move(result.losses),
                      std::move(retained), context);
}

Result<AssetReadResult> read_spz(const fs::path& path, const AssetReadOptions& options,
                                 const OperationContext& context) {
    const FormatProfileId profile = options.profile.value_or(FormatProfileId::spz_v1_v3);
    auto profile_valid = validate_profile(profile, FormatId::spz, false);
    if (!profile_valid.has_value()) {
        return Result<AssetReadResult>::failure(profile_valid.error_code(),
                                                profile_valid.diagnostics());
    }
    if (!options.source_unit_to_meter.has_value()) {
        return fail<AssetReadResult>(ErrorCode::invalid_argument, "MK1709_SPZ_UNIT_REQUIRED",
                                     "SPZ input requires --source-unit-to-meter");
    }
    if (!options.source_color_space.has_value()) {
        return fail<AssetReadResult>(ErrorCode::invalid_argument, "MK1710_SPZ_COLOR_SPACE_REQUIRED",
                                     "SPZ input requires --source-color-space");
    }
    if (options.source_frame_id.has_value()) {
        return fail<AssetReadResult>(
            ErrorCode::invalid_argument, "MK1732_SPZ_FRAME_FIXED",
            "SPZ defines its source frame and does not accept an override");
    }
    SpzDecodeConfig config;
    config.limits = context.budget->limits();
    config.source_unit_to_meter = options.source_unit_to_meter;
    config.source_color_space = options.source_color_space;
    SpzDecoder decoder;
    auto result = decoder.decodeFromFile(path, config, context);
    if (!result.success || !result.data.has_value()) {
        if (!result.diagnostics.empty()) {
            return Result<AssetReadResult>::failure(result.error_code(), result.diagnostics);
        }
        return fail<AssetReadResult>(result.error_code(), "MK1711_SPZ_READ_FAILED",
                                     result.error_message.empty()
                                         ? "the SPZ reader returned no canonical data"
                                         : result.error_message);
    }
    // The checks above prove that both optional values exist.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    SplatData data = std::move(result.data).value();
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const ColorSpace color_space = options.source_color_space.value();
    Budget::Charge retained = result.take_retained_memory();
    AssetSourceMetadata source;
    source.encoding = "gzip";
    source.source_bytes = result.metadata.source_bytes;
    source.declared_splats = result.metadata.declared_points;
    return make_asset(std::move(data), color_space, result.metadata.antialiased, FormatId::spz,
                      result.metadata.profile, std::move(source), {}, std::move(retained), context);
}

Result<AssetReadResult> read_gltf(const fs::path& path, FormatId format,
                                  const AssetReadOptions& options,
                                  const OperationContext& context) {
    const FormatProfileId profile =
        options.profile.value_or(FormatProfileId::gltf_khr_gaussian_splatting_rc_63770cc);
    auto profile_valid = validate_profile(profile, format, false);
    if (!profile_valid.has_value()) {
        return Result<AssetReadResult>::failure(profile_valid.error_code(),
                                                profile_valid.diagnostics());
    }
    if (options.source_frame_id.has_value() || options.source_unit_to_meter.has_value() ||
        options.source_color_space.has_value()) {
        return fail<AssetReadResult>(
            ErrorCode::invalid_argument, "MK1712_GLTF_SEMANTIC_OVERRIDE",
            "glTF defines its frame, unit, and color space and does not accept source overrides");
    }
    const format::gltf::FileEncoding encoding = format == FormatId::glb
                                                    ? format::gltf::FileEncoding::binary_glb
                                                    : format::gltf::FileEncoding::json;
    auto result = format::gltf::read_file(path, encoding, context);
    if (!result.has_value())
        return Result<AssetReadResult>::failure(result.error_code(), result.diagnostics());
    Budget::Charge retained = result.value().take_retained_memory();
    AssetSourceMetadata source;
    source.encoding = format == FormatId::glb ? "binary" : "json";
    source.source_bytes = result.value().source_bytes;
    source.declared_splats = result.value().data.size();
    return make_asset(std::move(result.value().data), result.value().color_space, std::nullopt,
                      format, profile, std::move(source), std::move(result.value().losses),
                      std::move(retained), context);
}

Result<void> add_antialias_loss(LossReport& losses, const SplatPrimitive& primitive,
                                FormatId output_format,
                                const std::optional<bool>& output_antialiased) {
    const std::optional<bool> source = primitive.metadata().antialiased;
    if (!source.has_value())
        return Result<void>::success();

    const bool target_drops = output_format == FormatId::glb || output_format == FormatId::gltf;
    const bool override_changes = output_antialiased.has_value() && *source != *output_antialiased;
    if (!target_drops && !override_changes)
        return Result<void>::success();

    LossItem item;
    item.code = loss_code::kAntialiasingMetadataDropped;
    item.severity = LossSeverity::severe;
    item.source_feature =
        *source ? "antialiased Gaussian metadata" : "non-antialiased Gaussian metadata";
    item.target_constraint = target_drops
                                 ? "the KHR_gaussian_splatting profile has no antialiasing field"
                                 : "the requested output value replaces the source value";
    item.affected_splats = primitive.data().size();
    item.remediation = target_drops
                           ? "select PLY or SPZ, or approve LOSS_ANTIALIASING_METADATA_DROPPED"
                           : "remove the output override or approve the metadata change";
    return losses.add(std::move(item));
}

Result<FormatProfileId> output_profile(const AssetWriteOptions& options, FormatId format) {
    auto selected = options.profile.has_value() ? options.profile : default_write_profile(format);
    if (!selected.has_value()) {
        return fail<FormatProfileId>(ErrorCode::unsupported_feature,
                                     "MK1713_OUTPUT_PROFILE_REQUIRED",
                                     "the output container has no default write profile");
    }
    auto valid = validate_profile(*selected, format, true);
    if (!valid.has_value())
        return Result<FormatProfileId>::failure(valid.error_code(), valid.diagnostics());
    return Result<FormatProfileId>::success(*selected);
}

Result<FormatId> resolve_output_format(const ConversionRequest& request) {
    if (request.output.empty()) {
        return fail<FormatId>(ErrorCode::invalid_argument, "MK1717_OUTPUT_FORMAT_UNKNOWN",
                              "the output path must not be empty");
    }
    const std::optional<FormatId> suffix_format = format_from_suffix(request.output);
    if (request.write.format.has_value() && suffix_format.has_value() &&
        *request.write.format != *suffix_format) {
        return fail<FormatId>(ErrorCode::invalid_argument, "MK1728_OUTPUT_SUFFIX_CONFLICT",
                              "the selected output format conflicts with the output suffix");
    }
    const std::optional<FormatId> selected =
        request.write.format.has_value() ? request.write.format : suffix_format;
    if (!selected.has_value() || *selected == FormatId::unknown) {
        return fail<FormatId>(ErrorCode::invalid_argument, "MK1717_OUTPUT_FORMAT_UNKNOWN",
                              "select an output format or use a supported suffix");
    }
    if (*selected == FormatId::gltf) {
        return fail<FormatId>(ErrorCode::unsupported_feature, "MK1718_GLTF_JSON_WRITE_UNSUPPORTED",
                              "Melkor writes KHR_gaussian_splatting as GLB only");
    }
    return Result<FormatId>::success(*selected);
}

Result<void> validate_output_options(const AssetWriteOptions& options, FormatId format) {
    if (options.sh_degree < -1 || options.sh_degree > 4) {
        return Result<void>::failure(
            ErrorCode::invalid_argument,
            Diagnostic("MK1729_SH_DEGREE_INVALID", Severity::error,
                       "the maximum SH degree must be -1 or between 0 and 4"));
    }
    if (format == FormatId::spz && (options.ascii || options.target_frame_id.has_value())) {
        return Result<void>::failure(
            ErrorCode::invalid_argument,
            Diagnostic("MK1721_OUTPUT_OPTION_UNSUPPORTED", Severity::error,
                       "ASCII and target-frame options apply only to PLY"));
    }
    if (format == FormatId::glb && (options.ascii || options.target_frame_id.has_value() ||
                                    options.antialiased.has_value() || options.sh_degree != -1)) {
        return Result<void>::failure(ErrorCode::invalid_argument,
                                     Diagnostic("MK1721_OUTPUT_OPTION_UNSUPPORTED", Severity::error,
                                                "the selected output options do not apply to GLB"));
    }
    return Result<void>::success();
}

Result<std::unique_ptr<io::AtomicWriter>> stage_bytes(const fs::path& path,
                                                      const std::uint8_t* data, std::size_t size,
                                                      bool overwrite,
                                                      const OperationContext& context) {
    io::WriteOptions options;
    options.overwrite = overwrite;
    auto output = io::AtomicWriter::create(path, options, context);
    if (!output.has_value())
        return Result<std::unique_ptr<io::AtomicWriter>>::failure(output.error_code(),
                                                                  output.diagnostics());
    auto write = output.value()->write(data, size);
    if (!write.has_value())
        return Result<std::unique_ptr<io::AtomicWriter>>::failure(write.error_code(),
                                                                  write.diagnostics());
    return Result<std::unique_ptr<io::AtomicWriter>>::success(std::move(output).value());
}

}  // namespace

Result<AssetReadResult> read_splat_asset(const fs::path& path, const AssetReadOptions& options,
                                         const OperationContext& context) try {
    if (context.budget == nullptr) {
        return fail<AssetReadResult>(ErrorCode::internal_error, "MK0310_NO_BUDGET",
                                     "the format registry requires a resource budget");
    }
    auto limits_valid = context.budget->limits().validate();
    if (!limits_valid.has_value()) {
        return Result<AssetReadResult>::failure(limits_valid.error_code(),
                                                limits_valid.diagnostics());
    }
    if (options.format.has_value() && !is_known_format(*options.format)) {
        return fail<AssetReadResult>(ErrorCode::invalid_argument, "MK1735_FORMAT_ID_INVALID",
                                     "the selected input format ID is invalid");
    }
    if (options.profile.has_value() &&
        format_profile(*options.profile).id == FormatProfileId::unknown) {
        return fail<AssetReadResult>(ErrorCode::invalid_argument, "MK1736_PROFILE_ID_INVALID",
                                     "the selected input profile ID is invalid");
    }
    auto format = probe_file(path, options.format, context);
    if (!format.has_value())
        return Result<AssetReadResult>::failure(format.error_code(), format.diagnostics());

    auto read_selected = [&]() -> Result<AssetReadResult> {
        switch (format.value()) {
        case FormatId::ply:
            return read_ply(path, options, context);
        case FormatId::spz:
            return read_spz(path, options, context);
        case FormatId::gltf:
        case FormatId::glb:
            return read_gltf(path, format.value(), options, context);
        case FormatId::unknown:
            break;
        }
        return fail<AssetReadResult>(ErrorCode::unsupported_feature, "MK1702_FORMAT_UNKNOWN",
                                     "the input container is unsupported");
    };
    auto result = read_selected();
    if (!result.has_value()) {
        std::vector<Diagnostic> diagnostics = result.diagnostics();
        const std::string display_path =
            redact_path(path.u8string(), context.path_policy, context.path_root);
        for (Diagnostic& diagnostic : diagnostics) {
            if (diagnostic.path.empty())
                diagnostic.with_path(display_path);
        }
        return Result<AssetReadResult>::failure(result.error_code(), std::move(diagnostics));
    }
    return result;
} catch (const std::bad_alloc&) {
    return fail<AssetReadResult>(ErrorCode::resource_limit, "MK1715_REGISTRY_MEMORY",
                                 "the format registry exhausted available memory");
} catch (const std::length_error&) {
    return fail<AssetReadResult>(ErrorCode::resource_limit, "MK1715_REGISTRY_MEMORY",
                                 "the format registry exceeded a container limit");
} catch (const fs::filesystem_error&) {
    return fail_for_path<AssetReadResult>(ErrorCode::io_error, "MK1737_REGISTRY_FILESYSTEM",
                                          "the format registry could not access the input file",
                                          path, context);
} catch (const std::exception&) {
    return fail<AssetReadResult>(ErrorCode::internal_error, "MK1738_REGISTRY_EXCEPTION",
                                 "the format registry caught an unexpected exception");
} catch (...) {
    return fail<AssetReadResult>(ErrorCode::internal_error, "MK1738_REGISTRY_EXCEPTION",
                                 "the format registry caught an unknown exception");
}

Result<ConversionResult> convert_file(const ConversionRequest& request,
                                      const OperationContext& context,
                                      const ConversionCommitGate& before_commit) try {
    if (context.budget == nullptr) {
        return fail<ConversionResult>(ErrorCode::internal_error, "MK0310_NO_BUDGET",
                                      "the conversion planner requires a resource budget");
    }
    auto valid_limits = context.budget->limits().validate();
    if (!valid_limits.has_value()) {
        return Result<ConversionResult>::failure(valid_limits.error_code(),
                                                 valid_limits.diagnostics());
    }
    auto approvals = LossReport{}.check_policy(request.write.approved_loss_codes);
    if (!approvals.has_value()) {
        return Result<ConversionResult>::failure(approvals.error_code(), approvals.diagnostics());
    }
    if (request.write.format.has_value() && !is_known_format(*request.write.format)) {
        return fail<ConversionResult>(ErrorCode::invalid_argument, "MK1735_FORMAT_ID_INVALID",
                                      "the selected output format ID is invalid");
    }
    if (request.write.profile.has_value() &&
        format_profile(*request.write.profile).id == FormatProfileId::unknown) {
        return fail<ConversionResult>(ErrorCode::invalid_argument, "MK1736_PROFILE_ID_INVALID",
                                      "the selected output profile ID is invalid");
    }
    auto control = context.checkpoint({"convert", "plan", 0, 1, "assets"});
    if (!control.has_value())
        return Result<ConversionResult>::failure(control.error_code(), control.diagnostics());

    auto output_format = resolve_output_format(request);
    if (!output_format.has_value()) {
        return Result<ConversionResult>::failure(output_format.error_code(),
                                                 output_format.diagnostics());
    }
    auto valid_options = validate_output_options(request.write, output_format.value());
    if (!valid_options.has_value()) {
        return Result<ConversionResult>::failure(valid_options.error_code(),
                                                 valid_options.diagnostics());
    }
    auto profile = output_profile(request.write, output_format.value());
    if (!profile.has_value()) {
        return Result<ConversionResult>::failure(profile.error_code(), profile.diagnostics());
    }

    auto same = io::is_same_file(request.input, request.output);
    if (!same.has_value())
        return Result<ConversionResult>::failure(same.error_code(), same.diagnostics());
    if (same.value()) {
        return fail<ConversionResult>(ErrorCode::invalid_argument, "MK1716_INPUT_OUTPUT_SAME",
                                      "the input and output must be different files");
    }

    auto read = read_splat_asset(request.input, request.read, context);
    if (!read.has_value())
        return Result<ConversionResult>::failure(read.error_code(), read.diagnostics());

    const SplatPrimitive& primitive = read.value().primitive;
    if (primitive.data().empty()) {
        return fail<ConversionResult>(ErrorCode::invalid_data, "MK1730_EMPTY_ASSET",
                                      "the input contains no Gaussian splats");
    }
    LossReport losses;
    auto appended = losses.append(read.value().losses);
    if (!appended.has_value())
        return Result<ConversionResult>::failure(appended.error_code(), appended.diagnostics());
    auto antialias =
        add_antialias_loss(losses, primitive, output_format.value(), request.write.antialiased);
    if (!antialias.has_value())
        return Result<ConversionResult>::failure(antialias.error_code(), antialias.diagnostics());
    auto policy = losses.check_policy(request.write.approved_loss_codes);
    if (!policy.has_value())
        return Result<ConversionResult>::failure(policy.error_code(), policy.diagnostics());

    const std::optional<bool> output_antialiased = request.write.antialiased.has_value()
                                                       ? request.write.antialiased
                                                       : primitive.metadata().antialiased;
    std::unique_ptr<io::AtomicWriter> staged_output;

    switch (output_format.value()) {
    case FormatId::ply: {
        PlyWriteConfig config;
        config.format = request.write.ascii ? PlyFormat::Ascii : PlyFormat::Binary;
        config.profile = profile.value();
        config.color_space = primitive.metadata().color_space;
        config.antialiased = output_antialiased;
        config.target_frame_id = request.write.target_frame_id;
        config.limits = context.budget->limits();
        config.overwrite = request.write.overwrite;
        config.include_sh_rest = true;
        config.sh_degree = request.write.sh_degree;
        config.approved_loss_codes = request.write.approved_loss_codes;

        io::WriteOptions output_options;
        output_options.overwrite = request.write.overwrite;
        auto output = io::AtomicWriter::create(request.output, output_options, context);
        if (!output.has_value()) {
            return Result<ConversionResult>::failure(output.error_code(), output.diagnostics());
        }

        PlyWriter writer;
        PlyWriteResult written;
        {
            io::AtomicOutputStream stream(*output.value());
            written = writer.writeToStream(stream, primitive.data(), config, context);
            stream.flush();
            if (stream.failed()) {
                if (!stream.diagnostics().empty()) {
                    return Result<ConversionResult>::failure(stream.error_code(),
                                                             stream.diagnostics());
                }
                return fail<ConversionResult>(ErrorCode::io_error, "MK1731_PLY_STREAM_FAILED",
                                              "the complete PLY output could not be written");
            }
        }
        if (!written.success) {
            if (!written.diagnostics.empty()) {
                return Result<ConversionResult>::failure(written.error_code(), written.diagnostics);
            }
            return fail<ConversionResult>(written.error_code(), "MK1719_PLY_WRITE_FAILED",
                                          written.error_message.empty() ? "the PLY writer failed"
                                                                        : written.error_message);
        }
        appended = losses.append(written.losses);
        if (!appended.has_value()) {
            return Result<ConversionResult>::failure(appended.error_code(), appended.diagnostics());
        }
        auto final_policy = losses.check_policy(request.write.approved_loss_codes);
        if (!final_policy.has_value()) {
            return Result<ConversionResult>::failure(final_policy.error_code(),
                                                     final_policy.diagnostics());
        }
        staged_output = std::move(output).value();
        break;
    }
    case FormatId::spz: {
        if (!output_antialiased.has_value()) {
            return fail<ConversionResult>(
                ErrorCode::invalid_argument, "MK1720_SPZ_ANTIALIASING_REQUIRED",
                "SPZ output requires --output-antialiased when the source value is unknown");
        }
        SpzEncodeConfig config;
        config.limits = context.budget->limits();
        config.sh_degree = request.write.sh_degree;
        config.antialiased = output_antialiased;
        config.color_space = primitive.metadata().color_space;
        config.approved_loss_codes = request.write.approved_loss_codes;

        Budget::Charge output_memory;
        std::vector<std::uint8_t> buffer;
        SpzEncoder encoder;
        auto written = encoder.encodeToBuffer(buffer, primitive.data(), config, context);
        if (!written.success) {
            if (!written.diagnostics.empty()) {
                return Result<ConversionResult>::failure(written.error_code(), written.diagnostics);
            }
            return fail<ConversionResult>(written.error_code(), "MK1722_SPZ_WRITE_FAILED",
                                          written.error_message.empty() ? "the SPZ writer failed"
                                                                        : written.error_message);
        }
        output_memory = written.take_retained_memory();
        appended = losses.append(written.losses);
        if (!appended.has_value()) {
            return Result<ConversionResult>::failure(appended.error_code(), appended.diagnostics());
        }
        auto final_policy = losses.check_policy(request.write.approved_loss_codes);
        if (!final_policy.has_value()) {
            return Result<ConversionResult>::failure(final_policy.error_code(),
                                                     final_policy.diagnostics());
        }
        auto staged = stage_bytes(request.output, buffer.data(), buffer.size(),
                                  request.write.overwrite, context);
        if (!staged.has_value()) {
            return Result<ConversionResult>::failure(staged.error_code(), staged.diagnostics());
        }
        staged_output = std::move(staged).value();
        break;
    }
    case FormatId::glb: {
        Budget::Charge output_memory;
        auto written =
            format::gltf::write_glb(primitive.data(), primitive.metadata().color_space, context);
        if (!written.has_value()) {
            return Result<ConversionResult>::failure(written.error_code(), written.diagnostics());
        }
        output_memory = written.value().take_retained_memory();
        appended = losses.append(written.value().losses);
        if (!appended.has_value()) {
            return Result<ConversionResult>::failure(appended.error_code(), appended.diagnostics());
        }
        auto final_policy = losses.check_policy(request.write.approved_loss_codes);
        if (!final_policy.has_value()) {
            return Result<ConversionResult>::failure(final_policy.error_code(),
                                                     final_policy.diagnostics());
        }
        auto staged = stage_bytes(request.output, written.value().bytes.data(),
                                  written.value().bytes.size(), request.write.overwrite, context);
        if (!staged.has_value()) {
            return Result<ConversionResult>::failure(staged.error_code(), staged.diagnostics());
        }
        staged_output = std::move(staged).value();
        break;
    }
    case FormatId::gltf:
    case FormatId::unknown:
        return fail<ConversionResult>(ErrorCode::unsupported_feature,
                                      "MK1723_OUTPUT_FORMAT_UNSUPPORTED",
                                      "the selected output format is unsupported");
    }

    ConversionResult result;
    result.input_format = read.value().format;
    result.output_format = output_format.value();
    result.input_profile = read.value().profile;
    result.output_profile = profile.value();
    result.splat_count = primitive.data().size();
    if (!staged_output) {
        return fail<ConversionResult>(ErrorCode::internal_error, "MK1739_OUTPUT_NOT_STAGED",
                                      "the conversion produced no staged output");
    }
    result.bytes_written = staged_output->bytes_written();
    result.losses = std::move(losses);

    if (before_commit) {
        auto gate_control = context.checkpoint({"convert", "report", 0, 1, "assets"});
        if (!gate_control.has_value()) {
            return Result<ConversionResult>::failure(gate_control.error_code(),
                                                     gate_control.diagnostics());
        }
        auto gate = before_commit(result);
        if (!gate.has_value())
            return Result<ConversionResult>::failure(gate.error_code(), gate.diagnostics());
    }

    auto commit_control = context.checkpoint({"convert", "commit", 0, 1, "assets"});
    if (!commit_control.has_value()) {
        return Result<ConversionResult>::failure(commit_control.error_code(),
                                                 commit_control.diagnostics());
    }
    auto committed = staged_output->commit();
    if (!committed.has_value()) {
        return Result<ConversionResult>::failure(committed.error_code(), committed.diagnostics());
    }

    context.report({"convert", "complete", 1, 1, "assets"});
    return Result<ConversionResult>::success(std::move(result));
} catch (const std::bad_alloc&) {
    return fail<ConversionResult>(ErrorCode::resource_limit, "MK1715_REGISTRY_MEMORY",
                                  "the conversion planner exhausted available memory");
} catch (const std::length_error&) {
    return fail<ConversionResult>(ErrorCode::resource_limit, "MK1715_REGISTRY_MEMORY",
                                  "the conversion planner exceeded a container limit");
} catch (const fs::filesystem_error&) {
    return fail<ConversionResult>(ErrorCode::io_error, "MK1737_REGISTRY_FILESYSTEM",
                                  "the conversion planner caught a filesystem error");
} catch (const std::exception&) {
    return fail<ConversionResult>(ErrorCode::internal_error, "MK1738_REGISTRY_EXCEPTION",
                                  "the conversion planner caught an unexpected exception");
} catch (...) {
    return fail<ConversionResult>(ErrorCode::internal_error, "MK1738_REGISTRY_EXCEPTION",
                                  "the conversion planner caught an unknown exception");
}

}  // namespace melkor
