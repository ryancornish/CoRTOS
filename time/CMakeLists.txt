add_library(cortos_time STATIC
    src/time_driver_periodic.cpp
    src/time_driver_simulation.cpp
)

target_include_directories(cortos_time PUBLIC
    ${CMAKE_SOURCE_DIR}/time/include
)

target_compile_features(cortos_time PUBLIC cxx_std_23)

# Simulation time driver uses std::thread
find_package(Threads REQUIRED)
target_link_libraries(cortos_time PUBLIC Threads::Threads)

# Alias
add_library(cortos::time ALIAS cortos_time)
