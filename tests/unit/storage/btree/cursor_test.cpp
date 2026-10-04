#include "modern_sqlite/storage/btree/cursor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/read_pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<TableBtreeCursor>);
static_assert(!std::is_copy_assignable_v<TableBtreeCursor>);
static_assert(std::is_nothrow_move_constructible_v<TableBtreeCursor>);
static_assert(std::is_nothrow_move_assignable_v<TableBtreeCursor>);
static_assert(!std::is_copy_constructible_v<IndexBtreeCursor>);
static_assert(!std::is_copy_assignable_v<IndexBtreeCursor>);
static_assert(std::is_nothrow_move_constructible_v<IndexBtreeCursor>);
static_assert(std::is_nothrow_move_assignable_v<IndexBtreeCursor>);
static_assert(kMaximumBtreeDepth == 20);

constexpr std::size_t kPageSize = 512;
constexpr std::uint32_t kSqliteVersion = 3'054'000;

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

template <typename T>
[[nodiscard]] T TakeOptional(std::optional<T> value, std::string_view message) {
  if (!value.has_value()) {
    throw std::runtime_error(std::string{message});
  }
  return std::move(value).value();
}

void RequireStatus(Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

[[nodiscard]] std::filesystem::path FixtureDirectory() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path().parent_path() /
         "fixtures" / "btree_read";
}

[[nodiscard]] std::filesystem::path FixtureDatabasePath() {
  return FixtureDirectory() / "sqlite-3.54.0-btree-read.db";
}

[[nodiscard]] std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("unable to open test fixture");
  }
  const std::vector<char> characters{std::istreambuf_iterator<char>{stream},
                                     std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes(characters.size());
  if (!characters.empty()) {
    std::memcpy(bytes.data(), characters.data(), characters.size());
  }
  return bytes;
}

[[nodiscard]] std::vector<std::int64_t> ReadExpectedRowids(std::string_view name) {
  std::ifstream stream(FixtureDirectory() / name);
  if (!stream) {
    throw std::runtime_error("unable to open expected rowid fixture");
  }
  std::vector<std::int64_t> rowids;
  std::int64_t rowid = 0;
  while (stream >> rowid) {
    rowids.push_back(rowid);
  }
  return rowids;
}

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

[[nodiscard]] std::uint32_t Read32(ByteView bytes, std::size_t offset) {
  return LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
      bytes.data() + offset, sizeof(std::uint32_t)});
}

void AppendVarint(std::vector<std::byte>& output, std::uint64_t value) {
  std::array<std::byte, 9> encoded{};
  const auto size = EncodeSqliteVarint(value, encoded);
  if (!size.has_value()) {
    throw std::runtime_error("failed to encode test varint");
  }
  output.insert(output.end(), encoded.begin(),
                encoded.begin() + static_cast<std::ptrdiff_t>(size->value()));
}

[[nodiscard]] ByteView PageAt(const std::vector<std::byte>& database, PageNumber page_number) {
  const std::size_t offset = static_cast<std::size_t>(page_number.value() - 1U) * kPageSize;
  return ByteView{database}.subspan(offset, kPageSize);
}

[[nodiscard]] MutableByteView MutablePageAt(std::vector<std::byte>& database,
                                            PageNumber page_number) {
  const std::size_t offset = static_cast<std::size_t>(page_number.value() - 1U) * kPageSize;
  return MutableByteView{database}.subspan(offset, kPageSize);
}

[[nodiscard]] BtreePageGeometry TestGeometry() {
  return TakeValue(BtreePageGeometry::Create(ByteCount{kPageSize}, ByteCount{kPageSize}));
}

void InitializeDatabaseHeader(std::vector<std::byte>& database, std::uint32_t page_count,
                              std::uint32_t schema_format = 4, std::uint32_t text_encoding = 1) {
  constexpr std::array<std::byte, 16> kMagic{
      std::byte{0x53}, std::byte{0x51}, std::byte{0x4c}, std::byte{0x69},
      std::byte{0x74}, std::byte{0x65}, std::byte{0x20}, std::byte{0x66},
      std::byte{0x6f}, std::byte{0x72}, std::byte{0x6d}, std::byte{0x61},
      std::byte{0x74}, std::byte{0x20}, std::byte{0x33}, std::byte{0x00},
  };
  std::ranges::copy(kMagic, database.begin());
  database[16] = std::byte{0x02};
  database[17] = std::byte{0x00};
  database[18] = std::byte{1};
  database[19] = std::byte{1};
  database[20] = std::byte{0};
  database[21] = std::byte{64};
  database[22] = std::byte{32};
  database[23] = std::byte{32};
  Write32(database, 24, 1);
  Write32(database, 28, page_count);
  Write32(database, 44, schema_format);
  Write32(database, 56, text_encoding);
  Write32(database, 92, 1);
  Write32(database, 96, kSqliteVersion);
}

void WriteBtreePage(std::vector<std::byte>& database, PageNumber page_number, BtreePageType type,
                    const std::vector<std::vector<std::byte>>& cells,
                    PageNumber rightmost_child = PageNumber{0}) {
  const MutableByteView page = MutablePageAt(database, page_number);
  const std::size_t header_offset = page_number == PageNumber{1} ? 100U : 0U;
  const bool leaf = type == BtreePageType::kLeafIndex || type == BtreePageType::kLeafTable;
  const std::size_t header_size = leaf ? 8U : 12U;
  page[header_offset] = static_cast<std::byte>(type);
  Write16(page, header_offset + 1U, 0);
  Write16(page, header_offset + 3U, static_cast<std::uint16_t>(cells.size()));
  page[header_offset + 7U] = std::byte{0};
  if (!leaf) {
    Write32(page, header_offset + 8U, rightmost_child.value());
  }

  std::size_t content_offset = kPageSize;
  std::vector<std::uint16_t> offsets(cells.size());
  for (std::size_t reverse = cells.size(); reverse > 0; --reverse) {
    const std::size_t index = reverse - 1U;
    content_offset -= cells[index].size();
    std::ranges::copy(cells[index], page.begin() + static_cast<std::ptrdiff_t>(content_offset));
    offsets[index] = static_cast<std::uint16_t>(content_offset);
  }
  Write16(page, header_offset + 5U, static_cast<std::uint16_t>(content_offset));
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    Write16(page, header_offset + header_size + index * 2U, offsets[index]);
  }
}

[[nodiscard]] std::vector<std::byte> MakeVirtualRootDatabase() {
  std::vector<std::byte> database(2U * kPageSize);
  InitializeDatabaseHeader(database, 2);
  WriteBtreePage(database, PageNumber{1}, BtreePageType::kInteriorTable, {}, PageNumber{2});

  std::vector<std::byte> cell;
  AppendVarint(cell, 0);
  AppendVarint(cell, 7);
  cell.resize(4);
  WriteBtreePage(database, PageNumber{2}, BtreePageType::kLeafTable, {cell});
  return database;
}

[[nodiscard]] std::size_t LocalTablePayloadSize(std::size_t payload_size) {
  constexpr std::size_t kMinimum = ((kPageSize - 12U) * 32U / 255U) - 23U;
  constexpr std::size_t kMaximum = kPageSize - 35U;
  constexpr std::size_t kCapacity = kPageSize - 4U;
  if (payload_size <= kMaximum) {
    return payload_size;
  }
  const std::size_t candidate = kMinimum + ((payload_size - kMinimum) % kCapacity);
  return candidate <= kMaximum ? candidate : kMinimum;
}

[[nodiscard]] std::vector<std::byte> MakeImpossiblePayloadDatabase() {
  constexpr std::size_t kPayloadSize = 0x7fffffffU;
  std::vector<std::byte> database(3U * kPageSize);
  InitializeDatabaseHeader(database, 3);

  std::vector<std::byte> cell;
  AppendVarint(cell, kPayloadSize);
  AppendVarint(cell, 1);
  cell.resize(cell.size() + LocalTablePayloadSize(kPayloadSize), std::byte{0});
  const std::size_t overflow_offset = cell.size();
  cell.resize(overflow_offset + 4U);
  Write32(cell, overflow_offset, 2);
  WriteBtreePage(database, PageNumber{1}, BtreePageType::kLeafTable, {cell});
  return database;
}

class TemporaryDatabase final {
 public:
  explicit TemporaryDatabase(std::vector<std::byte> bytes) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-btree-read-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(1)) + ".db");
    Rewrite(bytes);
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  // The error-code overloads are used for best-effort test cleanup.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~TemporaryDatabase() {
    std::error_code error;
    std::filesystem::remove(path_, error);
    std::filesystem::remove(path_.string() + "-journal", error);
    std::filesystem::remove(path_.string() + "-wal", error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  void Rewrite(ByteView bytes) {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("unable to create temporary database");
    }
    if (!bytes.empty()) {
      stream.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
      throw std::runtime_error("unable to write temporary database");
    }
  }

 private:
  std::filesystem::path path_;
};

struct LocatedTableCell {
  PageNumber page_number;
  std::size_t cell_index;
  BtreeCellView cell;
};

[[nodiscard]] LocatedTableCell LocateTableCell(const std::vector<std::byte>& database,
                                               PageNumber root_page, std::int64_t rowid) {
  const BtreePageGeometry geometry = TestGeometry();
  PageNumber page_number = root_page;
  for (std::size_t depth = 0; depth < kMaximumBtreeDepth; ++depth) {
    const BtreePageView page =
        TakeValue(BtreePageView::Parse(PageAt(database, page_number), page_number, geometry));
    if (!page.is_table()) {
      throw std::runtime_error("test fixture traversal entered an index page");
    }
    if (page.is_leaf()) {
      for (std::size_t index = 0; index < page.cell_count(); ++index) {
        const BtreeCellView cell = TakeValue(page.cell(index));
        if (cell.rowid() == rowid) {
          return LocatedTableCell{
              .page_number = page_number,
              .cell_index = index,
              .cell = cell,
          };
        }
      }
      throw std::runtime_error("test rowid was not found");
    }

    PageNumber child =
        TakeOptional(page.rightmost_child(), "interior test page is missing its rightmost child");
    for (std::size_t index = 0; index < page.cell_count(); ++index) {
      const BtreeCellView separator = TakeValue(page.cell(index));
      const std::int64_t separator_rowid =
          TakeOptional(separator.rowid(), "table separator is missing its rowid");
      if (rowid <= separator_rowid) {
        child = TakeOptional(separator.left_child(), "table separator is missing its left child");
        break;
      }
    }
    page_number = child;
  }
  throw std::runtime_error("test fixture exceeded the B-tree depth limit");
}

[[nodiscard]] std::vector<IndexColumnOrder> BinaryIndexColumns() {
  return {
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
}

[[nodiscard]] std::vector<IndexColumnOrder> NoCaseDescendingIndexColumns() {
  return {
      IndexColumnOrder{NoCaseCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
      IndexColumnOrder{BinaryCollation()},
  };
}

[[nodiscard]] std::vector<IndexColumnOrder> RTrimIndexColumns() {
  return {
      IndexColumnOrder{RTrimCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
}

[[nodiscard]] std::vector<IndexColumnOrder> MixedIndexColumns() {
  return {
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
}

[[nodiscard]] std::int64_t CurrentIndexRowid(IndexBtreeCursor& cursor) {
  const ByteBuffer encoded = TakeValue(cursor.CopyPayload());
  const RecordView record = TakeValue(RecordView::Parse(encoded.view()));
  const RecordFieldView field = TakeValue(record.field(record.field_count() - 1U));
  const auto rowid = field.integer_value();
  if (!rowid.has_value()) {
    throw std::runtime_error("index record has a non-integer rowid tail");
  }
  return rowid.value();
}

[[nodiscard]] std::string CurrentIndexText(IndexBtreeCursor& cursor) {
  const ByteBuffer encoded = TakeValue(cursor.CopyPayload());
  const RecordView record = TakeValue(RecordView::Parse(encoded.view()));
  const RecordFieldView field = TakeValue(record.field(0));
  const auto text = field.text_value();
  if (!text.has_value()) {
    throw std::runtime_error("index record has a non-text leading field");
  }
  return std::string{text.value().bytes()};
}

[[nodiscard]] std::vector<std::int64_t> CollectTableForward(TableBtreeCursor& cursor) {
  std::vector<std::int64_t> rowids;
  bool positioned = TakeValue(cursor.First());
  while (positioned) {
    rowids.push_back(TakeValue(cursor.rowid()));
    positioned = TakeValue(cursor.Next());
  }
  return rowids;
}

[[nodiscard]] std::vector<std::int64_t> CollectTableReverse(TableBtreeCursor& cursor) {
  std::vector<std::int64_t> rowids;
  bool positioned = TakeValue(cursor.Last());
  while (positioned) {
    rowids.push_back(TakeValue(cursor.rowid()));
    positioned = TakeValue(cursor.Previous());
  }
  return rowids;
}

[[nodiscard]] std::vector<std::int64_t> CollectIndexForward(IndexBtreeCursor& cursor) {
  std::vector<std::int64_t> rowids;
  bool positioned = TakeValue(cursor.First());
  while (positioned) {
    rowids.push_back(CurrentIndexRowid(cursor));
    positioned = TakeValue(cursor.Next());
  }
  return rowids;
}

[[nodiscard]] std::vector<std::int64_t> CollectIndexReverse(IndexBtreeCursor& cursor) {
  std::vector<std::int64_t> rowids;
  bool positioned = TakeValue(cursor.Last());
  while (positioned) {
    rowids.push_back(CurrentIndexRowid(cursor));
    positioned = TakeValue(cursor.Previous());
  }
  return rowids;
}

class BtreeCursorCompatibilityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto opened = ReadPager::Open(vfs_, FixtureDatabasePath().string(),
                                  ReadPagerOptions{.cache_capacity_pages = 256});
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    pager_ = std::move(*opened);
    const Status begun = pager_->BeginRead();
    ASSERT_TRUE(begun.has_value()) << begun.error().ToString();
  }

  void TearDown() override {
    if (pager_ != nullptr && pager_->in_read_transaction()) {
      const Status ended = pager_->EndRead();
      EXPECT_TRUE(ended.has_value()) << ended.error().ToString();
    }
  }

  PosixVfs vfs_;
  std::unique_ptr<ReadPager> pager_;
};

TEST_F(BtreeCursorCompatibilityTest, TraversesMultiLevelTableInBothDirections) {
  TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_, PageNumber{2}));
  const std::vector<std::int64_t> expected = ReadExpectedRowids("table-rowids.txt");

  EXPECT_EQ(CollectTableForward(cursor), expected);
  std::vector<std::int64_t> reversed = expected;
  std::ranges::reverse(reversed);
  EXPECT_EQ(CollectTableReverse(cursor), reversed);
}

TEST_F(BtreeCursorCompatibilityTest, ImplementsAllDirectionalTableSeekModes) {
  TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_, PageNumber{2}));

  struct SeekCase {
    std::int64_t target;
    BtreeSeekMode mode;
    std::optional<std::int64_t> expected;
  };
  constexpr std::array<SeekCase, 16> kCases{{
      {
          .target = std::numeric_limits<std::int64_t>::min(),
          .mode = BtreeSeekMode::kEqual,
          .expected = std::numeric_limits<std::int64_t>::min(),
      },
      {
          .target = std::numeric_limits<std::int64_t>::min(),
          .mode = BtreeSeekMode::kLess,
          .expected = std::nullopt,
      },
      {.target = -999, .mode = BtreeSeekMode::kEqual, .expected = std::nullopt},
      {.target = -999, .mode = BtreeSeekMode::kGreaterOrEqual, .expected = -1},
      {.target = -999, .mode = BtreeSeekMode::kGreater, .expected = -1},
      {.target = -999, .mode = BtreeSeekMode::kLessOrEqual, .expected = -1000},
      {.target = -999, .mode = BtreeSeekMode::kLess, .expected = -1000},
      {.target = 5, .mode = BtreeSeekMode::kEqual, .expected = 5},
      {.target = 5, .mode = BtreeSeekMode::kGreater, .expected = 6},
      {.target = 5, .mode = BtreeSeekMode::kLess, .expected = 4},
      {
          .target = 161,
          .mode = BtreeSeekMode::kGreaterOrEqual,
          .expected = 2147483648LL,
      },
      {.target = 161, .mode = BtreeSeekMode::kLessOrEqual, .expected = 160},
      {
          .target = std::numeric_limits<std::int64_t>::max(),
          .mode = BtreeSeekMode::kGreater,
          .expected = std::nullopt,
      },
      {
          .target = std::numeric_limits<std::int64_t>::max(),
          .mode = BtreeSeekMode::kLessOrEqual,
          .expected = std::numeric_limits<std::int64_t>::max(),
      },
      {.target = 0, .mode = BtreeSeekMode::kGreaterOrEqual, .expected = 0},
      {.target = 0, .mode = BtreeSeekMode::kLessOrEqual, .expected = 0},
  }};

  for (const SeekCase& test : kCases) {
    SCOPED_TRACE(test.target);
    SCOPED_TRACE(static_cast<int>(test.mode));
    const bool found = TakeValue(cursor.Seek(test.target, test.mode));
    ASSERT_EQ(test.expected.has_value(), found);
    EXPECT_EQ(found, cursor.valid());
    if (found) {
      EXPECT_EQ(TakeOptional(test.expected, "seek case is missing its expected rowid"),
                TakeValue(cursor.rowid()));
    }
  }

  const auto invalid_mode =
      cursor.Seek(1, static_cast<BtreeSeekMode>(std::numeric_limits<std::uint8_t>::max()));
  ASSERT_FALSE(invalid_mode.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_mode.error().code());
}

TEST_F(BtreeCursorCompatibilityTest, ReadsLocalAndOverflowTablePayloads) {
  TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_, PageNumber{2}));

  ASSERT_TRUE(TakeValue(cursor.Seek(1, BtreeSeekMode::kEqual)));
  const BtreePayloadView local = TakeValue(cursor.payload());
  EXPECT_TRUE(local.is_fully_local());
  EXPECT_EQ(local.size().value(), local.local_bytes().size());

  ASSERT_TRUE(TakeValue(cursor.Seek(44, BtreeSeekMode::kEqual)));
  const BtreePayloadView overflow = TakeValue(cursor.payload());
  ASSERT_FALSE(overflow.is_fully_local());
  ASSERT_GT(overflow.local_bytes().size(), 8U);
  ASSERT_GT(overflow.size().value(), 3000U);

  const ByteBuffer complete = TakeValue(cursor.CopyPayload());
  const RecordView record = TakeValue(RecordView::Parse(complete.view()));
  const RecordFieldView blob = TakeValue(record.field(record.field_count() - 1U));
  ASSERT_TRUE(blob.blob_value().has_value());
  EXPECT_EQ(3000U, blob.blob_value()->size());
  EXPECT_TRUE(std::ranges::all_of(*blob.blob_value(),
                                  [](std::byte value) { return value == std::byte{0}; }));

  const std::size_t offset = overflow.local_bytes().size() - 8U;
  std::array<std::byte, 32> crossing{};
  RequireStatus(cursor.ReadPayload(ByteOffset{offset}, crossing));
  EXPECT_TRUE(std::ranges::equal(crossing, complete.view().subspan(offset, crossing.size())));

  std::array<std::byte, 1> unchanged{std::byte{0x7f}};
  const Status out_of_range = cursor.ReadPayload(ByteOffset{overflow.size().value()}, unchanged);
  ASSERT_FALSE(out_of_range.has_value());
  EXPECT_EQ(ErrorCode::kOutOfRange, out_of_range.error().code());
  EXPECT_EQ(std::byte{0x7f}, unchanged[0]);

  RequireStatus(cursor.ReadPayload(ByteOffset{overflow.size().value()}, MutableByteView{}));
}

TEST_F(BtreeCursorCompatibilityTest, TraversesEveryIndexIncludingInteriorEntries) {
  struct IndexSpec {
    PageNumber root;
    std::string_view expected_file;
    std::vector<IndexColumnOrder> (*columns)();
  };
  constexpr std::array<IndexSpec, 5> kIndexes{{
      {
          .root = PageNumber{76},
          .expected_file = "idx-binary-rowids.txt",
          .columns = BinaryIndexColumns,
      },
      {
          .root = PageNumber{82},
          .expected_file = "idx-nocase-desc-rowids.txt",
          .columns = NoCaseDescendingIndexColumns,
      },
      {
          .root = PageNumber{89},
          .expected_file = "idx-rtrim-rowids.txt",
          .columns = RTrimIndexColumns,
      },
      {
          .root = PageNumber{96},
          .expected_file = "idx-mixed-rowids.txt",
          .columns = MixedIndexColumns,
      },
      {
          .root = PageNumber{102},
          .expected_file = "idx-overflow-rowids.txt",
          .columns = BinaryIndexColumns,
      },
  }};

  for (const IndexSpec& index : kIndexes) {
    SCOPED_TRACE(index.expected_file);
    const std::vector<IndexColumnOrder> columns = index.columns();
    IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager_, index.root, columns));
    const std::vector<std::int64_t> expected = ReadExpectedRowids(index.expected_file);
    ASSERT_EQ(166U, expected.size());

    EXPECT_EQ(CollectIndexForward(cursor), expected);
    std::vector<std::int64_t> reversed = expected;
    std::ranges::reverse(reversed);
    EXPECT_EQ(CollectIndexReverse(cursor), reversed);
  }
}

void ExpectTextSeek(IndexBtreeCursor& cursor, std::string key, BtreeSeekMode mode,
                    std::optional<std::int64_t> expected) {
  std::array<SqlValue, 1> search{SqlValue::Text(std::move(key))};
  const bool found = TakeValue(cursor.Seek(search, mode));
  ASSERT_EQ(expected.has_value(), found);
  if (found) {
    EXPECT_EQ(TakeOptional(expected, "text seek is missing its expected rowid"),
              CurrentIndexRowid(cursor));
  }
}

TEST_F(BtreeCursorCompatibilityTest, SeeksDuplicatePrefixesWithIndexOrderingMetadata) {
  {
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager_, PageNumber{76}, columns));
    ExpectTextSeek(cursor, "bin-05", BtreeSeekMode::kGreaterOrEqual, 5);
    ExpectTextSeek(cursor, "bin-05", BtreeSeekMode::kGreater, 6);
    ExpectTextSeek(cursor, "bin-05", BtreeSeekMode::kLessOrEqual, 148);
    ExpectTextSeek(cursor, "bin-05", BtreeSeekMode::kLess, 160);

    std::array<SqlValue, 1> equal{SqlValue::Text("bin-05")};
    ASSERT_TRUE(TakeValue(cursor.Seek(equal, BtreeSeekMode::kEqual)));
    EXPECT_EQ("bin-05", CurrentIndexText(cursor));
  }

  {
    const std::vector<IndexColumnOrder> columns = NoCaseDescendingIndexColumns();
    IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager_, PageNumber{82}, columns));
    ExpectTextSeek(cursor, "cAsE-03", BtreeSeekMode::kGreaterOrEqual, 3);
    ExpectTextSeek(cursor, "cAsE-03", BtreeSeekMode::kGreater, 2);
    ExpectTextSeek(cursor, "cAsE-03", BtreeSeekMode::kLessOrEqual, 157);
    ExpectTextSeek(cursor, "cAsE-03", BtreeSeekMode::kLess, 158);
  }

  {
    const std::vector<IndexColumnOrder> columns = RTrimIndexColumns();
    IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager_, PageNumber{89}, columns));
    ExpectTextSeek(cursor, "pad-04", BtreeSeekMode::kGreaterOrEqual, 4);
    ExpectTextSeek(cursor, "pad-04", BtreeSeekMode::kGreater, 5);
    ExpectTextSeek(cursor, "pad-04", BtreeSeekMode::kLessOrEqual, 157);
    ExpectTextSeek(cursor, "pad-04", BtreeSeekMode::kLess, 156);
  }
}

TEST_F(BtreeCursorCompatibilityTest, SeeksDescendingNullsAndOverflowBackedIndexKeys) {
  {
    const std::vector<IndexColumnOrder> columns = MixedIndexColumns();
    IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager_, PageNumber{96}, columns));

    std::array<SqlValue, 1> three{SqlValue::Integer(3)};
    ASSERT_TRUE(TakeValue(cursor.Seek(three, BtreeSeekMode::kGreaterOrEqual)));
    EXPECT_EQ(13, CurrentIndexRowid(cursor));
    ASSERT_TRUE(TakeValue(cursor.Seek(three, BtreeSeekMode::kGreater)));
    EXPECT_EQ(52, CurrentIndexRowid(cursor));
    ASSERT_TRUE(TakeValue(cursor.Seek(three, BtreeSeekMode::kLessOrEqual)));
    EXPECT_EQ(103, CurrentIndexRowid(cursor));
    ASSERT_TRUE(TakeValue(cursor.Seek(three, BtreeSeekMode::kLess)));
    EXPECT_EQ(129, CurrentIndexRowid(cursor));

    std::array<SqlValue, 1> null_key{};
    ASSERT_TRUE(TakeValue(cursor.Seek(null_key, BtreeSeekMode::kGreaterOrEqual)));
    EXPECT_EQ(143, CurrentIndexRowid(cursor));
    ASSERT_TRUE(TakeValue(cursor.Seek(null_key, BtreeSeekMode::kLessOrEqual)));
    EXPECT_EQ(std::numeric_limits<std::int64_t>::min(), CurrentIndexRowid(cursor));
    ASSERT_TRUE(TakeValue(cursor.Seek(null_key, BtreeSeekMode::kLess)));
    EXPECT_EQ(-1000, CurrentIndexRowid(cursor));
    EXPECT_FALSE(TakeValue(cursor.Seek(null_key, BtreeSeekMode::kGreater)));
  }

  {
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager_, PageNumber{102}, columns));
    const std::string overflow_key(3000, 'm');
    ExpectTextSeek(cursor, overflow_key, BtreeSeekMode::kGreaterOrEqual, 42);
    ExpectTextSeek(cursor, overflow_key, BtreeSeekMode::kGreater, 43);
    ExpectTextSeek(cursor, overflow_key, BtreeSeekMode::kLessOrEqual, 42);
    ExpectTextSeek(cursor, overflow_key, BtreeSeekMode::kLess, std::nullopt);
  }
}

TEST_F(BtreeCursorCompatibilityTest, EnforcesTreeKindsMetadataPinsAndMovedFromState) {
  {
    const auto wrong_table = TableBtreeCursor::Open(*pager_, PageNumber{76});
    ASSERT_FALSE(wrong_table.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, wrong_table.error().code());
  }
  {
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    const auto wrong_index = IndexBtreeCursor::Open(*pager_, PageNumber{2}, columns);
    ASSERT_FALSE(wrong_index.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, wrong_index.error().code());
  }
  {
    const auto zero_root = TableBtreeCursor::Open(*pager_, PageNumber{0});
    ASSERT_FALSE(zero_root.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, zero_root.error().code());
  }
  {
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    const auto zero_root = IndexBtreeCursor::Open(*pager_, PageNumber{0}, columns);
    ASSERT_FALSE(zero_root.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, zero_root.error().code());
  }
  {
    const auto empty_metadata =
        IndexBtreeCursor::Open(*pager_, PageNumber{76}, std::span<const IndexColumnOrder>{});
    ASSERT_FALSE(empty_metadata.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, empty_metadata.error().code());
  }

  TableBtreeCursor original = TakeValue(TableBtreeCursor::Open(*pager_, PageNumber{2}));
  TableBtreeCursor moved = std::move(original);
  // Moved-from behavior is part of the public cursor contract.
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(original.valid());
  const auto moved_from = original.First();
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(moved_from.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, moved_from.error().code());
  EXPECT_TRUE(TakeValue(moved.First()));

  const Status pinned_end = pager_->EndRead();
  ASSERT_FALSE(pinned_end.has_value());
  EXPECT_EQ(ErrorCode::kBusy, pinned_end.error().code());
}

TEST_F(BtreeCursorCompatibilityTest, EnforcesInvalidPositionAndSeekInputTransitions) {
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_, PageNumber{2}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_FALSE(TakeValue(cursor.Previous()));
    EXPECT_FALSE(cursor.valid());

    const auto repeated = cursor.Previous();
    ASSERT_FALSE(repeated.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, repeated.error().code());
    const auto invalid_rowid = cursor.rowid();
    ASSERT_FALSE(invalid_rowid.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, invalid_rowid.error().code());
    EXPECT_TRUE(TakeValue(cursor.Last()));
  }

  {
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager_, PageNumber{76}, columns));
    ASSERT_TRUE(TakeValue(cursor.First()));
    const auto empty_key = cursor.Seek(std::span<const SqlValue>{}, BtreeSeekMode::kEqual);
    ASSERT_FALSE(empty_key.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, empty_key.error().code());
    EXPECT_FALSE(cursor.valid());

    std::array<SqlValue, 3> long_key{
        SqlValue::Text("bin-00"),
        SqlValue::Integer(1),
        SqlValue::Integer(2),
    };
    const auto excessive_key = cursor.Seek(long_key, BtreeSeekMode::kGreaterOrEqual);
    ASSERT_FALSE(excessive_key.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, excessive_key.error().code());
    EXPECT_FALSE(cursor.valid());
  }
}

TEST(BtreeCursor, SupportsEmptyDatabasesVirtualRootsAndSnapshotInvalidation) {
  {
    TemporaryDatabase empty(std::vector<std::byte>{});
    PosixVfs vfs;
    std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, empty.path().string()));
    RequireStatus(pager->BeginRead());
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    EXPECT_FALSE(TakeValue(cursor.First()));
    EXPECT_FALSE(cursor.valid());

    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    const auto index = IndexBtreeCursor::Open(*pager, PageNumber{1}, columns);
    ASSERT_FALSE(index.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, index.error().code());
    const auto zero_index = IndexBtreeCursor::Open(*pager, PageNumber{0}, columns);
    ASSERT_FALSE(zero_index.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, zero_index.error().code());

    RequireStatus(pager->EndRead());
    empty.Rewrite(ReadBytes(FixtureDatabasePath()));
    RequireStatus(pager->BeginRead());
    const auto changed = cursor.First();
    ASSERT_FALSE(changed.has_value());
    EXPECT_EQ(ErrorCode::kSchemaChanged, changed.error().code());
    RequireStatus(pager->EndRead());
  }

  {
    const TemporaryDatabase database(MakeVirtualRootDatabase());
    PosixVfs vfs;
    std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, database.path().string()));
    RequireStatus(pager->BeginRead());
    {
      TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
      ASSERT_TRUE(TakeValue(cursor.First()));
      EXPECT_EQ(7, TakeValue(cursor.rowid()));
      ASSERT_TRUE(TakeValue(cursor.Seek(7, BtreeSeekMode::kEqual)));
      EXPECT_EQ(7, TakeValue(cursor.rowid()));
    }
    RequireStatus(pager->EndRead());
  }
}

TEST(BtreeCursor, RejectsInvalidChildLinksEmptyChildrenAndExcessiveDepth) {
  const std::vector<std::byte> fixture = ReadBytes(FixtureDatabasePath());
  const BtreePageGeometry geometry = TestGeometry();
  const BtreePageView root =
      TakeValue(BtreePageView::Parse(PageAt(fixture, PageNumber{2}), PageNumber{2}, geometry));
  const std::size_t first_cell_offset = TakeValue(root.cell_offset(0)).value();
  const PageNumber original_child = TakeOptional(TakeValue(root.cell(0)).left_child(),
                                                 "table root cell is missing its left child");

  const auto expect_corruption = [](std::vector<std::byte> bytes) {
    const TemporaryDatabase database(std::move(bytes));
    PosixVfs vfs;
    std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, database.path().string()));
    RequireStatus(pager->BeginRead());
    {
      TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{2}));
      const auto first = cursor.First();
      ASSERT_FALSE(first.has_value());
      EXPECT_EQ(ErrorCode::kCorruption, first.error().code());
      EXPECT_FALSE(cursor.valid());
    }
    RequireStatus(pager->EndRead());
  };

  {
    std::vector<std::byte> bytes = fixture;
    Write32(MutablePageAt(bytes, PageNumber{2}), first_cell_offset, 121);
    expect_corruption(std::move(bytes));
  }
  {
    std::vector<std::byte> bytes = fixture;
    Write32(MutablePageAt(bytes, PageNumber{2}), first_cell_offset, 76);
    expect_corruption(std::move(bytes));
  }
  {
    std::vector<std::byte> bytes = fixture;
    Write32(MutablePageAt(bytes, PageNumber{2}), first_cell_offset, 2);
    expect_corruption(std::move(bytes));
  }
  {
    std::vector<std::byte> bytes = fixture;
    const MutableByteView child = MutablePageAt(bytes, original_child);
    child[0] = static_cast<std::byte>(BtreePageType::kLeafTable);
    Write16(child, 1, 0);
    Write16(child, 3, 0);
    Write16(child, 5, kPageSize);
    child[7] = std::byte{0};
    expect_corruption(std::move(bytes));
  }
}

TEST(BtreeCursor, HandlesOverflowTerminationAndUnusedFinalLinksLikeSQLite) {
  const std::vector<std::byte> fixture = ReadBytes(FixtureDatabasePath());
  const LocatedTableCell located = LocateTableCell(fixture, PageNumber{2}, 44);
  const PageNumber first_overflow = TakeOptional(
      located.cell.first_overflow_page(), "overflow test cell is missing its first overflow page");

  {
    std::vector<std::byte> bytes = fixture;
    Write32(MutablePageAt(bytes, first_overflow), 0, 0);
    const TemporaryDatabase database(std::move(bytes));
    PosixVfs vfs;
    std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, database.path().string()));
    RequireStatus(pager->BeginRead());
    {
      TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{2}));
      ASSERT_TRUE(TakeValue(cursor.Seek(44, BtreeSeekMode::kEqual)));
      const auto copied = cursor.CopyPayload();
      ASSERT_FALSE(copied.has_value());
      EXPECT_EQ(ErrorCode::kCorruption, copied.error().code());
      EXPECT_TRUE(cursor.valid());
    }
    RequireStatus(pager->EndRead());
  }

  {
    std::vector<std::byte> bytes = fixture;
    std::size_t remaining =
        located.cell.payload_size().value() - located.cell.local_payload().size();
    PageNumber page_number = first_overflow;
    while (remaining > TestGeometry().overflow_payload_capacity().value()) {
      remaining -= TestGeometry().overflow_payload_capacity().value();
      page_number = PageNumber{Read32(PageAt(bytes, page_number), 0)};
    }
    Write32(MutablePageAt(bytes, page_number), 0, 0xffffffffU);

    const TemporaryDatabase database(std::move(bytes));
    PosixVfs vfs;
    std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, database.path().string()));
    RequireStatus(pager->BeginRead());
    {
      TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{2}));
      ASSERT_TRUE(TakeValue(cursor.Seek(44, BtreeSeekMode::kEqual)));
      const ByteBuffer copied = TakeValue(cursor.CopyPayload());
      EXPECT_EQ(located.cell.payload_size(), copied.size());
    }
    RequireStatus(pager->EndRead());
  }
}

TEST(BtreeCursor, RejectsMalformedIndexRecordsAndUnsupportedHeaderPolicy) {
  const std::vector<std::byte> fixture = ReadBytes(FixtureDatabasePath());
  const BtreePageGeometry geometry = TestGeometry();

  {
    std::vector<std::byte> bytes = fixture;
    const BtreePageView root =
        TakeValue(BtreePageView::Parse(PageAt(bytes, PageNumber{76}), PageNumber{76}, geometry));
    const MutableByteView root_bytes = MutablePageAt(bytes, PageNumber{76});
    for (std::size_t index = 0; index < root.cell_count(); ++index) {
      const BtreeCellView cell = TakeValue(root.cell(index));
      const std::size_t payload_offset = static_cast<std::size_t>(
          cell.local_payload().data() - PageAt(bytes, PageNumber{76}).data());
      root_bytes[payload_offset] = std::byte{0};
    }

    const TemporaryDatabase database(std::move(bytes));
    PosixVfs vfs;
    std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, database.path().string()));
    RequireStatus(pager->BeginRead());
    {
      const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
      IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager, PageNumber{76}, columns));
      std::array<SqlValue, 1> key{SqlValue::Text("bin-05")};
      const auto seek = cursor.Seek(key, BtreeSeekMode::kGreaterOrEqual);
      ASSERT_FALSE(seek.has_value());
      EXPECT_EQ(ErrorCode::kCorruption, seek.error().code());
      EXPECT_FALSE(cursor.valid());
    }
    RequireStatus(pager->EndRead());
  }

  const auto expect_open_error = [&fixture](std::size_t offset, std::uint32_t value,
                                            ErrorCode expected) {
    std::vector<std::byte> bytes = fixture;
    Write32(bytes, offset, value);
    const TemporaryDatabase database(std::move(bytes));
    PosixVfs vfs;
    std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, database.path().string()));
    RequireStatus(pager->BeginRead());
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    const auto cursor = IndexBtreeCursor::Open(*pager, PageNumber{76}, columns);
    ASSERT_FALSE(cursor.has_value());
    EXPECT_EQ(expected, cursor.error().code());
    RequireStatus(pager->EndRead());
  };
  const auto expect_open_success = [&fixture](std::size_t offset, std::uint32_t value) {
    std::vector<std::byte> bytes = fixture;
    Write32(bytes, offset, value);
    const TemporaryDatabase database(std::move(bytes));
    PosixVfs vfs;
    std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, database.path().string()));
    RequireStatus(pager->BeginRead());
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    {
      const auto cursor = IndexBtreeCursor::Open(*pager, PageNumber{76}, columns);
      EXPECT_TRUE(cursor.has_value());
    }
    RequireStatus(pager->EndRead());
  };

  expect_open_success(44, 0);
  expect_open_error(44, 5, ErrorCode::kNotDatabase);
  expect_open_error(56, 2, ErrorCode::kProtocol);
  expect_open_success(56, 4);
  expect_open_success(56, 5);
}

TEST(BtreeCursor, RejectsImpossibleCompletePayloadBeforeAllocation) {
  const TemporaryDatabase database(MakeImpossiblePayloadDatabase());
  PosixVfs vfs;
  std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, database.path().string()));
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.Seek(1, BtreeSeekMode::kEqual)));
    const BtreePayloadView payload = TakeValue(cursor.payload());
    EXPECT_EQ(ByteCount{0x7fffffffU}, payload.size());
    std::array<std::byte, 1> final_byte{};
    const Status impossible_range = cursor.ReadPayload(ByteOffset{0x7ffffffeU}, final_byte);
    ASSERT_FALSE(impossible_range.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, impossible_range.error().code());
    EXPECT_TRUE(cursor.valid());
    const auto copied = cursor.CopyPayload();
    ASSERT_FALSE(copied.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, copied.error().code());
  }
  RequireStatus(pager->EndRead());
}

TEST(BtreeCursor, RequiresAnActiveTransactionAtOpen) {
  PosixVfs vfs;
  std::unique_ptr<ReadPager> pager =
      TakeValue(ReadPager::Open(vfs, FixtureDatabasePath().string()));

  const auto table = TableBtreeCursor::Open(*pager, PageNumber{2});
  ASSERT_FALSE(table.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, table.error().code());

  const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
  const auto index = IndexBtreeCursor::Open(*pager, PageNumber{76}, columns);
  ASSERT_FALSE(index.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, index.error().code());
}

}  // namespace
}  // namespace modern_sqlite
