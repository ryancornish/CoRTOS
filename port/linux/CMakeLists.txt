add_library(cortos_port_linux STATIC
   port_linux.cpp
)

target_include_directories(cortos_port_linux PUBLIC
   ${CMAKE_SOURCE_DIR}/port/include
)

target_compile_features(cortos_port_linux PUBLIC cxx_std_23)

# Link against pthread for TLS and context switching support
find_package(Threads REQUIRED)
target_link_libraries(cortos_port_linux PUBLIC Threads::Threads)

# Alias for consistent naming across ports
add_library(cortos::port ALIAS cortos_port_linux)
