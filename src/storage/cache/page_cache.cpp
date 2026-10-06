#include "modern_sqlite/storage/cache/page_cache.hpp"

#include <cassert>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "modern_sqlite/instrumentation/counters.hpp"

namespace modern_sqlite {
namespace {

[[nodiscard]] Error Misuse(std::string message) {
  return Error::Create(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] Error Busy(std::string message) {
  return Error::Create(ErrorCode::kBusy, std::move(message));
}

[[nodiscard]] Error Internal(std::string message) {
  return Error::Create(ErrorCode::kInternal, std::move(message));
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
  assert(entry_ != nullptr);
  if (!exclusive_) {
    return std::unexpected(Misuse("mutable page bytes require an exclusive pin"));
  }
  if (!entry_->frame.dirty_) {
    return std::unexpected(Misuse("mutable page bytes require dirty state"));
  }
  return entry_->frame.bytes_.mutable_view();
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

Result<std::unique_ptr<PageCache>> PageCache::Create(PageCacheOptions options) {
  if (options.page_size.value() == 0) {
    return std::unexpected(Misuse("page-cache page size must be nonzero"));
  }
  return std::make_unique<PageCache>(ConstructionKey{}, options);
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

  auto [iterator, inserted] =
      pages_.try_emplace(page_number.value(), page_number, std::move(bytes));
  if (!inserted) {
    return std::unexpected(Misuse("page is already present in the cache"));
  }

  Pin pin = Acquire(iterator->second, false);
  EnforceCapacity();
  return pin;
}

Result<PageCache::Pin> PageCache::InsertExclusive(PageNumber page_number, ByteBuffer bytes) {
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("page zero cannot be cached"));
  }
  if (bytes.size() != page_size_) {
    return std::unexpected(Misuse("inserted page size does not match the cache page size"));
  }

  auto [iterator, inserted] =
      pages_.try_emplace(page_number.value(), page_number, std::move(bytes));
  if (!inserted) {
    return std::unexpected(Misuse("page is already present in the cache"));
  }

  Pin pin = Acquire(iterator->second, true);
  EnforceCapacity();
  return pin;
}

Result<std::optional<PageCache::Pin>> PageCache::Lookup(PageNumber page_number) {
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("page zero cannot be cached"));
  }

  const auto iterator = pages_.find(page_number.value());
  if (iterator == pages_.end()) {
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kCacheMisses, 1U);
    return std::optional<Pin>{};
  }
  if (iterator->second.exclusively_pinned) {
    return std::unexpected(Busy("cannot read a page while it has an exclusive pin"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kCacheHits, 1U);
  return std::optional<Pin>{Acquire(iterator->second, false)};
}

Result<std::optional<PageCache::Pin>> PageCache::LookupExclusive(PageNumber page_number) {
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("page zero cannot be cached"));
  }

  const auto iterator = pages_.find(page_number.value());
  if (iterator == pages_.end()) {
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kCacheMisses, 1U);
    return std::optional<Pin>{};
  }
  if (iterator->second.pin_count != 0) {
    return std::unexpected(Busy("cannot exclusively pin an already pinned page"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kCacheHits, 1U);
  return std::optional<Pin>{Acquire(iterator->second, true)};
}

Status PageCache::Discard(PageNumber page_number) {
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("page zero cannot be cached"));
  }

  const auto iterator = pages_.find(page_number.value());
  if (iterator == pages_.end()) {
    return {};
  }

  Entry& entry = iterator->second;
  if (entry.pin_count != 0) {
    return std::unexpected(Busy("cannot discard a pinned page"));
  }
  if (entry.frame.dirty_) {
    RemoveDirty(entry);
    assert(dirty_page_count_ > 0);
    --dirty_page_count_;
  } else {
    RemoveClean(entry);
  }
  pages_.erase(iterator);
  return {};
}

Status PageCache::DiscardAfter(std::uint32_t page_count) {
  for (const auto& [page_number, entry] : pages_) {
    if (page_number > page_count && entry.pin_count != 0) {
      return std::unexpected(Busy("cannot discard a range containing a pinned page"));
    }
  }

  for (auto iterator = pages_.begin(); iterator != pages_.end();) {
    if (iterator->first <= page_count) {
      ++iterator;
      continue;
    }
    Entry& entry = iterator->second;
    if (entry.frame.dirty_) {
      RemoveDirty(entry);
      assert(dirty_page_count_ > 0);
      --dirty_page_count_;
    } else {
      RemoveClean(entry);
    }
    iterator = pages_.erase(iterator);
  }
  return {};
}

Status PageCache::Clear() {
  if (total_pin_count_ != 0) {
    return std::unexpected(Busy("cannot clear a cache containing pinned pages"));
  }
  pages_.clear();
  clean_oldest_ = nullptr;
  clean_newest_ = nullptr;
  dirty_oldest_ = nullptr;
  dirty_newest_ = nullptr;
  dirty_page_count_ = 0;
  return {};
}

std::size_t PageCache::ReclaimClean() noexcept {
  std::size_t reclaimed = 0;
  while (clean_oldest_ != nullptr) {
    Entry* const victim = clean_oldest_;
    const std::uint32_t page_number = victim->frame.page_number_.value();
    RemoveClean(*victim);
    const auto iterator = pages_.find(page_number);
    assert(iterator != pages_.end());
    pages_.erase(iterator);
    ++reclaimed;
  }
  return reclaimed;
}

PageCachePressure PageCache::pressure() const noexcept {
  PageCachePressure result;
  if (pages_.size() <= capacity_pages_) {
    return result;
  }

  result.excess_pages = pages_.size() - capacity_pages_;
  result.writeback = writeback_candidate();
  return result;
}

std::optional<PageWritebackRequest> PageCache::writeback_candidate() const noexcept {
  for (const Entry* entry = dirty_oldest_; entry != nullptr; entry = entry->dirty_next) {
    if (entry->pin_count == 0) {
      return PageWritebackRequest{.page_number = entry->frame.page_number_};
    }
  }
  return std::nullopt;
}

PageCache::Pin PageCache::Acquire(Entry& entry, bool exclusive) noexcept {
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

Status PageCache::RekeyExclusiveForPager(Pin& pin, PageNumber new_page) {
  if (pin.cache_ != this || pin.entry_ == nullptr || !pin.exclusive_) {
    return std::unexpected(Misuse("page rekey requires an exclusive pin from this cache"));
  }
  if (new_page.value() == 0U) {
    return std::unexpected(Misuse("page rekey target must be nonzero"));
  }
  Entry& entry = *pin.entry_;
  if (!entry.frame.dirty_) {
    return std::unexpected(Misuse("page rekey requires a dirty frame"));
  }
  if (entry.frame.page_number_ == new_page) {
    return {};
  }
  if (pages_.contains(new_page.value())) {
    return std::unexpected(Busy("page rekey target already exists"));
  }

  const std::uint32_t old_page = entry.frame.page_number_.value();
  auto node = pages_.extract(old_page);
  if (node.empty()) {
    return std::unexpected(Internal("page rekey source is absent from the cache"));
  }
  node.key() = new_page.value();
  try {
    auto inserted = pages_.insert(std::move(node));
    if (!inserted.inserted) {
      auto rejected = std::move(inserted.node);
      rejected.key() = old_page;
      const auto restored = pages_.insert(std::move(rejected));
      if (!restored.inserted) {
        std::terminate();
      }
      return std::unexpected(Internal("page rekey insertion failed"));
    }
  } catch (const std::bad_alloc&) {
    if (node.empty()) {
      std::terminate();
    }
    node.key() = old_page;
    try {
      const auto restored = pages_.insert(std::move(node));
      if (!restored.inserted) {
        std::terminate();
      }
    } catch (const std::bad_alloc&) {
      std::terminate();
    }
    return std::unexpected(Error::OutOfMemory());
  }
  entry.frame.page_number_ = new_page;
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
    return;
  }

  RemoveDirty(entry);
  entry.frame.dirty_ = false;
  assert(dirty_page_count_ > 0);
  --dirty_page_count_;
}

void PageCache::AppendCleanNewest(Entry& entry) noexcept {
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
}

void PageCache::RemoveDirty(Entry& entry) noexcept {
  assert(entry.in_dirty_list);
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
}

void PageCache::RefreshDirtyRecency(Entry& entry) noexcept {
  assert(entry.frame.dirty_);
  assert(entry.in_dirty_list);
  if (dirty_newest_ == &entry) {
    return;
  }
  RemoveDirty(entry);
  AppendDirtyNewest(entry);
}

void PageCache::EnforceCapacity() noexcept {
  while (pages_.size() > capacity_pages_ && clean_oldest_ != nullptr) {
    Entry* const victim = clean_oldest_;
    const std::uint32_t page_number = victim->frame.page_number_.value();
    RemoveClean(*victim);
    const auto iterator = pages_.find(page_number);
    assert(iterator != pages_.end());
    pages_.erase(iterator);
  }
}

}  // namespace modern_sqlite
