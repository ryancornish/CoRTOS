
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/two_thread_equal_prio.cpp")
  add_executable(two_thread_equal_prio two_thread_equal_prio.cpp)
  target_link_libraries(two_thread_equal_prio PRIVATE cornishrtk_kernel)
  target_include_directories(two_thread_equal_prio PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_compile_features(two_thread_equal_prio PRIVATE cxx_std_23)
endif()


if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/three_thread_multi_prio.cpp")
  add_executable(three_thread_multi_prio three_thread_multi_prio.cpp)
  target_link_libraries(three_thread_multi_prio PRIVATE cornishrtk_kernel)
  target_include_directories(three_thread_multi_prio PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_compile_features(three_thread_multi_prio PRIVATE cxx_std_23)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/two_thread_mutex.cpp")
  add_executable(two_thread_mutex two_thread_mutex.cpp)
  target_link_libraries(two_thread_mutex PRIVATE cornishrtk_kernel)
  target_include_directories(two_thread_mutex PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_compile_features(two_thread_mutex PRIVATE cxx_std_23)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/four_thread_timed_mutex.cpp")
  add_executable(four_thread_timed_mutex four_thread_timed_mutex.cpp)
  target_link_libraries(four_thread_timed_mutex PRIVATE cornishrtk_kernel)
  target_include_directories(four_thread_timed_mutex PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_compile_features(four_thread_timed_mutex PRIVATE cxx_std_23)
endif()