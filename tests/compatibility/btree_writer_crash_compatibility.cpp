#include "tests/compatibility/btree_writer_crash_compatibility.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite::test {
namespace {

constexpr int kReadOnlyFlags = SQLITE_OPEN_READONLY + SQLITE_OPEN_NOMUTEX;
constexpr int kCreateFlags = SQLITE_OPEN_READWRITE + SQLITE_OPEN_CREATE + SQLITE_OPEN_NOMUTEX;
constexpr std::int64_t kNonRootTargetRowid = 10;
constexpr std::int64_t kReuseTargetRowid = 99;

enum class CrashOperation : std::uint8_t {
  kNonRootBalance,
  kInteriorPredecessorDelete,
  kFreelistReuse,
  kDrop,
};

struct CrashFixture {
  CrashOperation operation;
  ByteBuffer image;
  PageNumber table_root;
  PageNumber index_root;
  std::int64_t target_rowid;
  std::int64_t schema_rowid;
  ByteBuffer target_blob;
};

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

[[nodiscard]] std::string_view OperationName(CrashOperation operation) noexcept {
  switch (operation) {
    case CrashOperation::kNonRootBalance:
      return "non-root-balance";
    case CrashOperation::kInteriorPredecessorDelete:
      return "interior-predecessor-delete";
    case CrashOperation::kFreelistReuse:
      return "freelist-reuse";
    case CrashOperation::kDrop:
      return "drop";
  }
  return "unknown";
}

[[nodiscard]] std::int64_t ExpectedFreelistCount(CrashOperation operation,
                                                 bool committed) noexcept {
  switch (operation) {
    case CrashOperation::kNonRootBalance:
    case CrashOperation::kInteriorPredecessorDelete:
      return 0;
    case CrashOperation::kFreelistReuse:
      return committed ? 0 : 1;
    case CrashOperation::kDrop:
      return committed ? 1 : 0;
  }
  return -1;
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-btree-crash-compatibility-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path FixturePath(CrashOperation operation) const {
    return path_ / (std::string{OperationName(operation)} + ".sqlite");
  }

  [[nodiscard]] std::filesystem::path RecoveredPath(CrashOperation operation) const {
    return path_ / (std::string{OperationName(operation)} + "-recovered.sqlite");
  }

 private:
  std::filesystem::path path_;
};

class Database final {
 public:
  Database(const std::filesystem::path& path, int flags) {
    const int result = sqlite3_open_v2(path.string().c_str(), &database_, flags, nullptr);
    if (result != SQLITE_OK) {
      const std::string message =
          database_ == nullptr ? "sqlite3_open_v2 failed" : sqlite3_errmsg(database_);
      if (database_ != nullptr) {
        static_cast<void>(sqlite3_close(database_));
        database_ = nullptr;
      }
      throw std::runtime_error(message);
    }
  }

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  ~Database() noexcept {
    if (database_ != nullptr) {
      static_cast<void>(sqlite3_close(database_));
    }
  }

  [[nodiscard]] sqlite3* get() const noexcept { return database_; }

 private:
  sqlite3* database_ = nullptr;
};

class Statement final {
 public:
  Statement(sqlite3* database, std::string_view sql) {
    const std::string owned_sql{sql};
    const int result = sqlite3_prepare_v2(database, owned_sql.c_str(), -1, &statement_, nullptr);
    if (result != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(database));
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  ~Statement() noexcept {
    if (statement_ != nullptr) {
      static_cast<void>(sqlite3_finalize(statement_));
    }
  }

  [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

void Execute(sqlite3* database, std::string_view sql) {
  const std::string owned_sql{sql};
  char* error = nullptr;
  const int result = sqlite3_exec(database, owned_sql.c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string message = error == nullptr ? sqlite3_errmsg(database) : std::string{error};
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

[[nodiscard]] std::int64_t QueryInteger(sqlite3* database, std::string_view sql) {
  const Statement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error(sqlite3_errmsg(database));
  }
  return static_cast<std::int64_t>(sqlite3_column_int64(statement.get(), 0));
}

[[nodiscard]] std::string QueryText(sqlite3* database, std::string_view sql) {
  const Statement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error(sqlite3_errmsg(database));
  }
  const unsigned char* text = sqlite3_column_text(statement.get(), 0);
  if (text == nullptr) {
    throw std::runtime_error("SQLite query returned null text");
  }
  return reinterpret_cast<const char*>(text);
}

[[nodiscard]] PageNumber QueryPageNumber(sqlite3* database, std::string_view sql) {
  const std::int64_t value = QueryInteger(database, sql);
  if (value <= 0 || !std::in_range<std::uint32_t>(value)) {
    throw std::runtime_error("SQLite returned an invalid root page");
  }
  return PageNumber{static_cast<std::uint32_t>(value)};
}

[[nodiscard]] ByteBuffer ReadImage(const std::filesystem::path& path) {
  const std::uintmax_t raw_size = std::filesystem::file_size(path);
  if (raw_size > kWritePagerFileCapacity ||
      raw_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::runtime_error("crash fixture exceeds the deterministic VFS capacity");
  }
  const auto size = static_cast<std::size_t>(raw_size);
  ByteBuffer image{ByteCount{size}};
  std::ifstream input{path, std::ios::binary};
  input.read(reinterpret_cast<char*>(image.mutable_view().data()),
             static_cast<std::streamsize>(size));
  if (!input) {
    throw std::runtime_error("could not read crash fixture");
  }
  return image;
}

void WriteImage(const std::filesystem::path& path, ByteView image) {
  std::error_code error;
  static_cast<void>(std::filesystem::remove(path, error));
  static_cast<void>(std::filesystem::remove(path.string() + "-journal", error));
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(reinterpret_cast<const char*>(image.data()),
               static_cast<std::streamsize>(image.size()));
  if (!output) {
    throw std::runtime_error("could not write recovered crash image");
  }
}

void ConfigureFixture(sqlite3* database) {
  Execute(database, "PRAGMA page_size=512");
  Execute(database, "PRAGMA journal_mode=DELETE");
}

[[nodiscard]] CrashFixture CreateFixtureFile(const TemporaryDirectory& directory,
                                             CrashOperation operation) {
  const std::filesystem::path path = directory.FixturePath(operation);
  PageNumber table_root;
  PageNumber index_root;
  std::int64_t schema_rowid = 0;
  {
    const Database database{path, kCreateFlags};
    ConfigureFixture(database.get());
    switch (operation) {
      case CrashOperation::kNonRootBalance:
        Execute(database.get(), "CREATE TABLE items(id INTEGER PRIMARY KEY, data BLOB NOT NULL)");
        Execute(database.get(), "BEGIN");
        for (std::int64_t rowid = 1; rowid <= 17; rowid += 2) {
          Execute(database.get(),
                  "INSERT INTO items VALUES(" + std::to_string(rowid) + ",zeroblob(380))");
        }
        Execute(database.get(), "COMMIT");
        table_root = QueryPageNumber(database.get(),
                                     "SELECT rootpage FROM sqlite_schema WHERE name='items'");
        break;
      case CrashOperation::kInteriorPredecessorDelete:
        Execute(database.get(), "CREATE TABLE items(id INTEGER PRIMARY KEY, key BLOB NOT NULL)");
        Execute(database.get(), "CREATE INDEX idx_items_key ON items(key)");
        Execute(database.get(), "BEGIN");
        for (std::int64_t rowid = 1; rowid <= 24; ++rowid) {
          Execute(database.get(), "INSERT INTO items VALUES(" + std::to_string(rowid) +
                                      ",CAST(printf('%070d'," + std::to_string(rowid) +
                                      ") AS BLOB))");
        }
        Execute(database.get(), "COMMIT");
        table_root = QueryPageNumber(database.get(),
                                     "SELECT rootpage FROM sqlite_schema WHERE name='items'");
        index_root = QueryPageNumber(
            database.get(), "SELECT rootpage FROM sqlite_schema WHERE name='idx_items_key'");
        break;
      case CrashOperation::kFreelistReuse:
        Execute(database.get(), "CREATE TABLE items(id INTEGER PRIMARY KEY, data BLOB NOT NULL)");
        Execute(database.get(), "INSERT INTO items VALUES(1,zeroblob(8))");
        Execute(database.get(), "CREATE TABLE scratch(value)");
        Execute(database.get(), "DROP TABLE scratch");
        table_root = QueryPageNumber(database.get(),
                                     "SELECT rootpage FROM sqlite_schema WHERE name='items'");
        break;
      case CrashOperation::kDrop:
        Execute(database.get(), "CREATE TABLE keeper(id INTEGER PRIMARY KEY)");
        Execute(database.get(), "CREATE TABLE doomed(id INTEGER PRIMARY KEY, data BLOB NOT NULL)");
        Execute(database.get(), "INSERT INTO doomed VALUES(1,zeroblob(8))");
        table_root = QueryPageNumber(database.get(),
                                     "SELECT rootpage FROM sqlite_schema WHERE name='doomed'");
        schema_rowid =
            QueryInteger(database.get(), "SELECT rowid FROM sqlite_schema WHERE name='doomed'");
        break;
    }
    if (QueryText(database.get(), "PRAGMA integrity_check") != "ok") {
      throw std::runtime_error("SQLite rejected a generated crash fixture");
    }
    if (QueryInteger(database.get(), "PRAGMA freelist_count") !=
        ExpectedFreelistCount(operation, false)) {
      throw std::runtime_error("generated crash fixture has the wrong freelist count");
    }
  }
  return CrashFixture{
      .operation = operation,
      .image = ReadImage(path),
      .table_root = table_root,
      .index_root = index_root,
      .target_rowid =
          operation == CrashOperation::kNonRootBalance ? kNonRootTargetRowid : kReuseTargetRowid,
      .schema_rowid = schema_rowid,
      .target_blob = {},
  };
}

void ValidateNonRootFixture(const CrashFixture& fixture) {
  WritePagerFixedVfs vfs{false};
  vfs.LoadDatabase(fixture.image.view());
  std::unique_ptr<Pager> pager = OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error("could not open non-root crash fixture");
  }
  RequireStatus(pager->BeginRead());
  {
    const DatabaseHeader* header = pager->header();
    if (header == nullptr) {
      throw std::runtime_error("non-root crash fixture has no database header");
    }
    const BtreePageGeometry geometry =
        TakeValue(BtreePageGeometry::Create(header->page_size(), header->usable_size()));
    const auto root_pin = TakeValue(pager->ReadPage(fixture.table_root));
    const BtreePageView root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), fixture.table_root, geometry));
    if (root.type() != BtreePageType::kInteriorTable || root.cell_count() < 3U) {
      throw std::runtime_error("non-root crash fixture does not have enough root children");
    }
    std::size_t child_index = root.cell_count();
    for (std::size_t index = 0U; index < root.cell_count(); ++index) {
      const auto separator = TakeValue(root.cell(index)).rowid();
      if (!separator.has_value()) {
        throw std::runtime_error("table root separator has no rowid");
      }
      if (fixture.target_rowid <= *separator) {
        child_index = index;
        break;
      }
    }
    if (child_index == 0U || child_index >= root.cell_count()) {
      throw std::runtime_error("non-root crash target is not in a middle sibling");
    }
  }
  RequireStatus(pager->EndRead());
}

void DiscoverInteriorDeleteTarget(CrashFixture& fixture) {
  WritePagerFixedVfs vfs{false};
  vfs.LoadDatabase(fixture.image.view());
  std::unique_ptr<Pager> pager = OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error("could not open interior-delete crash fixture");
  }
  RequireStatus(pager->BeginRead());
  {
    const DatabaseHeader* header = pager->header();
    if (header == nullptr) {
      throw std::runtime_error("interior-delete crash fixture has no database header");
    }
    const BtreePageGeometry geometry =
        TakeValue(BtreePageGeometry::Create(header->page_size(), header->usable_size()));
    const auto root_pin = TakeValue(pager->ReadPage(fixture.index_root));
    const BtreePageView root =
        TakeValue(BtreePageView::Parse(root_pin.frame().bytes(), fixture.index_root, geometry));
    if (root.type() != BtreePageType::kInteriorIndex || root.cell_count() == 0U) {
      throw std::runtime_error("interior-delete crash fixture has no interior index record");
    }
    const BtreeCellView divider = TakeValue(root.cell(root.cell_count() / 2U));
    if (divider.payload_size().value() != divider.local_payload().size()) {
      throw std::runtime_error("interior-delete target unexpectedly uses overflow");
    }
    const RecordView record = TakeValue(RecordView::Parse(divider.local_payload()));
    const auto blob = TakeValue(record.field(0U)).blob_value();
    const auto rowid = TakeValue(record.field(1U)).integer_value();
    if (!blob.has_value() || !rowid.has_value()) {
      throw std::runtime_error("interior-delete target has the wrong record shape");
    }
    fixture.target_blob = ByteBuffer::CopyOf(*blob);
    fixture.target_rowid = *rowid;
  }
  RequireStatus(pager->EndRead());
}

[[nodiscard]] ByteBuffer TableBlobRecord(std::size_t size, std::byte fill) {
  ByteBuffer blob{ByteCount{size}};
  std::ranges::fill(blob.mutable_view(), fill);
  std::array<SqlValue, 2> values{
      SqlValue{},
      SqlValue::Blob(std::move(blob)),
  };
  return TakeValue(EncodeRecord(values));
}

[[nodiscard]] std::array<IndexColumnOrder, 2> IndexColumns() {
  return {
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
}

[[nodiscard]] std::array<SqlValue, 2> InteriorTarget(const CrashFixture& fixture) {
  return {
      SqlValue::Blob(fixture.target_blob.Clone()),
      SqlValue::Integer(fixture.target_rowid),
  };
}

[[nodiscard]] bool ApplyCrashOperation(WritePagerFixedVfs& vfs, const CrashFixture& fixture) {
  std::unique_ptr<Pager> pager = OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return false;
  }
  auto session = BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return false;
  }
  switch (fixture.operation) {
    case CrashOperation::kNonRootBalance: {
      auto table = session->OpenTableBtree(fixture.table_root);
      if (!table.has_value()) {
        return false;
      }
      const ByteBuffer payload = TableBlobRecord(380U, std::byte{0x0a});
      if (!table->Insert(fixture.target_rowid, payload.view()).has_value()) {
        return false;
      }
      break;
    }
    case CrashOperation::kInteriorPredecessorDelete: {
      auto table = session->OpenTableBtree(fixture.table_root);
      const auto columns = IndexColumns();
      auto index = session->OpenIndexBtree(fixture.index_root, columns);
      if (!table.has_value() || !index.has_value()) {
        return false;
      }
      auto target = InteriorTarget(fixture);
      const auto deleted_index = index->Delete(target);
      const auto deleted_table = table->Delete(fixture.target_rowid);
      if (!deleted_index.has_value() || !*deleted_index || !deleted_table.has_value() ||
          !*deleted_table) {
        return false;
      }
      break;
    }
    case CrashOperation::kFreelistReuse: {
      auto table = session->OpenTableBtree(fixture.table_root);
      if (!table.has_value()) {
        return false;
      }
      const ByteBuffer payload = TableBlobRecord(2'000U, std::byte{0x63});
      if (!table->Insert(fixture.target_rowid, payload.view()).has_value()) {
        return false;
      }
      break;
    }
    case CrashOperation::kDrop: {
      auto schema = session->OpenTableBtree(PageNumber{1});
      auto table = session->OpenTableBtree(fixture.table_root);
      if (!schema.has_value() || !table.has_value()) {
        return false;
      }
      const auto deleted_schema = schema->Delete(fixture.schema_rowid);
      if (!deleted_schema.has_value() || !*deleted_schema || !table->Drop().has_value()) {
        return false;
      }
      break;
    }
  }
  if (!pager->Commit().has_value()) {
    return false;
  }
  return pager->EndRead().has_value();
}

void RecoverOnce(WritePagerFixedVfs& vfs) {
  std::unique_ptr<Pager> pager = OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error("could not reopen crashed pager");
  }
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->EndRead());
}

void VerifyModernState(const CrashFixture& fixture, ByteView image, bool committed) {
  WritePagerFixedVfs vfs{false};
  vfs.LoadDatabase(image);
  std::unique_ptr<Pager> pager = OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error("Modern SQLite could not inspect a recovered image");
  }
  RequireStatus(pager->BeginRead());
  if (pager->header() == nullptr ||
      pager->header()->freelist_page_count() !=
          static_cast<std::uint32_t>(ExpectedFreelistCount(fixture.operation, committed))) {
    throw std::runtime_error("Modern SQLite read the wrong recovered freelist count");
  }

  switch (fixture.operation) {
    case CrashOperation::kNonRootBalance:
    case CrashOperation::kFreelistReuse: {
      TableBtreeCursor table = TakeValue(TableBtreeCursor::Open(*pager, fixture.table_root));
      if (TakeValue(table.Seek(fixture.target_rowid, BtreeSeekMode::kEqual)) != committed) {
        throw std::runtime_error("Modern SQLite read the wrong recovered table state");
      }
      break;
    }
    case CrashOperation::kInteriorPredecessorDelete: {
      TableBtreeCursor table = TakeValue(TableBtreeCursor::Open(*pager, fixture.table_root));
      const bool table_has_target =
          TakeValue(table.Seek(fixture.target_rowid, BtreeSeekMode::kEqual));
      const auto columns = IndexColumns();
      IndexBtreeCursor index =
          TakeValue(IndexBtreeCursor::Open(*pager, fixture.index_root, columns));
      auto target = InteriorTarget(fixture);
      const bool index_has_target = TakeValue(index.Seek(target, BtreeSeekMode::kEqual));
      if (table_has_target == committed || index_has_target == committed) {
        throw std::runtime_error("Modern SQLite read a partial interior-delete state");
      }
      break;
    }
    case CrashOperation::kDrop: {
      TableBtreeCursor schema = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
      if (TakeValue(schema.Seek(fixture.schema_rowid, BtreeSeekMode::kEqual)) == committed) {
        throw std::runtime_error("Modern SQLite read the wrong recovered schema state");
      }
      const auto dropped = TableBtreeCursor::Open(*pager, fixture.table_root);
      if (dropped.has_value() == committed) {
        throw std::runtime_error("Modern SQLite read the wrong recovered dropped-root state");
      }
      break;
    }
  }
  RequireStatus(pager->EndRead());
}

void VerifySqliteState(const TemporaryDirectory& directory, const CrashFixture& fixture,
                       ByteView image, bool committed) {
  const std::filesystem::path path = directory.RecoveredPath(fixture.operation);
  WriteImage(path, image);
  const Database database{path, kReadOnlyFlags};
  if (QueryText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error("SQLite integrity_check rejected a recovered image");
  }
  if (QueryInteger(database.get(), "PRAGMA freelist_count") !=
      ExpectedFreelistCount(fixture.operation, committed)) {
    throw std::runtime_error("SQLite read the wrong recovered freelist count");
  }

  switch (fixture.operation) {
    case CrashOperation::kNonRootBalance:
      if (QueryInteger(database.get(), "SELECT count(*) FROM items") != (committed ? 10 : 9) ||
          QueryInteger(database.get(), "SELECT count(*) FROM items WHERE id=10") !=
              (committed ? 1 : 0)) {
        throw std::runtime_error("SQLite read the wrong recovered non-root balance state");
      }
      break;
    case CrashOperation::kInteriorPredecessorDelete:
      if (QueryInteger(database.get(), "SELECT count(*) FROM items INDEXED BY idx_items_key") !=
              (committed ? 23 : 24) ||
          QueryInteger(database.get(),
                       "SELECT count(*) FROM items INDEXED BY idx_items_key WHERE id=" +
                           std::to_string(fixture.target_rowid)) != (committed ? 0 : 1)) {
        throw std::runtime_error("SQLite read the wrong recovered interior-delete state");
      }
      break;
    case CrashOperation::kFreelistReuse:
      if (QueryInteger(database.get(), "SELECT count(*) FROM items") != (committed ? 2 : 1) ||
          QueryInteger(database.get(),
                       "SELECT count(*) FROM items WHERE id=99 AND length(data)=2000") !=
              (committed ? 1 : 0)) {
        throw std::runtime_error("SQLite read the wrong recovered freelist-reuse state");
      }
      break;
    case CrashOperation::kDrop:
      if (QueryInteger(database.get(), "SELECT count(*) FROM sqlite_schema WHERE name='doomed'") !=
          (committed ? 0 : 1)) {
        throw std::runtime_error("SQLite read the wrong recovered drop state");
      }
      break;
  }
}

void VerifyCrashCuts(const TemporaryDirectory& directory, const CrashFixture& fixture) {
  for (const bool writes_are_durable : {false, true}) {
    WritePagerFixedVfs baseline{false};
    baseline.LoadDatabase(fixture.image.view());
    baseline.SetDatabaseWritesDurable(writes_are_durable);
    baseline.ArmCrashCut(std::nullopt);
    if (!ApplyCrashOperation(baseline, fixture)) {
      throw std::runtime_error("could not create the committed crash baseline");
    }
    const ByteBuffer committed_image = ByteBuffer::CopyOf(baseline.database_bytes());
    const std::size_t mutation_count = baseline.mutation_count();
    if (mutation_count == 0U) {
      throw std::runtime_error("crash operation performed no persistent mutation");
    }
    VerifyModernState(fixture, committed_image.view(), true);
    VerifySqliteState(directory, fixture, committed_image.view(), true);

    for (std::size_t cut = 1U; cut <= mutation_count; ++cut) {
      WritePagerFixedVfs vfs{false};
      vfs.LoadDatabase(fixture.image.view());
      vfs.SetDatabaseWritesDurable(writes_are_durable);
      vfs.ArmCrashCut(cut);
      static_cast<void>(ApplyCrashOperation(vfs, fixture));
      vfs.Crash();
      RecoverOnce(vfs);
      vfs.Crash();
      RecoverOnce(vfs);

      const ByteView recovered = vfs.database_bytes();
      const bool old_state = std::ranges::equal(recovered, fixture.image.view());
      const bool committed_state = std::ranges::equal(recovered, committed_image.view());
      if (!old_state && !committed_state) {
        throw std::runtime_error(std::string{OperationName(fixture.operation)} +
                                 " recovered a partial image at cut " + std::to_string(cut));
      }
      VerifyModernState(fixture, recovered, committed_state);
      VerifySqliteState(directory, fixture, recovered, committed_state);
    }
  }
}

}  // namespace

void RunBtreeWriterCrashCompatibility() {
  const TemporaryDirectory directory;
  for (const CrashOperation operation :
       {CrashOperation::kNonRootBalance, CrashOperation::kInteriorPredecessorDelete,
        CrashOperation::kFreelistReuse, CrashOperation::kDrop}) {
    CrashFixture fixture = CreateFixtureFile(directory, operation);
    if (operation == CrashOperation::kNonRootBalance) {
      ValidateNonRootFixture(fixture);
    } else if (operation == CrashOperation::kInteriorPredecessorDelete) {
      DiscoverInteriorDeleteTarget(fixture);
    }
    VerifyModernState(fixture, fixture.image.view(), false);
    VerifySqliteState(directory, fixture, fixture.image.view(), false);
    VerifyCrashCuts(directory, fixture);
  }
}

}  // namespace modern_sqlite::test
