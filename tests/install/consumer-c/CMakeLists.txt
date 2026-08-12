# Test the installed C ABI from a standalone C project.
# The project can see only the installed prefix.

cmake_minimum_required(VERSION 3.24)
project(melkor_consumer_c LANGUAGES C)

if(DEFINED MELKOR_CONSUMER_FIND_VERSION)
    find_package(Melkor ${MELKOR_CONSUMER_FIND_VERSION} CONFIG REQUIRED)
else()
    find_package(Melkor CONFIG REQUIRED)
endif()

add_executable(consumer_c main.c)
target_link_libraries(consumer_c PRIVATE Melkor::melkor)
set_target_properties(consumer_c PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
target_compile_options(consumer_c PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<C_COMPILER_ID:MSVC>:/WX>)

enable_testing()
add_test(NAME consumer_c_runs COMMAND consumer_c)
