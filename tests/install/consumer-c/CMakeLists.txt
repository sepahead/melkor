# A standalone C consumer of the installed Melkor SDK.
#
# This project is deliberately OUTSIDE the main build. It is configured against an installed
# prefix, exactly as a real downstream user would, so it proves the thing P0-04 is about: that
# `cmake --install` produces a package another project can actually find and link. A test that
# builds inside the main tree would prove nothing, because it would see the source headers
# directly.

cmake_minimum_required(VERSION 3.24)
project(melkor_consumer_c LANGUAGES C)

find_package(Melkor 2 CONFIG REQUIRED)

add_executable(consumer_c main.c)
target_link_libraries(consumer_c PRIVATE Melkor::melkor)

enable_testing()
add_test(NAME consumer_c_runs COMMAND consumer_c)
