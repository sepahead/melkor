// Tests for the container probe (WP06).
//
// The probe identifies a container from magic bytes, separately from the semantic profile, and
// is honest about ambiguity: an SPZ file is a gzip stream, so it can only be identified with low
// confidence from the raw bytes. These tests pin that honesty -- a probe that claimed certainty
// about a gzip file being SPZ would let a mislabelled file convert as the wrong format.
//
// Self-contained (no external test framework).

#include "melkor/format/probe.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

ContainerProbe probe(const std::string& s) {
    return probe_container(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

void test_glb_is_high_confidence() {
    auto p = probe("glTF\x02\x00\x00\x00");
    CHECK(p.format == FormatId::glb);
    CHECK(p.confidence == Confidence::high);
    CHECK(!p.evidence.empty());
}

void test_ply_is_high_confidence() {
    auto p = probe("ply\nformat ascii 1.0\n");
    CHECK(p.format == FormatId::ply);
    CHECK(p.confidence == Confidence::high);

    // Also accept CRLF headers.
    auto crlf = probe("ply\r\n");
    CHECK(crlf.format == FormatId::ply);
}

void test_gltf_json_is_low_confidence() {
    // A leading '{' looks like JSON glTF but is used by many things, so it must be LOW, not high:
    // a structural probe has to confirm it.
    auto p = probe("   {\"asset\":{\"version\":\"2.0\"}}");
    CHECK(p.format == FormatId::gltf);
    CHECK(p.confidence == Confidence::low);
}

void test_spz_gzip_is_low_confidence_honestly() {
    // This is the honesty test. An SPZ file is gzip-wrapped; from the first bytes it is
    // indistinguishable from any other gzip file. The probe must say 'spz' with LOW confidence,
    // never high -- claiming certainty here would let any gzip file be treated as SPZ.
    std::string gz;
    gz.push_back(static_cast<char>(0x1f));
    gz.push_back(static_cast<char>(0x8b));
    gz += "\x08\x00rest of a gzip stream";
    auto p = probe(gz);
    CHECK(p.format == FormatId::spz);
    CHECK(p.confidence == Confidence::low);  // NOT high
    CHECK(p.confidence != Confidence::certain);
}

void test_unknown_and_empty() {
    auto unknown = probe("this is just some text");
    CHECK(unknown.format == FormatId::unknown);
    CHECK(unknown.confidence == Confidence::none);

    auto empty = probe_container(nullptr, 0);
    CHECK(empty.format == FormatId::unknown);
    CHECK(empty.confidence == Confidence::none);
}

void test_short_input_does_not_over_read() {
    // Fewer bytes than a magic needs must lower confidence, never read past the buffer. "gl" is a
    // 2-byte prefix of "glTF"; it must NOT be identified as GLB.
    std::uint8_t two[2] = {'g', 'l'};
    auto p = probe_container(two, 2);
    CHECK(p.format != FormatId::glb);

    // A single gzip byte is not enough for the 2-byte gzip magic.
    std::uint8_t one[1] = {0x1f};
    auto p1 = probe_container(one, 1);
    CHECK(p1.format != FormatId::spz);
}

void test_suffix_mismatch_detection() {
    // A .ply suffix on a file whose bytes are glTF is a mismatch the caller must be told about.
    CHECK(suffix_matches(FormatId::ply, ".ply"));
    CHECK(!suffix_matches(FormatId::glb, ".ply"));
    CHECK(suffix_matches(FormatId::glb, ".glb"));
    CHECK(!suffix_matches(FormatId::unknown, ".ply"));
}

}  // namespace

int main() {
    test_glb_is_high_confidence();
    test_ply_is_high_confidence();
    test_gltf_json_is_low_confidence();
    test_spz_gzip_is_low_confidence_honestly();
    test_unknown_and_empty();
    test_short_input_does_not_over_read();
    test_suffix_mismatch_detection();

    if (g_failures == 0) {
        std::printf("container probe: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "container probe: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
