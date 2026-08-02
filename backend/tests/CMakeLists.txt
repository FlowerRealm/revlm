add_library(revlm_test_preload_override SHARED plugins/preload_override.cpp)
target_link_libraries(revlm_test_preload_override PRIVATE revlm_core)
set_target_properties(revlm_test_preload_override PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-plugins")

add_executable(revlm_plugin_preload_probe plugins/preload_probe.cpp)
target_link_libraries(revlm_plugin_preload_probe PRIVATE revlm_core revlm_warnings)

add_executable(revlm_provider_preload_probe plugins/provider_preload_probe.cpp)
target_link_libraries(revlm_provider_preload_probe PRIVATE revlm_core revlm_warnings)

file(GLOB_RECURSE REVLM_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

function(revlm_add_unit_test test_cpp)
  get_filename_component(test_basename "${test_cpp}" NAME_WE)
  set(target "revlm_${test_basename}")
  add_executable(${target} "${test_cpp}")
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(${target} PRIVATE
    REVLM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    REVLM_TEST_PRELOAD_PROBE_PATH="$<TARGET_FILE:revlm_plugin_preload_probe>"
    REVLM_TEST_PRELOAD_OVERRIDE_PATH="$<TARGET_FILE:revlm_test_preload_override>"
    REVLM_TEST_PROVIDER_PRELOAD_PROBE_PATH="$<TARGET_FILE:revlm_provider_preload_probe>"
    REVLM_TEST_OPENAI_PLUGIN_PATH="$<TARGET_FILE:OpenAI>"
    REVLM_TEST_ANTHROPIC_PLUGIN_PATH="$<TARGET_FILE:Anthropic>")
  target_link_libraries(${target} PRIVATE revlm_core revlm_warnings)
  add_dependencies(${target} OpenAI Anthropic revlm_test_preload_override revlm_plugin_preload_probe
    revlm_provider_preload_probe)
  add_test(NAME ${target} COMMAND ${target})
  if(NOT APPLE)
    set(_revlm_test_preload "LD_PRELOAD=$<TARGET_FILE:OpenAI> $<TARGET_FILE:Anthropic>")
    set_tests_properties(${target} PROPERTIES ENVIRONMENT "${_revlm_test_preload}")
  endif()
  set_tests_properties(${target} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

foreach(test_cpp IN LISTS REVLM_TEST_SOURCES)
  revlm_add_unit_test("${test_cpp}")
endforeach()
