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
  unit/format/record_codec_test.cpp
  unit/runtime/collation_test.cpp
  unit/runtime/function_registry_test.cpp
  unit/runtime/sql_value_test.cpp
  unit/text/text_test.cpp
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
