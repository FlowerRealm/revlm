cmake_minimum_required(VERSION 3.20)

project(revlm_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(REVLM_VERSION "$ENV{REVLM_VERSION}" CACHE STRING "Revlm build version")
set(REVLM_BUILD_DATE "$ENV{REVLM_BUILD_DATE}" CACHE STRING "Revlm build date")
if(NOT REVLM_VERSION)
  set(REVLM_VERSION "dev")
endif()
if(NOT REVLM_BUILD_DATE)
  set(REVLM_BUILD_DATE "unknown")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)
include(CompilerWarnings)

option(REVLM_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(backend)
