// Canonical PLY boundary and parser regression tests.
//
// PLY stores graphdeco training domains (log scale, logit opacity, wxyz rotation, and
// channel-major higher SH). SplatData stores linear scale, linear opacity, xyzw rotation, and
// coefficient/channel-interleaved SH. These tests pin both directions independently so a
// symmetric double-conversion or double-transpose cannot make a round trip pass by accident.

#include "melkor/math/color.hpp"
#include "melkor/math/covariance.hpp"
#include "melkor/math/quaternion.hpp"
#include "melkor/ply_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <locale>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr float kEpsilon = 2.0e-5f;

bool approx(float a, float b, float tolerance = kEpsilon) {
    const float scale = std::max({1.0f, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= tolerance * scale;
}

melkor::SplatData make_data(std::size_t count, std::uint32_t degree, std::mt19937& rng) {
    using namespace melkor;
    std::uniform_real_distribution<float> position(-100.0f, 100.0f);
    std::uniform_real_distribution<float> log_scale(-7.0f, 1.0f);
    std::uniform_real_distribution<float> opacity(0.002f, 0.998f);
    std::uniform_real_distribution<float> quaternion(-1.0f, 1.0f);
    std::uniform_real_distribution<float> sh(-3.0f, 3.0f);

    SplatBufferInput input;
    input.positions.reserve(count);
    input.scales.reserve(count);
    input.rotations.reserve(count);
    input.opacities.reserve(count);
    const std::size_t coefficients = static_cast<std::size_t>(degree + 1) * (degree + 1);
    std::vector<float> sh_values(count * coefficients * 3);

    for (std::size_t i = 0; i < count; ++i) {
        input.positions.push_back({position(rng), position(rng), position(rng)});
        input.scales.push_back(
            {std::exp(log_scale(rng)), std::exp(log_scale(rng)), std::exp(log_scale(rng))});
        auto unit = melkor::math::normalize(
            {quaternion(rng), quaternion(rng), quaternion(rng), quaternion(rng)});
        if (!unit.has_value())
            std::abort();
        input.rotations.push_back(
            {static_cast<float>(unit.value().x), static_cast<float>(unit.value().y),
             static_cast<float>(unit.value().z), static_cast<float>(unit.value().w)});
        input.opacities.push_back(opacity(rng));
        for (std::size_t j = 0; j < coefficients * 3; ++j) {
            sh_values[i * coefficients * 3 + j] = sh(rng);
        }
    }
    input.sh = ShBuffer::create(degree, count, std::move(sh_values)).value();
    return SplatData::create(std::move(input)).value();
}

bool same_data(const melkor::SplatData& expected, const melkor::SplatData& actual) {
    if (expected.size() != actual.size() || expected.sh().degree() != actual.sh().degree() ||
        expected.sh().raw().size() != actual.sh().raw().size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto a = expected.positions()[i];
        const auto b = actual.positions()[i];
        const auto as = expected.scales()[i];
        const auto bs = actual.scales()[i];
        if (!approx(a.x, b.x) || !approx(a.y, b.y) || !approx(a.z, b.z) || !approx(as.x, bs.x) ||
            !approx(as.y, bs.y) || !approx(as.z, bs.z) ||
            !approx(expected.opacities()[i], actual.opacities()[i])) {
            return false;
        }
        const auto aq = expected.rotations()[i];
        const auto bq = actual.rotations()[i];
        if (melkor::math::angular_distance({aq.x, aq.y, aq.z, aq.w}, {bq.x, bq.y, bq.z, bq.w}) >
            1.0e-3) {
            return false;
        }
    }
    for (std::size_t i = 0; i < expected.sh().raw().size(); ++i) {
        if (!approx(expected.sh().raw()[i], actual.sh().raw()[i]))
            return false;
    }
    return true;
}

bool same_semantics(const melkor::SplatData& expected, const melkor::SplatData& actual) {
    if (expected.size() != actual.size() || expected.sh().degree() != actual.sh().degree())
        return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& ep = expected.positions()[i];
        const auto& ap = actual.positions()[i];
        if (!approx(ep.x, ap.x, 1.0e-4f) || !approx(ep.y, ap.y, 1.0e-4f) ||
            !approx(ep.z, ap.z, 1.0e-4f) ||
            !approx(expected.opacities()[i], actual.opacities()[i], 1.0e-4f)) {
            return false;
        }
        const auto& er = expected.rotations()[i];
        const auto& es = expected.scales()[i];
        const auto& ar = actual.rotations()[i];
        const auto& as = actual.scales()[i];
        auto expected_covariance = melkor::math::covariance_from_rotation_scale(
            {er.x, er.y, er.z, er.w}, {es.x, es.y, es.z});
        auto actual_covariance = melkor::math::covariance_from_rotation_scale(
            {ar.x, ar.y, ar.z, ar.w}, {as.x, as.y, as.z});
        if (!expected_covariance.has_value() || !actual_covariance.has_value())
            return false;
        for (std::size_t component = 0; component < 9; ++component) {
            if (!approx(static_cast<float>(expected_covariance.value()[component]),
                        static_cast<float>(actual_covariance.value()[component]), 2.0e-4f)) {
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < expected.sh().raw().size(); ++i) {
        if (!approx(expected.sh().raw()[i], actual.sh().raw()[i], 2.0e-4f))
            return false;
    }
    return true;
}

bool has_diagnostic(const melkor::PlyWriteResult& result, const char* code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [&](const auto& diagnostic) { return diagnostic.code == code; });
}

bool has_diagnostic(const melkor::PlyReader::ReadResult& result, const char* code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [&](const auto& diagnostic) { return diagnostic.code == code; });
}

class NonSeekingBuffer final : public std::streambuf {
public:
    const std::string& bytes() const noexcept { return bytes_; }

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count > 0)
            bytes_.append(data, static_cast<std::size_t>(count));
        return count;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        bytes_.push_back(traits_type::to_char_type(value));
        return value;
    }

private:
    std::string bytes_;
};

class RejectingBuffer final : public std::streambuf {
protected:
    std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
    int_type overflow(int_type) override { return traits_type::eof(); }
};

class GroupEveryDigit final : public std::numpunct<char> {
protected:
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\1"; }
};

bool has_loss(const melkor::LossReport& report, const char* code) {
    return std::any_of(report.items().begin(), report.items().end(),
                       [&](const auto& item) { return item.code == code; });
}

bool has_loss(const melkor::LossReport& report, const char* code, melkor::LossSeverity severity) {
    return std::any_of(report.items().begin(), report.items().end(), [&](const auto& item) {
        return item.code == code && item.severity == severity;
    });
}

melkor::PlyReadConfig graphdeco_read_config() {
    melkor::PlyReadConfig config;
    config.profile = melkor::FormatProfileId::ply_graphdeco_3dgs_v1;
    config.source_frame_id = "gltf-luf";
    config.source_unit_to_meter = 1.0;
    config.source_color_space = melkor::ColorSpace::lin_rec709_display;
    return config;
}

melkor::PlyReadConfig canonical_read_config() {
    melkor::PlyReadConfig config;
    config.profile = melkor::FormatProfileId::ply_melkor_canonical_v1;
    config.source_color_space = melkor::ColorSpace::lin_rec709_display;
    return config;
}

melkor::PlyWriteConfig canonical_write_config() {
    melkor::PlyWriteConfig config;
    config.color_space = melkor::ColorSpace::lin_rec709_display;
    config.antialiased = false;
    return config;
}

melkor::PlyWriteConfig graphdeco_write_config() {
    melkor::PlyWriteConfig config = canonical_write_config();
    config.profile = melkor::FormatProfileId::ply_graphdeco_3dgs_v1;
    config.target_frame_id = "gltf-luf";
    return config;
}

melkor::PlyWriteConfig da3_write_config() {
    melkor::PlyWriteConfig config = canonical_write_config();
    config.profile = melkor::FormatProfileId::ply_da3_gaussian_v1;
    return config;
}

std::uint32_t float_bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

}  // namespace

int main() {
    using namespace melkor;
    int failures = 0;
    auto expect = [&](const char* label, bool condition) {
        std::printf(condition ? "  PASS [%s]\n" : "  FAIL [%s]\n", label);
        if (!condition)
            ++failures;
    };
    std::mt19937 rng(7);

    std::printf("[ply] canonical binary round trips\n");
    for (std::uint32_t degree : {0u, 1u, 2u, 3u, 4u}) {
        for (std::size_t count : {0u, 1u, 17u, 100u}) {
            auto input = make_data(count, degree, rng);
            PlyWriteConfig config = canonical_write_config();
            std::vector<std::uint8_t> bytes;
            const auto written = PlyWriter{}.writeToBuffer(bytes, input, config);
            const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
            const bool ok = written.success && read.success && read.data.has_value() &&
                            read.losses.items().empty() && same_data(input, *read.data);
            char label[64];
            std::snprintf(label, sizeof(label), "degree=%u count=%zu", degree, count);
            expect(label, ok);
        }
    }
    {
        auto input = make_data(17, 3, rng);
        auto config = graphdeco_write_config();
        config.target_frame_id = "ply-rdf";
        std::vector<std::uint8_t> bytes;
        const auto written = PlyWriter{}.writeToBuffer(bytes, input, config);
        const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        const std::string header(bytes.begin(), bytes.end());
        bool representation_preserved = false;
        if (read.data.has_value()) {
            const auto& source_scale = input.scales()[0];
            const auto& result_scale = read.data->scales()[0];
            const auto& source_rotation = input.rotations()[0];
            const auto& result_rotation = read.data->rotations()[0];
            representation_preserved =
                approx(source_scale.x, result_scale.x) && approx(source_scale.y, result_scale.y) &&
                approx(source_scale.z, result_scale.z) &&
                melkor::math::angular_distance(
                    {source_rotation.x, source_rotation.y, source_rotation.z, source_rotation.w},
                    {result_rotation.x, result_rotation.y, result_rotation.z, result_rotation.w}) <
                    1.0e-5;
        }
        expect("Graphdeco RDF output returns to canonical semantics",
               written.success && read.success && read.data.has_value() &&
                   header.find("comment melkor_coordinate_system ply-rdf\n") != std::string::npos &&
                   same_semantics(input, *read.data) && representation_preserved);
    }

    std::printf("[ply] explicit boundary semantics\n");
    {
        // Hand-authored training-domain values: logit(0)=0 -> 0.5, ln scales -> linear scales,
        // and PLY wxyz -> canonical xyzw. A distinct unit quaternion makes reordering observable.
        const std::string source =
            "ply\nformat ascii 1.0\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
            "property float opacity\n"
            "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
            "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float "
            "rot_3\n"
            "end_header\n1 2 3 0.1 0.2 0.3 0 -2.3025851 -1.609438 -1.2039728 "
            "0.5 0.5 0.5 0.5\n";
        const auto read =
            PlyReader{}.readFromBuffer(reinterpret_cast<const std::uint8_t*>(source.data()),
                                       source.size(), graphdeco_read_config());
        const bool ok =
            read.success && read.data.has_value() && read.data->size() == 1 &&
            approx(read.data->opacities()[0], 0.5f) && approx(read.data->scales()[0].x, 0.1f) &&
            approx(read.data->scales()[0].y, 0.2f) && approx(read.data->scales()[0].z, 0.3f) &&
            approx(read.data->rotations()[0].x, 0.5f) && approx(read.data->rotations()[0].w, 0.5f);
        expect("training domains decode exactly once", ok);

        auto centimeter_config = graphdeco_read_config();
        centimeter_config.source_unit_to_meter = 0.01;
        const auto centimeters = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(source.data()), source.size(), centimeter_config);
        expect("explicit PLY units scale positions and Gaussian radii",
               centimeters.success && centimeters.data.has_value() &&
                   approx(centimeters.data->positions()[0].x, 0.01f) &&
                   approx(centimeters.data->positions()[0].y, 0.02f) &&
                   approx(centimeters.data->scales()[0].x, 0.001f));

        auto overflowing_unit = graphdeco_read_config();
        overflowing_unit.source_unit_to_meter = std::numeric_limits<double>::max();
        const auto overflowed = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(source.data()), source.size(), overflowing_unit);
        expect("PLY frame scaling rejects canonical float overflow",
               !overflowed.success && overflowed.error_code() == ErrorCode::invalid_data &&
                   has_diagnostic(overflowed, "MK1214_PLY_TRANSFORM_RANGE"));

        auto underflowing_unit = graphdeco_read_config();
        underflowing_unit.source_unit_to_meter = std::numeric_limits<double>::denorm_min();
        const auto underflowed = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(source.data()), source.size(), underflowing_unit);
        expect("PLY frame scaling rejects canonical float underflow",
               !underflowed.success && underflowed.error_code() == ErrorCode::invalid_data &&
                   has_diagnostic(underflowed, "MK1214_PLY_TRANSFORM_RANGE"));

        auto missing_unit = graphdeco_read_config();
        missing_unit.source_unit_to_meter.reset();
        const auto refused = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(source.data()), source.size(), missing_unit);
        expect("unmarked PLY input does not assume a length unit", !refused.success);
    }
    {
        // graphdeco f_rest is channel-major. Give every property a unique value and prove the
        // canonical [coefficient][channel] transpose independently of Melkor's writer.
        std::string source =
            "ply\nformat ascii 1.0\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n";
        for (int i = 0; i < 9; ++i) {
            source += "property float f_rest_" + std::to_string(i) + "\n";
        }
        source += "property float opacity\n"
                  "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
                  "property float rot_0\nproperty float rot_1\nproperty float rot_2\n"
                  "property float rot_3\n"
                  "end_header\n0 0 0 10 11 12 20 21 22 30 31 32 40 41 42 "
                  "0 0 0 0 1 0 0 0\n";
        const auto read =
            PlyReader{}.readFromBuffer(reinterpret_cast<const std::uint8_t*>(source.data()),
                                       source.size(), graphdeco_read_config());
        bool ok = read.success && read.data.has_value() && read.data->sh().degree() == 1;
        const std::vector<float> expected{10, 11, 12, 20, 30, 40, 21, 31, 41, 22, 32, 42};
        if (ok)
            ok = read.data->sh().raw() == expected;
        expect("channel-major higher SH transposes to canonical layout", ok);
    }
    {
        auto endpoints = make_data(2, 0, rng);
        SplatBufferInput replacement;
        replacement.positions = endpoints.positions();
        replacement.scales = endpoints.scales();
        replacement.rotations = endpoints.rotations();
        replacement.opacities = {0.0f, 1.0f};
        replacement.sh =
            ShBuffer::create(endpoints.sh().degree(), endpoints.size(), endpoints.sh().raw())
                .value();
        endpoints = SplatData::create(std::move(replacement)).value();
        std::vector<std::uint8_t> bytes;
        auto config = graphdeco_write_config();
        const auto written = PlyWriter{}.writeToBuffer(bytes, endpoints, config);
        const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("endpoint clamp is explicit and finite",
               written.success && has_diagnostic(written, "MK1210_PLY_OPACITY_ENDPOINT_CLAMPED") &&
                   read.success && read.data.has_value() && read.data->opacities()[0] > 0.0f &&
                   read.data->opacities()[1] < 1.0f);
    }
    {
        auto source = make_data(1, 0, rng);
        SplatBufferInput replacement;
        replacement.positions = source.positions();
        replacement.scales = {{0.0f, source.scales()[0].y, source.scales()[0].z}};
        replacement.rotations = source.rotations();
        replacement.opacities = source.opacities();
        replacement.sh =
            ShBuffer::create(source.sh().degree(), source.size(), source.sh().raw()).value();
        source = SplatData::create(std::move(replacement)).value();

        std::vector<std::uint8_t> bytes;
        const auto canonical = PlyWriter{}.writeToBuffer(bytes, source, canonical_write_config());
        const auto canonical_read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        auto graphdeco = graphdeco_write_config();
        bytes.clear();
        const auto blocked = PlyWriter{}.writeToBuffer(bytes, source, graphdeco);
        graphdeco.approved_loss_codes = {loss_code::kScaleClamped};
        const auto clamped = PlyWriter{}.writeToBuffer(bytes, source, graphdeco);
        const auto graphdeco_read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("zero scale is exact in canonical PLY and explicit in Graphdeco PLY",
               canonical.success && canonical_read.success && canonical_read.data.has_value() &&
                   canonical_read.data->scales()[0].x == 0.0f && !blocked.success &&
                   has_loss(blocked.losses, loss_code::kScaleClamped) && clamped.success &&
                   graphdeco_read.success && graphdeco_read.data.has_value() &&
                   graphdeco_read.data->scales()[0].x > 0.0f);
    }
    {
        auto degree_one = make_data(1, 1, rng);
        std::vector<std::uint8_t> bytes;
        PlyWriteConfig config = canonical_write_config();
        config.include_sh_rest = false;
        const auto blocked = PlyWriter{}.writeToBuffer(bytes, degree_one, config);
        config.approved_loss_codes = {loss_code::kShCoefficientsDropped};
        const auto written = PlyWriter{}.writeToBuffer(bytes, degree_one, config);
        const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("configured SH omission is reported",
               !blocked.success && has_diagnostic(blocked, "MK1602_UNAPPROVED_SEVERE_LOSS") &&
                   written.success && has_loss(written.losses, loss_code::kShCoefficientsDropped) &&
                   read.success && read.data.has_value() && read.data->sh().degree() == 0);
    }
    {
        auto degree_four = make_data(1, 4, rng);
        std::vector<std::uint8_t> bytes;
        const auto canonical_written =
            PlyWriter{}.writeToBuffer(bytes, degree_four, canonical_write_config());
        const auto canonical_read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("canonical PLY preserves degree 4",
               canonical_written.success && canonical_read.success && canonical_read.data &&
                   same_data(degree_four, *canonical_read.data));

        auto graphdeco = graphdeco_write_config();
        bytes.clear();
        const auto graphdeco_blocked = PlyWriter{}.writeToBuffer(bytes, degree_four, graphdeco);
        expect("Graphdeco PLY does not silently truncate degree 4",
               !graphdeco_blocked.success && bytes.empty() &&
                   has_loss(graphdeco_blocked.losses, loss_code::kShDegreeTruncated));

        bytes.clear();
        const auto da3_written = PlyWriter{}.writeToBuffer(bytes, degree_four, da3_write_config());
        const std::string da3_output(bytes.begin(), bytes.end());
        const auto da3_read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        auto wrong_frame = da3_write_config();
        wrong_frame.target_frame_id = "gltf-luf";
        std::vector<std::uint8_t> wrong_frame_bytes;
        const auto wrong_frame_write =
            PlyWriter{}.writeToBuffer(wrong_frame_bytes, degree_four, wrong_frame);
        expect("DA3 PLY preserves degree 4 with an exact profile marker",
               da3_written.success && da3_read.success && da3_read.data.has_value() &&
                   da3_read.metadata.profile == FormatProfileId::ply_da3_gaussian_v1 &&
                   da3_output.find("comment melkor_profile da3-gaussian-v1\n") !=
                       std::string::npos &&
                   da3_output.find("comment melkor_coordinate_system ply-rdf\n") !=
                       std::string::npos &&
                   da3_output.find("property float f_rest_71\n") != std::string::npos &&
                   same_semantics(degree_four, *da3_read.data) && !wrong_frame_write.success &&
                   wrong_frame_bytes.empty());
    }
    {
        auto precise = make_data(1, 0, rng);
        constexpr float value = 1.0317308636832647e-16f;
        SplatBufferInput replacement;
        replacement.positions = {{value, -value, value}};
        replacement.scales = precise.scales();
        replacement.rotations = precise.rotations();
        replacement.opacities = precise.opacities();
        replacement.sh =
            ShBuffer::create(precise.sh().degree(), precise.size(), precise.sh().raw()).value();
        precise = SplatData::create(std::move(replacement)).value();
        PlyWriteConfig config = canonical_write_config();
        config.format = PlyFormat::Ascii;
        std::vector<std::uint8_t> bytes;
        const auto written = PlyWriter{}.writeToBuffer(bytes, precise, config);
        const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("ASCII output preserves every float bit",
               written.success && read.success && read.data.has_value() &&
                   float_bits(read.data->positions()[0].x) == float_bits(value) &&
                   float_bits(read.data->positions()[0].y) == float_bits(-value));
    }
    {
        auto data = make_data(3, 0, rng);
        PlyWriteConfig config = canonical_write_config();
        config.format = PlyFormat::Ascii;
        NonSeekingBuffer sink;
        std::ostream stream(&sink);
        const auto written = PlyWriter{}.writeToStream(stream, data, config);
        expect("ASCII byte count works for a non-seekable stream",
               written.success && written.bytes_written == sink.bytes().size());
    }
    {
        auto data = make_data(4096, 0, rng);
        RejectingBuffer sink;
        std::ostream stream(&sink);
        const auto written = PlyWriter{}.writeToStream(stream, data, canonical_write_config());
        expect("stream failure stops before vertex conversion",
               !written.success && written.error_code() == ErrorCode::io_error);
    }
    {
        auto data = make_data(17, 0, rng);
        const std::locale previous = std::locale();
        std::locale::global(std::locale(previous, new GroupEveryDigit));
        std::vector<std::uint8_t> bytes;
        const auto written = PlyWriter{}.writeToBuffer(bytes, data, canonical_write_config());
        std::locale::global(previous);
        const std::string output(bytes.begin(), bytes.end());
        expect("PLY header output does not depend on the process locale",
               written.success && output.find("element vertex 17\n") != std::string::npos &&
                   output.find("element vertex 1,7\n") == std::string::npos);
    }
    {
        auto data = make_data(1, 0, rng);
        PlyWriteConfig config = canonical_write_config();
        config.format = PlyFormat::Ascii;
        config.comment = "safe\nproperty float injected\rend_header\x7f UTF-8 \xC3\xA9";
        std::vector<std::uint8_t> bytes;
        const auto written = PlyWriter{}.writeToBuffer(bytes, data, config);
        const std::string output(bytes.begin(), bytes.end());
        const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("comment bytes cannot inject invalid PLY header data",
               written.success && has_diagnostic(written, "MK1213_PLY_COMMENT_SANITIZED") &&
                   read.success &&
                   output.find("\nproperty float injected\n") == std::string::npos &&
                   std::none_of(output.begin(), output.end(),
                                [](unsigned char byte) { return byte > 0x7e; }));
    }
    {
        auto data = make_data(1, 4, rng);
        std::vector<std::uint8_t> bytes;
        const auto written = PlyWriter{}.writeToBuffer(bytes, data, canonical_write_config());
        const std::string output(bytes.begin(), bytes.end());
        expect("canonical output declares its exact profile",
               written.success &&
                   output.find("comment melkor_profile melkor-canonical-v1\n") !=
                       std::string::npos &&
                   output.find("comment melkor_color_space lin_rec709_display\n") !=
                       std::string::npos &&
                   output.find("property float sh_4_8_b\n") != std::string::npos &&
                   output.find("property float f_dc_0\n") == std::string::npos);
    }
    {
        const std::string non_unit =
            "ply\nformat ascii 1.0\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
            "property float opacity\n"
            "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
            "property float rot_0\nproperty float rot_1\nproperty float rot_2\n"
            "property float rot_3\n"
            "end_header\n0 0 0 0 0 0 0 0 0 0 2 0 0 0\n";
        const auto read =
            PlyReader{}.readFromBuffer(reinterpret_cast<const std::uint8_t*>(non_unit.data()),
                                       non_unit.size(), graphdeco_read_config());
        expect("Graphdeco raw quaternion is normalized",
               read.success && read.data.has_value() && approx(read.data->rotations()[0].w, 1.0f) &&
                   has_loss(read.losses, loss_code::kQuaternionNormalized));

        const std::string zero = non_unit.substr(0, non_unit.rfind("2 0 0 0")) + "0 0 0 0\n";
        const auto rejected =
            PlyReader{}.readFromBuffer(reinterpret_cast<const std::uint8_t*>(zero.data()),
                                       zero.size(), graphdeco_read_config());
        expect("zero Graphdeco quaternion fails closed",
               !rejected.success && !rejected.data.has_value());
    }
    {
        const std::string source =
            "ply\nformat ascii 1.0\ncomment source note\nobj_info object note\n"
            "element face 0\nproperty list uchar int vertex_indices\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property float nx\nproperty float ny\nproperty float nz\n"
            "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
            "property float opacity\nproperty float scale_0\nproperty float scale_1\n"
            "property float scale_2\nproperty float rot_0\nproperty float rot_1\n"
            "property float rot_2\nproperty float rot_3\nend_header\n"
            "0 0 0 1 0 0 0 0 0 0 0 0 0 1 0 0 0\n";
        const auto read =
            PlyReader{}.readFromBuffer(reinterpret_cast<const std::uint8_t*>(source.data()),
                                       source.size(), graphdeco_read_config());
        expect("PLY metadata and nonzero normals are explicit losses",
               read.success &&
                   has_loss(read.losses, loss_code::kMetadataDropped, LossSeverity::severe) &&
                   has_loss(read.losses, loss_code::kVertexNormalsDropped));
    }
    {
        const std::string empty_metadata =
            "ply\nformat ascii 1.0\ncomment\nobj_info\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property float scale_x\nproperty float scale_y\nproperty float scale_z\n"
            "property float rotation_x\nproperty float rotation_y\nproperty float rotation_z\n"
            "property float rotation_w\nproperty float opacity\n"
            "property float sh_0_0_r\nproperty float sh_0_0_g\nproperty float sh_0_0_b\n"
            "end_header\n0 0 0 1 1 1 0 0 0 1 0.5 0 0 0\n";
        const auto read =
            PlyReader{}.readFromBuffer(reinterpret_cast<const std::uint8_t*>(empty_metadata.data()),
                                       empty_metadata.size(), canonical_read_config());
        expect("empty PLY metadata does not require severe loss approval",
               read.success &&
                   !has_loss(read.losses, loss_code::kMetadataDropped, LossSeverity::severe));
    }
    {
        const std::string partial_normals =
            "ply\nformat ascii 1.0\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\nproperty float nx\n"
            "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
            "property float opacity\nproperty float scale_0\nproperty float scale_1\n"
            "property float scale_2\nproperty float rot_0\nproperty float rot_1\n"
            "property float rot_2\nproperty float rot_3\nend_header\n"
            "0 0 0 0 0 0 0 0 0 0 0 1 0 0 0\n";
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(partial_normals.data()), partial_normals.size(),
            graphdeco_read_config());
        expect("partial Graphdeco normal groups are invalid", !read.success);
    }
    {
        const std::string canonical_alias =
            "ply\nformat ascii 1.0\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property float scale_x\nproperty float scale_y\nproperty float scale_z\n"
            "property float rotation_x\nproperty float rotation_y\nproperty float rotation_z\n"
            "property float rotation_w\nproperty float opacity\n"
            "property float sh_00_0_r\nproperty float sh_0_0_g\nproperty float sh_0_0_b\n"
            "end_header\n0 0 0 1 1 1 0 0 0 1 0.5 0 0 0\n";
        const auto canonical_read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(canonical_alias.data()), canonical_alias.size(),
            canonical_read_config());
        expect("canonical PLY rejects SH spelling aliases", !canonical_read.success);

        std::string graphdeco_alias =
            "ply\nformat ascii 1.0\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n";
        graphdeco_alias += "property float f_rest_00\n";
        for (int index = 1; index < 9; ++index)
            graphdeco_alias += "property float f_rest_" + std::to_string(index) + "\n";
        graphdeco_alias +=
            "property float opacity\nproperty float scale_0\nproperty float scale_1\n"
            "property float scale_2\nproperty float rot_0\nproperty float rot_1\n"
            "property float rot_2\nproperty float rot_3\nend_header\n"
            "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0\n";
        const auto graphdeco_read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(graphdeco_alias.data()), graphdeco_alias.size(),
            graphdeco_read_config());
        expect("Graphdeco PLY rejects f_rest spelling aliases", !graphdeco_read.success);
    }
    {
        auto data = make_data(1, 0, rng);
        auto config = canonical_write_config();
        config.format = static_cast<PlyFormat>(99);
        std::vector<std::uint8_t> bytes = {1, 2, 3};
        const auto written = PlyWriter{}.writeToBuffer(bytes, data, config);
        expect("invalid PLY encoding fails before output",
               !written.success && written.error_code() == ErrorCode::invalid_argument &&
                   bytes.empty());
    }

    std::printf("[ply] parser encodings and limits\n");
    auto append = [](std::vector<std::uint8_t>& target, const std::string& text) {
        target.insert(target.end(), text.begin(), text.end());
    };
    auto push_be_float = [](std::vector<std::uint8_t>& target, float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        target.push_back(static_cast<std::uint8_t>(bits >> 24));
        target.push_back(static_cast<std::uint8_t>(bits >> 16));
        target.push_back(static_cast<std::uint8_t>(bits >> 8));
        target.push_back(static_cast<std::uint8_t>(bits));
    };
    {
        std::vector<std::uint8_t> bytes;
        append(bytes, "ply\nformat binary_big_endian 1.0\nelement vertex 1\n"
                      "property float x\nproperty float y\nproperty float z\n"
                      "property float scale_x\nproperty float scale_y\n"
                      "property float scale_z\nproperty float rotation_x\n"
                      "property float rotation_y\nproperty float rotation_z\n"
                      "property float rotation_w\nproperty float opacity\n"
                      "property float sh_0_0_r\nproperty float sh_0_0_g\n"
                      "property float sh_0_0_b\nend_header\n");
        for (float value : {1.0f, -2.5f, 3.25f, 1.0f, 2.0f, 3.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f,
                            0.1f, 0.2f, 0.3f}) {
            push_be_float(bytes, value);
        }
        const auto read =
            PlyReader{}.readFromBuffer(bytes.data(), bytes.size(), canonical_read_config());
        expect("big-endian binary", read.success && read.data.has_value() &&
                                        approx(read.data->positions()[0].x, 1.0f) &&
                                        approx(read.data->positions()[0].y, -2.5f) &&
                                        approx(read.data->positions()[0].z, 3.25f));
    }
    {
        const std::string crlf = "ply\r\nformat ascii 1.0\r\nelement vertex 1\r\n"
                                 "property float x\r\nproperty float y\r\nproperty float z\r\n"
                                 "property float scale_x\r\nproperty float scale_y\r\n"
                                 "property float scale_z\r\nproperty float rotation_x\r\n"
                                 "property float rotation_y\r\nproperty float rotation_z\r\n"
                                 "property float rotation_w\r\nproperty float opacity\r\n"
                                 "property float sh_0_0_r\r\nproperty float sh_0_0_g\r\n"
                                 "property float sh_0_0_b\r\n"
                                 "comment end_header is not a terminator here\r\n"
                                 "end_header\r\n1 2 3 1 1 1 0 0 0 1 0.5 0 0 0\r\n";
        const auto read =
            PlyReader{}.readFromBuffer(reinterpret_cast<const std::uint8_t*>(crlf.data()),
                                       crlf.size(), canonical_read_config());
        expect("CRLF and end_header in comment",
               read.success && read.data.has_value() && approx(read.data->positions()[0].y, 2.0f));
    }
    {
        const std::string generic =
            "ply\nformat ascii 1.0\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\nend_header\n1 2 3\n";
        const auto blocked = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(generic.data()), generic.size());
        expect("generic PLY is not a Gaussian profile",
               !blocked.success && blocked.error_code() == ErrorCode::unsupported_feature);
    }
    {
        const std::string generic = "ply\nformat ascii 1.0\nelement vertex 1\n"
                                    "property float x\nproperty float y\nproperty float z\n"
                                    "property float nx\nproperty float ny\nproperty float nz\n"
                                    "end_header\n1 2 3 0 0 1\n";
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(generic.data()), generic.size());
        expect("generic normals do not trigger Gaussian initialization",
               !read.success && read.error_code() == ErrorCode::unsupported_feature);
    }
    {
        auto input = make_data(1, 0, rng);
        std::vector<std::uint8_t> bytes;
        const auto written = PlyWriter{}.writeToBuffer(bytes, input, canonical_write_config());
        bytes.push_back(0);
        const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("binary PLY rejects trailing bytes", written.success && !read.success);
    }
    {
        auto input = make_data(1, 0, rng);
        std::vector<std::uint8_t> bytes;
        auto config = graphdeco_write_config();
        config.format = PlyFormat::Ascii;
        const auto written = PlyWriter{}.writeToBuffer(bytes, input, config);
        const std::string output(bytes.begin(), bytes.end());
        const std::size_t payload = output.find("end_header\n");
        std::istringstream record(
            payload == std::string::npos
                ? std::string{}
                : output.substr(payload + std::string("end_header\n").size()));
        float position_x = 0.0f;
        float position_y = 0.0f;
        float position_z = 0.0f;
        float normal_x = 1.0f;
        float normal_y = 1.0f;
        float normal_z = 1.0f;
        record >> position_x >> position_y >> position_z >> normal_x >> normal_y >> normal_z;
        expect("Graphdeco normal placeholders are zero", written.success && record.good() &&
                                                             normal_x == 0.0f && normal_y == 0.0f &&
                                                             normal_z == 0.0f);
    }
    {
        auto input = make_data(1, 0, rng);
        PlyWriteConfig config = canonical_write_config();
        config.format = PlyFormat::Ascii;
        std::vector<std::uint8_t> bytes;
        const auto written = PlyWriter{}.writeToBuffer(bytes, input, config);
        const char trailing[] = "1 2 3\n";
        bytes.insert(bytes.end(), std::begin(trailing), std::end(trailing) - 1);
        const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("ASCII PLY rejects trailing records", written.success && !read.success);
    }
    {
        const std::string unknown = "ply\nformat ascii 1.0\nunknown directive\nelement vertex 0\n"
                                    "property float x\nend_header\n";
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(unknown.data()), unknown.size());
        expect("PLY rejects unknown header directives", !read.success);
    }
    {
        const std::string unknown_double =
            "ply\nformat ascii 1.0\nelement vertex 2\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
            "property float opacity\n"
            "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
            "property float rot_0\nproperty float rot_1\nproperty float rot_2\n"
            "property float rot_3\nproperty double vendor_value\nend_header\n"
            "0 0 0 0 0 0 0 0 0 0 1 0 0 0 1e300\n"
            "0 0 0 0 0 0 0 0 0 0 1 0 0 0 1e-300\n";
        const auto read =
            PlyReader{}.readFromBuffer(reinterpret_cast<const std::uint8_t*>(unknown_double.data()),
                                       unknown_double.size(), graphdeco_read_config());
        expect("dropped float64 properties do not narrow into the canonical model",
               read.success && read.data.has_value() && read.data->size() == 2 &&
                   has_loss(read.losses, loss_code::kUnknownPropertyDropped));
    }
    {
        const std::string non_ascii =
            "ply\nformat ascii 1.0\ncomment invalid \xC3\xA9\nelement vertex 0\n"
            "property float x\nend_header\n";
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(non_ascii.data()), non_ascii.size());
        expect("PLY rejects non-ASCII header bytes", !read.success);
    }
    {
        const std::string faces = "ply\nformat ascii 1.0\nelement vertex 1\n"
                                  "property float x\nproperty float y\nproperty float z\n"
                                  "element face 1\nproperty list uchar int vertex_indices\n"
                                  "end_header\n0 0 0\n3 0 0 0\n";
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(faces.data()), faces.size());
        expect("Gaussian PLY rejects unparsed non-vertex data", !read.success);
    }
    {
        // Sized construction keeps the four NUL payload bytes; the const char* constructor
        // would stop at the first NUL and reduce this to an empty-payload case.
        static const char huge_bytes[] =
            "ply\nformat binary_little_endian 1.0\nelement vertex 4294967295\n"
            "property float x\nend_header\n\0\0\0\0";
        const std::string huge(huge_bytes, sizeof(huge_bytes) - 1);
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(huge.data()), huge.size());
        expect("huge declaration versus tiny payload", !read.success);
    }

    std::printf(failures == 0 ? "\n  ALL CANONICAL PLY TESTS PASSED\n" : "\n  %d FAILURES\n",
                failures);
    return failures == 0 ? 0 : 1;
}
