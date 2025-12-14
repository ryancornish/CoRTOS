add_library(cortos_simulation STATIC cortos_simulation.cpp)
target_link_libraries(cortos_simulation PUBLIC cortos_kernel)


if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/two_thread_equal_prio.cpp")
  add_executable(two_thread_equal_prio two_thread_equal_prio.cpp)
  target_link_libraries(two_thread_equal_prio PRIVATE cortos_kernel)
  target_include_directories(two_thread_equal_prio PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()


if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/three_thread_multi_prio.cpp")
  add_executable(three_thread_multi_prio three_thread_multi_prio.cpp)
  target_link_libraries(three_thread_multi_prio PRIVATE cortos_kernel)
  target_include_directories(three_thread_multi_prio PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/two_thread_mutex.cpp")
  add_executable(two_thread_mutex two_thread_mutex.cpp)
  target_link_libraries(two_thread_mutex PRIVATE cortos_kernel)
  target_include_directories(two_thread_mutex PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/four_thread_timed_mutex.cpp")
  add_executable(four_thread_timed_mutex four_thread_timed_mutex.cpp)
  target_link_libraries(four_thread_timed_mutex PRIVATE cortos_kernel)
  target_include_directories(four_thread_timed_mutex PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/priority_inheritance_stress_test.cpp")
  add_executable(priority_inheritance_stress_test priority_inheritance_stress_test.cpp)
  target_link_libraries(priority_inheritance_stress_test PRIVATE cortos_kernel)
  target_include_directories(priority_inheritance_stress_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/priority_inheritance_stress_test2.cpp")
  add_executable(priority_inheritance_stress_test2 priority_inheritance_stress_test2.cpp)
  target_link_libraries(priority_inheritance_stress_test2 PRIVATE cortos_kernel)
  target_include_directories(priority_inheritance_stress_test2 PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/semaphore.cpp")
  add_executable(semaphore semaphore.cpp)
  target_link_libraries(semaphore PRIVATE cortos_kernel)
  target_include_directories(semaphore PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/condition_variable.cpp")
  add_executable(condition_variable condition_variable.cpp)
  target_link_libraries(condition_variable PRIVATE cortos_simulation gtest gtest_main)
  target_include_directories(condition_variable PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME condition_variable COMMAND condition_variable)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/timers.cpp")
  add_executable(timers timers.cpp)
  target_link_libraries(timers PRIVATE cortos_kernel)
  target_include_directories(timers PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test1.cpp")
  add_executable(test1 test1.cpp)

  target_link_libraries(test1 PRIVATE cortos_kernel gtest gtest_main)
  add_test(NAME test1 COMMAND test1)
endif()