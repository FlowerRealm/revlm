# Tests load plugins the way production does: real packages on disk, opened with
# dlopen by the host. The LD_PRELOAD wiring this replaces could never work on
# macOS at all -- Mach-O's two-level namespace will not let a preloaded module
# override an already-bound core symbol -- so every test that went through a
# plugin route was silently Linux-only. Staging real packages fixes that.
set(REVLM_TEST_PLUGIN_ROOT "${CMAKE_BINARY_DIR}/test-plugins")

# Must match plugins/scan.cpp's current_platform_name().
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(REVLM_PLUGIN_PLATFORM "arm")
else()
  set(REVLM_PLUGIN_PLATFORM "amd")
endif()

# Staged with a stamp file rather than a POST_BUILD hook: the plugin targets are
# created in the submodule's own directory, and add_custom_command(TARGET ...)
# only works where its target was defined.
function(revlm_stage_test_plugin plugin_target)
  set(package_dir "${REVLM_TEST_PLUGIN_ROOT}/packages/${plugin_target}")
  set(source_dir "${CMAKE_SOURCE_DIR}/plugins/revlm-plugin/plugins/${plugin_target}")
  set(stamp "${REVLM_TEST_PLUGIN_ROOT}/${plugin_target}.staged")
  add_custom_command(OUTPUT "${stamp}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${package_dir}/backend/${REVLM_PLUGIN_PLATFORM}" "${package_dir}/frontend"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${plugin_target}>"
      "${package_dir}/backend/${REVLM_PLUGIN_PLATFORM}/$<TARGET_FILE_NAME:${plugin_target}>"
    COMMAND "${CMAKE_COMMAND}" -E copy "${source_dir}/plugin.json" "${package_dir}/plugin.json"
    COMMAND "${CMAKE_COMMAND}" -E touch "${package_dir}/frontend/entry.js"
    COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
    DEPENDS ${plugin_target} "${source_dir}/plugin.json"
    COMMENT "Staging ${plugin_target} as an installed plugin package for tests")
  add_custom_target(stage_${plugin_target} DEPENDS "${stamp}")
endfunction()

revlm_stage_test_plugin(OpenAI)
revlm_stage_test_plugin(Anthropic)

file(GLOB_RECURSE REVLM_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

function(revlm_add_unit_test test_cpp)
  get_filename_component(test_basename "${test_cpp}" NAME_WE)
  set(target "revlm_${test_basename}")
  add_executable(${target} "${test_cpp}")
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(${target} PRIVATE
    REVLM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    REVLM_TEST_PLUGIN_DIR="${REVLM_TEST_PLUGIN_ROOT}")
  target_link_libraries(${target} PRIVATE revlm_core revlm_warnings)
  # Plugins resolve core symbols out of the test binary, same as they resolve
  # them out of the revlm executable in production.
  set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
  add_dependencies(${target} stage_OpenAI stage_Anthropic)
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES
    ENVIRONMENT "REVLM_PLUGIN_DIR=${REVLM_TEST_PLUGIN_ROOT}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

foreach(test_cpp IN LISTS REVLM_TEST_SOURCES)
  revlm_add_unit_test("${test_cpp}")
endforeach()
