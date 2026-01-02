add_library(cortos STATIC
   src/mutex.cpp
   #src/semaphore.cpp
   #src/condition_variable.cpp
)

target_include_directories(cortos PUBLIC
   ${CMAKE_SOURCE_DIR}/libcortos/include
)

target_compile_features(cortos PUBLIC cxx_std_23)

# libcortos is built on top of the kernel
target_link_libraries(cortos PUBLIC
   cortos::kernel
)

# Alias
add_library(cortos::libcortos ALIAS cortos)
