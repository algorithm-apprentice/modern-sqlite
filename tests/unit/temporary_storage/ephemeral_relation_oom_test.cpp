#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
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

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && index == failing_allocation) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && index == failing_allocation) {
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

void RequireStatus(modern_sqlite::Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

[[nodiscard]] modern_sqlite::EphemeralRelationDescriptor Descriptor(
    modern_sqlite::EphemeralRelationPurpose purpose =
        modern_sqlite::EphemeralRelationPurpose::kMembership) {
  using namespace modern_sqlite;
  EphemeralRelationDescriptor descriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              IndexColumnOrder{BinaryCollation()},
          },
      .purpose = purpose,
  };
  if (purpose == EphemeralRelationPurpose::kLookup) {
    descriptor.key_field_count = descriptor.field_count;
    descriptor.key_columns.push_back(IndexColumnOrder{BinaryCollation()});
  } else if (purpose == EphemeralRelationPurpose::ReusableMembership) {
    descriptor.field_count = 1;
    descriptor.track_nulls = true;
  }
  return descriptor;
}

[[nodiscard]] modern_sqlite::ByteBuffer Key(std::int64_t key) {
  using namespace modern_sqlite;
  const std::array<SqlValue, 1> values{SqlValue::Integer(key)};
  return TakeValue(EncodeRecord(values));
}

struct RowSpec {
  std::int64_t key;
  std::size_t payload_size = 900U;
};

[[nodiscard]] modern_sqlite::ByteBuffer Row(RowSpec spec) {
  using namespace modern_sqlite;
  ByteBuffer payload{ByteCount{spec.payload_size}};
  const std::array<SqlValue, 2> values{
      SqlValue::Integer(spec.key),
      SqlValue::Blob(std::move(payload)),
  };
  return TakeValue(EncodeRecord(values));
}

struct Outcome {
  std::size_t allocations = 0;
  bool succeeded = false;
  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool cleaned = false;
};

enum class FileOperation : std::uint8_t {
  kCreate,
  kInsert,
  kContains,
  kReplace,
  kErase,
  kRewind,
  kNext,
  kReset,
  kCreateLookup,
  kSealLookup,
  kSeekLookupPrefix,
  kNextLookupPrefix,
  CreateReusable,
  SealReusable,
  ProbeReusable,
};

[[nodiscard]] Outcome RunMemoryCreate(std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  inject_failure = false;
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  const EphemeralRelationDescriptor descriptor = Descriptor();

  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  Result<EphemeralRelation> created = storage.CreateEphemeralRelation(descriptor);
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  const bool succeeded = created.has_value();
  const ErrorCode error = succeeded ? ErrorCode::kGeneric : created.error().code();
  if (created.has_value()) {
    created->Close();
  }
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .cleaned = !vfs.pathless_file_present(),
  };
}

[[nodiscard]] Outcome RunMemoryInsert(std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  inject_failure = false;
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  EphemeralRelation relation = TakeValue(storage.CreateEphemeralRelation(Descriptor()));
  ByteBuffer row = Row(RowSpec{.key = 1, .payload_size = 16U});

  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  Result<EphemeralInsertResult> inserted =
      relation.Insert(std::move(row), EphemeralInsertMode::kKeepExisting);
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  const bool succeeded = inserted.has_value() && *inserted == EphemeralInsertResult::kInserted;
  const ErrorCode error = inserted.has_value() ? ErrorCode::kGeneric : inserted.error().code();
  if (relation.valid()) {
    relation.Close();
  }
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .cleaned = !vfs.pathless_file_present(),
  };
}

[[nodiscard]] Outcome RunFileOperation(FileOperation operation,
                                       std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  inject_failure = false;
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager =
      TakeValue(Pager::Open(vfs, test::kWritePagerInputPath,
                            PagerOptions{
                                .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                                .cache_capacity_pages = 1,
                            }));
  RequireStatus(pager->BeginRead());
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{1024},
                                                }));
  std::optional<StatementTemporaryStorage> storage;
  storage.emplace(TakeValue(factory.CreateStatementStorage()));
  const bool lookup = operation == FileOperation::kCreateLookup ||
                      operation == FileOperation::kSealLookup ||
                      operation == FileOperation::kSeekLookupPrefix ||
                      operation == FileOperation::kNextLookupPrefix;
  const bool reusable = operation == FileOperation::CreateReusable ||
                        operation == FileOperation::SealReusable ||
                        operation == FileOperation::ProbeReusable;
  EphemeralRelationDescriptor descriptor =
      Descriptor(reusable ? EphemeralRelationPurpose::ReusableMembership
                          : (lookup ? EphemeralRelationPurpose::kLookup
                                    : EphemeralRelationPurpose::kMembership));
  if (operation == FileOperation::ProbeReusable) {
    descriptor.track_nulls = false;
  }

  if (operation == FileOperation::kCreate || operation == FileOperation::kCreateLookup ||
      operation == FileOperation::CreateReusable) {
    allocation_index.store(0, std::memory_order_relaxed);
    if (failure.has_value()) {
      failing_allocation = *failure;
      inject_failure = true;
    }
    Result<EphemeralRelation> created = storage->CreateEphemeralRelation(descriptor);
    inject_failure = false;
    const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
    const bool succeeded = created.has_value();
    const ErrorCode error = succeeded ? ErrorCode::kGeneric : created.error().code();
    if (created.has_value()) {
      created->Close();
    }
    storage.reset();
    return Outcome{
        .allocations = allocations,
        .succeeded = succeeded,
        .error = error,
        .cleaned = !vfs.pathless_file_present(),
    };
  }

  EphemeralRelation relation = TakeValue(storage->CreateEphemeralRelation(descriptor));
  if (operation != FileOperation::kInsert) {
    auto first = relation.Insert(reusable ? Key(1) : Row(RowSpec{.key = 1}),
                                 EphemeralInsertMode::kKeepExisting);
    if (!first.has_value() || *first != EphemeralInsertResult::kInserted) {
      return {};
    }
    auto second = relation.Insert(
        reusable ? Key(2)
                 : Row(RowSpec{.key = lookup ? 1 : 2, .payload_size = lookup ? 901U : 900U}),
        EphemeralInsertMode::kKeepExisting);
    if (!second.has_value() || *second != EphemeralInsertResult::kInserted) {
      return {};
    }
  }
  if (operation == FileOperation::kNext) {
    if (!relation.Rewind().has_value()) {
      return {};
    }
  }
  ByteBuffer row = Row(RowSpec{
      .key = operation == FileOperation::kReplace ? 1 : 3,
      .payload_size = operation == FileOperation::kInsert ? 1100U : 900U,
  });
  const ByteBuffer key = Key(1);
  const std::array<SqlValue, 1> prefix{SqlValue::Integer(1)};
  if (operation == FileOperation::kSeekLookupPrefix ||
      operation == FileOperation::kNextLookupPrefix || operation == FileOperation::ProbeReusable) {
    RequireStatus(relation.SealForLookup());
  }
  if (operation == FileOperation::kNextLookupPrefix && !TakeValue(relation.SeekPrefix(prefix))) {
    return {};
  }

  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  bool succeeded = false;
  ErrorCode error = ErrorCode::kGeneric;
  switch (operation) {
    case FileOperation::kCreate:
    case FileOperation::kCreateLookup:
    case FileOperation::CreateReusable:
      std::terminate();
    case FileOperation::kInsert: {
      auto result = relation.Insert(std::move(row), EphemeralInsertMode::kKeepExisting);
      succeeded = result.has_value() && *result == EphemeralInsertResult::kInserted;
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
    case FileOperation::kContains: {
      auto result = relation.Contains(key.view());
      succeeded = result.has_value() && *result;
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
    case FileOperation::kReplace: {
      auto result = relation.Insert(std::move(row), EphemeralInsertMode::kReplaceExisting);
      succeeded = result.has_value() && *result == EphemeralInsertResult::kReplaced;
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
    case FileOperation::kErase: {
      auto result = relation.Erase(key.view());
      succeeded = result.has_value() && *result;
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
    case FileOperation::kRewind: {
      const Status result = relation.Rewind();
      succeeded = result.has_value();
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
    case FileOperation::kNext: {
      auto result = relation.Next();
      succeeded = result.has_value() && *result;
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
    case FileOperation::kReset: {
      const Status result = relation.Reset();
      succeeded = result.has_value() && relation.state() == EphemeralRelationState::kWriting &&
                  relation.record_count() == 0U;
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
    case FileOperation::kSealLookup: {
      const Status result = relation.SealForLookup();
      succeeded = result.has_value() && relation.state() == EphemeralRelationState::kLookupReady;
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
    case FileOperation::SealReusable: {
      const Status result = relation.SealForLookup();
      succeeded = result.has_value() && relation.state() == EphemeralRelationState::kLookupReady;
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
    case FileOperation::ProbeReusable: {
      const auto result = relation.Contains(prefix);
      succeeded = result.has_value() && *result;
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
    case FileOperation::kSeekLookupPrefix:
    case FileOperation::kNextLookupPrefix: {
      auto result = operation == FileOperation::kSeekLookupPrefix ? relation.SeekPrefix(prefix)
                                                                  : relation.NextPrefix(prefix);
      succeeded = result.has_value() && *result;
      error = result.has_value() ? ErrorCode::kGeneric : result.error().code();
      break;
    }
  }
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  if (relation.valid()) {
    relation.Close();
  }
  storage.reset();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .cleaned = !vfs.pathless_file_present(),
  };
}

[[nodiscard]] Outcome RunFileReuse(std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  inject_failure = false;
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager =
      TakeValue(Pager::Open(vfs, test::kWritePagerInputPath,
                            PagerOptions{
                                .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                                .cache_capacity_pages = 1,
                            }));
  RequireStatus(pager->BeginRead());
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{1024},
                                                }));
  std::optional<StatementTemporaryStorage> storage;
  storage.emplace(TakeValue(factory.CreateStatementStorage()));

  EphemeralRelation first = TakeValue(storage->CreateEphemeralRelation(Descriptor()));
  for (std::int64_t key = 1; key <= 2; ++key) {
    auto inserted = first.Insert(Row(RowSpec{.key = key}), EphemeralInsertMode::kKeepExisting);
    if (!inserted.has_value() || *inserted != EphemeralInsertResult::kInserted) {
      return {};
    }
  }
  first.Close();
  EphemeralRelation reused = TakeValue(storage->CreateEphemeralRelation(Descriptor()));
  ByteBuffer row = Row(RowSpec{.key = 3, .payload_size = 1100U});

  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  auto inserted = reused.Insert(std::move(row), EphemeralInsertMode::kKeepExisting);
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  const bool succeeded = inserted.has_value() && *inserted == EphemeralInsertResult::kInserted;
  const ErrorCode error = inserted.has_value() ? ErrorCode::kGeneric : inserted.error().code();
  if (reused.valid()) {
    reused.Close();
  }
  storage.reset();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .cleaned = !vfs.pathless_file_present(),
  };
}

template <typename Runner>
[[nodiscard]] bool ExhaustAllocations(Runner&& runner, bool require_allocations = true) {
  const Outcome baseline = runner(std::nullopt);
  if (!baseline.succeeded || !baseline.cleaned ||
      (require_allocations && baseline.allocations == 0U)) {
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
  if (!ExhaustAllocations(RunMemoryCreate)) {
    return 10;
  }
  if (!ExhaustAllocations(RunMemoryInsert)) {
    return 11;
  }
  const std::array operations{
      FileOperation::kCreate,  FileOperation::kInsert, FileOperation::kContains,
      FileOperation::kReplace, FileOperation::kErase,  FileOperation::kRewind,
      FileOperation::kNext,    FileOperation::kReset,
  };
  for (std::size_t index = 0; index < operations.size(); ++index) {
    const FileOperation operation = operations[index];
    if (!ExhaustAllocations([operation](std::optional<std::size_t> failure) {
          return RunFileOperation(operation, failure);
        })) {
      return static_cast<int>(20U + index);
    }
  }
  if (!ExhaustAllocations(RunFileReuse)) {
    return 30;
  }
  const std::array lookup_operations{
      FileOperation::kCreateLookup,
      FileOperation::kSealLookup,
      FileOperation::kSeekLookupPrefix,
      FileOperation::kNextLookupPrefix,
  };
  for (std::size_t index = 0; index < lookup_operations.size(); ++index) {
    const FileOperation operation = lookup_operations[index];
    if (!ExhaustAllocations(
            [operation](std::optional<std::size_t> failure) {
              return RunFileOperation(operation, failure);
            },
            operation != FileOperation::kSealLookup)) {
      return static_cast<int>(40U + index);
    }
  }
  const std::array reusable_operations{
      FileOperation::CreateReusable,
      FileOperation::SealReusable,
      FileOperation::ProbeReusable,
  };
  for (std::size_t index = 0; index < reusable_operations.size(); ++index) {
    const FileOperation operation = reusable_operations[index];
    if (!ExhaustAllocations([operation](std::optional<std::size_t> failure) {
          return RunFileOperation(operation, failure);
        })) {
      return static_cast<int>(50U + index);
    }
  }
  return 0;
} catch (...) {
  inject_failure = false;
  return 1;
}
