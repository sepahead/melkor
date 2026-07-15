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
    // The decoder's success flag and canonical data must agree. Touch every position so ASan
    // sees any out-of-bounds the decoder set up.
    if (!result.success) {
        return;
    }
    if (!result.data.has_value() || !result.data->validate().has_value()) {
        __builtin_trap();
    }
    volatile float sink = 0.0f;
    for (std::size_t i = 0; i < result.data->size(); ++i) {
        sink += result.data->positions()[i].x + result.data->opacities()[i];
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
    int cases = 0;
    if (!melkor::fuzzing::replay_requested_inputs(argc, argv, exercise, cases))
        return 1;

    // Built-in adversarial inputs: an empty file, a wrong magic, a truncated header, and a header
    // claiming a colossal point count (the shape of an allocation attack).
    const std::vector<std::vector<uint8_t>> builtins = {
        {},
        {'N', 'O', 'T', 'S', 'P', 'Z'},
        {0x4e, 0x47, 0x53, 0x50},  // "NGSP" magic, then nothing
        {0x4e, 0x47, 0x53, 0x50, 0x03, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
         0xff},  // magic, version 3, count 0xffffffff
    };
    for (const auto& b : builtins) {
        exercise(b.data(), b.size());
        ++cases;
    }

    std::printf("spz fuzz replay: %d input(s) exercised without crash\n", cases);
    return 0;
}
#endif  // MELKOR_FUZZER_RUNTIME
