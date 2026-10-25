#include "modern_sqlite/platform/windows_vfs.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "tests/fuzz/private_database.hpp"
#include "tests/support/windows_process.hpp"
#include "tests/support/windows_temporary_directory.hpp"

namespace modern_sqlite {
namespace {

using TemporaryDirectory = test_support::WindowsTemporaryDirectory;

[[nodiscard]] DWORD Probe(std::initializer_list<std::wstring_view> arguments) {
  return test_support::RunWindowsProcess(test_support::WindowsProcessExecutable().parent_path() /
                                             L"modern_sqlite_windows_vfs_probe.exe",
                                         arguments);
}

[[nodiscard]] FileOpenOptions CreateMainDatabaseOptions() {
  return FileOpenOptions{
      .kind = FileKind::kMainDatabase,
      .access = FileAccessMode::kReadWrite,
      .create = true,
      .exclusive_create = true,
  };
}

[[nodiscard]] FileOpenOptions OpenMainDatabaseOptions() {
  return FileOpenOptions{.access = FileAccessMode::kReadWrite};
}

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error{result.error().ToString()};
  }
  return std::move(*result);
}

TEST(WindowsProcess, PreservesOwnedChildDiagnosticsWithoutInheritingUnrelatedHandles) {
  HANDLE raw_reader = nullptr;
  HANDLE raw_writer = nullptr;
  ASSERT_NE(0, ::CreatePipe(&raw_reader, &raw_writer, nullptr, 0));
  const auto reader = test_support::OwnWindowsHandle(raw_reader);
  auto writer = test_support::OwnWindowsHandle(raw_writer);
  const auto unrelated =
      test_support::OwnWindowsHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  ASSERT_NE(nullptr, unrelated.get());
  ASSERT_NE(0, ::SetHandleInformation(unrelated.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
  DWORD result = 0;
  {
    struct RestoreStandardError final {
      HANDLE previous;
      ~RestoreStandardError() { static_cast<void>(::SetStdHandle(STD_ERROR_HANDLE, previous)); }
    };
    const RestoreStandardError restore{::GetStdHandle(STD_ERROR_HANDLE)};
    ASSERT_NE(0, ::SetStdHandle(STD_ERROR_HANDLE, writer.get()));
    result =
        Probe({L"diagnostic", std::to_wstring(reinterpret_cast<std::uintptr_t>(unrelated.get()))});
  }
  writer.reset();
  EXPECT_EQ(17U, result);
  std::array<char, 64> output{};
  DWORD count = 0;
  static_cast<void>(
      ::ReadFile(reader.get(), output.data(), static_cast<DWORD>(output.size()), &count, nullptr));
  EXPECT_EQ("owned child diagnostic\r\n", std::string(output.data(), count));
  EXPECT_EQ(WAIT_TIMEOUT, ::WaitForSingleObject(unrelated.get(), 0));
}

TEST(WindowsVfs, CreatesPerformsPositionedIoSyncsAndReopens) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("positioned.db");
  WindowsVfs vfs;
  auto opened = vfs.Open(path, CreateMainDatabaseOptions());
  ASSERT_TRUE(opened.has_value());
  ASSERT_EQ(FileAccessMode::kReadWrite, opened->access);
  const std::array data{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  ASSERT_TRUE(opened->file->WriteAt(data, FileOffset{5}).has_value());
  ASSERT_TRUE(opened->file->Sync({.mode = SyncMode::kFull}).has_value());
  const auto size = opened->file->Size();
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(FileSize{9}, *size);

  std::array<std::byte, 8> output{};
  output.fill(std::byte{0xff});
  const auto read = opened->file->ReadAt(output, FileOffset{3});
  ASSERT_TRUE(read.has_value());
  EXPECT_FALSE(read->complete());
  EXPECT_EQ(ByteCount{6}, read->bytes_read());
  const std::array expected{std::byte{0}, std::byte{0}, std::byte{1}, std::byte{2},
                            std::byte{3}, std::byte{4}, std::byte{0}, std::byte{0}};
  EXPECT_EQ(expected, output);
  ASSERT_TRUE(opened->file->Truncate(FileSize{7}).has_value());
  ASSERT_TRUE(opened->file->Sync({.mode = SyncMode::kNormal, .data_only = true}).has_value());
  opened->file.reset();

  auto reopened = vfs.Open(path, FileOpenOptions{});
  ASSERT_TRUE(reopened.has_value());
  const auto retained = reopened->file->ReadAt(output, FileOffset{5});
  ASSERT_TRUE(retained.has_value());
  EXPECT_EQ(ByteCount{2}, retained->bytes_read());
  EXPECT_EQ(std::byte{1}, output[0]);
  EXPECT_EQ(std::byte{2}, output[1]);
  EXPECT_EQ(std::byte{0}, output[2]);
  reopened->file.reset();
  ASSERT_TRUE(vfs.Delete(path, DirectorySync::kYes).has_value());
  const auto exists = vfs.Access(path, FileAccessQuery::kExists);
  ASSERT_TRUE(exists.has_value());
  EXPECT_FALSE(*exists);
}

TEST(WindowsVfs, RejectsReadOnlyWritesAndChecksSignedOffsetsWithoutWrapping) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("readonly.db");
  WindowsVfs vfs;
  auto file = TakeValue(vfs.Open(path, CreateMainDatabaseOptions()));
  const std::array data{std::byte{42}};
  ASSERT_TRUE(file.file->WriteAt(data, FileOffset{0}).has_value());
  std::array<std::byte, 1> output{std::byte{0xff}};
  const auto far_read = TakeValue(file.file->ReadAt(output, FileOffset{0x1'0000'0000ULL}));
  EXPECT_EQ(ByteCount{0}, far_read.bytes_read());
  EXPECT_EQ(std::byte{0}, output.front());
  const auto invalid_read =
      file.file->ReadAt(output, FileOffset{std::numeric_limits<std::uint64_t>::max()});
  ASSERT_FALSE(invalid_read.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, invalid_read.error().code());
  const auto invalid_size =
      file.file->Truncate(FileSize{std::numeric_limits<std::uint64_t>::max()});
  ASSERT_FALSE(invalid_size.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, invalid_size.error().code());
  file.file.reset();

  auto readonly = TakeValue(vfs.Open(path, FileOpenOptions{}));
  const auto written = readonly.file->WriteAt(data, FileOffset{0});
  ASSERT_FALSE(written.has_value());
  EXPECT_EQ(ErrorCode::kReadOnly, written.error().code());
  const auto truncated = readonly.file->Truncate(FileSize{0});
  ASSERT_FALSE(truncated.has_value());
  EXPECT_EQ(ErrorCode::kReadOnly, truncated.error().code());
  ASSERT_TRUE(readonly.file->Lock(DatabaseLock::kShared).has_value());
  const auto locked = readonly.file->Lock(DatabaseLock::kReserved);
  ASSERT_FALSE(locked.has_value());
  EXPECT_EQ(ErrorCode::kReadOnly, locked.error().code());
}

TEST(WindowsVfs, PreservesExplicitReadOnlyFallbackAndNativeAccessObservations) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("attributes.db");
  const auto native = directory.NativePath("attributes.db");
  WindowsVfs vfs;
  auto file = TakeValue(vfs.Open(path, CreateMainDatabaseOptions()));
  EXPECT_FALSE(TakeValue(vfs.Access(path, FileAccessQuery::kExists)));
  EXPECT_TRUE(TakeValue(vfs.Access(path, FileAccessQuery::kRead)));
  const std::array data{std::byte{1}};
  ASSERT_TRUE(file.file->WriteAt(data, FileOffset{0}).has_value());
  EXPECT_TRUE(TakeValue(vfs.Access(path, FileAccessQuery::kExists)));
  file.file.reset();
  ASSERT_NE(0, ::SetFileAttributesW(native.c_str(), FILE_ATTRIBUTE_READONLY));
  EXPECT_FALSE(TakeValue(vfs.Access(path, FileAccessQuery::kReadWrite)));
  const auto denied = vfs.Open(path, OpenMainDatabaseOptions());
  EXPECT_FALSE(denied.has_value());
  auto options = OpenMainDatabaseOptions();
  options.allow_read_only_fallback = true;
  auto fallback = vfs.Open(path, options);
  ASSERT_NE(0, ::SetFileAttributesW(native.c_str(), FILE_ATTRIBUTE_NORMAL));
  ASSERT_TRUE(fallback.has_value());
  EXPECT_EQ(FileAccessMode::kReadOnly, fallback->access);
}

TEST(WindowsVfs, EnforcesExclusiveCreationAndDeleteOnCloseWithoutDeletingOpenFiles) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("exclusive.db");
  WindowsVfs vfs;
  auto file = TakeValue(vfs.Open(path, CreateMainDatabaseOptions()));
  EXPECT_FALSE(vfs.Open(path, CreateMainDatabaseOptions()).has_value());
  const auto deleted = vfs.Delete(path, DirectorySync::kYes);
  EXPECT_FALSE(deleted.has_value());
  file.file.reset();
  ASSERT_TRUE(vfs.Delete(path, DirectorySync::kYes).has_value());
  const auto missing = vfs.Delete(path, DirectorySync::kNo);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(ErrorCode::kNotFound, missing.error().code());

  const std::string temporary = directory.Path("temporary.db");
  auto options = CreateMainDatabaseOptions();
  options.kind = FileKind::kTemporaryDatabase;
  options.delete_on_close = true;
  auto transient = TakeValue(vfs.Open(temporary, options));
  const std::array data{std::byte{2}};
  ASSERT_TRUE(transient.file->WriteAt(data, FileOffset{0}).has_value());
  EXPECT_TRUE(TakeValue(vfs.Access(temporary, FileAccessQuery::kExists)));
  transient.file.reset();
  EXPECT_FALSE(TakeValue(vfs.Access(temporary, FileAccessQuery::kRead)));
  auto pathless = TakeValue(vfs.Open(std::nullopt, options));
  ASSERT_TRUE(pathless.file->WriteAt(data, FileOffset{0}).has_value());
  EXPECT_EQ(FileSize{1}, TakeValue(pathless.file->Size()));
}

TEST(WindowsVfs, ResolvesUnicodePathsRejectsInvalidUtf8AndReportsNativeProperties) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("\xE6\x95\xB0\xE6\x8D\xAE.db");
  WindowsVfs vfs;
  EXPECT_EQ(path, TakeValue(vfs.FullPath(path)));
  auto file = TakeValue(vfs.Open(path, CreateMainDatabaseOptions()));
  const auto properties = TakeValue(file.file->Properties());
  EXPECT_EQ(ByteCount{4096}, properties.sector_size);
  EXPECT_TRUE(properties.device_characteristics.Has(DeviceCapability::kUndeletableWhenOpen));
  EXPECT_TRUE(properties.device_characteristics.Has(DeviceCapability::kPowersafeOverwrite));
  EXPECT_TRUE(properties.device_characteristics.Has(DeviceCapability::kSubpageRead));
  EXPECT_FALSE(properties.device_characteristics.Has(DeviceCapability::kAtomic));
  EXPECT_FALSE(properties.device_characteristics.Has(DeviceCapability::kSafeAppend));
  auto invalid = vfs.FullPath("\xC3(");
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(ErrorCode::kCannotOpen, invalid.error().code());
  EXPECT_FALSE(vfs.Open(directory.Path(""), FileOpenOptions{}).has_value());
  auto options = OpenMainDatabaseOptions();
  options.no_follow = true;
  EXPECT_TRUE(vfs.Open(path, options).has_value());
}

TEST(WindowsVfs, RejectsFinalReparsePointsWithoutFollowingTheirTargets) {
  const TemporaryDirectory directory;
  const std::string target = directory.Path("target.db");
  const std::string link = directory.Path("link.db");
  WindowsVfs vfs;
  auto created = TakeValue(vfs.Open(target, CreateMainDatabaseOptions()));
  const std::array data{std::byte{47}};
  ASSERT_TRUE(created.file->WriteAt(data, FileOffset{0}).has_value());
  created.file.reset();
  ASSERT_NE(0, ::CreateSymbolicLinkW(directory.NativePath("link.db").c_str(),
                                     directory.NativePath("target.db").c_str(),
                                     SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
  auto followed = TakeValue(vfs.Open(link, FileOpenOptions{}));
  std::array<std::byte, 1> output{};
  ASSERT_TRUE(followed.file->ReadAt(output, FileOffset{0}).has_value());
  EXPECT_EQ(data, output);
  auto options = FileOpenOptions{};
  options.no_follow = true;
  const auto rejected = vfs.Open(link, options);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kCannotOpen, rejected.error().code());
  auto original = TakeValue(vfs.Open(target, options));
  ASSERT_TRUE(original.file->ReadAt(output, FileOffset{0}).has_value());
  EXPECT_EQ(data, output);
}

TEST(WindowsVfs, PinsOnePrivateImageOwnerForReadAndWriteFuzzConsumers) {
  WindowsVfs vfs;
  std::string retained;
  {
    fuzz::detail::PrivateDatabase database;
    const std::array<std::uint8_t, 4> image{1, 2, 3, 4};
    ASSERT_TRUE(database.Initialize(image));
    retained = database.path();
    auto opened = TakeValue(vfs.Open(retained, FileOpenOptions{}));
    std::array<std::byte, 4> output{};
    ASSERT_TRUE(opened.file->ReadAt(output, FileOffset{0}).has_value());
    EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span{image}), output));
    opened.file.reset();
    const auto native = std::filesystem::path{std::u8string{retained.begin(), retained.end()}};
    const auto parent = native.parent_path();
    auto moved = parent;
    moved += L"-moved";
    std::error_code error;
    std::filesystem::rename(parent, moved, error);
    if (!error) {
      std::filesystem::rename(moved, parent);
      FAIL() << "a live private image directory was not pinned against replacement";
    }
    opened = TakeValue(vfs.Open(retained, FileOpenOptions{}));
    database.Unlink();
    ASSERT_TRUE(opened.file->ReadAt(output, FileOffset{0}).has_value());
    EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span{image}), output));
  }
  EXPECT_FALSE(TakeValue(vfs.Access(retained, FileAccessQuery::kRead)));
}

TEST(WindowsVfs, RetainsPendingWriterFenceUntilExclusiveUpgradeOrDowngrade) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("locks.db");
  WindowsVfs first_vfs;
  WindowsVfs second_vfs;
  auto writer = TakeValue(first_vfs.Open(path, CreateMainDatabaseOptions()));
  auto reader = TakeValue(second_vfs.Open(path, OpenMainDatabaseOptions()));
  auto later = TakeValue(second_vfs.Open(path, OpenMainDatabaseOptions()));
  ASSERT_TRUE(writer.file->Lock(DatabaseLock::kShared).has_value());
  ASSERT_TRUE(reader.file->Lock(DatabaseLock::kShared).has_value());
  ASSERT_TRUE(writer.file->Lock(DatabaseLock::kReserved).has_value());
  EXPECT_TRUE(TakeValue(reader.file->HasReservedLock()));
  auto reserved = reader.file->Lock(DatabaseLock::kReserved);
  ASSERT_FALSE(reserved.has_value());
  EXPECT_EQ(ErrorCode::kBusy, reserved.error().code());
  auto upgrade = writer.file->Lock(DatabaseLock::kExclusive);
  ASSERT_FALSE(upgrade.has_value());
  EXPECT_EQ(ErrorCode::kBusy, upgrade.error().code());
  auto blocked = later.file->Lock(DatabaseLock::kShared);
  ASSERT_FALSE(blocked.has_value());
  EXPECT_EQ(ErrorCode::kBusy, blocked.error().code());
  ASSERT_TRUE(reader.file->Unlock(DatabaseLock::kNone).has_value());
  ASSERT_TRUE(writer.file->Lock(DatabaseLock::kExclusive).has_value());
  ASSERT_TRUE(writer.file->Unlock(DatabaseLock::kShared).has_value());
  ASSERT_TRUE(later.file->Lock(DatabaseLock::kShared).has_value());
  EXPECT_FALSE(TakeValue(later.file->HasReservedLock()));
}

TEST(WindowsVfs, ClosingASiblingHandleDoesNotReleaseAnotherHandlesLocks) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("close.db");
  WindowsVfs vfs;
  auto first = TakeValue(vfs.Open(path, CreateMainDatabaseOptions()));
  auto second = TakeValue(vfs.Open(path, OpenMainDatabaseOptions()));
  auto third = TakeValue(vfs.Open(path, OpenMainDatabaseOptions()));
  ASSERT_TRUE(first.file->Lock(DatabaseLock::kShared).has_value());
  ASSERT_TRUE(first.file->Lock(DatabaseLock::kReserved).has_value());
  ASSERT_TRUE(second.file->Lock(DatabaseLock::kShared).has_value());
  second.file.reset();
  ASSERT_TRUE(third.file->Lock(DatabaseLock::kShared).has_value());
  EXPECT_TRUE(TakeValue(third.file->HasReservedLock()));
  EXPECT_FALSE(third.file->Lock(DatabaseLock::kReserved).has_value());
  first.file.reset();
  ASSERT_TRUE(third.file->Lock(DatabaseLock::kReserved).has_value());
  ASSERT_TRUE(third.file->Lock(DatabaseLock::kExclusive).has_value());
}

TEST(WindowsVfs, KeepsMappedRegionsStableAcrossGrowthAndRetainsFinalDeleteRequests) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("mapping.db");
  WindowsVfs first_vfs;
  WindowsVfs second_vfs;
  auto first = TakeValue(first_vfs.Open(path, CreateMainDatabaseOptions()));
  auto second = TakeValue(second_vfs.Open(path, OpenMainDatabaseOptions()));
  const auto missing = TakeValue(first.file->MapSharedMemory(
      SharedMemoryRegionIndex{0}, ByteCount{4096}, SharedMemoryMapMode::kExistingOnly));
  EXPECT_FALSE(missing.has_value());
  auto initial = TakeValue(first.file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                       SharedMemoryMapMode::kExtend));
  ASSERT_TRUE(initial.has_value());
  EXPECT_TRUE(std::ranges::all_of(*initial, [](std::byte value) { return value == std::byte{0}; }));
  initial->front() = std::byte{37};
  auto shared = TakeValue(second.file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                       SharedMemoryMapMode::kExistingOnly));
  ASSERT_TRUE(shared.has_value());
  EXPECT_EQ(initial->data(), shared->data());
  EXPECT_EQ(std::byte{37}, shared->front());
  auto grown = TakeValue(first.file->MapSharedMemory(SharedMemoryRegionIndex{2}, ByteCount{4096},
                                                     SharedMemoryMapMode::kExtend));
  ASSERT_TRUE(grown.has_value());
  EXPECT_EQ(std::byte{37}, initial->front());
  EXPECT_EQ(std::byte{0}, grown->front());
  const auto mismatch = first.file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{8192},
                                                    SharedMemoryMapMode::kExtend);
  ASSERT_FALSE(mismatch.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, mismatch.error().code());
  ASSERT_TRUE(second.file->UnmapSharedMemory(SharedMemoryUnmapMode::kDelete).has_value());
  EXPECT_TRUE(TakeValue(first_vfs.Access(path + "-shm", FileAccessQuery::kExists)));
  EXPECT_EQ(std::byte{37}, initial->front());
  ASSERT_TRUE(first.file->UnmapSharedMemory(SharedMemoryUnmapMode::kKeep).has_value());
  EXPECT_FALSE(TakeValue(first_vfs.Access(path + "-shm", FileAccessQuery::kRead)));
}

TEST(WindowsVfs, ReleasesOnlyTheClosingConnectionsSharedAndGroupedExclusiveLocks) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("shared-locks.db");
  WindowsVfs vfs;
  auto first = TakeValue(vfs.Open(path, CreateMainDatabaseOptions()));
  auto second = TakeValue(vfs.Open(path, OpenMainDatabaseOptions()));
  auto third = TakeValue(vfs.Open(path, OpenMainDatabaseOptions()));
  for (auto* file : {first.file.get(), second.file.get(), third.file.get()}) {
    ASSERT_TRUE(file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                      SharedMemoryMapMode::kExtend)
                    .has_value());
  }
  const SharedMemoryLockRange shared{.offset = 1, .count = 1};
  for (auto* file : {first.file.get(), second.file.get()}) {
    ASSERT_TRUE(file->LockSharedMemory(shared, SharedMemoryLockOperation::kLock,
                                       SharedMemoryLockMode::kShared)
                    .has_value());
  }
  EXPECT_FALSE(third.file
                   ->LockSharedMemory(shared, SharedMemoryLockOperation::kLock,
                                      SharedMemoryLockMode::kExclusive)
                   .has_value());
  first.file.reset();
  EXPECT_FALSE(third.file
                   ->LockSharedMemory(shared, SharedMemoryLockOperation::kLock,
                                      SharedMemoryLockMode::kExclusive)
                   .has_value());
  ASSERT_TRUE(second.file->UnmapSharedMemory(SharedMemoryUnmapMode::kKeep).has_value());
  ASSERT_TRUE(third.file
                  ->LockSharedMemory(shared, SharedMemoryLockOperation::kLock,
                                     SharedMemoryLockMode::kExclusive)
                  .has_value());
  const SharedMemoryLockRange grouped{.offset = 2, .count = 3};
  ASSERT_TRUE(third.file
                  ->LockSharedMemory(grouped, SharedMemoryLockOperation::kLock,
                                     SharedMemoryLockMode::kExclusive)
                  .has_value());
  ASSERT_TRUE(second.file
                  ->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                    SharedMemoryMapMode::kExistingOnly)
                  .has_value());
  third.file.reset();
  EXPECT_TRUE(second.file
                  ->LockSharedMemory(grouped, SharedMemoryLockOperation::kLock,
                                     SharedMemoryLockMode::kExclusive)
                  .has_value());
  EXPECT_TRUE(second.file
                  ->LockSharedMemory(grouped, SharedMemoryLockOperation::kUnlock,
                                     SharedMemoryLockMode::kExclusive)
                  .has_value());
}

TEST(WindowsVfs, DoesNotRepublishMappingsAfterFailedFinalCleanup) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("cleanup.db");
  const auto sidecar = directory.NativePath("cleanup.db-shm");
  WindowsVfs vfs;
  auto first = TakeValue(vfs.Open(path, CreateMainDatabaseOptions()));
  ASSERT_TRUE(first.file
                  ->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                    SharedMemoryMapMode::kExtend)
                  .has_value());
  ASSERT_NE(0, ::SetFileAttributesW(sidecar.c_str(), FILE_ATTRIBUTE_READONLY));
  const auto failed = first.file->UnmapSharedMemory(SharedMemoryUnmapMode::kDelete);
  ASSERT_NE(0, ::SetFileAttributesW(sidecar.c_str(), FILE_ATTRIBUTE_NORMAL));
  ASSERT_FALSE(failed.has_value());
  EXPECT_FALSE(first.file
                   ->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                     SharedMemoryMapMode::kExistingOnly)
                   .has_value());
  auto peer = TakeValue(vfs.Open(path, OpenMainDatabaseOptions()));
  EXPECT_FALSE(peer.file
                   ->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                     SharedMemoryMapMode::kExtend)
                   .has_value());
  ASSERT_TRUE(first.file->UnmapSharedMemory(SharedMemoryUnmapMode::kDelete).has_value());
  auto recovered = TakeValue(peer.file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                        SharedMemoryMapMode::kExtend));
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(std::byte{0}, recovered->front());
}

TEST(WindowsVfs, ClearsStaleSharedMemoryOnlyOnTheFirstLiveAttachment) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("stale.db");
  WindowsVfs vfs;
  auto first = TakeValue(vfs.Open(path, CreateMainDatabaseOptions()));
  {
    std::ofstream output{directory.NativePath("stale.db-shm"), std::ios::binary};
    const std::array<char, 4096> stale = [] {
      std::array<char, 4096> result{};
      result.fill('x');
      return result;
    }();
    output.write(stale.data(), static_cast<std::streamsize>(stale.size()));
    ASSERT_TRUE(output.good());
  }
  const auto missing = TakeValue(first.file->MapSharedMemory(
      SharedMemoryRegionIndex{0}, ByteCount{4096}, SharedMemoryMapMode::kExistingOnly));
  EXPECT_FALSE(missing.has_value());
  auto initialized = TakeValue(first.file->MapSharedMemory(
      SharedMemoryRegionIndex{0}, ByteCount{4096}, SharedMemoryMapMode::kExtend));
  ASSERT_TRUE(initialized.has_value());
  EXPECT_EQ(std::byte{0}, initialized->front());
  initialized->front() = std::byte{11};
  auto second = TakeValue(vfs.Open(path, OpenMainDatabaseOptions()));
  auto live = TakeValue(second.file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                     SharedMemoryMapMode::kExtend));
  ASSERT_TRUE(live.has_value());
  EXPECT_EQ(std::byte{11}, live->front());
}

TEST(WindowsVfs, EnforcesPhysicalDatabaseLocksAcrossOwnedProcessesAndReleasesThemOnClose) {
  const TemporaryDirectory directory;
  const auto native = directory.NativePath("child \xE6\x95\xB0\xE6\x8D\xAE.db").native();
  WindowsVfs vfs;
  auto writer = TakeValue(
      vfs.Open(directory.Path("child \xE6\x95\xB0\xE6\x8D\xAE.db"), CreateMainDatabaseOptions()));
  auto reader = TakeValue(
      vfs.Open(directory.Path("child \xE6\x95\xB0\xE6\x8D\xAE.db"), OpenMainDatabaseOptions()));
  ASSERT_TRUE(writer.file->Lock(DatabaseLock::kShared).has_value());
  ASSERT_TRUE(writer.file->Lock(DatabaseLock::kReserved).has_value());
  EXPECT_EQ(10U, Probe({L"raw-lock", native, L"1073741825", L"1", L"exclusive"}));
  EXPECT_EQ(0U, Probe({L"reader", native}));
  ASSERT_TRUE(reader.file->Lock(DatabaseLock::kShared).has_value());
  ASSERT_FALSE(writer.file->Lock(DatabaseLock::kExclusive).has_value());
  EXPECT_EQ(10U, Probe({L"raw-lock", native, L"1073741824", L"1", L"exclusive"}));
  EXPECT_EQ(10U, Probe({L"reader", native}));
  reader.file.reset();
  ASSERT_TRUE(writer.file->Lock(DatabaseLock::kExclusive).has_value());
  EXPECT_EQ(10U, Probe({L"raw-lock", native, L"1073741826", L"510", L"shared"}));
  ASSERT_TRUE(writer.file->Unlock(DatabaseLock::kShared).has_value());
  EXPECT_EQ(0U, Probe({L"reader", native}));
  EXPECT_EQ(0U, Probe({L"raw-lock", native, L"1073741825", L"1", L"exclusive"}));
  ASSERT_TRUE(writer.file->Lock(DatabaseLock::kReserved).has_value());
  writer.file.reset();
  EXPECT_EQ(0U, Probe({L"writer", native}));
}

TEST(WindowsVfs, SharesMappedBytesAndExactPhysicalLockRangesWithOwnedProcesses) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("child-mapping.db");
  const auto native = directory.NativePath("child-mapping.db").native();
  const auto sidecar = directory.NativePath("child-mapping.db-shm").native();
  WindowsVfs vfs;
  auto opened = TakeValue(vfs.Open(path, CreateMainDatabaseOptions()));
  auto mapping = TakeValue(opened.file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                        SharedMemoryMapMode::kExtend));
  ASSERT_TRUE(mapping.has_value());
  mapping->front() = std::byte{37};
  opened.file->SharedMemoryBarrier();
  EXPECT_EQ(0U, Probe({L"shm-observe", native}));
  opened.file->SharedMemoryBarrier();
  EXPECT_EQ(std::byte{91}, mapping->front());
  ASSERT_TRUE(opened.file
                  ->LockSharedMemory({.offset = 1, .count = 1}, SharedMemoryLockOperation::kLock,
                                     SharedMemoryLockMode::kShared)
                  .has_value());
  EXPECT_EQ(10U, Probe({L"shm-exclusive", native}));
  ASSERT_TRUE(opened.file
                  ->LockSharedMemory({.offset = 2, .count = 3}, SharedMemoryLockOperation::kLock,
                                     SharedMemoryLockMode::kExclusive)
                  .has_value());
  for (const auto offset : {L"122", L"123", L"124"}) {
    EXPECT_EQ(10U, Probe({L"raw-lock", sidecar, offset, L"1", L"exclusive"}));
  }
  opened.file.reset();
  EXPECT_EQ(0U, Probe({L"raw-lock", sidecar, L"122", L"3", L"exclusive"}));
}

TEST(WindowsVfs, ReleasesTheDeadmanLockAndClearsStaleBytesAfterAnAbruptChildExit) {
  const TemporaryDirectory directory;
  const std::string path = directory.Path("child-crash.db");
  const auto native = directory.NativePath("child-crash.db").native();
  WindowsVfs vfs;
  auto opened = TakeValue(vfs.Open(path, CreateMainDatabaseOptions()));
  EXPECT_EQ(0U, Probe({L"shm-crash", native}));
  auto missing = TakeValue(opened.file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                        SharedMemoryMapMode::kExistingOnly));
  EXPECT_FALSE(missing.has_value());
  auto mapping = TakeValue(opened.file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                        SharedMemoryMapMode::kExtend));
  ASSERT_TRUE(mapping.has_value());
  EXPECT_EQ(std::byte{0}, mapping->front());
}

TEST(WindowsVfs, FillsRandomnessAndProvidesBoundedSleepAndWallTime) {
  WindowsVfs vfs;
  std::array<std::byte, 256> random{};
  ASSERT_TRUE(vfs.RandomBytes(random).has_value());
  EXPECT_TRUE(std::ranges::any_of(random, [](std::byte value) { return value != std::byte{0}; }));
  constexpr std::chrono::microseconds duration{1500};
  EXPECT_GE(TakeValue(vfs.SleepFor(duration)), duration);
  const auto invalid = vfs.SleepFor(std::chrono::microseconds::max());
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, invalid.error().code());
  const auto before = std::chrono::system_clock::now();
  const auto current = TakeValue(vfs.CurrentTime());
  const auto after = std::chrono::system_clock::now();
  EXPECT_GE(current, std::chrono::time_point_cast<std::chrono::milliseconds>(before));
  EXPECT_LE(current, after);
}

}  // namespace
}  // namespace modern_sqlite
