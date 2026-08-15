cmake_minimum_required(VERSION 3.20)

project(revlm_cpp LANGUAGES CXX)

# C++23 for std::expected, which carries plugin-facing failures as return values
# instead of exceptions crossing the control-plane C ABI (ADR 0008).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# clangd looks for compile_commands.json at the source root.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/compile_commands.json")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink
    "${CMAKE_BINARY_DIR}/compile_commands.json"
    "${CMAKE_SOURCE_DIR}/compile_commands.json")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)
include(CompilerWarnings)

option(REVLM_BUILD_TESTS "Build unit tests" ON)

if(REVLM_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(backend)
