#include "modern_sqlite/session/read_session.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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

#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
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
};

class UnlockFailingFile final : public File {
 public:
  UnlockFailingFile(std::unique_ptr<File> delegate, std::shared_ptr<UnlockFailureState> state)
      : delegate_(std::move(delegate)), state_(std::move(state)) {}

 private:
  Result<ByteCount> DoReadAt(MutableByteView destination, FileOffset offset) override {
    auto read = delegate_->ReadAt(destination, offset);
    if (!read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    return read->bytes_read();
  }

  Status DoWriteAt(ByteView source, FileOffset offset) override {
    return delegate_->WriteAt(source, offset);
  }

  Status DoTruncate(FileSize size) override { return delegate_->Truncate(size); }

  Status DoSync(SyncOptions options) override { return delegate_->Sync(options); }

  Result<FileSize> DoSize() override { return delegate_->Size(); }

  Status DoLock(DatabaseLock lock) override { return delegate_->Lock(lock); }

  Status DoUnlock(DatabaseLock lock) override {
    ++state_->attempts;
    if (state_->failures_remaining > 0) {
      --state_->failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected unlock failure"));
    }
    return delegate_->Unlock(lock);
  }

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
  std::shared_ptr<UnlockFailureState> state_;
};

class UnlockFailingVfs final : public Vfs {
 public:
  explicit UnlockFailingVfs(std::shared_ptr<UnlockFailureState> state) : state_(std::move(state)) {}

 private:
  Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                            FileOpenOptions options) override {
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

  PosixVfs delegate_;
  std::shared_ptr<UnlockFailureState> state_;
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
              .sorter_memory_threshold = ByteCount{1},
          },
  };
  ReadSession vfs_session = TakeValue(
      ReadSession::Open(std::make_unique<PosixVfs>(), FixturePath().string(), file_options));
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
              .sorter_memory_threshold = ByteCount{1},
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

TEST(ReadSession, RebindsOrderedLimitBetweenTopNAndExternalSorterStrategies) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  ReadStatement statement =
      PrepareStatement(session, "SELECT id, name FROM items ORDER BY name LIMIT ?1 OFFSET ?2");

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
      PrepareStatement(session, "SELECT name FROM items ORDER BY name LIMIT ?1");
  RequireStatus(invalid.Bind(1, SqlValue::Text("invalid")));
  const auto rejected = invalid.Step();
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kTypeMismatch, rejected.error().code());

  ReadStatement zero =
      PrepareStatement(session, "SELECT name FROM items ORDER BY name LIMIT 0 OFFSET ?1");
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
