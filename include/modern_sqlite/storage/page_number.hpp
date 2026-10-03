#ifndef MODERN_SQLITE_STORAGE_PAGE_NUMBER_HPP_
#define MODERN_SQLITE_STORAGE_PAGE_NUMBER_HPP_

#include <compare>
#include <cstdint>

namespace modern_sqlite {

class PageNumber final {
 public:
  constexpr PageNumber() noexcept = default;
  constexpr explicit PageNumber(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const PageNumber&) const noexcept = default;

 private:
  std::uint32_t value_ = 0;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_PAGE_NUMBER_HPP_
