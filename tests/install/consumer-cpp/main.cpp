// A minimal C++ program that consumes the installed Melkor SDK.
//
// It proves the C++ public headers install and compile against a relocated package. It uses
// only header-inline facilities -- the Result<T> template and the version macros -- because the
// compiled C++ API is not yet a frozen exported ABI. The supported cross-language boundary is
// the C ABI, exercised by the C consumer; this test guards the C++ headers' installability.

#include <melkor/c/melkor.h>
#include <melkor/error.hpp>
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

    // And the C ABI is linkable from C++ too.
    if (std::string(melkor_status_string(MELKOR_RESOURCE_LIMIT)) != "resource_limit") {
        std::cerr << "unexpected status string from the C ABI\n";
        return 1;
    }

    return 0;
}
