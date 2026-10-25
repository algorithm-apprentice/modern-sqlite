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
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/native_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "modern_sqlite/storage/page_number.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"
#include "tests/unit/storage/btree/database_test_support.hpp"

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
    auto opened = BtreeDatabase::Open(vfs_, FixtureDatabasePath().string(),
                                      PagerOptions{.cache_capacity_pages = 256});
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    database_ = std::move(*opened);
    pager_ = &database_->pager();
    const Status begun = pager_->BeginRead();
    ASSERT_TRUE(begun.has_value()) << begun.error().ToString();
  }

  void TearDown() override {
    if (pager_ != nullptr && pager_->in_read_transaction()) {
      const Status ended = pager_->EndRead();
      EXPECT_TRUE(ended.has_value()) << ended.error().ToString();
    }
  }

  NativeVfs vfs_;
  std::unique_ptr<BtreeDatabase> database_;
  Pager* pager_ = nullptr;
};

TEST_F(BtreeCursorCompatibilityTest, TraversesMultiLevelTableInBothDirections) {
  TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*database_, PageNumber{2}));
  const std::vector<std::int64_t> expected = ReadExpectedRowids("table-rowids.txt");

  EXPECT_EQ(CollectTableForward(cursor), expected);
  std::vector<std::int64_t> reversed = expected;
  std::ranges::reverse(reversed);
  EXPECT_EQ(CollectTableReverse(cursor), reversed);
}

TEST_F(BtreeCursorCompatibilityTest, CountsTableAndIndexEntriesThroughNativePageWalk) {
  const auto expected = ReadExpectedRowids("table-rowids.txt");
  const auto count = static_cast<std::uint64_t>(expected.size());
  TableBtreeCursor table = TakeValue(TableBtreeCursor::Open(*database_, PageNumber{2}));
  ASSERT_TRUE(TakeValue(table.First()));
  EXPECT_EQ(count, TakeValue(table.CountEntries()));
  EXPECT_FALSE(table.valid());
  EXPECT_EQ(count, TakeValue(table.CountEntries()));
  EXPECT_EQ(expected, CollectTableForward(table));

  const auto columns = BinaryIndexColumns();
  IndexBtreeCursor index = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, PageNumber{76}, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = (columns).size()}));
  ASSERT_TRUE(TakeValue(index.First()));
  EXPECT_EQ(count, TakeValue(index.CountEntries()));
  EXPECT_FALSE(index.valid());
  EXPECT_EQ(expected.size(), CollectIndexForward(index).size());
}

TEST_F(BtreeCursorCompatibilityTest, BorrowsBoundedRecordPrefixesAndRetainsCallerScratchOnly) {
  const auto columns = BinaryIndexColumns();
  IndexBtreeCursor local = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, PageNumber{76}, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = (columns).size()}));
  ASSERT_TRUE(TakeValue(local.First()));
  const auto local_payload = TakeValue(local.payload());
  ASSERT_TRUE(local_payload.is_fully_local());
  ByteBuffer scratch;
  const auto prefix = TakeValue(local.ReadRecordPrefix(1, scratch, local_payload.size()));
  EXPECT_EQ(1U, prefix.field_count());
  EXPECT_EQ(2U, prefix.complete_field_count());
  EXPECT_EQ(local_payload.local_bytes().data(), prefix.encoded().data());
  EXPECT_TRUE(scratch.empty());
  const auto zero = local.ReadRecordPrefix(0, scratch, local_payload.size());
  ASSERT_FALSE(zero.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, zero.error().code());
  const auto limited = local.ReadRecordPrefix(1, scratch, ByteCount{0});
  ASSERT_FALSE(limited.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, limited.error().code());
  RequireStatus(local.ClearPosition());
  const auto unpositioned = local.ReadRecordPrefix(1, scratch, local_payload.size());
  ASSERT_FALSE(unpositioned.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, unpositioned.error().code());

  IndexBtreeCursor overflow = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, PageNumber{102}, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = (columns).size()}));
  ASSERT_TRUE(TakeValue(overflow.First()));
  const auto overflow_payload = TakeValue(overflow.payload());
  ASSERT_FALSE(overflow_payload.is_fully_local());
  const auto complete = TakeValue(overflow.ReadRecordPrefix(2, scratch, overflow_payload.size()));
  EXPECT_EQ(2U, complete.field_count());
  EXPECT_EQ(2U, complete.complete_field_count());
  EXPECT_EQ(scratch.view().data(), complete.encoded().data());
  EXPECT_EQ(overflow_payload.size().value(), complete.encoded().size());
  const ByteBuffer expected = TakeValue(overflow.CopyPayload());
  EXPECT_TRUE(std::ranges::equal(expected.view(), complete.encoded()));
  const auto* storage = scratch.view().data();
  const auto shorter = TakeValue(overflow.ReadRecordPrefix(1, scratch, overflow_payload.size()));
  EXPECT_EQ(storage, scratch.view().data());
  EXPECT_LE(shorter.encoded().size(), scratch.size().value());
  EXPECT_EQ(1U, shorter.field_count());
  EXPECT_EQ(2U, shorter.complete_field_count());
}

TEST(BtreePayload, ReusesOverflowPageNumbersForRepeatedLateRanges) {
  constexpr std::size_t kCachePages = 4U;
  constexpr std::size_t kRangeBytes = 16U;
  constexpr std::size_t kReads = 4U;
  test::WritePagerFixedVfs vfs{false};
  const ByteBuffer fixture = ByteBuffer::CopyOf(ReadBytes(FixtureDatabasePath()));
  vfs.LoadDatabase(fixture.view());
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, kCachePages);
  ASSERT_NE(nullptr, database);
  RequireStatus(database->pager().BeginRead());
  {
    auto cursor = TakeValue(TableBtreeCursor::Open(*database, PageNumber{2}));
    ASSERT_TRUE(TakeValue(cursor.Seek(44, BtreeSeekMode::kEqual)));
    const ByteBuffer warm = TakeValue(cursor.CopyPayload());
    ASSERT_GE(warm.size().value(), kRangeBytes);
    ByteBuffer bytes{ByteCount{kRangeBytes}};
    const auto reads = vfs.total_database_reads();
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
    instrumentation::CounterCollection work;
    {
      const instrumentation::ScopedCounterCollection collect{work};
#endif
      for (std::size_t read = 0; read < kReads; ++read) {
        RequireStatus(cursor.ReadPayload(ByteOffset{warm.size().value() - kRangeBytes},
                                         bytes.mutable_view()));
        EXPECT_TRUE(std::ranges::equal(warm.view().last(kRangeBytes), bytes.view()));
      }
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
    }
    EXPECT_EQ(kReads, work.Value(instrumentation::Counter::kCacheHits) +
                          work.Value(instrumentation::Counter::kCacheMisses));
#endif
    EXPECT_LE(vfs.total_database_reads() - reads, 1U);
  }
  RequireStatus(database->pager().EndRead());
}

TEST_F(BtreeCursorCompatibilityTest, RetainsTheLazyOverflowListAcrossMovementAndSave) {
  auto cursor = TakeValue(TableBtreeCursor::Open(*database_, PageNumber{2}));
  const auto cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(1U, cursors.size());
  const auto& state = *cursors.front().identity;
  const auto list = [&] { return BtreeDatabaseTestPeer::OverflowPages(state); };
  std::array<std::byte, 1> byte{};
  ASSERT_TRUE(TakeValue(cursor.Seek(1, BtreeSeekMode::kEqual)));
  RequireStatus(cursor.ReadPayload(ByteOffset{0}, byte));
  EXPECT_TRUE(list().pages.empty());
  EXPECT_EQ(0U, list().capacity);
  EXPECT_FALSE(list().valid);

  ASSERT_TRUE(TakeValue(cursor.Seek(44, BtreeSeekMode::kEqual)));
  const auto payload = TakeValue(cursor.payload());
  RequireStatus(cursor.ReadPayload(ByteOffset{payload.size().value()}, {}));
  EXPECT_EQ(0U, list().capacity);
  RequireStatus(cursor.ReadPayload(ByteOffset{payload.local_bytes().size()}, byte));
  const auto geometry = TestGeometry();
  const std::size_t required = 1U + (payload.size().value() - payload.local_bytes().size() - 1U) /
                                        geometry.overflow_payload_capacity().value();
  EXPECT_EQ(required, list().pages.size());
  EXPECT_GE(list().capacity, required * 2U);
  EXPECT_TRUE(list().valid);
  EXPECT_NE(PageNumber{}, list().pages.front());
  EXPECT_TRUE(std::ranges::all_of(list().pages.subspan(1),
                                  [](PageNumber page) { return page == PageNumber{}; }));
  const PageNumber* const storage = list().pages.data();
  const std::size_t capacity = list().capacity;
  const ByteBuffer expected = TakeValue(cursor.CopyPayload());
  EXPECT_TRUE(
      std::ranges::none_of(list().pages, [](PageNumber page) { return page == PageNumber{}; }));

  RequireStatus(BtreeDatabaseTestPeer::SaveCursors(*database_, PageNumber{2}));
  EXPECT_FALSE(list().valid);
  EXPECT_EQ(storage, list().pages.data());
  EXPECT_EQ(capacity, list().capacity);
  EXPECT_EQ(0U, pager_->cache_pin_count());
  ASSERT_TRUE(TakeValue(cursor.RestorePosition()));
  EXPECT_FALSE(list().valid);
  EXPECT_TRUE(std::ranges::equal(expected.view(), TakeValue(cursor.CopyPayload()).view()));
  EXPECT_TRUE(list().valid);
  EXPECT_EQ(storage, list().pages.data());

  ASSERT_TRUE(TakeValue(cursor.Next()));
  EXPECT_FALSE(list().valid);
  ASSERT_TRUE(TakeValue(cursor.Previous()));
  EXPECT_FALSE(list().valid);
  EXPECT_TRUE(std::ranges::equal(expected.view(), TakeValue(cursor.CopyPayload()).view()));
  ASSERT_TRUE(TakeValue(cursor.Seek(44, BtreeSeekMode::kEqual)));
  EXPECT_FALSE(list().valid);
  static_cast<void>(TakeValue(cursor.CopyPayload()));
  auto moved = std::move(cursor);
  EXPECT_TRUE(list().valid);
  EXPECT_EQ(storage, list().pages.data());
  RequireStatus(moved.ClearPosition());
  EXPECT_FALSE(list().valid);
  EXPECT_EQ(capacity, list().capacity);
}

TEST(BtreePayload, RetainsPartialPageNumbersAfterIoFailureWithoutRetryOrPins) {
  test::WritePagerFixedVfs vfs{false};
  const ByteBuffer fixture = ByteBuffer::CopyOf(ReadBytes(FixtureDatabasePath()));
  vfs.LoadDatabase(fixture.view());
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 4U);
  ASSERT_NE(nullptr, database);
  RequireStatus(database->pager().BeginRead());
  {
    auto cursor = TakeValue(TableBtreeCursor::Open(*database, PageNumber{2}));
    ASSERT_TRUE(TakeValue(cursor.Seek(44, BtreeSeekMode::kEqual)));
    const auto payload = TakeValue(cursor.payload());
    const auto cursors = BtreeDatabaseTestPeer::Cursors(*database);
    ASSERT_EQ(1U, cursors.size());
    const auto& state = *cursors.front().identity;
    const auto pins = database->pager().cache_pin_count();
    const auto reads = vfs.total_database_reads();
    std::array<std::byte, 16> bytes{};
    vfs.FailDatabaseReadAfter(2U, ErrorCode::kIo);
    const auto failed =
        cursor.ReadPayload(ByteOffset{payload.size().value() - bytes.size()}, bytes);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_TRUE(cursor.valid());
    EXPECT_EQ(reads + 3U, vfs.total_database_reads());
    EXPECT_EQ(pins, database->pager().cache_pin_count());
    const auto partial = BtreeDatabaseTestPeer::OverflowPages(state);
    EXPECT_TRUE(partial.valid);
    ASSERT_GT(partial.pages.size(), 3U);
    EXPECT_TRUE(std::ranges::none_of(partial.pages.first(3),
                                     [](PageNumber page) { return page == PageNumber{}; }));
    EXPECT_TRUE(std::ranges::all_of(partial.pages.subspan(3),
                                    [](PageNumber page) { return page == PageNumber{}; }));
    RequireStatus(cursor.ReadPayload(ByteOffset{payload.size().value() - bytes.size()}, bytes));
    EXPECT_TRUE(std::ranges::all_of(bytes, [](std::byte value) { return value == std::byte{0}; }));
    const auto after_retry = vfs.total_database_reads();
    RequireStatus(cursor.ReadPayload(ByteOffset{payload.size().value() - bytes.size()}, bytes));
    EXPECT_EQ(after_retry, vfs.total_database_reads());
    EXPECT_EQ(pins, database->pager().cache_pin_count());
  }
  RequireStatus(database->pager().EndRead());
}

TEST_F(BtreeCursorCompatibilityTest, InvalidatesOverflowListsForEachIndexSearchCandidate) {
  const auto columns = BinaryIndexColumns();
  auto cursor = TakeValue(test::OpenIndexCursor(
      *database_, PageNumber{102}, columns,
      IndexKeyShape{.key_field_count = columns.size(), .all_field_count = columns.size()}));
  const auto cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(1U, cursors.size());
  const auto& state = *cursors.front().identity;
  const auto list = [&] { return BtreeDatabaseTestPeer::OverflowPages(state); };
  ASSERT_TRUE(TakeValue(cursor.First()));
  const ByteBuffer first = TakeValue(cursor.CopyPayload());
  const auto first_key = std::array{SqlValue::Text(CurrentIndexText(cursor)),
                                    SqlValue::Integer(CurrentIndexRowid(cursor))};
  EXPECT_TRUE(list().valid);
  EXPECT_EQ(42, CurrentIndexRowid(cursor));
  ASSERT_TRUE(TakeValue(cursor.Next()));
  EXPECT_FALSE(list().valid);
  const ByteBuffer second = TakeValue(cursor.CopyPayload());
  const auto second_key = std::array{SqlValue::Text(CurrentIndexText(cursor)),
                                     SqlValue::Integer(CurrentIndexRowid(cursor))};
  EXPECT_EQ(43, CurrentIndexRowid(cursor));
  EXPECT_TRUE(list().valid);
  for (const bool seek_first : {true, false, true}) {
    const auto& key = seek_first ? first_key : second_key;
    const auto& expected = seek_first ? first : second;
    ASSERT_TRUE(TakeValue(cursor.Seek(key, BtreeSeekMode::kEqual)));
    EXPECT_FALSE(list().valid);
    EXPECT_TRUE(std::ranges::equal(expected.view(), TakeValue(cursor.CopyPayload()).view()));
    EXPECT_TRUE(list().valid);
  }
  ASSERT_TRUE(TakeValue(cursor.Next()));
  EXPECT_FALSE(list().valid);
  ASSERT_TRUE(TakeValue(cursor.Previous()));
  EXPECT_FALSE(list().valid);
  EXPECT_TRUE(std::ranges::equal(first.view(), TakeValue(cursor.CopyPayload()).view()));
}

TEST(BtreePayload, InvalidatesTheOverflowListWhenAdvancingFromALeafToAnInteriorKey) {
  constexpr std::size_t kEntries = 24U;
  constexpr std::size_t kKeyBytes = kPageSize * 3U;
  constexpr std::int64_t kFirstRowid = 1'000;
  test::WritePagerFixedVfs vfs{false};
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  ASSERT_NE(nullptr, database);
  Pager& pager = database->pager();
  RequireStatus(pager.BeginRead());
  RequireStatus(pager.BeginWrite());
  const auto info = TakeValue(IndexComparisonInfo::CopyOf(BinaryIndexColumns()));
  PageNumber root;
  {
    auto session = TakeValue(BtreeWriteSession::Open(*database));
    RequireStatus(session.InitializeDatabase());
    auto index = TakeValue(session.CreateIndexBtree(info));
    root = index.root_page();
    for (std::size_t entry = 0; entry < kEntries; ++entry) {
      const auto character = static_cast<char>('a' + static_cast<int>(entry));
      RequireStatus(index.Insert(
          std::array{SqlValue::Text(std::string(kKeyBytes, character)),
                     SqlValue::Integer(kFirstRowid + static_cast<std::int64_t>(entry))}));
    }
  }
  RequireStatus(pager.Commit());
  char parent_character = 0;
  {
    const auto pin = TakeValue(pager.ReadPage(root));
    const auto page = TakeValue(BtreePageView::Parse(pin.frame().bytes(), root, TestGeometry()));
    ASSERT_FALSE(page.is_leaf());
    const auto cell = TakeValue(page.cell(0));
    ASSERT_FALSE(cell.local_payload().empty());
    parent_character = std::to_integer<char>(cell.local_payload().back());
  }
  ASSERT_GE(parent_character, 'a');
  ASSERT_LT(parent_character, 'a' + static_cast<int>(kEntries));
  const auto key = std::array{
      SqlValue::Text(std::string(kKeyBytes, parent_character)),
      SqlValue::Integer(kFirstRowid + static_cast<std::int64_t>(parent_character - 'a'))};
  const ByteBuffer expected = TakeValue(EncodeRecord(key));
  {
    auto cursor = TakeValue(IndexBtreeCursor::Open(*database, root, info));
    ASSERT_TRUE(TakeValue(cursor.Seek(key, BtreeSeekMode::kEqual)));
    ASSERT_TRUE(TakeValue(cursor.Previous()));
    const ByteBuffer leaf = TakeValue(cursor.CopyPayload());
    ASSERT_EQ(expected.size(), leaf.size());
    const auto cursors = BtreeDatabaseTestPeer::Cursors(*database);
    ASSERT_EQ(1U, cursors.size());
    const auto& state = *cursors.front().identity;
    ASSERT_TRUE(BtreeDatabaseTestPeer::OverflowPages(state).valid);
    ASSERT_TRUE(TakeValue(cursor.Next()));
    EXPECT_FALSE(BtreeDatabaseTestPeer::OverflowPages(state).valid);
    std::array<std::byte, 16> tail{};
    RequireStatus(cursor.ReadPayload(ByteOffset{expected.size().value() - tail.size()}, tail));
    EXPECT_TRUE(std::ranges::equal(expected.view().last(tail.size()), tail));
    ASSERT_TRUE(TakeValue(cursor.Next()));
    static_cast<void>(TakeValue(cursor.CopyPayload()));
    ASSERT_TRUE(TakeValue(cursor.Previous()));
    EXPECT_FALSE(BtreeDatabaseTestPeer::OverflowPages(state).valid);
    RequireStatus(cursor.ReadPayload(ByteOffset{expected.size().value() - tail.size()}, tail));
    EXPECT_TRUE(std::ranges::equal(expected.view().last(tail.size()), tail));
  }
  RequireStatus(pager.EndRead());
}

TEST(BtreePayload, DirectOverflowReadsDoNotPopulateThePageCache) {
  NativeVfs vfs;
  auto database = TakeValue(BtreeDatabase::Open(vfs, FixtureDatabasePath().string(),
                                                PagerOptions{.cache_capacity_pages = 4U}));
  Pager& pager = database->pager();
  RequireStatus(pager.BeginRead());
  {
    auto cursor = TakeValue(TableBtreeCursor::Open(*database, PageNumber{2}));
    ASSERT_TRUE(TakeValue(cursor.Seek(44, BtreeSeekMode::kEqual)));
    const auto payload = TakeValue(cursor.payload());
    const auto before = pager.cache_page_count();
    const auto pins = pager.cache_pin_count();
    const ByteBuffer expected = TakeValue(cursor.CopyPayload());
    EXPECT_EQ(before, pager.cache_page_count());
    EXPECT_EQ(pins, pager.cache_pin_count());
    std::array<std::byte, 16> tail{};
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
    instrumentation::CounterCollection work;
    {
      const instrumentation::ScopedCounterCollection collect{work};
#endif
      for (std::size_t read = 0; read < 4U; ++read) {
        RequireStatus(cursor.ReadPayload(ByteOffset{payload.size().value() - tail.size()}, tail));
        EXPECT_TRUE(std::ranges::equal(expected.view().last(tail.size()), tail));
      }
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
    }
    EXPECT_EQ(3U, work.Value(instrumentation::Counter::kCacheHits));
    EXPECT_EQ(1U, work.Value(instrumentation::Counter::kCacheMisses));
#endif
  }
  RequireStatus(pager.EndRead());
}

TEST(BtreePayload, UsesNativePointerMapGuessesOnlyInAutoVacuumMode) {
  const auto path = FixtureDirectory().parent_path() / "storage_diagnostics" /
                    "sqlite-3.54.0-storage-autovacuum.db";
  const ByteBuffer original = ByteBuffer::CopyOf(ReadBytes(path));
  const auto geometry = TestGeometry();
  constexpr PageNumber kRoot{3};
  const auto root = TakeValue(BtreePageView::Parse(
      original.view().subspan((kRoot.value() - 1U) * kPageSize, kPageSize), kRoot, geometry));
  const auto cell = TakeValue(root.cell(0));
  ASSERT_EQ(3, cell.rowid());
  const auto first = TakeOptional(cell.first_overflow_page(), "missing native overflow chain");
  const auto first_offset = (first.value() - 1U) * kPageSize;
  const auto next = LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
      original.view().data() + first_offset, sizeof(std::uint32_t)});
  const auto candidate = first.value() + 1U;
  const auto map = TakeValue(PointerMapPageFor(PageNumber{candidate}, geometry));
  const auto entry_offset = (map.value() - 1U) * kPageSize + (candidate - map.value() - 1U) * 5U;
  ByteBuffer sequential = ByteBuffer::CopyOf(original.view());
  std::ranges::copy(
      original.view().subspan((next - 1U) * kPageSize, kPageSize),
      sequential.mutable_view().subspan((candidate - 1U) * kPageSize, kPageSize).begin());
  Write32(sequential.mutable_view(), first_offset, candidate);
  sequential.mutable_view()[entry_offset] = static_cast<std::byte>(PointerMapType::kLaterOverflow);
  Write32(sequential.mutable_view(), entry_offset + 1U, first.value());
  for (const bool auto_vacuum : {false, true}) {
    for (const bool malformed_map : {false, true}) {
      for (const auto parent : {first.value(), 0U, geometry.locking_page().value(),
                                std::numeric_limits<std::uint32_t>::max()}) {
        SCOPED_TRACE(auto_vacuum);
        SCOPED_TRACE(malformed_map);
        SCOPED_TRACE(parent);
        ByteBuffer image = ByteBuffer::CopyOf(sequential.view());
        if (!auto_vacuum) {
          Write32(image.mutable_view(), 52U, 0U);
        }
        Write32(image.mutable_view(), entry_offset + 1U, parent);
        const bool matching_map = !malformed_map && parent == first.value();
        if (malformed_map) {
          image.mutable_view()[entry_offset] = std::byte{0};
        } else if (matching_map) {
          Write32(image.mutable_view(), first_offset, 0U);
        }
        test::WritePagerFixedVfs vfs{false};
        vfs.LoadDatabase(image.view());
        auto database = TakeValue(BtreeDatabase::Open(vfs, test::kWritePagerInputPath));
        Pager& pager = database->pager();
        RequireStatus(pager.BeginRead());
        {
          auto cursor = TakeValue(TableBtreeCursor::Open(*database, kRoot));
          ASSERT_TRUE(TakeValue(cursor.Seek(3, BtreeSeekMode::kEqual)));
          const auto payload = TakeValue(cursor.payload());
          std::array<std::byte, 16> tail{};
          const ByteOffset offset{payload.size().value() - tail.size()};
          if (auto_vacuum && matching_map) {
            const auto reads = vfs.total_database_reads();
            const auto pins = pager.cache_pin_count();
            vfs.FailDatabaseReadAfter(0U, ErrorCode::kIo);
            const auto failed = cursor.ReadPayload(offset, tail);
            ASSERT_FALSE(failed.has_value());
            EXPECT_EQ(ErrorCode::kIo, failed.error().code());
            EXPECT_EQ(reads + 1U, vfs.total_database_reads());
            EXPECT_EQ(pins, pager.cache_pin_count());
          }
          const auto result = cursor.ReadPayload(offset, tail);
          const bool expected = auto_vacuum ? !malformed_map : !matching_map;
          ASSERT_EQ(expected, result.has_value());
          if (expected) {
            EXPECT_TRUE(
                std::ranges::all_of(tail, [](std::byte value) { return value == std::byte{'m'}; }));
          } else {
            EXPECT_EQ(ErrorCode::kCorruption, result.error().code());
          }
        }
        RequireStatus(pager.EndRead());
      }
    }
  }
}

TEST(BtreePayload, ChecksCandidatePageBoundsAndSkipsReservedPages) {
  const auto geometry = TestGeometry();
  EXPECT_EQ(std::optional{PageNumber{106}},
            TakeValue(btree_internal::OverflowPageCandidate(PageNumber{104}, geometry, 110U)));
  EXPECT_FALSE(TakeValue(btree_internal::OverflowPageCandidate(PageNumber{104}, geometry, 105U)));
  const auto locking = geometry.locking_page().value();
  EXPECT_EQ(std::optional{PageNumber{locking + 1U}},
            TakeValue(btree_internal::OverflowPageCandidate(PageNumber{locking - 1U}, geometry,
                                                            locking + 1U)));
  const auto displaced = TakeValue(BtreePageGeometry::Create(ByteCount{1024}, ByteCount{820}));
  const auto displaced_locking = displaced.locking_page().value();
  EXPECT_EQ(std::optional{PageNumber{displaced_locking + 2U}},
            TakeValue(btree_internal::OverflowPageCandidate(PageNumber{displaced_locking - 1U},
                                                            displaced, displaced_locking + 2U)));
  constexpr auto kLastPage = std::numeric_limits<std::uint32_t>::max() - 1U;
  EXPECT_FALSE(
      TakeValue(btree_internal::OverflowPageCandidate(PageNumber{kLastPage}, geometry, kLastPage)));
  const auto invalid = btree_internal::OverflowPageCandidate(PageNumber{}, geometry, 12U);
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid.error().code());
  const auto outside = btree_internal::OverflowPageCandidate(PageNumber{13}, geometry, 12U);
  ASSERT_FALSE(outside.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, outside.error().code());
}

TEST(BtreePayload, DirectReadCapabilityAndErrorsPreserveTheOutputPrefix) {
  const ByteBuffer fixture = ByteBuffer::CopyOf(ReadBytes(FixtureDatabasePath()));
  for (const bool capable : {false, true}) {
    for (const bool short_read : {false, true}) {
      SCOPED_TRACE(capable);
      SCOPED_TRACE(short_read);
      test::WritePagerFixedVfs vfs{false};
      vfs.LoadDatabase(fixture.view());
      if (capable) {
        vfs.SetDatabaseDeviceCharacteristics(
            DeviceCharacteristics{}.With(DeviceCapability::kSubpageRead));
      }
      auto database = TakeValue(BtreeDatabase::Open(vfs, test::kWritePagerInputPath));
      Pager& pager = database->pager();
      RequireStatus(pager.BeginRead());
      {
        auto cursor = TakeValue(TableBtreeCursor::Open(*database, PageNumber{2}));
        ASSERT_TRUE(TakeValue(cursor.Seek(44, BtreeSeekMode::kEqual)));
        const auto payload = TakeValue(cursor.payload());
        const ByteBuffer expected = TakeValue(cursor.CopyPayload());
        ByteBuffer output{payload.size()};
        std::ranges::fill(output.mutable_view(), std::byte{0x7f});
        const auto pins = pager.cache_pin_count();
        const auto pages = pager.cache_page_count();
        if (capable) {
          const auto reads = vfs.total_database_reads();
          if (short_read) {
            vfs.ShortDatabaseReadAfter(0U, ByteCount{sizeof(std::uint32_t)});
          } else {
            vfs.FailDatabaseReadAfter(0U, ErrorCode::kIo);
          }
          const auto failed = cursor.ReadPayload(ByteOffset{0}, output.mutable_view());
          ASSERT_FALSE(failed.has_value());
          EXPECT_EQ(ErrorCode::kIo, failed.error().code());
          EXPECT_EQ(short_read ? 522 : ToSqlitePrimaryCode(ErrorCode::kIo),
                    failed.error().sqlite_code());
          EXPECT_TRUE(std::ranges::equal(expected.view().first(payload.local_bytes().size()),
                                         output.view().first(payload.local_bytes().size())));
          EXPECT_EQ(reads + 1U, vfs.total_database_reads());
          const auto request = vfs.last_database_read();
          ASSERT_TRUE(request.has_value());
          EXPECT_EQ(ByteCount{kPageSize}, request->second);
          EXPECT_EQ(pins, pager.cache_pin_count());
          EXPECT_EQ(pages, pager.cache_page_count());
        }
        RequireStatus(cursor.ReadPayload(ByteOffset{0}, output.mutable_view()));
        EXPECT_TRUE(std::ranges::equal(expected.view(), output.view()));
        if (capable) {
          EXPECT_EQ(pages, pager.cache_page_count());
        }
        EXPECT_EQ(pins, pager.cache_pin_count());
      }
      RequireStatus(pager.EndRead());
    }
  }
}

TEST(BtreePayload, DirectReadDoesNotUseBackingFileWhileTheCacheIsDirty) {
  test::WritePagerFixedVfs vfs{false};
  vfs.SetDatabaseDeviceCharacteristics(
      DeviceCharacteristics{}.With(DeviceCapability::kSubpageRead));
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  ASSERT_NE(nullptr, database);
  Pager& pager = database->pager();
  RequireStatus(pager.BeginRead());
  RequireStatus(pager.BeginWrite());
  {
    auto session = TakeValue(BtreeWriteSession::Open(*database));
    RequireStatus(session.InitializeDatabase());
    auto table = TakeValue(session.CreateTableBtree());
    ByteBuffer bytes{ByteCount{kPageSize * 3U}};
    std::ranges::fill(bytes.mutable_view(), std::byte{0x5a});
    RequireStatus(table.Insert(1, bytes.view()));
    std::array<std::byte, 8> untouched{};
    std::ranges::fill(untouched, std::byte{0x7f});
    const auto reads = vfs.total_database_reads();
    EXPECT_FALSE(TakeValue(pager.ReadOverflowDirect(table.root_page(), untouched)));
    EXPECT_EQ(reads, vfs.total_database_reads());
    EXPECT_TRUE(
        std::ranges::all_of(untouched, [](std::byte byte) { return byte == std::byte{0x7f}; }));
    auto cursor = TakeValue(TableBtreeCursor::Open(*database, table.root_page()));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_TRUE(std::ranges::equal(bytes.view(), TakeValue(cursor.CopyPayload()).view()));
  }
  RequireStatus(pager.Rollback());
  RequireStatus(pager.EndRead());
}

TEST(BtreeCursor, CountsWithoutReadingOverflowPayloadAndRecoversPageIoFailures) {
  test::WritePagerFixedVfs vfs{false};
  const ByteBuffer fixture = ByteBuffer::CopyOf(ReadBytes(FixtureDatabasePath()));
  vfs.LoadDatabase(fixture.view());
  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 256U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    const auto columns = BinaryIndexColumns();
    auto cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *pager_database, PageNumber{102}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));
    vfs.FailDatabaseReadAfter(0, ErrorCode::kIo);
    const auto failed = cursor.CountEntries();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_FALSE(cursor.valid());
    EXPECT_EQ(166U, TakeValue(cursor.CountEntries()));
    vfs.FailDatabaseReadAfter(0, ErrorCode::kIo);
    EXPECT_EQ(166U, TakeValue(cursor.CountEntries()));
    EXPECT_FALSE(cursor.valid());
    ASSERT_TRUE(TakeValue(cursor.First()));
    const auto payload = cursor.CopyPayload();
    ASSERT_FALSE(payload.has_value());
    EXPECT_EQ(ErrorCode::kIo, payload.error().code());
  }
  RequireStatus(pager->EndRead());
}

TEST_F(BtreeCursorCompatibilityTest, ClearsReadPositionsWithoutClosingOrRevivingCursors) {
  TableBtreeCursor table = TakeValue(TableBtreeCursor::Open(*database_, PageNumber{2}));
  ASSERT_TRUE(TakeValue(table.First()));
  const std::int64_t first_rowid = TakeValue(table.rowid());
  RequireStatus(table.ClearPosition());
  RequireStatus(table.ClearPosition());
  EXPECT_FALSE(table.valid());
  EXPECT_FALSE(table.rowid().has_value());
  ASSERT_TRUE(TakeValue(table.First()));
  EXPECT_EQ(first_rowid, TakeValue(table.rowid()));
  const auto columns = BinaryIndexColumns();
  IndexBtreeCursor index = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, PageNumber{76}, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = (columns).size()}));
  ASSERT_TRUE(TakeValue(index.First()));
  const ByteBuffer first_record = TakeValue(index.CopyPayload());
  RequireStatus(index.ClearPosition());
  EXPECT_FALSE(index.valid());
  EXPECT_FALSE(index.payload().has_value());
  ASSERT_TRUE(TakeValue(index.First()));
  const ByteBuffer restored = TakeValue(index.CopyPayload());
  EXPECT_TRUE(std::ranges::equal(first_record.view(), restored.view()));
  TableBtreeCursor moved_table = std::move(table);
  IndexBtreeCursor moved_index = std::move(index);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  const auto invalid_table = table.ClearPosition();
  const auto invalid_index = index.ClearPosition();
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(invalid_table.has_value());
  ASSERT_FALSE(invalid_index.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_table.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_index.error().code());
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  const auto invalid_table_count = table.CountEntries();
  const auto invalid_index_count = index.CountEntries();
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(invalid_table_count.has_value());
  ASSERT_FALSE(invalid_index_count.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_table_count.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_index_count.error().code());
  RequireStatus(moved_table.ClearPosition());
  RequireStatus(moved_index.ClearPosition());
}

TEST_F(BtreeCursorCompatibilityTest, ImplementsAllDirectionalTableSeekModes) {
  TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*database_, PageNumber{2}));

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
  TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*database_, PageNumber{2}));

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
    IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *database_, index.root, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));
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
    IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *database_, PageNumber{76}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));
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
    IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *database_, PageNumber{82}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));
    ExpectTextSeek(cursor, "cAsE-03", BtreeSeekMode::kGreaterOrEqual, 3);
    ExpectTextSeek(cursor, "cAsE-03", BtreeSeekMode::kGreater, 2);
    ExpectTextSeek(cursor, "cAsE-03", BtreeSeekMode::kLessOrEqual, 157);
    ExpectTextSeek(cursor, "cAsE-03", BtreeSeekMode::kLess, 158);
  }

  {
    const std::vector<IndexColumnOrder> columns = RTrimIndexColumns();
    IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *database_, PageNumber{89}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));
    ExpectTextSeek(cursor, "pad-04", BtreeSeekMode::kGreaterOrEqual, 4);
    ExpectTextSeek(cursor, "pad-04", BtreeSeekMode::kGreater, 5);
    ExpectTextSeek(cursor, "pad-04", BtreeSeekMode::kLessOrEqual, 157);
    ExpectTextSeek(cursor, "pad-04", BtreeSeekMode::kLess, 156);
  }
}

TEST_F(BtreeCursorCompatibilityTest, ComparesCurrentIndexPrefixesWithoutRepositioning) {
  const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
  IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, PageNumber{76}, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = (columns).size()}));
  std::array<SqlValue, 1> seek_key{SqlValue::Text("bin-05")};
  ASSERT_TRUE(TakeValue(cursor.Seek(seek_key, BtreeSeekMode::kGreaterOrEqual)));
  EXPECT_EQ(5, CurrentIndexRowid(cursor));

  std::array<SqlValue, 1> lower{SqlValue::Text("bin-04")};
  std::array<SqlValue, 1> equal{SqlValue::Text("bin-05")};
  std::array<SqlValue, 1> upper{SqlValue::Text("bin-06")};
  EXPECT_TRUE(TakeValue(cursor.CompareCurrent(lower)) == std::weak_ordering::greater);
  EXPECT_TRUE(TakeValue(cursor.CompareCurrent(equal)) == std::weak_ordering::equivalent);
  EXPECT_TRUE(TakeValue(cursor.CompareCurrent(upper)) == std::weak_ordering::less);
  EXPECT_EQ(5, CurrentIndexRowid(cursor));

  std::array<SqlValue, 2> full_key{
      SqlValue::Text("bin-05"),
      SqlValue::Integer(5),
  };
  EXPECT_TRUE(TakeValue(cursor.CompareCurrent(full_key)) == std::weak_ordering::equivalent);

  const auto empty = cursor.CompareCurrent(std::span<const SqlValue>{});
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, empty.error().code());
  EXPECT_TRUE(cursor.valid());

  std::array<SqlValue, 3> excessive{
      SqlValue::Text("bin-05"),
      SqlValue::Integer(5),
      SqlValue::Integer(0),
  };
  const auto too_long = cursor.CompareCurrent(excessive);
  ASSERT_FALSE(too_long.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, too_long.error().code());
  EXPECT_TRUE(cursor.valid());

  IndexBtreeCursor unpositioned = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, PageNumber{76}, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = (columns).size()}));
  const auto no_position = unpositioned.CompareCurrent(equal);
  ASSERT_FALSE(no_position.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, no_position.error().code());
}

TEST_F(BtreeCursorCompatibilityTest, ComparesOverflowBackedCurrentIndexPrefixes) {
  const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
  IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, PageNumber{102}, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = (columns).size()}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  EXPECT_EQ(42, CurrentIndexRowid(cursor));

  std::array<SqlValue, 1> key{SqlValue::Text(std::string(3000, 'm'))};
  EXPECT_TRUE(TakeValue(cursor.CompareCurrent(key)) == std::weak_ordering::equivalent);
  EXPECT_EQ(42, CurrentIndexRowid(cursor));
}

class BtreePrefixComparisonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    database_ = TakeValue(
        BtreeDatabase::OpenEphemeral(vfs_, PagerOptions{
                                               .empty_database_page_size = ByteCount{kPageSize},
                                               .cache_capacity_pages = 4,
                                           }));
    pager_ = &database_->pager();
    RequireStatus(pager_->BeginRead());
    RequireStatus(pager_->BeginWrite());
  }

  [[nodiscard]] PageNumber CreateIndex(std::span<const SqlValue> fields) {
    const ByteBuffer record = TakeValue(EncodeRecord(fields));
    BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*database_));
    RequireStatus(session.InitializeDatabase());
    IndexBtreeWriter index = TakeValue(modern_sqlite::test::CreateIndexBtree(
        session, columns_,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns_).size(),
                                     .all_field_count = std::max(fields.size(), columns_.size())}));
    RequireStatus(index.InsertEncoded(record.view()));
    return index.root_page();
  }

  [[nodiscard]] std::vector<PageNumber> OverflowPages(PageNumber root) {
    std::optional<PageNumber> next;
    std::size_t remaining = 0;
    {
      const ReadPagePin pin = TakeValue(pager_->ReadPage(root));
      const BtreePageView page =
          TakeValue(BtreePageView::Parse(pin.frame().bytes(), root, TestGeometry()));
      const BtreeCellView cell = TakeValue(page.cell(0));
      next = cell.first_overflow_page();
      remaining = cell.payload_size().value() - cell.local_payload().size();
    }
    std::vector<PageNumber> pages;
    const std::size_t capacity = TestGeometry().overflow_payload_capacity().value();
    while (remaining > 0) {
      const PageNumber page = TakeOptional(next, "test overflow chain ended early");
      pages.push_back(page);
      remaining -= std::min(remaining, capacity);
      if (remaining > 0) {
        const ReadPagePin pin = TakeValue(pager_->ReadPage(page));
        next = TakeValue(OverflowPageView::Parse(pin.frame().bytes(), TestGeometry())).next_page();
      }
    }
    return pages;
  }

  template <typename Operation>
  void ExpectCopyBound([[maybe_unused]] std::string_view property,
                       [[maybe_unused]] std::uint64_t maximum, Operation&& operation) {
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
    instrumentation::CounterCollection work;
    {
      const instrumentation::ScopedCounterCollection scope{work};
      std::forward<Operation>(operation)();
    }
    const std::uint64_t copied = work.Value(instrumentation::Counter::kBytesCopied);
    RecordProperty(std::string{property}, std::to_string(copied));
    EXPECT_LE(copied, maximum);
    EXPECT_GT(work.Value(instrumentation::Counter::kBtreeComparisons), 0U);
#else
    std::forward<Operation>(operation)();
#endif
  }

  test::WritePagerFixedVfs vfs_{false};
  std::unique_ptr<BtreeDatabase> database_;
  Pager* pager_ = nullptr;
  const std::array<IndexColumnOrder, 1> columns_{IndexColumnOrder{BinaryCollation()}};
};

#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
TEST_F(BtreePrefixComparisonTest, CountsSearchAndBothCurrentComparisonPathsOnce) {
  const std::array fields{SqlValue::Integer(7), SqlValue::Text("payload")};
  const PageNumber root = CreateIndex(fields);
  auto cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, root, columns_,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns_).size(),
                                   .all_field_count = std::max(fields.size(), columns_.size())}));
  const std::array key{SqlValue::Integer(7)};
  const ByteBuffer encoded = TakeValue(EncodeRecord(key));
  const auto key_record = TakeValue(RecordView::Parse(encoded.view()));
  const auto measure = [](auto&& operation) {
    instrumentation::CounterCollection work;
    {
      const instrumentation::ScopedCounterCollection scope{work};
      std::forward<decltype(operation)>(operation)();
    }
    return work.Value(instrumentation::Counter::kBtreeComparisons);
  };

  EXPECT_EQ(1U, measure([&] { EXPECT_TRUE(TakeValue(cursor.Seek(key, BtreeSeekMode::kEqual))); }));
  EXPECT_EQ(1U, measure([&] {
              EXPECT_EQ(std::weak_ordering::equivalent, TakeValue(cursor.CompareCurrent(key)));
            }));
  EXPECT_EQ(1U, measure([&] {
              EXPECT_EQ(std::weak_ordering::equivalent,
                        TakeValue(cursor.CompareCurrentRecordPrefix(key_record, 1)));
            }));
}
#endif

TEST_F(BtreePrefixComparisonTest, DoesNotCopyUncomparedOverflowCoveringFields) {
  const std::array<SqlValue, 2> fields{
      SqlValue::Integer(7),
      SqlValue::Blob(ByteBuffer{ByteCount{8192}}),
  };
  const PageNumber root = CreateIndex(fields);
  IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, root, columns_,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns_).size(),
                                   .all_field_count = std::max(fields.size(), columns_.size())}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  EXPECT_FALSE(TakeValue(cursor.payload()).is_fully_local());

  const std::array<SqlValue, 1> key{SqlValue::Integer(7)};
  for (const bool seek : {false, true}) {
    SCOPED_TRACE(seek);
    ExpectCopyBound(seek ? "seek_bytes_copied" : "current_bytes_copied", 8U, [&] {
      if (seek) {
        EXPECT_TRUE(TakeValue(cursor.Seek(key, BtreeSeekMode::kEqual)));
      } else {
        EXPECT_TRUE(TakeValue(cursor.CompareCurrent(key)) == std::weak_ordering::equivalent);
      }
    });
  }
}

TEST_F(BtreePrefixComparisonTest, BorrowsPrefixWithoutCopyingUnrequestedOverflowFields) {
  const std::array fields{
      SqlValue::Integer(7),
      SqlValue::Blob(ByteBuffer{ByteCount{8192}}),
  };
  const PageNumber root = CreateIndex(fields);
  auto cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, root, columns_,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns_).size(),
                                   .all_field_count = std::max(fields.size(), columns_.size())}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  const auto payload = TakeValue(cursor.payload());
  ASSERT_FALSE(payload.is_fully_local());
  ByteBuffer scratch;
  const auto read_prefix = [&] {
    const auto prefix = TakeValue(cursor.ReadRecordPrefix(1, scratch, payload.size()));
    EXPECT_EQ(1U, prefix.field_count());
    EXPECT_EQ(2U, prefix.complete_field_count());
    auto record_cursor = prefix.cursor();
    const auto field = record_cursor.Next();
    ASSERT_TRUE(field.has_value());
    EXPECT_EQ(7, field->integer_value());
  };
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
  instrumentation::CounterCollection work;
  {
    const instrumentation::ScopedCounterCollection scope{work};
    read_prefix();
  }
  EXPECT_EQ(0U, work.Value(instrumentation::Counter::kBytesCopied));
  EXPECT_EQ(0U, work.Value(instrumentation::Counter::kBtreeComparisons));
#else
  read_prefix();
#endif
  EXPECT_TRUE(scratch.empty());
}

TEST_F(BtreePrefixComparisonTest, BoundsCopiedBytesWhenTheCompleteHeaderIsNotLocal) {
  std::vector<SqlValue> fields(200);
  fields.front() = SqlValue::Integer(7);
  fields.back() = SqlValue::Blob(ByteBuffer{ByteCount{8192}});
  const ByteBuffer encoded = TakeValue(EncodeRecord(fields));
  const RecordView complete = TakeValue(RecordView::Parse(encoded.view()));
  const PageNumber root = CreateIndex(fields);
  IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, root, columns_,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns_).size(),
                                   .all_field_count = std::max(fields.size(), columns_.size())}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  EXPECT_GT(complete.header_size().value(), TakeValue(cursor.payload()).local_bytes().size());

  const std::array<SqlValue, 1> key{SqlValue::Integer(7)};
  ExpectCopyBound("current_bytes_copied", 2U * complete.header_size().value() + 1U, [&] {
    EXPECT_TRUE(TakeValue(cursor.CompareCurrent(key)) == std::weak_ordering::equivalent);
  });
  EXPECT_TRUE(TakeValue(cursor.Seek(key, BtreeSeekMode::kEqual)));
}

TEST_F(BtreePrefixComparisonTest, CopiesCompleteComparedTextWithoutTheCoveringSuffix) {
  const std::array<SqlValue, 2> fields{
      SqlValue::Text(std::string(1500, 'x')),
      SqlValue::Blob(ByteBuffer{ByteCount{8192}}),
  };
  const ByteBuffer encoded = TakeValue(EncodeRecord(fields));
  const RecordView complete = TakeValue(RecordView::Parse(encoded.view()));
  const PageNumber root = CreateIndex(fields);
  const std::array<IndexColumnOrder, 1> columns{IndexColumnOrder{NoCaseCollation()}};
  IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, root, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = std::max(fields.size(), columns.size())}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  const std::array<SqlValue, 1> key{SqlValue::Text(std::string(1500, 'X'))};
  for (const bool seek : {false, true}) {
    SCOPED_TRACE(seek);
    ExpectCopyBound(
        seek ? "seek_bytes_copied" : "current_bytes_copied", complete.header_size().value() + 1500U,
        [&] {
          if (seek) {
            EXPECT_TRUE(TakeValue(cursor.Seek(key, BtreeSeekMode::kEqual)));
          } else {
            EXPECT_TRUE(TakeValue(cursor.CompareCurrent(key)) == std::weak_ordering::equivalent);
          }
        });
  }
  RequireStatus(cursor.ClearPosition());
  ASSERT_TRUE(TakeValue(cursor.First()));
  EXPECT_TRUE(TakeValue(cursor.CompareCurrent(key)) == std::weak_ordering::equivalent);
  RequireStatus(cursor.ClearPosition());
}

TEST_F(BtreePrefixComparisonTest, KeepsFullChainValidationSeparateFromOrdinaryRangeReads) {
  const std::array<SqlValue, 2> fields{
      SqlValue::Integer(7),
      SqlValue::Blob(ByteBuffer{ByteCount{8192}}),
  };
  const PageNumber root = CreateIndex(fields);
  const std::vector<PageNumber> pages = OverflowPages(root);
  ASSERT_GT(pages.size(), 3U);
  {
    WritePagePin page = TakeValue(pager_->WritePage(pages[pages.size() - 2U]));
    Write32(page.mutable_bytes().value(), 0, 0);
  }
  IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, root, columns_,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns_).size(),
                                   .all_field_count = std::max(fields.size(), columns_.size())}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  const BtreePayloadView payload = TakeValue(cursor.payload());
  std::array<std::byte, 1> first_byte{};
  const std::size_t reads_before = vfs_.total_pathless_reads();
  RequireStatus(cursor.ReadPayload(ByteOffset{0}, first_byte));
  RequireStatus(cursor.ReadPayload(ByteOffset{payload.size().value()}, {}));
  EXPECT_EQ(payload.local_bytes()[0], first_byte[0]);
  EXPECT_EQ(reads_before, vfs_.total_pathless_reads());

  const std::array<SqlValue, 1> key{SqlValue::Integer(7)};
  const auto comparison = cursor.CompareCurrent(key);
  ASSERT_FALSE(comparison.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, comparison.error().code());
  EXPECT_TRUE(cursor.valid());
  const auto copied = cursor.CopyPayload();
  ASSERT_FALSE(copied.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, copied.error().code());
  const auto seek = cursor.Seek(key, BtreeSeekMode::kEqual);
  ASSERT_FALSE(seek.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, seek.error().code());
  EXPECT_FALSE(cursor.valid());
}

TEST_F(BtreePrefixComparisonTest, ReportsLateReadFailuresEvenWhenTheComparedKeyIsLocal) {
  const std::array<SqlValue, 2> fields{
      SqlValue::Integer(7),
      SqlValue::Blob(ByteBuffer{ByteCount{8192}}),
  };
  const PageNumber root = CreateIndex(fields);
  IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, root, columns_,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns_).size(),
                                   .all_field_count = std::max(fields.size(), columns_.size())}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  const std::array<SqlValue, 1> key{SqlValue::Integer(7)};
  for (const bool seek : {false, true}) {
    SCOPED_TRACE(seek);
    const std::size_t reads_before = vfs_.total_pathless_reads();
    vfs_.FailPathlessReadAfter(2, ErrorCode::kIo);
    if (seek) {
      const auto result = cursor.Seek(key, BtreeSeekMode::kEqual);
      ASSERT_FALSE(result.has_value());
      EXPECT_EQ(ErrorCode::kIo, result.error().code());
    } else {
      const auto result = cursor.CompareCurrent(key);
      ASSERT_FALSE(result.has_value());
      EXPECT_EQ(ErrorCode::kIo, result.error().code());
    }
    EXPECT_EQ(reads_before + 3U, vfs_.total_pathless_reads());
    EXPECT_EQ(!seek, cursor.valid());
  }
}

TEST_F(BtreePrefixComparisonTest, PreservesPayloadCopyPositionAfterLateReadFailure) {
  const std::array fields{
      SqlValue::Integer(7),
      SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(std::string(8192, 'x')))),
  };
  const ByteBuffer expected = TakeValue(EncodeRecord(fields));
  const PageNumber root = CreateIndex(fields);
  IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, root, columns_,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns_).size(),
                                   .all_field_count = std::max(fields.size(), columns_.size())}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  const BtreePayloadView before = TakeValue(cursor.payload());
  const ByteBuffer local = ByteBuffer::CopyOf(before.local_bytes());
  const std::size_t reads_before = vfs_.total_pathless_reads();
  vfs_.FailPathlessReadAfter(2, ErrorCode::kIo);
  const auto failed = cursor.CopyPayload();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_EQ(reads_before + 3U, vfs_.total_pathless_reads());
  EXPECT_TRUE(cursor.valid());
  const BtreePayloadView after = TakeValue(cursor.payload());
  EXPECT_EQ(before.local_bytes().data(), after.local_bytes().data());
  EXPECT_TRUE(std::ranges::equal(local.view(), after.local_bytes()));
  const ByteBuffer retry = TakeValue(cursor.CopyPayload());
  EXPECT_TRUE(std::ranges::equal(expected.view(), retry.view()));
}

TEST_F(BtreePrefixComparisonTest, PrioritizesLateReadFailuresOverStagedFormatErrors) {
  const std::array<SqlValue, 2> fields{
      SqlValue::Integer(7),
      SqlValue::Blob(ByteBuffer{ByteCount{8192}}),
  };
  const PageNumber root = CreateIndex(fields);
  ByteBuffer original;
  {
    const ReadPagePin pin = TakeValue(pager_->ReadPage(root));
    original = ByteBuffer::CopyOf(pin.frame().bytes());
  }
  const BtreePageView original_page =
      TakeValue(BtreePageView::Parse(original.view(), root, TestGeometry()));
  const BtreeCellView original_cell = TakeValue(original_page.cell(0));
  const auto payload_offset =
      static_cast<std::size_t>(original_cell.local_payload().data() - original.view().data());
  const std::array<SqlValue, 1> key{SqlValue::Integer(7)};
  const std::array<std::pair<std::size_t, std::byte>, 3> mutations{
      std::pair{0U, std::byte{0}},
      std::pair{1U, std::byte{0}},
      std::pair{2U, std::byte{10}},
  };
  for (const auto& [offset, replacement] : mutations) {
    SCOPED_TRACE(offset);
    {
      WritePagePin pin = TakeValue(pager_->WritePage(root));
      std::ranges::copy(original.view(), pin.mutable_bytes().value().begin());
      pin.mutable_bytes().value()[payload_offset + offset] = replacement;
    }
    IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *database_, root, columns_,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns_).size(),
                                     .all_field_count = std::max(fields.size(), columns_.size())}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    const auto malformed = cursor.CompareCurrent(key);
    ASSERT_FALSE(malformed.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, malformed.error().code());
    for (const bool seek : {false, true}) {
      SCOPED_TRACE(seek);
      const std::size_t reads_before = vfs_.total_pathless_reads();
      vfs_.FailPathlessReadAfter(2, ErrorCode::kIo);
      if (seek) {
        const auto result = cursor.Seek(key, BtreeSeekMode::kEqual);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(ErrorCode::kIo, result.error().code());
      } else {
        const auto result = cursor.CompareCurrent(key);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(ErrorCode::kIo, result.error().code());
      }
      EXPECT_EQ(reads_before + 3U, vfs_.total_pathless_reads());
      EXPECT_EQ(!seek, cursor.valid());
    }
  }
}

TEST_F(BtreePrefixComparisonTest, AcceptsInvalidAndCyclicUnusedFinalOverflowLinks) {
  const std::array<SqlValue, 2> fields{
      SqlValue::Integer(7),
      SqlValue::Blob(ByteBuffer{ByteCount{8192}}),
  };
  const PageNumber root = CreateIndex(fields);
  const std::vector<PageNumber> pages = OverflowPages(root);
  ASSERT_FALSE(pages.empty());
  const std::array<SqlValue, 1> key{SqlValue::Integer(7)};
  for (const std::uint32_t unused : {0xffffffffU, pages.front().value(), pages.back().value()}) {
    SCOPED_TRACE(unused);
    {
      WritePagePin pin = TakeValue(pager_->WritePage(pages.back()));
      Write32(pin.mutable_bytes().value(), 0, unused);
    }
    IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *database_, root, columns_,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns_).size(),
                                     .all_field_count = std::max(fields.size(), columns_.size())}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_TRUE(TakeValue(cursor.CompareCurrent(key)) == std::weak_ordering::equivalent);
    EXPECT_TRUE(TakeValue(cursor.Seek(key, BtreeSeekMode::kEqual)));
  }
}

TEST_F(BtreePrefixComparisonTest, RejectsShortOverflowRecordsAsCorruption) {
  const std::array<SqlValue, 1> fields{SqlValue::Blob(ByteBuffer{ByteCount{8192}})};
  const PageNumber root = CreateIndex(fields);
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, root, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = std::max(fields.size(), columns.size())}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  const std::array<SqlValue, 2> key{
      SqlValue::Blob(ByteBuffer{ByteCount{8192}}),
      SqlValue::Integer(1),
  };
  const auto comparison = cursor.CompareCurrent(key);
  ASSERT_FALSE(comparison.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, comparison.error().code());
  EXPECT_TRUE(cursor.valid());
  const auto seek = cursor.Seek(key, BtreeSeekMode::kEqual);
  ASSERT_FALSE(seek.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, seek.error().code());
  EXPECT_FALSE(cursor.valid());
}

TEST_F(BtreeCursorCompatibilityTest, ComparesCurrentPrefixesInPhysicalDescendingOrder) {
  const std::vector<IndexColumnOrder> columns = NoCaseDescendingIndexColumns();
  IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
      *database_, PageNumber{82}, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = (columns).size()}));
  std::array<SqlValue, 1> seek_key{SqlValue::Text("case-03")};
  ASSERT_TRUE(TakeValue(cursor.Seek(seek_key, BtreeSeekMode::kGreaterOrEqual)));
  EXPECT_EQ(3, CurrentIndexRowid(cursor));

  std::array<SqlValue, 1> physical_lower{SqlValue::Text("case-02")};
  std::array<SqlValue, 1> equal{SqlValue::Text("cAsE-03")};
  std::array<SqlValue, 1> physical_upper{SqlValue::Text("case-04")};
  EXPECT_TRUE(TakeValue(cursor.CompareCurrent(physical_lower)) == std::weak_ordering::less);
  EXPECT_TRUE(TakeValue(cursor.CompareCurrent(equal)) == std::weak_ordering::equivalent);
  EXPECT_TRUE(TakeValue(cursor.CompareCurrent(physical_upper)) == std::weak_ordering::greater);
  EXPECT_EQ(3, CurrentIndexRowid(cursor));
}

TEST_F(BtreeCursorCompatibilityTest, SeeksDescendingNullsAndOverflowBackedIndexKeys) {
  {
    const std::vector<IndexColumnOrder> columns = MixedIndexColumns();
    IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *database_, PageNumber{96}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));

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
    IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *database_, PageNumber{102}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));
    const std::string overflow_key(3000, 'm');
    ExpectTextSeek(cursor, overflow_key, BtreeSeekMode::kGreaterOrEqual, 42);
    ExpectTextSeek(cursor, overflow_key, BtreeSeekMode::kGreater, 43);
    ExpectTextSeek(cursor, overflow_key, BtreeSeekMode::kLessOrEqual, 42);
    ExpectTextSeek(cursor, overflow_key, BtreeSeekMode::kLess, std::nullopt);
  }
}

TEST_F(BtreeCursorCompatibilityTest, EnforcesTreeKindsMetadataPinsAndMovedFromState) {
  {
    const auto wrong_table = TableBtreeCursor::Open(*database_, PageNumber{76});
    ASSERT_FALSE(wrong_table.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, wrong_table.error().code());
  }
  {
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    const auto wrong_index = modern_sqlite::test::OpenIndexCursor(
        *database_, PageNumber{2}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()});
    ASSERT_FALSE(wrong_index.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, wrong_index.error().code());
  }
  {
    const auto zero_root = TableBtreeCursor::Open(*database_, PageNumber{0});
    ASSERT_FALSE(zero_root.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, zero_root.error().code());
  }
  {
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    const auto zero_root = modern_sqlite::test::OpenIndexCursor(
        *database_, PageNumber{0}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()});
    ASSERT_FALSE(zero_root.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, zero_root.error().code());
  }
  {
    const auto empty_metadata = modern_sqlite::test::OpenIndexCursor(
        *database_, PageNumber{76}, std::span<const IndexColumnOrder>{},
        modern_sqlite::IndexKeyShape{
            .key_field_count = (std::span<const IndexColumnOrder>{}).size(),
            .all_field_count = (std::span<const IndexColumnOrder>{}).size()});
    ASSERT_FALSE(empty_metadata.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, empty_metadata.error().code());
  }

  TableBtreeCursor original = TakeValue(TableBtreeCursor::Open(*database_, PageNumber{2}));
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
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*database_, PageNumber{2}));
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
    IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *database_, PageNumber{76}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));
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
    NativeVfs vfs;
    std::unique_ptr<BtreeDatabase> pager_database =
        TakeValue(BtreeDatabase::Open(vfs, empty.path().string()));
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    RequireStatus(pager->BeginRead());
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    EXPECT_EQ(0U, TakeValue(cursor.CountEntries()));
    EXPECT_FALSE(TakeValue(cursor.First()));
    EXPECT_FALSE(cursor.valid());

    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    const auto index = modern_sqlite::test::OpenIndexCursor(
        *pager_database, PageNumber{1}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()});
    ASSERT_FALSE(index.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, index.error().code());
    const auto zero_index = modern_sqlite::test::OpenIndexCursor(
        *pager_database, PageNumber{0}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()});
    ASSERT_FALSE(zero_index.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, zero_index.error().code());

    RequireStatus(pager->EndRead());
    empty.Rewrite(ReadBytes(FixtureDatabasePath()));
    RequireStatus(pager->BeginRead());
    const auto changed = cursor.First();
    ASSERT_FALSE(changed.has_value());
    EXPECT_EQ(ErrorCode::kSchemaChanged, changed.error().code());
    const auto changed_count = cursor.CountEntries();
    ASSERT_FALSE(changed_count.has_value());
    EXPECT_EQ(ErrorCode::kSchemaChanged, changed_count.error().code());
    RequireStatus(pager->EndRead());
  }

  {
    const TemporaryDatabase database(MakeVirtualRootDatabase());
    NativeVfs vfs;
    std::unique_ptr<BtreeDatabase> pager_database =
        TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    RequireStatus(pager->BeginRead());
    {
      TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
      EXPECT_EQ(1U, TakeValue(cursor.CountEntries()));
      EXPECT_FALSE(cursor.valid());
      ASSERT_TRUE(TakeValue(cursor.First()));
      EXPECT_EQ(7, TakeValue(cursor.rowid()));
      ASSERT_TRUE(TakeValue(cursor.Seek(7, BtreeSeekMode::kEqual)));
      EXPECT_EQ(7, TakeValue(cursor.rowid()));
    }
    RequireStatus(pager->EndRead());
  }
}

TEST(BtreeCursor, RevalidatesReleasedChildPinsWithoutUnpinningTheRoot) {
  const TemporaryDatabase database(MakeVirtualRootDatabase());
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::OpenWritable(vfs, database.path().string()));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_EQ(7, TakeValue(cursor.rowid()));
    for (const PageNumber page : {PageNumber{1}, PageNumber{2}}) {
      const auto blocked = pager->WritePage(page);
      ASSERT_FALSE(blocked.has_value());
      EXPECT_EQ(ErrorCode::kBusy, blocked.error().code());
    }

    RequireStatus(cursor.ClearPosition());
    EXPECT_FALSE(cursor.valid());
    const auto blocked_root = pager->WritePage(PageNumber{1});
    ASSERT_FALSE(blocked_root.has_value());
    EXPECT_EQ(ErrorCode::kBusy, blocked_root.error().code());
    {
      WritePagePin child = TakeValue(pager->WritePage(PageNumber{2}));
      TakeValue(child.mutable_bytes())[0] = std::byte{0};
    }
    const auto malformed_header = cursor.First();
    ASSERT_FALSE(malformed_header.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, malformed_header.error().code());
    EXPECT_FALSE(cursor.valid());
    {
      WritePagePin child = TakeValue(pager->WritePage(PageNumber{2}));
      MutableByteView bytes = TakeValue(child.mutable_bytes());
      bytes[0] = static_cast<std::byte>(BtreePageType::kLeafTable);
      Write16(bytes, 8, kPageSize - 1U);
    }
    ASSERT_TRUE(TakeValue(cursor.First()));
    const auto malformed_cell = cursor.rowid();
    ASSERT_FALSE(malformed_cell.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, malformed_cell.error().code());
    EXPECT_TRUE(cursor.valid());
    const auto malformed_payload = cursor.payload();
    ASSERT_FALSE(malformed_payload.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, malformed_payload.error().code());
    EXPECT_TRUE(cursor.valid());

    TableBtreeCursor moved = std::move(cursor);
    RequireStatus(moved.ClearPosition());
    {
      WritePagePin child = TakeValue(pager->WritePage(PageNumber{2}));
      Write16(TakeValue(child.mutable_bytes()), 8, kPageSize - 4U);
    }
    ASSERT_TRUE(TakeValue(moved.First()));
    EXPECT_EQ(7, TakeValue(moved.rowid()));
    EXPECT_EQ(ByteCount{0}, TakeValue(moved.payload()).size());
    EXPECT_EQ(1U, TakeValue(moved.CountEntries()));
    EXPECT_FALSE(moved.valid());
    ASSERT_TRUE(TakeValue(moved.Last()));
    EXPECT_EQ(7, TakeValue(moved.rowid()));
    EXPECT_FALSE(TakeValue(moved.Next()));
  }
  RequireStatus(pager->Rollback());
  RequireStatus(pager->EndRead());
}

#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
TEST(BtreeCursor, ReusesOneHeaderPerPinnedCurrentFrame) {
  using instrumentation::BtreeReadWorkMetric;
  using instrumentation::ProbeCollectionKind;
  using instrumentation::ProbeCounterCollection;
  using instrumentation::ScopedProbeCounterCollection;

  const TemporaryDatabase database(MakeVirtualRootDatabase());
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  ProbeCounterCollection work{ProbeCollectionKind::BtreeReadWork};
  {
    const ScopedProbeCounterCollection scope{work};
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    for (std::size_t repeat = 0; repeat < 3U; ++repeat) {
      EXPECT_EQ(7, TakeValue(cursor.rowid()));
      EXPECT_EQ(ByteCount{0}, TakeValue(cursor.payload()).size());
      RequireStatus(cursor.ReadPayload(ByteOffset{0}, MutableByteView{}));
    }
    EXPECT_EQ(2U, work.Value(BtreeReadWorkMetric::PageHeaderParse));
  }
  RequireStatus(pager->EndRead());
}

TEST(BtreeCursor, ReusesCurrentCellMetadataUntilRepositioned) {
  using instrumentation::BtreeReadWorkMetric;
  using instrumentation::ProbeCollectionKind;
  using instrumentation::ProbeCounterCollection;
  using instrumentation::ScopedProbeCounterCollection;

  const TemporaryDatabase database(MakeVirtualRootDatabase());
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    const auto check_current = [&cursor] {
      ProbeCounterCollection work{ProbeCollectionKind::BtreeReadWork};
      const ScopedProbeCounterCollection scope{work};
      for (std::size_t repeat = 0; repeat < 3U; ++repeat) {
        EXPECT_EQ(7, TakeValue(cursor.rowid()));
        EXPECT_EQ(ByteCount{0}, TakeValue(cursor.payload()).size());
        RequireStatus(cursor.ReadPayload(ByteOffset{0}, MutableByteView{}));
      }
      EXPECT_EQ(1U, work.Value(BtreeReadWorkMetric::CellDecode));
    };

    ASSERT_TRUE(TakeValue(cursor.First()));
    check_current();
    ASSERT_TRUE(TakeValue(cursor.Last()));
    check_current();
    ASSERT_TRUE(TakeValue(cursor.Seek(7, BtreeSeekMode::kEqual)));
    check_current();
    EXPECT_EQ(1U, TakeValue(cursor.CountEntries()));
    EXPECT_FALSE(cursor.valid());
    ASSERT_TRUE(TakeValue(cursor.First()));
    check_current();
    EXPECT_FALSE(TakeValue(cursor.Next()));
    ASSERT_TRUE(TakeValue(cursor.First()));
    check_current();
    EXPECT_FALSE(TakeValue(cursor.Previous()));
    ASSERT_TRUE(TakeValue(cursor.Last()));
    check_current();
    RequireStatus(cursor.ClearPosition());
    ASSERT_TRUE(TakeValue(cursor.First()));
    check_current();
  }
  RequireStatus(pager->EndRead());
}
#endif

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
    NativeVfs vfs;
    std::unique_ptr<BtreeDatabase> pager_database =
        TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    RequireStatus(pager->BeginRead());
    {
      TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{2}));
      const auto first = cursor.First();
      ASSERT_FALSE(first.has_value());
      EXPECT_EQ(ErrorCode::kCorruption, first.error().code());
      EXPECT_FALSE(cursor.valid());
      const auto count = cursor.CountEntries();
      ASSERT_FALSE(count.has_value());
      EXPECT_EQ(ErrorCode::kCorruption, count.error().code());
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
    NativeVfs vfs;
    std::unique_ptr<BtreeDatabase> pager_database =
        TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    RequireStatus(pager->BeginRead());
    {
      TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{2}));
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
    NativeVfs vfs;
    std::unique_ptr<BtreeDatabase> pager_database =
        TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    RequireStatus(pager->BeginRead());
    {
      TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{2}));
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
    NativeVfs vfs;
    std::unique_ptr<BtreeDatabase> pager_database =
        TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    RequireStatus(pager->BeginRead());
    {
      const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
      IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
          *pager_database, PageNumber{76}, columns,
          modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                       .all_field_count = (columns).size()}));
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
    NativeVfs vfs;
    std::unique_ptr<BtreeDatabase> pager_database =
        TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    RequireStatus(pager->BeginRead());
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    const auto cursor = modern_sqlite::test::OpenIndexCursor(
        *pager_database, PageNumber{76}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()});
    ASSERT_FALSE(cursor.has_value());
    EXPECT_EQ(expected, cursor.error().code());
    RequireStatus(pager->EndRead());
  };
  const auto expect_open_success = [&fixture](std::size_t offset, std::uint32_t value) {
    std::vector<std::byte> bytes = fixture;
    Write32(bytes, offset, value);
    const TemporaryDatabase database(std::move(bytes));
    NativeVfs vfs;
    std::unique_ptr<BtreeDatabase> pager_database =
        TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    RequireStatus(pager->BeginRead());
    const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
    {
      const auto cursor = modern_sqlite::test::OpenIndexCursor(
          *pager_database, PageNumber{76}, columns,
          modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                       .all_field_count = (columns).size()});
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
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
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
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, FixtureDatabasePath().string()));

  const auto table = TableBtreeCursor::Open(*pager_database, PageNumber{2});
  ASSERT_FALSE(table.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, table.error().code());

  const std::vector<IndexColumnOrder> columns = BinaryIndexColumns();
  const auto index = modern_sqlite::test::OpenIndexCursor(
      *pager_database, PageNumber{76}, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = (columns).size()});
  ASSERT_FALSE(index.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, index.error().code());
}

}  // namespace
}  // namespace modern_sqlite
