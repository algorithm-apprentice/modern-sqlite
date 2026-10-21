#ifndef MODERN_SQLITE_STORAGE_CACHE_PAGE_CACHE_HPP_
#define MODERN_SQLITE_STORAGE_CACHE_PAGE_CACHE_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

class PageCache;
class PageCacheTestPeer;
class Pager;

class PageFrame final {
 public:
  PageFrame(const PageFrame&) = delete;
  PageFrame& operator=(const PageFrame&) = delete;
  PageFrame(PageFrame&&) = delete;
  PageFrame& operator=(PageFrame&&) = delete;
  ~PageFrame() = default;

  [[nodiscard]] PageNumber page_number() const noexcept { return page_number_; }
  [[nodiscard]] ByteView bytes() const noexcept { return bytes_.view(); }
  [[nodiscard]] bool dirty() const noexcept { return dirty_; }

 private:
  friend class PageCache;
  friend class Pager;

  PageFrame(PageNumber page_number, ByteBuffer bytes) noexcept
      : page_number_(page_number), bytes_(std::move(bytes)) {}

  PageNumber page_number_;
  ByteBuffer bytes_;
  bool dirty_ = false;
};

struct PageCacheOptions {
  ByteCount page_size;
  std::size_t capacity_pages = 0;
};

struct PageWritebackRequest {
  PageNumber page_number;
  bool journal_sync_required = false;
};

struct PageCachePressure {
  std::size_t excess_pages = 0;
  std::optional<PageWritebackRequest> writeback;
};

// Externally serialized. The cache must outlive every pin, and references or
// byte views obtained from a pin remain valid only while that pin lives.
class PageCache final {
 private:
  friend class Pager;
  friend class PageCacheTestPeer;

  struct Entry;
  struct ConstructionKey final {};

 public:
  class Pin final {
   public:
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
    Pin(Pin&& other) noexcept;
    Pin& operator=(Pin&& other) noexcept;
    ~Pin();

    [[nodiscard]] const PageFrame& frame() const noexcept;
    [[nodiscard]] const PageFrame* operator->() const noexcept;
    [[nodiscard]] const PageFrame& operator*() const noexcept;
    // The returned view must not outlive this pin or a subsequent MarkClean().
    [[nodiscard]] Result<MutableByteView> mutable_bytes();

    [[nodiscard]] Status MarkDirty();
    [[nodiscard]] Status MarkClean();
    [[nodiscard]] Status MarkNeedsSync();
    [[nodiscard]] Status MarkDontWrite();
    [[nodiscard]] bool exclusive() const noexcept { return exclusive_; }
    [[nodiscard]] bool sole() const noexcept;
    [[nodiscard]] bool needs_sync() const noexcept;
    [[nodiscard]] bool dont_write() const noexcept;

   private:
    friend class PageCache;
    friend class Pager;

    Pin(PageCache& cache, Entry& entry, bool exclusive) noexcept
        : cache_(&cache), entry_(&entry), exclusive_(exclusive) {}

    void Reset() noexcept;

    PageCache* cache_;
    Entry* entry_;
    bool exclusive_ = false;
  };

  [[nodiscard]] static Result<std::unique_ptr<PageCache>> Create(PageCacheOptions options);

  explicit PageCache(ConstructionKey, PageCacheOptions options) noexcept
      : page_size_(options.page_size), capacity_pages_(options.capacity_pages) {}
  PageCache(const PageCache&) = delete;
  PageCache& operator=(const PageCache&) = delete;
  PageCache(PageCache&&) = delete;
  PageCache& operator=(PageCache&&) = delete;
  ~PageCache();

  [[nodiscard]] Result<Pin> Insert(PageNumber page_number, ByteBuffer bytes);
  [[nodiscard]] Result<Pin> InsertExclusive(PageNumber page_number, ByteBuffer bytes);
  [[nodiscard]] Result<std::optional<Pin>> Lookup(PageNumber page_number);
  [[nodiscard]] Result<std::optional<Pin>> LookupExclusive(PageNumber page_number);
  [[nodiscard]] Status Discard(PageNumber page_number);
  [[nodiscard]] Status DiscardAfter(std::uint32_t page_count);
  [[nodiscard]] Status Clear();
  [[nodiscard]] std::size_t ReclaimClean() noexcept;
  void ClearSyncFlags() noexcept;

  [[nodiscard]] std::optional<PageWritebackRequest> writeback_candidate() const noexcept;
  [[nodiscard]] PageCachePressure pressure() const noexcept;
  [[nodiscard]] ByteCount page_size() const noexcept { return page_size_; }
  [[nodiscard]] std::size_t capacity_pages() const noexcept { return capacity_pages_; }
  [[nodiscard]] std::size_t page_count() const noexcept { return active_page_count_; }
  [[nodiscard]] std::size_t allocated_slot_count() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t peak_allocated_slot_count() const noexcept {
    return peak_allocated_slot_count_;
  }
  [[nodiscard]] std::size_t dirty_page_count() const noexcept { return dirty_page_count_; }
  [[nodiscard]] std::size_t pin_count() const noexcept { return total_pin_count_; }
  [[nodiscard]] std::size_t exclusive_pin_count() const noexcept { return exclusive_pin_count_; }

 private:
  class Fill final {
   public:
    Fill(const Fill&) = delete;
    Fill& operator=(const Fill&) = delete;
    Fill(Fill&& other) noexcept;
    Fill& operator=(Fill&& other) noexcept;
    ~Fill();

    [[nodiscard]] MutableByteView mutable_bytes() noexcept;
    [[nodiscard]] Result<Pin> Publish(bool exclusive);

   private:
    friend class PageCache;

    Fill(PageCache& cache, Entry& entry, PageNumber page_number) noexcept
        : cache_(&cache), entry_(&entry), page_number_(page_number) {}

    void Reset() noexcept;

    PageCache* cache_;
    Entry* entry_;
    PageNumber page_number_;
  };

  struct PageCacheRekey {
    Pin* pin = nullptr;
    PageNumber final_page;
  };

  struct Entry final {
    Entry(PageNumber page_number, ByteBuffer bytes) noexcept
        : frame(page_number, std::move(bytes)) {}

    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) = delete;
    Entry& operator=(Entry&&) = delete;
    ~Entry() = default;

    PageFrame frame;
    std::size_t pin_count = 0;
    Entry* hash_next = nullptr;
    Entry* clean_previous = nullptr;
    Entry* clean_next = nullptr;
    Entry* dirty_previous = nullptr;
    Entry* dirty_next = nullptr;
    Entry* free_next = nullptr;
    bool active = false;
    bool in_clean_list = false;
    bool in_dirty_list = false;
    bool exclusively_pinned = false;
    bool needs_sync = false;
    bool dont_write = false;
  };

  [[nodiscard]] Result<Fill> PrepareFill(PageNumber page_number);
  [[nodiscard]] Pin PublishFill(Fill& fill, bool exclusive) noexcept;
  void AbortFill(Fill& fill) noexcept;

  [[nodiscard]] Pin Acquire(Entry& entry, bool exclusive) noexcept;
  [[nodiscard]] bool OwnsPinForPager(const Pin& pin) const noexcept;
  [[nodiscard]] Status PromoteExclusiveForPager(Pin& pin);
  [[nodiscard]] Status DemoteExclusiveForPager(Pin& pin);
  [[nodiscard]] std::optional<ByteView> CachedBytesForPager(PageNumber page_number) const noexcept;
  [[nodiscard]] bool NeedsSyncForPager(PageNumber page_number) const noexcept;
  void MarkDirtyForPager(PageNumber page_number) noexcept;
  void MarkNeedsSyncForPager(PageNumber page_number) noexcept;
  [[nodiscard]] bool MarkDontWriteForPager(PageNumber page_number) noexcept;
  [[nodiscard]] Status PermuteExclusiveForPager(std::span<const PageCacheRekey> pages);
  void Release(Entry& entry) noexcept;
  void MarkDirty(Entry& entry) noexcept;
  void MarkClean(Entry& entry) noexcept;
  void MarkNeedsSync(Entry& entry) noexcept;
  void MarkDontWrite(Entry& entry) noexcept;

  [[nodiscard]] Entry* Find(PageNumber page_number) const noexcept;
  [[nodiscard]] std::size_t HashIndex(PageNumber page_number) const noexcept;
  void MaybeGrowHash() noexcept;
  void LinkHash(Entry& entry) noexcept;
  void UnlinkHash(Entry& entry) noexcept;

  [[nodiscard]] Result<Entry*> AcquireFillSlot();
  [[nodiscard]] Result<Entry*> AllocateEntry();
  [[nodiscard]] Entry* TakeFreeSlot() noexcept;
  [[nodiscard]] Entry* TakeCleanVictim() noexcept;
  void RetainOrDestroyFreeSlot(Entry& entry) noexcept;
  void PushFreeSlot(Entry& entry) noexcept;
  void RemoveFreeSlot(Entry& entry) noexcept;
  void DestroyEntry(Entry& entry) noexcept;
  void RemoveActive(Entry& entry) noexcept;
  void ResetEntry(Entry& entry) noexcept;

  void AppendCleanNewest(Entry& entry) noexcept;
  void RemoveClean(Entry& entry) noexcept;
  void AppendDirtyNewest(Entry& entry) noexcept;
  void RemoveDirty(Entry& entry) noexcept;
  void RefreshDirtyRecency(Entry& entry) noexcept;
  void RepairSyncedCandidate() noexcept;
  void EnforceCapacity() noexcept;

  static constexpr std::size_t kInitialHashBuckets = 256;
  static constexpr std::size_t kMaximumPermutationPages = 5;

  ByteCount page_size_;
  std::size_t capacity_pages_;
  std::vector<std::unique_ptr<Entry>> entries_;
  std::vector<Entry*> buckets_;
  Entry* free_slots_ = nullptr;
  Entry* clean_oldest_ = nullptr;
  Entry* clean_newest_ = nullptr;
  Entry* dirty_oldest_ = nullptr;
  Entry* dirty_newest_ = nullptr;
  mutable Entry* synced_candidate_ = nullptr;
  std::size_t active_page_count_ = 0;
  std::size_t peak_allocated_slot_count_ = 0;
  std::size_t dirty_page_count_ = 0;
  std::size_t total_pin_count_ = 0;
  std::size_t exclusive_pin_count_ = 0;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_CACHE_PAGE_CACHE_HPP_
