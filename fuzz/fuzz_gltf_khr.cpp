// Coverage-guided fuzz target for the KHR_gaussian_splatting GLB reader.
//
// read_glb walks a long chain of attacker-controlled arithmetic -- GLB chunk lengths, glTF buffer
// and bufferView and accessor offsets, counts and strides, SH degree, and a whole JSON document --
// which is exactly where an out-of-bounds read or an unbounded allocation would hide. This feeds
// arbitrary bytes to read_glb and asserts it only ever returns a clean success or a clean failure,
// never a crash. It is the highest-value glTF target because it exercises the entire read path
// through one entry point.
//
// Builds two ways, like the other targets: a libFuzzer entry point for CI, and a standalone
// corpus-replay driver that runs the seed corpus (including a valid GLB) everywhere.

#include "melkor/format/gltf_reader.hpp"

#include <cstddef>
#include <cstdint>

namespace {

void exercise(const uint8_t* data, size_t size) {
    auto result = melkor::format::gltf::read_glb(data, size);
    if (!result.has_value()) {
        return;
    }
    // Touch the produced scene so a sanitizer build observes any bad memory the reader set up.
    const auto& splats = result.value().data;
    volatile float sink = 0.0f;
    for (std::size_t i = 0; i < splats.size(); ++i) {
        sink += splats.positions()[i].x + splats.opacities()[i];
    }
    (void)sink;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    exercise(data, size);
    return 0;
}

#ifndef MELKOR_FUZZER_RUNTIME
#include "melkor/format/glb_container.hpp"

#include "replay.hpp"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    int cases = 0;
    if (!melkor::fuzzing::replay_requested_inputs(argc, argv, exercise, cases)) return 1;

    // Built-in framing failures: empty, wrong magic, and a GLB header with a lying total length.
    const std::vector<std::vector<uint8_t>> builtins = {
        {},
        {'n', 'o', 'p', 'e'},
        // "glTF" magic, version 2, total length lying as 0xffffffff.
        {0x67, 0x6c, 0x54, 0x46, 0x02, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff},
    };
    for (const auto& b : builtins) {
        exercise(b.data(), b.size());
        ++cases;
    }

    // This is a correctly framed GLB whose complete KHR primitive declares UINT32_MAX elements
    // in every accessor while providing only one byte of BIN data. It reaches accessor-span
    // validation and exercises the huge-count arithmetic without allocating the claimed shape.
    const std::string huge_count_json =
        R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_gaussian_splatting"],"buffers":[{"byteLength":1}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":1}],"accessors":[{"bufferView":0,"componentType":5126,"count":4294967295,"type":"VEC3"},{"bufferView":0,"componentType":5126,"count":4294967295,"type":"VEC4"},{"bufferView":0,"componentType":5126,"count":4294967295,"type":"VEC3"},{"bufferView":0,"componentType":5126,"count":4294967295,"type":"SCALAR"},{"bufferView":0,"componentType":5126,"count":4294967295,"type":"VEC3"}],"meshes":[{"primitives":[{"mode":0,"attributes":{"POSITION":0,"KHR_gaussian_splatting:ROTATION":1,"KHR_gaussian_splatting:SCALE":2,"KHR_gaussian_splatting:OPACITY":3,"KHR_gaussian_splatting:SH_DEGREE_0_COEF_0":4},"extensions":{"KHR_gaussian_splatting":{"kernel":"ellipse","colorSpace":"srgb_rec709_display"}}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    const std::uint8_t one_byte_bin = 0;
    auto huge_count_glb = melkor::format::glb::build_glb(
        huge_count_json, &one_byte_bin, sizeof(one_byte_bin));
    if (!huge_count_glb.has_value()) {
        std::fprintf(stderr, "gltf_khr fuzz replay: failed to build huge-count input\n");
        return 1;
    }
    exercise(huge_count_glb.value().data(), huge_count_glb.value().size());
    ++cases;

    std::printf("gltf_khr fuzz replay: %d input(s) exercised without crash\n", cases);
    return 0;
}
#endif  // MELKOR_FUZZER_RUNTIME
