function(modern_sqlite_enable_clang_tidy target)
  if(NOT MODERN_SQLITE_ENABLE_CLANG_TIDY)
    return()
  endif()

  get_target_property(target_source_directory "${target}" SOURCE_DIR)
  get_target_property(target_sources "${target}" SOURCES)
  if(NOT target_sources)
    return()
  endif()

  set(production_source_directory "${PROJECT_SOURCE_DIR}/src")
  set(has_production_source OFF)
  foreach(source IN LISTS target_sources)
    if(source MATCHES "^\\$<")
      return()
    endif()
    if(NOT source MATCHES "\\.(cc|cpp|cxx)$")
      continue()
    endif()

    set(source_path "${source}")
    cmake_path(
      ABSOLUTE_PATH source_path
      BASE_DIRECTORY "${target_source_directory}"
      NORMALIZE
    )
    cmake_path(
      IS_PREFIX production_source_directory
      "${source_path}"
      NORMALIZE
      is_production_source
    )
    if(NOT is_production_source)
      return()
    endif()
    set(has_production_source ON)
  endforeach()

  if(NOT has_production_source)
    return()
  endif()

  find_program(MODERN_SQLITE_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
  set(
    clang_tidy_command
    "${MODERN_SQLITE_CLANG_TIDY_EXECUTABLE}"
    "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    "--warnings-as-errors=*"
  )
  if(MSVC)
    set_property(TARGET "${target}" PROPERTY EXPORT_COMPILE_COMMANDS ON)
    list(APPEND clang_tidy_command "-p" "${PROJECT_BINARY_DIR}")
  endif()
  set_property(TARGET "${target}" PROPERTY CXX_CLANG_TIDY "${clang_tidy_command}")
endfunction()
