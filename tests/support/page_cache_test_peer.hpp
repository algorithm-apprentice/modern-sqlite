#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "modern_sqlite/storage/cache/page_cache.hpp"

namespace modern_sqlite {

class PageCacheTestPeer final {
 public:
  struct Rekey {
    PageCache::Pin* pin;
    PageNumber final_page;
  };

  [[nodiscard]] static Status Permute(PageCache& cache, std::span<const Rekey> pages) {
    std::array<PageCache::PageCacheRekey, 5> rekeys{};
    for (std::size_t index = 0; index < pages.size(); ++index) {
      rekeys[index] = PageCache::PageCacheRekey{
          .pin = pages[index].pin,
          .final_page = pages[index].final_page,
      };
    }
    return cache.PermuteExclusiveForPager(std::span{rekeys}.first(pages.size()));
  }

  [[nodiscard]] static std::size_t allocated_slots(const PageCache& cache) noexcept {
    return cache.entries_.size();
  }

  [[nodiscard]] static Status Demote(PageCache& cache, PageCache::Pin& pin) {
    return cache.DemoteExclusiveForPager(pin);
  }

  [[nodiscard]] static Status RestoreBytes(PageCache& cache, PageCache::Pin& pin, ByteView bytes) {
    return cache.RestoreBytesForPager(pin, bytes);
  }
};

}  // namespace modern_sqlite
