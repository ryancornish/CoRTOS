# Generic time driver tests (interface tests)
add_cortos_test(test_time_driver test_time_driver.cpp)

# Simulation driver tests (always compiled, links against simulation driver)
add_executable(test_time_driver_simulation test_time_driver_simulation.cpp)
target_link_libraries(test_time_driver_simulation PRIVATE
   gtest_main
   gtest
   cortos::time  # For ITimeDriver interface
)

# Add simulation driver source directly (not linked through cortos::time)
target_sources(test_time_driver_simulation PRIVATE
   ${CMAKE_SOURCE_DIR}/time/base/time_driver.cpp
   ${CMAKE_SOURCE_DIR}/time/simulation/time_driver_simulation.cpp
)

target_include_directories(test_time_driver_simulation PRIVATE
   ${CMAKE_SOURCE_DIR}/time/include
   ${CMAKE_SOURCE_DIR}/time/simulation/include
)

# Find Threads for SimulationTimeDriver
find_package(Threads REQUIRED)
target_link_libraries(test_time_driver_simulation PRIVATE Threads::Threads)

add_test(NAME test_time_driver_simulation COMMAND test_time_driver_simulation)

# Periodic driver tests (always compiled, links against periodic driver)
add_executable(test_time_driver_periodic test_time_driver_periodic.cpp)
target_link_libraries(test_time_driver_periodic PRIVATE
   gtest_main
   cortos::time  # For ITimeDriver interface
)

# Add periodic driver source directly
target_sources(test_time_driver_periodic PRIVATE
   ${CMAKE_SOURCE_DIR}/time/base/time_driver.cpp
   ${CMAKE_SOURCE_DIR}/time/periodic/time_driver_periodic.cpp
)

target_include_directories(test_time_driver_periodic PRIVATE
   ${CMAKE_SOURCE_DIR}/time/include
   ${CMAKE_SOURCE_DIR}/time/periodic/include
)

add_test(NAME test_time_driver_periodic COMMAND test_time_driver_periodic)
