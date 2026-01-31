# Kernel unit tests

# Function tests (type-erased callable)
add_cortos_gtest(test_function test_function.cpp)
target_link_libraries(test_function PRIVATE
   cortos::kernel
)

add_cortos_gtest_no_main(test_singlecore_singlethreading test_singlecore_singlethreading.cpp)
target_link_libraries(test_singlecore_singlethreading PRIVATE
   cortos::kernel
)

add_cortos_gtest_no_main(test_singlecore_multithreading test_singlecore_multithreading.cpp)
target_link_libraries(test_singlecore_multithreading PRIVATE
   cortos::kernel
)

add_cortos_gtest_no_main(test_multicore_multithreading test_multicore_multithreading.cpp)
target_link_libraries(test_multicore_multithreading PRIVATE
   cortos::kernel
)

# Spinlock tests
# add_cortos_test(test_spinlock test_spinlock.cpp)
# target_link_libraries(test_spinlock PRIVATE
#    cortos::kernel
#    Threads::Threads  # For std::thread in tests
# )