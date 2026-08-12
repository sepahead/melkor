#pragma once

namespace melkor::cli {

// Convert one Gaussian asset through the canonical model.
// Each successful conversion writes one loss report to stdout.
int runConvertCommand(int argc, char* argv[], const char* program);

}  // namespace melkor::cli
