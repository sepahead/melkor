/* Melkor C ABI.
 *
 * This header defines the supported installed SDK surface. The C++ headers are
 * implementation files and are not part of the installed SDK.
 * The C header requires C99 or a later C standard.
 *
 * Follow these ABI rules:
 *
 * - No C++ exception crosses this boundary.
 * - Each function returns a status code.
 * - Each public structure starts with struct_size.
 * - The library ignores unknown trailing structure fields.
 * - The library owns all returned string pointers.
 */

#ifndef MELKOR_C_MELKOR_H
#define MELKOR_C_MELKOR_H

#include <stddef.h>
#include <stdint.h>

#include "melkor/version.h"

#ifdef __cplusplus
extern "C" {
#if __cplusplus >= 201103L || (defined(_MSC_VER) && _MSC_VER >= 1900)
#define MELKOR_NOEXCEPT noexcept
#else
#define MELKOR_NOEXCEPT
#endif
#else
#define MELKOR_NOEXCEPT
#endif

/* Export public functions and hide implementation symbols. */
#if defined(_WIN32)
#if defined(MELKOR_BUILDING_LIBRARY)
#define MELKOR_API __declspec(dllexport)
#else
#define MELKOR_API __declspec(dllimport)
#endif
#else
#if defined(MELKOR_BUILDING_LIBRARY)
#define MELKOR_API __attribute__((visibility("default")))
#else
#define MELKOR_API
#endif
#endif

/* These values identify the same error classes as the CLI. They are not CLI exit codes.
 * A status value does not change meaning. */
#if defined(__cplusplus) && (__cplusplus >= 201103L || (defined(_MSC_VER) && _MSC_VER >= 1900))
typedef enum melkor_status : uint32_t {
#else
typedef enum melkor_status {
#endif
    MELKOR_OK = 0,
    MELKOR_INVALID_ARGUMENT = 2,
    MELKOR_INVALID_DATA = 3,
    MELKOR_UNSUPPORTED_FEATURE = 4,
    MELKOR_IO_ERROR = 5,
    MELKOR_RESOURCE_LIMIT = 6,
    /* Reserved for ABI compatibility. Current operations do not return this value. */
    MELKOR_BACKEND_UNAVAILABLE = 7,
    MELKOR_CANCELLED = 8,
    MELKOR_INTERNAL_ERROR = 9
} melkor_status;

/* C++11 and later set the enum storage type to uint32_t.
 * Other language modes reject enum storage that is not 32 bits. */
typedef char
    melkor_status_storage_must_be_32_bits[sizeof(melkor_status) == sizeof(uint32_t) ? 1 : -1];

/* Get a stable name for a status code. The library owns the returned string. */
MELKOR_API const char* melkor_status_string(melkor_status status) MELKOR_NOEXCEPT;

/* Information about the linked library. */
typedef struct melkor_version_info {
    size_t struct_size;

    /* The library owns these strings. */
    const char* version_string;

    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint32_t abi_version;
    uint32_t inspect_schema_version;
    uint32_t loss_schema_version;

    /* This string is empty when the build did not record a commit. */
    const char* build_commit;
} melkor_version_info;

/*
 * Set info->struct_size before this call.
 *
 * The function writes each complete field that fits in struct_size. It
 * preserves the caller's struct_size value.
 */
MELKOR_API melkor_status melkor_get_version(melkor_version_info* info) MELKOR_NOEXCEPT;

/* The ABI version that this header declares. */
#define MELKOR_C_ABI_VERSION MELKOR_ABI_VERSION

/* Named resource profiles for data operations. */
typedef uint32_t melkor_limits_profile;

#define MELKOR_LIMITS_PROFILE_WEB UINT32_C(1)
#define MELKOR_LIMITS_PROFILE_DESKTOP UINT32_C(2)
#define MELKOR_LIMITS_PROFILE_SERVER UINT32_C(3)

/* Supported PLY semantic profiles. */
typedef uint32_t melkor_ply_profile;

#define MELKOR_PLY_PROFILE_AUTO UINT32_C(0)
#define MELKOR_PLY_PROFILE_CANONICAL UINT32_C(1)
#define MELKOR_PLY_PROFILE_GRAPHDECO_3DGS UINT32_C(2)
#define MELKOR_PLY_PROFILE_DA3_GAUSSIAN UINT32_C(3)

/* Supported source coordinate frames. */
typedef uint32_t melkor_coordinate_frame;

#define MELKOR_COORDINATE_FRAME_AUTO UINT32_C(0)
#define MELKOR_COORDINATE_FRAME_GLTF_LUF UINT32_C(1)
#define MELKOR_COORDINATE_FRAME_PLY_RDF UINT32_C(2)
#define MELKOR_COORDINATE_FRAME_SPZ_RUB UINT32_C(3)

/* Supported source color spaces. */
typedef uint32_t melkor_color_space;

#define MELKOR_COLOR_SPACE_AUTO UINT32_C(0)
#define MELKOR_COLOR_SPACE_SRGB_REC709_DISPLAY UINT32_C(1)
#define MELKOR_COLOR_SPACE_LIN_REC709_DISPLAY UINT32_C(2)

/* Optional Boolean values in result structures. */
typedef uint32_t melkor_optional_bool;

#define MELKOR_OPTIONAL_BOOL_UNKNOWN UINT32_C(0)
#define MELKOR_OPTIONAL_BOOL_FALSE UINT32_C(1)
#define MELKOR_OPTIONAL_BOOL_TRUE UINT32_C(2)

/* Options for melkor_inspect_ply_file. */
typedef struct melkor_ply_inspect_options {
    size_t struct_size;
    melkor_limits_profile limits_profile;
    melkor_ply_profile profile;
    melkor_coordinate_frame source_frame;
    melkor_color_space source_color_space;

    /* Set zero when the PLY markers define the unit. */
    double source_unit_to_meter;
} melkor_ply_inspect_options;

/* Initialize options with desktop limits and automatic semantic values. */
#define MELKOR_PLY_INSPECT_OPTIONS_INIT                                                            \
    {sizeof(melkor_ply_inspect_options), MELKOR_LIMITS_PROFILE_DESKTOP, MELKOR_PLY_PROFILE_AUTO,   \
     MELKOR_COORDINATE_FRAME_AUTO,       MELKOR_COLOR_SPACE_AUTO,       0.0}

/* Result from melkor_inspect_ply_file. */
typedef struct melkor_ply_info {
    size_t struct_size;
    uint64_t splat_count;
    uint32_t sh_degree;
    melkor_ply_profile profile;
    melkor_coordinate_frame source_frame;
    melkor_color_space source_color_space;
    melkor_optional_bool antialiased;

    /* The library sets this reserved field to zero. */
    uint32_t reserved;

    double source_unit_to_meter;

    /* Loss counts describe source fields that the canonical Gaussian model does not retain. */
    uint64_t loss_count;
    uint64_t blocking_loss_count;
} melkor_ply_info;

/* Initialize an empty PLY result. */
#define MELKOR_PLY_INFO_INIT                                                                       \
    {sizeof(melkor_ply_info),                                                                      \
     0,                                                                                            \
     0,                                                                                            \
     MELKOR_PLY_PROFILE_AUTO,                                                                      \
     MELKOR_COORDINATE_FRAME_AUTO,                                                                 \
     MELKOR_COLOR_SPACE_AUTO,                                                                      \
     MELKOR_OPTIONAL_BOOL_UNKNOWN,                                                                 \
     0,                                                                                            \
     0.0,                                                                                          \
     0,                                                                                            \
     0}

/*
 * Read and validate a PLY file. Return its splat count and SH degree.
 *
 * The path uses UTF-8. The function reads PLY data independently of the path suffix.
 *
 * Set info->struct_size before this call. The function writes only complete
 * fields. The function does not change info after a failed call.
 *
 * Set options->struct_size when options is not NULL. A NULL options pointer
 * selects desktop limits and automatic semantic values.
 *
 * Automatic semantic values use the Melkor PLY markers. Supply each value
 * when an unmarked PLY file needs it.
 *
 * The function returns these status values:
 *
 * - MELKOR_INVALID_ARGUMENT for invalid options or missing semantic values.
 * - MELKOR_UNSUPPORTED_FEATURE for an unsupported PLY semantic feature.
 * - MELKOR_IO_ERROR for an open, seek, or read failure.
 * - MELKOR_RESOURCE_LIMIT when the selected profile rejects resource use.
 * - MELKOR_INVALID_DATA when PLY validation fails.
 * - MELKOR_INTERNAL_ERROR for an unexpected internal failure.
 */
MELKOR_API melkor_status melkor_inspect_ply_file(const char* path,
                                                 const melkor_ply_inspect_options* options,
                                                 melkor_ply_info* info) MELKOR_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef MELKOR_NOEXCEPT

#endif /* MELKOR_C_MELKOR_H */
