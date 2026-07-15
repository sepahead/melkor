// Canonical PLY boundary and parser regression tests.
//
// PLY stores graphdeco training domains (log scale, logit opacity, wxyz rotation, and
// channel-major higher SH). SplatData stores linear scale, linear opacity, xyzw rotation, and
// coefficient/channel-interleaved SH. These tests pin both directions independently so a
// symmetric double-conversion or double-transpose cannot make a round trip pass by accident.

#include "melkor/math/color.hpp"
#include "melkor/math/quaternion.hpp"
#include "melkor/ply_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
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

bool has_diagnostic(const melkor::PlyWriteResult& result, const char* code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [&](const auto& diagnostic) { return diagnostic.code == code; });
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
    for (std::uint32_t degree : {0u, 1u, 2u, 3u}) {
        for (std::size_t count : {0u, 1u, 17u, 100u}) {
            auto input = make_data(count, degree, rng);
            PlyWriteConfig config;
            config.include_sh_rest = degree != 0;
            std::vector<std::uint8_t> bytes;
            const auto written = PlyWriter{}.writeToBuffer(bytes, input, config);
            const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
            const bool ok = written.success && read.success && read.data.has_value() &&
                            same_data(input, *read.data);
            char label[64];
            std::snprintf(label, sizeof(label), "degree=%u count=%zu", degree, count);
            expect(label, ok);
        }
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
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(source.data()), source.size());
        const bool ok =
            read.success && read.data.has_value() && read.data->size() == 1 &&
            approx(read.data->opacities()[0], 0.5f) && approx(read.data->scales()[0].x, 0.1f) &&
            approx(read.data->scales()[0].y, 0.2f) && approx(read.data->scales()[0].z, 0.3f) &&
            approx(read.data->rotations()[0].x, 0.5f) && approx(read.data->rotations()[0].w, 0.5f);
        expect("training domains decode exactly once", ok);
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
        source += "end_header\n0 0 0 10 11 12 20 21 22 30 31 32 40 41 42\n";
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(source.data()), source.size());
        bool ok = read.success && read.data.has_value() && read.data->sh().degree() == 1;
        const std::vector<float> expected{10, 11, 12, 20, 30, 40, 21, 31, 41, 22, 32, 42};
        if (ok)
            ok = read.data->sh().raw() == expected;
        expect("channel-major higher SH transposes to canonical layout", ok);
    }
    {
        auto endpoints = make_data(2, 0, rng);
        auto edit = endpoints.edit();
        edit.set_opacities({0.0f, 1.0f});
        endpoints = edit.commit().value();
        std::vector<std::uint8_t> bytes;
        const auto written = PlyWriter{}.writeToBuffer(bytes, endpoints);
        const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("endpoint clamp is explicit and finite",
               written.success && has_diagnostic(written, "MK1210_PLY_OPACITY_ENDPOINT_CLAMPED") &&
                   read.success && read.data.has_value() && read.data->opacities()[0] > 0.0f &&
                   read.data->opacities()[1] < 1.0f);
    }
    {
        auto degree_one = make_data(1, 1, rng);
        std::vector<std::uint8_t> bytes;
        const auto written = PlyWriter{}.writeToBuffer(bytes, degree_one);
        const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("configured SH omission is reported",
               written.success && has_diagnostic(written, "MK1211_PLY_SH_OMITTED") &&
                   read.success && read.data.has_value() && read.data->sh().degree() == 0);
    }
    {
        auto degree_four = make_data(1, 4, rng);
        PlyWriteConfig config;
        config.include_sh_rest = true;
        std::vector<std::uint8_t> bytes;
        expect("degree 4 cannot be silently truncated",
               !PlyWriter{}.writeToBuffer(bytes, degree_four, config).success && bytes.empty());
    }
    {
        const std::string non_unit = "ply\nformat ascii 1.0\nelement vertex 1\n"
                                     "property float x\nproperty float y\nproperty float z\n"
                                     "property float rot_0\nproperty float rot_1\nproperty float "
                                     "rot_2\nproperty float rot_3\n"
                                     "end_header\n0 0 0 2 0 0 0\n";
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(non_unit.data()), non_unit.size());
        expect("far non-unit quaternion fails closed", !read.success && !read.data.has_value());
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
                      "property float x\nproperty float y\nproperty float z\nend_header\n");
        push_be_float(bytes, 1.0f);
        push_be_float(bytes, -2.5f);
        push_be_float(bytes, 3.25f);
        const auto read = PlyReader{}.readFromBuffer(bytes.data(), bytes.size());
        expect("big-endian binary", read.success && read.data.has_value() &&
                                        approx(read.data->positions()[0].x, 1.0f) &&
                                        approx(read.data->positions()[0].y, -2.5f) &&
                                        approx(read.data->positions()[0].z, 3.25f));
    }
    {
        const std::string crlf =
            "ply\r\nformat ascii 1.0\r\nelement vertex 1\r\n"
            "property float x\r\nproperty float y\r\nproperty float z\r\n"
            "comment end_header is not a terminator here\r\nend_header\r\n1 2 3\r\n";
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(crlf.data()), crlf.size());
        expect("CRLF and end_header in comment",
               read.success && read.data.has_value() && approx(read.data->positions()[0].y, 2.0f));
    }
    {
        const std::string huge = "ply\nformat binary_little_endian 1.0\nelement vertex 4294967295\n"
                                 "property float x\nend_header\n\0\0\0\0";
        const auto read = PlyReader{}.readFromBuffer(
            reinterpret_cast<const std::uint8_t*>(huge.data()), huge.size());
        expect("huge declaration versus tiny payload", !read.success);
    }

    std::printf(failures == 0 ? "\n  ALL CANONICAL PLY TESTS PASSED\n" : "\n  %d FAILURES\n",
                failures);
    return failures == 0 ? 0 : 1;
}
