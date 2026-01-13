# Kernel unit tests

# Function tests (type-erased callable)
add_cortos_test(test_function test_function.cpp)
target_link_libraries(test_function PRIVATE
   cortos::kernel
)

add_cortos_non_gtest_test(test_single_thread test_single_thread.cpp)
target_link_libraries(test_single_thread PRIVATE
   cortos::kernel
)

# Spinlock tests
# add_cortos_test(test_spinlock test_spinlock.cpp)
# target_link_libraries(test_spinlock PRIVATE
#    cortos::kernel
#    Threads::Threads  # For std::thread in tests
# )