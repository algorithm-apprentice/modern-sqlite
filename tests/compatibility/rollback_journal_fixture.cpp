#include <sqlite3.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

class Database final {
 public:
  explicit Database(const std::filesystem::path& path) {
    const int result = sqlite3_open_v2(path.string().c_str(), &database_,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
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

  void Close() {
    if (database_ == nullptr) {
      return;
    }
    const int result = sqlite3_close(database_);
    database_ = nullptr;
    if (result != SQLITE_OK) {
      throw std::runtime_error("sqlite3_close failed");
    }
  }

 private:
  sqlite3* database_ = nullptr;
};

class Statement final {
 public:
  Statement(sqlite3* database, std::string_view sql) {
    const int result =
        sqlite3_prepare_v2(database, std::string{sql}.c_str(), -1, &statement_, nullptr);
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

  void BindBlob(std::span<const std::byte> bytes) {
    const int result =
        sqlite3_bind_blob(statement_, 1, bytes.data(), static_cast<int>(bytes.size()), nullptr);
    if (result != SQLITE_OK) {
      throw std::runtime_error("sqlite3_bind_blob failed");
    }
  }

  void Execute() {
    const int result = sqlite3_step(statement_);
    if (result != SQLITE_DONE) {
      throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(statement_)));
    }
  }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-rollback-fixture-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void Execute(sqlite3* database, std::string_view sql) {
  char* error = nullptr;
  const int result = sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    std::string message = error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    throw std::runtime_error(std::move(message));
  }
}

[[nodiscard]] std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to open " + path.string());
  }
  input.seekg(0, std::ios::end);
  const std::streamoff end = input.tellg();
  if (end < 0) {
    throw std::runtime_error("unable to size " + path.string());
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.read(reinterpret_cast<char*>(bytes.data()), end);
  if (!input && end != 0) {
    throw std::runtime_error("unable to read " + path.string());
  }
  return bytes;
}

void WriteFile(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("unable to create " + path.string());
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("unable to write " + path.string());
  }
}

struct Fixture {
  std::vector<std::byte> database;
  std::vector<std::byte> journal;
};

[[nodiscard]] Fixture GenerateFixture(std::size_t page_size,
                                      const std::filesystem::path& working_directory) {
  const std::filesystem::path database_path =
      working_directory / ("rollback-" + std::to_string(page_size) + ".db");
  const std::filesystem::path journal_path = database_path.string() + "-journal";
  const std::size_t payload_size = page_size * 3U;

  {
    Database database(database_path);
    Execute(database.get(), "PRAGMA page_size=" + std::to_string(page_size));
    Execute(database.get(), "PRAGMA auto_vacuum=NONE");
    Execute(database.get(), "PRAGMA journal_mode=DELETE");
    Execute(database.get(), "PRAGMA synchronous=FULL");
    Execute(database.get(), "PRAGMA secure_delete=OFF");
    Execute(database.get(), "CREATE TABLE sample(id INTEGER PRIMARY KEY, value BLOB NOT NULL)");
    std::vector<std::byte> original(payload_size, std::byte{0x11});
    {
      Statement insert(database.get(), "INSERT INTO sample(value) VALUES(?)");
      insert.BindBlob(original);
      insert.Execute();
    }
    database.Close();
  }

  const std::vector<std::byte> original_database = ReadFile(database_path);
  std::vector<std::byte> journal;
  {
    Database database(database_path);
    Execute(database.get(), "PRAGMA journal_mode=DELETE");
    Execute(database.get(), "PRAGMA synchronous=FULL");
    Execute(database.get(), "PRAGMA cache_size=1");
    Execute(database.get(), "BEGIN IMMEDIATE");
    static_cast<void>(sqlite3_test_control(SQLITE_TESTCTRL_PRNG_SEED, 0x13579bdf, nullptr));
    std::vector<std::byte> replacement(payload_size, std::byte{0x22});
    {
      Statement update(database.get(), "UPDATE sample SET value=? WHERE id=1");
      update.BindBlob(replacement);
      update.Execute();
    }
    const int flush_result = sqlite3_db_cacheflush(database.get());
    if (flush_result != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    journal = ReadFile(journal_path);
    Execute(database.get(), "ROLLBACK");
    database.Close();
  }

  if (ReadFile(database_path) != original_database) {
    throw std::runtime_error("SQLite rollback did not restore the fixture database");
  }
  return Fixture{
      .database = original_database,
      .journal = std::move(journal),
  };
}

[[nodiscard]] std::string DatabaseFilename(std::size_t page_size) {
  return "sqlite-3.54.0-rollback-page-" + std::to_string(page_size) + ".db";
}

[[nodiscard]] std::string JournalFilename(std::size_t page_size) {
  return "sqlite-3.54.0-rollback-page-" + std::to_string(page_size) + ".journal";
}

void WriteFixtures(const std::filesystem::path& output_directory) {
  TemporaryDirectory working;
  for (const std::size_t page_size : {512U, 4096U, 65536U}) {
    const Fixture fixture = GenerateFixture(page_size, working.path());
    WriteFile(output_directory / DatabaseFilename(page_size), fixture.database);
    WriteFile(output_directory / JournalFilename(page_size), fixture.journal);
  }
}

void VerifyFixtures(const std::filesystem::path& fixture_directory) {
  TemporaryDirectory working;
  for (const std::size_t page_size : {512U, 4096U, 65536U}) {
    const Fixture generated = GenerateFixture(page_size, working.path());
    if (ReadFile(fixture_directory / DatabaseFilename(page_size)) != generated.database) {
      throw std::runtime_error("database fixture differs for page size " +
                               std::to_string(page_size));
    }
    if (ReadFile(fixture_directory / JournalFilename(page_size)) != generated.journal) {
      throw std::runtime_error("journal fixture differs for page size " +
                               std::to_string(page_size));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "usage: rollback_journal_fixture <--write|--verify> <directory>\n";
      return 2;
    }
    const std::string_view operation{argv[1]};
    const std::filesystem::path directory{argv[2]};
    if (operation == "--write") {
      WriteFixtures(directory);
    } else if (operation == "--verify") {
      VerifyFixtures(directory);
    } else {
      std::cerr << "unknown operation: " << operation << '\n';
      return 2;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
