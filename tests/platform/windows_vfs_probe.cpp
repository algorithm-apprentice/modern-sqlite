#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#include "modern_sqlite/platform/windows_vfs.hpp"
#include "tests/support/windows_handle.hpp"

namespace {

[[nodiscard]] int Report(const modern_sqlite::Error& error) {
  std::fprintf(stderr, "Windows VFS probe: %s\n", error.ToString().c_str());
  return error.code() == modern_sqlite::ErrorCode::kBusy ? 10 : 1;
}

[[nodiscard]] int RawLock(int argc, wchar_t* argv[]) {
  if (argc != 6) {
    std::fputs("raw lock probe requires path, offset, size and mode\n", stderr);
    return 1;
  }
  const auto offset = std::stoul(argv[3]);
  const auto length = std::stoul(argv[4]);
  const std::wstring_view mode{argv[5]};
  if (offset > std::numeric_limits<DWORD>::max() || length > std::numeric_limits<DWORD>::max() ||
      (mode != L"shared" && mode != L"exclusive")) {
    std::fputs("raw lock probe received an invalid range or mode\n", stderr);
    return 1;
  }
  const auto handle = modern_sqlite::test_support::OwnWindowsHandle(
      ::CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle) {
    std::fprintf(stderr, "raw lock probe open failed: %lu\n", ::GetLastError());
    return 1;
  }
  OVERLAPPED position{};
  position.Offset = static_cast<DWORD>(offset);
  const DWORD flags =
      LOCKFILE_FAIL_IMMEDIATELY | (mode == L"exclusive" ? LOCKFILE_EXCLUSIVE_LOCK : 0U);
  if (::LockFileEx(handle.get(), flags, 0, static_cast<DWORD>(length), 0, &position) != 0) {
    return 0;
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_LOCK_VIOLATION) {
    return 10;
  }
  std::fprintf(stderr, "raw lock probe lock failed: %lu\n", error);
  return 1;
}

[[nodiscard]] int Probe(int argc, wchar_t* argv[]) {
  if (argc < 3) {
    std::fputs("Windows VFS probe requires a mode and database path\n", stderr);
    return 1;
  }
  const std::wstring_view mode{argv[1]};
  if (mode == L"raw-lock") {
    return RawLock(argc, argv);
  }
  const auto utf8 = std::filesystem::path{argv[2]}.u8string();
  const std::string path{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
  modern_sqlite::WindowsVfs vfs;
  auto opened = vfs.Open(
      path, modern_sqlite::FileOpenOptions{.access = modern_sqlite::FileAccessMode::kReadWrite});
  if (!opened.has_value()) {
    return Report(opened.error());
  }
  auto& file = *opened->file;
  if (mode == L"reader" || mode == L"writer") {
    auto locked = file.Lock(modern_sqlite::DatabaseLock::kShared);
    if (!locked.has_value()) {
      return Report(locked.error());
    }
    if (mode == L"writer") {
      locked = file.Lock(modern_sqlite::DatabaseLock::kReserved);
      if (!locked.has_value()) {
        return Report(locked.error());
      }
      locked = file.Lock(modern_sqlite::DatabaseLock::kExclusive);
    }
    return locked.has_value() ? 0 : Report(locked.error());
  }
  const bool crash = mode == L"shm-crash";
  auto mapping = file.MapSharedMemory(modern_sqlite::SharedMemoryRegionIndex{0},
                                      modern_sqlite::ByteCount{4096},
                                      crash ? modern_sqlite::SharedMemoryMapMode::kExtend
                                            : modern_sqlite::SharedMemoryMapMode::kExistingOnly);
  if (!mapping.has_value()) {
    return Report(mapping.error());
  }
  if (!mapping->has_value()) {
    std::fputs("Windows VFS probe could not observe the shared mapping\n", stderr);
    return 1;
  }
  if (crash) {
    mapping->value().front() = std::byte{99};
    file.SharedMemoryBarrier();
    if (::TerminateProcess(::GetCurrentProcess(), 0) == 0) {
      std::fprintf(stderr, "could not abruptly exit the probe: %lu\n", ::GetLastError());
    }
    return 1;
  }
  if (mode == L"shm-observe") {
    file.SharedMemoryBarrier();
    if (mapping->value().front() != std::byte{37}) {
      std::fputs("Windows VFS probe observed incorrect shared bytes\n", stderr);
      return 1;
    }
    mapping->value().front() = std::byte{91};
    file.SharedMemoryBarrier();
    return 0;
  }
  if (mode == L"shm-exclusive") {
    const auto locked = file.LockSharedMemory({.offset = 1, .count = 1},
                                              modern_sqlite::SharedMemoryLockOperation::kLock,
                                              modern_sqlite::SharedMemoryLockMode::kExclusive);
    return locked.has_value() ? 0 : Report(locked.error());
  }
  std::fputs("Windows VFS probe received an unknown mode\n", stderr);
  return 1;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  try {
    return Probe(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Windows VFS probe: %s\n", error.what());
    return 1;
  }
}
