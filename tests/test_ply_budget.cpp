// Tests that the PLY reader enforces resource limits before allocating.
//
// A well-formed PLY whose header declares more vertices than the limits profile allows must be
// refused by policy (the Budget), not merely by the OS running out of memory. These write a valid
// PLY with PlyWriter, then read it back under tight and generous limits.
//
// Self-contained (no external test framework).

#include "melkor/limits.hpp"
#include "melkor/ply_writer.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

SplatData make_data(int n) {
    SplatBufferInput input;
    input.positions.reserve(static_cast<std::size_t>(n));
    input.scales.reserve(static_cast<std::size_t>(n));
    input.rotations.reserve(static_cast<std::size_t>(n));
    input.opacities.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        input.positions.push_back({static_cast<float>(i), 0.0f, 0.0f});
        input.scales.push_back({0.05f, 0.05f, 0.05f});
        input.rotations.push_back({});
        input.opacities.push_back(0.5f);
    }
    input.sh = ShBuffer::black(static_cast<std::size_t>(n)).value();
    return SplatData::create(std::move(input)).value();
}

}  // namespace

int main() {
    // A valid 3-splat PLY.
    PlyWriter writer;
    std::vector<std::uint8_t> buffer;
    auto written = writer.writeToBuffer(buffer, make_data(3));
    CHECK(written.success);
    CHECK(!buffer.empty());

    PlyReader reader;

    // Generous default limits accept it and decode all three splats.
    auto ok = reader.readFromBuffer(buffer.data(), buffer.size());
    CHECK(ok.success);
    CHECK(ok.data.has_value() && ok.data->size() == 3);

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
    tiny_mem.max_memory_bytes = 16;  // less than three canonical splat records
    auto rejected_mem = reader.readFromBuffer(buffer.data(), buffer.size(), tiny_mem);
    CHECK(!rejected_mem.success);

    // A header that never reaches end_header (a header bomb) is refused once it exceeds the
    // configured header-size limit, instead of scanning the whole buffer with an O(n^2) property
    // check.
    std::string header_bomb = "ply\n";
    for (int i = 0; i < 200; ++i)
        header_bomb += "comment padding to grow the header without end\n";
    Limits tiny_header = Limits::for_profile(LimitsProfile::desktop);
    tiny_header.max_ply_header_bytes = 64;
    auto rejected_header = reader.readFromBuffer(
        reinterpret_cast<const std::uint8_t*>(header_bomb.data()), header_bomb.size(), tiny_header);
    CHECK(!rejected_header.success);

    // Zero means unlimited for every Limits field. The file preflight must not substitute an
    // undocumented 1 MiB cap while the in-memory path correctly permits the same header.
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path large_header_path =
        std::filesystem::temp_directory_path() /
        ("melkor-ply-unlimited-header-" + std::to_string(nonce) + ".ply");
    {
        std::ofstream file(large_header_path, std::ios::binary | std::ios::trunc);
        file << "ply\nformat ascii 1.0\nelement vertex 0\ncomment ";
        file << std::string(1024 * 1024 + 64, 'x');
        file << "\nend_header\n";
    }
    Limits unlimited_header = Limits::for_profile(LimitsProfile::desktop);
    unlimited_header.max_ply_header_bytes = 0;
    const auto accepted_large_header =
        reader.readFromFile(large_header_path.string(), unlimited_header);
    std::error_code remove_error;
    std::filesystem::remove(large_header_path, remove_error);
    CHECK(accepted_large_header.success && accepted_large_header.data.has_value() &&
          accepted_large_header.data->empty());

    if (g_failures == 0) {
        std::printf("ply budget: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "ply budget: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
