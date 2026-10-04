include(FetchContent)

if(NOT DEFINED BUILD_GMOCK)
  set(BUILD_GMOCK OFF)
endif()
if(NOT DEFINED INSTALL_GTEST)
  set(INSTALL_GTEST OFF)
endif()
if(NOT DEFINED gtest_force_shared_crt)
  set(gtest_force_shared_crt ON)
endif()

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(
  modern_sqlite_unit_tests
  bootstrap/toolchain_test.cpp
  unit/base/bytes_test.cpp
  unit/base/coding_test.cpp
  unit/base/result_test.cpp
  unit/binder/bound_select_test.cpp
  unit/bytecode/program_test.cpp
  unit/catalog/catalog_test.cpp
  unit/catalog/catalog_loader_test.cpp
  unit/diagnostics/storage_inspector_test.cpp
  unit/format/record_codec_test.cpp
  unit/pager/read_pager_test.cpp
  unit/planner/logical_plan_test.cpp
  unit/platform/vfs_test.cpp
  unit/runtime/collation_test.cpp
  unit/runtime/function_registry_test.cpp
  unit/runtime/sql_value_test.cpp
  unit/storage/btree/cursor_test.cpp
  unit/storage/btree/page_test.cpp
  unit/storage/cache/page_cache_test.cpp
  unit/storage/database_format_test.cpp
  unit/syntax/ast_test.cpp
  unit/syntax/lexer_test.cpp
  unit/syntax/parser_test.cpp
  unit/text/text_test.cpp
  unit/vm/read_vm_test.cpp
  format/record_codec_golden_test.cpp
)
target_link_libraries(
  modern_sqlite_unit_tests
  PRIVATE
    modern_sqlite::modern_sqlite
    GTest::gtest_main
)
modern_sqlite_set_project_warnings(modern_sqlite_unit_tests PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_unit_tests)

include(GoogleTest)
gtest_discover_tests(
  modern_sqlite_unit_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_bytecode_allocation_test
  unit/bytecode/program_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_bytecode_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_bytecode_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_bytecode_allocation_test)
add_test(
  NAME bytecode.program_allocation
  COMMAND modern_sqlite_bytecode_allocation_test
)
set_tests_properties(
  bytecode.program_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_binder_oom_test
  unit/binder/bound_select_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_binder_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_binder_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_binder_oom_test)
add_test(
  NAME binder.bound_select_oom
  COMMAND modern_sqlite_binder_oom_test
)
set_tests_properties(
  binder.bound_select_oom
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_binder_allocation_test
  unit/binder/bound_select_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_binder_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_binder_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_binder_allocation_test)
add_test(
  NAME binder.bound_select_allocation
  COMMAND modern_sqlite_binder_allocation_test
)
set_tests_properties(
  binder.bound_select_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_logical_plan_oom_test
  unit/planner/logical_plan_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_logical_plan_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_logical_plan_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_logical_plan_oom_test)
add_test(
  NAME planner.logical_plan_oom
  COMMAND modern_sqlite_logical_plan_oom_test
)
set_tests_properties(
  planner.logical_plan_oom
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_logical_plan_allocation_test
  unit/planner/logical_plan_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_logical_plan_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_logical_plan_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_logical_plan_allocation_test)
add_test(
  NAME planner.logical_plan_allocation
  COMMAND modern_sqlite_logical_plan_allocation_test
)
set_tests_properties(
  planner.logical_plan_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_read_vm_oom_test
  unit/vm/read_vm_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_read_vm_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_read_vm_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_read_vm_oom_test)
add_test(
  NAME vm.read_vm_oom
  COMMAND modern_sqlite_read_vm_oom_test
)
set_tests_properties(
  vm.read_vm_oom
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_read_vm_allocation_test
  unit/vm/read_vm_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_read_vm_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_read_vm_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_read_vm_allocation_test)
add_test(
  NAME vm.read_vm_allocation
  COMMAND modern_sqlite_read_vm_allocation_test
)
set_tests_properties(
  vm.read_vm_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_storage_inspector_oom_test
  unit/diagnostics/storage_inspector_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_storage_inspector_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_storage_inspector_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_storage_inspector_oom_test)
add_test(
  NAME diagnostics.storage_inspector_oom
  COMMAND modern_sqlite_storage_inspector_oom_test
)
set_tests_properties(
  diagnostics.storage_inspector_oom
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_lexer_allocation_test
  unit/syntax/lexer_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_lexer_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_lexer_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_lexer_allocation_test)
add_test(
  NAME syntax.lexer_allocation
  COMMAND modern_sqlite_lexer_allocation_test
)
set_tests_properties(
  syntax.lexer_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_catalog_allocation_test
  unit/catalog/catalog_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_catalog_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_catalog_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_catalog_allocation_test)
add_test(
  NAME catalog.lookup_allocation
  COMMAND modern_sqlite_catalog_allocation_test
)
set_tests_properties(
  catalog.lookup_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_catalog_loader_oom_test
  unit/catalog/catalog_loader_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_catalog_loader_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_catalog_loader_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_catalog_loader_oom_test)
add_test(
  NAME catalog.loader_oom
  COMMAND modern_sqlite_catalog_loader_oom_test
)
set_tests_properties(
  catalog.loader_oom
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_posix_tests
  platform/posix_vfs_test.cpp
)
target_link_libraries(
  modern_sqlite_posix_tests
  PRIVATE
    modern_sqlite::modern_sqlite
    GTest::gtest_main
)
modern_sqlite_set_project_warnings(modern_sqlite_posix_tests PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_posix_tests)
gtest_discover_tests(
  modern_sqlite_posix_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES
    LABELS platform
    TIMEOUT 10
)

add_executable(
  modern_sqlite_instrumentation_tests
  unit/instrumentation/counters_test.cpp
)
target_link_libraries(
  modern_sqlite_instrumentation_tests
  PRIVATE
    modern_sqlite::instrumentation
    GTest::gtest_main
)
modern_sqlite_set_project_warnings(modern_sqlite_instrumentation_tests PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_instrumentation_tests)
gtest_discover_tests(
  modern_sqlite_instrumentation_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_instrumentation_disabled_tests
  unit/instrumentation/disabled_test.cpp
)
target_include_directories(
  modern_sqlite_instrumentation_disabled_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(
  modern_sqlite_instrumentation_disabled_tests
  PRIVATE
    modern_sqlite_project_options
    GTest::gtest_main
)
modern_sqlite_set_project_warnings(modern_sqlite_instrumentation_disabled_tests PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_instrumentation_disabled_tests)
gtest_discover_tests(
  modern_sqlite_instrumentation_disabled_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(
  NAME project.graph
  COMMAND
    "${Python3_EXECUTABLE}"
    -m unittest discover
    -s tests/project
    -p "test_*.py"
)
set_tests_properties(
  project.graph
  PROPERTIES
    LABELS project
    TIMEOUT 10
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)

add_test(
  NAME cmake.no_tests_is_error
  COMMAND
    "${CMAKE_CTEST_COMMAND}"
    --test-dir "${PROJECT_BINARY_DIR}"
    --label-regex modern_sqlite_intentionally_missing_label
    --no-tests=error
)
set_tests_properties(
  cmake.no_tests_is_error
  PROPERTIES
    LABELS cmake
    TIMEOUT 10
    WILL_FAIL TRUE
)

add_test(
  NAME cli.storage_inspector
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/cli/storage_inspector_cli_test.py"
    --binary "$<TARGET_FILE:modern_sqlite_inspect>"
    --fixture
      "${PROJECT_SOURCE_DIR}/tests/fixtures/storage_diagnostics/sqlite-3.54.0-storage.db"
    --golden
      "${PROJECT_SOURCE_DIR}/tests/golden/storage_inspector.json"
)
set_tests_properties(
  cli.storage_inspector
  PROPERTIES
    LABELS cli
    TIMEOUT 10
)
