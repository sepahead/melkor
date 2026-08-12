// Tests that the SPZ decoder checks all declared sizes before upstream decoding.
//
// Self-contained (no external test framework).

#include "melkor/limits.hpp"
#include "melkor/spz_encoder.hpp"

#include <cstdio>

#ifdef MELKOR_HAS_SPZ
#include "load-spz.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include <zlib.h>

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

class CountingProgress final : public ProgressSink {
public:
    void on_progress(const ProgressEvent&) override { ++events; }

    std::size_t events = 0;
};

class CancelDuringInflate final : public ProgressSink {
public:
    explicit CancelDuringInflate(CancellationToken token) : token_(std::move(token)) {}

    void on_progress(const ProgressEvent& event) override {
        if (event.phase == "inflate" && event.completed != 0)
            token_.cancel();
    }

private:
    CancellationToken token_;
};

SpzEncodeConfig encode_config() {
    SpzEncodeConfig config;
    config.color_space = ColorSpace::lin_rec709_display;
    config.antialiased = false;
    config.approved_loss_codes = {loss_code::kColorSpaceMetadataDropped,
                                  loss_code::kCoordinateMetadataDropped};
    return config;
}

SpzDecodeConfig decode_config(const Limits& limits = Limits::for_profile(LimitsProfile::desktop)) {
    SpzDecodeConfig config;
    config.limits = limits;
    config.source_unit_to_meter = 1.0;
    config.source_color_space = ColorSpace::lin_rec709_display;
    return config;
}

std::vector<std::uint8_t> gzip_decompress(const std::vector<std::uint8_t>& input) {
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK)
        return {};
    std::vector<std::uint8_t> output;
    std::uint8_t chunk[4096];
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = chunk;
        stream.avail_out = sizeof(chunk);
        status = inflate(&stream, Z_NO_FLUSH);
        output.insert(output.end(), chunk, chunk + sizeof(chunk) - stream.avail_out);
    }
    const bool valid = status == Z_STREAM_END && stream.avail_in == 0;
    inflateEnd(&stream);
    return valid ? output : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> gzip_compress(const std::vector<std::uint8_t>& input) {
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 9,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    std::vector<std::uint8_t> output;
    std::uint8_t chunk[4096];
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = chunk;
        stream.avail_out = sizeof(chunk);
        status = deflate(&stream, Z_FINISH);
        output.insert(output.end(), chunk, chunk + sizeof(chunk) - stream.avail_out);
    }
    const bool valid = status == Z_STREAM_END;
    deflateEnd(&stream);
    return valid ? output : std::vector<std::uint8_t>{};
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_i24(std::vector<std::uint8_t>& output, std::int32_t value) {
    const std::uint32_t bits = static_cast<std::uint32_t>(value) & 0x00ffffffU;
    output.push_back(static_cast<std::uint8_t>(bits));
    output.push_back(static_cast<std::uint8_t>(bits >> 8U));
    output.push_back(static_cast<std::uint8_t>(bits >> 16U));
}

std::vector<std::uint8_t> make_degree_zero_payload(std::uint32_t version,
                                                   std::uint32_t point_count) {
    std::vector<std::uint8_t> payload;
    const std::size_t record_bytes = version == 1 ? 16 : version == 2 ? 19 : 20;
    payload.reserve(16 + static_cast<std::size_t>(point_count) * record_bytes);
    append_u32(payload, 0x5053474eU);
    append_u32(payload, version);
    append_u32(payload, point_count);
    payload.push_back(0);
    payload.push_back(version == 1 ? 0 : 8);
    payload.push_back(0);
    payload.push_back(0);
    payload.resize(16 + static_cast<std::size_t>(point_count) * record_bytes, 0);
    return payload;
}

std::vector<std::uint8_t> make_legacy_payload(std::uint32_t version) {
    std::vector<std::uint8_t> payload;
    append_u32(payload, 0x5053474eU);
    append_u32(payload, version);
    append_u32(payload, 1);
    payload.push_back(0);
    payload.push_back(version == 1 ? 0 : 8);
    payload.push_back(0);
    payload.push_back(0);
    if (version == 1) {
        append_u16(payload, 0x3c00U);
        append_u16(payload, 0xc000U);
        append_u16(payload, 0x3800U);
    } else {
        append_i24(payload, 256);
        append_i24(payload, -512);
        append_i24(payload, 128);
    }
    payload.push_back(128);
    payload.insert(payload.end(), {128, 128, 128});
    payload.insert(payload.end(), {160, 160, 160});
    payload.insert(payload.end(), {128, 128, 128});
    return payload;
}

bool near(float actual, float expected, float tolerance = 1.0e-6f) {
    return std::abs(actual - expected) <= tolerance;
}

}  // namespace
#endif

int main() {
#ifdef MELKOR_HAS_SPZ
    constexpr std::size_t kPointCount = 64;
    constexpr std::uint64_t kPackedBytes = 16 + kPointCount * 20;
    constexpr std::uint64_t kCloudBytes = kPointCount * 14 * sizeof(float);
    constexpr std::uint64_t kWorkingMemory = (128U << 10) + kPackedBytes + kCloudBytes;

    SplatBufferInput input;
    for (std::size_t i = 0; i < kPointCount; ++i) {
        input.positions.push_back({static_cast<float>(i), 0.0f, 0.0f});
        input.scales.push_back({0.05f, 0.05f, 0.05f});
        input.rotations.push_back({});
        input.opacities.push_back(0.5f);
    }
    input.sh = ShBuffer::black(kPointCount).value();
    auto data = SplatData::create(std::move(input)).value();

    SpzEncoder encoder;
    std::vector<std::uint8_t> buffer;
    const auto missing_color = encoder.encodeToBuffer(buffer, data);
    CHECK(!missing_color.success);
    CHECK(missing_color.error_code() == ErrorCode::invalid_argument);
    auto encoded = encoder.encodeToBuffer(buffer, data, encode_config());
    CHECK(encoded.success);
    CHECK(!buffer.empty());
    CHECK(encoded.losses.items().size() == 3);

    Budget encode_budget(Limits::for_profile(LimitsProfile::desktop));
    OperationContext encode_context = make_default_context(encode_budget);
    std::vector<std::uint8_t> controlled_buffer;
    {
        auto controlled_encode =
            encoder.encodeToBuffer(controlled_buffer, data, encode_config(), encode_context);
        CHECK(controlled_encode.success);
        CHECK(controlled_encode.retained_memory_bytes() == controlled_buffer.capacity());
        CHECK(encode_budget.used(BudgetKind::memory_bytes) ==
              controlled_encode.retained_memory_bytes());
    }
    CHECK(encode_budget.used(BudgetKind::memory_bytes) == 0);

    SplatBufferInput endpoint_input;
    endpoint_input.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    endpoint_input.scales = {{0.05f, 0.05f, 0.05f}, {0.05f, 0.05f, 0.05f}};
    endpoint_input.rotations = {{}, {}};
    endpoint_input.opacities = {0.0f, 1.0f};
    endpoint_input.sh = ShBuffer::black(2).value();
    const auto endpoint_data = SplatData::create(std::move(endpoint_input)).value();
    std::vector<std::uint8_t> endpoint_output;
    const auto endpoint_encoded =
        encoder.encodeToBuffer(endpoint_output, endpoint_data, encode_config());
    CHECK(endpoint_encoded.success);
    CHECK(
        std::none_of(endpoint_encoded.losses.items().begin(), endpoint_encoded.losses.items().end(),
                     [](const LossItem& item) { return item.code == loss_code::kOpacityClamped; }));
    const auto endpoint_decoded = SpzDecoder{}.decodeFromBuffer(
        endpoint_output.data(), endpoint_output.size(), decode_config());
    CHECK(endpoint_decoded.success && endpoint_decoded.data.has_value());
    if (endpoint_decoded.data.has_value()) {
        CHECK(endpoint_decoded.data->opacities()[0] == 0.0f);
        CHECK(endpoint_decoded.data->opacities()[1] == 1.0f);
    }

    SplatBufferInput positive_x_boundary_input;
    positive_x_boundary_input.positions = {{2048.0f, 0.0f, 0.0f}};
    positive_x_boundary_input.scales = {{0.05f, 0.05f, 0.05f}};
    positive_x_boundary_input.rotations = {{}};
    positive_x_boundary_input.opacities = {0.5f};
    positive_x_boundary_input.sh = ShBuffer::black(1).value();
    const auto positive_x_boundary =
        SplatData::create(std::move(positive_x_boundary_input)).value();
    std::vector<std::uint8_t> positive_x_boundary_buffer;
    const auto positive_x_boundary_encoded =
        encoder.encodeToBuffer(positive_x_boundary_buffer, positive_x_boundary, encode_config());
    CHECK(positive_x_boundary_encoded.success);
    const auto positive_x_boundary_decoded = SpzDecoder{}.decodeFromBuffer(
        positive_x_boundary_buffer.data(), positive_x_boundary_buffer.size(), decode_config());
    CHECK(positive_x_boundary_decoded.success && positive_x_boundary_decoded.data.has_value());
    if (positive_x_boundary_decoded.data.has_value())
        CHECK(positive_x_boundary_decoded.data->positions()[0].x == 2048.0f);

    SplatBufferInput negative_x_boundary_input;
    negative_x_boundary_input.positions = {{-2048.0f, 0.0f, 0.0f}};
    negative_x_boundary_input.scales = {{0.05f, 0.05f, 0.05f}};
    negative_x_boundary_input.rotations = {{}};
    negative_x_boundary_input.opacities = {0.5f};
    negative_x_boundary_input.sh = ShBuffer::black(1).value();
    const auto negative_x_boundary =
        SplatData::create(std::move(negative_x_boundary_input)).value();
    std::vector<std::uint8_t> negative_x_boundary_buffer;
    const auto negative_x_boundary_encoded =
        encoder.encodeToBuffer(negative_x_boundary_buffer, negative_x_boundary, encode_config());
    CHECK(!negative_x_boundary_encoded.success);
    CHECK(negative_x_boundary_buffer.empty());
    CHECK(std::any_of(negative_x_boundary_encoded.diagnostics.begin(),
                      negative_x_boundary_encoded.diagnostics.end(),
                      [](const Diagnostic& diagnostic) {
                          return diagnostic.code == "MK1305_SPZ_POSITION_RANGE";
                      }));

    SplatBufferInput unsafe_sh_input;
    unsafe_sh_input.positions = {{0.0f, 0.0f, 0.0f}};
    unsafe_sh_input.scales = {{0.05f, 0.05f, 0.05f}};
    unsafe_sh_input.rotations = {{}};
    unsafe_sh_input.opacities = {0.5f};
    std::vector<float> unsafe_sh_values(12, 0.0f);
    unsafe_sh_values[6] = -16777216.0f;
    unsafe_sh_input.sh = ShBuffer::create(1, 1, std::move(unsafe_sh_values)).value();
    const auto unsafe_sh = SplatData::create(std::move(unsafe_sh_input)).value();
    std::vector<std::uint8_t> unsafe_sh_buffer;
    const auto unsafe_sh_encoded =
        encoder.encodeToBuffer(unsafe_sh_buffer, unsafe_sh, encode_config());
    CHECK(!unsafe_sh_encoded.success);
    CHECK(unsafe_sh_buffer.empty());
    CHECK(std::any_of(
        unsafe_sh_encoded.diagnostics.begin(), unsafe_sh_encoded.diagnostics.end(),
        [](const Diagnostic& diagnostic) { return diagnostic.code == "MK1306_SPZ_SH_RANGE"; }));

    SplatBufferInput zero_input;
    zero_input.positions = {{0.0f, 0.0f, 0.0f}};
    zero_input.scales = {{0.0f, 0.05f, 0.05f}};
    zero_input.rotations = {{}};
    zero_input.opacities = {0.5f};
    zero_input.sh = ShBuffer::black(1).value();
    const auto zero_data = SplatData::create(std::move(zero_input)).value();
    std::vector<std::uint8_t> zero_buffer;
    auto zero_config = encode_config();
    const auto zero_blocked = encoder.encodeToBuffer(zero_buffer, zero_data, zero_config);
    CHECK(!zero_blocked.success);
    CHECK(zero_buffer.empty());
    CHECK(std::any_of(zero_blocked.losses.items().begin(), zero_blocked.losses.items().end(),
                      [](const LossItem& item) { return item.code == loss_code::kScaleClamped; }));
    zero_config.approved_loss_codes.push_back(loss_code::kScaleClamped);
    const auto zero_encoded = encoder.encodeToBuffer(zero_buffer, zero_data, zero_config);
    CHECK(zero_encoded.success);
    const auto zero_decoded =
        SpzDecoder{}.decodeFromBuffer(zero_buffer.data(), zero_buffer.size(), decode_config());
    CHECK(zero_decoded.success && zero_decoded.data.has_value());
    if (zero_decoded.data.has_value())
        CHECK(zero_decoded.data->scales()[0].x > 0.0f);

    SplatBufferInput extreme_color_input;
    extreme_color_input.positions = {{0.0f, 0.0f, 0.0f}};
    extreme_color_input.scales = {{0.05f, 0.05f, 0.05f}};
    extreme_color_input.rotations = {{}};
    extreme_color_input.opacities = {0.5f};
    extreme_color_input.sh =
        ShBuffer::create(
            0, 1, {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), 0.0f})
            .value();
    const auto extreme_color = SplatData::create(std::move(extreme_color_input)).value();
    auto extreme_color_config = encode_config();
    extreme_color_config.approved_loss_codes.push_back(loss_code::kColorClamped);
    std::vector<std::uint8_t> extreme_color_buffer;
    const auto extreme_color_encoded =
        encoder.encodeToBuffer(extreme_color_buffer, extreme_color, extreme_color_config);
    CHECK(extreme_color_encoded.success);
    const auto extreme_color_decoded = SpzDecoder{}.decodeFromBuffer(
        extreme_color_buffer.data(), extreme_color_buffer.size(), decode_config());
    CHECK(extreme_color_decoded.success && extreme_color_decoded.data.has_value());
    if (extreme_color_decoded.data.has_value()) {
        const auto dc = extreme_color_decoded.data->sh().dc(0);
        CHECK(dc.has_value());
        if (dc.has_value()) {
            CHECK(std::isfinite(dc->x) && dc->x > 0.0f);
            CHECK(std::isfinite(dc->y) && dc->y < 0.0f);
        }
    }

    SpzEncodeConfig tight_write = encode_config();
    tight_write.limits = Limits::for_profile(LimitsProfile::desktop);
    tight_write.limits.max_splats = kPointCount - 1;
    std::vector<std::uint8_t> refused_output = {1, 2, 3};
    auto refused_encode = encoder.encodeToBuffer(refused_output, data, tight_write);
    CHECK(!refused_encode.success);
    CHECK(refused_encode.error_message.find("limit") != std::string::npos);
    CHECK(refused_encode.bytes_written == 0);
    CHECK(refused_encode.retained_memory_bytes() == 0);
    CHECK(refused_output.empty());

    SpzEncodeConfig tiny_output = encode_config();
    tiny_output.limits = Limits::for_profile(LimitsProfile::desktop);
    tiny_output.limits.max_temp_bytes = 16;
    auto refused_output_size = encoder.encodeToBuffer(refused_output, data, tiny_output);
    CHECK(refused_output_size.success);
    CHECK(!refused_output.empty());

    SpzDecoder decoder;
    // Default limits accept it.
    const auto missing_decode_unit = decoder.decodeFromBuffer(buffer.data(), buffer.size());
    CHECK(!missing_decode_unit.success);
    CHECK(missing_decode_unit.error_code() == ErrorCode::invalid_argument);
    SpzDecodeConfig unit_only;
    unit_only.source_unit_to_meter = 1.0;
    const auto missing_decode_color =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), unit_only);
    CHECK(missing_decode_color.success);
    CHECK(!missing_decode_color.metadata.color_space.has_value());
    auto ok = decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config());
    CHECK(ok.success && ok.data.has_value() && ok.data->size() == kPointCount);
    CHECK(ok.data->positions()[1].x == data.positions()[1].x);
    SpzDecodeConfig centimeter_config = decode_config();
    centimeter_config.source_unit_to_meter = 0.01;
    const auto centimeters =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), centimeter_config);
    CHECK(centimeters.success && centimeters.data.has_value());
    CHECK(std::abs(centimeters.data->positions()[1].x - 0.01f) < 1.0e-6f);
    CHECK(std::abs(centimeters.data->scales()[0].x - ok.data->scales()[0].x * 0.01f) < 1.0e-8f);
    SpzDecodeConfig invalid_unit = decode_config();
    invalid_unit.source_unit_to_meter = 0.0;
    const auto refused_unit = decoder.decodeFromBuffer(buffer.data(), buffer.size(), invalid_unit);
    CHECK(!refused_unit.success);
    CHECK(refused_unit.error_code() == ErrorCode::invalid_argument);
    SpzDecodeConfig underflow_unit = decode_config();
    underflow_unit.source_unit_to_meter = std::numeric_limits<double>::denorm_min();
    const auto refused_underflow =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), underflow_unit);
    CHECK(!refused_underflow.success);
    CHECK(refused_underflow.error_code() == ErrorCode::invalid_data);

    SpzEncodeConfig antialiased_config = encode_config();
    antialiased_config.antialiased = true;
    std::vector<std::uint8_t> antialiased_buffer;
    const auto antialiased_encoded =
        encoder.encodeToBuffer(antialiased_buffer, data, antialiased_config);
    CHECK(antialiased_encoded.success);
    const auto antialiased_decoded = decoder.decodeFromBuffer(
        antialiased_buffer.data(), antialiased_buffer.size(), decode_config());
    CHECK(antialiased_decoded.success);
    CHECK(antialiased_decoded.metadata.antialiased);

    SplatBufferInput rich_input;
    rich_input.positions = {{-1.25f, 2.5f, -3.75f}, {4.5f, -5.25f, 6.0f}};
    rich_input.scales = {{0.1f, 0.2f, 0.3f}, {1.0f, 1.5f, 2.0f}};
    rich_input.rotations = {{}, {0.5f, -0.5f, 0.5f, 0.5f}};
    rich_input.opacities = {0.2f, 0.8f};
    std::vector<float> rich_sh(2 * 16 * 3);
    for (std::size_t i = 0; i < rich_sh.size(); ++i)
        rich_sh[i] = static_cast<float>(static_cast<int>(i % 11) - 5) * 0.03f;
    rich_input.sh = ShBuffer::create(3, 2, std::move(rich_sh)).value();
    const auto rich_data = SplatData::create(std::move(rich_input)).value();
    std::vector<std::uint8_t> rich_buffer;
    const auto rich_encoded = encoder.encodeToBuffer(rich_buffer, rich_data, encode_config());
    CHECK(rich_encoded.success);
    const auto rich_decoded =
        decoder.decodeFromBuffer(rich_buffer.data(), rich_buffer.size(), decode_config());
    CHECK(rich_decoded.success && rich_decoded.data.has_value());
    if (rich_decoded.data.has_value()) {
        constexpr float kSpzDcQuantizationTolerance = 0.014f;
        for (std::size_t splat = 0; splat < rich_data.size(); ++splat) {
            const auto source_dc = rich_data.sh().dc(splat);
            const auto decoded_dc = rich_decoded.data->sh().dc(splat);
            CHECK(source_dc.has_value());
            CHECK(decoded_dc.has_value());
            if (source_dc.has_value() && decoded_dc.has_value()) {
                CHECK(near(decoded_dc->x, source_dc->x, kSpzDcQuantizationTolerance));
                CHECK(near(decoded_dc->y, source_dc->y, kSpzDcQuantizationTolerance));
                CHECK(near(decoded_dc->z, source_dc->z, kSpzDcQuantizationTolerance));
            }
        }
    }

    spz::UnpackOptions reference_options;
    reference_options.to = spz::CoordinateSystem::LUF;
    const spz::GaussianCloud reference = spz::loadSpz(
        rich_buffer.data(), static_cast<std::int32_t>(rich_buffer.size()), reference_options);
    CHECK(reference.numPoints == 2);
    CHECK(reference.shDegree == 3);
    if (rich_decoded.data.has_value() && reference.numPoints == 2 && reference.shDegree == 3) {
        const SplatData& actual = *rich_decoded.data;
        for (std::size_t splat = 0; splat < actual.size(); ++splat) {
            const float* reference_position = reference.positions.data() + splat * 3;
            CHECK(near(actual.positions()[splat].x, reference_position[0]));
            CHECK(near(actual.positions()[splat].y, reference_position[1]));
            CHECK(near(actual.positions()[splat].z, reference_position[2]));

            const float* reference_scale = reference.scales.data() + splat * 3;
            CHECK(near(actual.scales()[splat].x, std::exp(reference_scale[0])));
            CHECK(near(actual.scales()[splat].y, std::exp(reference_scale[1])));
            CHECK(near(actual.scales()[splat].z, std::exp(reference_scale[2])));

            const float* reference_rotation = reference.rotations.data() + splat * 4;
            CHECK(near(actual.rotations()[splat].x, reference_rotation[0]));
            CHECK(near(actual.rotations()[splat].y, reference_rotation[1]));
            CHECK(near(actual.rotations()[splat].z, reference_rotation[2]));
            CHECK(near(actual.rotations()[splat].w, reference_rotation[3]));
            const float reference_opacity = 1.0f / (1.0f + std::exp(-reference.alphas[splat]));
            CHECK(near(actual.opacities()[splat], reference_opacity));

            const std::size_t actual_base = splat * 16 * 3;
            const std::size_t reference_dc = splat * 3;
            for (std::size_t channel = 0; channel < 3; ++channel)
                CHECK(near(actual.sh().raw()[actual_base + channel],
                           reference.colors[reference_dc + channel]));
            const std::size_t reference_rest = splat * 15 * 3;
            for (std::size_t coefficient = 1; coefficient < 16; ++coefficient) {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    CHECK(near(actual.sh().raw()[actual_base + coefficient * 3 + channel],
                               reference.sh[reference_rest + (coefficient - 1) * 3 + channel]));
                }
            }
        }
    }

    for (const std::uint32_t version : {1U, 2U}) {
        const auto legacy_payload = make_legacy_payload(version);
        const auto legacy_buffer = gzip_compress(legacy_payload);
        const auto legacy =
            decoder.decodeFromBuffer(legacy_buffer.data(), legacy_buffer.size(), decode_config());
        CHECK(legacy.success && legacy.data.has_value());
        CHECK(legacy.metadata.declared_points == 1);
        CHECK(legacy.metadata.sh_degree == 0);
        if (legacy.data.has_value()) {
            CHECK(near(legacy.data->positions()[0].x, -1.0f));
            CHECK(near(legacy.data->positions()[0].y, -2.0f));
            CHECK(near(legacy.data->positions()[0].z, -0.5f));
            CHECK(near(legacy.data->scales()[0].x, 1.0f));
            CHECK(near(legacy.data->scales()[0].y, 1.0f));
            CHECK(near(legacy.data->scales()[0].z, 1.0f));
            CHECK(near(legacy.data->opacities()[0], 128.0f / 255.0f));
        }
    }

    for (const std::uint8_t alpha : {std::uint8_t{0}, std::uint8_t{255}}) {
        auto endpoint_payload = make_degree_zero_payload(3, 1);
        endpoint_payload[25] = alpha;
        const auto endpoint_buffer = gzip_compress(endpoint_payload);
        const auto endpoint = decoder.decodeFromBuffer(endpoint_buffer.data(),
                                                       endpoint_buffer.size(), decode_config());
        CHECK(endpoint.success && endpoint.data.has_value());
        if (endpoint.data.has_value())
            CHECK(endpoint.data->opacities()[0] == static_cast<float>(alpha) / 255.0f);
    }

    auto quantized_legacy_payload = make_legacy_payload(2);
    quantized_legacy_payload[32] = 216;
    quantized_legacy_payload[33] = 98;
    quantized_legacy_payload[34] = 215;
    const auto quantized_legacy_buffer = gzip_compress(quantized_legacy_payload);
    const auto quantized_legacy = decoder.decodeFromBuffer(
        quantized_legacy_buffer.data(), quantized_legacy_buffer.size(), decode_config());
    CHECK(quantized_legacy.success && quantized_legacy.data.has_value());
    if (quantized_legacy.data.has_value()) {
        const Quatf rotation = quantized_legacy.data->rotations()[0];
        const double norm = std::sqrt(static_cast<double>(rotation.x) * rotation.x +
                                      static_cast<double>(rotation.y) * rotation.y +
                                      static_cast<double>(rotation.z) * rotation.z +
                                      static_cast<double>(rotation.w) * rotation.w);
        CHECK(std::abs(norm - 1.0) < 1.0e-6);
    }

    auto invalid_legacy_payload = make_legacy_payload(2);
    invalid_legacy_payload[32] = 0;
    invalid_legacy_payload[33] = 0;
    invalid_legacy_payload[34] = 0;
    const auto invalid_legacy_buffer = gzip_compress(invalid_legacy_payload);
    const auto invalid_legacy = decoder.decodeFromBuffer(
        invalid_legacy_buffer.data(), invalid_legacy_buffer.size(), decode_config());
    CHECK(!invalid_legacy.success);
    CHECK(invalid_legacy.error_code() == ErrorCode::invalid_data);

    auto invalid_rotation_payload = make_degree_zero_payload(3, 1);
    const std::uint32_t invalid_rotation_bits = 0x1ffU | (0x1ffU << 10U) | (0x1ffU << 20U);
    invalid_rotation_payload[32] = static_cast<std::uint8_t>(invalid_rotation_bits);
    invalid_rotation_payload[33] = static_cast<std::uint8_t>(invalid_rotation_bits >> 8U);
    invalid_rotation_payload[34] = static_cast<std::uint8_t>(invalid_rotation_bits >> 16U);
    invalid_rotation_payload[35] = static_cast<std::uint8_t>(invalid_rotation_bits >> 24U);
    const auto invalid_rotation_buffer = gzip_compress(invalid_rotation_payload);
    const auto invalid_rotation = decoder.decodeFromBuffer(
        invalid_rotation_buffer.data(), invalid_rotation_buffer.size(), decode_config());
    CHECK(!invalid_rotation.success);
    CHECK(invalid_rotation.error_code() == ErrorCode::invalid_data);

    Limits context_limits = Limits::for_profile(LimitsProfile::desktop);
    Budget cancelled_budget(context_limits);
    OperationContext cancelled_context = make_default_context(cancelled_budget);
    cancelled_context.cancellation.cancel();
    std::vector<std::uint8_t> cancelled_output = {1, 2, 3};
    const auto cancelled_encode =
        encoder.encodeToBuffer(cancelled_output, data, encode_config(), cancelled_context);
    CHECK(!cancelled_encode.success);
    CHECK(cancelled_encode.error_code() == ErrorCode::cancelled);
    CHECK(cancelled_output.empty());
    const auto cancelled_decode =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(), cancelled_context);
    CHECK(!cancelled_decode.success);
    CHECK(cancelled_decode.error_code() == ErrorCode::cancelled);
    CHECK(!cancelled_decode.data.has_value());
    CHECK(cancelled_decode.retained_memory_bytes() == 0);

    const auto large_payload = make_degree_zero_payload(3, 5000);
    const auto large_buffer = gzip_compress(large_payload);
    CHECK(large_buffer.size() < large_payload.size());
    Budget inflate_budget(context_limits);
    OperationContext inflate_context = make_default_context(inflate_budget);
    CancelDuringInflate inflate_canceller(inflate_context.cancellation);
    inflate_context.progress = &inflate_canceller;
    const auto interrupted_inflate = decoder.decodeFromBuffer(
        large_buffer.data(), large_buffer.size(), decode_config(), inflate_context);
    CHECK(!interrupted_inflate.success);
    CHECK(interrupted_inflate.error_code() == ErrorCode::cancelled);
    CHECK(inflate_budget.used(BudgetKind::memory_bytes) == 0);

    Budget expired_budget(context_limits);
    OperationContext expired_context = make_default_context(expired_budget);
    expired_context.deadline = Deadline::at(Deadline::Clock::now());
    const auto expired_decode =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(), expired_context);
    CHECK(!expired_decode.success);
    CHECK(expired_decode.error_code() == ErrorCode::resource_limit);

    Budget progress_budget(context_limits);
    OperationContext progress_context = make_default_context(progress_budget);
    CountingProgress progress;
    progress_context.progress = &progress;
    {
        const auto progress_decode = decoder.decodeFromBuffer(buffer.data(), buffer.size(),
                                                              decode_config(), progress_context);
        CHECK(progress_decode.success);
        CHECK(progress.events >= 4);
        CHECK(progress_budget.used(BudgetKind::input_bytes) == buffer.size());
        CHECK(progress_budget.used(BudgetKind::memory_bytes) == kCloudBytes);
        CHECK(progress_decode.retained_memory_bytes() == kCloudBytes);
    }
    CHECK(progress_budget.used(BudgetKind::memory_bytes) == 0);

    Limits tight_points = Limits::for_profile(LimitsProfile::desktop);
    tight_points.max_splats = kPointCount - 1;
    const auto rejected_points =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(tight_points));
    CHECK(!rejected_points.success);
    CHECK(rejected_points.error_code() == ErrorCode::resource_limit);
    CHECK(rejected_points.metadata.declared_points == kPointCount);
    CHECK(rejected_points.error_message.find("limit") != std::string::npos);

    Limits exact_points = tight_points;
    exact_points.max_splats = kPointCount;
    const auto accepted_points =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(exact_points));
    CHECK(accepted_points.success);

    // The packed size comes from the fixed header and the v3 degree-0 record width.
    CHECK(buffer.size() < kPackedBytes);
    Limits tight_decoded = Limits::for_profile(LimitsProfile::desktop);
    tight_decoded.max_input_bytes = buffer.size();
    tight_decoded.max_decoded_bytes = kPackedBytes - 1;
    const auto rejected_decoded =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(tight_decoded));
    CHECK(!rejected_decoded.success);
    CHECK(rejected_decoded.error_code() == ErrorCode::resource_limit);
    CHECK(rejected_decoded.error_message.find("limit") != std::string::npos);

    Limits exact_decoded = tight_decoded;
    exact_decoded.max_decoded_bytes = kPackedBytes;
    const auto accepted_decoded =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(exact_decoded));
    CHECK(accepted_decoded.success);

    Limits tight_ratio = Limits::for_profile(LimitsProfile::desktop);
    tight_ratio.max_decompression_ratio = 1;
    const auto rejected_ratio =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(tight_ratio));
    CHECK(!rejected_ratio.success);
    CHECK(rejected_ratio.error_code() == ErrorCode::resource_limit);
    CHECK(rejected_ratio.error_message.find("ratio") != std::string::npos);

    Limits tight_memory = Limits::for_profile(LimitsProfile::desktop);
    tight_memory.max_memory_bytes = buffer.size() + kWorkingMemory - 1;
    const auto rejected_memory =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(tight_memory));
    CHECK(!rejected_memory.success);
    CHECK(rejected_memory.error_code() == ErrorCode::resource_limit);
    CHECK(rejected_memory.error_message.find("limit") != std::string::npos);

    Limits exact_memory = tight_memory;
    exact_memory.max_memory_bytes = buffer.size() + kWorkingMemory;
    const auto accepted_memory =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(exact_memory));
    CHECK(accepted_memory.success);

    // An input-size limit below the compressed buffer refuses it before the decode.
    CHECK(buffer.size() > 1);
    Limits tiny = Limits::for_profile(LimitsProfile::desktop);
    tiny.max_input_bytes = buffer.size() - 1;
    auto rejected = decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(tiny));
    CHECK(!rejected.success);
    CHECK(rejected.error_code() == ErrorCode::resource_limit);
    CHECK(rejected.error_message.find("limit") != std::string::npos);

    const std::vector<std::uint8_t> truncated_gzip = {0x1f, 0x8b, 0x08};
    const auto rejected_header =
        decoder.decodeFromBuffer(truncated_gzip.data(), truncated_gzip.size(), decode_config());
    CHECK(!rejected_header.success);
    CHECK(rejected_header.error_code() == ErrorCode::invalid_data);

    std::vector<std::uint8_t> trailing = buffer;
    trailing.push_back(0);
    const auto rejected_trailing =
        decoder.decodeFromBuffer(trailing.data(), trailing.size(), decode_config());
    CHECK(!rejected_trailing.success);
    CHECK(rejected_trailing.error_code() == ErrorCode::invalid_data);

    auto packed = gzip_decompress(buffer);
    CHECK(packed.size() >= 16);
    if (packed.size() >= 16) {
        packed[14] = 0x80;
        const auto unknown_flag = gzip_compress(packed);
        const auto rejected_flag =
            decoder.decodeFromBuffer(unknown_flag.data(), unknown_flag.size(), decode_config());
        CHECK(!rejected_flag.success);
        CHECK(rejected_flag.error_code() == ErrorCode::invalid_data);

        packed[14] = 0;
        packed[15] = 1;
        const auto reserved = gzip_compress(packed);
        const auto rejected_reserved =
            decoder.decodeFromBuffer(reserved.data(), reserved.size(), decode_config());
        CHECK(!rejected_reserved.success);
        CHECK(rejected_reserved.error_code() == ErrorCode::invalid_data);
    }

    Limits tiny_probe = Limits::for_profile(LimitsProfile::desktop);
    tiny_probe.max_metadata_string_bytes = 1;
    tiny_probe.max_metadata_total_bytes = 1;
    const auto rejected_probe =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(tiny_probe));
    CHECK(!rejected_probe.success);
    CHECK(rejected_probe.error_code() == ErrorCode::resource_limit);
    CHECK(rejected_probe.error_message.find("limit") != std::string::npos);

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("melkor-spz-budget-" + std::to_string(nonce) + ".spz");
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() /
        ("melkor-spz-output-budget-" + std::to_string(nonce) + ".spz");
    const auto refused_file = encoder.encodeToFile(output_path, data, tiny_output);
    CHECK(!refused_file.success);
    CHECK(refused_file.error_message.find("limit") != std::string::npos);
    CHECK(!std::filesystem::exists(output_path));
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
    }
    Limits file_memory = Limits::for_profile(LimitsProfile::desktop);
    file_memory.max_memory_bytes = buffer.size() - 1;
    const auto rejected_file = decoder.decodeFromFile(path.string(), decode_config(file_memory));
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    CHECK(!rejected_file.success);
    CHECK(rejected_file.error_code() == ErrorCode::resource_limit);
    CHECK(rejected_file.error_message.find("limit") != std::string::npos);

    const Limits invalid = Limits::for_profile(LimitsProfile::custom);
    const auto rejected_invalid =
        decoder.decodeFromBuffer(buffer.data(), buffer.size(), decode_config(invalid));
    CHECK(!rejected_invalid.success);
    CHECK(rejected_invalid.error_code() == ErrorCode::invalid_argument);
    CHECK(rejected_invalid.error_message.find("limit") != std::string::npos);

    if (g_failures == 0) {
        std::printf("spz budget: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "spz budget: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
#else
    std::printf("spz budget: skipped (MELKOR_HAS_SPZ off)\n");
    return 0;
#endif
}
