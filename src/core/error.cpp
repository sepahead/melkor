#include "melkor/error.hpp"

#include <algorithm>
#include <filesystem>

namespace melkor {
namespace {

bool is_path_separator(char character) noexcept {
    return character == '/' || character == '\\';
}

std::string path_basename(const std::string& path) {
    std::size_t end = path.size();
    while (end > 1 && is_path_separator(path[end - 1])) {
        const bool drive_root = end == 3 && path[1] == ':';
        if (drive_root) {
            break;
        }
        --end;
    }

    const std::size_t slash = path.find_last_of("/\\", end - 1);
    if (slash == std::string::npos) {
        return path.substr(0, end);
    }
    if (slash + 1 == end) {
        return path.substr(0, end);
    }
    return path.substr(slash + 1, end - slash - 1);
}

bool stays_within_root(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    const auto first = relative.begin();
    return first != relative.end() && *first != "..";
}

}  // namespace

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::ok:
        return "ok";
    case ErrorCode::invalid_argument:
        return "invalid_argument";
    case ErrorCode::invalid_data:
        return "invalid_data";
    case ErrorCode::unsupported_feature:
        return "unsupported_feature";
    case ErrorCode::io_error:
        return "io_error";
    case ErrorCode::resource_limit:
        return "resource_limit";
    case ErrorCode::backend_unavailable:
        return "backend_unavailable";
    case ErrorCode::cancelled:
        return "cancelled";
    case ErrorCode::internal_error:
        return "internal_error";
    }
    return "unknown";
}

int exit_code_for(ErrorCode code) noexcept {
    // These are the documented CLI exit classes. They are part of the public contract: a
    // script may branch on them, so a value never changes meaning between releases.
    switch (code) {
    case ErrorCode::ok:
        return 0;
    case ErrorCode::invalid_argument:
        return 2;
    case ErrorCode::invalid_data:
        return 3;
    case ErrorCode::unsupported_feature:
        return 4;
    case ErrorCode::io_error:
        return 5;
    case ErrorCode::resource_limit:
        return 6;
    case ErrorCode::backend_unavailable:
        return 7;
    case ErrorCode::internal_error:
        return 8;
    case ErrorCode::cancelled:
        // 130 is the shell convention for "terminated by SIGINT" (128 + SIGINT). Using it
        // means `melkor ... ; echo $?` behaves the way a user's other tools do.
        return 130;
    }
    return 8;
}

const char* to_string(Severity severity) noexcept {
    switch (severity) {
    case Severity::note:
        return "note";
    case Severity::warning:
        return "warning";
    case Severity::error:
        return "error";
    }
    return "unknown";
}

Diagnostic& Diagnostic::with_path(std::string value) & {
    path = std::move(value);
    return *this;
}

Diagnostic& Diagnostic::with_offset(std::uint64_t value) & {
    byte_offset = value;
    return *this;
}

Diagnostic& Diagnostic::with_context(std::string key, JsonScalar value) & {
    context.insert_or_assign(std::move(key), std::move(value));
    return *this;
}

std::string redact_path(const std::string& path, DiagnosticPathPolicy policy,
                        const std::string& root) {
    if (path.empty()) {
        return path;
    }

    switch (policy) {
    case DiagnosticPathPolicy::full:
        return path;

    case DiagnosticPathPolicy::relative: {
        if (!root.empty()) {
            const std::filesystem::path normalized_path =
                std::filesystem::u8path(path).lexically_normal();
            const std::filesystem::path normalized_root =
                std::filesystem::u8path(root).lexically_normal();
            const std::filesystem::path relative =
                normalized_path.lexically_relative(normalized_root);
            if (stays_within_root(relative)) {
                return relative.generic_u8string();
            }
        }
        // An outside path falls back to the stricter policy.
        [[fallthrough]];
    }

    case DiagnosticPathPolicy::basename:
        return path_basename(path);
    }
    return path;
}

}  // namespace melkor
