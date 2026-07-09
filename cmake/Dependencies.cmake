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

# SOCI: pkg-config on Debian, CMake config on from-source installs.
pkg_check_modules(SOCI IMPORTED_TARGET soci_core soci_mysql)
add_library(revlm_soci INTERFACE)
if(SOCI_FOUND)
  target_link_libraries(revlm_soci INTERFACE PkgConfig::SOCI)
else()
  find_package(soci 4.1 REQUIRED CONFIG COMPONENTS Core MySQL)
  target_link_libraries(revlm_soci INTERFACE SOCI::Core SOCI::MySQL)
endif()
add_library(revlm::soci ALIAS revlm_soci)

# SOCI's MySQL backend needs the client library.
pkg_check_modules(MYSQLCLIENT IMPORTED_TARGET mariadb)
if(NOT MYSQLCLIENT_FOUND)
  pkg_check_modules(MYSQLCLIENT IMPORTED_TARGET libmariadb)
endif()
if(NOT MYSQLCLIENT_FOUND)
  pkg_check_modules(MYSQLCLIENT REQUIRED IMPORTED_TARGET mysqlclient)
endif()
