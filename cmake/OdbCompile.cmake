# Compile ODB-annotated headers into a single -odb.{hxx,ixx,cxx} set (--at-once).
#
# Usage:
#   set_property(TARGET revlm_core PROPERTY ODB_HEADERS
#     "${CMAKE_CURRENT_SOURCE_DIR}/include/odb_entities.hpp")
#   odb_compile(revlm_core)

find_program(ODB_EXECUTABLE odb REQUIRED)

function(odb_compile target)
  get_property(_odb_headers TARGET ${target} PROPERTY ODB_HEADERS)
  if(NOT _odb_headers)
    message(FATAL_ERROR "odb_compile(${target}): ODB_HEADERS property is empty")
  endif()

  set(_odb_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/odb_gen")
  set(_odb_tmp_dir "${_odb_gen_dir}.tmp")
  set(_include_root "${CMAKE_CURRENT_SOURCE_DIR}/include")
  file(MAKE_DIRECTORY "${_odb_gen_dir}")

  set(_odb_includes
    "-I${_include_root}"
    "-I${_odb_gen_dir}")
  if(DEFINED ODB_INCLUDE_DIRS)
    foreach(_inc IN LISTS ODB_INCLUDE_DIRS)
      list(APPEND _odb_includes "-I${_inc}")
    endforeach()
  endif()
  if(DEFINED Boost_INCLUDE_DIRS)
    foreach(_inc IN LISTS Boost_INCLUDE_DIRS)
      list(APPEND _odb_includes "-I${_inc}")
    endforeach()
  endif()

  # Prefer paths relative to include/ so generated #include paths resolve.
  set(_header_rel_list)
  set(_header_abs_list)
  foreach(_header IN LISTS _odb_headers)
    get_filename_component(_header_abs "${_header}" ABSOLUTE)
    list(APPEND _header_abs_list "${_header_abs}")
    file(RELATIVE_PATH _header_rel "${_include_root}" "${_header_abs}")
    list(APPEND _header_rel_list "${_header_rel}")
  endforeach()

  set(_out_base "revlm_entities")
  set(_out_cxx "${_odb_gen_dir}/${_out_base}-odb.cxx")
  set(_out_hxx "${_odb_gen_dir}/${_out_base}-odb.hxx")
  set(_out_ixx "${_odb_gen_dir}/${_out_base}-odb.ixx")
  set(_out_stamp "${_odb_gen_dir}/${_out_base}-odb.stamp")

  # Stamp is the sole primary OUTPUT (Makefile-safe). Generate into a temp dir
  # and publish ixx→hxx→cxx so parallel TUs never see a half-written set.
  add_custom_command(
    OUTPUT "${_out_stamp}"
    BYPRODUCTS "${_out_cxx}" "${_out_hxx}" "${_out_ixx}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_odb_tmp_dir}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_odb_tmp_dir}"
    COMMAND "${ODB_EXECUTABLE}"
      --database mysql
      --std c++20
      --generate-query
      --generate-schema
      --schema-format embedded
      --at-once
      --input-name "${_out_base}"
      --hxx-suffix .hxx
      --ixx-suffix .ixx
      --cxx-suffix .cxx
      --output-dir "${_odb_tmp_dir}"
      ${_odb_includes}
      ${_header_rel_list}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${_odb_tmp_dir}/${_out_base}-odb.ixx" "${_out_ixx}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${_odb_tmp_dir}/${_out_base}-odb.hxx" "${_out_hxx}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${_odb_tmp_dir}/${_out_base}-odb.cxx" "${_out_cxx}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_odb_tmp_dir}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_out_stamp}"
    DEPENDS ${_header_abs_list}
    WORKING_DIRECTORY "${_include_root}"
    COMMENT "ODB compiling persistent entities"
    VERBATIM
  )

  target_sources(${target} PRIVATE "${_out_cxx}")
  add_custom_target(${target}_odb_gen DEPENDS "${_out_stamp}")
  add_dependencies(${target} ${target}_odb_gen)

  # Every TU must wait for codegen — includes pull in .hxx → .ixx.
  get_target_property(_odb_target_sources ${target} SOURCES)
  set_source_files_properties(${_odb_target_sources} TARGET_DIRECTORY ${target}
    PROPERTIES OBJECT_DEPENDS "${_out_stamp}")

  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${_odb_gen_dir}>")
endfunction()
