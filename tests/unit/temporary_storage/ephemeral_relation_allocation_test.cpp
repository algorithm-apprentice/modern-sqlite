#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

std::atomic<std::size_t> allocation_count = 0;
bool count_allocations = false;
bool fail_allocations = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  if (count_allocations) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  if (count_allocations) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
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

void RequireStatus(modern_sqlite::Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

[[nodiscard]] modern_sqlite::EphemeralRelationDescriptor Descriptor() {
  using namespace modern_sqlite;
  return EphemeralRelationDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              IndexColumnOrder{BinaryCollation()},
          },
  };
}

[[nodiscard]] modern_sqlite::ByteBuffer Row(std::int64_t key) {
  using namespace modern_sqlite;
  const std::array<SqlValue, 2> values{
      SqlValue::Integer(key),
      SqlValue::Integer(key * 10),
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
  const TemporaryStorageFactory memory_factory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));

  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  Result<EphemeralRelation> memory_created = memory_factory.CreateEphemeralRelation(Descriptor());
  count_allocations = false;
  if (!memory_created.has_value()) {
    return 1;
  }
  const std::size_t memory_create_allocations = allocation_count.load(std::memory_order_relaxed);
  if (memory_create_allocations == 0U || memory_create_allocations > 8U) {
    return 1;
  }
  EphemeralRelation memory = std::move(*memory_created);

  ByteBuffer first = Row(1);
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  Result<EphemeralInsertResult> inserted =
      memory.Insert(std::move(first), EphemeralInsertMode::kKeepExisting);
  count_allocations = false;
  if (!inserted.has_value() || *inserted != EphemeralInsertResult::kInserted) {
    return 1;
  }
  const std::size_t memory_insert_allocations = allocation_count.load(std::memory_order_relaxed);
  if (memory_insert_allocations == 0U || memory_insert_allocations > 4U) {
    return 1;
  }

  ByteBuffer duplicate = Row(1);
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  Result<EphemeralInsertResult> kept =
      memory.Insert(std::move(duplicate), EphemeralInsertMode::kKeepExisting);
  count_allocations = false;
  if (!kept.has_value() || *kept != EphemeralInsertResult::kDuplicate ||
      allocation_count.load(std::memory_order_relaxed) != 0U) {
    return 1;
  }

  ByteBuffer second = Row(2);
  if (!memory.Insert(std::move(second), EphemeralInsertMode::kKeepExisting).has_value()) {
    return 1;
  }
  ByteBuffer replacement = Row(1);
  const ByteBuffer first_key =
      TakeValue(EncodeRecord(std::array<SqlValue, 1>{SqlValue::Integer(1)}));
  const ByteBuffer second_key =
      TakeValue(EncodeRecord(std::array<SqlValue, 1>{SqlValue::Integer(2)}));
  fail_allocations = true;
  const Result<EphemeralInsertResult> replaced =
      memory.Insert(std::move(replacement), EphemeralInsertMode::kReplaceExisting);
  const Result<bool> contained = memory.Contains(first_key.view());
  const Result<bool> erased = memory.Erase(second_key.view());
  fail_allocations = false;
  if (!replaced.has_value() || *replaced != EphemeralInsertResult::kReplaced ||
      !contained.has_value() || !*contained || !erased.has_value() || !*erased) {
    return 1;
  }
  if (!memory.Insert(Row(2), EphemeralInsertMode::kKeepExisting).has_value()) {
    return 1;
  }
  RequireStatus(memory.Rewind());

  std::uint64_t checksum = 0;
  fail_allocations = true;
  checksum += memory.valid() ? 1U : 0U;
  checksum += static_cast<std::uint64_t>(memory.state());
  checksum += memory.record_count();
  checksum += memory.file_backed() ? 1U : 0U;
  const Result<RecordView> current = memory.current_record();
  if (!current.has_value()) {
    fail_allocations = false;
    return 1;
  }
  checksum += current->field_count();
  const Result<bool> advanced = memory.Next();
  if (!advanced.has_value() || !*advanced) {
    fail_allocations = false;
    return 1;
  }
  RequireStatus(memory.Reset());
  checksum += memory.record_count();
  memory.Close();
  fail_allocations = false;

  test::WritePagerFixedVfs file_vfs;
  const std::unique_ptr<Pager> file_pager =
      TakeValue(Pager::Open(file_vfs, test::kWritePagerInputPath,
                            PagerOptions{
                                .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                                .cache_capacity_pages = 1,
                            }));
  RequireStatus(file_pager->BeginRead());
  const TemporaryStorageFactory file_factory = TakeValue(TemporaryStorageFactory::Create(
      file_vfs, *file_pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kFile}));

  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  Result<EphemeralRelation> file_created = file_factory.CreateEphemeralRelation(Descriptor());
  count_allocations = false;
  if (!file_created.has_value()) {
    return 1;
  }
  const std::size_t file_create_allocations = allocation_count.load(std::memory_order_relaxed);
  if (file_create_allocations == 0U || file_create_allocations > 128U) {
    return 1;
  }
  EphemeralRelation file = std::move(*file_created);
  ByteBuffer file_row = Row(1);
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  const Result<EphemeralInsertResult> file_inserted =
      file.Insert(std::move(file_row), EphemeralInsertMode::kKeepExisting);
  count_allocations = false;
  if (!file_inserted.has_value() || *file_inserted != EphemeralInsertResult::kInserted) {
    return 1;
  }
  const std::size_t file_insert_allocations = allocation_count.load(std::memory_order_relaxed);
  if (file_insert_allocations == 0U || file_insert_allocations > 128U) {
    return 1;
  }
  RequireStatus(file.Rewind());

  fail_allocations = true;
  checksum += file.valid() ? 1U : 0U;
  checksum += static_cast<std::uint64_t>(file.state());
  checksum += file.record_count();
  checksum += file.file_backed() ? 1U : 0U;
  const Result<RecordView> file_current = file.current_record();
  if (!file_current.has_value()) {
    fail_allocations = false;
    return 1;
  }
  checksum += file_current->field_count();
  file.Close();
  fail_allocations = false;

  return checksum == 0U || file_vfs.pathless_file_present() ? 1 : 0;
} catch (...) {
  count_allocations = false;
  fail_allocations = false;
  return 1;
}
