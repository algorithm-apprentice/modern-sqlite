#include "modern_sqlite/platform/posix_vfs.hpp"

#if !defined(__APPLE__) && !defined(__linux__)
#error "PosixVfs currently supports only macOS and Linux"
#endif

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/platform/vfs.hpp"

namespace modern_sqlite {
namespace {

static_assert(std::numeric_limits<off_t>::is_signed);

constexpr off_t kPendingByte = 0x40000000;
constexpr off_t kReservedByte = kPendingByte + 1;
constexpr off_t kSharedFirst = kPendingByte + 2;
constexpr off_t kSharedSize = 510;
constexpr off_t kSharedMemoryLockBase = 120;
constexpr off_t kSharedMemoryDeadmanSwitch = 128;
constexpr std::size_t kRandomnessChunkSize = 256;

#ifdef O_CLOEXEC
constexpr int kCloseOnExecOpenFlag = O_CLOEXEC;
#else
constexpr int kCloseOnExecOpenFlag = 0;
#endif

[[nodiscard]] ErrorCode MapErrno(int error_number, ErrorCode fallback,
                                 bool write_operation = false) noexcept {
  switch (error_number) {
    case EACCES:
    case EPERM:
      return ErrorCode::kPermissionDenied;
    case EROFS:
      return ErrorCode::kReadOnly;
    case ENOMEM:
      return ErrorCode::kOutOfMemory;
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
      return ErrorCode::kFull;
    case EFBIG:
      return write_operation ? ErrorCode::kFull : ErrorCode::kTooLarge;
#ifdef EOVERFLOW
    case EOVERFLOW:
      return ErrorCode::kTooLarge;
#endif
    default:
      return fallback;
  }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Error SystemError(ErrorCode fallback, std::string_view operation,
                                std::string_view path, int error_number,
                                bool write_operation = false) {
  std::string message{operation};
  message += " failed";
  if (!path.empty()) {
    message += " for ";
    message.append(path);
  }
  message += ": ";
  message += std::generic_category().message(error_number);
  return Error::Create(MapErrno(error_number, fallback, write_operation), std::move(message));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] Error BusyError(std::string_view operation, std::string_view path) {
  std::string message{operation};
  message += " is blocked by another lock";
  if (!path.empty()) {
    message += " for ";
    message.append(path);
  }
  return Error::Create(ErrorCode::kBusy, std::move(message));
}

[[nodiscard]] Error MisuseError(std::string message) {
  return Error::Create(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] Error ReadOnlyError(std::string message) {
  return Error::Create(ErrorCode::kReadOnly, std::move(message));
}

[[nodiscard]] Error TooLargeError(std::string message) {
  return Error::Create(ErrorCode::kTooLarge, std::move(message));
}

[[nodiscard]] Error IoError(std::string message) {
  return Error::Create(ErrorCode::kIo, std::move(message));
}

void CloseNoRetry(int descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
  }
}

class ScopedDescriptor final {
 public:
  ScopedDescriptor() noexcept = default;
  explicit ScopedDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}

  ScopedDescriptor(const ScopedDescriptor&) = delete;
  ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

  ScopedDescriptor(ScopedDescriptor&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}

  ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept {
    if (this != &other) {
      CloseNoRetry(descriptor_);
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }

  ~ScopedDescriptor() { CloseNoRetry(descriptor_); }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }
  [[nodiscard]] int release() noexcept { return std::exchange(descriptor_, -1); }

 private:
  int descriptor_ = -1;
};

[[nodiscard]] Status SetCloseOnExec(int descriptor, std::string_view path) {
#ifndef O_CLOEXEC
  int flags = -1;
  do {
    flags = ::fcntl(descriptor, F_GETFD);
  } while (flags == -1 && errno == EINTR);
  if (flags == -1) {
    return std::unexpected(SystemError(ErrorCode::kIo, "fcntl(F_GETFD)", path, errno));
  }

  int result = -1;
  do {
    result = ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    return std::unexpected(SystemError(ErrorCode::kIo, "fcntl(F_SETFD)", path, errno));
  }
#else
  static_cast<void>(descriptor);
  static_cast<void>(path);
#endif
  return {};
}

[[nodiscard]] int OpenFile(std::string_view path, int flags, mode_t mode) {
  const std::string owned_path{path};
  int descriptor = -1;
  do {
    descriptor = ::open(owned_path.c_str(), flags, mode);
  } while (descriptor == -1 && errno == EINTR);
  return descriptor;
}

[[nodiscard]] int OpenDirectory(std::string_view path) {
  return OpenFile(path, O_RDONLY | O_DIRECTORY | kCloseOnExecOpenFlag, 0);
}

[[nodiscard]] Status CloseChecked(int descriptor, std::string_view path) {
  if (::close(descriptor) == 0) {
    return {};
  }
  return std::unexpected(SystemError(ErrorCode::kIo, "close", path, errno));
}

[[nodiscard]] Result<struct stat> DescriptorStatus(int descriptor, std::string_view path) {
  struct stat status{};
  int result = -1;
  do {
    result = ::fstat(descriptor, &status);
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    return std::unexpected(SystemError(ErrorCode::kIo, "fstat", path, errno));
  }
  return status;
}

[[nodiscard]] bool IsPosixRangeRepresentable(FileOffset offset, std::size_t size) noexcept {
  const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  if (offset.value() > maximum) {
    return false;
  }
  const auto length = static_cast<std::uint64_t>(size);
  return length <= maximum - offset.value();
}

[[nodiscard]] bool IsPosixSizeRepresentable(FileSize size) noexcept {
  return size.value() <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
}

struct AdvisoryLockRange {
  int type = F_UNLCK;
  off_t start = 0;
  off_t length = 0;
};

enum class LockConflict : std::uint8_t {
  kBusy,
  kIo,
};

[[nodiscard]] Status SetAdvisoryLock(int descriptor, AdvisoryLockRange range, std::string_view path,
                                     LockConflict conflict) {
  struct flock lock{};
  lock.l_type = static_cast<short>(range.type);
  lock.l_whence = SEEK_SET;
  lock.l_start = range.start;
  lock.l_len = range.length;

  int result = -1;
  do {
    result = ::fcntl(descriptor, F_SETLK, &lock);
  } while (result == -1 && errno == EINTR);
  if (result == 0) {
    return {};
  }
  const int error_number = errno;
  if (conflict == LockConflict::kBusy && (error_number == EACCES || error_number == EAGAIN)) {
    return std::unexpected(BusyError("fcntl(F_SETLK)", path));
  }
  return std::unexpected(SystemError(ErrorCode::kIo, "fcntl(F_SETLK)", path, error_number));
}

void BestEffortSetAdvisoryLock(int descriptor, AdvisoryLockRange range) noexcept {
  struct flock lock{};
  lock.l_type = static_cast<short>(range.type);
  lock.l_whence = SEEK_SET;
  lock.l_start = range.start;
  lock.l_len = range.length;

  int result = -1;
  do {
    result = ::fcntl(descriptor, F_SETLK, &lock);
  } while (result == -1 && errno == EINTR);
}

[[nodiscard]] Result<int> QueryAdvisoryLock(int descriptor, AdvisoryLockRange range,
                                            std::string_view path) {
  struct flock lock{};
  lock.l_type = static_cast<short>(range.type);
  lock.l_whence = SEEK_SET;
  lock.l_start = range.start;
  lock.l_len = range.length;

  int result = -1;
  do {
    result = ::fcntl(descriptor, F_GETLK, &lock);
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    return std::unexpected(SystemError(ErrorCode::kIo, "fcntl(F_GETLK)", path, errno));
  }
  return static_cast<int>(lock.l_type);
}

[[nodiscard]] Status TruncateDescriptor(int descriptor, off_t size, std::string_view path) {
  int result = -1;
  do {
    result = ::ftruncate(descriptor, size);
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    return std::unexpected(SystemError(ErrorCode::kIo, "ftruncate", path, errno, true));
  }
  return {};
}

[[nodiscard]] Status SyncDescriptor(int descriptor, SyncOptions options, std::string_view path) {
  int result = -1;
#if defined(__APPLE__)
  if (options.mode == SyncMode::kFull) {
    do {
      result = ::fcntl(descriptor, F_FULLFSYNC);
    } while (result == -1 && errno == EINTR);
    if (result == 0) {
      return {};
    }
  }
  do {
    result = ::fsync(descriptor);
  } while (result == -1 && errno == EINTR);
#else
  if (options.mode == SyncMode::kNormal && options.data_only) {
    do {
      result = ::fdatasync(descriptor);
    } while (result == -1 && errno == EINTR);
  } else {
    do {
      result = ::fsync(descriptor);
    } while (result == -1 && errno == EINTR);
  }
#endif
  if (result == -1) {
    return std::unexpected(SystemError(ErrorCode::kIo, "sync", path, errno));
  }
  return {};
}

[[nodiscard]] Status SyncDirectoryDescriptor(int descriptor, std::string_view path) {
  int result = -1;
  do {
    result = ::fsync(descriptor);
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    return std::unexpected(SystemError(ErrorCode::kIo, "fsync directory", path, errno));
  }
  return {};
}

struct FileIdentity {
  dev_t device{};
  ino_t inode{};

  [[nodiscard]] bool operator<(const FileIdentity& other) const noexcept {
    if (device < other.device) {
      return true;
    }
    if (other.device < device) {
      return false;
    }
    return inode < other.inode;
  }
};

struct SharedMemoryState;

struct DeferredDescriptor {
  int descriptor = -1;
  std::unique_ptr<DeferredDescriptor> next;
};

struct InodeState {
  std::mutex mutex;
  DatabaseLock process_lock = DatabaseLock::kNone;
  std::size_t shared_count = 0;
  std::optional<std::uint64_t> elevated_owner;
  std::unique_ptr<DeferredDescriptor> deferred_descriptors;
  std::weak_ptr<SharedMemoryState> shared_memory;
  std::string database_path;

  ~InodeState() {
    while (deferred_descriptors != nullptr) {
      auto descriptor = std::move(deferred_descriptors);
      deferred_descriptors = std::move(descriptor->next);
      CloseNoRetry(descriptor->descriptor);
    }
  }
};

[[nodiscard]] std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

[[nodiscard]] std::map<FileIdentity, std::weak_ptr<InodeState>>& InodeRegistry() {
  static std::map<FileIdentity, std::weak_ptr<InodeState>> registry;
  return registry;
}

[[nodiscard]] std::shared_ptr<InodeState> RegisterInode(const struct stat& status) {
  const FileIdentity identity{
      .device = status.st_dev,
      .inode = status.st_ino,
  };
  const std::scoped_lock lock{RegistryMutex()};
  auto& registry = InodeRegistry();
  std::erase_if(registry, [](const auto& entry) { return entry.second.expired(); });
  auto& entry = registry[identity];
  auto state = entry.lock();
  if (state == nullptr) {
    state = std::make_shared<InodeState>();
    entry = state;
  }
  return state;
}

void CloseDeferredDescriptorsLocked(InodeState& state) noexcept {
  while (state.deferred_descriptors != nullptr) {
    auto descriptor = std::move(state.deferred_descriptors);
    state.deferred_descriptors = std::move(descriptor->next);
    CloseNoRetry(descriptor->descriptor);
  }
}

void ReleaseRegisteredDescriptor(const std::shared_ptr<InodeState>& state, int descriptor,
                                 std::unique_ptr<DeferredDescriptor> deferred_descriptor) noexcept {
  if (descriptor < 0) {
    return;
  }
  const std::scoped_lock lock{state->mutex};
  if (state->shared_count != 0) {
    deferred_descriptor->descriptor = descriptor;
    deferred_descriptor->next = std::move(state->deferred_descriptors);
    state->deferred_descriptors = std::move(deferred_descriptor);
    return;
  }
  CloseNoRetry(descriptor);
  CloseDeferredDescriptorsLocked(*state);
}

class RegisteredDescriptor final {
 public:
  RegisteredDescriptor() = default;
  RegisteredDescriptor(std::shared_ptr<InodeState> state, int descriptor,
                       std::unique_ptr<DeferredDescriptor> deferred_descriptor)
      : state_(std::move(state)),
        descriptor_(descriptor),
        deferred_descriptor_(std::move(deferred_descriptor)) {}

  RegisteredDescriptor(const RegisteredDescriptor&) = delete;
  RegisteredDescriptor& operator=(const RegisteredDescriptor&) = delete;

  RegisteredDescriptor(RegisteredDescriptor&& other) noexcept
      : state_(std::move(other.state_)),
        descriptor_(std::exchange(other.descriptor_, -1)),
        deferred_descriptor_(std::move(other.deferred_descriptor_)) {}

  RegisteredDescriptor& operator=(RegisteredDescriptor&& other) noexcept {
    if (this != &other) {
      ReleaseRegisteredDescriptor(state_, descriptor_, std::move(deferred_descriptor_));
      state_ = std::move(other.state_);
      descriptor_ = std::exchange(other.descriptor_, -1);
      deferred_descriptor_ = std::move(other.deferred_descriptor_);
    }
    return *this;
  }

  ~RegisteredDescriptor() {
    ReleaseRegisteredDescriptor(state_, descriptor_, std::move(deferred_descriptor_));
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] const std::shared_ptr<InodeState>& state() const noexcept { return state_; }

 private:
  std::shared_ptr<InodeState> state_;
  int descriptor_ = -1;
  std::unique_ptr<DeferredDescriptor> deferred_descriptor_;
};

class MappedRegion final {
 public:
  MappedRegion() noexcept = default;
  MappedRegion(void* mapping, std::size_t mapping_size, std::byte* data) noexcept
      : mapping_(mapping), mapping_size_(mapping_size), data_(data) {}

  MappedRegion(const MappedRegion&) = delete;
  MappedRegion& operator=(const MappedRegion&) = delete;

  MappedRegion(MappedRegion&& other) noexcept
      : mapping_(std::exchange(other.mapping_, MAP_FAILED)),
        mapping_size_(std::exchange(other.mapping_size_, 0)),
        data_(std::exchange(other.data_, nullptr)) {}

  MappedRegion& operator=(MappedRegion&& other) noexcept {
    if (this != &other) {
      Reset();
      mapping_ = std::exchange(other.mapping_, MAP_FAILED);
      mapping_size_ = std::exchange(other.mapping_size_, 0);
      data_ = std::exchange(other.data_, nullptr);
    }
    return *this;
  }

  ~MappedRegion() { Reset(); }

  [[nodiscard]] std::byte* data() const noexcept { return data_; }

 private:
  void Reset() noexcept {
    if (mapping_ != MAP_FAILED) {
      static_cast<void>(::munmap(mapping_, mapping_size_));
      mapping_ = MAP_FAILED;
      mapping_size_ = 0;
      data_ = nullptr;
    }
  }

  void* mapping_ = MAP_FAILED;
  std::size_t mapping_size_ = 0;
  std::byte* data_ = nullptr;
};

struct SharedMemoryState {
  std::mutex mutex;
  int descriptor = -1;
  std::string path;
  std::size_t region_size = 0;
  std::vector<std::unique_ptr<MappedRegion>> mappings;
  std::array<int, kSharedMemoryLockSlotCount> locks{};
  std::size_t connections = 0;
  bool delete_requested = false;

  ~SharedMemoryState() { CloseNoRetry(descriptor); }
};

[[nodiscard]] int LockRank(DatabaseLock lock) noexcept {
  switch (lock) {
    case DatabaseLock::kNone:
      return 0;
    case DatabaseLock::kShared:
      return 1;
    case DatabaseLock::kReserved:
      return 2;
    case DatabaseLock::kPending:
      return 3;
    case DatabaseLock::kExclusive:
      return 4;
  }
  return -1;
}

[[nodiscard]] bool IsDirectorySyncKind(FileKind kind) noexcept {
  switch (kind) {
    case FileKind::kMainJournal:
    case FileKind::kSuperJournal:
    case FileKind::kWriteAheadLog:
      return true;
    case FileKind::kMainDatabase:
    case FileKind::kTemporaryDatabase:
    case FileKind::kTransientDatabase:
    case FileKind::kTemporaryJournal:
    case FileKind::kSubjournal:
      return false;
  }
  return false;
}

[[nodiscard]] Result<std::string> CanonicalExistingPath(std::string_view path) {
  std::error_code error;
  const std::filesystem::path canonical =
      std::filesystem::canonical(std::filesystem::path{path}, error);
  if (error) {
    return std::unexpected(SystemError(ErrorCode::kCannotOpen, "canonical", path, error.value()));
  }
  return canonical.string();
}

[[nodiscard]] std::string ParentPath(std::string_view path) {
  std::filesystem::path parent = std::filesystem::path{path}.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  return parent.string();
}

[[nodiscard]] Result<mode_t> CreationMode(std::string_view path, FileOpenOptions options) {
  if (options.delete_on_close) {
    return static_cast<mode_t>(0600);
  }

  std::string_view database_path;
  if (options.kind == FileKind::kMainJournal && path.ends_with("-journal")) {
    database_path = path.substr(0, path.size() - std::string_view{"-journal"}.size());
  } else if (options.kind == FileKind::kWriteAheadLog && path.ends_with("-wal")) {
    database_path = path.substr(0, path.size() - std::string_view{"-wal"}.size());
  }
  if (database_path.empty()) {
    return static_cast<mode_t>(0644);
  }

  const std::string owned_database_path{database_path};
  struct stat status{};
  int result = -1;
  do {
    result = ::stat(owned_database_path.c_str(), &status);
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    return std::unexpected(
        SystemError(ErrorCode::kCannotOpen, "stat database permissions", database_path, errno));
  }
  return status.st_mode & static_cast<mode_t>(0777);
}

[[nodiscard]] std::uint16_t SharedMemoryMask(SharedMemoryLockRange range) noexcept {
  const auto high = std::uint32_t{1} << static_cast<std::uint32_t>(range.offset + range.count);
  const auto low = std::uint32_t{1} << static_cast<std::uint32_t>(range.offset);
  return static_cast<std::uint16_t>(high - low);
}

[[nodiscard]] std::uint16_t SharedMemoryBit(std::uint8_t slot) noexcept {
  return static_cast<std::uint16_t>(std::uint32_t{1} << static_cast<std::uint32_t>(slot));
}

[[nodiscard]] bool HasAllBits(std::uint16_t value, std::uint16_t mask) noexcept {
  return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(mask)) ==
         static_cast<std::uint32_t>(mask);
}

[[nodiscard]] bool HasAnyBits(std::uint16_t value, std::uint16_t mask) noexcept {
  return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(mask)) != 0U;
}

[[nodiscard]] std::uint16_t AddBits(std::uint16_t value, std::uint16_t mask) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint32_t>(value) |
                                    static_cast<std::uint32_t>(mask));
}

[[nodiscard]] std::uint16_t ClearBits(std::uint16_t value, std::uint16_t mask) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint32_t>(value) &
                                    ~static_cast<std::uint32_t>(mask));
}

[[nodiscard]] Status ExtendSharedMemoryFile(const SharedMemoryState& state,
                                            std::uint64_t current_size,
                                            std::uint64_t requested_size) {
  if (current_size >= requested_size) {
    return {};
  }

  const long page_size_result = ::sysconf(_SC_PAGESIZE);
  if (page_size_result <= 0) {
    return std::unexpected(IoError("sysconf(_SC_PAGESIZE) failed"));
  }
  const auto page_size = static_cast<std::uint64_t>(page_size_result);
  const std::byte zero{0};
  std::uint64_t target =
      std::min((((current_size / page_size) + 1U) * page_size) - 1U, requested_size - 1U);

  while (true) {
    ssize_t written = -1;
    do {
      written = ::pwrite(state.descriptor, &zero, 1, static_cast<off_t>(target));
    } while (written == -1 && errno == EINTR);
    if (written != 1) {
      const int error_number = written == -1 ? errno : EIO;
      return std::unexpected(
          SystemError(ErrorCode::kIo, "pwrite shared memory", state.path, error_number, true));
    }
    if (target == requested_size - 1U) {
      break;
    }
    target = std::min(target + page_size, requested_size - 1U);
  }
  return {};
}

[[nodiscard]] Result<std::shared_ptr<SharedMemoryState>> CreateSharedMemoryState(
    int database_descriptor, const std::string& database_path) {
  auto database_status = DescriptorStatus(database_descriptor, database_path);
  if (!database_status.has_value()) {
    return std::unexpected(std::move(database_status.error()));
  }

  auto state = std::make_shared<SharedMemoryState>();
  state->path = database_path + "-shm";
  const mode_t mode = database_status->st_mode & static_cast<mode_t>(0777);
  state->descriptor =
      OpenFile(state->path, O_RDWR | O_CREAT | O_NOFOLLOW | kCloseOnExecOpenFlag, mode);
  if (state->descriptor == -1) {
    const int error_number = errno;
    if (error_number == EACCES || error_number == EROFS) {
      std::string message{"shared memory must be writable: "};
      message += std::generic_category().message(error_number);
      return std::unexpected(ReadOnlyError(std::move(message)));
    }
    return std::unexpected(
        SystemError(ErrorCode::kCannotOpen, "open shared memory", state->path, error_number));
  }
  auto close_on_exec = SetCloseOnExec(state->descriptor, state->path);
  if (!close_on_exec.has_value()) {
    return std::unexpected(std::move(close_on_exec.error()));
  }

  auto deadman_lock = QueryAdvisoryLock(state->descriptor,
                                        AdvisoryLockRange{
                                            .type = F_WRLCK,
                                            .start = kSharedMemoryDeadmanSwitch,
                                            .length = 1,
                                        },
                                        state->path);
  if (!deadman_lock.has_value()) {
    return std::unexpected(std::move(deadman_lock.error()));
  }
  if (*deadman_lock == F_UNLCK) {
    auto locked = SetAdvisoryLock(state->descriptor,
                                  AdvisoryLockRange{
                                      .type = F_WRLCK,
                                      .start = kSharedMemoryDeadmanSwitch,
                                      .length = 1,
                                  },
                                  state->path, LockConflict::kBusy);
    if (!locked.has_value()) {
      return std::unexpected(std::move(locked.error()));
    }
    auto truncated = TruncateDescriptor(state->descriptor, 3, state->path);
    if (!truncated.has_value()) {
      return std::unexpected(std::move(truncated.error()));
    }
  } else if (*deadman_lock == F_WRLCK) {
    return std::unexpected(BusyError("shared-memory initialization", state->path));
  }

  auto shared_lock = SetAdvisoryLock(state->descriptor,
                                     AdvisoryLockRange{
                                         .type = F_RDLCK,
                                         .start = kSharedMemoryDeadmanSwitch,
                                         .length = 1,
                                     },
                                     state->path, LockConflict::kBusy);
  if (!shared_lock.has_value()) {
    return std::unexpected(std::move(shared_lock.error()));
  }
  return state;
}

[[nodiscard]] std::atomic<std::uint64_t>& NextConnectionId() {
  static std::atomic<std::uint64_t> next{1};
  return next;
}

class PosixFile final : public File {
 public:
  PosixFile(RegisteredDescriptor descriptor, std::string path, FileKind kind, FileAccessMode access,
            ScopedDescriptor directory_descriptor, std::string directory_path)
      : descriptor_(std::move(descriptor)),
        path_(std::move(path)),
        kind_(kind),
        access_(access),
        directory_descriptor_(std::move(directory_descriptor)),
        directory_path_(std::move(directory_path)),
        connection_id_(NextConnectionId().fetch_add(1, std::memory_order_relaxed)) {}

  // std::mutex::lock is specified as potentially throwing, but destruction
  // cannot surface cleanup failures.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~PosixFile() override {
    BestEffortUnmapSharedMemory();
    BestEffortUnlockDatabase();
  }

 protected:
  [[nodiscard]] Result<ByteCount> DoReadAt(MutableByteView destination, FileOffset offset) override;
  [[nodiscard]] Status DoWriteAt(ByteView source, FileOffset offset) override;
  [[nodiscard]] Status DoTruncate(FileSize size) override;
  [[nodiscard]] Status DoSync(SyncOptions options) override;
  [[nodiscard]] Result<FileSize> DoSize() override;

  [[nodiscard]] Status DoLock(DatabaseLock lock) override;
  [[nodiscard]] Status DoUnlock(DatabaseLock lock) override;
  [[nodiscard]] Result<bool> DoHasReservedLock() override;
  [[nodiscard]] FileProperties DoProperties() const noexcept override;

  [[nodiscard]] Result<std::optional<MutableByteView>> DoMapSharedMemory(
      SharedMemoryRegionIndex region, ByteCount region_size, SharedMemoryMapMode mode) override;
  [[nodiscard]] Status DoLockSharedMemory(SharedMemoryLockRange range,
                                          SharedMemoryLockOperation operation,
                                          SharedMemoryLockMode mode) override;
  void DoSharedMemoryBarrier() noexcept override;
  [[nodiscard]] Status DoUnmapSharedMemory(SharedMemoryUnmapMode mode) override;

 private:
  [[nodiscard]] Status CheckDatabaseLockFile() const;
  [[nodiscard]] Status DowngradeToSharedLocked(InodeState& state);
  [[nodiscard]] Result<std::shared_ptr<SharedMemoryState>> EnsureSharedMemory();
  [[nodiscard]] Status ReleaseSharedMemoryLocksLocked(SharedMemoryState& state);
  void BestEffortReleaseSharedMemoryLocksLocked(SharedMemoryState& state) noexcept;
  void BestEffortUnlockDatabase() noexcept;
  void BestEffortUnmapSharedMemory() noexcept;

  RegisteredDescriptor descriptor_;
  std::string path_;
  FileKind kind_;
  FileAccessMode access_;
  ScopedDescriptor directory_descriptor_;
  std::string directory_path_;
  std::uint64_t connection_id_;
  DatabaseLock lock_ = DatabaseLock::kNone;
  std::shared_ptr<SharedMemoryState> shared_memory_;
  std::uint16_t shared_memory_shared_mask_ = 0;
  std::uint16_t shared_memory_exclusive_mask_ = 0;
};

Result<ByteCount> PosixFile::DoReadAt(MutableByteView destination, FileOffset offset) {
  if (!IsPosixRangeRepresentable(offset, destination.size())) {
    return std::unexpected(TooLargeError("the read range exceeds POSIX off_t"));
  }

  std::size_t total = 0;
  while (total < destination.size()) {
    const std::size_t remaining = destination.size() - total;
    const std::size_t request = std::min(remaining, static_cast<std::size_t>(SSIZE_MAX));
    const auto current_offset =
        static_cast<off_t>(offset.value() + static_cast<std::uint64_t>(total));

    ssize_t bytes_read = -1;
    do {
      bytes_read = ::pread(descriptor_.get(), destination.data() + total, request, current_offset);
    } while (bytes_read == -1 && errno == EINTR);
    if (bytes_read == 0) {
      break;
    }
    if (bytes_read == -1) {
      return std::unexpected(SystemError(ErrorCode::kIo, "pread", path_, errno));
    }
    total += static_cast<std::size_t>(bytes_read);
  }
  return ByteCount{total};
}

Status PosixFile::DoWriteAt(ByteView source, FileOffset offset) {
  if (access_ == FileAccessMode::kReadOnly) {
    return std::unexpected(ReadOnlyError("cannot write through a read-only file"));
  }
  if (!IsPosixRangeRepresentable(offset, source.size())) {
    return std::unexpected(TooLargeError("the write range exceeds POSIX off_t"));
  }

  std::size_t total = 0;
  while (total < source.size()) {
    const std::size_t remaining = source.size() - total;
    const std::size_t request = std::min(remaining, static_cast<std::size_t>(SSIZE_MAX));
    const auto current_offset =
        static_cast<off_t>(offset.value() + static_cast<std::uint64_t>(total));

    ssize_t bytes_written = -1;
    do {
      bytes_written = ::pwrite(descriptor_.get(), source.data() + total, request, current_offset);
    } while (bytes_written == -1 && errno == EINTR);
    if (bytes_written == -1) {
      return std::unexpected(SystemError(ErrorCode::kIo, "pwrite", path_, errno, true));
    }
    if (bytes_written == 0) {
      return std::unexpected(IoError("pwrite made no progress"));
    }
    total += static_cast<std::size_t>(bytes_written);
  }
  return {};
}

Status PosixFile::DoTruncate(FileSize size) {
  if (access_ == FileAccessMode::kReadOnly) {
    return std::unexpected(ReadOnlyError("cannot truncate a read-only file"));
  }
  if (!IsPosixSizeRepresentable(size)) {
    return std::unexpected(TooLargeError("the truncate size exceeds POSIX off_t"));
  }
  return TruncateDescriptor(descriptor_.get(), static_cast<off_t>(size.value()), path_);
}

Status PosixFile::DoSync(SyncOptions options) {
  auto synchronized = SyncDescriptor(descriptor_.get(), options, path_);
  if (!synchronized.has_value()) {
    return synchronized;
  }
  if (!directory_descriptor_.valid()) {
    return {};
  }

  synchronized = SyncDirectoryDescriptor(directory_descriptor_.get(), directory_path_);
  if (!synchronized.has_value()) {
    return synchronized;
  }
  const int descriptor = directory_descriptor_.release();
  return CloseChecked(descriptor, directory_path_);
}

Result<FileSize> PosixFile::DoSize() {
  auto status = DescriptorStatus(descriptor_.get(), path_);
  if (!status.has_value()) {
    return std::unexpected(std::move(status.error()));
  }
  if (status->st_size < 0) {
    return std::unexpected(IoError("fstat returned a negative file size"));
  }
  return FileSize{static_cast<std::uint64_t>(status->st_size)};
}

Status PosixFile::CheckDatabaseLockFile() const {
  if (kind_ != FileKind::kMainDatabase) {
    return std::unexpected(MisuseError("database locks require a main database file"));
  }
  return {};
}

Status PosixFile::DoLock(DatabaseLock requested_lock) {
  auto valid_file = CheckDatabaseLockFile();
  if (!valid_file.has_value()) {
    return valid_file;
  }
  if (LockRank(lock_) >= LockRank(requested_lock)) {
    return {};
  }

  InodeState& state = *descriptor_.state();
  const std::scoped_lock guard{state.mutex};

  if (requested_lock == DatabaseLock::kShared) {
    if (lock_ != DatabaseLock::kNone) {
      return std::unexpected(MisuseError("a shared lock requires an unlocked file"));
    }
    if (state.process_lock == DatabaseLock::kPending ||
        state.process_lock == DatabaseLock::kExclusive) {
      return std::unexpected(BusyError("database shared lock", path_));
    }
    if (state.shared_count == 0) {
      auto pending = SetAdvisoryLock(descriptor_.get(),
                                     AdvisoryLockRange{
                                         .type = F_RDLCK,
                                         .start = kPendingByte,
                                         .length = 1,
                                     },
                                     path_, LockConflict::kBusy);
      if (!pending.has_value()) {
        return pending;
      }
      auto shared = SetAdvisoryLock(descriptor_.get(),
                                    AdvisoryLockRange{
                                        .type = F_RDLCK,
                                        .start = kSharedFirst,
                                        .length = kSharedSize,
                                    },
                                    path_, LockConflict::kBusy);
      auto released = SetAdvisoryLock(descriptor_.get(),
                                      AdvisoryLockRange{
                                          .type = F_UNLCK,
                                          .start = kPendingByte,
                                          .length = 1,
                                      },
                                      path_, LockConflict::kIo);
      if (!shared.has_value()) {
        return shared;
      }
      if (!released.has_value()) {
        [[maybe_unused]] const Status cleanup = SetAdvisoryLock(descriptor_.get(),
                                                                AdvisoryLockRange{
                                                                    .type = F_UNLCK,
                                                                    .start = kSharedFirst,
                                                                    .length = kSharedSize,
                                                                },
                                                                path_, LockConflict::kIo);
        return released;
      }
      state.process_lock = DatabaseLock::kShared;
    }
    ++state.shared_count;
    lock_ = DatabaseLock::kShared;
    return {};
  }

  if (lock_ != DatabaseLock::kShared && lock_ != DatabaseLock::kReserved &&
      lock_ != DatabaseLock::kPending) {
    return std::unexpected(
        MisuseError("reserved and exclusive locks require an existing shared lock"));
  }
  if (access_ == FileAccessMode::kReadOnly) {
    return std::unexpected(ReadOnlyError("a read-only file cannot obtain a write lock"));
  }
  if (state.elevated_owner.has_value() && *state.elevated_owner != connection_id_) {
    return std::unexpected(BusyError("database write lock", path_));
  }

  if (requested_lock == DatabaseLock::kReserved) {
    if (lock_ != DatabaseLock::kShared) {
      return std::unexpected(MisuseError("a reserved lock requires a shared lock"));
    }
    auto reserved = SetAdvisoryLock(descriptor_.get(),
                                    AdvisoryLockRange{
                                        .type = F_WRLCK,
                                        .start = kReservedByte,
                                        .length = 1,
                                    },
                                    path_, LockConflict::kBusy);
    if (!reserved.has_value()) {
      return reserved;
    }
    lock_ = DatabaseLock::kReserved;
    state.process_lock = DatabaseLock::kReserved;
    state.elevated_owner = connection_id_;
    return {};
  }

  if (requested_lock != DatabaseLock::kExclusive) {
    return std::unexpected(MisuseError("the database lock transition is invalid"));
  }

  if (lock_ == DatabaseLock::kReserved) {
    auto pending = SetAdvisoryLock(descriptor_.get(),
                                   AdvisoryLockRange{
                                       .type = F_WRLCK,
                                       .start = kPendingByte,
                                       .length = 1,
                                   },
                                   path_, LockConflict::kBusy);
    if (!pending.has_value()) {
      return pending;
    }
    lock_ = DatabaseLock::kPending;
    state.process_lock = DatabaseLock::kPending;
    state.elevated_owner = connection_id_;
  }

  if (state.shared_count > 1) {
    return std::unexpected(BusyError("database exclusive lock", path_));
  }

  auto exclusive = SetAdvisoryLock(descriptor_.get(),
                                   AdvisoryLockRange{
                                       .type = F_WRLCK,
                                       .start = kSharedFirst,
                                       .length = kSharedSize,
                                   },
                                   path_, LockConflict::kBusy);
  if (!exclusive.has_value()) {
    return exclusive;
  }
  lock_ = DatabaseLock::kExclusive;
  state.process_lock = DatabaseLock::kExclusive;
  state.elevated_owner = connection_id_;
  return {};
}

Status PosixFile::DowngradeToSharedLocked(InodeState& state) {
  auto shared = SetAdvisoryLock(descriptor_.get(),
                                AdvisoryLockRange{
                                    .type = F_RDLCK,
                                    .start = kSharedFirst,
                                    .length = kSharedSize,
                                },
                                path_, LockConflict::kIo);
  if (!shared.has_value()) {
    return shared;
  }
  auto released = SetAdvisoryLock(descriptor_.get(),
                                  AdvisoryLockRange{
                                      .type = F_UNLCK,
                                      .start = kPendingByte,
                                      .length = 2,
                                  },
                                  path_, LockConflict::kIo);
  if (!released.has_value()) {
    return released;
  }
  lock_ = DatabaseLock::kShared;
  state.process_lock = DatabaseLock::kShared;
  state.elevated_owner.reset();
  return {};
}

Status PosixFile::DoUnlock(DatabaseLock target_lock) {
  auto valid_file = CheckDatabaseLockFile();
  if (!valid_file.has_value()) {
    return valid_file;
  }
  if (LockRank(lock_) <= LockRank(target_lock)) {
    return {};
  }

  InodeState& state = *descriptor_.state();
  const std::scoped_lock guard{state.mutex};
  if (lock_ > DatabaseLock::kShared) {
    auto downgraded = DowngradeToSharedLocked(state);
    if (!downgraded.has_value()) {
      return downgraded;
    }
  }
  if (target_lock == DatabaseLock::kShared) {
    return {};
  }

  if (state.shared_count == 0) {
    return std::unexpected(IoError("database lock accounting underflow"));
  }
  if (state.shared_count > 1) {
    --state.shared_count;
    lock_ = DatabaseLock::kNone;
    return {};
  }

  auto unlocked = SetAdvisoryLock(descriptor_.get(),
                                  AdvisoryLockRange{
                                      .type = F_UNLCK,
                                      .start = 0,
                                      .length = 0,
                                  },
                                  path_, LockConflict::kIo);
  if (!unlocked.has_value()) {
    return unlocked;
  }
  state.shared_count = 0;
  state.process_lock = DatabaseLock::kNone;
  state.elevated_owner.reset();
  CloseDeferredDescriptorsLocked(state);
  lock_ = DatabaseLock::kNone;
  return {};
}

// std::mutex::lock is specified as potentially throwing, but destruction
// cannot surface cleanup failures.
// NOLINTNEXTLINE(bugprone-exception-escape)
void PosixFile::BestEffortUnlockDatabase() noexcept {
  if (lock_ == DatabaseLock::kNone) {
    return;
  }

  InodeState& state = *descriptor_.state();
  const std::scoped_lock guard{state.mutex};
  if (LockRank(lock_) > LockRank(DatabaseLock::kShared)) {
    BestEffortSetAdvisoryLock(descriptor_.get(), AdvisoryLockRange{
                                                     .type = F_RDLCK,
                                                     .start = kSharedFirst,
                                                     .length = kSharedSize,
                                                 });
    BestEffortSetAdvisoryLock(descriptor_.get(), AdvisoryLockRange{
                                                     .type = F_UNLCK,
                                                     .start = kPendingByte,
                                                     .length = 2,
                                                 });
    state.process_lock = DatabaseLock::kShared;
    state.elevated_owner.reset();
  }

  if (state.shared_count > 0) {
    --state.shared_count;
  }
  if (state.shared_count == 0) {
    BestEffortSetAdvisoryLock(descriptor_.get(), AdvisoryLockRange{
                                                     .type = F_UNLCK,
                                                     .start = 0,
                                                     .length = 0,
                                                 });
    state.process_lock = DatabaseLock::kNone;
    state.elevated_owner.reset();
    CloseDeferredDescriptorsLocked(state);
  }
  lock_ = DatabaseLock::kNone;
}

Result<bool> PosixFile::DoHasReservedLock() {
  auto valid_file = CheckDatabaseLockFile();
  if (!valid_file.has_value()) {
    return std::unexpected(std::move(valid_file.error()));
  }

  InodeState& state = *descriptor_.state();
  const std::scoped_lock guard{state.mutex};
  if (LockRank(state.process_lock) > LockRank(DatabaseLock::kShared)) {
    return true;
  }
  auto lock = QueryAdvisoryLock(descriptor_.get(),
                                AdvisoryLockRange{
                                    .type = F_WRLCK,
                                    .start = kReservedByte,
                                    .length = 1,
                                },
                                path_);
  if (!lock.has_value()) {
    return std::unexpected(std::move(lock.error()));
  }
  if (*lock != F_UNLCK) {
    return true;
  }
  lock = QueryAdvisoryLock(descriptor_.get(),
                           AdvisoryLockRange{
                               .type = F_RDLCK,
                               .start = kPendingByte,
                               .length = 1,
                           },
                           path_);
  if (!lock.has_value()) {
    return std::unexpected(std::move(lock.error()));
  }
  if (*lock != F_UNLCK) {
    return true;
  }
  lock = QueryAdvisoryLock(descriptor_.get(),
                           AdvisoryLockRange{
                               .type = F_RDLCK,
                               .start = kSharedFirst,
                               .length = kSharedSize,
                           },
                           path_);
  if (!lock.has_value()) {
    return std::unexpected(std::move(lock.error()));
  }
  return *lock != F_UNLCK;
}

FileProperties PosixFile::DoProperties() const noexcept {
  return FileProperties{
      .sector_size = ByteCount{4096},
      .device_characteristics = DeviceCharacteristics{}
                                    .With(DeviceCapability::kPowersafeOverwrite)
                                    .With(DeviceCapability::kSubpageRead),
  };
}

Result<std::shared_ptr<SharedMemoryState>> PosixFile::EnsureSharedMemory() {
  if (kind_ != FileKind::kMainDatabase) {
    return std::unexpected(MisuseError("shared memory requires a main database file"));
  }
  if (shared_memory_ != nullptr) {
    return shared_memory_;
  }

  InodeState& inode = *descriptor_.state();
  const std::scoped_lock inode_guard{inode.mutex};
  auto state = inode.shared_memory.lock();
  if (state == nullptr) {
    if (inode.database_path.empty()) {
      return std::unexpected(IoError("the database has no stable path for shared memory"));
    }
    auto created = CreateSharedMemoryState(descriptor_.get(), inode.database_path);
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    state = std::move(*created);
    inode.shared_memory = state;
  }

  {
    const std::scoped_lock state_guard{state->mutex};
    ++state->connections;
  }
  shared_memory_ = state;
  return state;
}

Result<std::optional<MutableByteView>> PosixFile::DoMapSharedMemory(SharedMemoryRegionIndex region,
                                                                    ByteCount region_size,
                                                                    SharedMemoryMapMode mode) {
  auto shared_memory = EnsureSharedMemory();
  if (!shared_memory.has_value()) {
    return std::unexpected(std::move(shared_memory.error()));
  }
  SharedMemoryState& state = **shared_memory;
  const std::scoped_lock guard{state.mutex};

  if (state.region_size == 0) {
    state.region_size = region_size.value();
  } else if (state.region_size != region_size.value()) {
    return std::unexpected(MisuseError("shared-memory region size must remain constant"));
  }

  const auto region_number = static_cast<std::uint64_t>(region.value());
  const auto size = static_cast<std::uint64_t>(region_size.value());
  const auto offset = region_number * size;
  const auto end = offset + size;
  if (end > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return std::unexpected(TooLargeError("the shared-memory range exceeds POSIX off_t"));
  }

  auto status = DescriptorStatus(state.descriptor, state.path);
  if (!status.has_value()) {
    return std::unexpected(std::move(status.error()));
  }
  if (status->st_size < 0) {
    return std::unexpected(IoError("fstat returned a negative shared-memory size"));
  }
  const auto current_size = static_cast<std::uint64_t>(status->st_size);
  if (current_size < end) {
    if (mode == SharedMemoryMapMode::kExistingOnly) {
      return std::optional<MutableByteView>{};
    }
    auto extended = ExtendSharedMemoryFile(state, current_size, end);
    if (!extended.has_value()) {
      return std::unexpected(std::move(extended.error()));
    }
  }

  const auto index = static_cast<std::size_t>(region.value());
  if (state.mappings.size() <= index) {
    state.mappings.resize(index + 1U);
  }
  if (state.mappings[index] == nullptr) {
    const long page_size_result = ::sysconf(_SC_PAGESIZE);
    if (page_size_result <= 0) {
      return std::unexpected(IoError("sysconf(_SC_PAGESIZE) failed"));
    }
    const auto page_size = static_cast<std::uint64_t>(page_size_result);
    const auto aligned_offset = offset - (offset % page_size);
    const auto prefix = static_cast<std::size_t>(offset - aligned_offset);
    if (region_size.value() > std::numeric_limits<std::size_t>::max() - prefix) {
      return std::unexpected(TooLargeError("the shared-memory mapping size is too large"));
    }
    const std::size_t mapping_size = prefix + region_size.value();
    void* const mapping = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                                 state.descriptor, static_cast<off_t>(aligned_offset));
    if (mapping == MAP_FAILED) {
      return std::unexpected(SystemError(ErrorCode::kIo, "mmap", state.path, errno));
    }
    auto* const data = static_cast<std::byte*>(mapping) + prefix;
    MappedRegion mapped_region{mapping, mapping_size, data};
    state.mappings[index] = std::make_unique<MappedRegion>(std::move(mapped_region));
  }
  return std::optional<MutableByteView>{
      MutableByteView{state.mappings[index]->data(), region_size.value()}};
}

Status PosixFile::DoLockSharedMemory(SharedMemoryLockRange range,
                                     SharedMemoryLockOperation operation,
                                     SharedMemoryLockMode mode) {
  if (shared_memory_ == nullptr) {
    return std::unexpected(MisuseError("shared-memory locks require an attached mapping"));
  }
  SharedMemoryState& state = *shared_memory_;
  const std::scoped_lock guard{state.mutex};
  const std::uint16_t mask = SharedMemoryMask(range);

  if (operation == SharedMemoryLockOperation::kUnlock) {
    if (mode == SharedMemoryLockMode::kShared) {
      if (!HasAllBits(shared_memory_shared_mask_, mask)) {
        return std::unexpected(MisuseError("the shared-memory shared lock is not held"));
      }
      const std::size_t slot = range.offset;
      if (state.locks[slot] > 1) {
        --state.locks[slot];
      } else {
        auto unlocked =
            SetAdvisoryLock(state.descriptor,
                            AdvisoryLockRange{
                                .type = F_UNLCK,
                                .start = kSharedMemoryLockBase + static_cast<off_t>(range.offset),
                                .length = static_cast<off_t>(range.count),
                            },
                            state.path, LockConflict::kIo);
        if (!unlocked.has_value()) {
          return unlocked;
        }
        state.locks[slot] = 0;
      }
      shared_memory_shared_mask_ = ClearBits(shared_memory_shared_mask_, mask);
      return {};
    }

    if (!HasAllBits(shared_memory_exclusive_mask_, mask)) {
      return std::unexpected(MisuseError("the shared-memory exclusive lock is not held"));
    }
    auto unlocked =
        SetAdvisoryLock(state.descriptor,
                        AdvisoryLockRange{
                            .type = F_UNLCK,
                            .start = kSharedMemoryLockBase + static_cast<off_t>(range.offset),
                            .length = static_cast<off_t>(range.count),
                        },
                        state.path, LockConflict::kIo);
    if (!unlocked.has_value()) {
      return unlocked;
    }
    for (std::uint8_t slot = range.offset;
         slot < static_cast<std::uint8_t>(range.offset + range.count); ++slot) {
      state.locks[slot] = 0;
    }
    shared_memory_exclusive_mask_ = ClearBits(shared_memory_exclusive_mask_, mask);
    return {};
  }

  if (mode == SharedMemoryLockMode::kShared) {
    if (HasAnyBits(shared_memory_shared_mask_, mask)) {
      return {};
    }
    if (HasAnyBits(shared_memory_exclusive_mask_, mask)) {
      return std::unexpected(MisuseError("shared-memory locks cannot change mode in place"));
    }
    const std::size_t slot = range.offset;
    if (state.locks[slot] < 0) {
      return std::unexpected(BusyError("shared-memory shared lock", state.path));
    }
    if (state.locks[slot] == 0) {
      auto locked =
          SetAdvisoryLock(state.descriptor,
                          AdvisoryLockRange{
                              .type = F_RDLCK,
                              .start = kSharedMemoryLockBase + static_cast<off_t>(range.offset),
                              .length = 1,
                          },
                          state.path, LockConflict::kBusy);
      if (!locked.has_value()) {
        return locked;
      }
    }
    ++state.locks[slot];
    shared_memory_shared_mask_ = AddBits(shared_memory_shared_mask_, mask);
    return {};
  }

  if (HasAnyBits(AddBits(shared_memory_shared_mask_, shared_memory_exclusive_mask_), mask)) {
    return std::unexpected(MisuseError("shared-memory locks cannot change mode in place"));
  }
  for (std::uint8_t slot = range.offset;
       slot < static_cast<std::uint8_t>(range.offset + range.count); ++slot) {
    if (state.locks[slot] != 0) {
      return std::unexpected(BusyError("shared-memory exclusive lock", state.path));
    }
  }
  auto locked =
      SetAdvisoryLock(state.descriptor,
                      AdvisoryLockRange{
                          .type = F_WRLCK,
                          .start = kSharedMemoryLockBase + static_cast<off_t>(range.offset),
                          .length = static_cast<off_t>(range.count),
                      },
                      state.path, LockConflict::kBusy);
  if (!locked.has_value()) {
    return locked;
  }
  for (std::uint8_t slot = range.offset;
       slot < static_cast<std::uint8_t>(range.offset + range.count); ++slot) {
    state.locks[slot] = -1;
  }
  shared_memory_exclusive_mask_ = AddBits(shared_memory_exclusive_mask_, mask);
  return {};
}

void PosixFile::DoSharedMemoryBarrier() noexcept {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

Status PosixFile::ReleaseSharedMemoryLocksLocked(SharedMemoryState& state) {
  for (std::uint8_t slot = 0; slot < kSharedMemoryLockSlotCount; ++slot) {
    const std::uint16_t bit = SharedMemoryBit(slot);
    if (HasAnyBits(shared_memory_shared_mask_, bit)) {
      if (state.locks[slot] > 1) {
        --state.locks[slot];
        shared_memory_shared_mask_ = ClearBits(shared_memory_shared_mask_, bit);
      } else {
        auto unlocked =
            SetAdvisoryLock(state.descriptor,
                            AdvisoryLockRange{
                                .type = F_UNLCK,
                                .start = kSharedMemoryLockBase + static_cast<off_t>(slot),
                                .length = 1,
                            },
                            state.path, LockConflict::kIo);
        if (!unlocked.has_value()) {
          return unlocked;
        }
        state.locks[slot] = 0;
        shared_memory_shared_mask_ = ClearBits(shared_memory_shared_mask_, bit);
      }
    }
  }

  std::uint8_t slot = 0;
  while (slot < kSharedMemoryLockSlotCount) {
    const std::uint16_t bit = SharedMemoryBit(slot);
    if (!HasAnyBits(shared_memory_exclusive_mask_, bit)) {
      ++slot;
      continue;
    }
    const std::uint8_t first_slot = slot;
    while (slot < kSharedMemoryLockSlotCount) {
      const std::uint16_t current_bit = SharedMemoryBit(slot);
      if (!HasAnyBits(shared_memory_exclusive_mask_, current_bit)) {
        break;
      }
      ++slot;
    }
    const auto count = static_cast<std::uint8_t>(slot - first_slot);
    auto unlocked =
        SetAdvisoryLock(state.descriptor,
                        AdvisoryLockRange{
                            .type = F_UNLCK,
                            .start = kSharedMemoryLockBase + static_cast<off_t>(first_slot),
                            .length = static_cast<off_t>(count),
                        },
                        state.path, LockConflict::kIo);
    if (!unlocked.has_value()) {
      return unlocked;
    }
    for (std::uint8_t current = first_slot; current < slot; ++current) {
      const std::uint16_t current_bit = SharedMemoryBit(current);
      state.locks[current] = 0;
      shared_memory_exclusive_mask_ = ClearBits(shared_memory_exclusive_mask_, current_bit);
    }
  }
  return {};
}

void PosixFile::BestEffortReleaseSharedMemoryLocksLocked(SharedMemoryState& state) noexcept {
  for (std::uint8_t slot = 0; slot < kSharedMemoryLockSlotCount; ++slot) {
    const std::uint16_t bit = SharedMemoryBit(slot);
    if (!HasAnyBits(shared_memory_shared_mask_, bit)) {
      continue;
    }
    if (state.locks[slot] > 1) {
      --state.locks[slot];
    } else {
      BestEffortSetAdvisoryLock(state.descriptor,
                                AdvisoryLockRange{
                                    .type = F_UNLCK,
                                    .start = kSharedMemoryLockBase + static_cast<off_t>(slot),
                                    .length = 1,
                                });
      state.locks[slot] = 0;
    }
    shared_memory_shared_mask_ = ClearBits(shared_memory_shared_mask_, bit);
  }

  std::uint8_t slot = 0;
  while (slot < kSharedMemoryLockSlotCount) {
    const std::uint16_t bit = SharedMemoryBit(slot);
    if (!HasAnyBits(shared_memory_exclusive_mask_, bit)) {
      ++slot;
      continue;
    }
    const std::uint8_t first_slot = slot;
    while (slot < kSharedMemoryLockSlotCount &&
           HasAnyBits(shared_memory_exclusive_mask_, SharedMemoryBit(slot))) {
      ++slot;
    }
    const auto count = static_cast<std::uint8_t>(slot - first_slot);
    BestEffortSetAdvisoryLock(state.descriptor,
                              AdvisoryLockRange{
                                  .type = F_UNLCK,
                                  .start = kSharedMemoryLockBase + static_cast<off_t>(first_slot),
                                  .length = static_cast<off_t>(count),
                              });
    for (std::uint8_t current = first_slot; current < slot; ++current) {
      state.locks[current] = 0;
      shared_memory_exclusive_mask_ =
          ClearBits(shared_memory_exclusive_mask_, SharedMemoryBit(current));
    }
  }
}

Status PosixFile::DoUnmapSharedMemory(SharedMemoryUnmapMode mode) {
  if (shared_memory_ == nullptr) {
    return {};
  }

  auto state_holder = shared_memory_;
  Status result;
  {
    InodeState& inode = *descriptor_.state();
    const std::scoped_lock inode_guard{inode.mutex};
    SharedMemoryState& state = *state_holder;
    const std::scoped_lock state_guard{state.mutex};
    if (mode == SharedMemoryUnmapMode::kDelete) {
      state.delete_requested = true;
    }
    auto released = ReleaseSharedMemoryLocksLocked(state);
    if (!released.has_value()) {
      return released;
    }
    if (state.connections == 0) {
      return std::unexpected(IoError("shared-memory connection accounting underflow"));
    }
    --state.connections;
    if (state.connections == 0) {
      if (state.delete_requested && ::unlink(state.path.c_str()) != 0 && errno != ENOENT) {
        result =
            std::unexpected(SystemError(ErrorCode::kIo, "unlink shared memory", state.path, errno));
      }
      inode.shared_memory.reset();
    }
  }
  shared_memory_.reset();
  state_holder.reset();
  return result;
}

// std::mutex::lock is specified as potentially throwing, but destruction
// cannot surface cleanup failures.
// NOLINTNEXTLINE(bugprone-exception-escape)
void PosixFile::BestEffortUnmapSharedMemory() noexcept {
  if (shared_memory_ == nullptr) {
    return;
  }

  auto state_holder = shared_memory_;
  {
    InodeState& inode = *descriptor_.state();
    const std::scoped_lock inode_guard{inode.mutex};
    SharedMemoryState& state = *state_holder;
    const std::scoped_lock state_guard{state.mutex};
    BestEffortReleaseSharedMemoryLocksLocked(state);
    if (state.connections > 0) {
      --state.connections;
    }
    if (state.connections == 0) {
      if (state.delete_requested) {
        static_cast<void>(::unlink(state.path.c_str()));
      }
      inode.shared_memory.reset();
    }
  }
  shared_memory_.reset();
  state_holder.reset();
}

[[nodiscard]] Result<OpenedFile> OpenNamedFile(std::string_view path, FileOpenOptions options) {
  auto mode = CreationMode(path, options);
  if (!mode.has_value()) {
    return std::unexpected(std::move(mode.error()));
  }

  // POSIX open flags are a signed C bitmask by definition.
  // NOLINTBEGIN(bugprone-signed-bitwise)
  int flags =
      (options.access == FileAccessMode::kReadWrite ? O_RDWR : O_RDONLY) | kCloseOnExecOpenFlag;
  if (options.create) {
    flags |= O_CREAT;
  }
  if (options.exclusive_create) {
    flags |= O_EXCL | O_NOFOLLOW;
  } else if (options.no_follow) {
    flags |= O_NOFOLLOW;
  }

  int descriptor = OpenFile(path, flags, *mode);
  FileAccessMode actual_access = options.access;
  if (descriptor == -1 && options.access == FileAccessMode::kReadWrite &&
      options.allow_read_only_fallback && errno != EISDIR) {
    flags &= ~(O_RDWR | O_CREAT);
    flags |= O_RDONLY;
    descriptor = OpenFile(path, flags, *mode);
    actual_access = FileAccessMode::kReadOnly;
  }
  // NOLINTEND(bugprone-signed-bitwise)
  if (descriptor == -1) {
    return std::unexpected(SystemError(ErrorCode::kCannotOpen, "open", path, errno));
  }

  ScopedDescriptor opened_descriptor{descriptor};
  auto close_on_exec = SetCloseOnExec(descriptor, path);
  if (!close_on_exec.has_value()) {
    return std::unexpected(std::move(close_on_exec.error()));
  }
  auto status = DescriptorStatus(descriptor, path);
  if (!status.has_value()) {
    return std::unexpected(std::move(status.error()));
  }
  auto inode = RegisterInode(*status);
  auto deferred_descriptor = std::make_unique<DeferredDescriptor>();
  RegisteredDescriptor registered{std::move(inode), opened_descriptor.release(),
                                  std::move(deferred_descriptor)};
  if (S_ISDIR(status->st_mode)) {
    return std::unexpected(
        Error::Create(ErrorCode::kCannotOpen, "directories cannot be opened as files"));
  }

  auto canonical = CanonicalExistingPath(path);
  if (!canonical.has_value()) {
    return std::unexpected(std::move(canonical.error()));
  }
  if (options.kind == FileKind::kMainDatabase) {
    const std::scoped_lock lock{registered.state()->mutex};
    if (registered.state()->database_path.empty()) {
      registered.state()->database_path = *canonical;
    }
  }

  ScopedDescriptor directory_descriptor;
  std::string directory_path;
  if (options.create && IsDirectorySyncKind(options.kind)) {
    directory_path = ParentPath(*canonical);
    const int parent_descriptor = OpenDirectory(directory_path);
    if (parent_descriptor == -1) {
      return std::unexpected(
          SystemError(ErrorCode::kCannotOpen, "open parent directory", directory_path, errno));
    }
    directory_descriptor = ScopedDescriptor{parent_descriptor};
    close_on_exec = SetCloseOnExec(parent_descriptor, directory_path);
    if (!close_on_exec.has_value()) {
      return std::unexpected(std::move(close_on_exec.error()));
    }
  }

  if (options.delete_on_close) {
    const std::string owned_path{path};
    int result = -1;
    do {
      result = ::unlink(owned_path.c_str());
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
      return std::unexpected(
          SystemError(ErrorCode::kIo, "unlink delete-on-close file", path, errno));
    }
  }

  auto file = std::make_unique<PosixFile>(
      std::move(registered), std::move(*canonical), options.kind, actual_access,
      std::move(directory_descriptor), std::move(directory_path));
  return OpenedFile{
      .file = std::move(file),
      .access = actual_access,
  };
}

[[nodiscard]] Result<OpenedFile> OpenTemporaryFile(FileOpenOptions options) {
  std::error_code path_error;
  const std::filesystem::path temporary_directory =
      std::filesystem::temp_directory_path(path_error);
  if (path_error) {
    return std::unexpected(
        SystemError(ErrorCode::kCannotOpen, "find temporary directory", {}, path_error.value()));
  }

  std::string pattern = (temporary_directory / "modern-sqlite-XXXXXX").string();
  std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
  mutable_pattern.push_back('\0');
  int descriptor = -1;
  do {
    descriptor = ::mkstemp(mutable_pattern.data());
  } while (descriptor == -1 && errno == EINTR);
  if (descriptor == -1) {
    return std::unexpected(SystemError(ErrorCode::kCannotOpen, "mkstemp", pattern, errno));
  }

  const std::string created_path{mutable_pattern.data()};
  ScopedDescriptor opened_descriptor{descriptor};
  auto close_on_exec = SetCloseOnExec(descriptor, created_path);
  if (!close_on_exec.has_value()) {
    return std::unexpected(std::move(close_on_exec.error()));
  }
  auto status = DescriptorStatus(descriptor, created_path);
  if (!status.has_value()) {
    return std::unexpected(std::move(status.error()));
  }
  auto inode = RegisterInode(*status);
  auto deferred_descriptor = std::make_unique<DeferredDescriptor>();
  RegisteredDescriptor registered{std::move(inode), opened_descriptor.release(),
                                  std::move(deferred_descriptor)};
  auto canonical = CanonicalExistingPath(created_path);
  if (!canonical.has_value()) {
    return std::unexpected(std::move(canonical.error()));
  }

  int result = -1;
  do {
    result = ::unlink(created_path.c_str());
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    return std::unexpected(
        SystemError(ErrorCode::kIo, "unlink temporary file", created_path, errno));
  }

  auto file =
      std::make_unique<PosixFile>(std::move(registered), std::move(*canonical), options.kind,
                                  FileAccessMode::kReadWrite, ScopedDescriptor{}, std::string{});
  return OpenedFile{
      .file = std::move(file),
      .access = FileAccessMode::kReadWrite,
  };
}

}  // namespace

Result<OpenedFile> PosixVfs::DoOpen(std::optional<std::string_view> path, FileOpenOptions options) {
  if (path.has_value()) {
    return OpenNamedFile(*path, options);
  }
  return OpenTemporaryFile(options);
}

Status PosixVfs::DoDelete(std::string_view path, DirectorySync directory_sync) {
  const std::string owned_path{path};
  int result = -1;
  do {
    result = ::unlink(owned_path.c_str());
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    const int error_number = errno;
    const ErrorCode fallback = error_number == ENOENT ? ErrorCode::kNotFound : ErrorCode::kIo;
    return std::unexpected(SystemError(fallback, "unlink", path, error_number));
  }
  if (directory_sync == DirectorySync::kNo) {
    return {};
  }

  const std::string parent = ParentPath(path);
  const int descriptor = OpenDirectory(parent);
  if (descriptor == -1) {
    return std::unexpected(SystemError(ErrorCode::kIo, "open parent directory", parent, errno));
  }
  ScopedDescriptor directory{descriptor};
  auto close_on_exec = SetCloseOnExec(descriptor, parent);
  if (!close_on_exec.has_value()) {
    return close_on_exec;
  }
  auto synchronized = SyncDirectoryDescriptor(descriptor, parent);
  if (!synchronized.has_value()) {
    return synchronized;
  }
  return CloseChecked(directory.release(), parent);
}

Result<bool> PosixVfs::DoAccess(std::string_view path, FileAccessQuery query) {
  int mode = F_OK;
  switch (query) {
    case FileAccessQuery::kExists:
      mode = F_OK;
      break;
    case FileAccessQuery::kRead:
      mode = R_OK;
      break;
    case FileAccessQuery::kReadWrite:
      mode = R_OK | W_OK;
      break;
  }

  const std::string owned_path{path};
  int result = -1;
  do {
    result = ::access(owned_path.c_str(), mode);
  } while (result == -1 && errno == EINTR);
  if (result == 0) {
    return true;
  }
  const int error_number = errno;
  if (error_number == ENOENT || error_number == ENOTDIR || error_number == EACCES ||
      error_number == EROFS) {
    return false;
  }
  return std::unexpected(SystemError(ErrorCode::kIo, "access", path, error_number));
}

Result<std::string> PosixVfs::DoFullPath(std::string_view path) {
  std::error_code error;
  const std::filesystem::path absolute =
      std::filesystem::absolute(std::filesystem::path{path}, error);
  if (error) {
    return std::unexpected(
        SystemError(ErrorCode::kCannotOpen, "absolute path", path, error.value()));
  }
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
  if (error) {
    return std::unexpected(
        SystemError(ErrorCode::kCannotOpen, "weakly canonical path", path, error.value()));
  }
  return canonical.lexically_normal().string();
}

Result<ByteCount> PosixVfs::DoRandomBytes(MutableByteView output) {
  std::size_t offset = 0;
  while (offset < output.size()) {
    const std::size_t count = std::min(kRandomnessChunkSize, output.size() - offset);
    int result = -1;
    do {
      result = ::getentropy(output.data() + offset, count);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
      return std::unexpected(SystemError(ErrorCode::kIo, "getentropy", {}, errno));
    }
    offset += count;
  }
  return ByteCount{offset};
}

Result<std::chrono::microseconds> PosixVfs::DoSleepFor(std::chrono::microseconds duration) {
  using SteadyClock = std::chrono::steady_clock;
  const auto maximum =
      std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::duration::max());
  if (duration > maximum) {
    return std::unexpected(TooLargeError("the sleep duration is too large"));
  }

  const auto started = SteadyClock::now();
  const auto deadline = started + std::chrono::duration_cast<SteadyClock::duration>(duration);
  while (true) {
    const auto now = SteadyClock::now();
    if (now >= deadline) {
      return std::chrono::duration_cast<std::chrono::microseconds>(now - started);
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
    struct timespec request{};
    request.tv_sec = static_cast<time_t>(seconds.count());
    request.tv_nsec = static_cast<long>((remaining - seconds).count());
    if (::nanosleep(&request, nullptr) == 0 || errno == EINTR) {
      continue;
    }
    return std::unexpected(SystemError(ErrorCode::kIo, "nanosleep", {}, errno));
  }
}

Result<WallClockTime> PosixVfs::DoCurrentTime() {
  return std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

}  // namespace modern_sqlite
