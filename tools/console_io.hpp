#ifndef MODERN_SQLITE_TOOLS_CONSOLE_IO_HPP_
#define MODERN_SQLITE_TOOLS_CONSOLE_IO_HPP_

#include <cstdio>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace modern_sqlite::tools {

[[nodiscard]] inline bool ConfigureProtocolStreams(bool read_input) noexcept {
#if defined(_WIN32)
  return (::_setmode(::_fileno(stdout), _O_BINARY) != -1) &&
         (::_setmode(::_fileno(stderr), _O_BINARY) != -1) &&
         (!read_input || ::_setmode(::_fileno(stdin), _O_BINARY) != -1);
#else
  static_cast<void>(read_input);
  return true;
#endif
}

}  // namespace modern_sqlite::tools

#endif  // MODERN_SQLITE_TOOLS_CONSOLE_IO_HPP_
