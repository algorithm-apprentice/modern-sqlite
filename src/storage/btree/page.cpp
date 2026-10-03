#include "modern_sqlite/storage/btree/page.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/base/coding.hpp"

namespace modern_sqlite {
namespace {

constexpr std::size_t kMinimumPageSize = 512;
constexpr std::size_t kMaximumPageSize = 65536;
constexpr std::size_t kMinimumUsableSize = 480;
constexpr std::size_t kMaximumReservedBytes = 255;
constexpr std::uint32_t kMaximumPageNumber = 0xfffffffeU;
constexpr std::uint64_t kPendingByte = 0x40000000ULL;
constexpr std::uint64_t kMaximumPayloadSize = 0x7fffffffULL;

[[nodiscard]] Error Misuse(std::string message) {
  return Error::Create(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] Error Corruption(std::string message) {
  return Error::Create(ErrorCode::kCorruption, std::move(message));
}

[[nodiscard]] Error OutOfRange(std::string message) {
  return Error::Create(ErrorCode::kOutOfRange, std::move(message));
}

[[nodiscard]] std::uint16_t Load16(ByteView bytes, std::size_t offset) noexcept {
  return LoadBigEndian<std::uint16_t>(std::span<const std::byte, sizeof(std::uint16_t)>{
      bytes.data() + static_cast<std::ptrdiff_t>(offset), sizeof(std::uint16_t)});
}

[[nodiscard]] std::uint32_t Load32(ByteView bytes, std::size_t offset) noexcept {
  return LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
      bytes.data() + static_cast<std::ptrdiff_t>(offset), sizeof(std::uint32_t)});
}

[[nodiscard]] bool IsValidPageReference(PageNumber page_number,
                                        BtreePageGeometry geometry) noexcept {
  return page_number.value() != 0 && page_number.value() <= kMaximumPageNumber &&
         page_number != geometry.locking_page();
}

[[nodiscard]] Result<std::optional<PageNumber>> DecodeOptionalPageReference(
    std::uint32_t raw_page_number, BtreePageGeometry geometry, std::string_view message) {
  if (raw_page_number == 0) {
    return std::optional<PageNumber>{};
  }
  const PageNumber page_number{raw_page_number};
  if (!IsValidPageReference(page_number, geometry)) {
    return std::unexpected(Corruption(std::string{message}));
  }
  return std::optional<PageNumber>{page_number};
}

[[nodiscard]] Result<PageNumber> DecodeRequiredPageReference(std::uint32_t raw_page_number,
                                                             BtreePageGeometry geometry,
                                                             std::string_view message) {
  const PageNumber page_number{raw_page_number};
  if (!IsValidPageReference(page_number, geometry)) {
    return std::unexpected(Corruption(std::string{message}));
  }
  return page_number;
}

[[nodiscard]] Result<DecodedVarint> DecodeCellVarint(ByteView cell, std::size_t offset) {
  if (offset >= cell.size()) {
    return std::unexpected(Corruption("B-tree cell varint is truncated"));
  }
  const auto decoded = DecodeSqliteVarint(cell.subspan(offset));
  if (!decoded.has_value()) {
    return std::unexpected(Corruption("B-tree cell varint is truncated"));
  }
  return *decoded;
}

[[nodiscard]] std::size_t LocalPayloadSize(BtreePageGeometry geometry, BtreePageType type,
                                           std::size_t payload_size) noexcept {
  const std::size_t maximum = type == BtreePageType::kLeafTable
                                  ? geometry.maximum_table_leaf_local_payload().value()
                                  : geometry.maximum_index_local_payload().value();
  if (payload_size <= maximum) {
    return payload_size;
  }

  const std::size_t minimum = geometry.minimum_local_payload().value();
  const std::size_t candidate =
      minimum + ((payload_size - minimum) % geometry.overflow_payload_capacity().value());
  return candidate <= maximum ? candidate : minimum;
}

[[nodiscard]] Result<BtreePageType> DecodePageType(std::byte value) {
  switch (std::to_integer<std::uint8_t>(value)) {
    case static_cast<std::uint8_t>(BtreePageType::kInteriorIndex):
      return BtreePageType::kInteriorIndex;
    case static_cast<std::uint8_t>(BtreePageType::kInteriorTable):
      return BtreePageType::kInteriorTable;
    case static_cast<std::uint8_t>(BtreePageType::kLeafIndex):
      return BtreePageType::kLeafIndex;
    case static_cast<std::uint8_t>(BtreePageType::kLeafTable):
      return BtreePageType::kLeafTable;
    default:
      return std::unexpected(Corruption("B-tree page has an invalid type"));
  }
}

}  // namespace

Result<BtreePageGeometry> BtreePageGeometry::Create(ByteCount page_size, ByteCount usable_size) {
  const std::size_t page_size_value = page_size.value();
  const std::size_t usable_size_value = usable_size.value();
  if (page_size_value < kMinimumPageSize || page_size_value > kMaximumPageSize ||
      !std::has_single_bit(page_size_value)) {
    return std::unexpected(
        Misuse("B-tree page size must be a power of two from 512 through 65536"));
  }
  if (usable_size_value < kMinimumUsableSize || usable_size_value > page_size_value) {
    return std::unexpected(Misuse("B-tree usable size must be between 480 and the page size"));
  }
  if (page_size_value - usable_size_value > kMaximumReservedBytes) {
    return std::unexpected(
        Misuse("B-tree reserved region must fit the one-byte database-header field"));
  }

  const std::size_t minimum_local_payload = ((usable_size_value - 12U) * 32U / 255U) - 23U;
  const std::size_t maximum_index_local_payload = ((usable_size_value - 12U) * 64U / 255U) - 23U;
  const std::size_t maximum_table_leaf_local_payload = usable_size_value - 35U;
  const std::size_t overflow_payload_capacity = usable_size_value - 4U;
  const std::uint64_t locking_page =
      kPendingByte / static_cast<std::uint64_t>(page_size_value) + 1U;
  if (locking_page > kMaximumPageNumber) {
    return std::unexpected(Misuse("B-tree locking page exceeds the page-number range"));
  }

  return BtreePageGeometry{
      page_size,
      usable_size,
      ByteCount{minimum_local_payload},
      ByteCount{maximum_index_local_payload},
      ByteCount{maximum_table_leaf_local_payload},
      ByteCount{overflow_payload_capacity},
      PageNumber{static_cast<std::uint32_t>(locking_page)},
  };
}

std::optional<BtreeFreeblock> BtreeFreeblockCursor::Next() noexcept {
  if (next_offset_ == 0) {
    return std::nullopt;
  }
  const std::uint16_t current_offset = next_offset_;
  next_offset_ = Load16(page_, current_offset);
  return BtreeFreeblock{
      .offset = ByteCount{current_offset},
      .size = ByteCount{Load16(page_, static_cast<std::size_t>(current_offset) + 2U)},
  };
}

Result<BtreePageView> BtreePageView::Parse(ByteView page, PageNumber page_number,
                                           BtreePageGeometry geometry) {
  if (page.size() != geometry.page_size().value()) {
    return std::unexpected(Misuse("B-tree page buffer must match the configured page size"));
  }
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("B-tree page zero is invalid"));
  }
  if (!IsValidPageReference(page_number, geometry)) {
    return std::unexpected(Corruption("B-tree page number is reserved or out of range"));
  }

  const std::size_t header_offset = page_number == PageNumber{1} ? 100U : 0U;
  const auto decoded_type = DecodePageType(page[header_offset]);
  if (!decoded_type.has_value()) {
    return std::unexpected(decoded_type.error());
  }
  const BtreePageType type = *decoded_type;
  const bool is_leaf = type == BtreePageType::kLeafIndex || type == BtreePageType::kLeafTable;
  const bool is_table = type == BtreePageType::kInteriorTable || type == BtreePageType::kLeafTable;
  if (page_number == PageNumber{1} && !is_table) {
    return std::unexpected(Corruption("database page one must contain a table B-tree"));
  }

  const std::size_t header_size = is_leaf ? 8U : 12U;
  const std::size_t usable_size = geometry.usable_size().value();
  if (header_offset + header_size > usable_size) {
    return std::unexpected(Corruption("B-tree page header exceeds the usable region"));
  }

  const std::size_t cell_count = Load16(page, header_offset + 3U);
  const std::size_t maximum_cell_count = (geometry.page_size().value() - 8U) / 6U;
  if (cell_count > maximum_cell_count) {
    return std::unexpected(Corruption("B-tree page cell count exceeds SQLite's limit"));
  }

  const std::uint16_t raw_content_offset = Load16(page, header_offset + 5U);
  const std::size_t cell_content_offset = raw_content_offset == 0 ? 65536U : raw_content_offset;
  const std::size_t cell_pointer_array_offset = header_offset + header_size;
  const std::size_t cell_pointer_array_end = cell_pointer_array_offset + cell_count * 2U;
  if (cell_content_offset > usable_size || cell_pointer_array_end > cell_content_offset) {
    return std::unexpected(Corruption("B-tree cell pointer array crosses the cell content area"));
  }
  if (cell_count == 0 && cell_content_offset != usable_size) {
    return std::unexpected(Corruption("empty B-tree page has an invalid cell content offset"));
  }

  const auto fragmented_free_bytes = std::to_integer<std::uint8_t>(page[header_offset + 7U]);
  if (fragmented_free_bytes > 60U) {
    return std::unexpected(Corruption("B-tree page has too many fragmented free bytes"));
  }

  std::optional<PageNumber> rightmost_child;
  if (!is_leaf) {
    const auto decoded_child =
        DecodeRequiredPageReference(Load32(page, header_offset + 8U), geometry,
                                    "B-tree interior page has an invalid rightmost child");
    if (!decoded_child.has_value()) {
      return std::unexpected(decoded_child.error());
    }
    rightmost_child = *decoded_child;
  }

  return BtreePageView{
      page,
      page_number,
      geometry,
      type,
      cell_pointer_array_offset,
      cell_count,
      cell_content_offset,
      Load16(page, header_offset + 1U),
      fragmented_free_bytes,
      rightmost_child,
  };
}

Result<ByteCount> BtreePageView::cell_offset(std::size_t index) const {
  if (index >= cell_count_) {
    return std::unexpected(OutOfRange("B-tree cell index is out of range"));
  }
  const std::size_t offset = Load16(page_, cell_pointer_array_offset_ + index * 2U);
  const std::size_t minimum_offset = cell_content_offset_;
  const std::size_t minimum_cell_size = is_leaf() ? 4U : 5U;
  const std::size_t usable_size = geometry_.usable_size().value();
  if (offset < minimum_offset || offset > usable_size - minimum_cell_size) {
    return std::unexpected(Corruption("B-tree cell offset is outside the content area"));
  }
  return ByteCount{offset};
}

Result<BtreeCellView> BtreePageView::cell(std::size_t index) const {
  const auto decoded_offset = cell_offset(index);
  if (!decoded_offset.has_value()) {
    return std::unexpected(decoded_offset.error());
  }

  const std::size_t offset = decoded_offset->value();
  const ByteView cell_bytes = page_.subspan(offset, geometry_.usable_size().value() - offset);
  std::size_t cursor = 0;
  std::optional<PageNumber> left_child;
  std::optional<std::int64_t> rowid;

  if (!is_leaf()) {
    const auto child = DecodeRequiredPageReference(
        Load32(cell_bytes, 0), geometry_, "B-tree interior cell has an invalid left child");
    if (!child.has_value()) {
      return std::unexpected(child.error());
    }
    left_child = *child;
    cursor = 4;
  }

  if (type_ == BtreePageType::kInteriorTable) {
    const auto decoded_rowid = DecodeCellVarint(cell_bytes, cursor);
    if (!decoded_rowid.has_value()) {
      return std::unexpected(decoded_rowid.error());
    }
    cursor += decoded_rowid->bytes_consumed.value();
    return BtreeCellView{
        left_child,   std::bit_cast<std::int64_t>(decoded_rowid->value),
        ByteCount{0}, {},
        std::nullopt, ByteCount{cursor},
    };
  }

  const auto decoded_payload_size = DecodeCellVarint(cell_bytes, cursor);
  if (!decoded_payload_size.has_value()) {
    return std::unexpected(decoded_payload_size.error());
  }
  if (decoded_payload_size->value > kMaximumPayloadSize) {
    return std::unexpected(Corruption("B-tree cell payload size exceeds SQLite's limit"));
  }
  cursor += decoded_payload_size->bytes_consumed.value();
  const auto payload_size = static_cast<std::size_t>(decoded_payload_size->value);

  if (type_ == BtreePageType::kLeafTable) {
    const auto decoded_rowid = DecodeCellVarint(cell_bytes, cursor);
    if (!decoded_rowid.has_value()) {
      return std::unexpected(decoded_rowid.error());
    }
    rowid = std::bit_cast<std::int64_t>(decoded_rowid->value);
    cursor += decoded_rowid->bytes_consumed.value();
  }

  const std::size_t local_payload_size = LocalPayloadSize(geometry_, type_, payload_size);
  const bool has_overflow = local_payload_size < payload_size;
  const std::size_t overflow_pointer_size = has_overflow ? 4U : 0U;
  if (cursor > cell_bytes.size() || local_payload_size > cell_bytes.size() - cursor ||
      overflow_pointer_size > cell_bytes.size() - cursor - local_payload_size) {
    return std::unexpected(Corruption("B-tree cell body exceeds the usable page region"));
  }

  const ByteView local_payload = cell_bytes.subspan(cursor, local_payload_size);
  cursor += local_payload_size;
  std::optional<PageNumber> first_overflow_page;
  if (has_overflow) {
    const auto decoded_overflow = DecodeRequiredPageReference(
        Load32(cell_bytes, cursor), geometry_, "B-tree cell has an invalid first overflow page");
    if (!decoded_overflow.has_value()) {
      return std::unexpected(decoded_overflow.error());
    }
    first_overflow_page = *decoded_overflow;
    cursor += 4U;
  }

  if (is_leaf()) {
    cursor = std::max(cursor, std::size_t{4});
    if (cursor > cell_bytes.size()) {
      return std::unexpected(Corruption("B-tree leaf cell does not contain its four-byte minimum"));
    }
  }

  return BtreeCellView{
      left_child,        rowid, ByteCount{payload_size}, local_payload, first_overflow_page,
      ByteCount{cursor},
  };
}

Result<BtreePageFreeSpace> BtreePageView::AnalyzeFreeSpace() const {
  const std::size_t pointer_array_end = cell_pointer_array_offset_ + cell_count_ * 2U;
  const std::size_t usable_size = geometry_.usable_size().value();
  std::size_t free_bytes = cell_content_offset_ + static_cast<std::size_t>(fragmented_free_bytes_);
  std::uint16_t freeblock_offset = first_freeblock_;

  if (freeblock_offset != 0 && freeblock_offset < cell_content_offset_) {
    return std::unexpected(Corruption("first B-tree freeblock precedes the cell content area"));
  }

  while (freeblock_offset != 0) {
    const std::size_t offset = freeblock_offset;
    if (offset > usable_size - 4U) {
      return std::unexpected(Corruption("B-tree freeblock header exceeds the usable region"));
    }
    const std::uint16_t next = Load16(page_, offset);
    const std::size_t size = Load16(page_, offset + 2U);
    if (size < 4U) {
      return std::unexpected(Corruption("B-tree freeblock is smaller than four bytes"));
    }
    if (size > usable_size - offset) {
      return std::unexpected(Corruption("B-tree freeblock exceeds the usable region"));
    }
    free_bytes += size;
    if (next != 0 && static_cast<std::size_t>(next) < offset + size + 4U) {
      return std::unexpected(
          Corruption("B-tree freeblocks are unordered or insufficiently separated"));
    }
    freeblock_offset = next;
  }

  if (free_bytes > usable_size || free_bytes < pointer_array_end) {
    return std::unexpected(Corruption("B-tree free-space total is inconsistent"));
  }
  return BtreePageFreeSpace{
      page_,
      first_freeblock_,
      ByteCount{free_bytes - pointer_array_end},
  };
}

Result<OverflowPageView> OverflowPageView::Parse(ByteView page, BtreePageGeometry geometry) {
  if (page.size() != geometry.page_size().value()) {
    return std::unexpected(Misuse("overflow page buffer must match the configured page size"));
  }
  const auto next_page = DecodeOptionalPageReference(
      Load32(page, 0), geometry, "overflow page has an invalid next-page reference");
  if (!next_page.has_value()) {
    return std::unexpected(next_page.error());
  }
  return OverflowPageView{
      *next_page,
      page.subspan(4, geometry.overflow_payload_capacity().value()),
  };
}

Result<FreelistTrunkView> FreelistTrunkView::Parse(ByteView page, BtreePageGeometry geometry) {
  if (page.size() != geometry.page_size().value()) {
    return std::unexpected(Misuse("freelist trunk buffer must match the configured page size"));
  }
  const auto next_trunk = DecodeOptionalPageReference(
      Load32(page, 0), geometry, "freelist trunk has an invalid next-trunk reference");
  if (!next_trunk.has_value()) {
    return std::unexpected(next_trunk.error());
  }

  const std::uint32_t raw_leaf_count = Load32(page, 4);
  const std::size_t maximum_leaf_count = geometry.usable_size().value() / 4U - 2U;
  if (raw_leaf_count > maximum_leaf_count) {
    return std::unexpected(Corruption("freelist trunk leaf count exceeds the usable region"));
  }
  const std::size_t leaf_count = raw_leaf_count;
  for (std::size_t index = 0; index < leaf_count; ++index) {
    const auto leaf =
        DecodeRequiredPageReference(Load32(page, 8U + index * 4U), geometry,
                                    "freelist trunk has an invalid leaf-page reference");
    if (!leaf.has_value()) {
      return std::unexpected(leaf.error());
    }
  }

  return FreelistTrunkView{page, geometry, *next_trunk, leaf_count};
}

Result<PageNumber> FreelistTrunkView::leaf_page(std::size_t index) const {
  if (index >= leaf_count_) {
    return std::unexpected(OutOfRange("freelist trunk leaf index is out of range"));
  }
  return DecodeRequiredPageReference(Load32(page_, 8U + index * 4U), geometry_,
                                     "freelist trunk has an invalid leaf-page reference");
}

Result<PageNumber> PointerMapPageFor(PageNumber target, BtreePageGeometry geometry) {
  if (target.value() == 0) {
    return std::unexpected(Misuse("pointer-map target page zero is invalid"));
  }
  if (target == PageNumber{1} || target == geometry.locking_page() ||
      target.value() > kMaximumPageNumber) {
    return std::unexpected(OutOfRange("page has no pointer-map entry"));
  }

  const std::uint64_t pages_per_map =
      static_cast<std::uint64_t>(geometry.usable_size().value() / 5U + 1U);
  const std::uint64_t map_index = (static_cast<std::uint64_t>(target.value()) - 2U) / pages_per_map;
  std::uint64_t map_page = map_index * pages_per_map + 2U;
  if (map_page == geometry.locking_page().value()) {
    ++map_page;
  }
  if (map_page > kMaximumPageNumber) {
    return std::unexpected(OutOfRange("pointer-map page exceeds the page-number range"));
  }
  return PageNumber{static_cast<std::uint32_t>(map_page)};
}

Result<bool> IsPointerMapPage(PageNumber page_number, BtreePageGeometry geometry) {
  if (page_number.value() == 0) {
    return std::unexpected(Misuse("pointer-map page zero is invalid"));
  }
  if (page_number == PageNumber{1} || page_number == geometry.locking_page()) {
    return false;
  }
  if (page_number.value() > kMaximumPageNumber) {
    return std::unexpected(OutOfRange("pointer-map page number is out of range"));
  }
  const auto map_page = PointerMapPageFor(page_number, geometry);
  if (!map_page.has_value()) {
    return std::unexpected(map_page.error());
  }
  return *map_page == page_number;
}

Result<PointerMapView> PointerMapView::Parse(ByteView page, PageNumber page_number,
                                             BtreePageGeometry geometry) {
  if (page.size() != geometry.page_size().value()) {
    return std::unexpected(Misuse("pointer-map buffer must match the configured page size"));
  }
  const auto is_pointer_map = IsPointerMapPage(page_number, geometry);
  if (!is_pointer_map.has_value()) {
    return std::unexpected(is_pointer_map.error());
  }
  if (!*is_pointer_map) {
    return std::unexpected(Misuse("page number does not identify a pointer-map page"));
  }
  return PointerMapView{page, page_number, geometry};
}

Result<PointerMapEntry> PointerMapView::entry(PageNumber target) const {
  const auto map_page = PointerMapPageFor(target, geometry_);
  if (!map_page.has_value()) {
    return std::unexpected(map_page.error());
  }
  if (*map_page != page_number_ || target == page_number_) {
    return std::unexpected(OutOfRange("pointer-map target does not belong to this map page"));
  }

  const std::uint64_t relative_page =
      static_cast<std::uint64_t>(target.value()) - page_number_.value() - 1U;
  const std::uint64_t raw_offset = relative_page * 5U;
  if (raw_offset + 5U > geometry_.usable_size().value()) {
    return std::unexpected(OutOfRange("pointer-map target offset is out of range"));
  }
  const auto offset = static_cast<std::size_t>(raw_offset);
  const auto raw_type = std::to_integer<std::uint8_t>(page_[offset]);
  if (raw_type < static_cast<std::uint8_t>(PointerMapType::kRootPage) ||
      raw_type > static_cast<std::uint8_t>(PointerMapType::kBtreeChild)) {
    return std::unexpected(Corruption("pointer-map entry has an invalid type"));
  }

  const auto type = static_cast<PointerMapType>(raw_type);
  const std::uint32_t raw_parent = Load32(page_, offset + 1U);
  if (type == PointerMapType::kRootPage || type == PointerMapType::kFreePage) {
    if (raw_parent != 0) {
      return std::unexpected(Corruption("root and free pointer-map entries must have parent zero"));
    }
    return PointerMapEntry{.type = type, .parent = std::nullopt};
  }

  const auto parent = DecodeRequiredPageReference(
      raw_parent, geometry_, "pointer-map entry has an invalid parent reference");
  if (!parent.has_value()) {
    return std::unexpected(parent.error());
  }
  return PointerMapEntry{.type = type, .parent = *parent};
}

}  // namespace modern_sqlite
