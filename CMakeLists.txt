cmake_minimum_required(VERSION 3.16)

project(CoRTOS LANGUAGES C CXX)

# Global settings
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -Wno-unused")

# Public headers
include_directories(${CMAKE_SOURCE_DIR}/include)

# Port backend
set(CORTOS_PORT "linux/boost_context" CACHE STRING "CoRTOS backend port")
set_property(CACHE CORTOS_PORT PROPERTY STRINGS linux linux/boost_context)

# Build selected port (must provide target cortos_port and alias cortos::port)
add_subdirectory(port/${CORTOS_PORT})

# Kernel & tests
add_subdirectory(kernel)
add_subdirectory(tests)
