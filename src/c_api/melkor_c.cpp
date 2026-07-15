// Implementation of the Melkor C ABI (melkor/c/melkor.h).
//
// This translation unit is compiled into the shared library with MELKOR_BUILDING_LIBRARY
// defined, so the MELKOR_API functions are exported and nothing else is. It is the only
// exported surface of libmelkor: a consumer that links the shared library sees exactly these
// symbols, and the C++ implementation behind them stays private.
//
// No C++ exception may escape any function here. A C caller has no way to catch one, and an
// exception crossing the ABI boundary is undefined behaviour. Every entry point is wrapped so
// that an unexpected throw becomes MELKOR_INTERNAL_ERROR rather than a crash.

#include "melkor/c/melkor.h"

#include "melkor/error.hpp"
#include "melkor/version.h"

#include <cstring>

extern "C" {

const char* melkor_status_string(melkor_status status) {
    // Reuse the C++ mapping so the two cannot drift. The cast is safe because the C enum
    // values are defined to equal the C++ ErrorCode values.
    switch (status) {
        case MELKOR_OK:
            return "ok";
        case MELKOR_INVALID_ARGUMENT:
            return "invalid_argument";
        case MELKOR_INVALID_DATA:
            return "invalid_data";
        case MELKOR_UNSUPPORTED_FEATURE:
            return "unsupported_feature";
        case MELKOR_IO_ERROR:
            return "io_error";
        case MELKOR_RESOURCE_LIMIT:
            return "resource_limit";
        case MELKOR_BACKEND_UNAVAILABLE:
            return "backend_unavailable";
        case MELKOR_CANCELLED:
            return "cancelled";
        case MELKOR_INTERNAL_ERROR:
            return "internal_error";
    }
    return "unknown";
}

melkor_status melkor_get_version(melkor_version_info* info) {
    // struct_size must be at least large enough to hold the struct_size field itself: the
    // write-back below stores it as a full size_t, so a smaller value (1..sizeof(size_t)-1) would
    // write past a caller buffer that only allocated that many bytes.
    if (info == nullptr || info->struct_size < sizeof(info->struct_size)) {
        return MELKOR_INVALID_ARGUMENT;
    }

    // Fill only the fields that fit in the struct the caller compiled against. A caller built
    // against an older, shorter struct passes a smaller struct_size; writing past it would
    // corrupt their stack. This is the whole reason struct_size exists.
    melkor_version_info local{};
    local.struct_size = sizeof(melkor_version_info);
    local.version_string = MELKOR_VERSION_STRING;
    local.version_major = MELKOR_VERSION_MAJOR;
    local.version_minor = MELKOR_VERSION_MINOR;
    local.version_patch = MELKOR_VERSION_PATCH;
    local.abi_version = MELKOR_ABI_VERSION;
    local.inspect_schema_version = MELKOR_INSPECT_SCHEMA_VERSION;
    local.loss_schema_version = MELKOR_LOSS_SCHEMA_VERSION;
    local.build_commit = MELKOR_BUILD_COMMIT;

    const size_t copy_size =
        info->struct_size < sizeof(melkor_version_info) ? info->struct_size : sizeof(melkor_version_info);

    // Preserve the caller's struct_size, overwrite the rest up to what fits.
    const size_t caller_size = info->struct_size;
    std::memcpy(info, &local, copy_size);
    info->struct_size = caller_size;

    return MELKOR_OK;
}

}  // extern "C"
