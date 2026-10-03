#ifndef MODERN_SQLITE_STORAGE_BTREE_PAGE_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_PAGE_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

class BtreePageGeometry final {
 public:
  [[nodiscard]] static Result<BtreePageGeometry> Create(ByteCount page_size, ByteCount usable_size);

  [[nodiscard]] constexpr ByteCount page_size() const noexcept { return page_size_; }
  [[nodiscard]] constexpr ByteCount usable_size() const noexcept { return usable_size_; }
  [[nodiscard]] constexpr ByteCount minimum_local_payload() const noexcept {
    return minimum_local_payload_;
  }
  [[nodiscard]] constexpr ByteCount maximum_index_local_payload() const noexcept {
    return maximum_index_local_payload_;
  }
  [[nodiscard]] constexpr ByteCount maximum_table_leaf_local_payload() const noexcept {
    return maximum_table_leaf_local_payload_;
  }
  [[nodiscard]] constexpr ByteCount overflow_payload_capacity() const noexcept {
    return overflow_payload_capacity_;
  }
  [[nodiscard]] constexpr PageNumber locking_page() const noexcept { return locking_page_; }

 private:
  constexpr BtreePageGeometry(ByteCount page_size, ByteCount usable_size,
                              ByteCount minimum_local_payload,
                              ByteCount maximum_index_local_payload,
                              ByteCount maximum_table_leaf_local_payload,
                              ByteCount overflow_payload_capacity, PageNumber locking_page) noexcept
      : page_size_(page_size),
        usable_size_(usable_size),
        minimum_local_payload_(minimum_local_payload),
        maximum_index_local_payload_(maximum_index_local_payload),
        maximum_table_leaf_local_payload_(maximum_table_leaf_local_payload),
        overflow_payload_capacity_(overflow_payload_capacity),
        locking_page_(locking_page) {}

  ByteCount page_size_;
  ByteCount usable_size_;
  ByteCount minimum_local_payload_;
  ByteCount maximum_index_local_payload_;
  ByteCount maximum_table_leaf_local_payload_;
  ByteCount overflow_payload_capacity_;
  PageNumber locking_page_;
};

enum class BtreePageType : std::uint8_t {
  kInteriorIndex = 0x02,
  kInteriorTable = 0x05,
  kLeafIndex = 0x0a,
  kLeafTable = 0x0d,
};

class BtreePageView;

class BtreeCellView final {
 public:
  [[nodiscard]] constexpr std::optional<PageNumber> left_child() const noexcept {
    return left_child_;
  }
  [[nodiscard]] constexpr std::optional<std::int64_t> rowid() const noexcept { return rowid_; }
  [[nodiscard]] constexpr ByteCount payload_size() const noexcept { return payload_size_; }
  [[nodiscard]] constexpr ByteView local_payload() const noexcept { return local_payload_; }
  [[nodiscard]] constexpr std::optional<PageNumber> first_overflow_page() const noexcept {
    return first_overflow_page_;
  }
  [[nodiscard]] constexpr ByteCount encoded_size() const noexcept { return encoded_size_; }

 private:
  friend class BtreePageView;

  constexpr BtreeCellView(std::optional<PageNumber> left_child, std::optional<std::int64_t> rowid,
                          ByteCount payload_size, ByteView local_payload,
                          std::optional<PageNumber> first_overflow_page,
                          ByteCount encoded_size) noexcept
      : left_child_(left_child),
        rowid_(rowid),
        payload_size_(payload_size),
        local_payload_(local_payload),
        first_overflow_page_(first_overflow_page),
        encoded_size_(encoded_size) {}

  std::optional<PageNumber> left_child_;
  std::optional<std::int64_t> rowid_;
  ByteCount payload_size_;
  ByteView local_payload_;
  std::optional<PageNumber> first_overflow_page_;
  ByteCount encoded_size_;
};

struct BtreeFreeblock {
  ByteCount offset;
  ByteCount size;
};

class BtreeFreeblockCursor final {
 public:
  [[nodiscard]] std::optional<BtreeFreeblock> Next() noexcept;

 private:
  friend class BtreePageFreeSpace;

  constexpr BtreeFreeblockCursor(ByteView page, std::uint16_t next_offset) noexcept
      : page_(page), next_offset_(next_offset) {}

  ByteView page_;
  std::uint16_t next_offset_;
};

class BtreePageFreeSpace final {
 public:
  [[nodiscard]] constexpr ByteCount total() const noexcept { return total_; }
  [[nodiscard]] constexpr BtreeFreeblockCursor freeblocks() const noexcept {
    return BtreeFreeblockCursor{page_, first_freeblock_};
  }

 private:
  friend class BtreePageView;

  constexpr BtreePageFreeSpace(ByteView page, std::uint16_t first_freeblock,
                               ByteCount total) noexcept
      : page_(page), first_freeblock_(first_freeblock), total_(total) {}

  ByteView page_;
  std::uint16_t first_freeblock_;
  ByteCount total_;
};

class BtreePageView final {
 public:
  [[nodiscard]] static Result<BtreePageView> Parse(ByteView page, PageNumber page_number,
                                                   BtreePageGeometry geometry);

  [[nodiscard]] constexpr PageNumber page_number() const noexcept { return page_number_; }
  [[nodiscard]] constexpr BtreePageType type() const noexcept { return type_; }
  [[nodiscard]] constexpr bool is_leaf() const noexcept {
    return type_ == BtreePageType::kLeafIndex || type_ == BtreePageType::kLeafTable;
  }
  [[nodiscard]] constexpr bool is_table() const noexcept {
    return type_ == BtreePageType::kInteriorTable || type_ == BtreePageType::kLeafTable;
  }
  [[nodiscard]] constexpr std::size_t cell_count() const noexcept { return cell_count_; }
  [[nodiscard]] constexpr ByteCount cell_content_offset() const noexcept {
    return ByteCount{cell_content_offset_};
  }
  [[nodiscard]] constexpr ByteCount fragmented_free_bytes() const noexcept {
    return ByteCount{fragmented_free_bytes_};
  }
  [[nodiscard]] constexpr std::optional<PageNumber> rightmost_child() const noexcept {
    return rightmost_child_;
  }
  [[nodiscard]] Result<ByteCount> cell_offset(std::size_t index) const;
  [[nodiscard]] Result<BtreeCellView> cell(std::size_t index) const;
  [[nodiscard]] Result<BtreePageFreeSpace> AnalyzeFreeSpace() const;

 private:
  constexpr BtreePageView(ByteView page, PageNumber page_number, BtreePageGeometry geometry,
                          BtreePageType type, std::size_t cell_pointer_array_offset,
                          std::size_t cell_count, std::size_t cell_content_offset,
                          std::uint16_t first_freeblock, std::uint8_t fragmented_free_bytes,
                          std::optional<PageNumber> rightmost_child) noexcept
      : page_(page),
        page_number_(page_number),
        geometry_(geometry),
        type_(type),
        cell_pointer_array_offset_(cell_pointer_array_offset),
        cell_count_(cell_count),
        cell_content_offset_(cell_content_offset),
        first_freeblock_(first_freeblock),
        fragmented_free_bytes_(fragmented_free_bytes),
        rightmost_child_(rightmost_child) {}

  ByteView page_;
  PageNumber page_number_;
  BtreePageGeometry geometry_;
  BtreePageType type_;
  std::size_t cell_pointer_array_offset_;
  std::size_t cell_count_;
  std::size_t cell_content_offset_;
  std::uint16_t first_freeblock_;
  std::uint8_t fragmented_free_bytes_;
  std::optional<PageNumber> rightmost_child_;
};

class OverflowPageView final {
 public:
  [[nodiscard]] static Result<OverflowPageView> Parse(ByteView page, BtreePageGeometry geometry);

  [[nodiscard]] constexpr std::optional<PageNumber> next_page() const noexcept {
    return next_page_;
  }
  [[nodiscard]] constexpr ByteView payload() const noexcept { return payload_; }

 private:
  constexpr OverflowPageView(std::optional<PageNumber> next_page, ByteView payload) noexcept
      : next_page_(next_page), payload_(payload) {}

  std::optional<PageNumber> next_page_;
  ByteView payload_;
};

class FreelistTrunkView final {
 public:
  [[nodiscard]] static Result<FreelistTrunkView> Parse(ByteView page, BtreePageGeometry geometry);

  [[nodiscard]] constexpr std::optional<PageNumber> next_trunk() const noexcept {
    return next_trunk_;
  }
  [[nodiscard]] constexpr std::size_t leaf_count() const noexcept { return leaf_count_; }
  [[nodiscard]] Result<PageNumber> leaf_page(std::size_t index) const;

 private:
  constexpr FreelistTrunkView(ByteView page, BtreePageGeometry geometry,
                              std::optional<PageNumber> next_trunk, std::size_t leaf_count) noexcept
      : page_(page), geometry_(geometry), next_trunk_(next_trunk), leaf_count_(leaf_count) {}

  ByteView page_;
  BtreePageGeometry geometry_;
  std::optional<PageNumber> next_trunk_;
  std::size_t leaf_count_;
};

enum class PointerMapType : std::uint8_t {
  kRootPage = 1,
  kFreePage = 2,
  kFirstOverflow = 3,
  kLaterOverflow = 4,
  kBtreeChild = 5,
};

struct PointerMapEntry {
  PointerMapType type;
  std::optional<PageNumber> parent;
};

[[nodiscard]] Result<PageNumber> PointerMapPageFor(PageNumber target, BtreePageGeometry geometry);
[[nodiscard]] Result<bool> IsPointerMapPage(PageNumber page_number, BtreePageGeometry geometry);

class PointerMapView final {
 public:
  [[nodiscard]] static Result<PointerMapView> Parse(ByteView page, PageNumber page_number,
                                                    BtreePageGeometry geometry);

  [[nodiscard]] Result<PointerMapEntry> entry(PageNumber target) const;

 private:
  constexpr PointerMapView(ByteView page, PageNumber page_number,
                           BtreePageGeometry geometry) noexcept
      : page_(page), page_number_(page_number), geometry_(geometry) {}

  ByteView page_;
  PageNumber page_number_;
  BtreePageGeometry geometry_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_BTREE_PAGE_HPP_
