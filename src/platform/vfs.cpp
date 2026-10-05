#include "modern_sqlite/platform/vfs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include "modern_sqlite/instrumentation/counters.hpp"

namespace modern_sqlite {
namespace {

[[nodiscard]] Error Misuse(std::string message) {
  return Error::Create(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] Error TooLarge(std::string message) {
  return Error::Create(ErrorCode::kTooLarge, std::move(message));
}

[[nodiscard]] Error Internal(std::string message) {
  return Error::Create(ErrorCode::kInternal, std::move(message));
}

[[nodiscard]] bool IsRangeRepresentable(FileOffset offset, std::size_t size) noexcept {
  if (size > std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  const auto length = static_cast<std::uint64_t>(size);
  return length <= std::numeric_limits<std::uint64_t>::max() - offset.value();
}

[[nodiscard]] bool IsValid(SyncMode mode) noexcept {
  switch (mode) {
    case SyncMode::kNormal:
    case SyncMode::kFull:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValidLockTarget(DatabaseLock lock) noexcept {
  switch (lock) {
    case DatabaseLock::kShared:
    case DatabaseLock::kReserved:
    case DatabaseLock::kExclusive:
      return true;
    case DatabaseLock::kNone:
    case DatabaseLock::kPending:
      return false;
  }
  return false;
}

[[nodiscard]] bool IsValidUnlockTarget(DatabaseLock lock) noexcept {
  switch (lock) {
    case DatabaseLock::kNone:
    case DatabaseLock::kShared:
      return true;
    case DatabaseLock::kReserved:
    case DatabaseLock::kPending:
    case DatabaseLock::kExclusive:
      return false;
  }
  return false;
}

[[nodiscard]] bool IsValid(SharedMemoryMapMode mode) noexcept {
  switch (mode) {
    case SharedMemoryMapMode::kExistingOnly:
    case SharedMemoryMapMode::kExtend:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(SharedMemoryLockOperation operation) noexcept {
  switch (operation) {
    case SharedMemoryLockOperation::kLock:
    case SharedMemoryLockOperation::kUnlock:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(SharedMemoryLockMode mode) noexcept {
  switch (mode) {
    case SharedMemoryLockMode::kShared:
    case SharedMemoryLockMode::kExclusive:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(SharedMemoryUnmapMode mode) noexcept {
  switch (mode) {
    case SharedMemoryUnmapMode::kKeep:
    case SharedMemoryUnmapMode::kDelete:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(FileKind kind) noexcept {
  switch (kind) {
    case FileKind::kMainDatabase:
    case FileKind::kTemporaryDatabase:
    case FileKind::kTransientDatabase:
    case FileKind::kMainJournal:
    case FileKind::kTemporaryJournal:
    case FileKind::kSubjournal:
    case FileKind::kSuperJournal:
    case FileKind::kWriteAheadLog:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsTemporary(FileKind kind) noexcept {
  switch (kind) {
    case FileKind::kTemporaryDatabase:
    case FileKind::kTransientDatabase:
    case FileKind::kTemporaryJournal:
    case FileKind::kSubjournal:
      return true;
    case FileKind::kMainDatabase:
    case FileKind::kMainJournal:
    case FileKind::kSuperJournal:
    case FileKind::kWriteAheadLog:
      return false;
  }
  return false;
}

[[nodiscard]] bool IsValid(FileAccessMode access) noexcept {
  switch (access) {
    case FileAccessMode::kReadOnly:
    case FileAccessMode::kReadWrite:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(DirectorySync directory_sync) noexcept {
  switch (directory_sync) {
    case DirectorySync::kNo:
    case DirectorySync::kYes:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(FileAccessQuery query) noexcept {
  switch (query) {
    case FileAccessQuery::kExists:
    case FileAccessQuery::kRead:
    case FileAccessQuery::kReadWrite:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValidPath(std::string_view path) noexcept {
  return !path.empty() && path.find('\0') == std::string_view::npos;
}

[[nodiscard]] Status ValidateOpenOptions(std::optional<std::string_view> path,
                                         FileOpenOptions options) {
  if (!IsValid(options.kind) || !IsValid(options.access)) {
    return std::unexpected(Misuse("file open options contain an invalid enum value"));
  }
  if (path.has_value() && !IsValidPath(*path)) {
    return std::unexpected(Misuse("file paths must be non-empty and contain no NUL bytes"));
  }
  if (options.exclusive_create && !options.create) {
    return std::unexpected(Misuse("exclusive file creation requires create access"));
  }
  if (options.allow_read_only_fallback && (options.exclusive_create || options.delete_on_close)) {
    return std::unexpected(
        Misuse("read-only fallback is incompatible with exclusive or delete-on-close opens"));
  }
  if (options.access == FileAccessMode::kReadOnly &&
      (options.create || options.exclusive_create || options.delete_on_close)) {
    return std::unexpected(Misuse("read-only files cannot be created or deleted on close"));
  }
  if (options.access == FileAccessMode::kReadOnly && options.allow_read_only_fallback) {
    return std::unexpected(Misuse("read-only fallback is valid only for read-write open requests"));
  }
  if (!path.has_value()) {
    if (!IsTemporary(options.kind) || options.access != FileAccessMode::kReadWrite ||
        !options.create || !options.exclusive_create || !options.delete_on_close ||
        options.allow_read_only_fallback || options.no_follow) {
      return std::unexpected(
          Misuse("pathless opens require exclusive delete-on-close temporary files"));
    }
  }
  return {};
}

[[nodiscard]] Status ValidateOpenedFile(const OpenedFile& opened, FileOpenOptions options) {
  if (opened.file == nullptr) {
    return std::unexpected(Internal("the VFS returned a null file"));
  }
  if (!IsValid(opened.access)) {
    return std::unexpected(Internal("the VFS returned an invalid file access mode"));
  }
  if (options.access == FileAccessMode::kReadOnly && opened.access != FileAccessMode::kReadOnly) {
    return std::unexpected(Internal("the VFS elevated a read-only open request"));
  }
  if (options.access == FileAccessMode::kReadWrite && opened.access == FileAccessMode::kReadOnly &&
      !options.allow_read_only_fallback) {
    return std::unexpected(Internal("the VFS used read-only fallback when it was not allowed"));
  }
  return {};
}

[[nodiscard]] bool HasAtomicWriteCapability(DeviceCharacteristics characteristics,
                                            ByteCount size) noexcept {
  switch (size.value()) {
    case 512:
      return characteristics.Has(DeviceCapability::kAtomic512);
    case 1024:
      return characteristics.Has(DeviceCapability::kAtomic1K);
    case 2048:
      return characteristics.Has(DeviceCapability::kAtomic2K);
    case 4096:
      return characteristics.Has(DeviceCapability::kAtomic4K);
    case 8192:
      return characteristics.Has(DeviceCapability::kAtomic8K);
    case 16384:
      return characteristics.Has(DeviceCapability::kAtomic16K);
    case 32768:
      return characteristics.Has(DeviceCapability::kAtomic32K);
    case 65536:
      return characteristics.Has(DeviceCapability::kAtomic64K);
    default:
      return false;
  }
}

}  // namespace

bool DeviceCharacteristics::SupportsAtomicWriteSize(ByteCount size) const noexcept {
  return size.value() != 0 &&
         (Has(DeviceCapability::kAtomic) || HasAtomicWriteCapability(*this, size));
}

Result<FileReadResult> File::ReadAt(MutableByteView destination, FileOffset offset) {
  if (destination.empty()) {
    return FileReadResult{ByteCount{0}, true};
  }
  if (!IsRangeRepresentable(offset, destination.size())) {
    return std::unexpected(TooLarge("the file read range is not representable"));
  }

  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  auto bytes_read = DoReadAt(destination, offset);
  if (!bytes_read.has_value()) {
    return std::unexpected(std::move(bytes_read.error()));
  }
  if (bytes_read->value() > destination.size()) {
    std::ranges::fill(destination, std::byte{0});
    return std::unexpected(Internal("the file backend over-reported a read result"));
  }

  std::ranges::fill(destination.subspan(bytes_read->value()), std::byte{0});
  return FileReadResult{*bytes_read, bytes_read->value() == destination.size()};
}

Status File::WriteAt(ByteView source, FileOffset offset) {
  if (source.empty()) {
    return {};
  }
  if (!IsRangeRepresentable(offset, source.size())) {
    return std::unexpected(TooLarge("the file write range is not representable"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoWriteAt(source, offset);
}

Status File::Truncate(FileSize size) {
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoTruncate(size);
}

Status File::Sync(SyncOptions options) {
  if (!IsValid(options.mode)) {
    return std::unexpected(Misuse("the sync mode is invalid"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoSync(options);
}

Result<FileSize> File::Size() {
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoSize();
}

Status File::Lock(DatabaseLock lock) {
  if (!IsValidLockTarget(lock)) {
    return std::unexpected(Misuse("the requested database lock is invalid"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoLock(lock);
}

Status File::Unlock(DatabaseLock lock) {
  if (!IsValidUnlockTarget(lock)) {
    return std::unexpected(Misuse("the requested database unlock target is invalid"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoUnlock(lock);
}

Result<bool> File::HasReservedLock() {
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoHasReservedLock();
}

Result<FileProperties> File::Properties() const {
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  const FileProperties properties = DoProperties();
  if (properties.sector_size.value() == 0) {
    return std::unexpected(Internal("the file backend returned a zero sector size"));
  }
  return properties;
}

Result<std::optional<MutableByteView>> File::MapSharedMemory(SharedMemoryRegionIndex region,
                                                             ByteCount region_size,
                                                             SharedMemoryMapMode mode) {
  if (region_size.value() == 0 || !IsValid(mode)) {
    return std::unexpected(Misuse("the shared-memory map request is invalid"));
  }
  if (region_size.value() > std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TooLarge("the shared-memory map range is not representable"));
  }
  const auto size = static_cast<std::uint64_t>(region_size.value());
  const auto region_number = static_cast<std::uint64_t>(region.value());
  if ((region_number != 0 && size > std::numeric_limits<std::uint64_t>::max() / region_number) ||
      size > std::numeric_limits<std::uint64_t>::max() - (region_number * size)) {
    return std::unexpected(TooLarge("the shared-memory map range is not representable"));
  }

  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  auto mapping = DoMapSharedMemory(region, region_size, mode);
  if (!mapping.has_value()) {
    return std::unexpected(std::move(mapping.error()));
  }
  if (mapping->has_value() && mapping->value().size() != region_size.value()) {
    return std::unexpected(
        Internal("the file backend returned an unexpected shared-memory region size"));
  }
  return mapping;
}

Status File::LockSharedMemory(SharedMemoryLockRange range, SharedMemoryLockOperation operation,
                              SharedMemoryLockMode mode) {
  const auto range_end = static_cast<std::uint16_t>(static_cast<std::uint16_t>(range.offset) +
                                                    static_cast<std::uint16_t>(range.count));
  if (range.count == 0 || range_end > kSharedMemoryLockSlotCount ||
      (mode == SharedMemoryLockMode::kShared && range.count != 1) || !IsValid(operation) ||
      !IsValid(mode)) {
    return std::unexpected(Misuse("the shared-memory lock request is invalid"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoLockSharedMemory(range, operation, mode);
}

void File::SharedMemoryBarrier() noexcept {
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  DoSharedMemoryBarrier();
}

Status File::UnmapSharedMemory(SharedMemoryUnmapMode mode) {
  if (!IsValid(mode)) {
    return std::unexpected(Misuse("the shared-memory unmap mode is invalid"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoUnmapSharedMemory(mode);
}

Result<OpenedFile> Vfs::Open(std::optional<std::string_view> path, FileOpenOptions options) {
  auto validated = ValidateOpenOptions(path, options);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }

  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  auto opened = DoOpen(path, options);
  if (!opened.has_value()) {
    return std::unexpected(std::move(opened.error()));
  }
  validated = ValidateOpenedFile(*opened, options);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }
  return opened;
}

Status Vfs::Delete(std::string_view path, DirectorySync directory_sync) {
  if (!IsValidPath(path) || !IsValid(directory_sync)) {
    return std::unexpected(Misuse("the file deletion request is invalid"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  try {
    return DoDelete(path, directory_sync);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<bool> Vfs::Access(std::string_view path, FileAccessQuery query) {
  if (!IsValidPath(path) || !IsValid(query)) {
    return std::unexpected(Misuse("the file access request is invalid"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoAccess(path, query);
}

Result<std::string> Vfs::FullPath(std::string_view path) {
  if (!IsValidPath(path)) {
    return std::unexpected(Misuse("the input path is invalid"));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  auto full_path = DoFullPath(path);
  if (!full_path.has_value()) {
    return std::unexpected(std::move(full_path.error()));
  }
  if (!IsValidPath(*full_path)) {
    return std::unexpected(Internal("the VFS returned an invalid full path"));
  }
  return full_path;
}

Status Vfs::RandomBytes(MutableByteView output) {
  if (output.empty()) {
    return {};
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  auto bytes_written = DoRandomBytes(output);
  if (!bytes_written.has_value()) {
    return std::unexpected(std::move(bytes_written.error()));
  }
  if (bytes_written->value() != output.size()) {
    std::ranges::fill(output, std::byte{0});
    return std::unexpected(Internal("the VFS did not fill the randomness destination"));
  }
  return {};
}

Result<std::chrono::microseconds> Vfs::SleepFor(std::chrono::microseconds duration) {
  if (duration.count() < 0) {
    return std::unexpected(Misuse("sleep duration cannot be negative"));
  }
  if (duration.count() == 0) {
    return std::chrono::microseconds{0};
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  auto elapsed = DoSleepFor(duration);
  if (!elapsed.has_value()) {
    return std::unexpected(std::move(elapsed.error()));
  }
  if (*elapsed < duration) {
    return std::unexpected(Internal("the VFS under-reported elapsed sleep time"));
  }
  return elapsed;
}

Result<WallClockTime> Vfs::CurrentTime() {
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVfsCalls, 1U);
  return DoCurrentTime();
}

ByteCount Vfs::MaximumPathLength() const noexcept { return DoMaximumPathLength(); }

}  // namespace modern_sqlite
