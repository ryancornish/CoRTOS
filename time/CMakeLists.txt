# Time driver implementation selection
set(CORTOS_TIME_DRIVER "simulation" CACHE STRING "TimeDriver implementation (periodic, simulation)")
set_property(CACHE CORTOS_TIME_DRIVER PROPERTY STRINGS periodic simulation)

# Select the appropriate implementation
if(CORTOS_TIME_DRIVER STREQUAL "periodic")
   set(TIME_DRIVER_SRC src/time_driver_periodic.cpp)
elseif(CORTOS_TIME_DRIVER STREQUAL "simulation")
   set(TIME_DRIVER_SRC src/time_driver_simulation.cpp)
else()
   message(FATAL_ERROR "Unknown time driver: ${CORTOS_TIME_DRIVER}")
endif()

add_library(cortos_time STATIC
   ${TIME_DRIVER_SRC}
)

target_include_directories(cortos_time PUBLIC
   ${CMAKE_SOURCE_DIR}/time/include
)

target_compile_features(cortos_time PUBLIC cxx_std_23)

# Simulation time driver uses std::thread
if(CORTOS_TIME_DRIVER STREQUAL "simulation")
   find_package(Threads REQUIRED)
   target_link_libraries(cortos_time PUBLIC Threads::Threads)
endif()

# Alias
add_library(cortos::time ALIAS cortos_time)

# Inform user which driver was selected
message(STATUS "CoRTOS TimeDriver: ${CORTOS_TIME_DRIVER}")