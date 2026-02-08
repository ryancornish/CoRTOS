set(DEFAULT_CONFIG_DIR ${CMAKE_SOURCE_DIR}/config/include)
set(SINGLECORE_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/singlecore_config/include)
set(MULTICORE_CONFIG_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/multicore_config/include)

# 1. Function tests (default config)
add_cortos_gtest_with_config(
   test_function
   ${CMAKE_CURRENT_SOURCE_DIR}/test_function.cpp
   ${DEFAULT_CONFIG_DIR}
)

# 2. Single-core Single-threading (override config)
add_cortos_gtest_with_config(
   test_singlecore_singlethreading
   ${CMAKE_CURRENT_SOURCE_DIR}/test_singlecore_singlethreading.cpp
   ${SINGLECORE_CONFIG_DIR}
   NO_MAIN
)

# 3. Single-core Multi-threading (override config)
add_cortos_gtest_with_config(
   test_singlecore_multithreading
   ${CMAKE_CURRENT_SOURCE_DIR}/test_singlecore_multithreading.cpp
   ${SINGLECORE_CONFIG_DIR}
   NO_MAIN
)

# 4. Multi-core Multi-threading (override config)
add_cortos_gtest_with_config(
   test_multicore_multithreading
   ${CMAKE_CURRENT_SOURCE_DIR}/test_multicore_multithreading.cpp
   ${MULTICORE_CONFIG_DIR}
   NO_MAIN
)
