function(modern_sqlite_enable_sanitizers target)
  if(MODERN_SQLITE_ENABLE_TSAN AND
     (MODERN_SQLITE_ENABLE_ASAN OR MODERN_SQLITE_ENABLE_UBSAN))
    message(FATAL_ERROR "ThreadSanitizer must be configured separately from ASan and UBSan")
  endif()

  if(MSVC AND
     (MODERN_SQLITE_ENABLE_ASAN OR
      MODERN_SQLITE_ENABLE_UBSAN OR
      MODERN_SQLITE_ENABLE_TSAN))
    message(FATAL_ERROR "The current sanitizer presets require Clang or GCC")
  endif()

  set(sanitizers)
  if(MODERN_SQLITE_ENABLE_ASAN)
    list(APPEND sanitizers address)
  endif()
  if(MODERN_SQLITE_ENABLE_UBSAN)
    list(APPEND sanitizers undefined)
  endif()
  if(MODERN_SQLITE_ENABLE_TSAN)
    list(APPEND sanitizers thread)
  endif()

  if(sanitizers)
    list(JOIN sanitizers "," sanitizer_list)
    target_compile_options(
      "${target}"
      INTERFACE
        "-fsanitize=${sanitizer_list}"
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options("${target}" INTERFACE "-fsanitize=${sanitizer_list}")
  endif()
endfunction()
