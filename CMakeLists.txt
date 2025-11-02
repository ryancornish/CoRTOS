cmake_minimum_required(VERSION 3.16)
project(CornishRTK LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include path for public headers
include_directories(${CMAKE_SOURCE_DIR}/include)

add_subdirectory(port/linux)
add_subdirectory(kernel)
add_subdirectory(tests)

