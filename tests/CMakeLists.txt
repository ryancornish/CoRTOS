# Helper function to create tests
function(add_cortos_test name)
   add_executable(${name} ${ARGN})
   target_link_libraries(${name} PRIVATE
      cortos::kernel
      cortos::libcortos
      gtest_main
      gtest
   )
   add_test(NAME ${name} COMMAND ${name})
endfunction()

# Kernel tests
add_subdirectory(kernel)

# Port tests
add_subdirectory(port)

# Time driver tests
add_subdirectory(time)

# libcortos tests
add_subdirectory(libcortos)

# Integration tests
add_subdirectory(integration)
