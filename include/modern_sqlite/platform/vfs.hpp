#ifndef MODERN_SQLITE_PLATFORM_VFS_HPP_
#define MODERN_SQLITE_PLATFORM_VFS_HPP_

#include <chrono>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"

namespace modern_sqlite {

class FileOffset {
 public:
  constexpr FileOffset() noexcept = default;
  constexpr explicit FileOffset(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const FileOffset&) const noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

class FileSize {
 public:
  constexpr FileSize() noexcept = default;
  constexpr explicit FileSize(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const FileSize&) const noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

class SharedMemoryRegionIndex {
 public:
  constexpr SharedMemoryRegionIndex() noexcept = default;
  constexpr explicit SharedMemoryRegionIndex(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const SharedMemoryRegionIndex&) const noexcept = default;

 private:
  std::uint32_t value_ = 0;
};

class FileReadResult {
 public:
  [[nodiscard]] constexpr ByteCount bytes_read() const noexcept { return bytes_read_; }
  [[nodiscard]] constexpr bool complete() const noexcept { return complete_; }

 private:
  friend class File;

  constexpr FileReadResult(ByteCount bytes_read, bool complete) noexcept
      : bytes_read_(bytes_read), complete_(complete) {}

  ByteCount bytes_read_;
  bool complete_ = false;
};

enum class SyncMode : std::uint8_t {
  kNormal,
  kFull,
};

struct SyncOptions {
  SyncMode mode = SyncMode::kNormal;
  bool data_only = false;

  constexpr bool operator==(const SyncOptions&) const noexcept = default;
};

enum class DatabaseLock : std::uint8_t {
  kNone,
  kShared,
  kReserved,
  kPending,
  kExclusive,
};

enum class DeviceCapability : std::uint32_t {
  kAtomic = 1U << 0U,
  kAtomic512 = 1U << 1U,
  kAtomic1K = 1U << 2U,
  kAtomic2K = 1U << 3U,
  kAtomic4K = 1U << 4U,
  kAtomic8K = 1U << 5U,
  kAtomic16K = 1U << 6U,
  kAtomic32K = 1U << 7U,
  kAtomic64K = 1U << 8U,
  kSafeAppend = 1U << 9U,
  kSequential = 1U << 10U,
  kUndeletableWhenOpen = 1U << 11U,
  kPowersafeOverwrite = 1U << 12U,
  kImmutable = 1U << 13U,
  kBatchAtomic = 1U << 14U,
  kSubpageRead = 1U << 15U,
};

class DeviceCharacteristics {
 public:
  constexpr DeviceCharacteristics() noexcept = default;

  [[nodiscard]] constexpr DeviceCharacteristics With(DeviceCapability capability) const noexcept {
    return DeviceCharacteristics{bits_ | static_cast<std::uint32_t>(capability)};
  }

  [[nodiscard]] constexpr bool Has(DeviceCapability capability) const noexcept {
    return (bits_ & static_cast<std::uint32_t>(capability)) != 0;
  }

  [[nodiscard]] bool SupportsAtomicWriteSize(ByteCount size) const noexcept;

  constexpr auto operator<=>(const DeviceCharacteristics&) const noexcept = default;

 private:
  constexpr explicit DeviceCharacteristics(std::uint32_t bits) noexcept : bits_(bits) {}

  std::uint32_t bits_ = 0;
};

struct FileProperties {
  ByteCount sector_size;
  DeviceCharacteristics device_characteristics;

  constexpr bool operator==(const FileProperties&) const noexcept = default;
};

inline constexpr std::uint8_t kSharedMemoryLockSlotCount = 8;

struct SharedMemoryLockRange {
  std::uint8_t offset = 0;
  std::uint8_t count = 0;

  constexpr bool operator==(const SharedMemoryLockRange&) const noexcept = default;
};

enum class SharedMemoryMapMode : std::uint8_t {
  kExistingOnly,
  kExtend,
};

enum class SharedMemoryLockOperation : std::uint8_t {
  kLock,
  kUnlock,
};

enum class SharedMemoryLockMode : std::uint8_t {
  kShared,
  kExclusive,
};

enum class SharedMemoryUnmapMode : std::uint8_t {
  kKeep,
  kDelete,
};

class File {
 public:
  File() = default;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&&) = delete;
  File& operator=(File&&) = delete;
  virtual ~File() = default;

  [[nodiscard]] Result<FileReadResult> ReadAt(MutableByteView destination, FileOffset offset);
  [[nodiscard]] Status WriteAt(ByteView source, FileOffset offset);
  [[nodiscard]] Status Truncate(FileSize size);
  [[nodiscard]] Status Sync(SyncOptions options);
  [[nodiscard]] Result<FileSize> Size();

  [[nodiscard]] Status Lock(DatabaseLock lock);
  [[nodiscard]] Status Unlock(DatabaseLock lock);
  [[nodiscard]] Result<bool> HasReservedLock();
  [[nodiscard]] Result<FileProperties> Properties() const;

  [[nodiscard]] Result<std::optional<MutableByteView>> MapSharedMemory(
      SharedMemoryRegionIndex region, ByteCount region_size, SharedMemoryMapMode mode);
  [[nodiscard]] Status LockSharedMemory(SharedMemoryLockRange range,
                                        SharedMemoryLockOperation operation,
                                        SharedMemoryLockMode mode);
  void SharedMemoryBarrier() noexcept;
  [[nodiscard]] Status UnmapSharedMemory(SharedMemoryUnmapMode mode);

 protected:
  [[nodiscard]] virtual Result<ByteCount> DoReadAt(MutableByteView destination,
                                                   FileOffset offset) = 0;
  [[nodiscard]] virtual Status DoWriteAt(ByteView source, FileOffset offset) = 0;
  [[nodiscard]] virtual Status DoTruncate(FileSize size) = 0;
  [[nodiscard]] virtual Status DoSync(SyncOptions options) = 0;
  [[nodiscard]] virtual Result<FileSize> DoSize() = 0;

  [[nodiscard]] virtual Status DoLock(DatabaseLock lock) = 0;
  [[nodiscard]] virtual Status DoUnlock(DatabaseLock lock) = 0;
  [[nodiscard]] virtual Result<bool> DoHasReservedLock() = 0;
  [[nodiscard]] virtual FileProperties DoProperties() const noexcept = 0;

  [[nodiscard]] virtual Result<std::optional<MutableByteView>> DoMapSharedMemory(
      SharedMemoryRegionIndex region, ByteCount region_size, SharedMemoryMapMode mode) = 0;
  [[nodiscard]] virtual Status DoLockSharedMemory(SharedMemoryLockRange range,
                                                  SharedMemoryLockOperation operation,
                                                  SharedMemoryLockMode mode) = 0;
  virtual void DoSharedMemoryBarrier() noexcept = 0;
  [[nodiscard]] virtual Status DoUnmapSharedMemory(SharedMemoryUnmapMode mode) = 0;
};

enum class FileKind : std::uint8_t {
  kMainDatabase,
  kTemporaryDatabase,
  kTransientDatabase,
  kMainJournal,
  kTemporaryJournal,
  kSubjournal,
  kSuperJournal,
  kWriteAheadLog,
};

enum class FileAccessMode : std::uint8_t {
  kReadOnly,
  kReadWrite,
};

struct FileOpenOptions {
  FileKind kind = FileKind::kMainDatabase;
  FileAccessMode access = FileAccessMode::kReadOnly;
  bool create = false;
  bool exclusive_create = false;
  bool delete_on_close = false;
  bool allow_read_only_fallback = false;
  bool no_follow = false;

  constexpr bool operator==(const FileOpenOptions&) const noexcept = default;
};

struct OpenedFile {
  std::unique_ptr<File> file;
  FileAccessMode access = FileAccessMode::kReadOnly;
};

enum class DirectorySync : std::uint8_t {
  kNo,
  kYes,
};

enum class FileAccessQuery : std::uint8_t {
  kExists,
  kRead,
  kReadWrite,
};

using WallClockTime = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;

class Vfs {
 public:
  Vfs() = default;
  Vfs(const Vfs&) = delete;
  Vfs& operator=(const Vfs&) = delete;
  Vfs(Vfs&&) = delete;
  Vfs& operator=(Vfs&&) = delete;
  virtual ~Vfs() = default;

  [[nodiscard]] Result<OpenedFile> Open(std::optional<std::string_view> path,
                                        FileOpenOptions options);
  [[nodiscard]] Status Delete(std::string_view path, DirectorySync directory_sync);
  [[nodiscard]] Result<bool> Access(std::string_view path, FileAccessQuery query);
  [[nodiscard]] Result<std::string> FullPath(std::string_view path);
  [[nodiscard]] Status RandomBytes(MutableByteView output);
  [[nodiscard]] Result<std::chrono::microseconds> SleepFor(std::chrono::microseconds duration);
  [[nodiscard]] Result<WallClockTime> CurrentTime();
  [[nodiscard]] ByteCount MaximumPathLength() const noexcept;

 protected:
  [[nodiscard]] virtual Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                                                  FileOpenOptions options) = 0;
  [[nodiscard]] virtual Status DoDelete(std::string_view path, DirectorySync directory_sync) = 0;
  [[nodiscard]] virtual Result<bool> DoAccess(std::string_view path, FileAccessQuery query) = 0;
  [[nodiscard]] virtual Result<std::string> DoFullPath(std::string_view path) = 0;
  [[nodiscard]] virtual Result<ByteCount> DoRandomBytes(MutableByteView output) = 0;
  [[nodiscard]] virtual Result<std::chrono::microseconds> DoSleepFor(
      std::chrono::microseconds duration) = 0;
  [[nodiscard]] virtual Result<WallClockTime> DoCurrentTime() = 0;
  [[nodiscard]] virtual ByteCount DoMaximumPathLength() const noexcept = 0;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_PLATFORM_VFS_HPP_
