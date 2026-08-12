#include "inspect_command.hpp"

#include "cli_diagnostics.hpp"
#include "cli_interrupt.hpp"
#include "melkor/cloud_inspector.hpp"
#include "melkor/color_space.hpp"
#include "melkor/format/profile.hpp"
#include "melkor/format/registry.hpp"
#include "melkor/limits.hpp"
#include <nlohmann/json.hpp>
#include "safe_text.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace melkor::cli {
namespace {

namespace fs = std::filesystem;

struct FieldSummary {
    std::string position = "explicit";
    std::string color = "explicit_sh";
    std::string opacity = "explicit";
    std::string scale = "explicit";
    std::string rotation = "explicit";
    std::string sh_rest = "absent";
};

struct InspectDocument {
    std::string path = "<input>";
    std::string format = "unknown";
    std::string profile;
    std::string encoding = "unknown";
    std::optional<std::uintmax_t> bytes;
    std::optional<std::uint64_t> declared_splats;
    std::optional<bool> antialiased;
    std::string color_space;
    FieldSummary fields;
    CloudInspection inspection;
    LossReport losses;
    bool has_cloud = false;
    ErrorCode error_code = ErrorCode::ok;
};

std::string safe_string(const std::string& value) {
    std::ostringstream stream;
    text::writeDisplayString(stream, value);
    return stream.str();
}

std::string report_path(const fs::path& path) {
    const std::string filename = path.filename().u8string();
    return filename.empty() ? "<input>" : filename;
}

std::string suffix_format(const fs::path& path) {
    std::string suffix = path.extension().u8string();
    if (!suffix.empty() && suffix.front() == '.')
        suffix.erase(suffix.begin());
    for (char& value : suffix) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte >= 'A' && byte <= 'Z')
            value = static_cast<char>(byte - 'A' + 'a');
    }
    return suffix == "ply" || suffix == "spz" || suffix == "gltf" || suffix == "glb" ? suffix
                                                                                     : "unknown";
}

std::optional<FormatId> parse_format(const std::string& value) {
    if (value == "ply")
        return FormatId::ply;
    if (value == "spz")
        return FormatId::spz;
    if (value == "gltf")
        return FormatId::gltf;
    if (value == "glb")
        return FormatId::glb;
    return std::nullopt;
}

bool parse_positive_double(const std::string& value, double& output) {
    if (value.empty())
        return false;
    double parsed = 0.0;
    if (!text::parseClassicDouble(value, parsed) || !std::isfinite(parsed) || parsed <= 0.0) {
        return false;
    }
    output = parsed;
    return true;
}

void add_failure(InspectDocument& document, ErrorCode code, const std::string& diagnostic_code,
                 const std::string& message) {
    if (document.error_code == ErrorCode::ok)
        document.error_code = code;
    addInspectionIssue(document.inspection, InspectionSeverity::Error, diagnostic_code, message);
}

void add_diagnostics(InspectDocument& document, ErrorCode code,
                     const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) {
        add_failure(document, code, "MK1901_INSPECT_FAILED", "The asset inspection failed.");
        return;
    }
    if (document.error_code == ErrorCode::ok)
        document.error_code = code;
    for (const Diagnostic& diagnostic : diagnostics) {
        const InspectionSeverity severity = diagnostic.severity == Severity::warning
                                                ? InspectionSeverity::Warning
                                                : InspectionSeverity::Error;
        addInspectionIssue(document.inspection, severity, diagnostic.code, diagnostic.message);
        InspectionIssue& issue = document.inspection.issues.back();
        issue.path = diagnostic.path;
        issue.byte_offset = diagnostic.byte_offset;
        issue.context = diagnostic.context;
    }
}

InspectDocument inspect_path(const fs::path& path, const AssetReadOptions& options,
                             const OperationContext& context) {
    InspectDocument document;
    document.path = report_path(path);
    document.format =
        options.format.has_value() ? std::string(to_string(*options.format)) : suffix_format(path);

    auto asset = read_splat_asset(path, options, context);
    if (!asset.has_value()) {
        add_diagnostics(document, asset.error_code(), asset.diagnostics());
        return document;
    }

    AssetReadResult value = std::move(asset).value();
    document.format = std::string(to_string(value.format));
    document.profile = std::string(format_profile(value.profile).profile_id);
    document.encoding = value.source.encoding;
    document.bytes = value.source.source_bytes;
    document.declared_splats = value.source.declared_splats;
    document.antialiased = value.primitive.metadata().antialiased;
    document.color_space = std::string(to_string(value.primitive.metadata().color_space));
    document.fields.sh_rest = value.primitive.metadata().sh_degree > 0 ? "explicit" : "absent";
    document.losses = std::move(value.losses);
    auto inspection = inspectCloud(value.primitive.data(), context);
    if (!inspection.has_value()) {
        add_diagnostics(document, inspection.error_code(), inspection.diagnostics());
        return document;
    }
    document.inspection = std::move(inspection).value();
    document.has_cloud = true;
    if (document.inspection.error_count != 0)
        document.error_code = ErrorCode::invalid_data;
    return document;
}

nlohmann::ordered_json loss_json(const LossItem& item) {
    return {
        {"code", item.code},
        {"severity", to_string(item.severity)},
        {"source_feature", safe_string(item.source_feature)},
        {"target_constraint", safe_string(item.target_constraint)},
        {"affected_splats", item.affected_splats},
        {"remediation", safe_string(item.remediation)},
    };
}

nlohmann::ordered_json scalar_json(const JsonScalar& value) {
    return std::visit(
        [](const auto& item) -> nlohmann::ordered_json {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return nullptr;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return safe_string(item);
            } else if constexpr (std::is_same_v<T, double>) {
                if (!std::isfinite(item)) {
                    if (std::isnan(item))
                        return "nan";
                    return item < 0.0 ? "-infinity" : "infinity";
                }
                return item;
            } else {
                return item;
            }
        },
        value);
}

nlohmann::ordered_json context_json(const InspectionIssue& issue) {
    nlohmann::ordered_json context = nlohmann::ordered_json::object();
    for (const auto& [key, value] : issue.context)
        context[safe_string(key)] = scalar_json(value);
    return context;
}

nlohmann::ordered_json report_json(const InspectDocument& document) {
    nlohmann::ordered_json report;
    report["schema"] = "melkor.inspect.v1";
    report["valid"] = document.inspection.valid;
    report["source"] = {
        {"path", safe_string(document.path)},
        {"format", document.format},
        {"profile", document.profile.empty() ? nlohmann::ordered_json(nullptr)
                                             : nlohmann::ordered_json(document.profile)},
        {"bytes", document.bytes.has_value() ? nlohmann::ordered_json(*document.bytes)
                                             : nlohmann::ordered_json(nullptr)},
    };
    if (!document.has_cloud) {
        report["cloud"] = nullptr;
    } else {
        nlohmann::ordered_json bounds = nullptr;
        if (document.inspection.bounds.available) {
            bounds = {
                {"min",
                 {document.inspection.bounds.min[0], document.inspection.bounds.min[1],
                  document.inspection.bounds.min[2]}},
                {"max",
                 {document.inspection.bounds.max[0], document.inspection.bounds.max[1],
                  document.inspection.bounds.max[2]}},
            };
        }
        report["cloud"] = {
            {"splats", document.inspection.splat_count},
            {"sh_degree", document.inspection.sh_degree},
            {"color_space", document.color_space},
            {"coordinate_frame", "gltf-luf"},
            {"unit_to_meter", 1.0},
            {"bounds", std::move(bounds)},
            {"fields",
             {
                 {"position", document.fields.position},
                 {"color", document.fields.color},
                 {"opacity", document.fields.opacity},
                 {"scale", document.fields.scale},
                 {"rotation", document.fields.rotation},
                 {"sh_rest", document.fields.sh_rest},
             }},
        };
    }
    report["container"] = {
        {"encoding", document.encoding},
        {"declared_splats", document.declared_splats.has_value()
                                ? nlohmann::ordered_json(*document.declared_splats)
                                : nlohmann::ordered_json(nullptr)},
        {"antialiased", document.antialiased.has_value()
                            ? nlohmann::ordered_json(*document.antialiased)
                            : nlohmann::ordered_json(nullptr)},
    };
    report["losses"] = nlohmann::ordered_json::array();
    for (const LossItem& item : document.losses.items())
        report["losses"].push_back(loss_json(item));
    report["validation"] = {
        {"error_code", document.error_code == ErrorCode::ok
                           ? nlohmann::ordered_json(nullptr)
                           : nlohmann::ordered_json(to_string(document.error_code))},
        {"errors", document.inspection.error_count},
        {"warnings", document.inspection.warning_count},
        {"issues", nlohmann::ordered_json::array()},
    };
    for (const InspectionIssue& issue : document.inspection.issues) {
        report["validation"]["issues"].push_back({
            {"severity", issue.severity == InspectionSeverity::Error ? "error" : "warning"},
            {"code", issue.code},
            {"message", safe_string(issue.message)},
            {"count", issue.count},
            {"first_index", issue.has_index ? nlohmann::ordered_json(issue.first_index)
                                            : nlohmann::ordered_json(nullptr)},
            {"path", issue.path.empty() ? nlohmann::ordered_json(nullptr)
                                        : nlohmann::ordered_json(safe_string(issue.path))},
            {"byte_offset", issue.byte_offset.has_value()
                                ? nlohmann::ordered_json(*issue.byte_offset)
                                : nlohmann::ordered_json(nullptr)},
            {"context", context_json(issue)},
        });
    }
    return report;
}

void write_human(const InspectDocument& document, std::ostream& stream) {
    stream << "Inspection: ";
    text::writeDisplayString(stream, document.path);
    stream << '\n' << "  Valid: " << (document.inspection.valid ? "yes" : "no") << '\n';
    stream << "  Format: ";
    text::writeDisplayString(stream, document.format);
    stream << '\n';
    if (!document.profile.empty()) {
        stream << "  Profile: ";
        text::writeDisplayString(stream, document.profile);
        stream << '\n';
    }
    if (document.bytes.has_value())
        stream << "  Bytes: " << *document.bytes << '\n';
    if (document.has_cloud) {
        stream << "  Splats: " << document.inspection.splat_count << '\n'
               << "  SH degree: " << document.inspection.sh_degree << '\n'
               << "  Color space: ";
        text::writeDisplayString(stream, document.color_space);
        stream << '\n';
        if (document.inspection.bounds.available) {
            stream << "  Bounds: [" << document.inspection.bounds.min[0] << ", "
                   << document.inspection.bounds.min[1] << ", " << document.inspection.bounds.min[2]
                   << "] to [" << document.inspection.bounds.max[0] << ", "
                   << document.inspection.bounds.max[1] << ", " << document.inspection.bounds.max[2]
                   << "]\n";
        }
    }
    for (const LossItem& item : document.losses.items()) {
        stream << "  LOSS [";
        text::writeDisplayString(stream, item.code);
        stream << "] ";
        text::writeDisplayString(stream, item.source_feature);
        stream << '\n';
    }
    for (const InspectionIssue& issue : document.inspection.issues) {
        stream << "  " << (issue.severity == InspectionSeverity::Error ? "ERROR" : "WARNING")
               << " [";
        text::writeDisplayString(stream, issue.code);
        stream << "] ";
        text::writeDisplayString(stream, issue.message);
        if (issue.count > 1)
            stream << " (" << issue.count << " occurrences)";
        if (issue.has_index)
            stream << " First index: " << issue.first_index << '.';
        stream << '\n';
        if (!issue.path.empty()) {
            stream << "    Path: ";
            text::writeDisplayString(stream, issue.path);
            stream << '\n';
        }
        if (issue.byte_offset.has_value())
            stream << "    Byte offset: " << *issue.byte_offset << '\n';
        if (!issue.context.empty())
            stream << "    Context: " << context_json(issue).dump(-1, ' ', true) << '\n';
    }
}

void print_usage(const char* program, std::ostream& stream) {
    stream << "Usage: ";
    text::writeDisplayString(stream, program);
    stream << " inspect INPUT [options]\n\n"
              "Validate one Gaussian PLY, SPZ, glTF, or GLB asset.\n\n"
              "Input options:\n"
              "  --input-format FORMAT          ply | spz | gltf | glb\n"
              "  --input-profile PROFILE        Exact profile ID\n"
              "  --source-frame FRAME           gltf-luf | ply-rdf | spz-rub\n"
              "  --source-unit-to-meter VALUE   Positive meters per source unit\n"
              "  --source-color-space SPACE     srgb_rec709_display | lin_rec709_display\n\n"
              "Report options:\n"
              "  --json                         Write melkor.inspect.v1 JSON\n"
              "  --strict                       Fail on warnings or non-informational losses\n"
              "  --limits-profile PROFILE       web | desktop | server\n"
              "  -h, --help                     Show this help\n\n"
              "Exact profile IDs:\n"
              "  ply:melkor-canonical-v1\n"
              "  ply:graphdeco-3dgs-v1\n"
              "  ply:da3-gaussian-v1\n"
              "  spz:spz-v1-v3\n"
              "  khr-gaussian-splatting-rc-63770cc\n\n"
              "PLY and SPZ do not store all source semantics. Supply each missing value.\n"
              "Use -- before a path that starts with '-'.\n";
}

}  // namespace

int runInspectCommand(int argc, char* argv[], const char* program) try {
    AssetReadOptions options;
    LimitsProfile limits_profile = LimitsProfile::desktop;
    std::set<std::string> seen_options;
    std::vector<std::string> positionals;
    bool json = false;
    bool strict = false;
    bool positional_only = false;

    const auto require_value = [&](int index, const std::string& option) {
        if (index + 1 < argc)
            return true;
        std::cerr << "MK1802_USAGE: inspect: ";
        text::writeDisplayString(std::cerr, option);
        std::cerr << " requires a value\n";
        return false;
    };
    const auto mark_once = [&](const std::string& option) {
        if (seen_options.insert(option).second)
            return true;
        std::cerr << "MK1802_USAGE: inspect: option specified more than once: ";
        text::writeDisplayString(std::cerr, option);
        std::cerr << '\n';
        return false;
    };

    for (int index = 0; index < argc; ++index) {
        const std::string argument = argv[index];
        if (!positional_only && (argument == "-h" || argument == "--help")) {
            print_usage(program, std::cout);
            std::cout.flush();
            if (!std::cout) {
                std::cerr << "MK1806_OUTPUT_WRITE_FAILED: standard output could not be written\n";
                return exit_code_for(ErrorCode::io_error);
            }
            return 0;
        }
        if (!positional_only && argument == "--") {
            positional_only = true;
        } else if (!positional_only && argument == "--json") {
            if (!mark_once(argument))
                return 2;
            json = true;
        } else if (!positional_only && argument == "--strict") {
            if (!mark_once(argument))
                return 2;
            strict = true;
        } else if (!positional_only && argument == "--input-format") {
            if (!mark_once(argument) || !require_value(index, argument))
                return 2;
            options.format = parse_format(argv[++index]);
            if (!options.format.has_value()) {
                std::cerr << "MK1802_USAGE: inspect: invalid input format\n";
                return 2;
            }
        } else if (!positional_only && argument == "--input-profile") {
            if (!mark_once(argument) || !require_value(index, argument))
                return 2;
            options.profile = format_profile_from_string(argv[++index]);
            if (!options.profile.has_value()) {
                std::cerr << "MK1802_USAGE: inspect: unknown exact profile ID\n";
                return 2;
            }
        } else if (!positional_only && argument == "--source-frame") {
            if (!mark_once(argument) || !require_value(index, argument))
                return 2;
            options.source_frame_id = argv[++index];
        } else if (!positional_only && argument == "--source-unit-to-meter") {
            if (!mark_once(argument) || !require_value(index, argument))
                return 2;
            double value = 0.0;
            if (!parse_positive_double(argv[++index], value)) {
                std::cerr << "MK1802_USAGE: inspect: --source-unit-to-meter needs a positive "
                             "finite value\n";
                return 2;
            }
            options.source_unit_to_meter = value;
        } else if (!positional_only && argument == "--source-color-space") {
            if (!mark_once(argument) || !require_value(index, argument))
                return 2;
            options.source_color_space = color_space_from_string(argv[++index]);
            if (!options.source_color_space.has_value()) {
                std::cerr << "MK1802_USAGE: inspect: unknown source color space\n";
                return 2;
            }
        } else if (!positional_only && argument == "--limits-profile") {
            if (!mark_once(argument) || !require_value(index, argument))
                return 2;
            auto parsed = limits_profile_from_string(argv[++index]);
            if (!parsed.has_value()) {
                print_diagnostics(parsed.diagnostics(), std::cerr);
                return 2;
            }
            limits_profile = parsed.value();
        } else if (!positional_only && !argument.empty() && argument.front() == '-') {
            std::cerr << "MK1802_USAGE: inspect: unknown option ";
            text::writeDisplayString(std::cerr, argument);
            std::cerr << '\n';
            return 2;
        } else {
            positionals.push_back(argument);
        }
    }

    if (positionals.size() != 1) {
        std::cerr << "MK1802_USAGE: inspect requires one input\n";
        print_usage(program, std::cerr);
        return 2;
    }

    Limits limits = Limits::for_profile(limits_profile);
    auto limits_valid = limits.validate();
    if (!limits_valid.has_value()) {
        print_diagnostics(limits_valid.diagnostics(), std::cerr);
        return exit_code_for(limits_valid.error_code());
    }
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    InterruptController interrupt(context.cancellation);
    if (!interrupt.installed()) {
        std::cerr << "MK1805_SIGNAL_HANDLER: SIGINT handling could not be installed\n";
        return exit_code_for(ErrorCode::internal_error);
    }
    context.progress = &interrupt;

    InspectDocument document = inspect_path(fs::u8path(positionals.front()), options, context);
    if (json)
        std::cout << report_json(document).dump(-1, ' ', true) << '\n';
    else
        write_human(document, std::cout);
    std::cout.flush();
    if (!std::cout) {
        std::cerr << "MK1904_REPORT_WRITE_FAILED: the inspection report could not be written\n";
        return exit_code_for(ErrorCode::io_error);
    }
    if (!document.inspection.valid) {
        const ErrorCode code =
            document.error_code == ErrorCode::ok ? ErrorCode::invalid_data : document.error_code;
        return exit_code_for(code);
    }
    if (strict) {
        const bool has_loss_finding =
            std::any_of(document.losses.items().begin(), document.losses.items().end(),
                        [](const LossItem& item) { return item.severity != LossSeverity::info; });
        if (document.inspection.warning_count != 0 || has_loss_finding)
            return 1;
    }
    return 0;
} catch (const std::bad_alloc&) {
    std::cerr << "MK1905_INSPECT_MEMORY: inspection exhausted available memory\n";
    return exit_code_for(ErrorCode::resource_limit);
} catch (const std::length_error&) {
    std::cerr << "MK1905_INSPECT_MEMORY: inspection exceeded a container limit\n";
    return exit_code_for(ErrorCode::resource_limit);
} catch (const std::exception&) {
    std::cerr << "MK1906_INSPECT_EXCEPTION: inspection failed safely\n";
    return exit_code_for(ErrorCode::internal_error);
} catch (...) {
    std::cerr << "MK1906_INSPECT_EXCEPTION: inspection failed safely\n";
    return exit_code_for(ErrorCode::internal_error);
}

}  // namespace melkor::cli
