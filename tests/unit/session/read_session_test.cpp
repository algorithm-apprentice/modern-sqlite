#include "modern_sqlite/session/read_session.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/platform/native_vfs.hpp"
#include "modern_sqlite/syntax/ast.hpp"
#include "src/session/session_internal.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<ReadSession>);
static_assert(!std::is_copy_assignable_v<ReadSession>);
static_assert(std::is_nothrow_move_constructible_v<ReadSession>);
static_assert(std::is_nothrow_move_assignable_v<ReadSession>);
static_assert(!std::is_copy_constructible_v<ReadStatement>);
static_assert(!std::is_copy_assignable_v<ReadStatement>);
static_assert(std::is_nothrow_move_constructible_v<ReadStatement>);
static_assert(std::is_nothrow_move_assignable_v<ReadStatement>);

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

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "catalog_loader" / "sqlite-3.54.0-catalog.db";
}

[[nodiscard]] std::filesystem::path ReadCompatibilityFixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "read_compatibility" / "sqlite-3.54.0-read-compatibility.db";
}

[[nodiscard]] std::filesystem::path MembershipFixturePath() {
  return ReadCompatibilityFixturePath().parent_path().parent_path() / "in_membership" /
         "sqlite-3.54.0-in-membership.db";
}

[[nodiscard]] std::filesystem::path IndexFixturePath() {
  return ReadCompatibilityFixturePath().parent_path().parent_path() / "index_performance" /
         "indexed.db";
}

[[nodiscard]] std::filesystem::path JoinedIndexFixturePath() {
  return ReadCompatibilityFixturePath().parent_path().parent_path() / "joined_index" /
         "sqlite-3.54.0-joined-index.db";
}

[[nodiscard]] std::filesystem::path SessionFixturePath(std::string_view name) {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "session" / std::string{name};
}

[[nodiscard]] std::filesystem::path UniquePath(std::string_view label) {
  static std::atomic<std::uint64_t> sequence{0};
  return std::filesystem::temp_directory_path() /
         ("modern-sqlite-session-" + std::string{label} + "-" +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".db");
}

class TemporaryFile final {
 public:
  struct Contents {
    std::string_view bytes;
  };

  TemporaryFile(std::string_view label, Contents contents)
      : path_(UniquePath(label)), journal_path_(path_), wal_path_(path_) {
    journal_path_ += "-journal";
    wal_path_ += "-wal";
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error{"unable to create session test file"};
    }
    stream.write(contents.bytes.data(), static_cast<std::streamsize>(contents.bytes.size()));
    if (!stream) {
      throw std::runtime_error{"unable to write session test file"};
    }
  }

  TemporaryFile(std::string_view label, const std::filesystem::path& source)
      : path_(UniquePath(label)), journal_path_(path_), wal_path_(path_) {
    journal_path_ += "-journal";
    wal_path_ += "-wal";
    RewriteFrom(source);
  }

  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;

  ~TemporaryFile() noexcept {
    std::error_code error;
    std::filesystem::remove(path_, error);
    std::filesystem::remove(journal_path_, error);
    std::filesystem::remove(wal_path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  void RewriteFrom(const std::filesystem::path& source) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
      throw std::runtime_error{"unable to open session fixture"};
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error{"unable to rewrite session test database"};
    }
    if (!bytes.empty()) {
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (!output) {
      throw std::runtime_error{"unable to write session fixture bytes"};
    }
  }

 private:
  std::filesystem::path path_;
  std::filesystem::path journal_path_;
  std::filesystem::path wal_path_;
};

struct UnlockFailureState {
  int failures_remaining = 0;
  std::size_t attempts = 0;
  std::size_t delegated_calls = 0;
  bool fail_reads = false;
  std::optional<std::size_t> successful_reads_before_failure{};
};

class UnlockFailingFile final : public File {
 public:
  UnlockFailingFile(std::unique_ptr<File> delegate, std::shared_ptr<UnlockFailureState> state)
      : delegate_(std::move(delegate)), state_(std::move(state)) {}

 private:
  Result<ByteCount> DoReadAt(MutableByteView destination, FileOffset offset) override {
    ++state_->delegated_calls;
    if (state_->fail_reads) {
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected read failure"));
    }
    if (state_->successful_reads_before_failure.has_value()) {
      if (*state_->successful_reads_before_failure == 0U) {
        return std::unexpected(Error::Create(ErrorCode::kIo, "injected retry read failure"));
      }
      --*state_->successful_reads_before_failure;
    }
    auto read = delegate_->ReadAt(destination, offset);
    if (!read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    return read->bytes_read();
  }

  Status DoWriteAt(ByteView source, FileOffset offset) override {
    ++state_->delegated_calls;
    return delegate_->WriteAt(source, offset);
  }

  Status DoTruncate(FileSize size) override {
    ++state_->delegated_calls;
    return delegate_->Truncate(size);
  }

  Status DoSync(SyncOptions options) override {
    ++state_->delegated_calls;
    return delegate_->Sync(options);
  }

  Result<FileSize> DoSize() override {
    ++state_->delegated_calls;
    return delegate_->Size();
  }

  Status DoLock(DatabaseLock lock) override {
    ++state_->delegated_calls;
    return delegate_->Lock(lock);
  }

  Status DoUnlock(DatabaseLock lock) override {
    ++state_->delegated_calls;
    ++state_->attempts;
    if (state_->failures_remaining > 0) {
      --state_->failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected unlock failure"));
    }
    return delegate_->Unlock(lock);
  }

  Result<bool> DoHasReservedLock() override {
    ++state_->delegated_calls;
    return delegate_->HasReservedLock();
  }

  [[nodiscard]] FileProperties DoProperties() const noexcept override {
    ++state_->delegated_calls;
    auto properties = delegate_->Properties();
    if (properties.has_value()) {
      return *properties;
    }
    return FileProperties{
        .sector_size = ByteCount{1},
        .device_characteristics = {},
    };
  }

  Result<std::optional<MutableByteView>> DoMapSharedMemory(SharedMemoryRegionIndex region,
                                                           ByteCount region_size,
                                                           SharedMemoryMapMode mode) override {
    ++state_->delegated_calls;
    return delegate_->MapSharedMemory(region, region_size, mode);
  }

  Status DoLockSharedMemory(SharedMemoryLockRange range, SharedMemoryLockOperation operation,
                            SharedMemoryLockMode mode) override {
    ++state_->delegated_calls;
    return delegate_->LockSharedMemory(range, operation, mode);
  }

  void DoSharedMemoryBarrier() noexcept override {
    ++state_->delegated_calls;
    delegate_->SharedMemoryBarrier();
  }

  Status DoUnmapSharedMemory(SharedMemoryUnmapMode mode) override {
    ++state_->delegated_calls;
    return delegate_->UnmapSharedMemory(mode);
  }

  std::unique_ptr<File> delegate_;
  std::shared_ptr<UnlockFailureState> state_;
};

class UnlockFailingVfs final : public Vfs {
 public:
  explicit UnlockFailingVfs(std::shared_ptr<UnlockFailureState> state) : state_(std::move(state)) {}

 private:
  Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                            FileOpenOptions options) override {
    ++state_->delegated_calls;
    auto opened = delegate_.Open(path, options);
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    const FileAccessMode access = opened->access;
    auto file = std::make_unique<UnlockFailingFile>(std::move(opened->file), state_);
    return OpenedFile{
        .file = std::move(file),
        .access = access,
    };
  }

  Status DoDelete(std::string_view path, DirectorySync directory_sync) override {
    ++state_->delegated_calls;
    return delegate_.Delete(path, directory_sync);
  }

  Result<bool> DoAccess(std::string_view path, FileAccessQuery query) override {
    ++state_->delegated_calls;
    return delegate_.Access(path, query);
  }

  Result<std::string> DoFullPath(std::string_view path) override {
    ++state_->delegated_calls;
    return delegate_.FullPath(path);
  }

  Result<ByteCount> DoRandomBytes(MutableByteView output) override {
    ++state_->delegated_calls;
    auto random = delegate_.RandomBytes(output);
    if (!random.has_value()) {
      return std::unexpected(std::move(random.error()));
    }
    return ByteCount{output.size()};
  }

  Result<std::chrono::microseconds> DoSleepFor(std::chrono::microseconds duration) override {
    ++state_->delegated_calls;
    return delegate_.SleepFor(duration);
  }

  Result<WallClockTime> DoCurrentTime() override {
    ++state_->delegated_calls;
    return delegate_.CurrentTime();
  }

  [[nodiscard]] ByteCount DoMaximumPathLength() const noexcept override {
    ++state_->delegated_calls;
    return delegate_.MaximumPathLength();
  }

  NativeVfs delegate_;
  std::shared_ptr<UnlockFailureState> state_;
};

struct TemporaryFileState {
  std::optional<std::size_t> writes_until_failure{};
  std::size_t open_count = 0;
  std::size_t live_count = 0;
  std::size_t peak_live_count = 0;
  std::optional<FileOffset> main_read_failure_offset{};
  std::size_t main_read_failures = 0;
};

class TemporaryTrackingFile final : public File {
 public:
  TemporaryTrackingFile(std::unique_ptr<File> delegate, std::shared_ptr<TemporaryFileState> state,
                        bool temporary)
      : delegate_(std::move(delegate)), state_(std::move(state)), temporary_(temporary) {
    if (temporary_) {
      ++state_->open_count;
      ++state_->live_count;
      if (state_->live_count > state_->peak_live_count) {
        state_->peak_live_count = state_->live_count;
      }
    }
  }

  ~TemporaryTrackingFile() override {
    if (temporary_) {
      --state_->live_count;
    }
  }

 private:
  Result<ByteCount> DoReadAt(MutableByteView destination, FileOffset offset) override {
    if (!temporary_ && state_->main_read_failure_offset == offset) {
      state_->main_read_failure_offset.reset();
      ++state_->main_read_failures;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected source cursor read failure"));
    }
    auto read = delegate_->ReadAt(destination, offset);
    if (!read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    return read->bytes_read();
  }

  Status DoWriteAt(ByteView source, FileOffset offset) override {
    if (temporary_ && state_->writes_until_failure.has_value()) {
      if (*state_->writes_until_failure == 0U) {
        state_->writes_until_failure.reset();
        return std::unexpected(Error::Create(ErrorCode::kIo, "injected temporary write failure"));
      }
      --*state_->writes_until_failure;
    }
    return delegate_->WriteAt(source, offset);
  }

  Status DoTruncate(FileSize size) override { return delegate_->Truncate(size); }

  Status DoSync(SyncOptions options) override { return delegate_->Sync(options); }

  Result<FileSize> DoSize() override { return delegate_->Size(); }

  Status DoLock(DatabaseLock lock) override { return delegate_->Lock(lock); }

  Status DoUnlock(DatabaseLock lock) override { return delegate_->Unlock(lock); }

  Result<bool> DoHasReservedLock() override { return delegate_->HasReservedLock(); }

  [[nodiscard]] FileProperties DoProperties() const noexcept override {
    auto properties = delegate_->Properties();
    if (properties.has_value()) {
      return *properties;
    }
    return FileProperties{
        .sector_size = ByteCount{1},
        .device_characteristics = {},
    };
  }

  Result<std::optional<MutableByteView>> DoMapSharedMemory(SharedMemoryRegionIndex region,
                                                           ByteCount region_size,
                                                           SharedMemoryMapMode mode) override {
    return delegate_->MapSharedMemory(region, region_size, mode);
  }

  Status DoLockSharedMemory(SharedMemoryLockRange range, SharedMemoryLockOperation operation,
                            SharedMemoryLockMode mode) override {
    return delegate_->LockSharedMemory(range, operation, mode);
  }

  void DoSharedMemoryBarrier() noexcept override { delegate_->SharedMemoryBarrier(); }

  Status DoUnmapSharedMemory(SharedMemoryUnmapMode mode) override {
    return delegate_->UnmapSharedMemory(mode);
  }

  std::unique_ptr<File> delegate_;
  std::shared_ptr<TemporaryFileState> state_;
  bool temporary_;
};

class TemporaryTrackingVfs final : public Vfs {
 public:
  explicit TemporaryTrackingVfs(std::shared_ptr<TemporaryFileState> state)
      : state_(std::move(state)) {}

 private:
  Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                            FileOpenOptions options) override {
    auto opened = delegate_.Open(path, options);
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    const FileAccessMode access = opened->access;
    auto file =
        std::make_unique<TemporaryTrackingFile>(std::move(opened->file), state_, !path.has_value());
    return OpenedFile{
        .file = std::move(file),
        .access = access,
    };
  }

  Status DoDelete(std::string_view path, DirectorySync directory_sync) override {
    return delegate_.Delete(path, directory_sync);
  }

  Result<bool> DoAccess(std::string_view path, FileAccessQuery query) override {
    return delegate_.Access(path, query);
  }

  Result<std::string> DoFullPath(std::string_view path) override {
    return delegate_.FullPath(path);
  }

  Result<ByteCount> DoRandomBytes(MutableByteView output) override {
    auto random = delegate_.RandomBytes(output);
    if (!random.has_value()) {
      return std::unexpected(std::move(random.error()));
    }
    return ByteCount{output.size()};
  }

  Result<std::chrono::microseconds> DoSleepFor(std::chrono::microseconds duration) override {
    return delegate_.SleepFor(duration);
  }

  Result<WallClockTime> DoCurrentTime() override { return delegate_.CurrentTime(); }

  [[nodiscard]] ByteCount DoMaximumPathLength() const noexcept override {
    return delegate_.MaximumPathLength();
  }

  NativeVfs delegate_;
  std::shared_ptr<TemporaryFileState> state_;
};

[[nodiscard]] ReadStatement PrepareStatement(ReadSession& session, std::string_view sql) {
  ReadPrepareOutput output = TakeValue(session.Prepare(Utf8View{sql}));
  if (!output.statement.has_value()) {
    throw std::runtime_error{"session test SQL produced no statement"};
  }
  return std::move(*output.statement);
}

[[nodiscard]] std::int64_t IntegerValue(const SqlValue& value) {
  const std::optional<std::int64_t> integer = value.integer_value();
  if (!integer.has_value()) {
    throw std::runtime_error{"expected integer session result"};
  }
  return *integer;
}

[[nodiscard]] std::string_view TextValue(const SqlValue& value) {
  const std::optional<Utf8View> text = value.text_value();
  if (!text.has_value()) {
    throw std::runtime_error{"expected text session result"};
  }
  return text->bytes();
}

constexpr std::string_view kIntegratedAggregateSql =
    "SELECT nullable IS NULL AS missing_value,"
    "count() AS row_count,"
    "min(label) AS first_label,"
    "group_concat(label ORDER BY id DESC) FILTER (WHERE value>=?1) AS labels,"
    "count(DISTINCT value<0) AS signs "
    "FROM model_rows "
    "GROUP BY nullable IS NULL "
    "HAVING count()>?2 "
    "ORDER BY 1";

constexpr std::string_view kSpillingAggregateCompoundSql =
    "SELECT count(),group_concat(label ORDER BY id DESC) "
    "FROM model_rows GROUP BY nullable IS NULL "
    "UNION SELECT 99,'unused' LIMIT 1";

void ExpectIntegratedAggregateMetadata(const ReadStatement& statement) {
  constexpr std::array<std::string_view, 5> kNames{
      "missing_value", "row_count", "first_label", "labels", "signs",
  };
  ASSERT_EQ(kNames.size(), statement.result_columns().size());
  for (std::size_t index = 0; index < kNames.size(); ++index) {
    EXPECT_EQ(kNames[index], statement.result_columns()[index].name);
    EXPECT_EQ(std::nullopt, statement.result_columns()[index].declared_type);
    EXPECT_EQ(TypeAffinity::kNone, statement.result_columns()[index].affinity);
  }
}

void ExpectIntegratedAggregateFirstExecution(ReadStatement& statement) {
  RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(0)));

  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(5U, statement.row().size());
  EXPECT_EQ(0, IntegerValue(statement.row()[0]));
  EXPECT_EQ(21, IntegerValue(statement.row()[1]));
  EXPECT_EQ("row-02", TextValue(statement.row()[2]));
  EXPECT_EQ(
      "row-32,row-30,row-29,row-27,row-26,row-24,row-23,row-21,row-20,row-18,"
      "row-17,row-15,row-14",
      TextValue(statement.row()[3]));
  EXPECT_EQ(2, IntegerValue(statement.row()[4]));

  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(11, IntegerValue(statement.row()[1]));
  EXPECT_EQ("row-01", TextValue(statement.row()[2]));
  EXPECT_EQ("row-31,row-28,row-25,row-22,row-19,row-16,row-13", TextValue(statement.row()[3]));
  EXPECT_EQ(2, IntegerValue(statement.row()[4]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

void ExpectIntegratedAggregateReboundExecution(ReadStatement& statement) {
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(40)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(15)));

  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(5U, statement.row().size());
  EXPECT_EQ(0, IntegerValue(statement.row()[0]));
  EXPECT_EQ(21, IntegerValue(statement.row()[1]));
  EXPECT_EQ("row-02", TextValue(statement.row()[2]));
  EXPECT_EQ("row-32,row-30,row-29,row-27", TextValue(statement.row()[3]));
  EXPECT_EQ(2, IntegerValue(statement.row()[4]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

void ExerciseIntegratedAggregate(ReadStatement& statement) {
  ExpectIntegratedAggregateMetadata(statement);
  ExpectIntegratedAggregateFirstExecution(statement);
  ExpectIntegratedAggregateReboundExecution(statement);
  RequireStatus(statement.Finalize());
}

TEST(ReadSession, OpensLazilyAndReportsMissingCorruptAndEmptyDatabases) {
  const auto null_vfs = ReadSession::Open(std::unique_ptr<Vfs>{}, "unused.db");
  ASSERT_FALSE(null_vfs.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, null_vfs.error().code());

  const std::filesystem::path missing = UniquePath("missing");
  const auto missing_open = ReadSession::Open(missing.string());
  ASSERT_FALSE(missing_open.has_value());
  EXPECT_EQ(ErrorCode::kCannotOpen, missing_open.error().code());

  const TemporaryFile corrupt{"corrupt", TemporaryFile::Contents{"not a database"}};
  ReadSession corrupt_session = TakeValue(ReadSession::Open(corrupt.path().string()));
  const auto corrupt_prepare = corrupt_session.Prepare(Utf8View{"SELECT 1"});
  ASSERT_FALSE(corrupt_prepare.has_value());
  EXPECT_EQ(ErrorCode::kNotDatabase, corrupt_prepare.error().code());

  const TemporaryFile empty{"empty", TemporaryFile::Contents{}};
  ReadSession empty_session = TakeValue(ReadSession::Open(empty.path().string()));
  ReadStatement constant = PrepareStatement(empty_session, "SELECT 1");
  EXPECT_EQ(ReadStep::kRow, TakeValue(constant.Step()));
  EXPECT_EQ(1, IntegerValue(constant.row().front()));
  EXPECT_EQ(ReadStep::kDone, TakeValue(constant.Step()));

  ReadStatement schema = PrepareStatement(empty_session, "SELECT name FROM sqlite_schema");
  EXPECT_EQ(ReadStep::kDone, TakeValue(schema.Step()));
}

TEST(ReadSession, AcceptsTemporaryStorageOptionsOnPathAndVfsOverloads) {
  const ReadSessionOptions memory_options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kMemory,
              .sorter_memory_threshold = ByteCount{4096},
          },
  };
  ReadSession path_session = TakeValue(ReadSession::Open(FixturePath().string(), memory_options));
  ReadStatement path_statement = PrepareStatement(
      path_session, "SELECT id, name FROM items ORDER BY name DESC LIMIT 2 OFFSET 1");
  EXPECT_EQ(ReadStep::kRow, TakeValue(path_statement.Step()));
  EXPECT_EQ(2, IntegerValue(path_statement.row()[0]));
  EXPECT_EQ("beta", TextValue(path_statement.row()[1]));
  EXPECT_EQ(ReadStep::kRow, TakeValue(path_statement.Step()));
  EXPECT_EQ(1, IntegerValue(path_statement.row()[0]));
  EXPECT_EQ("alpha", TextValue(path_statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(path_statement.Step()));
  RequireStatus(path_statement.Reset());
  EXPECT_EQ(ReadStep::kRow, TakeValue(path_statement.Step()));
  EXPECT_EQ(2, IntegerValue(path_statement.row()[0]));
  RequireStatus(path_statement.Finalize());

  const ReadSessionOptions file_options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kFile,
              .sorter_memory_threshold = ByteCount{256},
          },
  };
  ReadSession vfs_session = TakeValue(
      ReadSession::Open(std::make_unique<NativeVfs>(), FixturePath().string(), file_options));
  ReadStatement vfs_statement =
      PrepareStatement(vfs_session, "SELECT id, name FROM items ORDER BY name LIMIT 2");
  EXPECT_EQ(ReadStep::kRow, TakeValue(vfs_statement.Step()));
  EXPECT_EQ(1, IntegerValue(vfs_statement.row()[0]));
  EXPECT_EQ("alpha", TextValue(vfs_statement.row()[1]));
  EXPECT_EQ(ReadStep::kRow, TakeValue(vfs_statement.Step()));
  EXPECT_EQ(2, IntegerValue(vfs_statement.row()[0]));
  EXPECT_EQ("beta", TextValue(vfs_statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(vfs_statement.Step()));
  RequireStatus(vfs_statement.Reset());
  EXPECT_EQ(ReadStep::kRow, TakeValue(vfs_statement.Step()));
  EXPECT_EQ(1, IntegerValue(vfs_statement.row()[0]));
  RequireStatus(vfs_statement.Finalize());

  ReadSessionOptions invalid = memory_options;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  invalid.temporary_storage.mode = static_cast<TemporaryStoreMode>(2);
  const auto rejected = ReadSession::Open(FixturePath().string(), invalid);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
}

TEST(ReadSession, ExecutesDistinctValuesAndCompoundsThroughThePublicApi) {
  const ReadSessionOptions memory_options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kMemory,
          },
  };
  ReadSession memory = TakeValue(ReadSession::Open(FixturePath().string(), memory_options));
  ReadStatement set = PrepareStatement(
      memory, "VALUES(3),(1),(2) UNION SELECT DISTINCT id FROM items ORDER BY 1 LIMIT 4");
  for (std::int64_t expected = 1; expected <= 3; ++expected) {
    EXPECT_EQ(ReadStep::kRow, TakeValue(set.Step()));
    EXPECT_EQ(expected, IntegerValue(set.row()[0]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(set.Step()));

  ReadStatement rebound =
      PrepareStatement(memory, "VALUES(?1),(?2) UNION ALL SELECT ?3 ORDER BY 1 LIMIT 2 OFFSET 1");
  const auto run = [&](std::int64_t first, std::int64_t second, std::int64_t third) {
    RequireStatus(rebound.Bind(1, SqlValue::Integer(first)));
    RequireStatus(rebound.Bind(2, SqlValue::Integer(second)));
    RequireStatus(rebound.Bind(3, SqlValue::Integer(third)));
    EXPECT_EQ(ReadStep::kRow, TakeValue(rebound.Step()));
    EXPECT_EQ(second, IntegerValue(rebound.row()[0]));
    EXPECT_EQ(ReadStep::kRow, TakeValue(rebound.Step()));
    EXPECT_EQ(third, IntegerValue(rebound.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(rebound.Step()));
  };
  run(1, 2, 3);
  RequireStatus(rebound.Reset());
  run(4, 5, 6);

  const ReadSessionOptions file_options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kFile,
              .sorter_memory_threshold = ByteCount{256},
          },
  };
  ReadSession file = TakeValue(ReadSession::Open(FixturePath().string(), file_options));
  ReadStatement ordered = PrepareStatement(file,
                                           "SELECT 'A' COLLATE NOCASE "
                                           "UNION SELECT 'B' "
                                           "UNION SELECT 'a' "
                                           "ORDER BY 1 COLLATE BINARY");
  EXPECT_EQ(ReadStep::kRow, TakeValue(ordered.Step()));
  EXPECT_EQ("B", TextValue(ordered.row()[0]));
  EXPECT_EQ(ReadStep::kRow, TakeValue(ordered.Step()));
  EXPECT_EQ("a", TextValue(ordered.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(ordered.Step()));
}

TEST(ReadSession, PreparesOneStatementAndPublishesTheTailOffset) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  constexpr std::string_view sql = " ; /* empty */ ; SELECT 1; SELECT 2";

  ReadPrepareOutput first = TakeValue(session.Prepare(Utf8View{sql}));
  ASSERT_TRUE(first.statement.has_value());
  EXPECT_EQ(26U, first.next_offset.value());
  EXPECT_EQ(ReadStep::kRow, TakeValue(first.statement->Step()));
  EXPECT_EQ(1, IntegerValue(first.statement->row().front()));

  constexpr std::string_view empty = " ; -- only empty\n ; ";
  const ReadPrepareOutput none = TakeValue(session.Prepare(Utf8View{empty}));
  EXPECT_FALSE(none.statement.has_value());
  EXPECT_EQ(empty.size(), none.next_offset.value());

  const auto syntax = session.Prepare(Utf8View{"SELECT )"});
  ASSERT_FALSE(syntax.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, syntax.error().code());

  ReadStatement order_by =
      PrepareStatement(session, "SELECT Name FROM Items ORDER BY Name LIMIT 1");
  EXPECT_EQ(ReadStep::kRow, TakeValue(order_by.Step()));
  EXPECT_EQ("alpha", TextValue(order_by.row().front()));
  EXPECT_EQ(ReadStep::kDone, TakeValue(order_by.Step()));
}

TEST(ReadSession, ExecutesInnerLeftOnAndWhereWithoutFakeUnmatchedRows) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement inner =
      PrepareStatement(session,
                       "SELECT a.id,b.id FROM Items a INNER JOIN Items b ON a.id=b.id AND b.id<2 "
                       "ORDER BY a.id");
  ASSERT_EQ(ReadStep::kRow, TakeValue(inner.Step()));
  EXPECT_EQ(1, IntegerValue(inner.row()[0]));
  EXPECT_EQ(1, IntegerValue(inner.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(inner.Step()));
  ReadStatement left = PrepareStatement(session,
                                        "SELECT a.id,b.id,b._rowid_ FROM Items a LEFT JOIN Items b "
                                        "ON a.id=b.id AND b.id<2 ORDER BY a.id");
  for (std::int64_t id = 1; id <= 3; ++id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(left.Step()));
    EXPECT_EQ(id, IntegerValue(left.row()[0]));
    if (id == 1) {
      EXPECT_EQ(1, IntegerValue(left.row()[1]));
      EXPECT_EQ(1, IntegerValue(left.row()[2]));
    } else {
      EXPECT_EQ(SqlValueType::kNull, left.row()[1].type());
      EXPECT_EQ(SqlValueType::kNull, left.row()[2].type());
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(left.Step()));
  ReadStatement filtered =
      PrepareStatement(session,
                       "SELECT a.id FROM Items a LEFT JOIN Items b ON a.id=b.id "
                       "WHERE b.id IS NULL");
  EXPECT_EQ(ReadStep::kDone, TakeValue(filtered.Step()));
}

TEST(ReadSession, ExecutesMergedUsingAndNaturalOutputWithoutLosingQualifiedRightNulls) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement using_columns =
      PrepareStatement(session,
                       "SELECT id,a.id,b.id FROM model_rows a LEFT JOIN items b USING(id) "
                       "WHERE a.id<=5 ORDER BY id");
  for (std::int64_t id = 1; id <= 5; ++id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(using_columns.Step()));
    EXPECT_EQ(id, IntegerValue(using_columns.row()[0]));
    EXPECT_EQ(id, IntegerValue(using_columns.row()[1]));
    if (id == 5) {
      EXPECT_EQ(SqlValueType::kNull, using_columns.row()[2].type());
    } else {
      EXPECT_EQ(id, IntegerValue(using_columns.row()[2]));
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(using_columns.Step()));
  ReadStatement natural = PrepareStatement(
      session, "SELECT a.id,b.id FROM items a NATURAL LEFT JOIN items b ORDER BY a.id");
  for (std::int64_t id = 1; id <= 4; ++id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(natural.Step()));
    EXPECT_EQ(id, IntegerValue(natural.row()[0]));
    if (id == 3) {
      EXPECT_EQ(SqlValueType::kNull, natural.row()[1].type());
    } else {
      EXPECT_EQ(id, IntegerValue(natural.row()[1]));
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(natural.Step()));
}

TEST(ReadSession, PreservesUnmatchedRightRowsAndNeverReplaysWhereRejectedMatches) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT a.id,b.id FROM items a RIGHT JOIN model_rows b ON a.id=b.id "
                       "WHERE b.id<=5 ORDER BY b.id");
  for (std::int64_t id = 1; id <= 5; ++id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(id, IntegerValue(statement.row()[1]));
    if (id == 5) {
      EXPECT_EQ(SqlValueType::kNull, statement.row()[0].type());
    } else {
      EXPECT_EQ(id, IntegerValue(statement.row()[0]));
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  ReadStatement rejected =
      PrepareStatement(session,
                       "SELECT a.id,b.id FROM items a RIGHT JOIN items b ON a.id=b.id "
                       "WHERE a.id IS NULL");
  EXPECT_EQ(ReadStep::kDone, TakeValue(rejected.Step()));
}

TEST(ReadSession, PreservesBothFullJoinSidesAndPublishesMergedKeys) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement disjoint = PrepareStatement(
      session, "SELECT count(),count(a.id),count(b.id) FROM items a FULL JOIN items b ON 0");
  ASSERT_EQ(ReadStep::kRow, TakeValue(disjoint.Step()));
  EXPECT_EQ(8, IntegerValue(disjoint.row()[0]));
  EXPECT_EQ(4, IntegerValue(disjoint.row()[1]));
  EXPECT_EQ(4, IntegerValue(disjoint.row()[2]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(disjoint.Step()));
  ReadStatement merged =
      PrepareStatement(session,
                       "SELECT id,a.id,b.id FROM items a FULL JOIN model_rows b USING(id) "
                       "WHERE id<=5 ORDER BY id");
  EXPECT_EQ(std::nullopt, merged.result_columns()[0].declared_type);
  for (std::int64_t id = 1; id <= 5; ++id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(merged.Step()));
    EXPECT_EQ(id, IntegerValue(merged.row()[0]));
    EXPECT_EQ(id, IntegerValue(merged.row()[2]));
    if (id == 5) {
      EXPECT_EQ(SqlValueType::kNull, merged.row()[1].type());
    } else {
      EXPECT_EQ(id, IntegerValue(merged.row()[1]));
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(merged.Step()));
}

TEST(ReadSession, ExecutesNativeScalarInExpressionLists) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session, "SELECT 2 IN (1,2,3),4 NOT IN (1,2,NULL),NULL IN (),NULL NOT IN ()");
  ASSERT_EQ(4U, statement.result_columns().size());
  for (std::size_t execution = 0; execution < 2U; ++execution) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    ASSERT_EQ(4U, statement.row().size());
    EXPECT_EQ(1, IntegerValue(statement.row()[0]));
    EXPECT_EQ(SqlValueType::kNull, statement.row()[1].type());
    EXPECT_EQ(0, IntegerValue(statement.row()[2]));
    EXPECT_EQ(1, IntegerValue(statement.row()[3]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Reset());
  }
}

TEST(ReadSession, PreservesNativeInParserRewritesCollationsAndBooleanNameResolution) {
  struct Case {
    std::string_view sql;
    std::vector<std::optional<std::int64_t>> expected;
  };
  const std::array cases{
      Case{.sql = "SELECT absent_column IN (),absent_column NOT IN (),"
                  "(SELECT missing_function()) IN ()",
           .expected = {0, 1, 0}},
      Case{.sql = "SELECT 'a' IN ('A' COLLATE NOCASE),"
                  "'a' IN ('A' COLLATE NOCASE,'b'),"
                  "'a' IN ('A' COLLATE NOCASE,'b','c'),"
                  "'a' COLLATE NOCASE IN ('A','b','c')",
           .expected = {1, 0, 0, 1}},
      Case{.sql = "SELECT 'a' IN (\"A\" COLLATE NOCASE),"
                  "1 IN (1 COLLATE missing,2),1 IN (1 COLLATE missing,2,3)",
           .expected = {0, 1, 1}},
      Case{.sql = "SELECT ((1 IN (1 COLLATE NOCASE,2))||'a')='1A',"
                  "((1 IN (1 COLLATE NOCASE))||'a')='1A'",
           .expected = {1, 1}},
      Case{.sql = "SELECT TRUE,TRUE IN (TRUE),FALSE,FALSE IN (FALSE) FROM booleans",
           .expected = {0, 0, 1, 0}},
      Case{.sql = "SELECT TRUE IN (TRUE,99),FALSE IN (FALSE,99) FROM booleans", .expected = {1, 1}},
      Case{.sql = "SELECT 1 IN (TRUE+x),1 IN (x+TRUE),"
                  "1 IN (abs(TRUE)+x),1 IN (x+abs(TRUE)) FROM booleans,t WHERE x=1",
           .expected = {0, 1, 0, 1}},
      Case{.sql = "SELECT 'abc' IS (abs(1) IN ()),'abc' IS (1 IN ())", .expected = {0, 1}},
      Case{.sql = "SELECT coalesce((SELECT 1,2),0) IN (),"
                  "coalesce(1 COLLATE missing,0) IN ()",
           .expected = {0, 0}},
      Case{.sql = "SELECT 2 IN (+(SELECT x FROM t WHERE x IS NOT NULL)),"
                  "2 IN ((SELECT x FROM t WHERE x IS NOT NULL),99)",
           .expected = {0, 0}},
      Case{.sql = "SELECT 3 IN (1,NULL),3 NOT IN (1,NULL),NULL IN (1,2,3)",
           .expected = {std::nullopt, std::nullopt, std::nullopt}},
      Case{.sql = "SELECT 1 IN (1,abs(-9223372036854775808)),"
                  "abs(-9223372036854775808) IN ()",
           .expected = {1, 0}},
  };
  ReadSession session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    auto statement = PrepareStatement(session, test_case.sql);
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    ASSERT_EQ(test_case.expected.size(), statement.row().size());
    for (std::size_t index = 0; index < test_case.expected.size(); ++index) {
      if (test_case.expected[index].has_value()) {
        EXPECT_EQ(*test_case.expected[index], IntegerValue(statement.row()[index]));
      } else {
        EXPECT_EQ(SqlValueType::kNull, statement.row()[index].type());
      }
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
}

TEST(ReadSession, RejectsNormalizedInConstructionOverflowBeforeAnEmptyListCanDeleteIt) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  const auto query = [](std::size_t singletons) {
    std::string sql = "SELECT 1";
    for (std::size_t index = 0; index < singletons; ++index) {
      sql += " IN (1)";
    }
    return sql;
  };
  const auto rejected =
      session.Prepare(Utf8View{query(kMaximumExpressionConstructionDepth - 1U) + " IN ()"});
  EXPECT_FALSE(rejected.has_value());
  auto admitted =
      PrepareStatement(session, query(kMaximumExpressionConstructionDepth - 2U) + " IN ()");
  ASSERT_EQ(ReadStep::kRow, TakeValue(admitted.Step()));
  EXPECT_EQ(0, IntegerValue(admitted.row()[0]));
  auto discarded = PrepareStatement(session, "SELECT absent_column IN (1) IN ()");
  ASSERT_EQ(ReadStep::kRow, TakeValue(discarded.Step()));
  EXPECT_EQ(0, IntegerValue(discarded.row()[0]));
  EXPECT_FALSE(session
                   .Prepare(Utf8View{"SELECT (" + query(kMaximumExpressionConstructionDepth - 2U) +
                                     ") IN ()"})
                   .has_value());
  constexpr std::size_t kUnaryPrefixes = 400;
  constexpr std::size_t kAdditionalSingletons =
      kMaximumExpressionConstructionDepth - kUnaryPrefixes - 2U;
  std::string folded = "SELECT 1 IN (" + std::string(kUnaryPrefixes, '+') + "1)";
  for (std::size_t index = 0; index < kAdditionalSingletons; ++index) {
    folded += " IN (1)";
  }
  auto unary = PrepareStatement(session, folded + " IN ()");
  ASSERT_EQ(ReadStep::kRow, TakeValue(unary.Step()));
  EXPECT_EQ(0, IntegerValue(unary.row()[0]));
}

TEST(ReadSession, PreservesNativeCollateConstructionHeightAfterValidatingItsOperand) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  std::string operand = "1 IN (1 IN (1))";
  constexpr std::size_t kNestedSingletonHeight = 5;
  for (std::size_t index = kNestedSingletonHeight; index < kMaximumExpressionConstructionDepth;
       ++index) {
    operand += " IN (1)";
  }
  const auto collate = [](const std::string& value) {
    return "SELECT (NOT ((" + value + ") COLLATE BINARY)) IN ()";
  };
  auto admitted = PrepareStatement(session, collate(operand));
  ASSERT_EQ(ReadStep::kRow, TakeValue(admitted.Step()));
  EXPECT_EQ(0, IntegerValue(admitted.row()[0]));
  const auto rejected = session.Prepare(Utf8View{collate(operand + " IN (1)")});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, rejected.error().code());
}

TEST(ReadSession, PreservesNativeFunctionAttachmentConstructionHeights) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  std::string operand = "1 IN (1 IN (1))";
  constexpr std::size_t kNestedSingletonHeight = 5;
  for (std::size_t index = kNestedSingletonHeight; index < kMaximumExpressionConstructionDepth;
       ++index) {
    operand += " IN (1)";
  }
  for (const std::string& sql : {"SELECT sum(1) FILTER (WHERE " + operand + ") IN ()",
                                 "SELECT sum(1 ORDER BY " + operand + ") IN ()"}) {
    auto statement = PrepareStatement(session, sql);
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(0, IntegerValue(statement.row()[0]));
  }
}

TEST(ReadSession, PreservesNativeCollateFunctionAndAggregatePresence) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  auto scalar = PrepareStatement(session, "SELECT missing_function() COLLATE BINARY IN ()");
  ASSERT_EQ(ReadStep::kRow, TakeValue(scalar.Step()));
  EXPECT_EQ(0, IntegerValue(scalar.row()[0]));
  auto aggregate = PrepareStatement(session, "SELECT sum(x) COLLATE BINARY IN () FROM t");
  for (std::size_t row = 0; row < 3U; ++row) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(aggregate.Step()));
    EXPECT_EQ(0, IntegerValue(aggregate.row()[0]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(aggregate.Step()));
}

TEST(ReadSession, PreservesNativeInInducedAndDeletionAndIntegerConstantIdentity) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  auto statement =
      PrepareStatement(session,
                       "SELECT (1 IN ()) AND absent_column,absent_column AND (1 IN ()),"
                       "((1 IN ()) AND absent_column) AND another_column,"
                       "'abc' IS ((1 IN ()) AND absent_column),1 IN ((1 IN ()) AND absent_column),"
                       "(1 IN ()) AND (SELECT missing_function()),"
                       "(1 IN ()) AND (missing_function() COLLATE BINARY)");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(7U, statement.row().size());
  for (const auto& value : statement.row()) {
    EXPECT_EQ(0, IntegerValue(value));
  }
  auto parameters = PrepareStatement(session, "SELECT ?9 AND (1 IN ())");
  EXPECT_EQ(9U, parameters.parameter_count());
  ASSERT_EQ(ReadStep::kRow, TakeValue(parameters.Step()));
  EXPECT_EQ(0, IntegerValue(parameters.row()[0]));
  for (const std::string_view sql : {
           "SELECT (1 IN ()) AND missing_function()",
           "SELECT (1 NOT IN ()) OR absent_column",
           "SELECT ((1 IN ()) COLLATE BINARY) AND absent_column",
       }) {
    EXPECT_FALSE(session.Prepare(Utf8View{sql}).has_value());
  }
}

TEST(ReadSession, PreservesNativeIntegerZeroFlagsBeforeInClassification) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  for (const std::string_view operand : {"0", "00", "0x0", "0X00000000000000000000", "(0)"}) {
    SCOPED_TRACE(operand);
    const std::string sql =
        "SELECT 1 IN (1," + std::string{operand} + " AND x,abs(-9223372036854775808)) FROM t";
    auto prepared = session.Prepare(Utf8View{sql});
    ASSERT_TRUE(prepared.has_value()) << sql;
    ASSERT_TRUE(prepared->statement.has_value());
    auto statement = std::move(*prepared->statement);
    const auto failed = statement.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  }
  for (const std::string_view operand :
       {"0_0", "0_1", "0x0_0", "0.0", "0e0", "+0", "-0", "0 COLLATE BINARY"}) {
    SCOPED_TRACE(operand);
    const std::string sql =
        "SELECT 1 IN (1," + std::string{operand} + " AND x,abs(-9223372036854775808)) FROM t";
    auto prepared = session.Prepare(Utf8View{sql});
    ASSERT_TRUE(prepared.has_value()) << sql;
    ASSERT_TRUE(prepared->statement.has_value());
    auto statement = std::move(*prepared->statement);
    for (std::size_t row = 0; row < 3U; ++row) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(1, IntegerValue(statement.row()[0]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  auto deleted =
      PrepareStatement(session, "SELECT 1 IN (0 AND absent_column),(0x10000000000000000) IN ()");
  ASSERT_EQ(ReadStep::kRow, TakeValue(deleted.Step()));
  EXPECT_EQ(0, IntegerValue(deleted.row()[0]));
  EXPECT_EQ(0, IntegerValue(deleted.row()[1]));
}

TEST(ReadSession, PreservesNativeNullTestFoldedConstructionHeights) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  std::string sql = "SELECT 1 IN (1 IS NULL)";
  constexpr std::size_t kLeadingExpressionHeight = 3;
  for (std::size_t index = kLeadingExpressionHeight; index < kMaximumExpressionConstructionDepth;
       ++index) {
    sql += " IN (1)";
  }
  auto statement = PrepareStatement(session, sql + " IN ()");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(0, IntegerValue(statement.row()[0]));
}

TEST(ReadSession, RetainsNativeFunctionConstructorLimitsBeforeInDeletion) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  const auto query = [](std::size_t arguments) {
    std::string sql = "SELECT missing_function(";
    for (std::size_t index = 0; index < arguments; ++index) {
      sql += index == 0U ? "1" : ",1";
    }
    return sql + ") COLLATE BINARY IN ()";
  };
  constexpr std::size_t kMaximumArguments = BindOptions{}.maximum_function_arguments;
  const auto rejected = session.Prepare(Utf8View{query(kMaximumArguments + 1U)});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, rejected.error().code());
  auto admitted = PrepareStatement(session, query(kMaximumArguments));
  ASSERT_EQ(ReadStep::kRow, TakeValue(admitted.Step()));
  EXPECT_EQ(0, IntegerValue(admitted.row()[0]));
}

TEST(ReadSession, KeepsInConstructorNullTestsSeparateFromQuotedColumnResolution) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  auto statement = PrepareStatement(session, "SELECT \"x\" IS NULL,x IN (1,2,3) FROM t");
  for (std::size_t row = 0; row < 3U; ++row) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(row == 2U ? 1 : 0, IntegerValue(statement.row()[0]));
    if (row == 2U) {
      EXPECT_EQ(SqlValueType::kNull, statement.row()[1].type());
    } else {
      EXPECT_EQ(1, IntegerValue(statement.row()[1]));
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  auto filtered = PrepareStatement(
      session, "SELECT x FROM t WHERE \"x\" IS NULL AND (x IN (1,2,NULL)) IS NULL");
  ASSERT_EQ(ReadStep::kRow, TakeValue(filtered.Step()));
  EXPECT_EQ(SqlValueType::kNull, filtered.row()[0].type());
  EXPECT_EQ(ReadStep::kDone, TakeValue(filtered.Step()));
}

TEST(ReadSession, DoesNotRewriteInBooleansInsideFilteredAggregateArguments) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  auto statement = PrepareStatement(
      session, "SELECT 1 IN (sum(TRUE) FILTER(WHERE TRUE>=0)),1 IN (sum(TRUE)) FROM booleans");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(0, IntegerValue(statement.row()[0]));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
}

TEST(ReadSession, RetainsNativeInAggregateGroupingAndCorrelatedProducerSemantics) {
  ReadSession session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  for (const std::string_view sql : {"SELECT sum(x) IN (),sum(x) NOT IN () FROM t",
                                     "SELECT sum(x) IN (),sum(x) NOT IN () FROM empty"}) {
    auto statement = PrepareStatement(session, sql);
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(0, IntegerValue(statement.row()[0]));
    EXPECT_EQ(1, IntegerValue(statement.row()[1]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  auto correlated = PrepareStatement(session, "SELECT x,(SELECT x IN (1,2,3)) FROM t");
  for (const std::optional<std::int64_t> x :
       {std::optional<std::int64_t>{1}, std::optional<std::int64_t>{2},
        std::optional<std::int64_t>{}}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(correlated.Step()));
    if (x.has_value()) {
      EXPECT_EQ(*x, IntegerValue(correlated.row()[0]));
      EXPECT_EQ(1, IntegerValue(correlated.row()[1]));
    } else {
      EXPECT_EQ(SqlValueType::kNull, correlated.row()[1].type());
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(correlated.Step()));
  auto grouped =
      PrepareStatement(session, "SELECT x IN (1,2,3),count() FROM t GROUP BY 1 ORDER BY 1");
  ASSERT_EQ(ReadStep::kRow, TakeValue(grouped.Step()));
  EXPECT_EQ(SqlValueType::kNull, grouped.row()[0].type());
  EXPECT_EQ(1, IntegerValue(grouped.row()[1]));
  ASSERT_EQ(ReadStep::kRow, TakeValue(grouped.Step()));
  EXPECT_EQ(1, IntegerValue(grouped.row()[0]));
  EXPECT_EQ(2, IntegerValue(grouped.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(grouped.Step()));
  auto joined = PrepareStatement(session, "SELECT count() FROM t a JOIN t b ON a.x IN (b.x,99)");
  ASSERT_EQ(ReadStep::kRow, TakeValue(joined.Step()));
  EXPECT_EQ(2, IntegerValue(joined.row()[0]));
}

TEST(ReadSession, RebuildsNativeInParameterSetsOnResetInBothTemporaryModes) {
  for (const auto mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    auto session = TakeValue(ReadSession::Open(
        MembershipFixturePath().string(),
        ReadSessionOptions{.temporary_storage = TemporaryStorageOptions{.mode = mode}}));
    auto statement = PrepareStatement(session, "SELECT ?1 IN (?2,?3,?4),?1 NOT IN (?2,?3,?4)");
    EXPECT_EQ(4U, statement.parameter_count());
    const auto execute = [&](const SqlValue& left, const std::array<SqlValue, 3>& values,
                             std::optional<std::int64_t> expected) {
      RequireStatus(statement.Bind(1, left));
      for (std::size_t index = 0; index < values.size(); ++index) {
        RequireStatus(statement.Bind(index + 2U, values[index]));
      }
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      if (expected.has_value()) {
        EXPECT_EQ(*expected, IntegerValue(statement.row()[0]));
        EXPECT_EQ(1 - *expected, IntegerValue(statement.row()[1]));
      } else {
        EXPECT_EQ(SqlValueType::kNull, statement.row()[0].type());
        EXPECT_EQ(SqlValueType::kNull, statement.row()[1].type());
      }
      EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
      RequireStatus(statement.Reset());
    };
    execute(SqlValue::Integer(3),
            {SqlValue::Integer(1), SqlValue::Integer(2), SqlValue::Integer(3)}, 1);
    execute(SqlValue::Integer(3), {SqlValue::Integer(1), SqlValue::Integer(2), SqlValue{}},
            std::nullopt);
    execute(SqlValue{}, {SqlValue::Integer(1), SqlValue::Integer(2), SqlValue::Integer(3)},
            std::nullopt);
    execute(SqlValue::Text("3"), {SqlValue::Integer(1), SqlValue::Integer(2), SqlValue::Integer(3)},
            0);
    execute(SqlValue::Real(3.0), {SqlValue::Integer(1), SqlValue::Integer(2), SqlValue::Integer(3)},
            1);
  }
}

TEST(ReadSession, PreservesNativeInPrepareAndLateErrorBoundaries) {
  auto session = TakeValue(ReadSession::Open(MembershipFixturePath().string()));
  for (const std::string_view sql : {
           "SELECT missing_function() IN ()",
           "SELECT 1 IN (1 COLLATE missing)",
           "SELECT 2 IN ((SELECT x FROM t))",
           "SELECT 2 IN (SELECT x FROM t)",
           "SELECT 1 IN empty",
           "SELECT (1,2) IN ((1,2))",
       }) {
    SCOPED_TRACE(sql);
    EXPECT_FALSE(session.Prepare(Utf8View{sql}).has_value());
  }
  for (const std::string_view sql : {
           "SELECT 1 IN (1,abs(-9223372036854775808),3)",
           "SELECT NULL IN (1,abs(-9223372036854775808))",
           "SELECT sum(abs(-9223372036854775808)) IN () FROM t",
       }) {
    SCOPED_TRACE(sql);
    auto statement = PrepareStatement(session, sql);
    const auto failed = statement.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  }
}

TEST(ReadSession, ExecutesScalarAndExistsQueriesThroughPrivateReadCalls) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(session,
                                             "SELECT (SELECT id FROM items ORDER BY id DESC),"
                                             "EXISTS(SELECT abs(-9223372036854775808) FROM items)");
  ASSERT_EQ(2U, statement.result_columns().size());
  EXPECT_EQ("INTEGER", statement.result_columns()[0].declared_type);
  EXPECT_EQ(std::nullopt, statement.result_columns()[1].declared_type);
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(4, IntegerValue(statement.row()[0]));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  RequireStatus(statement.Reset());
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(4, IntegerValue(statement.row()[0]));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
}

TEST(ReadSession, ExecutesSourceCorrelatedScalarAndExistsPerOuterRow) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT o.id,o.name,(SELECT o.name),"
                       "(SELECT i.id FROM model_rows i WHERE i.id=o.id),"
                       "EXISTS(SELECT 1 FROM model_rows i WHERE i.id=o.id AND i.id>?1) "
                       "FROM items o ORDER BY o.id");
  ASSERT_EQ(5U, statement.result_columns().size());
  EXPECT_EQ("TEXT", statement.result_columns()[2].declared_type);
  EXPECT_EQ("INTEGER", statement.result_columns()[3].declared_type);
  for (const std::int64_t threshold : {2, 3}) {
    RequireStatus(statement.Reset());
    RequireStatus(statement.Bind(1, SqlValue::Integer(threshold)));
    for (std::int64_t id = 1; id <= 4; ++id) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      ASSERT_EQ(5U, statement.row().size());
      EXPECT_EQ(id, IntegerValue(statement.row()[0]));
      EXPECT_EQ(statement.row()[1].type(), statement.row()[2].type());
      if (statement.row()[1].type() == SqlValueType::kText) {
        EXPECT_EQ(TextValue(statement.row()[1]), TextValue(statement.row()[2]));
      } else {
        EXPECT_EQ(SqlValueType::kNull, statement.row()[2].type());
      }
      EXPECT_EQ(id, IntegerValue(statement.row()[3]));
      EXPECT_EQ(id > threshold ? 1 : 0, IntegerValue(statement.row()[4]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
}

TEST(ReadSession, ExecutesOuterExpressionAliasesInNestedConsumers) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(session,
                                             "SELECT o.id+?1 AS shifted FROM items o "
                                             "WHERE (SELECT shifted)>?2 ORDER BY o.id");
  RequireStatus(statement.Bind(1, SqlValue::Integer(10)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(12)));
  for (const std::int64_t value : {13, 14}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    ASSERT_EQ(1U, statement.row().size());
    EXPECT_EQ(value, IntegerValue(statement.row()[0]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, RequiresOnlyExecutedOuterAliasComparisonsToResolveCollations) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  const auto live = session.Prepare(
      Utf8View{"SELECT EXISTS(SELECT (1 COLLATE missing)=2 AS x WHERE (SELECT x)=0)"});
  ASSERT_FALSE(live.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, live.error().code());

  ReadStatement ignored =
      PrepareStatement(session,
                       "SELECT EXISTS(SELECT (1 COLLATE missing)=2 AS x WHERE EXISTS(SELECT x)),"
                       "EXISTS(SELECT 1 COLLATE missing AS x WHERE (SELECT x)=1)");
  ASSERT_EQ(ReadStep::kRow, TakeValue(ignored.Step()));
  ASSERT_EQ(2U, ignored.row().size());
  EXPECT_EQ(1, IntegerValue(ignored.row()[0]));
  EXPECT_EQ(1, IntegerValue(ignored.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(ignored.Step()));

  ReadStatement inherited =
      PrepareStatement(session, "SELECT name AS x FROM items WHERE (SELECT x='ALPHA')");
  ASSERT_EQ(ReadStep::kRow, TakeValue(inherited.Step()));
  EXPECT_EQ("alpha", TextValue(inherited.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(inherited.Step()));
  ReadStatement scalar =
      PrepareStatement(session, "SELECT name AS x FROM items WHERE (SELECT x)='ALPHA'");
  EXPECT_EQ(ReadStep::kDone, TakeValue(scalar.Step()));
}

TEST(ReadSession, CapturesMergedAndQualifiedFullJoinValuesIncludingNullRows) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT a.id,b.id,score,(SELECT score),a.score,(SELECT a.score),"
                       "b.score,(SELECT b.score) FROM items a FULL JOIN items b USING(score) "
                       "ORDER BY coalesce(a.id,b.id),a.id");
  bool saw_left_only = false;
  bool saw_right_only = false;
  std::size_t rows = 0;
  while (TakeValue(statement.Step()) == ReadStep::kRow) {
    ++rows;
    saw_right_only = saw_right_only || statement.row()[0].type() == SqlValueType::kNull;
    saw_left_only = saw_left_only || statement.row()[1].type() == SqlValueType::kNull;
    for (const std::size_t field : {2U, 4U, 6U}) {
      const SqlValue& direct = statement.row()[field];
      const SqlValue& captured = statement.row()[field + 1U];
      EXPECT_EQ(direct.type(), captured.type());
      if (direct.type() != SqlValueType::kNull) {
        EXPECT_EQ(direct.real_value(), captured.real_value());
      }
    }
  }
  EXPECT_GT(rows, 4U);
  EXPECT_TRUE(saw_left_only);
  EXPECT_TRUE(saw_right_only);
}

TEST(ReadSession, IgnoresExistsDistinctProjectionWithoutIgnoringBinding) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT EXISTS(SELECT DISTINCT abs(-9223372036854775808) FROM items),"
                       "EXISTS(SELECT DISTINCT 1 COLLATE missing FROM items)");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  EXPECT_FALSE(session.Prepare(Utf8View{"SELECT EXISTS(SELECT missing FROM items)"}).has_value());
}

TEST(ReadSession, BindsButDoesNotExecuteIgnoredExistsOrdering) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT EXISTS(SELECT 1 FROM items ORDER BY abs(-9223372036854775808)),"
                       "EXISTS(SELECT 1 FROM items WHERE 0 ORDER BY sum(9223372036854775807)),"
                       "EXISTS(SELECT 1 ORDER BY 1 COLLATE missing)");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(0, IntegerValue(statement.row()[1]));
  EXPECT_EQ(1, IntegerValue(statement.row()[2]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  EXPECT_FALSE(session.Prepare(Utf8View{"SELECT EXISTS(SELECT 1 ORDER BY missing)"}).has_value());
}

TEST(ReadSession, OnlyNameBindsDiscardedNestedExistsExpressions) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT EXISTS(SELECT (SELECT 1,2)),"
                       "EXISTS(SELECT (SELECT 1 ORDER BY 1 COLLATE missing)),"
                       "EXISTS(SELECT (SELECT DISTINCT 1 COLLATE missing)),"
                       "EXISTS(SELECT 1 ORDER BY (SELECT 1,2)),"
                       "EXISTS(SELECT 1 FROM items ORDER BY sum((SELECT 1,2))),"
                       "EXISTS(SELECT (1 COLLATE missing)=2)");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(6U, statement.row().size());
  for (const SqlValue& value : statement.row()) {
    EXPECT_EQ(1, IntegerValue(value));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  for (const std::string_view sql : {
           "SELECT EXISTS(SELECT (SELECT missing))",
           "SELECT EXISTS(SELECT sum((SELECT 1,2)) FROM items)",
           "SELECT EXISTS(SELECT (SELECT 1,2) AS x FROM items GROUP BY x)",
       }) {
    SCOPED_TRACE(sql);
    const auto rejected = session.Prepare(Utf8View{sql});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, rejected.error().code());
  }
}

TEST(ReadSession, PreservesScalarEmptyGroupingSetLimitAndOffsetSemantics) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT (SELECT id FROM items WHERE 0),"
                       "(SELECT count() FROM items WHERE 0),"
                       "EXISTS(SELECT count() FROM items WHERE 0),"
                       "EXISTS(SELECT count() FROM items WHERE 0 GROUP BY name),"
                       "(SELECT id FROM model_rows ORDER BY id DESC LIMIT 2 OFFSET 1),"
                       "(SELECT 1 UNION SELECT 2 ORDER BY 1 DESC),"
                       "(SELECT 8 LIMIT 0),(VALUES((SELECT id FROM items)))");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(SqlValueType::kNull, statement.row()[0].type());
  EXPECT_EQ(0, IntegerValue(statement.row()[1]));
  EXPECT_EQ(1, IntegerValue(statement.row()[2]));
  EXPECT_EQ(0, IntegerValue(statement.row()[3]));
  EXPECT_EQ(31, IntegerValue(statement.row()[4]));
  EXPECT_EQ(2, IntegerValue(statement.row()[5]));
  EXPECT_EQ(SqlValueType::kNull, statement.row()[6].type());
  EXPECT_EQ(1, IntegerValue(statement.row()[7]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, KeepsUnreachedSubqueryCallsLazyAndResetsBoundValues) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement lazy =
      PrepareStatement(session, "SELECT coalesce(5,(SELECT abs(-9223372036854775808))) FROM items");
  for (std::size_t row = 0; row < 4U; ++row) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(lazy.Step()));
    EXPECT_EQ(5, IntegerValue(lazy.row()[0]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(lazy.Step()));
  ReadStatement empty = PrepareStatement(
      session, "SELECT id FROM items WHERE 0 AND (SELECT abs(-9223372036854775808))");
  EXPECT_EQ(ReadStep::kDone, TakeValue(empty.Step()));
  ReadStatement bound = PrepareStatement(session, "SELECT (SELECT ?1) FROM items");
  RequireStatus(bound.Bind(1, SqlValue::Integer(9)));
  ASSERT_EQ(ReadStep::kRow, TakeValue(bound.Step()));
  EXPECT_EQ(9, IntegerValue(bound.row()[0]));
  RequireStatus(bound.Reset());
  RequireStatus(bound.Bind(1, SqlValue::Integer(17)));
  for (std::size_t row = 0; row < 4U; ++row) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(bound.Step()));
    EXPECT_EQ(17, IntegerValue(bound.row()[0]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(bound.Step()));
}

TEST(ReadSession, KeepsParentAndChildAggregateCursorScopesDisjoint) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(session,
                                             "SELECT sum((SELECT id FROM items WHERE id=2)),"
                                             "(SELECT sum(id) FROM items),count() FROM model_rows");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(64, IntegerValue(statement.row()[0]));
  EXPECT_EQ(10, IntegerValue(statement.row()[1]));
  EXPECT_EQ(32, IntegerValue(statement.row()[2]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  ReadStatement grouped =
      PrepareStatement(session, "SELECT (SELECT count() FROM items) FROM model_rows GROUP BY id%3");
  for (std::size_t group = 0; group < 3U; ++group) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(grouped.Step()));
    EXPECT_EQ(4, IntegerValue(grouped.row()[0]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(grouped.Step()));
}

TEST(ReadSession, UsesThePinnedScalarCompoundMetadataExpression) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session, "SELECT (SELECT id FROM items UNION ALL SELECT abs(-9223372036854775808))");
  ASSERT_EQ(1U, statement.result_columns().size());
  EXPECT_EQ(std::nullopt, statement.result_columns()[0].declared_type);
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, DoesNotRequireUnusedScalarUnionComparisonCollations) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(session,
                                             "SELECT (SELECT 1 COLLATE missing UNION SELECT 2),"
                                             "EXISTS(SELECT 1 COLLATE missing UNION SELECT 2)");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, ExecutesTheAcceptedNestedJoinExpressionDepthBoundary) {
  constexpr std::size_t kAcceptedOnTerms = 998;
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  std::string sql = "SELECT (SELECT 1 FROM items a JOIN items b ON 1";
  for (std::size_t term = 1; term < kAcceptedOnTerms; ++term) {
    sql.append(" AND 1");
  }
  sql.push_back(')');
  ReadStatement statement = PrepareStatement(session, sql);
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  sql.insert(sql.size() - 1U, " AND 1");
  const auto rejected = session.Prepare(Utf8View{sql});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, rejected.error().code());
}

TEST(ReadSession, MatchesIndependentScalarAndExistsRangeModelsAcrossRebindings) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  for (const bool reverse : {false, true}) {
    ReadStatement statement = PrepareStatement(
        session, std::string{"SELECT (SELECT id FROM model_rows WHERE id>=?1 ORDER BY id "} +
                     (reverse ? "DESC" : "ASC") +
                     " LIMIT ?2 OFFSET ?3),"
                     "EXISTS(SELECT label FROM model_rows WHERE id>=?1 LIMIT ?2 OFFSET ?3)");
    for (const std::int64_t threshold : {0, 1, 2, 31, 32, 33}) {
      for (const std::int64_t limit : {0, 1, 5, -1}) {
        for (const std::int64_t offset : {-3, 0, 1, 32}) {
          SCOPED_TRACE(::testing::Message{} << reverse << '/' << threshold << '/' << limit << '/'
                                            << offset);
          RequireStatus(statement.Reset());
          RequireStatus(statement.Bind(1, SqlValue::Integer(threshold)));
          RequireStatus(statement.Bind(2, SqlValue::Integer(limit)));
          RequireStatus(statement.Bind(3, SqlValue::Integer(offset)));
          const std::int64_t first = threshold > 1 ? threshold : 1;
          const std::int64_t count = first <= 32 ? 33 - first : 0;
          const std::int64_t skipped = offset > 0 ? offset : 0;
          const bool produces = limit != 0 && skipped < count;
          ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
          if (produces) {
            EXPECT_EQ(reverse ? 32 - skipped : first + skipped, IntegerValue(statement.row()[0]));
          } else {
            EXPECT_EQ(SqlValueType::kNull, statement.row()[0].type());
          }
          EXPECT_EQ(produces ? 1 : 0, IntegerValue(statement.row()[1]));
          EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
        }
      }
    }
  }
}

TEST(ReadSession, PreservesFullMergedAffinityCollationAndPhysicalSubgroups) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement affinity =
      PrepareStatement(session,
                       "SELECT id='5',coalesce(a.id,b.id)='5',typeof(id) FROM items a "
                       "FULL JOIN model_rows b USING(id) WHERE id=5");
  ASSERT_EQ(ReadStep::kRow, TakeValue(affinity.Step()));
  EXPECT_EQ(1, IntegerValue(affinity.row()[0]));
  EXPECT_EQ(0, IntegerValue(affinity.row()[1]));
  EXPECT_EQ("integer", TextValue(affinity.row()[2]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(affinity.Step()));
  for (const bool reverse : {false, true}) {
    ReadStatement collation = PrepareStatement(
        session, reverse ? "SELECT count() FROM sqlite_schema a FULL JOIN items b USING(name) "
                           "WHERE name='ALPHA'"
                         : "SELECT count() FROM items a FULL JOIN sqlite_schema b USING(name) "
                           "WHERE name='ALPHA'");
    ASSERT_EQ(ReadStep::kRow, TakeValue(collation.Step()));
    EXPECT_EQ(reverse ? 0 : 1, IntegerValue(collation.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(collation.Step()));
  }
  ReadStatement subgroup =
      PrepareStatement(session,
                       "SELECT q.id,c.id FROM items q FULL JOIN items q USING(id) "
                       "FULL JOIN model_rows c USING(id) WHERE id=5");
  EXPECT_EQ("q.id", subgroup.result_columns()[0].name);
  EXPECT_EQ(std::nullopt, subgroup.result_columns()[0].declared_type);
  ASSERT_EQ(ReadStep::kRow, TakeValue(subgroup.Step()));
  EXPECT_EQ(SqlValueType::kNull, subgroup.row()[0].type());
  EXPECT_EQ(5, IntegerValue(subgroup.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(subgroup.Step()));
}

TEST(ReadSession, RebindsFullMatchesAndSuppressesDefaultsOnBothMissingSides) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT a.id,b.id,a.text_default,b.text_default,a._rowid_,b._rowid_ "
                       "FROM altered a FULL JOIN altered b ON b.id=a.id+abs(?1) "
                       "ORDER BY b.id IS NULL,b.id LIMIT ?2");
  RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(-1)));
  for (const auto& expected : std::array{std::array{0, 1}, std::array{1, 2}, std::array{2, 3},
                                         std::array{3, 4}, std::array{4, 0}}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    for (std::size_t side = 0; side < expected.size(); ++side) {
      const int id = expected[side];
      if (id == 0) {
        EXPECT_EQ(SqlValueType::kNull, statement.row()[side].type());
        EXPECT_EQ(SqlValueType::kNull, statement.row()[side + 4U].type());
      } else {
        EXPECT_EQ(id, IntegerValue(statement.row()[side]));
        EXPECT_EQ(id, IntegerValue(statement.row()[side + 4U]));
      }
      if (id == 1 || id == 2) {
        EXPECT_EQ("fallback", TextValue(statement.row()[side + 2U]));
      } else if (id == 3) {
        EXPECT_EQ("stored", TextValue(statement.row()[side + 2U]));
      } else {
        EXPECT_EQ(SqlValueType::kNull, statement.row()[side + 2U].type());
      }
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(1)));
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(std::numeric_limits<std::int64_t>::min())));
  RequireStatus(statement.Bind(2, SqlValue::Integer(0)));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(-1)));
  for (std::int64_t id = 1; id <= 4; ++id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(id, IntegerValue(statement.row()[0]));
    EXPECT_EQ(id, IntegerValue(statement.row()[1]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, RebindsExactJoinedRowidKeysWithoutRestrictingFullReplay) {
  struct Case {
    SqlValue key;
    bool matches = false;
  };
  const std::array cases{
      Case{.key = SqlValue::Integer(2), .matches = true},
      Case{.key = SqlValue::Real(2.0), .matches = true},
      Case{.key = SqlValue::Text("02"), .matches = true},
      Case{.key = SqlValue::Text("2.0"), .matches = true},
      Case{.key = SqlValue::Text("2e0"), .matches = true},
      Case{.key = SqlValue::Real(2.5)},
      Case{.key = SqlValue::Text("2x")},
      Case{.key = SqlValue{}},
      Case{.key = SqlValue::Integer(std::numeric_limits<std::int64_t>::min())},
      Case{.key = SqlValue::Integer(std::numeric_limits<std::int64_t>::max())},
      Case{.key = SqlValue::Integer(1), .matches = true},
  };
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session, "SELECT count(),count(a.id),count(b.id) FROM items a FULL JOIN items b ON b.id=?1");
  for (const Case& test_case : cases) {
    RequireStatus(statement.Bind(1, test_case.key));
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(test_case.matches ? 7 : 8, IntegerValue(statement.row()[0]));
    EXPECT_EQ(4, IntegerValue(statement.row()[1]));
    EXPECT_EQ(test_case.matches ? 7 : 4, IntegerValue(statement.row()[2]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Reset());
  }
}

TEST(ReadSession, RebindsComputedJoinedRowidKeysAcrossPreservedSources) {
  constexpr std::array<std::byte, 1> kNumericBlob{std::byte{0x31}};
  struct Case {
    SqlValue offset;
    std::int64_t matches = 0;
  };
  const std::array cases{
      Case{.offset = SqlValue::Integer(0), .matches = 4},
      Case{.offset = SqlValue::Integer(1), .matches = 3},
      Case{.offset = SqlValue::Integer(-1), .matches = 3},
      Case{.offset = SqlValue::Real(1.0), .matches = 3},
      Case{.offset = SqlValue::Real(0.5)},
      Case{.offset = SqlValue{}},
      Case{.offset = SqlValue::Text("1x"), .matches = 3},
      Case{.offset = SqlValue::Text("bad"), .matches = 4},
      Case{.offset = SqlValue::Blob(ByteBuffer::CopyOf(kNumericBlob)), .matches = 3},
      Case{.offset = SqlValue::Integer(std::numeric_limits<std::int64_t>::max())},
      Case{.offset = SqlValue::Integer(std::numeric_limits<std::int64_t>::min())},
      Case{.offset = SqlValue::Real(std::numeric_limits<double>::infinity())},
  };
  for (const bool file_backed : {false, true}) {
    const ReadSessionOptions options{
        .temporary_storage =
            TemporaryStorageOptions{
                .mode = file_backed ? TemporaryStoreMode::kFile : TemporaryStoreMode::kMemory,
                .sorter_memory_threshold = ByteCount{32U * 1024U},
            },
    };
    for (const std::string_view join : {"JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL JOIN"}) {
      SCOPED_TRACE(::testing::Message() << file_backed << " " << join);
      auto session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string(), options));
      auto statement =
          PrepareStatement(session, "SELECT count(),count(a.id),count(b.id) FROM items a " +
                                        std::string{join} + " items b ON b.id=a.id+?1");
      for (const auto& test_case : cases) {
        RequireStatus(statement.Bind(1, test_case.offset));
        ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
        const auto matches = test_case.matches;
        const auto rows = join == "JOIN" ? matches : join == "FULL JOIN" ? 8 - matches : 4;
        EXPECT_EQ(rows, IntegerValue(statement.row()[0]));
        EXPECT_EQ(join == "JOIN" || join == "RIGHT JOIN" ? matches : 4,
                  IntegerValue(statement.row()[1]));
        EXPECT_EQ(join == "JOIN" || join == "LEFT JOIN" ? matches : 4,
                  IntegerValue(statement.row()[2]));
        EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
        RequireStatus(statement.Reset());
      }
    }
  }
}

TEST(ReadSession, ResetsRightReplayAndKeepsNullPrefixDefaultsOutOfOutput) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT id,a.id,a.text_default,b.id FROM altered a "
                       "RIGHT JOIN model_rows b USING(id) WHERE id<=?1 ORDER BY id LIMIT ?2");
  RequireStatus(statement.Bind(1, SqlValue::Integer(5)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(-1)));
  for (std::int64_t id = 1; id <= 5; ++id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(id, IntegerValue(statement.row()[0]));
    EXPECT_EQ(id, IntegerValue(statement.row()[3]));
    if (id <= 2) {
      EXPECT_EQ("fallback", TextValue(statement.row()[2]));
    } else if (id == 3) {
      EXPECT_EQ("stored", TextValue(statement.row()[2]));
    } else {
      EXPECT_EQ(SqlValueType::kNull, statement.row()[2].type());
    }
    if (id == 5) {
      EXPECT_EQ(SqlValueType::kNull, statement.row()[1].type());
    } else {
      EXPECT_EQ(id, IntegerValue(statement.row()[1]));
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(32)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(1)));
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(2, SqlValue::Integer(0)));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

void ExercisePreservedRightMatchFailure(std::string_view sql, std::int64_t expected) {
  const auto state = std::make_shared<TemporaryFileState>();
  const ReadSessionOptions options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kFile,
              .sorter_memory_threshold = ByteCount{32U << 10U},
          },
  };
  ReadSession session = TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                                    IndexFixturePath().string(), options));
  ReadStatement statement = PrepareStatement(session, sql);
  state->writes_until_failure = 0U;
  const auto failed = statement.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_GT(state->open_count, 0U);
  EXPECT_EQ(0U, state->live_count);
  const auto reset = statement.Reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(ErrorCode::kIo, reset.error().code());
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(expected, IntegerValue(statement.row()[0]));
  EXPECT_EQ(expected, IntegerValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  EXPECT_EQ(0U, state->live_count);
  RequireStatus(statement.Finalize());
}

TEST(ReadSession, CleansAndRecoversRightMatchRelationsAfterTemporaryWriteFailure) {
  ExercisePreservedRightMatchFailure(
      "SELECT count(),count(a.rowid) FROM sqlite_schema a RIGHT JOIN items b ON a.name='items'",
      4096);
}

TEST(ReadSession, CleansAndRecoversFullMatchRelationsAfterTemporaryWriteFailure) {
  ExercisePreservedRightMatchFailure(
      "SELECT count(),count(a.rowid) FROM sqlite_schema a FULL JOIN items b ON a.name='items'",
      4100);
}

TEST(ReadSession, CleansAndRecoversParameterizedMatchRelationsAfterTemporaryWriteFailure) {
  ExercisePreservedRightMatchFailure(
      "SELECT count(),count(a.rowid) FROM items a FULL JOIN items b ON b.id=a.id", 4096);
}

TEST(ReadSession, CleansAndRecoversIndexedMatchRelationsAfterTemporaryWriteFailure) {
  ExercisePreservedRightMatchFailure(
      "SELECT count(),count(a.rowid) FROM items a FULL JOIN items b ON b.category=a.category",
      4096);
}

TEST(ReadSession, CleansAndRecoversNoncoveringIndexedMatchRelationsAfterTemporaryWriteFailure) {
  ExercisePreservedRightMatchFailure(
      "SELECT count(b.payload),count(a.rowid) FROM items a FULL JOIN items b "
      "ON b.category=a.category",
      4096);
}

TEST(ReadSession, CleansAndRecoversRangeMatchRelationsAfterTemporaryWriteFailure) {
  ExercisePreservedRightMatchFailure(
      "SELECT count(),count(a.rowid) FROM items a FULL JOIN items b "
      "ON b.category>=a.category AND b.category<=a.category",
      4096);
}

TEST(ReadSession, CleansAndRecoversNoncoveringRangeMatchRelationsAfterTemporaryWriteFailure) {
  ExercisePreservedRightMatchFailure(
      "SELECT count(b.payload),count(a.rowid) FROM items a FULL JOIN items b "
      "ON b.category>=a.category AND b.category<=a.category",
      4096);
}

TEST(ReadSession, PreservesBothFullSidesAcrossExternalOrderAndPmaFailureRecovery) {
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<int>(mode));
    const auto state = std::make_shared<TemporaryFileState>();
    const ReadSessionOptions options{
        .temporary_storage =
            TemporaryStorageOptions{
                .mode = mode,
                .sorter_memory_threshold =
                    ByteCount{mode == TemporaryStoreMode::kFile ? 32U << 10U : 1U << 20U},
            },
    };
    ReadSession session =
        TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                    ReadCompatibilityFixturePath().string(), options));
    ReadStatement statement =
        PrepareStatement(session,
                         "SELECT coalesce(a.id,b.id) FROM model_rows a FULL JOIN model_rows b ON 0 "
                         "CROSS JOIN model_rows c ORDER BY 1 DESC,a.id,b.id,c.id");
    if (mode == TemporaryStoreMode::kFile) {
      state->writes_until_failure = 0U;
      const auto failed = statement.Step();
      ASSERT_FALSE(failed.has_value());
      EXPECT_EQ(ErrorCode::kIo, failed.error().code());
      EXPECT_GT(state->open_count, 0U);
      EXPECT_EQ(0U, state->live_count);
      const auto reset = statement.Reset();
      ASSERT_FALSE(reset.has_value());
      EXPECT_EQ(ErrorCode::kIo, reset.error().code());
    }
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
    instrumentation::ProbeCounterCollection work{
        instrumentation::ProbeCollectionKind::AggregateWork};
    const instrumentation::ScopedProbeCounterCollection scope{work};
#endif
    for (std::size_t row = 0; row < 2048U; ++row) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(static_cast<std::int64_t>(32U - row / 64U), IntegerValue(statement.row()[0]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    EXPECT_EQ(0U, state->live_count);
    EXPECT_LE(state->peak_live_count, 3U);
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
    EXPECT_EQ(mode == TemporaryStoreMode::kFile,
              work.Value(instrumentation::AggregateWorkMetric::SorterSpillWrites) > 0U);
#endif
    RequireStatus(statement.Reset());
    EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    RequireStatus(statement.Finalize());
    EXPECT_EQ(0U, state->live_count);
  }
}

TEST(ReadSession, PreservesRightMatchedSetsAcrossMemoryAndGenuineExternalOrderSpill) {
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<int>(mode));
    const auto state = std::make_shared<TemporaryFileState>();
    const ReadSessionOptions options{
        .temporary_storage =
            TemporaryStorageOptions{
                .mode = mode,
                .sorter_memory_threshold =
                    ByteCount{mode == TemporaryStoreMode::kFile ? 64U << 10U : 1U << 20U},
            },
    };
    ReadSession session =
        TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                    ReadCompatibilityFixturePath().string(), options));
    ReadStatement statement =
        PrepareStatement(session,
                         "SELECT b.id FROM model_rows a RIGHT JOIN model_rows b ON 1 "
                         "CROSS JOIN model_rows c WHERE a.id<=8 ORDER BY b.id DESC,a.id,c.id");
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
    instrumentation::ProbeCounterCollection work{
        instrumentation::ProbeCollectionKind::AggregateWork};
    const instrumentation::ScopedProbeCounterCollection scope{work};
#endif
    for (std::size_t row = 0; row < 8192U; ++row) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(static_cast<std::int64_t>(32U - row / 256U), IntegerValue(statement.row()[0]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, state->open_count > 0U);
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
    EXPECT_EQ(mode == TemporaryStoreMode::kFile,
              work.Value(instrumentation::AggregateWorkMetric::SorterSpillWrites) > 0U);
#endif
    EXPECT_LE(state->peak_live_count, 3U);
    EXPECT_EQ(0U, state->live_count);
    RequireStatus(statement.Reset());
    EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    RequireStatus(statement.Finalize());
    EXPECT_EQ(0U, state->live_count);
  }
}

TEST(ReadSession, RebindsMergedGroupingAcrossEarlyAndZeroLimits) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT id,count(b.id),min(b.text_default) "
                       "FROM model_rows a LEFT JOIN altered b USING(id) WHERE id<=?1 "
                       "GROUP BY id ORDER BY id LIMIT ?2");
  RequireStatus(statement.Bind(1, SqlValue::Integer(5)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(-1)));
  for (std::int64_t id = 1; id <= 5; ++id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(id, IntegerValue(statement.row()[0]));
    EXPECT_EQ(id <= 4 ? 1 : 0, IntegerValue(statement.row()[1]));
    if (id <= 2) {
      EXPECT_EQ("fallback", TextValue(statement.row()[2]));
    } else if (id == 3) {
      EXPECT_EQ("stored", TextValue(statement.row()[2]));
    } else {
      EXPECT_EQ(SqlValueType::kNull, statement.row()[2].type());
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(32)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(1)));
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Text("not reached")));
  RequireStatus(statement.Bind(2, SqlValue::Integer(0)));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, ExecutesOwnedCommaAndCrossJoinRowsWithResetAndLimit) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  const std::array queries{
      "SELECT a.id,b.id FROM Items a,Items b "
      "WHERE a.id<=2 AND b.id<=2 ORDER BY a.id,b.id LIMIT ?1 OFFSET ?2",
      "SELECT a.id,b.id FROM Items a CROSS JOIN Items b "
      "WHERE a.id<=2 AND b.id<=2 ORDER BY a.id,b.id LIMIT ?1 OFFSET ?2",
  };
  for (const std::string_view sql : queries) {
    SCOPED_TRACE(sql);
    ReadStatement statement = PrepareStatement(session, sql);
    RequireStatus(statement.Bind(1, SqlValue::Integer(2)));
    RequireStatus(statement.Bind(2, SqlValue::Integer(1)));
    for (const auto& expected : std::array{std::array{1, 2}, std::array{2, 1}}) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      ASSERT_EQ(2U, statement.row().size());
      EXPECT_EQ(expected[0], IntegerValue(statement.row()[0]));
      EXPECT_EQ(expected[1], IntegerValue(statement.row()[1]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Reset());
    RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
    RequireStatus(statement.Bind(2, SqlValue::Text("not evaluated")));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
}

TEST(ReadSession, RebindsLeftMatchesAndSuppressesDefaultsOnlyForNullExtension) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT a.id,b.id,b.text_default,b.real_default,b.integer_default,b._rowid_ "
                       "FROM Items a LEFT JOIN altered b ON b.id=a.id+abs(?1) "
                       "ORDER BY a.id LIMIT ?2");
  RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(-1)));
  for (std::int64_t id = 1; id <= 4; ++id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(id, IntegerValue(statement.row()[0]));
    if (id == 1) {
      EXPECT_EQ(2, IntegerValue(statement.row()[1]));
      EXPECT_EQ("fallback", TextValue(statement.row()[2]));
      EXPECT_EQ(1.25, statement.row()[3].real_value());
      EXPECT_EQ(7, IntegerValue(statement.row()[4]));
      EXPECT_EQ(2, IntegerValue(statement.row()[5]));
    } else if (id == 2) {
      EXPECT_EQ(3, IntegerValue(statement.row()[1]));
      EXPECT_EQ("stored", TextValue(statement.row()[2]));
    } else {
      for (std::size_t column = id == 3 ? 2U : 1U; column < 5U; ++column) {
        EXPECT_EQ(SqlValueType::kNull, statement.row()[column].type());
      }
      if (id == 3) {
        EXPECT_EQ(4, IntegerValue(statement.row()[1]));
        EXPECT_EQ(4, IntegerValue(statement.row()[5]));
      } else {
        EXPECT_EQ(SqlValueType::kNull, statement.row()[5].type());
      }
    }
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(1)));
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
  EXPECT_EQ("fallback", TextValue(statement.row()[2]));
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(std::numeric_limits<std::int64_t>::min())));
  RequireStatus(statement.Bind(2, SqlValue::Integer(0)));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, RecoversJoinedCursorReadFailureAndCleansOwnedState) {
  struct Case {
    std::string_view sql;
    std::int64_t first;
    std::optional<std::int64_t> second{};
  };
  const std::array cases{
      Case{.sql = "SELECT a.id,b.id FROM Items a CROSS JOIN storage_values b LIMIT 1",
           .first = 1,
           .second = 1},
      Case{.sql = "SELECT count() FROM Items a CROSS JOIN storage_values b", .first = 44},
      Case{.sql = "SELECT b.id,count() FROM Items a CROSS JOIN storage_values b "
                  "GROUP BY b.id ORDER BY b.id LIMIT 1",
           .first = 1,
           .second = 4},
      Case{.sql = "SELECT a.id,b.id FROM Items a LEFT JOIN storage_values b ON b.id=a.id LIMIT 1",
           .first = 1,
           .second = 1},
      Case{.sql = "SELECT count(b.id) FROM Items a LEFT JOIN storage_values b ON b.id=a.id+99",
           .first = 0},
      Case{.sql = "SELECT a.id,b.id FROM Items a LEFT JOIN storage_values b USING(id) LIMIT 1",
           .first = 1,
           .second = 1},
      Case{.sql = "SELECT count(b.id) FROM Items a LEFT JOIN storage_values b USING(id)",
           .first = 4},
      Case{.sql = "SELECT a.id,b.id FROM Items a RIGHT JOIN storage_values b ON a.id=b.id LIMIT 1",
           .first = 1,
           .second = 1},
      Case{.sql = "SELECT count() FROM Items a RIGHT JOIN storage_values b ON a.id=b.id",
           .first = 11},
      Case{.sql = "SELECT a.id,b.id FROM Items a FULL JOIN storage_values b ON a.id=b.id LIMIT 1",
           .first = 1,
           .second = 1},
      Case{.sql = "SELECT count() FROM Items a FULL JOIN storage_values b ON 0", .first = 15},
      Case{.sql = "SELECT count() FROM Items a FULL JOIN storage_values b USING(id)", .first = 11},
      Case{.sql = "SELECT a.*,a.id,b.id FROM Items a FULL JOIN storage_values b USING(id) "
                  "WHERE b.id=11 LIMIT 1",
           .first = 11},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    const auto state = std::make_shared<TemporaryFileState>();
    auto vfs = std::make_unique<TemporaryTrackingVfs>(state);
    ReadSession session =
        TakeValue(ReadSession::Open(std::move(vfs), ReadCompatibilityFixturePath().string()));
    ReadStatement statement = PrepareStatement(session, test_case.sql);
    state->main_read_failure_offset = FileOffset{1024};
    const auto failed = statement.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_EQ(1U, state->main_read_failures);
    EXPECT_EQ(0U, state->live_count);
    const auto reset = statement.Reset();
    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(ErrorCode::kIo, reset.error().code());
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(test_case.first, IntegerValue(statement.row()[0]));
    if (test_case.second.has_value()) {
      EXPECT_EQ(*test_case.second, IntegerValue(statement.row()[1]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Finalize());
  }
}

TEST(ReadSession, ExecutesJoinDestinationsInMemoryAndFileModes) {
  struct JoinCase {
    std::string_view sql;
    std::size_t rows;
    bool descending;
    std::size_t copies = 1;
    bool exercise_write_failure = false;
  };
  const std::string wildcard_sql =
      "SELECT a.*,b.id,'" + std::string(2048, 'x') +
      "' FROM items a RIGHT JOIN model_rows b USING(id) ORDER BY 1 DESC,b.id";
  const std::array cases{
      JoinCase{.sql = "SELECT b.id FROM model_rows a CROSS JOIN model_rows b "
                      "ORDER BY b.id DESC,a.id",
               .rows = 1024,
               .descending = true,
               .copies = 32},
      JoinCase{.sql = "SELECT DISTINCT b.id FROM model_rows a CROSS JOIN model_rows b "
                      "WHERE a.id<=2 ORDER BY 1 DESC",
               .rows = 32,
               .descending = true},
      JoinCase{.sql = "SELECT b.id FROM model_rows a CROSS JOIN model_rows b WHERE a.id=1 "
                      "UNION SELECT 99 ORDER BY 1",
               .rows = 33,
               .descending = false},
      JoinCase{.sql = "SELECT b.id FROM model_rows a CROSS JOIN model_rows b "
                      "GROUP BY b.id HAVING count()>1 ORDER BY b.id DESC",
               .rows = 32,
               .descending = true},
      JoinCase{.sql = "SELECT a.id FROM model_rows a LEFT JOIN items b USING(id) "
                      "CROSS JOIN model_rows c ORDER BY a.id+0 DESC,c.id",
               .rows = 1024,
               .descending = true,
               .copies = 32},
      JoinCase{.sql = "SELECT id FROM model_rows a NATURAL LEFT JOIN model_rows b ORDER BY id DESC",
               .rows = 32,
               .descending = true},
      JoinCase{.sql = wildcard_sql, .rows = 32, .descending = true, .exercise_write_failure = true},
  };
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<int>(mode));
    const auto state = std::make_shared<TemporaryFileState>();
    auto vfs = std::make_unique<TemporaryTrackingVfs>(state);
    const ReadSessionOptions options{
        .temporary_storage =
            TemporaryStorageOptions{
                .mode = mode,
                .sorter_memory_threshold =
                    ByteCount{mode == TemporaryStoreMode::kFile ? 32U << 10U : 1U << 20U},
            },
    };
    ReadSession session = TakeValue(
        ReadSession::Open(std::move(vfs), ReadCompatibilityFixturePath().string(), options));
    for (const JoinCase& test_case : cases) {
      SCOPED_TRACE(test_case.sql);
      const std::size_t opens_before = state->open_count;
      ReadStatement statement = PrepareStatement(session, test_case.sql);
      if (mode == TemporaryStoreMode::kFile && test_case.exercise_write_failure) {
        state->writes_until_failure = 0U;
        const auto failed = statement.Step();
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(ErrorCode::kIo, failed.error().code());
        EXPECT_GT(state->open_count, opens_before);
        EXPECT_EQ(0U, state->live_count);
        const auto reset = statement.Reset();
        ASSERT_FALSE(reset.has_value());
        EXPECT_EQ(ErrorCode::kIo, reset.error().code());
      }
      std::size_t rows = 0;
      while (true) {
        const auto stepped = statement.Step();
        ASSERT_TRUE(stepped.has_value()) << stepped.error().ToString();
        if (*stepped == ReadStep::kDone) {
          break;
        }
        ASSERT_TRUE(statement.row()[0].integer_value().has_value());
        const auto expected =
            static_cast<std::int64_t>(test_case.descending ? 32U - rows / test_case.copies
                                      : rows == 32U        ? 99U
                                                           : rows + 1U);
        EXPECT_EQ(expected, IntegerValue(statement.row()[0]));
        ++rows;
      }
      EXPECT_EQ(test_case.rows, rows);
      if (mode == TemporaryStoreMode::kFile && test_case.copies > 1U) {
        EXPECT_GT(state->open_count, opens_before);
      }
      EXPECT_EQ(0U, state->live_count);
      EXPECT_LE(state->peak_live_count, 2U);
      RequireStatus(statement.Reset());
      EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      RequireStatus(statement.Finalize());
      EXPECT_EQ(0U, state->live_count);
    }
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, state->open_count > 0U);
  }
}

TEST(ReadSession, StreamsJoinedCompoundsWithinTheExistingWorkingSetWithoutLeakingFiles) {
  constexpr std::size_t kSorterWorkingSetBytes = 1024;
  const auto state = std::make_shared<TemporaryFileState>();
  auto vfs = std::make_unique<TemporaryTrackingVfs>(state);
  const ReadSessionOptions options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kFile,
              .sorter_memory_threshold = ByteCount{kSorterWorkingSetBytes},
          },
  };
  ReadSession session = TakeValue(
      ReadSession::Open(std::move(vfs), ReadCompatibilityFixturePath().string(), options));
  ReadStatement streaming =
      PrepareStatement(session,
                       "SELECT b.id FROM model_rows a CROSS JOIN model_rows b WHERE a.id=1 "
                       "UNION SELECT 99 ORDER BY 1");
  for (std::int64_t expected = 1; expected <= 32; ++expected) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(streaming.Step()));
    EXPECT_EQ(expected, IntegerValue(streaming.row()[0]));
  }
  ASSERT_EQ(ReadStep::kRow, TakeValue(streaming.Step()));
  EXPECT_EQ(99, IntegerValue(streaming.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(streaming.Step()));
  EXPECT_EQ(0U, state->open_count);
  RequireStatus(streaming.Reset());
  EXPECT_EQ(ReadStep::kRow, TakeValue(streaming.Step()));
  EXPECT_EQ(1, IntegerValue(streaming.row()[0]));
  RequireStatus(streaming.Finalize());
  EXPECT_EQ(0U, state->live_count);
}

TEST(ReadSession, GroupsPhysicalAndNullExtendedRowsThroughSharedSpillStorage) {
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<int>(mode));
    const auto state = std::make_shared<TemporaryFileState>();
    const ReadSessionOptions options{
        .temporary_storage =
            TemporaryStorageOptions{
                .mode = mode,
                .sorter_memory_threshold =
                    ByteCount{mode == TemporaryStoreMode::kFile ? 32U << 10U : 1U << 20U},
            },
    };
    ReadSession session =
        TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                    ReadCompatibilityFixturePath().string(), options));
    ReadStatement statement =
        PrepareStatement(session,
                         "SELECT a.id,count(),count(b.id),group_concat(b.id ORDER BY b.id DESC) "
                         "FROM model_rows a LEFT JOIN model_rows b ON a.id<32 "
                         "GROUP BY a.id ORDER BY a.id");
    std::string expected_ids;
    for (std::int64_t id = 32; id > 0; --id) {
      if (!expected_ids.empty()) {
        expected_ids += ',';
      }
      expected_ids += std::to_string(id);
    }
    for (std::int64_t id = 1; id <= 32; ++id) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(id, IntegerValue(statement.row()[0]));
      EXPECT_EQ(id == 32 ? 1 : 32, IntegerValue(statement.row()[1]));
      EXPECT_EQ(id == 32 ? 0 : 32, IntegerValue(statement.row()[2]));
      if (id == 32) {
        EXPECT_EQ(SqlValueType::kNull, statement.row()[3].type());
      } else {
        EXPECT_EQ(expected_ids, TextValue(statement.row()[3]));
      }
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, state->open_count > 0U);
    EXPECT_LE(state->peak_live_count, 2U);
    EXPECT_EQ(0U, state->live_count);
    RequireStatus(statement.Reset());
    EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    RequireStatus(statement.Finalize());
    EXPECT_EQ(0U, state->live_count);
  }
}

TEST(ReadSession, BypassesAllSixtyFourSourcesForZeroLimit) {
  struct Case {
    std::string_view operation;
    std::string_view condition;
  };
  const std::array cases{
      Case{.operation = " CROSS JOIN Items AS t", .condition = ""},
      Case{.operation = " LEFT JOIN Items AS t", .condition = " ON 0"},
      Case{.operation = " LEFT JOIN Items AS t", .condition = " USING(id)"},
      Case{.operation = " NATURAL LEFT JOIN Items AS t", .condition = ""},
      Case{.operation = " RIGHT JOIN Items AS t", .condition = " ON 0"},
      Case{.operation = " RIGHT JOIN Items AS t", .condition = " USING(id)"},
      Case{.operation = " NATURAL RIGHT JOIN Items AS t", .condition = ""},
      Case{.operation = " FULL JOIN Items AS t", .condition = " ON 0"},
      Case{.operation = " FULL JOIN Items AS t", .condition = " USING(id)"},
      Case{.operation = " NATURAL FULL JOIN Items AS t", .condition = ""},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.operation);
    SCOPED_TRACE(test_case.condition);
    ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
    std::string sql = "SELECT 1 FROM Items AS t0";
    for (std::size_t index = 1; index < kMaximumSelectSources; ++index) {
      sql += std::string{test_case.operation} + std::to_string(index) +
             std::string{test_case.condition};
    }
    sql += " LIMIT 0";
    ReadStatement statement = PrepareStatement(session, sql);
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
}

TEST(ReadSession, StopsInputOrderedRowsBeforeLaterPredicateError) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT id FROM items "
                       "WHERE id<=4 AND iif(id=4,abs(-9223372036854775808),1) ORDER BY id LIMIT 2");
  for (const std::int64_t expected : {1, 2}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(expected, IntegerValue(statement.row()[0]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, StreamsNonDefaultNullOrderBeforeSkippedPartitionErrors) {
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT id FROM key_rows "
                       "WHERE iif(numeric_key IS NULL,abs(-9223372036854775808),1) "
                       "ORDER BY numeric_key NULLS LAST LIMIT 1");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(3, IntegerValue(statement.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, ResetsBothNullPartitionsWithoutOpeningTemporaryFiles) {
  struct Case {
    std::string_view sql;
    std::vector<std::int64_t> ids;
  };
  const std::array cases{
      Case{"SELECT id FROM key_rows ORDER BY numeric_key NULLS LAST",
           {3, 2, 1, 4, 5, 7, 9, 11, 10, 8, 6, 12}},
      Case{"SELECT id,payload FROM key_rows ORDER BY numeric_key DESC NULLS FIRST",
           {12, 6, 8, 10, 11, 9, 7, 5, 4, 1, 2, 3}},
      Case{"SELECT id FROM key_rows WHERE default_text='fallback' "
           "ORDER BY numeric_key NULLS LAST LIMIT 4 OFFSET 8",
           {11, 6, 8, 12}},
      Case{"SELECT id FROM key_rows WHERE text_key='alpha' "
           "ORDER BY numeric_key DESC NULLS FIRST",
           {5, 4}},
      Case{"SELECT id FROM key_rows WHERE text_key='missing' ORDER BY numeric_key NULLS LAST", {}},
  };
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    const auto state = std::make_shared<TemporaryFileState>();
    const ReadSessionOptions options{
        .temporary_storage = TemporaryStorageOptions{.mode = mode},
    };
    ReadSession session = TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                                      JoinedIndexFixturePath().string(), options));
    for (const Case& test : cases) {
      SCOPED_TRACE(test.sql);
      ReadStatement statement = PrepareStatement(session, test.sql);
      if (!test.ids.empty()) {
        ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
        EXPECT_EQ(test.ids.front(), IntegerValue(statement.row()[0]));
        RequireStatus(statement.Reset());
      }
      for (int execution = 0; execution < 2; ++execution) {
        for (const std::int64_t id : test.ids) {
          ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
          EXPECT_EQ(id, IntegerValue(statement.row()[0]));
        }
        EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
        RequireStatus(statement.Reset());
      }
      RequireStatus(statement.Finalize());
    }
    EXPECT_EQ(0U, state->open_count);
    EXPECT_EQ(0U, state->live_count);
  }
}

TEST(ReadSession, RebindsNullOrderEqualityPrefixesAndRetainsLaterErrors) {
  struct Case {
    std::string_view prefix;
    std::vector<std::int64_t> ids;
  };
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session, "SELECT id,payload FROM key_rows WHERE text_key=?1 ORDER BY numeric_key NULLS LAST");
  for (const Case& test :
       {Case{"beta", {9, 8}}, Case{"ALPHA", {4, 5}}, Case{"missing", {}}, Case{"1", {1}}}) {
    SCOPED_TRACE(test.prefix);
    RequireStatus(statement.Bind(1, SqlValue::Text(std::string{test.prefix})));
    for (const std::int64_t id : test.ids) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(id, IntegerValue(statement.row()[0]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Reset());
  }
  RequireStatus(statement.Bind(1, SqlValue{}));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  RequireStatus(statement.Finalize());

  ReadStatement error = PrepareStatement(session,
                                         "SELECT id FROM key_rows WHERE text_key='beta' "
                                         "AND iif(numeric_key IS NULL,abs(-9223372036854775808),1) "
                                         "ORDER BY numeric_key NULLS LAST LIMIT 2");
  ASSERT_EQ(ReadStep::kRow, TakeValue(error.Step()));
  EXPECT_EQ(9, IntegerValue(error.row()[0]));
  const auto failed = error.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_NE(std::string_view::npos, failed.error().message().find("integer overflow"));
}

TEST(ReadSession, ReusesTwoPassNullOrderingInScalarJoinedAndCompoundProducers) {
  struct Case {
    std::string_view sql;
    std::vector<std::int64_t> ids;
  };
  const std::array cases{
      Case{"SELECT (SELECT id FROM key_rows "
           "WHERE iif(numeric_key IS NULL,abs(-9223372036854775808),1) "
           "ORDER BY numeric_key NULLS LAST LIMIT 1)",
           {3}},
      Case{"SELECT b.id FROM probe_rows a CROSS JOIN key_rows b "
           "WHERE a.id=1 AND iif(b.numeric_key IS NULL,abs(-9223372036854775808),1) "
           "ORDER BY b.numeric_key NULLS LAST LIMIT 1",
           {3}},
      Case{"SELECT id,numeric_key FROM key_rows UNION ALL SELECT 100,0 "
           "ORDER BY 2 NULLS LAST LIMIT 2",
           {100, 3}},
      Case{"SELECT id FROM key_rows "
           "WHERE iif(numeric_key IS NOT NULL,abs(-9223372036854775808),1) "
           "ORDER BY numeric_key DESC NULLS FIRST LIMIT 1",
           {12}},
      Case{"SELECT id FROM key_rows "
           "WHERE iif(text_key IS NULL,abs(-9223372036854775808),1) "
           "ORDER BY text_key COLLATE BINARY NULLS LAST LIMIT 1",
           {3}},
      Case{"SELECT b.id FROM probe_rows a RIGHT JOIN key_rows b "
           "ON b.default_text=a.text_key AND a.id=5 "
           "ORDER BY b.numeric_key NULLS LAST,b.id",
           {1, 2, 3, 4, 5, 7, 9, 10, 11, 6, 8, 12}},
  };
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  for (const Case& test : cases) {
    SCOPED_TRACE(test.sql);
    ReadStatement statement = PrepareStatement(session, test.sql);
    for (int execution = 0; execution < 2; ++execution) {
      for (const std::int64_t id : test.ids) {
        ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
        EXPECT_EQ(id, IntegerValue(statement.row()[0]));
      }
      EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
      RequireStatus(statement.Reset());
    }
    RequireStatus(statement.Finalize());
  }
}

TEST(ReadSession, ReusesNullPassStateAcrossVirtualRowsAndCapturedKeys) {
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  ReadStatement joined = PrepareStatement(session,
                                          "SELECT b.id FROM probe_rows a LEFT JOIN key_rows b "
                                          "ON b.default_text=a.text_key WHERE a.id=?1 "
                                          "ORDER BY b.numeric_key NULLS LAST");
  ReadStatement scalar = PrepareStatement(
      session,
      "SELECT (SELECT b.id FROM key_rows b WHERE b.default_text=a.text_key "
      "ORDER BY b.numeric_key NULLS LAST LIMIT 1) FROM probe_rows a WHERE a.id=?1");
  for (const std::int64_t probe : {6, 5, 3, 6}) {
    SCOPED_TRACE(probe);
    RequireStatus(joined.Bind(1, SqlValue::Integer(probe)));
    RequireStatus(scalar.Bind(1, SqlValue::Integer(probe)));
    if (probe == 6) {
      for (const std::int64_t id : {1, 2, 3, 4, 5, 7, 9, 10, 11, 6, 8, 12}) {
        ASSERT_EQ(ReadStep::kRow, TakeValue(joined.Step()));
        EXPECT_EQ(id, IntegerValue(joined.row()[0]));
      }
      ASSERT_EQ(ReadStep::kRow, TakeValue(scalar.Step()));
      EXPECT_EQ(1, IntegerValue(scalar.row()[0]));
    } else {
      ASSERT_EQ(ReadStep::kRow, TakeValue(joined.Step()));
      EXPECT_EQ(SqlValueType::kNull, joined.row()[0].type());
      ASSERT_EQ(ReadStep::kRow, TakeValue(scalar.Step()));
      EXPECT_EQ(SqlValueType::kNull, scalar.row()[0].type());
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(joined.Step()));
    EXPECT_EQ(ReadStep::kDone, TakeValue(scalar.Step()));
    RequireStatus(joined.Reset());
    RequireStatus(scalar.Reset());
  }
}

TEST(ReadSession, RecoversEitherTwoPassNullOrderCursorAfterAReadFailure) {
  constexpr std::uint64_t kFixturePageBytes = 512;
  for (const std::string_view object : {"keys_default_num", "key_rows"}) {
    SCOPED_TRACE(object);
    const auto state = std::make_shared<TemporaryFileState>();
    ReadSession session = TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                                      JoinedIndexFixturePath().string()));
    ReadStatement root = PrepareStatement(
        session, "SELECT rootpage FROM sqlite_schema WHERE name='" + std::string{object} + "'");
    ASSERT_EQ(ReadStep::kRow, TakeValue(root.Step()));
    const auto page = IntegerValue(root.row()[0]);
    ASSERT_GT(page, 0);
    RequireStatus(root.Finalize());
    ReadStatement statement =
        PrepareStatement(session,
                         "SELECT id,payload FROM key_rows WHERE default_text='fallback' "
                         "ORDER BY numeric_key NULLS LAST LIMIT ?1");
    RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
    state->main_read_failure_offset =
        FileOffset{(static_cast<std::uint64_t>(page) - 1U) * kFixturePageBytes};
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    EXPECT_EQ(0U, state->main_read_failures);
    RequireStatus(statement.Reset());
    RequireStatus(statement.Bind(1, SqlValue::Integer(2)));
    const auto failed = statement.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_EQ(1U, state->main_read_failures);
    const auto reset = statement.Reset();
    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(ErrorCode::kIo, reset.error().code());
    for (const std::int64_t id : {1, 2}) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(id, IntegerValue(statement.row()[0]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Finalize());
    EXPECT_EQ(0U, state->open_count);
    EXPECT_EQ(0U, state->live_count);
  }
}

TEST(ReadSession, StreamsNativeCompoundRowsBeforeLaterArmErrors) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT id FROM items "
                       "WHERE id<=4 AND iif(id=4,abs(-9223372036854775808),1) "
                       "UNION SELECT id FROM items WHERE id<=3 LIMIT 2");
  for (const std::int64_t expected : {1, 2}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(expected, IntegerValue(statement.row()[0]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, ResetsEmptyGroupedAndCapturedNativeCompoundSources) {
  struct TestCase {
    std::string_view sql;
    std::vector<std::int64_t> rows;
  };
  const std::array cases{
      TestCase{"SELECT count(*) FROM items HAVING 0 UNION SELECT 42", {42}},
      TestCase{"SELECT count(*) FROM items HAVING 0 "
               "EXCEPT SELECT abs(-9223372036854775808)",
               {}},
      TestCase{"SELECT count(*) FROM items GROUP BY id HAVING 0 "
               "INTERSECT SELECT abs(-9223372036854775808)",
               {}},
      TestCase{"SELECT count(*) FROM items WHERE id=0 GROUP BY id UNION SELECT 7", {7}},
      TestCase{"SELECT (SELECT iif(o.id=1,1,(SELECT o.id EXCEPT SELECT -1)) "
               "UNION SELECT 0 ORDER BY 1 LIMIT 1) "
               "FROM items o WHERE o.id<=2 ORDER BY o.id",
               {0, 0}},
  };
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  for (const TestCase& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    ReadStatement statement = PrepareStatement(session, test_case.sql);
    for (int execution = 0; execution < 2; ++execution) {
      for (const std::int64_t expected : test_case.rows) {
        ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
        EXPECT_EQ(expected, IntegerValue(statement.row()[0]));
      }
      EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
      RequireStatus(statement.Reset());
    }
    RequireStatus(statement.Finalize());
  }
}

TEST(ReadSession, ResetsSuspendedBalancedCompoundsWithRepeatedCompleteKeys) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT 2,'b' UNION SELECT 1,'a' UNION SELECT 2,'b' UNION SELECT 3,'c' "
                       "UNION ALL SELECT 4,'d' UNION ALL SELECT 5,'e' UNION ALL SELECT 6,'f' "
                       "UNION ALL SELECT 7,'g' ORDER BY 2,1,2,1 LIMIT 2 OFFSET 1");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(2, IntegerValue(statement.row()[0]));
  EXPECT_EQ("b", TextValue(statement.row()[1]));
  RequireStatus(statement.Reset());
  for (const auto& [id, text] :
       {std::pair{2, std::string_view{"b"}}, std::pair{3, std::string_view{"c"}}}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(id, IntegerValue(statement.row()[0]));
    EXPECT_EQ(text, TextValue(statement.row()[1]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  RequireStatus(statement.Finalize());
}

TEST(ReadSession, PreservesNativeCompoundGroupingCollationBarriers) {
  struct TestCase {
    std::string_view result;
    std::string_view grouping;
    bool reports_error;
  };
  const std::array cases{
      TestCase{"id+0", "id+0", true},
      TestCase{"id+0", "(id+0) COLLATE BINARY", false},
      TestCase{"id+0", "1", true},
      TestCase{"id+0", "1 COLLATE BINARY COLLATE BINARY", false},
      TestCase{"id+0", "g", true},
      TestCase{"(id+0) COLLATE BINARY", "id+0", true},
      TestCase{"(id+0) COLLATE BINARY", "(id+0) COLLATE BINARY", false},
      TestCase{"(id+0) COLLATE BINARY", "1", true},
      TestCase{"(id+0) COLLATE BINARY", "g", false},
  };
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  for (const TestCase& test_case : cases) {
    const std::string sql =
        "SELECT " + std::string{test_case.result} +
        " AS g,count(iif(id=4,abs(-9223372036854775808),id)) FROM items GROUP BY " +
        std::string{test_case.grouping} +
        " UNION ALL SELECT -1,count(id) FROM items ORDER BY 1 LIMIT 1 OFFSET 1";
    SCOPED_TRACE(sql);
    ReadStatement statement = PrepareStatement(session, sql);
    const auto first = statement.Step();
    if (test_case.reports_error) {
      EXPECT_FALSE(first.has_value());
      if (!first.has_value()) {
        EXPECT_EQ(ErrorCode::kGeneric, first.error().code());
        EXPECT_NE(std::string_view::npos, first.error().message().find("integer overflow"));
      }
    } else {
      ASSERT_TRUE(first.has_value()) << first.error().message();
      EXPECT_EQ(ReadStep::kRow, *first);
      EXPECT_EQ(1, IntegerValue(statement.row()[0]));
      EXPECT_EQ(1, IntegerValue(statement.row()[1]));
      EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    }
  }
}

TEST(ReadSession, KeepsCompoundDuplicateCollationsLocalWithoutInheritedOrdering) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  for (const std::string_view collation : {"NOCASE", "missing"}) {
    SCOPED_TRACE(collation);
    ReadStatement statement =
        PrepareStatement(session, "SELECT 'a' UNION SELECT 'A' UNION ALL SELECT 'b' COLLATE " +
                                      std::string{collation});
    for (const std::string_view expected : {"A", "a", "b"}) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(expected, TextValue(statement.row()[0]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
}

TEST(ReadSession, StopsJoinedInputOrderedRowsBeforeLaterPredicateError) {
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<int>(mode));
    const auto state = std::make_shared<TemporaryFileState>();
    const ReadSessionOptions options{
        .temporary_storage = TemporaryStorageOptions{.mode = mode},
    };
    ReadSession session =
        TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                    ReadCompatibilityFixturePath().string(), options));
    state->writes_until_failure = 0U;
    ReadStatement statement = PrepareStatement(
        session,
        "SELECT a.id FROM items a JOIN model_rows b ON b.id=a.id "
        "WHERE a.id<=4 AND iif(a.id=4,abs(-9223372036854775808),1) ORDER BY a.id LIMIT 2");
    for (const std::int64_t expected : {1, 2}) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(expected, IntegerValue(statement.row()[0]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    EXPECT_EQ(0U, state->open_count);
    EXPECT_EQ(0U, state->live_count);
  }
}

TEST(ReadSession, StopsMixedDirectionJoinedRowsBeforeInnerPredicateErrors) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  {
    ReadStatement statement =
        PrepareStatement(session,
                         "SELECT a.id,b.id FROM items a CROSS JOIN items b "
                         "WHERE a.id<=2 AND iif(b.id=3,abs(-9223372036854775808),1) "
                         "ORDER BY a.id DESC,b.id ASC LIMIT 2");
    for (const std::int64_t inner : {1, 2}) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(2, IntegerValue(statement.row()[0]));
      EXPECT_EQ(inner, IntegerValue(statement.row()[1]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  ReadStatement reversed =
      PrepareStatement(session,
                       "SELECT a.id,b.id FROM items a CROSS JOIN items b "
                       "WHERE a.id<=2 AND b.id<=2 AND iif(b.id=1,abs(-9223372036854775808),1) "
                       "ORDER BY a.id ASC,b.id DESC LIMIT 1");
  ASSERT_EQ(ReadStep::kRow, TakeValue(reversed.Step()));
  EXPECT_EQ(1, IntegerValue(reversed.row()[0]));
  EXPECT_EQ(2, IntegerValue(reversed.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(reversed.Step()));
}

TEST(ReadSession, ReusesReverseJoinedStoredIndexPositioningAndRangePhases) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  {
    ReadStatement statement =
        PrepareStatement(session,
                         "SELECT a.id,b.id,b.payload FROM items a CROSS JOIN items b "
                         "WHERE a.id=1 AND iif(b.id=4,abs(-9223372036854775808),1) "
                         "ORDER BY b.score ASC LIMIT 2");
    for (const std::int64_t expected : {4096, 3840}) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(1, IntegerValue(statement.row()[0]));
      EXPECT_EQ(expected, IntegerValue(statement.row()[1]));
      EXPECT_NE(SqlValueType::kNull, statement.row()[2].type());
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  ReadStatement bounded =
      PrepareStatement(session,
                       "SELECT a.id,b.id FROM items a CROSS JOIN items b "
                       "WHERE a.id=1 AND b.score>=1 AND b.score<=4 "
                       "AND iif(b.id=4,abs(-9223372036854775808),1) ORDER BY b.score ASC LIMIT 2");
  for (const std::int64_t expected : {3841, 3585}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(bounded.Step()));
    EXPECT_EQ(1, IntegerValue(bounded.row()[0]));
    EXPECT_EQ(expected, IntegerValue(bounded.row()[1]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(bounded.Step()));
}

TEST(ReadSession, RetainsRightReplayAndCapturedOnlyJoinedOrderingErrors) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  for (const std::string_view sql : {
           "SELECT a.id FROM items a RIGHT JOIN model_rows b ON b.id=a.id "
           "WHERE iif(a.id=4,abs(-9223372036854775808),1) ORDER BY a.id LIMIT 2",
           "SELECT (SELECT a.id FROM items a CROSS JOIN items b ORDER BY a.id,b.id,"
           "iif(o.id=o.id,abs(-9223372036854775808),0) LIMIT 1) FROM items o LIMIT 1",
       }) {
    SCOPED_TRACE(sql);
    ReadStatement statement = PrepareStatement(session, sql);
    const auto failed = statement.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
    EXPECT_NE(std::string_view::npos, failed.error().message().find("integer overflow"));
  }
  ReadStatement local =
      PrepareStatement(session,
                       "SELECT (SELECT a.id FROM items a CROSS JOIN items b ORDER BY a.id,b.id,"
                       "iif(a.id=o.id,abs(-9223372036854775808),0) LIMIT 1) FROM items o LIMIT 1");
  ASSERT_EQ(ReadStep::kRow, TakeValue(local.Step()));
  EXPECT_EQ(1, IntegerValue(local.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(local.Step()));
}

TEST(ReadSession, AppliesNativeJoinedOrderMaskBoundaryBeforePublication) {
  constexpr std::size_t kNativeOrderMaskTerms = 63;
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  for (const std::size_t terms : {kNativeOrderMaskTerms, kNativeOrderMaskTerms + 1U}) {
    SCOPED_TRACE(terms);
    std::string sql = "SELECT a.id,b.id FROM items a CROSS JOIN items b ORDER BY a.id,b.id";
    for (std::size_t term = 0; term < terms - 3U; ++term) {
      sql += ",a.id+" + std::to_string(term);
    }
    sql += ",abs(-9223372036854775808) LIMIT 1";
    ReadStatement statement = PrepareStatement(session, sql);
    const auto first = statement.Step();
    if (terms > kNativeOrderMaskTerms) {
      ASSERT_FALSE(first.has_value());
      EXPECT_EQ(ErrorCode::kGeneric, first.error().code());
      EXPECT_NE(std::string_view::npos, first.error().message().find("integer overflow"));
    } else {
      ASSERT_EQ(ReadStep::kRow, TakeValue(first));
      EXPECT_EQ(1, IntegerValue(statement.row()[0]));
      EXPECT_EQ(1, IntegerValue(statement.row()[1]));
      EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    }
  }
}

TEST(ReadSession, ReversesTheSixtyFourthJoinedLoop) {
  const std::size_t last = kMaximumSelectSources - 1U;
  std::string sql = "SELECT t" + std::to_string(last) + ".id FROM model_rows t0";
  for (std::size_t source = 1; source < kMaximumSelectSources; ++source) {
    sql += " CROSS JOIN model_rows t" + std::to_string(source);
  }
  sql += " WHERE t0.id=1";
  for (std::size_t source = 1; source < last; ++source) {
    sql += " AND t" + std::to_string(source) + ".id=1";
  }
  sql += " ORDER BY t" + std::to_string(last) + ".id DESC LIMIT 1";
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(session, sql);
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(32, IntegerValue(statement.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, PublishesInputOrderedBlocksBeforeLaterPredicateError) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session,
      "SELECT category,score FROM items "
      "WHERE iif(id=4096,abs(-9223372036854775808),1) ORDER BY category,score+0 LIMIT 2");
  for (const std::int64_t expected : {1, 2}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ("category-000" + std::to_string(expected), TextValue(statement.row()[0]));
    EXPECT_EQ(expected, IntegerValue(statement.row()[1]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, RetainsNextBlockPredicateErrorBeforePriorBlockRows) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session,
      "SELECT category,score FROM items "
      "WHERE iif(id=2,abs(-9223372036854775808),1) ORDER BY category,score+0 LIMIT 1");
  const auto failed = statement.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_NE(std::string_view::npos, failed.error().message().find("integer overflow"));
}

TEST(ReadSession, ReusesTheBlockProducerForJoinedRowsAndScalarDemand) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  {
    ReadStatement statement =
        PrepareStatement(session,
                         "SELECT b.category,b.score FROM items a CROSS JOIN items b "
                         "WHERE a.id=1 AND iif(b.id=4096,abs(-9223372036854775808),1) "
                         "ORDER BY b.category,b.score+0 LIMIT 2");
    for (const std::int64_t expected : {1, 2}) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ("category-000" + std::to_string(expected), TextValue(statement.row()[0]));
      EXPECT_EQ(expected, IntegerValue(statement.row()[1]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  ReadStatement scalar = PrepareStatement(
      session,
      "SELECT (SELECT score FROM items WHERE iif(id=4096,abs(-9223372036854775808),1) "
      "ORDER BY category,score+0 LIMIT 2)");
  ASSERT_EQ(ReadStep::kRow, TakeValue(scalar.Step()));
  EXPECT_EQ(1, IntegerValue(scalar.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(scalar.Step()));
}

TEST(ReadSession, EvaluatesTheNextBlockPayloadBeforePublishingThePreviousBlock) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT category,iif(id=2,abs(-9223372036854775808),score) FROM items "
                       "ORDER BY category,score+0 LIMIT 1");
  const auto failed = statement.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_NE(std::string_view::npos, failed.error().message().find("integer overflow"));
}

TEST(ReadSession, SpillsAndResetsOrderedBlocksWithoutLosingPendingRows) {
  const std::string padding(256, 'x');
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<int>(mode));
    const auto state = std::make_shared<TemporaryFileState>();
    const ReadSessionOptions options{
        .temporary_storage =
            TemporaryStorageOptions{
                .mode = mode,
                .sorter_memory_threshold =
                    ByteCount{mode == TemporaryStoreMode::kFile ? 32U : 1U << 20U},
            },
    };
    ReadSession session = TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                                      IndexFixturePath().string(), options));
    {
      ReadStatement statement =
          PrepareStatement(session, "SELECT category,score,payload||'" + padding +
                                        "' FROM items WHERE category<='category-0004' "
                                        "ORDER BY category,score+0");
      for (const std::int64_t expected : {1, 2, 3, 4}) {
        ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
        EXPECT_EQ(expected, IntegerValue(statement.row()[1]));
        EXPECT_GE(TextValue(statement.row()[2]).size(), padding.size());
      }
      EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
      EXPECT_EQ(0U, state->live_count);
      EXPECT_EQ(mode == TemporaryStoreMode::kFile, state->open_count > 0U);
      RequireStatus(statement.Reset());
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(1, IntegerValue(statement.row()[1]));
      RequireStatus(statement.Reset());
      EXPECT_EQ(0U, state->live_count);
    }
    ReadStatement bounded =
        PrepareStatement(session,
                         "SELECT category,score FROM items WHERE category<='category-0004' "
                         "ORDER BY category,score+0 LIMIT ?1 OFFSET ?2");
    for (const std::int64_t offset : {0, 1}) {
      RequireStatus(bounded.Bind(1, SqlValue::Integer(2)));
      RequireStatus(bounded.Bind(2, SqlValue::Integer(offset)));
      for (std::int64_t expected = offset + 1; expected <= offset + 2; ++expected) {
        ASSERT_EQ(ReadStep::kRow, TakeValue(bounded.Step()));
        EXPECT_EQ(expected, IntegerValue(bounded.row()[1]));
      }
      EXPECT_EQ(ReadStep::kDone, TakeValue(bounded.Step()));
      RequireStatus(bounded.Reset());
      EXPECT_EQ(0U, state->live_count);
    }
    const std::size_t opens = state->open_count;
    RequireStatus(bounded.Bind(1, SqlValue::Integer(0)));
    RequireStatus(bounded.Bind(2, SqlValue::Text("not evaluated")));
    EXPECT_EQ(ReadStep::kDone, TakeValue(bounded.Step()));
    EXPECT_EQ(opens, state->open_count);
    EXPECT_EQ(0U, state->live_count);
  }
}

TEST(ReadSession, RetainsNullableCollatedAndGrowingPrefixesAcrossBlockYields) {
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  ReadStatement statement = PrepareStatement(session,
                                             "SELECT id,text_key FROM key_rows "
                                             "ORDER BY text_key COLLATE NOCASE,numeric_key+0,id");
  for (const std::int64_t expected : {6, 12, 7, 3, 2, 1, 4, 5, 8, 9, 10, 11}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(expected, IntegerValue(statement.row()[0]));
    EXPECT_EQ(expected == 6 || expected == 12 || expected == 7,
              statement.row()[1].type() == SqlValueType::kNull);
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, StopsAnOrderedInnerLoopAfterTopNRejectsItsNextRow) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session,
      "SELECT a.id,b.id FROM items a CROSS JOIN items b "
      "WHERE a.id<=2 AND iif(b.id=3,abs(-9223372036854775808),1) ORDER BY b.id LIMIT 1");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, KeepsOrderedInnerPredicateAndReplayFailuresBeforeCutoff) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  for (const std::string_view sql : {
           "SELECT a.id,b.id FROM items a CROSS JOIN items b "
           "WHERE a.id<=2 AND iif(b.id=2,abs(-9223372036854775808),1) ORDER BY b.id LIMIT 1",
           "SELECT a.id,b.id FROM items a CROSS JOIN items b "
           "WHERE a.id<=2 AND iif(b.id=3,abs(-9223372036854775808),1) ORDER BY b.id DESC LIMIT 1",
           "SELECT a.id,b.id FROM items a CROSS JOIN items b "
           "WHERE a.id<=2 AND iif(b.id=3,abs(-9223372036854775808),1) ORDER BY b.id LIMIT -1",
           "SELECT b.id FROM items a RIGHT JOIN items b ON 0 "
           "WHERE iif(b.id=3,abs(-9223372036854775808),1) ORDER BY b.id LIMIT 1",
       }) {
    SCOPED_TRACE(sql);
    ReadStatement statement = PrepareStatement(session, sql);
    const auto failed = statement.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
    EXPECT_NE(std::string_view::npos, failed.error().message().find("integer overflow"));
  }
}

TEST(ReadSession, PreservesRejectedPayloadOffsetAndOuterReplayOrder) {
  struct Case {
    std::string_view sql;
    std::int64_t first;
    std::int64_t second;
  };
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  for (const Case& test : {
           Case{"SELECT a.id,iif(b.id=2,abs(-9223372036854775808),b.id) "
                "FROM items a CROSS JOIN items b WHERE a.id<=2 ORDER BY b.id LIMIT 1",
                1, 1},
           Case{"SELECT a.id,b.id FROM items a CROSS JOIN items b "
                "WHERE a.id<=2 AND iif(b.id=4,abs(-9223372036854775808),1) "
                "ORDER BY b.id LIMIT 1 OFFSET 1",
                2, 1},
           Case{"SELECT a.id,b.id FROM items a CROSS JOIN model_rows b "
                "WHERE a.id<=2 AND iif(b.id=3,abs(-9223372036854775808),1) "
                "ORDER BY b.id DESC LIMIT 1",
                1, 32},
           Case{"SELECT b.id,c.id FROM items a RIGHT JOIN model_rows b ON 0 "
                "CROSS JOIN items c WHERE b.id<=2 "
                "AND iif(c.id=3,abs(-9223372036854775808),1) ORDER BY c.id LIMIT 1",
                1, 1},
       }) {
    SCOPED_TRACE(test.sql);
    ReadStatement statement = PrepareStatement(session, test.sql);
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(test.first, IntegerValue(statement.row()[0]));
    EXPECT_EQ(test.second, IntegerValue(statement.row()[1]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  ReadStatement null_row = PrepareStatement(session,
                                            "SELECT a.id,b.id FROM items a LEFT JOIN items b ON 0 "
                                            "WHERE a.id<=2 ORDER BY b.id LIMIT 1");
  ASSERT_EQ(ReadStep::kRow, TakeValue(null_row.Step()));
  EXPECT_EQ(1, IntegerValue(null_row.row()[0]));
  EXPECT_EQ(SqlValueType::kNull, null_row.row()[1].type());
  EXPECT_EQ(ReadStep::kDone, TakeValue(null_row.Step()));
  ReadStatement exists = PrepareStatement(
      session,
      "SELECT EXISTS(SELECT a.id FROM items a CROSS JOIN model_rows b "
      "WHERE a.id<=2 AND iif(b.id=32,abs(-9223372036854775808),1) ORDER BY b.id DESC)");
  ASSERT_EQ(ReadStep::kRow, TakeValue(exists.Step()));
  EXPECT_EQ(1, IntegerValue(exists.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(exists.Step()));
}

TEST(ReadSession, RetiresNoncoveringOrderedInnerIndexRangesInEitherDirection) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  for (const bool descending : {false, true}) {
    SCOPED_TRACE(descending);
    ReadStatement statement =
        PrepareStatement(session,
                         "SELECT a.id,b.id,b.payload,a.payload FROM items a CROSS JOIN items b "
                         "WHERE a.id<=2 AND b.score>=a.id AND b.score<=4 "
                         "AND iif(b.id=3,abs(-9223372036854775808),1) ORDER BY b.score " +
                             std::string{descending ? "DESC" : "ASC"} + " LIMIT 1");
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(1, IntegerValue(statement.row()[0]));
    EXPECT_EQ(descending ? 4 : 3841, IntegerValue(statement.row()[1]));
    EXPECT_NE(SqlValueType::kNull, statement.row()[2].type());
    EXPECT_NE(SqlValueType::kNull, statement.row()[3].type());
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
}

TEST(ReadSession, KeepsNativeOuterRowIdRangeTraversalBeforeInnerCutoff) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT a.id,b.id,b.payload FROM items a CROSS JOIN items b "
                       "WHERE a.id<=2 AND b.score>=a.id AND b.score<=4 "
                       "AND iif(b.id=3,abs(-9223372036854775808),1) "
                       "ORDER BY b.score DESC LIMIT 1");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(4, IntegerValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, RebindsJoinedRowIdRangesAcrossDirectionsAndNumericKeyClasses) {
  struct Case {
    SqlValue lower;
    SqlValue upper;
    std::vector<std::int64_t> ids;
  };
  const std::array cases{
      Case{SqlValue::Real(1.5), SqlValue::Text("3"), {2, 3}},
      Case{SqlValue::Text("1"), SqlValue::Real(3.5), {2, 3}},
      Case{SqlValue{}, SqlValue::Integer(4), {}},
      Case{SqlValue::Integer(1), SqlValue{}, {}},
      Case{SqlValue::Text("not-numeric"), SqlValue::Integer(4), {}},
      Case{SqlValue::Integer(1), SqlValue::Blob(ByteBuffer{ByteCount{1}}), {2, 3, 4}},
      Case{SqlValue::Integer(2), SqlValue::Integer(2), {}},
      Case{SqlValue::Real(1.5), SqlValue::Text("3"), {2, 3}},
  };
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  for (const bool reverse : {false, true}) {
    ReadStatement statement =
        PrepareStatement(session,
                         "SELECT b.id FROM items a CROSS JOIN items b "
                         "WHERE a.id=1 AND b.id>?1 AND b.id<=?2 ORDER BY b.id " +
                             std::string{reverse ? "DESC" : "ASC"});
    for (const Case& test : cases) {
      SCOPED_TRACE(reverse);
      RequireStatus(statement.Bind(1, test.lower));
      RequireStatus(statement.Bind(2, test.upper));
      for (std::size_t index = 0; index < test.ids.size(); ++index) {
        ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
        EXPECT_EQ(test.ids[reverse ? test.ids.size() - index - 1U : index],
                  IntegerValue(statement.row()[0]));
      }
      EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
      RequireStatus(statement.Reset());
    }
  }
}

TEST(ReadSession, PreservesJoinedRowIdRangeEndTimingAndCompleteOuterReplay) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  for (const std::string_view start : {"b.id>10000", "b.id>NULL"}) {
    ReadStatement statement = PrepareStatement(
        session, "SELECT count(*) FROM items a CROSS JOIN items b WHERE a.id=1 AND " +
                     std::string{start} + " AND b.id<abs(-9223372036854775808)");
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(0, IntegerValue(statement.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  ReadStatement failed =
      PrepareStatement(session,
                       "SELECT count(*) FROM items a CROSS JOIN items b "
                       "WHERE a.id=1 AND b.id>0 AND b.id<abs(-9223372036854775808)");
  const auto error = failed.Step();
  ASSERT_FALSE(error.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, error.error().code());
  EXPECT_NE(std::string_view::npos, error.error().message().find("integer overflow"));
  const Status finalized = failed.Finalize();
  ASSERT_FALSE(finalized.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, finalized.error().code());
  for (const std::string_view join : {"RIGHT", "FULL"}) {
    SCOPED_TRACE(join);
    ReadStatement statement = PrepareStatement(
        session, "SELECT a.id,b.id FROM model_rows a " + std::string{join} +
                     " JOIN model_rows b ON a.id=1 AND b.id>=a.id AND b.id<=a.id+1 "
                     "WHERE b.id<=4 ORDER BY b.id");
    for (const std::int64_t id : {1, 2, 3, 4}) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      if (id <= 2) {
        EXPECT_EQ(1, IntegerValue(statement.row()[0]));
      } else {
        EXPECT_EQ(SqlValueType::kNull, statement.row()[0].type());
      }
      EXPECT_EQ(id, IntegerValue(statement.row()[1]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
}

TEST(ReadSession, PreservesNativeLeadingAndOrderDistinctHiddenKeyErrors) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement leading = PrepareStatement(
      session, "SELECT id FROM items ORDER BY abs(-9223372036854775808),id LIMIT 2");
  const auto failed = leading.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_NE(std::string_view::npos, failed.error().message().find("integer overflow"));

  ReadStatement trailing = PrepareStatement(
      session, "SELECT id FROM items ORDER BY id,abs(-9223372036854775808) LIMIT 2");
  for (const std::int64_t expected : {1, 2}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(trailing.Step()));
    EXPECT_EQ(expected, IntegerValue(trailing.row()[0]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(trailing.Step()));

  ReadStatement single = PrepareStatement(
      session, "SELECT id FROM items WHERE id=1 ORDER BY abs(-9223372036854775808) LIMIT 2");
  ASSERT_EQ(ReadStep::kRow, TakeValue(single.Step()));
  EXPECT_EQ(1, IntegerValue(single.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(single.Step()));
}

TEST(ReadSession, AppliesNativeOrderMaskBoundaryBeforeOneRowShortcut) {
  constexpr std::size_t kNativeOrderMaskTerms = 63;
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  for (const std::size_t terms : {kNativeOrderMaskTerms, kNativeOrderMaskTerms + 1U}) {
    for (const bool one_row : {false, true}) {
      SCOPED_TRACE(terms);
      SCOPED_TRACE(one_row);
      std::string sql = "SELECT id FROM items";
      if (one_row) {
        sql += " WHERE id=1";
      }
      sql += " ORDER BY id";
      for (std::size_t term = 0; term < terms - 2U; ++term) {
        sql += ",id+" + std::to_string(term);
      }
      sql += ",abs(-9223372036854775808) LIMIT 2";
      ReadStatement statement = PrepareStatement(session, sql);
      if (terms > kNativeOrderMaskTerms) {
        const auto failed = statement.Step();
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
        EXPECT_NE(std::string_view::npos, failed.error().message().find("integer overflow"));
      } else {
        for (std::int64_t expected = 1; expected <= (one_row ? 1 : 2); ++expected) {
          ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
          EXPECT_EQ(expected, IntegerValue(statement.row()[0]));
        }
        EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
      }
    }
  }
}

TEST(ReadSession, RebindsInputOrderedLimitAndOffsetWithoutVisitingLaterErrors) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session,
      "SELECT id FROM items "
      "WHERE id<=4 AND iif(id=4,abs(-9223372036854775808),1) ORDER BY id LIMIT ?1 OFFSET ?2");
  for (const std::int64_t offset : {0, 1}) {
    RequireStatus(statement.Bind(1, SqlValue::Integer(2)));
    RequireStatus(statement.Bind(2, SqlValue::Integer(offset)));
    for (std::int64_t expected = offset + 1; expected <= offset + 2; ++expected) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(expected, IntegerValue(statement.row()[0]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Reset());
  }
  RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
  RequireStatus(statement.Bind(2, SqlValue::Text("not evaluated")));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, ReusesInputOrderedRowsForScalarDemand) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session,
      "SELECT (SELECT id FROM items "
      "WHERE id<=4 AND iif(id=4,abs(-9223372036854775808),1) ORDER BY id LIMIT 2)");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, RetainsCapturedOnlyHiddenOrderingErrors) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  {
    ReadStatement captured = PrepareStatement(
        session,
        "SELECT (SELECT i.id FROM items AS i ORDER BY i.id,"
        "iif(o.id=o.id,abs(-9223372036854775808),0) LIMIT 1) FROM items AS o LIMIT 1");
    const auto failed = captured.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
    EXPECT_NE(std::string_view::npos, failed.error().message().find("integer overflow"));
  }
  for (const std::string_view sql : {
           "SELECT (SELECT i.id FROM items AS i ORDER BY i.id,"
           "iif(i.id=o.id,abs(-9223372036854775808),0) LIMIT 1) FROM items AS o LIMIT 1",
           "SELECT (SELECT i.id FROM items AS i WHERE i.id=1 ORDER BY i.id,"
           "iif(o.id=o.id,abs(-9223372036854775808),0) LIMIT 1) FROM items AS o LIMIT 1",
       }) {
    ReadStatement safe = PrepareStatement(session, sql);
    ASSERT_EQ(ReadStep::kRow, TakeValue(safe.Step()));
    EXPECT_EQ(1, IntegerValue(safe.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(safe.Step()));
  }
}

TEST(ReadSession, RebindsOrderedLimitBetweenTopNAndExternalSorterStrategies) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session, "SELECT id, name FROM items ORDER BY name||'' LIMIT ?1 OFFSET ?2");

  RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(1)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(2, IntegerValue(statement.row()[0]));
  EXPECT_EQ("beta", TextValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));

  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(-1)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(1)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ("beta", TextValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ("gamma", TextValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));

  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(std::numeric_limits<std::int64_t>::max())));
  RequireStatus(statement.Bind(2, SqlValue::Integer(1)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ("beta", TextValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ("gamma", TextValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));

  ReadStatement invalid =
      PrepareStatement(session, "SELECT name FROM items ORDER BY name||'' LIMIT ?1");
  RequireStatus(invalid.Bind(1, SqlValue::Text("invalid")));
  const auto rejected = invalid.Step();
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kTypeMismatch, rejected.error().code());

  ReadStatement zero =
      PrepareStatement(session, "SELECT name FROM items ORDER BY name||'' LIMIT 0 OFFSET ?1");
  RequireStatus(zero.Bind(1, SqlValue::Text("not evaluated")));
  EXPECT_EQ(ReadStep::kDone, TakeValue(zero.Step()));
}

TEST(ReadSession, RejectsNonSelectStatementsAtTheReadOnlyBoundary) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));

  for (const std::string_view sql : {
           "INSERT INTO items(id, name) VALUES(99, 'write')",
           "UPDATE items SET name='write' WHERE id=1",
           "DELETE FROM items WHERE id=1",
           "CREATE TABLE write_attempt(id)",
           "BEGIN",
           "SAVEPOINT write_attempt",
       }) {
    const auto prepared = session.Prepare(Utf8View{sql});
    ASSERT_FALSE(prepared.has_value()) << sql;
    EXPECT_EQ(ErrorCode::kGeneric, prepared.error().code()) << sql;
    EXPECT_NE(std::string_view::npos,
              prepared.error().message().find("only supports SELECT statements"))
        << sql;
  }
}

TEST(ReadSession, AccumulatesJoinedAggregateRowsAndRetainsEachSourceOwner) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement global =
      PrepareStatement(session,
                       "SELECT count(),sum(a.id),sum(b.id),sum(a._rowid_),sum(b._rowid_) "
                       "FROM Items a CROSS JOIN Items b WHERE a.id<=2");
  ASSERT_EQ(ReadStep::kRow, TakeValue(global.Step()));
  const std::array<std::int64_t, 5> expected{6, 9, 12, 9, 12};
  ASSERT_EQ(expected.size(), global.row().size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(expected[index], IntegerValue(global.row()[index]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(global.Step()));
  ReadStatement grouped =
      PrepareStatement(session,
                       "SELECT b.id,count(),sum(a.id),min(a._rowid_),max(b.oid) "
                       "FROM Items a,Items b WHERE a.id<=2 GROUP BY b.id ORDER BY b.id");
  for (std::int64_t id = 1; id <= 3; ++id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(grouped.Step()));
    EXPECT_EQ(id, IntegerValue(grouped.row()[0]));
    EXPECT_EQ(2, IntegerValue(grouped.row()[1]));
    EXPECT_EQ(3, IntegerValue(grouped.row()[2]));
    EXPECT_EQ(1, IntegerValue(grouped.row()[3]));
    EXPECT_EQ(id, IntegerValue(grouped.row()[4]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(grouped.Step()));
}

TEST(ReadSession, RebindsJoinedGroupingThroughCompletedRowLimits) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT b.id,sum(a.id) FROM Items a CROSS JOIN Items b WHERE a.id<=?1 "
                       "GROUP BY b.id ORDER BY b.id DESC LIMIT ?2");
  RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(1)));
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(3, IntegerValue(statement.row()[0]));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(2)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(-1)));
  for (std::int64_t id = 3; id > 0; --id) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(id, IntegerValue(statement.row()[0]));
    EXPECT_EQ(3, IntegerValue(statement.row()[1]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, ExecutesAndResetsGroupedAggregates) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement statement = PrepareStatement(session,
                                             "SELECT score IS NULL,count(),min(id) FROM items "
                                             "GROUP BY score IS NULL ORDER BY 1");

  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(3U, statement.row().size());
  EXPECT_EQ(0, IntegerValue(statement.row()[0]));
  EXPECT_EQ(2, IntegerValue(statement.row()[1]));
  EXPECT_EQ(1, IntegerValue(statement.row()[2]));
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
  EXPECT_EQ(3, IntegerValue(statement.row()[2]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));

  RequireStatus(statement.Reset());
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(0, IntegerValue(statement.row()[0]));
}

TEST(ReadSession, ExecutesAndResetsAggregateModifiers) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT score IS NULL,"
                       "group_concat(name ORDER BY id DESC) FILTER (WHERE id>1),"
                       "count(DISTINCT category) "
                       "FROM items GROUP BY score IS NULL ORDER BY 1");

  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(3U, statement.row().size());
  EXPECT_EQ(0, IntegerValue(statement.row()[0]));
  EXPECT_EQ("beta", TextValue(statement.row()[1]));
  EXPECT_EQ(2, IntegerValue(statement.row()[2]));
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ("gamma", TextValue(statement.row()[1]));
  EXPECT_EQ(0, IntegerValue(statement.row()[2]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));

  RequireStatus(statement.Reset());
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(0, IntegerValue(statement.row()[0]));
  EXPECT_EQ("beta", TextValue(statement.row()[1]));
  RequireStatus(statement.Reset());
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(0, IntegerValue(statement.row()[0]));
}

TEST(ReadSession, ExecutesAndResetsAggregateDistinctAndCompoundCores) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement distinct = PrepareStatement(
      session, "SELECT DISTINCT count() AS row_count FROM items GROUP BY score IS NULL ORDER BY 1");
  ASSERT_EQ(1U, distinct.result_columns().size());
  EXPECT_EQ("row_count", distinct.result_columns()[0].name);
  EXPECT_EQ(ReadStep::kRow, TakeValue(distinct.Step()));
  EXPECT_EQ(1, IntegerValue(distinct.row()[0]));
  EXPECT_EQ(ReadStep::kRow, TakeValue(distinct.Step()));
  EXPECT_EQ(2, IntegerValue(distinct.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(distinct.Step()));
  RequireStatus(distinct.Reset());
  EXPECT_EQ(ReadStep::kRow, TakeValue(distinct.Step()));
  EXPECT_EQ(1, IntegerValue(distinct.row()[0]));

  ReadStatement compound =
      PrepareStatement(session,
                       "SELECT count() AS value FROM items WHERE id>=?1 "
                       "UNION ALL SELECT sum(id) FROM items WHERE id>=?2 ORDER BY 1");
  const auto execute = [&](std::int64_t first, std::int64_t second, std::int64_t expected_first,
                           std::int64_t expected_second) {
    RequireStatus(compound.Bind(1, SqlValue::Integer(first)));
    RequireStatus(compound.Bind(2, SqlValue::Integer(second)));
    EXPECT_EQ(ReadStep::kRow, TakeValue(compound.Step()));
    EXPECT_EQ(expected_first, IntegerValue(compound.row()[0]));
    EXPECT_EQ(ReadStep::kRow, TakeValue(compound.Step()));
    EXPECT_EQ(expected_second, IntegerValue(compound.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(compound.Step()));
  };
  execute(2, 2, 2, 5);
  RequireStatus(compound.Reset());
  execute(3, 3, 1, 3);
}

TEST(ReadSession, IntegratesAggregateMetadataRebindingAndTemporaryModes) {
  constexpr ByteCount kMemoryBudget{64U * 1024U};
  constexpr ByteCount kFileBudget{2048};
  const ReadSessionOptions memory_options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kMemory,
              .sorter_memory_threshold = kMemoryBudget,
          },
  };
  ReadSession memory =
      TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string(), memory_options));
  ReadStatement memory_statement = PrepareStatement(memory, kIntegratedAggregateSql);
  ExerciseIntegratedAggregate(memory_statement);

  const auto file_state = std::make_shared<TemporaryFileState>();
  const ReadSessionOptions file_options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kFile,
              .sorter_memory_threshold = kFileBudget,
          },
  };
  ReadSession file =
      TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(file_state),
                                  ReadCompatibilityFixturePath().string(), file_options));
  ReadStatement file_statement = PrepareStatement(file, kIntegratedAggregateSql);
  ExerciseIntegratedAggregate(file_statement);
  EXPECT_GT(file_state->open_count, 0U);
  EXPECT_LE(file_state->peak_live_count, 2U);
  EXPECT_EQ(0U, file_state->live_count);
}

TEST(ReadSession, CleansAggregateTemporaryFilesAfterInjectedFailure) {
  constexpr ByteCount kFileBudget{2048};
  const auto state = std::make_shared<TemporaryFileState>();
  const ReadSessionOptions options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kFile,
              .sorter_memory_threshold = kFileBudget,
          },
  };
  ReadSession session =
      TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                  ReadCompatibilityFixturePath().string(), options));
  ReadStatement statement = PrepareStatement(session, kIntegratedAggregateSql);
  RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(0)));
  state->writes_until_failure = 0U;

  const Result<ReadStep> failed = statement.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_GT(state->open_count, 0U);
  EXPECT_LE(state->peak_live_count, 2U);
  EXPECT_EQ(0U, state->live_count);

  const Status reset = statement.Reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(ErrorCode::kIo, reset.error().code());
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(0, IntegerValue(statement.row()[0]));
  while (TakeValue(statement.Step()) == ReadStep::kRow) {
  }
  EXPECT_EQ(0U, state->live_count);
  RequireStatus(statement.Finalize());
}

TEST(ReadSession, CleansJoinedAggregateStateAfterPmaFlushFailure) {
  struct Case {
    std::string_view sql;
    std::int64_t first_count = 32;
  };
  const std::array queries{
      Case{.sql = "SELECT b.id,count(),group_concat(a.id ORDER BY a.id DESC) "
                  "FROM model_rows a CROSS JOIN model_rows b GROUP BY b.id ORDER BY b.id"},
      Case{.sql =
               "SELECT a.id,count(),group_concat(b.id ORDER BY b.id DESC) "
               "FROM model_rows a LEFT JOIN model_rows b ON a.id<32 GROUP BY a.id ORDER BY a.id"},
      Case{.sql = "SELECT a.id,count(),group_concat(c.id ORDER BY c.id DESC) "
                  "FROM model_rows a LEFT JOIN items b USING(id) CROSS JOIN model_rows c "
                  "GROUP BY a.id ORDER BY a.id"},
      Case{.sql = "SELECT coalesce(a.id,b.id),count(),group_concat(c.id ORDER BY c.id DESC) "
                  "FROM model_rows a FULL JOIN model_rows b ON 0 CROSS JOIN model_rows c "
                  "GROUP BY coalesce(a.id,b.id) ORDER BY coalesce(a.id,b.id)",
           .first_count = 64},
  };
  for (const Case& test_case : queries) {
    SCOPED_TRACE(test_case.sql);
    const auto state = std::make_shared<TemporaryFileState>();
    const ReadSessionOptions options{
        .temporary_storage =
            TemporaryStorageOptions{
                .mode = TemporaryStoreMode::kFile,
                .sorter_memory_threshold = ByteCount{32U << 10U},
            },
    };
    ReadSession session =
        TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                    ReadCompatibilityFixturePath().string(), options));
    ReadStatement statement = PrepareStatement(session, test_case.sql);
    state->writes_until_failure = 0U;
    const auto failed = statement.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_GT(state->open_count, 0U);
    EXPECT_LE(state->peak_live_count, 2U);
    EXPECT_EQ(0U, state->live_count);
    const auto reset = statement.Reset();
    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(ErrorCode::kIo, reset.error().code());
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(1, IntegerValue(statement.row()[0]));
    EXPECT_EQ(test_case.first_count, IntegerValue(statement.row()[1]));
    std::size_t rows = 1;
    while (TakeValue(statement.Step()) == ReadStep::kRow) {
      ++rows;
    }
    EXPECT_EQ(32U, rows);
    EXPECT_EQ(0U, state->live_count);
    RequireStatus(statement.Finalize());
  }
}

TEST(ReadSession, PreservesNumericComparisonAgainstIndexedTextSpellings) {
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session,
      "SELECT id FROM key_rows "
      "WHERE text_key=(SELECT numeric_key FROM probe_rows WHERE id=1) ORDER BY id");
  for (const std::int64_t expected : {1, 2, 3}) {
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(expected, IntegerValue(statement.row()[0]));
  }
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, RebindsTypedJoinedIndexKeysAfterSuspensionAndCompletion) {
  struct Case {
    SqlValue key;
    std::vector<std::int64_t> ids;
  };
  const std::array cases{
      Case{.key = SqlValue::Integer(1), .ids = {1}},
      Case{.key = SqlValue::Text("01"), .ids = {2}},
      Case{.key = SqlValue::Real(1.0), .ids = {}},
      Case{.key = SqlValue::Text("ALPHA"), .ids = {4, 5}},
      Case{.key = SqlValue{}, .ids = {}},
      Case{.key = SqlValue::Blob(ByteBuffer::CopyOf(AsBytes("1"))), .ids = {}},
      Case{.key = SqlValue::Text("1"), .ids = {1}},
  };
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  for (const bool noncovering : {false, true}) {
    SCOPED_TRACE(noncovering);
    ReadStatement statement = PrepareStatement(
        session, "SELECT b.id" +
                     std::string{noncovering ? ",b.payload,b.default_text,b.real_key" : ""} +
                     " FROM probe_rows p CROSS JOIN key_rows b "
                     "WHERE p.id=1 AND b.text_key=?1" +
                     (noncovering ? "" : " ORDER BY b.id"));
    for (const bool complete : {false, true}) {
      for (const Case& test_case : cases) {
        RequireStatus(statement.Bind(1, test_case.key));
        for (const std::int64_t id : test_case.ids) {
          ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
          EXPECT_EQ(id, IntegerValue(statement.row()[0]));
          if (noncovering) {
            ASSERT_EQ(4U, statement.row().size());
            EXPECT_EQ(SqlValueType::kText, statement.row()[1].type());
            EXPECT_EQ("fallback", TextValue(statement.row()[2]));
            EXPECT_EQ(SqlValueType::kReal, statement.row()[3].type());
          }
          if (!complete) {
            break;
          }
        }
        if (complete || test_case.ids.empty()) {
          EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
        }
        RequireStatus(statement.Reset());
      }
    }
  }
}

TEST(ReadSession, PublishesPreservedUsingKeysForQualifiedWildcards) {
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  ReadStatement statement = PrepareStatement(session,
                                             "SELECT a.*,a.id,b.id FROM probe_rows a "
                                             "FULL JOIN key_rows b USING(id) WHERE b.id=12");
  ASSERT_EQ(7U, statement.result_columns().size());
  EXPECT_EQ(std::nullopt, statement.result_columns()[0].declared_type);
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(7U, statement.row().size());
  EXPECT_EQ(12, statement.row()[0].integer_value());
  for (std::size_t index = 1; index <= 5; ++index) {
    EXPECT_EQ(SqlValueType::kNull, statement.row()[index].type());
  }
  EXPECT_EQ(12, statement.row()[6].integer_value());
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, KeepsWildcardOwnerNamesSeparateFromPreservedValueMetadata) {
  const auto fixture = JoinedIndexFixturePath().parent_path().parent_path() / "wildcard_namespace" /
                       "sqlite-3.54.0-wildcard-namespace.db";
  ReadSession session = TakeValue(ReadSession::Open(fixture.string()));
  for (const std::string_view join : {"LEFT JOIN", "RIGHT JOIN", "FULL JOIN"}) {
    for (const bool key_column : {false, true}) {
      for (const bool qualified : {false, true}) {
        SCOPED_TRACE(join);
        SCOPED_TRACE(key_column);
        SCOPED_TRACE(qualified);
        ReadStatement statement = PrepareStatement(
            session, "SELECT " + std::string{qualified ? "a.*" : "*"} +
                         ",a.ID,b.id FROM wild_l a " + std::string{join} + " wild_r b USING(" +
                         std::string{key_column ? "\"Key\"" : "ID"} + ")");
        const std::size_t wildcard_columns = qualified ? 3U : 5U;
        ASSERT_EQ(wildcard_columns + 2U, statement.result_columns().size());
        const std::size_t key = key_column ? 1U : 0U;
        EXPECT_EQ(key_column ? "Key" : "ID", statement.result_columns()[key].name);
        const std::optional<std::string> expected =
            join == "FULL JOIN"    ? std::nullopt
            : join == "RIGHT JOIN" ? std::optional<std::string>{key_column ? "NUMERIC" : "INTEGER"}
                                   : std::optional<std::string>{key_column ? "TEXT" : "INT"};
        EXPECT_EQ(expected, statement.result_columns()[key].declared_type);
        EXPECT_EQ(std::optional<std::string>{"INT"},
                  statement.result_columns()[wildcard_columns].declared_type);
        EXPECT_EQ(std::optional<std::string>{"INTEGER"},
                  statement.result_columns()[wildcard_columns + 1U].declared_type);
        EXPECT_EQ("ID", statement.result_columns()[wildcard_columns].name);
        EXPECT_EQ("id", statement.result_columns()[wildcard_columns + 1U].name);
        RequireStatus(statement.Finalize());
      }
    }
  }
}

TEST(ReadSession, MatchesIndependentJoinedIndexNullAndPreservationModels) {
  const std::array<std::optional<std::string_view>, 6> left_keys{
      "1", "ALPHA", std::nullopt, "beta", "missing", "fallback",
  };
  const std::array<std::optional<std::string_view>, 12> right_keys{
      "1",          "01",   "0001", "alpha", "ALPHA", std::nullopt,
      std::nullopt, "beta", "beta", "same",  "same",  std::nullopt,
  };
  struct Join {
    std::string_view sql;
    bool preserve_left = false;
    bool preserve_right = false;
  };
  const std::array joins{
      Join{.sql = "JOIN"},
      Join{.sql = "LEFT JOIN", .preserve_left = true},
      Join{.sql = "RIGHT JOIN", .preserve_right = true},
      Join{.sql = "FULL JOIN", .preserve_left = true, .preserve_right = true},
  };
  const auto matches = [](std::optional<std::string_view> left,
                          std::optional<std::string_view> right, std::string_view operation) {
    if (!left.has_value() || !right.has_value()) {
      return operation == " IS " && !left.has_value() && !right.has_value();
    }
    const auto fold = [](char character) {
      return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                  : character;
    };
    const auto order = std::lexicographical_compare_three_way(
        left->begin(), left->end(), right->begin(), right->end(),
        [&](char first, char second) { return fold(first) <=> fold(second); });
    if (operation == "=" || operation == " IS ") {
      return order == 0;
    }
    if (operation == ">") {
      return order > 0;
    }
    if (operation == ">=") {
      return order >= 0;
    }
    if (operation == "<") {
      return order < 0;
    }
    if (operation == "<=") {
      return order <= 0;
    }
    throw std::runtime_error{"unknown independent joined-index model operation"};
  };
  using Pair = std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>;
  constexpr std::array<std::string_view, 12> kPayloads{
      "one",      "zero-one",  "zeros-one", "alpha",    "upper-alpha", "null-one",
      "null-two", "beta-null", "beta-five", "same-one", "same-two",    "null-three",
  };
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  for (const bool noncovering : {false, true}) {
    SCOPED_TRACE(noncovering);
    for (const Join& join : joins) {
      for (const std::string_view operation : {"=", " IS ", ">", ">=", "<", "<="}) {
        ReadStatement statement = PrepareStatement(
            session,
            "SELECT p.id,b.id" + std::string{noncovering ? ",b.payload" : ""} +
                " FROM probe_rows p " + std::string{join.sql} + " key_rows b ON b.text_key" +
                std::string{operation} +
                "p.text_key AND b.id>?1 WHERE p.id IS NULL OR p.id<=?2 ORDER BY p.id,b.id");
        for (const std::int64_t floor : {0, 4, 12}) {
          for (const std::int64_t cutoff : {0, 2, 6}) {
            SCOPED_TRACE(::testing::Message() << join.sql << " operation=" << operation
                                              << " floor=" << floor << " cutoff=" << cutoff);
            std::vector<Pair> expected;
            std::array<bool, right_keys.size()> matched_right{};
            for (std::size_t left = 0; left < left_keys.size(); ++left) {
              const auto left_id = static_cast<std::int64_t>(left + 1U);
              bool matched_left = false;
              for (std::size_t right = 0; right < right_keys.size(); ++right) {
                const auto right_id = static_cast<std::int64_t>(right + 1U);
                if (right_id > floor && matches(right_keys[right], left_keys[left], operation)) {
                  matched_left = true;
                  matched_right[right] = true;
                  if (left_id <= cutoff) {
                    expected.emplace_back(left_id, right_id);
                  }
                }
              }
              if (join.preserve_left && !matched_left && left_id <= cutoff) {
                expected.emplace_back(left_id, std::nullopt);
              }
            }
            if (join.preserve_right) {
              for (std::size_t right = 0; right < right_keys.size(); ++right) {
                if (!matched_right[right]) {
                  expected.emplace_back(std::nullopt, static_cast<std::int64_t>(right + 1U));
                }
              }
            }
            std::ranges::sort(expected);
            RequireStatus(statement.Bind(1, SqlValue::Integer(floor)));
            RequireStatus(statement.Bind(2, SqlValue::Integer(cutoff)));
            for (const Pair& row : expected) {
              ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
              ASSERT_EQ(noncovering ? 3U : 2U, statement.row().size());
              EXPECT_EQ(row.first.has_value() ? SqlValueType::kInteger : SqlValueType::kNull,
                        statement.row()[0].type());
              EXPECT_EQ(row.second.has_value() ? SqlValueType::kInteger : SqlValueType::kNull,
                        statement.row()[1].type());
              EXPECT_EQ(row.first, statement.row()[0].integer_value());
              EXPECT_EQ(row.second, statement.row()[1].integer_value());
              if (noncovering) {
                if (row.second.has_value()) {
                  EXPECT_EQ(kPayloads[static_cast<std::size_t>(*row.second - 1)],
                            TextValue(statement.row()[2]));
                } else {
                  EXPECT_EQ(SqlValueType::kNull, statement.row()[2].type());
                }
              }
            }
            EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
            RequireStatus(statement.Reset());
          }
        }
      }
    }
  }
}

TEST(ReadSession, RejectsNullKeysFromUpperOnlyIndexRanges) {
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session, "SELECT count(*) FROM key_rows WHERE numeric_key<3");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(1U, statement.row().size());
  EXPECT_EQ(4, statement.row()[0].integer_value());
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, RebindsUpperOnlyRangesAcrossNullablePrefixesAndSuspension) {
  const std::array<std::optional<std::int64_t>, 12> numeric_keys{
      1, 1, 1, 2, 3, std::nullopt, 4, std::nullopt, 5, 6, 6, std::nullopt,
  };
  struct Prefix {
    std::string_view predicate;
    bool is_null_prefix = false;
  };
  const std::array prefixes{
      Prefix{.predicate = ""},
      Prefix{.predicate = "default_text='fallback' AND "},
      Prefix{.predicate = "text_key IS NULL AND ", .is_null_prefix = true},
  };
  struct Key {
    SqlValue value;
    std::optional<std::int64_t> upper;
  };
  const std::array keys{
      Key{.value = SqlValue{}, .upper = std::nullopt},
      Key{.value = SqlValue::Integer(3), .upper = 3},
      Key{.value = SqlValue::Text("3"), .upper = 3},
      Key{.value = SqlValue::Integer(5), .upper = 5},
      Key{.value = SqlValue::Integer(0), .upper = 0},
  };
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  for (const bool noncovering : {false, true}) {
    for (const Prefix& prefix : prefixes) {
      SCOPED_TRACE(prefix.predicate);
      SCOPED_TRACE(noncovering);
      ReadStatement statement =
          PrepareStatement(session, "SELECT id" + std::string{noncovering ? ",payload" : ""} +
                                        " FROM key_rows WHERE " + std::string{prefix.predicate} +
                                        "numeric_key<?1 ORDER BY id");
      for (const bool complete : {false, true}) {
        for (const Key& key : keys) {
          std::vector<std::int64_t> expected;
          for (std::size_t index = 0; index < numeric_keys.size(); ++index) {
            if (key.upper.has_value() && numeric_keys[index].has_value() &&
                *numeric_keys[index] < *key.upper &&
                (!prefix.is_null_prefix || index == 5U || index == 6U || index == 11U)) {
              expected.push_back(static_cast<std::int64_t>(index + 1U));
            }
          }
          RequireStatus(statement.Bind(1, key.value));
          for (const std::int64_t id : expected) {
            ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
            EXPECT_EQ(id, IntegerValue(statement.row()[0]));
            if (noncovering) {
              ASSERT_EQ(2U, statement.row().size());
              EXPECT_EQ(SqlValueType::kText, statement.row()[1].type());
            }
            if (!complete) {
              break;
            }
          }
          if (complete || expected.empty()) {
            EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
          }
          RequireStatus(statement.Reset());
        }
      }
    }
  }
}

TEST(ReadSession, ExcludesRangeNullsFromGroupedHavingAndIndexedExtrema) {
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  for (const std::string_view sql :
       {"SELECT numeric_key,count(*) FROM key_rows WHERE numeric_key<3 "
        "GROUP BY numeric_key ORDER BY numeric_key",
        "SELECT numeric_key,count(*) FROM key_rows GROUP BY numeric_key "
        "HAVING numeric_key<3 ORDER BY numeric_key"}) {
    SCOPED_TRACE(sql);
    ReadStatement grouped = PrepareStatement(session, sql);
    ASSERT_EQ(ReadStep::kRow, TakeValue(grouped.Step()));
    EXPECT_EQ(1, IntegerValue(grouped.row()[0]));
    EXPECT_EQ(3, IntegerValue(grouped.row()[1]));
    ASSERT_EQ(ReadStep::kRow, TakeValue(grouped.Step()));
    EXPECT_EQ(2, IntegerValue(grouped.row()[0]));
    EXPECT_EQ(1, IntegerValue(grouped.row()[1]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(grouped.Step()));
  }
  for (const std::string_view prefix : {"", "default_text='fallback' AND "}) {
    for (const bool maximum : {false, true}) {
      const std::string sql = "SELECT " + std::string{maximum ? "max" : "min"} +
                              "(numeric_key) FROM key_rows WHERE " + std::string{prefix} +
                              "numeric_key<3";
      SCOPED_TRACE(sql);
      ReadStatement extremum = PrepareStatement(session, sql);
      ASSERT_EQ(ReadStep::kRow, TakeValue(extremum.Step()));
      EXPECT_EQ(maximum ? 2 : 1, IntegerValue(extremum.row()[0]));
      EXPECT_EQ(ReadStep::kDone, TakeValue(extremum.Step()));
    }
  }
  ReadStatement empty = PrepareStatement(session,
                                         "SELECT min(numeric_key),payload FROM key_rows "
                                         "WHERE numeric_key<0");
  ASSERT_EQ(ReadStep::kRow, TakeValue(empty.Step()));
  ASSERT_EQ(2U, empty.row().size());
  EXPECT_EQ(SqlValueType::kNull, empty.row()[0].type());
  EXPECT_EQ(SqlValueType::kNull, empty.row()[1].type());
  EXPECT_EQ(ReadStep::kDone, TakeValue(empty.Step()));
}

TEST(ReadSession, RebindsJoinedRangesAcrossNullContradictoryAndTypedKeys) {
  ReadSession session = TakeValue(ReadSession::Open(JoinedIndexFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT b.id,b.payload,typeof(b.real_key),b.real_key FROM probe_rows p "
                       "LEFT JOIN key_rows b ON b.numeric_key>=?1 AND b.numeric_key<?2 "
                       "WHERE p.id=2 ORDER BY b.id");
  struct Case {
    SqlValue lower;
    SqlValue upper;
    std::vector<std::int64_t> ids;
  };
  const std::array cases{
      Case{.lower = SqlValue::Integer(1), .upper = SqlValue::Integer(3), .ids = {1, 2, 3, 4}},
      Case{.lower = SqlValue{}, .upper = SqlValue::Integer(3), .ids = {}},
      Case{.lower = SqlValue::Integer(3), .upper = SqlValue::Integer(3), .ids = {}},
      Case{.lower = SqlValue::Real(2.5), .upper = SqlValue::Text("5"), .ids = {5, 7}},
      Case{.lower = SqlValue::Text("2"), .upper = SqlValue::Real(5.0), .ids = {4, 5, 7}},
      Case{.lower = SqlValue::Integer(1), .upper = SqlValue{}, .ids = {}},
      Case{.lower = SqlValue::Integer(99), .upper = SqlValue::Integer(100), .ids = {}},
      Case{.lower = SqlValue::Integer(1), .upper = SqlValue::Integer(3), .ids = {1, 2, 3, 4}},
  };
  for (const Case& test_case : cases) {
    RequireStatus(statement.Bind(1, test_case.lower));
    RequireStatus(statement.Bind(2, test_case.upper));
    if (test_case.ids.empty()) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      ASSERT_EQ(4U, statement.row().size());
      EXPECT_EQ(SqlValueType::kNull, statement.row()[0].type());
      EXPECT_EQ(SqlValueType::kNull, statement.row()[1].type());
      EXPECT_EQ("null", TextValue(statement.row()[2]));
      EXPECT_EQ(SqlValueType::kNull, statement.row()[3].type());
    } else {
      for (const std::int64_t id : test_case.ids) {
        ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
        ASSERT_EQ(4U, statement.row().size());
        EXPECT_EQ(id, IntegerValue(statement.row()[0]));
        EXPECT_EQ(SqlValueType::kText, statement.row()[1].type());
        EXPECT_EQ("real", TextValue(statement.row()[2]));
        EXPECT_EQ(SqlValueType::kReal, statement.row()[3].type());
      }
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Reset());
  }
}

TEST(ReadSession, DefersJoinedIndexReadsForZeroLimitAndRecoversTypedReadFailure) {
  for (const std::string_view predicate :
       {"b.text_key=p.text_key", "b.text_key>=p.text_key AND b.text_key<=p.text_key",
        "b.text_key<=p.text_key AND +b.id>=4 AND +b.id<=5"}) {
    SCOPED_TRACE(predicate);
    const auto state = std::make_shared<TemporaryFileState>();
    ReadSession session = TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                                      JoinedIndexFixturePath().string()));
    ReadStatement root =
        PrepareStatement(session, "SELECT rootpage FROM sqlite_schema WHERE name='keys_text_num'");
    ASSERT_EQ(ReadStep::kRow, TakeValue(root.Step()));
    const auto page = IntegerValue(root.row()[0]);
    ASSERT_GT(page, 0);
    RequireStatus(root.Finalize());
    ReadStatement statement = PrepareStatement(
        session, "SELECT p.id,b.id FROM probe_rows p JOIN key_rows b ON " + std::string{predicate} +
                     " WHERE p.id=2 ORDER BY b.id+0 LIMIT ?1");
    RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
    state->main_read_failure_offset = FileOffset{(static_cast<std::uint64_t>(page) - 1U) * 512U};
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    EXPECT_EQ(0U, state->main_read_failures);
    ASSERT_TRUE(state->main_read_failure_offset.has_value());
    RequireStatus(statement.Reset());
    RequireStatus(statement.Bind(1, SqlValue::Integer(-1)));
    const auto failed = statement.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_EQ(1U, state->main_read_failures);
    EXPECT_EQ(0U, state->live_count);
    const auto reset = statement.Reset();
    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(ErrorCode::kIo, reset.error().code());
    for (const std::int64_t id : {4, 5}) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(2, IntegerValue(statement.row()[0]));
      EXPECT_EQ(id, IntegerValue(statement.row()[1]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Reset());
    RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(4, IntegerValue(statement.row()[1]));
    RequireStatus(statement.Finalize());
    EXPECT_EQ(0U, state->live_count);
  }
}

TEST(ReadSession, BypassesBothNoncoveringRootsForZeroLimitAndRecoversEitherReadFailure) {
  for (const std::string_view predicate :
       {"b.text_key=p.text_key", "b.text_key>=p.text_key AND b.text_key<=p.text_key"}) {
    for (const std::string_view object : {"keys_text_num", "key_rows"}) {
      for (const std::int64_t probe : {2, 5}) {
        SCOPED_TRACE(::testing::Message() << predicate << " " << object << " probe=" << probe);
        const auto state = std::make_shared<TemporaryFileState>();
        ReadSession session = TakeValue(ReadSession::Open(
            std::make_unique<TemporaryTrackingVfs>(state), JoinedIndexFixturePath().string()));
        ReadStatement root = PrepareStatement(
            session, "SELECT rootpage FROM sqlite_schema WHERE name='" + std::string{object} + "'");
        ASSERT_EQ(ReadStep::kRow, TakeValue(root.Step()));
        const auto page = IntegerValue(root.row()[0]);
        ASSERT_GT(page, 0);
        RequireStatus(root.Finalize());
        ReadStatement statement = PrepareStatement(
            session,
            "SELECT p.id,b.id,b.payload,b.default_text,b.real_key FROM probe_rows p "
            "JOIN key_rows b ON " +
                std::string{predicate} + " WHERE p.id=" + std::to_string(probe) + " LIMIT ?1");
        RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
        state->main_read_failure_offset =
            FileOffset{(static_cast<std::uint64_t>(page) - 1U) * 512U};
        EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
        EXPECT_EQ(0U, state->main_read_failures);
        ASSERT_TRUE(state->main_read_failure_offset.has_value());
        RequireStatus(statement.Reset());
        RequireStatus(statement.Bind(1, SqlValue::Integer(-1)));
        const auto failed = statement.Step();
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(ErrorCode::kIo, failed.error().code());
        EXPECT_EQ(1U, state->main_read_failures);
        EXPECT_EQ(0U, state->live_count);
        const auto reset = statement.Reset();
        ASSERT_FALSE(reset.has_value());
        EXPECT_EQ(ErrorCode::kIo, reset.error().code());
        if (probe == 2) {
          for (const std::int64_t id : {4, 5}) {
            ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
            ASSERT_EQ(5U, statement.row().size());
            EXPECT_EQ(2, IntegerValue(statement.row()[0]));
            EXPECT_EQ(id, IntegerValue(statement.row()[1]));
            EXPECT_EQ(id == 4 ? "alpha" : "upper-alpha", TextValue(statement.row()[2]));
            EXPECT_EQ("fallback", TextValue(statement.row()[3]));
            EXPECT_EQ(SqlValueType::kReal, statement.row()[4].type());
            EXPECT_EQ(id == 4 ? 2.5 : 3.5, statement.row()[4].real_value());
          }
        }
        EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
        RequireStatus(statement.Reset());
        RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
        if (probe == 2) {
          ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
          EXPECT_EQ(4, IntegerValue(statement.row()[1]));
        } else {
          EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
        }
        RequireStatus(statement.Finalize());
        EXPECT_EQ(0U, state->live_count);
      }
    }
  }
}

TEST(ReadSession, RetainsJoinedScanWhenMemoryTemporaryBudgetIsOneByte) {
  const ReadSessionOptions options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kMemory,
              .sorter_memory_threshold = ByteCount{1},
          },
  };
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string(), options));
  ReadStatement statement = PrepareStatement(session,
                                             "SELECT count(b.id) FROM items a JOIN items b "
                                             "ON b.payload=a.payload WHERE a.id<=32");
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(32, IntegerValue(statement.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, ExecutesSixtyFourJoinedCoveringPrefixesInOneSourcePipeline) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  std::string sql = "SELECT t0.id,t63.id FROM items t0";
  for (std::size_t source = 1; source < 64U; ++source) {
    sql += " JOIN items t" + std::to_string(source) + " ON t" + std::to_string(source) +
           ".category=t" + std::to_string(source - 1U) + ".category";
  }
  sql += " WHERE t0.id=1";
  ReadStatement statement = PrepareStatement(session, sql);
  ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(2U, statement.row().size());
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ(1, IntegerValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
TEST(ReadSession, RecordsOuterExpressionAliasFixedWorkBaseline) {
  for (const bool source_value : {false, true}) {
    for (const std::int64_t outer_rows : {1, 8, 32}) {
      SCOPED_TRACE(source_value);
      SCOPED_TRACE(outer_rows);
      ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
      const std::string definition = source_value ? "o.id+1" : "1";
      ReadStatement statement =
          PrepareStatement(session, "SELECT " + definition +
                                        " AS x FROM model_rows o "
                                        "WHERE o.id<=?1 AND (SELECT (SELECT x))>0 ORDER BY o.id");
      RequireStatus(statement.Bind(1, SqlValue::Integer(outer_rows)));
      instrumentation::CounterCollection work;
      {
        const instrumentation::ScopedCounterCollection scope{work};
        for (std::int64_t id = 1; id <= outer_rows; ++id) {
          ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
          ASSERT_EQ(1U, statement.row().size());
          EXPECT_EQ(source_value ? id + 1 : 1, IntegerValue(statement.row()[0]));
        }
        EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
      }
      const std::string prefix =
          std::string{source_value ? "source_" : "constant_"} + std::to_string(outer_rows) + "_";
      for (const auto counter :
           {instrumentation::Counter::kVmInstructions, instrumentation::Counter::kPagesRead,
            instrumentation::Counter::kBytesCopied}) {
        RecordProperty(prefix + std::string{instrumentation::CounterName(counter)},
                       std::to_string(work.Value(counter)));
      }
      EXPECT_GT(work.Value(instrumentation::Counter::kVmInstructions), 0U);
    }
  }
}

TEST(ReadSession, RecordsSourceCorrelatedScalarAndExistsFixedWorkBaseline) {
  struct Case {
    std::string_view name;
    std::string_view projection;
    bool grouped = false;
  };
  for (const Case& test_case :
       {Case{.name = "transitive", .projection = "(SELECT (SELECT o.id)),EXISTS(SELECT o.label)"},
        Case{.name = "point",
             .projection = "(SELECT i.id FROM model_rows i WHERE i.id=o.id),"
                           "EXISTS(SELECT 1 FROM model_rows i WHERE i.id=o.id)"},
        Case{.name = "grouped",
             .projection = "(SELECT sum(i.id) FROM model_rows i WHERE i.id<=o.id "
                           "GROUP BY i.id%2 ORDER BY 1 DESC),"
                           "EXISTS(SELECT 1 FROM model_rows i WHERE i.id>o.id)",
             .grouped = true}}) {
    for (const std::int64_t outer_rows : {1, 8, 32}) {
      SCOPED_TRACE(test_case.name);
      SCOPED_TRACE(outer_rows);
      ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
      ReadStatement statement =
          PrepareStatement(session, "SELECT o.id," + std::string{test_case.projection} +
                                        " FROM model_rows o WHERE o.id<=?1 ORDER BY o.id");
      RequireStatus(statement.Bind(1, SqlValue::Integer(outer_rows)));
      instrumentation::CounterCollection work;
      {
        const instrumentation::ScopedCounterCollection scope{work};
        for (std::int64_t id = 1; id <= outer_rows; ++id) {
          ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
          ASSERT_EQ(3U, statement.row().size());
          EXPECT_EQ(id, IntegerValue(statement.row()[0]));
          EXPECT_EQ(test_case.grouped ? (id + 1) * (id + 1) / 4 : id,
                    IntegerValue(statement.row()[1]));
          EXPECT_EQ(test_case.grouped && id == 32 ? 0 : 1, IntegerValue(statement.row()[2]));
        }
        EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
      }
      const std::string prefix =
          std::string{test_case.name} + "_" + std::to_string(outer_rows) + "_";
      for (const auto counter :
           {instrumentation::Counter::kVmInstructions, instrumentation::Counter::kPagesRead,
            instrumentation::Counter::kBytesCopied}) {
        RecordProperty(prefix + std::string{instrumentation::CounterName(counter)},
                       std::to_string(work.Value(counter)));
      }
      EXPECT_GT(work.Value(instrumentation::Counter::kVmInstructions), 0U);
    }
  }
}

TEST(ReadSession, BoundsJoinedCoveringEqualityWorkByIndexedPrefixes) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session, "SELECT count() FROM items a JOIN items b ON b.category=a.category WHERE a.id=1");
  instrumentation::CounterCollection work;
  {
    const instrumentation::ScopedCounterCollection scope{work};
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(1, IntegerValue(statement.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  const std::uint64_t instructions = work.Value(instrumentation::Counter::kVmInstructions);
  RecordProperty("vm_instructions", std::to_string(instructions));
  EXPECT_LT(instructions, 256U);
}

TEST(ReadSession, BoundsJoinedNoncoveringEqualityWorkByIndexedPrefixes) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session,
      "SELECT count(b.payload) FROM items a JOIN items b ON b.category=a.category WHERE a.id=1");
  instrumentation::CounterCollection work;
  {
    const instrumentation::ScopedCounterCollection scope{work};
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(1, IntegerValue(statement.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  const std::uint64_t instructions = work.Value(instrumentation::Counter::kVmInstructions);
  RecordProperty("vm_instructions", std::to_string(instructions));
  EXPECT_LT(instructions, 256U);
}

TEST(ReadSession, BoundsJoinedCoveringRangeWorkByIndexedIntervals) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT count(b.id) FROM items a JOIN items b "
                       "ON b.category>=a.category AND b.category<=a.category WHERE a.id=1");
  instrumentation::CounterCollection work;
  {
    const instrumentation::ScopedCounterCollection scope{work};
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(1, IntegerValue(statement.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  const std::uint64_t instructions = work.Value(instrumentation::Counter::kVmInstructions);
  RecordProperty("vm_instructions", std::to_string(instructions));
  EXPECT_LT(instructions, 256U);
}

TEST(ReadSession, BoundsJoinedNoncoveringRangeWorkByIndexedIntervals) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT count(b.payload) FROM items a JOIN items b "
                       "ON b.category>=a.category AND b.category<=a.category WHERE a.id=1");
  instrumentation::CounterCollection work;
  {
    const instrumentation::ScopedCounterCollection scope{work};
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(1, IntegerValue(statement.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  const std::uint64_t instructions = work.Value(instrumentation::Counter::kVmInstructions);
  RecordProperty("vm_instructions", std::to_string(instructions));
  EXPECT_LT(instructions, 256U);
}

TEST(ReadSession, BoundsJoinedRowIdWorkByItsParameterizedInterval) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session,
                       "SELECT count(b.payload) FROM items a CROSS JOIN items b "
                       "WHERE a.id=1 AND b.id>=a.id AND b.id<=a.id+1");
  instrumentation::CounterCollection work;
  {
    const instrumentation::ScopedCounterCollection scope{work};
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(2, IntegerValue(statement.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  const auto instructions = work.Value(instrumentation::Counter::kVmInstructions);
  RecordProperty("vm_instructions", std::to_string(instructions));
  EXPECT_LT(instructions, 256U);
}

TEST(ReadSession, BoundsJoinedEqualityWorkByPointPositionedPrefixes) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session, "SELECT count() FROM model_rows a JOIN model_rows b ON b.id=a.id");
  instrumentation::CounterCollection work;
  {
    const instrumentation::ScopedCounterCollection scope{work};
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(32, IntegerValue(statement.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  const std::uint64_t instructions = work.Value(instrumentation::Counter::kVmInstructions);
  RecordProperty("vm_instructions", std::to_string(instructions));
  EXPECT_LT(instructions, 1024U);
}

TEST(ReadSession, BoundsDeterministicFunctionJoinedRowidWorkByPointPrefixes) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session, "SELECT count() FROM model_rows a JOIN model_rows b ON b.id=abs(a.id)+1000");
  instrumentation::CounterCollection work;
  {
    const instrumentation::ScopedCounterCollection scope{work};
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(0, IntegerValue(statement.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  const std::uint64_t instructions = work.Value(instrumentation::Counter::kVmInstructions);
  RecordProperty("vm_instructions", std::to_string(instructions));
  EXPECT_LT(instructions, 1024U);
}

TEST(ReadSession, BoundsComputedFullJoinWorkByPointPositionedPrefixes) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement = PrepareStatement(
      session,
      "SELECT a.id,b.id,(SELECT coalesce(a.id,b.id)),(SELECT a.value),(SELECT b.nullable) "
      "FROM model_rows a FULL JOIN model_rows b "
      "ON b.id=a.id+?3 AND a.id<=?1 AND b.id<=?2 "
      "WHERE (a.id<=?1 OR a.id IS NULL) AND (b.id<=?2 OR b.id IS NULL) "
      "ORDER BY a.id IS NULL,a.id,b.id");
  RequireStatus(statement.Bind(1, SqlValue::Integer(8)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(8)));
  RequireStatus(statement.Bind(3, SqlValue::Integer(1)));
  constexpr std::array<std::array<std::optional<std::int64_t>, 5>, 9> kExpected{{
      {1, 2, 1, -31, 20},
      {2, 3, 2, -29, 30},
      {3, 4, 3, -23, std::nullopt},
      {4, 5, 4, -19, 50},
      {5, 6, 5, -17, 60},
      {6, 7, 6, -13, std::nullopt},
      {7, 8, 7, -11, 80},
      {8, std::nullopt, 8, -7, std::nullopt},
      {std::nullopt, 1, 1, std::nullopt, std::nullopt},
  }};
  instrumentation::CounterCollection work;
  {
    const instrumentation::ScopedCounterCollection scope{work};
    for (const auto& expected : kExpected) {
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      ASSERT_EQ(expected.size(), statement.row().size());
      for (std::size_t field = 0; field < expected.size(); ++field) {
        EXPECT_EQ(expected[field], statement.row()[field].integer_value());
        if (!expected[field].has_value()) {
          EXPECT_EQ(SqlValueType::kNull, statement.row()[field].type());
        }
      }
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  const auto instructions = work.Value(instrumentation::Counter::kVmInstructions);
  RecordProperty("vm_instructions", std::to_string(instructions));
  EXPECT_LT(instructions, 4096U);
}

TEST(ReadSession, RecordsUnindexedJoinedEqualityScalingBaseline) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  for (const std::int64_t outer_rows : {1, 8, 32}) {
    SCOPED_TRACE(outer_rows);
    const std::string predicate =
        outer_rows == 1 ? "a.id=1" : "a.id<=" + std::to_string(outer_rows);
    ReadStatement statement = PrepareStatement(session,
                                               "SELECT count(b.id) FROM items a JOIN items b "
                                               "ON b.payload=a.payload WHERE " +
                                                   predicate);
    instrumentation::CounterCollection work;
    {
      const instrumentation::ScopedCounterCollection scope{work};
      ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(outer_rows, IntegerValue(statement.row()[0]));
      EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    }
    RecordProperty("vm_instructions_" + std::to_string(outer_rows),
                   std::to_string(work.Value(instrumentation::Counter::kVmInstructions)));
  }
}

TEST(ReadSession, BoundsAutomaticJoinedEqualityWorkByOneMaterialization) {
  ReadSession session = TakeValue(ReadSession::Open(IndexFixturePath().string()));
  ReadStatement statement = PrepareStatement(session,
                                             "SELECT count(b.id) FROM items a JOIN items b "
                                             "ON b.payload=a.payload WHERE a.id<=32");
  instrumentation::CounterCollection work;
  {
    const instrumentation::ScopedCounterCollection scope{work};
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(32, IntegerValue(statement.row()[0]));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  const std::uint64_t instructions = work.Value(instrumentation::Counter::kVmInstructions);
  RecordProperty("vm_instructions", std::to_string(instructions));
  EXPECT_LT(instructions, 100000U);
}

TEST(ReadSession, CapturesAggregateLifecycleAndSharedTemporaryPeaks) {
  using instrumentation::AggregateWorkMetric;
  using instrumentation::ProbeCollectionKind;
  using instrumentation::ProbeCounterCollection;
  using instrumentation::ScopedProbeCounterCollection;
  constexpr ByteCount kBudget{64U * 1024U};
  const ReadSessionOptions options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kMemory,
              .sorter_memory_threshold = kBudget,
          },
  };
  ReadSession session =
      TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string(), options));
  ReadStatement statement = PrepareStatement(
      session,
      "SELECT nullable IS NULL,count(),group_concat(label ORDER BY id DESC),"
      "count(DISTINCT value<0) FROM model_rows GROUP BY nullable IS NULL ORDER BY 1");
  ProbeCounterCollection work{ProbeCollectionKind::AggregateWork};
  {
    const ScopedProbeCounterCollection scope{work};
    EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  EXPECT_EQ(3U, work.Value(AggregateWorkMetric::Open));
  EXPECT_EQ(68U, work.Value(AggregateWorkMetric::Step));
  EXPECT_EQ(6U, work.Value(AggregateWorkMetric::Finalize));
  EXPECT_EQ(3U, work.Value(AggregateWorkMetric::Reset));
  EXPECT_EQ(3U, work.Value(AggregateWorkMetric::Close));
  EXPECT_GT(work.Value(AggregateWorkMetric::RetainedBytesPeak), 0U);
  EXPECT_LE(work.Value(AggregateWorkMetric::RetainedBytesPeak), kBudget.value());
  EXPECT_EQ(0U, work.Value(AggregateWorkMetric::LiveFilesPeak));
  EXPECT_EQ(0U, work.Value(AggregateWorkMetric::FileAcquisitions));
}

TEST(ReadSession, CapturesActualSharedSorterSpillWrites) {
  using instrumentation::AggregateWorkMetric;
  using instrumentation::ProbeCollectionKind;
  using instrumentation::ProbeCounterCollection;
  using instrumentation::ScopedProbeCounterCollection;
  const ReadSessionOptions options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kFile,
              .sorter_memory_threshold = ByteCount{2048},
          },
  };
  ReadSession session =
      TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string(), options));
  ReadStatement statement = PrepareStatement(session, kSpillingAggregateCompoundSql);
  ProbeCounterCollection work{ProbeCollectionKind::AggregateWork};
  {
    const ScopedProbeCounterCollection scope{work};
    EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  }
  EXPECT_GT(work.Value(AggregateWorkMetric::SorterSpillWrites), 0U);
  EXPECT_GT(work.Value(AggregateWorkMetric::LiveFilesPeak), 0U);
  EXPECT_LE(work.Value(AggregateWorkMetric::LiveFilesPeak), 2U);
}
#endif

TEST(ReadSession, CleansSpilledAggregateCompoundFilesOnEarlyStop) {
  constexpr ByteCount kFileBudget{2048};
  const auto state = std::make_shared<TemporaryFileState>();
  const ReadSessionOptions options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kFile,
              .sorter_memory_threshold = kFileBudget,
          },
  };
  ReadSession session =
      TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                  ReadCompatibilityFixturePath().string(), options));
  ReadStatement statement = PrepareStatement(session, kSpillingAggregateCompoundSql);

  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(21, IntegerValue(statement.row()[0]));
  EXPECT_GT(state->open_count, 0U);
  EXPECT_LE(state->peak_live_count, 2U);
  RequireStatus(statement.Reset());
  EXPECT_EQ(0U, state->live_count);

  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(21, IntegerValue(statement.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  EXPECT_EQ(0U, state->live_count);
  RequireStatus(statement.Finalize());
}

TEST(ReadSession, RecoversAggregateCompoundAfterInjectedSpillFailure) {
  constexpr ByteCount kFileBudget{2048};
  const auto state = std::make_shared<TemporaryFileState>();
  const ReadSessionOptions options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kFile,
              .sorter_memory_threshold = kFileBudget,
          },
  };
  ReadSession session =
      TakeValue(ReadSession::Open(std::make_unique<TemporaryTrackingVfs>(state),
                                  ReadCompatibilityFixturePath().string(), options));
  ReadStatement statement = PrepareStatement(session, kSpillingAggregateCompoundSql);
  state->writes_until_failure = 0U;

  const auto failed = statement.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_GT(state->open_count, 0U);
  EXPECT_LE(state->peak_live_count, 2U);
  EXPECT_EQ(0U, state->live_count);

  const Status reset = statement.Reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(ErrorCode::kIo, reset.error().code());
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(21, IntegerValue(statement.row()[0]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  EXPECT_EQ(0U, state->live_count);
  RequireStatus(statement.Finalize());
}

TEST(ReadSession, RebindsNumericTextAggregatesWithoutChangingInputTypes) {
  ReadSession session = TakeValue(ReadSession::Open(ReadCompatibilityFixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session, "SELECT sum(?1),typeof(?1),total(?1),avg(?1),?1");
  struct Case {
    std::string_view bytes;
    SqlValueType type;
    std::int64_t integer = 0;
    double real = 0.0;
    bool blob = false;
  };
  const std::array cases{
      Case{.bytes = std::string_view{"1\0e2", 4},
           .type = SqlValueType::kInteger,
           .integer = 1,
           .real = 1.0},
      Case{.bytes = std::string_view{"9007199254740992\0x", 18},
           .type = SqlValueType::kReal,
           .real = 9007199254740992.0},
      Case{.bytes = std::string_view{"-2251799813685248\0x", 19},
           .type = SqlValueType::kInteger,
           .integer = -2251799813685248,
           .real = -2251799813685248.0},
      Case{.bytes = std::string_view{"2251799813685248\0x", 18},
           .type = SqlValueType::kReal,
           .real = 2251799813685248.0},
      Case{.bytes = std::string_view{"1.0\0x", 5}, .type = SqlValueType::kReal, .real = 1.0},
      Case{.bytes = std::string_view{"1e\0x", 4}, .type = SqlValueType::kReal, .real = 1.0},
      Case{.bytes = {}, .type = SqlValueType::kReal},
      Case{.bytes = std::string_view{"1\0e2", 4},
           .type = SqlValueType::kReal,
           .real = 1.0,
           .blob = true},
  };
  for (const Case& test : cases) {
    SCOPED_TRACE(test.bytes);
    RequireStatus(statement.Bind(1, test.blob
                                        ? SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(test.bytes)))
                                        : SqlValue::Text(std::string{test.bytes})));
    EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    ASSERT_EQ(5U, statement.row().size());
    EXPECT_EQ(test.type, statement.row()[0].type());
    if (test.type == SqlValueType::kInteger) {
      EXPECT_EQ(test.integer, statement.row()[0].integer_value());
    } else {
      EXPECT_EQ(test.real, statement.row()[0].real_value());
    }
    EXPECT_EQ(test.blob ? "blob" : "text", TextValue(statement.row()[1]));
    EXPECT_EQ(test.real, statement.row()[2].real_value());
    EXPECT_EQ(test.real, statement.row()[3].real_value());
    if (test.blob) {
      ASSERT_TRUE(statement.row()[4].blob_value().has_value());
      EXPECT_EQ(test.bytes, AsStringView(*statement.row()[4].blob_value()));
    } else {
      EXPECT_EQ(test.bytes, TextValue(statement.row()[4]));
    }
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Reset());
  }
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1.0, statement.row()[0].real_value());
  RequireStatus(statement.Finalize());
}

TEST(ReadSession, ExecutesAndResetsGlobalCount) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement statement = PrepareStatement(session, "SELECT count() FROM items");

  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(1U, statement.row().size());
  EXPECT_EQ(3, IntegerValue(statement.row().front()));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));

  RequireStatus(statement.Reset());
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(3, IntegerValue(statement.row().front()));
}

TEST(ReadSession, ExecutesGlobalAggregateFunctionsEmptyHavingAndSkipping) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement functions = PrepareStatement(
      session, "SELECT count(),sum(id),total(id),avg(id),min(name),max(name) FROM items");
  EXPECT_EQ(ReadStep::kRow, TakeValue(functions.Step()));
  ASSERT_EQ(6U, functions.row().size());
  EXPECT_EQ(3, IntegerValue(functions.row()[0]));
  EXPECT_EQ(6, IntegerValue(functions.row()[1]));
  EXPECT_EQ(6.0, functions.row()[2].real_value());
  EXPECT_EQ(2.0, functions.row()[3].real_value());
  EXPECT_EQ("alpha", TextValue(functions.row()[4]));
  EXPECT_EQ("gamma", TextValue(functions.row()[5]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(functions.Step()));

  ReadStatement empty = PrepareStatement(
      session, "SELECT count(),sum(id),total(id),avg(id),min(name) FROM items WHERE 0");
  EXPECT_EQ(ReadStep::kRow, TakeValue(empty.Step()));
  ASSERT_EQ(5U, empty.row().size());
  EXPECT_EQ(0, IntegerValue(empty.row()[0]));
  EXPECT_EQ(SqlValueType::kNull, empty.row()[1].type());
  EXPECT_EQ(0.0, empty.row()[2].real_value());
  EXPECT_EQ(SqlValueType::kNull, empty.row()[3].type());
  EXPECT_EQ(SqlValueType::kNull, empty.row()[4].type());

  ReadStatement representative =
      PrepareStatement(session, "SELECT id,min(score),max(score) FROM items");
  EXPECT_EQ(ReadStep::kRow, TakeValue(representative.Step()));
  EXPECT_EQ(2, IntegerValue(representative.row()[0]));
  EXPECT_EQ(1.5, representative.row()[1].real_value());
  EXPECT_EQ(3.0, representative.row()[2].real_value());

  ReadStatement source_free = PrepareStatement(session, "SELECT count(),sum(2)");
  EXPECT_EQ(ReadStep::kRow, TakeValue(source_free.Step()));
  EXPECT_EQ(1, IntegerValue(source_free.row()[0]));
  EXPECT_EQ(2, IntegerValue(source_free.row()[1]));

  ReadStatement having = PrepareStatement(session, "SELECT count() FROM items HAVING count()>2");
  EXPECT_EQ(ReadStep::kRow, TakeValue(having.Step()));
  EXPECT_EQ(3, IntegerValue(having.row().front()));
  ReadStatement rejected = PrepareStatement(session, "SELECT count() FROM items HAVING count()>3");
  EXPECT_EQ(ReadStep::kDone, TakeValue(rejected.Step()));

  ReadStatement zero = PrepareStatement(session, "SELECT count() FROM items LIMIT 0 OFFSET ?1");
  RequireStatus(zero.Bind(1, SqlValue::Text("not evaluated")));
  EXPECT_EQ(ReadStep::kDone, TakeValue(zero.Step()));

  ReadStatement ignored_order =
      PrepareStatement(session, "SELECT count() FROM items ORDER BY abs(-9223372036854775808)");
  EXPECT_EQ(ReadStep::kRow, TakeValue(ignored_order.Step()));
  EXPECT_EQ(3, IntegerValue(ignored_order.row().front()));

  ReadStatement order_only_leaf =
      PrepareStatement(session, "SELECT count() FROM items ORDER BY name");
  EXPECT_EQ(ReadStep::kRow, TakeValue(order_only_leaf.Step()));
  EXPECT_EQ(3, IntegerValue(order_only_leaf.row().front()));

  ReadStatement rowid = PrepareStatement(session, "SELECT count() FROM items WHERE rowid=?1");
  RequireStatus(rowid.Bind(1, SqlValue::Integer(2)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(rowid.Step()));
  EXPECT_EQ(1, IntegerValue(rowid.row().front()));

  ReadStatement covering = PrepareStatement(session, "SELECT sum(id) FROM items WHERE name=?1");
  RequireStatus(covering.Bind(1, SqlValue::Text("beta")));
  EXPECT_EQ(ReadStep::kRow, TakeValue(covering.Step()));
  EXPECT_EQ(2, IntegerValue(covering.row().front()));

  ReadStatement noncovering =
      PrepareStatement(session, "SELECT sum(score) FROM items WHERE name=?1");
  RequireStatus(noncovering.Bind(1, SqlValue::Text("beta")));
  EXPECT_EQ(ReadStep::kRow, TakeValue(noncovering.Step()));
  EXPECT_EQ(3.0, noncovering.row().front().real_value().value_or(-1.0));

  ReadStatement rowid_min = PrepareStatement(session, "SELECT min(id) FROM items");
  EXPECT_EQ(ReadStep::kRow, TakeValue(rowid_min.Step()));
  EXPECT_EQ(1, IntegerValue(rowid_min.row().front()));
  ReadStatement rowid_max = PrepareStatement(session, "SELECT max(_rowid_) FROM items");
  EXPECT_EQ(ReadStep::kRow, TakeValue(rowid_max.Step()));
  EXPECT_EQ(3, IntegerValue(rowid_max.row().front()));

  ReadStatement name_min = PrepareStatement(session, "SELECT min(name) FROM items");
  EXPECT_EQ(ReadStep::kRow, TakeValue(name_min.Step()));
  EXPECT_EQ("alpha", TextValue(name_min.row().front()));
  ReadStatement name_max = PrepareStatement(session, "SELECT max(name) FROM items");
  EXPECT_EQ(ReadStep::kRow, TakeValue(name_max.Step()));
  EXPECT_EQ("gamma", TextValue(name_max.row().front()));

  ReadStatement category_min = PrepareStatement(session, "SELECT min(category) FROM items");
  EXPECT_EQ(ReadStep::kRow, TakeValue(category_min.Step()));
  EXPECT_EQ("one", TextValue(category_min.row().front()));
  ReadStatement category_max = PrepareStatement(session, "SELECT max(category) FROM items");
  EXPECT_EQ(ReadStep::kRow, TakeValue(category_max.Step()));
  EXPECT_EQ("two", TextValue(category_max.row().front()));
  ReadStatement null_category =
      PrepareStatement(session, "SELECT min(category) FROM items WHERE category IS NULL");
  EXPECT_EQ(ReadStep::kRow, TakeValue(null_category.Step()));
  EXPECT_EQ(SqlValueType::kNull, null_category.row().front().type());
}

TEST(ReadSession, ExecutesIndexedMinMaxRangesResidualsAndNullFallback) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));

  ReadStatement rowid_min = PrepareStatement(session, "SELECT min(id) FROM items WHERE score>?1");
  RequireStatus(rowid_min.Bind(1, SqlValue::Integer(2)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(rowid_min.Step()));
  EXPECT_EQ(2, IntegerValue(rowid_min.row().front()));

  ReadStatement rowid_max = PrepareStatement(session, "SELECT max(id) FROM items WHERE score<?1");
  RequireStatus(rowid_max.Bind(1, SqlValue::Integer(2)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(rowid_max.Step()));
  EXPECT_EQ(1, IntegerValue(rowid_max.row().front()));

  ReadStatement noncovering =
      PrepareStatement(session, "SELECT min(category) FROM items WHERE score>?1");
  RequireStatus(noncovering.Bind(1, SqlValue::Integer(2)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(noncovering.Step()));
  EXPECT_EQ("two", TextValue(noncovering.row().front()));

  ReadStatement residual_null =
      PrepareStatement(session, "SELECT min(category) FROM items WHERE score IS NULL");
  EXPECT_EQ(ReadStep::kRow, TakeValue(residual_null.Step()));
  EXPECT_EQ(SqlValueType::kNull, residual_null.row().front().type());

  ReadStatement ranged = PrepareStatement(
      session, "SELECT min(category) FROM items WHERE category>=?1 AND category<?2");
  RequireStatus(ranged.Bind(1, SqlValue::Text("one")));
  RequireStatus(ranged.Bind(2, SqlValue::Text("zzz")));
  EXPECT_EQ(ReadStep::kRow, TakeValue(ranged.Step()));
  EXPECT_EQ("one", TextValue(ranged.row().front()));

  ReadStatement reverse_range =
      PrepareStatement(session, "SELECT max(category) FROM items WHERE category>?1");
  RequireStatus(reverse_range.Bind(1, SqlValue::Text("one")));
  EXPECT_EQ(ReadStep::kRow, TakeValue(reverse_range.Step()));
  EXPECT_EQ("two", TextValue(reverse_range.row().front()));

  ReadStatement fixed =
      PrepareStatement(session, "SELECT min(category) FROM items WHERE category=?1");
  RequireStatus(fixed.Bind(1, SqlValue::Text("two")));
  EXPECT_EQ(ReadStep::kRow, TakeValue(fixed.Step()));
  EXPECT_EQ("two", TextValue(fixed.row().front()));

  ReadStatement rejected_null =
      PrepareStatement(session, "SELECT min(category) FROM items WHERE category=?1");
  RequireStatus(rejected_null.Bind(1, SqlValue{}));
  EXPECT_EQ(ReadStep::kRow, TakeValue(rejected_null.Step()));
  EXPECT_EQ(SqlValueType::kNull, rejected_null.row().front().type());

  ReadStatement runtime_is =
      PrepareStatement(session, "SELECT min(category) FROM items WHERE category IS ?1");
  RequireStatus(runtime_is.Bind(1, SqlValue::Text("two")));
  EXPECT_EQ(ReadStep::kRow, TakeValue(runtime_is.Step()));
  EXPECT_EQ("two", TextValue(runtime_is.row().front()));
  RequireStatus(runtime_is.Reset());
  RequireStatus(runtime_is.Bind(1, SqlValue{}));
  EXPECT_EQ(ReadStep::kRow, TakeValue(runtime_is.Step()));
  EXPECT_EQ(SqlValueType::kNull, runtime_is.row().front().type());
}

TEST(ReadSession, ExecutesConstantsAfterThePublicSessionHandleIsDestroyed) {
  ReadStatement statement = [] {
    ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
    return PrepareStatement(session, "SELECT 1, 'constant'");
  }();

  EXPECT_TRUE(statement.valid());
  ASSERT_EQ(2U, statement.result_columns().size());
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(2U, statement.row().size());
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ("constant", TextValue(statement.row()[1]));
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
}

TEST(ReadSession, PublishesCompoundMetadataAcrossSetThenOrder) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  const ReadStatement ordinary = PrepareStatement(session,
                                                  "SELECT id,'left' FROM Items "
                                                  "UNION SELECT Score,Name FROM Items "
                                                  "ORDER BY 2");
  ASSERT_EQ(2U, ordinary.result_columns().size());
  EXPECT_EQ("id", ordinary.result_columns()[0].name);
  EXPECT_EQ("'left'", ordinary.result_columns()[1].name);
  EXPECT_EQ(std::optional<std::string>{"INTEGER"}, ordinary.result_columns()[0].declared_type);
  EXPECT_EQ(std::nullopt, ordinary.result_columns()[1].declared_type);
  EXPECT_EQ(TypeAffinity::kInteger, ordinary.result_columns()[0].affinity);
  EXPECT_EQ(TypeAffinity::kNone, ordinary.result_columns()[1].affinity);

  const ReadStatement set_then_order = PrepareStatement(session,
                                                        "SELECT id,'left' FROM Items "
                                                        "UNION SELECT Score,Name FROM Items "
                                                        "ORDER BY 2 COLLATE binary");
  ASSERT_EQ(2U, set_then_order.result_columns().size());
  EXPECT_EQ("id", set_then_order.result_columns()[0].name);
  EXPECT_EQ("'left'", set_then_order.result_columns()[1].name);
  EXPECT_EQ(std::optional<std::string>{"REAL"}, set_then_order.result_columns()[0].declared_type);
  EXPECT_EQ(std::optional<std::string>{"TEXT"}, set_then_order.result_columns()[1].declared_type);
  EXPECT_EQ(TypeAffinity::kReal, set_then_order.result_columns()[0].affinity);
  EXPECT_EQ(TypeAffinity::kText, set_then_order.result_columns()[1].affinity);
}

TEST(ReadSession, UsesOneBasedBindingsAndPreservesThemAcrossResetAndAutomaticReset) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement statement = PrepareStatement(session, "SELECT ?0001, ?1, :name, :name, ?");

  ASSERT_EQ(3U, statement.parameter_count());
  EXPECT_EQ("?0001", statement.parameter_name(1));
  EXPECT_EQ(":name", statement.parameter_name(2));
  EXPECT_EQ(std::nullopt, statement.parameter_name(3));
  EXPECT_EQ(std::nullopt, statement.parameter_name(4));
  EXPECT_EQ(1U, statement.parameter_index("?0001"));
  EXPECT_EQ(0U, statement.parameter_index("?1"));
  EXPECT_EQ(2U, statement.parameter_index(":name"));
  EXPECT_EQ(0U, statement.parameter_index(":missing"));

  const Status zero = statement.Bind(0, SqlValue::Integer(1));
  ASSERT_FALSE(zero.has_value());
  EXPECT_EQ(ErrorCode::kOutOfRange, zero.error().code());
  RequireStatus(statement.Bind(1, SqlValue::Integer(73)));
  RequireStatus(statement.Bind(2, SqlValue::Text("named")));

  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(5U, statement.row().size());
  EXPECT_EQ(73, IntegerValue(statement.row()[0]));
  EXPECT_EQ(73, IntegerValue(statement.row()[1]));
  EXPECT_EQ("named", TextValue(statement.row()[2]));
  EXPECT_EQ("named", TextValue(statement.row()[3]));
  EXPECT_EQ(SqlValueType::kNull, statement.row()[4].type());

  const Status active_bind = statement.Bind(1, SqlValue::Integer(99));
  ASSERT_FALSE(active_bind.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, active_bind.error().code());
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));

  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(73, IntegerValue(statement.row()[0]));
  RequireStatus(statement.Reset());
  EXPECT_TRUE(statement.row().empty());
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(73, IntegerValue(statement.row()[0]));
}

TEST(ReadSession, ExecutesScansLimitsAndSimultaneouslyActiveStatements) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement first =
      PrepareStatement(session, "SELECT id, name, score FROM items LIMIT ?1 OFFSET ?2");
  RequireStatus(first.Bind(1, SqlValue::Integer(1)));
  RequireStatus(first.Bind(2, SqlValue::Integer(1)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(first.Step()));
  ASSERT_EQ(3U, first.row().size());
  EXPECT_EQ(2, IntegerValue(first.row()[0]));
  EXPECT_EQ("beta", TextValue(first.row()[1]));
  EXPECT_DOUBLE_EQ(3.0, first.row()[2].real_value().value_or(0.0));

  ReadStatement second = PrepareStatement(session, "SELECT name FROM items WHERE rowid=?1");
  RequireStatus(second.Bind(1, SqlValue::Integer(1)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(second.Step()));
  EXPECT_EQ("alpha", TextValue(second.row().front()));

  EXPECT_EQ(ReadStep::kDone, TakeValue(first.Step()));
  EXPECT_EQ(ReadStep::kDone, TakeValue(second.Step()));
}

TEST(ReadSession, WarmPrepareUsesTheInitializedCatalogAndDefersReadFailureToStep) {
  const auto state = std::make_shared<UnlockFailureState>();
  ReadSession session = TakeValue(
      ReadSession::Open(std::make_unique<UnlockFailingVfs>(state), FixturePath().string()));
  ReadStatement warm = PrepareStatement(session, "SELECT id FROM items WHERE id=?1");
  RequireStatus(warm.Finalize());

  state->delegated_calls = 0;
  state->fail_reads = true;
  auto prepared = session.Prepare(Utf8View{"SELECT id FROM items WHERE id=?1"});
  ASSERT_TRUE(prepared.has_value());
  ASSERT_TRUE(prepared->statement.has_value());
  EXPECT_EQ(0U, state->delegated_calls);
  EXPECT_EQ(1U, prepared->statement->parameter_count());

  const auto step = prepared->statement->Step();
  ASSERT_FALSE(step.has_value());
  EXPECT_EQ(ErrorCode::kIo, step.error().code());
  EXPECT_GT(state->delegated_calls, 0U);

  state->fail_reads = false;
  const Status reset = prepared->statement->Reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(ErrorCode::kIo, reset.error().code());
  RequireStatus(prepared->statement->Bind(1, SqlValue::Integer(1)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(prepared->statement->Step()));
  EXPECT_EQ(1, IntegerValue(prepared->statement->row().front()));
  RequireStatus(prepared->statement->Finalize());
}

TEST(ReadSession, CachedWildcardMetadataChangesOnlyWhenTheStatementEntersItsSnapshot) {
  TemporaryFile database{"cached-wildcard", SessionFixturePath("sqlite-3.54.0-session-v1.db")};
  ReadSession session = TakeValue(ReadSession::Open(database.path().string()));
  ReadStatement warm = PrepareStatement(session, "SELECT id FROM items");
  RequireStatus(warm.Finalize());

  database.RewriteFrom(SessionFixturePath("sqlite-3.54.0-session-v2.db"));
  ReadStatement statement = PrepareStatement(session, "SELECT * FROM items WHERE id=?1");
  ASSERT_EQ(2U, statement.result_columns().size());
  RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  ASSERT_EQ(3U, statement.result_columns().size());
  EXPECT_EQ("added", statement.result_columns()[2].name);
  EXPECT_EQ("added", TextValue(statement.row()[2]));
}

TEST(ReadSession, ReloadsCachedSchemaForANewNameAndPreservesTheRetryMetadata) {
  TemporaryFile database{"prepare-new-name", SessionFixturePath("sqlite-3.54.0-session-v1.db")};
  ReadSession session = TakeValue(ReadSession::Open(database.path().string()));
  ReadStatement warm = PrepareStatement(session, "SELECT id FROM items");
  RequireStatus(warm.Finalize());

  database.RewriteFrom(SessionFixturePath("sqlite-3.54.0-session-v2.db"));
  ReadStatement statement = PrepareStatement(session, "SELECT added FROM items WHERE id=?1");
  ASSERT_EQ(1U, statement.result_columns().size());
  EXPECT_EQ("added", statement.result_columns().front().name);
  RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ("added", TextValue(statement.row().front()));
}

TEST(ReadSession, PreservesSchemaRetryRequestsFromDiscardedCompoundOrderBinding) {
  TemporaryFile database{"prepare-compound-name",
                         SessionFixturePath("sqlite-3.54.0-session-v1.db")};
  ReadSession session = TakeValue(ReadSession::Open(database.path().string()));
  ReadStatement warm = PrepareStatement(session, "SELECT id FROM items");
  RequireStatus(warm.Finalize());

  database.RewriteFrom(SessionFixturePath("sqlite-3.54.0-session-v2.db"));
  ReadStatement statement =
      PrepareStatement(session, "SELECT * FROM items UNION ALL SELECT * FROM items ORDER BY added");
  ASSERT_EQ(3U, statement.result_columns().size());
  EXPECT_EQ("added", statement.result_columns()[2].name);
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ("added", TextValue(statement.row()[2]));
}

TEST(ReadSession, WarmSemanticErrorsProbeOnlyNativeSchemaLookupFailures) {
  const auto state = std::make_shared<UnlockFailureState>();
  ReadSession session = TakeValue(
      ReadSession::Open(std::make_unique<UnlockFailingVfs>(state), FixturePath().string()));
  ReadStatement warm = PrepareStatement(session, "SELECT id FROM items");
  RequireStatus(warm.Finalize());
  state->fail_reads = true;

  for (const std::string_view sql :
       {"SELECT absent_function(id) FROM items", "SELECT absent_alias.* FROM items",
        "SELECT id FROM items UNION ALL SELECT id FROM items ORDER BY 99"}) {
    SCOPED_TRACE(sql);
    state->delegated_calls = 0;
    const auto result = session.Prepare(Utf8View{sql});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, result.error().code());
    EXPECT_EQ(0U, state->delegated_calls);
  }
  for (const std::string_view sql :
       {"SELECT id FROM absent_table", "SELECT absent_column FROM items",
        "SELECT id FROM items UNION ALL SELECT id FROM items ORDER BY absent_column"}) {
    SCOPED_TRACE(sql);
    state->delegated_calls = 0;
    const auto result = session.Prepare(Utf8View{sql});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, result.error().code());
    EXPECT_GT(state->delegated_calls, 0U);
    EXPECT_NE(std::string_view::npos, result.error().message().find("schema validation"));
    EXPECT_NE(std::string_view::npos, result.error().message().find("io"));
  }
  state->fail_reads = false;
}

TEST(ReadSession, AFailedKnownSchemaReplacementCannotReauthorizeTheOldCatalog) {
  TemporaryFile database{"prepare-invalidated", SessionFixturePath("sqlite-3.54.0-session-v1.db")};
  const auto state = std::make_shared<UnlockFailureState>();
  ReadSession session = TakeValue(
      ReadSession::Open(std::make_unique<UnlockFailingVfs>(state), database.path().string()));
  ReadStatement warm = PrepareStatement(session, "SELECT id FROM items");
  RequireStatus(warm.Finalize());
  database.RewriteFrom(SessionFixturePath("sqlite-3.54.0-session-v2.db"));

  state->successful_reads_before_failure = 1U;
  const auto replacement = session.Prepare(Utf8View{"SELECT added FROM items"});
  ASSERT_FALSE(replacement.has_value());
  EXPECT_EQ(ErrorCode::kIo, replacement.error().code());

  state->successful_reads_before_failure.reset();
  state->delegated_calls = 0;
  ReadStatement statement = PrepareStatement(session, "SELECT * FROM items");
  EXPECT_GT(state->delegated_calls, 0U);
  ASSERT_EQ(3U, statement.result_columns().size());
  EXPECT_EQ("added", statement.result_columns()[2].name);
}

TEST(ReadSession, WarmPrepareStillHonorsRetainedReadCleanupBeforeCompilation) {
  const auto state = std::make_shared<UnlockFailureState>();
  ReadSession session = TakeValue(
      ReadSession::Open(std::make_unique<UnlockFailingVfs>(state), FixturePath().string()));
  ReadStatement storage = PrepareStatement(session, "SELECT id FROM items WHERE id=1");
  EXPECT_EQ(ReadStep::kRow, TakeValue(storage.Step()));
  state->failures_remaining = 2;
  const auto done = storage.Step();
  ASSERT_FALSE(done.has_value());
  EXPECT_EQ(ErrorCode::kIo, done.error().code());

  const auto prepared = session.Prepare(Utf8View{"SELECT id FROM items"});
  ASSERT_FALSE(prepared.has_value());
  EXPECT_EQ(ErrorCode::kIo, prepared.error().code());
  const std::size_t attempts = state->attempts;
  ReadStatement next = PrepareStatement(session, "SELECT id FROM items");
  EXPECT_GT(state->attempts, attempts);
  EXPECT_EQ(ReadStep::kRow, TakeValue(next.Step()));
}

TEST(ReadSession, RepreparesAfterSchemaChangeAndPreservesBindingsAndMetadataViews) {
  TemporaryFile database{"reprepare", SessionFixturePath("sqlite-3.54.0-session-v1.db")};
  ReadSession session = TakeValue(ReadSession::Open(database.path().string()));
  ReadStatement statement = PrepareStatement(session, "SELECT * FROM items WHERE id=?1");
  ASSERT_EQ(2U, statement.result_columns().size());
  EXPECT_EQ("id", statement.result_columns()[0].name);
  EXPECT_EQ("name", statement.result_columns()[1].name);
  RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
  const std::optional<std::string_view> parameter_name = statement.parameter_name(1);
  ASSERT_EQ("?1", parameter_name);

  database.RewriteFrom(SessionFixturePath("sqlite-3.54.0-session-v2.db"));
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));

  ASSERT_EQ(3U, statement.result_columns().size());
  EXPECT_EQ("added", statement.result_columns()[2].name);
  ASSERT_EQ(3U, statement.row().size());
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
  EXPECT_EQ("one", TextValue(statement.row()[1]));
  EXPECT_EQ("added", TextValue(statement.row()[2]));
  EXPECT_EQ("?1", parameter_name);
  EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
  RequireStatus(statement.Reset());
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
  EXPECT_EQ(1, IntegerValue(statement.row()[0]));
}

TEST(ReadSession, SnapshotFreeStepDrainsAFailedEndReadCleanup) {
  const auto failure = std::make_shared<UnlockFailureState>();
  ReadSession session = TakeValue(
      ReadSession::Open(std::make_unique<UnlockFailingVfs>(failure), FixturePath().string()));
  ReadStatement constant = PrepareStatement(session, "SELECT 1");
  ReadStatement storage = PrepareStatement(session, "SELECT name FROM items WHERE rowid=1");
  EXPECT_EQ(ReadStep::kRow, TakeValue(storage.Step()));
  failure->failures_remaining = 1;

  const auto failed_done = storage.Step();
  ASSERT_FALSE(failed_done.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_done.error().code());
  const std::size_t attempts_after_failure = failure->attempts;

  EXPECT_EQ(ReadStep::kRow, TakeValue(constant.Step()));
  EXPECT_EQ(1, IntegerValue(constant.row().front()));
  EXPECT_GT(failure->attempts, attempts_after_failure);
  const Status reset = storage.Reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(ErrorCode::kIo, reset.error().code());
}

TEST(ReadSession, SuspendedSnapshotFreeStepHonorsTheCleanupBarrier) {
  const auto failure = std::make_shared<UnlockFailureState>();
  ReadSession session = TakeValue(
      ReadSession::Open(std::make_unique<UnlockFailingVfs>(failure), FixturePath().string()));
  ReadStatement constant = PrepareStatement(session, "SELECT 1");
  ReadStatement storage = PrepareStatement(session, "SELECT name FROM items WHERE rowid=1");
  EXPECT_EQ(ReadStep::kRow, TakeValue(constant.Step()));
  EXPECT_EQ(ReadStep::kRow, TakeValue(storage.Step()));
  failure->failures_remaining = 2;

  const auto failed_storage_done = storage.Step();
  ASSERT_FALSE(failed_storage_done.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_storage_done.error().code());
  const std::size_t attempts_after_storage = failure->attempts;

  const auto failed_constant_done = constant.Step();
  ASSERT_FALSE(failed_constant_done.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_constant_done.error().code());
  EXPECT_GT(failure->attempts, attempts_after_storage);

  EXPECT_EQ(ReadStep::kDone, TakeValue(constant.Step()));
  const Status reset = storage.Reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(ErrorCode::kIo, reset.error().code());
}

TEST(ReadSession, PrepareDrainsARetainedLockAfterFailedBeginCleanup) {
  TemporaryFile database{"retained-lock", TemporaryFile::Contents{"not a database"}};
  const auto failure = std::make_shared<UnlockFailureState>();
  failure->failures_remaining = 1;
  ReadSession session = TakeValue(
      ReadSession::Open(std::make_unique<UnlockFailingVfs>(failure), database.path().string()));

  const auto failed = session.Prepare(Utf8View{"SELECT 1"});
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_NE(std::string_view::npos, failed.error().message().find("not_database"));
  const std::size_t attempts_after_failure = failure->attempts;

  database.RewriteFrom(FixturePath());
  ReadStatement statement = PrepareStatement(session, "SELECT 1");
  EXPECT_GT(failure->attempts, attempts_after_failure);
  EXPECT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
}

TEST(ReadSession, ResetAndFinalizeReturnThePriorExecutionError) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement reset = PrepareStatement(session, "SELECT abs(-9223372036854775808)");
  const auto reset_step = reset.Step();
  ASSERT_FALSE(reset_step.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, reset_step.error().code());
  const Status reset_status = reset.Reset();
  ASSERT_FALSE(reset_status.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, reset_status.error().code());
  EXPECT_TRUE(reset.valid());

  ReadStatement automatic = PrepareStatement(session, "SELECT abs(-9223372036854775808)");
  const auto first_automatic = automatic.Step();
  ASSERT_FALSE(first_automatic.has_value());
  const auto second_automatic = automatic.Step();
  ASSERT_FALSE(second_automatic.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, second_automatic.error().code());
  const Status automatic_reset = automatic.Reset();
  ASSERT_FALSE(automatic_reset.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, automatic_reset.error().code());

  ReadStatement finalize = PrepareStatement(session, "SELECT abs(-9223372036854775808)");
  const auto finalize_step = finalize.Step();
  ASSERT_FALSE(finalize_step.has_value());
  const Status finalized = finalize.Finalize();
  ASSERT_FALSE(finalized.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, finalized.error().code());
  EXPECT_FALSE(finalize.valid());
  EXPECT_TRUE(finalize.Finalize().has_value());
}

TEST(ReadSession, MoveAssignmentFinalizesTheReplacedStatement) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement source = PrepareStatement(session, "SELECT 1");
  ReadStatement destination = PrepareStatement(session, "SELECT 2");

  destination = std::move(source);

  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(source.valid());
  EXPECT_TRUE(source.Finalize().has_value());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(ReadStep::kRow, TakeValue(destination.Step()));
  EXPECT_EQ(1, IntegerValue(destination.row().front()));
}

TEST(ReadSessionInternal, ChecksCatalogGenerationOverflowDeterministically) {
  using session_detail::CatalogGenerationError;
  using session_detail::NextCatalogGeneration;

  EXPECT_EQ(1U, *NextCatalogGeneration(0));
  const auto overflow = NextCatalogGeneration(std::numeric_limits<std::uint64_t>::max());
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(CatalogGenerationError::kOverflow, overflow.error());
}

#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
TEST(ReadSessionInstrumentation, RecordsIntegratedReadPathWork) {
  using instrumentation::Counter;
  using instrumentation::CounterCollection;
  using instrumentation::ScopedCounterCollection;

  CounterCollection counters;
  {
    const ScopedCounterCollection scope{counters};
    ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
    ReadStatement statement = PrepareStatement(session, "SELECT name FROM items WHERE rowid=?1");
    RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
    ASSERT_EQ(ReadStep::kRow, TakeValue(statement.Step()));
    EXPECT_FALSE(TextValue(statement.row().front()).empty());
    EXPECT_EQ(ReadStep::kDone, TakeValue(statement.Step()));
    RequireStatus(statement.Finalize());
  }

  EXPECT_GT(counters.Value(Counter::kVfsCalls), 0U);
  EXPECT_GT(counters.Value(Counter::kPagesRead), 0U);
  EXPECT_GT(counters.Value(Counter::kCacheMisses), 0U);
  EXPECT_GT(counters.Value(Counter::kBytesCopied), 0U);
  EXPECT_GT(counters.Value(Counter::kBtreeComparisons), 0U);
  EXPECT_GT(counters.Value(Counter::kVmInstructions), 0U);
  EXPECT_GT(counters.Value(Counter::kPlannerWork), 0U);
  EXPECT_EQ(0U, counters.Value(Counter::kPagesWritten));
}
#endif

}  // namespace
}  // namespace modern_sqlite
