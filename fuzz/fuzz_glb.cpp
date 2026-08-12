// Coverage-guided fuzz target for the GLB/glTF reader.
//
// The glTF path does a great deal of arithmetic on file-declared values -- buffer offsets,
// accessor counts, strides, chunk lengths -- which is exactly where an integer-overflow or
// out-of-bounds bug lives. This feeds arbitrary bytes to the GLB reader and asserts it never
// crashes or reads out of bounds, only returns a clean success or a clean failure.
//
// Builds two ways, like the other fuzz targets: a libFuzzer entry point for CI, and a standalone
// corpus-replay driver.

#include "melkor/format/gltf_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace {

void exercise(const uint8_t* data, size_t size) {
    auto result = melkor::format::gltf::read_glb(data, size);
    if (!result.has_value()) {
        return;
    }
    if (!result.value().data.validate().has_value()) {
        std::abort();
    }
    // Touch canonical positions so a sanitizer build observes any bad memory the reader set up.
    volatile float sink = 0.0f;
    for (const auto& position : result.value().data.positions()) {
        sink += position.x + position.y + position.z;
    }
    (void)sink;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    exercise(data, size);
    return 0;
}

#ifndef MELKOR_FUZZER_RUNTIME
#include "replay.hpp"

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    return melkor::fuzzing::run_standalone_main([&] {
        int cases = 0;
        if (!melkor::fuzzing::replay_requested_inputs(argc, argv, exercise, cases))
            return 1;

        const std::vector<std::vector<uint8_t>> builtins = {
            {},
            {'n', 'o', 'p', 'e'},
            {0x67, 0x6c, 0x54, 0x46, 0x02, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00},
            {0x67, 0x6c, 0x54, 0x46, 0x02, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff},
        };
        for (const auto& bytes : builtins) {
            exercise(bytes.data(), bytes.size());
            ++cases;
        }

        std::printf("glb fuzz replay: %d input(s) exercised without crash\n", cases);
        return 0;
    });
}
#endif  // MELKOR_FUZZER_RUNTIME
