# Embed backend/migrations/NNNN_name.sql into a generated C++ header.
#
# Usage:
#   include(EmbedSqlMigrations)
#   embed_sql_migrations(revlm_core "${CMAKE_CURRENT_SOURCE_DIR}/migrations")

function(embed_sql_migrations target migrations_dir)
  get_filename_component(_migrations_dir "${migrations_dir}" ABSOLUTE)
  set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/migrations_gen")
  set(_out_hxx "${_gen_dir}/embedded_migrations.hxx")
  set(_gen_script "${CMAKE_SOURCE_DIR}/cmake/GenerateEmbeddedMigrations.cmake")

  file(GLOB _migration_files CONFIGURE_DEPENDS "${_migrations_dir}/[0-9][0-9][0-9][0-9]_*.sql")
  list(SORT _migration_files)

  add_custom_command(
    OUTPUT "${_out_hxx}"
    COMMAND "${CMAKE_COMMAND}"
      "-DMIGRATIONS_DIR=${_migrations_dir}"
      "-DOUT_HXX=${_out_hxx}"
      -P "${_gen_script}"
    DEPENDS ${_migration_files} "${_gen_script}"
    COMMENT "Embedding SQL migrations"
    VERBATIM
  )
  add_custom_target(${target}_migrations DEPENDS "${_out_hxx}")
  add_dependencies(${target} ${target}_migrations)
  target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${_gen_dir}>")

  # Ensure the header exists before the first compile of dependent TUs.
  get_target_property(_sources ${target} SOURCES)
  if(_sources)
    set_source_files_properties(${_sources} TARGET_DIRECTORY ${target}
      PROPERTIES OBJECT_DEPENDS "${_out_hxx}")
  endif()
endfunction()
