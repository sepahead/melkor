#include "melkor/format/registry.hpp"

#include "melkor/format/gltf_writer.hpp"
#include "melkor/ply_writer.hpp"
#include "melkor/spz_encoder.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace {

using namespace melkor;
namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

class TempDirectory {
public:
    TempDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("melkor-registry-" + std::to_string(suffix));
        fs::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

SplatData sample_data() {
    SplatBufferInput input;
    input.positions = {{1.0f, 2.0f, 3.0f}, {-1.0f, 0.5f, 2.0f}};
    input.scales = {{0.1f, 0.2f, 0.3f}, {0.3f, 0.2f, 0.1f}};
    input.rotations = {Quatf{}, Quatf{0.0f, 0.0f, 0.0f, 1.0f}};
    input.opacities = {0.5f, 0.75f};
    input.sh = ShBuffer::create(1, 2, std::vector<float>(24, 0.0f)).value();
    return SplatData::create(std::move(input)).value();
}

bool write_bytes(const fs::path& path, const std::uint8_t* data, std::size_t size) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    if (size != 0)
        stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return stream.good();
}

bool write_glb_fixture(const fs::path& path) {
    auto encoded = format::gltf::write_glb(sample_data(), ColorSpace::srgb_rec709_display);
    return encoded.has_value() &&
           write_bytes(path, encoded.value().bytes.data(), encoded.value().bytes.size());
}

Result<ConversionResult> convert(const ConversionRequest& request) {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    return convert_file(request, context);
}

Result<ConversionResult> convert(const ConversionRequest& request,
                                 const ConversionCommitGate& before_commit) {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    return convert_file(request, context, before_commit);
}

Result<AssetReadResult> read_asset(const fs::path& path, const AssetReadOptions& options = {}) {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    return read_splat_asset(path, options, context);
}

void test_glb_ply_glb_registry_path() {
    TempDirectory directory;
    const fs::path input = directory.path() / "source.glb";
    const fs::path ply = directory.path() / "normalized.ply";
    const fs::path output = directory.path() / "roundtrip.glb";
    CHECK(write_glb_fixture(input));

    ConversionRequest to_ply;
    to_ply.input = input;
    to_ply.output = ply;
    auto converted = convert(to_ply);
    CHECK(converted.has_value());
    if (converted.has_value()) {
        CHECK(converted.value().input_format == FormatId::glb);
        CHECK(converted.value().output_format == FormatId::ply);
        CHECK(converted.value().output_profile == FormatProfileId::ply_melkor_canonical_v1);
        CHECK(converted.value().splat_count == 2);
        CHECK(converted.value().bytes_written == fs::file_size(ply));
    }

    auto normalized = read_asset(ply);
    CHECK(normalized.has_value());
    if (normalized.has_value()) {
        CHECK(normalized.value().primitive.data().size() == 2);
        CHECK(normalized.value().primitive.metadata().color_space ==
              ColorSpace::srgb_rec709_display);
        CHECK(!normalized.value().primitive.metadata().antialiased.has_value());
    }

    ConversionRequest to_glb;
    to_glb.input = ply;
    to_glb.output = output;
    converted = convert(to_glb);
    CHECK(converted.has_value());
    auto roundtrip = read_asset(output);
    CHECK(roundtrip.has_value());
    if (roundtrip.has_value()) {
        CHECK(roundtrip.value().primitive.data().size() == 2);
        CHECK(roundtrip.value().primitive.data().sh().degree() == 1);
    }
}

void test_probe_override_and_same_file_guards() {
    TempDirectory directory;
    const fs::path wrong_suffix = directory.path() / "source.ply";
    CHECK(write_glb_fixture(wrong_suffix));
    auto rejected = read_asset(wrong_suffix);
    CHECK(!rejected.has_value());
    CHECK(rejected.error_code() == ErrorCode::invalid_data);

    AssetReadOptions override;
    override.format = FormatId::glb;
    auto accepted = read_asset(wrong_suffix, override);
    CHECK(accepted.has_value());

    ConversionRequest same;
    same.input = wrong_suffix;
    same.output = wrong_suffix;
    same.read.format = FormatId::glb;
    same.write.format = FormatId::glb;
    same.write.overwrite = true;
    auto same_result = convert(same);
    CHECK(!same_result.has_value());
    CHECK(same_result.error_code() == ErrorCode::invalid_argument);
}

void test_probe_accepts_content_without_a_known_suffix() {
    TempDirectory directory;
    const fs::path suffixless = directory.path() / "source";
    const fs::path unknown_suffix = directory.path() / "source.asset";
    const fs::path uppercase_suffix = directory.path() / "source.GLB";
    CHECK(write_glb_fixture(suffixless));
    CHECK(write_glb_fixture(unknown_suffix));
    CHECK(write_glb_fixture(uppercase_suffix));

    auto suffixless_result = read_asset(suffixless);
    CHECK(suffixless_result.has_value());
    if (suffixless_result.has_value())
        CHECK(suffixless_result.value().format == FormatId::glb);

    auto unknown_suffix_result = read_asset(unknown_suffix);
    CHECK(unknown_suffix_result.has_value());
    if (unknown_suffix_result.has_value())
        CHECK(unknown_suffix_result.value().format == FormatId::glb);

    auto uppercase_suffix_result = read_asset(uppercase_suffix);
    CHECK(uppercase_suffix_result.has_value());
    if (uppercase_suffix_result.has_value())
        CHECK(uppercase_suffix_result.value().format == FormatId::glb);
}

void test_invalid_options_fail_before_file_access() {
    TempDirectory directory;
    const fs::path missing = directory.path() / "missing.ply";

    AssetReadOptions read_options;
    read_options.format = FormatId::unknown;
    auto invalid_read_format = read_asset(missing, read_options);
    CHECK(!invalid_read_format.has_value());
    CHECK(invalid_read_format.error_code() == ErrorCode::invalid_argument);
    if (!invalid_read_format.diagnostics().empty())
        CHECK(invalid_read_format.diagnostics()[0].code == "MK1735_FORMAT_ID_INVALID");

    read_options.format = static_cast<FormatId>(999);
    auto out_of_range_read_format = read_asset(missing, read_options);
    CHECK(!out_of_range_read_format.has_value());
    CHECK(out_of_range_read_format.error_code() == ErrorCode::invalid_argument);

    read_options.format.reset();
    read_options.profile = static_cast<FormatProfileId>(255);
    auto invalid_read_profile = read_asset(missing, read_options);
    CHECK(!invalid_read_profile.has_value());
    CHECK(invalid_read_profile.error_code() == ErrorCode::invalid_argument);
    if (!invalid_read_profile.diagnostics().empty())
        CHECK(invalid_read_profile.diagnostics()[0].code == "MK1736_PROFILE_ID_INVALID");

    ConversionRequest request;
    request.input = missing;
    request.output = directory.path() / "output.ply";
    request.write.approved_loss_codes = {"LOSS_NOT_REGISTERED"};
    auto invalid_approval = convert(request);
    CHECK(!invalid_approval.has_value());
    CHECK(invalid_approval.error_code() == ErrorCode::invalid_argument);
    if (!invalid_approval.diagnostics().empty())
        CHECK(invalid_approval.diagnostics()[0].code == "MK1604_INVALID_LOSS_APPROVAL");

    request.write.approved_loss_codes.clear();
    request.write.format = static_cast<FormatId>(999);
    auto invalid_write_format = convert(request);
    CHECK(!invalid_write_format.has_value());
    CHECK(invalid_write_format.error_code() == ErrorCode::invalid_argument);
    if (!invalid_write_format.diagnostics().empty())
        CHECK(invalid_write_format.diagnostics()[0].code == "MK1735_FORMAT_ID_INVALID");

    request.write.format = FormatId::ply;
    request.write.profile = static_cast<FormatProfileId>(255);
    auto invalid_write_profile = convert(request);
    CHECK(!invalid_write_profile.has_value());
    CHECK(invalid_write_profile.error_code() == ErrorCode::invalid_argument);
    if (!invalid_write_profile.diagnostics().empty())
        CHECK(invalid_write_profile.diagnostics()[0].code == "MK1736_PROFILE_ID_INVALID");
}

void test_input_symlink_is_rejected_before_probe() {
    TempDirectory directory;
    const fs::path target = directory.path() / "target.ply";
    const fs::path link = directory.path() / "link.ply";
    PlyWriteConfig config;
    config.color_space = ColorSpace::srgb_rec709_display;
    auto written = PlyWriter{}.writeToFile(target.string(), sample_data(), config);
    CHECK(written.success);
    if (!written.success)
        return;

    std::error_code error;
    fs::create_symlink(target.filename(), link, error);
    if (error)
        return;

    auto result = read_asset(link);
    CHECK(!result.has_value());
    CHECK(result.error_code() == ErrorCode::io_error);
    CHECK(!result.diagnostics().empty());
    if (!result.diagnostics().empty()) {
        CHECK(result.diagnostics()[0].code == "MK2302_INPUT_TYPE");
        CHECK(result.diagnostics()[0].path == "link.ply");
    }
}

#if defined(__unix__) || defined(__APPLE__)
class AppendDuringRead final : public ProgressSink {
public:
    explicit AppendDuringRead(fs::path path) : path_(std::move(path)) {}

    void on_progress(const ProgressEvent& event) override {
        if (changed_ || event.operation != "ply.read" || event.phase != "header_file" ||
            event.completed != 0) {
            return;
        }
        std::ofstream stream(path_, std::ios::binary | std::ios::app);
        stream.put('x');
        stream.flush();
        changed_ = stream.good();
    }

    bool changed() const noexcept { return changed_; }

private:
    fs::path path_;
    bool changed_ = false;
};

void test_nonregular_and_changed_inputs_are_rejected() {
    TempDirectory directory;
    const fs::path fifo = directory.path() / "stream.ply";
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    auto fifo_result = read_asset(fifo);
    CHECK(!fifo_result.has_value());
    CHECK(fifo_result.error_code() == ErrorCode::io_error);
    if (!fifo_result.diagnostics().empty())
        CHECK(fifo_result.diagnostics()[0].code == "MK2302_INPUT_TYPE");

    const fs::path changing = directory.path() / "changing.ply";
    PlyWriteConfig config;
    config.color_space = ColorSpace::srgb_rec709_display;
    auto written = PlyWriter{}.writeToFile(changing.string(), sample_data(), config);
    CHECK(written.success);
    if (!written.success)
        return;

    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    Budget budget(limits);
    OperationContext context = make_default_context(budget);
    AppendDuringRead mutation(changing);
    context.progress = &mutation;
    auto changed = PlyReader{}.readFromFile(changing, context);
    CHECK(mutation.changed());
    CHECK(!changed.success);
    CHECK(changed.error_code() == ErrorCode::io_error);
    CHECK(!changed.diagnostics.empty());
    if (!changed.diagnostics.empty())
        CHECK(changed.diagnostics[0].code == "MK2305_INPUT_CHANGED");
}
#endif

void test_antialias_loss_blocks_glb_commit() {
    TempDirectory directory;
    const fs::path input = directory.path() / "known-antialias.ply";
    const fs::path output = directory.path() / "output.glb";
    PlyWriteConfig config;
    config.color_space = ColorSpace::lin_rec709_display;
    config.antialiased = true;
    PlyWriter writer;
    auto fixture = writer.writeToFile(input.string(), sample_data(), config);
    CHECK(fixture.success);

    ConversionRequest request;
    request.input = input;
    request.output = output;
    auto blocked = convert(request);
    CHECK(!blocked.has_value());
    CHECK(blocked.error_code() == ErrorCode::unsupported_feature);
    CHECK(!fs::exists(output));

    request.write.approved_loss_codes = {loss_code::kAntialiasingMetadataDropped};
    auto approved = convert(request);
    CHECK(approved.has_value());
    CHECK(fs::is_regular_file(output));
    if (approved.has_value()) {
        bool found = false;
        for (const LossItem& item : approved.value().losses.items()) {
            found = found || item.code == loss_code::kAntialiasingMetadataDropped;
        }
        CHECK(found);
    }
}

void test_spz_requires_explicit_semantics_and_antialiasing() {
    TempDirectory directory;
    const fs::path fake_spz = directory.path() / "fake.spz";
    const std::uint8_t gzip[] = {0x1f, 0x8b};
    CHECK(write_bytes(fake_spz, gzip, sizeof(gzip)));
    auto missing_semantics = read_asset(fake_spz);
    CHECK(!missing_semantics.has_value());
    CHECK(missing_semantics.error_code() == ErrorCode::invalid_argument);

    if (!isSpzAvailable())
        return;

    const fs::path input = directory.path() / "source.glb";
    const fs::path output = directory.path() / "output.spz";
    CHECK(write_glb_fixture(input));
    ConversionRequest request;
    request.input = input;
    request.output = output;
    auto missing_antialias = convert(request);
    CHECK(!missing_antialias.has_value());
    CHECK(missing_antialias.error_code() == ErrorCode::invalid_argument);
    CHECK(!fs::exists(output));

    request.write.antialiased = false;
    request.write.approved_loss_codes = {loss_code::kColorSpaceMetadataDropped,
                                         loss_code::kCoordinateMetadataDropped};
    auto encoded = convert(request);
    CHECK(encoded.has_value());
    CHECK(fs::is_regular_file(output));

    AssetReadOptions options;
    options.source_unit_to_meter = 1.0;
    options.source_color_space = ColorSpace::srgb_rec709_display;
    options.source_frame_id = "custom-frame";
    auto frame_override = read_asset(output, options);
    CHECK(!frame_override.has_value());
    CHECK(frame_override.error_code() == ErrorCode::invalid_argument);
    if (!frame_override.diagnostics().empty())
        CHECK(frame_override.diagnostics()[0].code == "MK1732_SPZ_FRAME_FIXED");

    options.source_frame_id.reset();
    auto decoded = read_asset(output, options);
    CHECK(decoded.has_value());
    if (decoded.has_value()) {
        CHECK(decoded.value().primitive.data().size() == 2);
        CHECK(decoded.value().primitive.metadata().antialiased == false);
    }
}

void test_failed_commit_gate_preserves_destination() {
    TempDirectory directory;
    const fs::path input = directory.path() / "source.glb";
    const fs::path output = directory.path() / "output.glb";
    CHECK(write_glb_fixture(input));

    ConversionRequest request;
    request.input = input;
    request.output = output;
    int calls = 0;
    const ConversionCommitGate reject = [&](const ConversionResult& result) {
        ++calls;
        CHECK(result.input_format == FormatId::glb);
        CHECK(result.output_format == FormatId::glb);
        CHECK(result.splat_count == 2);
        CHECK(result.bytes_written > 0);
        return Result<void>::failure(ErrorCode::io_error,
                                     Diagnostic("MK9999_TEST_COMMIT_GATE", Severity::error,
                                                "the test commit gate rejected the output"));
    };

    auto fresh = convert(request, reject);
    CHECK(!fresh.has_value());
    CHECK(fresh.error_code() == ErrorCode::io_error);
    CHECK(!fs::exists(output));

    {
        std::ofstream stream(output, std::ios::binary);
        stream << "existing";
        CHECK(stream.good());
    }
    request.write.overwrite = true;
    auto overwrite = convert(request, reject);
    CHECK(!overwrite.has_value());
    CHECK(overwrite.error_code() == ErrorCode::io_error);
    CHECK(fs::is_regular_file(output));
    std::ifstream preserved(output, std::ios::binary);
    std::string contents;
    preserved >> contents;
    CHECK(contents == "existing");
    CHECK(calls == 2);
}

}  // namespace

int main() {
    test_glb_ply_glb_registry_path();
    test_probe_override_and_same_file_guards();
    test_probe_accepts_content_without_a_known_suffix();
    test_invalid_options_fail_before_file_access();
    test_input_symlink_is_rejected_before_probe();
#if defined(__unix__) || defined(__APPLE__)
    test_nonregular_and_changed_inputs_are_rejected();
#endif
    test_antialias_loss_blocks_glb_commit();
    test_spz_requires_explicit_semantics_and_antialiasing();
    test_failed_commit_gate_preserves_destination();

    if (g_failures == 0) {
        std::printf("format registry: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "format registry: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
