#include "inspect_command.hpp"

#include "melkor/cloud_inspector.hpp"
#include "melkor/format/gltf_reader.hpp"
#include "melkor/glb_reader.hpp"
#include "melkor/ply_writer.hpp"
#include "melkor/spz_encoder.hpp"
#include "safe_text.hpp"

#include <fstream>
#include <vector>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace melkor::cli {
namespace {

namespace fs = std::filesystem;

struct FieldSummary {
    std::string position = "unknown";
    std::string color = "unknown";
    std::string opacity = "unknown";
    std::string scale = "unknown";
    std::string rotation = "unknown";
    std::string sh_rest = "unknown";
};

struct InspectDocument {
    std::string path;
    std::string format = "unknown";
    std::string kind = "unknown";
    std::string encoding = "unknown";
    uintmax_t bytes = 0;
    bool has_bytes = false;
    size_t declared_splats = 0;
    bool has_declared_splats = false;
    bool antialiased = false;
    bool has_antialiased = false;
    bool has_cloud = false;
    FieldSummary fields;
    CloudInspection inspection;
};

std::string extensionOf(const std::string& path) {
    std::string extension = fs::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    return extension;
}

void addSourceError(InspectDocument& document, std::string code, std::string message) {
    addInspectionIssue(document.inspection, InspectionSeverity::Error, std::move(code),
                       std::move(message));
}

const char* severityName(InspectionSeverity severity) {
    return severity == InspectionSeverity::Error ? "error" : "warning";
}

const char* plyEncodingName(PlyReader::Metadata::Encoding encoding) {
    switch (encoding) {
        case PlyReader::Metadata::Encoding::Ascii: return "ascii";
        case PlyReader::Metadata::Encoding::BinaryLittleEndian: return "binary_little_endian";
        case PlyReader::Metadata::Encoding::BinaryBigEndian: return "binary_big_endian";
        case PlyReader::Metadata::Encoding::Unknown:
        default: return "unknown";
    }
}

void addPlyFieldWarnings(InspectDocument& document, const PlyReader::Metadata& metadata) {
    if (metadata.declared_vertices == 0) return;
    if (!metadata.has_sh_dc && metadata.has_rgb) {
        addInspectionIssue(document.inspection, InspectionSeverity::Warning,
                           "rgb_color_converted",
                           "RGB byte colors were converted to spherical-harmonics DC values.");
    } else if (!metadata.has_sh_dc && !metadata.has_rgb) {
        addInspectionIssue(document.inspection, InspectionSeverity::Warning,
                           "defaulted_color", "Color was absent and defaulted by the reader.");
    }
    if (!metadata.has_opacity) {
        addInspectionIssue(document.inspection, InspectionSeverity::Warning,
                           "defaulted_opacity", "Opacity was absent and defaulted by the reader.");
    }
    if (!metadata.has_scale) {
        addInspectionIssue(document.inspection, InspectionSeverity::Warning,
                           "defaulted_scale", "Scale was absent and defaulted by the reader.");
    }
    if (!metadata.has_rotation) {
        addInspectionIssue(document.inspection, InspectionSeverity::Warning,
                           "defaulted_rotation", "Rotation was absent and defaulted by the reader.");
    }
}

InspectDocument inspectPath(const std::string& path) {
    InspectDocument document;
    document.path = path;
    const std::string extension = extensionOf(path);
    if (!extension.empty()) document.format = extension;

    std::error_code error;
    const auto status = fs::status(path, error);
    if (error || !fs::exists(status)) {
        addSourceError(document, "input_not_found", "The input file does not exist.");
        return document;
    }
    if (!fs::is_regular_file(status)) {
        addSourceError(document, "input_not_regular_file", "The input path is not a regular file.");
        return document;
    }
    document.bytes = fs::file_size(path, error);
    if (error) {
        addSourceError(document, "file_size_unavailable", "The input file size could not be read.");
        return document;
    }
    document.has_bytes = true;

    GaussianCloud cloud;
    if (document.format == "ply") {
        PlyReader reader;
        auto result = reader.readFromFile(path);
        if (!result.success) {
            addSourceError(document, "read_error", result.error_message);
            return document;
        }
        cloud = std::move(result.cloud);
        document.kind = "gaussian_cloud";
        document.encoding = plyEncodingName(result.metadata.encoding);
        document.declared_splats = result.metadata.declared_vertices;
        document.has_declared_splats = true;
        document.fields.position = result.metadata.has_position ? "explicit" : "missing";
        document.fields.color = result.metadata.has_sh_dc
            ? "explicit_sh_dc" : result.metadata.has_rgb ? "converted_rgb" : "defaulted";
        document.fields.opacity = result.metadata.has_opacity ? "explicit" : "defaulted";
        document.fields.scale = result.metadata.has_scale ? "explicit" : "defaulted";
        document.fields.rotation = result.metadata.has_rotation ? "explicit" : "defaulted";
        document.fields.sh_rest = result.metadata.has_sh_rest ? "explicit" : "absent";
        document.has_cloud = true;
        document.inspection = inspectLegacyCloud(cloud);
        if (result.metadata.declared_vertices != cloud.size()) {
            addInspectionIssue(document.inspection, InspectionSeverity::Error,
                               "decoded_count_mismatch",
                               "Decoded PLY count differs from the declared count.");
        }
        addPlyFieldWarnings(document, result.metadata);
    } else if (document.format == "spz") {
#ifdef MELKOR_HAS_SPZ
        SpzDecoder decoder;
        auto result = decoder.decodeFromFile(path);
        if (!result.success) {
            const bool unsupported_version =
                result.error_message.rfind("Unsupported SPZ version", 0) == 0;
            addSourceError(document,
                           unsupported_version ? "unsupported_spz_version" : "read_error",
                           result.error_message);
            return document;
        }
        cloud = std::move(result.cloud);
        document.kind = "gaussian_cloud";
        document.encoding = "spz";
        document.declared_splats = result.metadata.declared_points;
        document.has_declared_splats = true;
        document.antialiased = result.metadata.antialiased;
        document.has_antialiased = true;
        document.fields = {"explicit", "explicit_sh_dc", "explicit", "explicit", "explicit",
                           result.metadata.sh_degree > 0 ? "explicit" : "absent"};
        document.has_cloud = true;
        document.inspection = inspectLegacyCloud(cloud);
        if (result.metadata.decoded_points != result.metadata.declared_points) {
            addInspectionIssue(document.inspection, InspectionSeverity::Error,
                               "decoded_count_mismatch",
                               "Decoded SPZ count differs from the declared count.");
        }
#else
        addSourceError(document, "spz_unavailable",
                       "SPZ support is not compiled into this binary.");
        return document;
#endif
    } else if (document.format == "glb" || document.format == "gltf") {
        // Prefer the real KHR_gaussian_splatting reader: a GLB carrying splats is read as actual
        // Gaussian data, not as mesh vertices. Only when the asset has no splat primitive do we
        // fall back to the legacy vertices-to-splats path.
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        auto scene = melkor::format::gltf::read_glb(bytes.data(), bytes.size());
        const bool no_splat_primitive =
            !scene.has_value() && !scene.diagnostics().empty() &&
            scene.diagnostics()[0].code == "MK2161_GLTF_NO_SPLATS";

        if (scene.has_value()) {
            // KHR_gaussian_splatting is already canonical. Inspect it directly: converting linear
            // scale/opacity back into the legacy log/logit model merely to compute bounds created
            // exactly the double-activation bug class A1 is removing.
            const auto& sd = scene.value().data;
            document.kind = "gaussian_cloud";
            document.encoding = "khr_gaussian_splatting";
            document.declared_splats = sd.size();
            document.has_declared_splats = true;
            document.fields = {"explicit", "explicit_sh_dc", "explicit", "explicit", "explicit",
                               scene.value().sh_degree > 0 ? "explicit" : "absent"};
            document.has_cloud = true;
            document.inspection = inspectCloud(sd);
            // Surface the conversion loss report so an inspector sees what a conversion would lose.
            for (const auto& item : scene.value().losses.items()) {
                addInspectionIssue(document.inspection, InspectionSeverity::Warning, item.code,
                                   item.source_feature.empty() ? item.target_constraint
                                                               : item.source_feature);
            }
        } else if (no_splat_primitive) {
            // Not a splat asset: fall back to the legacy vertices-as-splats reading.
            GlbReader reader;
            GlbConversionConfig config;
            config.convert_coordinate_system = false;
            auto result = reader.loadFromFile(path, config);
            if (!result.success) {
                addSourceError(document, "read_error", result.error_message);
                return document;
            }
            cloud = std::move(result.cloud);
            document.kind = "mesh_vertices";
            document.encoding = document.format == "glb" ? "binary" : "json";
            document.declared_splats = result.total_vertices;
            document.has_declared_splats = true;
            document.fields = {"explicit", "converted_or_defaulted", "generated", "generated",
                               "generated", "absent"};
            document.has_cloud = true;
            document.inspection = inspectLegacyCloud(cloud);
        } else {
            addSourceError(document, "read_error",
                           scene.diagnostics().empty() ? "failed to read GLB"
                                                       : scene.diagnostics()[0].message);
            return document;
        }
    } else {
        addSourceError(document, "unsupported_format",
                       "Supported inspect inputs are PLY, SPZ, GLB, and glTF.");
    }
    return document;
}

void writeJsonString(std::ostream& stream, const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    stream.put('"');
    for (size_t index = 0; index < value.size();) {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        if (ch >= 0x80) {
            const size_t sequence_length = text::utf8SequenceLength(value, index);
            if (sequence_length > 0) {
                stream.write(value.data() + index, static_cast<std::streamsize>(sequence_length));
                index += sequence_length;
                continue;
            }
            // JSON text must be valid UTF-8. Preserve malformed parser/path
            // bytes deterministically as U+00xx escapes instead of emitting
            // an invalid byte sequence that downstream JSON parsers reject.
            stream << "\\u00" << hex[ch >> 4] << hex[ch & 0x0f];
            ++index;
            continue;
        }
        switch (ch) {
            case '"': stream << "\\\""; break;
            case '\\': stream << "\\\\"; break;
            case '\b': stream << "\\b"; break;
            case '\f': stream << "\\f"; break;
            case '\n': stream << "\\n"; break;
            case '\r': stream << "\\r"; break;
            case '\t': stream << "\\t"; break;
            default:
                if (ch < 0x20) {
                    stream << "\\u00" << hex[ch >> 4] << hex[ch & 0x0f];
                } else {
                    stream.put(static_cast<char>(ch));
                }
        }
        ++index;
    }
    stream.put('"');
}

void writeFloat(std::ostream& stream, float value) {
    if (value == 0.0f) {
        stream << '0';
        return;
    }
    std::ostringstream formatted;
    formatted.imbue(std::locale::classic());
    formatted << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    stream << formatted.str();
}

void writeNullableSize(std::ostream& stream, bool available, uintmax_t value) {
    if (available) stream << value;
    else stream << "null";
}

void writeJson(const InspectDocument& document, std::ostream& stream) {
    stream << "{\"schema\":\"melkor.inspect.v1\",\"valid\":"
           << (document.inspection.valid ? "true" : "false") << ",\"source\":{";
    stream << "\"path\":"; writeJsonString(stream, document.path);
    stream << ",\"format\":"; writeJsonString(stream, document.format);
    stream << ",\"kind\":"; writeJsonString(stream, document.kind);
    stream << ",\"bytes\":"; writeNullableSize(stream, document.has_bytes, document.bytes);
    stream << "},\"cloud\":";
    if (!document.has_cloud) {
        stream << "null";
    } else {
        stream << "{\"splats\":" << document.inspection.splat_count
               << ",\"sh_degree\":" << document.inspection.sh_degree << ",\"bounds\":";
        if (!document.inspection.bounds.available) {
            stream << "null";
        } else {
            stream << "{\"min\":[";
            for (int i = 0; i < 3; ++i) {
                if (i) stream << ',';
                writeFloat(stream, document.inspection.bounds.min[i]);
            }
            stream << "],\"max\":[";
            for (int i = 0; i < 3; ++i) {
                if (i) stream << ',';
                writeFloat(stream, document.inspection.bounds.max[i]);
            }
            stream << "]},\"fields\":{";
        }
        if (!document.inspection.bounds.available) stream << ",\"fields\":{";
        stream << "\"position\":"; writeJsonString(stream, document.fields.position);
        stream << ",\"color\":"; writeJsonString(stream, document.fields.color);
        stream << ",\"opacity\":"; writeJsonString(stream, document.fields.opacity);
        stream << ",\"scale\":"; writeJsonString(stream, document.fields.scale);
        stream << ",\"rotation\":"; writeJsonString(stream, document.fields.rotation);
        stream << ",\"sh_rest\":"; writeJsonString(stream, document.fields.sh_rest);
        stream << "}}";
    }
    stream << ",\"container\":{\"encoding\":";
    writeJsonString(stream, document.encoding);
    stream << ",\"declared_splats\":";
    writeNullableSize(stream, document.has_declared_splats, document.declared_splats);
    stream << ",\"antialiased\":";
    if (document.has_antialiased) stream << (document.antialiased ? "true" : "false");
    else stream << "null";
    stream << "},\"validation\":{\"errors\":" << document.inspection.error_count
           << ",\"warnings\":" << document.inspection.warning_count << ",\"issues\":[";
    for (size_t index = 0; index < document.inspection.issues.size(); ++index) {
        const auto& issue = document.inspection.issues[index];
        if (index) stream << ',';
        stream << "{\"severity\":"; writeJsonString(stream, severityName(issue.severity));
        stream << ",\"code\":"; writeJsonString(stream, issue.code);
        stream << ",\"message\":"; writeJsonString(stream, issue.message);
        stream << ",\"count\":" << issue.count << ",\"first_index\":";
        if (issue.has_index) stream << issue.first_index;
        else stream << "null";
        stream << '}';
    }
    stream << "]}}\n";
}

void writeHuman(const InspectDocument& document, std::ostream& stream) {
    stream << "Inspection: ";
    text::writeDisplayString(stream, document.path);
    stream << '\n';
    stream << "  Format: ";
    text::writeDisplayString(stream, document.format);
    stream << " (";
    text::writeDisplayString(stream, document.encoding);
    stream << ")\n";
    if (document.has_bytes) stream << "  Bytes: " << document.bytes << '\n';
    stream << "  Valid: " << (document.inspection.valid ? "yes" : "no") << '\n';
    if (document.has_cloud) {
        stream << "  Kind: ";
        text::writeDisplayString(stream, document.kind);
        stream << '\n';
        stream << "  Splats: " << document.inspection.splat_count << '\n';
        stream << "  SH degree: " << document.inspection.sh_degree << '\n';
        if (document.inspection.bounds.available) {
            stream << "  Bounds: [";
            for (int i = 0; i < 3; ++i) {
                if (i) stream << ", ";
                writeFloat(stream, document.inspection.bounds.min[i]);
            }
            stream << "] to [";
            for (int i = 0; i < 3; ++i) {
                if (i) stream << ", ";
                writeFloat(stream, document.inspection.bounds.max[i]);
            }
            stream << "]\n";
        }
    }
    for (const auto& issue : document.inspection.issues) {
        stream << "  " << (issue.severity == InspectionSeverity::Error ? "ERROR" : "WARNING")
               << " [";
        text::writeDisplayString(stream, issue.code);
        stream << "] ";
        text::writeDisplayString(stream, issue.message);
        if (issue.count > 1) stream << " (" << issue.count << " occurrences)";
        if (issue.has_index) stream << " First index: " << issue.first_index << '.';
        stream << '\n';
    }
}

void printUsage(const char* program, std::ostream& stream) {
    stream << "Usage: ";
    text::writeDisplayString(stream, program);
    stream << " inspect INPUT [--json] [--strict]\n"
           << "\nValidate and inspect PLY, SPZ v1-v3, GLB, or glTF without writing output.\n"
           << "  --json    emit one deterministic melkor.inspect.v1 JSON document\n"
           << "  --strict  return exit 1 when validation warnings are present\n";
}

}  // namespace

int runInspectCommand(int argc, char* argv[], const char* program) {
    bool json = false;
    bool strict = false;
    bool positional_only = false;
    std::string input;

    for (int index = 0; index < argc; ++index) {
        const std::string argument = argv[index];
        if (!positional_only && (argument == "-h" || argument == "--help")) {
            printUsage(program, std::cout);
            return 0;
        }
        if (!positional_only && argument == "--") {
            positional_only = true;
        } else if (!positional_only && argument == "--json") {
            json = true;
        } else if (!positional_only && argument == "--strict") {
            strict = true;
        } else if (!positional_only && !argument.empty() && argument.front() == '-') {
            std::cerr << "Error: Unknown inspect option: ";
            text::writeDisplayString(std::cerr, argument);
            std::cerr << '\n';
            printUsage(program, std::cerr);
            return 2;
        } else if (input.empty()) {
            input = argument;
        } else {
            std::cerr << "Error: inspect accepts exactly one input file.\n";
            printUsage(program, std::cerr);
            return 2;
        }
    }

    if (input.empty()) {
        std::cerr << "Error: inspect requires an input file.\n";
        printUsage(program, std::cerr);
        return 2;
    }

    InspectDocument document;
    try {
        document = inspectPath(input);
    } catch (const std::bad_alloc&) {
        document.path = input;
        document.format = extensionOf(input);
        addSourceError(document, "resource_limit",
                       "Inspection exceeded available memory for this input.");
    } catch (const std::length_error&) {
        document.path = input;
        document.format = extensionOf(input);
        addSourceError(document, "resource_limit",
                       "Inspection exceeded a container or allocation size limit.");
    } catch (const std::exception& error) {
        document.path = input;
        document.format = extensionOf(input);
        addSourceError(document, "read_error",
                       std::string("Inspection failed safely: ") + error.what());
    }
    if (json) writeJson(document, std::cout);
    else writeHuman(document, std::cout);

    if (!document.inspection.valid) return 1;
    if (strict && document.inspection.warning_count > 0) return 1;
    return 0;
}

}  // namespace melkor::cli
