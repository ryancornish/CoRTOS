# Time driver implementation selection
set(CORTOS_TIME_DRIVER "simulation" CACHE STRING "TimeDriver implementation (periodic, tickless, simulation)")
set_property(CACHE CORTOS_TIME_DRIVER PROPERTY STRINGS periodic tickless simulation)

# Always compile the base interface
add_library(cortos_time STATIC
   base/time_driver.cpp
)

# Base include directory (interface only)
target_include_directories(cortos_time PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(cortos_time PUBLIC cxx_std_23)

# Select the appropriate driver implementation
if(CORTOS_TIME_DRIVER STREQUAL "periodic")
   target_sources(cortos_time PRIVATE
      periodic/time_driver_periodic.cpp
   )
   target_include_directories(cortos_time PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/periodic/include
   )

   target_link_libraries(cortos_time PUBLIC
      cortos::port
   )

elseif(CORTOS_TIME_DRIVER STREQUAL "tickless")
   target_sources(cortos_time PRIVATE
      tickless/time_driver_tickless.cpp
   )
   target_include_directories(cortos_time PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/tickless/include
   )

   target_link_libraries(cortos_time PUBLIC
      cortos::port
   )

elseif(CORTOS_TIME_DRIVER STREQUAL "simulation")
   target_sources(cortos_time PRIVATE
      simulation/time_driver_simulation.cpp
   )
   target_include_directories(cortos_time PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/simulation/include
   )

   find_package(Threads REQUIRED)
   target_link_libraries(cortos_time PUBLIC
      cortos::port
      Threads::Threads
   )

else()
   message(FATAL_ERROR "Unknown time driver: ${CORTOS_TIME_DRIVER}")
endif()

# Alias
add_library(cortos::time ALIAS cortos_time)

# Inform user which driver was selected
message(STATUS "CoRTOS TimeDriver: ${CORTOS_TIME_DRIVER}")
