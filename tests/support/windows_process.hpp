#ifndef MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_PROCESS_HPP_
#define MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_PROCESS_HPP_

#include <windows.h>

#include <array>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "windows_handle.hpp"

namespace modern_sqlite::test_support {

[[nodiscard]] inline std::filesystem::path WindowsProcessExecutable() {
  std::array<wchar_t, 32768> path{};
  const DWORD size = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (size == 0 || size == path.size()) {
    throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                            "could not resolve owned Windows test executable"};
  }
  return std::wstring{path.data(), size};
}

[[nodiscard]] inline std::wstring QuoteWindowsArgument(std::wstring_view value) {
  std::wstring quoted{L"\""};
  std::size_t backslashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    quoted.append(character == L'"' ? 2 * backslashes + 1 : backslashes, L'\\');
    quoted.push_back(character);
    backslashes = 0;
  }
  quoted.append(2 * backslashes, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

[[nodiscard]] inline DWORD RunWindowsProcess(const std::filesystem::path& executable,
                                             std::initializer_list<std::wstring_view> arguments) {
  std::wstring command = QuoteWindowsArgument(executable.native());
  for (const auto argument : arguments) {
    command.push_back(L' ');
    command += QuoteWindowsArgument(argument);
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  if (::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information) == 0) {
    throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                            "could not launch owned Windows test child"};
  }
  const auto process = OwnWindowsHandle(information.hProcess);
  const auto thread = OwnWindowsHandle(information.hThread);
  const DWORD waited = ::WaitForSingleObject(process.get(), 5000);
  if (waited != WAIT_OBJECT_0) {
    const DWORD error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : ::GetLastError();
    const BOOL terminated = ::TerminateProcess(process.get(), ERROR_TIMEOUT);
    const DWORD termination_error = ::GetLastError();
    if (terminated == 0 && ::WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) {
      throw std::system_error{static_cast<int>(termination_error), std::system_category(),
                              "could not terminate owned Windows test child"};
    }
    if (::WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) {
      throw std::runtime_error{"owned Windows test child did not exit after termination"};
    }
    throw std::system_error{static_cast<int>(error), std::system_category(),
                            "owned Windows test child did not finish"};
  }
  DWORD exit_code = 0;
  if (::GetExitCodeProcess(process.get(), &exit_code) == 0) {
    throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                            "could not read owned Windows test child exit code"};
  }
  return exit_code;
}

}  // namespace modern_sqlite::test_support

#endif  // MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_PROCESS_HPP_
