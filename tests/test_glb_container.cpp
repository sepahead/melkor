// Tests for GLB container framing.
//
// The GLB header and chunk lengths are all attacker-controlled, so the tests here are mostly
// adversarial: a lying total length, a chunk length that would overflow, a misaligned chunk, the
// JSON chunk missing or not first. Each must produce a clean failure with no out-of-bounds read
// (run this target under ASan to enforce the latter). The positive tests pin round-tripping with
// build_glb.
//
// Self-contained (no external test framework).

#include "melkor/format/glb_container.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace melkor;
namespace glb = melkor::format::glb;

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

void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
}

// Builds a raw GLB by hand so tests can lie about individual fields. `total_override` < 0 means
// "use the real assembled length".
std::vector<std::uint8_t> raw_glb(std::uint32_t magic, std::uint32_t version,
                                  long long total_override,
                                  const std::vector<std::pair<std::uint32_t, std::string>>& chunks) {
    std::vector<std::uint8_t> body;
    for (const auto& [type, payload] : chunks) {
        put_u32(body, static_cast<std::uint32_t>(payload.size()));
        put_u32(body, type);
        body.insert(body.end(), payload.begin(), payload.end());
    }
    std::vector<std::uint8_t> out;
    put_u32(out, magic);
    put_u32(out, version);
    const std::uint32_t real_total = static_cast<std::uint32_t>(12 + body.size());
    put_u32(out, total_override < 0 ? real_total : static_cast<std::uint32_t>(total_override));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

auto parse(const std::vector<std::uint8_t>& v) { return glb::parse_glb(v.data(), v.size()); }

void test_rejects_short_and_bad_magic() {
    std::vector<std::uint8_t> empty;
    CHECK(!glb::parse_glb(nullptr, 0).has_value());
    CHECK(!parse(empty).has_value());
    std::vector<std::uint8_t> eleven(11, 0);
    CHECK(!parse(eleven).has_value());

    auto bad_magic = raw_glb(0xDEADBEEF, 2, -1, {{glb::kChunkTypeJson, "{}  "}});
    CHECK(!parse(bad_magic).has_value());
}

void test_rejects_bad_version() {
    auto v1 = raw_glb(glb::kMagic, 1, -1, {{glb::kChunkTypeJson, "{}  "}});
    CHECK(!parse(v1).has_value());
    auto v3 = raw_glb(glb::kMagic, 3, -1, {{glb::kChunkTypeJson, "{}  "}});
    CHECK(!parse(v3).has_value());
}

void test_rejects_lying_total_length() {
    // Declared length larger than the buffer => truncated => reject, no over-read.
    auto truncated = raw_glb(glb::kMagic, 2, 1'000'000, {{glb::kChunkTypeJson, "{}  "}});
    CHECK(!parse(truncated).has_value());
    // Declared length smaller than the header => reject.
    auto tiny = raw_glb(glb::kMagic, 2, 4, {{glb::kChunkTypeJson, "{}  "}});
    CHECK(!parse(tiny).has_value());
}

void test_rejects_overflowing_chunk_length() {
    // A chunk claiming 0xFFFFFFFC bytes must not wrap or over-read; it exceeds the file.
    std::vector<std::uint8_t> v;
    put_u32(v, glb::kMagic);
    put_u32(v, 2);
    put_u32(v, 24);                 // declared total
    put_u32(v, 0xFFFFFFFCu);        // chunk length: enormous, 4-aligned
    put_u32(v, glb::kChunkTypeJson);
    v.push_back('{');
    v.push_back('}');
    v.push_back(' ');
    v.push_back(' ');
    CHECK(!parse(v).has_value());
}

void test_rejects_misaligned_chunk() {
    // Chunk length 3 is not a multiple of 4.
    auto misaligned = raw_glb(glb::kMagic, 2, -1, {{glb::kChunkTypeJson, "{ }"}});  // 3 bytes
    CHECK(!parse(misaligned).has_value());
}

void test_rejects_chunk_header_straddling_end() {
    // A declared length that leaves fewer than 8 bytes for a second chunk header.
    std::vector<std::uint8_t> v;
    put_u32(v, glb::kMagic);
    put_u32(v, 2);
    // header(12) + json chunk header(8) + json(4) = 24, then claim 4 more bytes of a chunk that
    // cannot hold its own 8-byte header.
    put_u32(v, 28);
    put_u32(v, 4);
    put_u32(v, glb::kChunkTypeJson);
    v.insert(v.end(), {'{', '}', ' ', ' '});
    v.insert(v.end(), {0x00, 0x00, 0x00, 0x00});  // 4 trailing bytes: not a full chunk header
    CHECK(!parse(v).has_value());
}

void test_rejects_json_not_first_or_missing() {
    // First chunk is BIN, not JSON.
    auto bin_first = raw_glb(glb::kMagic, 2, -1,
                             {{glb::kChunkTypeBin, "\x00\x00\x00\x00"}, {glb::kChunkTypeJson, "{}  "}});
    CHECK(!parse(bin_first).has_value());

    // No JSON chunk at all (only an unknown chunk).
    auto no_json = raw_glb(glb::kMagic, 2, -1, {{0x12345678u, "\x00\x00\x00\x00"}});
    CHECK(!parse(no_json).has_value());

    // Two JSON chunks.
    auto two_json = raw_glb(glb::kMagic, 2, -1,
                            {{glb::kChunkTypeJson, "{}  "}, {glb::kChunkTypeJson, "{}  "}});
    CHECK(!parse(two_json).has_value());
}

void test_accepts_json_only_and_json_plus_bin() {
    auto json_only = raw_glb(glb::kMagic, 2, -1, {{glb::kChunkTypeJson, "{\"a\":1}"  " "}});
    auto r1 = parse(json_only);
    CHECK(r1.has_value());
    if (r1.has_value()) {
        CHECK(!r1.value().bin.has_value());
        CHECK(r1.value().json.length == 8);  // 7 payload + 1 space pad in the literal above
    }

    std::string bin4 = std::string(8, '\x00');
    auto json_bin = raw_glb(glb::kMagic, 2, -1,
                            {{glb::kChunkTypeJson, "{}  "}, {glb::kChunkTypeBin, bin4}});
    auto r2 = parse(json_bin);
    CHECK(r2.has_value());
    if (r2.has_value()) {
        CHECK(r2.value().bin.has_value());
        CHECK(r2.value().bin->length == 8);
    }
}

void test_skips_unknown_trailing_chunk() {
    // A recognised JSON chunk followed by an unknown chunk: the unknown one is ignored, and the
    // overall parse still succeeds with the JSON recovered.
    auto with_unknown = raw_glb(glb::kMagic, 2, -1,
                                {{glb::kChunkTypeJson, "{}  "}, {0x99887766u, "\x01\x02\x03\x04"}});
    auto r = parse(with_unknown);
    CHECK(r.has_value());
    if (r.has_value()) CHECK(!r.value().bin.has_value());
}

void test_build_roundtrip() {
    const std::string json = "{\"asset\":{\"version\":\"2.0\"}}";  // 27 bytes, needs 1 pad byte
    const std::vector<std::uint8_t> bin = {1, 2, 3, 4, 5};        // 5 bytes, needs 3 pad bytes

    // JSON only.
    auto g1 = glb::build_glb(json, nullptr, 0);
    CHECK(g1.has_value());
    if (g1.has_value()) {
        CHECK(g1.value().size() % 4 == 0);
        auto p = glb::parse_glb(g1.value().data(), g1.value().size());
        CHECK(p.has_value());
        if (p.has_value()) {
            CHECK(!p.value().bin.has_value());
            // The JSON payload, with trailing pad spaces stripped, must equal the original.
            const auto& range = p.value().json;
            std::string recovered(reinterpret_cast<const char*>(g1.value().data() + range.offset),
                                  static_cast<std::size_t>(range.length));
            while (!recovered.empty() && recovered.back() == ' ') recovered.pop_back();
            CHECK(recovered == json);
        }
    }

    // JSON + BIN.
    auto g2 = glb::build_glb(json, bin.data(), bin.size());
    CHECK(g2.has_value());
    if (g2.has_value()) {
        CHECK(g2.value().size() % 4 == 0);
        auto p = glb::parse_glb(g2.value().data(), g2.value().size());
        CHECK(p.has_value());
        if (p.has_value() && p.value().bin.has_value()) {
            const auto& br = *p.value().bin;
            CHECK(br.length == 8);  // 5 + 3 pad
            CHECK(std::memcmp(g2.value().data() + br.offset, bin.data(), bin.size()) == 0);
        }
    }
}

}  // namespace

int main() {
    test_rejects_short_and_bad_magic();
    test_rejects_bad_version();
    test_rejects_lying_total_length();
    test_rejects_overflowing_chunk_length();
    test_rejects_misaligned_chunk();
    test_rejects_chunk_header_straddling_end();
    test_rejects_json_not_first_or_missing();
    test_accepts_json_only_and_json_plus_bin();
    test_skips_unknown_trailing_chunk();
    test_build_roundtrip();

    if (g_failures == 0) {
        std::printf("glb container: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "glb container: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
