function(modern_sqlite_add_format_targets)
  find_program(MODERN_SQLITE_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
  if(NOT MODERN_SQLITE_CLANG_FORMAT_EXECUTABLE)
    foreach(target IN ITEMS format check-format)
      add_custom_target(
        "${target}"
        COMMAND
          "${CMAKE_COMMAND}"
          -DTOOL=clang-format
          -P "${PROJECT_SOURCE_DIR}/cmake/MissingTool.cmake"
        VERBATIM
      )
    endforeach()
    return()
  endif()

  add_custom_target(
    format
    COMMAND "${MODERN_SQLITE_CLANG_FORMAT_EXECUTABLE}" -i ${ARGN}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Formatting C++ sources"
    VERBATIM
  )
  add_custom_target(
    check-format
    COMMAND
      "${MODERN_SQLITE_CLANG_FORMAT_EXECUTABLE}"
      --dry-run
      --Werror
      ${ARGN}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Checking C++ source formatting"
    VERBATIM
  )
endfunction()
