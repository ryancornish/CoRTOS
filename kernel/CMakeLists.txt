add_library(cortos_kernel_obj OBJECT
    src/kernel.cpp
)

target_include_directories(cortos_kernel_obj PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(cortos_kernel_obj PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(cortos_kernel_obj PUBLIC cxx_std_23)

target_link_libraries(cortos_kernel_obj PUBLIC
    cortos::port
    cortos::config
)

add_library(cortos_kernel STATIC $<TARGET_OBJECTS:cortos_kernel_obj>)
target_link_libraries(cortos_kernel PUBLIC cortos::port cortos::config)

# Alias
add_library(cortos::kernel ALIAS cortos_kernel)
