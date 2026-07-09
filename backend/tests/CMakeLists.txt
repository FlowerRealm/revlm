add_library(revlm_test_support INTERFACE)
target_include_directories(revlm_test_support INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(revlm_test_support INTERFACE
  REVLM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

file(GLOB_RECURSE REVLM_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

function(revlm_add_unit_test test_cpp)
  get_filename_component(test_basename "${test_cpp}" NAME_WE)
  set(target "revlm_${test_basename}")
  add_executable(${target} "${test_cpp}")
  target_link_libraries(${target} PRIVATE revlm_core revlm_test_support revlm_warnings)
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

foreach(test_cpp IN LISTS REVLM_TEST_SOURCES)
  revlm_add_unit_test("${test_cpp}")
endforeach()
