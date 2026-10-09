#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <utility>

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

[[nodiscard]] modern_sqlite::ByteBuffer Key(std::int64_t key) {
  using namespace modern_sqlite;
  const std::array<SqlValue, 1> values{SqlValue::Integer(key)};
  return TakeValue(EncodeRecord(values));
}

[[nodiscard]] modern_sqlite::ByteBuffer Row(std::int64_t key) {
  using namespace modern_sqlite;
  const std::array<SqlValue, 2> values{
      SqlValue::Integer(key),
      SqlValue::Integer(key),
  };
  return TakeValue(EncodeRecord(values));
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
  const Result<BoundedTopN> baseline = factory.CreateTopN(descriptor, 32);
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
    const Result<BoundedTopN> rejected = factory.CreateTopN(descriptor, 32);
    inject_failure = false;
    if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory) {
      return 1;
    }
  }

  BoundedTopN insertion = TakeValue(factory.CreateTopN(descriptor, 1));
  ByteBuffer key = Key(1);
  fail_all_allocations = true;
  const auto checked = insertion.CheckCandidate(std::move(key));
  fail_all_allocations = false;
  if (!checked.has_value() || *checked != TopNCheckResult::kAccepted ||
      !insertion.has_pending_candidate()) {
    return 1;
  }
  ByteBuffer row = Row(1);
  failing_allocation = 0;
  allocation_index.store(0, std::memory_order_relaxed);
  inject_failure = true;
  const Status failed_insert = insertion.Insert(std::move(row));
  inject_failure = false;
  if (failed_insert.has_value() || failed_insert.error().code() != ErrorCode::kOutOfMemory ||
      insertion.valid()) {
    return 1;
  }

  BoundedTopN iteration = TakeValue(factory.CreateTopN(descriptor, 32));
  for (std::int64_t key_value = 32; key_value >= 1; --key_value) {
    if (TakeValue(iteration.CheckCandidate(Key(key_value))) != TopNCheckResult::kAccepted ||
        !iteration.Insert(Row(key_value)).has_value()) {
      return 1;
    }
  }
  fail_all_allocations = true;
  if (!iteration.Rewind().has_value()) {
    fail_all_allocations = false;
    return 1;
  }
  for (std::int64_t expected = 1; expected <= 32; ++expected) {
    const Result<RecordView> current = iteration.current_record();
    if (!current.has_value() || current->field(0)->integer_value().value_or(-1) != expected) {
      fail_all_allocations = false;
      return 1;
    }
    const Result<bool> next = iteration.Next();
    if (!next.has_value() || *next != (expected < 32)) {
      fail_all_allocations = false;
      return 1;
    }
  }
  iteration.Close();
  fail_all_allocations = false;
  return 0;
} catch (...) {
  inject_failure = false;
  fail_all_allocations = false;
  return 1;
}
