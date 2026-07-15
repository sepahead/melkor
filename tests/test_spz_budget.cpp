// Tests that the SPZ decoder charges the compressed input against a budget before decoding.
//
// The decoded allocation happens inside vendored upstream (bounded there at 10M points); melkor's
// contribution is to refuse an over-large compressed stream by policy, consistently with the PLY
// and glTF readers. This encodes a real cloud, then decodes it under tight and generous limits.
//
// Self-contained (no external test framework).

#include "melkor/limits.hpp"
#include "melkor/spz_encoder.hpp"

#include <cstdio>
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

}  // namespace

int main() {
#ifdef MELKOR_HAS_SPZ
    SplatBufferInput input;
    for (int i = 0; i < 8; ++i) {
        input.positions.push_back({static_cast<float>(i), 0.0f, 0.0f});
        input.scales.push_back({0.05f, 0.05f, 0.05f});
        input.rotations.push_back({});
        input.opacities.push_back(0.5f);
    }
    input.sh = ShBuffer::black(8).value();
    auto data = SplatData::create(std::move(input)).value();

    SpzEncoder encoder;
    std::vector<std::uint8_t> buffer;
    auto encoded = encoder.encodeToBuffer(buffer, data);
    CHECK(encoded.success);
    CHECK(!buffer.empty());

    SpzDecoder decoder;
    // Default limits accept it.
    auto ok = decoder.decodeFromBuffer(buffer.data(), buffer.size());
    CHECK(ok.success && ok.data.has_value() && ok.data->size() == 8);

    // An input-size limit below the compressed buffer refuses it before the decode.
    Limits tiny = Limits::for_profile(LimitsProfile::desktop);
    tiny.max_input_bytes = buffer.size() > 1 ? buffer.size() - 1 : 0;
    auto rejected = decoder.decodeFromBuffer(buffer.data(), buffer.size(), tiny);
    CHECK(!rejected.success);
#else
    std::printf("spz budget: skipped (MELKOR_HAS_SPZ off)\n");
    return 0;
#endif

    if (g_failures == 0) {
        std::printf("spz budget: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "spz budget: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
