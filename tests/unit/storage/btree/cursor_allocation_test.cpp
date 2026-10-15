#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

std::atomic<std::size_t> live_allocations = 0;
std::atomic<std::size_t> allocation_index = 0;
std::optional<std::size_t> failing_allocation;

void BeforeAllocation() {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation == index) {
    throw std::bad_alloc{};
  }
}

[[nodiscard]] void* Allocate(std::size_t size) {
  BeforeAllocation();
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) {
    live_allocations.fetch_add(1, std::memory_order_relaxed);
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  BeforeAllocation();
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0U ? alignment : size) == 0) {
    live_allocations.fetch_add(1, std::memory_order_relaxed);
    return memory;
  }
  throw std::bad_alloc{};
}

void Deallocate(void* memory) noexcept {
  if (memory != nullptr) {
    live_allocations.fetch_sub(1, std::memory_order_relaxed);
    std::free(memory);
  }
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

struct OperationResult {
  bool succeeded;
  bool equivalent;
  modern_sqlite::ErrorCode error;
};

[[nodiscard]] OperationResult Execute(modern_sqlite::IndexBtreeCursor& cursor,
                                      std::span<const modern_sqlite::SqlValue> key, bool seek) {
  if (seek) {
    const auto result = cursor.Seek(key, modern_sqlite::BtreeSeekMode::kEqual);
    return OperationResult{
        .succeeded = result.has_value(),
        .equivalent = result.has_value() && *result,
        .error = result.has_value() ? modern_sqlite::ErrorCode::kGeneric : result.error().code(),
    };
  }
  const auto result = cursor.CompareCurrent(key);
  return OperationResult{
      .succeeded = result.has_value(),
      .equivalent = result.has_value() && *result == std::weak_ordering::equivalent,
      .error = result.has_value() ? modern_sqlite::ErrorCode::kGeneric : result.error().code(),
  };
}

struct Outcome {
  std::size_t allocations;
  bool invariant_holds;
};

[[nodiscard]] Outcome RunCase(bool wide_header, bool seek, std::size_t requested_fields,
                              std::optional<std::size_t> failure,
                              std::optional<modern_sqlite::ErrorCode> expected_error) {
  using namespace modern_sqlite;
  failing_allocation.reset();
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = TakeValue(Pager::OpenEphemeral(
      vfs, PagerOptions{
               .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
               .cache_capacity_pages = 32,
           }));
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  const std::array<IndexColumnOrder, 3> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  const std::array<SqlValue, 3> key{
      SqlValue::Text(std::string(1024, 'a')),
      SqlValue::Blob(ByteBuffer{ByteCount{8192}}),
      SqlValue::Integer(1),
  };
  PageNumber root;
  {
    std::vector<SqlValue> fields(wide_header ? 200U : 2U);
    fields.front() = SqlValue::Text(std::string(1024, 'a'));
    fields.back() = SqlValue::Blob(ByteBuffer{ByteCount{8192}});
    const ByteBuffer record = TakeValue(EncodeRecord(fields));
    BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*pager));
    RequireStatus(session.InitializeDatabase());
    IndexBtreeWriter index = TakeValue(session.CreateIndexBtree(std::span{columns}.first(1)));
    root = index.root_page();
    RequireStatus(index.InsertEncoded(record.view()));
  }
  IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager, root, columns));
  if (!TakeValue(cursor.First()) || !cursor.CopyPayload().has_value()) {
    return Outcome{.allocations = 0, .invariant_holds = false};
  }

  std::size_t allocations = 0;
  for (std::size_t repetition = 0; repetition < 3U; ++repetition) {
    if (!TakeValue(cursor.First())) {
      return Outcome{.allocations = 0, .invariant_holds = false};
    }
    const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    const OperationResult result = Execute(cursor, std::span{key}.first(requested_fields), seek);
    failing_allocation.reset();
    allocations = allocation_index.load(std::memory_order_relaxed);
    const bool released = live_before == live_allocations.load(std::memory_order_relaxed);
    const bool correct_result = expected_error.has_value()
                                    ? !result.succeeded && result.error == *expected_error
                                    : result.succeeded && result.equivalent;
    const bool correct_position = cursor.valid() == (!expected_error.has_value() || !seek);
    if (!released || !correct_result || !correct_position) {
      std::fprintf(stderr,
                   "cursor allocation invariant: wide=%d seek=%d fields=%zu "
                   "released=%d result=%d position=%d\n",
                   wide_header, seek, requested_fields, released, correct_result, correct_position);
      return Outcome{.allocations = allocations, .invariant_holds = false};
    }
  }
  RequireStatus(cursor.ClearPosition());
  return Outcome{.allocations = allocations, .invariant_holds = true};
}

[[nodiscard]] bool ExhaustAllocations(bool wide_header, bool seek, std::size_t requested_fields,
                                      std::optional<modern_sqlite::ErrorCode> expected_error) {
  const Outcome baseline =
      RunCase(wide_header, seek, requested_fields, std::nullopt, expected_error);
  if (!baseline.invariant_holds || baseline.allocations == 0) {
    return false;
  }
  for (std::size_t failure = 0; failure < baseline.allocations; ++failure) {
    const Outcome outcome = RunCase(wide_header, seek, requested_fields, failure,
                                    modern_sqlite::ErrorCode::kOutOfMemory);
    if (!outcome.invariant_holds) {
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
void operator delete(void* memory) noexcept { Deallocate(memory); }
void operator delete[](void* memory) noexcept { Deallocate(memory); }
void operator delete(void* memory, std::size_t) noexcept { Deallocate(memory); }
void operator delete[](void* memory, std::size_t) noexcept { Deallocate(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { Deallocate(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { Deallocate(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { Deallocate(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { Deallocate(memory); }

int main() try {
  for (const bool seek : {false, true}) {
    for (const bool wide_header : {false, true}) {
      if (!ExhaustAllocations(wide_header, seek, 1, std::nullopt)) {
        return 1;
      }
    }
    if (!ExhaustAllocations(false, seek, 3, modern_sqlite::ErrorCode::kCorruption)) {
      return 1;
    }
  }
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "cursor allocation fixture failure: %s\n", error.what());
  return 1;
}
