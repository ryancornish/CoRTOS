cmake_minimum_required(VERSION 3.20)

project(CoRTOS
   VERSION 0.1.0
   DESCRIPTION "A small, modern C++ real-time kernel for embedded systems"
   LANGUAGES CXX C ASM
)

# Export compile commands for clangd/IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# C++23 required
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter")

# Build options
option(CORTOS_BUILD_TESTS "Build tests" ON)
option(CORTOS_BUILD_EXAMPLES "Build examples" ON)
option(CORTOS_ENABLE_ASSERTIONS "Enable runtime assertions" ON)

# CMake modules
include(cmake/CodeCoverage.cmake)

# Port selection
set(CORTOS_PORT "linux_boost" CACHE STRING "Port to build (linux_boost, cortex-m, riscv)")
set_property(CACHE CORTOS_PORT PROPERTY STRINGS linux_boost cortex-m riscv)

# Public include directories (visible to all targets)
include_directories(
   ${CMAKE_SOURCE_DIR}/kernel/include
   ${CMAKE_SOURCE_DIR}/time/include
   ${CMAKE_SOURCE_DIR}/port/include
   ${CMAKE_SOURCE_DIR}/libcortos/include
)

# Build port layer (must provide cortos::port target)
add_subdirectory(port/${CORTOS_PORT})

# Build time driver
add_subdirectory(time)

# Build kernel
add_subdirectory(kernel)

# Build libcortos (standard library)
add_subdirectory(libcortos)

# Tests (with Google Test)
if(CORTOS_BUILD_TESTS)
   add_subdirectory(external/googletest)
   enable_testing()
   add_subdirectory(tests)
endif()

# Examples
if(CORTOS_BUILD_EXAMPLES)
   add_subdirectory(examples)
endif()

# Coverage make
setup_coverage()
