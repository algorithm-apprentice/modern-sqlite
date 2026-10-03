function(modern_sqlite_enable_clang_tidy target)
  if(NOT MODERN_SQLITE_ENABLE_CLANG_TIDY)
    return()
  endif()

  find_program(MODERN_SQLITE_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
  set(
    clang_tidy_command
    "${MODERN_SQLITE_CLANG_TIDY_EXECUTABLE}"
    "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    "--warnings-as-errors=*"
  )
  set_property(TARGET "${target}" PROPERTY CXX_CLANG_TIDY "${clang_tidy_command}")
endfunction()
