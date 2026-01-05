# cmake/CodeCoverage.cmake
# Code coverage configuration for CoRTOS using gcovr

option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

if(ENABLE_COVERAGE)
   if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      message(STATUS "Code coverage enabled")

      # Coverage flags
      add_compile_options(
         --coverage              # Instruments code for coverage
         -O0                     # No optimization
         -fno-inline            # Don't inline functions
         -fno-inline-small-functions
         -fno-default-inline
      )

      add_link_options(--coverage)

   else()
      message(WARNING "Code coverage only supported with GCC or Clang")
   endif()
endif()

# Add coverage target
function(setup_coverage)
   if(NOT ENABLE_COVERAGE)
      return()
   endif()

   find_program(GCOVR gcovr)

   if(NOT GCOVR)
      message(WARNING "gcovr not found. Install with: pip install gcovr")
      return()
   endif()

   # Build filter patterns WITHOUT quotes - CMake handles escaping with VERBATIM
   set(FILTER_KERNEL "${CMAKE_SOURCE_DIR}/kernel/.*")
   set(FILTER_TIME "${CMAKE_SOURCE_DIR}/time/.*")
   set(FILTER_PORT "${CMAKE_SOURCE_DIR}/port/.*")
   set(FILTER_LIBCORTOS "${CMAKE_SOURCE_DIR}/libcortos/.*")

   # HTML coverage report
   add_custom_target(coverage
      # Clean old data and create output directory
      COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/coverage_html
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage_html
      COMMAND find ${CMAKE_BINARY_DIR} -name "*.gcda" -delete || true

      # Run tests
      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure

      # Generate HTML report
      COMMAND ${GCOVR}
         --root ${CMAKE_SOURCE_DIR}
         --object-directory ${CMAKE_BINARY_DIR}
         --filter ${FILTER_KERNEL}
         --filter ${FILTER_TIME}
         --filter ${FILTER_PORT}
         --filter ${FILTER_LIBCORTOS}
         --html-details ${CMAKE_BINARY_DIR}/coverage_html/index.html
         --print-summary

      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Generating coverage report..."
      VERBATIM
   )

   # XML coverage (for CI/CD)
   add_custom_target(coverage-xml
      # Run tests
      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure

      # Generate XML
      COMMAND ${GCOVR}
         --root ${CMAKE_SOURCE_DIR}
         --object-directory ${CMAKE_BINARY_DIR}
         --filter ${FILTER_KERNEL}
         --filter ${FILTER_TIME}
         --filter ${FILTER_PORT}
         --filter ${FILTER_LIBCORTOS}
         --xml-pretty
         --output ${CMAKE_BINARY_DIR}/coverage.xml

      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Generating XML coverage..."
      VERBATIM
   )

   message(STATUS "Coverage targets configured:")
   message(STATUS "  make coverage     -> HTML at build/coverage_html/index.html")
   message(STATUS "  make coverage-xml -> XML at build/coverage.xml")

endfunction()
