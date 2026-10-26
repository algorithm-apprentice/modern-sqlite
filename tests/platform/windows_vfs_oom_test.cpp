#include <windows.h>

#include <winternl.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "modern_sqlite/platform/windows_vfs.hpp"
#include "tests/support/aligned_allocation.hpp"
#include "tests/support/windows_process.hpp"
#include "tests/support/windows_temporary_directory.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::optional<std::size_t> failing_allocation;

using HandleSnapshot = std::array<bool, 4096>;

[[nodiscard]] HandleSnapshot CaptureHandles() noexcept {
  HandleSnapshot handles{};
  for (std::size_t index = 1; index < handles.size(); ++index) {
    DWORD flags = 0;
    handles[index] =
        ::GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(index * 4U)),
                               &flags) != 0;
  }
  return handles;
}

void ReportAddedHandles(const HandleSnapshot& before) {
  const auto query = std::bit_cast<decltype(&::NtQueryObject)>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
  if (query == nullptr) {
    std::fputs("native handle-type diagnostic service is unavailable\n", stderr);
    return;
  }
  const auto after = CaptureHandles();
  for (std::size_t index = 1; index < before.size(); ++index) {
    if (before[index] || !after[index]) {
      continue;
    }
    struct TypeStorage {
      PUBLIC_OBJECT_TYPE_INFORMATION information{};
      std::array<std::byte, 1024> trailing{};
    };
    TypeStorage storage{};
    ULONG length = 0;
    const NTSTATUS status =
        query(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(index * 4U)),
              ObjectTypeInformation, &storage, static_cast<ULONG>(sizeof(storage)), &length);
    if (status >= 0 && storage.information.TypeName.Buffer != nullptr &&
        storage.information.TypeName.Length % sizeof(wchar_t) == 0) {
      const auto& name = storage.information.TypeName;
      std::fprintf(stderr, "added native handle %zu type %.*ls\n", index * 4U,
                   static_cast<int>(name.Length / sizeof(wchar_t)), name.Buffer);
    } else {
      std::fprintf(stderr, "added native handle %zu type query unavailable or invalid: %ld\n",
                   index * 4U, status);
    }
  }
}

[[nodiscard]] void* Allocate(std::size_t size) {
  const auto index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation == index) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const auto index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation == index) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (modern_sqlite::test_support::AllocateAligned(&memory, alignment,
                                                   size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

void Arm(std::optional<std::size_t> failure) noexcept {
  allocation_index.store(0, std::memory_order_relaxed);
  failing_allocation = failure;
}

[[nodiscard]] std::size_t Disarm() noexcept {
  failing_allocation.reset();
  return allocation_index.load(std::memory_order_relaxed);
}

template <typename T>
[[nodiscard]] bool MatchesAllocationResult(const modern_sqlite::Result<T>& result,
                                           std::optional<std::size_t> failure) {
  if (failure.has_value()) {
    if (!result.has_value() && result.error().code() == modern_sqlite::ErrorCode::kOutOfMemory) {
      return true;
    }
    std::fputs("allocation failure was not returned as a typed out-of-memory error\n", stderr);
    return false;
  }
  if (!result.has_value()) {
    std::fprintf(stderr, "uninjected operation failed: %s\n", result.error().ToString().c_str());
    return false;
  }
  return true;
}

[[nodiscard]] int RunCase(std::string_view mode, std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  const test_support::WindowsTemporaryDirectory directory;
  const std::string path = directory.Path("allocation.db");
  WindowsVfs vfs;
  // Initialize CNG's provider and notification wait before measuring owned handles.
  ByteBuffer entropy{ByteCount{15}};
  for (int request = 0; request < 2; ++request) {
    if (!vfs.RandomBytes(entropy.mutable_view()).has_value()) {
      std::fputs("could not initialize the native randomness service\n", stderr);
      return 1;
    }
  }
  DWORD synchronization_before = 0;
  DWORD synchronization_after = 0;
  if (::GetProcessHandleCount(::GetCurrentProcess(), &synchronization_before) == 0) {
    std::fputs("could not capture the synchronization-service baseline\n", stderr);
    return 1;
  }
  std::mutex synchronization_service;
  {
    const std::lock_guard initialize{synchronization_service};
  }
  if (::GetProcessHandleCount(::GetCurrentProcess(), &synchronization_after) == 0) {
    std::fputs("could not capture initialized synchronization-service handles\n", stderr);
    return 1;
  }
  const HandleSnapshot handle_snapshot = CaptureHandles();
  DWORD before = 0;
  if (::GetProcessHandleCount(::GetCurrentProcess(), &before) == 0) {
    std::fputs("could not capture the initial native handle count\n", stderr);
    return 1;
  }
  std::size_t allocations = 0;
  {
    FileOpenOptions options{
        .access = FileAccessMode::kReadWrite,
        .create = true,
        .exclusive_create = true,
    };
    if (mode == "open" || mode == "temporary") {
      const bool temporary = mode == "temporary";
      if (temporary) {
        options.kind = FileKind::kTemporaryDatabase;
        options.delete_on_close = true;
      }
      Arm(failure);
      auto opened =
          vfs.Open(temporary ? std::nullopt : std::optional<std::string_view>{path}, options);
      allocations = Disarm();
      if (!MatchesAllocationResult(opened, failure)) {
        return 1;
      }
      if (opened.has_value()) {
        opened->file.reset();
      }
      options.exclusive_create = temporary;
      auto recovered =
          vfs.Open(temporary ? std::nullopt : std::optional<std::string_view>{path}, options);
      if (!recovered.has_value()) {
        std::fprintf(stderr, "the backend could not reopen after allocation failure: %s\n",
                     recovered.error().ToString().c_str());
        return 1;
      }
    } else if (mode == "mapping" || mode == "growth") {
      auto opened = vfs.Open(path, options);
      if (!opened.has_value()) {
        std::fputs("could not prepare the mapping allocation fixture\n", stderr);
        return 1;
      }
      const bool growth = mode == "growth";
      std::optional<MutableByteView> retained;
      if (growth) {
        auto initial = opened->file->MapSharedMemory(SharedMemoryRegionIndex{0}, ByteCount{4096},
                                                     SharedMemoryMapMode::kExtend);
        if (!initial.has_value() || !initial->has_value()) {
          std::fputs("could not prepare the existing mapped view\n", stderr);
          return 1;
        }
        retained = *initial;
        retained->front() = std::byte{37};
      }
      const SharedMemoryRegionIndex region{growth ? 2U : 0U};
      Arm(failure);
      auto mapped =
          opened->file->MapSharedMemory(region, ByteCount{4096}, SharedMemoryMapMode::kExtend);
      allocations = Disarm();
      if (!MatchesAllocationResult(mapped, failure)) {
        return 1;
      }
      if (retained.has_value() && retained->front() != std::byte{37}) {
        std::fputs("allocation failure invalidated a previously published view\n", stderr);
        return 1;
      }
      auto recovered =
          opened->file->MapSharedMemory(region, ByteCount{4096}, SharedMemoryMapMode::kExtend);
      if (!recovered.has_value() || !recovered->has_value() ||
          recovered->value().front() != std::byte{0}) {
        std::fputs("mapping allocation failure did not preserve retryable zero-filled growth\n",
                   stderr);
        return 1;
      }
      if (!opened->file->UnmapSharedMemory(SharedMemoryUnmapMode::kDelete).has_value()) {
        std::fputs("could not clean up the mapping allocation fixture\n", stderr);
        return 1;
      }
    } else {
      std::fputs("unknown native allocation operation\n", stderr);
      return 1;
    }
  }
  DWORD after = 0;
  const BOOL counted = ::GetProcessHandleCount(::GetCurrentProcess(), &after);
  if (mode == "mapping" && !failure.has_value()) {
    std::fprintf(stderr, "independent C++ synchronization-service handles: %lu -> %lu\n",
                 synchronization_before, synchronization_after);
  }
  if (counted == 0 || before != after) {
    std::fprintf(stderr, "native allocation cleanup handle-count mismatch: %lu -> %lu\n", before,
                 after);
    ReportAddedHandles(handle_snapshot);
    return 1;
  }
  return failure.has_value() ? 0 : static_cast<int>(1000 + allocations);
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}

int main(int argc, char* argv[]) {
  try {
    if (argc == 2 && std::string_view{argv[1]} == "--verify-handle-diagnostics") {
      const auto before = CaptureHandles();
      const auto event = modern_sqlite::test_support::OwnWindowsHandle(
          ::CreateEventW(nullptr, TRUE, FALSE, nullptr));
      const auto section = modern_sqlite::test_support::OwnWindowsHandle(
          ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr));
      if (!event || !section) {
        std::fputs("could not prepare the native handle-type diagnostic control\n", stderr);
        return 1;
      }
      ReportAddedHandles(before);
      return 0;
    }
    if (argc == 3) {
      const std::optional<std::size_t> failure =
          std::string_view{argv[2]} == "measure" ? std::nullopt
                                                 : std::optional<std::size_t>{std::stoull(argv[2])};
      return RunCase(argv[1], failure);
    }
    if (argc != 1) {
      std::fputs("native allocation test received unexpected arguments\n", stderr);
      return 1;
    }
    const auto executable = modern_sqlite::test_support::WindowsProcessExecutable();
    for (const auto* mode : {L"open", L"temporary", L"mapping", L"growth"}) {
      const DWORD measured =
          modern_sqlite::test_support::RunWindowsProcess(executable, {mode, L"measure"});
      if (measured <= 1000 || measured > 1512) {
        std::fprintf(stderr, "native %ls allocation baseline failed: %lu\n", mode, measured);
        return 1;
      }
      const DWORD allocations = measured - 1000;
      for (DWORD failure = 0; failure < allocations; ++failure) {
        const DWORD result = modern_sqlite::test_support::RunWindowsProcess(
            executable, {mode, std::to_wstring(failure)});
        if (result != 0) {
          std::fprintf(stderr, "native %ls allocation prefix failed: %lu, exit %lu\n", mode,
                       failure, result);
          return 1;
        }
      }
      std::printf("Qualified %lu cold-process allocation prefixes\n", allocations);
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "native allocation test: %s\n", error.what());
    return 1;
  }
}
