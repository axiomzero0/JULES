# JULES compiler — CMake build (spec fidelity; build.sh is the sandbox driver).
cmake_minimum_required(VERSION 3.16)
project(julesc CXX)

# C++26 with graceful fallback for older toolchains.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-std=c++26" COMPILER_HAS_CXX26)
check_cxx_compiler_flag("-std=c++2c" COMPILER_HAS_CXX2C)
if (COMPILER_HAS_CXX26)
    set(CMAKE_CXX_STANDARD 26)
    add_compile_options(-std=c++26)
elseif (COMPILER_HAS_CXX2C)
    set(CMAKE_CXX_STANDARD 26)
    add_compile_options(-std=c++2c)
else ()
    set(CMAKE_CXX_STANDARD 23)
    message(WARNING "C++26 unavailable; falling back to C++23")
endif ()

# Project laws: no exceptions, no RTTI in the compiler itself.
add_compile_options(-fno-exceptions -fno-rtti -Wall -Wextra)

include("${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake")

add_executable(julesc ${JULES_SOURCES})
target_include_directories(julesc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
