// Confirm that the generated version header is self-contained in C++98.

#include <melkor/version.h>

uint64_t melkor_test_version_number() {
    return MELKOR_VERSION_NUMBER;
}
