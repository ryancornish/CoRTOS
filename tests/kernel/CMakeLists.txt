# Kernel unit tests

# 1. Function tests (type-erased callable)
# Uses default config automatically via the helper function
add_cortos_gtest(test_function test_function.cpp)

# 2. Single-core Single-threading (Specialised Config)
add_cortos_gtest_no_main(test_singlecore_singlethreading test_singlecore_singlethreading.cpp)
target_include_directories(test_singlecore_singlethreading BEFORE PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}/singlecore_config/include
)

# 3. Single-core Multi-threading (Specialised Config)
add_cortos_gtest_no_main(test_singlecore_multithreading test_singlecore_multithreading.cpp)
target_include_directories(test_singlecore_multithreading BEFORE PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}/singlecore_config/include
)

# 4. Multi-core Multi-threading (Uses Default Config)
add_cortos_gtest_no_main(test_multicore_multithreading test_multicore_multithreading.cpp)
target_include_directories(test_multicore_multithreading BEFORE PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}/multicore_config/include
)

# Spinlock tests
# add_cortos_gtest(test_spinlock test_spinlock.cpp)
# target_link_libraries(test_spinlock PRIVATE
#    Threads::Threads  # Still needed for host-side threading tests
# )
