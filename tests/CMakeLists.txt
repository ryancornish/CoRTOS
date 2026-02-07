# Helper function to create tests
# Replaces 'cortos::kernel' with '$<TARGET_OBJECTS:cortos_kernel_obj>'
# to ensure kernel.cpp is recompiled with the test's specific config.
function(add_cortos_gtest name)
   add_executable(${name} ${ARGN} $<TARGET_OBJECTS:cortos_kernel_obj>)
   target_link_libraries(${name} PRIVATE
      cortos::libcortos
      cortos::port      # Added explicitly as kernel_obj needs it
      gtest_main
      gtest
   )
   # Inherit public headers from the kernel (kernel.hpp)
   target_include_directories(${name} PRIVATE $<TARGET_PROPERTY:cortos_kernel_obj,INTERFACE_INCLUDE_DIRECTORIES>)

   add_test(NAME ${name} COMMAND ${name})
endfunction()

function(add_cortos_gtest_no_main name)
   add_executable(${name} ${ARGN} $<TARGET_OBJECTS:cortos_kernel_obj>)
   target_link_libraries(${name} PRIVATE
      cortos::libcortos
      cortos::port
      gtest
   )
   target_include_directories(${name} PRIVATE $<TARGET_PROPERTY:cortos_kernel_obj,INTERFACE_INCLUDE_DIRECTORIES>)

   add_test(NAME ${name} COMMAND ${name})
endfunction()

function(add_cortos_standalone_test name)
   add_executable(${name} ${ARGN} $<TARGET_OBJECTS:cortos_kernel_obj>)
   target_link_libraries(${name} PRIVATE
      cortos::libcortos
      cortos::port
   )
   target_include_directories(${name} PRIVATE $<TARGET_PROPERTY:cortos_kernel_obj,INTERFACE_INCLUDE_DIRECTORIES>)

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
