#include "modern_sqlite/storage/btree/page.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {
namespace {

static_assert(std::is_trivially_copyable_v<BtreePageGeometry>);
static_assert(std::is_trivially_copyable_v<BtreePageView>);
static_assert(std::is_trivially_copyable_v<BtreeCellView>);

constexpr std::size_t kDefaultPageSize = 512;

void Write16(MutableByteView bytes, std::size_t offset, std::uint16_t value) {
  StoreBigEndian<std::uint16_t>(
      std::span<std::byte, sizeof(std::uint16_t)>{bytes.data() + offset, sizeof(std::uint16_t)},
      value);
}

void Write32(MutableByteView bytes, std::size_t offset, std::uint32_t value) {
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + offset, sizeof(std::uint32_t)},
      value);
}

void AppendVarint(std::vector<std::byte>& output, std::uint64_t value) {
  std::array<std::byte, 9> encoded{};
  const auto size = EncodeSqliteVarint(value, encoded);
  ASSERT_TRUE(size.has_value());
  output.insert(output.end(), encoded.begin(),
                encoded.begin() + static_cast<std::ptrdiff_t>(size->value()));
}

[[nodiscard]] BtreePageGeometry Geometry(std::size_t page_size = kDefaultPageSize,
                                         std::size_t usable_size = kDefaultPageSize) {
  const auto geometry = BtreePageGeometry::Create(ByteCount{page_size}, ByteCount{usable_size});
  if (!geometry.has_value()) {
    std::terminate();
  }
  return *geometry;
}

[[nodiscard]] std::size_t HeaderOffset(PageNumber page_number) noexcept {
  return page_number == PageNumber{1} ? 100 : 0;
}

[[nodiscard]] std::size_t LocalPayloadSize(BtreePageGeometry geometry, BtreePageType type,
                                           std::size_t payload_size) {
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

[[nodiscard]] std::vector<std::byte> TableInteriorCell(PageNumber left_child, std::int64_t rowid) {
  std::vector<std::byte> cell(4);
  Write32(cell, 0, left_child.value());
  AppendVarint(cell, std::bit_cast<std::uint64_t>(rowid));
  return cell;
}

struct PayloadCellSpec {
  std::size_t payload_size;
  std::int64_t rowid = 0;
  PageNumber left_child{3};
  PageNumber first_overflow{99};
};

[[nodiscard]] std::vector<std::byte> PayloadCell(BtreePageGeometry geometry, BtreePageType type,
                                                 const PayloadCellSpec& spec) {
  std::vector<std::byte> cell;
  const bool interior = type == BtreePageType::kInteriorIndex;
  const bool table_leaf = type == BtreePageType::kLeafTable;
  if (interior) {
    cell.resize(4);
    Write32(cell, 0, spec.left_child.value());
  }
  AppendVarint(cell, spec.payload_size);
  if (table_leaf) {
    AppendVarint(cell, std::bit_cast<std::uint64_t>(spec.rowid));
  }

  const std::size_t local_size = LocalPayloadSize(geometry, type, spec.payload_size);
  for (std::size_t index = 0; index < local_size; ++index) {
    cell.push_back(static_cast<std::byte>((index * 17U + 3U) & 0xffU));
  }
  if (local_size < spec.payload_size) {
    const std::size_t pointer_offset = cell.size();
    cell.resize(pointer_offset + 4);
    Write32(cell, pointer_offset, spec.first_overflow.value());
  }
  if (cell.size() < 4) {
    cell.resize(4);
  }
  return cell;
}

[[nodiscard]] std::vector<std::byte> MakeBtreePage(
    BtreePageGeometry geometry, PageNumber page_number, BtreePageType type,
    const std::vector<std::vector<std::byte>>& cells = {},
    PageNumber rightmost_child = PageNumber{4}) {
  std::vector<std::byte> page(geometry.page_size().value());
  const std::size_t header_offset = HeaderOffset(page_number);
  const bool leaf = type == BtreePageType::kLeafIndex || type == BtreePageType::kLeafTable;
  const std::size_t header_size = leaf ? 8 : 12;
  page[header_offset] = static_cast<std::byte>(type);
  Write16(page, header_offset + 1, 0);
  Write16(page, header_offset + 3, static_cast<std::uint16_t>(cells.size()));
  page[header_offset + 7] = std::byte{0};
  if (!leaf) {
    Write32(page, header_offset + 8, rightmost_child.value());
  }

  std::vector<std::uint16_t> offsets(cells.size());
  std::size_t content_offset = geometry.usable_size().value();
  for (std::size_t reverse_index = cells.size(); reverse_index > 0; --reverse_index) {
    const std::size_t index = reverse_index - 1U;
    content_offset -= cells[index].size();
    std::ranges::copy(cells[index], page.begin() + static_cast<std::ptrdiff_t>(content_offset));
    offsets[index] = static_cast<std::uint16_t>(content_offset);
  }
  Write16(page, header_offset + 5,
          content_offset == 65536 ? 0 : static_cast<std::uint16_t>(content_offset));

  const std::size_t pointer_offset = header_offset + header_size;
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    Write16(page, pointer_offset + index * 2U, offsets[index]);
  }
  return page;
}

void SetContentOffset(std::vector<std::byte>& page, PageNumber page_number,
                      std::size_t content_offset) {
  const std::size_t offset = HeaderOffset(page_number) + 5;
  Write16(page, offset, content_offset == 65536 ? 0 : static_cast<std::uint16_t>(content_offset));
}

void SetFirstFreeblock(std::vector<std::byte>& page, PageNumber page_number, std::uint16_t offset) {
  Write16(page, HeaderOffset(page_number) + 1, offset);
}

void SetFragments(std::vector<std::byte>& page, PageNumber page_number, std::uint8_t fragments) {
  page[HeaderOffset(page_number) + 7] = static_cast<std::byte>(fragments);
}

void WriteFreeblock(std::vector<std::byte>& page, std::size_t offset, std::uint16_t next,
                    std::uint16_t size) {
  Write16(page, offset, next);
  Write16(page, offset + 2, size);
}

[[nodiscard]] std::vector<std::byte> ReadFixture(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  const std::vector<char> characters{std::istreambuf_iterator<char>{stream},
                                     std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes(characters.size());
  if (!characters.empty()) {
    std::memcpy(bytes.data(), characters.data(), characters.size());
  }
  return bytes;
}

[[nodiscard]] ByteView PageAt(const std::vector<std::byte>& database, std::size_t page_size,
                              PageNumber page_number) {
  const std::size_t offset = static_cast<std::size_t>(page_number.value() - 1U) * page_size;
  return ByteView{database}.subspan(offset, page_size);
}

TEST(BtreePageGeometry, ComputesSQLitePayloadThresholdsAndLockingPage) {
  const auto geometry = BtreePageGeometry::Create(ByteCount{512}, ByteCount{512});

  ASSERT_TRUE(geometry.has_value());
  EXPECT_EQ(ByteCount{512}, geometry->page_size());
  EXPECT_EQ(ByteCount{512}, geometry->usable_size());
  EXPECT_EQ(ByteCount{39}, geometry->minimum_local_payload());
  EXPECT_EQ(ByteCount{102}, geometry->maximum_index_local_payload());
  EXPECT_EQ(ByteCount{477}, geometry->maximum_table_leaf_local_payload());
  EXPECT_EQ(ByteCount{508}, geometry->overflow_payload_capacity());
  EXPECT_EQ(PageNumber{2'097'153}, geometry->locking_page());

  const auto maximum_reserved = BtreePageGeometry::Create(ByteCount{1024}, ByteCount{769});
  ASSERT_TRUE(maximum_reserved.has_value());
  const auto excessive_reserved = BtreePageGeometry::Create(ByteCount{1024}, ByteCount{768});
  ASSERT_FALSE(excessive_reserved.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, excessive_reserved.error().code());
}

TEST(BtreePageGeometry, RejectsInvalidPageAndUsableSizes) {
  constexpr std::array<std::pair<std::size_t, std::size_t>, 6> kInvalid{{
      {256, 256},
      {1000, 1000},
      {131072, 131072},
      {512, 479},
      {512, 513},
      {512, 256},
  }};
  for (const auto& [page_size, usable_size] : kInvalid) {
    SCOPED_TRACE(page_size);
    SCOPED_TRACE(usable_size);
    const auto geometry = BtreePageGeometry::Create(ByteCount{page_size}, ByteCount{usable_size});
    ASSERT_FALSE(geometry.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, geometry.error().code());
  }
}

TEST(BtreePage, DecodesAllFourPageAndCellKinds) {
  const BtreePageGeometry geometry = Geometry();

  const auto interior_table_bytes =
      MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kInteriorTable,
                    {TableInteriorCell(PageNumber{7}, -9)}, PageNumber{8});
  const auto interior_table = BtreePageView::Parse(interior_table_bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(interior_table.has_value());
  EXPECT_EQ(BtreePageType::kInteriorTable, interior_table->type());
  EXPECT_FALSE(interior_table->is_leaf());
  EXPECT_TRUE(interior_table->is_table());
  EXPECT_EQ(PageNumber{8}, interior_table->rightmost_child());
  const auto table_separator = interior_table->cell(0);
  ASSERT_TRUE(table_separator.has_value());
  EXPECT_EQ(PageNumber{7}, table_separator->left_child());
  EXPECT_EQ(-9, table_separator->rowid());
  EXPECT_EQ(ByteCount{0}, table_separator->payload_size());
  EXPECT_TRUE(table_separator->local_payload().empty());
  EXPECT_FALSE(table_separator->first_overflow_page().has_value());

  const auto leaf_table_bytes = MakeBtreePage(
      geometry, PageNumber{3}, BtreePageType::kLeafTable,
      {PayloadCell(geometry, BtreePageType::kLeafTable, {.payload_size = 3, .rowid = -1})});
  const auto leaf_table = BtreePageView::Parse(leaf_table_bytes, PageNumber{3}, geometry);
  ASSERT_TRUE(leaf_table.has_value());
  EXPECT_TRUE(leaf_table->is_leaf());
  EXPECT_TRUE(leaf_table->is_table());
  const auto table_row = leaf_table->cell(0);
  ASSERT_TRUE(table_row.has_value());
  EXPECT_FALSE(table_row->left_child().has_value());
  EXPECT_EQ(-1, table_row->rowid());
  EXPECT_EQ(ByteCount{3}, table_row->payload_size());
  EXPECT_EQ(ByteCount{13}, table_row->encoded_size());

  const auto interior_index_bytes =
      MakeBtreePage(geometry, PageNumber{4}, BtreePageType::kInteriorIndex,
                    {PayloadCell(geometry, BtreePageType::kInteriorIndex,
                                 {.payload_size = 12, .left_child = PageNumber{9}})},
                    PageNumber{10});
  const auto interior_index = BtreePageView::Parse(interior_index_bytes, PageNumber{4}, geometry);
  ASSERT_TRUE(interior_index.has_value());
  EXPECT_FALSE(interior_index->is_leaf());
  EXPECT_FALSE(interior_index->is_table());
  const auto index_separator = interior_index->cell(0);
  ASSERT_TRUE(index_separator.has_value());
  EXPECT_EQ(PageNumber{9}, index_separator->left_child());
  EXPECT_FALSE(index_separator->rowid().has_value());
  EXPECT_EQ(ByteCount{12}, index_separator->payload_size());

  const auto leaf_index_bytes =
      MakeBtreePage(geometry, PageNumber{5}, BtreePageType::kLeafIndex,
                    {PayloadCell(geometry, BtreePageType::kLeafIndex, {.payload_size = 2})});
  const auto leaf_index = BtreePageView::Parse(leaf_index_bytes, PageNumber{5}, geometry);
  ASSERT_TRUE(leaf_index.has_value());
  EXPECT_TRUE(leaf_index->is_leaf());
  EXPECT_FALSE(leaf_index->is_table());
  const auto index_key = leaf_index->cell(0);
  ASSERT_TRUE(index_key.has_value());
  EXPECT_EQ(ByteCount{2}, index_key->payload_size());
  EXPECT_EQ(ByteCount{4}, index_key->encoded_size());
}

TEST(BtreePage, HandlesPageOneAndThe65536ContentOffsetEncoding) {
  const BtreePageGeometry ordinary_geometry = Geometry();
  const auto page_one_bytes =
      MakeBtreePage(ordinary_geometry, PageNumber{1}, BtreePageType::kLeafTable);
  const auto page_one = BtreePageView::Parse(page_one_bytes, PageNumber{1}, ordinary_geometry);
  ASSERT_TRUE(page_one.has_value());
  EXPECT_EQ(ByteCount{512}, page_one->cell_content_offset());

  const auto invalid_page_one_bytes =
      MakeBtreePage(ordinary_geometry, PageNumber{1}, BtreePageType::kLeafIndex);
  const auto invalid_page_one =
      BtreePageView::Parse(invalid_page_one_bytes, PageNumber{1}, ordinary_geometry);
  ASSERT_FALSE(invalid_page_one.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, invalid_page_one.error().code());

  const BtreePageGeometry large_geometry = Geometry(65536, 65536);
  const auto large_page_bytes =
      MakeBtreePage(large_geometry, PageNumber{2}, BtreePageType::kLeafTable);
  const auto large_page = BtreePageView::Parse(large_page_bytes, PageNumber{2}, large_geometry);
  ASSERT_TRUE(large_page.has_value());
  EXPECT_EQ(ByteCount{65536}, large_page->cell_content_offset());
}

TEST(BtreePage, EnforcesCellCountPointerExtentFragmentsAndPageReferences) {
  const BtreePageGeometry geometry = Geometry();
  std::vector<std::byte> maximum =
      MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kLeafTable);
  constexpr std::size_t kMaximumCells = (kDefaultPageSize - 8) / 6;
  Write16(maximum, 3, static_cast<std::uint16_t>(kMaximumCells));
  SetContentOffset(maximum, PageNumber{2}, 8 + kMaximumCells * 2);
  EXPECT_TRUE(BtreePageView::Parse(maximum, PageNumber{2}, geometry).has_value());

  std::vector<std::byte> too_many = maximum;
  Write16(too_many, 3, static_cast<std::uint16_t>(kMaximumCells + 1));
  const auto count_error = BtreePageView::Parse(too_many, PageNumber{2}, geometry);
  ASSERT_FALSE(count_error.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, count_error.error().code());

  std::vector<std::byte> crossed = maximum;
  SetContentOffset(crossed, PageNumber{2}, 8 + kMaximumCells * 2 - 1);
  EXPECT_FALSE(BtreePageView::Parse(crossed, PageNumber{2}, geometry).has_value());

  std::vector<std::byte> fragmented =
      MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kLeafTable);
  SetFragments(fragmented, PageNumber{2}, 61);
  EXPECT_FALSE(BtreePageView::Parse(fragmented, PageNumber{2}, geometry).has_value());

  const auto zero_page = BtreePageView::Parse(maximum, PageNumber{0}, geometry);
  ASSERT_FALSE(zero_page.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, zero_page.error().code());
  const auto locking_page = BtreePageView::Parse(maximum, geometry.locking_page(), geometry);
  ASSERT_FALSE(locking_page.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, locking_page.error().code());
  const auto oversized_page = BtreePageView::Parse(
      maximum, PageNumber{std::numeric_limits<std::uint32_t>::max()}, geometry);
  ASSERT_FALSE(oversized_page.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, oversized_page.error().code());
}

TEST(BtreePage, AcceptsHeaderBoundariesAndRejectsInvalidHeaderValues) {
  const BtreePageGeometry geometry = Geometry();
  std::vector<std::byte> page_bytes =
      MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kLeafTable);
  SetFragments(page_bytes, PageNumber{2}, 60);
  EXPECT_TRUE(BtreePageView::Parse(page_bytes, PageNumber{2}, geometry).has_value());

  std::vector<std::byte> zero_content_offset = page_bytes;
  Write16(zero_content_offset, 5, 0);
  EXPECT_FALSE(BtreePageView::Parse(zero_content_offset, PageNumber{2}, geometry).has_value());

  std::vector<std::byte> invalid_type = page_bytes;
  invalid_type[0] = std::byte{0x01};
  EXPECT_FALSE(BtreePageView::Parse(invalid_type, PageNumber{2}, geometry).has_value());

  std::vector<std::byte> invalid_rightmost =
      MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kInteriorTable);
  Write32(invalid_rightmost, 8, 0);
  EXPECT_FALSE(BtreePageView::Parse(invalid_rightmost, PageNumber{2}, geometry).has_value());

  const std::vector<std::byte> page_one =
      MakeBtreePage(geometry, PageNumber{1}, BtreePageType::kInteriorTable, {}, PageNumber{3});
  EXPECT_TRUE(BtreePageView::Parse(page_one, PageNumber{1}, geometry).has_value());
}

TEST(BtreeCell, MatchesSQLiteLocalPayloadBoundaryFormula) {
  const BtreePageGeometry geometry = Geometry();
  struct Case {
    BtreePageType type;
    std::size_t payload_size;
    std::size_t local_size;
  };
  constexpr std::array<Case, 10> kCases{{
      {.type = BtreePageType::kLeafIndex, .payload_size = 102, .local_size = 102},
      {.type = BtreePageType::kLeafIndex, .payload_size = 103, .local_size = 39},
      {.type = BtreePageType::kLeafIndex, .payload_size = 609, .local_size = 101},
      {.type = BtreePageType::kLeafIndex, .payload_size = 610, .local_size = 102},
      {.type = BtreePageType::kLeafIndex, .payload_size = 611, .local_size = 39},
      {.type = BtreePageType::kLeafTable, .payload_size = 477, .local_size = 477},
      {.type = BtreePageType::kLeafTable, .payload_size = 478, .local_size = 39},
      {.type = BtreePageType::kLeafTable, .payload_size = 984, .local_size = 476},
      {.type = BtreePageType::kLeafTable, .payload_size = 985, .local_size = 477},
      {.type = BtreePageType::kLeafTable, .payload_size = 986, .local_size = 39},
  }};

  for (const Case& test : kCases) {
    SCOPED_TRACE(test.payload_size);
    const auto page_bytes = MakeBtreePage(
        geometry, PageNumber{2}, test.type,
        {PayloadCell(geometry, test.type, {.payload_size = test.payload_size, .rowid = 17})});
    const auto page = BtreePageView::Parse(page_bytes, PageNumber{2}, geometry);
    ASSERT_TRUE(page.has_value());
    const auto cell = page->cell(0);
    ASSERT_TRUE(cell.has_value());
    EXPECT_EQ(ByteCount{test.payload_size}, cell->payload_size());
    EXPECT_EQ(test.local_size, cell->local_payload().size());
    EXPECT_EQ(test.local_size < test.payload_size, cell->first_overflow_page().has_value());
  }
}

TEST(BtreeCell, AcceptsSQLiteCompatibleOverlongVarintsAndMaximumPayload) {
  const BtreePageGeometry geometry = Geometry();
  std::vector<std::byte> overlong_rowid(4);
  Write32(overlong_rowid, 0, 7);
  overlong_rowid.push_back(std::byte{0x80});
  overlong_rowid.push_back(std::byte{0x01});
  const auto rowid_page_bytes =
      MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kInteriorTable, {overlong_rowid});
  const auto rowid_page = BtreePageView::Parse(rowid_page_bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(rowid_page.has_value());
  const auto rowid_cell = rowid_page->cell(0);
  ASSERT_TRUE(rowid_cell.has_value());
  EXPECT_EQ(1, rowid_cell->rowid());
  EXPECT_EQ(ByteCount{6}, rowid_cell->encoded_size());

  constexpr std::size_t kMaximumPayload = 0x7fffffffU;
  const auto payload_page_bytes = MakeBtreePage(
      geometry, PageNumber{2}, BtreePageType::kLeafIndex,
      {PayloadCell(geometry, BtreePageType::kLeafIndex, {.payload_size = kMaximumPayload})});
  const auto payload_page = BtreePageView::Parse(payload_page_bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(payload_page.has_value());
  const auto payload_cell = payload_page->cell(0);
  ASSERT_TRUE(payload_cell.has_value());
  EXPECT_EQ(ByteCount{kMaximumPayload}, payload_cell->payload_size());
  EXPECT_TRUE(payload_cell->first_overflow_page().has_value());
}

TEST(BtreeCell, RejectsMalformedOffsetsVarintsPayloadsAndReferences) {
  const BtreePageGeometry geometry = Geometry();
  std::vector<std::byte> page_bytes =
      MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kLeafIndex,
                    {PayloadCell(geometry, BtreePageType::kLeafIndex, {.payload_size = 3})});
  const std::size_t pointer_offset = 8;

  std::vector<std::byte> before_pointer_array = page_bytes;
  Write16(before_pointer_array, pointer_offset, 9);
  auto page = BtreePageView::Parse(before_pointer_array, PageNumber{2}, geometry);
  ASSERT_TRUE(page.has_value());
  EXPECT_FALSE(page->cell(0).has_value());

  std::vector<std::byte> past_usable = page_bytes;
  Write16(past_usable, pointer_offset, 509);
  page = BtreePageView::Parse(past_usable, PageNumber{2}, geometry);
  ASSERT_TRUE(page.has_value());
  EXPECT_FALSE(page->cell(0).has_value());

  std::vector<std::byte> truncated = page_bytes;
  Write16(truncated, pointer_offset, 508);
  std::ranges::fill(truncated.begin() + 508, truncated.end(), std::byte{0x80});
  page = BtreePageView::Parse(truncated, PageNumber{2}, geometry);
  ASSERT_TRUE(page.has_value());
  EXPECT_FALSE(page->cell(0).has_value());

  std::vector<std::byte> excessive_payload =
      PayloadCell(geometry, BtreePageType::kLeafIndex, {.payload_size = 1});
  excessive_payload.clear();
  AppendVarint(excessive_payload, std::uint64_t{1} << 31U);
  excessive_payload.resize(9);
  page_bytes =
      MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kLeafIndex, {excessive_payload});
  page = BtreePageView::Parse(page_bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(page.has_value());
  EXPECT_FALSE(page->cell(0).has_value());

  std::vector<std::byte> missing_overflow;
  AppendVarint(missing_overflow, 103);
  missing_overflow.resize(missing_overflow.size() + 39);
  page_bytes =
      MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kLeafIndex, {missing_overflow});
  page = BtreePageView::Parse(page_bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(page.has_value());
  EXPECT_FALSE(page->cell(0).has_value());

  page_bytes = MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kLeafIndex,
                             {PayloadCell(geometry, BtreePageType::kLeafIndex,
                                          {.payload_size = 103, .first_overflow = PageNumber{0}})});
  page = BtreePageView::Parse(page_bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(page.has_value());
  EXPECT_FALSE(page->cell(0).has_value());

  page_bytes = MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kInteriorTable,
                             {TableInteriorCell(PageNumber{0}, 1)});
  page = BtreePageView::Parse(page_bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(page.has_value());
  EXPECT_FALSE(page->cell(0).has_value());

  const auto valid_page_bytes = MakeBtreePage(geometry, PageNumber{2}, BtreePageType::kLeafTable);
  const auto valid_page = BtreePageView::Parse(valid_page_bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(valid_page.has_value());
  const auto missing_cell = valid_page->cell(0);
  ASSERT_FALSE(missing_cell.has_value());
  EXPECT_EQ(ErrorCode::kOutOfRange, missing_cell.error().code());
}

TEST(BtreeFreeSpace, ValidatesAndIteratesFreeblocksLazily) {
  const BtreePageGeometry geometry = Geometry();
  std::vector<std::byte> page_bytes = MakeBtreePage(
      geometry, PageNumber{2}, BtreePageType::kLeafTable,
      {PayloadCell(geometry, BtreePageType::kLeafTable, {.payload_size = 0, .rowid = 1})});
  SetContentOffset(page_bytes, PageNumber{2}, 400);
  SetFirstFreeblock(page_bytes, PageNumber{2}, 420);
  SetFragments(page_bytes, PageNumber{2}, 3);
  WriteFreeblock(page_bytes, 420, 450, 10);
  WriteFreeblock(page_bytes, 450, 0, 8);

  const auto page = BtreePageView::Parse(page_bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(page.has_value());
  const auto space = page->AnalyzeFreeSpace();
  ASSERT_TRUE(space.has_value());
  EXPECT_EQ(ByteCount{411}, space->total());

  auto freeblocks = space->freeblocks();
  const auto first = freeblocks.Next();
  const auto second = freeblocks.Next();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(ByteCount{420}, first->offset);
  EXPECT_EQ(ByteCount{10}, first->size);
  EXPECT_EQ(ByteCount{450}, second->offset);
  EXPECT_EQ(ByteCount{8}, second->size);
  EXPECT_FALSE(freeblocks.Next().has_value());
}

TEST(BtreeFreeSpace, RejectsMalformedFreeblockChainsAndTotals) {
  const BtreePageGeometry geometry = Geometry();
  const auto make_page = [&] {
    auto page = MakeBtreePage(
        geometry, PageNumber{2}, BtreePageType::kLeafTable,
        {PayloadCell(geometry, BtreePageType::kLeafTable, {.payload_size = 0, .rowid = 1})});
    SetContentOffset(page, PageNumber{2}, 400);
    return page;
  };
  const auto expect_corruption = [&](std::vector<std::byte> bytes) {
    const auto page = BtreePageView::Parse(bytes, PageNumber{2}, geometry);
    ASSERT_TRUE(page.has_value());
    const auto space = page->AnalyzeFreeSpace();
    ASSERT_FALSE(space.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, space.error().code());
  };

  std::vector<std::byte> before_content = make_page();
  SetFirstFreeblock(before_content, PageNumber{2}, 399);
  WriteFreeblock(before_content, 399, 0, 4);
  expect_corruption(std::move(before_content));

  std::vector<std::byte> too_small = make_page();
  SetFirstFreeblock(too_small, PageNumber{2}, 420);
  WriteFreeblock(too_small, 420, 0, 3);
  expect_corruption(std::move(too_small));

  std::vector<std::byte> overlapping = make_page();
  SetFirstFreeblock(overlapping, PageNumber{2}, 420);
  WriteFreeblock(overlapping, 420, 425, 10);
  WriteFreeblock(overlapping, 425, 0, 8);
  expect_corruption(std::move(overlapping));

  std::vector<std::byte> past_end = make_page();
  SetFirstFreeblock(past_end, PageNumber{2}, 500);
  WriteFreeblock(past_end, 500, 0, 13);
  expect_corruption(std::move(past_end));

  std::vector<std::byte> excessive_total = make_page();
  SetContentOffset(excessive_total, PageNumber{2}, 500);
  SetFirstFreeblock(excessive_total, PageNumber{2}, 500);
  SetFragments(excessive_total, PageNumber{2}, 20);
  WriteFreeblock(excessive_total, 500, 0, 12);
  expect_corruption(std::move(excessive_total));
}

TEST(OverflowPage, ExposesUsablePayloadAndValidatesNextPage) {
  const BtreePageGeometry geometry = Geometry(512, 500);
  std::vector<std::byte> bytes(512);
  Write32(bytes, 0, 9);
  bytes[4] = std::byte{0x42};
  bytes[499] = std::byte{0x7f};
  bytes[500] = std::byte{0x55};

  const auto page = OverflowPageView::Parse(bytes, geometry);

  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(PageNumber{9}, page->next_page());
  ASSERT_EQ(496U, page->payload().size());
  EXPECT_EQ(std::byte{0x42}, page->payload().front());
  EXPECT_EQ(std::byte{0x7f}, page->payload().back());

  Write32(bytes, 0, 0);
  const auto final_page = OverflowPageView::Parse(bytes, geometry);
  ASSERT_TRUE(final_page.has_value());
  EXPECT_FALSE(final_page->next_page().has_value());

  Write32(bytes, 0, geometry.locking_page().value());
  EXPECT_FALSE(OverflowPageView::Parse(bytes, geometry).has_value());
}

TEST(FreelistTrunk, DecodesMaximumOccupancyAndRejectsMalformedEntries) {
  const BtreePageGeometry geometry = Geometry();
  constexpr std::size_t kMaximumLeaves = 126;
  std::vector<std::byte> bytes(512);
  Write32(bytes, 0, 9);
  Write32(bytes, 4, static_cast<std::uint32_t>(kMaximumLeaves));
  for (std::size_t index = 0; index < kMaximumLeaves; ++index) {
    Write32(bytes, 8 + index * 4, static_cast<std::uint32_t>(100 + index));
  }

  const auto trunk = FreelistTrunkView::Parse(bytes, geometry);

  ASSERT_TRUE(trunk.has_value());
  EXPECT_EQ(PageNumber{9}, trunk->next_trunk());
  EXPECT_EQ(kMaximumLeaves, trunk->leaf_count());
  EXPECT_EQ(PageNumber{100}, trunk->leaf_page(0));
  EXPECT_EQ(PageNumber{225}, trunk->leaf_page(kMaximumLeaves - 1));
  const auto out_of_range = trunk->leaf_page(kMaximumLeaves);
  ASSERT_FALSE(out_of_range.has_value());
  EXPECT_EQ(ErrorCode::kOutOfRange, out_of_range.error().code());

  Write32(bytes, 4, static_cast<std::uint32_t>(kMaximumLeaves + 1));
  EXPECT_FALSE(FreelistTrunkView::Parse(bytes, geometry).has_value());

  Write32(bytes, 4, 1);
  Write32(bytes, 8, 0);
  EXPECT_FALSE(FreelistTrunkView::Parse(bytes, geometry).has_value());
}

TEST(PointerMap, ComputesPlacementIncludingLockingPageDisplacement) {
  const BtreePageGeometry geometry = Geometry();
  EXPECT_EQ(PageNumber{2}, PointerMapPageFor(PageNumber{3}, geometry));
  EXPECT_EQ(PageNumber{2}, PointerMapPageFor(PageNumber{104}, geometry));
  EXPECT_EQ(PageNumber{105}, PointerMapPageFor(PageNumber{105}, geometry));
  EXPECT_EQ(PageNumber{105}, PointerMapPageFor(PageNumber{106}, geometry));
  EXPECT_EQ(true, IsPointerMapPage(PageNumber{2}, geometry));
  EXPECT_EQ(true, IsPointerMapPage(PageNumber{105}, geometry));
  EXPECT_EQ(false, IsPointerMapPage(PageNumber{3}, geometry));

  const BtreePageGeometry displaced_geometry = Geometry(1024, 820);
  const PageNumber locking_page = displaced_geometry.locking_page();
  EXPECT_EQ(PageNumber{locking_page.value() + 1U},
            PointerMapPageFor(PageNumber{locking_page.value() + 1U}, displaced_geometry));
  EXPECT_EQ(true, IsPointerMapPage(PageNumber{locking_page.value() + 1U}, displaced_geometry));
  EXPECT_EQ(false, IsPointerMapPage(locking_page, displaced_geometry));
}

TEST(PointerMap, DecodesAllEntryTypesAndParentRules) {
  const BtreePageGeometry geometry = Geometry();
  std::vector<std::byte> bytes(512);
  const auto write_entry = [&](PageNumber target, std::byte type, PageNumber parent) {
    const std::size_t offset = static_cast<std::size_t>(target.value() - 3U) * 5U;
    bytes[offset] = type;
    Write32(bytes, offset + 1, parent.value());
  };
  write_entry(PageNumber{3}, std::byte{1}, PageNumber{0});
  write_entry(PageNumber{4}, std::byte{2}, PageNumber{0});
  write_entry(PageNumber{5}, std::byte{3}, PageNumber{17});
  write_entry(PageNumber{6}, std::byte{4}, PageNumber{5});
  write_entry(PageNumber{7}, std::byte{5}, PageNumber{3});

  const auto map = PointerMapView::Parse(bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(map.has_value());
  const auto root = map->entry(PageNumber{3});
  const auto free = map->entry(PageNumber{4});
  const auto first_overflow = map->entry(PageNumber{5});
  const auto later_overflow = map->entry(PageNumber{6});
  const auto child = map->entry(PageNumber{7});
  ASSERT_TRUE(root.has_value());
  ASSERT_TRUE(free.has_value());
  ASSERT_TRUE(first_overflow.has_value());
  ASSERT_TRUE(later_overflow.has_value());
  ASSERT_TRUE(child.has_value());
  EXPECT_EQ(PointerMapType::kRootPage, root->type);
  EXPECT_FALSE(root->parent.has_value());
  EXPECT_EQ(PointerMapType::kFreePage, free->type);
  EXPECT_FALSE(free->parent.has_value());
  EXPECT_EQ(PointerMapType::kFirstOverflow, first_overflow->type);
  EXPECT_EQ(PageNumber{17}, first_overflow->parent);
  EXPECT_EQ(PointerMapType::kLaterOverflow, later_overflow->type);
  EXPECT_EQ(PageNumber{5}, later_overflow->parent);
  EXPECT_EQ(PointerMapType::kBtreeChild, child->type);
  EXPECT_EQ(PageNumber{3}, child->parent);

  write_entry(PageNumber{3}, std::byte{0}, PageNumber{0});
  const auto invalid_type_map = PointerMapView::Parse(bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(invalid_type_map.has_value());
  EXPECT_FALSE(invalid_type_map->entry(PageNumber{3}).has_value());

  write_entry(PageNumber{3}, std::byte{1}, PageNumber{9});
  const auto invalid_root_map = PointerMapView::Parse(bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(invalid_root_map.has_value());
  EXPECT_FALSE(invalid_root_map->entry(PageNumber{3}).has_value());

  write_entry(PageNumber{3}, std::byte{3}, PageNumber{0});
  const auto invalid_parent_map = PointerMapView::Parse(bytes, PageNumber{2}, geometry);
  ASSERT_TRUE(invalid_parent_map.has_value());
  EXPECT_FALSE(invalid_parent_map->entry(PageNumber{3}).has_value());

  const auto page_one = map->entry(PageNumber{1});
  ASSERT_FALSE(page_one.has_value());
  EXPECT_EQ(ErrorCode::kOutOfRange, page_one.error().code());
  const auto map_itself = map->entry(PageNumber{2});
  ASSERT_FALSE(map_itself.has_value());
  EXPECT_EQ(ErrorCode::kOutOfRange, map_itself.error().code());
  const auto other_map = map->entry(PageNumber{106});
  ASSERT_FALSE(other_map.has_value());
  EXPECT_EQ(ErrorCode::kOutOfRange, other_map.error().code());
}

TEST(BtreePage, RejectsIncorrectBufferSizesAcrossPageKinds) {
  const BtreePageGeometry geometry = Geometry();
  const std::vector<std::byte> short_page(511);

  const auto btree = BtreePageView::Parse(short_page, PageNumber{2}, geometry);
  const auto overflow = OverflowPageView::Parse(short_page, geometry);
  const auto freelist = FreelistTrunkView::Parse(short_page, geometry);
  const auto pointer_map = PointerMapView::Parse(short_page, PageNumber{2}, geometry);

  ASSERT_FALSE(btree.has_value());
  ASSERT_FALSE(overflow.has_value());
  ASSERT_FALSE(freelist.has_value());
  ASSERT_FALSE(pointer_map.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, btree.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, overflow.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, freelist.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, pointer_map.error().code());
}

TEST(BtreePage, ReadsPinnedSQLite354CompatibilityFixtures) {
  const std::filesystem::path fixture_directory =
      std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path().parent_path() /
      "fixtures" / "btree_page";
  const std::vector<std::byte> btree_database =
      ReadFixture(fixture_directory / "sqlite-3.54.0-btree-pages.db");
  ASSERT_EQ(59U * 512U, btree_database.size());
  const BtreePageGeometry geometry = Geometry();

  const auto schema =
      BtreePageView::Parse(PageAt(btree_database, 512, PageNumber{1}), PageNumber{1}, geometry);
  const auto table_interior =
      BtreePageView::Parse(PageAt(btree_database, 512, PageNumber{3}), PageNumber{3}, geometry);
  const auto index_interior =
      BtreePageView::Parse(PageAt(btree_database, 512, PageNumber{4}), PageNumber{4}, geometry);
  const auto table_leaf =
      BtreePageView::Parse(PageAt(btree_database, 512, PageNumber{5}), PageNumber{5}, geometry);
  const auto index_leaf =
      BtreePageView::Parse(PageAt(btree_database, 512, PageNumber{7}), PageNumber{7}, geometry);
  ASSERT_TRUE(schema.has_value());
  ASSERT_TRUE(table_interior.has_value());
  ASSERT_TRUE(index_interior.has_value());
  ASSERT_TRUE(table_leaf.has_value());
  ASSERT_TRUE(index_leaf.has_value());
  EXPECT_EQ(BtreePageType::kLeafTable, schema->type());
  EXPECT_EQ(BtreePageType::kInteriorTable, table_interior->type());
  EXPECT_EQ(BtreePageType::kInteriorIndex, index_interior->type());
  EXPECT_EQ(BtreePageType::kLeafTable, table_leaf->type());
  EXPECT_EQ(BtreePageType::kLeafIndex, index_leaf->type());

  const auto overflow_leaf =
      BtreePageView::Parse(PageAt(btree_database, 512, PageNumber{59}), PageNumber{59}, geometry);
  ASSERT_TRUE(overflow_leaf.has_value());
  ASSERT_EQ(1U, overflow_leaf->cell_count());
  const auto overflow_cell = overflow_leaf->cell(0);
  ASSERT_TRUE(overflow_cell.has_value());
  EXPECT_EQ(1001, overflow_cell->rowid());
  EXPECT_EQ(ByteCount{3017}, overflow_cell->payload_size());
  EXPECT_EQ(477U, overflow_cell->local_payload().size());
  EXPECT_EQ(PageNumber{54}, overflow_cell->first_overflow_page());

  for (std::uint32_t page_number = 54; page_number <= 58; ++page_number) {
    const auto overflow =
        OverflowPageView::Parse(PageAt(btree_database, 512, PageNumber{page_number}), geometry);
    ASSERT_TRUE(overflow.has_value());
    if (page_number == 58) {
      EXPECT_FALSE(overflow->next_page().has_value());
    } else {
      EXPECT_EQ(PageNumber{page_number + 1U}, overflow->next_page());
    }
  }

  const auto pointer_map =
      PointerMapView::Parse(PageAt(btree_database, 512, PageNumber{2}), PageNumber{2}, geometry);
  ASSERT_TRUE(pointer_map.has_value());
  const auto root_entry = pointer_map->entry(PageNumber{3});
  const auto child_entry = pointer_map->entry(PageNumber{5});
  const auto overflow_entry = pointer_map->entry(PageNumber{54});
  ASSERT_TRUE(root_entry.has_value());
  ASSERT_TRUE(child_entry.has_value());
  ASSERT_TRUE(overflow_entry.has_value());
  EXPECT_EQ(PointerMapType::kRootPage, root_entry->type);
  EXPECT_EQ(PointerMapType::kBtreeChild, child_entry->type);
  EXPECT_EQ(PageNumber{3}, child_entry->parent);
  EXPECT_EQ(PointerMapType::kFirstOverflow, overflow_entry->type);
  EXPECT_EQ(PageNumber{59}, overflow_entry->parent);

  const std::vector<std::byte> freelist_database =
      ReadFixture(fixture_directory / "sqlite-3.54.0-freelist.db");
  ASSERT_EQ(329U * 512U, freelist_database.size());
  const auto first_trunk =
      FreelistTrunkView::Parse(PageAt(freelist_database, 512, PageNumber{213}), geometry);
  const auto second_trunk =
      FreelistTrunkView::Parse(PageAt(freelist_database, 512, PageNumber{126}), geometry);
  const auto final_trunk =
      FreelistTrunkView::Parse(PageAt(freelist_database, 512, PageNumber{4}), geometry);
  ASSERT_TRUE(first_trunk.has_value());
  ASSERT_TRUE(second_trunk.has_value());
  ASSERT_TRUE(final_trunk.has_value());
  EXPECT_EQ(PageNumber{126}, first_trunk->next_trunk());
  EXPECT_EQ(84U, first_trunk->leaf_count());
  EXPECT_EQ(PageNumber{4}, second_trunk->next_trunk());
  EXPECT_EQ(120U, second_trunk->leaf_count());
  EXPECT_FALSE(final_trunk->next_trunk().has_value());
  EXPECT_EQ(120U, final_trunk->leaf_count());
}

TEST(BtreePage, DeterministicMutationSmokeCoversEveryDecoder) {
  const BtreePageGeometry geometry = Geometry();
  const std::vector<std::byte> base = MakeBtreePage(
      geometry, PageNumber{2}, BtreePageType::kLeafTable,
      {PayloadCell(geometry, BtreePageType::kLeafTable, {.payload_size = 80, .rowid = 7})});
  std::uint32_t state = 0x9e3779b9U;
  for (std::size_t iteration = 0; iteration < 2000; ++iteration) {
    state = state * 1'664'525U + 1'013'904'223U;
    std::vector<std::byte> mutated = base;
    const std::size_t offset = state % mutated.size();
    state = state * 1'664'525U + 1'013'904'223U;
    mutated[offset] ^= static_cast<std::byte>((state >> 24U) | 1U);

    const auto page = BtreePageView::Parse(mutated, PageNumber{2}, geometry);
    if (page.has_value()) {
      for (std::size_t index = 0; index < page->cell_count(); ++index) {
        [[maybe_unused]] const auto decoded_cell = page->cell(index);
      }
      [[maybe_unused]] const auto free_space = page->AnalyzeFreeSpace();
    }
    [[maybe_unused]] const auto overflow = OverflowPageView::Parse(mutated, geometry);
    [[maybe_unused]] const auto freelist = FreelistTrunkView::Parse(mutated, geometry);
    const auto pointer_map = PointerMapView::Parse(mutated, PageNumber{2}, geometry);
    if (pointer_map.has_value()) {
      [[maybe_unused]] const auto entry = pointer_map->entry(PageNumber{3});
    }
  }
}

}  // namespace
}  // namespace modern_sqlite
