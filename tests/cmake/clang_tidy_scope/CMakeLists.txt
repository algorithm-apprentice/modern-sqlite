cmake_minimum_required(VERSION 3.25)

project(clang_tidy_scope LANGUAGES CXX)

if(NOT DEFINED MODERN_SQLITE_REPOSITORY_ROOT)
  message(FATAL_ERROR "MODERN_SQLITE_REPOSITORY_ROOT is required")
endif()

set(MODERN_SQLITE_ENABLE_CLANG_TIDY ON)
set(MODERN_SQLITE_CLANG_TIDY_EXECUTABLE "${CMAKE_COMMAND}")
include("${MODERN_SQLITE_REPOSITORY_ROOT}/cmake/ClangTidy.cmake")

add_library(production_target STATIC src/production.cpp)
modern_sqlite_enable_clang_tidy(production_target)
get_target_property(production_clang_tidy production_target CXX_CLANG_TIDY)
if(NOT production_clang_tidy)
  message(FATAL_ERROR "src-only targets must enable clang-tidy")
endif()

add_executable(test_target tests/test.cpp)
modern_sqlite_enable_clang_tidy(test_target)
get_target_property(test_clang_tidy test_target CXX_CLANG_TIDY)
if(test_clang_tidy)
  message(FATAL_ERROR "test targets must not enable clang-tidy")
endif()

add_executable(mixed_target src/production.cpp tests/test.cpp)
modern_sqlite_enable_clang_tidy(mixed_target)
get_target_property(mixed_clang_tidy mixed_target CXX_CLANG_TIDY)
if(mixed_clang_tidy)
  message(FATAL_ERROR "mixed-source targets must not enable clang-tidy")
endif()
