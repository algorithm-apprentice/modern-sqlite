#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::size_t failing_allocation = 0;
bool inject_failure = false;
bool fail_all_allocations = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (fail_all_allocations || (inject_failure && index == failing_allocation)) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (fail_all_allocations || (inject_failure && index == failing_allocation)) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
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

[[nodiscard]] modern_sqlite::ByteBuffer EncodedRecord(std::int64_t key) {
  using namespace modern_sqlite;
  std::array<SqlValue, 2> fields{
      SqlValue::Integer(key),
      SqlValue::Integer(key),
  };
  return TakeValue(EncodeRecord(fields));
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

  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                    .sorter_memory_threshold = ByteCount{1U << 20U},
                                                }));
  const RecordSorterDescriptor descriptor = Descriptor();

  allocation_index.store(0, std::memory_order_relaxed);
  const Result<RecordSorter> baseline = factory.CreateRecordSorter(descriptor);
  if (!baseline.has_value()) {
    return 1;
  }
  const std::size_t create_allocations = allocation_index.load(std::memory_order_relaxed);
  if (create_allocations == 0U || create_allocations > 4U) {
    return 1;
  }

  for (std::size_t failure = 0; failure < create_allocations; ++failure) {
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    const Result<RecordSorter> rejected = factory.CreateRecordSorter(descriptor);
    inject_failure = false;
    if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory) {
      return 1;
    }
  }

  RecordSorter sorter = TakeValue(factory.CreateRecordSorter(descriptor));
  ByteBuffer failed_record = EncodedRecord(5);
  allocation_index.store(0, std::memory_order_relaxed);
  failing_allocation = 0;
  inject_failure = true;
  const Status failed_insert = sorter.Insert(std::move(failed_record));
  inject_failure = false;
  if (failed_insert.has_value() || failed_insert.error().code() != ErrorCode::kOutOfMemory ||
      sorter.record_count() != 0U || sorter.memory_usage() != ByteCount{0}) {
    return 1;
  }

  constexpr std::size_t kRecordCount = 32;
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    const Status inserted =
        sorter.Insert(EncodedRecord(static_cast<std::int64_t>(kRecordCount - index)));
    if (!inserted.has_value()) {
      return 1;
    }
  }

  fail_all_allocations = true;
  const Status rewound = sorter.Rewind();
  if (!rewound.has_value()) {
    fail_all_allocations = false;
    return 1;
  }
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    const Result<RecordView> current = sorter.current_record();
    if (!current.has_value()) {
      fail_all_allocations = false;
      return 1;
    }
    const Result<RecordFieldView> field = current->field(0);
    if (!field.has_value() || field->integer_value() != static_cast<std::int64_t>(index + 1U)) {
      fail_all_allocations = false;
      return 1;
    }
    const Result<bool> next = sorter.Next();
    if (!next.has_value() || *next != (index + 1U < kRecordCount)) {
      fail_all_allocations = false;
      return 1;
    }
  }
  sorter.Close();
  fail_all_allocations = false;
  return 0;
} catch (...) {
  inject_failure = false;
  fail_all_allocations = false;
  return 1;
}
