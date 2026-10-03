#include "modern_sqlite/platform/posix_vfs.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/platform/vfs.hpp"

namespace modern_sqlite {
namespace {

using namespace std::chrono_literals;

constexpr off_t kPendingByte = 0x40000000;
constexpr off_t kReservedByte = kPendingByte + 1;
constexpr off_t kSharedFirst = kPendingByte + 2;
constexpr off_t kSharedSize = 510;
constexpr off_t kSharedMemoryLockBase = 120;

constexpr int kLockAcquired = 0;
constexpr int kLockBusy = 1;
constexpr int kLockProbeError = 2;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) {
      throw std::system_error(error, "failed to find the temporary directory");
    }

    std::string pattern = (base / "modern-sqlite-posix-XXXXXX").string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    const char* const created = ::mkdtemp(mutable_pattern.data());
    if (created == nullptr) {
      throw std::system_error(errno, std::generic_category(),
                              "failed to create a temporary directory");
    }
    path_ = created;
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  TemporaryDirectory(TemporaryDirectory&&) = delete;
  TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

  // The error-code overload is used for best-effort test cleanup.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] std::string path() const { return path_; }

  [[nodiscard]] std::string Path(std::string_view name) const {
    return (std::filesystem::path(path_) / name).string();
  }

 private:
  std::string path_;
};

class UmaskGuard final {
 public:
  explicit UmaskGuard(mode_t replacement) noexcept : previous_(::umask(replacement)) {}

  UmaskGuard(const UmaskGuard&) = delete;
  UmaskGuard& operator=(const UmaskGuard&) = delete;
  UmaskGuard(UmaskGuard&&) = delete;
  UmaskGuard& operator=(UmaskGuard&&) = delete;

  ~UmaskGuard() { ::umask(previous_); }

 private:
  mode_t previous_;
};

[[nodiscard]] FileOpenOptions CreateMainDatabaseOptions() {
  return FileOpenOptions{
      .kind = FileKind::kMainDatabase,
      .access = FileAccessMode::kReadWrite,
      .create = true,
      .exclusive_create = true,
  };
}

[[nodiscard]] FileOpenOptions OpenMainDatabaseOptions() {
  return FileOpenOptions{
      .kind = FileKind::kMainDatabase,
      .access = FileAccessMode::kReadWrite,
  };
}

[[nodiscard]] FileOpenOptions ReadOnlyMainDatabaseOptions() {
  return FileOpenOptions{
      .kind = FileKind::kMainDatabase,
      .access = FileAccessMode::kReadOnly,
  };
}

[[nodiscard]] ByteView View(const auto& bytes) noexcept { return std::as_bytes(std::span{bytes}); }

[[nodiscard]] MutableByteView MutableView(auto& bytes) noexcept {
  return std::as_writable_bytes(std::span{bytes});
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] int SetLock(int descriptor, short type, off_t start, off_t length) noexcept {
  struct flock lock{};
  lock.l_type = type;
  lock.l_whence = SEEK_SET;
  lock.l_start = start;
  lock.l_len = length;

  int result = -1;
  do {
    result = ::fcntl(descriptor, F_SETLK, &lock);
  } while (result == -1 && errno == EINTR);
  return result;
}

[[nodiscard]] int ProbeLockInChild(std::string_view path, short type, off_t start, off_t length) {
  const std::string owned_path{path};
  const pid_t child = ::fork();
  if (child == -1) {
    return kLockProbeError;
  }
  if (child == 0) {
    int descriptor = -1;
    do {
      descriptor = ::open(owned_path.c_str(), O_RDWR | O_CLOEXEC);
    } while (descriptor == -1 && errno == EINTR);
    if (descriptor == -1) {
      ::_exit(kLockProbeError);
    }

    if (SetLock(descriptor, type, start, length) == 0) {
      static_cast<void>(SetLock(descriptor, F_UNLCK, start, length));
      static_cast<void>(::close(descriptor));
      ::_exit(kLockAcquired);
    }
    const int lock_errno = errno;
    static_cast<void>(::close(descriptor));
    ::_exit(lock_errno == EACCES || lock_errno == EAGAIN ? kLockBusy : kLockProbeError);
  }

  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  if (waited != child || !WIFEXITED(status)) {
    return kLockProbeError;
  }
  return WEXITSTATUS(status);
}

[[nodiscard]] bool TransferByte(int descriptor, void* byte, bool is_write) noexcept {
  ssize_t result = -1;
  do {
    result = is_write ? ::write(descriptor, byte, 1) : ::read(descriptor, byte, 1);
  } while (result == -1 && errno == EINTR);
  return result == 1;
}

template <typename Callback>
[[nodiscard]] int WithChildLock(std::string_view path, short type, off_t start, off_t length,
                                Callback&& callback) {
  std::array<int, 2> ready_pipe{-1, -1};
  std::array<int, 2> release_pipe{-1, -1};
  if (::pipe(ready_pipe.data()) != 0) {
    return kLockProbeError;
  }
  if (::pipe(release_pipe.data()) != 0) {
    static_cast<void>(::close(ready_pipe[0]));
    static_cast<void>(::close(ready_pipe[1]));
    return kLockProbeError;
  }

  const std::string owned_path{path};
  const pid_t child = ::fork();
  if (child == -1) {
    static_cast<void>(::close(ready_pipe[0]));
    static_cast<void>(::close(ready_pipe[1]));
    static_cast<void>(::close(release_pipe[0]));
    static_cast<void>(::close(release_pipe[1]));
    return kLockProbeError;
  }
  if (child == 0) {
    static_cast<void>(::close(ready_pipe[0]));
    static_cast<void>(::close(release_pipe[1]));
    int descriptor = -1;
    do {
      descriptor = ::open(owned_path.c_str(), O_RDWR | O_CLOEXEC);
    } while (descriptor == -1 && errno == EINTR);

    auto status = static_cast<std::uint8_t>(kLockProbeError);
    if (descriptor >= 0) {
      if (SetLock(descriptor, type, start, length) == 0) {
        status = kLockAcquired;
      } else if (errno == EACCES || errno == EAGAIN) {
        status = kLockBusy;
      }
    }
    static_cast<void>(TransferByte(ready_pipe[1], &status, true));
    if (status == kLockAcquired) {
      std::uint8_t release = 0;
      static_cast<void>(TransferByte(release_pipe[0], &release, false));
      static_cast<void>(SetLock(descriptor, F_UNLCK, start, length));
    }
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    static_cast<void>(::close(ready_pipe[1]));
    static_cast<void>(::close(release_pipe[0]));
    ::_exit(status);
  }

  static_cast<void>(::close(ready_pipe[1]));
  static_cast<void>(::close(release_pipe[0]));
  auto status = static_cast<std::uint8_t>(kLockProbeError);
  if (TransferByte(ready_pipe[0], &status, false) && status == kLockAcquired) {
    callback();
    std::uint8_t release = 1;
    static_cast<void>(TransferByte(release_pipe[1], &release, true));
  }
  static_cast<void>(::close(ready_pipe[0]));
  static_cast<void>(::close(release_pipe[1]));

  int child_status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &child_status, 0);
  } while (waited == -1 && errno == EINTR);
  if (waited != child || !WIFEXITED(child_status)) {
    return kLockProbeError;
  }
  return static_cast<int>(status);
}

[[nodiscard]] mode_t PermissionBits(std::string_view path) {
  const std::string owned_path{path};
  struct stat status{};
  if (::stat(owned_path.c_str(), &status) != 0) {
    throw std::system_error(errno, std::generic_category(), "failed to stat a test file");
  }
  return status.st_mode & static_cast<mode_t>(0777);
}

TEST(PosixVfs, CreatesPerformsPositionedIoSyncsAndReopens) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  PosixVfs vfs;

  auto opened = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(opened.has_value());
  ASSERT_NE(nullptr, opened->file);
  EXPECT_EQ(FileAccessMode::kReadWrite, opened->access);

  const std::array<std::uint8_t, 3> payload{0x11, 0x22, 0x33};
  EXPECT_TRUE(opened->file->WriteAt(View(payload), FileOffset{2}).has_value());

  const auto size = opened->file->Size();
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(FileSize{5}, *size);

  std::array<std::uint8_t, 5> output{0xff, 0xff, 0xff, 0xff, 0xff};
  const auto read = opened->file->ReadAt(MutableView(output), FileOffset{0});
  ASSERT_TRUE(read.has_value());
  EXPECT_TRUE(read->complete());
  EXPECT_EQ(ByteCount{5}, read->bytes_read());
  EXPECT_EQ((std::array<std::uint8_t, 5>{0x00, 0x00, 0x11, 0x22, 0x33}), output);

  EXPECT_TRUE(opened->file->Sync(SyncOptions{}).has_value());
  EXPECT_TRUE(
      opened->file->Sync(SyncOptions{.mode = SyncMode::kNormal, .data_only = true}).has_value());
  EXPECT_TRUE(
      opened->file->Sync(SyncOptions{.mode = SyncMode::kFull, .data_only = false}).has_value());
  EXPECT_TRUE(
      opened->file->Sync(SyncOptions{.mode = SyncMode::kFull, .data_only = true}).has_value());

  EXPECT_TRUE(opened->file->Truncate(FileSize{2}).has_value());
  const auto truncated_size = opened->file->Size();
  ASSERT_TRUE(truncated_size.has_value());
  EXPECT_EQ(FileSize{2}, *truncated_size);
  opened->file.reset();

  auto reopened = vfs.Open(path, ReadOnlyMainDatabaseOptions());
  ASSERT_TRUE(reopened.has_value());
  std::array<std::uint8_t, 4> short_output{0xff, 0xff, 0xff, 0xff};
  const auto short_read = reopened->file->ReadAt(MutableView(short_output), FileOffset{0});
  ASSERT_TRUE(short_read.has_value());
  EXPECT_FALSE(short_read->complete());
  EXPECT_EQ(ByteCount{2}, short_read->bytes_read());
  EXPECT_EQ((std::array<std::uint8_t, 4>{0x00, 0x00, 0x00, 0x00}), short_output);
}

TEST(PosixVfs, RejectsReadOnlyMutationsAndUnrepresentableOffsets) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  PosixVfs vfs;

  auto writable = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(writable.has_value());
  const std::array<std::uint8_t, 2> payload{0xaa, 0xbb};
  ASSERT_TRUE(writable->file->WriteAt(View(payload), FileOffset{0}).has_value());
  const auto oversized_truncate =
      writable->file->Truncate(FileSize{std::numeric_limits<std::uint64_t>::max()});
  ASSERT_FALSE(oversized_truncate.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, oversized_truncate.error().code());
  writable->file.reset();

  auto opened = vfs.Open(path, ReadOnlyMainDatabaseOptions());
  ASSERT_TRUE(opened.has_value());

  const auto write = opened->file->WriteAt(View(payload), FileOffset{0});
  const auto truncate = opened->file->Truncate(FileSize{1});
  ASSERT_FALSE(write.has_value());
  ASSERT_FALSE(truncate.has_value());
  EXPECT_EQ(ErrorCode::kReadOnly, write.error().code());
  EXPECT_EQ(ErrorCode::kReadOnly, truncate.error().code());

  std::array<std::uint8_t, 1> byte{};
  const auto oversized_read = opened->file->ReadAt(
      MutableView(byte), FileOffset{static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())});
  ASSERT_FALSE(oversized_read.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, oversized_read.error().code());
}

TEST(PosixVfs, SupportsExplicitReadOnlyFallback) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "root bypasses ordinary file permission checks";
  }

  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  PosixVfs vfs;

  auto created = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(created.has_value());
  created->file.reset();
  ASSERT_EQ(0, ::chmod(path.c_str(), 0400));

  FileOpenOptions fallback = OpenMainDatabaseOptions();
  fallback.allow_read_only_fallback = true;
  auto opened = vfs.Open(path, fallback);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(FileAccessMode::kReadOnly, opened->access);

  const std::array<std::uint8_t, 1> value{0x5a};
  const auto write = opened->file->WriteAt(View(value), FileOffset{0});
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(ErrorCode::kReadOnly, write.error().code());
  opened->file.reset();

  const auto rejected = vfs.Open(path, OpenMainDatabaseOptions());
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kPermissionDenied, rejected.error().code());
}

TEST(PosixVfs, HonorsExclusiveCreateSymlinkPolicyAndDeleteOnClose) {
  const TemporaryDirectory directory;
  const std::string target_path = directory.Path("target.sqlite");
  const std::string alias_path = directory.Path("alias.sqlite");
  PosixVfs vfs;

  auto target = vfs.Open(target_path, CreateMainDatabaseOptions());
  ASSERT_TRUE(target.has_value());
  target->file.reset();

  const auto duplicate = vfs.Open(target_path, CreateMainDatabaseOptions());
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kCannotOpen, duplicate.error().code());

  std::error_code symlink_error;
  std::filesystem::create_symlink(target_path, alias_path, symlink_error);
  ASSERT_FALSE(symlink_error);

  FileOpenOptions no_follow = ReadOnlyMainDatabaseOptions();
  no_follow.no_follow = true;
  const auto rejected_symlink = vfs.Open(alias_path, no_follow);
  ASSERT_FALSE(rejected_symlink.has_value());
  EXPECT_EQ(ErrorCode::kCannotOpen, rejected_symlink.error().code());

  const auto followed_symlink = vfs.Open(alias_path, ReadOnlyMainDatabaseOptions());
  ASSERT_TRUE(followed_symlink.has_value());

  const std::string named_temporary_path = directory.Path("named.tmp");
  const FileOpenOptions temporary_options{
      .kind = FileKind::kTemporaryDatabase,
      .access = FileAccessMode::kReadWrite,
      .create = true,
      .exclusive_create = true,
      .delete_on_close = true,
  };
  auto named_temporary = vfs.Open(named_temporary_path, temporary_options);
  ASSERT_TRUE(named_temporary.has_value());
  const auto named_exists = vfs.Access(named_temporary_path, FileAccessQuery::kExists);
  ASSERT_TRUE(named_exists.has_value());
  EXPECT_FALSE(*named_exists);

  const std::array<std::uint8_t, 1> payload{0x7f};
  EXPECT_TRUE(named_temporary->file->WriteAt(View(payload), FileOffset{0}).has_value());

  auto pathless_temporary = vfs.Open(std::nullopt, temporary_options);
  ASSERT_TRUE(pathless_temporary.has_value());
  EXPECT_TRUE(pathless_temporary->file->WriteAt(View(payload), FileOffset{0}).has_value());
}

TEST(PosixVfs, ResolvesPathsChecksAccessDeletesAndProvidesServices) {
  const TemporaryDirectory directory;
  const std::string real_directory = directory.Path("real");
  const std::string alias_directory = directory.Path("alias");
  ASSERT_TRUE(std::filesystem::create_directory(real_directory));
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(real_directory, alias_directory, symlink_error);
  ASSERT_FALSE(symlink_error);

  PosixVfs vfs;
  const std::string unresolved_path =
      (std::filesystem::path(alias_directory) / "child" / ".." / "missing.sqlite").string();
  const auto resolved = vfs.FullPath(unresolved_path);
  ASSERT_TRUE(resolved.has_value());
  std::error_code canonical_error;
  const std::filesystem::path expected = std::filesystem::weakly_canonical(
      std::filesystem::path(real_directory) / "missing.sqlite", canonical_error);
  ASSERT_FALSE(canonical_error);
  EXPECT_EQ(expected.string(), *resolved);

  const std::string path = directory.Path("delete-me.sqlite");
  auto file = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(file.has_value());
  file->file.reset();

  const auto exists = vfs.Access(path, FileAccessQuery::kExists);
  const auto readable = vfs.Access(path, FileAccessQuery::kRead);
  const auto writable = vfs.Access(path, FileAccessQuery::kReadWrite);
  ASSERT_TRUE(exists.has_value());
  ASSERT_TRUE(readable.has_value());
  ASSERT_TRUE(writable.has_value());
  EXPECT_TRUE(*exists);
  EXPECT_TRUE(*readable);
  EXPECT_TRUE(*writable);

  EXPECT_TRUE(vfs.Delete(path, DirectorySync::kYes).has_value());
  const auto deleted_exists = vfs.Access(path, FileAccessQuery::kExists);
  ASSERT_TRUE(deleted_exists.has_value());
  EXPECT_FALSE(*deleted_exists);

  const auto missing_delete = vfs.Delete(path, DirectorySync::kNo);
  ASSERT_FALSE(missing_delete.has_value());
  EXPECT_EQ(ErrorCode::kNotFound, missing_delete.error().code());

  std::array<std::uint8_t, 32> random_bytes{};
  EXPECT_TRUE(vfs.RandomBytes(MutableView(random_bytes)).has_value());

  const auto before_sleep = std::chrono::steady_clock::now();
  const auto slept = vfs.SleepFor(2ms);
  const auto after_sleep = std::chrono::steady_clock::now();
  ASSERT_TRUE(slept.has_value());
  EXPECT_GE(*slept, 2ms);
  EXPECT_GE(after_sleep - before_sleep, 2ms);

  const auto before_time =
      std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
  const auto current_time = vfs.CurrentTime();
  const auto after_time =
      std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
  ASSERT_TRUE(current_time.has_value());
  EXPECT_GE(*current_time, before_time);
  EXPECT_LE(*current_time, after_time);
}

TEST(PosixVfs, ReportsConservativeFileProperties) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  PosixVfs vfs;
  auto opened = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(opened.has_value());

  const auto properties = opened->file->Properties();
  ASSERT_TRUE(properties.has_value());
  EXPECT_EQ(ByteCount{4096}, properties->sector_size);
  EXPECT_TRUE(properties->device_characteristics.Has(DeviceCapability::kPowersafeOverwrite));
  EXPECT_TRUE(properties->device_characteristics.Has(DeviceCapability::kSubpageRead));
  EXPECT_FALSE(properties->device_characteristics.Has(DeviceCapability::kAtomic));
  EXPECT_FALSE(properties->device_characteristics.SupportsAtomicWriteSize(ByteCount{4096}));
}

TEST(PosixVfs, CoordinatesDatabaseLocksWithinAndAcrossProcesses) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  PosixVfs vfs;

  auto first = vfs.Open(path, CreateMainDatabaseOptions());
  auto second = vfs.Open(path, OpenMainDatabaseOptions());
  auto third = vfs.Open(path, OpenMainDatabaseOptions());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());

  ASSERT_TRUE(first->file->Lock(DatabaseLock::kShared).has_value());
  ASSERT_TRUE(second->file->Lock(DatabaseLock::kShared).has_value());
  EXPECT_EQ(kLockBusy, ProbeLockInChild(path, F_WRLCK, kSharedFirst, kSharedSize));

  ASSERT_TRUE(first->file->Lock(DatabaseLock::kReserved).has_value());
  const auto second_reserved = second->file->Lock(DatabaseLock::kReserved);
  ASSERT_FALSE(second_reserved.has_value());
  EXPECT_EQ(ErrorCode::kBusy, second_reserved.error().code());

  const auto has_reserved = second->file->HasReservedLock();
  ASSERT_TRUE(has_reserved.has_value());
  EXPECT_TRUE(*has_reserved);

  const auto pending = first->file->Lock(DatabaseLock::kExclusive);
  ASSERT_FALSE(pending.has_value());
  EXPECT_EQ(ErrorCode::kBusy, pending.error().code());

  const auto blocked_reader = third->file->Lock(DatabaseLock::kShared);
  ASSERT_FALSE(blocked_reader.has_value());
  EXPECT_EQ(ErrorCode::kBusy, blocked_reader.error().code());

  ASSERT_TRUE(second->file->Unlock(DatabaseLock::kNone).has_value());
  ASSERT_TRUE(first->file->Lock(DatabaseLock::kExclusive).has_value());
  EXPECT_EQ(kLockBusy, ProbeLockInChild(path, F_RDLCK, kSharedFirst, kSharedSize));

  ASSERT_TRUE(first->file->Unlock(DatabaseLock::kShared).has_value());
  ASSERT_TRUE(third->file->Lock(DatabaseLock::kShared).has_value());
  ASSERT_TRUE(first->file->Unlock(DatabaseLock::kNone).has_value());
  ASSERT_TRUE(third->file->Unlock(DatabaseLock::kNone).has_value());
  EXPECT_EQ(kLockAcquired, ProbeLockInChild(path, F_WRLCK, kSharedFirst, kSharedSize));
}

TEST(PosixVfs, DefersSiblingDescriptorCloseWhileLocksRemain) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  PosixVfs vfs;

  auto locked = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(locked.has_value());
  ASSERT_TRUE(locked->file->Lock(DatabaseLock::kShared).has_value());
  EXPECT_EQ(kLockBusy, ProbeLockInChild(path, F_WRLCK, kSharedFirst, kSharedSize));

  {
    const auto sibling = vfs.Open(path, OpenMainDatabaseOptions());
    ASSERT_TRUE(sibling.has_value());
  }

  EXPECT_EQ(kLockBusy, ProbeLockInChild(path, F_WRLCK, kSharedFirst, kSharedSize));
  ASSERT_TRUE(locked->file->Unlock(DatabaseLock::kNone).has_value());
  EXPECT_EQ(kLockAcquired, ProbeLockInChild(path, F_WRLCK, kSharedFirst, kSharedSize));
}

TEST(PosixVfs, DetectsCrossProcessReservedPendingAndExclusiveLocks) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  PosixVfs vfs;

  auto opened = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(opened.has_value());

  for (const auto [start, length] : std::array<std::pair<off_t, off_t>, 3>{{
           {kReservedByte, 1},
           {kPendingByte, 1},
           {kSharedFirst, kSharedSize},
       }}) {
    std::optional<Result<bool>> observed;
    const int child_status = WithChildLock(path, F_WRLCK, start, length,
                                           [&] { observed = opened->file->HasReservedLock(); });
    ASSERT_EQ(kLockAcquired, child_status);
    ASSERT_TRUE(observed.has_value());
    ASSERT_TRUE(observed->has_value());
    EXPECT_TRUE(**observed);
  }

  const auto unlocked = opened->file->HasReservedLock();
  ASSERT_TRUE(unlocked.has_value());
  EXPECT_FALSE(*unlocked);

  std::optional<Result<bool>> observed_shared_acquisition;
  const int shared_acquisition_status = WithChildLock(path, F_RDLCK, kPendingByte, 1, [&] {
    observed_shared_acquisition = opened->file->HasReservedLock();
  });
  ASSERT_EQ(kLockAcquired, shared_acquisition_status);
  ASSERT_TRUE(observed_shared_acquisition.has_value());
  ASSERT_TRUE(observed_shared_acquisition->has_value());
  EXPECT_FALSE(**observed_shared_acquisition);
}

TEST(PosixVfs, EnforcesDatabaseLockSequencesAndReadOnlyLimits) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  PosixVfs vfs;

  auto writable = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(writable.has_value());
  const auto reserved_without_shared = writable->file->Lock(DatabaseLock::kReserved);
  const auto exclusive_without_shared = writable->file->Lock(DatabaseLock::kExclusive);
  ASSERT_FALSE(reserved_without_shared.has_value());
  ASSERT_FALSE(exclusive_without_shared.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, reserved_without_shared.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, exclusive_without_shared.error().code());
  EXPECT_TRUE(writable->file->Unlock(DatabaseLock::kShared).has_value());
  writable->file.reset();

  auto read_only = vfs.Open(path, ReadOnlyMainDatabaseOptions());
  ASSERT_TRUE(read_only.has_value());
  ASSERT_TRUE(read_only->file->Lock(DatabaseLock::kShared).has_value());
  EXPECT_TRUE(read_only->file->Lock(DatabaseLock::kShared).has_value());
  const auto reserved = read_only->file->Lock(DatabaseLock::kReserved);
  ASSERT_FALSE(reserved.has_value());
  EXPECT_EQ(ErrorCode::kReadOnly, reserved.error().code());
  EXPECT_TRUE(read_only->file->Unlock(DatabaseLock::kNone).has_value());
}

TEST(PosixVfs, SharesLockAndMappingStateAcrossAliasesAndVfsInstances) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  const std::string alias = directory.Path("database-alias.sqlite");
  PosixVfs first_vfs;
  PosixVfs second_vfs;

  auto first = first_vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(first.has_value());
  std::error_code symlink_error;
  std::filesystem::create_symlink(path, alias, symlink_error);
  ASSERT_FALSE(symlink_error);
  auto second = second_vfs.Open(alias, OpenMainDatabaseOptions());
  ASSERT_TRUE(second.has_value());

  ASSERT_TRUE(first->file->Lock(DatabaseLock::kShared).has_value());
  ASSERT_TRUE(second->file->Lock(DatabaseLock::kShared).has_value());
  ASSERT_TRUE(first->file->Lock(DatabaseLock::kReserved).has_value());
  const auto alias_reserved = second->file->Lock(DatabaseLock::kReserved);
  ASSERT_FALSE(alias_reserved.has_value());
  EXPECT_EQ(ErrorCode::kBusy, alias_reserved.error().code());

  auto first_mapping = first->file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                    SharedMemoryMapMode::kExtend);
  ASSERT_TRUE(first_mapping.has_value());
  ASSERT_TRUE(first_mapping->has_value());
  (*first_mapping)->front() = std::byte{0x6a};

  auto second_mapping = second->file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                      SharedMemoryMapMode::kExistingOnly);
  ASSERT_TRUE(second_mapping.has_value());
  ASSERT_TRUE(second_mapping->has_value());
  EXPECT_EQ(std::byte{0x6a}, (*second_mapping)->front());

  EXPECT_TRUE(first->file->UnmapSharedMemory(SharedMemoryUnmapMode::kKeep).has_value());
  EXPECT_TRUE(second->file->UnmapSharedMemory(SharedMemoryUnmapMode::kDelete).has_value());
  EXPECT_TRUE(first->file->Unlock(DatabaseLock::kNone).has_value());
  EXPECT_TRUE(second->file->Unlock(DatabaseLock::kNone).has_value());
}

TEST(PosixVfs, MapsSharesLocksAndCleansSharedMemory) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  const std::string shared_memory_path = path + "-shm";
  PosixVfs vfs;

  auto first = vfs.Open(path, CreateMainDatabaseOptions());
  auto second = vfs.Open(path, OpenMainDatabaseOptions());
  auto third = vfs.Open(path, OpenMainDatabaseOptions());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());

  auto first_region = first->file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                   SharedMemoryMapMode::kExtend);
  ASSERT_TRUE(first_region.has_value());
  ASSERT_TRUE(first_region->has_value());
  (*first_region)->front() = std::byte{0x42};

  auto second_region = second->file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                     SharedMemoryMapMode::kExistingOnly);
  ASSERT_TRUE(second_region.has_value());
  ASSERT_TRUE(second_region->has_value());
  EXPECT_EQ(std::byte{0x42}, (*second_region)->front());

  const auto third_region = third->file->MapSharedMemory(
      SharedMemoryRegionIndex{0}, ByteCount{4096}, SharedMemoryMapMode::kExistingOnly);
  ASSERT_TRUE(third_region.has_value());
  ASSERT_TRUE(third_region->has_value());

  const auto absent = first->file->MapSharedMemory(SharedMemoryRegionIndex{1}, ByteCount{4096},
                                                   SharedMemoryMapMode::kExistingOnly);
  ASSERT_TRUE(absent.has_value());
  EXPECT_FALSE(absent->has_value());

  auto extended = second->file->MapSharedMemory(SharedMemoryRegionIndex{1}, ByteCount{4096},
                                                SharedMemoryMapMode::kExtend);
  ASSERT_TRUE(extended.has_value());
  ASSERT_TRUE(extended->has_value());
  (*extended)->back() = std::byte{0x24};

  auto observed = first->file->MapSharedMemory(SharedMemoryRegionIndex{1}, ByteCount{4096},
                                               SharedMemoryMapMode::kExistingOnly);
  ASSERT_TRUE(observed.has_value());
  ASSERT_TRUE(observed->has_value());
  EXPECT_EQ(std::byte{0x24}, (*observed)->back());

  const auto mismatched_size = first->file->MapSharedMemory(
      SharedMemoryRegionIndex{0}, ByteCount{8192}, SharedMemoryMapMode::kExistingOnly);
  ASSERT_FALSE(mismatched_size.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, mismatched_size.error().code());

  const SharedMemoryLockRange slot_three{.offset = 3, .count = 1};
  ASSERT_TRUE(first->file
                  ->LockSharedMemory(slot_three, SharedMemoryLockOperation::kLock,
                                     SharedMemoryLockMode::kShared)
                  .has_value());
  ASSERT_TRUE(second->file
                  ->LockSharedMemory(slot_three, SharedMemoryLockOperation::kLock,
                                     SharedMemoryLockMode::kShared)
                  .has_value());
  const auto blocked_exclusive = third->file->LockSharedMemory(
      slot_three, SharedMemoryLockOperation::kLock, SharedMemoryLockMode::kExclusive);
  ASSERT_FALSE(blocked_exclusive.has_value());
  EXPECT_EQ(ErrorCode::kBusy, blocked_exclusive.error().code());

  EXPECT_TRUE(first->file
                  ->LockSharedMemory(slot_three, SharedMemoryLockOperation::kUnlock,
                                     SharedMemoryLockMode::kShared)
                  .has_value());
  EXPECT_TRUE(second->file
                  ->LockSharedMemory(slot_three, SharedMemoryLockOperation::kUnlock,
                                     SharedMemoryLockMode::kShared)
                  .has_value());

  const SharedMemoryLockRange exclusive_range{.offset = 2, .count = 3};
  ASSERT_TRUE(first->file
                  ->LockSharedMemory(exclusive_range, SharedMemoryLockOperation::kLock,
                                     SharedMemoryLockMode::kExclusive)
                  .has_value());
  const auto blocked_shared = second->file->LockSharedMemory(
      SharedMemoryLockRange{.offset = 2, .count = 1}, SharedMemoryLockOperation::kLock,
      SharedMemoryLockMode::kShared);
  ASSERT_FALSE(blocked_shared.has_value());
  EXPECT_EQ(ErrorCode::kBusy, blocked_shared.error().code());
  EXPECT_EQ(kLockBusy, ProbeLockInChild(shared_memory_path, F_RDLCK, kSharedMemoryLockBase + 2, 1));

  first->file->SharedMemoryBarrier();
  EXPECT_TRUE(first->file->UnmapSharedMemory(SharedMemoryUnmapMode::kKeep).has_value());
  EXPECT_EQ(kLockAcquired,
            ProbeLockInChild(shared_memory_path, F_WRLCK, kSharedMemoryLockBase + 2, 1));
  EXPECT_TRUE(second->file
                  ->LockSharedMemory(exclusive_range, SharedMemoryLockOperation::kLock,
                                     SharedMemoryLockMode::kExclusive)
                  .has_value());
  EXPECT_TRUE(second->file
                  ->LockSharedMemory(exclusive_range, SharedMemoryLockOperation::kUnlock,
                                     SharedMemoryLockMode::kExclusive)
                  .has_value());

  EXPECT_EQ(std::byte{0x42}, (*second_region)->front());
  EXPECT_TRUE(second->file->UnmapSharedMemory(SharedMemoryUnmapMode::kKeep).has_value());
  EXPECT_TRUE(third->file->UnmapSharedMemory(SharedMemoryUnmapMode::kDelete).has_value());

  const auto shared_memory_exists = vfs.Access(shared_memory_path, FileAccessQuery::kExists);
  ASSERT_TRUE(shared_memory_exists.has_value());
  EXPECT_FALSE(*shared_memory_exists);
}

TEST(PosixVfs, TruncatesStaleSharedMemoryBeforeMapping) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  const std::string shared_memory_path = path + "-shm";
  PosixVfs vfs;

  auto database = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(database.has_value());

  const FileOpenOptions stale_options{
      .kind = FileKind::kWriteAheadLog,
      .access = FileAccessMode::kReadWrite,
      .create = true,
      .exclusive_create = true,
  };
  auto stale = vfs.Open(shared_memory_path, stale_options);
  ASSERT_TRUE(stale.has_value());
  std::array<std::uint8_t, 4096> stale_bytes{};
  std::ranges::fill(stale_bytes, static_cast<std::uint8_t>(0xa5));
  ASSERT_TRUE(stale->file->WriteAt(View(stale_bytes), FileOffset{0}).has_value());
  stale->file.reset();

  const auto absent = database->file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                      SharedMemoryMapMode::kExistingOnly);
  ASSERT_TRUE(absent.has_value());
  EXPECT_FALSE(absent->has_value());

  auto mapped = database->file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                SharedMemoryMapMode::kExtend);
  ASSERT_TRUE(mapped.has_value());
  ASSERT_TRUE(mapped->has_value());
  EXPECT_TRUE(std::ranges::all_of((*mapped)->first(3),
                                  [](std::byte value) { return value == std::byte{0xa5}; }));
  EXPECT_TRUE(std::ranges::all_of((*mapped)->subspan(3),
                                  [](std::byte value) { return value == std::byte{0}; }));
  EXPECT_TRUE(database->file->UnmapSharedMemory(SharedMemoryUnmapMode::kDelete).has_value());
}

TEST(PosixVfs, InheritsDatabasePermissionsAndSyncsNewJournalDirectory) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  const std::string journal_path = path + "-journal";
  PosixVfs vfs;

  auto database = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(database.has_value());
  database->file.reset();
  ASSERT_EQ(0, ::chmod(path.c_str(), 0640));

  {
    const UmaskGuard umask{0};
    const FileOpenOptions journal_options{
        .kind = FileKind::kMainJournal,
        .access = FileAccessMode::kReadWrite,
        .create = true,
        .exclusive_create = true,
    };
    auto journal = vfs.Open(journal_path, journal_options);
    ASSERT_TRUE(journal.has_value());
    const std::array<std::uint8_t, 4> payload{0x10, 0x20, 0x30, 0x40};
    ASSERT_TRUE(journal->file->WriteAt(View(payload), FileOffset{0}).has_value());
    EXPECT_TRUE(
        journal->file->Sync(SyncOptions{.mode = SyncMode::kFull, .data_only = false}).has_value());
  }

  EXPECT_EQ(static_cast<mode_t>(0640), PermissionBits(journal_path));
}

TEST(PosixVfs, SupportsConcurrentPositionedIoOnSeparateHandles) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("database.sqlite");
  PosixVfs vfs;

  auto first = vfs.Open(path, CreateMainDatabaseOptions());
  auto second = vfs.Open(path, OpenMainDatabaseOptions());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  const std::array<std::uint8_t, 4096> first_payload = [] {
    std::array<std::uint8_t, 4096> value{};
    value.fill(0x11);
    return value;
  }();
  const std::array<std::uint8_t, 4096> second_payload = [] {
    std::array<std::uint8_t, 4096> value{};
    value.fill(0x22);
    return value;
  }();
  std::atomic<bool> first_ok{false};
  std::atomic<bool> second_ok{false};

  std::jthread first_writer([&] {
    first_ok.store(first->file->WriteAt(View(first_payload), FileOffset{0}).has_value(),
                   std::memory_order_relaxed);
  });
  std::jthread second_writer([&] {
    second_ok.store(second->file->WriteAt(View(second_payload), FileOffset{4096}).has_value(),
                    std::memory_order_relaxed);
  });
  first_writer.join();
  second_writer.join();
  ASSERT_TRUE(first_ok.load(std::memory_order_relaxed));
  ASSERT_TRUE(second_ok.load(std::memory_order_relaxed));

  std::array<std::uint8_t, 8192> output{};
  const auto read = first->file->ReadAt(MutableView(output), FileOffset{0});
  ASSERT_TRUE(read.has_value());
  ASSERT_TRUE(read->complete());
  EXPECT_TRUE(std::ranges::all_of(output.begin(), output.begin() + 4096,
                                  [](std::uint8_t value) { return value == 0x11; }));
  EXPECT_TRUE(std::ranges::all_of(output.begin() + 4096, output.end(),
                                  [](std::uint8_t value) { return value == 0x22; }));
}

TEST(PosixVfs, MapsRepresentativeOperatingSystemErrors) {
  const TemporaryDirectory directory;
  PosixVfs vfs;
  const std::string missing = directory.Path("missing.sqlite");

  const auto missing_open = vfs.Open(missing, ReadOnlyMainDatabaseOptions());
  ASSERT_FALSE(missing_open.has_value());
  EXPECT_EQ(ErrorCode::kCannotOpen, missing_open.error().code());

  const auto directory_open = vfs.Open(directory.path(), ReadOnlyMainDatabaseOptions());
  ASSERT_FALSE(directory_open.has_value());
  EXPECT_EQ(ErrorCode::kCannotOpen, directory_open.error().code());
}

}  // namespace
}  // namespace modern_sqlite
