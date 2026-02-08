# ---- Create a config include target for tests
function(cortos_test_config_target out_target include_dir)
add_library(${out_target} INTERFACE)
target_include_directories(${out_target} INTERFACE ${include_dir})
endfunction()

# ---- Kernel-aware gtest: compiles kernel sources with requested config include dir
# Usage:
#   add_cortos_gtest_with_config(name test.cpp <config_include_dir> [NO_MAIN])
function(add_cortos_gtest_with_config name test_src config_include_dir)
   set(options NO_MAIN)
   cmake_parse_arguments(ARG "${options}" "" "" ${ARGN})

   # 1) config target for this test
   set(cfg_tgt ${name}__config)
   cortos_test_config_target(${cfg_tgt} ${config_include_dir})

   # 2) kernel objects compiled specifically for this test + config
   set(obj_tgt ${name}__kernel_obj)
   cortos_add_kernel_objects(${obj_tgt} ${cfg_tgt})

   # 3) build test executable using those objects
   add_executable(${name}
      ${test_src}
      $<TARGET_OBJECTS:${obj_tgt}>
   )

   # IMPORTANT: ensure the *test sources* also see the same config include dir
   target_link_libraries(${name} PRIVATE
      ${cfg_tgt}
      cortos::libcortos
      cortos::port
      cortos::time
      gtest
      $<$<NOT:$<BOOL:${ARG_NO_MAIN}>>:gtest_main>
   )

   add_test(NAME ${name} COMMAND ${name})
endfunction()

# ---- Convenience: kernel test using DEFAULT config
function(add_cortos_gtest name)
set(DEFAULT_CONFIG_DIR ${CMAKE_SOURCE_DIR}/config/include)
add_cortos_gtest_with_config(${name} ${ARGN} ${DEFAULT_CONFIG_DIR})
endfunction()

function(add_cortos_gtest_no_main name)
set(DEFAULT_CONFIG_DIR ${CMAKE_SOURCE_DIR}/config/include)
# ARGN: test sources
add_cortos_gtest_with_config(${name} ${ARGN} ${DEFAULT_CONFIG_DIR} NO_MAIN)
endfunction()

# ---- Non-kernel tests (port/time/libcortos) ----
# These do NOT compile kernel.cpp, and do not depend on cortos/config.hpp.

function(add_cortos_gtest_port name)
add_executable(${name} ${ARGN})
target_link_libraries(${name} PRIVATE
   cortos::port
   gtest
   gtest_main
)
add_test(NAME ${name} COMMAND ${name})
endfunction()

function(add_cortos_gtest_time name)
add_executable(${name} ${ARGN})
target_link_libraries(${name} PRIVATE
   cortos::time
   cortos::port
   gtest
   gtest_main
)
add_test(NAME ${name} COMMAND ${name})
endfunction()

function(add_cortos_gtest_libcortos name)
add_executable(${name} ${ARGN})
target_link_libraries(${name} PRIVATE
   cortos::libcortos
   gtest
   gtest_main
)
add_test(NAME ${name} COMMAND ${name})
endfunction()

# ---- Subdirs
add_subdirectory(kernel)
add_subdirectory(port)
add_subdirectory(time)
add_subdirectory(libcortos)
add_subdirectory(integration)
