#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/transaction/transaction_coordinator.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

constexpr int kReadOnlyFlags = SQLITE_OPEN_READONLY + SQLITE_OPEN_NOMUTEX;
constexpr int kCreateFlags = SQLITE_OPEN_READWRITE + SQLITE_OPEN_CREATE + SQLITE_OPEN_NOMUTEX;

enum class CrashOperation : std::uint8_t {
  kImplicitCommit,
  kExplicitCommit,
  kStatementRollback,
  kNamedRollback,
  kTransactionSavepoint,
  kFullRollback,
};

struct Row {
  std::int64_t rowid;
  std::size_t payload_size;
  std::byte fill;

  bool operator==(const Row&) const = default;
};

struct CrashFixture {
  modern_sqlite::ByteBuffer image;
  modern_sqlite::PageNumber table_root;
  std::vector<Row> rows;
  std::uint32_t freelist_count;
};

struct ExpectedState {
  std::vector<Row> rows;
  std::uint32_t freelist_count;
  bool rollback_only;
};

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

[[nodiscard]] std::string_view OperationName(CrashOperation operation) noexcept {
  switch (operation) {
    case CrashOperation::kImplicitCommit:
      return "implicit-commit";
    case CrashOperation::kExplicitCommit:
      return "explicit-commit";
    case CrashOperation::kStatementRollback:
      return "statement-rollback";
    case CrashOperation::kNamedRollback:
      return "named-rollback";
    case CrashOperation::kTransactionSavepoint:
      return "transaction-savepoint";
    case CrashOperation::kFullRollback:
      return "full-rollback";
  }
  return "unknown";
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-transaction-compatibility-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path FixturePath() const { return path_ / "fixture.sqlite"; }

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

[[nodiscard]] modern_sqlite::ByteBuffer ReadImage(const std::filesystem::path& path) {
  const std::uintmax_t raw_size = std::filesystem::file_size(path);
  if (raw_size > modern_sqlite::test::kWritePagerFileCapacity ||
      raw_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::runtime_error("transaction fixture exceeds deterministic VFS capacity");
  }
  const auto size = static_cast<std::size_t>(raw_size);
  modern_sqlite::ByteBuffer image{modern_sqlite::ByteCount{size}};
  std::ifstream input{path, std::ios::binary};
  input.read(reinterpret_cast<char*>(image.mutable_view().data()),
             static_cast<std::streamsize>(size));
  if (!input) {
    throw std::runtime_error("could not read transaction fixture");
  }
  return image;
}

void WriteImage(const std::filesystem::path& path, modern_sqlite::ByteView image) {
  std::error_code error;
  static_cast<void>(std::filesystem::remove(path, error));
  static_cast<void>(std::filesystem::remove(path.string() + "-journal", error));
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(reinterpret_cast<const char*>(image.data()),
               static_cast<std::streamsize>(image.size()));
  if (!output) {
    throw std::runtime_error("could not write recovered transaction image");
  }
}

[[nodiscard]] CrashFixture CreateFixture(const TemporaryDirectory& directory) {
  const std::filesystem::path path = directory.FixturePath();
  modern_sqlite::PageNumber table_root;
  {
    const Database database{path, kCreateFlags};
    Execute(database.get(), "PRAGMA page_size=512");
    Execute(database.get(), "PRAGMA journal_mode=DELETE");
    Execute(database.get(), "CREATE TABLE items(id INTEGER PRIMARY KEY, data BLOB NOT NULL)");
    Execute(database.get(), "INSERT INTO items VALUES(1,zeroblob(8))");
    table_root = modern_sqlite::PageNumber{static_cast<std::uint32_t>(
        QueryInteger(database.get(), "SELECT rootpage FROM sqlite_schema WHERE name='items'"))};
    if (QueryText(database.get(), "PRAGMA integrity_check") != "ok" ||
        QueryInteger(database.get(), "PRAGMA freelist_count") != 0) {
      throw std::runtime_error("SQLite rejected transaction crash fixture");
    }
  }
  return CrashFixture{
      .image = ReadImage(path),
      .table_root = table_root,
      .rows =
          {
              Row{.rowid = 1, .payload_size = 8U, .fill = std::byte{0}},
          },
      .freelist_count = 0U,
  };
}

[[nodiscard]] modern_sqlite::ByteBuffer TableRecord(std::size_t size, std::byte fill) {
  modern_sqlite::ByteBuffer blob{modern_sqlite::ByteCount{size}};
  std::ranges::fill(blob.mutable_view(), fill);
  std::array<modern_sqlite::SqlValue, 2> values{
      modern_sqlite::SqlValue{},
      modern_sqlite::SqlValue::Blob(std::move(blob)),
  };
  return TakeValue(modern_sqlite::EncodeRecord(values));
}

[[nodiscard]] bool InsertRow(modern_sqlite::TransactionCoordinator& coordinator,
                             modern_sqlite::PageNumber root_page, Row row,
                             modern_sqlite::StatementRollbackMode rollback =
                                 modern_sqlite::StatementRollbackMode::kStatement) {
  auto statement = coordinator.BeginStatement(modern_sqlite::TransactionStatementOptions{
      .access = modern_sqlite::StatementAccess::kWrite,
      .rollback = rollback,
  });
  if (!statement.has_value() || statement->writer() == nullptr) {
    return false;
  }
  auto table = statement->writer()->OpenTableBtree(root_page);
  if (!table.has_value()) {
    return false;
  }
  const modern_sqlite::ByteBuffer payload = TableRecord(row.payload_size, row.fill);
  return table->Insert(row.rowid, payload.view()).has_value() && statement->Succeed().has_value();
}

[[nodiscard]] bool InsertRowWithSpill(modern_sqlite::TransactionCoordinator& coordinator,
                                      const modern_sqlite::test::WritePagerFixedVfs& vfs,
                                      modern_sqlite::PageNumber root_page, Row row) {
  auto statement = coordinator.BeginStatement(modern_sqlite::TransactionStatementOptions{
      .access = modern_sqlite::StatementAccess::kWrite,
  });
  if (!statement.has_value() || statement->writer() == nullptr) {
    return false;
  }
  auto table = statement->writer()->OpenTableBtree(root_page);
  if (!table.has_value()) {
    return false;
  }
  const std::size_t writes_before = vfs.database_write_count();
  const modern_sqlite::ByteBuffer payload = TableRecord(row.payload_size, row.fill);
  return table->Insert(row.rowid, payload.view()).has_value() &&
         vfs.database_write_count() > writes_before && statement->Succeed().has_value();
}

[[nodiscard]] bool ApplyOperation(modern_sqlite::test::WritePagerFixedVfs& vfs,
                                  modern_sqlite::PageNumber root_page, CrashOperation operation) {
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 1U);
  if (pager == nullptr) {
    return false;
  }
  auto opened = modern_sqlite::TransactionCoordinator::Open(std::move(pager));
  if (!opened.has_value()) {
    return false;
  }
  modern_sqlite::TransactionCoordinator coordinator = std::move(*opened);

  switch (operation) {
    case CrashOperation::kImplicitCommit:
      return InsertRow(coordinator, root_page,
                       Row{.rowid = 2, .payload_size = 380U, .fill = std::byte{0x02}});
    case CrashOperation::kExplicitCommit:
      return coordinator.Begin().has_value() &&
             InsertRow(coordinator, root_page,
                       Row{.rowid = 2, .payload_size = 8U, .fill = std::byte{0x02}}) &&
             InsertRow(coordinator, root_page,
                       Row{.rowid = 3, .payload_size = 2'000U, .fill = std::byte{0x03}}) &&
             coordinator.Commit().has_value();
    case CrashOperation::kStatementRollback: {
      if (!coordinator.Begin().has_value() ||
          !InsertRow(coordinator, root_page,
                     Row{.rowid = 2, .payload_size = 8U, .fill = std::byte{0x02}})) {
        return false;
      }
      auto statement = coordinator.BeginStatement(modern_sqlite::TransactionStatementOptions{
          .access = modern_sqlite::StatementAccess::kWrite,
      });
      if (!statement.has_value() || statement->writer() == nullptr) {
        return false;
      }
      auto table = statement->writer()->OpenTableBtree(root_page);
      const modern_sqlite::ByteBuffer payload = TableRecord(2'000U, std::byte{0x03});
      const std::size_t writes_before = vfs.database_write_count();
      if (!table.has_value() || !table->Insert(3, payload.view()).has_value() ||
          vfs.database_write_count() <= writes_before || !statement->Rollback().has_value()) {
        return false;
      }
      return coordinator.Commit().has_value();
    }
    case CrashOperation::kNamedRollback:
      return coordinator.Begin().has_value() &&
             coordinator.Savepoint(modern_sqlite::Utf8View{"s"}).has_value() &&
             InsertRowWithSpill(coordinator, vfs, root_page,
                                Row{.rowid = 2, .payload_size = 2'000U, .fill = std::byte{0x02}}) &&
             coordinator.RollbackTo(modern_sqlite::Utf8View{"s"}).has_value() &&
             InsertRow(coordinator, root_page,
                       Row{.rowid = 3, .payload_size = 8U, .fill = std::byte{0x03}}) &&
             coordinator.Commit().has_value();
    case CrashOperation::kTransactionSavepoint:
      return coordinator.Savepoint(modern_sqlite::Utf8View{"outer"}).has_value() &&
             InsertRow(coordinator, root_page,
                       Row{.rowid = 2, .payload_size = 380U, .fill = std::byte{0x02}}) &&
             coordinator.Release(modern_sqlite::Utf8View{"outer"}).has_value();
    case CrashOperation::kFullRollback: {
      if (!coordinator.Begin().has_value()) {
        return false;
      }
      auto statement = coordinator.BeginStatement(modern_sqlite::TransactionStatementOptions{
          .access = modern_sqlite::StatementAccess::kWrite,
          .rollback = modern_sqlite::StatementRollbackMode::kTransaction,
      });
      if (!statement.has_value() || statement->writer() == nullptr) {
        return false;
      }
      auto table = statement->writer()->OpenTableBtree(root_page);
      const modern_sqlite::ByteBuffer payload = TableRecord(2'000U, std::byte{0x02});
      return table.has_value() && table->Insert(2, payload.view()).has_value() &&
             vfs.database_write_count() > 0U && statement->Rollback().has_value();
    }
  }
  return false;
}

[[nodiscard]] ExpectedState TerminalState(const CrashFixture& fixture, CrashOperation operation) {
  std::vector<Row> rows = fixture.rows;
  switch (operation) {
    case CrashOperation::kImplicitCommit:
    case CrashOperation::kTransactionSavepoint:
      rows.push_back(Row{.rowid = 2, .payload_size = 380U, .fill = std::byte{0x02}});
      break;
    case CrashOperation::kExplicitCommit:
      rows.push_back(Row{.rowid = 2, .payload_size = 8U, .fill = std::byte{0x02}});
      rows.push_back(Row{.rowid = 3, .payload_size = 2'000U, .fill = std::byte{0x03}});
      break;
    case CrashOperation::kStatementRollback:
      rows.push_back(Row{.rowid = 2, .payload_size = 8U, .fill = std::byte{0x02}});
      break;
    case CrashOperation::kNamedRollback:
      rows.push_back(Row{.rowid = 3, .payload_size = 8U, .fill = std::byte{0x03}});
      break;
    case CrashOperation::kFullRollback:
      break;
  }
  return ExpectedState{
      .rows = std::move(rows),
      .freelist_count = 0U,
      .rollback_only = operation == CrashOperation::kFullRollback,
  };
}

void RecoverOnce(modern_sqlite::test::WritePagerFixedVfs& vfs) {
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error("could not reopen transaction crash pager");
  }
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->EndRead());
}

[[nodiscard]] std::uint32_t FreelistCount(modern_sqlite::ByteView image) {
  if (image.size() < 40U) {
    throw std::runtime_error("transaction image has no database header");
  }
  return modern_sqlite::LoadBigEndian<std::uint32_t>(
      std::span<const std::byte, 4>{image.data() + 36U, 4U});
}

void VerifyModernState(modern_sqlite::ByteView image, modern_sqlite::PageNumber root_page,
                       const std::vector<Row>& expected, std::uint32_t expected_freelist) {
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  vfs.LoadDatabase(image);
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error("Modern SQLite could not inspect transaction image");
  }
  RequireStatus(pager->BeginRead());
  if (FreelistCount(image) != expected_freelist || pager->header() == nullptr ||
      pager->header()->freelist_page_count() != expected_freelist) {
    throw std::runtime_error("Modern SQLite read the wrong transaction freelist");
  }
  {
    modern_sqlite::TableBtreeCursor cursor =
        TakeValue(modern_sqlite::TableBtreeCursor::Open(*pager, root_page));
    bool has_row = TakeValue(cursor.First());
    for (const Row& row : expected) {
      if (!has_row || TakeValue(cursor.rowid()) != row.rowid) {
        throw std::runtime_error("Modern SQLite read the wrong transaction rowid");
      }
      const modern_sqlite::ByteBuffer payload = TakeValue(cursor.CopyPayload());
      const modern_sqlite::ByteBuffer encoded = TableRecord(row.payload_size, row.fill);
      if (!std::ranges::equal(payload.view(), encoded.view())) {
        throw std::runtime_error("Modern SQLite read the wrong transaction payload");
      }
      has_row = TakeValue(cursor.Next());
    }
    if (has_row) {
      throw std::runtime_error("Modern SQLite read extra transaction rows");
    }
  }
  RequireStatus(pager->EndRead());
}

void VerifySqliteRows(sqlite3* database, const std::vector<Row>& expected) {
  const Statement statement{database, "SELECT id,data FROM items ORDER BY id"};
  for (const Row& row : expected) {
    if (sqlite3_step(statement.get()) != SQLITE_ROW ||
        sqlite3_column_int64(statement.get(), 0) != row.rowid) {
      throw std::runtime_error("SQLite read the wrong recovered transaction rowid");
    }
    const int byte_count = sqlite3_column_bytes(statement.get(), 1);
    const void* blob = sqlite3_column_blob(statement.get(), 1);
    if (byte_count < 0 || std::cmp_not_equal(byte_count, row.payload_size) ||
        (byte_count > 0 && blob == nullptr)) {
      throw std::runtime_error("SQLite read the wrong recovered transaction payload size");
    }
    const auto* bytes = static_cast<const std::byte*>(blob);
    if (!std::ranges::all_of(std::span<const std::byte>{bytes, row.payload_size},
                             [fill = row.fill](std::byte value) { return value == fill; })) {
      throw std::runtime_error("SQLite read the wrong recovered transaction payload");
    }
  }
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    throw std::runtime_error("SQLite read extra recovered transaction rows");
  }
}

void VerifySqliteState(const TemporaryDirectory& directory, CrashOperation operation,
                       modern_sqlite::ByteView image, const std::vector<Row>& expected,
                       std::uint32_t expected_freelist) {
  const std::filesystem::path path = directory.RecoveredPath(operation);
  WriteImage(path, image);
  const Database database{path, kReadOnlyFlags};
  if (QueryText(database.get(), "PRAGMA integrity_check") != "ok" ||
      QueryInteger(database.get(), "PRAGMA freelist_count") != expected_freelist ||
      FreelistCount(image) != expected_freelist) {
    throw std::runtime_error("SQLite rejected recovered transaction image");
  }
  VerifySqliteRows(database.get(), expected);
}

void VerifyCrashCuts(const TemporaryDirectory& directory, const CrashFixture& fixture,
                     CrashOperation operation) {
  const ExpectedState terminal = TerminalState(fixture, operation);
  for (const bool writes_are_durable : {false, true}) {
    modern_sqlite::test::WritePagerFixedVfs baseline{false};
    baseline.LoadDatabase(fixture.image.view());
    baseline.SetDatabaseWritesDurable(writes_are_durable);
    baseline.ArmCrashCut(std::nullopt);
    if (!ApplyOperation(baseline, fixture.table_root, operation)) {
      throw std::runtime_error(std::string{OperationName(operation)} +
                               " could not create committed baseline");
    }
    const modern_sqlite::ByteBuffer committed_image =
        modern_sqlite::ByteBuffer::CopyOf(baseline.database_bytes());
    const std::size_t mutation_count = baseline.mutation_count();
    if (mutation_count == 0U) {
      throw std::runtime_error("transaction crash operation performed no mutation");
    }
    const bool terminal_matches_old =
        std::ranges::equal(committed_image.view(), fixture.image.view());
    if (terminal.rollback_only != terminal_matches_old) {
      throw std::runtime_error("transaction terminal image has the wrong rollback classification");
    }
    VerifyModernState(committed_image.view(), fixture.table_root, terminal.rows,
                      terminal.freelist_count);
    VerifySqliteState(directory, operation, committed_image.view(), terminal.rows,
                      terminal.freelist_count);

    for (std::size_t cut = 1U; cut <= mutation_count; ++cut) {
      modern_sqlite::test::WritePagerFixedVfs vfs{false};
      vfs.LoadDatabase(fixture.image.view());
      vfs.SetDatabaseWritesDurable(writes_are_durable);
      vfs.ArmCrashCut(cut);
      static_cast<void>(ApplyOperation(vfs, fixture.table_root, operation));
      vfs.Crash();
      RecoverOnce(vfs);
      vfs.Crash();
      RecoverOnce(vfs);

      const modern_sqlite::ByteView recovered = vfs.database_bytes();
      const bool old_state = std::ranges::equal(recovered, fixture.image.view());
      const bool committed_state = std::ranges::equal(recovered, committed_image.view());
      if (terminal.rollback_only) {
        if (!old_state) {
          throw std::runtime_error(std::string{OperationName(operation)} +
                                   " failed to restore the original image at cut " +
                                   std::to_string(cut));
        }
        VerifyModernState(recovered, fixture.table_root, fixture.rows, fixture.freelist_count);
        VerifySqliteState(directory, operation, recovered, fixture.rows, fixture.freelist_count);
        continue;
      }
      if (!old_state && !committed_state) {
        throw std::runtime_error(std::string{OperationName(operation)} +
                                 " recovered a partial image at cut " + std::to_string(cut));
      }
      const std::vector<Row>& expected = committed_state ? terminal.rows : fixture.rows;
      const std::uint32_t expected_freelist =
          committed_state ? terminal.freelist_count : fixture.freelist_count;
      VerifyModernState(recovered, fixture.table_root, expected, expected_freelist);
      VerifySqliteState(directory, operation, recovered, expected, expected_freelist);
    }
  }
}

}  // namespace

int main() try {
  if (sqlite3_libversion_number() != 3'054'000) {
    throw std::runtime_error("compatibility test requires SQLite 3.54.0");
  }
  const TemporaryDirectory directory;
  const CrashFixture fixture = CreateFixture(directory);
  for (const CrashOperation operation :
       {CrashOperation::kImplicitCommit, CrashOperation::kExplicitCommit,
        CrashOperation::kStatementRollback, CrashOperation::kNamedRollback,
        CrashOperation::kTransactionSavepoint, CrashOperation::kFullRollback}) {
    VerifyCrashCuts(directory, fixture, operation);
  }
  return 0;
} catch (const std::exception& error) {
  static_cast<void>(std::fprintf(stderr, "%s\n", error.what()));
  return 1;
}
