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

template <std::size_t Capacity>
struct WritePagerFileState {
  std::array<std::byte, Capacity> bytes{};
  std::array<std::byte, Capacity> durable_bytes{};
  std::size_t size = 0;
  std::size_t durable_size = 0;
  bool present = false;
  bool durable_present = false;
  bool writes_are_durable = false;
  DatabaseLock lock = DatabaseLock::kNone;
  std::optional<FileSize> reported_size;
  std::optional<std::pair<DatabaseLock, ErrorCode>> unlock_failure;
};

struct WritePagerCrashState {
  std::optional<std::size_t> fail_after_mutation;
  std::size_t mutation_count = 0;
  bool cut_triggered = false;

  [[nodiscard]] bool CutAfterMutation() noexcept {
    ++mutation_count;
    if (fail_after_mutation == mutation_count) {
      fail_after_mutation.reset();
      cut_triggered = true;
      return true;
    }
    return false;
  }
};

template <std::size_t Capacity>
class WritePagerMemoryFile final : public File {
 public:
  WritePagerMemoryFile(WritePagerFileState<Capacity>& state, WritePagerCrashState& crash,
                       bool delete_on_close) noexcept
      : state_(&state), crash_(&crash), delete_on_close_(delete_on_close) {}

  ~WritePagerMemoryFile() override {
    state_->lock = DatabaseLock::kNone;
    if (delete_on_close_) {
      state_->present = false;
      state_->size = 0;
      state_->durable_present = false;
      state_->durable_size = 0;
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
    if (offset.value() > Capacity) {
      return std::unexpected(Error::Create(ErrorCode::kFull, "fixed-file capacity exceeded"));
    }
    const std::size_t start = static_cast<std::size_t>(offset.value());
    if (source.size() > Capacity - start) {
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
    if (state_->writes_are_durable) {
      state_->durable_bytes = state_->bytes;
      state_->durable_size = state_->size;
      state_->durable_present = true;
    }
    if (crash_->CutAfterMutation()) {
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected crash after fixed write"));
    }
    return {};
  }

  [[nodiscard]] Status DoTruncate(FileSize size) override {
    if (size.value() > Capacity) {
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
    if (state_->writes_are_durable) {
      state_->durable_bytes = state_->bytes;
      state_->durable_size = state_->size;
      state_->durable_present = true;
    }
    if (crash_->CutAfterMutation()) {
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected crash after fixed truncate"));
    }
    return {};
  }

  [[nodiscard]] Status DoSync(SyncOptions) override {
    std::ranges::copy(state_->bytes, state_->durable_bytes.begin());
    state_->durable_size = state_->size;
    state_->durable_present = state_->present;
    if (crash_->CutAfterMutation()) {
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected crash after fixed sync"));
    }
    return {};
  }

  [[nodiscard]] Result<FileSize> DoSize() override {
    return state_->reported_size.value_or(FileSize{state_->size});
  }

  [[nodiscard]] Status DoLock(DatabaseLock lock) override {
    state_->lock = lock;
    return {};
  }

  [[nodiscard]] Status DoUnlock(DatabaseLock lock) override {
    if (state_->unlock_failure.has_value() && state_->unlock_failure->first == lock) {
      const ErrorCode code = state_->unlock_failure->second;
      state_->unlock_failure.reset();
      return std::unexpected(Error::Create(code, "injected fixed-file unlock failure"));
    }
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

  WritePagerFileState<Capacity>* state_;
  WritePagerCrashState* crash_;
  bool delete_on_close_;
};

template <std::size_t Capacity>
class WritePagerMemoryVfs final : public Vfs {
 public:
  explicit WritePagerMemoryVfs(bool initialize_database = true) {
    if (initialize_database) {
      InitializeDatabase();
    }
  }

  [[nodiscard]] bool journal_present() const noexcept { return journal_.present; }

  [[nodiscard]] DatabaseLock database_lock() const noexcept { return main_.lock; }

  [[nodiscard]] ByteView database_bytes() const noexcept {
    return ByteView{main_.bytes}.first(main_.size);
  }

  [[nodiscard]] std::size_t mutation_count() const noexcept { return crash_.mutation_count; }

  void SetDatabaseWritesDurable(bool durable) noexcept { main_.writes_are_durable = durable; }

  void FailNextDatabaseUnlock(DatabaseLock lock, ErrorCode code) noexcept {
    main_.unlock_failure = std::pair{lock, code};
  }

  void ArmCrashCut(std::optional<std::size_t> cut) noexcept {
    crash_.fail_after_mutation = cut;
    crash_.mutation_count = 0U;
    crash_.cut_triggered = false;
  }

  void Crash() noexcept {
    RestoreDurable(main_);
    RestoreDurable(journal_);
    RestoreDurable(subjournal_);
    RestoreDurable(wal_);
    crash_.fail_after_mutation.reset();
  }

  void LoadDatabase(ByteView bytes) noexcept {
    main_ = {};
    const std::size_t count = std::min(bytes.size(), main_.bytes.size());
    std::ranges::copy(bytes.first(count), main_.bytes.begin());
    std::ranges::copy(bytes.first(count), main_.durable_bytes.begin());
    main_.size = count;
    main_.durable_size = count;
    main_.present = true;
    main_.durable_present = true;
    journal_ = {};
    subjournal_ = {};
    wal_ = {};
    crash_ = {};
  }

  void SetReportedPageCount(std::uint32_t page_count) noexcept {
    Store32(28U, page_count);
    main_.reported_size = FileSize{static_cast<std::uint64_t>(page_count) * kWritePagerPageSize};
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
    main_.durable_bytes = main_.bytes;
    main_.durable_size = main_.size;
    main_.durable_present = true;
  }

  static void RestoreDurable(WritePagerFileState<Capacity>& state) noexcept {
    state.bytes = state.durable_bytes;
    state.size = state.durable_size;
    state.present = state.durable_present;
    state.lock = DatabaseLock::kNone;
  }

  void Store32(std::size_t offset, std::uint32_t value) noexcept {
    StoreBigEndian<std::uint32_t>(
        std::span<std::byte, sizeof(value)>{main_.bytes.data() + offset, sizeof(value)}, value);
  }

  [[nodiscard]] Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                                          FileOpenOptions options) override {
    WritePagerFileState<Capacity>* state = nullptr;
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
          .file = std::make_unique<WritePagerMemoryFile<Capacity>>(*state, crash_,
                                                                   options.delete_on_close),
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
    journal_.durable_present = false;
    journal_.durable_size = 0;
    if (crash_.CutAfterMutation()) {
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected crash after fixed delete"));
    }
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

  WritePagerFileState<Capacity> main_;
  WritePagerFileState<Capacity> journal_;
  WritePagerFileState<Capacity> subjournal_;
  WritePagerFileState<Capacity> wal_;
  WritePagerCrashState crash_;
};

using WritePagerFixedVfs = WritePagerMemoryVfs<kWritePagerFileCapacity>;

[[nodiscard]] inline Status InitializeEmptyBtreeImage(Pager& pager) {
  if (!pager.in_write_transaction() || pager.page_count() != 0U) {
    return std::unexpected(
        Error::Create(ErrorCode::kMisuse, "test image requires an empty writer"));
  }
  auto page = pager.AllocatePage();
  if (!page.has_value()) {
    return std::unexpected(std::move(page.error()));
  }
  const MutableByteView bytes = page->mutable_bytes();
  static constexpr std::array<std::byte, 16> kMagic{
      std::byte{0x53}, std::byte{0x51}, std::byte{0x4c}, std::byte{0x69},
      std::byte{0x74}, std::byte{0x65}, std::byte{0x20}, std::byte{0x66},
      std::byte{0x6f}, std::byte{0x72}, std::byte{0x6d}, std::byte{0x61},
      std::byte{0x74}, std::byte{0x20}, std::byte{0x33}, std::byte{0},
  };
  std::ranges::copy(kMagic, bytes.begin());
  const auto encoded_size = static_cast<std::uint16_t>(
      pager.page_size().value() == 65536U ? 1U : pager.page_size().value());
  StoreBigEndian<std::uint16_t>(std::span<std::byte, 2>{bytes.data() + 16U, 2U}, encoded_size);
  bytes[18] = std::byte{1};
  bytes[19] = std::byte{1};
  bytes[21] = std::byte{64};
  bytes[22] = std::byte{32};
  bytes[23] = std::byte{32};
  StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{bytes.data() + 28U, 4U}, 1U);
  StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{bytes.data() + 44U, 4U}, 4U);
  StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{bytes.data() + 56U, 4U}, 1U);
  bytes[100] = std::byte{0x0d};
  const auto content_offset = static_cast<std::uint16_t>(
      pager.page_size().value() == 65536U ? 0U : pager.page_size().value());
  StoreBigEndian<std::uint16_t>(std::span<std::byte, 2>{bytes.data() + 105U, 2U}, content_offset);
  return {};
}

template <std::size_t Capacity>
[[nodiscard]] inline std::unique_ptr<Pager> OpenWritePager(WritePagerMemoryVfs<Capacity>& vfs,
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
