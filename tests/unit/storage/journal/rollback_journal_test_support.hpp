#ifndef MODERN_SQLITE_TESTS_UNIT_STORAGE_JOURNAL_ROLLBACK_JOURNAL_TEST_SUPPORT_HPP_
#define MODERN_SQLITE_TESTS_UNIT_STORAGE_JOURNAL_ROLLBACK_JOURNAL_TEST_SUPPORT_HPP_

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/platform/vfs.hpp"
#include "modern_sqlite/storage/journal/journal.hpp"

namespace modern_sqlite::test {

inline constexpr std::size_t kFixedJournalCapacity = 256U * 1024U;
inline constexpr std::size_t kTestPageSize = 512;
inline constexpr std::size_t kTestSectorSize = 512;

struct FixedFileState {
  std::array<std::byte, kFixedJournalCapacity> bytes{};
  std::size_t size = 0;
  bool present = false;
  FileProperties properties{
      .sector_size = ByteCount{kTestSectorSize},
      .device_characteristics = DeviceCharacteristics{},
  };
};

class FixedMemoryFile final : public File {
 public:
  FixedMemoryFile(FixedFileState& state, bool delete_on_close) noexcept
      : state_(&state), delete_on_close_(delete_on_close) {}

  ~FixedMemoryFile() override {
    if (delete_on_close_) {
      state_->present = false;
      state_->size = 0;
    }
  }

 protected:
  [[nodiscard]] Result<ByteCount> DoReadAt(MutableByteView destination,
                                           FileOffset offset) override {
    if (offset.value() > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(Error::Create(ErrorCode::kTooLarge, "test file offset is too large"));
    }
    const std::size_t start = static_cast<std::size_t>(offset.value());
    if (start >= state_->size) {
      return ByteCount{0};
    }
    const std::size_t count = std::min(destination.size(), state_->size - start);
    std::ranges::copy(ByteView{state_->bytes}.subspan(start, count), destination.begin());
    return ByteCount{count};
  }

  [[nodiscard]] Status DoWriteAt(ByteView source, FileOffset offset) override {
    if (offset.value() > kFixedJournalCapacity) {
      return std::unexpected(Error::Create(ErrorCode::kFull, "test file capacity exceeded"));
    }
    const std::size_t start = static_cast<std::size_t>(offset.value());
    if (source.size() > kFixedJournalCapacity - start) {
      return std::unexpected(Error::Create(ErrorCode::kFull, "test file capacity exceeded"));
    }
    const std::size_t end = start + source.size();
    if (end > state_->size) {
      std::ranges::fill(MutableByteView{state_->bytes}.subspan(state_->size, end - state_->size),
                        std::byte{0});
      state_->size = end;
    }
    std::ranges::copy(source, state_->bytes.begin() + static_cast<std::ptrdiff_t>(start));
    state_->present = true;
    return {};
  }

  [[nodiscard]] Status DoTruncate(FileSize size) override {
    if (size.value() > kFixedJournalCapacity) {
      return std::unexpected(Error::Create(ErrorCode::kFull, "test file capacity exceeded"));
    }
    const std::size_t next_size = static_cast<std::size_t>(size.value());
    if (next_size > state_->size) {
      std::ranges::fill(
          MutableByteView{state_->bytes}.subspan(state_->size, next_size - state_->size),
          std::byte{0});
    }
    state_->size = next_size;
    state_->present = true;
    return {};
  }

  [[nodiscard]] Status DoSync(SyncOptions) override { return {}; }
  [[nodiscard]] Result<FileSize> DoSize() override { return FileSize{state_->size}; }
  [[nodiscard]] Status DoLock(DatabaseLock) override { return {}; }
  [[nodiscard]] Status DoUnlock(DatabaseLock) override { return {}; }
  [[nodiscard]] Result<bool> DoHasReservedLock() override { return false; }
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

 private:
  FixedFileState* state_;
  bool delete_on_close_;
};

class FixedMemoryVfs final : public Vfs {
 public:
  [[nodiscard]] static FileProperties DatabaseProperties() noexcept {
    return FileProperties{
        .sector_size = ByteCount{kTestSectorSize},
        .device_characteristics = DeviceCharacteristics{},
    };
  }

  [[nodiscard]] static JournalTransactionInfo TransactionInfo(
      std::uint32_t original_page_count = 64) noexcept {
    return JournalTransactionInfo{
        .page_size = ByteCount{kTestPageSize},
        .sector_size = ByteCount{kTestSectorSize},
        .original_page_count = original_page_count,
    };
  }

  [[nodiscard]] bool LoadHotJournal(std::uint32_t record_count,
                                    std::string_view super_journal = {}) noexcept {
    constexpr std::array<std::byte, 8> kMagic{
        std::byte{0xd9}, std::byte{0xd5}, std::byte{0x05}, std::byte{0xf9},
        std::byte{0x20}, std::byte{0xa1}, std::byte{0x63}, std::byte{0xd7},
    };
    constexpr std::uint32_t kChecksumSeed = 0x13579bdfU;
    constexpr std::size_t kRecordSize = kTestPageSize + 8U;
    const std::size_t trailer_size = super_journal.empty() ? 0U : 4U + super_journal.size() + 16U;
    const std::size_t required =
        kTestSectorSize + static_cast<std::size_t>(record_count) * kRecordSize + trailer_size;
    if (required > journal_.bytes.size()) {
      return false;
    }

    std::ranges::fill(journal_.bytes, std::byte{0});
    std::ranges::copy(kMagic, journal_.bytes.begin());
    StoreU32(8, record_count);
    StoreU32(12, kChecksumSeed);
    StoreU32(16, record_count);
    StoreU32(20, kTestSectorSize);
    StoreU32(24, kTestPageSize);

    std::size_t offset = kTestSectorSize;
    for (std::uint32_t index = 0; index < record_count; ++index) {
      StoreU32(offset, index + 1U);
      MutableByteView page = MutableByteView{journal_.bytes}.subspan(offset + 4U, kTestPageSize);
      std::ranges::fill(page, static_cast<std::byte>((index + 1U) & 0xffU));
      StoreU32(offset + 4U + kTestPageSize, ComputeRollbackJournalChecksum(page, kChecksumSeed));
      offset += kRecordSize;
    }

    if (!super_journal.empty()) {
      StoreU32(offset, 0xdeadbeefU);
      offset += 4U;
      std::ranges::copy(AsBytes(super_journal),
                        journal_.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
      offset += super_journal.size();
      StoreU32(offset, static_cast<std::uint32_t>(super_journal.size()));
      offset += 4U;
      std::uint32_t checksum = 0;
      for (const char value : super_journal) {
        checksum += static_cast<std::uint8_t>(value);
      }
      StoreU32(offset, checksum);
      offset += 4U;
      std::ranges::copy(kMagic, journal_.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
      offset += kMagic.size();
    }

    journal_.size = offset;
    journal_.present = true;
    return true;
  }

  [[nodiscard]] bool journal_present() const noexcept { return journal_.present; }

 protected:
  [[nodiscard]] Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                                          FileOpenOptions options) override {
    FixedFileState& state = path.has_value() ? journal_ : subjournal_;
    if (!state.present && !options.create) {
      return std::unexpected(Error::Create(ErrorCode::kNotFound, "test file does not exist"));
    }
    if (options.create) {
      state.present = true;
    }
    try {
      return OpenedFile{
          .file = std::make_unique<FixedMemoryFile>(state, options.delete_on_close),
          .access = options.access,
      };
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Status DoDelete(std::string_view, DirectorySync) override {
    journal_.present = false;
    journal_.size = 0;
    return {};
  }

  [[nodiscard]] Result<bool> DoAccess(std::string_view, FileAccessQuery) override {
    return super_journal_exists_;
  }

  [[nodiscard]] Result<std::string> DoFullPath(std::string_view) override {
    static constexpr std::string_view kCanonicalPath =
        "/allocation-tests/modern-sqlite/rollback/database.sqlite";
    try {
      return std::string{kCanonicalPath};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
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

 private:
  void StoreU32(std::size_t offset, std::uint32_t value) noexcept {
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(value)>{journal_.bytes.data() + offset, sizeof(value)}, value);
  }

  FixedFileState journal_;
  FixedFileState subjournal_;
  bool super_journal_exists_ = false;
};

class CountingRecoveryTarget final : public JournalRecoveryTarget {
 public:
  [[nodiscard]] Status PreparePlayback(JournalPlaybackInfo) override {
    ++prepare_count;
    return {};
  }

  [[nodiscard]] Status ResizeDatabase(std::uint32_t) override {
    ++resize_count;
    return {};
  }

  [[nodiscard]] Status RestorePage(JournalPageImage) override {
    ++restore_count;
    return {};
  }

  [[nodiscard]] Status SyncDatabase() override {
    ++sync_count;
    return {};
  }

  [[nodiscard]] Status CompletePlayback(JournalPlaybackInfo) override {
    ++complete_count;
    return {};
  }

  std::size_t prepare_count = 0;
  std::size_t resize_count = 0;
  std::size_t restore_count = 0;
  std::size_t sync_count = 0;
  std::size_t complete_count = 0;
};

}  // namespace modern_sqlite::test

#endif  // MODERN_SQLITE_TESTS_UNIT_STORAGE_JOURNAL_ROLLBACK_JOURNAL_TEST_SUPPORT_HPP_
