#ifndef MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_HANDLE_HPP_
#define MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_HANDLE_HPP_

#include <windows.h>

#include <memory>

namespace modern_sqlite::test_support {

struct WindowsHandleCloser {
  void operator()(void* handle) const noexcept { static_cast<void>(::CloseHandle(handle)); }
};

using WindowsHandle = std::unique_ptr<void, WindowsHandleCloser>;

[[nodiscard]] inline WindowsHandle OwnWindowsHandle(HANDLE handle) noexcept {
  return WindowsHandle{handle == INVALID_HANDLE_VALUE ? nullptr : handle};
}

}  // namespace modern_sqlite::test_support

#endif  // MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_HANDLE_HPP_
