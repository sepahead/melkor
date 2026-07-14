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

# Read exactly one line so a stray trailing line cannot silently become the version.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" MELKOR_VERSION_FULL LIMIT_COUNT 1)
string(STRIP "${MELKOR_VERSION_FULL}" MELKOR_VERSION_FULL)

# Strict SemVer 2.0.0 grammar. A malformed version must stop the configure rather
# than propagate a nonsense string into artifact names and provenance records.
if(NOT MELKOR_VERSION_FULL MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)([-+][0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR
        "Invalid VERSION: '${MELKOR_VERSION_FULL}'\n"
        "Expected SemVer, for example 2.0.0, 2.0.0-dev, 2.0.0-rc.2.")
endif()

set(MELKOR_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(MELKOR_VERSION_MINOR "${CMAKE_MATCH_2}")
set(MELKOR_VERSION_PATCH "${CMAKE_MATCH_3}")
set(MELKOR_VERSION_CORE "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

# CMAKE_MATCH_4 keeps its leading '-' or '+'; the prerelease field should not.
set(MELKOR_VERSION_PRERELEASE "")
if(CMAKE_MATCH_4)
    string(SUBSTRING "${CMAKE_MATCH_4}" 1 -1 MELKOR_VERSION_PRERELEASE)
endif()

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
# substitute and is honoured here.
# ---------------------------------------------------------------------------

if(DEFINED ENV{SOURCE_DATE_EPOCH})
    set(MELKOR_SOURCE_DATE_EPOCH "$ENV{SOURCE_DATE_EPOCH}")
else()
    set(MELKOR_SOURCE_DATE_EPOCH "")
endif()

# The build commit is recorded only when the build environment supplies it, so an
# ordinary developer build does not bake a dirty SHA into a header and then differ
# from a clean one. Release workflows pass -DMELKOR_BUILD_COMMIT=<sha> explicitly.
set(MELKOR_BUILD_COMMIT "" CACHE STRING
    "Exact source commit to embed in version metadata; empty in developer builds")

message(STATUS "Melkor version: ${MELKOR_VERSION_FULL} "
               "(core ${MELKOR_VERSION_CORE}, ABI ${MELKOR_ABI_VERSION})")
