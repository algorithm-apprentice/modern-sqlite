#ifndef MODERN_SQLITE_TESTS_SUPPORT_TEST_PROGRESS_HPP_
#define MODERN_SQLITE_TESTS_SUPPORT_TEST_PROGRESS_HPP_

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace modern_sqlite::test_support {

class TestProgress final {
 public:
  explicit TestProgress(std::string_view prefix) : prefix_{prefix} {}

  void Report(std::string_view phase, std::size_t index, std::string_view trace) {
    const auto now = std::chrono::steady_clock::now();
    std::cerr << prefix_ << " index=" << index << " phase=" << phase << " wall-us="
              << std::chrono::duration_cast<std::chrono::microseconds>(now - started_).count()
              << " previous-phase-us="
              << std::chrono::duration_cast<std::chrono::microseconds>(now - previous_).count();
#ifdef _WIN32
    std::cerr << " cpu-source=GetProcessTimes cpu-us=" << ProcessCpuMicroseconds() - cpu_started_;
#else
    std::cerr << " cpu-source=unavailable";
#endif
    std::cerr << " trace=" << trace << '\n';
    if (!std::cerr.good()) {
      throw std::runtime_error{"could not publish test progress"};
    }
    previous_ = now;
  }

 private:
#ifdef _WIN32
  [[nodiscard]] static std::uint64_t ProcessCpuMicroseconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0) {
      throw std::system_error{static_cast<int>(::GetLastError()), std::system_category(),
                              "could not measure test process CPU"};
    }
    const auto value = [](FILETIME time) {
      return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    };
    return (value(kernel) + value(user)) / 10U;
  }
#endif
  const std::string prefix_;
  const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point previous_ = started_;
#ifdef _WIN32
  const std::uint64_t cpu_started_ = ProcessCpuMicroseconds();
#endif
};

}  // namespace modern_sqlite::test_support

#endif  // MODERN_SQLITE_TESTS_SUPPORT_TEST_PROGRESS_HPP_
