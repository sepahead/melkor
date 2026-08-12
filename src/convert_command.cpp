#include "convert_command.hpp"

#include "cli_diagnostics.hpp"
#include "cli_interrupt.hpp"

#include "melkor/budget.hpp"
#include "melkor/color_space.hpp"
#include "melkor/format/registry.hpp"
#include "melkor/limits.hpp"
#include <nlohmann/json.hpp>
#include "safe_text.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace melkor::cli {
namespace {

void print_usage(const char* program, std::ostream& stream) {
    stream << "Usage: ";
    text::writeDisplayString(stream, program);
    stream << " convert INPUT OUTPUT [options]\n\n"
              "Convert a Gaussian asset through the canonical Melkor model.\n\n"
              "Input options:\n"
              "  --input-format FORMAT          ply | spz | gltf | glb\n"
              "  --input-profile PROFILE        Exact profile ID listed below\n"
              "  --source-frame FRAME           gltf-luf | ply-rdf | spz-rub\n"
              "  --source-unit-to-meter VALUE   Positive meters per source unit\n"
              "  --source-color-space SPACE     srgb_rec709_display | lin_rec709_display\n\n"
              "Output options:\n"
              "  --output-format FORMAT         ply | spz | glb\n"
              "  --output-profile PROFILE       Exact profile ID listed below\n"
              "  --target-frame FRAME           gltf-luf | ply-rdf | spz-rub\n"
              "  --output-antialiased BOOL      true | false\n"
              "  --max-sh-degree DEGREE         0 through 4\n"
              "  --ascii                        Write ASCII PLY\n"
              "  --force                        Replace an existing output\n\n"
              "Policy options:\n"
              "  --allow-loss CODE              Approve one severe loss. Repeat as needed.\n"
              "  --limits-profile PROFILE       web | desktop | server\n"
              "  -h, --help                     Show this help\n\n"
              "Exact profile IDs:\n"
              "  ply:melkor-canonical-v1\n"
              "  ply:graphdeco-3dgs-v1\n"
              "  ply:da3-gaussian-v1\n"
              "  spz:spz-v1-v3\n"
              "  khr-gaussian-splatting-rc-63770cc\n\n"
              "Use -- before a path that starts with '-'.\n"
              "The command writes one loss-report JSON document to stdout.\n";
}

std::optional<FormatId> parse_format(const std::string& value, bool output) {
    if (value == "ply")
        return FormatId::ply;
    if (value == "spz")
        return FormatId::spz;
    if (!output && value == "gltf")
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

bool parse_degree(const std::string& value, int& output) {
    int parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < 0 || parsed > 4)
        return false;
    output = parsed;
    return true;
}

bool parse_bool(const std::string& value, bool& output) {
    if (value == "true") {
        output = true;
        return true;
    }
    if (value == "false") {
        output = false;
        return true;
    }
    return false;
}

nlohmann::ordered_json loss_report_json(const ConversionResult& result,
                                        const std::vector<std::string>& approved) {
    nlohmann::ordered_json report;
    report["schema_version"] = LossReport::kSchemaVersion;
    report["input"] = {
        {"format", to_string(result.input_format)},
        {"profile", std::string(format_profile(result.input_profile).profile_id)},
    };
    report["output"] = {
        {"format", to_string(result.output_format)},
        {"profile", std::string(format_profile(result.output_profile).profile_id)},
    };
    report["items"] = nlohmann::ordered_json::array();
    for (const LossItem& item : result.losses.items()) {
        report["items"].push_back({
            {"code", item.code},
            {"severity", to_string(item.severity)},
            {"source_feature", item.source_feature},
            {"target_constraint", item.target_constraint},
            {"affected_splats", item.affected_splats},
            {"remediation", item.remediation},
        });
    }
    report["approved_codes"] = approved;
    return report;
}

Result<void> publish_loss_report(const ConversionResult& result,
                                 const std::vector<std::string>& approved) try {
    const std::string report = loss_report_json(result, approved).dump(-1, ' ', true);
    std::cout << report << '\n';
    std::cout.flush();
    if (!std::cout) {
        return Result<void>::failure(ErrorCode::io_error,
                                     Diagnostic("MK1726_REPORT_WRITE_FAILED", Severity::error,
                                                "the loss report could not be written"));
    }
    return Result<void>::success();
} catch (const std::bad_alloc&) {
    return Result<void>::failure(ErrorCode::resource_limit,
                                 Diagnostic("MK1715_REGISTRY_MEMORY", Severity::error,
                                            "loss-report creation exhausted available memory"));
} catch (const std::length_error&) {
    return Result<void>::failure(ErrorCode::resource_limit,
                                 Diagnostic("MK1715_REGISTRY_MEMORY", Severity::error,
                                            "loss-report creation exceeded a container limit"));
} catch (const nlohmann::json::exception&) {
    return Result<void>::failure(ErrorCode::internal_error,
                                 Diagnostic("MK1725_REPORT_ENCODING_FAILED", Severity::error,
                                            "the loss report could not be encoded"));
}

}  // namespace

int runConvertCommand(int argc, char* argv[], const char* program) try {
    ConversionRequest request;
    LimitsProfile limits_profile = LimitsProfile::desktop;
    std::vector<std::string> positionals;
    std::set<std::string> seen_options;
    bool positional_only = false;

    const auto require_value = [&](int index, const std::string& option) {
        if (index + 1 < argc)
            return true;
        std::cerr << "MK1802_USAGE: convert: ";
        text::writeDisplayString(std::cerr, option);
        std::cerr << " requires a value\n";
        return false;
    };

    const auto mark_once = [&](const std::string& option) {
        if (seen_options.insert(option).second)
            return true;
        std::cerr << "MK1802_USAGE: convert: option specified more than once: ";
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
        } else if (!positional_only &&
                   (argument == "--input-format" || argument == "--output-format")) {
            if (!mark_once(argument))
                return 2;
            if (!require_value(index, argument))
                return 2;
            const bool output = argument == "--output-format";
            auto format = parse_format(argv[++index], output);
            if (!format.has_value()) {
                std::cerr << "MK1802_USAGE: convert: invalid format for ";
                text::writeDisplayString(std::cerr, argument);
                std::cerr << '\n';
                return 2;
            }
            if (output)
                request.write.format = format;
            else
                request.read.format = format;
        } else if (!positional_only &&
                   (argument == "--input-profile" || argument == "--output-profile")) {
            if (!mark_once(argument))
                return 2;
            if (!require_value(index, argument))
                return 2;
            auto profile = format_profile_from_string(argv[++index]);
            if (!profile.has_value()) {
                std::cerr << "MK1802_USAGE: convert: unknown exact profile ID\n";
                return 2;
            }
            if (argument == "--output-profile")
                request.write.profile = profile;
            else
                request.read.profile = profile;
        } else if (!positional_only && argument == "--source-frame") {
            if (!mark_once(argument))
                return 2;
            if (!require_value(index, argument))
                return 2;
            request.read.source_frame_id = argv[++index];
        } else if (!positional_only && argument == "--source-unit-to-meter") {
            if (!mark_once(argument))
                return 2;
            if (!require_value(index, argument))
                return 2;
            double scale = 0.0;
            if (!parse_positive_double(argv[++index], scale)) {
                std::cerr << "MK1802_USAGE: convert: --source-unit-to-meter needs a positive "
                             "finite value\n";
                return 2;
            }
            request.read.source_unit_to_meter = scale;
        } else if (!positional_only && argument == "--source-color-space") {
            if (!mark_once(argument))
                return 2;
            if (!require_value(index, argument))
                return 2;
            auto color_space = color_space_from_string(argv[++index]);
            if (!color_space.has_value()) {
                std::cerr << "MK1802_USAGE: convert: unknown source color space\n";
                return 2;
            }
            request.read.source_color_space = color_space;
        } else if (!positional_only && argument == "--target-frame") {
            if (!mark_once(argument))
                return 2;
            if (!require_value(index, argument))
                return 2;
            request.write.target_frame_id = argv[++index];
        } else if (!positional_only && argument == "--output-antialiased") {
            if (!mark_once(argument))
                return 2;
            if (!require_value(index, argument))
                return 2;
            bool antialiased = false;
            if (!parse_bool(argv[++index], antialiased)) {
                std::cerr << "MK1802_USAGE: convert: --output-antialiased must be true or false\n";
                return 2;
            }
            request.write.antialiased = antialiased;
        } else if (!positional_only && argument == "--max-sh-degree") {
            if (!mark_once(argument))
                return 2;
            if (!require_value(index, argument))
                return 2;
            if (!parse_degree(argv[++index], request.write.sh_degree)) {
                std::cerr << "MK1802_USAGE: convert: --max-sh-degree must be from 0 through 4\n";
                return 2;
            }
        } else if (!positional_only && argument == "--allow-loss") {
            if (!require_value(index, argument))
                return 2;
            request.write.approved_loss_codes.emplace_back(argv[++index]);
        } else if (!positional_only && argument == "--limits-profile") {
            if (!mark_once(argument))
                return 2;
            if (!require_value(index, argument))
                return 2;
            auto parsed = limits_profile_from_string(argv[++index]);
            if (!parsed.has_value()) {
                print_diagnostics(parsed.diagnostics(), std::cerr);
                return 2;
            }
            limits_profile = parsed.value();
        } else if (!positional_only && argument == "--ascii") {
            if (!mark_once(argument))
                return 2;
            request.write.ascii = true;
        } else if (!positional_only && argument == "--force") {
            if (!mark_once(argument))
                return 2;
            request.write.overwrite = true;
        } else if (!positional_only && !argument.empty() && argument.front() == '-') {
            std::cerr << "MK1802_USAGE: convert: unknown option ";
            text::writeDisplayString(std::cerr, argument);
            std::cerr << '\n';
            return 2;
        } else {
            positionals.push_back(argument);
        }
    }

    if (positionals.size() != 2) {
        std::cerr << "MK1802_USAGE: convert requires one input and one output\n";
        print_usage(program, std::cerr);
        return 2;
    }
    request.input = std::filesystem::u8path(positionals[0]);
    request.output = std::filesystem::u8path(positionals[1]);

    std::sort(request.write.approved_loss_codes.begin(), request.write.approved_loss_codes.end());
    if (std::adjacent_find(request.write.approved_loss_codes.begin(),
                           request.write.approved_loss_codes.end()) !=
        request.write.approved_loss_codes.end()) {
        std::cerr << "MK1802_USAGE: convert: each --allow-loss code must be unique\n";
        return 2;
    }
    for (const std::string& code : request.write.approved_loss_codes) {
        if (!is_known_loss_code(code)) {
            std::cerr << "MK1802_USAGE: convert: unknown loss code ";
            text::writeDisplayString(std::cerr, code);
            std::cerr << '\n';
            return 2;
        }
    }

    Limits limits = Limits::for_profile(limits_profile);
    auto valid_limits = limits.validate();
    if (!valid_limits.has_value()) {
        print_diagnostics(valid_limits.diagnostics(), std::cerr);
        return exit_code_for(valid_limits.error_code());
    }
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    InterruptController interrupt(context.cancellation);
    if (!interrupt.installed()) {
        std::cerr << "MK1805_SIGNAL_HANDLER: SIGINT handling could not be installed\n";
        return exit_code_for(ErrorCode::internal_error);
    }
    context.progress = &interrupt;

    const ConversionCommitGate report_gate = [&](const ConversionResult& result) {
        return publish_loss_report(result, request.write.approved_loss_codes);
    };
    auto converted = convert_file(request, context, report_gate);
    if (!converted.has_value()) {
        print_diagnostics(converted.diagnostics(), std::cerr);
        return exit_code_for(converted.error_code());
    }

    return 0;
} catch (const std::bad_alloc&) {
    std::cerr << "MK1715_REGISTRY_MEMORY: conversion exhausted available memory\n";
    return exit_code_for(ErrorCode::resource_limit);
} catch (const std::length_error&) {
    std::cerr << "MK1715_REGISTRY_MEMORY: conversion exceeded a container limit\n";
    return exit_code_for(ErrorCode::resource_limit);
} catch (const std::exception&) {
    std::cerr << "MK1727_CONVERT_EXCEPTION: conversion failed safely\n";
    return exit_code_for(ErrorCode::internal_error);
}

}  // namespace melkor::cli
