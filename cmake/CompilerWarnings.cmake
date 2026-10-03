function(modern_sqlite_set_project_warnings target scope)
  if(MSVC)
    set(warnings /W4 /permissive-)
    if(MODERN_SQLITE_WARNINGS_AS_ERRORS)
      list(APPEND warnings /WX)
    endif()
  else()
    set(
      warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wcast-align
      -Wconversion
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough
      -Wnon-virtual-dtor
      -Wnull-dereference
      -Wold-style-cast
      -Woverloaded-virtual
      -Wshadow
      -Wsign-conversion
    )
    if(MODERN_SQLITE_WARNINGS_AS_ERRORS)
      list(APPEND warnings -Werror)
    endif()
  endif()
  target_compile_options("${target}" "${scope}" ${warnings})
endfunction()
endfunction()
