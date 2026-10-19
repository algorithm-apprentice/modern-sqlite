#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

std::atomic<std::size_t> allocation_count = 0;
bool count_allocations = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  if (count_allocations) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  if (count_allocations) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0U ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

template <typename T>
[[nodiscard]] T TakeValue(modern_sqlite::Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

[[nodiscard]] modern_sqlite::RecordSorterDescriptor Descriptor() {
  using namespace modern_sqlite;
  return RecordSorterDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              IndexColumnOrder{BinaryCollation()},
          },
  };
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
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

int main() try {
  using namespace modern_sqlite;
  constexpr std::size_t kRecordCount = 65536;

  test::WritePagerFixedVfs memory_vfs;
  const std::unique_ptr<Pager> memory_pager =
      TakeValue(Pager::Open(memory_vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory memory_factory = TakeValue(
      TemporaryStorageFactory::Create(memory_vfs, *memory_pager,
                                      TemporaryStorageOptions{
                                          .mode = TemporaryStoreMode::kMemory,
                                          .sorter_memory_threshold = ByteCount{16U << 20U},
                                      }));
  StatementTemporaryStorage memory_storage = TakeValue(memory_factory.CreateStatementStorage());
  RecordSorter memory_sorter = TakeValue(memory_storage.CreateRecordSorter(Descriptor()));

  allocation_count.store(0U, std::memory_order_relaxed);
  count_allocations = true;
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    const std::array<SqlValue, 2> values{
        SqlValue::Integer(static_cast<std::int64_t>(kRecordCount - index)),
        SqlValue::Integer(static_cast<std::int64_t>(index)),
    };
    if (!memory_sorter.Insert(values).has_value()) {
      count_allocations = false;
      return 1;
    }
  }
  count_allocations = false;
  if (allocation_count.load(std::memory_order_relaxed) != kRecordCount ||
      memory_sorter.record_count() != kRecordCount || memory_sorter.has_spilled()) {
    return 1;
  }
  memory_sorter.Close();

  test::WritePagerFixedVfs file_vfs;
  const std::unique_ptr<Pager> file_pager =
      TakeValue(Pager::Open(file_vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory file_factory =
      TakeValue(TemporaryStorageFactory::Create(file_vfs, *file_pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{4U << 20U},
                                                }));
  StatementTemporaryStorage file_storage = TakeValue(file_factory.CreateStatementStorage());
  RecordSorter file_sorter = TakeValue(file_storage.CreateRecordSorter(Descriptor()));

  allocation_count.store(0U, std::memory_order_relaxed);
  count_allocations = true;
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    const std::array<SqlValue, 2> values{
        SqlValue::Integer(static_cast<std::int64_t>(kRecordCount - index)),
        SqlValue::Integer(static_cast<std::int64_t>(index)),
    };
    if (!file_sorter.Insert(values).has_value()) {
      count_allocations = false;
      return 1;
    }
  }
  count_allocations = false;
  const std::size_t file_allocations = allocation_count.load(std::memory_order_relaxed);
  if (file_allocations == 0U || file_allocations > 32U ||
      file_sorter.record_count() != kRecordCount || file_sorter.has_spilled() ||
      file_vfs.pathless_open_count() != 0U) {
    return 1;
  }
  return 0;
} catch (...) {
  count_allocations = false;
  return 1;
}
