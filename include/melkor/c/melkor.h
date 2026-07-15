/* Melkor C ABI.
 *
 * This is the stable binary boundary for the 2.x line. A program compiled against this header
 * and linked to libmelkor keeps working across 2.x patch and minor releases without
 * recompilation. The C++ headers under melkor/ (the .hpp files) are a source-level convenience
 * with a weaker promise: source-compatible within a minor line, but no cross-compiler binary
 * stability, because C++ has no stable ABI for STL types.
 *
 * The rules that make this ABI stable are load-bearing, not stylistic:
 *   - Opaque handles only. A caller never sees the layout of a Melkor object, so the layout
 *     may change freely.
 *   - Fixed-width integers, never `int`/`long`, whose widths differ across platforms.
 *   - No C++ exceptions cross this boundary. Every function reports failure by return code.
 *   - Explicit ownership. Every function that returns allocated memory has a matching free.
 *   - Struct arguments carry a `struct_size` so the library can tell which version of a struct
 *     the caller was compiled against, and new trailing fields can be added compatibly.
 *
 * This ABI is intentionally small in 2.0.0-dev. It grows by ADDING functions, never by
 * changing the meaning of an existing one. It is frozen at the v2.0.0 release, not before,
 * so that it is not locked around types that later work proves wrong.
 */

#ifndef MELKOR_C_MELKOR_H
#define MELKOR_C_MELKOR_H

#include <stddef.h>
#include <stdint.h>

#include "melkor/version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Symbol visibility / import-export.
 *
 * On Windows a symbol must be marked dllexport when building the library and dllimport when
 * consuming it. On ELF/Mach-O the library is built with hidden visibility by default and only
 * MELKOR_API symbols are exported, so the exported surface is exactly this header and nothing
 * leaks by accident. */
#if defined(_WIN32)
#  if defined(MELKOR_BUILDING_LIBRARY)
#    define MELKOR_API __declspec(dllexport)
#  else
#    define MELKOR_API __declspec(dllimport)
#  endif
#else
#  if defined(MELKOR_BUILDING_LIBRARY)
#    define MELKOR_API __attribute__((visibility("default")))
#  else
#    define MELKOR_API
#  endif
#endif

/* Error classes. These match melkor::ErrorCode and the CLI exit classes one-to-one, and a
 * value never changes meaning between releases. */
typedef enum melkor_status {
    MELKOR_OK = 0,
    MELKOR_INVALID_ARGUMENT = 2,
    MELKOR_INVALID_DATA = 3,
    MELKOR_UNSUPPORTED_FEATURE = 4,
    MELKOR_IO_ERROR = 5,
    MELKOR_RESOURCE_LIMIT = 6,
    MELKOR_BACKEND_UNAVAILABLE = 7,
    MELKOR_CANCELLED = 8,
    MELKOR_INTERNAL_ERROR = 9
} melkor_status;

/* A stable, human-readable name for a status code. The returned pointer is a static string
 * owned by the library; the caller must not free it. Never returns NULL. */
MELKOR_API const char* melkor_status_string(melkor_status status);

/* Version information about the linked library.
 *
 * struct_size lets the library detect which version of this struct the caller compiled
 * against. A caller sets it to sizeof(melkor_version_info) before the call; the library fills
 * only the fields that fit. This is how trailing fields can be added in a later release
 * without breaking a caller compiled against the older struct. */
typedef struct melkor_version_info {
    size_t struct_size;

    /* Full semantic version of the linked library, e.g. "2.0.0" or "2.0.0-rc.2". Static
     * string owned by the library; do not free. */
    const char* version_string;

    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;

    /* Stable ABI version. A program built against ABI N works with any library reporting the
     * same N. */
    uint32_t abi_version;

    /* JSON document schema versions, so a consumer can check compatibility before parsing. */
    uint32_t inspect_schema_version;
    uint32_t loss_schema_version;

    /* Exact source commit, or "" when the build did not record one. Do not free. */
    const char* build_commit;
} melkor_version_info;

/* Fills *info with the linked library's version. The caller must set info->struct_size to
 * sizeof(melkor_version_info) first. Returns MELKOR_INVALID_ARGUMENT if info is NULL or its
 * struct_size is zero. */
MELKOR_API melkor_status melkor_get_version(melkor_version_info* info);

/* The ABI version this HEADER declares, as a compile-time constant. Compare it against the
 * abi_version returned by melkor_get_version() to detect a header/library mismatch at
 * startup rather than through a crash later. */
#define MELKOR_C_ABI_VERSION MELKOR_ABI_VERSION

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MELKOR_C_MELKOR_H */
