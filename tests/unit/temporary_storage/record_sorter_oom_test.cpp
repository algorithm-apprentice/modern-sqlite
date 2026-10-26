#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>
#include <ranges>
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
#include "tests/support/aligned_allocation.hpp"
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
  if (modern_sqlite::test_support::AllocateAligned(&memory, alignment,
                                                   size == 0 ? alignment : size) == 0) {
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

[[nodiscard]] modern_sqlite::ByteBuffer EncodedBlobRecord(std::int64_t key,
                                                          std::size_t payload_size = 900U) {
  using namespace modern_sqlite;
  ByteBuffer payload{ByteCount{payload_size}};
  std::ranges::fill(payload.mutable_view(), std::byte{0x5a});
  std::array<SqlValue, 2> fields{
      SqlValue::Integer(key),
      SqlValue::Blob(std::move(payload)),
  };
  return TakeValue(EncodeRecord(fields));
}

struct Outcome {
  std::size_t allocations = 0;
  bool succeeded = false;
  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool cleaned = false;
};

[[nodiscard]] Outcome RunFileInsert(bool reuse, std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  inject_failure = false;
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{128},
                                                }));
  std::optional<StatementTemporaryStorage> storage;
  storage.emplace(TakeValue(factory.CreateStatementStorage()));
  if (reuse) {
    RecordSorter first = TakeValue(storage->CreateRecordSorter(Descriptor()));
    if (!first.Insert(EncodedBlobRecord(1)).has_value()) {
      return {};
    }
    first.Close();
  }
  RecordSorter sorter = TakeValue(storage->CreateRecordSorter(Descriptor()));
  ByteBuffer record = EncodedBlobRecord(2);

  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  const Status inserted = sorter.Insert(std::move(record));
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  const bool succeeded = inserted.has_value();
  const ErrorCode error = succeeded ? ErrorCode::kGeneric : inserted.error().code();
  if (sorter.valid()) {
    sorter.Close();
  }
  const bool released = storage->retained_bytes() == ByteCount{0};
  storage.reset();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .cleaned = released && !vfs.pathless_file_present(),
  };
}

[[nodiscard]] Outcome RunFileMerge(std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  inject_failure = false;
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{1024},
                                                }));
  std::optional<StatementTemporaryStorage> storage;
  storage.emplace(TakeValue(factory.CreateStatementStorage()));
  RecordSorter sorter = TakeValue(storage->CreateRecordSorter(Descriptor()));
  constexpr std::int64_t kRecordCount = 20;
  for (std::int64_t key = kRecordCount; key > 0; --key) {
    if (!sorter.Insert(EncodedBlobRecord(key, 1100U)).has_value()) {
      return {};
    }
  }
  if (sorter.spilled_run_count() <= 16U) {
    return {};
  }

  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  const Status rewound = sorter.Rewind();
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  const bool succeeded = rewound.has_value();
  const ErrorCode error = succeeded ? ErrorCode::kGeneric : rewound.error().code();
  bool positioned = !succeeded;
  if (succeeded) {
    const auto record = sorter.current_record();
    if (record.has_value()) {
      const auto key = record->field(0);
      positioned = key.has_value() && key->integer_value() == 1;
    }
  }
  sorter.Close();
  const bool released = storage->retained_bytes() == ByteCount{0};
  storage.reset();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .cleaned = positioned && released && !vfs.pathless_file_present(),
  };
}

template <typename Runner>
[[nodiscard]] bool ExhaustAllocations(Runner&& runner) {
  const Outcome baseline = runner(std::nullopt);
  if (!baseline.succeeded || !baseline.cleaned || baseline.allocations == 0U) {
    return false;
  }
  for (std::size_t failure = 0; failure < baseline.allocations; ++failure) {
    const Outcome outcome = runner(failure);
    if (outcome.succeeded || outcome.error != modern_sqlite::ErrorCode::kOutOfMemory ||
        !outcome.cleaned) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool DetachedRunAllocationPrecedesIo() {
  using namespace modern_sqlite;

  {
    test::WritePagerFixedVfs vfs;
    const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
    const TemporaryStorageFactory factory =
        TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                  TemporaryStorageOptions{
                                                      .mode = TemporaryStoreMode::kFile,
                                                      .sorter_memory_threshold = ByteCount{128},
                                                  }));
    StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
    RecordSorter sorter = TakeValue(storage.CreateRecordSorter(Descriptor()));
    ByteBuffer record = EncodedBlobRecord(1);

    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    inject_failure = true;
    const Status rejected = sorter.Insert(std::move(record));
    inject_failure = false;
    if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory ||
        sorter.valid() || vfs.pathless_open_count() != 0U || vfs.total_subjournal_writes() != 0U ||
        storage.retained_bytes() != ByteCount{0}) {
      return false;
    }
  }

  {
    test::WritePagerFixedVfs vfs;
    const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
    const TemporaryStorageFactory factory =
        TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                  TemporaryStorageOptions{
                                                      .mode = TemporaryStoreMode::kFile,
                                                      .sorter_memory_threshold = ByteCount{128},
                                                  }));
    StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
    RecordSorter sorter = TakeValue(storage.CreateRecordSorter(Descriptor()));
    if (!sorter.Insert(EncodedRecord(1)).has_value() ||
        !sorter.Insert(EncodedRecord(2)).has_value()) {
      return false;
    }
    ByteBuffer record = EncodedRecord(3);

    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    inject_failure = true;
    const Status rejected = sorter.Insert(std::move(record));
    inject_failure = false;
    if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory ||
        sorter.valid() || vfs.pathless_open_count() != 0U || vfs.total_subjournal_writes() != 0U ||
        storage.retained_bytes() != ByteCount{0}) {
      return false;
    }
  }

  {
    test::WritePagerFixedVfs vfs;
    const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
    const TemporaryStorageFactory factory =
        TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                  TemporaryStorageOptions{
                                                      .mode = TemporaryStoreMode::kFile,
                                                      .sorter_memory_threshold = ByteCount{1024},
                                                  }));
    std::optional<StatementTemporaryStorage> storage;
    storage.emplace(TakeValue(factory.CreateStatementStorage()));
    RecordSorter sorter = TakeValue(storage->CreateRecordSorter(Descriptor()));
    for (std::int64_t key = 20; key > 0; --key) {
      if (!sorter.Insert(EncodedBlobRecord(key, 1100U)).has_value()) {
        return false;
      }
    }
    if (sorter.spilled_run_count() <= 16U) {
      return false;
    }
    const std::size_t opens_before = vfs.pathless_open_count();
    const std::size_t writes_before = vfs.total_subjournal_writes();
    const std::vector<std::byte> bytes_before(vfs.pathless_file_bytes().begin(),
                                              vfs.pathless_file_bytes().end());

    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    inject_failure = true;
    const Status rejected = sorter.Rewind();
    inject_failure = false;
    if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory ||
        sorter.valid() || storage->failed() || vfs.pathless_open_count() != opens_before ||
        vfs.total_subjournal_writes() != writes_before ||
        !std::ranges::equal(bytes_before, vfs.pathless_file_bytes())) {
      return false;
    }
    storage.reset();
    if (vfs.pathless_file_present()) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] bool BulkGrowthFailurePreservesPublishedPrefix() {
  using namespace modern_sqlite;
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  if (!pager->BeginRead().has_value()) {
    return false;
  }
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{4096},
                                                }));
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  RecordSorter sorter = TakeValue(storage.CreateRecordSorter(Descriptor()));
  const auto values = [](std::int64_t key) {
    return std::array<SqlValue, 2>{
        SqlValue::Integer(key),
        SqlValue::Integer(key),
    };
  };

  allocation_index.store(0, std::memory_order_relaxed);
  failing_allocation = 0;
  inject_failure = true;
  const std::array<SqlValue, 2> first = values(1);
  const Status first_failure = sorter.Insert(first);
  inject_failure = false;
  if (first_failure.has_value() || first_failure.error().code() != ErrorCode::kOutOfMemory ||
      !sorter.valid() || sorter.record_count() != 0U || sorter.memory_usage() != ByteCount{0} ||
      vfs.pathless_open_count() != 0U) {
    return false;
  }
  if (!sorter.Insert(first).has_value()) {
    return false;
  }

  std::size_t accepted = 1;
  bool observed_growth_failure = false;
  for (std::int64_t key = 2; key < 64; ++key) {
    const std::array<SqlValue, 2> record = values(key);
    const ByteCount usage_before = sorter.memory_usage();
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    inject_failure = true;
    const Status inserted = sorter.Insert(record);
    inject_failure = false;
    if (inserted.has_value()) {
      ++accepted;
      continue;
    }
    if (inserted.error().code() != ErrorCode::kOutOfMemory || !sorter.valid() ||
        sorter.record_count() != accepted || sorter.memory_usage() != usage_before ||
        vfs.pathless_open_count() != 0U) {
      return false;
    }
    if (!sorter.Insert(record).has_value() || sorter.memory_usage() <= usage_before) {
      return false;
    }
    ++accepted;
    observed_growth_failure = true;
    break;
  }
  return observed_growth_failure;
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
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  const RecordSorterDescriptor descriptor = Descriptor();

  allocation_index.store(0, std::memory_order_relaxed);
  const Result<RecordSorter> baseline = storage.CreateRecordSorter(descriptor);
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
    const Result<RecordSorter> rejected = storage.CreateRecordSorter(descriptor);
    inject_failure = false;
    if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory) {
      return 1;
    }
  }

  RecordSorter sorter = TakeValue(storage.CreateRecordSorter(descriptor));
  const std::array<SqlValue, 2> failed_record{
      SqlValue::Integer(5),
      SqlValue::Integer(5),
  };
  allocation_index.store(0, std::memory_order_relaxed);
  failing_allocation = 0;
  inject_failure = true;
  const Status failed_insert = sorter.Insert(failed_record);
  inject_failure = false;
  if (failed_insert.has_value() || failed_insert.error().code() != ErrorCode::kOutOfMemory ||
      sorter.valid() || sorter.record_count() != 0U || sorter.memory_usage() != ByteCount{0}) {
    return 1;
  }

  sorter = TakeValue(storage.CreateRecordSorter(descriptor));
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

  const TemporaryStorageFactory file_factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{128},
                                                }));
  std::optional<StatementTemporaryStorage> file_storage;
  const auto build_spilled_sorter = [&]() {
    if (!file_storage.has_value()) {
      file_storage.emplace(TakeValue(file_factory.CreateStatementStorage()));
    }
    RecordSorter result = TakeValue(file_storage->CreateRecordSorter(descriptor));
    for (std::int64_t key = 4; key >= 1; --key) {
      if (!result.Insert(EncodedRecord(key)).has_value()) {
        throw std::runtime_error{"failed to build spilled sorter OOM fixture"};
      }
    }
    return result;
  };

  RecordSorter rewind_baseline = build_spilled_sorter();
  allocation_index.store(0, std::memory_order_relaxed);
  const Status baseline_rewind = rewind_baseline.Rewind();
  if (!baseline_rewind.has_value()) {
    return 1;
  }
  const std::size_t rewind_allocations = allocation_index.load(std::memory_order_relaxed);
  if (rewind_allocations == 0U || rewind_allocations > 32U) {
    return 1;
  }
  rewind_baseline.Close();
  file_storage.reset();
  if (vfs.pathless_file_present()) {
    return 1;
  }

  for (std::size_t failure = 0; failure < rewind_allocations; ++failure) {
    RecordSorter candidate = build_spilled_sorter();
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    const Status rejected = candidate.Rewind();
    inject_failure = false;
    if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory ||
        candidate.valid() || !vfs.pathless_file_present()) {
      return 1;
    }
    file_storage.reset();
    if (vfs.pathless_file_present()) {
      return 1;
    }
  }
  if (!ExhaustAllocations(
          [](std::optional<std::size_t> failure) { return RunFileInsert(false, failure); })) {
    return 2;
  }
  if (!ExhaustAllocations(
          [](std::optional<std::size_t> failure) { return RunFileInsert(true, failure); })) {
    return 3;
  }
  if (!ExhaustAllocations(RunFileMerge)) {
    return 4;
  }
  if (!DetachedRunAllocationPrecedesIo()) {
    return 5;
  }
  if (!BulkGrowthFailurePreservesPublishedPrefix()) {
    return 6;
  }
  return 0;
} catch (...) {
  inject_failure = false;
  fail_all_allocations = false;
  return 1;
}
