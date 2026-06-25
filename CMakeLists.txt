cmake_minimum_required(VERSION 3.20)
project(melkor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Detect platform
if(APPLE)
    set(MELKOR_PLATFORM "APPLE")
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment version")
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
    elseif(CUDAToolkit_FOUND)
        message(STATUS "CUDA available but not enabled. Use -DMELKOR_USE_CUDA=ON to enable.")
    endif()
endif()

# Option to build with SPZ support (requires submodule)
option(MELKOR_BUILD_SPZ "Build with SPZ support" ON)

# Third-party dependencies directory
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)

# tinygltf (header-only)
add_library(tinygltf INTERFACE)
target_include_directories(tinygltf INTERFACE ${THIRD_PARTY_DIR}/tinygltf)
target_compile_definitions(tinygltf INTERFACE
    TINYGLTF_NO_STB_IMAGE_WRITE
    TINYGLTF_NO_EXTERNAL_IMAGE
)

# stb_image for tinygltf
add_library(stb_image INTERFACE)
target_include_directories(stb_image INTERFACE ${THIRD_PARTY_DIR}/stb)

# SPZ library (if available)
if(MELKOR_BUILD_SPZ AND EXISTS ${THIRD_PARTY_DIR}/spz/CMakeLists.txt)
    add_subdirectory(${THIRD_PARTY_DIR}/spz)
    set(SPZ_AVAILABLE ON)
else()
    set(SPZ_AVAILABLE OFF)
    message(STATUS "SPZ not available - SPZ encoding will be disabled")
endif()

# Core library (platform-independent)
add_library(melkor_core STATIC
    src/glb_reader.cpp
    src/ply_writer.cpp
    src/gaussian_data.cpp
    src/enhanced_converter.cpp
    src/feedforward_model.cpp
    src/spz_encoder.cpp
)

target_include_directories(melkor_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(melkor_core PUBLIC
    tinygltf
    stb_image
)

if(SPZ_AVAILABLE)
    target_link_libraries(melkor_core PUBLIC spz)
    target_compile_definitions(melkor_core PUBLIC MELKOR_HAS_SPZ)
    # Add SPZ include directories
    target_include_directories(melkor_core PUBLIC ${THIRD_PARTY_DIR}/spz/src/cc)
endif()
# Enable strict warnings on Melkor's own code (not third-party).
target_compile_options(melkor_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)

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
        src/metal/gaussian_fitter.mm
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
    # melkor_core calls into melkor_metal for GPU-accelerated enhanced
    # conversion and k-NN. CMake resolves the circular static-library
    # dependency by re-scanning archives at link time.
    target_link_libraries(melkor_core PUBLIC melkor_metal)
    
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
    
    # Enable CUDA language
    enable_language(CUDA)
    
    # CUDA compute library
    add_library(melkor_cuda STATIC
        src/cuda/cuda_compute.cpp
        src/cuda/gaussian_processor.cu
    )
    
    target_include_directories(melkor_cuda PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
    
    target_link_libraries(melkor_cuda PUBLIC
        melkor_core
        CUDA::cudart
    )
    
    target_compile_definitions(melkor_cuda PUBLIC MELKOR_HAS_CUDA)
    
    # Set CUDA architecture (adjust based on your GPU)
    # SM 60 = Pascal (GTX 10xx), SM 70 = Volta (V100), SM 75 = Turing (RTX 20xx)
    # SM 80 = Ampere (A100), SM 86 = Ampere (RTX 30xx), SM 89 = Ada (RTX 40xx)
    # SM 90 = Hopper (H100) - requires CUDA 12.0+
    if(CUDAToolkit_VERSION VERSION_LESS "11.8")
        # CUDA < 11.8: basic architectures only
        set(CUDA_ARCHS "60;70;75;80;86")
        message(STATUS "CUDA ${CUDAToolkit_VERSION}: Using architectures ${CUDA_ARCHS}")
    elseif(CUDAToolkit_VERSION VERSION_LESS "12.0")
        # CUDA 11.8+: include SM 89 (Ada/RTX 40xx)
        set(CUDA_ARCHS "60;70;75;80;86;89")
        message(STATUS "CUDA ${CUDAToolkit_VERSION}: Using architectures ${CUDA_ARCHS}")
    else()
        # CUDA 12.0+: include SM 89 (Ada) and SM 90 (Hopper)
        set(CUDA_ARCHS "60;70;75;80;86;89;90")
        message(STATUS "CUDA ${CUDAToolkit_VERSION}: Using architectures ${CUDA_ARCHS}")
    endif()
    
    set_target_properties(melkor_cuda PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_ARCHITECTURES "${CUDA_ARCHS}"
    )
    
    set(MELKOR_GPU_LIBRARY melkor_cuda)
    set(MELKOR_HAS_GPU ON)
    set(MELKOR_GPU_BACKEND "CUDA")
    
else()
    message(STATUS "Building CPU-only version (no GPU acceleration)")
    
    # Stub library for non-GPU platforms
    add_library(melkor_gpu_stub STATIC
        src/gpu_stub.cpp
    )
    
    target_include_directories(melkor_gpu_stub PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
    
    target_link_libraries(melkor_gpu_stub PUBLIC
        melkor_core
    )
    
    set(MELKOR_GPU_LIBRARY melkor_gpu_stub)
    set(MELKOR_HAS_GPU OFF)
    set(MELKOR_GPU_BACKEND "CPU")
endif()

# Main CLI executable
add_executable(melkor
    src/main.cpp
)

target_link_libraries(melkor PRIVATE
    melkor_core
    ${MELKOR_GPU_LIBRARY}
)
target_compile_options(melkor PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)

# Copy metallib to executable directory (macOS only)
if(APPLE AND MELKOR_USE_METAL)
    add_custom_command(TARGET melkor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${METAL_LIBRARY_OUTPUT} $<TARGET_FILE_DIR:melkor>/default.metallib
    )
endif()

# Installation
install(TARGETS melkor RUNTIME DESTINATION bin)
if(APPLE AND MELKOR_USE_METAL)
    install(FILES ${METAL_LIBRARY_OUTPUT} DESTINATION bin)
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
target_link_libraries(test_melkor_core PRIVATE melkor_core)
if(SPZ_AVAILABLE)
    target_link_libraries(test_melkor_core PRIVATE spz)
    target_compile_definitions(test_melkor_core PRIVATE MELKOR_HAS_SPZ)
endif()
set_target_properties(test_melkor_core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME melkor_core_tests COMMAND test_melkor_core)

# Fuzz test for SPZ quaternion rotation preservation across the full rotation
# space (random + edge cases). Decodes with spz's own loader so a symmetric
# double-bug cannot hide; compares rotation matrices since q/-q are equivalent.
if(SPZ_AVAILABLE)
    add_executable(test_spz_quaternion_fuzz tests/test_spz_quaternion_fuzz.cpp)
    target_link_libraries(test_spz_quaternion_fuzz PRIVATE melkor_core spz)
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
target_link_libraries(test_ply_roundtrip_fuzz PRIVATE melkor_core)
set_target_properties(test_ply_roundtrip_fuzz PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME ply_roundtrip_fuzz COMMAND test_ply_roundtrip_fuzz)

# Gradient-check test for the differentiable rasterizer (macOS/Metal only).
# Compares the analytic backward gradients against central finite differences
# for color and opacity -- the correctness bar for any backward pass. Gated
# on MELKOR_HAS_METAL because the DifferentiableRenderer lives in the Metal TU.
if(APPLE AND MELKOR_USE_METAL)
    add_executable(test_gradient_check tests/test_gradient_check.mm)
    target_link_libraries(test_gradient_check PRIVATE melkor_metal)
    set_target_properties(test_gradient_check PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_test(NAME gradient_check COMMAND test_gradient_check)
endif()

# Python test for the DA3 model-direct Gaussians extraction path. It stubs
# torch so it runs anywhere Python3 + numpy are available (no DA3 weights or
# GPU needed). Validates that the field transforms match ByteDance's exporter
# and that the DC->rgb->DC round-trip through save_ply does not drift.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME gaussians_from_prediction_tests
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tests/test_gaussians_from_prediction.py)
endif()
