#include "modern_sqlite/platform/vfs.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"

namespace modern_sqlite {
namespace {

[[nodiscard]] constexpr std::byte ByteValue(std::uint8_t value) noexcept {
  return static_cast<std::byte>(value);
}

[[nodiscard]] ByteBuffer Bytes(std::initializer_list<std::uint8_t> values) {
  ByteBuffer result{ByteCount{values.size()}};
  std::size_t offset = 0;
  for (const std::uint8_t value : values) {
    result.mutable_view()[offset] = ByteValue(value);
    ++offset;
  }
  return result;
}

[[nodiscard]] Error IoError(std::string message = "injected I/O failure") {
  return Error::Create(ErrorCode::kIo, std::move(message));
}

template <typename Enum>
[[nodiscard]] constexpr Enum InvalidEnumValue(std::uint8_t value) noexcept {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  return static_cast<Enum>(value);
}

class FakeFile final : public File {
 public:
  ByteBuffer read_bytes;
  std::optional<ByteCount> reported_read_size;
  std::optional<Error> read_error;
  std::optional<Error> write_error;
  std::optional<Error> truncate_error;
  std::optional<Error> sync_error;
  std::optional<Error> size_error;
  std::optional<Error> lock_error;
  std::optional<Error> unlock_error;
  std::optional<Error> reserved_lock_error;
  std::optional<Error> shared_memory_map_error;
  std::optional<Error> shared_memory_lock_error;
  std::optional<Error> shared_memory_unmap_error;
  FileSize file_size{4096};
  bool reserved_lock = false;
  bool shared_memory_absent = false;
  bool shared_memory_wrong_size = false;
  ByteBuffer shared_memory{ByteCount{64}};
  FileProperties properties{
      .sector_size = ByteCount{4096},
      .device_characteristics = DeviceCharacteristics{}.With(DeviceCapability::kPowersafeOverwrite),
  };

  std::size_t read_calls = 0;
  std::size_t write_calls = 0;
  std::size_t truncate_calls = 0;
  std::size_t sync_calls = 0;
  std::size_t size_calls = 0;
  std::size_t lock_calls = 0;
  std::size_t unlock_calls = 0;
  std::size_t reserved_lock_calls = 0;
  std::size_t shared_memory_map_calls = 0;
  std::size_t shared_memory_lock_calls = 0;
  std::size_t shared_memory_barrier_calls = 0;
  std::size_t shared_memory_unmap_calls = 0;

  FileOffset last_read_offset;
  FileOffset last_write_offset;
  std::vector<std::byte> last_write;
  FileSize last_truncate_size;
  SyncOptions last_sync;
  DatabaseLock last_lock = DatabaseLock::kNone;
  DatabaseLock last_unlock = DatabaseLock::kNone;
  SharedMemoryRegionIndex last_region;
  ByteCount last_region_size;
  SharedMemoryMapMode last_map_mode = SharedMemoryMapMode::kExistingOnly;
  SharedMemoryLockRange last_shared_memory_lock;
  SharedMemoryLockOperation last_shared_memory_operation = SharedMemoryLockOperation::kLock;
  SharedMemoryLockMode last_shared_memory_mode = SharedMemoryLockMode::kShared;
  SharedMemoryUnmapMode last_unmap_mode = SharedMemoryUnmapMode::kKeep;

 protected:
  Result<ByteCount> DoReadAt(MutableByteView destination, FileOffset offset) override {
    ++read_calls;
    last_read_offset = offset;
    if (read_error.has_value()) {
      return std::unexpected(*read_error);
    }
    const ByteCount reported = reported_read_size.value_or(read_bytes.size());
    const std::size_t copied =
        std::min({reported.value(), read_bytes.size().value(), destination.size()});
    if (copied != 0) {
      std::memcpy(destination.data(), read_bytes.view().data(), copied);
    }
    return reported;
  }

  Status DoWriteAt(ByteView source, FileOffset offset) override {
    ++write_calls;
    last_write_offset = offset;
    last_write.assign(source.begin(), source.end());
    if (write_error.has_value()) {
      return std::unexpected(*write_error);
    }
    return {};
  }

  Status DoTruncate(FileSize size) override {
    ++truncate_calls;
    last_truncate_size = size;
    if (truncate_error.has_value()) {
      return std::unexpected(*truncate_error);
    }
    return {};
  }

  Status DoSync(SyncOptions options) override {
    ++sync_calls;
    last_sync = options;
    if (sync_error.has_value()) {
      return std::unexpected(*sync_error);
    }
    return {};
  }

  Result<FileSize> DoSize() override {
    ++size_calls;
    if (size_error.has_value()) {
      return std::unexpected(*size_error);
    }
    return file_size;
  }

  Status DoLock(DatabaseLock lock) override {
    ++lock_calls;
    last_lock = lock;
    if (lock_error.has_value()) {
      return std::unexpected(*lock_error);
    }
    return {};
  }

  Status DoUnlock(DatabaseLock lock) override {
    ++unlock_calls;
    last_unlock = lock;
    if (unlock_error.has_value()) {
      return std::unexpected(*unlock_error);
    }
    return {};
  }

  Result<bool> DoHasReservedLock() override {
    ++reserved_lock_calls;
    if (reserved_lock_error.has_value()) {
      return std::unexpected(*reserved_lock_error);
    }
    return reserved_lock;
  }

  [[nodiscard]] FileProperties DoProperties() const noexcept override { return properties; }

  Result<std::optional<MutableByteView>> DoMapSharedMemory(SharedMemoryRegionIndex region,
                                                           ByteCount region_size,
                                                           SharedMemoryMapMode mode) override {
    ++shared_memory_map_calls;
    last_region = region;
    last_region_size = region_size;
    last_map_mode = mode;
    if (shared_memory_map_error.has_value()) {
      return std::unexpected(*shared_memory_map_error);
    }
    if (shared_memory_absent) {
      return std::optional<MutableByteView>{};
    }
    const std::size_t returned_size =
        shared_memory_wrong_size ? region_size.value() - 1U : region_size.value();
    return std::optional<MutableByteView>{shared_memory.mutable_view().first(returned_size)};
  }

  Status DoLockSharedMemory(SharedMemoryLockRange range, SharedMemoryLockOperation operation,
                            SharedMemoryLockMode mode) override {
    ++shared_memory_lock_calls;
    last_shared_memory_lock = range;
    last_shared_memory_operation = operation;
    last_shared_memory_mode = mode;
    if (shared_memory_lock_error.has_value()) {
      return std::unexpected(*shared_memory_lock_error);
    }
    return {};
  }

  void DoSharedMemoryBarrier() noexcept override { ++shared_memory_barrier_calls; }

  Status DoUnmapSharedMemory(SharedMemoryUnmapMode mode) override {
    ++shared_memory_unmap_calls;
    last_unmap_mode = mode;
    if (shared_memory_unmap_error.has_value()) {
      return std::unexpected(*shared_memory_unmap_error);
    }
    return {};
  }
};

class FakeVfs final : public Vfs {
 public:
  std::optional<Error> open_error;
  std::optional<Error> delete_error;
  std::optional<Error> access_error;
  std::optional<Error> full_path_error;
  std::optional<Error> randomness_error;
  std::optional<Error> sleep_error;
  std::optional<Error> time_error;
  FileAccessMode opened_access = FileAccessMode::kReadWrite;
  bool return_null_file = false;
  bool access_result = true;
  std::string full_path_result = "/absolute/database.sqlite";
  ByteCount randomness_size{4};
  std::chrono::microseconds slept_for{10};
  WallClockTime current_time{std::chrono::milliseconds{123456}};

  std::size_t open_calls = 0;
  std::size_t delete_calls = 0;
  std::size_t access_calls = 0;
  std::size_t full_path_calls = 0;
  std::size_t randomness_calls = 0;
  std::size_t sleep_calls = 0;
  std::size_t time_calls = 0;

  std::optional<std::string> last_open_path;
  FileOpenOptions last_open_options;
  std::string last_delete_path;
  DirectorySync last_directory_sync = DirectorySync::kNo;
  std::string last_access_path;
  FileAccessQuery last_access_query = FileAccessQuery::kExists;
  std::string last_full_path_input;
  std::chrono::microseconds last_sleep_request{0};

 protected:
  Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                            FileOpenOptions options) override {
    ++open_calls;
    last_open_path = path.has_value() ? std::optional<std::string>{*path} : std::nullopt;
    last_open_options = options;
    if (open_error.has_value()) {
      return std::unexpected(*open_error);
    }
    if (return_null_file) {
      return OpenedFile{.file = nullptr, .access = opened_access};
    }
    auto file = std::make_unique<FakeFile>();
    return OpenedFile{
        .file = std::move(file),
        .access = opened_access,
    };
  }

  Status DoDelete(std::string_view path, DirectorySync directory_sync) override {
    ++delete_calls;
    last_delete_path = path;
    last_directory_sync = directory_sync;
    if (delete_error.has_value()) {
      return std::unexpected(*delete_error);
    }
    return {};
  }

  Result<bool> DoAccess(std::string_view path, FileAccessQuery query) override {
    ++access_calls;
    last_access_path = path;
    last_access_query = query;
    if (access_error.has_value()) {
      return std::unexpected(*access_error);
    }
    return access_result;
  }

  Result<std::string> DoFullPath(std::string_view path) override {
    ++full_path_calls;
    last_full_path_input = path;
    if (full_path_error.has_value()) {
      return std::unexpected(*full_path_error);
    }
    return full_path_result;
  }

  Result<ByteCount> DoRandomBytes(MutableByteView output) override {
    ++randomness_calls;
    if (randomness_error.has_value()) {
      return std::unexpected(*randomness_error);
    }
    const std::size_t filled = std::min(randomness_size.value(), output.size());
    std::ranges::fill(output.first(filled), ByteValue(0xa5));
    return randomness_size;
  }

  Result<std::chrono::microseconds> DoSleepFor(std::chrono::microseconds duration) override {
    ++sleep_calls;
    last_sleep_request = duration;
    if (sleep_error.has_value()) {
      return std::unexpected(*sleep_error);
    }
    return slept_for;
  }

  Result<WallClockTime> DoCurrentTime() override {
    ++time_calls;
    if (time_error.has_value()) {
      return std::unexpected(*time_error);
    }
    return current_time;
  }
};

[[nodiscard]] FileOpenOptions ReadWriteMainDatabaseOptions() {
  return FileOpenOptions{
      .kind = FileKind::kMainDatabase,
      .access = FileAccessMode::kReadWrite,
      .create = true,
      .allow_read_only_fallback = true,
  };
}

TEST(DeviceCharacteristics, TracksCapabilitiesAndAtomicWriteSizes) {
  const DeviceCharacteristics characteristics = DeviceCharacteristics{}
                                                    .With(DeviceCapability::kAtomic4K)
                                                    .With(DeviceCapability::kSafeAppend)
                                                    .With(DeviceCapability::kPowersafeOverwrite);

  EXPECT_TRUE(characteristics.Has(DeviceCapability::kAtomic4K));
  EXPECT_TRUE(characteristics.Has(DeviceCapability::kSafeAppend));
  EXPECT_TRUE(characteristics.Has(DeviceCapability::kPowersafeOverwrite));
  EXPECT_FALSE(characteristics.Has(DeviceCapability::kAtomic8K));
  EXPECT_TRUE(characteristics.SupportsAtomicWriteSize(ByteCount{4096}));
  EXPECT_FALSE(characteristics.SupportsAtomicWriteSize(ByteCount{8192}));
  EXPECT_FALSE(characteristics.SupportsAtomicWriteSize(ByteCount{0}));

  const DeviceCharacteristics all_atomic = DeviceCharacteristics{}.With(DeviceCapability::kAtomic);
  EXPECT_TRUE(all_atomic.SupportsAtomicWriteSize(ByteCount{3}));
}

TEST(FileContracts, CompletesAndZeroFillsPositionedReads) {
  FakeFile file;
  file.read_bytes = Bytes({0x10, 0x20, 0x30, 0x40});
  ByteBuffer complete_buffer{ByteCount{4}};

  const auto complete = file.ReadAt(complete_buffer.mutable_view(), FileOffset{7});

  ASSERT_TRUE(complete.has_value());
  EXPECT_TRUE(complete->complete());
  EXPECT_EQ(ByteCount{4}, complete->bytes_read());
  EXPECT_EQ(FileOffset{7}, file.last_read_offset);
  EXPECT_TRUE(std::ranges::equal(complete_buffer.view(), file.read_bytes.view()));

  file.reported_read_size = ByteCount{2};
  ByteBuffer short_buffer{ByteCount{4}};
  std::ranges::fill(short_buffer.mutable_view(), ByteValue(0xff));

  const auto short_read = file.ReadAt(short_buffer.mutable_view(), FileOffset{9});

  ASSERT_TRUE(short_read.has_value());
  EXPECT_FALSE(short_read->complete());
  EXPECT_EQ(ByteCount{2}, short_read->bytes_read());
  EXPECT_EQ(ByteValue(0x10), short_buffer.view()[0]);
  EXPECT_EQ(ByteValue(0x20), short_buffer.view()[1]);
  EXPECT_EQ(ByteValue(0x00), short_buffer.view()[2]);
  EXPECT_EQ(ByteValue(0x00), short_buffer.view()[3]);
}

TEST(FileContracts, RejectsReadOverreportAndPreservesBackendErrors) {
  FakeFile file;
  file.read_bytes = Bytes({0x10, 0x20});
  file.reported_read_size = ByteCount{3};
  ByteBuffer buffer{ByteCount{2}};

  const auto overreported = file.ReadAt(buffer.mutable_view(), FileOffset{0});

  ASSERT_FALSE(overreported.has_value());
  EXPECT_EQ(ErrorCode::kInternal, overreported.error().code());

  file.reported_read_size.reset();
  file.read_error = IoError();
  const auto failed = file.ReadAt(buffer.mutable_view(), FileOffset{0});
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
}

TEST(FileContracts, SkipsEmptyIoAndRejectsOverflowingRanges) {
  FakeFile file;
  const MutableByteView empty_output;
  const ByteView empty_input;

  const auto empty_read = file.ReadAt(empty_output, FileOffset{12});
  const auto empty_write = file.WriteAt(empty_input, FileOffset{12});

  ASSERT_TRUE(empty_read.has_value());
  EXPECT_TRUE(empty_read->complete());
  EXPECT_EQ(ByteCount{0}, empty_read->bytes_read());
  EXPECT_TRUE(empty_write.has_value());
  EXPECT_EQ(0U, file.read_calls);
  EXPECT_EQ(0U, file.write_calls);

  ByteBuffer one_byte{ByteCount{1}};
  const FileOffset maximum_offset{std::numeric_limits<std::uint64_t>::max()};
  const auto read_overflow = file.ReadAt(one_byte.mutable_view(), maximum_offset);
  const auto write_overflow = file.WriteAt(one_byte.view(), maximum_offset);

  ASSERT_FALSE(read_overflow.has_value());
  ASSERT_FALSE(write_overflow.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, read_overflow.error().code());
  EXPECT_EQ(ErrorCode::kTooLarge, write_overflow.error().code());
}

TEST(FileContracts, ForwardsExactWritesTruncationAndSize) {
  FakeFile file;
  const ByteBuffer source = Bytes({0xaa, 0xbb, 0xcc});

  const auto write = file.WriteAt(source.view(), FileOffset{99});
  const auto truncate = file.Truncate(FileSize{2048});
  const auto size = file.Size();

  EXPECT_TRUE(write.has_value());
  EXPECT_TRUE(truncate.has_value());
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(FileOffset{99}, file.last_write_offset);
  EXPECT_TRUE(std::ranges::equal(source.view(), file.last_write));
  EXPECT_EQ(FileSize{2048}, file.last_truncate_size);
  EXPECT_EQ(FileSize{4096}, *size);

  file.write_error = IoError();
  const auto failed = file.WriteAt(source.view(), FileOffset{0});
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
}

TEST(FileContracts, ValidatesSyncAndDatabaseLockOperations) {
  FakeFile file;
  const SyncOptions full_data_only{
      .mode = SyncMode::kFull,
      .data_only = true,
  };

  EXPECT_TRUE(file.Sync(full_data_only).has_value());
  EXPECT_EQ(full_data_only, file.last_sync);
  EXPECT_TRUE(file.Lock(DatabaseLock::kShared).has_value());
  EXPECT_EQ(DatabaseLock::kShared, file.last_lock);
  EXPECT_TRUE(file.Unlock(DatabaseLock::kNone).has_value());
  EXPECT_EQ(DatabaseLock::kNone, file.last_unlock);

  file.reserved_lock = true;
  const auto reserved = file.HasReservedLock();
  ASSERT_TRUE(reserved.has_value());
  EXPECT_TRUE(*reserved);

  const auto invalid_sync =
      file.Sync(SyncOptions{.mode = InvalidEnumValue<SyncMode>(9), .data_only = false});
  const auto invalid_lock = file.Lock(DatabaseLock::kNone);
  const auto pending_lock = file.Lock(DatabaseLock::kPending);
  const auto invalid_unlock = file.Unlock(DatabaseLock::kReserved);

  ASSERT_FALSE(invalid_sync.has_value());
  ASSERT_FALSE(invalid_lock.has_value());
  ASSERT_FALSE(pending_lock.has_value());
  ASSERT_FALSE(invalid_unlock.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_sync.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_lock.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, pending_lock.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_unlock.error().code());

  file.lock_error = Error::Create(ErrorCode::kBusy, "lock conflict");
  const auto busy = file.Lock(DatabaseLock::kExclusive);
  ASSERT_FALSE(busy.has_value());
  EXPECT_EQ(ErrorCode::kBusy, busy.error().code());
}

TEST(FileContracts, ExposesDurabilityProperties) {
  FakeFile file;

  const auto properties = file.Properties();

  ASSERT_TRUE(properties.has_value());
  EXPECT_EQ(ByteCount{4096}, properties->sector_size);
  EXPECT_TRUE(properties->device_characteristics.Has(DeviceCapability::kPowersafeOverwrite));

  file.properties.sector_size = ByteCount{0};
  const auto invalid = file.Properties();
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(ErrorCode::kInternal, invalid.error().code());
}

TEST(FileContracts, ValidatesSharedMemoryMappings) {
  FakeFile file;

  const auto mapped =
      file.MapSharedMemory(SharedMemoryRegionIndex{3}, ByteCount{32}, SharedMemoryMapMode::kExtend);

  ASSERT_TRUE(mapped.has_value());
  ASSERT_TRUE(mapped->has_value());
  EXPECT_EQ(32U, (*mapped)->size());
  (*mapped)->front() = ByteValue(0x7f);
  EXPECT_EQ(ByteValue(0x7f), file.shared_memory.view().front());
  EXPECT_EQ(SharedMemoryRegionIndex{3}, file.last_region);
  EXPECT_EQ(ByteCount{32}, file.last_region_size);
  EXPECT_EQ(SharedMemoryMapMode::kExtend, file.last_map_mode);

  file.shared_memory_absent = true;
  const auto absent = file.MapSharedMemory(SharedMemoryRegionIndex{4}, ByteCount{32},
                                           SharedMemoryMapMode::kExistingOnly);
  ASSERT_TRUE(absent.has_value());
  EXPECT_FALSE(absent->has_value());

  const auto empty = file.MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{0},
                                          SharedMemoryMapMode::kExistingOnly);
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, empty.error().code());

  file.shared_memory_absent = false;
  file.shared_memory_wrong_size = true;
  const auto wrong_size = file.MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{32},
                                               SharedMemoryMapMode::kExistingOnly);
  ASSERT_FALSE(wrong_size.has_value());
  EXPECT_EQ(ErrorCode::kInternal, wrong_size.error().code());

  const ByteCount overflowing_region_size{(std::numeric_limits<std::uint64_t>::max() / 2U) + 1U};
  const auto overflow = file.MapSharedMemory(SharedMemoryRegionIndex{1}, overflowing_region_size,
                                             SharedMemoryMapMode::kExistingOnly);
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, overflow.error().code());
}

TEST(FileContracts, ValidatesSharedMemoryLocksBarrierAndUnmap) {
  FakeFile file;
  const SharedMemoryLockRange valid_range{.offset = 2, .count = 3};
  const SharedMemoryLockRange single_slot_range{.offset = 2, .count = 1};

  EXPECT_TRUE(file.LockSharedMemory(valid_range, SharedMemoryLockOperation::kLock,
                                    SharedMemoryLockMode::kExclusive)
                  .has_value());
  EXPECT_EQ(valid_range, file.last_shared_memory_lock);
  EXPECT_EQ(SharedMemoryLockOperation::kLock, file.last_shared_memory_operation);
  EXPECT_EQ(SharedMemoryLockMode::kExclusive, file.last_shared_memory_mode);

  file.SharedMemoryBarrier();
  EXPECT_EQ(1U, file.shared_memory_barrier_calls);

  EXPECT_TRUE(file.UnmapSharedMemory(SharedMemoryUnmapMode::kDelete).has_value());
  EXPECT_EQ(SharedMemoryUnmapMode::kDelete, file.last_unmap_mode);

  const auto zero_count =
      file.LockSharedMemory(SharedMemoryLockRange{.offset = 0, .count = 0},
                            SharedMemoryLockOperation::kLock, SharedMemoryLockMode::kShared);
  const auto out_of_range =
      file.LockSharedMemory(SharedMemoryLockRange{.offset = 7, .count = 2},
                            SharedMemoryLockOperation::kUnlock, SharedMemoryLockMode::kExclusive);
  const auto invalid_operation =
      file.LockSharedMemory(single_slot_range, InvalidEnumValue<SharedMemoryLockOperation>(3),
                            SharedMemoryLockMode::kShared);
  const auto invalid_mode = file.LockSharedMemory(valid_range, SharedMemoryLockOperation::kLock,
                                                  InvalidEnumValue<SharedMemoryLockMode>(3));
  const auto multi_slot_shared =
      file.LockSharedMemory(SharedMemoryLockRange{.offset = 0, .count = 2},
                            SharedMemoryLockOperation::kLock, SharedMemoryLockMode::kShared);

  ASSERT_FALSE(zero_count.has_value());
  ASSERT_FALSE(out_of_range.has_value());
  ASSERT_FALSE(invalid_operation.has_value());
  ASSERT_FALSE(invalid_mode.has_value());
  ASSERT_FALSE(multi_slot_shared.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, zero_count.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, out_of_range.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_operation.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_mode.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, multi_slot_shared.error().code());
}

TEST(FileContracts, PreservesBackendFailures) {
  FakeFile file;
  const Error injected = IoError();
  file.truncate_error = injected;
  file.sync_error = injected;
  file.size_error = injected;
  file.unlock_error = injected;
  file.reserved_lock_error = injected;
  file.shared_memory_map_error = injected;
  file.shared_memory_lock_error = injected;
  file.shared_memory_unmap_error = injected;

  const auto truncate = file.Truncate(FileSize{1});
  const auto sync = file.Sync(SyncOptions{});
  const auto size = file.Size();
  const auto unlock = file.Unlock(DatabaseLock::kNone);
  const auto reserved = file.HasReservedLock();
  const auto mapping = file.MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{16},
                                            SharedMemoryMapMode::kExistingOnly);
  const auto shared_lock =
      file.LockSharedMemory(SharedMemoryLockRange{.offset = 0, .count = 1},
                            SharedMemoryLockOperation::kLock, SharedMemoryLockMode::kShared);
  const auto unmap = file.UnmapSharedMemory(SharedMemoryUnmapMode::kKeep);

  ASSERT_FALSE(truncate.has_value());
  ASSERT_FALSE(sync.has_value());
  ASSERT_FALSE(size.has_value());
  ASSERT_FALSE(unlock.has_value());
  ASSERT_FALSE(reserved.has_value());
  ASSERT_FALSE(mapping.has_value());
  ASSERT_FALSE(shared_lock.has_value());
  ASSERT_FALSE(unmap.has_value());
  EXPECT_EQ(ErrorCode::kIo, truncate.error().code());
  EXPECT_EQ(ErrorCode::kIo, sync.error().code());
  EXPECT_EQ(ErrorCode::kIo, size.error().code());
  EXPECT_EQ(ErrorCode::kIo, unlock.error().code());
  EXPECT_EQ(ErrorCode::kIo, reserved.error().code());
  EXPECT_EQ(ErrorCode::kIo, mapping.error().code());
  EXPECT_EQ(ErrorCode::kIo, shared_lock.error().code());
  EXPECT_EQ(ErrorCode::kIo, unmap.error().code());
}

TEST(FileContracts, RejectsInvalidEnumValuesBeforeCallingBackend) {
  FakeFile file;

  const auto lock = file.Lock(InvalidEnumValue<DatabaseLock>(9));
  const auto unlock = file.Unlock(InvalidEnumValue<DatabaseLock>(9));
  const auto mapping = file.MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{16},
                                            InvalidEnumValue<SharedMemoryMapMode>(9));
  const auto unmap = file.UnmapSharedMemory(InvalidEnumValue<SharedMemoryUnmapMode>(9));

  ASSERT_FALSE(lock.has_value());
  ASSERT_FALSE(unlock.has_value());
  ASSERT_FALSE(mapping.has_value());
  ASSERT_FALSE(unmap.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, lock.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, unlock.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, mapping.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, unmap.error().code());
  EXPECT_EQ(0U, file.lock_calls);
  EXPECT_EQ(0U, file.unlock_calls);
  EXPECT_EQ(0U, file.shared_memory_map_calls);
  EXPECT_EQ(0U, file.shared_memory_unmap_calls);
}

TEST(VfsContracts, ValidatesOpenRequestsBeforeCallingBackend) {
  FakeVfs vfs;
  const std::string embedded_nul{"bad\0path", 8};
  const FileOpenOptions read_only_create{
      .kind = FileKind::kMainDatabase,
      .access = FileAccessMode::kReadOnly,
      .create = true,
  };
  FileOpenOptions exclusive_without_create = ReadWriteMainDatabaseOptions();
  exclusive_without_create.create = false;
  exclusive_without_create.exclusive_create = true;
  FileOpenOptions fallback_delete = ReadWriteMainDatabaseOptions();
  fallback_delete.delete_on_close = true;
  FileOpenOptions temporary = ReadWriteMainDatabaseOptions();
  temporary.kind = FileKind::kTemporaryDatabase;
  temporary.exclusive_create = true;
  temporary.delete_on_close = true;

  const auto invalid_create = vfs.Open("database.sqlite", read_only_create);
  const auto invalid_exclusive = vfs.Open("database.sqlite", exclusive_without_create);
  const auto invalid_fallback_delete = vfs.Open("database.sqlite", fallback_delete);
  const auto empty_path = vfs.Open("", ReadWriteMainDatabaseOptions());
  const auto embedded_nul_path = vfs.Open(embedded_nul, ReadWriteMainDatabaseOptions());
  const auto missing_main_database_path = vfs.Open(std::nullopt, ReadWriteMainDatabaseOptions());
  const auto noncanonical_temporary_open = vfs.Open(std::nullopt, temporary);

  ASSERT_FALSE(invalid_create.has_value());
  ASSERT_FALSE(invalid_exclusive.has_value());
  ASSERT_FALSE(invalid_fallback_delete.has_value());
  ASSERT_FALSE(empty_path.has_value());
  ASSERT_FALSE(embedded_nul_path.has_value());
  ASSERT_FALSE(missing_main_database_path.has_value());
  ASSERT_FALSE(noncanonical_temporary_open.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_create.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_exclusive.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_fallback_delete.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, empty_path.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, embedded_nul_path.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, missing_main_database_path.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, noncanonical_temporary_open.error().code());
  EXPECT_EQ(0U, vfs.open_calls);
}

TEST(VfsContracts, OpensNamedAndTemporaryFilesWithExplicitAccess) {
  FakeVfs vfs;
  const FileOpenOptions main_options = ReadWriteMainDatabaseOptions();

  auto main_file = vfs.Open("database.sqlite", main_options);

  ASSERT_TRUE(main_file.has_value());
  ASSERT_NE(nullptr, main_file->file);
  EXPECT_EQ(FileAccessMode::kReadWrite, main_file->access);
  EXPECT_EQ(std::optional<std::string>{"database.sqlite"}, vfs.last_open_path);
  EXPECT_EQ(main_options, vfs.last_open_options);

  const FileOpenOptions temporary{
      .kind = FileKind::kTemporaryDatabase,
      .access = FileAccessMode::kReadWrite,
      .create = true,
      .exclusive_create = true,
      .delete_on_close = true,
  };
  const auto temporary_file = vfs.Open(std::nullopt, temporary);

  ASSERT_TRUE(temporary_file.has_value());
  EXPECT_FALSE(vfs.last_open_path.has_value());
  EXPECT_EQ(temporary, vfs.last_open_options);
}

TEST(VfsContracts, ValidatesOpenedFileAndReadOnlyFallback) {
  FakeVfs vfs;
  vfs.return_null_file = true;

  const auto null_file = vfs.Open("database.sqlite", ReadWriteMainDatabaseOptions());

  ASSERT_FALSE(null_file.has_value());
  EXPECT_EQ(ErrorCode::kInternal, null_file.error().code());

  vfs.return_null_file = false;
  vfs.opened_access = FileAccessMode::kReadOnly;
  FileOpenOptions no_fallback = ReadWriteMainDatabaseOptions();
  no_fallback.allow_read_only_fallback = false;
  const auto rejected_fallback = vfs.Open("database.sqlite", no_fallback);
  ASSERT_FALSE(rejected_fallback.has_value());
  EXPECT_EQ(ErrorCode::kInternal, rejected_fallback.error().code());

  const auto accepted_fallback = vfs.Open("database.sqlite", ReadWriteMainDatabaseOptions());
  ASSERT_TRUE(accepted_fallback.has_value());
  EXPECT_EQ(FileAccessMode::kReadOnly, accepted_fallback->access);

  vfs.opened_access = FileAccessMode::kReadWrite;
  const FileOpenOptions read_only{
      .kind = FileKind::kMainDatabase,
      .access = FileAccessMode::kReadOnly,
  };
  const auto elevated = vfs.Open("database.sqlite", read_only);
  ASSERT_FALSE(elevated.has_value());
  EXPECT_EQ(ErrorCode::kInternal, elevated.error().code());

  vfs.opened_access = InvalidEnumValue<FileAccessMode>(9);
  const auto invalid_access = vfs.Open("database.sqlite", ReadWriteMainDatabaseOptions());
  ASSERT_FALSE(invalid_access.has_value());
  EXPECT_EQ(ErrorCode::kInternal, invalid_access.error().code());
}

TEST(VfsContracts, ValidatesPathsAndForwardsPathOperations) {
  FakeVfs vfs;
  const std::string embedded_nul{"bad\0path", 8};

  const auto empty_delete = vfs.Delete("", DirectorySync::kNo);
  const auto invalid_access = vfs.Access(embedded_nul, FileAccessQuery::kExists);
  const auto empty_full_path = vfs.FullPath("");

  ASSERT_FALSE(empty_delete.has_value());
  ASSERT_FALSE(invalid_access.has_value());
  ASSERT_FALSE(empty_full_path.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, empty_delete.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_access.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, empty_full_path.error().code());
  EXPECT_EQ(0U, vfs.delete_calls);
  EXPECT_EQ(0U, vfs.access_calls);
  EXPECT_EQ(0U, vfs.full_path_calls);

  EXPECT_TRUE(vfs.Delete("journal", DirectorySync::kYes).has_value());
  EXPECT_EQ("journal", vfs.last_delete_path);
  EXPECT_EQ(DirectorySync::kYes, vfs.last_directory_sync);

  const auto accessible = vfs.Access("database.sqlite", FileAccessQuery::kReadWrite);
  ASSERT_TRUE(accessible.has_value());
  EXPECT_TRUE(*accessible);
  EXPECT_EQ(FileAccessQuery::kReadWrite, vfs.last_access_query);

  const auto full_path = vfs.FullPath("database.sqlite");
  ASSERT_TRUE(full_path.has_value());
  EXPECT_EQ("/absolute/database.sqlite", *full_path);

  vfs.full_path_result.clear();
  const auto invalid_result = vfs.FullPath("database.sqlite");
  ASSERT_FALSE(invalid_result.has_value());
  EXPECT_EQ(ErrorCode::kInternal, invalid_result.error().code());

  vfs.full_path_result = std::string{"bad\0path", 8};
  const auto embedded_nul_result = vfs.FullPath("database.sqlite");
  ASSERT_FALSE(embedded_nul_result.has_value());
  EXPECT_EQ(ErrorCode::kInternal, embedded_nul_result.error().code());
}

TEST(VfsContracts, RequiresCompleteRandomnessAndSkipsEmptyRequests) {
  FakeVfs vfs;
  const MutableByteView empty;

  EXPECT_TRUE(vfs.RandomBytes(empty).has_value());
  EXPECT_EQ(0U, vfs.randomness_calls);

  ByteBuffer output{ByteCount{4}};
  EXPECT_TRUE(vfs.RandomBytes(output.mutable_view()).has_value());
  EXPECT_TRUE(
      std::ranges::all_of(output.view(), [](std::byte value) { return value == ByteValue(0xa5); }));

  vfs.randomness_size = ByteCount{2};
  std::ranges::fill(output.mutable_view(), ByteValue(0xff));
  const auto incomplete = vfs.RandomBytes(output.mutable_view());
  ASSERT_FALSE(incomplete.has_value());
  EXPECT_EQ(ErrorCode::kInternal, incomplete.error().code());
  EXPECT_TRUE(
      std::ranges::all_of(output.view(), [](std::byte value) { return value == ByteValue(0x00); }));

  vfs.randomness_size = ByteCount{5};
  const auto overreported = vfs.RandomBytes(output.mutable_view());
  ASSERT_FALSE(overreported.has_value());
  EXPECT_EQ(ErrorCode::kInternal, overreported.error().code());
}

TEST(VfsContracts, ValidatesSleepAndForwardsCurrentTime) {
  FakeVfs vfs;
  using namespace std::chrono_literals;

  const auto zero_sleep = vfs.SleepFor(0us);
  ASSERT_TRUE(zero_sleep.has_value());
  EXPECT_EQ(0us, *zero_sleep);
  EXPECT_EQ(0U, vfs.sleep_calls);

  vfs.slept_for = 15us;
  const auto slept = vfs.SleepFor(10us);
  ASSERT_TRUE(slept.has_value());
  EXPECT_EQ(15us, *slept);
  EXPECT_EQ(10us, vfs.last_sleep_request);

  const auto negative = vfs.SleepFor(-1us);
  ASSERT_FALSE(negative.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, negative.error().code());

  vfs.slept_for = 9us;
  const auto underreported = vfs.SleepFor(10us);
  ASSERT_FALSE(underreported.has_value());
  EXPECT_EQ(ErrorCode::kInternal, underreported.error().code());

  const auto now = vfs.CurrentTime();
  ASSERT_TRUE(now.has_value());
  EXPECT_EQ(vfs.current_time, *now);
}

TEST(VfsContracts, PreservesBackendFailures) {
  const Error injected = IoError();

  FakeVfs open_vfs;
  open_vfs.open_error = injected;
  const auto open = open_vfs.Open("database.sqlite", ReadWriteMainDatabaseOptions());

  FakeVfs delete_vfs;
  delete_vfs.delete_error = injected;
  const auto deleted = delete_vfs.Delete("database.sqlite", DirectorySync::kNo);

  FakeVfs access_vfs;
  access_vfs.access_error = injected;
  const auto access = access_vfs.Access("database.sqlite", FileAccessQuery::kExists);

  FakeVfs path_vfs;
  path_vfs.full_path_error = injected;
  const auto full_path = path_vfs.FullPath("database.sqlite");

  FakeVfs randomness_vfs;
  randomness_vfs.randomness_error = injected;
  ByteBuffer random_bytes{ByteCount{4}};
  const auto randomness = randomness_vfs.RandomBytes(random_bytes.mutable_view());

  FakeVfs sleep_vfs;
  sleep_vfs.sleep_error = injected;
  const auto sleep = sleep_vfs.SleepFor(std::chrono::microseconds{1});

  FakeVfs time_vfs;
  time_vfs.time_error = injected;
  const auto time = time_vfs.CurrentTime();

  ASSERT_FALSE(open.has_value());
  ASSERT_FALSE(deleted.has_value());
  ASSERT_FALSE(access.has_value());
  ASSERT_FALSE(full_path.has_value());
  ASSERT_FALSE(randomness.has_value());
  ASSERT_FALSE(sleep.has_value());
  ASSERT_FALSE(time.has_value());
  EXPECT_EQ(ErrorCode::kIo, open.error().code());
  EXPECT_EQ(ErrorCode::kIo, deleted.error().code());
  EXPECT_EQ(ErrorCode::kIo, access.error().code());
  EXPECT_EQ(ErrorCode::kIo, full_path.error().code());
  EXPECT_EQ(ErrorCode::kIo, randomness.error().code());
  EXPECT_EQ(ErrorCode::kIo, sleep.error().code());
  EXPECT_EQ(ErrorCode::kIo, time.error().code());
}

TEST(VfsContracts, RejectsInvalidEnumValuesBeforeCallingBackend) {
  FakeVfs vfs;
  FileOpenOptions invalid_kind = ReadWriteMainDatabaseOptions();
  invalid_kind.kind = InvalidEnumValue<FileKind>(20);
  FileOpenOptions invalid_access = ReadWriteMainDatabaseOptions();
  invalid_access.access = InvalidEnumValue<FileAccessMode>(20);

  const auto open_kind = vfs.Open("database.sqlite", invalid_kind);
  const auto open_access = vfs.Open("database.sqlite", invalid_access);
  const auto deleted = vfs.Delete("database.sqlite", InvalidEnumValue<DirectorySync>(20));
  const auto access = vfs.Access("database.sqlite", InvalidEnumValue<FileAccessQuery>(20));

  ASSERT_FALSE(open_kind.has_value());
  ASSERT_FALSE(open_access.has_value());
  ASSERT_FALSE(deleted.has_value());
  ASSERT_FALSE(access.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, open_kind.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, open_access.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, deleted.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, access.error().code());
  EXPECT_EQ(0U, vfs.open_calls);
  EXPECT_EQ(0U, vfs.delete_calls);
  EXPECT_EQ(0U, vfs.access_calls);
}

}  // namespace
}  // namespace modern_sqlite
