#ifndef MODERN_SQLITE_STORAGE_CACHE_PAGE_CACHE_HPP_
#define MODERN_SQLITE_STORAGE_CACHE_PAGE_CACHE_HPP_

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

class PageCache;

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
};

struct PageCachePressure {
  std::size_t excess_pages = 0;
  std::optional<PageWritebackRequest> writeback;
};

// Externally serialized. The cache must outlive every pin, and references or
// byte views obtained from a pin remain valid only while that pin lives.
class PageCache final {
 private:
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

    void MarkDirty() noexcept;
    void MarkClean() noexcept;

   private:
    friend class PageCache;

    Pin(PageCache& cache, Entry& entry) noexcept : cache_(&cache), entry_(&entry) {}

    void Reset() noexcept;

    PageCache* cache_;
    Entry* entry_;
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
  [[nodiscard]] Result<std::optional<Pin>> Lookup(PageNumber page_number);
  [[nodiscard]] Status Discard(PageNumber page_number);
  [[nodiscard]] std::size_t ReclaimClean() noexcept;

  [[nodiscard]] PageCachePressure pressure() const noexcept;
  [[nodiscard]] ByteCount page_size() const noexcept { return page_size_; }
  [[nodiscard]] std::size_t capacity_pages() const noexcept { return capacity_pages_; }
  [[nodiscard]] std::size_t page_count() const noexcept { return pages_.size(); }
  [[nodiscard]] std::size_t dirty_page_count() const noexcept { return dirty_page_count_; }
  [[nodiscard]] std::size_t pin_count() const noexcept { return total_pin_count_; }

 private:
  struct PageNumberHash final {
    [[nodiscard]] std::size_t operator()(std::uint32_t value) const noexcept {
      return static_cast<std::size_t>(value);
    }
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
    Entry* clean_previous = nullptr;
    Entry* clean_next = nullptr;
    Entry* dirty_previous = nullptr;
    Entry* dirty_next = nullptr;
    bool in_clean_list = false;
    bool in_dirty_list = false;
  };

  using PageMap = std::unordered_map<std::uint32_t, Entry, PageNumberHash>;

  [[nodiscard]] Pin Acquire(Entry& entry) noexcept;
  void Release(Entry& entry) noexcept;
  void MarkDirty(Entry& entry) noexcept;
  void MarkClean(Entry& entry) noexcept;

  void AppendCleanNewest(Entry& entry) noexcept;
  void RemoveClean(Entry& entry) noexcept;
  void AppendDirtyNewest(Entry& entry) noexcept;
  void RemoveDirty(Entry& entry) noexcept;
  void RefreshDirtyRecency(Entry& entry) noexcept;
  void EnforceCapacity() noexcept;

  ByteCount page_size_;
  std::size_t capacity_pages_;
  PageMap pages_;
  Entry* clean_oldest_ = nullptr;
  Entry* clean_newest_ = nullptr;
  Entry* dirty_oldest_ = nullptr;
  Entry* dirty_newest_ = nullptr;
  std::size_t dirty_page_count_ = 0;
  std::size_t total_pin_count_ = 0;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_CACHE_PAGE_CACHE_HPP_
