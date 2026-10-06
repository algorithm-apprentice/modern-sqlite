#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"

namespace {

constexpr std::size_t kDefaultPageSize = 512;
constexpr int kReadWriteFlags = static_cast<int>(static_cast<unsigned int>(SQLITE_OPEN_READWRITE) |
                                                 static_cast<unsigned int>(SQLITE_OPEN_NOMUTEX));
constexpr int kCreateFlags = static_cast<int>(static_cast<unsigned int>(SQLITE_OPEN_READWRITE) |
                                              static_cast<unsigned int>(SQLITE_OPEN_CREATE) |
                                              static_cast<unsigned int>(SQLITE_OPEN_NOMUTEX));

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-btree-write-compatibility-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path Path(std::string_view name) const { return path_ / name; }

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

template <typename T>
[[nodiscard]] T Take(modern_sqlite::Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(std::string{result.error().message()});
  }
  return std::move(*result);
}

void Require(modern_sqlite::Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(std::string{status.error().message()});
  }
}

[[nodiscard]] modern_sqlite::ByteBuffer Encode(std::vector<modern_sqlite::SqlValue> values) {
  return Take(modern_sqlite::EncodeRecord(values));
}

[[nodiscard]] std::vector<modern_sqlite::SqlValue> TableRecord(std::string name) {
  std::vector<modern_sqlite::SqlValue> values;
  values.reserve(2);
  values.emplace_back();
  values.push_back(modern_sqlite::SqlValue::Text(std::move(name)));
  return values;
}

[[nodiscard]] std::vector<modern_sqlite::SqlValue> IndexRecord(std::string name,
                                                               std::int64_t rowid) {
  std::vector<modern_sqlite::SqlValue> values;
  values.reserve(2);
  values.push_back(modern_sqlite::SqlValue::Text(std::move(name)));
  values.push_back(modern_sqlite::SqlValue::Integer(rowid));
  return values;
}

[[nodiscard]] std::string ItemName(std::int64_t rowid) {
  return "modern-" + std::to_string(rowid) + "-" +
         std::string(static_cast<std::size_t>(rowid % 70), 'x');
}

[[nodiscard]] std::string OverflowName(std::int64_t rowid) {
  return "overflow-" + std::to_string(100'000 + rowid) + "-" +
         std::string(900U, static_cast<char>('a' + rowid % 20));
}

[[nodiscard]] std::string ModernOverflowName(std::size_t page_size) {
  return "modern-overflow-" + std::to_string(page_size) + "-" +
         std::string(page_size + 1'024U, 'z');
}

[[nodiscard]] modern_sqlite::ByteBuffer CopyCellPayload(modern_sqlite::Pager& pager,
                                                        const modern_sqlite::BtreeCellView& cell,
                                                        modern_sqlite::BtreePageGeometry geometry) {
  modern_sqlite::ByteBuffer payload{cell.payload_size()};
  const modern_sqlite::MutableByteView output = payload.mutable_view();
  const modern_sqlite::ByteView local = cell.local_payload();
  std::ranges::copy(local, output.begin());
  std::size_t offset = local.size();
  std::optional<modern_sqlite::PageNumber> overflow_page = cell.first_overflow_page();
  while (offset < output.size()) {
    if (!overflow_page.has_value()) {
      throw std::runtime_error("overflow chain ended before the compatibility payload");
    }
    const auto pin = Take(pager.ReadPage(*overflow_page));
    const auto overflow =
        Take(modern_sqlite::OverflowPageView::Parse(pin.frame().bytes(), geometry));
    const std::size_t count = std::min(overflow.payload().size(), output.size() - offset);
    std::ranges::copy(overflow.payload().first(count),
                      output.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += count;
    overflow_page = overflow.next_page();
  }
  if (overflow_page.has_value()) {
    throw std::runtime_error("overflow chain exceeded the compatibility payload");
  }
  return payload;
}

[[nodiscard]] std::unique_ptr<modern_sqlite::Pager> OpenModern(modern_sqlite::PosixVfs& vfs,
                                                               const std::filesystem::path& path,
                                                               std::size_t page_size =
                                                                   kDefaultPageSize) {
  return Take(modern_sqlite::Pager::OpenWritable(
      vfs, path.string(),
      modern_sqlite::WritablePagerOptions{
          .pager =
              modern_sqlite::PagerOptions{
                  .empty_database_page_size = modern_sqlite::ByteCount{page_size},
                  .cache_capacity_pages = 128,
              },
          .journal =
              modern_sqlite::RollbackJournalOptions{
                  .legacy_page_size = modern_sqlite::ByteCount{page_size},
              },
      }));
}

struct RootPages {
  modern_sqlite::PageNumber table;
  modern_sqlite::PageNumber index;
};

[[nodiscard]] RootPages CreateWithSqlite(const std::filesystem::path& path) {
  const Database database{path, kCreateFlags};
  Execute(database.get(), "PRAGMA page_size=512");
  Execute(database.get(), "PRAGMA journal_mode=DELETE");
  Execute(database.get(), "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT NOT NULL)");
  Execute(database.get(), "CREATE INDEX idx_items_name ON items(name)");
  Execute(database.get(), "INSERT INTO items VALUES (1, 'alpha'), (2, 'beta'), (3, 'gamma')");
  Execute(database.get(), "BEGIN");
  for (std::int64_t rowid = 1'000; rowid < 1'080; ++rowid) {
    Execute(database.get(), "INSERT INTO items VALUES (" + std::to_string(rowid) + ", '" +
                                OverflowName(rowid) + "')");
  }
  Execute(database.get(), "COMMIT");
  return RootPages{
      .table = modern_sqlite::PageNumber{static_cast<std::uint32_t>(
          QueryInteger(database.get(), "SELECT rootpage FROM sqlite_schema WHERE name='items'"))},
      .index = modern_sqlite::PageNumber{static_cast<std::uint32_t>(QueryInteger(
          database.get(), "SELECT rootpage FROM sqlite_schema WHERE name='idx_items_name'"))},
  };
}

[[nodiscard]] std::int64_t MutateSqliteDatabase(const std::filesystem::path& path,
                                                RootPages roots) {
  modern_sqlite::PosixVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = OpenModern(vfs, path);
  Require(pager->BeginRead());
  Require(pager->BeginWrite());
  auto session = Take(modern_sqlite::BtreeWriteSession::Open(*pager));
  auto table = Take(session.OpenTableBtree(roots.table));
  const std::array<modern_sqlite::IndexColumnOrder, 2> columns{
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
  auto index = Take(session.OpenIndexBtree(roots.index, columns));

  std::vector<modern_sqlite::SqlValue> overflow_key;
  {
    const modern_sqlite::DatabaseHeader* header = pager->header();
    if (header == nullptr) {
      throw std::runtime_error("SQLite-created database has no pager header");
    }
    const auto geometry =
        Take(modern_sqlite::BtreePageGeometry::Create(header->page_size(), header->usable_size()));
    const auto root_pin = Take(pager->ReadPage(roots.index));
    const auto root =
        Take(modern_sqlite::BtreePageView::Parse(root_pin.frame().bytes(), roots.index, geometry));
    if (root.type() != modern_sqlite::BtreePageType::kInteriorIndex) {
      throw std::runtime_error("SQLite overflow index did not create an interior root");
    }
    for (std::size_t cell_index = 0; cell_index < root.cell_count(); ++cell_index) {
      const auto cell = Take(root.cell(cell_index));
      if (cell.first_overflow_page().has_value()) {
        overflow_key =
            Take(modern_sqlite::DecodeRecord(CopyCellPayload(*pager, cell, geometry).view()));
        break;
      }
    }
  }
  if (overflow_key.size() != 2U || !overflow_key[1].integer_value().has_value()) {
    throw std::runtime_error("SQLite overflow index has no interior overflow record");
  }
  const std::int64_t overflow_rowid = overflow_key[1].integer_value().value_or(0);
  if (!Take(index.Delete(overflow_key)) || !Take(table.Delete(overflow_rowid))) {
    throw std::runtime_error("Modern SQLite did not delete the interior overflow record");
  }

  Require(table.Delete(2).transform([](bool deleted) {
    if (!deleted) {
      throw std::runtime_error("Modern SQLite did not find the SQLite table row");
    }
  }));
  const auto beta_key = IndexRecord("beta", 2);
  if (!Take(index.Delete(beta_key))) {
    throw std::runtime_error("Modern SQLite did not find the SQLite index entry");
  }
  for (std::int64_t rowid = 100; rowid < 400; ++rowid) {
    const std::string name = ItemName(rowid);
    const modern_sqlite::ByteBuffer payload = Encode(TableRecord(name));
    Require(table.Insert(rowid, payload.view()));
    const auto key = IndexRecord(name, rowid);
    Require(index.Insert(key));
  }
  Require(pager->Commit());
  Require(pager->EndRead());
  return overflow_rowid;
}

void VerifySqliteDatabase(const std::filesystem::path& path, std::int64_t overflow_rowid) {
  const Database database{path, kReadWriteFlags};
  if (QueryInteger(database.get(), "SELECT count(*) FROM items") != 381 ||
      QueryInteger(database.get(), "SELECT count(*) FROM items WHERE id=2") != 0 ||
      QueryInteger(database.get(),
                   "SELECT count(*) FROM items WHERE id=" + std::to_string(overflow_rowid)) != 0 ||
      QueryText(database.get(), "SELECT name FROM items WHERE id=399") != ItemName(399) ||
      QueryInteger(database.get(),
                   "SELECT count(*) FROM items INDEXED BY idx_items_name WHERE name >= ''") !=
          381) {
    throw std::runtime_error("SQLite observed incorrect Modern SQLite mutations");
  }
  if (QueryText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error("SQLite integrity_check rejected Modern SQLite mutations");
  }
  Execute(database.get(), "INSERT INTO items VALUES (500, 'sqlite-after-modern')");
  Execute(database.get(), "DELETE FROM items WHERE id=100");
  if (QueryText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error("SQLite could not safely mutate the Modern SQLite B-trees");
  }
}

[[nodiscard]] std::vector<modern_sqlite::SqlValue> SchemaRecord(std::string type, std::string name,
                                                                std::string table_name,
                                                                std::int64_t root_page,
                                                                std::string sql) {
  std::vector<modern_sqlite::SqlValue> values;
  values.reserve(5);
  values.push_back(modern_sqlite::SqlValue::Text(std::move(type)));
  values.push_back(modern_sqlite::SqlValue::Text(std::move(name)));
  values.push_back(modern_sqlite::SqlValue::Text(std::move(table_name)));
  values.push_back(modern_sqlite::SqlValue::Integer(root_page));
  values.push_back(modern_sqlite::SqlValue::Text(std::move(sql)));
  return values;
}

struct ModernDatabaseExpectations {
  std::int64_t item_count;
  std::int64_t overflow_rowid;
  std::string overflow_name;
  std::int64_t freelist_count;
  std::size_t page_size;
};

[[nodiscard]] ModernDatabaseExpectations CreateWithModernSqlite(
    const std::filesystem::path& path, std::size_t page_size, std::size_t reserved_bytes,
    std::int64_t item_count) {
  modern_sqlite::PosixVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = OpenModern(vfs, path, page_size);
  Require(pager->BeginRead());
  Require(pager->BeginWrite());
  auto session = Take(modern_sqlite::BtreeWriteSession::Open(*pager));
  Require(session.InitializeDatabase(modern_sqlite::BtreeDatabaseOptions{
      .reserved_bytes = modern_sqlite::ByteCount{reserved_bytes},
      .schema_format = modern_sqlite::DatabaseSchemaFormat::kFour,
      .text_encoding = modern_sqlite::DatabaseTextEncoding::kUtf8,
  }));
  auto schema = Take(session.OpenTableBtree(modern_sqlite::PageNumber{1}));
  auto table = Take(session.CreateTableBtree());
  const std::array<modern_sqlite::IndexColumnOrder, 2> columns{
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
  auto index = Take(session.CreateIndexBtree(columns));
  const std::array<modern_sqlite::IndexColumnOrder, 1> tiny_columns{
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
  auto tiny_keys = Take(session.CreateIndexBtree(tiny_columns));
  auto cleared_items = Take(session.CreateTableBtree());
  auto dropped_table = Take(session.CreateTableBtree());
  auto dropped_index = Take(session.CreateIndexBtree(columns));

  const std::string table_sql = "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT NOT NULL)";
  const std::string index_sql = "CREATE INDEX idx_items_name ON items(name)";
  const std::string tiny_keys_sql =
      "CREATE TABLE tiny_keys(value INTEGER PRIMARY KEY) WITHOUT ROWID";
  const std::string cleared_items_sql =
      "CREATE TABLE cleared_items(id INTEGER PRIMARY KEY, name TEXT NOT NULL)";
  const modern_sqlite::ByteBuffer table_schema =
      Encode(SchemaRecord("table", "items", "items", table.root_page().value(), table_sql));
  const modern_sqlite::ByteBuffer index_schema = Encode(
      SchemaRecord("index", "idx_items_name", "items", index.root_page().value(), index_sql));
  const modern_sqlite::ByteBuffer tiny_keys_schema = Encode(SchemaRecord(
      "table", "tiny_keys", "tiny_keys", tiny_keys.root_page().value(), tiny_keys_sql));
  const modern_sqlite::ByteBuffer cleared_items_schema =
      Encode(SchemaRecord("table", "cleared_items", "cleared_items",
                          cleared_items.root_page().value(), cleared_items_sql));
  Require(schema.Insert(1, table_schema.view()));
  Require(schema.Insert(2, index_schema.view()));
  Require(schema.Insert(3, tiny_keys_schema.view()));
  Require(schema.Insert(4, cleared_items_schema.view()));

  for (std::int64_t rowid = 1; rowid <= item_count; ++rowid) {
    const std::string name = ItemName(rowid);
    const modern_sqlite::ByteBuffer payload = Encode(TableRecord(name));
    Require(table.Insert(rowid, payload.view()));
    const auto key = IndexRecord(name, rowid);
    Require(index.Insert(key));
  }
  const std::int64_t overflow_rowid = item_count + 1'000;
  const std::string overflow_name = ModernOverflowName(page_size);
  const modern_sqlite::ByteBuffer overflow_payload = Encode(TableRecord(overflow_name));
  Require(table.Insert(overflow_rowid, overflow_payload.view()));
  Require(index.Insert(IndexRecord(overflow_name, overflow_rowid)));

  constexpr auto kLargeValue = static_cast<std::int64_t>(std::uint64_t{1} << 48U);
  for (std::int64_t offset = 19; offset >= 1; --offset) {
    Require(tiny_keys.Insert(std::array{modern_sqlite::SqlValue::Integer(-kLargeValue - offset)}));
  }
  Require(tiny_keys.Insert(std::array{modern_sqlite::SqlValue::Integer(0)}));
  for (std::int64_t offset = 1; offset <= 19; ++offset) {
    Require(tiny_keys.Insert(std::array{modern_sqlite::SqlValue::Integer(kLargeValue + offset)}));
  }
  Require(tiny_keys.Insert(std::array{modern_sqlite::SqlValue::Text("")}));

  Require(cleared_items.Insert(1, overflow_payload.view()));
  if (Take(cleared_items.Clear()) != 1U) {
    throw std::runtime_error("Modern SQLite clear returned the wrong entry count");
  }
  Require(dropped_table.Insert(1, overflow_payload.view()));
  Require(dropped_index.Insert(IndexRecord(overflow_name, 1)));
  Require(dropped_index.Drop());
  Require(dropped_table.Drop());

  {
    auto page_one = Take(pager->WritePage(modern_sqlite::PageNumber{1}));
    modern_sqlite::StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{page_one.mutable_bytes().data() + 40,
                                                    sizeof(std::uint32_t)},
        1U);
  }
  Require(pager->Commit());
  const modern_sqlite::DatabaseHeader* header = pager->header();
  if (header == nullptr) {
    throw std::runtime_error("Modern-created database has no committed header");
  }
  const std::int64_t freelist_count =
      static_cast<std::int64_t>(header->freelist_page_count());
  if (freelist_count == 0) {
    throw std::runtime_error("Modern clear/drop did not publish free pages");
  }
  Require(pager->EndRead());
  return ModernDatabaseExpectations{
      .item_count = item_count + 1,
      .overflow_rowid = overflow_rowid,
      .overflow_name = overflow_name,
      .freelist_count = freelist_count,
      .page_size = page_size,
  };
}

void VerifyModernDatabase(const std::filesystem::path& path,
                          const ModernDatabaseExpectations& expected) {
  const Database database{path, kReadWriteFlags};
  const std::string overflow_query =
      "SELECT count(*) FROM items INDEXED BY idx_items_name WHERE name='" +
      expected.overflow_name + "'";
  if (QueryInteger(database.get(), "PRAGMA page_size") !=
          static_cast<std::int64_t>(expected.page_size) ||
      QueryInteger(database.get(), "PRAGMA freelist_count") != expected.freelist_count ||
      QueryInteger(database.get(), "SELECT count(*) FROM items") != expected.item_count ||
      QueryText(database.get(), "SELECT name FROM items WHERE id=" +
                                    std::to_string(expected.overflow_rowid)) !=
          expected.overflow_name ||
      QueryInteger(database.get(), overflow_query) != 1 ||
      QueryInteger(database.get(),
                   "SELECT count(*) FROM items INDEXED BY idx_items_name WHERE name >= ''") !=
          expected.item_count ||
      QueryInteger(database.get(), "SELECT count(*) FROM tiny_keys") != 40 ||
      QueryInteger(database.get(), "SELECT count(*) FROM tiny_keys WHERE value=0") != 1 ||
      QueryInteger(database.get(), "SELECT count(*) FROM cleared_items") != 0) {
    throw std::runtime_error("SQLite observed incorrect Modern-created B-trees");
  }
  if (QueryText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error("SQLite rejected the Modern-created database");
  }
  Execute(database.get(), "INSERT INTO items VALUES (100000, 'sqlite-created')");
  Execute(database.get(),
          "DELETE FROM items WHERE id=" + std::to_string(expected.item_count / 2));
  Execute(database.get(), "INSERT INTO tiny_keys VALUES (42)");
  Execute(database.get(), "DELETE FROM tiny_keys WHERE value=0");
  Execute(database.get(), "INSERT INTO cleared_items VALUES (1, 'sqlite-after-clear')");
  if (QueryText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error("SQLite could not mutate the Modern-created database");
  }
}

}  // namespace

int main() try {
  if (sqlite3_libversion_number() != 3'054'000) {
    throw std::runtime_error("compatibility test requires SQLite 3.54.0");
  }
  const TemporaryDirectory directory;

  const std::filesystem::path sqlite_path = directory.Path("sqlite-created.db");
  const RootPages roots = CreateWithSqlite(sqlite_path);
  const std::int64_t overflow_rowid = MutateSqliteDatabase(sqlite_path, roots);
  VerifySqliteDatabase(sqlite_path, overflow_rowid);

  struct ModernCase {
    std::string_view file_name;
    std::size_t page_size;
    std::size_t reserved_bytes;
    std::int64_t item_count;
  };
  constexpr std::array<ModernCase, 3> kModernCases{{
      {
          .file_name = "modern-created-512.db",
          .page_size = 512,
          .reserved_bytes = 0,
          .item_count = 300,
      },
      {
          .file_name = "modern-created-4096-reserved.db",
          .page_size = 4'096,
          .reserved_bytes = 16,
          .item_count = 80,
      },
      {
          .file_name = "modern-created-65536.db",
          .page_size = 65'536,
          .reserved_bytes = 0,
          .item_count = 24,
      },
  }};
  for (const ModernCase& test_case : kModernCases) {
    const std::filesystem::path modern_path = directory.Path(test_case.file_name);
    const ModernDatabaseExpectations expected =
        CreateWithModernSqlite(modern_path, test_case.page_size, test_case.reserved_bytes,
                               test_case.item_count);
    VerifyModernDatabase(modern_path, expected);
  }
  return 0;
} catch (const std::exception& error) {
  static_cast<void>(std::fprintf(stderr, "%s\n", error.what()));
  return 1;
}
