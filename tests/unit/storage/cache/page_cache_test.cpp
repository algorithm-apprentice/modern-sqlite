#include "modern_sqlite/storage/cache/page_cache.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<PageCache>);
static_assert(!std::is_copy_assignable_v<PageCache>);
static_assert(!std::is_move_constructible_v<PageCache>);
static_assert(!std::is_move_assignable_v<PageCache>);
static_assert(!std::is_copy_constructible_v<PageFrame>);
static_assert(!std::is_move_constructible_v<PageFrame>);
static_assert(!std::is_default_constructible_v<PageCache::Pin>);
static_assert(!std::is_copy_constructible_v<PageCache::Pin>);
static_assert(!std::is_copy_assignable_v<PageCache::Pin>);
static_assert(std::is_nothrow_move_constructible_v<PageCache::Pin>);
static_assert(std::is_nothrow_move_assignable_v<PageCache::Pin>);

[[nodiscard]] ByteBuffer MakePage(std::uint8_t seed, ByteCount size = ByteCount{8}) {
  ByteBuffer page{size};
  for (std::size_t index = 0; index < size.value(); ++index) {
    page.mutable_view()[index] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(index));
  }
  return page;
}

[[nodiscard]] std::unique_ptr<PageCache> MakeCache(std::size_t page_size,
                                                   std::size_t capacity_pages) {
  auto cache = PageCache::Create(PageCacheOptions{
      .page_size = ByteCount{page_size},
      .capacity_pages = capacity_pages,
  });
  EXPECT_TRUE(cache.has_value());
  if (!cache.has_value()) {
    return nullptr;
  }
  return std::move(*cache);
}

[[nodiscard]] bool Contains(PageCache& cache, PageNumber page_number) {
  auto found = cache.Lookup(page_number);
  EXPECT_TRUE(found.has_value());
  return found.has_value() && found->has_value();
}

TEST(PageCache, ValidatesOptionsPageNumbersSizesAndDuplicateInsertion) {
  const auto invalid_cache = PageCache::Create(PageCacheOptions{
      .page_size = ByteCount{0},
      .capacity_pages = 1,
  });
  ASSERT_FALSE(invalid_cache.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_cache.error().code());

  auto cache = MakeCache(16, 2);
  ASSERT_NE(nullptr, cache);
  EXPECT_EQ(ByteCount{16}, cache->page_size());
  EXPECT_EQ(2U, cache->capacity_pages());

  const auto zero_page = cache->Insert(PageNumber{0}, MakePage(0x10, ByteCount{16}));
  ASSERT_FALSE(zero_page.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, zero_page.error().code());

  const auto wrong_size = cache->Insert(PageNumber{1}, MakePage(0x20, ByteCount{15}));
  ASSERT_FALSE(wrong_size.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, wrong_size.error().code());

  {
    const auto inserted = cache->Insert(PageNumber{1}, MakePage(0x30, ByteCount{16}));
    ASSERT_TRUE(inserted.has_value());
  }
  const auto duplicate = cache->Insert(PageNumber{1}, MakePage(0x40, ByteCount{16}));
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, duplicate.error().code());

  const auto invalid_lookup = cache->Lookup(PageNumber{0});
  ASSERT_FALSE(invalid_lookup.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_lookup.error().code());

  const auto invalid_discard = cache->Discard(PageNumber{0});
  ASSERT_FALSE(invalid_discard.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_discard.error().code());
}

TEST(PageCache, InsertsLooksUpAndPinsTypedFrames) {
  auto cache = MakeCache(8, 4);
  ASSERT_NE(nullptr, cache);

  std::optional<PageCache::Pin> first_pin;
  {
    auto inserted = cache->Insert(PageNumber{7}, MakePage(0x10));
    ASSERT_TRUE(inserted.has_value());
    first_pin.emplace(std::move(*inserted));
  }
  EXPECT_EQ(1U, cache->page_count());
  EXPECT_EQ(1U, cache->pin_count());
  EXPECT_EQ(PageNumber{7}, first_pin->frame().page_number());
  EXPECT_EQ(PageNumber{7}, first_pin->operator->()->page_number());
  EXPECT_EQ(std::byte{0x10}, first_pin->frame().bytes().front());
  EXPECT_FALSE(first_pin->frame().dirty());

  auto found = cache->Lookup(PageNumber{7});
  ASSERT_TRUE(found.has_value());
  ASSERT_TRUE(found->has_value());
  EXPECT_EQ(2U, cache->pin_count());
  EXPECT_EQ(std::byte{0x17}, (**found)->bytes().back());

  auto missing = cache->Lookup(PageNumber{8});
  ASSERT_TRUE(missing.has_value());
  EXPECT_FALSE(missing->has_value());

  first_pin.reset();
  EXPECT_EQ(1U, cache->pin_count());
}

TEST(PageCache, MoveAssignmentReleasesThePreviousPin) {
  auto cache = MakeCache(8, 1);
  ASSERT_NE(nullptr, cache);

  auto first_result = cache->Insert(PageNumber{1}, MakePage(0x10));
  auto second_result = cache->Insert(PageNumber{2}, MakePage(0x20));
  ASSERT_TRUE(first_result.has_value());
  ASSERT_TRUE(second_result.has_value());
  PageCache::Pin first = std::move(*first_result);
  PageCache::Pin second = std::move(*second_result);
  EXPECT_EQ(2U, cache->pin_count());

  first = std::move(second);
  EXPECT_EQ(PageNumber{2}, first->page_number());
  EXPECT_EQ(1U, cache->pin_count());
  EXPECT_FALSE(Contains(*cache, PageNumber{1}));

  PageCache::Pin* alias = &first;
  first = std::move(*alias);
  EXPECT_EQ(PageNumber{2}, first->page_number());
  EXPECT_EQ(1U, cache->pin_count());
}

TEST(PageCache, EvictsLeastRecentlyUsedUnpinnedCleanPage) {
  auto cache = MakeCache(8, 2);
  ASSERT_NE(nullptr, cache);

  {
    const auto first = cache->Insert(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(first.has_value());
  }
  {
    const auto second = cache->Insert(PageNumber{2}, MakePage(0x20));
    ASSERT_TRUE(second.has_value());
  }
  {
    auto refreshed = cache->Lookup(PageNumber{1});
    ASSERT_TRUE(refreshed.has_value());
    ASSERT_TRUE(refreshed->has_value());
  }
  {
    const auto third = cache->Insert(PageNumber{3}, MakePage(0x30));
    ASSERT_TRUE(third.has_value());
  }

  EXPECT_EQ(2U, cache->page_count());
  EXPECT_TRUE(Contains(*cache, PageNumber{1}));
  EXPECT_FALSE(Contains(*cache, PageNumber{2}));
  EXPECT_TRUE(Contains(*cache, PageNumber{3}));
}

TEST(PageCache, AllowsPinnedPagesToExceedSoftCapacity) {
  auto cache = MakeCache(8, 1);
  ASSERT_NE(nullptr, cache);

  std::optional<PageCache::Pin> first;
  std::optional<PageCache::Pin> second;
  {
    auto inserted = cache->Insert(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(inserted.has_value());
    first.emplace(std::move(*inserted));
  }
  {
    auto inserted = cache->Insert(PageNumber{2}, MakePage(0x20));
    ASSERT_TRUE(inserted.has_value());
    second.emplace(std::move(*inserted));
  }

  EXPECT_EQ(2U, cache->page_count());
  EXPECT_EQ(2U, cache->pin_count());
  EXPECT_EQ(1U, cache->pressure().excess_pages);
  EXPECT_FALSE(cache->pressure().writeback.has_value());

  first.reset();
  EXPECT_EQ(1U, cache->page_count());
  EXPECT_FALSE(Contains(*cache, PageNumber{1}));
  EXPECT_TRUE(Contains(*cache, PageNumber{2}));
}

TEST(PageCache, TracksDirtyStateAndRestrictsMutableAccess) {
  auto cache = MakeCache(8, 2);
  ASSERT_NE(nullptr, cache);
  auto inserted = cache->Insert(PageNumber{1}, MakePage(0x10));
  ASSERT_TRUE(inserted.has_value());
  PageCache::Pin pin = std::move(*inserted);

  const auto clean_mutable = pin.mutable_bytes();
  ASSERT_FALSE(clean_mutable.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, clean_mutable.error().code());

  pin.MarkDirty();
  pin.MarkDirty();
  EXPECT_TRUE(pin->dirty());
  EXPECT_EQ(1U, cache->dirty_page_count());
  auto mutable_bytes = pin.mutable_bytes();
  ASSERT_TRUE(mutable_bytes.has_value());
  mutable_bytes->front() = std::byte{0x7f};
  EXPECT_EQ(std::byte{0x7f}, pin->bytes().front());

  pin.MarkClean();
  pin.MarkClean();
  EXPECT_FALSE(pin->dirty());
  EXPECT_EQ(0U, cache->dirty_page_count());
  const auto cleaned_mutable = pin.mutable_bytes();
  ASSERT_FALSE(cleaned_mutable.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, cleaned_mutable.error().code());
}

TEST(PageCache, RequestsOldestUnpinnedDirtyPageUnderPressure) {
  auto cache = MakeCache(8, 2);
  ASSERT_NE(nullptr, cache);

  {
    auto first = cache->Insert(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(first.has_value());
    first->MarkDirty();
  }
  {
    auto second = cache->Insert(PageNumber{2}, MakePage(0x20));
    ASSERT_TRUE(second.has_value());
    second->MarkDirty();
  }
  const auto third = cache->Insert(PageNumber{3}, MakePage(0x30));
  ASSERT_TRUE(third.has_value());

  PageCachePressure pressure = cache->pressure();
  ASSERT_EQ(1U, pressure.excess_pages);
  ASSERT_TRUE(pressure.writeback.has_value());
  EXPECT_EQ(PageNumber{1}, pressure.writeback->page_number);

  {
    auto first_lookup = cache->Lookup(PageNumber{1});
    ASSERT_TRUE(first_lookup.has_value());
    ASSERT_TRUE(first_lookup->has_value());
    pressure = cache->pressure();
    ASSERT_TRUE(pressure.writeback.has_value());
    EXPECT_EQ(PageNumber{2}, pressure.writeback->page_number);

    auto second_lookup = cache->Lookup(PageNumber{2});
    ASSERT_TRUE(second_lookup.has_value());
    ASSERT_TRUE(second_lookup->has_value());
    pressure = cache->pressure();
    EXPECT_EQ(1U, pressure.excess_pages);
    EXPECT_FALSE(pressure.writeback.has_value());
  }
  pressure = cache->pressure();
  ASSERT_TRUE(pressure.writeback.has_value());
  EXPECT_EQ(PageNumber{2}, pressure.writeback->page_number);

  {
    auto second_lookup = cache->Lookup(PageNumber{2});
    ASSERT_TRUE(second_lookup.has_value());
    ASSERT_TRUE(second_lookup->has_value());
    (**second_lookup).MarkClean();
  }

  EXPECT_EQ(2U, cache->page_count());
  EXPECT_EQ(0U, cache->pressure().excess_pages);
  EXPECT_FALSE(cache->pressure().writeback.has_value());
  EXPECT_FALSE(Contains(*cache, PageNumber{2}));
}

TEST(PageCache, ReclaimsOnlyUnpinnedCleanPages) {
  auto cache = MakeCache(8, 8);
  ASSERT_NE(nullptr, cache);

  {
    const auto first = cache->Insert(PageNumber{1}, MakePage(0x10));
    const auto second = cache->Insert(PageNumber{2}, MakePage(0x20));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
  }
  {
    auto dirty = cache->Insert(PageNumber{3}, MakePage(0x30));
    ASSERT_TRUE(dirty.has_value());
    dirty->MarkDirty();
  }
  const auto pinned = cache->Insert(PageNumber{4}, MakePage(0x40));
  ASSERT_TRUE(pinned.has_value());

  EXPECT_EQ(2U, cache->ReclaimClean());
  EXPECT_EQ(2U, cache->page_count());
  EXPECT_EQ(1U, cache->dirty_page_count());
  EXPECT_EQ(1U, cache->pin_count());
  EXPECT_FALSE(Contains(*cache, PageNumber{1}));
  EXPECT_FALSE(Contains(*cache, PageNumber{2}));
  EXPECT_TRUE(Contains(*cache, PageNumber{3}));
  EXPECT_TRUE(Contains(*cache, PageNumber{4}));
}

TEST(PageCache, DiscardsOnlyUnpinnedFramesAndMayDiscardDirtyDataExplicitly) {
  auto cache = MakeCache(8, 4);
  ASSERT_NE(nullptr, cache);
  EXPECT_TRUE(cache->Discard(PageNumber{9}).has_value());

  {
    const auto pinned = cache->Insert(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(pinned.has_value());
    const auto busy = cache->Discard(PageNumber{1});
    ASSERT_FALSE(busy.has_value());
    EXPECT_EQ(ErrorCode::kBusy, busy.error().code());
  }
  EXPECT_TRUE(cache->Discard(PageNumber{1}).has_value());
  EXPECT_FALSE(Contains(*cache, PageNumber{1}));

  {
    auto dirty = cache->Insert(PageNumber{2}, MakePage(0x20));
    ASSERT_TRUE(dirty.has_value());
    dirty->MarkDirty();
  }
  EXPECT_EQ(1U, cache->dirty_page_count());
  EXPECT_TRUE(cache->Discard(PageNumber{2}).has_value());
  EXPECT_EQ(0U, cache->dirty_page_count());
  EXPECT_FALSE(Contains(*cache, PageNumber{2}));
}

TEST(PageCache, ZeroCapacityRetainsCleanPagesOnlyWhilePinned) {
  auto cache = MakeCache(8, 0);
  ASSERT_NE(nullptr, cache);

  {
    const auto inserted = cache->Insert(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(1U, cache->page_count());

    auto found = cache->Lookup(PageNumber{1});
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(2U, cache->pin_count());
  }

  EXPECT_EQ(0U, cache->page_count());
  EXPECT_EQ(0U, cache->pin_count());
  EXPECT_FALSE(Contains(*cache, PageNumber{1}));
}

}  // namespace
}  // namespace modern_sqlite
