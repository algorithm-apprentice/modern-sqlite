#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"
#include "tests/unit/storage/btree/database_test_support.hpp"

namespace {

std::atomic<std::size_t> live_allocations = 0;
std::atomic<std::size_t> allocation_index = 0;
std::optional<std::size_t> failing_allocation;
void* first_allocation = nullptr;

void BeforeAllocation() {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation == index) {
    throw std::bad_alloc{};
  }
}

[[nodiscard]] void* Allocate(std::size_t size) {
  BeforeAllocation();
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) {
    if (allocation_index.load(std::memory_order_relaxed) == 1U) {
      first_allocation = memory;
    }
    live_allocations.fetch_add(1, std::memory_order_relaxed);
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  BeforeAllocation();
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0U ? alignment : size) == 0) {
    if (allocation_index.load(std::memory_order_relaxed) == 1U) {
      first_allocation = memory;
    }
    live_allocations.fetch_add(1, std::memory_order_relaxed);
    return memory;
  }
  throw std::bad_alloc{};
}

void Deallocate(void* memory) noexcept {
  if (memory != nullptr) {
    live_allocations.fetch_sub(1, std::memory_order_relaxed);
    std::free(memory);
  }
}

template <typename T>
[[nodiscard]] T TakeValue(modern_sqlite::Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

void RequireStatus(modern_sqlite::Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

struct OperationResult {
  bool succeeded;
  bool equivalent;
  modern_sqlite::ErrorCode error;
};

[[nodiscard]] OperationResult Execute(modern_sqlite::IndexBtreeCursor& cursor,
                                      std::span<const modern_sqlite::SqlValue> key, bool seek) {
  if (seek) {
    const auto result = cursor.Seek(key, modern_sqlite::BtreeSeekMode::kEqual);
    return OperationResult{
        .succeeded = result.has_value(),
        .equivalent = result.has_value() && *result,
        .error = result.has_value() ? modern_sqlite::ErrorCode::kGeneric : result.error().code(),
    };
  }
  const auto result = cursor.CompareCurrent(key);
  return OperationResult{
      .succeeded = result.has_value(),
      .equivalent = result.has_value() && *result == std::weak_ordering::equivalent,
      .error = result.has_value() ? modern_sqlite::ErrorCode::kGeneric : result.error().code(),
  };
}

struct Outcome {
  std::size_t allocations;
  bool invariant_holds;
};

template <typename Operation>
[[nodiscard]] Outcome ObserveAllocation(const char* name, Operation&& operation,
                                        std::optional<std::size_t> failure,
                                        std::optional<modern_sqlite::ErrorCode> expected_error,
                                        std::string_view expected_message = {}) {
  const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
  allocation_index.store(0, std::memory_order_relaxed);
  first_allocation = nullptr;
  failing_allocation = failure;
  bool correct = false;
  try {
    const auto result = operation();
    failing_allocation.reset();
    correct = expected_error.has_value()
                  ? !result.has_value() && result.error().code() == *expected_error &&
                        (expected_message.empty() || result.error().message() == expected_message)
                  : result.has_value();
  } catch (const std::bad_alloc&) {
    failing_allocation.reset();
    std::fprintf(stderr, "%s allocation %zu escaped std::bad_alloc\n", name, failure.value_or(0));
  }
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  return Outcome{.allocations = allocations,
                 .invariant_holds =
                     correct && live_before == live_allocations.load(std::memory_order_relaxed)};
}

template <typename Operation>
[[nodiscard]] bool ExhaustOwnedAllocations(const char* name, Operation&& operation,
                                           std::size_t expected_allocations,
                                           std::optional<modern_sqlite::ErrorCode> expected_error,
                                           std::string_view expected_message = {}) {
  const Outcome baseline =
      ObserveAllocation(name, operation, std::nullopt, expected_error, expected_message);
  if (baseline.allocations > 16U) {
    return false;
  }
  bool correct = baseline.invariant_holds && baseline.allocations == expected_allocations;
  for (std::size_t failure = 0; failure < baseline.allocations; ++failure) {
    const Outcome outcome =
        ObserveAllocation(name, operation, failure, modern_sqlite::ErrorCode::kOutOfMemory);
    correct = outcome.invariant_holds && correct;
  }
  const Outcome after_message =
      ObserveAllocation(name, operation, baseline.allocations, expected_error, expected_message);
  return after_message.invariant_holds && after_message.allocations == expected_allocations &&
         correct;
}

template <typename Operation>
[[nodiscard]] bool CheckDiagnostic(const char* name, Operation&& operation,
                                   modern_sqlite::ErrorCode expected_error) {
  const auto original = operation();
  if (original.has_value() || original.error().code() != expected_error) {
    return false;
  }
  return ExhaustOwnedAllocations(name, operation, 1, expected_error, original.error().message());
}

[[nodiscard]] bool CheckDatabaseOwnership() {
  using namespace modern_sqlite;
  const std::array openers{
      std::pair{"read-only database open",
                +[](Vfs& vfs) { return BtreeDatabase::Open(vfs, test::kWritePagerInputPath); }},
      std::pair{"writable database open",
                +[](Vfs& vfs) {
                  return BtreeDatabase::OpenWritable(vfs, test::kWritePagerDatabasePath);
                }},
      std::pair{"ephemeral-file database open",
                +[](Vfs& vfs) { return BtreeDatabase::OpenEphemeral(vfs); }},
      std::pair{"ephemeral-memory database open",
                +[](Vfs& vfs) { return BtreeDatabase::OpenEphemeralMemory(vfs); }},
  };
  for (const auto& [name, open_database] : openers) {
    test::WritePagerFixedVfs vfs;
    bool owner_allocated_first = true;
    const auto operation = [&] {
      auto result = open_database(vfs);
      if (result.has_value()) {
        owner_allocated_first = owner_allocated_first && result->get() == first_allocation;
      }
      return result;
    };
    const Outcome baseline = ObserveAllocation(name, operation, std::nullopt, std::nullopt);
    if (!baseline.invariant_holds || baseline.allocations == 0U ||
        !ExhaustOwnedAllocations(name, operation, baseline.allocations, std::nullopt) ||
        !owner_allocated_first || vfs.pathless_file_present()) {
      std::fprintf(stderr, "%s ownership/failure invariant failed (%zu allocations)\n", name,
                   baseline.allocations);
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::unique_ptr<modern_sqlite::BtreeDatabase> OpenNativeFixture(
    modern_sqlite::PosixVfs& vfs) {
  const auto path =
      std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path().parent_path() /
      "fixtures" / "btree_read" / "sqlite-3.54.0-btree-read.db";
  auto database = TakeValue(modern_sqlite::BtreeDatabase::Open(
      vfs, path.string(), modern_sqlite::PagerOptions{.cache_capacity_pages = 256}));
  RequireStatus(database->pager().BeginRead());
  return database;
}

[[nodiscard]] bool CheckComparisonInfoOwnership() {
  using namespace modern_sqlite;
  const std::array columns{IndexColumnOrder{},
                           IndexColumnOrder{NoCaseCollation(), IndexSortDirection::kDescending,
                                            IndexNullPlacement::kLast}};
  const bool created = ExhaustOwnedAllocations(
      "comparison info copy", [&] { return IndexComparisonInfo::CopyOf(columns); }, 1,
      std::nullopt);
  const IndexComparisonInfo original = TakeValue(IndexComparisonInfo::CopyOf(columns));
  const bool shared = ExhaustOwnedAllocations(
      "comparison info sharing",
      [&]() -> Status {
        auto copied = original;
        auto moved = std::move(copied);
        if (!copied.columns().empty() || moved.columns().data() != original.columns().data() ||
            moved.columns().size() != columns.size()) {
          return std::unexpected(
              Error::Create(ErrorCode::kInternal, "comparison info sharing changed its storage"));
        }
        return {};
      },
      0, std::nullopt);
  const bool empty = CheckDiagnostic(
      "empty comparison info", [] { return IndexComparisonInfo::CopyOf({}); }, ErrorCode::kMisuse);
  return created && shared && empty;
}

[[nodiscard]] bool CheckOpenFailures() {
  using namespace modern_sqlite;
  PosixVfs vfs;
  auto database = OpenNativeFixture(vfs);
  Pager* const pager = &database->pager();
  for (const PageNumber root : {PageNumber{2}, PageNumber{76}}) {
    const ReadPagePin warm = TakeValue(pager->ReadPage(root));
  }
  const std::array columns{IndexColumnOrder{BinaryCollation()},
                           IndexColumnOrder{BinaryCollation()}};
  const bool table = ExhaustOwnedAllocations(
      "table open", [&] { return TableBtreeCursor::Open(*database, PageNumber{2}); }, 1,
      std::nullopt);
  const bool index = ExhaustOwnedAllocations(
      "index open", [&] { return IndexBtreeCursor::Open(*database, PageNumber{76}, columns); }, 2,
      std::nullopt);
  const bool zero_root = CheckDiagnostic(
      "cursor misuse", [&] { return TableBtreeCursor::Open(*database, PageNumber{0}); },
      ErrorCode::kMisuse);
  const bool released = pager->EndRead().has_value();
  return table && index && zero_root && released;
}

[[nodiscard]] bool CheckTablePeerSaveAndRestoreFailures() {
  using namespace modern_sqlite;
  test::WritePagerFixedVfs vfs{false};
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 1U);
  if (database == nullptr) {
    return false;
  }
  Pager& pager = database->pager();
  RequireStatus(pager.BeginRead());
  RequireStatus(pager.BeginWrite());
  PageNumber root;
  {
    BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*database));
    RequireStatus(session.InitializeDatabase());
    TableBtreeWriter table = TakeValue(session.CreateTableBtree());
    root = table.root_page();
    const ByteBuffer record =
        TakeValue(EncodeRecord(std::array{SqlValue::Text(std::string(2'000, 's'))}));
    RequireStatus(table.Insert(1, record.view()));
  }
  RequireStatus(pager.Commit());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*database, root));
    if (!TakeValue(cursor.First()) || TakeValue(cursor.payload()).is_fully_local()) {
      return false;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0U;
    const std::size_t reads = vfs.total_database_reads();
    const auto saved = BtreeDatabaseTestPeer::SaveTableCursors(*database, root);
    failing_allocation.reset();
    if (!saved.has_value() || allocation_index.load(std::memory_order_relaxed) != 0U ||
        pager.cache_pin_count() != 0U || vfs.total_database_reads() != reads) {
      std::fprintf(stderr, "table peer save allocated, read payload, or retained a path pin\n");
      return false;
    }
    const auto header = TakeValue(pager.ReadPage(PageNumber{1}));
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0U;
    const auto failed = cursor.RestorePosition();
    failing_allocation.reset();
    if (failed.has_value() || failed.error().code() != ErrorCode::kOutOfMemory ||
        cursor.HasPosition() || TakeValue(cursor.RestorePosition()) ||
        pager.cache_pin_count() != 1U) {
      std::fprintf(stderr, "table peer OOM restore retried or retained an invalid position\n");
      return false;
    }
    if (!TakeValue(cursor.First()) || TakeValue(cursor.rowid()) != 1) {
      return false;
    }
    RequireStatus(BtreeDatabaseTestPeer::SaveTableCursors(*database, root));
    const std::size_t reads_before_failure = vfs.total_database_reads();
    vfs.FailDatabaseReadAfter(0U, ErrorCode::kIo);
    const auto io_failed = cursor.RestorePosition();
    if (io_failed.has_value() || io_failed.error().code() != ErrorCode::kIo ||
        cursor.HasPosition() || TakeValue(cursor.RestorePosition()) ||
        vfs.total_database_reads() != reads_before_failure + 1U) {
      std::fprintf(stderr, "table peer I/O restore lost its error or retried its root walk\n");
      return false;
    }
    if (!TakeValue(cursor.First()) || TakeValue(cursor.rowid()) != 1) {
      return false;
    }
  }
  return pager.cache_pin_count() == 0U && pager.EndRead().has_value();
}

template <typename Cursor>
[[nodiscard]] bool CheckCopies(Cursor& cursor, bool overflow) {
  using namespace modern_sqlite;
  const BtreePayloadView before = TakeValue(cursor.payload());
  if (before.is_fully_local() == overflow) {
    return false;
  }
  const ByteBuffer original = TakeValue(cursor.CopyPayload());
  const ByteBuffer local = ByteBuffer::CopyOf(before.local_bytes());
  const bool copied = ExhaustOwnedAllocations(
      overflow ? "overflow copy" : "local copy", [&] { return cursor.CopyPayload(); }, 1,
      std::nullopt);
  const BtreePayloadView after = TakeValue(cursor.payload());
  const ByteBuffer retry = TakeValue(cursor.CopyPayload());
  return copied && cursor.valid() && before.local_bytes().data() == after.local_bytes().data() &&
         std::ranges::equal(local.view(), after.local_bytes()) &&
         std::ranges::equal(original.view(), retry.view());
}

[[nodiscard]] bool CheckPayloadCopyFailures() {
  using namespace modern_sqlite;
  PosixVfs vfs;
  auto database = OpenNativeFixture(vfs);
  Pager* const pager = &database->pager();
  const std::array columns{IndexColumnOrder{BinaryCollation()},
                           IndexColumnOrder{BinaryCollation()}};
  bool correct = true;
  for (const bool overflow : {false, true}) {
    {
      auto cursor = TakeValue(TableBtreeCursor::Open(*database, PageNumber{2}));
      if (!TakeValue(cursor.Seek(overflow ? 44 : 1, BtreeSeekMode::kEqual))) {
        return false;
      }
      correct = CheckCopies(cursor, overflow) && correct;
    }
    {
      auto cursor =
          TakeValue(IndexBtreeCursor::Open(*database, PageNumber{overflow ? 102U : 76U}, columns));
      if (!TakeValue(cursor.First())) {
        return false;
      }
      correct = CheckCopies(cursor, overflow) && correct;
    }
  }
  return pager->EndRead().has_value() && correct;
}

[[nodiscard]] bool CheckCountAndBorrowedPrefixAllocations() {
  using namespace modern_sqlite;
  PosixVfs vfs;
  auto database = OpenNativeFixture(vfs);
  Pager* const pager = &database->pager();
  const std::array columns{IndexColumnOrder{BinaryCollation()},
                           IndexColumnOrder{BinaryCollation()}};
  bool correct = true;
  {
    auto table = TakeValue(TableBtreeCursor::Open(*database, PageNumber{2}));
    const auto expected = TakeValue(table.CountEntries());
    correct = expected == 166U && !table.valid() && correct;
    correct = ExhaustOwnedAllocations(
                  "warm table page count", [&] { return table.CountEntries(); }, 0, std::nullopt) &&
              correct;
    auto index = TakeValue(IndexBtreeCursor::Open(*database, PageNumber{102}, columns));
    correct = TakeValue(index.CountEntries()) == expected && !index.valid() && correct;
    correct = ExhaustOwnedAllocations(
                  "warm index page count", [&] { return index.CountEntries(); }, 0, std::nullopt) &&
              correct;
    if (!TakeValue(index.First())) {
      return false;
    }
    const ByteCount size = TakeValue(index.payload()).size();
    const ByteBuffer warm = TakeValue(index.CopyPayload());
    correct = warm.size() == size && correct;
    correct = ExhaustOwnedAllocations(
                  "borrowed overflow prefix",
                  [&]() -> Status {
                    ByteBuffer scratch;
                    const auto prefix = index.ReadRecordPrefix(1, scratch, size);
                    if (!prefix.has_value()) {
                      return std::unexpected(prefix.error());
                    }
                    return {};
                  },
                  1, std::nullopt) &&
              correct;
    ByteBuffer scratch;
    static_cast<void>(TakeValue(index.ReadRecordPrefix(1, scratch, size)));
    correct = ExhaustOwnedAllocations(
                  "reused overflow prefix",
                  [&] { return index.ReadRecordPrefix(1, scratch, size); }, 0, std::nullopt) &&
              correct;
    correct = CheckDiagnostic(
                  "zero borrowed prefix", [&] { return index.ReadRecordPrefix(0, scratch, size); },
                  ErrorCode::kMisuse) &&
              correct;
    correct = CheckDiagnostic(
                  "limited borrowed prefix",
                  [&] { return index.ReadRecordPrefix(1, scratch, ByteCount{0}); },
                  ErrorCode::kTooLarge) &&
              correct;
  }
  return pager->EndRead().has_value() && correct;
}

void Write16(modern_sqlite::ByteBuffer& bytes, std::size_t offset, std::uint16_t value) {
  modern_sqlite::StoreBigEndian<std::uint16_t>(
      std::span<std::byte, 2>{bytes.mutable_view().data() + offset, 2}, value);
}

void Write32(modern_sqlite::ByteBuffer& bytes, std::size_t offset, std::uint32_t value) {
  modern_sqlite::StoreBigEndian<std::uint32_t>(
      std::span<std::byte, 4>{bytes.mutable_view().data() + offset, 4}, value);
}

[[nodiscard]] modern_sqlite::ByteBuffer DiagnosticPage(modern_sqlite::BtreePageType type,
                                                       std::size_t cell_offset = 512) {
  using namespace modern_sqlite;
  ByteBuffer page{ByteCount{512}};
  std::ranges::fill(page.mutable_view(), std::byte{0});
  page.mutable_view()[0] = static_cast<std::byte>(type);
  Write16(page, 5, static_cast<std::uint16_t>(cell_offset));
  if (cell_offset != 512) {
    Write16(page, 3, 1);
    const bool interior =
        type == BtreePageType::kInteriorTable || type == BtreePageType::kInteriorIndex;
    Write16(page, interior ? 12U : 8U, static_cast<std::uint16_t>(cell_offset));
  }
  return page;
}

[[nodiscard]] bool CheckPageDiagnostics() {
  using namespace modern_sqlite;
  const auto geometry = TakeValue(BtreePageGeometry::Create(ByteCount{512}, ByteCount{512}));
  bool correct = true;
  const auto check = [&]<typename Operation>(const char* name, Operation&& operation,
                                             ErrorCode code = ErrorCode::kCorruption) {
    correct = CheckDiagnostic(name, operation, code) && correct;
  };
  ByteBuffer bad_type = DiagnosticPage(BtreePageType::kLeafTable);
  bad_type.mutable_view()[0] = std::byte{0};
  check("page type propagation",
        [&] { return BtreePageView::Parse(bad_type.view(), PageNumber{2}, geometry); });
  const ByteBuffer right_child = DiagnosticPage(BtreePageType::kInteriorTable);
  check("right child propagation",
        [&] { return BtreePageView::Parse(right_child.view(), PageNumber{2}, geometry); });

  const auto first_cell = [&](const ByteBuffer& bytes) -> Result<BtreeCellView> {
    auto page = BtreePageView::Parse(bytes.view(), PageNumber{2}, geometry);
    if (!page.has_value()) {
      return std::unexpected(std::move(page.error()));
    }
    return page->cell(0);
  };
  const ByteBuffer empty = DiagnosticPage(BtreePageType::kLeafTable);
  check("cell range propagation", [&] { return first_cell(empty); }, ErrorCode::kOutOfRange);
  ByteBuffer invalid_offset = DiagnosticPage(BtreePageType::kLeafTable, 511);
  Write16(invalid_offset, 8, 512);
  check("cell offset propagation", [&] { return first_cell(invalid_offset); });
  ByteBuffer child = DiagnosticPage(BtreePageType::kInteriorTable, 507);
  Write32(child, 8, 1);
  child.mutable_view()[511] = std::byte{0x80};
  check("left child propagation", [&] { return first_cell(child); });
  ByteBuffer interior_rowid = ByteBuffer::CopyOf(child.view());
  Write32(interior_rowid, 507, 1);
  check("interior rowid propagation", [&] { return first_cell(interior_rowid); });
  ByteBuffer payload = DiagnosticPage(BtreePageType::kLeafTable, 511);
  payload.mutable_view()[511] = std::byte{0x80};
  check("payload varint propagation", [&] { return first_cell(payload); });
  const ByteBuffer rowid = DiagnosticPage(BtreePageType::kLeafTable, 511);
  check("table rowid propagation", [&] { return first_cell(rowid); });
  ByteBuffer first_overflow = DiagnosticPage(BtreePageType::kLeafTable, 466);
  first_overflow.mutable_view()[466] = std::byte{0x83};
  first_overflow.mutable_view()[467] = std::byte{0x74};
  first_overflow.mutable_view()[468] = std::byte{1};
  check("first overflow propagation", [&] { return first_cell(first_overflow); });

  ByteBuffer chain = DiagnosticPage(BtreePageType::kLeafTable);
  Write32(chain, 0, 0xffffffffU);
  check("overflow link propagation",
        [&] { return OverflowPageView::Parse(chain.view(), geometry); });
  check("freelist link propagation",
        [&] { return FreelistTrunkView::Parse(chain.view(), geometry); });
  ByteBuffer trunk = DiagnosticPage(BtreePageType::kLeafTable);
  Write32(trunk, 0, 0);
  Write32(trunk, 4, 1);
  check("freelist leaf propagation",
        [&] { return FreelistTrunkView::Parse(trunk.view(), geometry); });
  ByteBuffer map = DiagnosticPage(BtreePageType::kLeafTable);
  std::ranges::fill(map.mutable_view(), std::byte{0});
  map.mutable_view()[0] = static_cast<std::byte>(PointerMapType::kBtreeChild);
  check(
      "pointer map parse propagation",
      [&] { return PointerMapView::Parse(map.view(), PageNumber{0}, geometry); },
      ErrorCode::kMisuse);
  const auto pointer_map = TakeValue(PointerMapView::Parse(map.view(), PageNumber{2}, geometry));
  check(
      "pointer map target propagation", [&] { return pointer_map.entry(PageNumber{0}); },
      ErrorCode::kMisuse);
  check("pointer map parent propagation", [&] { return pointer_map.entry(PageNumber{3}); });
  return correct;
}

[[nodiscard]] Outcome RunCase(bool wide_header, bool seek, std::size_t requested_fields,
                              std::optional<std::size_t> failure,
                              std::optional<modern_sqlite::ErrorCode> expected_error) {
  using namespace modern_sqlite;
  failing_allocation.reset();
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<BtreeDatabase> pager_database = TakeValue(BtreeDatabase::OpenEphemeral(
      vfs, PagerOptions{
               .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
               .cache_capacity_pages = 32,
           }));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  const std::array<IndexColumnOrder, 3> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  const std::array<SqlValue, 3> key{
      SqlValue::Text(std::string(1024, 'a')),
      SqlValue::Blob(ByteBuffer{ByteCount{8192}}),
      SqlValue::Integer(1),
  };
  PageNumber root;
  {
    std::vector<SqlValue> fields(wide_header ? 200U : 2U);
    fields.front() = SqlValue::Text(std::string(1024, 'a'));
    fields.back() = SqlValue::Blob(ByteBuffer{ByteCount{8192}});
    const ByteBuffer record = TakeValue(EncodeRecord(fields));
    BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*pager_database));
    RequireStatus(session.InitializeDatabase());
    IndexBtreeWriter index = TakeValue(session.CreateIndexBtree(std::span{columns}.first(1)));
    root = index.root_page();
    RequireStatus(index.InsertEncoded(record.view()));
  }
  IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager_database, root, columns));
  if (!TakeValue(cursor.First()) || !cursor.CopyPayload().has_value()) {
    return Outcome{.allocations = 0, .invariant_holds = false};
  }

  std::size_t allocations = 0;
  for (std::size_t repetition = 0; repetition < 3U; ++repetition) {
    if (!TakeValue(cursor.First())) {
      return Outcome{.allocations = 0, .invariant_holds = false};
    }
    const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    const OperationResult result = Execute(cursor, std::span{key}.first(requested_fields), seek);
    failing_allocation.reset();
    allocations = allocation_index.load(std::memory_order_relaxed);
    const bool released = live_before == live_allocations.load(std::memory_order_relaxed);
    const bool correct_result = expected_error.has_value()
                                    ? !result.succeeded && result.error == *expected_error
                                    : result.succeeded && result.equivalent;
    const bool correct_position = cursor.valid() == (!expected_error.has_value() || !seek);
    if (!released || !correct_result || !correct_position) {
      std::fprintf(stderr,
                   "cursor allocation invariant: wide=%d seek=%d fields=%zu "
                   "released=%d result=%d position=%d\n",
                   wide_header, seek, requested_fields, released, correct_result, correct_position);
      return Outcome{.allocations = allocations, .invariant_holds = false};
    }
  }
  RequireStatus(cursor.ClearPosition());
  return Outcome{.allocations = allocations, .invariant_holds = true};
}

[[nodiscard]] bool ExhaustAllocations(bool wide_header, bool seek, std::size_t requested_fields,
                                      std::optional<modern_sqlite::ErrorCode> expected_error) {
  const Outcome baseline =
      RunCase(wide_header, seek, requested_fields, std::nullopt, expected_error);
  if (!baseline.invariant_holds || baseline.allocations == 0) {
    return false;
  }
  for (std::size_t failure = 0; failure < baseline.allocations; ++failure) {
    const Outcome outcome = RunCase(wide_header, seek, requested_fields, failure,
                                    modern_sqlite::ErrorCode::kOutOfMemory);
    if (!outcome.invariant_holds) {
      return false;
    }
  }
  return true;
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* memory) noexcept { Deallocate(memory); }
void operator delete[](void* memory) noexcept { Deallocate(memory); }
void operator delete(void* memory, std::size_t) noexcept { Deallocate(memory); }
void operator delete[](void* memory, std::size_t) noexcept { Deallocate(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { Deallocate(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { Deallocate(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { Deallocate(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { Deallocate(memory); }

int main() try {
  if (!CheckComparisonInfoOwnership() || !CheckDatabaseOwnership() ||
      !CheckTablePeerSaveAndRestoreFailures()) {
    return 1;
  }
  for (const bool seek : {false, true}) {
    for (const bool wide_header : {false, true}) {
      if (!ExhaustAllocations(wide_header, seek, 1, std::nullopt)) {
        return 1;
      }
    }
    if (!ExhaustAllocations(false, seek, 3, modern_sqlite::ErrorCode::kCorruption)) {
      return 1;
    }
  }
  const bool opened = CheckOpenFailures();
  const bool copied = CheckPayloadCopyFailures();
  const bool borrowed = CheckCountAndBorrowedPrefixAllocations();
  const bool diagnostics = CheckPageDiagnostics();
  return opened && copied && borrowed && diagnostics ? 0 : 1;
} catch (const std::exception& error) {
  std::fprintf(stderr, "cursor allocation fixture failure: %s\n", error.what());
  return 1;
}
