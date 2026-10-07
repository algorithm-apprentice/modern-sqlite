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
  unit/lowering/read_lowering_test.cpp
  unit/optimizer/physical_plan_test.cpp
  unit/pager/pager_test.cpp
  unit/pager/write_pager_test.cpp
  unit/planner/logical_plan_test.cpp
  unit/platform/vfs_test.cpp
  unit/runtime/collation_test.cpp
  unit/runtime/function_registry_test.cpp
  unit/runtime/sql_value_test.cpp
  unit/session/read_session_test.cpp
  unit/storage/btree/cursor_test.cpp
  unit/storage/btree/page_test.cpp
  unit/storage/btree/writer_internal_test.cpp
  unit/storage/btree/writer_test.cpp
  unit/storage/cache/page_cache_test.cpp
  unit/storage/database_format_test.cpp
  unit/storage/journal/journal_test.cpp
  unit/storage/journal/rollback_journal_test.cpp
  unit/storage/page_bitvec_test.cpp
  unit/syntax/ast_test.cpp
  unit/syntax/lexer_test.cpp
  unit/syntax/parser_test.cpp
  unit/text/text_test.cpp
  unit/transaction/transaction_coordinator_test.cpp
  unit/vm/vm_test.cpp
  format/record_codec_golden_test.cpp
)
target_include_directories(
  modern_sqlite_unit_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src"
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

foreach(kind allocation oom)
  add_executable(
    modern_sqlite_page_bitvec_${kind}_test
    unit/storage/page_bitvec_${kind}_test.cpp
  )
  target_link_libraries(
    modern_sqlite_page_bitvec_${kind}_test
    PRIVATE modern_sqlite::modern_sqlite
  )
  modern_sqlite_set_project_warnings(modern_sqlite_page_bitvec_${kind}_test PRIVATE)
  modern_sqlite_enable_clang_tidy(modern_sqlite_page_bitvec_${kind}_test)
  add_test(
    NAME storage.page_bitvec_${kind}
    COMMAND modern_sqlite_page_bitvec_${kind}_test
  )
  set_tests_properties(storage.page_bitvec_${kind} PROPERTIES LABELS unit TIMEOUT 10)
endforeach()

add_executable(
  modern_sqlite_btree_writer_internal_oom_test
  unit/storage/btree/writer_internal_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_btree_writer_internal_oom_test
  PRIVATE modern_sqlite::modern_sqlite
)
target_include_directories(
  modern_sqlite_btree_writer_internal_oom_test
  PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
)
modern_sqlite_set_project_warnings(modern_sqlite_btree_writer_internal_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_btree_writer_internal_oom_test)
add_test(
  NAME btree.writer_internal_oom
  COMMAND modern_sqlite_btree_writer_internal_oom_test
)
set_tests_properties(btree.writer_internal_oom PROPERTIES LABELS unit TIMEOUT 20)

add_executable(
  modern_sqlite_transaction_coordinator_oom_test
  unit/transaction/transaction_coordinator_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_transaction_coordinator_oom_test
  PRIVATE modern_sqlite::modern_sqlite
)
target_include_directories(
  modern_sqlite_transaction_coordinator_oom_test
  PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
)
modern_sqlite_set_project_warnings(modern_sqlite_transaction_coordinator_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_transaction_coordinator_oom_test)
add_test(
  NAME transaction.coordinator_oom
  COMMAND modern_sqlite_transaction_coordinator_oom_test
)
set_tests_properties(transaction.coordinator_oom PROPERTIES LABELS unit TIMEOUT 20)

add_executable(
  modern_sqlite_read_session_model_test
  model/read_session_model_test.cpp
)
target_link_libraries(
  modern_sqlite_read_session_model_test
  PRIVATE
    modern_sqlite::modern_sqlite
    GTest::gtest_main
)
modern_sqlite_set_project_warnings(modern_sqlite_read_session_model_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_read_session_model_test)
gtest_discover_tests(
  modern_sqlite_read_session_model_test
  DISCOVERY_MODE PRE_TEST
  PROPERTIES
    LABELS model
    TIMEOUT 10
)

add_executable(
  modern_sqlite_read_fuzz_smoke_test
  fuzz/database_image_fuzz.cpp
  fuzz/read_fuzz_smoke_test.cpp
  fuzz/read_sql_fuzz.cpp
)
target_link_libraries(
  modern_sqlite_read_fuzz_smoke_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_read_fuzz_smoke_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_read_fuzz_smoke_test)
add_test(
  NAME fuzz.read_smoke
  COMMAND
    modern_sqlite_read_fuzz_smoke_test
    "${PROJECT_SOURCE_DIR}/tests/fixtures/read_compatibility/sqlite-3.54.0-read-compatibility.db"
    "${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/read_sql"
    "${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/read_database"
)
set_tests_properties(
  fuzz.read_smoke
  PROPERTIES
    LABELS fuzz
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
  modern_sqlite_journal_allocation_test
  unit/storage/journal/journal_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_journal_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_journal_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_journal_allocation_test)
add_test(
  NAME journal.allocation
  COMMAND modern_sqlite_journal_allocation_test
)
set_tests_properties(
  journal.allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_journal_oom_test
  unit/storage/journal/journal_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_journal_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_journal_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_journal_oom_test)
add_test(
  NAME journal.oom
  COMMAND modern_sqlite_journal_oom_test
)
set_tests_properties(
  journal.oom
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_rollback_journal_allocation_test
  unit/storage/journal/rollback_journal_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_rollback_journal_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_rollback_journal_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_rollback_journal_allocation_test)
add_test(
  NAME journal.rollback_allocation
  COMMAND modern_sqlite_rollback_journal_allocation_test
)
set_tests_properties(
  journal.rollback_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_write_pager_allocation_test
  unit/pager/write_pager_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_write_pager_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(
  modern_sqlite_write_pager_allocation_test
  PRIVATE
)
modern_sqlite_enable_clang_tidy(modern_sqlite_write_pager_allocation_test)
add_test(
  NAME pager.write_allocation
  COMMAND modern_sqlite_write_pager_allocation_test
)
set_tests_properties(
  pager.write_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_write_pager_oom_test
  unit/pager/write_pager_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_write_pager_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(
  modern_sqlite_write_pager_oom_test
  PRIVATE
)
modern_sqlite_enable_clang_tidy(modern_sqlite_write_pager_oom_test)
add_test(
  NAME pager.write_oom
  COMMAND modern_sqlite_write_pager_oom_test
)
set_tests_properties(
  pager.write_oom
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_rollback_journal_oom_test
  unit/storage/journal/rollback_journal_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_rollback_journal_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_rollback_journal_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_rollback_journal_oom_test)
add_test(
  NAME journal.rollback_oom
  COMMAND modern_sqlite_rollback_journal_oom_test
)
set_tests_properties(
  journal.rollback_oom
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
  modern_sqlite_optimizer_oom_test
  unit/optimizer/physical_plan_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_optimizer_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_optimizer_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_optimizer_oom_test)
add_test(
  NAME optimizer.physical_plan_oom
  COMMAND modern_sqlite_optimizer_oom_test
)
set_tests_properties(
  optimizer.physical_plan_oom
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_optimizer_allocation_test
  unit/optimizer/physical_plan_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_optimizer_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_optimizer_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_optimizer_allocation_test)
add_test(
  NAME optimizer.physical_plan_allocation
  COMMAND modern_sqlite_optimizer_allocation_test
)
set_tests_properties(
  optimizer.physical_plan_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_read_lowering_oom_test
  unit/lowering/read_lowering_oom_test.cpp
)
target_link_libraries(
  modern_sqlite_read_lowering_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_read_lowering_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_read_lowering_oom_test)
add_test(
  NAME lowering.read_lowering_oom
  COMMAND modern_sqlite_read_lowering_oom_test
)
set_tests_properties(
  lowering.read_lowering_oom
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_read_lowering_allocation_test
  unit/lowering/read_lowering_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_read_lowering_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_read_lowering_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_read_lowering_allocation_test)
add_test(
  NAME lowering.read_lowering_allocation
  COMMAND modern_sqlite_read_lowering_allocation_test
)
set_tests_properties(
  lowering.read_lowering_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_vm_oom_test
  unit/vm/vm_oom_test.cpp
)
target_include_directories(
  modern_sqlite_vm_oom_test
  PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
  modern_sqlite_vm_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_vm_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_vm_oom_test)
add_test(
  NAME vm.oom
  COMMAND modern_sqlite_vm_oom_test
)
set_tests_properties(
  vm.oom
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_vm_allocation_test
  unit/vm/vm_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_vm_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_vm_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_vm_allocation_test)
add_test(
  NAME vm.allocation
  COMMAND modern_sqlite_vm_allocation_test
)
set_tests_properties(
  vm.allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

add_executable(
  modern_sqlite_read_session_oom_test
  unit/session/read_session_oom_test.cpp
)

add_executable(
  modern_sqlite_read_session_allocation_test
  unit/session/read_session_allocation_test.cpp
)
target_link_libraries(
  modern_sqlite_read_session_allocation_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_read_session_allocation_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_read_session_allocation_test)
add_test(
  NAME session.read_session_allocation
  COMMAND modern_sqlite_read_session_allocation_test
)
set_tests_properties(
  session.read_session_allocation
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)
target_link_libraries(
  modern_sqlite_read_session_oom_test
  PRIVATE
    modern_sqlite::modern_sqlite
)
modern_sqlite_set_project_warnings(modern_sqlite_read_session_oom_test PRIVATE)
modern_sqlite_enable_clang_tidy(modern_sqlite_read_session_oom_test)
add_test(
  NAME session.read_session_oom
  COMMAND modern_sqlite_read_session_oom_test
)
set_tests_properties(
  session.read_session_oom
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
  NAME performance.read_performance_tool
  COMMAND
    "${Python3_EXECUTABLE}"
    -m unittest discover
    -s tests/performance
    -p "read_performance_tool_test.py"
)
set_tests_properties(
  performance.read_performance_tool
  PROPERTIES
    LABELS performance
    TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
if(EXISTS "${PROJECT_SOURCE_DIR}/benchmarks/read-baseline-v1/aggregate.json")
  add_test(
    NAME performance.read_baseline_validation
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tools/read_performance.py"
      validate-baseline
      --repository-root "${PROJECT_SOURCE_DIR}"
      --workloads "${PROJECT_SOURCE_DIR}/tests/performance/read-workloads-v1.json"
      --baseline "${PROJECT_SOURCE_DIR}/benchmarks/read-baseline-v1"
  )
  set_tests_properties(
    performance.read_baseline_validation
    PROPERTIES
      LABELS performance
      TIMEOUT 30
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  )
endif()
if(TARGET modern_sqlite_benchmark_sqlite)
  add_executable(
    modern_sqlite_write_pager_sqlite_compatibility
    compatibility/write_pager_sqlite_compatibility.cpp
  )
  target_link_libraries(
    modern_sqlite_write_pager_sqlite_compatibility
    PRIVATE
      modern_sqlite::modern_sqlite
      modern_sqlite_benchmark_sqlite
  )
  modern_sqlite_set_project_warnings(
    modern_sqlite_write_pager_sqlite_compatibility
    PRIVATE
  )
  modern_sqlite_enable_clang_tidy(
    modern_sqlite_write_pager_sqlite_compatibility
  )
  add_test(
    NAME compatibility.write_pager_sqlite
    COMMAND modern_sqlite_write_pager_sqlite_compatibility
  )
  set_tests_properties(
    compatibility.write_pager_sqlite
    PROPERTIES
      LABELS compatibility
      TIMEOUT 30
  )

  add_executable(
    modern_sqlite_btree_writer_sqlite_compatibility
    compatibility/btree_writer_crash_compatibility.cpp
    compatibility/btree_writer_sqlite_compatibility.cpp
  )
  target_include_directories(
    modern_sqlite_btree_writer_sqlite_compatibility
    PRIVATE
      "${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    modern_sqlite_btree_writer_sqlite_compatibility
    PRIVATE
      modern_sqlite::modern_sqlite
      modern_sqlite_benchmark_sqlite
  )
  modern_sqlite_set_project_warnings(
    modern_sqlite_btree_writer_sqlite_compatibility
    PRIVATE
  )
  modern_sqlite_enable_clang_tidy(
    modern_sqlite_btree_writer_sqlite_compatibility
  )
  add_test(
    NAME compatibility.btree_writer_sqlite
    COMMAND modern_sqlite_btree_writer_sqlite_compatibility
  )
  set_tests_properties(
    compatibility.btree_writer_sqlite
    PROPERTIES
      LABELS compatibility
      TIMEOUT 30
  )

  add_executable(
    modern_sqlite_transaction_sqlite_compatibility
    compatibility/transaction_sqlite_compatibility.cpp
  )
  target_include_directories(
    modern_sqlite_transaction_sqlite_compatibility
    PRIVATE
      "${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    modern_sqlite_transaction_sqlite_compatibility
    PRIVATE
      modern_sqlite::modern_sqlite
      modern_sqlite_benchmark_sqlite
  )
  modern_sqlite_set_project_warnings(
    modern_sqlite_transaction_sqlite_compatibility
    PRIVATE
  )
  modern_sqlite_enable_clang_tidy(
    modern_sqlite_transaction_sqlite_compatibility
  )
  add_test(
    NAME compatibility.transaction_sqlite
    COMMAND modern_sqlite_transaction_sqlite_compatibility
  )
  set_tests_properties(
    compatibility.transaction_sqlite
    PROPERTIES
      LABELS compatibility
      TIMEOUT 30
  )

  add_executable(
    modern_sqlite_rollback_journal_fixture
    compatibility/rollback_journal_fixture.cpp
  )
  target_link_libraries(
    modern_sqlite_rollback_journal_fixture
    PRIVATE
      modern_sqlite_benchmark_sqlite
      modern_sqlite_project_options
  )
  modern_sqlite_set_project_warnings(
    modern_sqlite_rollback_journal_fixture
    PRIVATE
  )
  modern_sqlite_enable_clang_tidy(modern_sqlite_rollback_journal_fixture)
  add_test(
    NAME compatibility.rollback_journal_fixture_regeneration
    COMMAND
      modern_sqlite_rollback_journal_fixture
      --verify
      "${PROJECT_SOURCE_DIR}/tests/fixtures/rollback_journal"
  )
  set_tests_properties(
    compatibility.rollback_journal_fixture_regeneration
    PROPERTIES
      LABELS compatibility
      TIMEOUT 30
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  )
endif()
if(TARGET modern_sqlite_read_benchmark AND TARGET modern_sqlite_read_diagnostics)
  add_test(
    NAME performance.read_benchmark_cli
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/performance/read_benchmark_cli_test.py"
      --timing-binary "$<TARGET_FILE:modern_sqlite_read_benchmark>"
      --diagnostic-binary "$<TARGET_FILE:modern_sqlite_read_diagnostics>"
      --repository-root "${PROJECT_SOURCE_DIR}"
      --workloads "${PROJECT_SOURCE_DIR}/tests/performance/read-workloads-v1.json"
  )
  set_tests_properties(
    performance.read_benchmark_cli
    PROPERTIES
      LABELS performance
      TIMEOUT 300
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME performance.read_fixture_regeneration
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/performance/read_fixture_regeneration_test.py"
      --repository-root "${PROJECT_SOURCE_DIR}"
      --workloads "${PROJECT_SOURCE_DIR}/tests/performance/read-workloads-v1.json"
      --profile
      "${PROJECT_SOURCE_DIR}/tests/compatibility/sqlite-oracle-profile-v1.json"
      --sqlite-library "$<TARGET_FILE:modern_sqlite_benchmark_sqlite_shared>"
      --sqlite-c "${MODERN_SQLITE_PINNED_SQLITE_C}"
      --sqlite-h "${MODERN_SQLITE_PINNED_SQLITE_H}"
  )
  set_tests_properties(
    performance.read_fixture_regeneration
    PROPERTIES
      LABELS performance
      TIMEOUT 300
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  )
endif()

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

add_test(
  NAME cli.read_trace
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/cli/read_trace_cli_test.py"
    --binary "$<TARGET_FILE:modern_sqlite_read_trace>"
    --fixture
      "${PROJECT_SOURCE_DIR}/tests/fixtures/session/sqlite-3.54.0-session-v1.db"
)
set_tests_properties(
  cli.read_trace
  PROPERTIES
    LABELS cli
    TIMEOUT 10
)

add_test(
  NAME differential.read_compatibility_tool
  COMMAND
    "${Python3_EXECUTABLE}"
    -m unittest
    tests/differential/read_compatibility_tool_test.py
)
set_tests_properties(
  differential.read_compatibility_tool
  PROPERTIES
    LABELS differential
    TIMEOUT 10
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)

set(_modern_sqlite_oracle_inputs
  "${MODERN_SQLITE_PINNED_SQLITE_LIBRARY}"
  "${MODERN_SQLITE_PINNED_SQLITE_C}"
  "${MODERN_SQLITE_PINNED_SQLITE_H}"
)
set(_modern_sqlite_oracle_input_count 0)
foreach(_oracle_input IN LISTS _modern_sqlite_oracle_inputs)
  if(NOT _oracle_input STREQUAL "")
    math(EXPR _modern_sqlite_oracle_input_count
      "${_modern_sqlite_oracle_input_count} + 1")
  endif()
endforeach()
if(NOT _modern_sqlite_oracle_input_count EQUAL 0
   AND NOT _modern_sqlite_oracle_input_count EQUAL 3
   AND NOT (
     MODERN_SQLITE_BUILD_BENCHMARKS
     AND MODERN_SQLITE_PINNED_SQLITE_LIBRARY STREQUAL ""
     AND NOT MODERN_SQLITE_PINNED_SQLITE_C STREQUAL ""
     AND NOT MODERN_SQLITE_PINNED_SQLITE_H STREQUAL ""
   ))
  message(FATAL_ERROR
    "Pinned SQLite oracle tests require the library, sqlite3.c, and sqlite3.h together")
endif()
if(_modern_sqlite_oracle_input_count EQUAL 3)
  add_test(
    NAME differential.sqlite_oracle
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/differential/sqlite_oracle_test.py"
      --sqlite-library "${MODERN_SQLITE_PINNED_SQLITE_LIBRARY}"
      --sqlite-c "${MODERN_SQLITE_PINNED_SQLITE_C}"
      --sqlite-h "${MODERN_SQLITE_PINNED_SQLITE_H}"
  )
  set_tests_properties(
    differential.sqlite_oracle
    PROPERTIES
      LABELS differential
      TIMEOUT 10
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME differential.sqlite_oracle_regeneration
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/differential/sqlite_oracle_regeneration_test.py"
      --tool "${PROJECT_SOURCE_DIR}/tools/read_compatibility.py"
      --repository-root "${PROJECT_SOURCE_DIR}"
      --corpus
        "${PROJECT_SOURCE_DIR}/tests/compatibility/read-corpus-v1.json"
      --oracle
        "${PROJECT_SOURCE_DIR}/tests/compatibility/read-oracle-v1.json"
      --sqlite-library "${MODERN_SQLITE_PINNED_SQLITE_LIBRARY}"
      --sqlite-c "${MODERN_SQLITE_PINNED_SQLITE_C}"
      --sqlite-h "${MODERN_SQLITE_PINNED_SQLITE_H}"
  )
  set_tests_properties(
    differential.sqlite_oracle_regeneration
    PROPERTIES
      LABELS differential
      TIMEOUT 30
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  )
endif()

add_test(
  NAME differential.read_compatibility
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/differential/read_compatibility_test.py"
    --tool "${PROJECT_SOURCE_DIR}/tools/read_compatibility.py"
    --runner "$<TARGET_FILE:modern_sqlite_read_trace>"
    --repository-root "${PROJECT_SOURCE_DIR}"
    --corpus
      "${PROJECT_SOURCE_DIR}/tests/compatibility/read-corpus-v1.json"
    --oracle
      "${PROJECT_SOURCE_DIR}/tests/compatibility/read-oracle-v1.json"
)
set_tests_properties(
  differential.read_compatibility
  PROPERTIES
    LABELS differential
    TIMEOUT 60
)
