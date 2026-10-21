#include "modern_sqlite/storage/cache/page_cache.hpp"

#include <algorithm>
#include <cassert>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "modern_sqlite/instrumentation/counters.hpp"

namespace modern_sqlite {
namespace {

[[nodiscard]] Error Misuse(std::string message) {
  return Error::Create(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] Error Busy(std::string message) {
  return Error::Create(ErrorCode::kBusy, std::move(message));
}

}  // namespace

PageCache::Pin::Pin(Pin&& other) noexcept
    : cache_(std::exchange(other.cache_, nullptr)),
      entry_(std::exchange(other.entry_, nullptr)),
      exclusive_(std::exchange(other.exclusive_, false)) {}

PageCache::Pin& PageCache::Pin::operator=(Pin&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Reset();
  cache_ = std::exchange(other.cache_, nullptr);
  entry_ = std::exchange(other.entry_, nullptr);
  exclusive_ = std::exchange(other.exclusive_, false);
  return *this;
}

PageCache::Pin::~Pin() { Reset(); }

const PageFrame& PageCache::Pin::frame() const noexcept {
  assert(entry_ != nullptr);
  return entry_->frame;
}

const PageFrame* PageCache::Pin::operator->() const noexcept { return &frame(); }

const PageFrame& PageCache::Pin::operator*() const noexcept { return frame(); }

Result<MutableByteView> PageCache::Pin::mutable_bytes() {
  try {
    assert(entry_ != nullptr);
    if (!exclusive_) {
      return std::unexpected(Misuse("mutable page bytes require an exclusive pin"));
    }
    if (!entry_->frame.dirty_) {
      return std::unexpected(Misuse("mutable page bytes require dirty state"));
    }
    if (entry_->dont_write) {
      return std::unexpected(Misuse("mutable page bytes require writable state"));
    }
    return entry_->frame.bytes_.mutable_view();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Status PageCache::Pin::MarkDirty() {
  assert(cache_ != nullptr);
  assert(entry_ != nullptr);
  if (!exclusive_) {
    return std::unexpected(Misuse("dirty transition requires an exclusive pin"));
  }
  cache_->MarkDirty(*entry_);
  return {};
}

Status PageCache::Pin::MarkClean() {
  assert(cache_ != nullptr);
  assert(entry_ != nullptr);
  if (!exclusive_) {
    return std::unexpected(Misuse("clean transition requires an exclusive pin"));
  }
  cache_->MarkClean(*entry_);
  return {};
}

Status PageCache::Pin::MarkNeedsSync() {
  assert(cache_ != nullptr);
  assert(entry_ != nullptr);
  cache_->MarkNeedsSync(*entry_);
  return {};
}

Status PageCache::Pin::MarkDontWrite() {
  assert(cache_ != nullptr);
  assert(entry_ != nullptr);
  if (!exclusive_) {
    return std::unexpected(Misuse("DONT_WRITE requires an exclusive pin"));
  }
  if (!entry_->frame.dirty_) {
    return std::unexpected(Misuse("DONT_WRITE requires dirty state"));
  }
  cache_->MarkDontWrite(*entry_);
  return {};
}

bool PageCache::Pin::sole() const noexcept { return entry_ != nullptr && entry_->pin_count == 1U; }

bool PageCache::Pin::needs_sync() const noexcept { return entry_ != nullptr && entry_->needs_sync; }

bool PageCache::Pin::dont_write() const noexcept { return entry_ != nullptr && entry_->dont_write; }

void PageCache::Pin::Reset() noexcept {
  if (cache_ == nullptr) {
    assert(entry_ == nullptr);
    return;
  }
  assert(entry_ != nullptr);
  cache_->Release(*entry_);
  cache_ = nullptr;
  entry_ = nullptr;
  exclusive_ = false;
}

PageCache::Fill::Fill(Fill&& other) noexcept
    : cache_(std::exchange(other.cache_, nullptr)),
      entry_(std::exchange(other.entry_, nullptr)),
      page_number_(other.page_number_) {}

PageCache::Fill& PageCache::Fill::operator=(Fill&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Reset();
  cache_ = std::exchange(other.cache_, nullptr);
  entry_ = std::exchange(other.entry_, nullptr);
  page_number_ = other.page_number_;
  return *this;
}

PageCache::Fill::~Fill() { Reset(); }

MutableByteView PageCache::Fill::mutable_bytes() noexcept {
  assert(entry_ != nullptr);
  return entry_->frame.bytes_.mutable_view();
}

Result<PageCache::Pin> PageCache::Fill::Publish(bool exclusive) {
  if (cache_ == nullptr || entry_ == nullptr) {
    return std::unexpected(Misuse("page-cache fill is invalid"));
  }
  return cache_->PublishFill(*this, exclusive);
}

void PageCache::Fill::Reset() noexcept {
  if (cache_ != nullptr) {
    assert(entry_ != nullptr);
    cache_->AbortFill(*this);
  } else {
    assert(entry_ == nullptr);
  }
}

Result<std::unique_ptr<PageCache>> PageCache::Create(PageCacheOptions options) {
  if (options.page_size.value() == 0) {
    return std::unexpected(Misuse("page-cache page size must be nonzero"));
  }
  try {
    auto cache = std::make_unique<PageCache>(ConstructionKey{}, options);
    cache->buckets_.assign(kInitialHashBuckets, nullptr);
    cache->entries_.reserve(options.capacity_pages);
    return cache;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

PageCache::~PageCache() {
  assert(total_pin_count_ == 0);
  assert(exclusive_pin_count_ == 0);
  assert(clean_oldest_ == nullptr || clean_oldest_->in_clean_list);
  assert(clean_newest_ == nullptr || clean_newest_->in_clean_list);
  assert(dirty_oldest_ == nullptr || dirty_oldest_->in_dirty_list);
  assert(dirty_newest_ == nullptr || dirty_newest_->in_dirty_list);
}

Result<PageCache::Pin> PageCache::Insert(PageNumber page_number, ByteBuffer bytes) {
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("page zero cannot be cached"));
  }
  if (bytes.size() != page_size_) {
    return std::unexpected(Misuse("inserted page size does not match the cache page size"));
  }
  if (Find(page_number) != nullptr) {
    return std::unexpected(Misuse("page is already present in the cache"));
  }

  MaybeGrowHash();
  auto slot = AcquireFillSlot();
  if (!slot.has_value()) {
    return std::unexpected(std::move(slot.error()));
  }
  (*slot)->frame.bytes_ = std::move(bytes);
  Fill fill{*this, **slot, page_number};
  return PublishFill(fill, false);
}

Result<PageCache::Pin> PageCache::InsertExclusive(PageNumber page_number, ByteBuffer bytes) {
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("page zero cannot be cached"));
  }
  if (bytes.size() != page_size_) {
    return std::unexpected(Misuse("inserted page size does not match the cache page size"));
  }
  if (Find(page_number) != nullptr) {
    return std::unexpected(Misuse("page is already present in the cache"));
  }

  MaybeGrowHash();
  auto slot = AcquireFillSlot();
  if (!slot.has_value()) {
    return std::unexpected(std::move(slot.error()));
  }
  (*slot)->frame.bytes_ = std::move(bytes);
  Fill fill{*this, **slot, page_number};
  return PublishFill(fill, true);
}

Result<std::optional<PageCache::Pin>> PageCache::Lookup(PageNumber page_number) {
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("page zero cannot be cached"));
  }

  Entry* const entry = Find(page_number);
  if (entry == nullptr) {
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kCacheMisses, 1U);
    return std::optional<Pin>{};
  }
  if (entry->exclusively_pinned) {
    return std::unexpected(Busy("cannot read a page while it has an exclusive pin"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kCacheHits, 1U);
  return std::optional<Pin>{Acquire(*entry, false)};
}

Result<std::optional<PageCache::Pin>> PageCache::LookupExclusive(PageNumber page_number) {
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("page zero cannot be cached"));
  }

  Entry* const entry = Find(page_number);
  if (entry == nullptr) {
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kCacheMisses, 1U);
    return std::optional<Pin>{};
  }
  if (entry->pin_count != 0) {
    return std::unexpected(Busy("cannot exclusively pin an already pinned page"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kCacheHits, 1U);
  return std::optional<Pin>{Acquire(*entry, true)};
}

Status PageCache::Discard(PageNumber page_number) {
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("page zero cannot be cached"));
  }

  Entry* const entry = Find(page_number);
  if (entry == nullptr) {
    return {};
  }
  if (entry->pin_count != 0) {
    return std::unexpected(Busy("cannot discard a pinned page"));
  }
  RemoveActive(*entry);
  DestroyEntry(*entry);
  return {};
}

Status PageCache::DiscardAfter(std::uint32_t page_count) {
  for (const std::unique_ptr<Entry>& owned : entries_) {
    const Entry& entry = *owned;
    if (entry.active && entry.frame.page_number_.value() > page_count && entry.pin_count != 0) {
      return std::unexpected(Busy("cannot discard a range containing a pinned page"));
    }
  }

  for (std::size_t index = 0; index < entries_.size();) {
    Entry& entry = *entries_[index];
    if (!entry.active || entry.frame.page_number_.value() <= page_count) {
      ++index;
      continue;
    }
    RemoveActive(entry);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
  }
  return {};
}

Status PageCache::Clear() {
  if (total_pin_count_ != 0) {
    return std::unexpected(Busy("cannot clear a cache containing pinned pages"));
  }
  entries_.clear();
  std::ranges::fill(buckets_, nullptr);
  free_slots_ = nullptr;
  clean_oldest_ = nullptr;
  clean_newest_ = nullptr;
  dirty_oldest_ = nullptr;
  dirty_newest_ = nullptr;
  synced_candidate_ = nullptr;
  active_page_count_ = 0;
  dirty_page_count_ = 0;
  return {};
}

std::size_t PageCache::ReclaimClean() noexcept {
  std::size_t reclaimed = 0;
  while (clean_oldest_ != nullptr) {
    Entry* const victim = clean_oldest_;
    RemoveActive(*victim);
    DestroyEntry(*victim);
    ++reclaimed;
  }
  while (free_slots_ != nullptr) {
    Entry* const victim = free_slots_;
    RemoveFreeSlot(*victim);
    DestroyEntry(*victim);
  }
  return reclaimed;
}

void PageCache::ClearSyncFlags() noexcept {
  for (Entry* entry = dirty_oldest_; entry != nullptr; entry = entry->dirty_next) {
    entry->needs_sync = false;
  }
  synced_candidate_ = dirty_oldest_;
}

PageCachePressure PageCache::pressure() const noexcept {
  PageCachePressure result;
  if (active_page_count_ <= capacity_pages_) {
    return result;
  }

  result.excess_pages = active_page_count_ - capacity_pages_;
  result.writeback = writeback_candidate();
  return result;
}

std::optional<PageWritebackRequest> PageCache::writeback_candidate() const noexcept {
  Entry* candidate = synced_candidate_;
  while (candidate != nullptr && (candidate->pin_count != 0 || candidate->needs_sync)) {
    candidate = candidate->dirty_next;
  }
  synced_candidate_ = candidate;
  if (candidate != nullptr) {
    return PageWritebackRequest{
        .page_number = candidate->frame.page_number_,
        .journal_sync_required = false,
    };
  }

  for (const Entry* entry = dirty_oldest_; entry != nullptr; entry = entry->dirty_next) {
    if (entry->pin_count == 0) {
      return PageWritebackRequest{
          .page_number = entry->frame.page_number_,
          .journal_sync_required = entry->needs_sync,
      };
    }
  }
  return std::nullopt;
}

Result<PageCache::Fill> PageCache::PrepareFill(PageNumber page_number) {
  if (page_number.value() == 0U) {
    return std::unexpected(Misuse("page zero cannot be filled"));
  }
  if (Find(page_number) != nullptr) {
    return std::unexpected(Misuse("page is already present in the cache"));
  }

  MaybeGrowHash();
  auto slot = AcquireFillSlot();
  if (!slot.has_value()) {
    return std::unexpected(std::move(slot.error()));
  }
  return Fill{*this, **slot, page_number};
}

PageCache::Pin PageCache::PublishFill(Fill& fill, bool exclusive) noexcept {
  assert(fill.cache_ == this);
  assert(fill.entry_ != nullptr);
  Entry& entry = *fill.entry_;
  assert(!entry.active);
  assert(entry.pin_count == 0U);
  assert(Find(fill.page_number_) == nullptr);

  entry.frame.page_number_ = fill.page_number_;
  entry.active = true;
  LinkHash(entry);
  ++active_page_count_;
  Pin pin = Acquire(entry, exclusive);
  fill.cache_ = nullptr;
  fill.entry_ = nullptr;
  EnforceCapacity();
  return pin;
}

void PageCache::AbortFill(Fill& fill) noexcept {
  assert(fill.cache_ == this);
  assert(fill.entry_ != nullptr);
  Entry& entry = *fill.entry_;
  assert(!entry.active);
  ResetEntry(entry);
  RetainOrDestroyFreeSlot(entry);
  fill.cache_ = nullptr;
  fill.entry_ = nullptr;
}

PageCache::Pin PageCache::Acquire(Entry& entry, bool exclusive) noexcept {
  assert(entry.active);
  if (entry.in_clean_list) {
    assert(entry.pin_count == 0);
    assert(!entry.frame.dirty_);
    RemoveClean(entry);
  }
  assert(!entry.exclusively_pinned);
  if (exclusive) {
    assert(entry.pin_count == 0);
    entry.exclusively_pinned = true;
    ++exclusive_pin_count_;
  }
  ++entry.pin_count;
  ++total_pin_count_;
  return Pin{*this, entry, exclusive};
}

bool PageCache::OwnsPinForPager(const Pin& pin) const noexcept {
  return pin.cache_ == this && pin.entry_ != nullptr;
}

Status PageCache::PromoteExclusiveForPager(Pin& pin) {
  if (pin.cache_ != this || pin.entry_ == nullptr) {
    return std::unexpected(Misuse("page promotion requires a pin from this cache"));
  }
  if (pin.exclusive_) {
    return {};
  }
  Entry& entry = *pin.entry_;
  if (entry.pin_count != 1U || entry.exclusively_pinned) {
    return std::unexpected(Busy("page promotion requires the sole read pin"));
  }
  entry.exclusively_pinned = true;
  pin.exclusive_ = true;
  ++exclusive_pin_count_;
  return {};
}

Status PageCache::DemoteExclusiveForPager(Pin& pin) {
  if (pin.cache_ != this || pin.entry_ == nullptr || !pin.exclusive_) {
    return std::unexpected(Misuse("page demotion requires an exclusive pin from this cache"));
  }
  Entry& entry = *pin.entry_;
  assert(entry.exclusively_pinned);
  assert(entry.pin_count == 1U);
  assert(exclusive_pin_count_ != 0U);
  entry.exclusively_pinned = false;
  pin.exclusive_ = false;
  --exclusive_pin_count_;
  return {};
}

std::optional<ByteView> PageCache::CachedBytesForPager(PageNumber page_number) const noexcept {
  const Entry* entry = Find(page_number);
  if (entry == nullptr) {
    return std::nullopt;
  }
  return entry->frame.bytes();
}

bool PageCache::NeedsSyncForPager(PageNumber page_number) const noexcept {
  const Entry* entry = Find(page_number);
  return entry != nullptr && entry->needs_sync;
}

void PageCache::MarkDirtyForPager(PageNumber page_number) noexcept {
  Entry* entry = Find(page_number);
  if (entry == nullptr) {
    return;
  }
  entry->dont_write = false;
  if (entry->frame.dirty_) {
    return;
  }
  if (entry->in_clean_list) {
    RemoveClean(*entry);
  }
  entry->frame.dirty_ = true;
  AppendDirtyNewest(*entry);
  ++dirty_page_count_;
}

void PageCache::MarkNeedsSyncForPager(PageNumber page_number) noexcept {
  Entry* entry = Find(page_number);
  if (entry != nullptr) {
    MarkNeedsSync(*entry);
  }
}

bool PageCache::MarkDontWriteForPager(PageNumber page_number) noexcept {
  Entry* entry = Find(page_number);
  if (entry == nullptr || !entry->frame.dirty_) {
    return false;
  }
  entry->dont_write = true;
  return true;
}

Status PageCache::PermuteExclusiveForPager(std::span<const PageCacheRekey> pages) {
  if (pages.size() > kMaximumPermutationPages) {
    return std::unexpected(Misuse("page permutation supports at most five pages"));
  }
  if (pages.empty()) {
    return {};
  }

  std::array<PageNumber, kMaximumPermutationPages> current{};
  std::array<bool, kMaximumPermutationPages> needs_sync{};
  for (std::size_t index = 0; index < pages.size(); ++index) {
    const PageCacheRekey& rekey = pages[index];
    if (rekey.pin == nullptr || rekey.pin->cache_ != this || rekey.pin->entry_ == nullptr ||
        !rekey.pin->exclusive_) {
      return std::unexpected(Misuse("page permutation requires exclusive pins from this cache"));
    }
    const Entry& entry = *rekey.pin->entry_;
    if (!entry.active || !entry.frame.dirty_ || rekey.final_page.value() == 0U) {
      return std::unexpected(
          Misuse("page permutation requires active dirty pins and nonzero targets"));
    }
    current[index] = entry.frame.page_number_;
    needs_sync[index] = entry.needs_sync;
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (pages[prior].pin == rekey.pin || current[prior] == current[index] ||
          pages[prior].final_page == rekey.final_page) {
        return std::unexpected(Misuse("page permutation contains duplicate pages"));
      }
    }
  }
  const auto sources = std::span{current}.first(pages.size());
  for (const PageCacheRekey& rekey : pages) {
    if (std::ranges::find(sources, rekey.final_page) == sources.end()) {
      return std::unexpected(Misuse("page permutation targets must match its source pages"));
    }
  }

  for (const PageCacheRekey& rekey : pages) {
    UnlinkHash(*rekey.pin->entry_);
  }
  for (const PageCacheRekey& rekey : pages) {
    Entry& entry = *rekey.pin->entry_;
    const auto source = std::ranges::find(sources, rekey.final_page);
    assert(source != sources.end());
    const auto source_index = static_cast<std::size_t>(source - sources.begin());
    entry.frame.page_number_ = rekey.final_page;
    entry.needs_sync = needs_sync[source_index];
    LinkHash(entry);
  }
  RepairSyncedCandidate();
  return {};
}

void PageCache::Release(Entry& entry) noexcept {
  assert(entry.pin_count > 0);
  assert(total_pin_count_ > 0);
  if (entry.exclusively_pinned) {
    assert(entry.pin_count == 1);
    assert(exclusive_pin_count_ > 0);
    entry.exclusively_pinned = false;
    --exclusive_pin_count_;
  }
  --entry.pin_count;
  --total_pin_count_;
  if (entry.pin_count != 0) {
    return;
  }

  if (entry.frame.dirty_) {
    RefreshDirtyRecency(entry);
  } else {
    AppendCleanNewest(entry);
  }
  EnforceCapacity();
}

void PageCache::MarkDirty(Entry& entry) noexcept {
  assert(entry.pin_count > 0);
  entry.dont_write = false;
  if (entry.frame.dirty_) {
    return;
  }

  assert(!entry.in_clean_list);
  entry.frame.dirty_ = true;
  AppendDirtyNewest(entry);
  ++dirty_page_count_;
}

void PageCache::MarkClean(Entry& entry) noexcept {
  assert(entry.pin_count > 0);
  if (!entry.frame.dirty_) {
    entry.needs_sync = false;
    entry.dont_write = false;
    return;
  }

  RemoveDirty(entry);
  entry.frame.dirty_ = false;
  entry.needs_sync = false;
  entry.dont_write = false;
  assert(dirty_page_count_ > 0);
  --dirty_page_count_;
}

void PageCache::MarkNeedsSync(Entry& entry) noexcept {
  if (entry.needs_sync) {
    return;
  }
  entry.needs_sync = true;
  if (entry.in_dirty_list && synced_candidate_ == &entry) {
    Entry* candidate = entry.dirty_next;
    while (candidate != nullptr && candidate->needs_sync) {
      candidate = candidate->dirty_next;
    }
    synced_candidate_ = candidate;
  }
}

void PageCache::MarkDontWrite(Entry& entry) noexcept {
  assert(entry.pin_count > 0);
  assert(entry.frame.dirty_);
  entry.dont_write = true;
}

PageCache::Entry* PageCache::Find(PageNumber page_number) const noexcept {
  if (page_number.value() == 0U || buckets_.empty()) {
    return nullptr;
  }
  for (Entry* entry = buckets_[HashIndex(page_number)]; entry != nullptr;
       entry = entry->hash_next) {
    if (entry->frame.page_number_ == page_number) {
      return entry;
    }
  }
  return nullptr;
}

std::size_t PageCache::HashIndex(PageNumber page_number) const noexcept {
  assert(!buckets_.empty());
  assert((buckets_.size() & (buckets_.size() - 1U)) == 0U);
  return static_cast<std::size_t>(page_number.value()) & (buckets_.size() - 1U);
}

void PageCache::MaybeGrowHash() noexcept {
  if (active_page_count_ < buckets_.size() ||
      buckets_.size() > std::numeric_limits<std::size_t>::max() / 2U) {
    return;
  }
  try {
    std::vector<Entry*> grown(buckets_.size() * 2U, nullptr);
    for (const std::unique_ptr<Entry>& owned : entries_) {
      Entry& entry = *owned;
      if (!entry.active) {
        continue;
      }
      const std::size_t bucket =
          static_cast<std::size_t>(entry.frame.page_number_.value()) & (grown.size() - 1U);
      entry.hash_next = grown[bucket];
      grown[bucket] = &entry;
    }
    buckets_ = std::move(grown);
  } catch (const std::bad_alloc&) {
    return;
  } catch (const std::length_error&) {
    return;
  }
}

void PageCache::LinkHash(Entry& entry) noexcept {
  assert(entry.active);
  assert(entry.frame.page_number_.value() != 0U);
  const std::size_t bucket = HashIndex(entry.frame.page_number_);
  entry.hash_next = buckets_[bucket];
  buckets_[bucket] = &entry;
}

void PageCache::UnlinkHash(Entry& entry) noexcept {
  assert(entry.active);
  Entry** link = &buckets_[HashIndex(entry.frame.page_number_)];
  while (*link != &entry) {
    assert(*link != nullptr);
    link = &(*link)->hash_next;
  }
  *link = entry.hash_next;
  entry.hash_next = nullptr;
}

Result<PageCache::Entry*> PageCache::AcquireFillSlot() {
  if (Entry* const free = TakeFreeSlot(); free != nullptr) {
    return free;
  }
  if (capacity_pages_ != 0U && active_page_count_ >= capacity_pages_ && clean_oldest_ != nullptr) {
    return TakeCleanVictim();
  }
  return AllocateEntry();
}

Result<PageCache::Entry*> PageCache::AllocateEntry() {
  try {
    ByteBuffer bytes{page_size_};
    auto entry = std::make_unique<Entry>(PageNumber{0}, std::move(bytes));
    Entry* const result = entry.get();
    entries_.push_back(std::move(entry));
    peak_allocated_slot_count_ = std::max(peak_allocated_slot_count_, entries_.size());
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

PageCache::Entry* PageCache::TakeFreeSlot() noexcept {
  if (free_slots_ == nullptr) {
    return nullptr;
  }
  Entry* const result = free_slots_;
  free_slots_ = result->free_next;
  result->free_next = nullptr;
  return result;
}

PageCache::Entry* PageCache::TakeCleanVictim() noexcept {
  assert(clean_oldest_ != nullptr);
  Entry* const victim = clean_oldest_;
  assert(victim->pin_count == 0U);
  assert(!victim->frame.dirty_);
  RemoveActive(*victim);
  return victim;
}

void PageCache::RetainOrDestroyFreeSlot(Entry& entry) noexcept {
  assert(!entry.active);
  assert(entry.pin_count == 0U);
  if (capacity_pages_ != 0U && entries_.size() <= capacity_pages_) {
    PushFreeSlot(entry);
    return;
  }
  DestroyEntry(entry);
}

void PageCache::PushFreeSlot(Entry& entry) noexcept {
  assert(!entry.active);
  assert(entry.free_next == nullptr);
  entry.free_next = free_slots_;
  free_slots_ = &entry;
}

void PageCache::RemoveFreeSlot(Entry& entry) noexcept {
  Entry** link = &free_slots_;
  while (*link != nullptr && *link != &entry) {
    link = &(*link)->free_next;
  }
  if (*link == &entry) {
    *link = entry.free_next;
    entry.free_next = nullptr;
  }
}

void PageCache::DestroyEntry(Entry& entry) noexcept {
  assert(!entry.active);
  assert(entry.pin_count == 0U);
  assert(!entry.in_clean_list);
  assert(!entry.in_dirty_list);
  RemoveFreeSlot(entry);
  const auto found = std::ranges::find(
      entries_, &entry, [](const std::unique_ptr<Entry>& owned) { return owned.get(); });
  assert(found != entries_.end());
  entries_.erase(found);
}

void PageCache::RemoveActive(Entry& entry) noexcept {
  assert(entry.active);
  assert(entry.pin_count == 0U);
  if (entry.frame.dirty_) {
    RemoveDirty(entry);
    assert(dirty_page_count_ > 0U);
    --dirty_page_count_;
  } else if (entry.in_clean_list) {
    RemoveClean(entry);
  }
  UnlinkHash(entry);
  assert(active_page_count_ > 0U);
  --active_page_count_;
  entry.active = false;
  ResetEntry(entry);
}

void PageCache::ResetEntry(Entry& entry) noexcept {
  entry.frame.page_number_ = PageNumber{0};
  entry.frame.dirty_ = false;
  entry.pin_count = 0;
  entry.hash_next = nullptr;
  entry.clean_previous = nullptr;
  entry.clean_next = nullptr;
  entry.dirty_previous = nullptr;
  entry.dirty_next = nullptr;
  entry.free_next = nullptr;
  entry.in_clean_list = false;
  entry.in_dirty_list = false;
  entry.exclusively_pinned = false;
  entry.needs_sync = false;
  entry.dont_write = false;
}

void PageCache::AppendCleanNewest(Entry& entry) noexcept {
  assert(entry.active);
  assert(entry.pin_count == 0);
  assert(!entry.frame.dirty_);
  assert(!entry.in_clean_list);
  assert(entry.clean_previous == nullptr);
  assert(entry.clean_next == nullptr);

  entry.clean_previous = clean_newest_;
  if (clean_newest_ == nullptr) {
    clean_oldest_ = &entry;
  } else {
    clean_newest_->clean_next = &entry;
  }
  clean_newest_ = &entry;
  entry.in_clean_list = true;
}

void PageCache::RemoveClean(Entry& entry) noexcept {
  assert(entry.in_clean_list);
  if (entry.clean_previous == nullptr) {
    assert(clean_oldest_ == &entry);
    clean_oldest_ = entry.clean_next;
  } else {
    entry.clean_previous->clean_next = entry.clean_next;
  }
  if (entry.clean_next == nullptr) {
    assert(clean_newest_ == &entry);
    clean_newest_ = entry.clean_previous;
  } else {
    entry.clean_next->clean_previous = entry.clean_previous;
  }
  entry.clean_previous = nullptr;
  entry.clean_next = nullptr;
  entry.in_clean_list = false;
}

void PageCache::AppendDirtyNewest(Entry& entry) noexcept {
  assert(entry.active);
  assert(entry.frame.dirty_);
  assert(!entry.in_dirty_list);
  assert(entry.dirty_previous == nullptr);
  assert(entry.dirty_next == nullptr);

  entry.dirty_previous = dirty_newest_;
  if (dirty_newest_ == nullptr) {
    dirty_oldest_ = &entry;
  } else {
    dirty_newest_->dirty_next = &entry;
  }
  dirty_newest_ = &entry;
  entry.in_dirty_list = true;
  if (synced_candidate_ == nullptr) {
    RepairSyncedCandidate();
  }
}

void PageCache::RemoveDirty(Entry& entry) noexcept {
  assert(entry.in_dirty_list);
  Entry* const next = entry.dirty_next;
  if (entry.dirty_previous == nullptr) {
    assert(dirty_oldest_ == &entry);
    dirty_oldest_ = entry.dirty_next;
  } else {
    entry.dirty_previous->dirty_next = entry.dirty_next;
  }
  if (entry.dirty_next == nullptr) {
    assert(dirty_newest_ == &entry);
    dirty_newest_ = entry.dirty_previous;
  } else {
    entry.dirty_next->dirty_previous = entry.dirty_previous;
  }
  entry.dirty_previous = nullptr;
  entry.dirty_next = nullptr;
  entry.in_dirty_list = false;
  if (synced_candidate_ == &entry) {
    synced_candidate_ = next;
    while (synced_candidate_ != nullptr && synced_candidate_->needs_sync) {
      synced_candidate_ = synced_candidate_->dirty_next;
    }
  }
}

void PageCache::RefreshDirtyRecency(Entry& entry) noexcept {
  assert(entry.frame.dirty_);
  assert(entry.in_dirty_list);
  if (dirty_newest_ == &entry) {
    if (synced_candidate_ == nullptr) {
      RepairSyncedCandidate();
    }
    return;
  }
  RemoveDirty(entry);
  AppendDirtyNewest(entry);
}

void PageCache::RepairSyncedCandidate() noexcept {
  synced_candidate_ = dirty_oldest_;
  while (synced_candidate_ != nullptr && synced_candidate_->needs_sync) {
    synced_candidate_ = synced_candidate_->dirty_next;
  }
}

void PageCache::EnforceCapacity() noexcept {
  while (active_page_count_ > capacity_pages_ && clean_oldest_ != nullptr) {
    Entry* const victim = TakeCleanVictim();
    RetainOrDestroyFreeSlot(*victim);
  }
  while (entries_.size() > capacity_pages_ && free_slots_ != nullptr) {
    Entry* const victim = TakeFreeSlot();
    DestroyEntry(*victim);
  }
}

}  // namespace modern_sqlite
