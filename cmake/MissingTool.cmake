if(NOT DEFINED TOOL OR TOOL STREQUAL "")
  message(FATAL_ERROR "TOOL must name the missing validation tool")
endif()

message(FATAL_ERROR "${TOOL} is required for this validation target")
