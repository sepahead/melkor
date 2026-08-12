cmake_minimum_required(VERSION 3.24)

# The root VERSION file is the single authoritative version source. It is read before
# project() because project(VERSION ...) needs the parsed numeric core.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/MelkorVersion.cmake)

# Set the deployment target before project() enables a compiler. An empty cache entry after
# project() is too late because CMake has already selected the platform availability macros.
if(CMAKE_HOST_APPLE AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
    endif()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}" CACHE STRING
        "Minimum macOS deployment version" FORCE)
endif()

project(melkor VERSION ${MELKOR_VERSION_CORE} LANGUAGES CXX)
set(MELKOR_VERSION "${MELKOR_VERSION_FULL}")
include(GNUInstallDirs)
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()

# Generated version header. Consumers include <melkor/version.h>; the generated copy
# lives in the build tree so the source tree stays free of build products.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/melkor/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/melkor/version.h"
    @ONLY)
set(MELKOR_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single-config generators default to an optimized build; an empty
# CMAKE_BUILD_TYPE silently produces unoptimized binaries.
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

option(MELKOR_INSTALL "Generate installation and packaging rules" ${PROJECT_IS_TOP_LEVEL})

# Reject removed options. An ignored cache entry can make a stale build script
# appear to enable a feature that Melkor does not provide.
foreach(removed_option MELKOR_USE_METAL MELKOR_USE_CUDA)
    if(DEFINED ${removed_option})
        message(FATAL_ERROR
            "${removed_option} was removed. Melkor no longer builds GPU backends.")
    endif()
endforeach()

# Treat warnings as errors on first-party code (used by CI).
option(MELKOR_WERROR "Treat compiler warnings as errors" OFF)
set(MELKOR_WARNING_FLAGS
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<AND:$<BOOL:${MELKOR_WERROR}>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<CXX_COMPILER_ID:MSVC>:/permissive->
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>
    $<$<AND:$<BOOL:${MELKOR_WERROR}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
)

# Feature settings accept AUTO, ON, or OFF. ON fails if a requirement is missing.
function(melkor_validate_feature_setting variable description)
    string(TOUPPER "${${variable}}" MELKOR_FEATURE_VALUE)
    if(NOT MELKOR_FEATURE_VALUE MATCHES "^(AUTO|ON|OFF)$")
        message(FATAL_ERROR
            "${variable} must be AUTO, ON, or OFF. Received '${${variable}}'.")
    endif()
    set(${variable} "${MELKOR_FEATURE_VALUE}" CACHE STRING "${description}" FORCE)
    set_property(CACHE ${variable} PROPERTY STRINGS AUTO ON OFF)
endfunction()

set(MELKOR_BUILD_SPZ "AUTO" CACHE STRING "Build with vendored SPZ support")
melkor_validate_feature_setting(
    MELKOR_BUILD_SPZ "Build with vendored SPZ support")

# Third-party dependencies directory
set(THIRD_PARTY_DIR "${PROJECT_SOURCE_DIR}/third_party")
set(MELKOR_SPZ_SOURCE_DIR "${THIRD_PARTY_DIR}/spz" CACHE PATH
    "Path to the SPZ source tree")
set(MELKOR_SPZ_LICENSE_FILE "${THIRD_PARTY_DIR}/spz/LICENSE")

# Melkor uses a reviewed nlohmann/json single-header release.
if(NOT EXISTS "${THIRD_PARTY_DIR}/nlohmann-json/single_include/nlohmann/json.hpp")
    message(FATAL_ERROR
        "The pinned nlohmann/json header is missing. Restore third_party/nlohmann-json.")
endif()
add_library(melkor_json INTERFACE)
target_include_directories(
    melkor_json SYSTEM INTERFACE
    ${THIRD_PARTY_DIR}/nlohmann-json/single_include)

# Resolve SPZ before Melkor creates its core target.
set(MELKOR_SPZ_ENABLED OFF)
if(NOT MELKOR_BUILD_SPZ STREQUAL "OFF")
    if(EXISTS "${MELKOR_SPZ_SOURCE_DIR}/CMakeLists.txt")
        set(MELKOR_SPZ_LICENSE_FILE "${MELKOR_SPZ_SOURCE_DIR}/LICENSE")
        if(NOT EXISTS "${MELKOR_SPZ_LICENSE_FILE}" OR
           IS_DIRECTORY "${MELKOR_SPZ_LICENSE_FILE}" OR
           IS_SYMLINK "${MELKOR_SPZ_LICENSE_FILE}")
            message(FATAL_ERROR
                "The selected SPZ source must contain a regular LICENSE file.")
        endif()
        find_package(ZLIB QUIET)
        if(ZLIB_FOUND)
            set(MELKOR_SPZ_ENABLED ON)
        elseif(MELKOR_BUILD_SPZ STREQUAL "ON")
            message(FATAL_ERROR "MELKOR_BUILD_SPZ=ON requires zlib.")
        else()
            message(STATUS "zlib is unavailable. SPZ support is disabled.")
        endif()
    elseif(MELKOR_BUILD_SPZ STREQUAL "ON")
        message(FATAL_ERROR
            "MELKOR_BUILD_SPZ=ON requires ${MELKOR_SPZ_SOURCE_DIR}/CMakeLists.txt.")
    else()
        message(STATUS "The SPZ source is unavailable. SPZ support is disabled.")
    endif()
endif()

if(MELKOR_SPZ_ENABLED)
    # EXCLUDE_FROM_ALL: build spz because melkor_core depends on it, but do NOT run its own
    # install rules. Without this, spz installs its CLI tools (ply_to_spz, spz_info, ...) and
    # its headers (load-spz.h, splat-types.h) into our SDK prefix, polluting the include root
    # and the bin directory with a vendored dependency's surface. spz is an implementation
    # detail of libmelkor, not part of the Melkor SDK.
    # The upstream target follows BUILD_SHARED_LIBS because it has no explicit type.
    # Add it in a function scope to keep its generic options out of the parent project.
    function(melkor_add_spz_subdirectory)
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_TOOLS OFF)
        set(BUILD_PYTHON_BINDINGS OFF)
        add_subdirectory(
            "${MELKOR_SPZ_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/third_party/spz"
            EXCLUDE_FROM_ALL)
    endfunction()
    melkor_add_spz_subdirectory()
endif()

# Format and safety core.
add_library(melkor_core STATIC
    # Safety substrate.
    src/core/error.cpp
    src/core/color_space.cpp
    src/core/checked.cpp
    src/core/limits.cpp
    src/core/budget.cpp
    src/core/scene.cpp
    src/core/provenance.cpp

    # Format adapters and the conversion loss policy.
    src/formats/format_id.cpp
    src/formats/profile.cpp
    src/formats/registry.cpp
    src/formats/glb_container.cpp
    src/formats/gltf_accessor.cpp
    src/formats/gltf_document.cpp
    src/formats/gltf_extensions.cpp
    src/formats/gltf_khr.cpp
    src/formats/gltf_file_reader.cpp
    src/formats/gltf_reader.cpp
    src/formats/gltf_resolve.cpp
    src/formats/gltf_transform.cpp
    src/formats/gltf_usage.cpp
    src/formats/gltf_writer.cpp
    src/formats/loss.cpp
    src/formats/probe.cpp

    # Canonical math operations.
    src/math/activation.cpp
    src/math/quaternion.cpp
    src/math/covariance.cpp
    src/math/sh_rotation.cpp
    src/math/color.cpp
    src/math/coordinate_frame.cpp

    # Atomic output for every writer.
    src/io/input_file.cpp
    src/io/atomic_writer.cpp

    src/ply_writer.cpp
    src/cloud_inspector.cpp
    src/spz_encoder.cpp
)

# Generator-expression include dirs so the installed package relocates. A hard-coded source
# path in an exported target would point every downstream consumer back at THIS machine's
# checkout, which is the classic reason a "installed" SDK only works on the box that built it.
target_include_directories(melkor_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${MELKOR_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# The shared C ABI links this static core.
# Use position-independent code and keep implementation symbols private.
set_target_properties(melkor_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

target_link_libraries(melkor_core PRIVATE
    melkor_json
)

if(MELKOR_SPZ_ENABLED)
    target_link_libraries(melkor_core PRIVATE spz ZLIB::ZLIB)
    target_compile_definitions(melkor_core PUBLIC MELKOR_HAS_SPZ)
    target_include_directories(melkor_core SYSTEM PRIVATE ${MELKOR_SPZ_SOURCE_DIR}/src/cc)
endif()
# Enable strict warnings on Melkor's own code (not third-party).
target_compile_options(melkor_core PRIVATE ${MELKOR_WARNING_FLAGS})

# Main CLI executable
add_executable(melkor
    src/main.cpp
    src/cli_diagnostics.cpp
    src/cli_interrupt.cpp
    src/inspect_command.cpp
    src/convert_command.cpp
)

target_link_libraries(melkor PRIVATE melkor_core melkor_json)
target_compile_options(melkor PRIVATE ${MELKOR_WARNING_FLAGS})
# The CLI reads its version from the generated <melkor/version.h>, which derives from
# the root VERSION file. There is no second version string to keep in step.

# ---------------------------------------------------------------------------
# libmelkor is the installed shared library.
#
# The supported SDK boundary is the C ABI in include/melkor/c/melkor.h.
# The shared library keeps the C++ implementation private.
# The installed package does not expose C++ declarations without exported implementations.
# ---------------------------------------------------------------------------
add_library(melkor_lib SHARED src/c_api/melkor_c.cpp)
set_target_properties(melkor_lib PROPERTIES
    OUTPUT_NAME melkor           # produces libmelkor.so / libmelkor.dylib / melkor.dll
    EXPORT_NAME melkor           # exported to consumers as Melkor::melkor, not Melkor::melkor_lib
    CXX_VISIBILITY_PRESET hidden # export only MELKOR_API symbols; nothing leaks by accident
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${MELKOR_VERSION_CORE}
    SOVERSION ${MELKOR_ABI_VERSION})
if(MSVC)
    # The CLI and the SDK have the same output name on Windows. Keep the SDK link files in a
    # separate directory so parallel links cannot both write melkor.pdb or melkor.ilk.
    set_target_properties(melkor_lib PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/c-sdk"
        ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/c-sdk"
        PDB_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/c-sdk")
endif()
target_compile_definitions(melkor_lib PRIVATE MELKOR_BUILDING_LIBRARY)
# The C API uses the platform-neutral core.
# It does not require a GPU backend.
target_link_libraries(melkor_lib PRIVATE melkor_core)
target_include_directories(melkor_lib PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${MELKOR_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(melkor_lib PRIVATE ${MELKOR_WARNING_FLAGS})

# Enforce the C ABI at the linker boundary. Hidden visibility on Melkor objects is not
# sufficient because a static dependency can contain default-visible symbols.
if(APPLE)
    target_link_options(melkor_lib PRIVATE
        "LINKER:-exported_symbol,_melkor_get_version"
        "LINKER:-exported_symbol,_melkor_inspect_ply_file"
        "LINKER:-exported_symbol,_melkor_status_string")
elseif(UNIX)
    set(MELKOR_EXPORT_MAP "${PROJECT_SOURCE_DIR}/cmake/MelkorExports.map")
    target_link_options(melkor_lib PRIVATE
        "LINKER:--version-script=${MELKOR_EXPORT_MAP}")
    set_property(TARGET melkor_lib APPEND PROPERTY LINK_DEPENDS "${MELKOR_EXPORT_MAP}")
endif()
add_library(Melkor::melkor ALIAS melkor_lib)

if(MELKOR_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS melkor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    # Export the shared library as Melkor::melkor.
    install(TARGETS melkor_lib
        EXPORT MelkorTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Install only the supported C ABI and its generated version constants.
    # Keep all C++ headers private until Melkor exports a supported C++ ABI.
    install(FILES ${PROJECT_SOURCE_DIR}/include/melkor/c/melkor.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/melkor/c)
    install(FILES ${MELKOR_GENERATED_INCLUDE_DIR}/melkor/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/melkor)

    # Install the reference profiles and schemas with the SDK.
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/profiles
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melkor)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/schemas
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melkor)

    install(FILES
        ${PROJECT_SOURCE_DIR}/LICENSE
        ${PROJECT_SOURCE_DIR}/NOTICE
        ${PROJECT_SOURCE_DIR}/THIRD_PARTY_LICENSES.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melkor)
    install(FILES ${MELKOR_SPZ_LICENSE_FILE}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melkor/licenses
        RENAME spz-LICENSE)
    install(FILES ${PROJECT_SOURCE_DIR}/third_party/nlohmann-json/LICENSE.MIT
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melkor/licenses
        RENAME nlohmann-json-LICENSE)
    install(FILES ${PROJECT_SOURCE_DIR}/third_party/glib/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melkor/licenses
        RENAME glib-LICENSE)

    # The exported target set, the config, and a version file. Together these are what makes
    # `find_package(Melkor 2 CONFIG REQUIRED)` work from a clean consumer project.
    install(EXPORT MelkorTargets
        FILE MelkorTargets.cmake
        NAMESPACE Melkor::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Melkor)

    configure_package_config_file(
        ${PROJECT_SOURCE_DIR}/cmake/MelkorConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/MelkorConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Melkor)

    # SameMajorVersion: a consumer that asks for Melkor 2 accepts any 2.x, matching the ABI
    # promise, but is refused a 3.x that could have broken the ABI.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/MelkorConfigVersion.cmake
        VERSION ${MELKOR_VERSION_CORE}
        COMPATIBILITY SameMajorVersion)

    # Keep comparisons on the numeric core. Report the complete SemVer after those checks.
    if(NOT MELKOR_VERSION_FULL STREQUAL MELKOR_VERSION_CORE)
        file(READ ${CMAKE_CURRENT_BINARY_DIR}/MelkorConfigVersion.cmake
            MELKOR_PACKAGE_VERSION_CONTENT)
        set(MELKOR_PACKAGE_VERSION_PREFIX "")
        if(MELKOR_VERSION_IS_PRERELEASE)
            # A prerelease package must not satisfy a stable version request.
            string(CONCAT MELKOR_PACKAGE_VERSION_PREFIX
                "if(NOT \"\${PACKAGE_FIND_VERSION}\" STREQUAL \"\")\n"
                "    set(PACKAGE_VERSION \"${MELKOR_VERSION_FULL}\")\n"
                "    set(PACKAGE_VERSION_COMPATIBLE FALSE)\n"
                "    set(PACKAGE_VERSION_EXACT FALSE)\n"
                "    set(PACKAGE_VERSION_UNSUITABLE TRUE)\n"
                "    return()\n"
                "endif()\n")
        endif()
        set(MELKOR_PACKAGE_ARCH_MARKER
            "# if the installed or the using project don't have CMAKE_SIZEOF_VOID_P set, ignore it:")
        string(FIND "${MELKOR_PACKAGE_VERSION_CONTENT}"
            "${MELKOR_PACKAGE_ARCH_MARKER}" MELKOR_PACKAGE_ARCH_MARKER_OFFSET)
        if(MELKOR_PACKAGE_ARCH_MARKER_OFFSET EQUAL -1)
            message(FATAL_ERROR "The CMake package version template has an unknown structure")
        endif()
        string(REPLACE
            "${MELKOR_PACKAGE_ARCH_MARKER}"
            "set(PACKAGE_VERSION \"${MELKOR_VERSION_FULL}\")\n\n${MELKOR_PACKAGE_ARCH_MARKER}"
            MELKOR_PACKAGE_VERSION_CONTENT "${MELKOR_PACKAGE_VERSION_CONTENT}")
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/MelkorConfigVersion.cmake
            "${MELKOR_PACKAGE_VERSION_PREFIX}"
            "${MELKOR_PACKAGE_VERSION_CONTENT}")
    endif()

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/MelkorConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/MelkorConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Melkor)
endif()

# Print configuration summary
message(STATUS "")
message(STATUS "Melkor Build Configuration:")
message(STATUS "  SPZ Setting: ${MELKOR_BUILD_SPZ}")
message(STATUS "  SPZ Support: ${MELKOR_SPZ_ENABLED}")
message(STATUS "")

# ============================================================================
# Tests
# ============================================================================
# These tests verify the platform-independent format and safety core.
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)

# Safety substrate: checked arithmetic, resource budgets, decompression-bomb guards,
# cancellation, and the diagnostic/exit-code contract.
add_executable(test_safety_substrate tests/test_safety_substrate.cpp)
target_link_libraries(test_safety_substrate PRIVATE melkor_core)
target_include_directories(test_safety_substrate PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(test_safety_substrate PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME safety_substrate_tests COMMAND test_safety_substrate)

# Atomic output: the existing destination must survive every injected failure.
add_executable(test_atomic_writer tests/test_atomic_writer.cpp)
target_link_libraries(test_atomic_writer PRIVATE melkor_core)
target_compile_options(test_atomic_writer PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME atomic_writer_tests COMMAND test_atomic_writer)

# PLY color conversion regression (P0-07): the source property type decides the scaling.
add_executable(test_ply_color tests/test_ply_color.cpp)
target_link_libraries(test_ply_color PRIVATE melkor_core)
target_compile_options(test_ply_color PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME ply_color_tests COMMAND test_ply_color)

# The PLY reader charges input/splats/memory against a Budget before allocating (P0-12).
add_executable(test_ply_budget tests/test_ply_budget.cpp)
target_link_libraries(test_ply_budget PRIVATE melkor_core)
target_compile_options(test_ply_budget PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME ply_budget_tests COMMAND test_ply_budget)

# The SPZ decoder charges the compressed input against a Budget before the decode (P0-12).
add_executable(test_spz_budget tests/test_spz_budget.cpp)
target_link_libraries(test_spz_budget PRIVATE melkor_core)
target_compile_options(test_spz_budget PRIVATE ${MELKOR_WARNING_FLAGS})
if(MELKOR_SPZ_ENABLED)
    target_compile_definitions(test_spz_budget PRIVATE MELKOR_HAS_SPZ)
    target_link_libraries(test_spz_budget PRIVATE spz)
endif()
add_test(NAME spz_budget_tests COMMAND test_spz_budget)

# Canonical math oracle: activation domains, quaternion contract, and the covariance affine
# transform that P0-17 is about.
add_executable(test_math_oracle tests/test_math_oracle.cpp)
target_link_libraries(test_math_oracle PRIVATE melkor_core)
target_compile_options(test_math_oracle PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME math_oracle_tests COMMAND test_math_oracle)

# Canonical scene model: validated invariants, no uninitialized scalars, no invariant-violating
# mutation (P0-06).
add_executable(test_scene tests/test_scene.cpp)
target_link_libraries(test_scene PRIVATE melkor_core)
target_compile_options(test_scene PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME scene_tests COMMAND test_scene)

# Canonical metadata and deterministic, path-safe provenance JSON (P0-06).
add_executable(test_provenance tests/test_provenance.cpp)
target_link_libraries(test_provenance PRIVATE melkor_core melkor_json)
target_compile_options(test_provenance PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME provenance_tests COMMAND test_provenance)

# Conversion loss policy: info/warning pass, severe blocks unless the exact code is approved,
# fatal can never be approved (WP06).
add_executable(test_loss_report tests/test_loss_report.cpp)
target_link_libraries(test_loss_report PRIVATE melkor_core)
target_compile_options(test_loss_report PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME loss_report_tests COMMAND test_loss_report)

# Container probe: magic-byte identification separate from semantic profile, honest about the
# gzip/SPZ ambiguity (WP06).
add_executable(test_probe tests/test_probe.cpp)
target_link_libraries(test_probe PRIVATE melkor_core)
target_compile_options(test_probe PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME probe_tests COMMAND test_probe)

# KHR_gaussian_splatting layout core: SH attribute naming and m-ordering, the spec's C matrix,
# and the defined color spaces, pinned against the vendored RC spec text (WP09).
add_executable(test_gltf_khr tests/test_gltf_khr.cpp)
target_link_libraries(test_gltf_khr PRIVATE melkor_core)
target_compile_options(test_gltf_khr PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME gltf_khr_tests COMMAND test_gltf_khr)

# Real spherical-harmonic rotation: band matrices verified orthogonal, round-tripping, composing,
# and matching a direct evaluation of the rotated basis (P0-17).
add_executable(test_sh_rotation tests/test_sh_rotation.cpp)
target_link_libraries(test_sh_rotation PRIVATE melkor_core)
target_compile_options(test_sh_rotation PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME sh_rotation_tests COMMAND test_sh_rotation)

# GLB container framing: adversarial tests that a lying header/chunk length produces a clean
# failure with no out-of-bounds read, plus build/parse round-tripping (WP09).
add_executable(test_glb_container tests/test_glb_container.cpp)
target_link_libraries(test_glb_container PRIVATE melkor_core)
target_compile_options(test_glb_container PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME glb_container_tests COMMAND test_glb_container)

# glTF accessor decoding: the normalized-integer decode rules and the count/stride/offset bounds
# arithmetic, tested with hand-built bytes including overflow and out-of-range cases (WP09).
add_executable(test_gltf_accessor tests/test_gltf_accessor.cpp)
target_link_libraries(test_gltf_accessor PRIVATE melkor_core)
target_compile_options(test_gltf_accessor PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME gltf_accessor_tests COMMAND test_gltf_accessor)

# glTF JSON document parsing: extraction and validation of the reduced document the splat reader
# needs, with adversarial fixtures for malformed JSON, bad indices, and an incomplete extension.
add_executable(test_gltf_document tests/test_gltf_document.cpp)
target_link_libraries(test_gltf_document PRIVATE melkor_core)
target_compile_options(test_gltf_document PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME gltf_document_tests COMMAND test_gltf_document)

# glTF node transforms: column-major matrix extraction and the T*R*S / parent-chain composition,
# cross-checked by applying the transform to points (WP09).
add_executable(test_gltf_transform tests/test_gltf_transform.cpp)
target_link_libraries(test_gltf_transform PRIVATE melkor_core)
target_compile_options(test_gltf_transform PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME gltf_transform_tests COMMAND test_gltf_transform)

# glTF extension policy: reject only unsupported *required* extensions (the P0-10 fix), ignore but
# report used-but-not-required unsupported ones (WP09).
add_executable(test_gltf_extensions tests/test_gltf_extensions.cpp)
target_link_libraries(test_gltf_extensions PRIVATE melkor_core)
target_compile_options(test_gltf_extensions PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME gltf_extensions_tests COMMAND test_gltf_extensions)

# glTF accessor resolution: composes bufferView+accessor offsets and validates the accessor lies
# within its bufferView within its buffer, end-to-end from a parsed document (WP09).
add_executable(test_gltf_resolve tests/test_gltf_resolve.cpp)
target_link_libraries(test_gltf_resolve PRIVATE melkor_core)
target_compile_options(test_gltf_resolve PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME gltf_resolve_tests COMMAND test_gltf_resolve)

# glTF per-primitive reader: reads one KHR_gaussian_splatting primitive into a local-space
# SplatData, pinning the SH coefficient-major -> splat-major transpose and the structural rejections.
add_executable(test_gltf_reader tests/test_gltf_reader.cpp)
target_link_libraries(test_gltf_reader PRIVATE melkor_core)
target_compile_options(test_gltf_reader PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME gltf_reader_tests COMMAND test_gltf_reader)

add_executable(test_format_registry tests/test_format_registry.cpp)
target_link_libraries(test_format_registry PRIVATE melkor_core)
target_compile_options(test_format_registry PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME format_registry_tests COMMAND test_format_registry)

# glTF scene-graph walk: node transforms composed and applied, cycle-safe traversal, SH-degree
# merge, and the loss report (WP09).
add_executable(test_gltf_scene tests/test_gltf_scene.cpp)
target_link_libraries(test_gltf_scene PRIVATE melkor_core)
target_compile_options(test_gltf_scene PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME gltf_scene_tests COMMAND test_gltf_scene)

# glTF writer: round-trips a SplatData through a GLB (write then read must return the input,
# including the SH transpose), and pins the degree-3 truncation loss for a degree-4 source (WP09).
add_executable(test_gltf_writer tests/test_gltf_writer.cpp)
target_link_libraries(test_gltf_writer PRIVATE melkor_core)
target_compile_options(test_gltf_writer PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME gltf_writer_tests COMMAND test_gltf_writer)

# ---------------------------------------------------------------------------
# Fuzzing.
#
# Real coverage-guided fuzz targets, not the randomized-loop-over-valid-data the pre-v2 tests
# called fuzzing (P1-12). Each target builds two ways from one source: a libFuzzer binary that
# runs continuously in CI, and a standalone corpus-replay executable that runs the seed corpus
# plus built-in adversarial inputs as an ordinary regression test on every platform. Every fixed
# crash becomes a permanent corpus entry the replay then guards.
# ---------------------------------------------------------------------------
option(MELKOR_BUILD_FUZZERS "Build libFuzzer targets (requires a fuzzer-capable Clang)" OFF)

set(MELKOR_FUZZ_TARGETS fuzz_ply fuzz_spz fuzz_glb fuzz_gltf_khr)

# The replay executables always build, so the harnesses cannot rot and the corpus is exercised
# even where no libFuzzer runtime exists.
foreach(fuzzer ${MELKOR_FUZZ_TARGETS})
    add_executable(${fuzzer}_replay fuzz/${fuzzer}.cpp)
    target_link_libraries(${fuzzer}_replay PRIVATE melkor_core)
    target_compile_options(${fuzzer}_replay PRIVATE ${MELKOR_WARNING_FLAGS})
    string(REPLACE "fuzz_" "" _corpus_name ${fuzzer})
    add_test(NAME ${fuzzer}_replay
             COMMAND ${fuzzer}_replay ${PROJECT_SOURCE_DIR}/fuzz/corpus/${_corpus_name})
endforeach()

# A missing corpus must fail closed rather than silently exercising only built-in cases. Keep
# the sentinel in the source tree, where this configure-time assertion guarantees the path is
# absent for every subsequent CTest invocation from this build.
set(MELKOR_MISSING_FUZZ_CORPUS
    "${PROJECT_SOURCE_DIR}/fuzz/__melkor_intentionally_missing_corpus__")
if(EXISTS "${MELKOR_MISSING_FUZZ_CORPUS}")
    message(FATAL_ERROR "Fuzz replay missing-corpus sentinel unexpectedly exists")
endif()
add_test(NAME fuzz_replay_rejects_missing_corpus
         COMMAND fuzz_ply_replay "${MELKOR_MISSING_FUZZ_CORPUS}")
set_tests_properties(fuzz_replay_rejects_missing_corpus PROPERTIES
    WILL_FAIL TRUE)

if(MELKOR_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MELKOR_BUILD_FUZZERS requires Clang (libFuzzer).")
    endif()

    # Instrument the parser implementations, not only the thin harness translation units.
    # fuzzer-no-link provides coverage hooks without injecting libFuzzer's main into static
    # libraries. The ASan/UBSan runtime requirement is a usage requirement so every executable
    # that consumes an instrumented archive links the matching runtimes, including replay and
    # non-fuzzer targets built in this configuration.
    set(MELKOR_FUZZ_LIBRARY_SANITIZERS -fsanitize=fuzzer-no-link,address,undefined)
    set(MELKOR_FUZZ_RUNTIME_SANITIZERS -fsanitize=address,undefined)
    target_compile_options(melkor_core PRIVATE ${MELKOR_FUZZ_LIBRARY_SANITIZERS})
    target_link_options(melkor_core INTERFACE ${MELKOR_FUZZ_RUNTIME_SANITIZERS})
    if(MELKOR_SPZ_ENABLED)
        target_compile_options(spz PRIVATE ${MELKOR_FUZZ_LIBRARY_SANITIZERS})
        target_link_options(spz INTERFACE ${MELKOR_FUZZ_RUNTIME_SANITIZERS})
    endif()

    foreach(fuzzer ${MELKOR_FUZZ_TARGETS})
        add_executable(${fuzzer} fuzz/${fuzzer}.cpp)
        target_link_libraries(${fuzzer} PRIVATE melkor_core)
        # MELKOR_FUZZER_RUNTIME switches the source to the libFuzzer entry point (no main()).
        target_compile_definitions(${fuzzer} PRIVATE MELKOR_FUZZER_RUNTIME)
        target_compile_options(${fuzzer} PRIVATE -fsanitize=fuzzer,address,undefined
                                                 ${MELKOR_WARNING_FLAGS})
        target_link_options(${fuzzer} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()

add_executable(test_cloud_inspector tests/test_cloud_inspector.cpp)
target_link_libraries(test_cloud_inspector PRIVATE melkor_core)
target_compile_options(test_cloud_inspector PRIVATE ${MELKOR_WARNING_FLAGS})
set_target_properties(test_cloud_inspector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
)
add_test(NAME cloud_inspector_tests COMMAND test_cloud_inspector)

# Compile the public no-SPZ facade without inheriting melkor_core's
# MELKOR_HAS_SPZ definition. Both feature branches must expose the same result
# metadata contract to downstream code.
add_executable(test_spz_stub_api tests/test_spz_stub_api.cpp)
target_include_directories(test_spz_stub_api PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(test_spz_stub_api PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME spz_stub_api_tests COMMAND test_spz_stub_api)

# Fuzz test for SPZ quaternion rotation preservation across the full rotation
# space (random + edge cases). Decodes with spz's own loader so a symmetric
# double-bug cannot hide; compares rotation matrices since q/-q are equivalent.
if(MELKOR_SPZ_ENABLED)
    add_executable(test_spz_quaternion_fuzz tests/test_spz_quaternion_fuzz.cpp)
    target_link_libraries(test_spz_quaternion_fuzz PRIVATE melkor_core spz)
    target_compile_options(test_spz_quaternion_fuzz PRIVATE ${MELKOR_WARNING_FLAGS})
    target_compile_definitions(test_spz_quaternion_fuzz PRIVATE MELKOR_HAS_SPZ)
    set_target_properties(test_spz_quaternion_fuzz PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
    )
    add_test(NAME spz_quaternion_fuzz COMMAND test_spz_quaternion_fuzz)
endif()

# Fuzz test for the binary (and ASCII) PLY round-trip across random splats.
# Catches stride/ordering/endian regressions in both the writer and the
# header-driven reader.
add_executable(test_ply_roundtrip_fuzz tests/test_ply_roundtrip_fuzz.cpp)
target_link_libraries(test_ply_roundtrip_fuzz PRIVATE melkor_core)
target_compile_options(test_ply_roundtrip_fuzz PRIVATE ${MELKOR_WARNING_FLAGS})
set_target_properties(test_ply_roundtrip_fuzz PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
)
add_test(NAME ply_roundtrip_fuzz COMMAND test_ply_roundtrip_fuzz)

# Public CLI contract.
add_test(NAME cli_validation
         COMMAND bash
                 ${PROJECT_SOURCE_DIR}/tests/test_cli.sh
                 $<TARGET_FILE:melkor>
                 ${MELKOR_SPZ_ENABLED})

# Python test for the DA3 model-direct Gaussians extraction path. It stubs
# torch so it runs anywhere Python3 + numpy are available (no DA3 weights or
# GPU needed). Validates that the field transforms match ByteDance's exporter
# and that the DC->rgb->DC round-trip through save_ply does not drift.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    message(WARNING "Python3 interpreter not found: the seven Python-driven tests "
                    "(gaussians_from_prediction, release_metadata, release_evidence, "
                    "inspect_cli, convert_cli, tools, cmake_feature_options) will not be registered.")
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME gaussians_from_prediction_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/tests/test_gaussians_from_prediction.py)
    add_test(NAME release_metadata_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/tests/test_release_metadata.py)
    add_test(NAME release_evidence_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/tests/test_release_evidence.py)
    add_test(NAME inspect_cli_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/tests/test_inspect_cli.py
                     $<TARGET_FILE:melkor>
                     ${MELKOR_SPZ_ENABLED})
    add_test(NAME convert_cli_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/tests/test_convert_cli.py
                     $<TARGET_FILE:melkor>
                     ${MELKOR_SPZ_ENABLED})
    # Regression tests for the repository tooling (version sync, source-bundle allowlist,
    # notice generation) -- locking in adversarial-review fixes.
    add_test(NAME tools_tests
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test_tools.py)
    add_test(NAME cmake_feature_option_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/tests/test_cmake_features.py
                     ${CMAKE_COMMAND}
                     ${PROJECT_SOURCE_DIR})
endif()

endif() # PROJECT_IS_TOP_LEVEL AND BUILD_TESTING
