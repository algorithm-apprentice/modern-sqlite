#include <sqlite3.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"

namespace {

constexpr std::size_t kPageSize = 512;
constexpr std::uint32_t kApplicationId = 0x4d53514cU;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-write-compatibility-" + std::to_string(suffix));
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

void CreateDatabase(const std::filesystem::path& path) {
  Database database{path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX};
  Execute(database.get(), "PRAGMA page_size=512");
  Execute(database.get(), "PRAGMA journal_mode=DELETE");
  Execute(database.get(), "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT NOT NULL)");
  Execute(database.get(), "INSERT INTO items(name) VALUES ('alpha'), ('beta'), ('gamma')");
}

void ModifyWithModernSqlite(const std::filesystem::path& path) {
  modern_sqlite::PosixVfs vfs;
  auto opened = modern_sqlite::Pager::OpenWritable(
      vfs, path.string(),
      modern_sqlite::WritablePagerOptions{
          .pager =
              modern_sqlite::PagerOptions{
                  .empty_database_page_size = modern_sqlite::ByteCount{kPageSize},
                  .cache_capacity_pages = 4,
              },
          .journal =
              modern_sqlite::RollbackJournalOptions{
                  .legacy_page_size = modern_sqlite::ByteCount{kPageSize},
              },
      });
  if (!opened.has_value()) {
    throw std::runtime_error(std::string{opened.error().message()});
  }
  std::unique_ptr<modern_sqlite::Pager> pager = std::move(*opened);
  if (!pager->BeginRead().has_value() ||
      pager->page_size() != modern_sqlite::ByteCount{kPageSize} ||
      !pager->BeginWrite().has_value()) {
    throw std::runtime_error("Modern SQLite could not begin the write transaction");
  }
  {
    auto page = pager->WritePage(modern_sqlite::PageNumber{1});
    if (!page.has_value()) {
      throw std::runtime_error(std::string{page.error().message()});
    }
    modern_sqlite::StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(std::uint32_t)>{page->mutable_bytes().data() + 68,
                                                    sizeof(std::uint32_t)},
        kApplicationId);
  }
  const auto committed = pager->Commit();
  if (!committed.has_value()) {
    throw std::runtime_error(std::string{committed.error().message()});
  }
  const auto ended = pager->EndRead();
  if (!ended.has_value()) {
    throw std::runtime_error(std::string{ended.error().message()});
  }
}

void VerifyWithSqlite(const std::filesystem::path& path) {
  Database database{path, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX};
  if (QueryInteger(database.get(), "PRAGMA application_id") !=
      static_cast<std::int64_t>(kApplicationId)) {
    throw std::runtime_error("SQLite did not observe the committed application_id");
  }
  if (QueryInteger(database.get(), "SELECT count(*) FROM items") != 3 ||
      QueryText(database.get(),
                "SELECT group_concat(name, ',') FROM "
                "(SELECT name FROM items ORDER BY id)") != "alpha,beta,gamma") {
    throw std::runtime_error("SQLite table contents changed after pager commit");
  }
  if (QueryText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error("SQLite integrity_check rejected the committed image");
  }
}

}  // namespace

int main() try {
  if (sqlite3_libversion_number() != 3'054'000) {
    throw std::runtime_error("compatibility test requires SQLite 3.54.0");
  }
  const TemporaryDirectory directory;
  const std::filesystem::path database_path = directory.DatabasePath();
  CreateDatabase(database_path);
  ModifyWithModernSqlite(database_path);
  if (std::filesystem::exists(database_path.string() + "-journal")) {
    throw std::runtime_error("Modern SQLite left a rollback journal after commit");
  }
  VerifyWithSqlite(database_path);
  return 0;
} catch (const std::exception& error) {
  static_cast<void>(std::fprintf(stderr, "%s\n", error.what()));
  return 1;
}
