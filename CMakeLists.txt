cmake_minimum_required(VERSION 3.20)

project(CoRTOS
   VERSION 0.1.0
   DESCRIPTION "A small, modern C++ real-time kernel for embedded systems"
   LANGUAGES CXX C ASM
)

# Export compile commands for clangd/IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CORTOS_BUILD_TESTS "Build tests" ON)
option(CORTOS_BUILD_EXAMPLES "Build examples" ON)
option(CORTOS_ENABLE_ASSERTIONS "Enable runtime assertions" ON)

include(cmake/CodeCoverage.cmake)

set(CORTOS_PORT "linux_boost" CACHE STRING "Port to build (linux_boost, cortex-m, riscv)")
set_property(CACHE CORTOS_PORT PROPERTY STRINGS linux_boost cortex-m riscv)

add_library(cortos_warnings INTERFACE)
target_compile_options(cortos_warnings INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror;-Wno-unused-parameter>
)

# ---- config (default) is header-only, provided by target
add_subdirectory(config)

# Build port layer (must provide cortos::port target)
add_subdirectory(port/${CORTOS_PORT})

# Build time driver
add_subdirectory(time)

# Build kernel (object lib template + default compiled lib)
add_subdirectory(kernel)

# Build libcortos
add_subdirectory(libcortos)

if(CORTOS_BUILD_TESTS)
  add_subdirectory(external/googletest)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CORTOS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

setup_coverage()
