// Coverage-guided fuzz target for the GLB/glTF reader.
//
// The glTF path does a great deal of arithmetic on file-declared values -- buffer offsets,
// accessor counts, strides, chunk lengths -- which is exactly where an integer-overflow or
// out-of-bounds bug lives. This feeds arbitrary bytes to the GLB reader and asserts it never
// crashes or reads out of bounds, only returns a clean success or a clean failure.
//
// Builds two ways, like the other fuzz targets: a libFuzzer entry point for CI, and a standalone
// corpus-replay driver.

#include "melkor/glb_reader.hpp"

#include <cstddef>
#include <cstdint>

namespace {

void exercise(const uint8_t* data, size_t size) {
    melkor::GlbReader reader;
    auto result = reader.loadFromMemory(data, size);
    if (!result.success) {
        return;
    }
    // Touch the produced cloud so a sanitizer build observes any bad memory the reader set up.
    const auto& splats = result.cloud.splats();
    volatile float sink = 0.0f;
    for (const auto& s : splats) {
        sink += s.x + s.y + s.z;
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

    // Built-in adversarial GLB shapes: empty, wrong magic, valid magic with a lying chunk length.
    const std::vector<std::vector<uint8_t>> builtins = {
        {},
        {'n', 'o', 'p', 'e'},
        // "glTF" magic, version 2, total length 12 (header only, no chunks).
        {0x67, 0x6c, 0x54, 0x46, 0x02, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00},
        // magic, version 2, total length lying as 0xffffffff.
        {0x67, 0x6c, 0x54, 0x46, 0x02, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff},
    };
    for (const auto& b : builtins) {
        exercise(b.data(), b.size());
        ++cases;
    }

    std::printf("glb fuzz replay: %d input(s) exercised without crash\n", cases);
    return 0;
}
#endif  // MELKOR_FUZZER_RUNTIME
