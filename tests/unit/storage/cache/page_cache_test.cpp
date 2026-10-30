#include "modern_sqlite/storage/cache/page_cache.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "tests/support/page_cache_test_peer.hpp"

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
    auto inserted = cache->InsertExclusive(PageNumber{1}, MakePage(0x10));
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
  auto inserted = cache->InsertExclusive(PageNumber{1}, MakePage(0x10));
  ASSERT_TRUE(inserted.has_value());
  PageCache::Pin pin = std::move(*inserted);

  const auto clean_mutable = pin.mutable_bytes();
  ASSERT_FALSE(clean_mutable.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, clean_mutable.error().code());

  EXPECT_TRUE(pin.MarkDirty().has_value());
  EXPECT_TRUE(pin.MarkDirty().has_value());
  EXPECT_TRUE(pin->dirty());
  EXPECT_EQ(1U, cache->dirty_page_count());
  auto mutable_bytes = pin.mutable_bytes();
  ASSERT_TRUE(mutable_bytes.has_value());
  mutable_bytes->front() = std::byte{0x7f};
  EXPECT_EQ(std::byte{0x7f}, pin->bytes().front());

  EXPECT_TRUE(pin.MarkClean().has_value());
  EXPECT_TRUE(pin.MarkClean().has_value());
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
    auto first = cache->InsertExclusive(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->MarkDirty().has_value());
  }
  {
    auto second = cache->InsertExclusive(PageNumber{2}, MakePage(0x20));
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second->MarkDirty().has_value());
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
    auto second_lookup = cache->LookupExclusive(PageNumber{2});
    ASSERT_TRUE(second_lookup.has_value());
    ASSERT_TRUE(second_lookup->has_value());
    ASSERT_TRUE((**second_lookup).MarkClean().has_value());
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
    auto dirty = cache->InsertExclusive(PageNumber{3}, MakePage(0x30));
    ASSERT_TRUE(dirty.has_value());
    ASSERT_TRUE(dirty->MarkDirty().has_value());
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
    auto dirty = cache->InsertExclusive(PageNumber{2}, MakePage(0x20));
    ASSERT_TRUE(dirty.has_value());
    ASSERT_TRUE(dirty->MarkDirty().has_value());
  }
  EXPECT_EQ(1U, cache->dirty_page_count());
  EXPECT_TRUE(cache->Discard(PageNumber{2}).has_value());
  EXPECT_EQ(0U, cache->dirty_page_count());
  EXPECT_FALSE(Contains(*cache, PageNumber{2}));
}

TEST(PageCache, ExclusivePinsRejectAliasingReadsAndWrites) {
  auto cache = MakeCache(8, 4);
  ASSERT_NE(nullptr, cache);
  {
    const auto inserted = cache->Insert(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(inserted.has_value());
  }

  auto reader = cache->Lookup(PageNumber{1});
  ASSERT_TRUE(reader.has_value());
  ASSERT_TRUE(reader->has_value());
  const auto busy_writer = cache->LookupExclusive(PageNumber{1});
  ASSERT_FALSE(busy_writer.has_value());
  EXPECT_EQ(ErrorCode::kBusy, busy_writer.error().code());
  reader->reset();

  auto writer = cache->LookupExclusive(PageNumber{1});
  ASSERT_TRUE(writer.has_value());
  ASSERT_TRUE(writer->has_value());
  const auto busy_reader = cache->Lookup(PageNumber{1});
  const auto second_writer = cache->LookupExclusive(PageNumber{1});
  ASSERT_FALSE(busy_reader.has_value());
  ASSERT_FALSE(second_writer.has_value());
  EXPECT_EQ(ErrorCode::kBusy, busy_reader.error().code());
  EXPECT_EQ(ErrorCode::kBusy, second_writer.error().code());

  ASSERT_TRUE((**writer).MarkDirty().has_value());
  auto bytes = (**writer).mutable_bytes();
  ASSERT_TRUE(bytes.has_value());
  bytes->front() = std::byte{0x7f};
  writer->reset();

  auto reloaded = cache->Lookup(PageNumber{1});
  ASSERT_TRUE(reloaded.has_value());
  ASSERT_TRUE(reloaded->has_value());
  EXPECT_EQ(std::byte{0x7f}, (***reloaded).bytes().front());
}

TEST(PageCache, InsertsAnExclusivelyPinnedPage) {
  auto cache = MakeCache(8, 4);
  ASSERT_NE(nullptr, cache);

  auto inserted = cache->InsertExclusive(PageNumber{2}, MakePage(0x20));

  ASSERT_TRUE(inserted.has_value());
  EXPECT_EQ(1U, cache->pin_count());
  EXPECT_EQ(1U, cache->exclusive_pin_count());
  const auto busy_lookup = cache->Lookup(PageNumber{2});
  ASSERT_FALSE(busy_lookup.has_value());
  EXPECT_EQ(ErrorCode::kBusy, busy_lookup.error().code());
  EXPECT_TRUE(inserted->MarkDirty().has_value());
  EXPECT_TRUE(inserted->mutable_bytes().has_value());
  inserted = std::unexpected(Error::Create(ErrorCode::kGeneric, "release exclusive test pin"));
  EXPECT_EQ(0U, cache->exclusive_pin_count());
}

TEST(PageCache, ExposesTheOldestUnpinnedDirtyWritebackCandidate) {
  auto cache = MakeCache(8, 4);
  ASSERT_NE(nullptr, cache);
  {
    auto first = cache->InsertExclusive(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->MarkDirty().has_value());
  }
  auto second = cache->InsertExclusive(PageNumber{2}, MakePage(0x20));
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(second->MarkDirty().has_value());

  ASSERT_TRUE(cache->writeback_candidate().has_value());
  EXPECT_EQ(PageNumber{1}, cache->writeback_candidate()->page_number);
}

TEST(PageCache, DiscardsARangeAtomicallyAndClearsOnlyWithoutPins) {
  auto cache = MakeCache(8, 4);
  ASSERT_NE(nullptr, cache);
  {
    const auto first = cache->Insert(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(first.has_value());
  }
  {
    auto second = cache->InsertExclusive(PageNumber{2}, MakePage(0x20));
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second->MarkDirty().has_value());
  }
  auto third_result = cache->Insert(PageNumber{3}, MakePage(0x30));
  ASSERT_TRUE(third_result.has_value());
  std::optional<PageCache::Pin> third;
  third.emplace(std::move(*third_result));

  const auto busy_discard = cache->DiscardAfter(1);
  ASSERT_FALSE(busy_discard.has_value());
  EXPECT_EQ(ErrorCode::kBusy, busy_discard.error().code());
  EXPECT_TRUE(Contains(*cache, PageNumber{1}));
  EXPECT_TRUE(Contains(*cache, PageNumber{2}));
  EXPECT_TRUE(Contains(*cache, PageNumber{3}));

  const auto busy_clear = cache->Clear();
  ASSERT_FALSE(busy_clear.has_value());
  EXPECT_EQ(ErrorCode::kBusy, busy_clear.error().code());
  third.reset();

  ASSERT_TRUE(cache->DiscardAfter(1).has_value());
  EXPECT_EQ(1U, cache->page_count());
  EXPECT_EQ(0U, cache->dirty_page_count());
  EXPECT_TRUE(Contains(*cache, PageNumber{1}));
  EXPECT_FALSE(Contains(*cache, PageNumber{2}));
  EXPECT_FALSE(Contains(*cache, PageNumber{3}));
  EXPECT_TRUE(cache->Clear().has_value());
  EXPECT_EQ(0U, cache->page_count());
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

TEST(PageCache, ResolvesHashCollisionsAndBenignlyGrowsTheBucketTable) {
  auto cache = MakeCache(8, 300);
  ASSERT_NE(nullptr, cache);

  {
    const auto first = cache->Insert(PageNumber{1}, MakePage(0x10));
    const auto collision = cache->Insert(PageNumber{257}, MakePage(0x20));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(collision.has_value());
  }
  EXPECT_TRUE(Contains(*cache, PageNumber{1}));
  EXPECT_TRUE(Contains(*cache, PageNumber{257}));

  for (std::uint32_t page = 2; page <= 256; ++page) {
    const auto inserted =
        cache->Insert(PageNumber{page}, MakePage(static_cast<std::uint8_t>(page)));
    ASSERT_TRUE(inserted.has_value()) << page;
  }
  EXPECT_EQ(257U, cache->page_count());
  EXPECT_TRUE(Contains(*cache, PageNumber{1}));
  EXPECT_TRUE(Contains(*cache, PageNumber{128}));
  EXPECT_TRUE(Contains(*cache, PageNumber{257}));
}

TEST(PageCache, PrefersSyncSafeDirtyPagesAndAcceptsSharedPeerMarking) {
  auto cache = MakeCache(8, 2);
  ASSERT_NE(nullptr, cache);

  for (std::uint32_t page = 1; page <= 3; ++page) {
    auto inserted = cache->InsertExclusive(PageNumber{page}, MakePage(0x10));
    ASSERT_TRUE(inserted.has_value());
    ASSERT_TRUE(inserted->MarkDirty().has_value());
    if (page <= 2U) {
      ASSERT_TRUE(inserted->MarkNeedsSync().has_value());
    }
  }
  auto candidate = cache->writeback_candidate();
  ASSERT_TRUE(candidate.has_value());
  EXPECT_EQ(PageNumber{3}, candidate->page_number);
  EXPECT_FALSE(candidate->journal_sync_required);

  auto safe_pin = cache->Lookup(PageNumber{3});
  ASSERT_TRUE(safe_pin.has_value());
  ASSERT_TRUE(safe_pin->has_value());
  candidate = cache->writeback_candidate();
  ASSERT_TRUE(candidate.has_value());
  EXPECT_EQ(PageNumber{1}, candidate->page_number);
  EXPECT_TRUE(candidate->journal_sync_required);

  safe_pin->reset();
  candidate = cache->writeback_candidate();
  ASSERT_TRUE(candidate.has_value());
  EXPECT_EQ(PageNumber{3}, candidate->page_number);
  EXPECT_FALSE(candidate->journal_sync_required);

  auto shared_peer = cache->Lookup(PageNumber{1});
  ASSERT_TRUE(shared_peer.has_value());
  ASSERT_TRUE(shared_peer->has_value());
  EXPECT_TRUE((**shared_peer).MarkNeedsSync().has_value());
  cache->ClearSyncFlags();
  EXPECT_FALSE((**shared_peer).needs_sync());
}

TEST(PageCache, DontWriteRevokesMutableAccessUntilTheNextDirtyTransition) {
  auto cache = MakeCache(8, 2);
  auto page = cache->InsertExclusive(PageNumber{1}, MakePage(0x10));
  ASSERT_TRUE(page.has_value());
  ASSERT_TRUE(page->MarkDirty().has_value());
  ASSERT_TRUE(page->MarkNeedsSync().has_value());
  ASSERT_TRUE(page->MarkDontWrite().has_value());

  EXPECT_TRUE(page->dont_write());
  EXPECT_TRUE(page->needs_sync());
  const auto blocked = page->mutable_bytes();
  ASSERT_FALSE(blocked.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, blocked.error().code());

  ASSERT_TRUE(page->MarkDirty().has_value());
  EXPECT_FALSE(page->dont_write());
  EXPECT_TRUE(page->needs_sync());
  EXPECT_TRUE(page->mutable_bytes().has_value());

  ASSERT_TRUE(page->MarkDontWrite().has_value());
  ASSERT_TRUE(page->MarkClean().has_value());
  EXPECT_FALSE(page->dont_write());
  EXPECT_FALSE(page->needs_sync());
}

TEST(PageCache, DemotesEveryExclusiveCapabilityWithoutChangingReferencesOrSyncFlags) {
  auto cache = MakeCache(8, 2);
  ASSERT_NE(nullptr, cache);
  auto first = cache->InsertExclusive(PageNumber{1}, MakePage(0x10));
  auto second = cache->InsertExclusive(PageNumber{2}, MakePage(0x20));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(first->MarkDirty().has_value());
  ASSERT_TRUE(second->MarkDirty().has_value());
  ASSERT_TRUE(first->MarkNeedsSync().has_value());
  ASSERT_TRUE(second->MarkDontWrite().has_value());
  const PageFrame* const first_frame = &first->frame();
  const PageFrame* const second_frame = &second->frame();
  ASSERT_EQ(2U, cache->pin_count());
  ASSERT_EQ(2U, cache->exclusive_pin_count());

  ASSERT_TRUE(PageCacheTestPeer::Demote(*cache, *first).has_value());
  ASSERT_TRUE(PageCacheTestPeer::Demote(*cache, *second).has_value());

  EXPECT_EQ(2U, cache->pin_count());
  EXPECT_EQ(0U, cache->exclusive_pin_count());
  EXPECT_EQ(2U, cache->dirty_page_count());
  EXPECT_EQ(first_frame, &first->frame());
  EXPECT_EQ(second_frame, &second->frame());
  EXPECT_FALSE(first->exclusive());
  EXPECT_FALSE(second->exclusive());
  EXPECT_TRUE(first->needs_sync());
  EXPECT_FALSE(first->dont_write());
  EXPECT_FALSE(second->needs_sync());
  EXPECT_TRUE(second->dont_write());
  auto peer = cache->Lookup(PageNumber{1});
  ASSERT_TRUE(peer.has_value());
  ASSERT_TRUE(peer->has_value());
  EXPECT_EQ(first_frame, &(**peer).frame());
  const auto mutable_bytes = first->mutable_bytes();
  ASSERT_FALSE(mutable_bytes.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, mutable_bytes.error().code());
}

TEST(PageCache, RestoresExclusiveBytesWithoutChangingFrameFlagsOrIdentity) {
  constexpr std::array states{
      std::array{false, false, false}, std::array{false, true, false},
      std::array{true, false, false},  std::array{true, true, false},
      std::array{true, false, true},   std::array{true, true, true},
  };
  for (const auto& [dirty, needs_sync, dont_write] : states) {
    SCOPED_TRACE(::testing::Message{} << dirty << '/' << needs_sync << '/' << dont_write);
    auto cache = MakeCache(8, 1);
    ASSERT_NE(nullptr, cache);
    auto pin = cache->InsertExclusive(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(pin.has_value());
    if (dirty) {
      ASSERT_TRUE(pin->MarkDirty().has_value());
    }
    if (needs_sync) {
      ASSERT_TRUE(pin->MarkNeedsSync().has_value());
    }
    if (dont_write) {
      ASSERT_TRUE(pin->MarkDontWrite().has_value());
    }
    const PageFrame* const frame = &pin->frame();
    const std::byte* const storage = pin->frame().bytes().data();
    const ByteBuffer image = MakePage(0x70);

    ASSERT_TRUE(PageCacheTestPeer::RestoreBytes(*cache, *pin, image.view()).has_value());

    EXPECT_TRUE(std::ranges::equal(image.view(), pin->frame().bytes()));
    EXPECT_EQ(frame, &pin->frame());
    EXPECT_EQ(storage, pin->frame().bytes().data());
    EXPECT_EQ(PageNumber{1}, pin->frame().page_number());
    EXPECT_EQ(dirty, pin->frame().dirty());
    EXPECT_EQ(needs_sync, pin->needs_sync());
    EXPECT_EQ(dont_write, pin->dont_write());
    EXPECT_EQ(1U, cache->pin_count());
    EXPECT_EQ(1U, cache->exclusive_pin_count());
    EXPECT_TRUE(PageCacheTestPeer::RestoreBytes(*cache, *pin, pin->frame().bytes()).has_value());
  }
}

TEST(PageCache, RejectsReplayWithoutOwnedExclusivePageSizedBytes) {
  auto cache = MakeCache(8, 2);
  auto other = MakeCache(8, 1);
  ASSERT_NE(nullptr, cache);
  ASSERT_NE(nullptr, other);
  auto owned = cache->InsertExclusive(PageNumber{1}, MakePage(0x10));
  auto shared = cache->Insert(PageNumber{2}, MakePage(0x20));
  auto foreign = other->InsertExclusive(PageNumber{3}, MakePage(0x30));
  ASSERT_TRUE(owned.has_value());
  ASSERT_TRUE(shared.has_value());
  ASSERT_TRUE(foreign.has_value());
  const ByteBuffer image = MakePage(0x70);
  const ByteBuffer long_image = MakePage(0x70, ByteCount{9});
  const auto reject = [&](PageCache::Pin& pin, ByteView bytes) {
    const auto rejected = PageCacheTestPeer::RestoreBytes(*cache, pin, bytes);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
  };

  reject(*shared, image.view());
  reject(*foreign, image.view());
  reject(*owned, ByteView{});
  reject(*owned, image.view().first(7));
  reject(*owned, long_image.view());
  auto moved = std::move(*owned);
  reject(*owned, image.view());

  EXPECT_EQ(std::byte{0x10}, moved.frame().bytes().front());
  EXPECT_EQ(std::byte{0x20}, shared->frame().bytes().front());
  EXPECT_EQ(std::byte{0x30}, foreign->frame().bytes().front());
  EXPECT_TRUE(PageCacheTestPeer::RestoreBytes(*cache, moved, image.view()).has_value());
}

TEST(PageCache, PermutesPageNumberOwnedSyncFlagsWithoutMovingFrames) {
  auto cache = MakeCache(8, 3);
  ASSERT_NE(nullptr, cache);

  auto first = cache->InsertExclusive(PageNumber{1}, MakePage(0x10));
  auto second = cache->InsertExclusive(PageNumber{2}, MakePage(0x20));
  auto third = cache->InsertExclusive(PageNumber{3}, MakePage(0x30));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());
  ASSERT_TRUE(first->MarkDirty().has_value());
  ASSERT_TRUE(second->MarkDirty().has_value());
  ASSERT_TRUE(third->MarkDirty().has_value());
  ASSERT_TRUE(first->MarkNeedsSync().has_value());
  ASSERT_TRUE(third->MarkNeedsSync().has_value());
  ASSERT_TRUE(first->MarkDontWrite().has_value());

  const PageFrame* const first_frame = &first->frame();
  const PageFrame* const second_frame = &second->frame();
  const PageFrame* const third_frame = &third->frame();
  const std::array rekeys{
      PageCacheTestPeer::Rekey{.pin = &*first, .final_page = PageNumber{2}},
      PageCacheTestPeer::Rekey{.pin = &*second, .final_page = PageNumber{3}},
      PageCacheTestPeer::Rekey{.pin = &*third, .final_page = PageNumber{1}},
  };
  ASSERT_TRUE(PageCacheTestPeer::Permute(*cache, rekeys).has_value());

  EXPECT_EQ(first_frame, &first->frame());
  EXPECT_EQ(second_frame, &second->frame());
  EXPECT_EQ(third_frame, &third->frame());
  EXPECT_EQ(PageNumber{2}, first->frame().page_number());
  EXPECT_EQ(PageNumber{3}, second->frame().page_number());
  EXPECT_EQ(PageNumber{1}, third->frame().page_number());
  EXPECT_FALSE(first->needs_sync());
  EXPECT_TRUE(second->needs_sync());
  EXPECT_TRUE(third->needs_sync());
  EXPECT_TRUE(first->dont_write());
  EXPECT_FALSE(second->dont_write());
  EXPECT_FALSE(third->dont_write());
}

TEST(PageCache, ReleasesTemporaryOverflowSlotsAfterPinsSubside) {
  auto cache = MakeCache(8, 1);
  ASSERT_NE(nullptr, cache);

  std::optional<PageCache::Pin> first;
  std::optional<PageCache::Pin> second;
  auto first_result = cache->Insert(PageNumber{1}, MakePage(0x10));
  auto second_result = cache->Insert(PageNumber{2}, MakePage(0x20));
  ASSERT_TRUE(first_result.has_value());
  ASSERT_TRUE(second_result.has_value());
  first.emplace(std::move(*first_result));
  second.emplace(std::move(*second_result));
  ASSERT_EQ(2U, PageCacheTestPeer::allocated_slots(*cache));

  first.reset();
  EXPECT_EQ(1U, cache->page_count());
  EXPECT_EQ(1U, PageCacheTestPeer::allocated_slots(*cache));
  second.reset();
  EXPECT_EQ(1U, PageCacheTestPeer::allocated_slots(*cache));
}

#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
TEST(PageCacheInstrumentation, CountsOnlyValidHitAndMissLookups) {
  using instrumentation::Counter;
  using instrumentation::CounterCollection;
  using instrumentation::ScopedCounterCollection;

  auto cache = MakeCache(8, 2);
  ASSERT_NE(nullptr, cache);
  {
    const auto inserted = cache->Insert(PageNumber{1}, MakePage(0x10));
    ASSERT_TRUE(inserted.has_value());
  }

  CounterCollection counters;
  {
    const ScopedCounterCollection scope{counters};

    const auto hit = cache->Lookup(PageNumber{1});
    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(hit->has_value());

    const auto miss = cache->Lookup(PageNumber{2});
    ASSERT_TRUE(miss.has_value());
    EXPECT_FALSE(miss->has_value());

    const auto invalid = cache->Lookup(PageNumber{0});
    ASSERT_FALSE(invalid.has_value());
  }

  EXPECT_EQ(1U, counters.Value(Counter::kCacheHits));
  EXPECT_EQ(1U, counters.Value(Counter::kCacheMisses));
}
#endif

}  // namespace
}  // namespace modern_sqlite
