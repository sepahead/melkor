# A standalone C++ consumer of the installed Melkor SDK.
#
# Like the C consumer, this is configured against an installed prefix, not the source tree. It
# proves the C++ public headers install and compile against a relocated package, and that the
# header-inline facilities (Result, the version macros) work without the source tree present.
#
# It deliberately uses only header-inline C++ facilities. The compiled C++ API is not yet a
# frozen exported ABI -- that would prematurely lock symbols around types later work is still
# reshaping -- so the supported cross-language boundary remains the C ABI, exercised by the C
# consumer.

cmake_minimum_required(VERSION 3.24)
project(melkor_consumer_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Melkor 2 CONFIG REQUIRED)

add_executable(consumer_cpp main.cpp)
target_link_libraries(consumer_cpp PRIVATE Melkor::melkor)

enable_testing()
add_test(NAME consumer_cpp_runs COMMAND consumer_cpp)
