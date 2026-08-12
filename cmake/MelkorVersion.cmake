# Melkor version resolution.
#
# The root VERSION file is the single authoritative source of the project version.
# Nothing else in the repository may declare a version by hand. Every other surface
# — CMake, the generated C header, the CLI, the Python package, the viewer's
# package.json, the Tauri config, the Cargo manifest — is derived from it, and
# tools/check_version_sync.py fails CI when any of them drifts.
#
# This file must be included *before* project(), because project(VERSION ...) needs
# the parsed numeric core.

# Read the complete bounded file. Reject a link or an extra line.
set(MELKOR_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../VERSION")
if(IS_SYMLINK "${MELKOR_VERSION_FILE}")
    message(FATAL_ERROR "VERSION must be a regular file, not a symbolic link")
endif()
file(SIZE "${MELKOR_VERSION_FILE}" MELKOR_VERSION_FILE_SIZE)
if(MELKOR_VERSION_FILE_SIZE GREATER 256)
    message(FATAL_ERROR "VERSION exceeds the 256-byte limit")
endif()
file(READ "${MELKOR_VERSION_FILE}" MELKOR_VERSION_TEXT LIMIT 256)
if(NOT MELKOR_VERSION_TEXT MATCHES "\n$")
    message(FATAL_ERROR "VERSION must end with one LF character")
endif()
string(REGEX REPLACE "\n$" "" MELKOR_VERSION_TEXT "${MELKOR_VERSION_TEXT}")
if(MELKOR_VERSION_TEXT MATCHES "[\r\n]")
    message(FATAL_ERROR "VERSION must contain exactly one line")
endif()
string(STRIP "${MELKOR_VERSION_TEXT}" MELKOR_VERSION_STRIPPED)
if(NOT MELKOR_VERSION_TEXT STREQUAL MELKOR_VERSION_STRIPPED)
    message(FATAL_ERROR "VERSION must not contain surrounding whitespace")
endif()
set(MELKOR_VERSION_FULL "${MELKOR_VERSION_TEXT}")

# Strict SemVer 2.0.0 grammar. A malformed version must stop the configure rather
# than propagate a nonsense string into artifact names and provenance records.
if(NOT MELKOR_VERSION_FULL MATCHES
        "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9A-Za-z.-]+))?(\\+([0-9A-Za-z.-]+))?$")
    message(FATAL_ERROR
        "Invalid VERSION: '${MELKOR_VERSION_FULL}'\n"
        "Expected SemVer, for example 2.0.0, 2.0.0-dev, 2.0.0-rc.2.")
endif()

set(MELKOR_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(MELKOR_VERSION_MINOR "${CMAKE_MATCH_2}")
set(MELKOR_VERSION_PATCH "${CMAKE_MATCH_3}")
set(MELKOR_VERSION_PRERELEASE "${CMAKE_MATCH_5}")
set(MELKOR_VERSION_BUILD_METADATA "${CMAKE_MATCH_7}")
set(MELKOR_VERSION_CORE "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

foreach(component IN ITEMS MELKOR_VERSION_MAJOR MELKOR_VERSION_MINOR MELKOR_VERSION_PATCH)
    if("${${component}}" MATCHES "^0[0-9]+$")
        message(FATAL_ERROR "Invalid VERSION: a numeric core field has a leading zero")
    endif()
    string(LENGTH "${${component}}" MELKOR_VERSION_COMPONENT_LENGTH)
    if(MELKOR_VERSION_COMPONENT_LENGTH GREATER 10)
        message(FATAL_ERROR "Invalid VERSION: a numeric core field exceeds uint32")
    elseif(${component} GREATER 4294967295)
        message(FATAL_ERROR "Invalid VERSION: a numeric core field exceeds uint32")
    endif()
endforeach()

function(melkor_validate_version_identifiers value label reject_numeric_leading_zero)
    if("${value}" STREQUAL "")
        return()
    endif()
    if("${value}" MATCHES "^\\." OR "${value}" MATCHES "\\.$" OR
       "${value}" MATCHES "\\.\\.")
        message(FATAL_ERROR "Invalid VERSION: ${label} contains an empty identifier")
    endif()
    string(REPLACE "." ";" identifiers "${value}")
    foreach(identifier IN LISTS identifiers)
        if(reject_numeric_leading_zero AND identifier MATCHES "^[0-9]+$" AND
           identifier MATCHES "^0[0-9]+$")
            message(FATAL_ERROR
                "Invalid VERSION: numeric prerelease identifier '${identifier}' has a leading zero")
        endif()
    endforeach()
endfunction()

melkor_validate_version_identifiers("${MELKOR_VERSION_PRERELEASE}" "prerelease" TRUE)
melkor_validate_version_identifiers("${MELKOR_VERSION_BUILD_METADATA}" "build metadata" FALSE)

# A prerelease is anything that is not a bare X.Y.Z. Release workflows gate on this:
# a development or release-candidate build may not be published as a stable release.
if(MELKOR_VERSION_PRERELEASE STREQUAL "")
    set(MELKOR_VERSION_IS_PRERELEASE 0)
else()
    set(MELKOR_VERSION_IS_PRERELEASE 1)
endif()

# ---------------------------------------------------------------------------
# Compatibility versions.
#
# These are deliberately independent of the software version. The C ABI does not
# rebreak because the patch level changed, and a JSON consumer should not have to
# re-derive its schema from the release number.
# ---------------------------------------------------------------------------

# Stable binary ABI for the 2.x line. Bumped only by a reviewed breaking-ABI change.
set(MELKOR_ABI_VERSION 1)

# Version of the inspection report JSON document. New optional fields may be added
# within a version; an existing field never changes meaning.
set(MELKOR_INSPECT_SCHEMA_VERSION 1)

# Version of the loss report JSON document.
set(MELKOR_LOSS_SCHEMA_VERSION 1)

# Version of the external adapter protocol spoken by melkor-pipeline.
set(MELKOR_ADAPTER_PROTOCOL_VERSION 1)

# ---------------------------------------------------------------------------
# Reproducible build identity.
#
# Wall-clock time is not embedded by default: it would make two builds of the same
# commit differ, which defeats the reproducibility comparison in the release gate.
# SOURCE_DATE_EPOCH, when the environment supplies it, is the standard reproducible
# substitute and is honored here.
# ---------------------------------------------------------------------------

if(DEFINED ENV{SOURCE_DATE_EPOCH})
    set(MELKOR_SOURCE_DATE_EPOCH "$ENV{SOURCE_DATE_EPOCH}")
else()
    set(MELKOR_SOURCE_DATE_EPOCH "")
endif()

if(NOT MELKOR_SOURCE_DATE_EPOCH STREQUAL "")
    if(NOT MELKOR_SOURCE_DATE_EPOCH MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "Invalid SOURCE_DATE_EPOCH: expected a decimal integer from 0 through 4294967295")
    endif()
    string(LENGTH "${MELKOR_SOURCE_DATE_EPOCH}" MELKOR_SOURCE_DATE_EPOCH_LENGTH)
    if(MELKOR_SOURCE_DATE_EPOCH_LENGTH GREATER 10)
        message(FATAL_ERROR
            "Invalid SOURCE_DATE_EPOCH: expected a decimal integer from 0 through 4294967295")
    elseif(MELKOR_SOURCE_DATE_EPOCH GREATER 4294967295)
        message(FATAL_ERROR
            "Invalid SOURCE_DATE_EPOCH: expected a decimal integer from 0 through 4294967295")
    endif()
endif()

# The build commit is recorded only when the build environment supplies it, so an
# ordinary developer build does not bake a dirty SHA into a header and then differ
# from a clean one. Release workflows pass -DMELKOR_BUILD_COMMIT=<sha> explicitly.
set(MELKOR_BUILD_COMMIT "" CACHE STRING
    "Exact source commit to embed in version metadata; empty in developer builds")
if(NOT MELKOR_BUILD_COMMIT STREQUAL "")
    string(LENGTH "${MELKOR_BUILD_COMMIT}" MELKOR_BUILD_COMMIT_LENGTH)
    if(NOT MELKOR_BUILD_COMMIT MATCHES "^[0-9a-f]+$" OR
       NOT (MELKOR_BUILD_COMMIT_LENGTH EQUAL 40 OR
            MELKOR_BUILD_COMMIT_LENGTH EQUAL 64))
        message(FATAL_ERROR
            "MELKOR_BUILD_COMMIT must be an exact lowercase 40-character or 64-character hexadecimal object ID")
    endif()
endif()

message(STATUS "Melkor version: ${MELKOR_VERSION_FULL} "
               "(core ${MELKOR_VERSION_CORE}, ABI ${MELKOR_ABI_VERSION})")
