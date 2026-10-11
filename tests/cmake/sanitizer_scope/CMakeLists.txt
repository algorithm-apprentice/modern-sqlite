cmake_minimum_required(VERSION 3.25)

project(sanitizer_scope LANGUAGES CXX)

if(NOT DEFINED MODERN_SQLITE_REPOSITORY_ROOT)
  message(FATAL_ERROR "MODERN_SQLITE_REPOSITORY_ROOT is required")
endif()

set(MODERN_SQLITE_ENABLE_ASAN ON)
set(MODERN_SQLITE_ENABLE_UBSAN ON)
set(MODERN_SQLITE_ENABLE_TSAN OFF)
include("${MODERN_SQLITE_REPOSITORY_ROOT}/cmake/Sanitizers.cmake")

add_library(production_target STATIC src/production.cpp)
modern_sqlite_enable_sanitizers(production_target)

get_target_property(production_compile_options production_target COMPILE_OPTIONS)
if(NOT "${production_compile_options}" MATCHES "-fsanitize=address,undefined")
  message(FATAL_ERROR "production targets must receive sanitizer compile options")
endif()

get_target_property(
  production_interface_compile_options
  production_target
  INTERFACE_COMPILE_OPTIONS
)
if(production_interface_compile_options)
  message(FATAL_ERROR "sanitizer compile options must not propagate to consumers")
endif()

get_target_property(
  production_interface_link_options
  production_target
  INTERFACE_LINK_OPTIONS
)
if(NOT "${production_interface_link_options}" MATCHES "-fsanitize=address,undefined")
  message(FATAL_ERROR "sanitizer runtime link options must propagate to consumers")
endif()

add_executable(test_target tests/test.cpp)
target_link_libraries(test_target PRIVATE production_target)
