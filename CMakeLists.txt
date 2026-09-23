cmake_minimum_required(VERSION 3.20)
project(hydrolix_exercise LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(HDX_SANITIZE "Build with ASan + UBSan" OFF)

add_library(hdx INTERFACE)
target_include_directories(hdx INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(hdx INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(HDX_SANITIZE)
  target_compile_options(hdx INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(hdx INTERFACE -fsanitize=address,undefined)
endif()

enable_testing()
add_executable(tests tests/tests.cpp)
target_link_libraries(tests PRIVATE hdx)
# Keep asserts active in tests regardless of build type.
target_compile_options(tests PRIVATE -UNDEBUG)
add_test(NAME tests COMMAND tests)

add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE hdx)
target_compile_options(bench PRIVATE -O3 -march=native)
