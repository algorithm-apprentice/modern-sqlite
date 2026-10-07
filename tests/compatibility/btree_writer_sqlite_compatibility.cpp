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
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"

namespace {

constexpr std::array<std::size_t, 8> kPageSizes{
    512U, 1024U, 2048U, 4096U, 8192U, 16384U, 32768U, 65536U,
};
constexpr std::array<std::size_t, 2> kReservedByteCounts{0U, 8U};
constexpr std::int64_t kExpectedFreelistCount = 1;
constexpr int kReadOnlyFlags = SQLITE_OPEN_READONLY + SQLITE_OPEN_NOMUTEX;
constexpr int kReadWriteFlags = SQLITE_OPEN_READWRITE + SQLITE_OPEN_NOMUTEX;
constexpr int kCreateFlags = SQLITE_OPEN_READWRITE + SQLITE_OPEN_CREATE + SQLITE_OPEN_NOMUTEX;
constexpr std::string_view kTableSql =
    "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT NOT NULL)";
constexpr std::string_view kIndexSql = "CREATE INDEX idx_items_name ON items(name)";

struct DatabaseGeometry {
  std::size_t page_size;
  std::size_t reserved_bytes;
};

struct BtreeRoots {
  modern_sqlite::PageNumber table;
  modern_sqlite::PageNumber index;
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

[[nodiscard]] std::string GeometryDescription(DatabaseGeometry geometry) {
  return "page-size=" + std::to_string(geometry.page_size) +
         ", reserved-bytes=" + std::to_string(geometry.reserved_bytes);
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

  [[nodiscard]] std::filesystem::path DatabasePath(std::string_view creator,
                                                   DatabaseGeometry geometry) const {
    return path_ / (std::string{creator} + "-" + std::to_string(geometry.page_size) + "-" +
                    std::to_string(geometry.reserved_bytes) + ".sqlite");
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
  return sqlite3_column_int64(statement.get(), 0);
}

[[nodiscard]] std::string QueryText(sqlite3* database, std::string_view sql, int column = 0) {
  const Statement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error(sqlite3_errmsg(database));
  }
  const unsigned char* text = sqlite3_column_text(statement.get(), column);
  if (text == nullptr) {
    throw std::runtime_error("SQLite query returned null text");
  }
  return reinterpret_cast<const char*>(text);
}

void SetReservedBytes(sqlite3* database, std::size_t reserved_bytes) {
  int requested = static_cast<int>(reserved_bytes);
  if (sqlite3_file_control(database, "main", SQLITE_FCNTL_RESERVE_BYTES, &requested) != SQLITE_OK) {
    throw std::runtime_error("SQLite could not configure reserved bytes");
  }
}

[[nodiscard]] std::int64_t QueryReservedBytes(sqlite3* database) {
  int requested = -1;
  if (sqlite3_file_control(database, "main", SQLITE_FCNTL_RESERVE_BYTES, &requested) != SQLITE_OK) {
    throw std::runtime_error("SQLite could not query reserved bytes");
  }
  return requested;
}

[[nodiscard]] modern_sqlite::ByteBuffer TableRecord(std::string_view value) {
  std::array<modern_sqlite::SqlValue, 2> values{
      modern_sqlite::SqlValue{},
      modern_sqlite::SqlValue::Text(std::string{value}),
  };
  return TakeValue(modern_sqlite::EncodeRecord(values));
}

[[nodiscard]] std::array<modern_sqlite::SqlValue, 2> IndexRecord(std::string_view name,
                                                                 std::int64_t rowid) {
  return {
      modern_sqlite::SqlValue::Text(std::string{name}),
      modern_sqlite::SqlValue::Integer(rowid),
  };
}

[[nodiscard]] std::array<modern_sqlite::IndexColumnOrder, 2> IndexColumns() {
  return {
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
}

void InsertIndexRecord(modern_sqlite::IndexBtreeWriter& index, std::string_view name,
                       std::int64_t rowid) {
  auto values = IndexRecord(name, rowid);
  RequireStatus(index.Insert(values));
}

void DeleteIndexRecord(modern_sqlite::IndexBtreeWriter& index, std::string_view name,
                       std::int64_t rowid) {
  auto values = IndexRecord(name, rowid);
  if (!TakeValue(index.Delete(values))) {
    throw std::runtime_error("Modern SQLite could not delete an expected index record");
  }
}

void VerifyModernHeader(const modern_sqlite::Pager& pager, DatabaseGeometry geometry) {
  const modern_sqlite::DatabaseHeader* header = pager.header();
  if (header == nullptr || header->page_size() != modern_sqlite::ByteCount{geometry.page_size} ||
      header->reserved_bytes() != modern_sqlite::ByteCount{geometry.reserved_bytes} ||
      header->freelist_page_count() != kExpectedFreelistCount) {
    throw std::runtime_error("Modern SQLite read the wrong database geometry or freelist count");
  }
}

void VerifySqliteMetadata(sqlite3* database, BtreeRoots roots, DatabaseGeometry geometry) {
  if (QueryInteger(database, "PRAGMA page_size") != static_cast<std::int64_t>(geometry.page_size) ||
      QueryReservedBytes(database) != static_cast<std::int64_t>(geometry.reserved_bytes)) {
    throw std::runtime_error("SQLite read the wrong database geometry");
  }
  if (QueryInteger(database, "PRAGMA freelist_count") != kExpectedFreelistCount) {
    throw std::runtime_error("SQLite read the wrong freelist count");
  }
  if (QueryInteger(database, "SELECT rootpage FROM sqlite_schema WHERE name='items'") !=
          roots.table.value() ||
      QueryInteger(database, "SELECT rootpage FROM sqlite_schema WHERE name='idx_items_name'") !=
          roots.index.value()) {
    throw std::runtime_error("SQLite read the wrong Modern B-tree root page");
  }
  if (QueryText(database, "SELECT sql FROM sqlite_schema WHERE name='items'") != kTableSql ||
      QueryText(database, "SELECT sql FROM sqlite_schema WHERE name='idx_items_name'") !=
          kIndexSql) {
    throw std::runtime_error("SQLite read the wrong schema SQL");
  }
  const std::string query_plan = QueryText(
      database,
      "EXPLAIN QUERY PLAN SELECT name,id FROM items INDEXED BY idx_items_name ORDER BY name,id", 3);
  if (!query_plan.contains("USING COVERING INDEX idx_items_name")) {
    throw std::runtime_error("SQLite did not use the Modern-compatible index");
  }
  if (QueryText(database, "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error("SQLite integrity_check rejected the database image");
  }
}

[[nodiscard]] BtreeRoots CreateWithModernSqlite(const std::filesystem::path& path,
                                                DatabaseGeometry geometry) {
  modern_sqlite::PosixVfs vfs;
  auto opened = modern_sqlite::Pager::OpenWritable(
      vfs, path.string(),
      modern_sqlite::WritablePagerOptions{
          .pager =
              modern_sqlite::PagerOptions{
                  .empty_database_page_size = modern_sqlite::ByteCount{geometry.page_size},
                  .cache_capacity_pages = 32,
              },
          .journal =
              modern_sqlite::RollbackJournalOptions{
                  .legacy_page_size = modern_sqlite::ByteCount{geometry.page_size},
              },
      });
  std::unique_ptr<modern_sqlite::Pager> pager = TakeValue(std::move(opened));
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  BtreeRoots roots;
  {
    modern_sqlite::BtreeWriteSession session =
        TakeValue(modern_sqlite::BtreeWriteSession::Open(*pager));
    RequireStatus(session.InitializeDatabase(modern_sqlite::BtreeDatabaseOptions{
        .reserved_bytes = modern_sqlite::ByteCount{geometry.reserved_bytes},
    }));
    modern_sqlite::TableBtreeWriter schema =
        TakeValue(session.OpenTableBtree(modern_sqlite::PageNumber{1}));
    modern_sqlite::TableBtreeWriter table = TakeValue(session.CreateTableBtree());
    const auto columns = IndexColumns();
    modern_sqlite::IndexBtreeWriter index = TakeValue(session.CreateIndexBtree(columns));
    roots = BtreeRoots{.table = table.root_page(), .index = index.root_page()};

    const std::array table_schema_values{
        modern_sqlite::SqlValue::Text("table"),
        modern_sqlite::SqlValue::Text("items"),
        modern_sqlite::SqlValue::Text("items"),
        modern_sqlite::SqlValue::Integer(roots.table.value()),
        modern_sqlite::SqlValue::Text(std::string{kTableSql}),
    };
    const modern_sqlite::ByteBuffer table_schema_record =
        TakeValue(modern_sqlite::EncodeRecord(table_schema_values));
    RequireStatus(schema.Insert(1, table_schema_record.view()));

    const std::array index_schema_values{
        modern_sqlite::SqlValue::Text("index"),
        modern_sqlite::SqlValue::Text("idx_items_name"),
        modern_sqlite::SqlValue::Text("items"),
        modern_sqlite::SqlValue::Integer(roots.index.value()),
        modern_sqlite::SqlValue::Text(std::string{kIndexSql}),
    };
    const modern_sqlite::ByteBuffer index_schema_record =
        TakeValue(modern_sqlite::EncodeRecord(index_schema_values));
    RequireStatus(schema.Insert(2, index_schema_record.view()));

    RequireStatus(table.Insert(1, TableRecord("alpha").view()));
    RequireStatus(table.Insert(2, TableRecord("beta").view()));
    RequireStatus(table.Insert(3, TableRecord("gamma").view()));
    InsertIndexRecord(index, "alpha", 1);
    InsertIndexRecord(index, "beta", 2);
    InsertIndexRecord(index, "gamma", 3);

    modern_sqlite::TableBtreeWriter scratch = TakeValue(session.CreateTableBtree());
    RequireStatus(scratch.Drop());
  }
  RequireStatus(pager->Commit());
  RequireStatus(pager->EndRead());
  return roots;
}

void VerifyAndMutateWithSqlite(const std::filesystem::path& path, BtreeRoots roots,
                               DatabaseGeometry geometry) {
  const Database database{path, kReadWriteFlags};
  VerifySqliteMetadata(database.get(), roots, geometry);
  if (QueryText(database.get(),
                "SELECT group_concat(name || ':' || id, ',') FROM "
                "(SELECT name,id FROM items INDEXED BY idx_items_name ORDER BY name,id)") !=
      "alpha:1,beta:2,gamma:3") {
    throw std::runtime_error("SQLite rejected Modern-created table or index rows");
  }

  Execute(database.get(), "UPDATE items SET name='beta-two' WHERE id=2");
  Execute(database.get(), "DELETE FROM items WHERE id=1");
  Execute(database.get(), "INSERT INTO items(id,name) VALUES(4,'delta')");
  VerifySqliteMetadata(database.get(), roots, geometry);
  if (QueryText(database.get(),
                "SELECT group_concat(name || ':' || id, ',') FROM "
                "(SELECT name,id FROM items INDEXED BY idx_items_name ORDER BY name,id)") !=
      "beta-two:2,delta:4,gamma:3") {
    throw std::runtime_error("SQLite mutation produced the wrong table or index rows");
  }
}

void VerifySqliteMutationWithModernSqlite(const std::filesystem::path& path, BtreeRoots roots,
                                          DatabaseGeometry geometry) {
  modern_sqlite::PosixVfs vfs;
  auto opened = modern_sqlite::Pager::Open(
      vfs, path.string(),
      modern_sqlite::PagerOptions{
          .empty_database_page_size = modern_sqlite::ByteCount{geometry.page_size},
          .cache_capacity_pages = 32,
      });
  std::unique_ptr<modern_sqlite::Pager> pager = TakeValue(std::move(opened));
  RequireStatus(pager->BeginRead());
  VerifyModernHeader(*pager, geometry);

  {
    modern_sqlite::TableBtreeCursor cursor =
        TakeValue(modern_sqlite::TableBtreeCursor::Open(*pager, roots.table));
    constexpr std::array expected{
        std::pair{std::int64_t{2}, std::string_view{"beta-two"}},
        std::pair{std::int64_t{3}, std::string_view{"gamma"}},
        std::pair{std::int64_t{4}, std::string_view{"delta"}},
    };
    if (!TakeValue(cursor.First())) {
      throw std::runtime_error("Modern SQLite found no SQLite-written table rows");
    }
    for (std::size_t index = 0U; index < expected.size(); ++index) {
      const auto& [rowid, text] = expected[index];
      if (TakeValue(cursor.rowid()) != rowid) {
        throw std::runtime_error("Modern SQLite read the wrong SQLite-written rowid");
      }
      const modern_sqlite::ByteBuffer actual = TakeValue(cursor.CopyPayload());
      const modern_sqlite::ByteBuffer encoded = TableRecord(text);
      if (!std::ranges::equal(actual.view(), encoded.view())) {
        throw std::runtime_error("Modern SQLite decoded the wrong SQLite-written table record");
      }
      const bool has_next = TakeValue(cursor.Next());
      if (has_next != (index + 1U < expected.size())) {
        throw std::runtime_error("Modern SQLite observed an unexpected table row count");
      }
    }
  }

  {
    const auto columns = IndexColumns();
    modern_sqlite::IndexBtreeCursor cursor =
        TakeValue(modern_sqlite::IndexBtreeCursor::Open(*pager, roots.index, columns));
    constexpr std::array expected{
        std::pair{std::string_view{"beta-two"}, std::int64_t{2}},
        std::pair{std::string_view{"delta"}, std::int64_t{4}},
        std::pair{std::string_view{"gamma"}, std::int64_t{3}},
    };
    if (!TakeValue(cursor.First())) {
      throw std::runtime_error("Modern SQLite found no SQLite-written index rows");
    }
    for (std::size_t index = 0U; index < expected.size(); ++index) {
      const modern_sqlite::ByteBuffer payload = TakeValue(cursor.CopyPayload());
      const modern_sqlite::RecordView record =
          TakeValue(modern_sqlite::RecordView::Parse(payload.view()));
      const modern_sqlite::RecordFieldView name = TakeValue(record.field(0U));
      const modern_sqlite::RecordFieldView rowid = TakeValue(record.field(1U));
      const auto actual_name = name.text_value();
      if (!actual_name.has_value() || actual_name->bytes() != expected[index].first ||
          rowid.integer_value() != expected[index].second) {
        throw std::runtime_error("Modern SQLite decoded the wrong SQLite-written index record");
      }
      const bool has_next = TakeValue(cursor.Next());
      if (has_next != (index + 1U < expected.size())) {
        throw std::runtime_error("Modern SQLite observed an unexpected index row count");
      }
    }
  }

  RequireStatus(pager->EndRead());
}

[[nodiscard]] BtreeRoots CreateWithSqlite(const std::filesystem::path& path,
                                          DatabaseGeometry geometry) {
  const Database database{path, kCreateFlags};
  Execute(database.get(), "PRAGMA page_size=" + std::to_string(geometry.page_size));
  SetReservedBytes(database.get(), geometry.reserved_bytes);
  Execute(database.get(), "PRAGMA journal_mode=DELETE");
  Execute(database.get(), kTableSql);
  Execute(database.get(), kIndexSql);
  Execute(database.get(), "INSERT INTO items(id,name) VALUES(1,'alpha'),(2,'beta'),(3,'gamma')");
  Execute(database.get(), "CREATE TABLE scratch(value)");
  Execute(database.get(), "DROP TABLE scratch");
  const BtreeRoots roots{
      .table = modern_sqlite::PageNumber{static_cast<std::uint32_t>(
          QueryInteger(database.get(), "SELECT rootpage FROM sqlite_schema WHERE name='items'"))},
      .index = modern_sqlite::PageNumber{static_cast<std::uint32_t>(QueryInteger(
          database.get(), "SELECT rootpage FROM sqlite_schema WHERE name='idx_items_name'"))},
  };
  VerifySqliteMetadata(database.get(), roots, geometry);
  return roots;
}

void MutateSqliteDatabaseWithModernSqlite(const std::filesystem::path& path, BtreeRoots roots,
                                          DatabaseGeometry geometry) {
  modern_sqlite::PosixVfs vfs;
  auto opened = modern_sqlite::Pager::OpenWritable(
      vfs, path.string(),
      modern_sqlite::WritablePagerOptions{
          .pager =
              modern_sqlite::PagerOptions{
                  .empty_database_page_size = modern_sqlite::ByteCount{geometry.page_size},
                  .cache_capacity_pages = 32,
              },
          .journal =
              modern_sqlite::RollbackJournalOptions{
                  .legacy_page_size = modern_sqlite::ByteCount{geometry.page_size},
              },
      });
  std::unique_ptr<modern_sqlite::Pager> pager = TakeValue(std::move(opened));
  RequireStatus(pager->BeginRead());
  VerifyModernHeader(*pager, geometry);
  RequireStatus(pager->BeginWrite());
  {
    modern_sqlite::BtreeWriteSession session =
        TakeValue(modern_sqlite::BtreeWriteSession::Open(*pager));
    modern_sqlite::TableBtreeWriter table = TakeValue(session.OpenTableBtree(roots.table));
    const auto columns = IndexColumns();
    modern_sqlite::IndexBtreeWriter index = TakeValue(session.OpenIndexBtree(roots.index, columns));

    DeleteIndexRecord(index, "beta", 2);
    InsertIndexRecord(index, "beta-modern", 2);
    RequireStatus(table.Insert(2, TableRecord("beta-modern").view(),
                               modern_sqlite::BtreeInsertMode::kReplace));

    DeleteIndexRecord(index, "alpha", 1);
    if (!TakeValue(table.Delete(1))) {
      throw std::runtime_error("Modern SQLite could not delete a SQLite-created table row");
    }

    RequireStatus(table.Insert(4, TableRecord("delta").view()));
    InsertIndexRecord(index, "delta", 4);
  }
  RequireStatus(pager->Commit());
  RequireStatus(pager->EndRead());
}

void VerifyModernMutationWithSqlite(const std::filesystem::path& path, BtreeRoots roots,
                                    DatabaseGeometry geometry) {
  const Database database{path, kReadOnlyFlags};
  VerifySqliteMetadata(database.get(), roots, geometry);
  if (QueryText(database.get(),
                "SELECT group_concat(name || ':' || id, ',') FROM "
                "(SELECT name,id FROM items INDEXED BY idx_items_name ORDER BY name,id)") !=
      "beta-modern:2,delta:4,gamma:3") {
    throw std::runtime_error("SQLite read the wrong Modern-mutated table or index rows");
  }
}

void RunGeometryCase(const TemporaryDirectory& directory, DatabaseGeometry geometry) {
  const std::filesystem::path modern_path = directory.DatabasePath("modern", geometry);
  const BtreeRoots modern_roots = CreateWithModernSqlite(modern_path, geometry);
  if (std::filesystem::exists(modern_path.string() + "-journal")) {
    throw std::runtime_error("Modern SQLite left a rollback journal after commit");
  }
  VerifyAndMutateWithSqlite(modern_path, modern_roots, geometry);
  VerifySqliteMutationWithModernSqlite(modern_path, modern_roots, geometry);

  const std::filesystem::path sqlite_path = directory.DatabasePath("sqlite", geometry);
  const BtreeRoots sqlite_roots = CreateWithSqlite(sqlite_path, geometry);
  MutateSqliteDatabaseWithModernSqlite(sqlite_path, sqlite_roots, geometry);
  VerifyModernMutationWithSqlite(sqlite_path, sqlite_roots, geometry);
}

}  // namespace

int main() try {
  if (sqlite3_libversion_number() != 3'054'000) {
    throw std::runtime_error("compatibility test requires SQLite 3.54.0");
  }
  const TemporaryDirectory directory;
  for (const std::size_t page_size : kPageSizes) {
    for (const std::size_t reserved_bytes : kReservedByteCounts) {
      const DatabaseGeometry geometry{
          .page_size = page_size,
          .reserved_bytes = reserved_bytes,
      };
      try {
        RunGeometryCase(directory, geometry);
      } catch (const std::exception& error) {
        throw std::runtime_error(GeometryDescription(geometry) + ": " + error.what());
      }
    }
  }
  return 0;
} catch (const std::exception& error) {
  static_cast<void>(std::fprintf(stderr, "%s\n", error.what()));
  return 1;
}
