#ifndef MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_PROCESS_HPP_
#define MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_PROCESS_HPP_

#include <windows.h>

#include <array>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "modern_sqlite/base/bytes.hpp"
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
  const auto duplicate_standard_handle = [](DWORD kind) {
    HANDLE duplicated = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), ::GetStdHandle(kind), ::GetCurrentProcess(),
                          &duplicated, 0, TRUE, DUPLICATE_SAME_ACCESS) == 0) {
      throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                              "could not duplicate owned Windows test standard handle"};
    }
    return OwnWindowsHandle(duplicated);
  };
  const auto output = duplicate_standard_handle(STD_OUTPUT_HANDLE);
  const auto error_output = duplicate_standard_handle(STD_ERROR_HANDLE);
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  const auto input =
      OwnWindowsHandle(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!input) {
    throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                            "could not open owned Windows test child input"};
  }
  SIZE_T attribute_size = 0;
  if (::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size) != 0 ||
      ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    throw std::runtime_error{"could not size the owned Windows test handle allowlist"};
  }
  ByteBuffer storage{ByteCount{attribute_size}};
  auto* attribute_list =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.mutable_view().data());
  if (::InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size) == 0) {
    throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                            "could not initialize the owned Windows test handle allowlist"};
  }
  const auto destroy_attributes = [](LPPROC_THREAD_ATTRIBUTE_LIST list) {
    ::DeleteProcThreadAttributeList(list);
  };
  const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>,
                        decltype(destroy_attributes)>
      attributes{attribute_list, destroy_attributes};
  std::array handles{input.get(), output.get(), error_output.get()};
  if (::UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  handles.data(), sizeof(handles), nullptr, nullptr) == 0) {
    throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                            "could not restrict owned Windows test handle inheritance"};
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.get();
  startup.StartupInfo.hStdOutput = output.get();
  startup.StartupInfo.hStdError = error_output.get();
  startup.lpAttributeList = attributes.get();
  PROCESS_INFORMATION information{};
  if (::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                       &startup.StartupInfo, &information) == 0) {
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
