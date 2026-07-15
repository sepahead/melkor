/* A minimal C program that consumes the installed Melkor SDK through its stable C ABI.
 *
 * If this compiles, links, and runs against an installed prefix, then the SDK genuinely
 * installs: the header is found, the shared library is found and linked, and the exported
 * symbols resolve. That is the concrete evidence for P0-04. */

#include <melkor/c/melkor.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    /* The header the consumer compiled against and the library it linked must agree on the
     * ABI version. Checking at startup turns a mismatch into a clear message instead of a
     * crash somewhere later. */
    melkor_version_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);

    melkor_status status = melkor_get_version(&info);
    if (status != MELKOR_OK) {
        fprintf(stderr, "melkor_get_version failed: %s\n", melkor_status_string(status));
        return 1;
    }

    printf("Melkor SDK consumed successfully.\n");
    printf("  version: %s\n", info.version_string);
    printf("  abi:     %u\n", info.abi_version);

    if (info.abi_version != MELKOR_C_ABI_VERSION) {
        fprintf(stderr,
                "ABI mismatch: header declares %u, library reports %u\n",
                (unsigned)MELKOR_C_ABI_VERSION, info.abi_version);
        return 1;
    }

    /* Exercise the status-string mapping so the whole exported surface is touched. */
    if (strcmp(melkor_status_string(MELKOR_RESOURCE_LIMIT), "resource_limit") != 0) {
        fprintf(stderr, "unexpected status string\n");
        return 1;
    }

    return 0;
}
