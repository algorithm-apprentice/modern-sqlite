#ifndef MODERN_SQLITE_TESTS_UNIT_PAGER_WRITE_PAGER_TEST_SUPPORT_HPP_
#define MODERN_SQLITE_TESTS_UNIT_PAGER_WRITE_PAGER_TEST_SUPPORT_HPP_

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
#include <utility>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/vfs.hpp"

namespace modern_sqlite::test {

inline constexpr std::size_t kWritePagerPageSize = 512;
inline constexpr std::size_t kWritePagerFileCapacity = 64U * 1024U;
inline constexpr std::string_view kWritePagerInputPath = "allocation.db";
inline constexpr std::string_view kWritePagerDatabasePath = "/allocation/write-pager.db";
inline constexpr std::string_view kWritePagerJournalPath = "/allocation/write-pager.db-journal";
inline constexpr std::string_view kWritePagerWalPath = "/allocation/write-pager.db-wal";

struct WritePagerFixedFileState {
  std::array<std::byte, kWritePagerFileCapacity> bytes{};
  std::size_t size = 0;
  bool present = false;
  DatabaseLock lock = DatabaseLock::kNone;
};

class WritePagerFixedFile final : public File {
 public:
  WritePagerFixedFile(WritePagerFixedFileState& state, bool delete_on_close) noexcept
      : state_(&state), delete_on_close_(delete_on_close) {}

  ~WritePagerFixedFile() override {
    state_->lock = DatabaseLock::kNone;
    if (delete_on_close_) {
      state_->present = false;
      state_->size = 0;
    }
  }

 private:
  [[nodiscard]] Result<ByteCount> DoReadAt(MutableByteView destination,
                                           FileOffset offset) override {
    if (offset.value() > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(Error::Create(ErrorCode::kTooLarge, "fixed-file offset is too large"));
    }
    const std::size_t start = static_cast<std::size_t>(offset.value());
    if (start >= state_->size) {
      return ByteCount{0};
    }
    const std::size_t count = std::min(destination.size(), state_->size - start);
    std::ranges::copy_n(state_->bytes.begin() + static_cast<std::ptrdiff_t>(start),
                        static_cast<std::ptrdiff_t>(count), destination.begin());
    return ByteCount{count};
  }

  [[nodiscard]] Status DoWriteAt(ByteView source, FileOffset offset) override {
    if (offset.value() > kWritePagerFileCapacity) {
      return std::unexpected(Error::Create(ErrorCode::kFull, "fixed-file capacity exceeded"));
    }
    const std::size_t start = static_cast<std::size_t>(offset.value());
    if (source.size() > kWritePagerFileCapacity - start) {
      return std::unexpected(Error::Create(ErrorCode::kFull, "fixed-file capacity exceeded"));
    }
    const std::size_t end = start + source.size();
    if (end > state_->size) {
      std::ranges::fill(
          std::span<std::byte>{state_->bytes}.subspan(state_->size, end - state_->size),
          std::byte{0});
      state_->size = end;
    }
    std::ranges::copy(source, state_->bytes.begin() + static_cast<std::ptrdiff_t>(start));
    state_->present = true;
    return {};
  }

  [[nodiscard]] Status DoTruncate(FileSize size) override {
    if (size.value() > kWritePagerFileCapacity) {
      return std::unexpected(Error::Create(ErrorCode::kFull, "fixed-file capacity exceeded"));
    }
    const std::size_t next_size = static_cast<std::size_t>(size.value());
    if (next_size > state_->size) {
      std::ranges::fill(
          std::span<std::byte>{state_->bytes}.subspan(state_->size, next_size - state_->size),
          std::byte{0});
    }
    state_->size = next_size;
    state_->present = true;
    return {};
  }

  [[nodiscard]] Status DoSync(SyncOptions) override { return {}; }

  [[nodiscard]] Result<FileSize> DoSize() override { return FileSize{state_->size}; }

  [[nodiscard]] Status DoLock(DatabaseLock lock) override {
    state_->lock = lock;
    return {};
  }

  [[nodiscard]] Status DoUnlock(DatabaseLock lock) override {
    state_->lock = lock;
    return {};
  }

  [[nodiscard]] Result<bool> DoHasReservedLock() override {
    return state_->lock >= DatabaseLock::kReserved;
  }

  [[nodiscard]] FileProperties DoProperties() const noexcept override {
    return FileProperties{
        .sector_size = ByteCount{kWritePagerPageSize},
        .device_characteristics = {},
    };
  }

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

  WritePagerFixedFileState* state_;
  bool delete_on_close_;
};

class WritePagerFixedVfs final : public Vfs {
 public:
  WritePagerFixedVfs() { InitializeDatabase(); }

  [[nodiscard]] bool journal_present() const noexcept { return journal_.present; }

  [[nodiscard]] DatabaseLock database_lock() const noexcept { return main_.lock; }

  [[nodiscard]] ByteView database_bytes() const noexcept {
    return ByteView{main_.bytes}.first(main_.size);
  }

 private:
  void InitializeDatabase() noexcept {
    static constexpr std::array<std::byte, 16> kMagic{
        std::byte{0x53}, std::byte{0x51}, std::byte{0x4c}, std::byte{0x69},
        std::byte{0x74}, std::byte{0x65}, std::byte{0x20}, std::byte{0x66},
        std::byte{0x6f}, std::byte{0x72}, std::byte{0x6d}, std::byte{0x61},
        std::byte{0x74}, std::byte{0x20}, std::byte{0x33}, std::byte{0x00},
    };
    std::ranges::copy(kMagic, main_.bytes.begin());
    main_.bytes[16] = std::byte{0x02};
    main_.bytes[17] = std::byte{0x00};
    main_.bytes[18] = std::byte{1};
    main_.bytes[19] = std::byte{1};
    main_.bytes[21] = std::byte{64};
    main_.bytes[22] = std::byte{32};
    main_.bytes[23] = std::byte{32};
    Store32(24, 7);
    Store32(28, 2);
    Store32(44, 4);
    Store32(56, 1);
    Store32(92, 7);
    Store32(96, 3'054'000);
    main_.bytes[kWritePagerPageSize + 100U] = std::byte{2};
    main_.size = kWritePagerPageSize * 2U;
    main_.present = true;
  }

  void Store32(std::size_t offset, std::uint32_t value) noexcept {
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(value)>{main_.bytes.data() + offset, sizeof(value)}, value);
  }

  [[nodiscard]] Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                                          FileOpenOptions options) override {
    WritePagerFixedFileState* state = nullptr;
    if (!path.has_value()) {
      state = &subjournal_;
    } else if (*path == kWritePagerDatabasePath) {
      state = &main_;
    } else if (*path == kWritePagerJournalPath) {
      state = &journal_;
    } else if (*path == kWritePagerWalPath) {
      state = &wal_;
    } else {
      return std::unexpected(Error::Create(ErrorCode::kNotFound, "fixed VFS path is absent"));
    }
    if (!state->present && !options.create) {
      return std::unexpected(Error::Create(ErrorCode::kNotFound, "fixed VFS file is absent"));
    }
    if (options.create) {
      state->present = true;
    }
    try {
      return OpenedFile{
          .file = std::make_unique<WritePagerFixedFile>(*state, options.delete_on_close),
          .access = options.access,
      };
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Status DoDelete(std::string_view path, DirectorySync) override {
    if (path != kWritePagerJournalPath || !journal_.present) {
      return std::unexpected(Error::Create(ErrorCode::kNotFound, "fixed VFS file is absent"));
    }
    journal_.present = false;
    journal_.size = 0;
    return {};
  }

  [[nodiscard]] Result<bool> DoAccess(std::string_view path, FileAccessQuery) override {
    if (path == kWritePagerJournalPath) {
      return journal_.present;
    }
    if (path == kWritePagerWalPath) {
      return wal_.present;
    }
    return false;
  }

  [[nodiscard]] Result<std::string> DoFullPath(std::string_view path) override {
    if (path != kWritePagerInputPath && path != kWritePagerDatabasePath) {
      return std::unexpected(Error::Create(ErrorCode::kNotFound, "fixed VFS path is absent"));
    }
    try {
      return std::string{kWritePagerDatabasePath};
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

  WritePagerFixedFileState main_;
  WritePagerFixedFileState journal_;
  WritePagerFixedFileState subjournal_;
  WritePagerFixedFileState wal_;
};

[[nodiscard]] inline std::unique_ptr<Pager> OpenWritePager(WritePagerFixedVfs& vfs,
                                                           std::size_t cache_pages) {
  auto opened =
      Pager::OpenWritable(vfs, kWritePagerInputPath,
                          WritablePagerOptions{
                              .pager =
                                  PagerOptions{
                                      .empty_database_page_size = ByteCount{kWritePagerPageSize},
                                      .cache_capacity_pages = cache_pages,
                                  },
                              .journal =
                                  RollbackJournalOptions{
                                      .legacy_page_size = ByteCount{kWritePagerPageSize},
                                  },
                          });
  return opened.has_value() ? std::move(*opened) : nullptr;
}

}  // namespace modern_sqlite::test

#endif  // MODERN_SQLITE_TESTS_UNIT_PAGER_WRITE_PAGER_TEST_SUPPORT_HPP_
