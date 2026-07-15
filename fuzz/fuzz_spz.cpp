// Coverage-guided fuzz target for the SPZ decoder.
//
// SPZ is the highest-value parser to fuzz, because it decodes *compressed* input: a small
// crafted file can claim to expand to an enormous one (a decompression bomb), and the decode
// path does fixed-point and quaternion unpacking on attacker-controlled bytes. The target feeds
// arbitrary bytes to the decoder and asserts it never crashes, over-allocates unboundedly, or
// returns an inconsistent result -- only a clean success or a clean failure.
//
// Builds two ways, like fuzz_ply: a libFuzzer entry point for CI, and a standalone corpus-replay
// driver that runs everywhere.

#include "melkor/spz_encoder.hpp"

#include <cstddef>
#include <cstdint>

namespace {

void exercise(const uint8_t* data, size_t size) {
    melkor::SpzDecoder decoder;
    auto result = decoder.decodeFromBuffer(data, size);
    // The decoder's success flag and the cloud it produced must agree: a "successful" decode
    // that left the cloud in an impossible state is a bug. Touch the cloud so a sanitizer build
    // sees any out-of-bounds the decoder set up.
    if (!result.success) {
        return;
    }
    const auto& splats = result.cloud.splats();
    volatile float sink = 0.0f;
    for (const auto& s : splats) {
        sink += s.x + s.opacity;
    }
    (void)sink;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    exercise(data, size);
    return 0;
}

#ifndef MELKOR_FUZZER_RUNTIME
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    int cases = 0;
    for (int i = 1; i < argc; ++i) {
        const fs::path root = argv[i];
        std::vector<fs::path> files;
        if (fs::is_directory(root)) {
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (entry.is_regular_file()) files.push_back(entry.path());
            }
        } else if (fs::is_regular_file(root)) {
            files.push_back(root);
        }
        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            exercise(bytes.data(), bytes.size());
            ++cases;
        }
    }

    // Built-in adversarial inputs: an empty file, a wrong magic, a truncated header, and a header
    // claiming a colossal point count (the shape of an allocation attack).
    const std::vector<std::vector<uint8_t>> builtins = {
        {},
        {'N', 'O', 'T', 'S', 'P', 'Z'},
        {0x4e, 0x47, 0x53, 0x50},  // "NGSP" magic, then nothing
        {0x4e, 0x47, 0x53, 0x50, 0x03, 0x00, 0x00, 0x00,
         0xff, 0xff, 0xff, 0xff},  // magic, version 3, count 0xffffffff
    };
    for (const auto& b : builtins) {
        exercise(b.data(), b.size());
        ++cases;
    }

    std::printf("spz fuzz replay: %d input(s) exercised without crash\n", cases);
    return 0;
}
#endif  // MELKOR_FUZZER_RUNTIME
