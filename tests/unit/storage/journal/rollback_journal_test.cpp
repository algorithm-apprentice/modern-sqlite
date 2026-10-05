#include "modern_sqlite/storage/journal/rollback_journal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/platform/vfs.hpp"

namespace modern_sqlite {
namespace {

constexpr std::array<std::byte, 8> kJournalMagic{
    std::byte{0xd9}, std::byte{0xd5}, std::byte{0x05}, std::byte{0xf9},
    std::byte{0x20}, std::byte{0xa1}, std::byte{0x63}, std::byte{0xd7},
};

struct IoEvent {
  std::string operation;
  std::string path;
  std::uint64_t offset = 0;
  std::size_t size = 0;
  SyncOptions sync_options{};
  DirectorySync directory_sync = DirectorySync::kNo;
};

struct MemoryFileState {
  std::vector<std::byte> bytes;
  std::vector<std::byte> durable_bytes;
  FileProperties properties{
      .sector_size = ByteCount{4096},
      .device_characteristics = DeviceCharacteristics{},
  };
  std::string path;
  std::vector<IoEvent>* events = nullptr;
  std::size_t sync_calls = 0;
  std::optional<std::size_t> failing_sync_call;
  bool fail_publish_write = false;
  bool fail_next_read = false;
  std::optional<std::uint64_t> failing_read_offset;
  bool fail_next_size = false;
  bool fail_next_write = false;
  bool fail_next_truncate = false;
};

void RecordEvent(const MemoryFileState& state, std::string operation, std::uint64_t offset = 0,
                 std::size_t size = 0, SyncOptions sync_options = {},
                 DirectorySync directory_sync = DirectorySync::kNo) {
  state.events->push_back(IoEvent{
      .operation = std::move(operation),
      .path = state.path,
      .offset = offset,
      .size = size,
      .sync_options = sync_options,
      .directory_sync = directory_sync,
  });
}

class MemoryFile final : public File {
 public:
  explicit MemoryFile(std::shared_ptr<MemoryFileState> state) : state_(std::move(state)) {}

 protected:
  Result<ByteCount> DoReadAt(MutableByteView destination, FileOffset offset) override {
    RecordEvent(*state_, "read", offset.value(), destination.size());
    if (state_->fail_next_read) {
      state_->fail_next_read = false;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected memory-file read failure"));
    }
    if (state_->failing_read_offset == offset.value()) {
      state_->failing_read_offset.reset();
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected record read failure"));
    }
    if (offset.value() > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(
          Error::Create(ErrorCode::kTooLarge, "memory-file offset is too large"));
    }
    const auto start = static_cast<std::size_t>(offset.value());
    if (start >= state_->bytes.size()) {
      return ByteCount{0};
    }
    const std::size_t count = std::min(destination.size(), state_->bytes.size() - start);
    std::ranges::copy(ByteView{state_->bytes}.subspan(start, count), destination.begin());
    return ByteCount{count};
  }

  Status DoWriteAt(ByteView source, FileOffset offset) override {
    RecordEvent(*state_, "write", offset.value(), source.size());
    if (state_->fail_next_write) {
      state_->fail_next_write = false;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected memory-file write failure"));
    }
    if (offset.value() > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(
          Error::Create(ErrorCode::kTooLarge, "memory-file offset is too large"));
    }
    const auto start = static_cast<std::size_t>(offset.value());
    if (source.size() > std::numeric_limits<std::size_t>::max() - start) {
      return std::unexpected(Error::Create(ErrorCode::kTooLarge, "memory-file write is too large"));
    }
    const std::size_t end = start + source.size();
    if (state_->bytes.size() < end) {
      state_->bytes.resize(end);
    }
    if (state_->fail_publish_write && offset.value() == 0U && source.size() == 12U) {
      state_->fail_publish_write = false;
      state_->bytes[start] = source.front();
      return std::unexpected(
          Error::Create(ErrorCode::kIo, "injected torn journal-header publication"));
    }
    std::ranges::copy(source, state_->bytes.begin() + static_cast<std::ptrdiff_t>(start));
    return {};
  }

  Status DoTruncate(FileSize size) override {
    RecordEvent(*state_, "truncate", size.value());
    if (state_->fail_next_truncate) {
      state_->fail_next_truncate = false;
      return std::unexpected(
          Error::Create(ErrorCode::kIo, "injected memory-file truncate failure"));
    }
    if (size.value() > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(Error::Create(ErrorCode::kTooLarge, "memory-file size is too large"));
    }
    state_->bytes.resize(static_cast<std::size_t>(size.value()));
    return {};
  }

  Status DoSync(SyncOptions options) override {
    RecordEvent(*state_, "sync", 0, 0, options);
    ++state_->sync_calls;
    if (state_->failing_sync_call == state_->sync_calls) {
      state_->failing_sync_call.reset();
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected memory-file sync failure"));
    }
    state_->durable_bytes = state_->bytes;
    return {};
  }

  Result<FileSize> DoSize() override {
    RecordEvent(*state_, "size");
    if (state_->fail_next_size) {
      state_->fail_next_size = false;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected memory-file size failure"));
    }
    return FileSize{state_->bytes.size()};
  }

  Status DoLock(DatabaseLock) override { return {}; }
  Status DoUnlock(DatabaseLock) override { return {}; }
  Result<bool> DoHasReservedLock() override { return false; }
  [[nodiscard]] FileProperties DoProperties() const noexcept override { return state_->properties; }

  Result<std::optional<MutableByteView>> DoMapSharedMemory(SharedMemoryRegionIndex, ByteCount,
                                                           SharedMemoryMapMode) override {
    return std::optional<MutableByteView>{};
  }

  Status DoLockSharedMemory(SharedMemoryLockRange, SharedMemoryLockOperation,
                            SharedMemoryLockMode) override {
    return {};
  }

  void DoSharedMemoryBarrier() noexcept override {}
  Status DoUnmapSharedMemory(SharedMemoryUnmapMode) override { return {}; }

 private:
  std::shared_ptr<MemoryFileState> state_;
};

class MemoryVfs final : public Vfs {
 public:
  FileProperties journal_properties{
      .sector_size = ByteCount{8192},
      .device_characteristics = DeviceCharacteristics{}.With(DeviceCapability::kSequential),
  };
  std::vector<IoEvent> events;
  std::size_t open_calls = 0;
  std::optional<Error> open_error;
  std::optional<Error> delete_error;
  std::optional<Error> access_error;
  std::optional<Error> randomness_error;
  bool throw_bad_alloc_on_open = false;
  bool throw_bad_alloc_on_full_path = false;
  bool unlink_before_delete_error = false;

  [[nodiscard]] const std::vector<std::byte>& Bytes(std::string_view path) const {
    return files_.at(std::string{path})->bytes;
  }

  [[nodiscard]] std::vector<std::byte>& MutableBytes(std::string_view path) {
    return files_.at(std::string{path})->bytes;
  }

  [[nodiscard]] const std::vector<std::byte>& TemporaryBytes(std::size_t index) const {
    return temporary_files_.at(index)->bytes;
  }

  [[nodiscard]] std::vector<std::byte>& MutableTemporaryBytes(std::size_t index) {
    return temporary_files_.at(index)->bytes;
  }

  [[nodiscard]] bool HasFile(std::string_view path) const {
    return files_.contains(std::string{path});
  }

  void PutFile(std::string path, std::vector<std::byte> bytes = {}) {
    auto state = std::make_shared<MemoryFileState>();
    state->bytes = std::move(bytes);
    state->durable_bytes = state->bytes;
    state->properties = journal_properties;
    state->path = path;
    state->events = &events;
    files_.insert_or_assign(std::move(path), std::move(state));
  }

  void FailSync(std::string_view path, std::size_t relative_call) {
    auto& state = *files_.at(std::string{path});
    state.failing_sync_call = state.sync_calls + relative_call;
  }

  void FailPublishWrite(std::string_view path) {
    files_.at(std::string{path})->fail_publish_write = true;
  }

  void FailNextRead(std::string_view path) { files_.at(std::string{path})->fail_next_read = true; }

  void FailReadAt(std::string_view path, std::uint64_t offset) {
    files_.at(std::string{path})->failing_read_offset = offset;
  }

  void FailNextSize(std::string_view path) { files_.at(std::string{path})->fail_next_size = true; }

  void FailNextWrite(std::string_view path) {
    files_.at(std::string{path})->fail_next_write = true;
  }

  void FailNextTruncate(std::string_view path) {
    files_.at(std::string{path})->fail_next_truncate = true;
  }

  void Crash() {
    for (auto& [path, state] : files_) {
      static_cast<void>(path);
      state->bytes = state->durable_bytes;
    }
    for (const auto& state : temporary_files_) {
      state->bytes = state->durable_bytes;
    }
  }

  void ClearEvents() { events.clear(); }

 protected:
  Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                            FileOpenOptions options) override {
    ++open_calls;
    if (throw_bad_alloc_on_open) {
      throw_bad_alloc_on_open = false;
      throw std::bad_alloc{};
    }
    if (open_error.has_value()) {
      return std::unexpected(*open_error);
    }
    std::shared_ptr<MemoryFileState> state;
    if (path.has_value()) {
      const std::string key{*path};
      const auto found = files_.find(key);
      if (found == files_.end()) {
        if (!options.create) {
          return std::unexpected(Error::Create(ErrorCode::kNotFound, "memory file is absent"));
        }
        state = std::make_shared<MemoryFileState>();
        state->properties = journal_properties;
        state->path = key;
        state->events = &events;
        files_.emplace(key, state);
      } else {
        if (options.exclusive_create) {
          return std::unexpected(Error::Create(ErrorCode::kBusy, "memory file already exists"));
        }
        state = found->second;
      }
    } else {
      state = std::make_shared<MemoryFileState>();
      state->properties = journal_properties;
      state->path = "<temporary-" + std::to_string(temporary_sequence_++) + ">";
      state->events = &events;
      temporary_files_.push_back(state);
    }
    RecordEvent(*state, "open");
    return OpenedFile{
        .file = std::make_unique<MemoryFile>(std::move(state)),
        .access = FileAccessMode::kReadWrite,
    };
  }

  Status DoDelete(std::string_view path, DirectorySync directory_sync) override {
    const auto found = files_.find(std::string{path});
    if (found == files_.end()) {
      return std::unexpected(Error::Create(ErrorCode::kNotFound, "memory file is absent"));
    }
    RecordEvent(*found->second, "delete", 0, 0, {}, directory_sync);
    if (delete_error.has_value()) {
      if (unlink_before_delete_error) {
        files_.erase(found);
      }
      return std::unexpected(*delete_error);
    }
    files_.erase(found);
    return {};
  }

  Result<bool> DoAccess(std::string_view path, FileAccessQuery) override {
    if (access_error.has_value()) {
      return std::unexpected(*access_error);
    }
    return files_.contains(std::string{path});
  }

  Result<std::string> DoFullPath(std::string_view path) override {
    if (throw_bad_alloc_on_full_path) {
      throw_bad_alloc_on_full_path = false;
      throw std::bad_alloc{};
    }
    if (path.starts_with('/')) {
      return std::string{path};
    }
    return "/" + std::string{path};
  }

  Result<ByteCount> DoRandomBytes(MutableByteView output) override {
    if (randomness_error.has_value()) {
      return std::unexpected(*randomness_error);
    }
    constexpr std::array<std::byte, 4> kSeed{
        std::byte{0x12},
        std::byte{0x34},
        std::byte{0x56},
        std::byte{0x78},
    };
    if (output.size() != kSeed.size()) {
      return std::unexpected(Error::Create(ErrorCode::kInternal, "unexpected random request"));
    }
    std::ranges::copy(kSeed, output.begin());
    return ByteCount{output.size()};
  }

  Result<std::chrono::microseconds> DoSleepFor(std::chrono::microseconds duration) override {
    return duration;
  }

  Result<WallClockTime> DoCurrentTime() override { return WallClockTime{}; }
  [[nodiscard]] ByteCount DoMaximumPathLength() const noexcept override { return ByteCount{512}; }

 private:
  std::map<std::string, std::shared_ptr<MemoryFileState>, std::less<>> files_;
  std::vector<std::shared_ptr<MemoryFileState>> temporary_files_;
  std::size_t temporary_sequence_ = 0;
};

[[nodiscard]] FileProperties Properties(
    ByteCount sector_size, DeviceCharacteristics characteristics = DeviceCharacteristics{}) {
  return FileProperties{
      .sector_size = sector_size,
      .device_characteristics = characteristics,
  };
}

[[nodiscard]] std::unique_ptr<RollbackJournal> CreateJournal(
    MemoryVfs& vfs, FileProperties database_properties,
    RollbackJournalOptions options = RollbackJournalOptions{}) {
  auto created = RollbackJournal::Create(vfs, "database.sqlite", database_properties, options);
  if (!created.has_value()) {
    return nullptr;
  }
  return std::move(*created);
}

[[nodiscard]] std::unique_ptr<JournalTransaction> BeginTransaction(
    RollbackJournal& journal, ByteCount page_size = ByteCount{512},
    ByteCount sector_size = ByteCount{512}, std::uint32_t original_page_count = 3) {
  auto begun = JournalTransaction::Begin(journal, JournalTransactionInfo{
                                                      .page_size = page_size,
                                                      .sector_size = sector_size,
                                                      .original_page_count = original_page_count,
                                                  });
  if (!begun.has_value()) {
    return nullptr;
  }
  return std::move(*begun);
}

[[nodiscard]] std::uint32_t ReadU32(const std::vector<std::byte>& bytes, std::size_t offset) {
  return LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
      bytes.data() + offset, sizeof(std::uint32_t)});
}

[[nodiscard]] std::vector<std::byte> Page(std::byte seed) {
  std::vector<std::byte> page(512);
  for (std::size_t index = 0; index < page.size(); ++index) {
    page[index] = static_cast<std::byte>(
        static_cast<std::uint8_t>((std::to_integer<std::uint8_t>(seed) + index) % 251U));
  }
  return page;
}

[[nodiscard]] std::filesystem::path FixturePath(std::string_view filename) {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path().parent_path() /
         "fixtures" / "rollback_journal" / filename;
}

[[nodiscard]] std::vector<std::byte> ReadFixture(std::string_view filename) {
  const std::filesystem::path path = FixturePath(filename);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  input.seekg(0, std::ios::end);
  const std::streamoff end = input.tellg();
  if (end < 0) {
    return {};
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.read(reinterpret_cast<char*>(bytes.data()), end);
  if (!input && end != 0) {
    return {};
  }
  return bytes;
}

[[nodiscard]] std::size_t SyncCount(const MemoryVfs& vfs) {
  return static_cast<std::size_t>(std::ranges::count_if(
      vfs.events, [](const IoEvent& event) { return event.operation == "sync"; }));
}

class RecordingTarget final : public JournalRecoveryTarget {
 public:
  std::optional<JournalPlaybackInfo> prepared_info;
  std::optional<JournalPlaybackInfo> completed_info;
  std::optional<std::uint32_t> resized_page_count;
  std::vector<PageNumber> restored_pages;
  std::vector<std::vector<std::byte>> restored_images;
  std::size_t sync_count = 0;

  Status PreparePlayback(JournalPlaybackInfo info) override {
    prepared_info = info;
    return {};
  }

  Status ResizeDatabase(std::uint32_t page_count) override {
    resized_page_count = page_count;
    return {};
  }

  Status RestorePage(JournalPageImage image) override {
    restored_pages.push_back(image.page_number);
    restored_images.emplace_back(image.bytes.begin(), image.bytes.end());
    return {};
  }

  Status SyncDatabase() override {
    ++sync_count;
    return {};
  }

  Status CompletePlayback(JournalPlaybackInfo info) override {
    completed_info = info;
    return {};
  }
};

class TemporaryDatabasePath final {
 public:
  TemporaryDatabasePath() {
    static std::atomic<std::uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-rollback-" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".db");
    journal_path_ = path_;
    journal_path_ += "-journal";
  }

  TemporaryDatabasePath(const TemporaryDatabasePath&) = delete;
  TemporaryDatabasePath& operator=(const TemporaryDatabasePath&) = delete;

  ~TemporaryDatabasePath() noexcept {
    std::error_code error;
    std::filesystem::remove(journal_path_, error);
    std::filesystem::remove(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] const std::filesystem::path& journal_path() const noexcept { return journal_path_; }

 private:
  std::filesystem::path path_;
  std::filesystem::path journal_path_;
};

void AppendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
  const std::size_t offset = bytes.size();
  bytes.resize(offset + sizeof(value));
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(value)>{bytes.data() + offset, sizeof(value)}, value);
}

void AppendSuperJournal(std::vector<std::byte>& bytes, std::string_view path,
                        std::uint32_t leading_page_number = 0xdeadbeefU) {
  AppendU32(bytes, leading_page_number);
  const ByteView name = AsBytes(path);
  bytes.insert(bytes.end(), name.begin(), name.end());
  AppendU32(bytes, static_cast<std::uint32_t>(name.size()));
  std::uint32_t checksum = 0;
  for (const std::byte value : name) {
    checksum += std::to_integer<std::uint8_t>(value);
  }
  AppendU32(bytes, checksum);
  bytes.insert(bytes.end(), kJournalMagic.begin(), kJournalMagic.end());
}

TEST(RollbackJournalSectorSize, AppliesSQLiteNormalizationAndCoordinatorValidation) {
  const auto powersafe = ResolveRollbackJournalSectorSize(Properties(
      ByteCount{4096}, DeviceCharacteristics{}.With(DeviceCapability::kPowersafeOverwrite)));
  ASSERT_TRUE(powersafe.has_value());
  EXPECT_EQ(ByteCount{512}, *powersafe);

  const auto too_small = ResolveRollbackJournalSectorSize(Properties(ByteCount{16}));
  ASSERT_TRUE(too_small.has_value());
  EXPECT_EQ(ByteCount{512}, *too_small);

  const auto too_large = ResolveRollbackJournalSectorSize(Properties(ByteCount{131072}));
  ASSERT_TRUE(too_large.has_value());
  EXPECT_EQ(ByteCount{65536}, *too_large);

  const auto non_power = ResolveRollbackJournalSectorSize(Properties(ByteCount{1000}));
  ASSERT_FALSE(non_power.has_value());
  EXPECT_EQ(ErrorCode::kInternal, non_power.error().code());
}

TEST(RollbackJournalWriter, WritesExactInitialHeaderAndPageRecord) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(
      ByteCount{4096}, DeviceCharacteristics{}.With(DeviceCapability::kPowersafeOverwrite));
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);

  const auto& header = vfs.Bytes("/database.sqlite-journal");
  ASSERT_EQ(512U, header.size());
  EXPECT_TRUE(std::ranges::all_of(header | std::views::take(12),
                                  [](std::byte value) { return value == std::byte{0}; }));
  EXPECT_EQ(0x12345678U, ReadU32(header, 12));
  EXPECT_EQ(3U, ReadU32(header, 16));
  EXPECT_EQ(512U, ReadU32(header, 20));
  EXPECT_EQ(512U, ReadU32(header, 24));

  const std::vector<std::byte> page = Page(std::byte{0x21});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{2},
                      .bytes = page,
                  })
                  .has_value());

  const auto& bytes = vfs.Bytes("/database.sqlite-journal");
  ASSERT_EQ(1032U, bytes.size());
  EXPECT_EQ(2U, ReadU32(bytes, 512));
  EXPECT_TRUE(std::ranges::equal(page, ByteView{bytes}.subspan(516, 512)));
  EXPECT_EQ(ComputeRollbackJournalChecksum(page, 0x12345678U), ReadU32(bytes, 1028));
}

TEST(RollbackJournalWriter, SupportsEverySQLiteDatabasePageSizeBoundary) {
  for (const std::size_t page_size : {512U, 4096U, 65536U}) {
    MemoryVfs vfs;
    std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, Properties(ByteCount{512}));
    ASSERT_NE(nullptr, journal);
    std::unique_ptr<JournalTransaction> transaction =
        BeginTransaction(*journal, ByteCount{page_size});
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page(page_size, std::byte{0x5a});

    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());

    const auto& bytes = vfs.Bytes("/database.sqlite-journal");
    ASSERT_EQ(512U + page_size + 8U, bytes.size());
    EXPECT_EQ(page_size, ReadU32(bytes, 24));
    EXPECT_EQ(ComputeRollbackJournalChecksum(page, 0x12345678U),
              ReadU32(bytes, 512U + 4U + page_size));
  }
}

TEST(RollbackJournalWriter, PublishesTheHeaderBetweenTwoNormalSyncs) {
  MemoryVfs vfs;
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, Properties(ByteCount{512}));
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  const std::vector<std::byte> page = Page(std::byte{0x31});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = page,
                  })
                  .has_value());
  vfs.ClearEvents();

  ASSERT_TRUE(transaction->SyncJournal().has_value());

  ASSERT_EQ(4U, vfs.events.size());
  EXPECT_EQ("read", vfs.events[0].operation);
  EXPECT_EQ("sync", vfs.events[1].operation);
  EXPECT_EQ((SyncOptions{.mode = SyncMode::kNormal, .data_only = false}),
            vfs.events[1].sync_options);
  EXPECT_EQ("write", vfs.events[2].operation);
  EXPECT_EQ(0U, vfs.events[2].offset);
  EXPECT_EQ(12U, vfs.events[2].size);
  EXPECT_EQ("sync", vfs.events[3].operation);
  EXPECT_EQ((SyncOptions{.mode = SyncMode::kNormal, .data_only = false}),
            vfs.events[3].sync_options);

  const auto& bytes = vfs.Bytes("/database.sqlite-journal");
  EXPECT_TRUE(std::ranges::equal(kJournalMagic, ByteView{bytes}.first<8>()));
  EXPECT_EQ(1U, ReadU32(bytes, 8));
}

TEST(RollbackJournalWriter, StartsAnAlignedCohortAfterARepeatedSyncEpoch) {
  MemoryVfs vfs;
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, Properties(ByteCount{512}));
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  const std::vector<std::byte> first_page = Page(std::byte{0x41});
  const std::vector<std::byte> second_page = Page(std::byte{0x51});

  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = first_page,
                  })
                  .has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{2},
                      .bytes = second_page,
                  })
                  .has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());

  const auto& bytes = vfs.Bytes("/database.sqlite-journal");
  ASSERT_EQ(2568U, bytes.size());
  EXPECT_TRUE(std::ranges::all_of(ByteView{bytes}.subspan(1032, 504),
                                  [](std::byte value) { return value == std::byte{0}; }));
  EXPECT_TRUE(std::ranges::equal(kJournalMagic, ByteView{bytes}.subspan(1536, 8)));
  EXPECT_EQ(1U, ReadU32(bytes, 1544));
  EXPECT_EQ(2U, ReadU32(bytes, 2048));
  EXPECT_TRUE(std::ranges::equal(second_page, ByteView{bytes}.subspan(2052, 512)));
}

TEST(RollbackJournalWriter, UsesSafeAppendFromTheDatabaseFileProperties) {
  MemoryVfs vfs;
  const FileProperties properties =
      Properties(ByteCount{512}, DeviceCharacteristics{}.With(DeviceCapability::kSafeAppend));
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  const auto& header = vfs.Bytes("/database.sqlite-journal");
  EXPECT_TRUE(std::ranges::equal(kJournalMagic, ByteView{header}.first<8>()));
  EXPECT_EQ(0xffffffffU, ReadU32(header, 8));
  const std::vector<std::byte> page = Page(std::byte{0x61});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = page,
                  })
                  .has_value());
  vfs.ClearEvents();

  ASSERT_TRUE(transaction->SyncJournal().has_value());

  EXPECT_EQ(1U, SyncCount(vfs));
  EXPECT_EQ(0U, std::ranges::count_if(vfs.events, [](const IoEvent& event) {
              return event.operation == "write" && event.offset == 0 && event.size == 12;
            }));
}

TEST(RollbackJournalWriter, OmitsSyncsForASequentialDatabaseDevice) {
  MemoryVfs vfs;
  const FileProperties properties =
      Properties(ByteCount{512}, DeviceCharacteristics{}.With(DeviceCapability::kSequential));
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  const std::vector<std::byte> page = Page(std::byte{0x71});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = page,
                  })
                  .has_value());
  vfs.ClearEvents();

  ASSERT_TRUE(transaction->SyncJournal().has_value());

  EXPECT_EQ(0U, SyncCount(vfs));
  EXPECT_EQ(1U, std::ranges::count_if(vfs.events, [](const IoEvent& event) {
              return event.operation == "write" && event.offset == 0 && event.size == 12;
            }));
}

TEST(RollbackJournalWriter, RejectsAMismatchedEffectiveSectorBeforeOpeningTheJournal) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(
      ByteCount{4096}, DeviceCharacteristics{}.With(DeviceCapability::kPowersafeOverwrite));
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, journal);

  auto begun = JournalTransaction::Begin(*journal, JournalTransactionInfo{
                                                       .page_size = ByteCount{512},
                                                       .sector_size = ByteCount{4096},
                                                       .original_page_count = 3,
                                                   });

  ASSERT_FALSE(begun.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, begun.error().code());
  EXPECT_EQ(0U, vfs.open_calls);
}

TEST(RollbackJournalWriter, RejectsAnInvalidLegacyPageSizeAtConstruction) {
  MemoryVfs vfs;

  auto created = RollbackJournal::Create(vfs, "database.sqlite", Properties(ByteCount{512}),
                                         RollbackJournalOptions{
                                             .legacy_page_size = ByteCount{1000},
                                             .delete_directory_sync = DirectorySync::kNo,
                                         });

  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, created.error().code());
  EXPECT_EQ(0U, vfs.open_calls);
}

TEST(RollbackJournalPlayback, StreamsTransactionRollbackAcrossPublishedCohorts) {
  MemoryVfs vfs;
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, Properties(ByteCount{512}));
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  const std::vector<std::byte> first_page = Page(std::byte{0x81});
  const std::vector<std::byte> second_page = Page(std::byte{0x91});

  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = first_page,
                  })
                  .has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{2},
                      .bytes = second_page,
                  })
                  .has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  RecordingTarget target;

  ASSERT_TRUE(transaction->Rollback(target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{1}, PageNumber{2}}), target.restored_pages);
  ASSERT_EQ(2U, target.restored_images.size());
  EXPECT_EQ(first_page, target.restored_images[0]);
  EXPECT_EQ(second_page, target.restored_images[1]);
  EXPECT_EQ(3U, target.resized_page_count);
  EXPECT_EQ(1U, target.sync_count);
  EXPECT_FALSE(vfs.HasFile("/database.sqlite-journal"));
}

TEST(RollbackJournalPlayback, RecoversAHotJournalAfterSynchronizingIt) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  const std::vector<std::byte> page = Page(std::byte{0xa1});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{2},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  vfs.ClearEvents();
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{2}}), target.restored_pages);
  ASSERT_FALSE(vfs.events.empty());
  const auto sync = std::ranges::find_if(
      vfs.events, [](const IoEvent& event) { return event.operation == "sync"; });
  const auto first_read = std::ranges::find_if(vfs.events, [](const IoEvent& event) {
    return event.operation == "read" || event.operation == "size";
  });
  ASSERT_NE(vfs.events.end(), sync);
  ASSERT_NE(vfs.events.end(), first_read);
  EXPECT_LT(sync, first_read);
  EXPECT_EQ((SyncOptions{.mode = SyncMode::kNormal, .data_only = false}), sync->sync_options);
  EXPECT_FALSE(vfs.HasFile("/database.sqlite-journal"));
}

TEST(RollbackJournalPlayback, StopsAtAChecksumDamagedTail) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page = Page(std::byte{0xb1});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  vfs.MutableBytes("/database.sqlite-journal").back() ^= std::byte{0x01};
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_TRUE(target.restored_pages.empty());
  EXPECT_EQ(3U, target.resized_page_count);
  EXPECT_EQ(1U, target.sync_count);
}

TEST(RollbackJournalPlayback, IgnoresLaterHeaderGeometryLikeSQLite) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  const std::vector<std::byte> first_page = Page(std::byte{0xc1});
  const std::vector<std::byte> second_page = Page(std::byte{0xd1});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = first_page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{2},
                        .bytes = second_page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  auto& bytes = vfs.MutableBytes("/database.sqlite-journal");
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + 1556, sizeof(std::uint32_t)},
      1000U);
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + 1560, sizeof(std::uint32_t)},
      1000U);
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{1}, PageNumber{2}}), target.restored_pages);
}

TEST(RollbackJournalPlayback, MissingSuperJournalSuppressesRollback) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page = Page(std::byte{0xe1});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  AppendSuperJournal(vfs.MutableBytes("/database.sqlite-journal"), "/database.sqlite-mj000000900");
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_FALSE(target.prepared_info.has_value());
  EXPECT_TRUE(target.restored_pages.empty());
  EXPECT_FALSE(vfs.HasFile("/database.sqlite-journal"));
}

TEST(RollbackJournalPlayback, ExistingSuperJournalPermitsRollback) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  const std::vector<std::byte> page = Page(std::byte{0xf1});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  constexpr std::string_view kSuperJournal = "/database.sqlite-mj000000900";
  AppendSuperJournal(vfs.MutableBytes("/database.sqlite-journal"), kSuperJournal);
  vfs.PutFile(std::string{kSuperJournal});
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{1}}), target.restored_pages);
  ASSERT_EQ(1U, target.restored_images.size());
  EXPECT_EQ(page, target.restored_images.front());
}

TEST(RollbackJournalPlayback, DerivesSafeAppendRecordCountFromFileSize) {
  MemoryVfs vfs;
  const FileProperties properties =
      Properties(ByteCount{512}, DeviceCharacteristics{}.With(DeviceCapability::kSafeAppend));
  const std::vector<std::byte> first_page = Page(std::byte{0x14});
  const std::vector<std::byte> second_page = Page(std::byte{0x24});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = first_page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{2},
                        .bytes = second_page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{1}, PageNumber{2}}), target.restored_pages);
}

TEST(RollbackJournalPlayback, UsesTheConfiguredLegacyPageSizeForAZeroHeaderField) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  const std::vector<std::byte> page = Page(std::byte{0x34});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  auto& bytes = vfs.MutableBytes("/database.sqlite-journal");
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + 24, sizeof(std::uint32_t)}, 0U);
  std::unique_ptr<RollbackJournal> recovery =
      CreateJournal(vfs, properties,
                    RollbackJournalOptions{
                        .legacy_page_size = ByteCount{512},
                        .delete_directory_sync = DirectorySync::kNo,
                    });
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{1}}), target.restored_pages);
  EXPECT_EQ(page, target.restored_images.front());
}

TEST(RollbackJournalPlayback, DoesNotInferHotRecordsFromAZeroCount) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page = Page(std::byte{0x45});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  auto& bytes = vfs.MutableBytes("/database.sqlite-journal");
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + 8, sizeof(std::uint32_t)}, 0U);
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_TRUE(target.restored_pages.empty());
  EXPECT_EQ(3U, target.resized_page_count);
}

TEST(RollbackJournalPlayback, StopsBeforeAnUnrecognizableLaterHeader) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> first_page = Page(std::byte{0x56});
    const std::vector<std::byte> second_page = Page(std::byte{0x67});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = first_page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{2},
                        .bytes = second_page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  vfs.MutableBytes("/database.sqlite-journal")[1536] = std::byte{0};
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{1}}), target.restored_pages);
}

TEST(RollbackJournalPlayback, SkipsOutOfRangeRecordsBeforeChecksumValidation) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  const std::vector<std::byte> valid_page = Page(std::byte{0x37});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> skipped_page = Page(std::byte{0x19});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = skipped_page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{2},
                        .bytes = valid_page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  auto& bytes = vfs.MutableBytes("/database.sqlite-journal");
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + 512, sizeof(std::uint32_t)}, 4U);
  bytes[1028] ^= std::byte{0x01};
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{2}}), target.restored_pages);
  ASSERT_EQ(1U, target.restored_images.size());
  EXPECT_EQ(valid_page, target.restored_images.front());
}

TEST(RollbackJournalPlayback, TreatsInvalidFirstGeometryAsAnUnusableJournal) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page = Page(std::byte{0x78});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  auto& bytes = vfs.MutableBytes("/database.sqlite-journal");
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + 20, sizeof(std::uint32_t)}, 1000U);
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_FALSE(target.prepared_info.has_value());
  EXPECT_TRUE(target.restored_pages.empty());
  EXPECT_FALSE(vfs.HasFile("/database.sqlite-journal"));
}

TEST(RollbackJournalPlayback, IgnoresASuperJournalNameBeyondTheVfsPathLimit) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  const std::vector<std::byte> page = Page(std::byte{0x89});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  std::string oversized_name = "/";
  oversized_name.append(500, 'a');
  oversized_name += "-mj000000900";
  AppendSuperJournal(vfs.MutableBytes("/database.sqlite-journal"), oversized_name);
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{1}}), target.restored_pages);
}

TEST(RollbackJournalPlayback, ReadsPinnedSQLite354HotJournals) {
  for (const std::size_t page_size : {512U, 4096U, 65536U}) {
    SCOPED_TRACE(page_size);
    const std::string prefix = "sqlite-3.54.0-rollback-page-" + std::to_string(page_size);
    const std::vector<std::byte> database = ReadFixture(prefix + ".db");
    const std::vector<std::byte> journal_bytes = ReadFixture(prefix + ".journal");
    ASSERT_FALSE(database.empty());
    ASSERT_FALSE(journal_bytes.empty());
    ASSERT_EQ(0U, database.size() % page_size);

    MemoryVfs vfs;
    vfs.PutFile("/database.sqlite-journal", journal_bytes);
    const FileProperties properties = Properties(
        ByteCount{4096}, DeviceCharacteristics{}.With(DeviceCapability::kPowersafeOverwrite));
    std::unique_ptr<RollbackJournal> journal =
        CreateJournal(vfs, properties,
                      RollbackJournalOptions{
                          .legacy_page_size = ByteCount{page_size},
                          .delete_directory_sync = DirectorySync::kNo,
                      });
    ASSERT_NE(nullptr, journal);
    RecordingTarget target;

    ASSERT_TRUE(RecoverHotJournal(*journal, target).has_value());

    EXPECT_EQ(database.size() / page_size, target.resized_page_count);
    ASSERT_FALSE(target.restored_pages.empty());
    ASSERT_EQ(target.restored_pages.size(), target.restored_images.size());
    for (std::size_t index = 0; index < target.restored_pages.size(); ++index) {
      const std::size_t page_number = target.restored_pages[index].value();
      ASSERT_GE(page_number, 1U);
      ASSERT_LE(page_number, database.size() / page_size);
      const ByteView expected =
          ByteView{database}.subspan((page_number - 1U) * page_size, page_size);
      EXPECT_TRUE(std::ranges::equal(expected, target.restored_images[index]));
    }
  }
}

TEST(RollbackJournalLifecycle, UsesConfiguredDeleteDurabilityAndCanBeReused) {
  MemoryVfs vfs;
  std::unique_ptr<RollbackJournal> journal =
      CreateJournal(vfs, Properties(ByteCount{512}),
                    RollbackJournalOptions{
                        .legacy_page_size = ByteCount{512},
                        .delete_directory_sync = DirectorySync::kYes,
                    });
  ASSERT_NE(nullptr, journal);
  {
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page = Page(std::byte{0x9a});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
    ASSERT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{1}).has_value());
    ASSERT_TRUE(transaction->MarkDatabaseSynced().has_value());
    ASSERT_TRUE(transaction->Commit().has_value());
  }
  const auto deleted = std::ranges::find_if(
      vfs.events, [](const IoEvent& event) { return event.operation == "delete"; });
  ASSERT_NE(vfs.events.end(), deleted);
  EXPECT_EQ(DirectorySync::kYes, deleted->directory_sync);

  std::unique_ptr<JournalTransaction> second = BeginTransaction(*journal);
  ASSERT_NE(nullptr, second);
  RecordingTarget target;
  EXPECT_TRUE(second->Rollback(target).has_value());
}

TEST(RollbackJournalDurability, PublicationFailuresDoNotCreateADurableHotJournal) {
  enum class FailurePoint : std::uint8_t {
    kFirstSync,
    kPublishWrite,
    kSecondSync,
    kNone,
  };
  for (const FailurePoint failure : {FailurePoint::kFirstSync, FailurePoint::kPublishWrite,
                                     FailurePoint::kSecondSync, FailurePoint::kNone}) {
    SCOPED_TRACE(static_cast<int>(failure));
    MemoryVfs vfs;
    const FileProperties properties = Properties(ByteCount{512});
    const std::vector<std::byte> page = Page(std::byte{0x5c});
    {
      std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
      ASSERT_NE(nullptr, writer);
      std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
      ASSERT_NE(nullptr, transaction);
      ASSERT_TRUE(transaction
                      ->CapturePage(JournalPageImage{
                          .page_number = PageNumber{1},
                          .bytes = page,
                      })
                      .has_value());
      if (failure == FailurePoint::kFirstSync) {
        vfs.FailSync("/database.sqlite-journal", 1);
      } else if (failure == FailurePoint::kPublishWrite) {
        vfs.FailPublishWrite("/database.sqlite-journal");
      } else if (failure == FailurePoint::kSecondSync) {
        vfs.FailSync("/database.sqlite-journal", 2);
      }

      const auto synced = transaction->SyncJournal();
      if (failure == FailurePoint::kNone) {
        ASSERT_TRUE(synced.has_value());
        ASSERT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{1}).has_value());
      } else {
        ASSERT_FALSE(synced.has_value());
        EXPECT_EQ(ErrorCode::kIo, synced.error().code());
        EXPECT_EQ(JournalTransactionState::kFailed, transaction->state());
      }
    }

    vfs.Crash();
    std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, recovery);
    RecordingTarget target;
    ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());
    if (failure == FailurePoint::kNone) {
      EXPECT_EQ((std::vector<PageNumber>{PageNumber{1}}), target.restored_pages);
      EXPECT_EQ(page, target.restored_images.front());
    } else {
      EXPECT_FALSE(target.prepared_info.has_value());
      EXPECT_TRUE(target.restored_pages.empty());
    }
  }
}

TEST(RollbackJournalFailures, PropagatesPlaybackReadMetadataErrorsWithoutDeleting) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page = Page(std::byte{0x6d});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  vfs.FailNextSize("/database.sqlite-journal");
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  const auto recovered = RecoverHotJournal(*recovery, target);

  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(ErrorCode::kIo, recovered.error().code());
  EXPECT_FALSE(target.prepared_info.has_value());
  EXPECT_TRUE(vfs.HasFile("/database.sqlite-journal"));
}

TEST(RollbackJournalFailures, PropagatesSuperJournalAccessErrors) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page = Page(std::byte{0x7e});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  AppendSuperJournal(vfs.MutableBytes("/database.sqlite-journal"), "/database.sqlite-mj000000900");
  vfs.access_error = Error::Create(ErrorCode::kIo, "injected access failure");
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  const auto recovered = RecoverHotJournal(*recovery, target);

  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(ErrorCode::kIo, recovered.error().code());
  EXPECT_TRUE(vfs.HasFile("/database.sqlite-journal"));
}

TEST(RollbackJournalFailures, DeleteFailureLeavesTheTransactionInPersistentError) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  const std::vector<std::byte> page = Page(std::byte{0x8f});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = page,
                  })
                  .has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  vfs.delete_error = Error::Create(ErrorCode::kIo, "injected delete durability failure");
  vfs.unlink_before_delete_error = true;
  RecordingTarget target;

  const auto rolled_back = transaction->Rollback(target);

  ASSERT_FALSE(rolled_back.has_value());
  EXPECT_EQ(ErrorCode::kIo, rolled_back.error().code());
  EXPECT_EQ(JournalTransactionState::kError, transaction->state());
  EXPECT_FALSE(vfs.HasFile("/database.sqlite-journal"));
  const auto retried = transaction->Rollback(target);
  ASSERT_FALSE(retried.has_value());
  EXPECT_EQ(ErrorCode::kIo, retried.error().code());
}

TEST(RollbackJournalFailures, BeginRandomnessFailureCanBeRetriedSafely) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, journal);
  vfs.randomness_error = Error::Create(ErrorCode::kIo, "injected randomness failure");

  auto failed = JournalTransaction::Begin(*journal, JournalTransactionInfo{
                                                        .page_size = ByteCount{512},
                                                        .sector_size = ByteCount{512},
                                                        .original_page_count = 3,
                                                    });

  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  ASSERT_TRUE(vfs.HasFile("/database.sqlite-journal"));
  EXPECT_TRUE(vfs.Bytes("/database.sqlite-journal").empty());

  vfs.randomness_error.reset();
  const std::unique_ptr<JournalTransaction> retried = BeginTransaction(*journal);
  ASSERT_NE(nullptr, retried);
}

TEST(RollbackJournalFailures, FactoryTranslatesFullPathAllocationFailures) {
  MemoryVfs vfs;
  vfs.throw_bad_alloc_on_full_path = true;

  const auto created = RollbackJournal::Create(vfs, "database.sqlite", Properties(ByteCount{512}));

  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(ErrorCode::kOutOfMemory, created.error().code());
}

TEST(RollbackJournalFailures, BeginTranslatesOpenAllocationFailuresAndCanRetry) {
  MemoryVfs vfs;
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, Properties(ByteCount{512}));
  ASSERT_NE(nullptr, journal);
  vfs.throw_bad_alloc_on_open = true;

  const auto failed = JournalTransaction::Begin(*journal, JournalTransactionInfo{
                                                              .page_size = ByteCount{512},
                                                              .sector_size = ByteCount{512},
                                                              .original_page_count = 3,
                                                          });

  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kOutOfMemory, failed.error().code());
  const std::unique_ptr<JournalTransaction> retried = BeginTransaction(*journal);
  ASSERT_NE(nullptr, retried);
}

TEST(RollbackJournalFailures, MainRecordWriteFailureRemainsRollbackSafe) {
  MemoryVfs vfs;
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, Properties(ByteCount{512}));
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  vfs.FailNextWrite("/database.sqlite-journal");
  const std::vector<std::byte> page = Page(std::byte{0x9f});

  const auto captured = transaction->CapturePage(JournalPageImage{
      .page_number = PageNumber{1},
      .bytes = page,
  });

  ASSERT_FALSE(captured.has_value());
  EXPECT_EQ(ErrorCode::kIo, captured.error().code());
  EXPECT_EQ(JournalTransactionState::kFailed, transaction->state());
  RecordingTarget target;
  EXPECT_TRUE(transaction->Rollback(target).has_value());
  EXPECT_TRUE(target.restored_pages.empty());
}

TEST(RollbackJournalFailures, RecordReadErrorsRemainErrorsAndLeaveTheJournal) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page = Page(std::byte{0xaf});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  vfs.FailReadAt("/database.sqlite-journal", 512);
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  const auto recovered = RecoverHotJournal(*recovery, target);

  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(ErrorCode::kIo, recovered.error().code());
  EXPECT_TRUE(target.prepared_info.has_value());
  EXPECT_TRUE(vfs.HasFile("/database.sqlite-journal"));
}

TEST(RollbackJournalPlayback, StopsAtATornPageRecord) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page = Page(std::byte{0xbf});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  vfs.MutableBytes("/database.sqlite-journal").resize(700);
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_TRUE(target.restored_pages.empty());
  EXPECT_EQ(3U, target.resized_page_count);
}

TEST(RollbackJournalPlayback, StopsAtAnIllegalPageNumber) {
  MemoryVfs vfs;
  const FileProperties properties = Properties(ByteCount{512});
  {
    std::unique_ptr<RollbackJournal> writer = CreateJournal(vfs, properties);
    ASSERT_NE(nullptr, writer);
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*writer);
    ASSERT_NE(nullptr, transaction);
    const std::vector<std::byte> page = Page(std::byte{0xcf});
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{1},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  auto& bytes = vfs.MutableBytes("/database.sqlite-journal");
  std::ranges::fill(bytes.begin() + 512, bytes.begin() + 516, std::byte{0});
  std::unique_ptr<RollbackJournal> recovery = CreateJournal(vfs, properties);
  ASSERT_NE(nullptr, recovery);
  RecordingTarget target;

  ASSERT_TRUE(RecoverHotJournal(*recovery, target).has_value());

  EXPECT_TRUE(target.restored_pages.empty());
}

TEST(RollbackJournalPosix, CreatesRecoversAndDeletesARealJournal) {
  const TemporaryDatabasePath temporary;
  PosixVfs vfs;
  auto database = vfs.Open(temporary.path().string(), FileOpenOptions{
                                                          .kind = FileKind::kMainDatabase,
                                                          .access = FileAccessMode::kReadWrite,
                                                          .create = true,
                                                          .exclusive_create = false,
                                                          .delete_on_close = false,
                                                          .allow_read_only_fallback = false,
                                                          .no_follow = false,
                                                      });
  ASSERT_TRUE(database.has_value()) << database.error().ToString();
  auto properties = database->file->Properties();
  ASSERT_TRUE(properties.has_value()) << properties.error().ToString();
  auto sector_size = ResolveRollbackJournalSectorSize(*properties);
  ASSERT_TRUE(sector_size.has_value()) << sector_size.error().ToString();
  const std::vector<std::byte> page = Page(std::byte{0xdf});
  {
    auto created = RollbackJournal::Create(vfs, temporary.path().string(), *properties,
                                           RollbackJournalOptions{
                                               .legacy_page_size = ByteCount{512},
                                               .delete_directory_sync = DirectorySync::kNo,
                                           });
    ASSERT_TRUE(created.has_value()) << created.error().ToString();
    std::unique_ptr<RollbackJournal> writer = std::move(*created);
    auto begun = JournalTransaction::Begin(*writer, JournalTransactionInfo{
                                                        .page_size = ByteCount{512},
                                                        .sector_size = *sector_size,
                                                        .original_page_count = 3,
                                                    });
    ASSERT_TRUE(begun.has_value()) << begun.error().ToString();
    std::unique_ptr<JournalTransaction> transaction = std::move(*begun);
    ASSERT_TRUE(transaction
                    ->CapturePage(JournalPageImage{
                        .page_number = PageNumber{2},
                        .bytes = page,
                    })
                    .has_value());
    ASSERT_TRUE(transaction->SyncJournal().has_value());
  }
  ASSERT_TRUE(std::filesystem::exists(temporary.journal_path()));

  auto created = RollbackJournal::Create(vfs, temporary.path().string(), *properties,
                                         RollbackJournalOptions{
                                             .legacy_page_size = ByteCount{512},
                                             .delete_directory_sync = DirectorySync::kNo,
                                         });
  ASSERT_TRUE(created.has_value()) << created.error().ToString();
  RecordingTarget target;
  ASSERT_TRUE(RecoverHotJournal(**created, target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{2}}), target.restored_pages);
  EXPECT_EQ(page, target.restored_images.front());
  EXPECT_FALSE(std::filesystem::exists(temporary.journal_path()));
}

TEST(RollbackJournalSavepoint, CombinesLaterMainRecordsAndSubjournalImages) {
  MemoryVfs vfs;
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, Properties(ByteCount{512}));
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  const std::vector<std::byte> transaction_page = Page(std::byte{0x11});
  const std::vector<std::byte> savepoint_page = Page(std::byte{0x22});
  const std::vector<std::byte> second_page = Page(std::byte{0x33});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = transaction_page,
                  })
                  .has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  const auto savepoint = transaction->CreateSavepoint(3);
  ASSERT_TRUE(savepoint.has_value());
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = savepoint_page,
                  })
                  .has_value());
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{2},
                      .bytes = second_page,
                  })
                  .has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  RecordingTarget savepoint_target;

  ASSERT_TRUE(transaction->RollbackToSavepoint(*savepoint, savepoint_target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{2}, PageNumber{1}}),
            savepoint_target.restored_pages);
  ASSERT_EQ(2U, savepoint_target.restored_images.size());
  EXPECT_EQ(second_page, savepoint_target.restored_images[0]);
  EXPECT_EQ(savepoint_page, savepoint_target.restored_images[1]);
  EXPECT_EQ(0U, savepoint_target.sync_count);
  EXPECT_TRUE(vfs.HasFile("/database.sqlite-journal"));

  RecordingTarget transaction_target;
  ASSERT_TRUE(transaction->Rollback(transaction_target).has_value());
  EXPECT_EQ((std::vector<PageNumber>{PageNumber{1}, PageNumber{2}}),
            transaction_target.restored_pages);
  EXPECT_EQ(transaction_page, transaction_target.restored_images[0]);
  EXPECT_EQ(second_page, transaction_target.restored_images[1]);
}

TEST(RollbackJournalSavepoint, ReusesReleasedSubjournalSpaceWhenSafe) {
  MemoryVfs vfs;
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, Properties(ByteCount{512}));
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  const auto first = transaction->CreateSavepoint(3);
  const auto second = transaction->CreateSavepoint(4);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const std::vector<std::byte> first_image = Page(std::byte{0x44});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{4},
                      .bytes = first_image,
                  })
                  .has_value());
  ASSERT_TRUE(transaction->ReleaseSavepoint(*second).has_value());
  const auto third = transaction->CreateSavepoint(4);
  ASSERT_TRUE(third.has_value());
  const std::vector<std::byte> second_image = Page(std::byte{0x55});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{4},
                      .bytes = second_image,
                  })
                  .has_value());

  const auto& subjournal = vfs.TemporaryBytes(0);
  ASSERT_EQ(516U, subjournal.size());
  EXPECT_EQ(4U, ReadU32(subjournal, 0));
  EXPECT_TRUE(std::ranges::equal(second_image, ByteView{subjournal}.subspan(4, 512)));
}

TEST(RollbackJournalSavepoint, RetainsSharedSubjournalRecordsForAnOlderSavepoint) {
  MemoryVfs vfs;
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, Properties(ByteCount{512}));
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  const std::vector<std::byte> transaction_page = Page(std::byte{0xaa});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = transaction_page,
                  })
                  .has_value());
  const auto first = transaction->CreateSavepoint(3);
  const auto second = transaction->CreateSavepoint(3);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const std::vector<std::byte> shared_image = Page(std::byte{0xbb});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = shared_image,
                  })
                  .has_value());
  ASSERT_TRUE(transaction->ReleaseSavepoint(*second).has_value());
  const auto third = transaction->CreateSavepoint(4);
  ASSERT_TRUE(third.has_value());
  const std::vector<std::byte> later_image = Page(std::byte{0xcc});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{4},
                      .bytes = later_image,
                  })
                  .has_value());

  const auto& subjournal = vfs.TemporaryBytes(0);
  ASSERT_EQ(1032U, subjournal.size());
  EXPECT_EQ(1U, ReadU32(subjournal, 0));
  EXPECT_EQ(4U, ReadU32(subjournal, 516));
}

TEST(RollbackJournalSavepoint, ReportsMalformedLiveSubjournalRecordsAsInternal) {
  MemoryVfs vfs;
  std::unique_ptr<RollbackJournal> journal = CreateJournal(vfs, Properties(ByteCount{512}));
  ASSERT_NE(nullptr, journal);
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(*journal);
  ASSERT_NE(nullptr, transaction);
  const std::vector<std::byte> transaction_page = Page(std::byte{0xdd});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = transaction_page,
                  })
                  .has_value());
  const auto savepoint = transaction->CreateSavepoint(3);
  ASSERT_TRUE(savepoint.has_value());
  const std::vector<std::byte> savepoint_page = Page(std::byte{0xee});
  ASSERT_TRUE(transaction
                  ->CapturePage(JournalPageImage{
                      .page_number = PageNumber{1},
                      .bytes = savepoint_page,
                  })
                  .has_value());
  auto& subjournal = vfs.MutableTemporaryBytes(0);
  std::ranges::fill(subjournal.begin(), subjournal.begin() + 4, std::byte{0});
  RecordingTarget target;

  const auto rolled_back = transaction->RollbackToSavepoint(*savepoint, target);

  ASSERT_FALSE(rolled_back.has_value());
  EXPECT_EQ(ErrorCode::kInternal, rolled_back.error().code());
  EXPECT_EQ(JournalTransactionState::kFailed, transaction->state());
}

}  // namespace
}  // namespace modern_sqlite
