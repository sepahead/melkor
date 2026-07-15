#pragma once

namespace melkor::cli {

// `melkor convert INPUT OUTPUT [--allow-loss CODE]... [--limits-profile web|desktop|server]`.
//
// Reads a KHR_gaussian_splatting GLB into the canonical scene model and writes it back out as a
// GLB, applying the loss policy (a severe loss blocks unless its exact code is approved) and
// writing the output atomically. This is the safe, canonical path; cross-format conversion (with
// the domain conversions it requires) is a separate, larger piece (WP13).
int runConvertCommand(int argc, char* argv[], const char* program);

}  // namespace melkor::cli
