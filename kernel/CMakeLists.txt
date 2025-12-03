# Kernel library
add_library(cortos_kernel STATIC
  cortos_kernel.cpp
)

# Kernel depends on the selected port.
target_link_libraries(cortos_kernel PUBLIC cortos::port)

# Expose public headers
target_include_directories(cortos_kernel PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)
