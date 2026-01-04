add_library(cortos_kernel STATIC
    src/kernel.cpp
)

target_include_directories(cortos_kernel PUBLIC
    ${CMAKE_SOURCE_DIR}/kernel/include
)

target_compile_features(cortos_kernel PUBLIC cxx_std_23)

target_link_libraries(cortos_kernel PUBLIC
    cortos::port
)

# Alias
add_library(cortos::kernel ALIAS cortos_kernel)
