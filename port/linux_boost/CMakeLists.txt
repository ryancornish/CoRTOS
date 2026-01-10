find_package(Boost REQUIRED COMPONENTS context)

add_library(cortos_port_boost STATIC
   port_linux_boost.cpp
)

target_include_directories(cortos_port_boost PRIVATE
   ${CMAKE_SOURCE_DIR}/port/include
)

target_include_directories(cortos_port_boost PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(cortos_port_boost PUBLIC cxx_std_23)

target_link_libraries(cortos_port_boost PUBLIC
   Boost::context
   pthread
)

# Alias for consistent naming
add_library(cortos::port ALIAS cortos_port_boost)