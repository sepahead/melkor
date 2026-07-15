#include "convert_command.hpp"

#include "melkor/budget.hpp"
#include "melkor/format/gltf_reader.hpp"
#include "melkor/format/gltf_writer.hpp"
#include "melkor/io/atomic_writer.hpp"
#include "melkor/limits.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace melkor::cli {

namespace {

std::string lowercase_extension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    return ext;
}

bool is_gltf_container(const std::string& ext) { return ext == "glb" || ext == "gltf"; }

void print_usage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s convert INPUT.glb OUTPUT.glb [options]\n"
                 "  Reads a KHR_gaussian_splatting GLB and writes it back out, applying the loss\n"
                 "  policy and writing the output atomically.\n\n"
                 "Options:\n"
                 "  --allow-loss CODE        Approve a specific severe loss (repeatable), e.g.\n"
                 "                           --allow-loss LOSS_SH_DEGREE_TRUNCATED\n"
                 "  --limits-profile PROFILE web | desktop | server (default: desktop)\n"
                 "  -h, --help               Show this help\n\n"
                 "Cross-format conversion (PLY/SPZ) is planned (WP13); this command handles the\n"
                 "canonical GLB path only.\n",
                 program);
}

void report_losses(const LossReport& losses, const char* stage) {
    if (losses.empty()) return;
    std::fprintf(stderr, "  %s losses:\n", stage);
    for (const auto& item : losses.items()) {
        std::fprintf(stderr, "    [%s] %s (%llu splats)%s%s\n", to_string(item.severity), item.code.c_str(),
                     static_cast<unsigned long long>(item.affected_splats),
                     item.source_feature.empty() ? "" : ": ", item.source_feature.c_str());
    }
}

}  // namespace

int runConvertCommand(int argc, char* argv[], const char* program) {
    std::vector<std::string> positionals;
    std::vector<std::string> approved_losses;
    LimitsProfile profile = LimitsProfile::desktop;

    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(program);
            return 0;
        }
        if (arg == "--allow-loss") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "convert: --allow-loss requires a loss code\n");
                return 2;
            }
            approved_losses.emplace_back(argv[++i]);
        } else if (arg == "--limits-profile") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "convert: --limits-profile requires a value\n");
                return 2;
            }
            auto parsed = limits_profile_from_string(argv[++i]);
            if (!parsed.has_value()) {
                std::fprintf(stderr, "convert: unknown limits profile '%s'\n", argv[i]);
                return 2;
            }
            profile = parsed.value();
        } else if (!arg.empty() && arg.front() == '-') {
            std::fprintf(stderr, "convert: unknown option '%s'\n", arg.c_str());
            return 2;
        } else {
            positionals.push_back(arg);
        }
    }

    if (positionals.size() != 2) {
        print_usage(program);
        return 2;
    }
    const std::string& input_path = positionals[0];
    const std::string& output_path = positionals[1];

    if (!is_gltf_container(lowercase_extension(input_path)) ||
        !is_gltf_container(lowercase_extension(output_path))) {
        std::fprintf(stderr,
                     "convert: this command handles GLB KHR_gaussian_splatting only; cross-format "
                     "conversion is planned (WP13)\n");
        return 2;
    }

    // Read the whole input file.
    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "convert: cannot open input '%s'\n", input_path.c_str());
        return 1;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());

    // Read into the canonical scene model, bounded by the chosen limits.
    auto scene = format::gltf::read_glb(bytes.data(), bytes.size(), Limits::for_profile(profile));
    if (!scene.has_value()) {
        std::fprintf(stderr, "convert: failed to read '%s': %s\n", input_path.c_str(),
                     scene.diagnostics().empty() ? "unknown error"
                                                 : scene.diagnostics()[0].message.c_str());
        return 1;
    }

    // The read's loss report must be approved before the conversion may proceed.
    if (auto policy = scene.value().losses.check_policy(approved_losses); !policy.has_value()) {
        std::fprintf(stderr, "convert: refusing to proceed -- %s\n",
                     policy.diagnostics().empty() ? "an unapproved severe loss"
                                                  : policy.diagnostics()[0].message.c_str());
        report_losses(scene.value().losses, "read");
        return 3;
    }

    // Write the canonical splats back out as a GLB.
    auto written = format::gltf::write_glb(scene.value().data, scene.value().color_space);
    if (!written.has_value()) {
        std::fprintf(stderr, "convert: failed to encode output: %s\n",
                     written.diagnostics().empty() ? "unknown error"
                                                   : written.diagnostics()[0].message.c_str());
        return 1;
    }
    if (auto policy = written.value().losses.check_policy(approved_losses); !policy.has_value()) {
        std::fprintf(stderr, "convert: refusing to write -- %s\n",
                     policy.diagnostics().empty() ? "an unapproved severe loss"
                                                  : policy.diagnostics()[0].message.c_str());
        report_losses(written.value().losses, "write");
        return 3;
    }

    // Write the output atomically: the destination is never truncated on failure.
    Budget budget(Limits::for_profile(profile));
    OperationContext context = make_default_context(budget);
    io::WriteOptions options;
    auto writer = io::AtomicWriter::create(output_path, options, context);
    if (!writer.has_value()) {
        std::fprintf(stderr, "convert: cannot create output '%s': %s\n", output_path.c_str(),
                     writer.diagnostics().empty() ? "unknown error"
                                                  : writer.diagnostics()[0].message.c_str());
        return 1;
    }
    if (auto w = writer.value()->write(written.value().bytes.data(), written.value().bytes.size());
        !w.has_value()) {
        std::fprintf(stderr, "convert: failed to write output: %s\n",
                     w.diagnostics().empty() ? "unknown error" : w.diagnostics()[0].message.c_str());
        return 1;
    }
    if (auto c = writer.value()->commit(); !c.has_value()) {
        std::fprintf(stderr, "convert: failed to commit output: %s\n",
                     c.diagnostics().empty() ? "unknown error" : c.diagnostics()[0].message.c_str());
        return 1;
    }

    std::fprintf(stderr, "convert: wrote %zu splats (SH degree %u) to %s\n", scene.value().data.size(),
                 scene.value().sh_degree, output_path.c_str());
    report_losses(scene.value().losses, "read");
    return 0;
}

}  // namespace melkor::cli
