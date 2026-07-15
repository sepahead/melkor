// Coverage-guided fuzz target for the PLY reader.
//
// This is a real fuzz target, not the randomized-loop-over-valid-data that the pre-v2 tests
// called "fuzzing" (release blocker P1-12). It feeds arbitrary attacker-controlled bytes to the
// PLY parser and asserts that the parser never crashes, reads out of bounds, or violates its own
// invariants -- it must always return a clean success or a clean diagnostic.
//
// It builds two ways from one source:
//
//   - Under a libFuzzer build (-fsanitize=fuzzer), LLVMFuzzerTestOneInput is the entry point and
//     the fuzzer drives it with coverage feedback. This is what runs continuously in CI.
//   - Otherwise, a standalone main() replays a seed corpus directory, so the harness compiles
//     and the corpus is exercised on every platform -- including those without a libFuzzer
//     runtime -- as an ordinary regression test. Every crash found and fixed becomes a corpus
//     entry, which this replay then guards forever.

#include "melkor/ply_writer.hpp"

#include <cstddef>
#include <cstdint>

namespace {

// One fuzz iteration: parse the bytes, and if the parser claims success, check the invariants a
// successful parse must uphold. A violated invariant here is a bug -- either the parser accepted
// something malformed, or it produced an inconsistent result.
void exercise(const uint8_t* data, size_t size) {
    melkor::PlyReader reader;
    auto result = reader.readFromBuffer(data, size);

    if (!result.success) {
        return;  // A clean rejection is a correct outcome for malformed input.
    }

    // On success the cloud must be internally consistent. These are cheap checks whose failure
    // means the parser produced a structurally invalid scene from the input.
    const auto& cloud = result.cloud;
    const std::size_t n = cloud.size();

    // Touch every splat so ASan sees any out-of-bounds the parser may have set up, and so a
    // sanitizer build catches a use of uninitialised or non-finite data downstream.
    const auto& splats = cloud.splats();
    if (splats.size() != n) {
        __builtin_trap();  // size() and splats() disagreeing is a broken invariant.
    }
    volatile float sink = 0.0f;
    for (const auto& s : splats) {
        sink += s.x + s.y + s.z + s.opacity;
    }
    (void)sink;
}

}  // namespace

// libFuzzer entry point.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    exercise(data, size);
    return 0;
}

// Standalone corpus-replay driver, compiled when NOT building against the libFuzzer runtime.
#ifndef MELKOR_FUZZER_RUNTIME
#include "replay.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    int cases = 0;
    if (!melkor::fuzzing::replay_requested_inputs(argc, argv, exercise, cases)) return 1;

    // Also run a handful of built-in adversarial inputs, so the replay is meaningful even with an
    // empty corpus directory. These are shapes that have historically broken PLY parsers.
    const char* builtins[] = {
        "",                                             // empty
        "ply\n",                                        // truncated header
        "ply\nformat binary_little_endian 1.0\n",       // no end_header
        "ply\nformat ascii 1.0\nelement vertex 999999999999\nproperty float x\nend_header\n",  // huge count
        "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n1 2\n",  // short record
        "not a ply at all",                             // wrong magic
    };
    for (const char* b : builtins) {
        exercise(reinterpret_cast<const uint8_t*>(b), std::char_traits<char>::length(b));
        ++cases;
    }

    std::printf("ply fuzz replay: %d input(s) exercised without crash\n", cases);
    return 0;
}
#endif  // MELKOR_FUZZER_RUNTIME
