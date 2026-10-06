#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/platform/vfs.hpp"
#include "write_pager_test_support.hpp"

namespace modern_sqlite {
namespace {

constexpr std::string_view kInputPath = "writable.db";
constexpr std::string_view kCanonicalPath = "/canonical/writable.db";
constexpr std::size_t kPageSize = 512;
constexpr std::uint32_t kSqliteVersion = 3'054'000;

void Store32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(value)>{bytes.data() + offset, sizeof(value)}, value);
}

struct DatabaseImageFormat {
  std::uint8_t write_version = 1;
  std::uint8_t read_version = 1;
};

[[nodiscard]] std::vector<std::byte> MakeDatabaseImage(std::uint32_t page_count = 2,
                                                       DatabaseImageFormat format = {}) {
  std::vector<std::byte> bytes(kPageSize * page_count);
  constexpr std::array<std::byte, 16> kMagic{
      std::byte{0x53}, std::byte{0x51}, std::byte{0x4c}, std::byte{0x69},
      std::byte{0x74}, std::byte{0x65}, std::byte{0x20}, std::byte{0x66},
      std::byte{0x6f}, std::byte{0x72}, std::byte{0x6d}, std::byte{0x61},
      std::byte{0x74}, std::byte{0x20}, std::byte{0x33}, std::byte{0x00},
  };
  std::ranges::copy(kMagic, bytes.begin());
  bytes[16] = std::byte{0x02};
  bytes[17] = std::byte{0x00};
  bytes[18] = static_cast<std::byte>(format.write_version);
  bytes[19] = static_cast<std::byte>(format.read_version);
  bytes[21] = std::byte{64};
  bytes[22] = std::byte{32};
  bytes[23] = std::byte{32};
  Store32(bytes, 24, 7);
  Store32(bytes, 28, page_count);
  Store32(bytes, 44, 4);
  Store32(bytes, 56, 1);
  Store32(bytes, 92, 7);
  Store32(bytes, 96, kSqliteVersion);
  for (std::uint32_t page = 1; page <= page_count; ++page) {
    bytes[(static_cast<std::size_t>(page) - 1U) * kPageSize + 100U] = static_cast<std::byte>(page);
  }
  return bytes;
}

struct MemoryFileState {
  std::vector<std::byte> bytes;
  std::vector<std::byte> durable_bytes;
  bool present = true;
  bool durable_present = true;
  bool writes_are_durable = false;
  FileProperties properties{
      .sector_size = ByteCount{kPageSize},
      .device_characteristics = {},
  };
  DatabaseLock lock = DatabaseLock::kNone;
  std::vector<DatabaseLock> locks;
  std::vector<DatabaseLock> unlocks;
  std::optional<DatabaseLock> failing_lock;
  std::optional<DatabaseLock> failing_unlock;
  std::optional<std::size_t> failing_write_attempt;
  int lock_failures_remaining = 0;
  int unlock_failures_remaining = 0;
  int sync_failures_remaining = 0;
  int truncate_failures_remaining = 0;
  int size_failures_remaining = 0;
  bool partial_write_before_failure = false;
  std::size_t read_count = 0;
  std::size_t write_attempt_count = 0;
  std::size_t sync_count = 0;
  std::size_t write_count = 0;
  std::size_t truncate_count = 0;
};

struct MemoryVfsState {
  std::map<std::string, std::shared_ptr<MemoryFileState>, std::less<>> files;
  std::vector<std::shared_ptr<MemoryFileState>> temporary_files;
  std::vector<std::string> trace;
  std::vector<std::pair<std::string, FileOpenOptions>> opens;
  std::optional<std::string> failing_delete;
  int delete_failures_remaining = 0;
  bool unlink_before_delete_failure = false;
  std::optional<std::size_t> fail_after_mutation;
  std::size_t mutation_count = 0;
  bool mutation_cut_triggered = false;

  [[nodiscard]] bool CutAfterMutation() {
    ++mutation_count;
    if (fail_after_mutation == mutation_count) {
      mutation_cut_triggered = true;
      fail_after_mutation.reset();
      return true;
    }
    return false;
  }

  void Crash() {
    for (auto& [path, file] : files) {
      static_cast<void>(path);
      file->bytes = file->durable_bytes;
      file->present = file->durable_present;
      file->lock = DatabaseLock::kNone;
    }
    for (const std::shared_ptr<MemoryFileState>& file : temporary_files) {
      file->bytes = file->durable_bytes;
      file->present = file->durable_present;
      file->lock = DatabaseLock::kNone;
    }
  }
};

class MemoryFile final : public File {
 public:
  MemoryFile(std::shared_ptr<MemoryFileState> state, std::shared_ptr<MemoryVfsState> vfs_state,
             std::string name, FileAccessMode access, bool delete_on_close)
      : state_(std::move(state)),
        vfs_state_(std::move(vfs_state)),
        name_(std::move(name)),
        access_(access),
        delete_on_close_(delete_on_close) {}

  ~MemoryFile() override {
    state_->lock = DatabaseLock::kNone;
    if (delete_on_close_) {
      state_->present = false;
      state_->bytes.clear();
    }
  }

 private:
  [[nodiscard]] Result<ByteCount> DoReadAt(MutableByteView destination,
                                           FileOffset offset) override {
    if (offset.value() > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(Error::Create(ErrorCode::kTooLarge, "test offset is too large"));
    }
    const auto start = static_cast<std::size_t>(offset.value());
    if (start >= state_->bytes.size()) {
      return ByteCount{0};
    }
    ++state_->read_count;
    const std::size_t count = std::min(destination.size(), state_->bytes.size() - start);
    std::ranges::copy_n(state_->bytes.begin() + static_cast<std::ptrdiff_t>(start),
                        static_cast<std::ptrdiff_t>(count), destination.begin());
    return ByteCount{count};
  }

  [[nodiscard]] Status DoWriteAt(ByteView source, FileOffset offset) override {
    if (access_ != FileAccessMode::kReadWrite) {
      return std::unexpected(Error::Create(ErrorCode::kReadOnly, "test file is read-only"));
    }
    if (offset.value() > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(Error::Create(ErrorCode::kTooLarge, "test offset is too large"));
    }
    const auto start = static_cast<std::size_t>(offset.value());
    if (source.size() > std::numeric_limits<std::size_t>::max() - start) {
      return std::unexpected(Error::Create(ErrorCode::kTooLarge, "test write is too large"));
    }
    const std::size_t end = start + source.size();
    ++state_->write_attempt_count;
    if (state_->failing_write_attempt == state_->write_attempt_count) {
      state_->failing_write_attempt.reset();
      if (state_->partial_write_before_failure && !source.empty()) {
        const std::size_t partial_size = std::max<std::size_t>(1, source.size() / 2U);
        const std::size_t partial_end = start + partial_size;
        if (partial_end > state_->bytes.size()) {
          state_->bytes.resize(partial_end);
        }
        std::ranges::copy_n(source.begin(), static_cast<std::ptrdiff_t>(partial_size),
                            state_->bytes.begin() + static_cast<std::ptrdiff_t>(start));
        state_->present = true;
        if (state_->writes_are_durable) {
          state_->durable_bytes = state_->bytes;
          state_->durable_present = true;
        }
      }
      vfs_state_->trace.push_back("write-error:" + name_);
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected write failure"));
    }
    if (end > state_->bytes.size()) {
      state_->bytes.resize(end);
    }
    std::ranges::copy(source, state_->bytes.begin() + static_cast<std::ptrdiff_t>(start));
    state_->present = true;
    if (state_->writes_are_durable) {
      state_->durable_bytes = state_->bytes;
      state_->durable_present = true;
    }
    ++state_->write_count;
    vfs_state_->trace.push_back("write:" + name_);
    if (vfs_state_->CutAfterMutation()) {
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected crash after write"));
    }
    return {};
  }

  [[nodiscard]] Status DoTruncate(FileSize size) override {
    if (access_ != FileAccessMode::kReadWrite) {
      return std::unexpected(Error::Create(ErrorCode::kReadOnly, "test file is read-only"));
    }
    if (size.value() > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(Error::Create(ErrorCode::kTooLarge, "test size is too large"));
    }
    if (state_->truncate_failures_remaining > 0) {
      --state_->truncate_failures_remaining;
      vfs_state_->trace.push_back("truncate-error:" + name_);
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected truncate failure"));
    }
    state_->bytes.resize(static_cast<std::size_t>(size.value()));
    state_->present = true;
    if (state_->writes_are_durable) {
      state_->durable_bytes = state_->bytes;
      state_->durable_present = true;
    }
    ++state_->truncate_count;
    vfs_state_->trace.push_back("truncate:" + name_);
    if (vfs_state_->CutAfterMutation()) {
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected crash after truncate"));
    }
    return {};
  }

  [[nodiscard]] Status DoSync(SyncOptions) override {
    if (state_->sync_failures_remaining > 0) {
      --state_->sync_failures_remaining;
      vfs_state_->trace.push_back("sync-error:" + name_);
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected sync failure"));
    }
    ++state_->sync_count;
    state_->durable_bytes = state_->bytes;
    state_->durable_present = state_->present;
    vfs_state_->trace.push_back("sync:" + name_);
    if (vfs_state_->CutAfterMutation()) {
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected crash after sync"));
    }
    return {};
  }

  [[nodiscard]] Result<FileSize> DoSize() override {
    if (state_->size_failures_remaining > 0) {
      --state_->size_failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected size failure"));
    }
    return FileSize{state_->bytes.size()};
  }

  [[nodiscard]] Status DoLock(DatabaseLock lock) override {
    state_->locks.push_back(lock);
    if (state_->failing_lock == lock && state_->lock_failures_remaining > 0) {
      --state_->lock_failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kBusy, "injected lock conflict"));
    }
    state_->lock = lock;
    return {};
  }

  [[nodiscard]] Status DoUnlock(DatabaseLock lock) override {
    state_->unlocks.push_back(lock);
    if (state_->failing_unlock == lock && state_->unlock_failures_remaining > 0) {
      --state_->unlock_failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected unlock failure"));
    }
    state_->lock = lock;
    return {};
  }

  [[nodiscard]] Result<bool> DoHasReservedLock() override {
    return state_->lock >= DatabaseLock::kReserved;
  }

  [[nodiscard]] FileProperties DoProperties() const noexcept override { return state_->properties; }

  [[nodiscard]] Result<std::optional<MutableByteView>> DoMapSharedMemory(
      SharedMemoryRegionIndex, ByteCount, SharedMemoryMapMode) override {
    return std::optional<MutableByteView>{};
  }

  [[nodiscard]] Status DoLockSharedMemory(SharedMemoryLockRange, SharedMemoryLockOperation,
                                          SharedMemoryLockMode) override {
    return {};
  }

  void DoSharedMemoryBarrier() noexcept override {}

  [[nodiscard]] Status DoUnmapSharedMemory(SharedMemoryUnmapMode) override { return {}; }

  std::shared_ptr<MemoryFileState> state_;
  std::shared_ptr<MemoryVfsState> vfs_state_;
  std::string name_;
  FileAccessMode access_;
  bool delete_on_close_;
};

class MemoryVfs final : public Vfs {
 public:
  explicit MemoryVfs(std::shared_ptr<MemoryVfsState> state) : state_(std::move(state)) {}

 private:
  [[nodiscard]] Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                                          FileOpenOptions options) override {
    const std::string name = path.has_value() ? std::string{*path} : "<temporary>";
    state_->opens.emplace_back(name, options);

    std::shared_ptr<MemoryFileState> file_state;
    if (!path.has_value()) {
      file_state = std::make_shared<MemoryFileState>();
      file_state->durable_present = false;
      state_->temporary_files.push_back(file_state);
    } else {
      const auto existing = state_->files.find(*path);
      if (existing == state_->files.end()) {
        if (!options.create) {
          return std::unexpected(Error::Create(ErrorCode::kNotFound, "test file is absent"));
        }
        file_state = std::make_shared<MemoryFileState>();
        file_state->durable_present = false;
        state_->files.emplace(std::string{*path}, file_state);
      } else {
        file_state = existing->second;
        if (!file_state->present && !options.create) {
          return std::unexpected(Error::Create(ErrorCode::kNotFound, "test file is absent"));
        }
        if (options.exclusive_create && file_state->present) {
          return std::unexpected(Error::Create(ErrorCode::kCannotOpen, "test file already exists"));
        }
      }
    }
    file_state->present = true;
    return OpenedFile{
        .file = std::make_unique<MemoryFile>(file_state, state_, name, options.access,
                                             options.delete_on_close),
        .access = options.access,
    };
  }

  [[nodiscard]] Status DoDelete(std::string_view path, DirectorySync) override {
    const auto file = state_->files.find(path);
    if (file == state_->files.end() || !file->second->present) {
      return std::unexpected(Error::Create(ErrorCode::kNotFound, "test file is absent"));
    }
    if (state_->failing_delete == path && state_->delete_failures_remaining > 0) {
      --state_->delete_failures_remaining;
      if (state_->unlink_before_delete_failure) {
        file->second->present = false;
        file->second->bytes.clear();
        file->second->durable_present = false;
        file->second->durable_bytes.clear();
      }
      state_->trace.push_back("delete-error:" + std::string{path});
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected delete failure"));
    }
    file->second->present = false;
    file->second->bytes.clear();
    file->second->durable_present = false;
    file->second->durable_bytes.clear();
    state_->trace.push_back("delete:" + std::string{path});
    if (state_->CutAfterMutation()) {
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected crash after delete"));
    }
    return {};
  }

  [[nodiscard]] Result<bool> DoAccess(std::string_view path, FileAccessQuery) override {
    const auto file = state_->files.find(path);
    return file != state_->files.end() && file->second->present;
  }

  [[nodiscard]] Result<std::string> DoFullPath(std::string_view path) override {
    if (path == kInputPath) {
      return std::string{kCanonicalPath};
    }
    return std::string{path};
  }

  [[nodiscard]] Result<ByteCount> DoRandomBytes(MutableByteView output) override {
    std::ranges::fill(output, std::byte{0x5a});
    return ByteCount{output.size()};
  }

  [[nodiscard]] Result<std::chrono::microseconds> DoSleepFor(
      std::chrono::microseconds duration) override {
    return duration;
  }

  [[nodiscard]] Result<WallClockTime> DoCurrentTime() override { return WallClockTime{}; }

  [[nodiscard]] ByteCount DoMaximumPathLength() const noexcept override { return ByteCount{512}; }

  std::shared_ptr<MemoryVfsState> state_;
};

class WritableEnvironment final {
 public:
  explicit WritableEnvironment(std::vector<std::byte> database = MakeDatabaseImage())
      : state(std::make_shared<MemoryVfsState>()),
        main(std::make_shared<MemoryFileState>()),
        vfs(state) {
    main->bytes = std::move(database);
    main->durable_bytes = main->bytes;
    state->files.emplace(std::string{kCanonicalPath}, main);
  }

  [[nodiscard]] bool JournalPresent() const {
    const auto journal = state->files.find(std::string{kCanonicalPath} + "-journal");
    return journal != state->files.end() && journal->second->present;
  }

  [[nodiscard]] std::size_t TraceIndex(std::string_view event) const {
    const auto iterator = std::ranges::find(state->trace, event);
    return iterator == state->trace.end()
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(std::distance(state->trace.begin(), iterator));
  }

  [[nodiscard]] std::shared_ptr<MemoryFileState> PrepareJournalFile() {
    const std::string path = std::string{kCanonicalPath} + "-journal";
    auto journal = std::make_shared<MemoryFileState>();
    journal->present = false;
    journal->durable_present = false;
    state->files.insert_or_assign(path, journal);
    return journal;
  }

  [[nodiscard]] std::shared_ptr<MemoryFileState> JournalFile() const {
    return state->files.at(std::string{kCanonicalPath} + "-journal");
  }

  void Crash() { state->Crash(); }

  std::shared_ptr<MemoryVfsState> state;
  std::shared_ptr<MemoryFileState> main;
  MemoryVfs vfs;
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    static std::uint64_t next_id = 0;
    std::error_code error;
    path_ = std::filesystem::temp_directory_path(error);
    if (error) {
      return;
    }
    path_ /= "modern-sqlite-write-pager-" + std::to_string(++next_id);
    std::filesystem::create_directories(path_, error);
    valid_ = !error;
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] std::filesystem::path DatabasePath() const { return path_ / "database.sqlite"; }

 private:
  std::filesystem::path path_;
  bool valid_ = false;
};

[[nodiscard]] bool WriteBytes(const std::filesystem::path& path, ByteView bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

[[nodiscard]] std::unique_ptr<Pager> OpenWritable(WritableEnvironment& environment,
                                                  std::size_t cache_pages = 4) {
  auto opened = Pager::OpenWritable(environment.vfs, kInputPath,
                                    WritablePagerOptions{
                                        .pager =
                                            PagerOptions{
                                                .empty_database_page_size = ByteCount{kPageSize},
                                                .cache_capacity_pages = cache_pages,
                                            },
                                        .journal =
                                            RollbackJournalOptions{
                                                .legacy_page_size = ByteCount{kPageSize},
                                            },
                                    });
  if (!opened.has_value()) {
    return nullptr;
  }
  return std::move(*opened);
}

[[nodiscard]] bool CommitOnePageChange(WritableEnvironment& environment) {
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return false;
  }
  {
    auto writable = pager->WritePage(PageNumber{2});
    if (!writable.has_value()) {
      return false;
    }
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  return pager->Commit().has_value();
}

[[nodiscard]] bool CommitSpilledChange(WritableEnvironment& environment) {
  std::unique_ptr<Pager> pager = OpenWritable(environment, 1);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return false;
  }
  {
    auto writable = pager->WritePage(PageNumber{2});
    if (!writable.has_value()) {
      return false;
    }
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  {
    const auto writable = pager->WritePage(PageNumber{1});
    if (!writable.has_value()) {
      return false;
    }
  }
  {
    const auto trigger = pager->ReadPage(PageNumber{1});
    if (!trigger.has_value()) {
      return false;
    }
  }
  return pager->Commit().has_value();
}

[[nodiscard]] bool RollbackSpilledChange(WritableEnvironment& environment) {
  std::unique_ptr<Pager> pager = OpenWritable(environment, 1);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return false;
  }
  {
    auto writable = pager->WritePage(PageNumber{2});
    if (!writable.has_value()) {
      return false;
    }
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  {
    const auto writable = pager->WritePage(PageNumber{1});
    if (!writable.has_value()) {
      return false;
    }
  }
  {
    const auto trigger = pager->ReadPage(PageNumber{1});
    if (!trigger.has_value()) {
      return false;
    }
  }
  return pager->Rollback().has_value();
}

[[nodiscard]] bool CommitGrowth(WritableEnvironment& environment) {
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return false;
  }
  {
    auto appended = pager->AllocatePage();
    if (!appended.has_value()) {
      return false;
    }
    appended->mutable_bytes()[100] = std::byte{0x7f};
  }
  {
    auto header = pager->WritePage(PageNumber{1});
    if (!header.has_value()) {
      return false;
    }
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{header->mutable_bytes().data() + 28, 4},
                                  3);
  }
  return pager->Commit().has_value();
}

[[nodiscard]] bool CommitShrink(WritableEnvironment& environment) {
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return false;
  }
  {
    auto header = pager->WritePage(PageNumber{1});
    if (!header.has_value()) {
      return false;
    }
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{header->mutable_bytes().data() + 28, 4},
                                  2);
  }
  return pager->TruncateImage(2).has_value() && pager->Commit().has_value();
}

[[nodiscard]] bool RecoverWritableImage(WritableEnvironment& environment) {
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  return pager != nullptr && pager->BeginRead().has_value();
}

TEST(WritePager, OpensReadWriteAndTransitionsThroughAnEmptyWriteTransaction) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_FALSE(environment.state->opens.empty());
  EXPECT_EQ(FileAccessMode::kReadWrite, environment.state->opens.front().second.access);
  EXPECT_TRUE(environment.state->opens.front().second.create);
  EXPECT_TRUE(pager->writable());
  EXPECT_EQ(PagerState::kOpen, pager->state());

  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  EXPECT_EQ(PagerState::kWriterLocked, pager->state());
  ASSERT_EQ(2U, environment.main->locks.size());
  EXPECT_EQ(DatabaseLock::kShared, environment.main->locks[0]);
  EXPECT_EQ(DatabaseLock::kReserved, environment.main->locks[1]);
  EXPECT_FALSE(environment.JournalPresent());

  ASSERT_TRUE(pager->Commit().has_value());
  EXPECT_EQ(PagerState::kReader, pager->state());
  EXPECT_FALSE(environment.JournalPresent());
  EXPECT_TRUE(pager->EndRead().has_value());
}

TEST(WritePager, ClaimsOneWriteCoordinatorPerAdmittedGeneration) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(0U, pager->write_transaction_generation());

  ASSERT_TRUE(pager->BeginWrite().has_value());
  const std::uint64_t first_generation = pager->write_transaction_generation();
  EXPECT_GT(first_generation, 0U);
  ASSERT_TRUE(pager->ClaimWriteCoordinator().has_value());
  const auto duplicate = pager->ClaimWriteCoordinator();
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kLocked, duplicate.error().code());

  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  ASSERT_TRUE(pager->RollbackToSavepoint(*savepoint).has_value());
  EXPECT_GT(pager->write_transaction_generation(), first_generation);
  ASSERT_TRUE(pager->ClaimWriteCoordinator().has_value());

  ASSERT_TRUE(pager->Rollback().has_value());
  const std::uint64_t rolled_back_generation = pager->write_transaction_generation();
  ASSERT_TRUE(pager->BeginWrite().has_value());
  EXPECT_GT(pager->write_transaction_generation(), rolled_back_generation);
  ASSERT_TRUE(pager->ClaimWriteCoordinator().has_value());
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, FailedCommitKeepsTheWriteCoordinatorClaimLatched) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  ASSERT_TRUE(pager->ClaimWriteCoordinator().has_value());
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  const std::uint64_t transaction_generation = pager->write_transaction_generation();
  environment.main->failing_lock = DatabaseLock::kExclusive;
  environment.main->lock_failures_remaining = 1;

  const auto first = pager->Commit();

  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(ErrorCode::kBusy, first.error().code());
  EXPECT_GT(pager->write_transaction_generation(), transaction_generation);
  const auto reopened = pager->ClaimWriteCoordinator();
  ASSERT_FALSE(reopened.has_value());
  EXPECT_EQ(ErrorCode::kLocked, reopened.error().code());
  const auto write = pager->WritePage(PageNumber{2});
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, write.error().code());
  const auto allocated = pager->AllocatePage();
  ASSERT_FALSE(allocated.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, allocated.error().code());
  const auto late_savepoint = pager->CreateSavepoint();
  ASSERT_FALSE(late_savepoint.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, late_savepoint.error().code());
  const auto released = pager->ReleaseSavepoint(*savepoint);
  ASSERT_FALSE(released.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, released.error().code());
  const auto truncated = pager->TruncateImage(pager->page_count());
  ASSERT_FALSE(truncated.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, truncated.error().code());

  ASSERT_TRUE(pager->RollbackToSavepoint(*savepoint).has_value());
  ASSERT_TRUE(pager->ClaimWriteCoordinator().has_value());
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, CoordinatorFailurePoisonsTheGenerationAndRollsBackExactly) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment, 1);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  const std::vector<std::byte> committed = environment.main->bytes;

  ASSERT_TRUE(pager->ClaimWriteCoordinator().has_value());
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x5a};
  }
  constexpr ErrorCode failure_code = ErrorCode::kBusy;
  pager->ReportWriteCoordinatorFailure(failure_code);
  EXPECT_EQ(failure_code, pager->write_failure_code());

  const auto blocked = pager->ClaimWriteCoordinator();
  ASSERT_FALSE(blocked.has_value());
  EXPECT_EQ(failure_code, blocked.error().code());
  const auto late_savepoint = pager->CreateSavepoint();
  ASSERT_FALSE(late_savepoint.has_value());
  EXPECT_EQ(failure_code, late_savepoint.error().code());
  const auto committed_partial_image = pager->Commit();
  ASSERT_FALSE(committed_partial_image.has_value());
  EXPECT_EQ(failure_code, committed_partial_image.error().code());
  const auto direct_write = pager->WritePage(PageNumber{1});
  ASSERT_FALSE(direct_write.has_value());
  EXPECT_EQ(failure_code, direct_write.error().code());

  ASSERT_TRUE(pager->RollbackToSavepoint(*savepoint).has_value());
  EXPECT_FALSE(pager->write_failure_code().has_value());
  ASSERT_TRUE(pager->ClaimWriteCoordinator().has_value());
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(committed, environment.main->bytes);
}

TEST(WritePager, EmptyInitializationRollbackCommitsAsANoOp) {
  WritableEnvironment environment{std::vector<std::byte>{}};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  ASSERT_TRUE(test::InitializeEmptyBtreeImage(*pager).has_value());
  environment.main->failing_lock = DatabaseLock::kExclusive;
  environment.main->lock_failures_remaining = 1;
  const auto failed_commit = pager->Commit();
  ASSERT_FALSE(failed_commit.has_value());
  EXPECT_EQ(ErrorCode::kBusy, failed_commit.error().code());

  ASSERT_TRUE(pager->RollbackToSavepoint(*savepoint).has_value());
  EXPECT_EQ(0U, pager->page_count());
  const std::size_t database_writes = environment.main->write_count;
  const std::size_t database_syncs = environment.main->sync_count;
  ASSERT_TRUE(pager->Commit().has_value());
  EXPECT_EQ(database_writes, environment.main->write_count);
  EXPECT_EQ(database_syncs, environment.main->sync_count);
  EXPECT_TRUE(environment.main->bytes.empty());
}

TEST(WritePager, RejectsUnsupportedWriteFormatsBeforeReservedLock) {
  WritableEnvironment environment{
      MakeDatabaseImage(2, DatabaseImageFormat{.write_version = 2, .read_version = 1})};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());

  const auto begun = pager->BeginWrite();

  ASSERT_FALSE(begun.has_value());
  EXPECT_EQ(ErrorCode::kProtocol, begun.error().code());
  ASSERT_EQ(1U, environment.main->locks.size());
  EXPECT_EQ(DatabaseLock::kShared, environment.main->locks.front());
  EXPECT_FALSE(environment.JournalPresent());
}

TEST(WritePager, GrantsExclusiveMutableAccessOnlyAfterJournalCapture) {
  WritableEnvironment environment;
  const std::byte original = environment.main->bytes[kPageSize + 100U];
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  std::optional<ReadPagePin> reader;
  {
    auto read = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(read.has_value());
    reader.emplace(std::move(*read));
  }
  const auto busy = pager->WritePage(PageNumber{2});
  ASSERT_FALSE(busy.has_value());
  EXPECT_EQ(ErrorCode::kBusy, busy.error().code());
  reader.reset();

  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    EXPECT_TRUE(environment.JournalPresent());
    writable->mutable_bytes()[100] = std::byte{0x7f};
    const auto aliased_read = pager->ReadPage(PageNumber{2});
    ASSERT_FALSE(aliased_read.has_value());
    EXPECT_EQ(ErrorCode::kBusy, aliased_read.error().code());
  }
  EXPECT_EQ(PagerState::kWriterCacheModified, pager->state());

  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(original, environment.main->bytes[kPageSize + 100U]);
  EXPECT_FALSE(environment.JournalPresent());
  EXPECT_EQ(PagerState::kReader, pager->state());
}

TEST(WritePager, CommitsJournalBeforeDatabaseAndDatabaseBeforeJournalDeletion) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }

  ASSERT_TRUE(pager->Commit().has_value());

  EXPECT_EQ(std::byte{0x7f}, environment.main->bytes[kPageSize + 100U]);
  EXPECT_EQ(PagerState::kReader, pager->state());
  EXPECT_FALSE(environment.JournalPresent());
  const std::string journal_name = std::string{kCanonicalPath} + "-journal";
  const std::size_t journal_sync = environment.TraceIndex("sync:" + journal_name);
  const std::size_t database_write = environment.TraceIndex("write:" + std::string{kCanonicalPath});
  const std::size_t database_sync = environment.TraceIndex("sync:" + std::string{kCanonicalPath});
  const std::size_t journal_delete = environment.TraceIndex("delete:" + journal_name);
  EXPECT_LT(journal_sync, database_write);
  EXPECT_LT(database_write, database_sync);
  EXPECT_LT(database_sync, journal_delete);
  EXPECT_EQ(8U, LoadBigEndian<std::uint32_t>(
                    std::span<const std::byte, 4>{environment.main->bytes.data() + 24, 4}));
}

TEST(WritePager, RollsBackToASavepointWithoutEndingTheWriteTransaction) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }

  ASSERT_TRUE(pager->RollbackToSavepoint(*savepoint).has_value());
  EXPECT_TRUE(pager->in_write_transaction());
  {
    auto restored = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(std::byte{2}, restored->frame().bytes()[100]);
  }
  EXPECT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, SpillsUnderPressureAndFullRollbackRestoresTheDatabase) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment, 1);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  {
    const auto writable = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(writable.has_value());
  }
  {
    const auto trigger = pager->ReadPage(PageNumber{1});
    ASSERT_TRUE(trigger.has_value());
  }

  EXPECT_EQ(PagerState::kWriterDatabaseModified, pager->state());
  EXPECT_EQ(std::byte{0x7f}, environment.main->bytes[kPageSize + 100U]);
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(std::byte{2}, environment.main->bytes[kPageSize + 100U]);
}

TEST(WritePager, AllocatesZeroFilledPagesAndRollbackRemovesThem) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  {
    auto page = pager->AllocatePage();
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(PageNumber{3}, page->frame().page_number());
    EXPECT_TRUE(std::ranges::all_of(page->frame().bytes(),
                                    [](std::byte value) { return value == std::byte{0}; }));
  }
  EXPECT_EQ(3U, pager->page_count());
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(2U, pager->page_count());
  EXPECT_EQ(kPageSize * 2U, environment.main->bytes.size());
}

TEST(WritePager, MakesTruncationTerminalAndDurableBeforePhysicalCleanup) {
  WritableEnvironment environment{MakeDatabaseImage(3)};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto header = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(header.has_value());
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{header->mutable_bytes().data() + 28, 4},
                                  2);
  }
  auto retained_result = pager->ReadPage(PageNumber{2});
  ASSERT_TRUE(retained_result.has_value());
  std::optional<ReadPagePin> retained;
  retained.emplace(std::move(*retained_result));
  const auto pinned = pager->TruncateImage(2);
  ASSERT_FALSE(pinned.has_value());
  EXPECT_EQ(ErrorCode::kBusy, pinned.error().code());
  retained.reset();

  ASSERT_TRUE(pager->TruncateImage(2).has_value());
  const auto later_write = pager->WritePage(PageNumber{2});
  ASSERT_FALSE(later_write.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, later_write.error().code());
  ASSERT_TRUE(pager->Commit().has_value());
  EXPECT_EQ(kPageSize * 2U, environment.main->bytes.size());
  EXPECT_EQ(2U, LoadBigEndian<std::uint32_t>(
                    std::span<const std::byte, 4>{environment.main->bytes.data() + 28, 4}));
}

TEST(WritePager, ReopensAndRecoversAHotJournalBeforePublishingTheSnapshot) {
  WritableEnvironment environment;
  {
    std::unique_ptr<Pager> crashed = OpenWritable(environment, 1);
    ASSERT_NE(nullptr, crashed);
    ASSERT_TRUE(crashed->BeginRead().has_value());
    ASSERT_TRUE(crashed->BeginWrite().has_value());
    {
      auto writable = crashed->WritePage(PageNumber{2});
      ASSERT_TRUE(writable.has_value());
      writable->mutable_bytes()[100] = std::byte{0x7f};
    }
    {
      const auto writable = crashed->WritePage(PageNumber{1});
      ASSERT_TRUE(writable.has_value());
    }
    {
      const auto trigger = crashed->ReadPage(PageNumber{1});
      ASSERT_TRUE(trigger.has_value());
    }
    ASSERT_TRUE(environment.JournalPresent());
    EXPECT_EQ(std::byte{0x7f}, environment.main->bytes[kPageSize + 100U]);
  }

  std::unique_ptr<Pager> recovered = OpenWritable(environment);
  ASSERT_NE(nullptr, recovered);
  ASSERT_TRUE(recovered->BeginRead().has_value());
  EXPECT_FALSE(environment.JournalPresent());
  EXPECT_EQ(std::byte{2}, environment.main->bytes[kPageSize + 100U]);
  {
    auto page = recovered->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{2}, page->frame().bytes()[100]);
  }
}

TEST(WritePager, UpdatesTheChangeCounterOnlyOnceAcrossExclusiveLockRetry) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  environment.main->failing_lock = DatabaseLock::kExclusive;
  environment.main->lock_failures_remaining = 1;

  const auto first = pager->Commit();

  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(ErrorCode::kBusy, first.error().code());
  EXPECT_EQ(PagerState::kWriterCacheModified, pager->state());
  ASSERT_TRUE(pager->Commit().has_value());
  EXPECT_EQ(8U, LoadBigEndian<std::uint32_t>(
                    std::span<const std::byte, 4>{environment.main->bytes.data() + 24, 4}));
  EXPECT_EQ(8U, LoadBigEndian<std::uint32_t>(
                    std::span<const std::byte, 4>{environment.main->bytes.data() + 92, 4}));
}

TEST(WritePager, RollsBackAfterAPartiallyFailedDatabaseWrite) {
  WritableEnvironment environment;
  const std::vector<std::byte> original = environment.main->bytes;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    std::ranges::fill(writable->mutable_bytes(), std::byte{0x7f});
  }
  environment.main->failing_write_attempt = environment.main->write_attempt_count + 1U;
  environment.main->partial_write_before_failure = true;

  const auto committed = pager->Commit();

  ASSERT_FALSE(committed.has_value());
  EXPECT_EQ(ErrorCode::kIo, committed.error().code());
  ASSERT_NE(original, environment.main->bytes);
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(original, environment.main->bytes);
  EXPECT_EQ(PagerState::kReader, pager->state());
}

TEST(WritePager, SavepointRollbackShrinksGrowthThatWasAlreadySpilled) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment, 1);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  {
    auto appended = pager->AllocatePage();
    ASSERT_TRUE(appended.has_value());
    appended->mutable_bytes()[100] = std::byte{0x7f};
  }
  {
    const auto trigger = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(trigger.has_value());
  }
  {
    const auto trigger = pager->ReadPage(PageNumber{1});
    ASSERT_TRUE(trigger.has_value());
  }
  ASSERT_EQ(kPageSize * 3U, environment.main->bytes.size());

  ASSERT_TRUE(pager->RollbackToSavepoint(*savepoint).has_value());

  EXPECT_EQ(2U, pager->page_count());
  EXPECT_EQ(kPageSize * 2U, environment.main->bytes.size());
  ASSERT_TRUE(pager->Commit().has_value());
  EXPECT_EQ(kPageSize * 2U, environment.main->bytes.size());
}

TEST(WritePager, WriterFinishedAllowsOnlyTheMatchingCleanupRetry) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  environment.main->failing_unlock = DatabaseLock::kShared;
  environment.main->unlock_failures_remaining = 1;

  const auto committed = pager->Commit();

  ASSERT_FALSE(committed.has_value());
  EXPECT_EQ(ErrorCode::kIo, committed.error().code());
  EXPECT_EQ(PagerState::kWriterFinished, pager->state());
  const auto read = pager->ReadPage(PageNumber{1});
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, read.error().code());
  const auto rollback = pager->Rollback();
  ASSERT_FALSE(rollback.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, rollback.error().code());
  ASSERT_TRUE(pager->Commit().has_value());
  EXPECT_EQ(PagerState::kReader, pager->state());
}

TEST(WritePager, CacheOnlyRollbackPerformsNoDatabaseIo) {
  WritableEnvironment environment;
  const std::vector<std::byte> original = environment.main->bytes;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }

  ASSERT_TRUE(pager->Rollback().has_value());

  EXPECT_EQ(original, environment.main->bytes);
  EXPECT_EQ(0U, environment.main->write_count);
  EXPECT_EQ(0U, environment.main->sync_count);
  EXPECT_EQ(0U, environment.main->truncate_count);
}

TEST(WritePager, CacheOnlySavepointRollbackRestoresTheCachedImageWithoutDatabaseIo) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x33};
  }
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }

  ASSERT_TRUE(pager->RollbackToSavepoint(*savepoint).has_value());

  {
    auto restored = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(std::byte{0x33}, restored->frame().bytes()[100]);
  }
  EXPECT_EQ(0U, environment.main->write_count);
  EXPECT_EQ(0U, environment.main->sync_count);
  EXPECT_EQ(0U, environment.main->truncate_count);
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, JournalRecordFailureNeverGrantsMutableAccessAndRequiresRollback) {
  WritableEnvironment environment;
  const std::shared_ptr<MemoryFileState> journal = environment.PrepareJournalFile();
  journal->failing_write_attempt = 2;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  const auto first = pager->WritePage(PageNumber{2});

  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(ErrorCode::kIo, first.error().code());
  EXPECT_EQ(PagerState::kWriterLocked, pager->state());
  EXPECT_EQ(ErrorCode::kIo, pager->write_failure_code());
  const auto claimed = pager->ClaimWriteCoordinator();
  ASSERT_FALSE(claimed.has_value());
  EXPECT_EQ(ErrorCode::kIo, claimed.error().code());
  const auto second = pager->WritePage(PageNumber{2});
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(ErrorCode::kIo, second.error().code());
  const auto allocated = pager->AllocatePage();
  ASSERT_FALSE(allocated.has_value());
  EXPECT_EQ(ErrorCode::kIo, allocated.error().code());
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_FALSE(savepoint.has_value());
  EXPECT_EQ(ErrorCode::kIo, savepoint.error().code());
  const auto truncated = pager->TruncateImage(1);
  ASSERT_FALSE(truncated.has_value());
  EXPECT_EQ(ErrorCode::kIo, truncated.error().code());
  const auto committed = pager->Commit();
  ASSERT_FALSE(committed.has_value());
  EXPECT_EQ(ErrorCode::kIo, committed.error().code());
  EXPECT_EQ(0U, environment.main->write_count);
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_FALSE(environment.JournalPresent());
}

TEST(WritePager, PromotesTheSoleReadPinWithoutEvictionOrReread) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment, 0);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  {
    auto read = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(read.has_value());
    const PageFrame* const frame = &read->frame();
    const std::size_t reads = environment.main->read_count;

    auto write = pager->WritePage(std::move(*read));

    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(frame, &write->frame());
    EXPECT_EQ(reads, environment.main->read_count);
    write->mutable_bytes()[100] = std::byte{0x6a};
  }
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, RejectsReadPinPromotionWhileAnotherPinExists) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  {
    auto first = pager->ReadPage(PageNumber{2});
    auto second = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    const auto promoted = pager->WritePage(std::move(*first));

    ASSERT_FALSE(promoted.has_value());
    EXPECT_EQ(ErrorCode::kBusy, promoted.error().code());
    EXPECT_EQ(PageNumber{2}, second->frame().page_number());
  }
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, PromotedReadPinJournalFailureReleasesTheCleanFrame) {
  WritableEnvironment environment;
  const std::shared_ptr<MemoryFileState> journal = environment.PrepareJournalFile();
  journal->failing_write_attempt = 2;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  const PageFrame* frame = nullptr;
  std::size_t reads = 0;
  {
    auto read = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(read.has_value());
    frame = &read->frame();
    reads = environment.main->read_count;

    const auto promoted = pager->WritePage(std::move(*read));

    ASSERT_FALSE(promoted.has_value());
    EXPECT_EQ(ErrorCode::kIo, promoted.error().code());
  }
  {
    auto read = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(frame, &read->frame());
    EXPECT_FALSE(read->frame().dirty());
    EXPECT_EQ(std::byte{2}, read->frame().bytes()[100]);
    EXPECT_EQ(reads, environment.main->read_count);
  }
  EXPECT_EQ(ErrorCode::kIo, pager->write_failure_code());
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_FALSE(environment.JournalPresent());
}

TEST(WritePager, RejectsAConsumedReadPinWithoutDereferencingIt) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  auto read = pager->ReadPage(PageNumber{2});
  ASSERT_TRUE(read.has_value());
  {
    const auto promoted = pager->WritePage(std::move(*read));
    ASSERT_TRUE(promoted.has_value());
  }

  const auto repeated = pager->WritePage(std::move(*read));

  ASSERT_FALSE(repeated.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, repeated.error().code());
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, RejectsForeignPermutationPinsBeforeOpeningTheJournal) {
  WritableEnvironment environment{MakeDatabaseImage(3)};
  WritableEnvironment foreign_environment{MakeDatabaseImage(3)};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  std::unique_ptr<Pager> foreign_pager = OpenWritable(foreign_environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_NE(nullptr, foreign_pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  ASSERT_TRUE(foreign_pager->BeginRead().has_value());
  ASSERT_TRUE(foreign_pager->BeginWrite().has_value());

  {
    auto second = foreign_pager->WritePage(PageNumber{2});
    auto third = foreign_pager->WritePage(PageNumber{3});
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    std::array<PageNumberRekey, 2> rekeys{
        PageNumberRekey{.pin = &*second, .final_page = PageNumber{3}},
        PageNumberRekey{.pin = &*third, .final_page = PageNumber{2}},
    };

    const auto permuted = pager->PermutePageNumbers(rekeys);

    ASSERT_FALSE(permuted.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, permuted.error().code());
    EXPECT_EQ(PageNumber{2}, second->frame().page_number());
    EXPECT_EQ(PageNumber{3}, third->frame().page_number());
    EXPECT_FALSE(environment.JournalPresent());
  }
  ASSERT_TRUE(foreign_pager->Rollback().has_value());
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, RejectsInvalidPermutationWithoutChangingPageNumbers) {
  WritableEnvironment environment{MakeDatabaseImage(3)};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  {
    auto second = pager->WritePage(PageNumber{2});
    auto third = pager->WritePage(PageNumber{3});
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    std::array<PageNumberRekey, 2> rekeys{
        PageNumberRekey{.pin = &*second, .final_page = PageNumber{3}},
        PageNumberRekey{.pin = &*third, .final_page = PageNumber{3}},
    };

    const auto permuted = pager->PermutePageNumbers(rekeys);

    ASSERT_FALSE(permuted.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, permuted.error().code());
    EXPECT_EQ(PageNumber{2}, second->frame().page_number());
    EXPECT_EQ(PageNumber{3}, third->frame().page_number());
  }
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, RejectsASinglePageMoveThatIsNotAPermutation) {
  WritableEnvironment environment{MakeDatabaseImage(3)};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  {
    auto second = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(second.has_value());
    std::array<PageNumberRekey, 1> rekeys{
        PageNumberRekey{.pin = &*second, .final_page = PageNumber{3}},
    };

    const auto permuted = pager->PermutePageNumbers(rekeys);

    ASSERT_FALSE(permuted.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, permuted.error().code());
    EXPECT_EQ(PageNumber{2}, second->frame().page_number());
  }
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, PermutesCapturedDirtyPageNumbersAndSavepointRestoresThem) {
  WritableEnvironment environment{MakeDatabaseImage(3)};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());

  {
    auto second = pager->WritePage(PageNumber{2});
    auto third = pager->WritePage(PageNumber{3});
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    second->mutable_bytes()[100] = std::byte{0x22};
    third->mutable_bytes()[100] = std::byte{0x33};
    std::array<PageNumberRekey, 2> rekeys{
        PageNumberRekey{.pin = &*second, .final_page = PageNumber{3}},
        PageNumberRekey{.pin = &*third, .final_page = PageNumber{2}},
    };

    ASSERT_TRUE(pager->PermutePageNumbers(rekeys).has_value());
    EXPECT_EQ(PageNumber{3}, second->frame().page_number());
    EXPECT_EQ(PageNumber{2}, third->frame().page_number());
    EXPECT_EQ(std::byte{0x22}, second->frame().bytes()[100]);
    EXPECT_EQ(std::byte{0x33}, third->frame().bytes()[100]);
  }

  {
    auto second = pager->ReadPage(PageNumber{2});
    auto third = pager->ReadPage(PageNumber{3});
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(std::byte{0x33}, second->frame().bytes()[100]);
    EXPECT_EQ(std::byte{0x22}, third->frame().bytes()[100]);
  }
  ASSERT_TRUE(pager->RollbackToSavepoint(*savepoint).has_value());
  {
    auto second = pager->ReadPage(PageNumber{2});
    auto third = pager->ReadPage(PageNumber{3});
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(std::byte{2}, second->frame().bytes()[100]);
    EXPECT_EQ(std::byte{3}, third->frame().bytes()[100]);
  }
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, PermutesAThreePageCycleWithoutExposingTheLockingSentinel) {
  WritableEnvironment environment{MakeDatabaseImage(4)};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  {
    auto second = pager->WritePage(PageNumber{2});
    auto third = pager->WritePage(PageNumber{3});
    auto fourth = pager->WritePage(PageNumber{4});
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    ASSERT_TRUE(fourth.has_value());
    second->mutable_bytes()[100] = std::byte{0x22};
    third->mutable_bytes()[100] = std::byte{0x33};
    fourth->mutable_bytes()[100] = std::byte{0x44};
    std::array<PageNumberRekey, 3> rekeys{
        PageNumberRekey{.pin = &*second, .final_page = PageNumber{3}},
        PageNumberRekey{.pin = &*third, .final_page = PageNumber{4}},
        PageNumberRekey{.pin = &*fourth, .final_page = PageNumber{2}},
    };

    ASSERT_TRUE(pager->PermutePageNumbers(rekeys).has_value());
    EXPECT_EQ(PageNumber{3}, second->frame().page_number());
    EXPECT_EQ(PageNumber{4}, third->frame().page_number());
    EXPECT_EQ(PageNumber{2}, fourth->frame().page_number());
  }
  {
    auto second = pager->ReadPage(PageNumber{2});
    auto third = pager->ReadPage(PageNumber{3});
    auto fourth = pager->ReadPage(PageNumber{4});
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    ASSERT_TRUE(fourth.has_value());
    EXPECT_EQ(std::byte{0x44}, second->frame().bytes()[100]);
    EXPECT_EQ(std::byte{0x22}, third->frame().bytes()[100]);
    EXPECT_EQ(std::byte{0x33}, fourth->frame().bytes()[100]);
  }
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, ContentHistorySurvivesSavepointRollbackAndClearsWithOuterRollback) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  ASSERT_TRUE(pager->MarkPageContentRequired(PageNumber{2}).has_value());
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  ASSERT_TRUE(pager->MarkPageContentRequired(PageNumber{1}).has_value());

  ASSERT_TRUE(pager->RollbackToSavepoint(*savepoint).has_value());
  EXPECT_TRUE(pager->PageContentRequired(PageNumber{1}));
  EXPECT_TRUE(pager->PageContentRequired(PageNumber{2}));
  ASSERT_TRUE(pager->Rollback().has_value());

  ASSERT_TRUE(pager->BeginWrite().has_value());
  EXPECT_FALSE(pager->PageContentRequired(PageNumber{1}));
  EXPECT_FALSE(pager->PageContentRequired(PageNumber{2}));
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, JournalSyncFailurePermitsOnlyRollbackAndDoesNotWriteTheDatabase) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  environment.JournalFile()->sync_failures_remaining = 1;

  const auto committed = pager->Commit();

  ASSERT_FALSE(committed.has_value());
  EXPECT_EQ(ErrorCode::kIo, committed.error().code());
  const auto write = pager->WritePage(PageNumber{1});
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(ErrorCode::kIo, write.error().code());
  EXPECT_EQ(0U, environment.main->write_count);
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(PagerState::kReader, pager->state());
}

TEST(WritePager, DatabaseSyncFailureRollsBackEveryWrittenPage) {
  WritableEnvironment environment;
  const std::vector<std::byte> original = environment.main->bytes;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  environment.main->sync_failures_remaining = 1;

  const auto committed = pager->Commit();

  ASSERT_FALSE(committed.has_value());
  EXPECT_EQ(ErrorCode::kIo, committed.error().code());
  ASSERT_NE(original, environment.main->bytes);
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(original, environment.main->bytes);
  EXPECT_EQ(PagerState::kReader, pager->state());
}

TEST(WritePager, CommitJournalDeletionFailureEntersPersistentError) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  environment.state->failing_delete = std::string{kCanonicalPath} + "-journal";
  environment.state->delete_failures_remaining = 1;

  const auto committed = pager->Commit();

  ASSERT_FALSE(committed.has_value());
  EXPECT_EQ(ErrorCode::kIo, committed.error().code());
  EXPECT_EQ(PagerState::kError, pager->state());
  EXPECT_TRUE(environment.JournalPresent());
  const auto rollback = pager->Rollback();
  ASSERT_FALSE(rollback.has_value());
  EXPECT_EQ(ErrorCode::kIo, rollback.error().code());
}

TEST(WritePager, RollbackJournalDeletionFailureEntersPersistentError) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x7f};
  }
  environment.state->failing_delete = std::string{kCanonicalPath} + "-journal";
  environment.state->delete_failures_remaining = 1;

  const auto rolled_back = pager->Rollback();

  ASSERT_FALSE(rolled_back.has_value());
  EXPECT_EQ(ErrorCode::kIo, rolled_back.error().code());
  EXPECT_EQ(PagerState::kError, pager->state());
  EXPECT_TRUE(environment.JournalPresent());
}

TEST(WritePager, PostCommitTruncateFailureRetriesOnlyCleanup) {
  WritableEnvironment environment{MakeDatabaseImage(3)};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto header = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(header.has_value());
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{header->mutable_bytes().data() + 28, 4},
                                  2);
  }
  ASSERT_TRUE(pager->TruncateImage(2).has_value());
  environment.main->truncate_failures_remaining = 1;

  const auto first = pager->Commit();

  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(ErrorCode::kIo, first.error().code());
  EXPECT_EQ(PagerState::kWriterFinished, pager->state());
  EXPECT_FALSE(environment.JournalPresent());
  const std::size_t writes = environment.main->write_count;
  const std::size_t syncs = environment.main->sync_count;
  ASSERT_TRUE(pager->Commit().has_value());
  EXPECT_EQ(writes, environment.main->write_count);
  EXPECT_EQ(syncs, environment.main->sync_count);
  EXPECT_EQ(kPageSize * 2U, environment.main->bytes.size());
}

TEST(WritePager, HotRecoveryContentionReleasesSharedLockBeforeRetry) {
  WritableEnvironment environment;
  {
    std::unique_ptr<Pager> crashed = OpenWritable(environment, 1);
    ASSERT_NE(nullptr, crashed);
    ASSERT_TRUE(crashed->BeginRead().has_value());
    ASSERT_TRUE(crashed->BeginWrite().has_value());
    {
      auto writable = crashed->WritePage(PageNumber{2});
      ASSERT_TRUE(writable.has_value());
      writable->mutable_bytes()[100] = std::byte{0x7f};
    }
    {
      const auto writable = crashed->WritePage(PageNumber{1});
      ASSERT_TRUE(writable.has_value());
    }
    {
      const auto trigger = crashed->ReadPage(PageNumber{1});
      ASSERT_TRUE(trigger.has_value());
    }
  }
  environment.main->failing_lock = DatabaseLock::kExclusive;
  environment.main->lock_failures_remaining = 1;
  std::unique_ptr<Pager> recovered = OpenWritable(environment);
  ASSERT_NE(nullptr, recovered);

  const auto first = recovered->BeginRead();

  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(ErrorCode::kBusy, first.error().code());
  EXPECT_EQ(DatabaseLock::kNone, environment.main->lock);
  EXPECT_EQ(PagerState::kOpen, recovered->state());
  ASSERT_TRUE(recovered->BeginRead().has_value());
  EXPECT_FALSE(environment.JournalPresent());
  EXPECT_EQ(std::byte{2}, environment.main->bytes[kPageSize + 100U]);
}

TEST(WritePager, RepeatedLargeSectorWritesDoNotReloadProtectedNeighbors) {
  WritableEnvironment environment{MakeDatabaseImage(8)};
  environment.main->properties.sector_size = ByteCount{4096};
  std::unique_ptr<Pager> pager = OpenWritable(environment, 1);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[100] = std::byte{0x33};
  }
  const std::size_t reads_after_capture = environment.main->read_count;

  {
    auto writable = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(writable.has_value());
    writable->mutable_bytes()[101] = std::byte{0x44};
  }

  EXPECT_EQ(reads_after_capture, environment.main->read_count);
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, CommitsLogicalGrowthWhenPageOneMatchesTheNewImageSize) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto appended = pager->AllocatePage();
    ASSERT_TRUE(appended.has_value());
    appended->mutable_bytes()[100] = std::byte{0x7f};
  }
  {
    auto header = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(header.has_value());
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{header->mutable_bytes().data() + 28, 4},
                                  3);
  }

  ASSERT_TRUE(pager->Commit().has_value());

  EXPECT_EQ(3U, pager->page_count());
  EXPECT_EQ(kPageSize * 3U, environment.main->bytes.size());
  EXPECT_EQ(3U, LoadBigEndian<std::uint32_t>(
                    std::span<const std::byte, 4>{environment.main->bytes.data() + 28, 4}));
  EXPECT_EQ(std::byte{0x7f}, environment.main->bytes[kPageSize * 2U + 100U]);
  ASSERT_TRUE(pager->EndRead().has_value());
  ASSERT_TRUE(pager->BeginRead().has_value());
  auto appended = pager->ReadPage(PageNumber{3});
  ASSERT_TRUE(appended.has_value());
  EXPECT_EQ(std::byte{0x7f}, appended->frame().bytes()[100]);
}

TEST(WritePager, SpilledGrowthRollbackRestoresTheOriginalPhysicalImage) {
  WritableEnvironment environment;
  const std::vector<std::byte> original = environment.main->bytes;
  std::unique_ptr<Pager> pager = OpenWritable(environment, 1);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto appended = pager->AllocatePage();
    ASSERT_TRUE(appended.has_value());
    appended->mutable_bytes()[100] = std::byte{0x7f};
  }
  {
    const auto header = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(header.has_value());
  }
  {
    const auto trigger = pager->ReadPage(PageNumber{1});
    ASSERT_TRUE(trigger.has_value());
  }
  ASSERT_EQ(kPageSize * 3U, environment.main->bytes.size());

  ASSERT_TRUE(pager->Rollback().has_value());

  EXPECT_EQ(original, environment.main->bytes);
  EXPECT_EQ(2U, pager->page_count());
}

TEST(WritePager, TerminalShrinkRollbackRestoresTheOriginalImage) {
  WritableEnvironment environment{MakeDatabaseImage(3)};
  const std::vector<std::byte> original = environment.main->bytes;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto header = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(header.has_value());
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{header->mutable_bytes().data() + 28, 4},
                                  2);
  }
  ASSERT_TRUE(pager->TruncateImage(2).has_value());

  ASSERT_TRUE(pager->Rollback().has_value());

  EXPECT_EQ(original, environment.main->bytes);
  EXPECT_EQ(3U, pager->page_count());
  auto tail = pager->ReadPage(PageNumber{3});
  ASSERT_TRUE(tail.has_value());
  EXPECT_EQ(std::byte{3}, tail->frame().bytes()[100]);
}

TEST(WritePager, FinalImageRejectsEveryOperationExceptCommitOrFullRollback) {
  WritableEnvironment environment{MakeDatabaseImage(3)};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  {
    auto header = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(header.has_value());
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{header->mutable_bytes().data() + 28, 4},
                                  2);
  }
  ASSERT_TRUE(pager->TruncateImage(2).has_value());

  const auto write = pager->WritePage(PageNumber{1});
  const auto allocate = pager->AllocatePage();
  const auto create = pager->CreateSavepoint();
  const auto release = pager->ReleaseSavepoint(*savepoint);
  const auto savepoint_rollback = pager->RollbackToSavepoint(*savepoint);
  const auto truncate = pager->TruncateImage(1);

  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, write.error().code());
  ASSERT_FALSE(allocate.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, allocate.error().code());
  ASSERT_FALSE(create.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, create.error().code());
  ASSERT_FALSE(release.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, release.error().code());
  ASSERT_FALSE(savepoint_rollback.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, savepoint_rollback.error().code());
  ASSERT_FALSE(truncate.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, truncate.error().code());
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, LivePinsBlockCommitRollbackAndSavepointRollback) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  std::optional<WritePagePin> writable;
  {
    auto acquired = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(acquired.has_value());
    writable.emplace(std::move(*acquired));
  }

  const auto commit = pager->Commit();
  const auto rollback = pager->Rollback();
  const auto savepoint_rollback = pager->RollbackToSavepoint(*savepoint);

  ASSERT_FALSE(commit.has_value());
  EXPECT_EQ(ErrorCode::kBusy, commit.error().code());
  ASSERT_FALSE(rollback.has_value());
  EXPECT_EQ(ErrorCode::kBusy, rollback.error().code());
  ASSERT_FALSE(savepoint_rollback.has_value());
  EXPECT_EQ(ErrorCode::kBusy, savepoint_rollback.error().code());
  writable.reset();
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, SavepointCreationAllowsReadPinsButRejectsWritePins) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    const auto read = pager->ReadPage(PageNumber{1});
    ASSERT_TRUE(read.has_value());
    ASSERT_TRUE(pager->CreateSavepoint().has_value());
  }
  std::optional<WritePagePin> write;
  {
    auto acquired = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(acquired.has_value());
    write.emplace(std::move(*acquired));
  }

  const auto blocked = pager->CreateSavepoint();

  ASSERT_FALSE(blocked.has_value());
  EXPECT_EQ(ErrorCode::kBusy, blocked.error().code());
  write.reset();
  ASSERT_TRUE(pager->CreateSavepoint().has_value());
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, NestedSavepointRollbackRestoresEachCapturedGeneration) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto page = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    page->mutable_bytes()[100] = std::byte{0x11};
  }
  const auto outer = pager->CreateSavepoint();
  ASSERT_TRUE(outer.has_value());
  {
    auto page = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    page->mutable_bytes()[100] = std::byte{0x22};
  }
  const auto inner = pager->CreateSavepoint();
  ASSERT_TRUE(inner.has_value());
  {
    auto page = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    page->mutable_bytes()[100] = std::byte{0x33};
  }

  ASSERT_TRUE(pager->RollbackToSavepoint(*inner).has_value());
  {
    auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{0x22}, page->frame().bytes()[100]);
  }
  ASSERT_TRUE(pager->RollbackToSavepoint(*outer).has_value());
  {
    auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{0x11}, page->frame().bytes()[100]);
  }
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(std::byte{2}, environment.main->bytes[kPageSize + 100U]);
}

TEST(WritePager, SavepointRollbackReappliesOneChangeCounterUpdateAfterCommitRetry) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto page = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    page->mutable_bytes()[100] = std::byte{0x7f};
  }
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  environment.main->failing_lock = DatabaseLock::kExclusive;
  environment.main->lock_failures_remaining = 1;
  ASSERT_FALSE(pager->Commit().has_value());

  ASSERT_TRUE(pager->RollbackToSavepoint(*savepoint).has_value());
  ASSERT_TRUE(pager->Commit().has_value());

  EXPECT_EQ(8U, LoadBigEndian<std::uint32_t>(
                    std::span<const std::byte, 4>{environment.main->bytes.data() + 24, 4}));
}

TEST(WritePager, RollbackCleanupFailureRetriesOnlyTheLockDowngrade) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto page = pager->WritePage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    page->mutable_bytes()[100] = std::byte{0x7f};
  }
  environment.main->failing_unlock = DatabaseLock::kShared;
  environment.main->unlock_failures_remaining = 1;

  const auto first = pager->Rollback();

  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(ErrorCode::kIo, first.error().code());
  EXPECT_EQ(PagerState::kWriterFinished, pager->state());
  EXPECT_FALSE(environment.JournalPresent());
  const std::size_t writes = environment.main->write_count;
  const std::size_t syncs = environment.main->sync_count;
  const auto commit = pager->Commit();
  ASSERT_FALSE(commit.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, commit.error().code());
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(writes, environment.main->write_count);
  EXPECT_EQ(syncs, environment.main->sync_count);
  EXPECT_EQ(PagerState::kReader, pager->state());
}

TEST(WritePager, CreatesAndCommitsAValidPageOneFromAnEmptyDatabase) {
  WritableEnvironment environment{{}};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(0U, pager->page_count());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto page_one = pager->AllocatePage();
    ASSERT_TRUE(page_one.has_value());
    ASSERT_EQ(PageNumber{1}, page_one->frame().page_number());
    const std::vector<std::byte> image = MakeDatabaseImage(1);
    std::ranges::copy(image, page_one->mutable_bytes().begin());
  }

  ASSERT_TRUE(pager->Commit().has_value());

  EXPECT_EQ(kPageSize, environment.main->bytes.size());
  EXPECT_EQ(1U, pager->page_count());
  EXPECT_EQ(8U, LoadBigEndian<std::uint32_t>(
                    std::span<const std::byte, 4>{environment.main->bytes.data() + 24, 4}));
  ASSERT_TRUE(pager->EndRead().has_value());
  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(1U, pager->page_count());
}

TEST(WritePager, ReservedLockContentionLeavesTheReaderRetryable) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  environment.main->failing_lock = DatabaseLock::kReserved;
  environment.main->lock_failures_remaining = 1;

  const auto first = pager->BeginWrite();

  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(ErrorCode::kBusy, first.error().code());
  EXPECT_EQ(PagerState::kReader, pager->state());
  EXPECT_EQ(DatabaseLock::kShared, environment.main->lock);
  EXPECT_FALSE(environment.JournalPresent());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, SavepointOnlyCommitFinalizesWithoutDatabaseIo) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  ASSERT_TRUE(pager->CreateSavepoint().has_value());
  ASSERT_TRUE(environment.JournalPresent());

  ASSERT_TRUE(pager->Commit().has_value());

  EXPECT_FALSE(environment.JournalPresent());
  EXPECT_EQ(0U, environment.main->write_count);
  EXPECT_EQ(0U, environment.main->sync_count);
  EXPECT_EQ(0U, environment.main->truncate_count);
  EXPECT_EQ(PagerState::kReader, pager->state());
}

TEST(WritePager, GrowthCommitMismatchSealsTheGenerationUntilRollback) {
  WritableEnvironment environment;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    const auto page = pager->AllocatePage();
    ASSERT_TRUE(page.has_value());
  }

  const auto invalid = pager->Commit();

  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid.error().code());
  EXPECT_EQ(0U, environment.main->write_count);
  const auto header = pager->WritePage(PageNumber{1});
  ASSERT_FALSE(header.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, header.error().code());
  const auto retried = pager->Commit();
  ASSERT_FALSE(retried.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, retried.error().code());
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(kPageSize * 2U, environment.main->bytes.size());
}

TEST(WritePager, CommitRejectsPageSizeChangesOutsideTheActiveGeometry) {
  WritableEnvironment environment;
  const std::vector<std::byte> original = environment.main->bytes;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto header = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(header.has_value());
    header->mutable_bytes()[16] = std::byte{0x04};
    header->mutable_bytes()[17] = std::byte{0x00};
  }

  const auto committed = pager->Commit();

  ASSERT_FALSE(committed.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, committed.error().code());
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(original, environment.main->bytes);
}

TEST(WritePager, SavepointRollbackRejectsAnIncompatibleRestoredPageSize) {
  WritableEnvironment environment;
  const std::vector<std::byte> original = environment.main->bytes;
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());
  {
    auto header = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(header.has_value());
    header->mutable_bytes()[16] = std::byte{0x04};
    header->mutable_bytes()[17] = std::byte{0x00};
  }
  const auto savepoint = pager->CreateSavepoint();
  ASSERT_TRUE(savepoint.has_value());
  {
    auto header = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(header.has_value());
    header->mutable_bytes()[68] = std::byte{0x7f};
  }

  const auto restored = pager->RollbackToSavepoint(*savepoint);

  ASSERT_FALSE(restored.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, restored.error().code());
  ASSERT_TRUE(pager->Rollback().has_value());
  EXPECT_EQ(original, environment.main->bytes);
}

TEST(WritePager, TruncateImageRejectsMissingOrMismatchedPageOneAndZeroSize) {
  WritableEnvironment environment{MakeDatabaseImage(3)};
  std::unique_ptr<Pager> pager = OpenWritable(environment);
  ASSERT_NE(nullptr, pager);
  ASSERT_TRUE(pager->BeginRead().has_value());
  ASSERT_TRUE(pager->BeginWrite().has_value());

  const auto clean_header = pager->TruncateImage(2);
  ASSERT_FALSE(clean_header.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, clean_header.error().code());
  {
    auto header = pager->WritePage(PageNumber{1});
    ASSERT_TRUE(header.has_value());
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{header->mutable_bytes().data() + 28, 4},
                                  1);
  }
  const auto mismatch = pager->TruncateImage(2);
  ASSERT_FALSE(mismatch.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, mismatch.error().code());
  const auto zero = pager->TruncateImage(0);
  ASSERT_FALSE(zero.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, zero.error().code());
  ASSERT_TRUE(pager->Rollback().has_value());
}

TEST(WritePager, PosixVfsCommitsAndRecoversAHotJournal) {
  const TemporaryDirectory directory;
  ASSERT_TRUE(directory.valid());
  const std::filesystem::path database_path = directory.DatabasePath();
  const std::vector<std::byte> original = MakeDatabaseImage();
  ASSERT_TRUE(WriteBytes(database_path, original));

  PosixVfs vfs;
  {
    auto opened = Pager::OpenWritable(vfs, database_path.string(),
                                      WritablePagerOptions{
                                          .pager =
                                              PagerOptions{
                                                  .empty_database_page_size = ByteCount{kPageSize},
                                                  .cache_capacity_pages = 4,
                                              },
                                          .journal =
                                              RollbackJournalOptions{
                                                  .legacy_page_size = ByteCount{kPageSize},
                                              },
                                      });
    ASSERT_TRUE(opened.has_value());
    std::unique_ptr<Pager> pager = std::move(*opened);
    ASSERT_TRUE(pager->BeginRead().has_value());
    ASSERT_TRUE(pager->BeginWrite().has_value());
    {
      auto page = pager->WritePage(PageNumber{2});
      ASSERT_TRUE(page.has_value());
      page->mutable_bytes()[100] = std::byte{0x44};
    }
    ASSERT_TRUE(pager->Commit().has_value());
    ASSERT_TRUE(pager->EndRead().has_value());
  }
  EXPECT_FALSE(std::filesystem::exists(database_path.string() + "-journal"));
  {
    auto opened = Pager::Open(vfs, database_path.string(),
                              PagerOptions{
                                  .empty_database_page_size = ByteCount{kPageSize},
                                  .cache_capacity_pages = 4,
                              });
    ASSERT_TRUE(opened.has_value());
    std::unique_ptr<Pager> pager = std::move(*opened);
    ASSERT_TRUE(pager->BeginRead().has_value());
    auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{0x44}, page->frame().bytes()[100]);
  }

  ASSERT_TRUE(WriteBytes(database_path, original));
  {
    auto opened = Pager::OpenWritable(vfs, database_path.string(),
                                      WritablePagerOptions{
                                          .pager =
                                              PagerOptions{
                                                  .empty_database_page_size = ByteCount{kPageSize},
                                                  .cache_capacity_pages = 1,
                                              },
                                          .journal =
                                              RollbackJournalOptions{
                                                  .legacy_page_size = ByteCount{kPageSize},
                                              },
                                      });
    ASSERT_TRUE(opened.has_value());
    std::unique_ptr<Pager> crashed = std::move(*opened);
    ASSERT_TRUE(crashed->BeginRead().has_value());
    ASSERT_TRUE(crashed->BeginWrite().has_value());
    {
      auto page = crashed->WritePage(PageNumber{2});
      ASSERT_TRUE(page.has_value());
      page->mutable_bytes()[100] = std::byte{0x7f};
    }
    {
      const auto page = crashed->WritePage(PageNumber{1});
      ASSERT_TRUE(page.has_value());
    }
    {
      const auto trigger = crashed->ReadPage(PageNumber{1});
      ASSERT_TRUE(trigger.has_value());
    }
    ASSERT_TRUE(std::filesystem::exists(database_path.string() + "-journal"));
  }
  {
    auto opened = Pager::OpenWritable(vfs, database_path.string(),
                                      WritablePagerOptions{
                                          .pager =
                                              PagerOptions{
                                                  .empty_database_page_size = ByteCount{kPageSize},
                                                  .cache_capacity_pages = 4,
                                              },
                                          .journal =
                                              RollbackJournalOptions{
                                                  .legacy_page_size = ByteCount{kPageSize},
                                              },
                                      });
    ASSERT_TRUE(opened.has_value());
    std::unique_ptr<Pager> recovered = std::move(*opened);
    ASSERT_TRUE(recovered->BeginRead().has_value());
    auto page = recovered->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{2}, page->frame().bytes()[100]);
  }
  EXPECT_FALSE(std::filesystem::exists(database_path.string() + "-journal"));
}

TEST(WritePager, EveryOnePageCommitCrashCutRecoversTheOldOrCommittedImage) {
  WritableEnvironment baseline;
  baseline.main->writes_are_durable = true;
  ASSERT_TRUE(CommitOnePageChange(baseline));
  const std::vector<std::byte> committed = baseline.main->bytes;
  const std::size_t mutation_count = baseline.state->mutation_count;
  ASSERT_GT(mutation_count, 0U);

  const std::vector<std::byte> original = MakeDatabaseImage();
  for (std::size_t cut = 1; cut <= mutation_count; ++cut) {
    SCOPED_TRACE(cut);
    WritableEnvironment environment;
    environment.main->writes_are_durable = true;
    environment.state->fail_after_mutation = cut;

    static_cast<void>(CommitOnePageChange(environment));

    ASSERT_TRUE(environment.state->mutation_cut_triggered);
    environment.Crash();
    environment.state->mutation_count = 0;
    ASSERT_TRUE(RecoverWritableImage(environment));
    EXPECT_TRUE(environment.main->bytes == original || environment.main->bytes == committed);
    if (environment.JournalPresent()) {
      const std::vector<std::byte>& journal = environment.JournalFile()->bytes;
      ASSERT_FALSE(journal.empty());
      EXPECT_EQ(std::byte{0}, journal.front());
    }
  }
}

TEST(WritePager, EverySpilledCommitCrashCutRecoversTheOldOrCommittedImage) {
  WritableEnvironment baseline;
  baseline.main->writes_are_durable = true;
  ASSERT_TRUE(CommitSpilledChange(baseline));
  const std::vector<std::byte> committed = baseline.main->bytes;
  const std::size_t mutation_count = baseline.state->mutation_count;
  ASSERT_GT(mutation_count, 0U);

  const std::vector<std::byte> original = MakeDatabaseImage();
  for (std::size_t cut = 1; cut <= mutation_count; ++cut) {
    SCOPED_TRACE(cut);
    WritableEnvironment environment;
    environment.main->writes_are_durable = true;
    environment.state->fail_after_mutation = cut;

    static_cast<void>(CommitSpilledChange(environment));

    ASSERT_TRUE(environment.state->mutation_cut_triggered);
    environment.Crash();
    environment.state->mutation_count = 0;
    ASSERT_TRUE(RecoverWritableImage(environment));
    EXPECT_TRUE(environment.main->bytes == original || environment.main->bytes == committed);
  }
}

TEST(WritePager, EveryGrowthCommitCrashCutRecoversTheOldOrCommittedImage) {
  WritableEnvironment baseline;
  baseline.main->writes_are_durable = true;
  ASSERT_TRUE(CommitGrowth(baseline));
  const std::vector<std::byte> committed = baseline.main->bytes;
  const std::size_t mutation_count = baseline.state->mutation_count;
  ASSERT_GT(mutation_count, 0U);

  const std::vector<std::byte> original = MakeDatabaseImage();
  for (std::size_t cut = 1; cut <= mutation_count; ++cut) {
    SCOPED_TRACE(cut);
    WritableEnvironment environment;
    environment.main->writes_are_durable = true;
    environment.state->fail_after_mutation = cut;

    static_cast<void>(CommitGrowth(environment));

    ASSERT_TRUE(environment.state->mutation_cut_triggered);
    environment.Crash();
    environment.state->mutation_count = 0;
    ASSERT_TRUE(RecoverWritableImage(environment));
    EXPECT_TRUE(environment.main->bytes == original || environment.main->bytes == committed);
  }
}

TEST(WritePager, EveryShrinkCommitCrashCutRecoversTheOldOrLogicalCommittedImage) {
  WritableEnvironment baseline{MakeDatabaseImage(3)};
  baseline.main->writes_are_durable = true;
  ASSERT_TRUE(CommitShrink(baseline));
  const std::vector<std::byte> committed = baseline.main->bytes;
  const std::size_t mutation_count = baseline.state->mutation_count;
  ASSERT_GT(mutation_count, 0U);

  const std::vector<std::byte> original = MakeDatabaseImage(3);
  for (std::size_t cut = 1; cut <= mutation_count; ++cut) {
    SCOPED_TRACE(cut);
    WritableEnvironment environment{original};
    environment.main->writes_are_durable = true;
    environment.state->fail_after_mutation = cut;

    static_cast<void>(CommitShrink(environment));

    ASSERT_TRUE(environment.state->mutation_cut_triggered);
    environment.Crash();
    environment.state->mutation_count = 0;
    ASSERT_TRUE(RecoverWritableImage(environment));
    const bool old_image = environment.main->bytes == original;
    const bool committed_image =
        environment.main->bytes.size() >= committed.size() &&
        std::ranges::equal(
            committed, std::span<const std::byte>{environment.main->bytes}.first(committed.size()));
    EXPECT_TRUE(old_image || committed_image);
  }
}

TEST(WritePager, EverySpilledRollbackCrashCutRecoversTheOriginalImage) {
  WritableEnvironment baseline;
  baseline.main->writes_are_durable = true;
  ASSERT_TRUE(RollbackSpilledChange(baseline));
  const std::size_t mutation_count = baseline.state->mutation_count;
  ASSERT_GT(mutation_count, 0U);

  const std::vector<std::byte> original = MakeDatabaseImage();
  ASSERT_EQ(original, baseline.main->bytes);
  for (std::size_t cut = 1; cut <= mutation_count; ++cut) {
    SCOPED_TRACE(cut);
    WritableEnvironment environment;
    environment.main->writes_are_durable = true;
    environment.state->fail_after_mutation = cut;

    static_cast<void>(RollbackSpilledChange(environment));

    ASSERT_TRUE(environment.state->mutation_cut_triggered);
    environment.Crash();
    environment.state->mutation_count = 0;
    ASSERT_TRUE(RecoverWritableImage(environment));
    EXPECT_EQ(original, environment.main->bytes);
  }
}

}  // namespace
}  // namespace modern_sqlite
