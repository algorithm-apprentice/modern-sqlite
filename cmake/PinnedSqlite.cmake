if(MODERN_SQLITE_PINNED_SQLITE_C STREQUAL "" OR
   MODERN_SQLITE_PINNED_SQLITE_H STREQUAL "")
  message(
    FATAL_ERROR
    "Pinned SQLite requires MODERN_SQLITE_PINNED_SQLITE_C and "
    "MODERN_SQLITE_PINNED_SQLITE_H"
  )
endif()
if(NOT EXISTS "${MODERN_SQLITE_PINNED_SQLITE_C}" OR
   NOT EXISTS "${MODERN_SQLITE_PINNED_SQLITE_H}")
  message(FATAL_ERROR "Pinned SQLite source inputs do not exist")
endif()
file(SHA256 "${MODERN_SQLITE_PINNED_SQLITE_C}" _modern_sqlite_benchmark_c_sha256)
file(SHA256 "${MODERN_SQLITE_PINNED_SQLITE_H}" _modern_sqlite_benchmark_h_sha256)
if(NOT _modern_sqlite_benchmark_c_sha256 STREQUAL
   "4cee66a6b5eecaf9a016fd62bdc64be75a1f5ab1c34c5881a539fdca639fada6")
  message(FATAL_ERROR "Pinned sqlite3.c has the wrong SHA-256")
endif()
if(NOT _modern_sqlite_benchmark_h_sha256 STREQUAL
   "ab12aaa090d3921ba8fd2c8c198ae1e8cafce79fd9897a5b89830fab1f6ecef5")
  message(FATAL_ERROR "Pinned sqlite3.h has the wrong SHA-256")
endif()

enable_language(C)
add_library(
  modern_sqlite_benchmark_sqlite
  STATIC
  EXCLUDE_FROM_ALL
    "${MODERN_SQLITE_PINNED_SQLITE_C}"
)
add_library(
  modern_sqlite_benchmark_sqlite_shared
  SHARED
    "${MODERN_SQLITE_PINNED_SQLITE_C}"
)
set_target_properties(
  modern_sqlite_benchmark_sqlite_shared
  PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
get_filename_component(
  _modern_sqlite_benchmark_header_dir
  "${MODERN_SQLITE_PINNED_SQLITE_H}"
  DIRECTORY
)
foreach(
  _modern_sqlite_benchmark_sqlite_target
  IN ITEMS
    modern_sqlite_benchmark_sqlite
    modern_sqlite_benchmark_sqlite_shared
)
  set_target_properties(
    ${_modern_sqlite_benchmark_sqlite_target}
    PROPERTIES
      C_STANDARD 99
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS YES
  )
  target_compile_definitions(
    ${_modern_sqlite_benchmark_sqlite_target}
    PRIVATE
      NDEBUG
      SQLITE_DQS=3
      SQLITE_THREADSAFE=0
  )
  target_include_directories(
    ${_modern_sqlite_benchmark_sqlite_target}
    PUBLIC
      "${_modern_sqlite_benchmark_header_dir}"
  )
  target_link_libraries(
    ${_modern_sqlite_benchmark_sqlite_target}
    PUBLIC
      ${CMAKE_DL_LIBS}
  )
  if(UNIX AND NOT APPLE)
    target_link_libraries(
      ${_modern_sqlite_benchmark_sqlite_target}
      PUBLIC
        m
    )
  endif()
endforeach()
