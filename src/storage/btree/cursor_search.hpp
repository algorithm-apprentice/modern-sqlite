#ifndef MODERN_SQLITE_STORAGE_BTREE_CURSOR_SEARCH_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_CURSOR_SEARCH_HPP_

#include <compare>
#include <cstddef>
#include <utility>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/storage/btree/page.hpp"

namespace modern_sqlite::btree_internal {

template <typename CompareCell>
[[nodiscard]] Result<BtreePageSearchResult> SearchIndexCells(std::size_t cell_count,
                                                             CompareCell&& compare_cell) {
  if (cell_count == 0U) {
    return BtreePageSearchResult{.cell_index = 0, .insertion_index = 0, .comparison = -1};
  }
  std::size_t lower = 0;
  std::size_t upper = cell_count;
  std::size_t index = 0;
  int comparison = 0;
  while (lower < upper) {
    index = lower + (upper - lower - 1U) / 2U;
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kBtreeComparisons, 1);
    auto ordering = compare_cell(index);
    if (!ordering.has_value()) {
      return std::unexpected(std::move(ordering.error()));
    }
    if (*ordering == std::weak_ordering::less) {
      lower = index + 1U;
      comparison = -1;
    } else if (*ordering == std::weak_ordering::greater) {
      upper = index;
      comparison = 1;
    } else {
      return BtreePageSearchResult{
          .cell_index = index,
          .insertion_index = index,
          .comparison = 0,
      };
    }
  }
  return BtreePageSearchResult{
      .cell_index = index,
      .insertion_index = lower,
      .comparison = comparison,
  };
}

}  // namespace modern_sqlite::btree_internal

#endif  // MODERN_SQLITE_STORAGE_BTREE_CURSOR_SEARCH_HPP_
