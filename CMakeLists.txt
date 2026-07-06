cmake_minimum_required(VERSION 3.20)

project(revlm_cpp LANGUAGES CXX)

option(REVLM_STATIC_LINK "Link the revlm executable statically" OFF)

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

find_package(PkgConfig REQUIRED)
find_package(boost_json 1.83 REQUIRED CONFIG)
find_package(boost_url 1.83 REQUIRED CONFIG)
pkg_check_modules(CPPHTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)

find_path(REVLM_MYSQL_INCLUDE_DIR mysql.h PATH_SUFFIXES mariadb mysql)
if(NOT REVLM_MYSQL_INCLUDE_DIR)
  message(FATAL_ERROR "mysql.h not found; install libmariadb-dev")
endif()

find_path(SOCI_INCLUDE_DIR soci/soci.h)
if(NOT SOCI_INCLUDE_DIR)
  message(FATAL_ERROR "soci/soci.h not found; install libsoci-dev")
endif()

set(_REVLM_LIB_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
set(CMAKE_FIND_LIBRARY_SUFFIXES .a .so ${CMAKE_FIND_LIBRARY_SUFFIXES})
find_library(SOCI_CORE_LIBRARY NAMES soci_core REQUIRED)
find_library(SOCI_MYSQL_LIBRARY NAMES soci_mysql REQUIRED)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${_REVLM_LIB_SUFFIXES})

find_library(REVLM_MYSQLCLIENT_LIB NAMES mysqlclient libmysqlclient libmysqlclient.so.21
  PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu NO_DEFAULT_PATH)
if(NOT REVLM_MYSQLCLIENT_LIB)
  set(REVLM_MYSQLCLIENT_LIB "/usr/lib/x86_64-linux-gnu/libmysqlclient.so.21")
endif()
if(NOT EXISTS "${REVLM_MYSQLCLIENT_LIB}")
  message(FATAL_ERROR "libmysqlclient not found; install libmysqlclient21")
endif()

file(GLOB_RECURSE REVLM_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/src/*.cpp")
list(FILTER REVLM_CORE_SOURCES EXCLUDE REGEX ".*/(main|tmp_devseed)\\.cpp$")

add_library(revlm_core ${REVLM_CORE_SOURCES})
target_include_directories(revlm_core PUBLIC backend/include)
target_compile_options(revlm_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(revlm_core PUBLIC
  Boost::json
  Boost::url
  PkgConfig::CPPHTTPLIB
  ${SOCI_MYSQL_LIBRARY}
  ${SOCI_CORE_LIBRARY}
  ${REVLM_MYSQLCLIENT_LIB}
  crypto ssl crypt pthread
)
target_include_directories(revlm_core PRIVATE
  "${SOCI_INCLUDE_DIR}"
  "${REVLM_MYSQL_INCLUDE_DIR}"
)
target_compile_definitions(revlm_core PRIVATE
  REVLM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  REVLM_VERSION="${REVLM_VERSION}"
  REVLM_BUILD_DATE="${REVLM_BUILD_DATE}"
  REVLM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

add_executable(revlm backend/src/main.cpp)
target_link_libraries(revlm PRIVATE revlm_core)
target_compile_options(revlm PRIVATE -Wall -Wextra -Wpedantic)
if(REVLM_STATIC_LINK)
  target_link_options(revlm PRIVATE -static -static-libgcc -static-libstdc++)
endif()

add_executable(tmp-devseed backend/src/tmp_devseed.cpp)
target_link_libraries(tmp-devseed PRIVATE revlm_core)
target_compile_options(tmp-devseed PRIVATE -Wall -Wextra -Wpedantic)
if(REVLM_STATIC_LINK)
  target_link_options(tmp-devseed PRIVATE -static -static-libgcc -static-libstdc++)
endif()

enable_testing()

add_library(revlm_test_support INTERFACE)
target_include_directories(revlm_test_support INTERFACE backend/tests)

file(GLOB_RECURSE REVLM_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/tests/*_test.cpp")

function(revlm_add_unit_test test_cpp)
  get_filename_component(test_basename "${test_cpp}" NAME_WE)
  set(target "revlm_${test_basename}")
  add_executable(${target} "${test_cpp}")
  target_link_libraries(${target} PRIVATE revlm_core revlm_test_support)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ${target} COMMAND ${target})

  if(test_basename MATCHES "_mysql_test$"
      OR test_basename MATCHES "^http_server_mysql_(channel_admin|token_models|chat_completions|messages)_test$"
      OR test_basename STREQUAL "admin_users_test")
    set_tests_properties(${target} PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  endif()

  if(test_basename STREQUAL "token_channel_groups_mysql_test"
      OR test_basename STREQUAL "token_api_mysql_test"
      OR test_basename STREQUAL "admin_channel_groups_contract_test")
    target_compile_definitions(${target} PRIVATE
      REVLM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  endif()
endfunction()

foreach(test_cpp IN LISTS REVLM_TEST_SOURCES)
  revlm_add_unit_test("${test_cpp}")
endforeach()
