#ifndef MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_TEMPORARY_DIRECTORY_HPP_
#define MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_TEMPORARY_DIRECTORY_HPP_

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace modern_sqlite::test_support {

class WindowsTemporaryDirectory final {
 public:
  explicit WindowsTemporaryDirectory(
      std::filesystem::path base = std::filesystem::temp_directory_path()) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::move(base) / ("modern-sqlite-windows-" + std::to_string(::GetCurrentProcessId()) +
                               "-" + std::to_string(suffix));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error{"could not create Windows test directory"};
    }
  }

  WindowsTemporaryDirectory(const WindowsTemporaryDirectory&) = delete;
  WindowsTemporaryDirectory& operator=(const WindowsTemporaryDirectory&) = delete;

  ~WindowsTemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] std::string Path(std::string_view name) const {
    const auto utf8 = NativePath(name).u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
  }

  [[nodiscard]] std::filesystem::path NativePath(std::string_view name) const {
    return path_ / std::filesystem::path{std::u8string{name.begin(), name.end()}};
  }

 private:
  std::filesystem::path path_;
};

}  // namespace modern_sqlite::test_support

#endif  // MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_TEMPORARY_DIRECTORY_HPP_
