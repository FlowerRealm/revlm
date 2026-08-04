file(GLOB_RECURSE REVLM_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER REVLM_CORE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
list(FILTER REVLM_CORE_SOURCES EXCLUDE REGEX ".*/main_worker\\.cpp$")

add_library(revlm_core SHARED ${REVLM_CORE_SOURCES})
add_library(revlm::core ALIAS revlm_core)

target_include_directories(revlm_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(revlm_core PUBLIC
  Boost::json
  Boost::url
  Boost::random
  revlm::http
  revlm::odb
  date::date-tz
  PkgConfig::CRYPT
  OpenSSL::SSL
  OpenSSL::Crypto
  ZLIB::ZLIB
  Threads::Threads
)
target_link_libraries(revlm_core PRIVATE revlm_warnings)
target_compile_definitions(revlm_core PRIVATE
  REVLM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(revlm_core PROPERTIES
  VERSION 1.0.0
  SOVERSION 1
  CXX_VISIBILITY_PRESET default
  VISIBILITY_INLINES_HIDDEN OFF
)

# The worker is intentionally interposable. On ELF, do not let the compiler
# turn intra-library calls into non-preemptible local calls: preload modules
# replace ordinary C++ symbols before this library is loaded.
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(revlm_core PRIVATE -fsemantic-interposition)
endif()

# Plugins intentionally compile against the same core headers as the worker.
# There is no narrowed SDK or registrar: a trusted module may replace any
# interposable core symbol with the exact ABI.
include(CMakePackageConfigHelpers)
install(TARGETS revlm_core
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include
  FILES_MATCHING PATTERN "*.hpp")
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/RevlmCoreConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/RevlmCoreConfig.cmake"
  INSTALL_DESTINATION lib/cmake/RevlmCore)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/RevlmCoreConfig.cmake"
  DESTINATION lib/cmake/RevlmCore)

set_property(TARGET revlm_core PROPERTY ODB_HEADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/odb_entities.hpp"
)
odb_compile(revlm_core)

include(EmbedSqlMigrations)
embed_sql_migrations(revlm_core "${CMAKE_CURRENT_SOURCE_DIR}/migrations")

add_executable(revlm src/main.cpp)
target_link_libraries(revlm PRIVATE revlm_core revlm_warnings)

add_executable(revlm_worker src/main_worker.cpp)
set_target_properties(revlm_worker PROPERTIES OUTPUT_NAME "revlm-worker")
target_link_libraries(revlm_worker PRIVATE revlm_core revlm_warnings)
install(TARGETS revlm revlm_worker RUNTIME DESTINATION bin)

if(REVLM_BUILD_TESTS)
  add_subdirectory(tests)
  # Build the channel data-plane plugins against the just-built revlm_core so
  # the plugin modules are exercised by the test build (and .so/.dylib
  # artifacts land under build/plugins/<target>).
  add_subdirectory("${CMAKE_SOURCE_DIR}/plugins/revlm-plugin" plugins/revlm-plugin)
endif()
