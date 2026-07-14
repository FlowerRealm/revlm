file(GLOB_RECURSE REVLM_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER REVLM_CORE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

add_library(revlm_core ${REVLM_CORE_SOURCES})
add_library(revlm::core ALIAS revlm_core)

target_include_directories(revlm_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(revlm_core PUBLIC
  Boost::json
  Boost::url
  revlm::http
  revlm::odb
  date::date-tz
  PkgConfig::CRYPT
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)
target_link_libraries(revlm_core PRIVATE revlm_warnings)
target_compile_definitions(revlm_core PRIVATE
  REVLM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_property(TARGET revlm_core PROPERTY ODB_HEADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/odb_entities.hpp"
)
odb_compile(revlm_core)

include(EmbedSqlMigrations)
embed_sql_migrations(revlm_core "${CMAKE_CURRENT_SOURCE_DIR}/migrations")

add_executable(revlm src/main.cpp)
target_link_libraries(revlm PRIVATE revlm_core revlm_warnings)

if(REVLM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
