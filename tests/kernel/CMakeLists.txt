# Kernel unit tests

# Function tests (type-erased callable)
add_cortos_gtest(test_function test_function.cpp)
target_link_libraries(test_function PRIVATE
   cortos::kernel
)

add_cortos_gtest_no_main(test_single_thread test_single_thread.cpp)
target_link_libraries(test_single_thread PRIVATE
   cortos::kernel
)

add_cortos_gtest_no_main(test_simple_multi_threading test_simple_multi_threading.cpp)
target_link_libraries(test_simple_multi_threading PRIVATE
   cortos::kernel
)


# Spinlock tests
# add_cortos_test(test_spinlock test_spinlock.cpp)
# target_link_libraries(test_spinlock PRIVATE
#    cortos::kernel
#    Threads::Threads  # For std::thread in tests
# )