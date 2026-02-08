# Store kernel dir globally so functions can see it no matter where they're called from.
set(CORTOS_KERNEL_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "CoRTOS kernel directory")

function(cortos_add_kernel_objects out_target config_target)
add_library(${out_target} OBJECT
   "${CORTOS_KERNEL_DIR}/src/kernel.cpp"
)

target_include_directories(${out_target}
   PUBLIC
      "${CORTOS_KERNEL_DIR}/include"
)

target_link_libraries(${out_target}
   PUBLIC
      ${config_target}     # provides cortos/config.hpp include path
      cortos_warnings
      cortos::port
      cortos::time
)
endfunction()

# Default kernel objects (default config)
cortos_add_kernel_objects(cortos_kernel_obj_default cortos::config)

# Normal kernel library for non-test consumers
add_library(cortos_kernel STATIC
$<TARGET_OBJECTS:cortos_kernel_obj_default>
)
add_library(cortos::kernel ALIAS cortos_kernel)

target_include_directories(cortos_kernel
PUBLIC
   "${CORTOS_KERNEL_DIR}/include"
)

target_link_libraries(cortos_kernel
PUBLIC
   cortos_warnings
   cortos::config
   cortos::port
   cortos::time
)
