#ifndef MODERN_SQLITE_BASE_COMPILER_CONTINUATION_HPP_
#define MODERN_SQLITE_BASE_COMPILER_CONTINUATION_HPP_

#include <cassert>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace modern_sqlite::compiler_detail {

// Internal storage shared by source-defined and header-defined compiler walks.
template <typename Frame>
class ContinuationStack final {
  static_assert(std::is_nothrow_move_constructible_v<Frame>);
  static_assert(std::is_nothrow_destructible_v<Frame>);

 public:
  ContinuationStack() = default;
  ContinuationStack(const ContinuationStack&) = delete;
  ContinuationStack& operator=(const ContinuationStack&) = delete;
  ContinuationStack(ContinuationStack&&) = delete;
  ContinuationStack& operator=(ContinuationStack&&) = delete;

  ~ContinuationStack() noexcept {
    while (!overflow_.empty()) {
      overflow_.pop_back();
    }
    first_.reset();
  }

  [[nodiscard]] bool empty() const noexcept { return !first_.has_value(); }

  [[nodiscard]] std::size_t size() const noexcept {
    return overflow_.size() + (first_.has_value() ? 1U : 0U);
  }

  void Push(Frame&& frame) {
    if (!first_.has_value()) {
      assert(overflow_.empty());
      first_.emplace(std::move(frame));
    } else {
      if (overflow_.size() == overflow_.capacity()) {
        const auto capacity = overflow_.capacity();
        const auto maximum = overflow_.max_size();
        const auto grown = capacity > maximum / 2U ? maximum : capacity * 2U;
        overflow_.reserve(capacity == 0U ? (maximum == 0U ? 0U : 1U) : grown);
      }
      overflow_.push_back(std::move(frame));
    }
  }

  [[nodiscard]] Frame Pop() noexcept {
    assert(first_.has_value());
    if (!overflow_.empty()) {
      Frame frame = std::move(overflow_.back());
      overflow_.pop_back();
      return frame;
    }
    Frame frame = std::move(*first_);
    first_.reset();
    return frame;
  }

 private:
  std::optional<Frame> first_{};
  std::vector<Frame> overflow_{};
};

}  // namespace modern_sqlite::compiler_detail

#endif  // MODERN_SQLITE_BASE_COMPILER_CONTINUATION_HPP_
