# Port layer tests
add_cortos_gtest(test_port test_port.cpp)

# Note: These tests are for the port layer fundamentals
# They don't require the kernel to be implemented yet