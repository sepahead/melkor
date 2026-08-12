// Confirm that the C ABI header parses in a C++98 translation unit.

#include <melkor/c/melkor.h>

void use_melkor_c_header() {
    melkor_status typedef_status = MELKOR_OK;
    enum melkor_status tagged_status = MELKOR_OK;
    melkor_ply_inspect_options options = MELKOR_PLY_INSPECT_OPTIONS_INIT;
    melkor_ply_info info = MELKOR_PLY_INFO_INIT;
    (void)typedef_status;
    (void)tagged_status;
    (void)options;
    (void)info;
}
