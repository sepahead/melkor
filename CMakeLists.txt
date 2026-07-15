cmake_minimum_required(VERSION 3.24)

# The root VERSION file is the single authoritative version source. It is read before
# project() because project(VERSION ...) needs the parsed numeric core.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/MelkorVersion.cmake)

project(melkor VERSION ${MELKOR_VERSION_CORE} LANGUAGES CXX)
set(MELKOR_VERSION "${MELKOR_VERSION_FULL}")
include(GNUInstallDirs)

# Generated version header. Consumers include <melkor/version.h>; the generated copy
# lives in the build tree so the source tree stays free of build products.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/melkor/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/melkor/version.h"
    @ONLY)
set(MELKOR_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single-config generators default to an optimized build; an empty
# CMAKE_BUILD_TYPE silently produces unoptimized binaries.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

# Treat warnings as errors on first-party code (used by CI).
option(MELKOR_WERROR "Treat compiler warnings as errors" OFF)
set(MELKOR_WARNING_FLAGS
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<AND:$<BOOL:${MELKOR_WERROR}>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Werror>
)

# Detect platform
if(APPLE)
    set(MELKOR_PLATFORM "APPLE")
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment version")
    # No -no_warn_duplicate_libraries here, deliberately.
    #
    # That suppression existed only to silence the diagnostic produced by the circular
    # melkor_core <-> backend static archives (P0-05). The graph is acyclic now, so the
    # warning has nothing to fire on -- and leaving the suppression in place would hide a real
    # cycle if one were ever reintroduced. A suppressed linker warning must never be
    # load-bearing again.
elseif(UNIX AND NOT APPLE)
    set(MELKOR_PLATFORM "LINUX")
else()
    set(MELKOR_PLATFORM "OTHER")
endif()

message(STATUS "Building for platform: ${MELKOR_PLATFORM}")

# Option to enable/disable Metal (macOS only)
option(MELKOR_USE_METAL "Use Metal GPU acceleration (macOS only)" ON)

# Option to enable/disable CUDA (Linux/NVIDIA)
# Note: CUDA is optional for the converter - the main GPU training uses OpenSplat
# Requirements for CUDA:
#   - CUDA Toolkit 11.0+ (12.0+ recommended for SM 90 / RTX 40 series)
#   - NVIDIA GPU with compute capability 6.0+ (Pascal or newer)
#   - Linux (Ubuntu 20.04+ recommended)
option(MELKOR_USE_CUDA "Use CUDA GPU acceleration (Linux/NVIDIA)" OFF)

# Auto-detect CUDA on Linux if not explicitly set
if(MELKOR_PLATFORM STREQUAL "LINUX")
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND AND MELKOR_USE_CUDA)
        message(STATUS "CUDA detected: ${CUDAToolkit_VERSION}")
        if(CUDAToolkit_VERSION VERSION_LESS "11.0")
            message(WARNING "CUDA ${CUDAToolkit_VERSION} detected. CUDA 11.0+ recommended.")
        endif()
        # Architecture list must be chosen HERE, before any add_library():
        # CMAKE_CUDA_ARCHITECTURES seeds each target's CUDA_ARCHITECTURES
        # property at target-creation time, and once the CUDA language is
        # enabled CMake requires the property on every target participating
        # in device linking — including CXX-only libraries in the circular
        # static-library arrangement. Some toolchains (distro-packaged nvcc)
        # provide no default. SM support per toolkit: 60 Pascal, 70 Volta,
        # 75 Turing, 80 Ampere DC, 86 Ampere consumer (11.1+), 89 Ada
        # (11.8+), 90 Hopper (12.0+).
        if(CUDAToolkit_VERSION VERSION_LESS "11.1")
            set(CMAKE_CUDA_ARCHITECTURES "60;70;75;80")
        elseif(CUDAToolkit_VERSION VERSION_LESS "11.8")
            set(CMAKE_CUDA_ARCHITECTURES "60;70;75;80;86")
        elseif(CUDAToolkit_VERSION VERSION_LESS "12.0")
            set(CMAKE_CUDA_ARCHITECTURES "60;70;75;80;86;89")
        else()
            set(CMAKE_CUDA_ARCHITECTURES "60;70;75;80;86;89;90")
        endif()
        message(STATUS "CUDA architectures: ${CMAKE_CUDA_ARCHITECTURES}")
    elseif(CUDAToolkit_FOUND)
        message(STATUS "CUDA available but not enabled. Use -DMELKOR_USE_CUDA=ON to enable.")
    endif()
endif()

# Option to build with SPZ support (requires submodule)
option(MELKOR_BUILD_SPZ "Build with SPZ support" ON)

# Third-party dependencies directory
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)

# tinygltf (header-only). SYSTEM: third-party header warnings must not be
# promoted to errors by MELKOR_WERROR when included from first-party code.
add_library(tinygltf INTERFACE)
target_include_directories(tinygltf SYSTEM INTERFACE ${THIRD_PARTY_DIR}/tinygltf)
target_compile_definitions(tinygltf INTERFACE
    TINYGLTF_NO_STB_IMAGE_WRITE
    TINYGLTF_NO_EXTERNAL_IMAGE
)

# stb_image for tinygltf (SYSTEM for the same reason as tinygltf)
add_library(stb_image INTERFACE)
target_include_directories(stb_image SYSTEM INTERFACE ${THIRD_PARTY_DIR}/stb)

# SPZ library (if available)
if(MELKOR_BUILD_SPZ AND EXISTS ${THIRD_PARTY_DIR}/spz/CMakeLists.txt)
    # EXCLUDE_FROM_ALL: build spz because melkor_core depends on it, but do NOT run its own
    # install rules. Without this, spz installs its CLI tools (ply_to_spz, spz_info, ...) and
    # its headers (load-spz.h, splat-types.h) into our SDK prefix, polluting the include root
    # and the bin directory with a vendored dependency's surface. spz is an implementation
    # detail of libmelkor, not part of the Melkor SDK.
    add_subdirectory(${THIRD_PARTY_DIR}/spz EXCLUDE_FROM_ALL)
    set(SPZ_AVAILABLE ON)
else()
    set(SPZ_AVAILABLE OFF)
    message(STATUS "SPZ not available - SPZ encoding will be disabled")
endif()

# Core library (platform-independent)
add_library(melkor_core STATIC
    # Safety substrate. Every reader, writer, and algorithm builds on these: results and
    # stable diagnostics, checked arithmetic for file-provided numbers, and shared resource
    # budgets with cancellation. Nothing here depends on a file format or a backend.
    src/core/error.cpp
    src/core/checked.cpp
    src/core/limits.cpp
    src/core/budget.cpp
    src/core/backend_registry.cpp

    # Canonical math oracle: the single semantic authority for activation domains, quaternion
    # handling, and covariance transform/decomposition. The place transforms are done correctly
    # so no format adapter or backend reinvents them wrongly (P0-17).
    src/math/activation.cpp
    src/math/quaternion.cpp
    src/math/covariance.cpp

    # Atomic output. One implementation for every writer -- PLY, SPZ, glTF, JSON reports.
    # Format-specific "open the file and hope" is how P0-08 happened.
    src/io/atomic_writer.cpp

    src/glb_reader.cpp
    src/ply_writer.cpp
    src/gaussian_data.cpp
    src/cloud_inspector.cpp
    src/enhanced_converter.cpp
    src/spz_encoder.cpp
    src/cpu_compute_provider.cpp
    src/spatial_grid.cpp
    src/densifier.cpp
)

# Generator-expression include dirs so the installed package relocates. A hard-coded source
# path in an exported target would point every downstream consumer back at THIS machine's
# checkout, which is the classic reason a "installed" SDK only works on the box that built it.
target_include_directories(melkor_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${MELKOR_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Position-independent code so the static core can be linked into the shared libmelkor. Without
# this, the shared-library link fails on Linux with a relocation error.
set_target_properties(melkor_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(melkor_core PUBLIC
    tinygltf
    stb_image
)

if(SPZ_AVAILABLE)
    target_link_libraries(melkor_core PUBLIC spz)
    target_compile_definitions(melkor_core PUBLIC MELKOR_HAS_SPZ)
    # Add SPZ include directories (SYSTEM: exempt from MELKOR_WERROR)
    target_include_directories(melkor_core SYSTEM PUBLIC ${THIRD_PARTY_DIR}/spz/src/cc)
endif()
# Enable strict warnings on Melkor's own code (not third-party).
target_compile_options(melkor_core PRIVATE ${MELKOR_WARNING_FLAGS})

# Platform-specific GPU acceleration
if(APPLE AND MELKOR_USE_METAL)
    message(STATUS "Building with Metal acceleration")
    
    # Find required frameworks
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
    
    # Metal compute library (Objective-C++)
    add_library(melkor_metal STATIC
        src/metal/metal_compute.mm
        src/metal/gaussian_processor.mm
        src/metal/metal_compute_provider.mm
    )
    
    target_include_directories(melkor_metal PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
    
    target_link_libraries(melkor_metal PUBLIC
        melkor_core
        ${METAL_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${METALKIT_FRAMEWORK}
    )
    
    target_compile_definitions(melkor_metal PUBLIC MELKOR_HAS_METAL)
    # The Objective-C++ sources are written for ARC (no manual
    # retain/release); without -fobjc-arc every newBuffer/newFunction object
    # leaks, growing resident memory on each GPU dispatch.
    target_compile_options(melkor_metal PRIVATE -fobjc-arc ${MELKOR_WARNING_FLAGS})
    # melkor_core does NOT link melkor_metal. That edge -- combined with melkor_metal's
    # link back to melkor_core -- is what made the graph cyclic. Dependencies now point one
    # way: backends depend on core, the runtime depends on backends, nothing depends
    # backwards. melkor_runtime is what pulls the backend into the final binary.
    
    # Compile Metal shaders
    set(METAL_SHADER_COMPUTE ${CMAKE_SOURCE_DIR}/src/metal/shaders/gaussian_compute.metal)
    set(METAL_SHADER_RASTERIZE ${CMAKE_SOURCE_DIR}/src/metal/shaders/rasterize_gaussians.metal)
    set(METAL_LIBRARY_OUTPUT ${CMAKE_BINARY_DIR}/default.metallib)
    
    add_custom_command(
        OUTPUT ${METAL_LIBRARY_OUTPUT}
        COMMAND xcrun -sdk macosx metal -c ${METAL_SHADER_COMPUTE} -o ${CMAKE_BINARY_DIR}/gaussian_compute.air
        COMMAND xcrun -sdk macosx metal -c ${METAL_SHADER_RASTERIZE} -o ${CMAKE_BINARY_DIR}/rasterize_gaussians.air
        COMMAND xcrun -sdk macosx metallib ${CMAKE_BINARY_DIR}/gaussian_compute.air ${CMAKE_BINARY_DIR}/rasterize_gaussians.air -o ${METAL_LIBRARY_OUTPUT}
        DEPENDS ${METAL_SHADER_COMPUTE} ${METAL_SHADER_RASTERIZE}
        COMMENT "Compiling Metal shaders"
    )
    
    add_custom_target(metal_shaders DEPENDS ${METAL_LIBRARY_OUTPUT})
    add_dependencies(melkor_metal metal_shaders)
    
    set(MELKOR_GPU_LIBRARY melkor_metal)
    set(MELKOR_HAS_GPU ON)
    set(MELKOR_GPU_BACKEND "Metal")
    
elseif(MELKOR_PLATFORM STREQUAL "LINUX" AND MELKOR_USE_CUDA AND CUDAToolkit_FOUND)
    message(STATUS "Building with CUDA acceleration")
    
    # Enable CUDA language. CMAKE_CXX_STANDARD does not apply to .cu files:
    # without an explicit CUDA standard, nvcc defaults to C++14 on every
    # CUDA 11.x and the shared headers (std::clamp in gaussian_data.hpp)
    # fail to compile.
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    
    # CUDA compute library. gaussian_processor.cu holds the __global__ kernels that
    # cuda_compute.cpp declares extern.
    #
    # It no longer compiles src/gpu_stub.cpp. That file existed to supply fake metal:: symbols
    # that platform-neutral code referenced -- which it should never have done. With
    # rawContext() removed from ComputeProvider and the dead Metal-typed facades deleted,
    # nothing outside src/backends names a platform type, so there is nothing left to stub.
    add_library(melkor_cuda STATIC
        src/cuda/cuda_compute.cpp
        src/cuda/gaussian_processor.cu
        src/cuda/cuda_compute_provider.cpp
    )

    target_include_directories(melkor_cuda PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )

    target_link_libraries(melkor_cuda PUBLIC
        melkor_core
        CUDA::cudart
    )

    target_compile_definitions(melkor_cuda PUBLIC MELKOR_HAS_CUDA)
    # Same circular static-library arrangement as the Metal branch: consumers
    # of melkor_core (tests, CLI) resolve the Metal-stub and provider-factory
    # symbols via this PUBLIC link.
    
    # Architectures were selected before enable_language(CUDA) above and
    # apply globally via CMAKE_CUDA_ARCHITECTURES.
    set_target_properties(melkor_cuda PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
    )
    
    set(MELKOR_GPU_LIBRARY melkor_cuda)
    set(MELKOR_HAS_GPU ON)
    set(MELKOR_GPU_BACKEND "CUDA")
    
else()
    message(STATUS "Building CPU-only (no GPU acceleration)")

    # There is no stub library any more.
    #
    # src/gpu_stub.cpp existed to fake the entire metal:: namespace so that platform-neutral
    # code -- which should never have referenced Metal types at all -- would link on non-Apple
    # platforms. With rawContext() gone from ComputeProvider and the dead GaussianFitter /
    # FeedforwardModel facades deleted, nothing outside src/backends names a platform type,
    # so there is nothing left to stub.
    #
    # A CPU-only build is now simply a build with no GPU backend registered. CPU is not a
    # fallback that fills a hole; it is a complete, always-present backend and the semantic
    # reference implementation.

    set(MELKOR_GPU_LIBRARY "")
    set(MELKOR_HAS_GPU OFF)
    set(MELKOR_GPU_BACKEND "CPU")
endif()

# ---------------------------------------------------------------------------
# melkor_runtime — the only layer allowed to name a concrete backend.
#
# This target is what makes the graph acyclic. Dependencies point one way:
#
#     melkor_core  <--  melkor_backend_{metal,cuda}  <--  melkor_runtime  <--  CLI / tests
#
# Previously melkor_core linked each backend PUBLIC *and* each backend linked melkor_core
# PUBLIC, because ComputeProvider::create() was declared in core but defined in the backend.
# The resulting mutually dependent static archives only linked because the linker rescans
# them, and on Apple only after -no_warn_duplicate_libraries suppressed the warning about it
# (P0-05). Core now owns a registry and knows nothing about Metal; the runtime populates it.
# ---------------------------------------------------------------------------
add_library(melkor_runtime STATIC
    src/runtime/backend_registration.cpp
)
target_include_directories(melkor_runtime PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(melkor_runtime PUBLIC melkor_core)
if(MELKOR_GPU_LIBRARY)
    target_link_libraries(melkor_runtime PUBLIC ${MELKOR_GPU_LIBRARY})
endif()
target_compile_options(melkor_runtime PRIVATE ${MELKOR_WARNING_FLAGS})

# Main CLI executable
add_executable(melkor
    src/main.cpp
    src/inspect_command.cpp
)

# The CLI links the runtime, which brings in core and whichever backend was compiled. It never
# names a backend itself.
target_link_libraries(melkor PRIVATE melkor_runtime)
target_compile_options(melkor PRIVATE ${MELKOR_WARNING_FLAGS})
# The CLI reads its version from the generated <melkor/version.h>, which derives from
# the root VERSION file. There is no second version string to keep in step.

# Copy metallib to executable directory (macOS only)
if(APPLE AND MELKOR_USE_METAL)
    add_custom_command(TARGET melkor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${METAL_LIBRARY_OUTPUT} $<TARGET_FILE_DIR:melkor>/default.metallib
    )
endif()

# ---------------------------------------------------------------------------
# libmelkor — the installable shared library and its stable C ABI.
#
# This is what closes P0-04: before this, `cmake --install` shipped the CLI and the licence
# files but not the library, the headers, or a CMake package config, so no downstream project
# could consume Melkor as an SDK at all.
#
# The stable, supported consumption boundary is the C ABI (include/melkor/c/melkor.h). The
# shared library links the whole runtime PRIVATELY and exports only the C ABI, so a consumer
# gets one relocatable libmelkor and a small, versioned surface -- not a web of internal static
# archives and third-party headers.
#
# The C++ headers are installed too, but their compiled symbols are deliberately NOT frozen as
# an exported ABI yet: the C++ scene/format API is still being reshaped by later work packages,
# and freezing exported C++ symbols around types that later work proves wrong is exactly the
# mistake the blueprint warns against. C++ consumers get the header-inline facilities (Result,
# version macros) now; the richer compiled C++ API is a later, deliberate export.
# ---------------------------------------------------------------------------
option(MELKOR_INSTALL "Generate installation and packaging rules for the SDK" ON)

add_library(melkor_lib SHARED src/c_api/melkor_c.cpp)
set_target_properties(melkor_lib PROPERTIES
    OUTPUT_NAME melkor           # produces libmelkor.so / libmelkor.dylib / melkor.dll
    EXPORT_NAME melkor           # exported to consumers as Melkor::melkor, not Melkor::melkor_lib
    CXX_VISIBILITY_PRESET hidden # export only MELKOR_API symbols; nothing leaks by accident
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${MELKOR_VERSION_CORE}
    SOVERSION ${MELKOR_ABI_VERSION})
target_compile_definitions(melkor_lib PRIVATE MELKOR_BUILDING_LIBRARY)
# PRIVATE: the runtime (core + backend) is an implementation detail of the shared library, not
# part of its public link surface. A consumer links libmelkor and nothing else.
target_link_libraries(melkor_lib PRIVATE melkor_runtime)
target_include_directories(melkor_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${MELKOR_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(melkor_lib PRIVATE ${MELKOR_WARNING_FLAGS})
add_library(Melkor::melkor ALIAS melkor_lib)

# Installation
install(TARGETS melkor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(APPLE AND MELKOR_USE_METAL)
    install(FILES ${METAL_LIBRARY_OUTPUT} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(MELKOR_INSTALL)
    include(CMakePackageConfigHelpers)

    # The shared library, exported under the Melkor:: namespace so downstream code links
    # Melkor::melkor and cannot accidentally pick up a same-named target.
    install(TARGETS melkor_lib
        EXPORT MelkorTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Public headers: the committed tree plus the generated version.h.
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/melkor
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
    install(FILES ${MELKOR_GENERATED_INCLUDE_DIR}/melkor/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/melkor)

    # Data the SDK and CLI need at runtime: format profiles and JSON schemas. Installed as
    # empty-tolerant so the rule holds before those directories are populated by later work.
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/profiles
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melkor OPTIONAL)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/schemas
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melkor OPTIONAL)

    install(FILES
        ${CMAKE_SOURCE_DIR}/LICENSE
        ${CMAKE_SOURCE_DIR}/NOTICE
        ${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melkor)

    # The exported target set, the config, and a version file. Together these are what makes
    # `find_package(Melkor 2 CONFIG REQUIRED)` work from a clean consumer project.
    install(EXPORT MelkorTargets
        FILE MelkorTargets.cmake
        NAMESPACE Melkor::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Melkor)

    configure_package_config_file(
        ${CMAKE_SOURCE_DIR}/cmake/MelkorConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/MelkorConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Melkor)

    # SameMajorVersion: a consumer that asks for Melkor 2 accepts any 2.x, matching the ABI
    # promise, but is refused a 3.x that could have broken the ABI.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/MelkorConfigVersion.cmake
        VERSION ${MELKOR_VERSION_CORE}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/MelkorConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/MelkorConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Melkor)
endif()

# Print configuration summary
message(STATUS "")
message(STATUS "Melkor Build Configuration:")
message(STATUS "  Platform: ${MELKOR_PLATFORM}")
message(STATUS "  GPU Backend: ${MELKOR_GPU_BACKEND}")
message(STATUS "  GPU Acceleration: ${MELKOR_HAS_GPU}")
message(STATUS "  SPZ Support: ${SPZ_AVAILABLE}")
message(STATUS "")

# ============================================================================
# Tests
# ============================================================================
# The previous CI "test" step was only `melkor --info`, which exercises no
# conversion logic. These tests assert real correctness properties of the
# platform-independent core: SPZ quaternion ordering, header-driven PLY
# parsing, PCA normal estimation, and the SH C0 constant.
enable_testing()

add_executable(test_melkor_core tests/test_melkor_core.cpp)
target_link_libraries(test_melkor_core PRIVATE melkor_runtime)
if(SPZ_AVAILABLE)
    target_compile_definitions(test_melkor_core PRIVATE MELKOR_HAS_SPZ)
endif()
set_target_properties(test_melkor_core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME melkor_core_tests COMMAND test_melkor_core)

# Safety substrate: checked arithmetic, resource budgets, decompression-bomb guards,
# cancellation, and the diagnostic/exit-code contract.
add_executable(test_safety_substrate tests/test_safety_substrate.cpp)
target_link_libraries(test_safety_substrate PRIVATE melkor_runtime)
target_compile_options(test_safety_substrate PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME safety_substrate_tests COMMAND test_safety_substrate)

# Atomic output: the existing destination must survive every injected failure.
add_executable(test_atomic_writer tests/test_atomic_writer.cpp)
target_link_libraries(test_atomic_writer PRIVATE melkor_runtime)
target_compile_options(test_atomic_writer PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME atomic_writer_tests COMMAND test_atomic_writer)

# PLY colour conversion regression (P0-07): the source property type decides the scaling.
add_executable(test_ply_color tests/test_ply_color.cpp)
target_link_libraries(test_ply_color PRIVATE melkor_core)
target_compile_options(test_ply_color PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME ply_color_tests COMMAND test_ply_color)

# Canonical math oracle: activation domains, quaternion contract, and the covariance affine
# transform that P0-17 is about.
add_executable(test_math_oracle tests/test_math_oracle.cpp)
target_link_libraries(test_math_oracle PRIVATE melkor_core)
target_compile_options(test_math_oracle PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME math_oracle_tests COMMAND test_math_oracle)

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

set(MELKOR_FUZZ_TARGETS fuzz_ply)

# The replay executables always build, so the harnesses cannot rot and the corpus is exercised
# even where no libFuzzer runtime exists.
foreach(fuzzer ${MELKOR_FUZZ_TARGETS})
    add_executable(${fuzzer}_replay fuzz/${fuzzer}.cpp)
    target_link_libraries(${fuzzer}_replay PRIVATE melkor_core)
    target_compile_options(${fuzzer}_replay PRIVATE ${MELKOR_WARNING_FLAGS})
    add_test(NAME ${fuzzer}_replay
             COMMAND ${fuzzer}_replay ${CMAKE_SOURCE_DIR}/fuzz/corpus)
endforeach()

if(MELKOR_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MELKOR_BUILD_FUZZERS requires Clang (libFuzzer).")
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
target_link_libraries(test_cloud_inspector PRIVATE melkor_runtime)
set_target_properties(test_cloud_inspector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME cloud_inspector_tests COMMAND test_cloud_inspector)

# Compile the public no-SPZ facade without inheriting melkor_core's
# MELKOR_HAS_SPZ definition. Both feature branches must expose the same result
# metadata contract to downstream code.
add_executable(test_spz_stub_api tests/test_spz_stub_api.cpp)
target_include_directories(test_spz_stub_api PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(test_spz_stub_api PRIVATE ${MELKOR_WARNING_FLAGS})
add_test(NAME spz_stub_api_tests COMMAND test_spz_stub_api)

# Fuzz test for SPZ quaternion rotation preservation across the full rotation
# space (random + edge cases). Decodes with spz's own loader so a symmetric
# double-bug cannot hide; compares rotation matrices since q/-q are equivalent.
if(SPZ_AVAILABLE)
    add_executable(test_spz_quaternion_fuzz tests/test_spz_quaternion_fuzz.cpp)
    target_link_libraries(test_spz_quaternion_fuzz PRIVATE melkor_runtime)
    target_compile_definitions(test_spz_quaternion_fuzz PRIVATE MELKOR_HAS_SPZ)
    set_target_properties(test_spz_quaternion_fuzz PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_test(NAME spz_quaternion_fuzz COMMAND test_spz_quaternion_fuzz)
endif()

# Fuzz test for the binary (and ASCII) PLY round-trip across random splats.
# Catches stride/ordering/endian regressions in both the writer and the
# header-driven reader.
add_executable(test_ply_roundtrip_fuzz tests/test_ply_roundtrip_fuzz.cpp)
target_link_libraries(test_ply_roundtrip_fuzz PRIVATE melkor_runtime)
set_target_properties(test_ply_roundtrip_fuzz PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME ply_roundtrip_fuzz COMMAND test_ply_roundtrip_fuzz)

# ComputeProvider abstraction tests: factory behavior, CPU backend math
# against the utils reference, Y-up->Z-up rotation semantics, and CPU/GPU
# parity of the shared operations when a GPU backend is present.
add_executable(test_compute_provider tests/test_compute_provider.cpp)
target_link_libraries(test_compute_provider PRIVATE melkor_runtime)
set_target_properties(test_compute_provider PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME compute_provider_tests COMMAND test_compute_provider)

# Scene-completion (densification) tests: uniform-grid neighbor search,
# hole filling on a synthetic plane, outer-boundary containment, and CPU/Metal
# parity of the grid k-NN kernels when a GPU is present.
add_executable(test_densifier tests/test_densifier.cpp)
target_link_libraries(test_densifier PRIVATE melkor_runtime)
set_target_properties(test_densifier PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME densifier_tests COMMAND test_densifier)

# Public CLI contract: numeric tokens are fully consumed/finite, unknown and
# conflicting options fail, and retired fake fit/feedforward surfaces fail
# closed with actionable guidance.
add_test(NAME cli_validation
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_cli.sh $<TARGET_FILE:melkor>)

# The gradient-check test is gone with the code it tested.
#
# It exercised DifferentiableRenderer's analytic backward pass against finite differences --
# a good test, but of a facade that was already retired at the CLI (--fit failed closed) and
# that has now been deleted along with GaussianFitter and FeedforwardModel (P1-03). A test for
# code that no longer ships is not coverage; it is confusion.

# Python test for the DA3 model-direct Gaussians extraction path. It stubs
# torch so it runs anywhere Python3 + numpy are available (no DA3 weights or
# GPU needed). Validates that the field transforms match ByteDance's exporter
# and that the DC->rgb->DC round-trip through save_ply does not drift.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME gaussians_from_prediction_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tests/test_gaussians_from_prediction.py)
    add_test(NAME release_metadata_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tests/test_release_metadata.py)
    add_test(NAME release_evidence_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tests/test_release_evidence.py)
    add_test(NAME inspect_cli_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tests/test_inspect_cli.py
                     $<TARGET_FILE:melkor>)
    # Regression tests for the repository tooling (version sync, source-bundle allowlist,
    # notice generation) -- locking in adversarial-review fixes.
    add_test(NAME tools_tests
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_tools.py)
endif()
