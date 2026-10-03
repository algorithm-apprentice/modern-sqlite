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

include("${CMAKE_CURRENT_LIST_DIR}/cmake/CompilerWarnings.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ClangTidy.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/Format.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/Sanitizers.cmake")

add_library(modern_sqlite_project_options INTERFACE)
target_compile_features(modern_sqlite_project_options INTERFACE cxx_std_23)
modern_sqlite_enable_sanitizers(modern_sqlite_project_options)

add_library(modern_sqlite_project_warnings INTERFACE)
modern_sqlite_set_project_warnings(modern_sqlite_project_warnings INTERFACE)

add_library(modern_sqlite INTERFACE)
add_library(modern_sqlite::modern_sqlite ALIAS modern_sqlite)
target_include_directories(
  modern_sqlite
  INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  modern_sqlite
  INTERFACE
    modern_sqlite_project_options
    modern_sqlite_project_warnings
)

if(MODERN_SQLITE_BUILD_TESTS)
  include(CTest)
  if(BUILD_TESTING)
    add_subdirectory(tests)
  endif()
endif()

modern_sqlite_add_format_targets(
  "${PROJECT_SOURCE_DIR}/tests/bootstrap/toolchain_test.cpp"
)
