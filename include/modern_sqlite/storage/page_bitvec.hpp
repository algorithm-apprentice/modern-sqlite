#ifndef MODERN_SQLITE_STORAGE_PAGE_BITVEC_HPP_
#define MODERN_SQLITE_STORAGE_PAGE_BITVEC_HPP_

#include <cstdint>
#include <memory>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

class PageBitvecNode;

class PageBitvec final {
 public:
  [[nodiscard]] static Result<PageBitvec> Create(std::uint32_t page_count);

  PageBitvec(const PageBitvec&) = delete;
  PageBitvec& operator=(const PageBitvec&) = delete;
  PageBitvec(PageBitvec&&) noexcept;
  PageBitvec& operator=(PageBitvec&&) noexcept;
  ~PageBitvec();

  [[nodiscard]] std::uint32_t size() const noexcept { return page_count_; }
  [[nodiscard]] bool Test(PageNumber page_number) const noexcept;
  [[nodiscard]] Status Set(PageNumber page_number);

 private:
  PageBitvec(std::uint32_t page_count, std::unique_ptr<PageBitvecNode> root) noexcept;

  std::uint32_t page_count_;
  std::unique_ptr<PageBitvecNode> root_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_PAGE_BITVEC_HPP_
