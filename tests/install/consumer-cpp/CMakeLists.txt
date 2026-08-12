# Test the installed C ABI from a standalone C++ project.
# The installed SDK does not publish a C++ API.

cmake_minimum_required(VERSION 3.24)
project(melkor_consumer_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(DEFINED MELKOR_CONSUMER_FIND_VERSION)
    find_package(Melkor ${MELKOR_CONSUMER_FIND_VERSION} CONFIG REQUIRED)
else()
    find_package(Melkor CONFIG REQUIRED)
endif()

add_executable(consumer_cpp main.cpp)
target_link_libraries(consumer_cpp PRIVATE Melkor::melkor)
target_compile_options(consumer_cpp PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<CXX_COMPILER_ID:MSVC>:/WX>
    $<$<CXX_COMPILER_ID:MSVC>:/permissive->
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>)

enable_testing()
add_test(NAME consumer_cpp_runs COMMAND consumer_cpp)
