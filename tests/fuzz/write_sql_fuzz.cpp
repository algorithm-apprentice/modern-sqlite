#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "write_fuzz.hpp"

namespace modern_sqlite::fuzz {
namespace {

constexpr std::size_t kMaximumSqlBytes = std::size_t{64} * 1024U;
constexpr std::size_t kMaximumStatements = 32;
constexpr std::size_t kMaximumTotalSteps = 4096;
constexpr std::size_t kMaximumRowsPerStatement = 256;
constexpr auto kDatabaseName = std::to_array("input.db");
constexpr auto kJournalName = std::to_array("input.db-journal");
constexpr auto kWalName = std::to_array("input.db-wal");
constexpr auto kSharedMemoryName = std::to_array("input.db-shm");

class OwnedFileDescriptor final {
 public:
  OwnedFileDescriptor() = default;
  explicit OwnedFileDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}
  OwnedFileDescriptor(const OwnedFileDescriptor&) = delete;
  OwnedFileDescriptor& operator=(const OwnedFileDescriptor&) = delete;
  ~OwnedFileDescriptor() { Reset(); }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

  void Reset(int descriptor = -1) noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
    descriptor_ = descriptor;
  }

 private:
  int descriptor_ = -1;
};

class PrivateDatabase final {
 public:
  PrivateDatabase() = default;
  PrivateDatabase(const PrivateDatabase&) = delete;
  PrivateDatabase& operator=(const PrivateDatabase&) = delete;

  ~PrivateDatabase() {
    if (directory_descriptor_.valid()) {
      static_cast<void>(::unlinkat(directory_descriptor_.get(), kSharedMemoryName.data(), 0));
      static_cast<void>(::unlinkat(directory_descriptor_.get(), kWalName.data(), 0));
      static_cast<void>(::unlinkat(directory_descriptor_.get(), kJournalName.data(), 0));
      static_cast<void>(::unlinkat(directory_descriptor_.get(), kDatabaseName.data(), 0));
    }
    directory_descriptor_.Reset();
    if (directory_created_) {
      static_cast<void>(::rmdir(directory_.data()));
    }
  }

  [[nodiscard]] bool Initialize() {
    constexpr auto directory_template = std::to_array("/tmp/modern-sqlite-write-fuzz-XXXXXX");
    std::ranges::copy(directory_template, directory_.begin());
    if (::mkdtemp(directory_.data()) == nullptr) {
      return false;
    }
    directory_created_ = true;

    directory_descriptor_.Reset(::open(directory_.data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory_descriptor_.valid()) {
      return false;
    }
    const OwnedFileDescriptor database{::openat(directory_descriptor_.get(), kDatabaseName.data(),
                                                O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                                0600)};
    if (!database.valid()) {
      return false;
    }

    const int count =
        std::snprintf(path_.data(), path_.size(), "%s/%s", directory_.data(), kDatabaseName.data());
    return count > 0 && static_cast<std::size_t>(count) < path_.size();
  }

  [[nodiscard]] std::string_view path() const noexcept { return path_.data(); }

 private:
  std::array<char, 64> directory_{};
  std::array<char, PATH_MAX> path_{};
  OwnedFileDescriptor directory_descriptor_;
  bool directory_created_ = false;
};

[[nodiscard]] bool ExecuteDone(WriteSession& session, std::string_view sql) {
  auto prepared = session.Prepare(Utf8View{sql});
  if (!prepared.has_value() || !prepared->statement.has_value() ||
      prepared->next_offset.value() != sql.size()) {
    return false;
  }
  WriteStatement statement = std::move(*prepared->statement);
  const Result<WriteStep> stepped = statement.Step();
  const Status finalized = statement.Finalize();
  return stepped.has_value() && *stepped == WriteStep::kDone && finalized.has_value();
}

[[nodiscard]] bool SeedDatabase(WriteSession& session) {
  return ExecuteDone(session,
                     "CREATE TABLE fuzz_target("
                     "id INTEGER PRIMARY KEY,"
                     "value TEXT NOT NULL DEFAULT 'seed',"
                     "score REAL,"
                     "payload BLOB"
                     ")") &&
         ExecuteDone(session, "INSERT INTO fuzz_target VALUES(1,'one',1,x'01')");
}

[[nodiscard]] bool ConsumeStatement(WriteStatement& statement, std::size_t& total_steps) {
  std::size_t rows = 0;
  while (total_steps < kMaximumTotalSteps) {
    const Result<WriteStep> stepped = statement.Step();
    ++total_steps;
    if (!stepped.has_value() || *stepped == WriteStep::kDone) {
      [[maybe_unused]] const Status finalized = statement.Finalize();
      return true;
    }
    ++rows;
    if (rows >= kMaximumRowsPerStatement) {
      [[maybe_unused]] const Status finalized = statement.Finalize();
      return false;
    }
  }
  [[maybe_unused]] const Status finalized = statement.Finalize();
  return false;
}

void BestEffortRollback(WriteSession& session) {
  if (!session.autocommit()) {
    static_cast<void>(ExecuteDone(session, "ROLLBACK"));
  }
}

}  // namespace

void RunWriteSqlInput(std::span<const std::uint8_t> input) {
  if (input.size() > kMaximumSqlBytes) {
    return;
  }

  PrivateDatabase database;
  if (!database.Initialize()) {
    return;
  }
  Result<WriteSession> opened = WriteSession::Open(database.path());
  if (!opened.has_value()) {
    return;
  }
  WriteSession session = std::move(*opened);
  if (!SeedDatabase(session)) {
    return;
  }

  const char* const bytes = input.empty() ? "" : reinterpret_cast<const char*>(input.data());
  std::string_view remaining{bytes, input.size()};
  std::size_t statements = 0;
  std::size_t total_steps = 0;
  while (!remaining.empty() && statements < kMaximumStatements &&
         total_steps < kMaximumTotalSteps) {
    Result<WritePrepareOutput> prepared = session.Prepare(Utf8View{remaining});
    if (!prepared.has_value()) {
      break;
    }
    const std::size_t consumed = prepared->next_offset.value();
    if (consumed == 0U || consumed > remaining.size()) {
      break;
    }
    remaining.remove_prefix(consumed);
    if (!prepared->statement.has_value()) {
      continue;
    }
    ++statements;
    WriteStatement statement = std::move(*prepared->statement);
    if (!ConsumeStatement(statement, total_steps)) {
      break;
    }
  }
  BestEffortRollback(session);
}

}  // namespace modern_sqlite::fuzz
