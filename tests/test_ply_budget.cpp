// Tests that the PLY reader enforces resource limits before allocating.
//
// A well-formed PLY whose header declares more vertices than the limits profile allows must be
// refused by policy (the Budget), not merely by the OS running out of memory. These write a valid
// PLY with PlyWriter, then read it back under tight and generous limits.
//
// Self-contained (no external test framework).

#include "melkor/gaussian_data.hpp"
#include "melkor/limits.hpp"
#include "melkor/ply_writer.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace melkor;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

GaussianCloud make_cloud(int n) {
    GaussianCloud cloud;
    for (int i = 0; i < n; ++i) {
        GaussianSplat s{};
        s.x = static_cast<float>(i);
        s.y = 0.0f;
        s.z = 0.0f;
        s.f_dc_0 = s.f_dc_1 = s.f_dc_2 = 0.0f;
        s.opacity = 0.5f;
        s.scale_0 = s.scale_1 = s.scale_2 = -3.0f;
        s.rot_0 = 1.0f;  // identity quaternion (w,x,y,z)
        s.rot_1 = s.rot_2 = s.rot_3 = 0.0f;
        cloud.addSplat(s);
    }
    return cloud;
}

}  // namespace

int main() {
    // A valid 3-splat PLY.
    PlyWriter writer;
    std::vector<std::uint8_t> buffer;
    auto written = writer.writeToBuffer(buffer, make_cloud(3));
    CHECK(written.success);
    CHECK(!buffer.empty());

    PlyReader reader;

    // Generous default limits accept it and decode all three splats.
    auto ok = reader.readFromBuffer(buffer.data(), buffer.size());
    CHECK(ok.success);
    CHECK(ok.cloud.size() == 3);

    // A splat limit below the declared count refuses it before reserving the cloud.
    Limits tight = Limits::for_profile(LimitsProfile::desktop);
    tight.max_splats = 2;
    auto rejected = reader.readFromBuffer(buffer.data(), buffer.size(), tight);
    CHECK(!rejected.success);

    // An input-size limit below the buffer refuses it before parsing the header.
    Limits tiny_input = Limits::for_profile(LimitsProfile::desktop);
    tiny_input.max_input_bytes = 8;  // far smaller than a real PLY
    auto rejected_input = reader.readFromBuffer(buffer.data(), buffer.size(), tiny_input);
    CHECK(!rejected_input.success);

    // A memory limit below the cloud's footprint refuses it before reserving.
    Limits tiny_mem = Limits::for_profile(LimitsProfile::desktop);
    tiny_mem.max_memory_bytes = 16;  // less than 3 * sizeof(GaussianSplat)
    auto rejected_mem = reader.readFromBuffer(buffer.data(), buffer.size(), tiny_mem);
    CHECK(!rejected_mem.success);

    // A header that never reaches end_header (a header bomb) is refused once it exceeds the
    // configured header-size limit, instead of scanning the whole buffer with an O(n^2) property
    // check.
    std::string header_bomb = "ply\n";
    for (int i = 0; i < 200; ++i) header_bomb += "comment padding to grow the header without end\n";
    Limits tiny_header = Limits::for_profile(LimitsProfile::desktop);
    tiny_header.max_ply_header_bytes = 64;
    auto rejected_header = reader.readFromBuffer(
        reinterpret_cast<const std::uint8_t*>(header_bomb.data()), header_bomb.size(), tiny_header);
    CHECK(!rejected_header.success);

    if (g_failures == 0) {
        std::printf("ply budget: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "ply budget: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
