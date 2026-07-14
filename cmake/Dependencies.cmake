# Discovered through environment-driven mechanisms: CMake config files
# (CMAKE_PREFIX_PATH), pkg-config (PKG_CONFIG_PATH) and compiler/linker default
# search paths (CPATH / LIBRARY_PATH / LD_LIBRARY_PATH). No vendor-specific
# prefixes (Homebrew, conda, vcpkg, ...) are hard-coded.

find_package(OpenSSL REQUIRED)
find_package(Boost 1.83 REQUIRED CONFIG COMPONENTS json url)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# cpp-httplib: pkg-config on Debian, CMake config on Homebrew.
pkg_check_modules(CPPHTTPLIB IMPORTED_TARGET cpp-httplib)
add_library(revlm_http INTERFACE)
if(CPPHTTPLIB_FOUND)
  target_link_libraries(revlm_http INTERFACE PkgConfig::CPPHTTPLIB)
else()
  find_package(httplib REQUIRED CONFIG)
  target_link_libraries(revlm_http INTERFACE httplib::httplib)
endif()
add_library(revlm::http ALIAS revlm_http)

pkg_check_modules(CRYPT REQUIRED IMPORTED_TARGET libxcrypt)

# Resolve the real MySQL client *before* adding $HOME/opt/odb to
# PKG_CONFIG_PATH — that prefix ships a mysqlclient.pc shim that can shadow
# system packages and omit headers ODB needs (<mysql/mysql_time.h>).
#
# Prefer Oracle mysqlclient over MariaDB connector: linking both causes
# MYSQL* ABI mismatches and segfaults in mysql_real_query. Homebrew's
# libodb-mysql is typically built against mysql-client.
if(DEFINED ENV{HOMEBREW_PREFIX})
  set(_revlm_mysql_pc "$ENV{HOMEBREW_PREFIX}/opt/mysql-client/lib/pkgconfig")
elseif(EXISTS "/opt/homebrew/opt/mysql-client/lib/pkgconfig")
  set(_revlm_mysql_pc "/opt/homebrew/opt/mysql-client/lib/pkgconfig")
elseif(EXISTS "/usr/local/opt/mysql-client/lib/pkgconfig")
  set(_revlm_mysql_pc "/usr/local/opt/mysql-client/lib/pkgconfig")
endif()
if(_revlm_mysql_pc)
  set(ENV{PKG_CONFIG_PATH} "${_revlm_mysql_pc}:$ENV{PKG_CONFIG_PATH}")
endif()

pkg_check_modules(MYSQLCLIENT IMPORTED_TARGET mysqlclient)
if(NOT MYSQLCLIENT_FOUND)
  pkg_check_modules(MYSQLCLIENT IMPORTED_TARGET libmariadb)
endif()
if(NOT MYSQLCLIENT_FOUND)
  pkg_check_modules(MYSQLCLIENT REQUIRED IMPORTED_TARGET mariadb)
endif()

# ODB often lives in a user prefix (e.g. $HOME/opt/odb) that IDEs do not put on
# PKG_CONFIG_PATH. Honor that layout when present.
set(_revlm_odb_prefix "")
if(DEFINED ENV{HOME} AND EXISTS "$ENV{HOME}/opt/odb")
  set(_revlm_odb_prefix "$ENV{HOME}/opt/odb")
  if(EXISTS "${_revlm_odb_prefix}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_PATH} "${_revlm_odb_prefix}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  endif()
  list(PREPEND CMAKE_PREFIX_PATH "${_revlm_odb_prefix}")
  if(EXISTS "${_revlm_odb_prefix}/bin")
    list(PREPEND CMAKE_PROGRAM_PATH "${_revlm_odb_prefix}/bin")
  endif()
endif()

pkg_check_modules(ODB REQUIRED IMPORTED_TARGET libodb)
pkg_check_modules(ODB_MYSQL REQUIRED IMPORTED_TARGET libodb-mysql)

add_library(revlm_odb INTERFACE)
target_link_libraries(revlm_odb INTERFACE PkgConfig::ODB PkgConfig::ODB_MYSQL PkgConfig::MYSQLCLIENT)

# Ensure <mysql/mysql_*.h> resolves. Prefer a full client tree (mysql_time.h),
# not the incomplete $HOME/opt/odb mysql/ shim that only satisfies mysql.h.
foreach(_mysql_inc IN LISTS MYSQLCLIENT_INCLUDE_DIRS)
  get_filename_component(_mysql_inc_name "${_mysql_inc}" NAME)
  if(_mysql_inc_name STREQUAL "mysql" OR _mysql_inc_name STREQUAL "mariadb")
    get_filename_component(_mysql_parent "${_mysql_inc}" DIRECTORY)
    target_include_directories(revlm_odb INTERFACE "${_mysql_parent}")
  endif()
endforeach()
set(_revlm_mysql_hints ${MYSQLCLIENT_INCLUDE_DIRS})
if(DEFINED ENV{HOMEBREW_PREFIX})
  list(APPEND _revlm_mysql_hints
    "$ENV{HOMEBREW_PREFIX}/opt/mysql-client/include"
    "$ENV{HOMEBREW_PREFIX}/opt/mariadb-connector-c/include")
endif()
foreach(_brew_root IN ITEMS /opt/homebrew /usr/local)
  list(APPEND _revlm_mysql_hints
    "${_brew_root}/opt/mysql-client/include"
    "${_brew_root}/opt/mariadb-connector-c/include")
endforeach()
find_path(REVLM_MYSQL_INCLUDE_DIR
  NAMES mysql/mysql_time.h
  HINTS ${_revlm_mysql_hints}
  PATH_SUFFIXES .. include
)
if(NOT REVLM_MYSQL_INCLUDE_DIR)
  message(FATAL_ERROR
    "Could not find mysql/mysql_time.h (needed by libodb-mysql). "
    "Install mysql-client or mariadb development headers.")
endif()
target_include_directories(revlm_odb INTERFACE "${REVLM_MYSQL_INCLUDE_DIR}")

# libodb-mysql.pc emits bare -lmysqlclient; Apple ld needs an explicit -L.
# Reuse the same client pkg-config already selected above — never mix
# libmysqlclient with libmariadb in one link line.
if(MYSQLCLIENT_LIBRARY_DIRS)
  target_link_directories(revlm_odb INTERFACE ${MYSQLCLIENT_LIBRARY_DIRS})
endif()
# If pkg-config only gave -lmariadb but ODB needs -lmysqlclient, map the
# library directory that actually contains libmysqlclient.
find_library(REVLM_MYSQLCLIENT_LIB
  NAMES mysqlclient
  HINTS ${MYSQLCLIENT_LIBRARY_DIRS}
        /opt/homebrew/opt/mysql-client/lib
        /usr/local/opt/mysql-client/lib
)
if(REVLM_MYSQLCLIENT_LIB)
  get_filename_component(_revlm_mysql_libdir "${REVLM_MYSQLCLIENT_LIB}" DIRECTORY)
  target_link_directories(revlm_odb INTERFACE "${_revlm_mysql_libdir}")
elseif(NOT MYSQLCLIENT_LIBRARIES MATCHES "mariadb")
  message(FATAL_ERROR
    "Could not find libmysqlclient (needed to link libodb-mysql). "
    "Install mysql-client development libraries.")
endif()

add_library(revlm::odb ALIAS revlm_odb)

include(OdbCompile)
