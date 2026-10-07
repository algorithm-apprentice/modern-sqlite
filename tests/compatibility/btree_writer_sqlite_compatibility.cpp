#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"

namespace {

constexpr std::size_t kPageSize = 512;

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

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-btree-compatibility-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path DatabasePath() const {
    return path_ / "compatibility.sqlite";
  }

  [[nodiscard]] std::filesystem::path SqliteCreatedPath() const {
    return path_ / "sqlite-created.sqlite";
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
    std::string message = error == nullptr ? sqlite3_errmsg(database) : std::string{error};
    sqlite3_free(error);
    throw std::runtime_error(std::move(message));
  }
}

[[nodiscard]] std::int64_t QueryInteger(sqlite3* database, std::string_view sql) {
  Statement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error(sqlite3_errmsg(database));
  }
  return sqlite3_column_int64(statement.get(), 0);
}

[[nodiscard]] std::string QueryText(sqlite3* database, std::string_view sql) {
  Statement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error(sqlite3_errmsg(database));
  }
  const unsigned char* text = sqlite3_column_text(statement.get(), 0);
  if (text == nullptr) {
    throw std::runtime_error("SQLite query returned null text");
  }
  return reinterpret_cast<const char*>(text);
}

[[nodiscard]] modern_sqlite::ByteBuffer TableRecord(std::string value) {
  std::array<modern_sqlite::SqlValue, 2> values{
      modern_sqlite::SqlValue{},
      modern_sqlite::SqlValue::Text(std::move(value)),
  };
  return TakeValue(modern_sqlite::EncodeRecord(values));
}

[[nodiscard]] modern_sqlite::PageNumber CreateWithModernSqlite(const std::filesystem::path& path) {
  modern_sqlite::PosixVfs vfs;
  auto opened = modern_sqlite::Pager::OpenWritable(
      vfs, path.string(),
      modern_sqlite::WritablePagerOptions{
          .pager =
              modern_sqlite::PagerOptions{
                  .empty_database_page_size = modern_sqlite::ByteCount{kPageSize},
                  .cache_capacity_pages = 32,
              },
          .journal =
              modern_sqlite::RollbackJournalOptions{
                  .legacy_page_size = modern_sqlite::ByteCount{kPageSize},
              },
      });
  std::unique_ptr<modern_sqlite::Pager> pager = TakeValue(std::move(opened));
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  modern_sqlite::PageNumber table_root;
  {
    modern_sqlite::BtreeWriteSession session =
        TakeValue(modern_sqlite::BtreeWriteSession::Open(*pager));
    RequireStatus(session.InitializeDatabase());
    modern_sqlite::TableBtreeWriter schema =
        TakeValue(session.OpenTableBtree(modern_sqlite::PageNumber{1}));
    modern_sqlite::TableBtreeWriter table = TakeValue(session.CreateTableBtree());
    table_root = table.root_page();

    const std::array schema_values{
        modern_sqlite::SqlValue::Text("table"),
        modern_sqlite::SqlValue::Text("items"),
        modern_sqlite::SqlValue::Text("items"),
        modern_sqlite::SqlValue::Integer(table_root.value()),
        modern_sqlite::SqlValue::Text(
            "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT NOT NULL)"),
    };
    const modern_sqlite::ByteBuffer schema_record =
        TakeValue(modern_sqlite::EncodeRecord(schema_values));
    RequireStatus(schema.Insert(1, schema_record.view()));
    RequireStatus(table.Insert(1, TableRecord("alpha").view()));
    RequireStatus(table.Insert(2, TableRecord("beta").view()));
    RequireStatus(table.Insert(3, TableRecord("gamma").view()));
  }
  RequireStatus(pager->Commit());
  RequireStatus(pager->EndRead());
  return table_root;
}

void VerifyAndMutateWithSqlite(const std::filesystem::path& path,
                               modern_sqlite::PageNumber table_root) {
  Database database{path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX};
  if (QueryInteger(database.get(), "SELECT rootpage FROM sqlite_schema WHERE name='items'") !=
      table_root.value()) {
    throw std::runtime_error("SQLite read the wrong Modern-created root page");
  }
  if (QueryText(database.get(),
                "SELECT group_concat(id || ':' || name, ',') FROM "
                "(SELECT id,name FROM items ORDER BY id)") != "1:alpha,2:beta,3:gamma") {
    throw std::runtime_error("SQLite rejected Modern-created table rows");
  }
  if (QueryText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error("SQLite integrity_check rejected the Modern-created image");
  }

  Execute(database.get(), "UPDATE items SET name='beta-two' WHERE id=2");
  Execute(database.get(), "DELETE FROM items WHERE id=1");
  Execute(database.get(), "INSERT INTO items(id,name) VALUES(4,'delta')");
  if (QueryText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error("SQLite mutation left an invalid image");
  }
}

void VerifySqliteMutationWithModernSqlite(const std::filesystem::path& path,
                                          modern_sqlite::PageNumber table_root) {
  modern_sqlite::PosixVfs vfs;
  auto opened = modern_sqlite::Pager::Open(
      vfs, path.string(),
      modern_sqlite::PagerOptions{
          .empty_database_page_size = modern_sqlite::ByteCount{kPageSize},
          .cache_capacity_pages = 32,
      });
  std::unique_ptr<modern_sqlite::Pager> pager = TakeValue(std::move(opened));
  RequireStatus(pager->BeginRead());
  {
    modern_sqlite::TableBtreeCursor cursor =
        TakeValue(modern_sqlite::TableBtreeCursor::Open(*pager, table_root));

    const std::array expected{
        std::pair{std::int64_t{2}, std::string{"beta-two"}},
        std::pair{std::int64_t{3}, std::string{"gamma"}},
        std::pair{std::int64_t{4}, std::string{"delta"}},
    };
    if (!TakeValue(cursor.First())) {
      throw std::runtime_error("Modern SQLite found no SQLite-written rows");
    }
    for (std::size_t index = 0U; index < expected.size(); ++index) {
      const auto& [rowid, text] = expected[index];
      if (TakeValue(cursor.rowid()) != rowid) {
        throw std::runtime_error("Modern SQLite read the wrong SQLite-written rowid");
      }
      const modern_sqlite::ByteBuffer actual = TakeValue(cursor.CopyPayload());
      const modern_sqlite::ByteBuffer encoded = TableRecord(text);
      if (!std::ranges::equal(actual.view(), encoded.view())) {
        throw std::runtime_error("Modern SQLite decoded the wrong SQLite-written record");
      }
      const bool has_next = TakeValue(cursor.Next());
      if (has_next != (index + 1U < expected.size())) {
        throw std::runtime_error("Modern SQLite observed an unexpected SQLite-written row count");
      }
    }
  }
  RequireStatus(pager->EndRead());
}

[[nodiscard]] modern_sqlite::PageNumber CreateWithSqlite(const std::filesystem::path& path) {
  Database database{path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX};
  Execute(database.get(), "PRAGMA page_size=512");
  Execute(database.get(), "PRAGMA journal_mode=DELETE");
  Execute(database.get(), "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT NOT NULL)");
  Execute(database.get(), "INSERT INTO items(id,name) VALUES(1,'alpha'),(2,'beta'),(3,'gamma')");
  return modern_sqlite::PageNumber{static_cast<std::uint32_t>(
      QueryInteger(database.get(), "SELECT rootpage FROM sqlite_schema WHERE name='items'"))};
}

void MutateSqliteDatabaseWithModernSqlite(const std::filesystem::path& path,
                                          modern_sqlite::PageNumber table_root) {
  modern_sqlite::PosixVfs vfs;
  auto opened = modern_sqlite::Pager::OpenWritable(
      vfs, path.string(),
      modern_sqlite::WritablePagerOptions{
          .pager =
              modern_sqlite::PagerOptions{
                  .empty_database_page_size = modern_sqlite::ByteCount{kPageSize},
                  .cache_capacity_pages = 32,
              },
          .journal =
              modern_sqlite::RollbackJournalOptions{
                  .legacy_page_size = modern_sqlite::ByteCount{kPageSize},
              },
      });
  std::unique_ptr<modern_sqlite::Pager> pager = TakeValue(std::move(opened));
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  {
    modern_sqlite::BtreeWriteSession session =
        TakeValue(modern_sqlite::BtreeWriteSession::Open(*pager));
    modern_sqlite::TableBtreeWriter table = TakeValue(session.OpenTableBtree(table_root));
    RequireStatus(table.Insert(2, TableRecord("beta-modern").view(),
                               modern_sqlite::BtreeInsertMode::kReplace));
    if (!TakeValue(table.Delete(1))) {
      throw std::runtime_error("Modern SQLite could not delete a SQLite-created row");
    }
    RequireStatus(table.Insert(4, TableRecord("delta").view()));
  }
  RequireStatus(pager->Commit());
  RequireStatus(pager->EndRead());
}

void VerifyModernMutationWithSqlite(const std::filesystem::path& path) {
  Database database{path, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX};
  if (QueryText(database.get(),
                "SELECT group_concat(id || ':' || name, ',') FROM "
                "(SELECT id,name FROM items ORDER BY id)") != "2:beta-modern,3:gamma,4:delta") {
    throw std::runtime_error("SQLite read the wrong Modern-mutated table rows");
  }
  if (QueryText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error("SQLite integrity_check rejected the Modern-mutated image");
  }
}

}  // namespace

int main() try {
  if (sqlite3_libversion_number() != 3'054'000) {
    throw std::runtime_error("compatibility test requires SQLite 3.54.0");
  }
  const TemporaryDirectory directory;
  const std::filesystem::path database_path = directory.DatabasePath();
  const modern_sqlite::PageNumber table_root = CreateWithModernSqlite(database_path);
  if (std::filesystem::exists(database_path.string() + "-journal")) {
    throw std::runtime_error("Modern SQLite left a rollback journal after commit");
  }
  VerifyAndMutateWithSqlite(database_path, table_root);
  VerifySqliteMutationWithModernSqlite(database_path, table_root);

  const std::filesystem::path sqlite_created_path = directory.SqliteCreatedPath();
  const modern_sqlite::PageNumber sqlite_root = CreateWithSqlite(sqlite_created_path);
  MutateSqliteDatabaseWithModernSqlite(sqlite_created_path, sqlite_root);
  VerifyModernMutationWithSqlite(sqlite_created_path);
  return 0;
} catch (const std::exception& error) {
  static_cast<void>(std::fprintf(stderr, "%s\n", error.what()));
  return 1;
}
