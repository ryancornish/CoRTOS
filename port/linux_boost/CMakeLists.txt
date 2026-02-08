find_package(Boost REQUIRED COMPONENTS context)
find_package(Threads REQUIRED)

add_library(cortos_port_boost STATIC
   port_linux_boost.cpp
)

# Publish the public API include path(s) to consumers (kernel, tests, etc.)
target_include_directories(cortos_port_boost
   PUBLIC
      ${CMAKE_SOURCE_DIR}/port/include          # provides cortos/port.h
      ${CMAKE_CURRENT_SOURCE_DIR}/include       # port-specific extras (if any)
)

target_compile_features(cortos_port_boost PUBLIC cxx_std_23)

target_link_libraries(cortos_port_boost PUBLIC
   Boost::context
   Threads::Threads
   cortos_warnings
)

add_library(cortos::port ALIAS cortos_port_boost)
