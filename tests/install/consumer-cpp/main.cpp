// A minimal C++ program that consumes the installed Melkor SDK.
//
// It proves the C++ public headers install and compile against a relocated package. It uses
// only header-inline facilities because the compiled C++ API is not yet a frozen exported ABI.
// The supported cross-language boundary is the C ABI, exercised by the C consumer; this test
// also guards the canonical scene and format headers' standalone installability.

#include <melkor/c/melkor.h>
#include <melkor/cloud_inspector.hpp>
#include <melkor/error.hpp>
#include <melkor/format/gltf_reader.hpp>
#include <melkor/format/gltf_writer.hpp>
#include <melkor/glb_reader.hpp>
#include <melkor/ply_writer.hpp>
#include <melkor/provenance.hpp>
#include <melkor/scene.hpp>
#include <melkor/spz_encoder.hpp>
#include <melkor/version.h>

#include <iostream>
#include <string>

int main() {
    // The version header is present and its macros expand.
    std::cout << "Melkor C++ SDK headers consumed successfully.\n";
    std::cout << "  version: " << MELKOR_VERSION_STRING << "\n";
    std::cout << "  abi:     " << MELKOR_ABI_VERSION << "\n";

    // Result<T> is entirely header-inline, so a consumer can use it without any exported C++
    // symbol. Exercise both the success and failure constructors and the diagnostics contract.
    auto ok = melkor::Result<int>::success(42);
    if (!ok.has_value() || ok.value() != 42) {
        std::cerr << "Result<int>::success did not round-trip\n";
        return 1;
    }

    melkor::Diagnostic diag("MK_TEST", melkor::Severity::error, "a test diagnostic");
    auto failed = melkor::Result<int>::failure(melkor::ErrorCode::invalid_data, std::move(diag));
    if (failed.has_value() || failed.error_code() != melkor::ErrorCode::invalid_data) {
        std::cerr << "Result<int>::failure did not carry its error code\n";
        return 1;
    }
    if (failed.diagnostics().size() != 1 || failed.diagnostics()[0].code != "MK_TEST") {
        std::cerr << "Result did not carry its diagnostic\n";
        return 1;
    }

    // The public C++ scene boundary is canonical SplatData and its linear-domain input. Keep
    // this header-only so the test does not accidentally promise an exported C++ ABI.
    melkor::SplatBufferInput canonical_input;
    canonical_input.positions.push_back({1.0f, 2.0f, 3.0f});
    canonical_input.scales.push_back({0.01f, 0.02f, 0.03f});
    canonical_input.rotations.push_back({0.0f, 0.0f, 0.0f, 1.0f});
    canonical_input.opacities.push_back(0.75f);
    if (canonical_input.positions.size() != canonical_input.opacities.size()) {
        std::cerr << "canonical scene headers did not compile or retain parallel-array shape\n";
        return 1;
    }

    // Pin representative configuration types from every migrated legacy-format header. Merely
    // naming these types proves their dependency closure is present in the clean install.
    melkor::GlbConversionConfig glb;
    melkor::PlyWriteConfig ply;
    const bool spz_available = melkor::isSpzAvailable();
    if (glb.default_scale <= 0.0f || ply.comment.empty()) {
        std::cerr << "canonical format configuration defaults are invalid\n";
        return 1;
    }
    (void)spz_available;

    // And the C ABI is linkable from C++ too.
    if (std::string(melkor_status_string(MELKOR_RESOURCE_LIMIT)) != "resource_limit") {
        std::cerr << "unexpected status string from the C ABI\n";
        return 1;
    }

    return 0;
}
