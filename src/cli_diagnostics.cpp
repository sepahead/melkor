#include "cli_diagnostics.hpp"

#include "safe_text.hpp"

#include <ostream>
#include <type_traits>
#include <variant>

namespace melkor::cli {
namespace {

void write_scalar(std::ostream& stream, const JsonScalar& value) {
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                stream << "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                stream << (item ? "true" : "false");
            } else if constexpr (std::is_same_v<T, std::string>) {
                text::writeDisplayString(stream, item);
            } else {
                stream << item;
            }
        },
        value);
}

}  // namespace

void print_diagnostics(const std::vector<Diagnostic>& diagnostics, std::ostream& stream) {
    for (const Diagnostic& diagnostic : diagnostics) {
        text::writeDisplayString(stream, diagnostic.code);
        stream << ": ";
        text::writeDisplayString(stream, diagnostic.message);
        stream << '\n';
        if (!diagnostic.path.empty()) {
            stream << "  path: ";
            text::writeDisplayString(stream, diagnostic.path);
            stream << '\n';
        }
        if (diagnostic.byte_offset.has_value())
            stream << "  byte_offset: " << *diagnostic.byte_offset << '\n';
        for (const auto& [key, value] : diagnostic.context) {
            stream << "  context.";
            text::writeDisplayString(stream, key);
            stream << ": ";
            write_scalar(stream, value);
            stream << '\n';
        }
    }
}

}  // namespace melkor::cli
