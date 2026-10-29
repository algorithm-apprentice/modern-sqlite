#ifndef MODERN_SQLITE_TESTS_SUPPORT_PHASE_TIMING_HPP_
#define MODERN_SQLITE_TESTS_SUPPORT_PHASE_TIMING_HPP_

#include <chrono>

namespace modern_sqlite::test_support {

struct PhaseTimes {
  long long elapsed_ms;
  long long previous_phase_ms;
};

class PhaseTiming final {
 public:
  PhaseTiming() noexcept = default;
  PhaseTiming(const PhaseTiming&) = delete;
  PhaseTiming& operator=(const PhaseTiming&) = delete;

  [[nodiscard]] PhaseTimes Mark() noexcept {
    const auto now = Clock::now();
    const PhaseTimes times{
        .elapsed_ms = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count()),
        .previous_phase_ms = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - previous_).count()),
    };
    previous_ = now;
    return times;
  }

 private:
  using Clock = std::chrono::steady_clock;
  Clock::time_point started_ = Clock::now();
  Clock::time_point previous_ = started_;
};

}  // namespace modern_sqlite::test_support

#endif  // MODERN_SQLITE_TESTS_SUPPORT_PHASE_TIMING_HPP_
