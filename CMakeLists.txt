cmake_minimum_required(VERSION 3.25)

project(
  modern_sqlite
  VERSION 0.1.0
  DESCRIPTION "A ground-up C++23 reimplementation of SQLite"
  LANGUAGES CXX
)

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
option(
  MODERN_SQLITE_ENABLE_INSTRUMENTATION
  "Compile and link optional instrumentation hooks"
  OFF
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

add_library(
  modern_sqlite
  STATIC
    src/base/bytes.cpp
    src/base/coding.cpp
    src/base/result.cpp
    src/binder/bound_select.cpp
    src/bytecode/program.cpp
    src/catalog/catalog.cpp
    src/catalog/catalog_loader.cpp
    src/diagnostics/storage_inspector.cpp
    src/format/record_codec.cpp
    src/optimizer/physical_plan.cpp
    src/pager/read_pager.cpp
    src/planner/logical_plan.cpp
    src/platform/posix_vfs.cpp
    src/platform/vfs.cpp
    src/runtime/collation.cpp
    src/runtime/function_registry.cpp
    src/runtime/sqlite_float.cpp
    src/runtime/sql_value.cpp
    src/storage/btree/cursor.cpp
    src/storage/btree/page.cpp
    src/storage/cache/page_cache.cpp
    src/storage/database_format.cpp
    src/syntax/ast.cpp
    src/syntax/lexer.cpp
    src/syntax/parser.cpp
    src/text/text.cpp
    src/vm/read_vm.cpp
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
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/optimizer/physical_plan.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/pager/read_pager.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/planner/logical_plan.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/platform/posix_vfs.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/platform/vfs.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/runtime/collation.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/runtime/function_registry.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/runtime/sql_value.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/btree/cursor.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/btree/page.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/cache/page_cache.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/database_format.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/storage/page_number.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/syntax/ast.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/syntax/lexer.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/syntax/parser.hpp"
  "${PROJECT_SOURCE_DIR}/include/modern_sqlite/text/text.hpp"
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
  "${PROJECT_SOURCE_DIR}/src/optimizer/physical_plan.cpp"
  "${PROJECT_SOURCE_DIR}/src/pager/read_pager.cpp"
  "${PROJECT_SOURCE_DIR}/src/planner/logical_plan.cpp"
  "${PROJECT_SOURCE_DIR}/src/platform/posix_vfs.cpp"
  "${PROJECT_SOURCE_DIR}/src/platform/vfs.cpp"
  "${PROJECT_SOURCE_DIR}/src/runtime/collation.cpp"
  "${PROJECT_SOURCE_DIR}/src/runtime/function_registry.cpp"
  "${PROJECT_SOURCE_DIR}/src/runtime/sqlite_float.cpp"
  "${PROJECT_SOURCE_DIR}/src/runtime/sqlite_float.hpp"
  "${PROJECT_SOURCE_DIR}/src/runtime/sql_value.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/btree/cursor.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/btree/page.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/cache/page_cache.cpp"
  "${PROJECT_SOURCE_DIR}/src/storage/database_format.cpp"
  "${PROJECT_SOURCE_DIR}/src/syntax/ast.cpp"
  "${PROJECT_SOURCE_DIR}/src/syntax/lexer.cpp"
  "${PROJECT_SOURCE_DIR}/src/syntax/parser.cpp"
  "${PROJECT_SOURCE_DIR}/src/syntax/token_text.hpp"
  "${PROJECT_SOURCE_DIR}/src/text/text.cpp"
  "${PROJECT_SOURCE_DIR}/src/vm/read_vm.cpp"
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
  "${PROJECT_SOURCE_DIR}/tests/unit/optimizer/physical_plan_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/optimizer/physical_plan_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/optimizer/physical_plan_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/pager/read_pager_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/planner/logical_plan_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/planner/logical_plan_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/planner/logical_plan_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/platform/vfs_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/runtime/collation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/runtime/function_registry_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/runtime/sql_value_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/btree/cursor_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/btree/page_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/cache/page_cache_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/storage/database_format_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/syntax/ast_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/syntax/lexer_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/syntax/lexer_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/syntax/parser_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/text/text_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/vm/read_vm_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/vm/read_vm_allocation_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/unit/vm/read_vm_oom_test.cpp"
  "${PROJECT_SOURCE_DIR}/tests/format/record_codec_golden_test.cpp"
  "${PROJECT_SOURCE_DIR}/tools/modern_sqlite_inspect.cpp"
)
