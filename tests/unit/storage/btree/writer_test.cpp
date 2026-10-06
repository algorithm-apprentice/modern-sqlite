#include "modern_sqlite/storage/btree/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {
namespace {

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

void RequireStatus(Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

template <typename Enum>
[[nodiscard]] constexpr Enum InvalidEnumValue(std::uint8_t value) noexcept {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  return static_cast<Enum>(value);
}

class TemporaryDatabase final {
 public:
  TemporaryDatabase() {
    static std::atomic<std::uint64_t> next_id{0};
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-btree-writer-" +
             std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)) + ".db");
    journal_path_ = path_;
    journal_path_ += "-journal";
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(journal_path_, ignored);
  }

  ~TemporaryDatabase() noexcept {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(journal_path_, ignored);
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
  std::filesystem::path journal_path_;
};

struct DatabaseU32Patch {
  std::size_t offset;
  std::uint32_t value;
};

void PatchDatabaseU32(const TemporaryDatabase& database, DatabaseU32Patch patch) {
  std::array<std::byte, sizeof(patch.value)> encoded{};
  StoreBigEndian<std::uint32_t>(encoded, patch.value);
  std::fstream file(database.path(), std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open temporary database for patching");
  }
  file.seekp(static_cast<std::streamoff>(patch.offset));
  file.write(reinterpret_cast<const char*>(encoded.data()),
             static_cast<std::streamsize>(encoded.size()));
  if (!file) {
    throw std::runtime_error("failed to patch temporary database");
  }
}

[[nodiscard]] std::unique_ptr<Pager> OpenEmptyWritable(PosixVfs& vfs,
                                                       const TemporaryDatabase& database) {
  return TakeValue(Pager::OpenWritable(vfs, database.path(),
                                       WritablePagerOptions{
                                           .pager =
                                               PagerOptions{
                                                   .empty_database_page_size = ByteCount{512},
                                                   .cache_capacity_pages = 32,
                                               },
                                       }));
}

[[nodiscard]] std::vector<SqlValue> IntegerIndexKey(std::int64_t value) {
  std::vector<SqlValue> values;
  values.reserve(2);
  values.push_back(SqlValue::Integer(value));
  values.push_back(SqlValue::Integer(value));
  return values;
}

[[nodiscard]] std::vector<SqlValue> MakeIndexKey(SqlValue value, std::int64_t rowid) {
  std::vector<SqlValue> values;
  values.reserve(2);
  values.push_back(std::move(value));
  values.push_back(SqlValue::Integer(rowid));
  return values;
}

[[nodiscard]] ByteBuffer CopyCellPayload(Pager& pager, const BtreeCellView& cell,
                                         BtreePageGeometry geometry) {
  ByteBuffer payload{cell.payload_size()};
  const MutableByteView output = payload.mutable_view();
  const ByteView local = cell.local_payload();
  std::ranges::copy(local, output.begin());
  std::size_t offset = local.size();
  std::optional<PageNumber> overflow_page = cell.first_overflow_page();
  while (offset < output.size()) {
    if (!overflow_page.has_value()) {
      throw std::runtime_error("overflow chain ended before the test payload");
    }
    const auto pin = TakeValue(pager.ReadPage(*overflow_page));
    const auto overflow = TakeValue(OverflowPageView::Parse(pin.frame().bytes(), geometry));
    const std::size_t count = std::min(overflow.payload().size(), output.size() - offset);
    std::ranges::copy(overflow.payload().first(count),
                      output.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += count;
    overflow_page = overflow.next_page();
  }
  if (overflow_page.has_value()) {
    throw std::runtime_error("overflow chain exceeded the test payload");
  }
  return payload;
}

void StoreTest16(MutableByteView bytes, std::size_t offset, std::uint16_t value) {
  StoreBigEndian<std::uint16_t>(
      std::span<std::byte, sizeof(value)>{bytes.data() + offset, sizeof(value)}, value);
}

void StoreTest32(MutableByteView bytes, std::size_t offset, std::uint32_t value) {
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(value)>{bytes.data() + offset, sizeof(value)}, value);
}

void WriteFullTableInteriorPage(MutableByteView page, bool page_one, PageNumber child,
                                std::size_t cell_count) {
  constexpr auto kSeparatorBase = static_cast<std::int64_t>(std::uint64_t{1} << 60U);
  const std::size_t header_offset = page_one ? 100U : 0U;
  std::ranges::fill(page.subspan(header_offset), std::byte{0});
  page[header_offset] = static_cast<std::byte>(BtreePageType::kInteriorTable);
  StoreTest16(page, header_offset + 3U, static_cast<std::uint16_t>(cell_count));
  StoreTest32(page, header_offset + 8U, child.value());

  std::size_t content_offset = page.size();
  for (std::size_t index = 0; index < cell_count; ++index) {
    std::array<std::byte, 9> rowid{};
    const auto encoded = EncodeSqliteVarint(
        std::bit_cast<std::uint64_t>(kSeparatorBase + static_cast<std::int64_t>(index)),
        MutableByteView{rowid});
    ASSERT_TRUE(encoded.has_value());
    const std::size_t cell_size = sizeof(std::uint32_t) + encoded->value();
    content_offset -= cell_size;
    StoreTest32(page, content_offset, child.value());
    std::ranges::copy(std::span{rowid}.first(encoded->value()),
                      page.begin() + static_cast<std::ptrdiff_t>(content_offset + 4U));
    StoreTest16(page, header_offset + 12U + index * 2U,
                static_cast<std::uint16_t>(content_offset));
  }
  StoreTest16(page, header_offset + 5U, static_cast<std::uint16_t>(content_offset));
}

void WriteNearlyFullTableLeaf(MutableByteView page) {
  constexpr std::size_t kPayloadSize = 230U;
  constexpr std::array<std::int64_t, 2> kRowids{
      (std::numeric_limits<std::int64_t>::max)() - 2,
      (std::numeric_limits<std::int64_t>::max)() - 1,
  };
  std::ranges::fill(page, std::byte{0});
  page[0] = static_cast<std::byte>(BtreePageType::kLeafTable);
  StoreTest16(page, 3U, static_cast<std::uint16_t>(kRowids.size()));

  std::size_t content_offset = page.size();
  for (std::size_t index = 0; index < kRowids.size(); ++index) {
    std::array<std::byte, 9> payload_size{};
    std::array<std::byte, 9> rowid{};
    const auto encoded_payload_size =
        EncodeSqliteVarint(kPayloadSize, MutableByteView{payload_size});
    const auto encoded_rowid =
        EncodeSqliteVarint(std::bit_cast<std::uint64_t>(kRowids[index]), MutableByteView{rowid});
    ASSERT_TRUE(encoded_payload_size.has_value());
    ASSERT_TRUE(encoded_rowid.has_value());
    const std::size_t cell_size =
        encoded_payload_size->value() + encoded_rowid->value() + kPayloadSize;
    content_offset -= cell_size;
    const MutableByteView output = page.subspan(content_offset, cell_size);
    std::ranges::copy(std::span{payload_size}.first(encoded_payload_size->value()), output.begin());
    std::ranges::copy(std::span{rowid}.first(encoded_rowid->value()),
                      output.begin() +
                          static_cast<std::ptrdiff_t>(encoded_payload_size->value()));
    std::ranges::fill(output.last(kPayloadSize), std::byte{0x6d});
    StoreTest16(page, 8U + index * 2U, static_cast<std::uint16_t>(content_offset));
  }
  StoreTest16(page, 5U, static_cast<std::uint16_t>(content_offset));
}

TEST(BtreeWriter, InitializesPageOneAndReadsAnEmptySchemaTree) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));

  RequireStatus(session.InitializeDatabase());
  EXPECT_EQ(1U, pager->page_count());
  EXPECT_EQ(PageNumber{1}, TakeValue(session.OpenTableBtree(PageNumber{1})).root_page());
  RequireStatus(pager->Commit());

  ASSERT_NE(nullptr, pager->header());
  EXPECT_EQ(ByteCount{512}, pager->header()->page_size());
  EXPECT_EQ(1U, pager->header()->header_page_count());
  EXPECT_EQ(4U, pager->header()->schema_format());
  EXPECT_EQ(1U, pager->header()->text_encoding());
  auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
  EXPECT_FALSE(TakeValue(cursor.First()));
}

TEST(BtreeWriter, InitializesAndMutatesA65536BytePageDatabase) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  auto opened = Pager::OpenWritable(vfs, database.path(),
                                    WritablePagerOptions{
                                        .pager =
                                            PagerOptions{
                                                .empty_database_page_size = ByteCount{65'536},
                                                .cache_capacity_pages = 8,
                                            },
                                        .journal =
                                            RollbackJournalOptions{
                                                .legacy_page_size = ByteCount{65'536},
                                            },
                                    });
  auto pager = TakeValue(std::move(opened));
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  constexpr std::array<std::byte, 2> kPayload{std::byte{0x11}, std::byte{0x22}};
  RequireStatus(writer.Insert((std::numeric_limits<std::int64_t>::min)(), kPayload));
  RequireStatus(writer.Insert((std::numeric_limits<std::int64_t>::max)(), kPayload));
  RequireStatus(pager->Commit());

  ASSERT_NE(nullptr, pager->header());
  EXPECT_EQ(ByteCount{65'536}, pager->header()->page_size());
  {
    const auto page = TakeValue(pager->ReadPage(PageNumber{1}));
    EXPECT_EQ(std::byte{0}, page.frame().bytes()[16]);
    EXPECT_EQ(std::byte{1}, page.frame().bytes()[17]);
  }

  auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  EXPECT_EQ((std::numeric_limits<std::int64_t>::min)(), TakeValue(cursor.rowid()));
  ASSERT_TRUE(TakeValue(cursor.Next()));
  EXPECT_EQ((std::numeric_limits<std::int64_t>::max)(), TakeValue(cursor.rowid()));
  EXPECT_FALSE(TakeValue(cursor.Next()));
}

TEST(BtreeWriter, AcceptsAnUntrustedHeaderPageCountAndPublishesTheEffectiveCount) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  const PageNumber root_page = TakeValue(session.CreateTableBtree()).root_page();
  RequireStatus(pager->Commit());
  RequireStatus(pager->EndRead());
  pager.reset();

  PatchDatabaseU32(database, DatabaseU32Patch{.offset = 28U, .value = 0U});

  pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  EXPECT_EQ(2U, pager->page_count());
  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  auto writer = TakeValue(session.OpenTableBtree(root_page));
  constexpr std::array<std::byte, 1> kPayload{std::byte{0x5a}};
  RequireStatus(writer.Insert(1, kPayload));
  RequireStatus(pager->Commit());
  ASSERT_NE(nullptr, pager->header());
  EXPECT_EQ(2U, pager->header()->header_page_count());
}

TEST(BtreeWriter, AcceptsRawZeroAsEffectiveUtf8Encoding) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  RequireStatus(pager->Commit());
  RequireStatus(pager->EndRead());
  pager.reset();

  PatchDatabaseU32(database, DatabaseU32Patch{.offset = 56U, .value = 0U});

  pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  constexpr std::array<std::byte, 1> kPayload{std::byte{0x6b}};
  RequireStatus(writer.Insert(1, kPayload));
  RequireStatus(pager->Commit());
}

TEST(BtreeWriter, InsertsReplacesAndDeletesOneTableRow) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  constexpr std::array<std::byte, 3> kInitial{
      std::byte{0x01},
      std::byte{0x02},
      std::byte{0x03},
  };
  RequireStatus(writer.Insert(42, kInitial));
  RequireStatus(pager->Commit());

  {
    auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.Seek(42, BtreeSeekMode::kEqual)));
    EXPECT_EQ(42, TakeValue(cursor.rowid()));
    const ByteBuffer copied = TakeValue(cursor.CopyPayload());
    EXPECT_TRUE(std::ranges::equal(ByteView{kInitial}, copied.view()));
  }

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const auto duplicate = writer.Insert(42, kInitial);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate.error().code());
  constexpr std::array<std::byte, 4> kReplacement{
      std::byte{0x04},
      std::byte{0x05},
      std::byte{0x06},
      std::byte{0x07},
  };
  RequireStatus(writer.Insert(42, kReplacement, BtreeInsertMode::kReplace));
  RequireStatus(pager->Commit());

  {
    auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.Seek(42, BtreeSeekMode::kEqual)));
    const ByteBuffer copied = TakeValue(cursor.CopyPayload());
    EXPECT_TRUE(std::ranges::equal(ByteView{kReplacement}, copied.view()));
  }

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  EXPECT_TRUE(TakeValue(writer.Delete(42)));
  EXPECT_FALSE(TakeValue(writer.Delete(42)));
  RequireStatus(pager->Commit());

  auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
  EXPECT_FALSE(TakeValue(cursor.First()));
}

TEST(BtreeWriter, InvalidatesHandlesAfterSavepointRollbackAndRejectsDuplicateSession) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  const auto duplicate = BtreeWriteSession::Open(*pager);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kLocked, duplicate.error().code());
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const auto savepoint = TakeValue(pager->CreateSavepoint());
  RequireStatus(pager->RollbackToSavepoint(savepoint));

  constexpr std::array<std::byte, 1> kPayload{std::byte{0x01}};
  const auto stale = writer.Insert(1, kPayload);
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(ErrorCode::kSchemaChanged, stale.error().code());

  auto restored_session = TakeValue(BtreeWriteSession::Open(*pager));
  auto restored_writer = TakeValue(restored_session.OpenTableBtree(PageNumber{1}));
  RequireStatus(restored_writer.Insert(1, kPayload));
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, SplitsTableLeavesInteriorPagesAndTheStableRoot) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));

  constexpr std::int64_t kRowCount = 1'500;
  for (std::int64_t rowid = 1; rowid <= kRowCount; ++rowid) {
    const auto encoded_rowid = static_cast<std::uint64_t>(rowid);
    std::array<std::byte, 20> payload{};
    payload.front() = static_cast<std::byte>(encoded_rowid & 0xffU);
    payload.back() = static_cast<std::byte>((encoded_rowid >> 8U) & 0xffU);
    RequireStatus(writer.Insert(rowid, payload));
  }
  RequireStatus(pager->Commit());

  {
    const auto root_pin = TakeValue(pager->ReadPage(PageNumber{1}));
    const auto geometry = TakeValue(
        BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
    const auto root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), PageNumber{1}, geometry));
    EXPECT_EQ(BtreePageType::kInteriorTable, root.type());
  }

  auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  for (std::int64_t expected = 1; expected <= kRowCount; ++expected) {
    const auto encoded_expected = static_cast<std::uint64_t>(expected);
    EXPECT_EQ(expected, TakeValue(cursor.rowid()));
    const ByteBuffer payload = TakeValue(cursor.CopyPayload());
    ASSERT_EQ(20U, payload.size().value());
    EXPECT_EQ(static_cast<std::byte>(encoded_expected & 0xffU), payload.view().front());
    EXPECT_EQ(static_cast<std::byte>((encoded_expected >> 8U) & 0xffU), payload.view().back());
    if (expected != kRowCount) {
      ASSERT_TRUE(TakeValue(cursor.Next()));
    }
  }
  EXPECT_FALSE(TakeValue(cursor.Next()));
}

TEST(BtreeWriter, WritesReplacesFreesAndReusesOverflowChains) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));

  std::vector<std::byte> first(2'000, std::byte{0x2a});
  first.front() = std::byte{0x11};
  first.back() = std::byte{0x12};
  RequireStatus(writer.Insert(7, first));
  RequireStatus(pager->Commit());

  {
    auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.Seek(7, BtreeSeekMode::kEqual)));
    const ByteBuffer copied = TakeValue(cursor.CopyPayload());
    EXPECT_TRUE(std::ranges::equal(first, copied.view()));
  }

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  std::vector<std::byte> replacement(3'000, std::byte{0x3a});
  replacement.front() = std::byte{0x21};
  replacement.back() = std::byte{0x22};
  RequireStatus(writer.Insert(7, replacement, BtreeInsertMode::kReplace));
  RequireStatus(pager->Commit());
  const std::uint32_t replacement_page_count = pager->page_count();

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  ASSERT_TRUE(TakeValue(writer.Delete(7)));
  RequireStatus(pager->Commit());
  ASSERT_NE(nullptr, pager->header());
  EXPECT_GT(pager->header()->freelist_page_count(), 0U);

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  RequireStatus(writer.Insert(8, replacement));
  RequireStatus(pager->Commit());
  EXPECT_LE(pager->page_count(), replacement_page_count);

  auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
  ASSERT_TRUE(TakeValue(cursor.Seek(8, BtreeSeekMode::kEqual)));
  const ByteBuffer copied = TakeValue(cursor.CopyPayload());
  EXPECT_TRUE(std::ranges::equal(replacement, copied.view()));
}

TEST(BtreeWriter, RejectsACyclicOverflowChainBeforeClearMutatesTheFreelist) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const std::vector<std::byte> payload(1'000, std::byte{0x4a});
  RequireStatus(writer.Insert(1, payload));
  RequireStatus(pager->Commit());

  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber first_overflow;
  {
    const auto root_pin = TakeValue(pager->ReadPage(PageNumber{1}));
    const auto root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), PageNumber{1}, geometry));
    ASSERT_EQ(1U, root.cell_count());
    first_overflow = TakeValue(root.cell(0)).first_overflow_page().value_or(PageNumber{});
  }
  ASSERT_NE(PageNumber{}, first_overflow);

  RequireStatus(pager->BeginWrite());
  {
    auto overflow = TakeValue(pager->WritePage(first_overflow));
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{overflow.mutable_bytes().data(),
                                                    sizeof(std::uint32_t)},
        first_overflow.value());
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const std::uint32_t freelist_count = pager->header()->freelist_page_count();
  const auto cleared = writer.Clear();
  ASSERT_FALSE(cleared.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, cleared.error().code());
  EXPECT_FALSE(writer.requires_rollback());
  EXPECT_EQ(freelist_count, pager->header()->freelist_page_count());
  RequireStatus(pager->Rollback());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  constexpr std::array<std::byte, 1> kReplacement{std::byte{0x6b}};
  const auto replaced = writer.Insert(1, kReplacement, BtreeInsertMode::kReplace);
  ASSERT_FALSE(replaced.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, replaced.error().code());
  EXPECT_FALSE(writer.requires_rollback());
  RequireStatus(pager->Rollback());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const auto deleted = writer.Delete(1);
  ASSERT_FALSE(deleted.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, deleted.error().code());
  EXPECT_FALSE(writer.requires_rollback());
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, RejectsAnImpossibleIndexPayloadBeforeMutation) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  auto writer = TakeValue(session.CreateIndexBtree(columns));
  const PageNumber root_page = writer.root_page();
  RequireStatus(writer.Insert(std::array{SqlValue::Text(std::string(900U, 'p'))}));
  RequireStatus(pager->Commit());

  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  std::size_t cell_offset = 0;
  std::size_t payload_size = 0;
  {
    const auto root_pin = TakeValue(pager->ReadPage(root_page));
    const auto root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), root_page, geometry));
    ASSERT_EQ(BtreePageType::kLeafIndex, root.type());
    ASSERT_EQ(1U, root.cell_count());
    const auto cell = TakeValue(root.cell(0));
    ASSERT_TRUE(cell.first_overflow_page().has_value());
    cell_offset = TakeValue(root.cell_offset(0)).value();
    payload_size = cell.payload_size().value();
  }
  const std::size_t maximum_local = geometry.maximum_index_local_payload().value();
  const std::size_t minimum_local = geometry.minimum_local_payload().value();
  const std::size_t overflow_capacity = geometry.overflow_payload_capacity().value();
  const std::size_t candidate_local =
      minimum_local + ((payload_size - minimum_local) % overflow_capacity);
  const std::size_t local_size =
      payload_size <= maximum_local
          ? payload_size
          : (candidate_local <= maximum_local ? candidate_local : minimum_local);
  const std::size_t original_overflow_pages =
      1U + (payload_size - local_size - 1U) / overflow_capacity;
  ASSERT_LT(original_overflow_pages, pager->page_count());
  const std::size_t impossible_size =
      payload_size +
      overflow_capacity * (static_cast<std::size_t>(pager->page_count()) - original_overflow_pages);
  EXPECT_EQ(pager->page_count(), 1U + (impossible_size - local_size - 1U) / overflow_capacity);

  RequireStatus(pager->BeginWrite());
  {
    auto root_pin = TakeValue(pager->WritePage(root_page));
    const MutableByteView bytes = root_pin.mutable_bytes();
    const auto original = DecodeSqliteVarint(ByteView{bytes}.subspan(cell_offset));
    ASSERT_TRUE(original.has_value());
    std::array<std::byte, 9> encoded{};
    const auto encoded_size = EncodeSqliteVarint(impossible_size, MutableByteView{encoded});
    ASSERT_TRUE(encoded_size.has_value());
    ASSERT_EQ(original->bytes_consumed, *encoded_size);
    std::ranges::copy(std::span{encoded}.first(encoded_size->value()),
                      bytes.begin() + static_cast<std::ptrdiff_t>(cell_offset));
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenIndexBtree(root_page, columns));
  const auto inserted = writer.Insert(std::array{SqlValue::Text("z")});
  ASSERT_FALSE(inserted.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, inserted.error().code());
  EXPECT_FALSE(writer.requires_rollback());
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, RejectsDuplicateFreelistOwnershipBeforeAllocation) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  RequireStatus(writer.Insert(1, std::vector<std::byte>(6'000U, std::byte{0x5c})));
  EXPECT_EQ(1U, TakeValue(writer.Clear()));
  RequireStatus(pager->Commit());

  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const PageNumber trunk_page = pager->header()->first_freelist_trunk();
  PageNumber duplicate_page;
  {
    const auto trunk_pin = TakeValue(pager->ReadPage(trunk_page));
    const auto trunk = TakeValue(FreelistTrunkView::Parse(trunk_pin.frame().bytes(), geometry));
    ASSERT_GE(trunk.leaf_count(), 2U);
    duplicate_page = TakeValue(trunk.leaf_page(0));
  }

  RequireStatus(pager->BeginWrite());
  {
    auto trunk_pin = TakeValue(pager->WritePage(trunk_page));
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{trunk_pin.mutable_bytes().data() + 12U,
                                                    sizeof(std::uint32_t)},
        duplicate_page.value());
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  const auto created = session.CreateTableBtree();
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, created.error().code());
  EXPECT_FALSE(session.requires_rollback());
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, RejectsOverflowPagesOwnedByTheFreelistBeforeReplacement) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const std::vector<std::byte> payload(500U, std::byte{0x5d});
  RequireStatus(writer.Insert(1, payload));
  RequireStatus(pager->Commit());

  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber overflow_page;
  {
    const auto root_pin = TakeValue(pager->ReadPage(PageNumber{1}));
    const auto root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), PageNumber{1}, geometry));
    ASSERT_EQ(1U, root.cell_count());
    overflow_page = TakeValue(root.cell(0)).first_overflow_page().value_or(PageNumber{});
  }
  ASSERT_NE(PageNumber{}, overflow_page);
  {
    const auto overflow_pin = TakeValue(pager->ReadPage(overflow_page));
    const auto overflow =
        TakeValue(OverflowPageView::Parse(overflow_pin.frame().bytes(), geometry));
    ASSERT_FALSE(overflow.next_page().has_value());
  }

  RequireStatus(pager->BeginWrite());
  {
    auto overflow_pin = TakeValue(pager->WritePage(overflow_page));
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{overflow_pin.mutable_bytes().data() + 4U,
                                                    sizeof(std::uint32_t)},
        0U);
  }
  {
    auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{page_one.mutable_bytes().data() + 32U,
                                                    sizeof(std::uint32_t)},
        overflow_page.value());
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{page_one.mutable_bytes().data() + 36U,
                                                    sizeof(std::uint32_t)},
        1U);
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const auto replaced = writer.Insert(1, payload, BtreeInsertMode::kReplace);
  ASSERT_FALSE(replaced.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, replaced.error().code());
  EXPECT_FALSE(writer.requires_rollback());
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, RejectsIndexComparisonOverflowOwnedByTheFreelist) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  auto writer = TakeValue(session.CreateIndexBtree(columns));
  const PageNumber root_page = writer.root_page();
  const std::array<SqlValue, 1> overflow_key{
      SqlValue::Text(std::string(450U, 'm')),
  };
  RequireStatus(writer.Insert(overflow_key));
  RequireStatus(pager->Commit());

  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber overflow_page;
  {
    const auto root_pin = TakeValue(pager->ReadPage(root_page));
    const auto root = TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), root_page, geometry));
    ASSERT_EQ(1U, root.cell_count());
    overflow_page = TakeValue(root.cell(0)).first_overflow_page().value_or(PageNumber{});
  }
  ASSERT_NE(PageNumber{}, overflow_page);
  {
    const auto overflow_pin = TakeValue(pager->ReadPage(overflow_page));
    const auto overflow =
        TakeValue(OverflowPageView::Parse(overflow_pin.frame().bytes(), geometry));
    ASSERT_FALSE(overflow.next_page().has_value());
  }

  RequireStatus(pager->BeginWrite());
  {
    auto overflow_pin = TakeValue(pager->WritePage(overflow_page));
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{overflow_pin.mutable_bytes().data() + 4U,
                                                    sizeof(std::uint32_t)},
        0U);
  }
  {
    auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{page_one.mutable_bytes().data() + 32U,
                                                    sizeof(std::uint32_t)},
        overflow_page.value());
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{page_one.mutable_bytes().data() + 36U,
                                                    sizeof(std::uint32_t)},
        1U);
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenIndexBtree(root_page, columns));
  const auto inserted = writer.Insert(std::array{SqlValue::Text("z")});
  ASSERT_FALSE(inserted.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, inserted.error().code());
  EXPECT_FALSE(writer.requires_rollback());
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, RejectsFreelistAliasesDuringClearPlanning) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  constexpr std::array<std::byte, 20> kPayload{};
  for (std::int64_t rowid = 1; rowid <= 100; ++rowid) {
    RequireStatus(writer.Insert(rowid, kPayload));
  }
  auto spare = TakeValue(session.CreateTableBtree());
  const PageNumber trunk_page = spare.root_page();
  RequireStatus(spare.Drop());
  RequireStatus(pager->Commit());

  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber child_page;
  {
    const auto root_pin = TakeValue(pager->ReadPage(PageNumber{1}));
    const auto root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), PageNumber{1}, geometry));
    ASSERT_EQ(BtreePageType::kInteriorTable, root.type());
    ASSERT_GT(root.cell_count(), 0U);
    child_page = TakeValue(root.cell(0)).left_child().value_or(PageNumber{});
  }
  ASSERT_NE(PageNumber{}, child_page);

  RequireStatus(pager->BeginWrite());
  {
    auto trunk_pin = TakeValue(pager->WritePage(trunk_page));
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{trunk_pin.mutable_bytes().data() + 4U,
                                                    sizeof(std::uint32_t)},
        1U);
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{trunk_pin.mutable_bytes().data() + 8U,
                                                    sizeof(std::uint32_t)},
        child_page.value());
  }
  {
    auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{page_one.mutable_bytes().data() + 36U,
                                                    sizeof(std::uint32_t)},
        2U);
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const auto inserted = writer.Insert(1, kPayload, BtreeInsertMode::kReplace);
  ASSERT_FALSE(inserted.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, inserted.error().code());
  EXPECT_FALSE(writer.requires_rollback());
  RequireStatus(pager->Rollback());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const auto cleared = writer.Clear();
  ASSERT_FALSE(cleared.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, cleared.error().code());
  EXPECT_FALSE(writer.requires_rollback());
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, RejectsDuplicateSiblingReferencesBeforeDeleteRebalancing) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const std::vector<std::byte> payload(230U, std::byte{0x4d});
  for (std::int64_t rowid = 1; rowid <= 4; ++rowid) {
    RequireStatus(writer.Insert(rowid, payload));
  }
  RequireStatus(pager->Commit());

  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber target_page;
  std::int64_t target_rowid = 0;
  std::optional<ByteCount> second_child_offset;
  {
    const auto root_pin = TakeValue(pager->ReadPage(PageNumber{1}));
    const auto root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), PageNumber{1}, geometry));
    ASSERT_EQ(BtreePageType::kInteriorTable, root.type());
    ASSERT_GT(root.cell_count(), 0U);
    target_page = TakeValue(root.cell(0)).left_child().value_or(PageNumber{});
    if (root.cell_count() > 1U) {
      second_child_offset = TakeValue(root.cell_offset(1));
    }
  }
  ASSERT_NE(PageNumber{}, target_page);
  {
    const auto leaf_pin = TakeValue(pager->ReadPage(target_page));
    const auto leaf = TakeValue(BtreePageView::Parse(leaf_pin.frame().bytes(), target_page, geometry));
    ASSERT_EQ(BtreePageType::kLeafTable, leaf.type());
    ASSERT_EQ(1U, leaf.cell_count());
    target_rowid = TakeValue(leaf.cell(0)).rowid().value_or(0);
  }

  RequireStatus(pager->BeginWrite());
  {
    auto root_pin = TakeValue(pager->WritePage(PageNumber{1}));
    const std::size_t child_offset =
        second_child_offset.has_value() ? second_child_offset->value() : 108U;
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{root_pin.mutable_bytes().data() + child_offset,
                                                    sizeof(std::uint32_t)},
        target_page.value());
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const auto deleted = writer.Delete(target_rowid);
  ASSERT_FALSE(deleted.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, deleted.error().code());
  EXPECT_FALSE(writer.requires_rollback());
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, CreatesAnIndependentTableRootAndPublishesThePageCount) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.CreateTableBtree());
  EXPECT_EQ(PageNumber{2}, writer.root_page());
  constexpr std::array<std::byte, 2> kPayload{std::byte{0x5a}, std::byte{0x6b}};
  RequireStatus(writer.Insert(-4, kPayload));
  RequireStatus(pager->Commit());

  ASSERT_NE(nullptr, pager->header());
  EXPECT_EQ(2U, pager->header()->header_page_count());
  auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{2}));
  ASSERT_TRUE(TakeValue(cursor.Seek(-4, BtreeSeekMode::kEqual)));
  const ByteBuffer copied = TakeValue(cursor.CopyPayload());
  EXPECT_TRUE(std::ranges::equal(ByteView{kPayload}, copied.view()));
}

TEST(BtreeWriter, RedistributesMergesAndCollapsesAfterTableDeletes) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));

  constexpr std::int64_t kRowCount = 700;
  constexpr std::array<std::byte, 18> kPayload{};
  for (std::int64_t rowid = 1; rowid <= kRowCount; ++rowid) {
    RequireStatus(writer.Insert(rowid, kPayload));
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  for (std::int64_t rowid = 1; rowid <= kRowCount; ++rowid) {
    if (rowid % 7 != 0) {
      ASSERT_TRUE(TakeValue(writer.Delete(rowid)));
    }
  }
  RequireStatus(pager->Commit());

  {
    auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    for (std::int64_t rowid = 7; rowid <= kRowCount; rowid += 7) {
      EXPECT_EQ(rowid, TakeValue(cursor.rowid()));
      if (rowid + 7 <= kRowCount) {
        ASSERT_TRUE(TakeValue(cursor.Next()));
      }
    }
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  for (std::int64_t rowid = 7; rowid <= kRowCount; rowid += 7) {
    ASSERT_TRUE(TakeValue(writer.Delete(rowid)));
  }
  RequireStatus(pager->Commit());

  auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
  EXPECT_FALSE(TakeValue(cursor.First()));
  const auto root_pin = TakeValue(pager->ReadPage(PageNumber{1}));
  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const auto root =
      TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), PageNumber{1}, geometry));
  EXPECT_EQ(BtreePageType::kLeafTable, root.type());
  EXPECT_EQ(0U, root.cell_count());
}

TEST(BtreeWriter, InsertsSplitsDeletesInteriorEntriesAndCollapsesAnIndexTree) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  auto writer = TakeValue(session.CreateIndexBtree(columns));
  const PageNumber root_page = writer.root_page();

  constexpr std::int64_t kRecordCount = 900;
  for (std::int64_t value = kRecordCount; value >= 1; --value) {
    std::vector<SqlValue> key = IntegerIndexKey(value);
    RequireStatus(writer.Insert(key));
  }
  {
    std::vector<SqlValue> duplicate = IntegerIndexKey(400);
    const auto inserted = writer.Insert(duplicate);
    ASSERT_FALSE(inserted.has_value());
    EXPECT_EQ(ErrorCode::kConstraint, inserted.error().code());
  }
  RequireStatus(pager->Commit());

  {
    auto cursor = TakeValue(IndexBtreeCursor::Open(*pager, root_page, columns));
    ASSERT_TRUE(TakeValue(cursor.First()));
    for (std::int64_t expected = 1; expected <= kRecordCount; ++expected) {
      const ByteBuffer payload = TakeValue(cursor.CopyPayload());
      auto values = TakeValue(DecodeRecord(payload.view()));
      ASSERT_EQ(2U, values.size());
      EXPECT_EQ(expected, values[0].integer_value());
      EXPECT_EQ(expected, values[1].integer_value());
      if (expected != kRecordCount) {
        ASSERT_TRUE(TakeValue(cursor.Next()));
      }
    }
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenIndexBtree(root_page, columns));
  for (std::int64_t value = 1; value <= kRecordCount; ++value) {
    if (value % 3 != 0) {
      std::vector<SqlValue> key = IntegerIndexKey(value);
      ASSERT_TRUE(TakeValue(writer.Delete(key)));
    }
  }
  RequireStatus(pager->Commit());

  {
    auto cursor = TakeValue(IndexBtreeCursor::Open(*pager, root_page, columns));
    ASSERT_TRUE(TakeValue(cursor.First()));
    for (std::int64_t expected = 3; expected <= kRecordCount; expected += 3) {
      const ByteBuffer payload = TakeValue(cursor.CopyPayload());
      auto values = TakeValue(DecodeRecord(payload.view()));
      EXPECT_EQ(expected, values[0].integer_value());
      if (expected + 3 <= kRecordCount) {
        ASSERT_TRUE(TakeValue(cursor.Next()));
      }
    }
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenIndexBtree(root_page, columns));
  for (std::int64_t value = 3; value <= kRecordCount; value += 3) {
    std::vector<SqlValue> key = IntegerIndexKey(value);
    ASSERT_TRUE(TakeValue(writer.Delete(key)));
  }
  RequireStatus(pager->Commit());

  auto cursor = TakeValue(IndexBtreeCursor::Open(*pager, root_page, columns));
  EXPECT_FALSE(TakeValue(cursor.First()));
}

TEST(BtreeWriter, ClearsDropsInvalidatesAndReusesATableRoot) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.CreateTableBtree());
  const PageNumber root_page = writer.root_page();
  auto stale = TakeValue(session.OpenTableBtree(root_page));
  constexpr std::array<std::byte, 12> kPayload{};
  for (std::int64_t rowid = 1; rowid <= 200; ++rowid) {
    RequireStatus(writer.Insert(rowid, kPayload));
  }

  EXPECT_EQ(200U, TakeValue(writer.Clear()));
  EXPECT_EQ(0U, TakeValue(writer.Clear()));
  RequireStatus(writer.Drop());
  const auto stale_insert = stale.Insert(1, kPayload);
  ASSERT_FALSE(stale_insert.has_value());
  EXPECT_EQ(ErrorCode::kSchemaChanged, stale_insert.error().code());

  const auto retired = session.OpenTableBtree(root_page);
  ASSERT_FALSE(retired.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, retired.error().code());

  auto replacement = TakeValue(session.CreateTableBtree());
  EXPECT_EQ(root_page, replacement.root_page());
  RequireStatus(replacement.Insert(9, kPayload));
  RequireStatus(pager->Commit());

  auto cursor = TakeValue(TableBtreeCursor::Open(*pager, root_page));
  ASSERT_TRUE(TakeValue(cursor.Seek(9, BtreeSeekMode::kEqual)));
  EXPECT_EQ(9, TakeValue(cursor.rowid()));
}

TEST(BtreeWriter, DropsInvalidatesAndReusesAnIndexRoot) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  auto writer = TakeValue(session.CreateIndexBtree(columns));
  const PageNumber root_page = writer.root_page();
  auto stale = TakeValue(session.OpenIndexBtree(root_page, columns));
  for (std::int64_t value = 1; value <= 200; ++value) {
    RequireStatus(writer.Insert(IntegerIndexKey(value)));
  }
  RequireStatus(writer.Drop());
  const auto stale_insert = stale.Insert(IntegerIndexKey(2));
  ASSERT_FALSE(stale_insert.has_value());
  EXPECT_EQ(ErrorCode::kSchemaChanged, stale_insert.error().code());

  const auto retired = session.OpenIndexBtree(root_page, columns);
  ASSERT_FALSE(retired.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, retired.error().code());

  auto replacement = TakeValue(session.CreateIndexBtree(columns));
  EXPECT_EQ(root_page, replacement.root_page());
  RequireStatus(replacement.Insert(IntegerIndexKey(3)));
  RequireStatus(pager->Commit());

  auto cursor = TakeValue(IndexBtreeCursor::Open(*pager, root_page, columns));
  ASSERT_TRUE(TakeValue(cursor.First()));
  auto values = TakeValue(DecodeRecord(TakeValue(cursor.CopyPayload()).view()));
  ASSERT_EQ(2U, values.size());
  EXPECT_EQ(3, values[0].integer_value());
  EXPECT_FALSE(TakeValue(cursor.Next()));
}

TEST(BtreeWriter, TransfersOverflowPredecessorsAndReusesTheirFreedPages) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending},
      IndexColumnOrder{BinaryCollation()},
  };
  auto writer = TakeValue(session.CreateIndexBtree(columns));
  const PageNumber root_page = writer.root_page();
  std::set<std::pair<std::string, std::int64_t>> expected;
  for (std::int64_t value = 1; value <= 80; ++value) {
    const std::string text(900, static_cast<char>('a' + value % 20));
    std::vector<SqlValue> key;
    key.reserve(2);
    key.push_back(SqlValue::Text(text));
    key.push_back(SqlValue::Integer(value));
    RequireStatus(writer.Insert(key));
    expected.emplace(text, value);
  }
  RequireStatus(pager->Commit());

  {
    auto cursor = TakeValue(IndexBtreeCursor::Open(*pager, root_page, columns));
    ASSERT_TRUE(TakeValue(cursor.First()));
    std::optional<std::string> previous;
    do {
      const ByteBuffer payload = TakeValue(cursor.CopyPayload());
      auto values = TakeValue(DecodeRecord(payload.view()));
      ASSERT_EQ(2U, values.size());
      const auto text = values[0].text_value();
      ASSERT_TRUE(text.has_value());
      const std::string current{text.value_or(Utf8View{}).bytes()};
      if (previous.has_value()) {
        EXPECT_GE(*previous, current);
      }
      previous = current;
    } while (TakeValue(cursor.Next()));
  }

  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  std::vector<SqlValue> interior_key;
  {
    const auto root_pin = TakeValue(pager->ReadPage(root_page));
    const auto root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), root_page, geometry));
    ASSERT_EQ(BtreePageType::kInteriorIndex, root.type());
    ASSERT_GT(root.cell_count(), 0U);
    const auto cell = TakeValue(root.cell(root.cell_count() / 2U));
    ASSERT_TRUE(cell.first_overflow_page().has_value());
    interior_key = TakeValue(DecodeRecord(CopyCellPayload(*pager, cell, geometry).view()));
    ASSERT_EQ(2U, interior_key.size());
  }
  const auto interior_text = interior_key[0].text_value();
  const auto interior_rowid = interior_key[1].integer_value();
  ASSERT_TRUE(interior_text.has_value());
  ASSERT_TRUE(interior_rowid.has_value());
  const std::string deleted_text{interior_text->bytes()};
  const std::int64_t deleted_rowid = *interior_rowid;
  ASSERT_EQ(1U, expected.erase(std::pair{deleted_text, deleted_rowid}));
  const std::uint32_t page_count_before_delete = pager->page_count();
  const std::uint32_t freelist_before_delete = pager->header()->freelist_page_count();

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenIndexBtree(root_page, columns));
  ASSERT_TRUE(TakeValue(writer.Delete(interior_key)));
  RequireStatus(pager->Commit());
  ASSERT_GT(pager->header()->freelist_page_count(), freelist_before_delete);
  const std::uint32_t freelist_after_delete = pager->header()->freelist_page_count();

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenIndexBtree(root_page, columns));
  constexpr std::int64_t kReplacementRowid = 1'000;
  std::vector<SqlValue> replacement_key;
  replacement_key.reserve(2);
  replacement_key.push_back(SqlValue::Text(deleted_text));
  replacement_key.push_back(SqlValue::Integer(kReplacementRowid));
  RequireStatus(writer.Insert(replacement_key));
  expected.emplace(deleted_text, kReplacementRowid);
  RequireStatus(pager->Commit());
  EXPECT_EQ(page_count_before_delete, pager->page_count());
  EXPECT_LT(pager->header()->freelist_page_count(), freelist_after_delete);

  {
    std::set<std::pair<std::string, std::int64_t>> actual;
    auto cursor = TakeValue(IndexBtreeCursor::Open(*pager, root_page, columns));
    ASSERT_TRUE(TakeValue(cursor.First()));
    do {
      auto values = TakeValue(DecodeRecord(TakeValue(cursor.CopyPayload()).view()));
      ASSERT_EQ(2U, values.size());
      const auto text = values[0].text_value();
      const auto rowid = values[1].integer_value();
      ASSERT_TRUE(text.has_value());
      ASSERT_TRUE(rowid.has_value());
      actual.emplace(std::string{text.value_or(Utf8View{}).bytes()}, rowid.value_or(0));
    } while (TakeValue(cursor.Next()));
    EXPECT_EQ(expected, actual);
  }

  RequireStatus(pager->BeginWrite());
  session = TakeValue(BtreeWriteSession::Open(*pager));
  writer = TakeValue(session.OpenIndexBtree(root_page, columns));
  EXPECT_EQ(80U, TakeValue(writer.Clear()));
  RequireStatus(pager->Commit());
  ASSERT_NE(nullptr, pager->header());
  EXPECT_GT(pager->header()->freelist_page_count(), 0U);
  auto cursor = TakeValue(IndexBtreeCursor::Open(*pager, root_page, columns));
  EXPECT_FALSE(TakeValue(cursor.First()));
}

TEST(BtreeWriter, RejectsUnsupportedDatabaseFormatsBeforeClaimingTheSession) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  const auto invalid_reserved =
      session.InitializeDatabase(BtreeDatabaseOptions{.reserved_bytes = ByteCount{256}});
  ASSERT_FALSE(invalid_reserved.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_reserved.error().code());
  const auto utf16_initialization = session.InitializeDatabase(BtreeDatabaseOptions{
      .text_encoding = DatabaseTextEncoding::kUtf16LittleEndian,
  });
  ASSERT_FALSE(utf16_initialization.has_value());
  EXPECT_EQ(ErrorCode::kProtocol, utf16_initialization.error().code());
  const auto invalid_schema = session.InitializeDatabase(BtreeDatabaseOptions{
      .schema_format = InvalidEnumValue<DatabaseSchemaFormat>(0),
  });
  ASSERT_FALSE(invalid_schema.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_schema.error().code());
  EXPECT_EQ(0U, pager->page_count());
  EXPECT_FALSE(session.requires_rollback());
  RequireStatus(session.InitializeDatabase());
  RequireStatus(pager->Commit());

  const auto set_header_field = [&pager](std::size_t offset, std::uint32_t value) {
    RequireStatus(pager->BeginWrite());
    {
      auto page = TakeValue(pager->WritePage(PageNumber{1}));
      StoreBigEndian<std::uint32_t>(
          std::span<std::byte, sizeof(std::uint32_t)>{page.mutable_bytes().data() + offset,
                                                      sizeof(std::uint32_t)},
          value);
    }
    RequireStatus(pager->Commit());
  };

  set_header_field(56U, static_cast<std::uint32_t>(DatabaseTextEncoding::kUtf16LittleEndian));
  RequireStatus(pager->BeginWrite());
  const auto utf16_session = BtreeWriteSession::Open(*pager);
  ASSERT_FALSE(utf16_session.has_value());
  EXPECT_EQ(ErrorCode::kProtocol, utf16_session.error().code());
  {
    auto page = TakeValue(pager->WritePage(PageNumber{1}));
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{page.mutable_bytes().data() + 56U,
                                                    sizeof(std::uint32_t)},
        static_cast<std::uint32_t>(DatabaseTextEncoding::kUtf8));
  }
  RequireStatus(pager->Commit());

  set_header_field(52U, 2U);
  RequireStatus(pager->BeginWrite());
  const auto auto_vacuum_session = BtreeWriteSession::Open(*pager);
  ASSERT_FALSE(auto_vacuum_session.has_value());
  EXPECT_EQ(ErrorCode::kProtocol, auto_vacuum_session.error().code());
  {
    auto page = TakeValue(pager->WritePage(PageNumber{1}));
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{page.mutable_bytes().data() + 52U,
                                                    sizeof(std::uint32_t)},
        0U);
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  EXPECT_TRUE(BtreeWriteSession::Open(*pager).has_value());
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, UsesReservedByteGeometryForOverflowMutation) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase(BtreeDatabaseOptions{.reserved_bytes = ByteCount{32}}));
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  std::vector<std::byte> payload(2'000U, std::byte{0x7c});
  RequireStatus(writer.Insert(1, payload));
  RequireStatus(pager->Commit());

  ASSERT_NE(nullptr, pager->header());
  EXPECT_EQ(ByteCount{480}, pager->header()->usable_size());
  auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
  ASSERT_TRUE(TakeValue(cursor.Seek(1, BtreeSeekMode::kEqual)));
  const ByteBuffer actual = TakeValue(cursor.CopyPayload());
  EXPECT_TRUE(std::ranges::equal(payload, actual.view()));
}

TEST(BtreeWriter, PreservesSQLiteIndexTypeCollationAndNullOrdering) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());

  const std::array<IndexColumnOrder, 2> type_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  auto type_writer = TakeValue(session.CreateIndexBtree(type_columns));
  const PageNumber type_root = type_writer.root_page();
  const std::array<std::byte, 1> zero_blob{std::byte{0}};
  const std::array<std::byte, 2> longer_blob{std::byte{0}, std::byte{1}};
  std::vector<std::vector<SqlValue>> type_keys;
  type_keys.push_back(MakeIndexKey(SqlValue{}, 1));
  type_keys.push_back(MakeIndexKey(SqlValue::Integer(-1), 2));
  type_keys.push_back(MakeIndexKey(SqlValue::Real(0.5), 3));
  type_keys.push_back(MakeIndexKey(SqlValue::Integer(2), 4));
  type_keys.push_back(MakeIndexKey(SqlValue::Text("A"), 5));
  type_keys.push_back(MakeIndexKey(SqlValue::Text("a"), 6));
  type_keys.push_back(MakeIndexKey(SqlValue::Blob(ByteBuffer::CopyOf(zero_blob)), 7));
  type_keys.push_back(MakeIndexKey(SqlValue::Blob(ByteBuffer::CopyOf(longer_blob)), 8));
  for (const auto& key : type_keys) {
    RequireStatus(type_writer.Insert(key));
  }

  const std::array<IndexColumnOrder, 3> collation_columns{
      IndexColumnOrder{NoCaseCollation()},
      IndexColumnOrder{RTrimCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  auto collation_writer = TakeValue(session.CreateIndexBtree(collation_columns));
  const PageNumber collation_root = collation_writer.root_page();
  const auto make_collation_key = [](std::string first, std::string second, std::int64_t rowid) {
    std::vector<SqlValue> key;
    key.reserve(3);
    key.push_back(SqlValue::Text(std::move(first)));
    key.push_back(SqlValue::Text(std::move(second)));
    key.push_back(SqlValue::Integer(rowid));
    return key;
  };
  RequireStatus(collation_writer.Insert(make_collation_key("alpha", "x", 2)));
  RequireStatus(collation_writer.Insert(make_collation_key("Alpha", "x ", 1)));
  RequireStatus(collation_writer.Insert(make_collation_key("beta", "a", 3)));
  const auto duplicate = collation_writer.Insert(make_collation_key("ALPHA", "x   ", 2));
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate.error().code());
  EXPECT_TRUE(TakeValue(collation_writer.Delete(make_collation_key("ALPHA", "x   ", 1))));

  const std::array<IndexColumnOrder, 2> null_columns{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
      IndexColumnOrder{BinaryCollation()},
  };
  auto null_writer = TakeValue(session.CreateIndexBtree(null_columns));
  const PageNumber null_root = null_writer.root_page();
  RequireStatus(null_writer.Insert(MakeIndexKey(SqlValue{}, 4)));
  RequireStatus(null_writer.Insert(MakeIndexKey(SqlValue::Integer(1), 1)));
  RequireStatus(null_writer.Insert(MakeIndexKey(SqlValue::Integer(3), 3)));
  RequireStatus(null_writer.Insert(MakeIndexKey(SqlValue::Integer(2), 2)));
  RequireStatus(pager->Commit());

  const auto read_rowids = [&pager](PageNumber root, std::span<const IndexColumnOrder> columns) {
    std::vector<std::int64_t> rowids;
    auto cursor = TakeValue(IndexBtreeCursor::Open(*pager, root, columns));
    if (!TakeValue(cursor.First())) {
      return rowids;
    }
    do {
      auto values = TakeValue(DecodeRecord(TakeValue(cursor.CopyPayload()).view()));
      if (values.empty() || !values.back().integer_value().has_value()) {
        throw std::runtime_error("index record is missing its rowid field");
      }
      rowids.push_back(*values.back().integer_value());
    } while (TakeValue(cursor.Next()));
    return rowids;
  };
  EXPECT_EQ((std::vector<std::int64_t>{1, 2, 3, 4, 5, 6, 7, 8}),
            read_rowids(type_root, type_columns));
  EXPECT_EQ((std::vector<std::int64_t>{2, 3}), read_rowids(collation_root, collation_columns));
  EXPECT_EQ((std::vector<std::int64_t>{3, 2, 1, 4}), read_rowids(null_root, null_columns));
}

TEST(BtreeWriter, RemovesLeafPaddingWhenPromotingATinyIndexRecord) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  auto writer = TakeValue(session.CreateIndexBtree(columns));
  const PageNumber root_page = writer.root_page();
  constexpr auto kLargeValue = static_cast<std::int64_t>(std::uint64_t{1} << 48U);
  for (std::int64_t offset = 19; offset >= 1; --offset) {
    RequireStatus(writer.Insert(std::array{SqlValue::Integer(-kLargeValue - offset)}));
  }
  RequireStatus(writer.Insert(std::array{SqlValue::Integer(0)}));
  for (std::int64_t offset = 1; offset <= 19; ++offset) {
    RequireStatus(writer.Insert(std::array{SqlValue::Integer(kLargeValue + offset)}));
  }
  RequireStatus(writer.Insert(std::array{SqlValue::Text("")}));
  RequireStatus(pager->Commit());

  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const auto root_pin = TakeValue(pager->ReadPage(root_page));
  const auto root = TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), root_page, geometry));
  ASSERT_EQ(BtreePageType::kInteriorIndex, root.type());
  ASSERT_EQ(1U, root.cell_count());
  const auto divider = TakeValue(root.cell(0));
  const auto values = TakeValue(DecodeRecord(divider.local_payload()));
  ASSERT_EQ(1U, values.size());
  EXPECT_EQ(0, values.front().integer_value());
  EXPECT_EQ(geometry.usable_size().value(),
            TakeValue(root.cell_offset(0)).value() + divider.encoded_size().value());
}

TEST(BtreeWriter, UsesThreePagesWhenARootLeafHasNoValidTwoWaySplit) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.CreateTableBtree());
  const PageNumber root_page = writer.root_page();
  RequireStatus(writer.Insert(1, std::vector<std::byte>(230U, std::byte{0x11})));
  RequireStatus(writer.Insert(3, std::vector<std::byte>(230U, std::byte{0x33})));
  const std::uint32_t page_count_before_split = pager->page_count();

  RequireStatus(writer.Insert(2, std::vector<std::byte>(300U, std::byte{0x22})));
  EXPECT_EQ(page_count_before_split + 3U, pager->page_count());
  RequireStatus(pager->Commit());

  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const auto root_pin = TakeValue(pager->ReadPage(root_page));
  const auto root = TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), root_page, geometry));
  ASSERT_EQ(BtreePageType::kInteriorTable, root.type());
  ASSERT_EQ(2U, root.cell_count());
  ASSERT_TRUE(root.rightmost_child().has_value());
  const std::array<PageNumber, 3> children{
      TakeValue(root.cell(0)).left_child().value_or(PageNumber{}),
      TakeValue(root.cell(1)).left_child().value_or(PageNumber{}),
      root.rightmost_child().value_or(PageNumber{}),
  };
  for (std::size_t index = 0; index < children.size(); ++index) {
    ASSERT_NE(PageNumber{}, children[index]);
    const auto child_pin = TakeValue(pager->ReadPage(children[index]));
    const auto child =
        TakeValue(BtreePageView::Parse(child_pin.frame().bytes(), children[index], geometry));
    EXPECT_EQ(BtreePageType::kLeafTable, child.type());
    ASSERT_EQ(1U, child.cell_count());
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), TakeValue(child.cell(0)).rowid().value_or(0));
  }
}

TEST(BtreeWriter, PropagatesTwoDividersFromAThreeWayNonRootTableSplit) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const std::array<std::pair<std::int64_t, std::size_t>, 10> rows{{
      {107, 101},
      {455, 61},
      {91, 1'494},
      {114, 14},
      {261, 37},
      {436, 0},
      {122, 83},
      {14, 1'180},
      {304, 23},
      {144, 899},
  }};
  for (const auto& [rowid, size] : std::span{rows}.first(rows.size() - 1U)) {
    std::vector<std::byte> payload(
        size, static_cast<std::byte>(static_cast<std::uint64_t>(rowid) & 0xffU));
    RequireStatus(writer.Insert(rowid, payload));
  }
  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  std::array<PageNumber, 2> children_before_split{};
  {
    const auto root_pin = TakeValue(pager->ReadPage(PageNumber{1}));
    const auto root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), PageNumber{1}, geometry));
    ASSERT_EQ(BtreePageType::kInteriorTable, root.type());
    ASSERT_EQ(1U, root.cell_count());
    ASSERT_TRUE(root.rightmost_child().has_value());
    children_before_split = {
        TakeValue(root.cell(0)).left_child().value_or(PageNumber{}),
        root.rightmost_child().value_or(PageNumber{}),
    };
  }
  const std::uint32_t page_count_before_split = pager->page_count();
  const auto& [final_rowid, final_size] = rows.back();
  const std::vector<std::byte> final_payload(
      final_size, static_cast<std::byte>(static_cast<std::uint64_t>(final_rowid) & 0xffU));
  RequireStatus(writer.Insert(final_rowid, final_payload));
  EXPECT_EQ(page_count_before_split + 3U, pager->page_count());
  RequireStatus(pager->Commit());

  {
    const auto root_pin = TakeValue(pager->ReadPage(PageNumber{1}));
    const auto root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), PageNumber{1}, geometry));
    ASSERT_EQ(BtreePageType::kInteriorTable, root.type());
    ASSERT_EQ(3U, root.cell_count());
    ASSERT_TRUE(root.rightmost_child().has_value());
    const std::array<PageNumber, 4> children{
        TakeValue(root.cell(0)).left_child().value_or(PageNumber{}),
        TakeValue(root.cell(1)).left_child().value_or(PageNumber{}),
        TakeValue(root.cell(2)).left_child().value_or(PageNumber{}),
        root.rightmost_child().value_or(PageNumber{}),
    };
    EXPECT_EQ(2U, std::ranges::count_if(children, [&children_before_split](PageNumber child) {
                return !std::ranges::contains(children_before_split, child);
              }));
    for (const PageNumber previous_child : children_before_split) {
      EXPECT_TRUE(std::ranges::contains(children, previous_child));
    }
    for (const PageNumber child_page : children) {
      ASSERT_NE(PageNumber{}, child_page);
      const auto child_pin = TakeValue(pager->ReadPage(child_page));
      const auto child =
          TakeValue(BtreePageView::Parse(child_pin.frame().bytes(), child_page, geometry));
      EXPECT_EQ(BtreePageType::kLeafTable, child.type());
      EXPECT_GT(child.cell_count(), 0U);
      EXPECT_LE(TakeValue(child.AnalyzeFreeSpace()).total().value(),
                geometry.usable_size().value());
    }
  }

  auto cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
  ASSERT_TRUE(TakeValue(cursor.First()));
  std::vector<std::int64_t> actual;
  do {
    actual.push_back(TakeValue(cursor.rowid()));
  } while (TakeValue(cursor.Next()));
  EXPECT_EQ((std::vector<std::int64_t>{14, 91, 107, 114, 122, 144, 261, 304, 436, 455}), actual);
}

TEST(BtreeWriter, RejectsRootSplitBeyondMaximumDepth) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  {
    auto session = TakeValue(BtreeWriteSession::Open(*pager));
    RequireStatus(session.InitializeDatabase());
  }
  for (std::uint32_t page_number = 2U; page_number <= kMaximumBtreeDepth; ++page_number) {
    const auto page = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{page_number}, page.frame().page_number());
  }
  for (std::uint32_t page_number = 1U; page_number < kMaximumBtreeDepth; ++page_number) {
    auto page = TakeValue(pager->WritePage(PageNumber{page_number}));
    WriteFullTableInteriorPage(page.mutable_bytes(), page_number == 1U,
                               PageNumber{page_number + 1U}, page_number == 1U ? 26U : 33U);
  }
  {
    auto leaf = TakeValue(pager->WritePage(PageNumber{kMaximumBtreeDepth}));
    WriteNearlyFullTableLeaf(leaf.mutable_bytes());
  }
  {
    auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
    StoreTest32(page_one.mutable_bytes(), 28U, static_cast<std::uint32_t>(kMaximumBtreeDepth));
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  auto writer = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const std::vector<std::byte> payload(230U, std::byte{0x7a});
  const auto inserted = writer.Insert((std::numeric_limits<std::int64_t>::max)(), payload);
  ASSERT_FALSE(inserted.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, inserted.error().code());
  EXPECT_TRUE(writer.requires_rollback());
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, MatchesDeterministicTableAndIndexModelsAcrossMixedMutations) {
  PosixVfs vfs;
  const TemporaryDatabase database;
  std::unique_ptr<Pager> pager = OpenEmptyWritable(vfs, database);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  auto table = TakeValue(session.CreateTableBtree());
  const PageNumber table_root = table.root_page();
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  auto index = TakeValue(session.CreateIndexBtree(columns));
  const PageNumber index_root = index.root_page();

  std::map<std::int64_t, std::vector<std::byte>> expected_table;
  std::set<std::int64_t> expected_index;
  const auto verify_models = [&] {
    {
      auto cursor = TakeValue(TableBtreeCursor::Open(*pager, table_root));
      bool valid = TakeValue(cursor.First());
      for (const auto& [rowid, expected_payload] : expected_table) {
        ASSERT_TRUE(valid);
        EXPECT_EQ(rowid, TakeValue(cursor.rowid()));
        const ByteBuffer actual = TakeValue(cursor.CopyPayload());
        EXPECT_TRUE(std::ranges::equal(expected_payload, actual.view()));
        valid = TakeValue(cursor.Next());
      }
      EXPECT_FALSE(valid);
    }
    {
      auto cursor = TakeValue(IndexBtreeCursor::Open(*pager, index_root, columns));
      bool valid = TakeValue(cursor.First());
      for (const std::int64_t expected : expected_index) {
        ASSERT_TRUE(valid);
        auto values = TakeValue(DecodeRecord(TakeValue(cursor.CopyPayload()).view()));
        ASSERT_EQ(2U, values.size());
        EXPECT_EQ(expected, values[0].integer_value());
        EXPECT_EQ(expected, values[1].integer_value());
        valid = TakeValue(cursor.Next());
      }
      EXPECT_FALSE(valid);
    }
  };
  // A fixed seed makes this model test reproducible.
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::minstd_rand random{0x5eedU};
  for (std::size_t operation = 0; operation < 1'200; ++operation) {
    const auto key = static_cast<std::int64_t>(random() % 500U);
    if (random() % 100U < 63U) {
      const std::size_t size = random() % 5U == 0U ? 600U + random() % 900U : random() % 120U;
      SCOPED_TRACE("operation " + std::to_string(operation) + " insert key " + std::to_string(key) +
                   " size " + std::to_string(size));
      std::vector<std::byte> payload(size);
      for (std::size_t index_value = 0; index_value < size; ++index_value) {
        const auto encoded =
            static_cast<std::uint64_t>(key) + static_cast<std::uint64_t>(index_value);
        payload[index_value] = static_cast<std::byte>(encoded & 0xffU);
      }
      const BtreeInsertMode mode =
          expected_table.contains(key) ? BtreeInsertMode::kReplace : BtreeInsertMode::kInsertOnly;
      const auto table_inserted = table.Insert(key, payload, mode);
      ASSERT_TRUE(table_inserted.has_value()) << "operation=" << operation << " key=" << key << ' '
                                              << table_inserted.error().ToString();
      expected_table[key] = std::move(payload);
      if (!expected_index.contains(key)) {
        const auto index_inserted = index.Insert(MakeIndexKey(SqlValue::Integer(key), key));
        ASSERT_TRUE(index_inserted.has_value()) << "operation=" << operation << " key=" << key
                                                << ' ' << index_inserted.error().ToString();
        expected_index.insert(key);
      }
    } else {
      SCOPED_TRACE("operation " + std::to_string(operation) + " delete key " + std::to_string(key));
      const bool table_deleted = TakeValue(table.Delete(key));
      EXPECT_EQ(expected_table.erase(key) != 0U, table_deleted);
      const bool index_deleted = TakeValue(index.Delete(MakeIndexKey(SqlValue::Integer(key), key)));
      EXPECT_EQ(expected_index.erase(key) != 0U, index_deleted);
    }
    if ((operation + 1U) % 100U == 0U) {
      verify_models();
    }
  }
  RequireStatus(pager->Commit());
  verify_models();
}

}  // namespace
}  // namespace modern_sqlite
