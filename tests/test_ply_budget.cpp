// Tests that the PLY reader enforces resource limits before allocation.
//
// Self-contained (no external test framework).

#include "melkor/limits.hpp"
#include "melkor/ply_writer.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
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

bool mentions_limit(const std::string& message) {
    return message.find("limit") != std::string::npos;
}

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

PlyWriteConfig write_config() {
    PlyWriteConfig config;
    config.color_space = ColorSpace::lin_rec709_display;
    config.antialiased = false;
    return config;
}

std::size_t header_bytes(const std::vector<std::uint8_t>& bytes) {
    const std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::size_t marker = view.find("end_header\n");
    return marker == std::string_view::npos ? 0 : marker + std::string_view("end_header\n").size();
}

}  // namespace

int main() {
    constexpr std::uint64_t kDegreeZeroCanonicalBytes =
        2 * sizeof(Vec3f) + sizeof(Quatf) + sizeof(float) + 3 * sizeof(float);

    // A valid 3-splat PLY.
    PlyWriter writer;
    std::vector<std::uint8_t> buffer;
    auto written = writer.writeToBuffer(buffer, make_data(3), write_config());
    CHECK(written.success);
    CHECK(!buffer.empty());

    Budget write_budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext write_context = make_default_context(write_budget);
    std::vector<std::uint8_t> accounted_buffer;
    {
        auto accounted_write =
            writer.writeToBuffer(accounted_buffer, make_data(3), write_config(), write_context);
        CHECK(accounted_write.success);
        CHECK(accounted_write.retained_memory_bytes() == accounted_buffer.capacity());
        CHECK(write_budget.used(BudgetKind::memory_bytes) ==
              accounted_write.retained_memory_bytes());
    }
    CHECK(write_budget.used(BudgetKind::memory_bytes) == 0);

    constexpr std::uint64_t kWriterHeaderWorkingBytes = 3U * 8U * 1024U;
    Limits tight_header_write = Limits::for_profile(LimitsProfile::desktop);
    tight_header_write.max_memory_bytes = kWriterHeaderWorkingBytes - 1;
    Budget tight_header_budget(tight_header_write);
    OperationContext tight_header_context = make_default_context(tight_header_budget);
    std::ostringstream refused_stream;
    const auto refused_header =
        writer.writeToStream(refused_stream, make_data(0), write_config(), tight_header_context);
    CHECK(!refused_header.success);
    CHECK(refused_header.error_code() == ErrorCode::resource_limit);
    CHECK(refused_stream.str().empty());

    tight_header_write.max_memory_bytes = kWriterHeaderWorkingBytes;
    Budget exact_header_budget(tight_header_write);
    OperationContext exact_header_context = make_default_context(exact_header_budget);
    std::ostringstream accepted_stream;
    const auto accepted_header =
        writer.writeToStream(accepted_stream, make_data(0), write_config(), exact_header_context);
    CHECK(accepted_header.success);
    CHECK(exact_header_budget.used(BudgetKind::memory_bytes) == 0);

    PlyWriteConfig tight_write = write_config();
    tight_write.limits = Limits::for_profile(LimitsProfile::desktop);
    tight_write.limits.max_splats = 2;
    std::vector<std::uint8_t> refused_output = {1, 2, 3};
    auto refused_write = writer.writeToBuffer(refused_output, make_data(3), tight_write);
    CHECK(!refused_write.success);
    CHECK(mentions_limit(refused_write.error_message));
    CHECK(refused_output.empty());

    PlyWriteConfig tiny_output = write_config();
    tiny_output.limits = Limits::for_profile(LimitsProfile::desktop);
    tiny_output.limits.max_temp_bytes = 16;
    auto refused_output_size = writer.writeToBuffer(refused_output, make_data(3), tiny_output);
    CHECK(refused_output_size.success);
    CHECK(!refused_output.empty());

    PlyReader reader;

    // Generous default limits accept it and decode all three splats.
    auto ok = reader.readFromBuffer(buffer.data(), buffer.size());
    CHECK(ok.success);
    CHECK(ok.data.has_value() && ok.data->size() == 3);

    Budget read_budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext read_context = make_default_context(read_budget);
    {
        auto accounted_read = reader.readFromBuffer(buffer.data(), buffer.size(), read_context);
        CHECK(accounted_read.success);
        CHECK(accounted_read.retained_memory_bytes() == 3 * kDegreeZeroCanonicalBytes);
        CHECK(read_budget.used(BudgetKind::memory_bytes) == accounted_read.retained_memory_bytes());
    }
    CHECK(read_budget.used(BudgetKind::memory_bytes) == 0);

    const std::size_t binary_header_bytes = header_bytes(buffer);
    CHECK(binary_header_bytes != 0);
    constexpr std::uint64_t kHeaderWorkingBase = 16U * 1024U;
    constexpr std::uint64_t kHeaderWorkingMultiplier = 12;
    const std::uint64_t binary_header_working =
        binary_header_bytes * kHeaderWorkingMultiplier + kHeaderWorkingBase;
    const std::uint64_t exact_binary_memory =
        buffer.size() + binary_header_working + 3 * kDegreeZeroCanonicalBytes;
    Limits cumulative_memory = Limits::for_profile(LimitsProfile::desktop);
    cumulative_memory.max_memory_bytes = exact_binary_memory - 1;
    const auto rejected_cumulative =
        reader.readFromBuffer(buffer.data(), buffer.size(), cumulative_memory);
    CHECK(!rejected_cumulative.success);
    CHECK(rejected_cumulative.error_code() == ErrorCode::resource_limit);
    cumulative_memory.max_memory_bytes = exact_binary_memory;
    const auto accepted_cumulative =
        reader.readFromBuffer(buffer.data(), buffer.size(), cumulative_memory);
    CHECK(accepted_cumulative.success);

    // A splat limit below the declared count refuses it before reserving the cloud.
    Limits tight = Limits::for_profile(LimitsProfile::desktop);
    tight.max_splats = 2;
    auto rejected = reader.readFromBuffer(buffer.data(), buffer.size(), tight);
    CHECK(!rejected.success);
    CHECK(rejected.error_code() == ErrorCode::resource_limit);
    CHECK(mentions_limit(rejected.error_message));

    // An input-size limit below the buffer refuses it before parsing the header.
    Limits tiny_input = Limits::for_profile(LimitsProfile::desktop);
    tiny_input.max_input_bytes = 8;  // far smaller than a real PLY
    auto rejected_input = reader.readFromBuffer(buffer.data(), buffer.size(), tiny_input);
    CHECK(!rejected_input.success);
    CHECK(rejected_input.error_code() == ErrorCode::resource_limit);
    CHECK(mentions_limit(rejected_input.error_message));

    // A memory limit below the cloud's footprint refuses it before reserving.
    Limits tiny_mem = Limits::for_profile(LimitsProfile::desktop);
    tiny_mem.max_memory_bytes = 16;  // less than three canonical splat records
    auto rejected_mem = reader.readFromBuffer(buffer.data(), buffer.size(), tiny_mem);
    CHECK(!rejected_mem.success);
    CHECK(rejected_mem.error_code() == ErrorCode::resource_limit);
    CHECK(mentions_limit(rejected_mem.error_message));

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
    CHECK(rejected_header.error_code() == ErrorCode::resource_limit);
    CHECK(mentions_limit(rejected_header.error_message));

    // Reject one long line before the reader creates a full-line string.
    const std::string long_line = "ply\ncomment " + std::string(256, 'x') +
                                  "\nformat ascii 1.0\nelement vertex 0\nend_header\n";
    auto rejected_long_line = reader.readFromBuffer(
        reinterpret_cast<const std::uint8_t*>(long_line.data()), long_line.size(), tiny_header);
    CHECK(!rejected_long_line.success);
    CHECK(rejected_long_line.error_code() == ErrorCode::resource_limit);
    CHECK(mentions_limit(rejected_long_line.error_message));

    // Parse ASCII records without a copy of the complete payload.
    constexpr int kAsciiPoints = 128;
    PlyWriteConfig ascii_config = write_config();
    ascii_config.format = PlyFormat::Ascii;
    std::vector<std::uint8_t> ascii_buffer;
    const auto ascii_written =
        writer.writeToBuffer(ascii_buffer, make_data(kAsciiPoints), ascii_config);
    CHECK(ascii_written.success);
    const std::size_t ascii_header_bytes = header_bytes(ascii_buffer);
    CHECK(ascii_header_bytes != 0);
    const std::uint64_t ascii_header_working =
        ascii_header_bytes * kHeaderWorkingMultiplier + kHeaderWorkingBase;
    constexpr std::uint64_t kAsciiPropertyBytes = 14 * sizeof(float);
    Limits ascii_memory = Limits::for_profile(LimitsProfile::desktop);
    ascii_memory.max_memory_bytes = ascii_buffer.size() + ascii_header_working +
                                    kAsciiPoints * kDegreeZeroCanonicalBytes + kAsciiPropertyBytes;
    Limits tight_ascii_memory = ascii_memory;
    --tight_ascii_memory.max_memory_bytes;
    const auto rejected_ascii =
        reader.readFromBuffer(ascii_buffer.data(), ascii_buffer.size(), tight_ascii_memory);
    CHECK(!rejected_ascii.success);
    CHECK(rejected_ascii.error_code() == ErrorCode::resource_limit);
    const auto accepted_ascii =
        reader.readFromBuffer(ascii_buffer.data(), ascii_buffer.size(), ascii_memory);
    CHECK(accepted_ascii.success);
    CHECK(accepted_ascii.data.has_value() && accepted_ascii.data->size() == kAsciiPoints);

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("melkor-ply-budget-" + std::to_string(nonce) + ".ply");
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() /
        ("melkor-ply-output-budget-" + std::to_string(nonce) + ".ply");
    const auto refused_file = writer.writeToFile(output_path.string(), make_data(3), tiny_output);
    CHECK(!refused_file.success);
    CHECK(mentions_limit(refused_file.error_message));
    CHECK(!std::filesystem::exists(output_path));
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
    }
    Limits file_memory = Limits::for_profile(LimitsProfile::desktop);
    file_memory.max_memory_bytes = buffer.size() - 1;
    const auto rejected_file = reader.readFromFile(path, file_memory);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    CHECK(!rejected_file.success);
    CHECK(rejected_file.error_code() == ErrorCode::resource_limit);
    CHECK(mentions_limit(rejected_file.error_message));

    // An all-zero profile is invalid. It must not disable every limit.
    const Limits invalid = Limits::for_profile(LimitsProfile::custom);
    const auto rejected_invalid = reader.readFromBuffer(buffer.data(), buffer.size(), invalid);
    CHECK(!rejected_invalid.success);
    CHECK(rejected_invalid.error_code() == ErrorCode::invalid_argument);
    CHECK(mentions_limit(rejected_invalid.error_message));

    std::string invalid_name("input.ply\0hidden", 16);
    const auto invalid_path = reader.readFromFile(std::filesystem::path(invalid_name));
    CHECK(!invalid_path.success);
    CHECK(invalid_path.error_code() == ErrorCode::invalid_argument);

    if (g_failures == 0) {
        std::printf("ply budget: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "ply budget: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
