#ifndef MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_PROCESS_HPP_
#define MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_PROCESS_HPP_

#include <windows.h>

#include <array>
#include <exception>
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

class OwnedWindowsProcess final {
 public:
  OwnedWindowsProcess(const std::filesystem::path& executable,
                      std::initializer_list<std::wstring_view> arguments,
                      HANDLE diagnostics = nullptr) {
    std::wstring command = QuoteWindowsArgument(executable.native());
    for (const auto argument : arguments) {
      command.push_back(L' ');
      command += QuoteWindowsArgument(argument);
    }
    const auto duplicate_standard_handle = [diagnostics](DWORD kind) {
      HANDLE duplicated = nullptr;
      const HANDLE original = diagnostics == nullptr ? ::GetStdHandle(kind) : diagnostics;
      if (::DuplicateHandle(::GetCurrentProcess(), original, ::GetCurrentProcess(), &duplicated, 0,
                            TRUE, DUPLICATE_SAME_ACCESS) == 0) {
        throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                                "could not duplicate owned Windows test standard handle"};
      }
      return OwnWindowsHandle(duplicated);
    };
    const auto output = duplicate_standard_handle(STD_OUTPUT_HANDLE);
    const auto error_output = duplicate_standard_handle(STD_ERROR_HANDLE);
    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    const auto input =
        OwnWindowsHandle(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input) {
      throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                              "could not open owned Windows test child input"};
    }
    job_ = OwnWindowsHandle(::CreateJobObjectW(nullptr, nullptr));
    if (!job_) {
      throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                              "could not create owned Windows test job"};
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (::SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits,
                                  sizeof(limits)) == 0) {
      throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                              "could not configure owned Windows test job"};
    }
    SIZE_T attribute_size = 0;
    if (::InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_size) != 0 ||
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw std::runtime_error{"could not size the owned Windows test handle allowlist"};
    }
    ByteBuffer storage{ByteCount{attribute_size}};
    auto* attribute_list =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.mutable_view().data());
    if (::InitializeProcThreadAttributeList(attribute_list, 2, 0, &attribute_size) == 0) {
      throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                              "could not initialize the owned Windows test handle allowlist"};
    }
    const auto destroy_attributes = [](LPPROC_THREAD_ATTRIBUTE_LIST list) {
      ::DeleteProcThreadAttributeList(list);
    };
    std::array handles{input.get(), output.get(), error_output.get()};
    std::array jobs{job_.get()};
    const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>,
                          decltype(destroy_attributes)>
        attributes{attribute_list, destroy_attributes};
    if (::UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    static_cast<void*>(handles.data()), sizeof(handles), nullptr,
                                    nullptr) == 0) {
      throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                              "could not restrict owned Windows test handle inheritance"};
    }
    if (::UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                    static_cast<void*>(jobs.data()), sizeof(jobs), nullptr,
                                    nullptr) == 0) {
      throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                              "could not assign the owned Windows test creation job"};
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
    process_ = OwnWindowsHandle(information.hProcess);
    const auto thread = OwnWindowsHandle(information.hThread);
    id_ = information.dwProcessId;
  }

  OwnedWindowsProcess(const OwnedWindowsProcess&) = delete;
  OwnedWindowsProcess& operator=(const OwnedWindowsProcess&) = delete;

  ~OwnedWindowsProcess() noexcept {
    job_.reset();
    if (process_ && ::WaitForSingleObject(process_.get(), 5000) != WAIT_OBJECT_0) {
      std::terminate();
    }
  }

  [[nodiscard]] DWORD Id() const noexcept { return id_; }

  [[nodiscard]] DWORD WaitForExit(DWORD milliseconds = 5000) {
    const DWORD waited = ::WaitForSingleObject(process_.get(), milliseconds);
    if (waited != WAIT_OBJECT_0) {
      const DWORD error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : ::GetLastError();
      const BOOL terminated = ::TerminateJobObject(job_.get(), ERROR_TIMEOUT);
      const DWORD termination_error = ::GetLastError();
      if (terminated == 0 && ::WaitForSingleObject(process_.get(), 0) != WAIT_OBJECT_0) {
        throw std::system_error{static_cast<int>(termination_error), std::system_category(),
                                "could not terminate owned Windows test child"};
      }
      if (::WaitForSingleObject(process_.get(), 5000) != WAIT_OBJECT_0) {
        throw std::runtime_error{"owned Windows test child did not exit after termination"};
      }
      throw std::system_error{static_cast<int>(error), std::system_category(),
                              "owned Windows test child did not finish"};
    }
    DWORD exit_code = 0;
    if (::GetExitCodeProcess(process_.get(), &exit_code) == 0) {
      throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                              "could not read owned Windows test child exit code"};
    }
    return exit_code;
  }

 private:
  WindowsHandle process_;
  WindowsHandle job_;
  DWORD id_ = 0;
};

[[nodiscard]] inline DWORD RunWindowsProcess(const std::filesystem::path& executable,
                                             std::initializer_list<std::wstring_view> arguments) {
  OwnedWindowsProcess process{executable, arguments};
  return process.WaitForExit();
}

}  // namespace modern_sqlite::test_support

#endif  // MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_PROCESS_HPP_
