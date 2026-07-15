// Tests that the SPZ decoder charges the compressed input against a budget before decoding.
//
// The decoded allocation happens inside vendored upstream (bounded there at 10M points); melkor's
// contribution is to refuse an over-large compressed stream by policy, consistently with the PLY
// and glTF readers. This encodes a real cloud, then decodes it under tight and generous limits.
//
// Self-contained (no external test framework).

#include "melkor/gaussian_data.hpp"
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
    // A small valid cloud.
    GaussianCloud cloud;
    for (int i = 0; i < 8; ++i) {
        GaussianSplat s{};
        s.x = static_cast<float>(i);
        s.f_dc_0 = s.f_dc_1 = s.f_dc_2 = 0.0f;
        s.opacity = 0.5f;
        s.scale_0 = s.scale_1 = s.scale_2 = -3.0f;
        s.rot_0 = 1.0f;
        cloud.addSplat(s);
    }

    SpzEncoder encoder;
    std::vector<std::uint8_t> buffer;
    auto encoded = encoder.encodeToBuffer(buffer, cloud);
    CHECK(encoded.success);
    CHECK(!buffer.empty());

    SpzDecoder decoder;
    // Default limits accept it.
    auto ok = decoder.decodeFromBuffer(buffer.data(), buffer.size());
    CHECK(ok.success);

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
