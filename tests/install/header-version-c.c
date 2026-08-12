/* Confirm that the generated version header is self-contained in C99. */

#include <melkor/version.h>

uint64_t melkor_test_version_number(void) {
    return MELKOR_VERSION_NUMBER;
}
