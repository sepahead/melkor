#include "melkor/version.h"

#include "convert_command.hpp"
#include "inspect_command.hpp"
#include "melkor/error.hpp"
#include "safe_text.hpp"

#include <csignal>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

void write_program(std::ostream& stream, const char* program) {
    melkor::text::writeDisplayString(stream, program == nullptr ? "melkor" : program);
}

void print_usage(std::ostream& stream, const char* program) {
    stream << "Melkor validates and converts Gaussian splat assets.\n\n"
              "Usage:\n  ";
    write_program(stream, program);
    stream << " convert INPUT OUTPUT [options]\n  ";
    write_program(stream, program);
    stream << " inspect INPUT [options]\n  ";
    write_program(stream, program);
    stream << " --version\n\n"
              "Commands:\n"
              "  convert  Convert PLY, SPZ, glTF, or GLB Gaussian data.\n"
              "  inspect  Validate an asset and report its structure.\n\n"
              "Global options:\n"
              "  -h, --help  Show this help.\n"
              "  --version   Show the Melkor version.\n\n"
              "Run a command with --help for its options.\n";
}

bool finish_stdout() {
    std::cout.flush();
    if (std::cout)
        return true;
    std::cerr << "MK1806_OUTPUT_WRITE_FAILED: standard output could not be written\n";
    return false;
}

#if defined(_WIN32)
std::optional<std::string> utf8_argument(const wchar_t* value) {
    if (value == nullptr)
        return std::nullopt;
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0,
                                               nullptr, nullptr);
    if (required <= 0)
        return std::nullopt;
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), required,
                              nullptr, nullptr) != required) {
        return std::nullopt;
    }
    result.pop_back();
    return result;
}
#endif

}  // namespace

int run_program(int argc, char* argv[]) try {
    const char* program = argc > 0 ? argv[0] : "melkor";
#if defined(SIGPIPE)
    // Let stream writes report EPIPE. This keeps staged output cleanup on the normal error path.
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        std::cerr << "MK1805_SIGNAL_HANDLER: SIGPIPE handling could not be installed\n";
        return melkor::exit_code_for(melkor::ErrorCode::internal_error);
    }
#endif
    if (argc < 2) {
        std::cerr << "MK1802_USAGE: a command is required\n";
        print_usage(std::cerr, program);
        return melkor::exit_code_for(melkor::ErrorCode::invalid_argument);
    }

    const std::string command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        if (argc != 2) {
            std::cerr << "MK1802_USAGE: the help command accepts no arguments\n";
            return melkor::exit_code_for(melkor::ErrorCode::invalid_argument);
        }
        print_usage(std::cout, program);
        return finish_stdout() ? 0 : melkor::exit_code_for(melkor::ErrorCode::io_error);
    }
    if (command == "--version" || command == "version") {
        if (argc != 2) {
            std::cerr << "MK1802_USAGE: the version command accepts no arguments\n";
            return melkor::exit_code_for(melkor::ErrorCode::invalid_argument);
        }
        std::cout << "melkor " << MELKOR_VERSION_STRING << '\n';
        return finish_stdout() ? 0 : melkor::exit_code_for(melkor::ErrorCode::io_error);
    }
    if (command == "convert")
        return melkor::cli::runConvertCommand(argc - 2, argv + 2, program);
    if (command == "inspect")
        return melkor::cli::runInspectCommand(argc - 2, argv + 2, program);
    std::cerr << "MK1802_USAGE: unknown command ";
    melkor::text::writeDisplayString(std::cerr, command);
    std::cerr << '\n';
    print_usage(std::cerr, program);
    return melkor::exit_code_for(melkor::ErrorCode::invalid_argument);
} catch (const std::bad_alloc&) {
    std::cerr << "MK1803_MEMORY: the command exhausted available memory\n";
    return melkor::exit_code_for(melkor::ErrorCode::resource_limit);
} catch (const std::length_error&) {
    std::cerr << "MK1803_MEMORY: the command exceeded a container limit\n";
    return melkor::exit_code_for(melkor::ErrorCode::resource_limit);
} catch (const std::exception&) {
    std::cerr << "MK1804_INTERNAL: the command failed safely\n";
    return melkor::exit_code_for(melkor::ErrorCode::internal_error);
} catch (...) {
    std::cerr << "MK1804_INTERNAL: the command failed safely\n";
    return melkor::exit_code_for(melkor::ErrorCode::internal_error);
}

#if defined(_WIN32)
int wmain(int argc, wchar_t* wide_argv[]) try {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        auto converted = utf8_argument(wide_argv[index]);
        if (!converted.has_value()) {
            std::cerr << "MK1802_USAGE: a command argument is not valid Unicode\n";
            return melkor::exit_code_for(melkor::ErrorCode::invalid_argument);
        }
        arguments.push_back(std::move(*converted));
    }
    std::vector<char*> pointers;
    pointers.reserve(arguments.size());
    for (std::string& argument : arguments)
        pointers.push_back(argument.data());
    return run_program(argc, pointers.data());
} catch (const std::bad_alloc&) {
    std::cerr << "MK1803_MEMORY: the command exhausted available memory\n";
    return melkor::exit_code_for(melkor::ErrorCode::resource_limit);
} catch (...) {
    std::cerr << "MK1804_INTERNAL: the command failed safely\n";
    return melkor::exit_code_for(melkor::ErrorCode::internal_error);
}
#else
int main(int argc, char* argv[]) {
    return run_program(argc, argv);
}
#endif
