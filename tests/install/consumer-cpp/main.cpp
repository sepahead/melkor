// Test the installed C ABI from a standalone C++ project.

#include <melkor/c/melkor.h>

#include <iostream>
#include <string>

int main() {
    static_assert(sizeof(melkor_status) == sizeof(uint32_t));
    static_assert(sizeof(enum melkor_status) == sizeof(uint32_t));
    static_assert(MELKOR_PLY_PROFILE_DA3_GAUSSIAN == UINT32_C(3));

    const melkor_status typedef_status = MELKOR_RESOURCE_LIMIT;
    const enum melkor_status tagged_status = MELKOR_RESOURCE_LIMIT;
    melkor_status (*const typedef_get_version)(melkor_version_info*) noexcept = &melkor_get_version;
    enum melkor_status (*const tagged_get_version)(melkor_version_info*) noexcept =
        &melkor_get_version;
    (void)typedef_get_version;
    (void)tagged_get_version;
    if (typedef_status != tagged_status) {
        std::cerr << "The status declarations have incorrect values.\n";
        return 1;
    }

    melkor_version_info info{};
    info.struct_size = sizeof(info);
    if (melkor_get_version(&info) != MELKOR_OK) {
        std::cerr << "melkor_get_version failed\n";
        return 1;
    }
    if (info.abi_version != MELKOR_C_ABI_VERSION) {
        std::cerr << "The C header and the linked library use different ABI versions.\n";
        return 1;
    }
    if (std::string(melkor_status_string(MELKOR_RESOURCE_LIMIT)) != "resource_limit") {
        std::cerr << "melkor_status_string returned an incorrect value.\n";
        return 1;
    }

    std::cout << "Melkor C ABI consumed from C++ successfully.\n";
    std::cout << "  version: " << info.version_string << '\n';
    std::cout << "  abi:     " << info.abi_version << '\n';
    return 0;
}
