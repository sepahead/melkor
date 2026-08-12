#pragma once

#include "melkor/error.hpp"

#include <iosfwd>
#include <vector>

namespace melkor::cli {

// Write stable diagnostic codes and their structured context as safe terminal text.
void print_diagnostics(const std::vector<Diagnostic>& diagnostics, std::ostream& stream);

}  // namespace melkor::cli
