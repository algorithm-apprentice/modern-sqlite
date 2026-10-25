#include "modern_sqlite/platform/windows_vfs.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modern_sqlite {
namespace {

constexpr DWORD kPendingByte = 0x40000000;
constexpr DWORD kReservedByte = kPendingByte + 1;
constexpr DWORD kSharedFirst = kPendingByte + 2;
constexpr DWORD kSharedSize = 510;
constexpr DWORD kSharedMemoryLockBase = 120;
constexpr DWORD kSharedMemoryDeadmanSwitch = 128;
constexpr unsigned kIoRetryCount = 10;
constexpr DWORD kIoRetryDelay = 25;
constexpr std::size_t kMaximumWidePath = 32'767;
constexpr std::size_t kTemporarySuffixLength = 15;

[[nodiscard]] Error Failure(ErrorCode code, std::string_view message) noexcept {
  try {
    return Error::Create(code, std::string{message});
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  }
}

[[nodiscard]] Error OsError(std::string_view operation, DWORD error,
                            ErrorCode fallback = ErrorCode::kIo) noexcept {
  ErrorCode code = fallback;
  switch (error) {
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
      return Error::OutOfMemory();
    case ERROR_ACCESS_DENIED:
      code = ErrorCode::kPermissionDenied;
      break;
    case ERROR_WRITE_PROTECT:
      code = ErrorCode::kReadOnly;
      break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_QUOTA_EXCEEDED:
      code = ErrorCode::kFull;
      break;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_FILE_TOO_LARGE:
    case ERROR_ARITHMETIC_OVERFLOW:
      code = ErrorCode::kTooLarge;
      break;
    default:
      break;
  }
  try {
    return Error::Create(
        code, std::string{operation} + " failed with Windows error " + std::to_string(error));
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  }
}

[[nodiscard]] bool IsMissing(DWORD error) noexcept {
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] bool RetryIo(DWORD error, unsigned& retries) noexcept {
  if (retries >= kIoRetryCount) {
    return false;
  }
  if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
      error != ERROR_LOCK_VIOLATION && error != ERROR_DEV_NOT_EXIST &&
      error != ERROR_NETNAME_DELETED && error != ERROR_SEM_TIMEOUT &&
      error != ERROR_NETWORK_UNREACHABLE) {
    return false;
  }
  ++retries;
  ::Sleep(kIoRetryDelay * retries);
  return true;
}

class Handle final {
 public:
  Handle() noexcept = default;
  explicit Handle(HANDLE value) noexcept : value_(value) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.release()) {}
  Handle& operator=(Handle&& other) noexcept {
    const Handle previous{std::move(*this)};
    value_ = other.release();
    return *this;
  }
  ~Handle() { [[maybe_unused]] const Status cleanup = Close(); }

  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept {
    return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
  }
  [[nodiscard]] HANDLE release() noexcept { return std::exchange(value_, INVALID_HANDLE_VALUE); }
  [[nodiscard]] Status Close() noexcept {
    if (!valid()) {
      return {};
    }
    DWORD error = ERROR_SUCCESS;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
      if (::CloseHandle(value_) != 0) {
        value_ = INVALID_HANDLE_VALUE;
        return {};
      }
      error = ::GetLastError();
      if (attempt != 2) {
        ::Sleep(100);
      }
    }
    return std::unexpected(OsError("CloseHandle", error));
  }

 private:
  HANDLE value_ = INVALID_HANDLE_VALUE;
};

[[nodiscard]] Result<std::wstring> ToWide(std::string_view input) {
  if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected(Failure(ErrorCode::kTooLarge, "UTF-8 path exceeds the Windows limit"));
  }
  const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0);
  if (count == 0) {
    return std::unexpected(
        OsError("MultiByteToWideChar", ::GetLastError(), ErrorCode::kCannotOpen));
  }
  if (std::cmp_greater(count, kMaximumWidePath)) {
    return std::unexpected(Failure(ErrorCode::kTooLarge, "UTF-16 path exceeds the Windows limit"));
  }
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), result.data(), count) != count) {
    return std::unexpected(
        OsError("MultiByteToWideChar", ::GetLastError(), ErrorCode::kCannotOpen));
  }
  return result;
}

[[nodiscard]] Result<std::string> ToUtf8(std::wstring_view input) {
  if (input.size() > kMaximumWidePath) {
    return std::unexpected(Failure(ErrorCode::kTooLarge, "UTF-16 path exceeds the Windows limit"));
  }
  const int count =
      ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
  if (count == 0) {
    return std::unexpected(
        OsError("WideCharToMultiByte", ::GetLastError(), ErrorCode::kCannotOpen));
  }
  std::string result(static_cast<std::size_t>(count), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), result.data(), count, nullptr,
                            nullptr) != count) {
    return std::unexpected(
        OsError("WideCharToMultiByte", ::GetLastError(), ErrorCode::kCannotOpen));
  }
  return result;
}

[[nodiscard]] Result<std::wstring> ResolveFullPath(std::wstring_view input) {
  const std::wstring terminated{input};
  DWORD count = ::GetFullPathNameW(terminated.c_str(), 0, nullptr, nullptr);
  if (count == 0) {
    return std::unexpected(OsError("GetFullPathNameW", ::GetLastError(), ErrorCode::kCannotOpen));
  }
  while (count <= kMaximumWidePath + 1U) {
    std::wstring result(count, L'\0');
    const DWORD written = ::GetFullPathNameW(terminated.c_str(), count, result.data(), nullptr);
    if (written == 0) {
      return std::unexpected(OsError("GetFullPathNameW", ::GetLastError(), ErrorCode::kCannotOpen));
    }
    if (written < count) {
      result.resize(written);
      return result;
    }
    count = written;
  }
  return std::unexpected(Failure(ErrorCode::kTooLarge, "absolute path exceeds the Windows limit"));
}

[[nodiscard]] Result<std::wstring> ResolveFullPath(std::string_view input) {
  auto wide = ToWide(input);
  if (!wide.has_value()) {
    return std::unexpected(std::move(wide.error()));
  }
  return ResolveFullPath(*wide);
}

[[nodiscard]] Result<std::wstring> ApiPath(std::wstring_view path) {
  if (path.size() > kMaximumWidePath) {
    return std::unexpected(Failure(ErrorCode::kTooLarge, "path exceeds the Windows limit"));
  }
  if (path.size() < MAX_PATH || path.starts_with(L"\\\\?\\")) {
    return std::wstring{path};
  }
  std::wstring result = path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + std::wstring{path.substr(2)}
                                                  : L"\\\\?\\" + std::wstring{path};
  if (result.size() > kMaximumWidePath) {
    return std::unexpected(
        Failure(ErrorCode::kTooLarge, "extended path exceeds the Windows limit"));
  }
  return result;
}

[[nodiscard]] OVERLAPPED AtOffset(std::uint64_t offset) noexcept {
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffU);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
  return overlapped;
}

[[nodiscard]] Status CheckRange(std::uint64_t offset, std::size_t count) noexcept {
  constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (offset > maximum || count > maximum - offset) {
    return std::unexpected(
        Failure(ErrorCode::kTooLarge, "file range exceeds signed Windows offsets"));
  }
  return {};
}

[[nodiscard]] Result<FileSize> FileSizeOf(HANDLE handle) noexcept {
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) == 0) {
    return std::unexpected(OsError("GetFileSizeEx", ::GetLastError()));
  }
  if (size.QuadPart < 0) {
    return std::unexpected(Failure(ErrorCode::kIo, "Windows returned a negative file size"));
  }
  return FileSize{static_cast<std::uint64_t>(size.QuadPart)};
}

[[nodiscard]] Status TruncateHandle(HANDLE handle, FileSize size) noexcept {
  if (auto checked = CheckRange(size.value(), 0); !checked.has_value()) {
    return checked;
  }
  LARGE_INTEGER offset{};
  offset.QuadPart = static_cast<LONGLONG>(size.value());
  if (::SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN) == 0) {
    return std::unexpected(OsError("SetFilePointerEx", ::GetLastError()));
  }
  if (::SetEndOfFile(handle) == 0) {
    return std::unexpected(OsError("SetEndOfFile", ::GetLastError()));
  }
  return {};
}

[[nodiscard]] Result<bool> TryLock(HANDLE handle, FileOffset offset, ByteCount count,
                                   bool exclusive) noexcept {
  auto position = AtOffset(offset.value());
  const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0U);
  if (::LockFileEx(handle, flags, 0, static_cast<DWORD>(count.value() & 0xffffffffU),
                   static_cast<DWORD>(static_cast<std::uint64_t>(count.value()) >> 32U),
                   &position) != 0) {
    return true;
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION) {
    return false;
  }
  return std::unexpected(OsError("LockFileEx", error));
}

[[nodiscard]] Status UnlockRange(HANDLE handle, FileOffset offset, ByteCount count) noexcept {
  auto position = AtOffset(offset.value());
  if (::UnlockFileEx(handle, 0, static_cast<DWORD>(count.value() & 0xffffffffU),
                     static_cast<DWORD>(static_cast<std::uint64_t>(count.value()) >> 32U),
                     &position) == 0) {
    return std::unexpected(OsError("UnlockFileEx", ::GetLastError()));
  }
  return {};
}

[[nodiscard]] Status RequireLock(Result<bool> result) noexcept {
  if (!result.has_value()) {
    return std::unexpected(std::move(result.error()));
  }
  if (!*result) {
    return std::unexpected(
        Failure(ErrorCode::kBusy, "Windows file lock is held by another handle"));
  }
  return {};
}

[[nodiscard]] Result<Handle> OpenHandle(std::wstring_view path, FileOpenOptions options) {
  auto native = ApiPath(path);
  if (!native.has_value()) {
    return std::unexpected(std::move(native.error()));
  }
  const DWORD access =
      GENERIC_READ | (options.access == FileAccessMode::kReadWrite ? GENERIC_WRITE : 0U);
  const DWORD disposition = options.exclusive_create ? CREATE_NEW
                            : options.create         ? OPEN_ALWAYS
                                                     : OPEN_EXISTING;
  DWORD attributes = options.delete_on_close ? FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN |
                                                   FILE_FLAG_DELETE_ON_CLOSE
                                             : FILE_ATTRIBUTE_NORMAL;
  if (options.no_follow) {
    attributes |= FILE_FLAG_OPEN_REPARSE_POINT;
  }
  unsigned retries = 0;
  while (true) {
    Handle opened{::CreateFileW(native->c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, disposition, attributes, nullptr)};
    if (opened.valid()) {
      FILE_ATTRIBUTE_TAG_INFO information{};
      if (::GetFileInformationByHandleEx(opened.get(), FileAttributeTagInfo, &information,
                                         sizeof(information)) == 0) {
        return std::unexpected(
            OsError("GetFileInformationByHandleEx", ::GetLastError(), ErrorCode::kCannotOpen));
      }
      if ((information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
          (options.no_follow &&
           (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)) {
        return std::unexpected(
            Failure(ErrorCode::kCannotOpen, "Windows open rejected a directory or reparse point"));
      }
      return opened;
    }
    const DWORD error = ::GetLastError();
    if (options.access == FileAccessMode::kReadWrite &&
        ::GetFileAttributesW(native->c_str()) != INVALID_FILE_ATTRIBUTES) {
      return std::unexpected(OsError("CreateFileW", error, ErrorCode::kCannotOpen));
    }
    if (!RetryIo(error, retries)) {
      return std::unexpected(OsError("CreateFileW", error, ErrorCode::kCannotOpen));
    }
  }
}

[[nodiscard]] Status DeletePath(std::wstring_view path, bool missing_ok = false) {
  auto native = ApiPath(path);
  if (!native.has_value()) {
    return std::unexpected(std::move(native.error()));
  }
  unsigned retries = 0;
  while (true) {
    const DWORD attributes = ::GetFileAttributesW(native->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = ::GetLastError();
      if (IsMissing(error)) {
        return missing_ok ? Status{}
                          : std::unexpected(OsError("DeleteFileW", error, ErrorCode::kNotFound));
      }
      return std::unexpected(OsError("GetFileAttributesW", error));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
      return std::unexpected(Failure(ErrorCode::kIo, "Windows file deletion rejected a directory"));
    }
    if (::DeleteFileW(native->c_str()) != 0) {
      return {};
    }
    const DWORD error = ::GetLastError();
    if (!RetryIo(error, retries)) {
      return std::unexpected(OsError("DeleteFileW", error));
    }
  }
}

[[nodiscard]] Status FillRandomBytes(MutableByteView output) noexcept {
  std::size_t done = 0;
  while (done < output.size()) {
    const auto count = static_cast<ULONG>(std::min(
        output.size() - done, static_cast<std::size_t>(std::numeric_limits<ULONG>::max())));
    const NTSTATUS result =
        ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(output.data() + done), count,
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (result < 0) {
      return std::unexpected(OsError("BCryptGenRandom", static_cast<DWORD>(result)));
    }
    done += count;
  }
  return {};
}

[[nodiscard]] Result<std::wstring> TemporaryPath() {
  DWORD count = ::GetTempPathW(0, nullptr);
  if (count == 0) {
    return std::unexpected(OsError("GetTempPathW", ::GetLastError(), ErrorCode::kCannotOpen));
  }
  while (count <= kMaximumWidePath) {
    std::wstring directory(count, L'\0');
    const DWORD written = ::GetTempPathW(count, directory.data());
    if (written == 0) {
      return std::unexpected(OsError("GetTempPathW", ::GetLastError(), ErrorCode::kCannotOpen));
    }
    if (written < count) {
      directory.resize(written);
      constexpr std::wstring_view characters =
          L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
      ByteBuffer random{ByteCount{kTemporarySuffixLength}};
      if (auto generated = FillRandomBytes(random.mutable_view()); !generated.has_value()) {
        return std::unexpected(std::move(generated.error()));
      }
      directory += L"modern-sqlite-";
      for (const std::byte value : random.view()) {
        directory += characters[std::to_integer<std::size_t>(value) % characters.size()];
      }
      return ResolveFullPath(directory);
    }
    count = written;
  }
  return std::unexpected(Failure(ErrorCode::kTooLarge, "Windows temporary path is too long"));
}

[[nodiscard]] bool UsesSharedLockHandle(std::wstring_view path) noexcept {
  if (path.starts_with(L"\\\\?\\") && path.size() >= 7U && path[5] == L':') {
    path.remove_prefix(4);
  } else if (path.starts_with(L"\\\\")) {
    return true;
  }
  if (path.size() >= 3U && path[1] == L':') {
    const std::array root{path[0], L':', L'\\', L'\0'};
    return ::GetDriveTypeW(root.data()) == DRIVE_REMOTE;
  }
  return false;
}

class Mapping final {
 public:
  Mapping() noexcept = default;
  Mapping(Handle handle, MutableByteView bytes, ByteOffset shift, ByteCount size) noexcept
      : handle_(std::move(handle)), bytes_(bytes), shift_(shift.value()), size_(size.value()) {}
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  Mapping(Mapping&& other) noexcept
      : handle_(std::move(other.handle_)),
        bytes_(std::exchange(other.bytes_, {})),
        shift_(other.shift_),
        size_(other.size_) {}
  Mapping& operator=(Mapping&&) = delete;
  ~Mapping() { [[maybe_unused]] const Status cleanup = Close(); }

  [[nodiscard]] MutableByteView view() const noexcept { return bytes_.subspan(shift_, size_); }

  [[nodiscard]] Status Close() noexcept {
    if (!bytes_.empty()) {
      if (::UnmapViewOfFile(bytes_.data()) == 0) {
        return std::unexpected(OsError("UnmapViewOfFile", ::GetLastError()));
      }
      bytes_ = {};
    }
    return handle_.Close();
  }

 private:
  Handle handle_;
  MutableByteView bytes_{};
  std::size_t shift_ = 0;
  std::size_t size_ = 0;
};

[[nodiscard]] std::uint16_t LockMask(SharedMemoryLockRange range) noexcept {
  return static_cast<std::uint16_t>(((1U << range.count) - 1U) << range.offset);
}

struct SharedMemoryConnection {
  Handle lock_handle;
  std::uint16_t shared_mask = 0;
  std::uint16_t exclusive_mask = 0;
  // Windows unlocks must match the exact ranges originally acquired.
  std::array<std::optional<SharedMemoryLockRange>, kSharedMemoryLockSlotCount> exclusive_ranges{};
};

struct SharedMemoryState {
  std::mutex mutex;
  std::wstring path;
  Handle mapping_file;
  bool shared_lock_handle = false;
  bool initialized = false;
  bool retiring = false;
  bool delete_requested = false;
  std::size_t region_size = 0;
  std::vector<Mapping> mappings;
  // Attachments borrow their immovable File owner's connection until detachment.
  std::vector<SharedMemoryConnection*> connections;
};

struct SharedMemoryRegistry {
  std::mutex mutex;
  std::vector<std::weak_ptr<SharedMemoryState>> states;
};

[[nodiscard]] SharedMemoryRegistry& Registry() {
  static SharedMemoryRegistry registry;
  return registry;
}

[[nodiscard]] bool SamePath(std::wstring_view left, std::wstring_view right) noexcept {
  return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] Status InitializeSharedMemory(SharedMemoryState& state) {
  if (state.initialized) {
    return {};
  }
  const auto exclusive =
      TryLock(state.mapping_file.get(), FileOffset{kSharedMemoryDeadmanSwitch}, ByteCount{1}, true);
  if (!exclusive.has_value()) {
    return std::unexpected(exclusive.error());
  }
  if (*exclusive) {
    auto cleared = TruncateHandle(state.mapping_file.get(), FileSize{0});
    auto released =
        UnlockRange(state.mapping_file.get(), FileOffset{kSharedMemoryDeadmanSwitch}, ByteCount{1});
    if (!released.has_value()) {
      return released;
    }
    if (!cleared.has_value()) {
      return cleared;
    }
  }
  if (auto locked = RequireLock(TryLock(
          state.mapping_file.get(), FileOffset{kSharedMemoryDeadmanSwitch}, ByteCount{1}, false));
      !locked.has_value()) {
    return locked;
  }
  state.initialized = true;
  return {};
}

class WindowsFile final : public File {
 public:
  WindowsFile(Handle handle, std::wstring path, FileKind kind, FileAccessMode access) noexcept
      : handle_(std::move(handle)), path_(std::move(path)), kind_(kind), access_(access) {}
  // Mutex acquisition can throw, but destruction must retire every borrowed attachment.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~WindowsFile() override {
    if (auto cleaned = DoUnmapSharedMemory(SharedMemoryUnmapMode::kKeep);
        !cleaned.has_value() && shared_memory_ != nullptr && connection_.has_value()) {
      SharedMemoryRegistry& registry = Registry();
      const std::scoped_lock registry_guard{registry.mutex};
      const std::scoped_lock state_guard{shared_memory_->mutex};
      [[maybe_unused]] const Status cleanup =
          ReleaseSharedMemoryLocks(*shared_memory_, *connection_);
      [[maybe_unused]] const bool detached =
          DetachConnection(registry, *shared_memory_, *connection_);
    }
  }

 protected:
  [[nodiscard]] Result<ByteCount> DoReadAt(MutableByteView destination,
                                           FileOffset offset) override {
    if (auto checked = CheckRange(offset.value(), destination.size()); !checked.has_value()) {
      return std::unexpected(std::move(checked.error()));
    }
    std::size_t done = 0;
    unsigned retries = 0;
    while (done < destination.size()) {
      const auto count = static_cast<DWORD>(std::min(
          destination.size() - done, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
      auto position = AtOffset(offset.value() + done);
      DWORD read = 0;
      if (::ReadFile(handle_.get(), destination.data() + done, count, &read, &position) == 0) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_HANDLE_EOF) {
          break;
        }
        if (RetryIo(error, retries)) {
          continue;
        }
        return std::unexpected(OsError("ReadFile", error));
      }
      done += read;
      if (read < count) {
        break;
      }
    }
    return ByteCount{done};
  }

  [[nodiscard]] Status DoWriteAt(ByteView source, FileOffset offset) override {
    if (auto writable = RequireWritable(); !writable.has_value()) {
      return writable;
    }
    if (auto checked = CheckRange(offset.value(), source.size()); !checked.has_value()) {
      return checked;
    }
    std::size_t done = 0;
    unsigned retries = 0;
    while (done < source.size()) {
      const auto count = static_cast<DWORD>(std::min(
          source.size() - done, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
      auto position = AtOffset(offset.value() + done);
      DWORD written = 0;
      if (::WriteFile(handle_.get(), source.data() + done, count, &written, &position) == 0) {
        const DWORD error = ::GetLastError();
        if (RetryIo(error, retries)) {
          continue;
        }
        return std::unexpected(OsError("WriteFile", error));
      }
      if (written == 0 || written > count) {
        return std::unexpected(Failure(ErrorCode::kIo, "Windows did not complete a file write"));
      }
      done += written;
    }
    return {};
  }

  [[nodiscard]] Status DoTruncate(FileSize size) override {
    if (auto writable = RequireWritable(); !writable.has_value()) {
      return writable;
    }
    return TruncateHandle(handle_.get(), size);
  }

  [[nodiscard]] Status DoSync(SyncOptions) override {
    if (::FlushFileBuffers(handle_.get()) == 0) {
      return std::unexpected(OsError("FlushFileBuffers", ::GetLastError()));
    }
    return {};
  }

  [[nodiscard]] Result<FileSize> DoSize() override { return FileSizeOf(handle_.get()); }

  [[nodiscard]] Status DoLock(DatabaseLock requested) override {
    if (kind_ != FileKind::kMainDatabase) {
      return std::unexpected(Failure(ErrorCode::kMisuse, "database locks require a main database"));
    }
    if (lock_ >= requested) {
      return {};
    }
    if (requested != DatabaseLock::kShared) {
      if (auto writable = RequireWritable(); !writable.has_value()) {
        return writable;
      }
      if (lock_ == DatabaseLock::kNone ||
          (requested == DatabaseLock::kReserved && lock_ != DatabaseLock::kShared)) {
        return std::unexpected(
            Failure(ErrorCode::kMisuse, "invalid Windows database lock sequence"));
      }
    }
    if (requested == DatabaseLock::kShared ||
        (requested == DatabaseLock::kExclusive && lock_ == DatabaseLock::kReserved)) {
      const bool exclusive = requested == DatabaseLock::kExclusive;
      Result<bool> pending = false;
      for (unsigned attempt = 0; attempt < 3; ++attempt) {
        pending = TryLock(handle_.get(), FileOffset{kPendingByte}, ByteCount{1}, exclusive);
        if (!pending.has_value() || *pending || attempt == 2) {
          break;
        }
        ::Sleep(1);
      }
      if (auto required = RequireLock(std::move(pending)); !required.has_value()) {
        return required;
      }
      pending_ = true;
      if (exclusive) {
        lock_ = DatabaseLock::kPending;
      }
    }
    if (requested == DatabaseLock::kShared) {
      auto acquired =
          TryLock(handle_.get(), FileOffset{kSharedFirst}, ByteCount{kSharedSize}, false);
      if (acquired.has_value() && *acquired) {
        shared_ = true;
        lock_ = DatabaseLock::kShared;
      }
      auto released = UnlockRange(handle_.get(), FileOffset{kPendingByte}, ByteCount{1});
      if (!released.has_value()) {
        return released;
      }
      pending_ = false;
      return RequireLock(std::move(acquired));
    }
    if (requested == DatabaseLock::kReserved) {
      if (auto acquired =
              RequireLock(TryLock(handle_.get(), FileOffset{kReservedByte}, ByteCount{1}, true));
          !acquired.has_value()) {
        return acquired;
      }
      reserved_ = true;
      lock_ = DatabaseLock::kReserved;
      return {};
    }
    if (shared_) {
      if (auto released =
              UnlockRange(handle_.get(), FileOffset{kSharedFirst}, ByteCount{kSharedSize});
          !released.has_value()) {
        return released;
      }
      shared_ = false;
    }
    lock_ = DatabaseLock::kPending;
    auto acquired = TryLock(handle_.get(), FileOffset{kSharedFirst}, ByteCount{kSharedSize}, true);
    if (acquired.has_value() && *acquired) {
      exclusive_ = true;
      lock_ = DatabaseLock::kExclusive;
      return {};
    }
    auto restored = RequireLock(
        TryLock(handle_.get(), FileOffset{kSharedFirst}, ByteCount{kSharedSize}, false));
    if (!restored.has_value()) {
      return restored;
    }
    shared_ = true;
    return RequireLock(std::move(acquired));
  }

  [[nodiscard]] Status DoUnlock(DatabaseLock requested) override {
    if (kind_ != FileKind::kMainDatabase) {
      return std::unexpected(Failure(ErrorCode::kMisuse, "database locks require a main database"));
    }
    if (lock_ <= requested && !pending_) {
      return {};
    }
    if (exclusive_) {
      if (auto released =
              UnlockRange(handle_.get(), FileOffset{kSharedFirst}, ByteCount{kSharedSize});
          !released.has_value()) {
        return released;
      }
      exclusive_ = false;
    }
    if (requested == DatabaseLock::kShared && !shared_) {
      if (auto acquired = RequireLock(
              TryLock(handle_.get(), FileOffset{kSharedFirst}, ByteCount{kSharedSize}, false));
          !acquired.has_value()) {
        return acquired;
      }
      shared_ = true;
    }
    if (reserved_) {
      if (auto released = UnlockRange(handle_.get(), FileOffset{kReservedByte}, ByteCount{1});
          !released.has_value()) {
        return released;
      }
      reserved_ = false;
    }
    if (requested == DatabaseLock::kNone && shared_) {
      if (auto released =
              UnlockRange(handle_.get(), FileOffset{kSharedFirst}, ByteCount{kSharedSize});
          !released.has_value()) {
        return released;
      }
      shared_ = false;
    }
    if (pending_) {
      if (auto released = UnlockRange(handle_.get(), FileOffset{kPendingByte}, ByteCount{1});
          !released.has_value()) {
        return released;
      }
      pending_ = false;
    }
    lock_ = requested;
    return {};
  }

  [[nodiscard]] Result<bool> DoHasReservedLock() override {
    if (kind_ != FileKind::kMainDatabase) {
      return std::unexpected(Failure(ErrorCode::kMisuse, "database locks require a main database"));
    }
    if (lock_ >= DatabaseLock::kReserved) {
      return true;
    }
    const auto acquired = TryLock(handle_.get(), FileOffset{kReservedByte}, ByteCount{1}, false);
    if (!acquired.has_value()) {
      return std::unexpected(acquired.error());
    }
    if (!*acquired) {
      return true;
    }
    if (auto released = UnlockRange(handle_.get(), FileOffset{kReservedByte}, ByteCount{1});
        !released.has_value()) {
      return std::unexpected(std::move(released.error()));
    }
    return false;
  }

  [[nodiscard]] FileProperties DoProperties() const noexcept override {
    auto properties = DeviceCharacteristics{}
                          .With(DeviceCapability::kUndeletableWhenOpen)
                          .With(DeviceCapability::kSubpageRead);
    if (kind_ == FileKind::kMainDatabase) {
      properties = properties.With(DeviceCapability::kPowersafeOverwrite);
    }
    return FileProperties{.sector_size = ByteCount{4096}, .device_characteristics = properties};
  }

  [[nodiscard]] Result<std::optional<MutableByteView>> DoMapSharedMemory(
      SharedMemoryRegionIndex region, ByteCount region_size, SharedMemoryMapMode mode) override {
    try {
      if (auto attached = AttachSharedMemory(); !attached.has_value()) {
        return std::unexpected(std::move(attached.error()));
      }
      SharedMemoryState& state = *shared_memory_;
      const std::scoped_lock guard{state.mutex};
      if (state.retiring) {
        return std::unexpected(Failure(ErrorCode::kIo, "shared-memory cleanup requires retry"));
      }
      if (state.region_size != 0 && state.region_size != region_size.value()) {
        return std::unexpected(Failure(ErrorCode::kMisuse, "shared-memory region size changed"));
      }
      if (auto initialized = InitializeSharedMemory(state); !initialized.has_value()) {
        return std::unexpected(std::move(initialized.error()));
      }
      if (region.value() < state.mappings.size()) {
        return state.mappings[region.value()].view();
      }
      const auto end = (static_cast<std::uint64_t>(region.value()) + 1U) * region_size.value();
      if (auto checked = CheckRange(end, 0); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
      auto size = FileSizeOf(state.mapping_file.get());
      if (!size.has_value()) {
        return std::unexpected(std::move(size.error()));
      }
      if (size->value() < end) {
        if (mode == SharedMemoryMapMode::kExistingOnly) {
          return std::optional<MutableByteView>{};
        }
        if (auto extended = TruncateHandle(state.mapping_file.get(), FileSize{end});
            !extended.has_value()) {
          return std::unexpected(std::move(extended.error()));
        }
      }
      state.region_size = region_size.value();
      state.mappings.reserve(static_cast<std::size_t>(region.value()) + 1U);
      SYSTEM_INFO system{};
      ::GetSystemInfo(&system);
      if (system.dwAllocationGranularity == 0) {
        return std::unexpected(
            Failure(ErrorCode::kInternal, "Windows mapping granularity is zero"));
      }
      while (state.mappings.size() <= region.value()) {
        const std::uint64_t offset = state.mappings.size() * region_size.value();
        const auto shift = static_cast<std::size_t>(offset % system.dwAllocationGranularity);
        if (region_size.value() > std::numeric_limits<std::size_t>::max() - shift) {
          return std::unexpected(Failure(ErrorCode::kTooLarge, "Windows mapping size overflow"));
        }
        const std::size_t mapped_size = region_size.value() + shift;
        Handle mapping{::CreateFileMappingW(state.mapping_file.get(), nullptr, PAGE_READWRITE,
                                            static_cast<DWORD>(end >> 32U),
                                            static_cast<DWORD>(end & 0xffffffffU), nullptr)};
        if (!mapping.valid()) {
          return std::unexpected(OsError("CreateFileMappingW", ::GetLastError()));
        }
        const std::uint64_t aligned = offset - shift;
        void* const address = ::MapViewOfFile(
            mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, static_cast<DWORD>(aligned >> 32U),
            static_cast<DWORD>(aligned & 0xffffffffU), mapped_size);
        if (address == nullptr) {
          return std::unexpected(OsError("MapViewOfFile", ::GetLastError()));
        }
        state.mappings.emplace_back(std::move(mapping),
                                    MutableByteView{static_cast<std::byte*>(address), mapped_size},
                                    ByteOffset{shift}, region_size);
      }
      return state.mappings[region.value()].view();
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Status DoLockSharedMemory(SharedMemoryLockRange range,
                                          SharedMemoryLockOperation operation,
                                          SharedMemoryLockMode mode) override {
    if (shared_memory_ == nullptr || !connection_.has_value()) {
      return std::unexpected(Failure(ErrorCode::kMisuse, "shared memory has not been attached"));
    }
    SharedMemoryState& state = *shared_memory_;
    const std::scoped_lock guard{state.mutex};
    if (state.retiring) {
      return std::unexpected(Failure(ErrorCode::kIo, "shared-memory cleanup requires retry"));
    }
    const std::uint16_t mask = LockMask(range);
    SharedMemoryConnection& connection = *connection_;
    if (operation == SharedMemoryLockOperation::kUnlock) {
      return ReleaseSharedMemoryRange(state, connection, range, mode);
    }
    const bool exclusive = mode == SharedMemoryLockMode::kExclusive;
    const std::uint16_t held = exclusive ? connection.exclusive_mask : connection.shared_mask;
    if ((held & mask) == mask) {
      return {};
    }
    if (((connection.exclusive_mask | connection.shared_mask) & mask) != 0) {
      return std::unexpected(Failure(ErrorCode::kMisuse, "overlapping shared-memory lock request"));
    }
    bool lock_os = true;
    if (state.shared_lock_handle) {
      for (const SharedMemoryConnection* peer : state.connections) {
        if (((exclusive ? peer->shared_mask | peer->exclusive_mask : peer->exclusive_mask) &
             mask) != 0) {
          return std::unexpected(Failure(ErrorCode::kBusy, "shared-memory lock is held locally"));
        }
        if (!exclusive && (peer->shared_mask & mask) != 0) {
          lock_os = false;
        }
      }
    }
    const HANDLE handle =
        state.shared_lock_handle ? state.mapping_file.get() : connection.lock_handle.get();
    if (lock_os) {
      if (auto acquired =
              RequireLock(TryLock(handle, FileOffset{kSharedMemoryLockBase + range.offset},
                                  ByteCount{range.count}, exclusive));
          !acquired.has_value()) {
        return acquired;
      }
    }
    if (exclusive) {
      const auto slot = std::ranges::find_if(connection.exclusive_ranges,
                                             [](const auto& entry) { return !entry.has_value(); });
      if (slot == connection.exclusive_ranges.end()) {
        return std::unexpected(Failure(ErrorCode::kInternal, "shared-memory range owner is full"));
      }
      *slot = range;
      connection.exclusive_mask |= mask;
    } else {
      connection.shared_mask |= mask;
    }
    return {};
  }

  void DoSharedMemoryBarrier() noexcept override {
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  [[nodiscard]] Status DoUnmapSharedMemory(SharedMemoryUnmapMode mode) override {
    if (shared_memory_ == nullptr) {
      return {};
    }
    if (!connection_.has_value()) {
      return std::unexpected(Failure(ErrorCode::kInternal, "shared-memory connection is missing"));
    }
    try {
      SharedMemoryRegistry& registry = Registry();
      const std::scoped_lock registry_guard{registry.mutex};
      auto retained = shared_memory_;
      SharedMemoryState& state = *retained;
      const std::scoped_lock state_guard{state.mutex};
      if (mode == SharedMemoryUnmapMode::kDelete) {
        state.delete_requested = true;
      }
      if (state.connections.size() == 1U) {
        state.retiring = true;
      }
      if (auto released = ReleaseSharedMemoryLocks(state, *connection_); !released.has_value()) {
        return released;
      }
      if (state.connections.size() == 1U) {
        for (Mapping& mapping : state.mappings) {
          if (auto closed = mapping.Close(); !closed.has_value()) {
            return closed;
          }
        }
        if (auto closed = state.mapping_file.Close(); !closed.has_value()) {
          return closed;
        }
        if (state.delete_requested) {
          if (auto deleted = DeletePath(state.path, true); !deleted.has_value()) {
            return deleted;
          }
        }
      }
      if (!DetachConnection(registry, state, *connection_)) {
        return std::unexpected(
            Failure(ErrorCode::kInternal, "shared-memory attachment is missing"));
      }
      connection_.reset();
      shared_memory_.reset();
      return {};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

 private:
  [[nodiscard]] Status RequireWritable() const noexcept {
    if (access_ == FileAccessMode::kReadOnly) {
      return std::unexpected(Failure(ErrorCode::kReadOnly, "Windows file was opened read-only"));
    }
    return {};
  }

  [[nodiscard]] Status AttachSharedMemory() {
    if (kind_ != FileKind::kMainDatabase) {
      return std::unexpected(Failure(ErrorCode::kMisuse, "shared memory requires a main database"));
    }
    if (auto writable = RequireWritable(); !writable.has_value()) {
      return writable;
    }
    if (shared_memory_ != nullptr) {
      return {};
    }
    SharedMemoryRegistry& registry = Registry();
    const std::scoped_lock registry_guard{registry.mutex};
    std::erase_if(registry.states, [](const auto& state) { return state.expired(); });
    const std::wstring path = path_ + L"-shm";
    std::shared_ptr<SharedMemoryState> state;
    for (const auto& entry : registry.states) {
      auto candidate = entry.lock();
      if (candidate != nullptr && SamePath(candidate->path, path)) {
        state = std::move(candidate);
        break;
      }
    }
    const bool created = state == nullptr;
    if (created) {
      state = std::make_shared<SharedMemoryState>();
      state->path = path;
      state->shared_lock_handle = UsesSharedLockHandle(path_);
      auto opened = OpenHandle(
          path,
          FileOpenOptions{.access = FileAccessMode::kReadWrite, .create = true, .no_follow = true});
      if (!opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
      state->mapping_file = std::move(*opened);
      registry.states.reserve(registry.states.size() + 1U);
    }
    const std::scoped_lock state_guard{state->mutex};
    if (state->retiring) {
      return std::unexpected(Failure(ErrorCode::kIo, "shared-memory cleanup requires retry"));
    }
    Handle lock_handle;
    if (!state->shared_lock_handle) {
      auto opened = OpenHandle(
          path,
          FileOpenOptions{.access = FileAccessMode::kReadWrite, .create = true, .no_follow = true});
      if (!opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
      lock_handle = std::move(*opened);
    }
    state->connections.reserve(state->connections.size() + 1U);
    connection_.emplace();
    connection_->lock_handle = std::move(lock_handle);
    state->connections.push_back(&*connection_);
    if (created) {
      registry.states.push_back(state);
    }
    shared_memory_ = std::move(state);
    return {};
  }

  [[nodiscard]] static bool DetachConnection(SharedMemoryRegistry& registry,
                                             SharedMemoryState& state,
                                             SharedMemoryConnection& connection) {
    const bool detached = std::erase(state.connections, &connection) == 1U;
    if (state.connections.empty()) {
      std::erase_if(registry.states, [&](const auto& entry) {
        const auto owner = entry.lock();
        return owner == nullptr || owner.get() == &state;
      });
    }
    return detached;
  }

  [[nodiscard]] static Status ReleaseSharedMemoryRange(SharedMemoryState& state,
                                                       SharedMemoryConnection& connection,
                                                       SharedMemoryLockRange range,
                                                       SharedMemoryLockMode mode) {
    const std::uint16_t mask = LockMask(range);
    const bool exclusive = mode == SharedMemoryLockMode::kExclusive;
    const std::uint16_t held = exclusive ? connection.exclusive_mask : connection.shared_mask;
    if (((connection.exclusive_mask | connection.shared_mask) & mask) == 0) {
      return {};
    }
    if ((held & mask) != mask) {
      return std::unexpected(Failure(ErrorCode::kMisuse, "shared-memory lock mode does not match"));
    }
    const HANDLE handle =
        state.shared_lock_handle ? state.mapping_file.get() : connection.lock_handle.get();
    if (exclusive) {
      for (const auto& entry : connection.exclusive_ranges) {
        if (entry.has_value() && (LockMask(*entry) & mask) != 0 &&
            (LockMask(*entry) & mask) != LockMask(*entry)) {
          return std::unexpected(
              Failure(ErrorCode::kMisuse, "Windows unlock splits an acquired lock range"));
        }
      }
      for (auto& entry : connection.exclusive_ranges) {
        if (!entry.has_value() || (LockMask(*entry) & mask) == 0) {
          continue;
        }
        if (auto released = UnlockRange(handle, FileOffset{kSharedMemoryLockBase + entry->offset},
                                        ByteCount{entry->count});
            !released.has_value()) {
          return released;
        }
        connection.exclusive_mask &= static_cast<std::uint16_t>(~LockMask(*entry));
        entry.reset();
      }
      return {};
    }
    bool unlock_os = true;
    if (state.shared_lock_handle) {
      unlock_os = std::ranges::none_of(state.connections, [&](const SharedMemoryConnection* peer) {
        return peer != &connection && (peer->shared_mask & mask) != 0;
      });
    }
    if (unlock_os) {
      if (auto released = UnlockRange(handle, FileOffset{kSharedMemoryLockBase + range.offset},
                                      ByteCount{range.count});
          !released.has_value()) {
        return released;
      }
    }
    connection.shared_mask &= static_cast<std::uint16_t>(~mask);
    return {};
  }

  [[nodiscard]] static Status ReleaseSharedMemoryLocks(SharedMemoryState& state,
                                                       SharedMemoryConnection& connection) {
    if (!state.shared_lock_handle) {
      auto closed = connection.lock_handle.Close();
      if (closed.has_value()) {
        connection.shared_mask = 0;
        connection.exclusive_mask = 0;
        connection.exclusive_ranges.fill(std::nullopt);
      }
      return closed;
    }
    for (std::uint8_t slot = 0; slot < kSharedMemoryLockSlotCount; ++slot) {
      const SharedMemoryLockRange range{.offset = slot, .count = 1};
      if ((connection.shared_mask & LockMask(range)) != 0) {
        if (auto released =
                ReleaseSharedMemoryRange(state, connection, range, SharedMemoryLockMode::kShared);
            !released.has_value()) {
          return released;
        }
      }
    }
    for (const auto& entry : connection.exclusive_ranges) {
      if (entry.has_value()) {
        if (auto released = ReleaseSharedMemoryRange(state, connection, *entry,
                                                     SharedMemoryLockMode::kExclusive);
            !released.has_value()) {
          return released;
        }
      }
    }
    return {};
  }

  Handle handle_;
  std::wstring path_;
  FileKind kind_;
  FileAccessMode access_;
  DatabaseLock lock_ = DatabaseLock::kNone;
  bool shared_ = false;
  bool reserved_ = false;
  bool pending_ = false;
  bool exclusive_ = false;
  std::shared_ptr<SharedMemoryState> shared_memory_;
  std::optional<SharedMemoryConnection> connection_;
};

}  // namespace

Result<OpenedFile> WindowsVfs::DoOpen(std::optional<std::string_view> path,
                                      FileOpenOptions options) {
  try {
    auto full = path.has_value() ? ResolveFullPath(*path) : TemporaryPath();
    if (!full.has_value()) {
      return std::unexpected(std::move(full.error()));
    }
    FileAccessMode access = options.access;
    auto opened = OpenHandle(*full, options);
    if (!opened.has_value() && options.allow_read_only_fallback &&
        (opened.error().code() == ErrorCode::kCannotOpen ||
         opened.error().code() == ErrorCode::kPermissionDenied)) {
      options.access = FileAccessMode::kReadOnly;
      options.create = false;
      opened = OpenHandle(*full, options);
      access = FileAccessMode::kReadOnly;
    }
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    return OpenedFile{
        .file = std::make_unique<WindowsFile>(std::move(*opened), std::move(*full), options.kind,
                                              access),
        .access = access,
    };
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Status WindowsVfs::DoDelete(std::string_view path, DirectorySync) {
  auto full = ResolveFullPath(path);
  if (!full.has_value()) {
    return std::unexpected(std::move(full.error()));
  }
  return DeletePath(*full);
}

Result<bool> WindowsVfs::DoAccess(std::string_view path, FileAccessQuery query) {
  try {
    auto native = ToWide(path);
    if (!native.has_value()) {
      return std::unexpected(std::move(native.error()));
    }
    if (native->size() >= MAX_PATH) {
      auto full = ResolveFullPath(*native);
      if (!full.has_value()) {
        return std::unexpected(std::move(full.error()));
      }
      native = ApiPath(*full);
      if (!native.has_value()) {
        return std::unexpected(std::move(native.error()));
      }
    }
    unsigned retries = 0;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    while (::GetFileAttributesExW(native->c_str(), GetFileExInfoStandard, &attributes) == 0) {
      const DWORD error = ::GetLastError();
      if (IsMissing(error)) {
        return false;
      }
      if (!RetryIo(error, retries)) {
        return std::unexpected(OsError("GetFileAttributesExW", error));
      }
    }
    if (query == FileAccessQuery::kExists) {
      return attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow != 0;
    }
    return query != FileAccessQuery::kReadWrite ||
           (attributes.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0U;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<std::string> WindowsVfs::DoFullPath(std::string_view path) {
  try {
    auto full = ResolveFullPath(path);
    if (!full.has_value()) {
      return std::unexpected(std::move(full.error()));
    }
    return ToUtf8(*full);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<ByteCount> WindowsVfs::DoRandomBytes(MutableByteView output) {
  if (auto filled = FillRandomBytes(output); !filled.has_value()) {
    return std::unexpected(std::move(filled.error()));
  }
  return ByteCount{output.size()};
}

Result<std::chrono::microseconds> WindowsVfs::DoSleepFor(std::chrono::microseconds duration) {
  using namespace std::chrono;
  constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<DWORD>::max() - 1U);
  if (duration.count() / 1000 + (duration.count() % 1000 != 0 ? 1 : 0) > maximum) {
    return std::unexpected(Failure(ErrorCode::kTooLarge, "Windows sleep duration is too large"));
  }
  const auto start = steady_clock::now();
  microseconds elapsed{0};
  while (elapsed < duration) {
    const auto remaining = (duration - elapsed).count();
    ::Sleep(static_cast<DWORD>(remaining / 1000 + (remaining % 1000 != 0 ? 1 : 0)));
    elapsed = duration_cast<microseconds>(steady_clock::now() - start);
  }
  return elapsed;
}

Result<WallClockTime> WindowsVfs::DoCurrentTime() {
  return std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

ByteCount WindowsVfs::DoMaximumPathLength() const noexcept {
  return ByteCount{kMaximumWidePath * 3U};
}

}  // namespace modern_sqlite
