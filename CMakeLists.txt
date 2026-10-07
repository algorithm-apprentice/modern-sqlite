cmake_minimum_required(VERSION 3.25)

project(
  modern_sqlite
  VERSION 0.1.0
  DESCRIPTION "A ground-up C++23 reimplementation of SQLite"
  LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)

option(
  MODERN_SQLITE_BUILD_TESTS
  "Build Modern SQLite tests"
  ${PROJECT_IS_TOP_LEVEL}
)
option(
  MODERN_SQLITE_WARNINGS_AS_ERRORS
  "Treat Modern SQLite compiler warnings as errors"
  ON
)
option(MODERN_SQLITE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MODERN_SQLITE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MODERN_SQLITE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MODERN_SQLITE_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)
option(MODERN_SQLITE_BUILD_FUZZERS "Build Clang libFuzzer targets" OFF)
option(MODERN_SQLITE_BUILD_BENCHMARKS "Build pinned read benchmark targets" OFF)
option(
  MODERN_SQLITE_ENABLE_INSTRUMENTATION
  "Compile and link optional instrumentation hooks"
  OFF
)
set(
  MODERN_SQLITE_PINNED_SQLITE_LIBRARY
  ""
  CACHE FILEPATH
  "Pinned SQLite shared library used only for oracle regeneration tests"
)
set(
  MODERN_SQLITE_PINNED_SQLITE_C
  ""
  CACHE FILEPATH
  "Pinned SQLite amalgamation source used by oracle and benchmark targets"
)
set(
  MODERN_SQLITE_PINNED_SQLITE_H
  ""
  CACHE FILEPATH
  "Pinned SQLite generated header used by oracle and benchmark targets"
)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/CompilerWarnings.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ClangTidy.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/Format.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/Sanitizers.cmake")

add_library(modern_sqlite_project_options INTERFACE)
target_compile_features(modern_sqlite_project_options INTERFACE cxx_std_23)
modern_sqlite_enable_sanitizers(modern_sqlite_project_options)

add_library(modern_sqlite_project_warnings INTERFACE)
modern_sqlite_set_project_warnings(modern_sqlite_project_warnings INTERFACE)

add_library(
  modern_sqlite_instrumentation
  STATIC
  EXCLUDE_FROM_ALL
    src/instrumentation/counters.cpp
)
add_library(modern_sqlite::instrumentation ALIAS modern_sqlite_instrumentation)
target_include_directories(
  modern_sqlite_instrumentation
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_compile_definitions(
  modern_sqlite_instrumentation
  PUBLIC
    MODERN_SQLITE_ENABLE_INSTRUMENTATION=1
)
target_link_libraries(
  modern_sqlite_instrumentation
  PUBLIC
    modern_sqlite_project_options
  PRIVATE
    modern_sqlite_project_warnings
)
modern_sqlite_enable_clang_tidy(modern_sqlite_instrumentation)

set(
  MODERN_SQLITE_ENGINE_SOURCES
    src/base/bytes.cpp
    src/base/coding.cpp
    src/base/result.cpp
    src/binder/bound_select.cpp
    src/bytecode/program.cpp
    src/catalog/catalog.cpp
    src/catalog/catalog_loader.cpp
    src/diagnostics/storage_inspector.cpp
    src/format/record_codec.cpp
    src/lowering/read_lowering.cpp
    src/optimizer/physical_plan.cpp
    src/pager/pager.cpp
    src/pager/write_pager.cpp
    src/planner/logical_plan.cpp
    src/platform/posix_vfs.cpp
    src/platform/vfs.cpp
    src/runtime/collation.cpp
    src/runtime/function_registry.cpp
    src/runtime/sqlite_float.cpp
    src/runtime/sql_value.cpp
    src/session/read_session.cpp
    src/storage/btree/cursor.cpp
    src/storage/btree/page.cpp
    src/storage/btree/writer.cpp
    src/storage/btree/writer_internal.cpp
    src/storage/cache/page_cache.cpp
    src/storage/database_format.cpp
    src/storage/journal/journal.cpp
    src/storage/journal/rollback_journal.cpp
    src/storage/page_bitvec.cpp
    src/syntax/ast.cpp
    src/syntax/lexer.cpp
    src/syntax/parser.cpp
    src/text/text.cpp
    src/transaction/transaction_coordinator.cpp
    src/vm/read_vm.cpp
)

add_library(
  modern_sqlite
  STATIC
    ${MODERN_SQLITE_ENGINE_SOURCES}
)
add_library(modern_sqlite::modern_sqlite ALIAS modern_sqlite)
target_include_directories(
  modern_sqlite
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  modern_sqlite
  PUBLIC
    modern_sqlite_project_options
  PRIVATE
    modern_sqlite_project_warnings
)
modern_sqlite_enable_clang_tidy(modern_sqlite)

if(MODERN_SQLITE_BUILD_FUZZERS)
  include(CheckCXXSourceCompiles)
  set(_modern_sqlite_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")

  set(CMAKE_REQUIRED_FLAGS
      "${_modern_sqlite_saved_required_flags} -fsanitize=fuzzer-no-link")
  check_cxx_source_compiles(
    "int main() { return 0; }"
    MODERN_SQLITE_HAS_FUZZER_NO_LINK
  )

  set(CMAKE_REQUIRED_FLAGS
      "${_modern_sqlite_saved_required_flags} -fsanitize=fuzzer")
  check_cxx_source_compiles(
    "
      #include <cstddef>
      extern \"C\" int LLVMFuzzerTestOneInput(
          const unsigned char*, std::size_t) {
        return 0;
      }
    "
    MODERN_SQLITE_HAS_FUZZER_RUNTIME
  )
  set(CMAKE_REQUIRED_FLAGS "${_modern_sqlite_saved_required_flags}")
  unset(_modern_sqlite_saved_required_flags)

  if(NOT MODERN_SQLITE_HAS_FUZZER_NO_LINK OR
     NOT MODERN_SQLITE_HAS_FUZZER_RUNTIME)
    message(
      FATAL_ERROR
      "MODERN_SQLITE_BUILD_FUZZERS requires working "
      "-fsanitize=fuzzer-no-link and -fsanitize=fuzzer flags"
    )
  endif()

  add_library(
    modern_sqlite_fuzz_engine
    STATIC
      ${MODERN_SQLITE_ENGINE_SOURCES}
  )
  target_include_directories(
    modern_sqlite_fuzz_engine
    PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
      "$<INSTALL_INTERFACE:include>"
  )
  target_link_libraries(
    modern_sqlite_fuzz_engine
    PUBLIC
      modern_sqlite_project_options
    PRIVATE
      modern_sqlite_project_warnings
  )
  target_compile_options(
    modern_sqlite_fuzz_engine
    PRIVATE
      -fsanitize=fuzzer-no-link
  )
  modern_sqlite_enable_clang_tidy(modern_sqlite_fuzz_engine)

  add_executable(
    modern_sqlite_read_sql_fuzz
    tests/fuzz/read_sql_fuzz.cpp
    tests/fuzz/read_sql_fuzzer_main.cpp
  )
  target_compile_definitions(
    modern_sqlite_read_sql_fuzz
    PRIVATE
      MODERN_SQLITE_READ_FUZZ_DATABASE="${PROJECT_SOURCE_DIR}/tests/fixtures/read_compatibility/sqlite-3.54.0-read-compatibility.db"
  )

  add_executable(
    modern_sqlite_read_database_fuzz
    tests/fuzz/database_image_fuzz.cpp
    tests/fuzz/database_image_fuzzer_main.cpp
  )

  foreach(
    fuzz_target
    IN ITEMS
      modern_sqlite_read_sql_fuzz
      modern_sqlite_read_database_fuzz
  )
    target_link_libraries(
      "${fuzz_target}"
      PRIVATE
        modern_sqlite_fuzz_engine
    )
    target_compile_options("${fuzz_target}" PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options("${fuzz_target}" PRIVATE -fsanitize=fuzzer)
    modern_sqlite_set_project_warnings("${fuzz_target}" PRIVATE)
    modern_sqlite_enable_clang_tidy("${fuzz_target}")
  endforeach()
endif()

if(MODERN_SQLITE_BUILD_BENCHMARKS)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Pinned performance benchmarks require a Release build")
  endif()
  if(MODERN_SQLITE_ENABLE_ASAN OR
     MODERN_SQLITE_ENABLE_UBSAN OR
     MODERN_SQLITE_ENABLE_TSAN OR
     MODERN_SQLITE_ENABLE_INSTRUMENTATION)
    message(
      FATAL_ERROR
      "Pinned timing benchmarks require sanitizers and ordinary instrumentation to be disabled"
    )
  endif()
  if(MODERN_SQLITE_PINNED_SQLITE_C STREQUAL "" OR
     MODERN_SQLITE_PINNED_SQLITE_H STREQUAL "")
    message(
      FATAL_ERROR
      "Pinned performance benchmarks require MODERN_SQLITE_PINNED_SQLITE_C and "
      "MODERN_SQLITE_PINNED_SQLITE_H"
    )
  endif()
  if(NOT EXISTS "${MODERN_SQLITE_PINNED_SQLITE_C}" OR
     NOT EXISTS "${MODERN_SQLITE_PINNED_SQLITE_H}")
    message(FATAL_ERROR "Pinned SQLite benchmark source inputs do not exist")
  endif()
  file(SHA256 "${MODERN_SQLITE_PINNED_SQLITE_C}" _modern_sqlite_benchmark_c_sha256)
  file(SHA256 "${MODERN_SQLITE_PINNED_SQLITE_H}" _modern_sqlite_benchmark_h_sha256)
  if(NOT _modern_sqlite_benchmark_c_sha256 STREQUAL
     "4cee66a6b5eecaf9a016fd62bdc64be75a1f5ab1c34c5881a539fdca639fada6")
    message(FATAL_ERROR "Pinned benchmark sqlite3.c has the wrong SHA-256")
  endif()
  if(NOT _modern_sqlite_benchmark_h_sha256 STREQUAL
     "ab12aaa090d3921ba8fd2c8c198ae1e8cafce79fd9897a5b89830fab1f6ecef5")
    message(FATAL_ERROR "Pinned benchmark sqlite3.h has the wrong SHA-256")
  endif()

  execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    RESULT_VARIABLE _modern_sqlite_benchmark_git_revision_result
    OUTPUT_VARIABLE MODERN_SQLITE_BENCHMARK_GIT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  execute_process(
    COMMAND git rev-parse HEAD^{tree}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    RESULT_VARIABLE _modern_sqlite_benchmark_git_tree_result
    OUTPUT_VARIABLE MODERN_SQLITE_BENCHMARK_GIT_TREE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(NOT _modern_sqlite_benchmark_git_revision_result EQUAL 0 OR
     NOT _modern_sqlite_benchmark_git_tree_result EQUAL 0)
    message(FATAL_ERROR "Pinned performance benchmarks require Git source identity")
  endif()
  set(_modern_sqlite_benchmark_generated_dir
      "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(MAKE_DIRECTORY "${_modern_sqlite_benchmark_generated_dir}")
  configure_file(
    benchmarks/read_performance_build_config.hpp.in
    "${_modern_sqlite_benchmark_generated_dir}/read_performance_build_config.hpp"
    @ONLY
  )

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

  add_executable(
    modern_sqlite_read_benchmark
    benchmarks/read_performance.cpp
  )
  target_include_directories(
    modern_sqlite_read_benchmark
    PRIVATE
      "${_modern_sqlite_benchmark_generated_dir}"
  )
  target_link_libraries(
    modern_sqlite_read_benchmark
    PRIVATE
      modern_sqlite::modern_sqlite
      modern_sqlite_benchmark_sqlite
  )
  modern_sqlite_set_project_warnings(modern_sqlite_read_benchmark PRIVATE)
  modern_sqlite_enable_clang_tidy(modern_sqlite_read_benchmark)

  add_library(
    modern_sqlite_read_diagnostic_engine
    STATIC
      ${MODERN_SQLITE_ENGINE_SOURCES}
  )
  target_include_directories(
    modern_sqlite_read_diagnostic_engine
    PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
      "$<INSTALL_INTERFACE:include>"
  )
  target_compile_definitions(
    modern_sqlite_read_diagnostic_engine
    PUBLIC
      MODERN_SQLITE_ENABLE_INSTRUMENTATION=1
  )
  target_link_libraries(
    modern_sqlite_read_diagnostic_engine
    PUBLIC
      modern_sqlite_project_options
    PRIVATE
      modern_sqlite_project_warnings
      modern_sqlite::instrumentation
  )
  modern_sqlite_enable_clang_tidy(modern_sqlite_read_diagnostic_engine)

  add_executable(
    modern_sqlite_read_diagnostics
    benchmarks/read_performance.cpp
  )
  target_include_directories(
    modern_sqlite_read_diagnostics
    PRIVATE
      "${_modern_sqlite_benchmark_generated_dir}"
  )
  target_compile_definitions(
    modern_sqlite_read_diagnostics
    PRIVATE
      MODERN_SQLITE_ENABLE_INSTRUMENTATION=1
      MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS=1
  )
  target_link_libraries(
    modern_sqlite_read_diagnostics
    PRIVATE
      modern_sqlite_read_diagnostic_engine
      modern_sqlite_benchmark_sqlite
      modern_sqlite::instrumentation
  )
  modern_sqlite_set_project_warnings(modern_sqlite_read_diagnostics PRIVATE)
  modern_sqlite_enable_clang_tidy(modern_sqlite_read_diagnostics)
endif()

add_executable(
  modern_sqlite_inspect
  tools/modern_sqlite_inspect.cpp
)
target_link_libraries(
  modern_sqlite_inspect
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_inspect PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_inspect)

add_executable(
  modern_sqlite_read_trace
  tools/modern_sqlite_read_trace.cpp
)
target_link_libraries(
  modern_sqlite_read_trace
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_read_trace PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_read_trace)

if(MODERN_SQLITE_ENABLE_INSTRUMENTATION)
  target_compile_definitions(
    modern_sqlite
    PUBLIC
      MODERN_SQLITE_ENABLE_INSTRUMENTATION=1
  )
  target_link_libraries(
    modern_sqlite
    PRIVATE
      modern_sqlite::instrumentation
  )
endif()

if(MODERN_SQLITE_BUILD_TESTS)
  include(CTest)
  if(BUILD_TESTING)
    add_subdirectory(tests)
  endif()
endif()

modern_sqlite_add_format_targets(
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/base/bytes.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/base/coding.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/base/result.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/binder/bound_select.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/bytecode/program.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/catalog/catalog.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/catalog/catalog_loader.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/diagnostics/storage_inspector.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/format/record_codec.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/instrumentation/counters.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/lowering/read_lowering.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/optimizer/physical_plan.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/pager/pager.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/planner/logical_plan.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/platform/posix_vfs.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/platform/vfs.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/runtime/collation.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/runtime/function_registry.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/runtime/sql_value.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/session/read_session.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/btree/cursor.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/btree/page.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/btree/writer.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/cache/page_cache.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/database_format.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/journal/journal.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/journal/rollback_journal.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/page_bitvec.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/page_number.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/syntax/ast.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/syntax/lexer.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/syntax/parser.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/text/text.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/transaction/transaction_coordinator.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/vm/read_vm.hpp"
  "${PROJECT_SOURCE_DIR}/src/base/bytes.cpp"
  "${PROJECT_SOURCE_DIR}/src/base/coding.cpp"
  "${PROJECT_SOURCE_DIR}/src/base/result.cpp"
  "${PROJECT_SOURCE_DIR}/src/binder/bound_select.cpp"
  "${PROJECT_SOURCE_DIR}/src/bytecode/program.cpp"
  "${PROJECT_SOURCE_DIR}/src/catalog/catalog.cpp"
  "${PROJECT_SOURCE_DIR}/src/catalog/catalog_loader.cpp"
  "${PROJECT_SOURCE_DIR}/src/diagnostics/storage_inspector.cpp"
  "${PROJECT_SOURCE_DIR}/src/format/record_codec.cpp"
  "${PROJECT_SOURCE_DIR}/src/instrumentation/counters.cpp"
  "${PROJECT_SOURCE_DIR}/src/lowering/read_lowering.cpp"
  "${PROJECT_SOURCE_DIR}/src/optimizer/physical_plan.cpp"
  "${PROJECT_SOURCE_DIR}/src/pager/pager.cpp"
  "${PROJECT_SOURCE_DIR}/src/pager/write_pager.cpp"
  "${PROJECT_SOURCE_DIR}/src/planner/logical_plan.cpp"
  "${PROJECT_SOURCE_DIR}/src/platform/posix_vfs.cpp"
  "${PROJECT_SOURCE_DIR}/src/platform/vfs.cpp"
  "${PROJECT_SOURCE_DIR}/src/runtime/collation.cpp"
  "${PROJECT_SOURCE_DIR}/src/runtime/function_registry.cpp"
  "${PROJECT_SOURCE_DIR}/src/runtime/sqlite_float.cpp"
  "${PROJECT_SOURCE_DIR}/src/runtime/sqlite_float.hpp"
  "${PROJECT_SOURCE_DIR}/src/runtime/sql_value.cpp"
  "${PROJECT_SOURCE_DIR}/src/session/read_session.cpp"
  "${PROJECT_SOURCE_DIR}/src/session/read_session_internal.hpp"
  "${PROJECT_SOURCE_DIR}/src/storage/btree/cursor.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/btree/page.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/btree/writer.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/btree/writer_internal.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/btree/writer_internal.hpp"
  "${PROJECT_SOURCE_DIR}/src/storage/cache/page_cache.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/database_format.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/journal/journal.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/journal/rollback_journal.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/page_bitvec.cpp"
  "${PROJECT_SOURCE_DIR}/src/syntax/ast.cpp"
  "${PROJECT_SOURCE_DIR}/src/syntax/lexer.cpp"
  "${PROJECT_SOURCE_DIR}/src/syntax/parser.cpp"
  "${PROJECT_SOURCE_DIR}/src/syntax/token_text.hpp"
  "${PROJECT_SOURCE_DIR}/src/text/text.cpp"
  "${PROJECT_SOURCE_DIR}/src/transaction/transaction_coordinator.cpp"
  "${PROJECT_SOURCE_DIR}/src/vm/read_vm.cpp"
  "${PROJECT_SOURCE_DIR}/benchmarks/read_performance.cpp"
  "${PROJECT_SOURCE_DIR}/tests/bootstrap/toolchain_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/platform/posix_vfs_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/base/bytes_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/base/coding_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/base/result_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/binder/bound_select_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/binder/bound_select_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/binder/bound_select_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/bytecode/program_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/bytecode/program_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/catalog/catalog_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/catalog/catalog_loader_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/catalog/catalog_loader_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/catalog/catalog_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/diagnostics/storage_inspector_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/diagnostics/storage_inspector_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/format/record_codec_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/instrumentation/counters_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/instrumentation/disabled_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/lowering/read_lowering_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/lowering/read_lowering_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/lowering/read_lowering_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/optimizer/physical_plan_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/optimizer/physical_plan_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/optimizer/physical_plan_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/pager/pager_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/pager/write_pager_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/pager/write_pager_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/pager/write_pager_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/pager/write_pager_test_support.hpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/planner/logical_plan_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/planner/logical_plan_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/planner/logical_plan_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/platform/vfs_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/runtime/collation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/runtime/function_registry_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/runtime/sql_value_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/session/read_session_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/session/read_session_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/session/read_session_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/btree/cursor_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/btree/page_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/btree/writer_internal_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/btree/writer_internal_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/btree/writer_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/cache/page_cache_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/database_format_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/journal/journal_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/journal/journal_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/journal/journal_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/journal/rollback_journal_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/journal/rollback_journal_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/journal/rollback_journal_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/page_bitvec_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/page_bitvec_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/page_bitvec_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/journal/rollback_journal_test_support.hpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/syntax/ast_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/syntax/lexer_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/syntax/lexer_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/syntax/parser_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/text/text_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/transaction/transaction_coordinator_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/transaction/transaction_coordinator_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/vm/read_vm_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/vm/read_vm_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/vm/read_vm_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/format/record_codec_golden_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/compatibility/rollback_journal_fixture.cpp"
  "${PROJECT_SOURCE_DIR}/tests/compatibility/btree_writer_crash_compatibility.cpp"
  "${PROJECT_SOURCE_DIR}/tests/compatibility/btree_writer_crash_compatibility.hpp"
  "${PROJECT_SOURCE_DIR}/tests/compatibility/btree_writer_sqlite_compatibility.cpp"
  "${PROJECT_SOURCE_DIR}/tests/compatibility/transaction_sqlite_compatibility.cpp"
  "${PROJECT_SOURCE_DIR}/tests/compatibility/write_pager_sqlite_compatibility.cpp"
  "${PROJECT_SOURCE_DIR}/tests/fuzz/database_image_fuzz.cpp"
  "${PROJECT_SOURCE_DIR}/tests/fuzz/database_image_fuzzer_main.cpp"
  "${PROJECT_SOURCE_DIR}/tests/fuzz/read_fuzz.hpp"
  "${PROJECT_SOURCE_DIR}/tests/fuzz/read_fuzz_smoke_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/fuzz/read_sql_fuzz.cpp"
  "${PROJECT_SOURCE_DIR}/tests/fuzz/read_sql_fuzzer_main.cpp"
  "${PROJECT_SOURCE_DIR}/tests/model/read_session_model_test.cpp"
  "${PROJECT_SOURCE_DIR}/tools/modern_sqlite_inspect.cpp"
  "${PROJECT_SOURCE_DIR}/tools/modern_sqlite_read_trace.cpp"
)
