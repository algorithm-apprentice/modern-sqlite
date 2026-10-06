#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "storage/btree/writer_internal.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::optional<std::size_t> failing_allocation;

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation == index) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation == index) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0U ? alignment : size) == 0) {
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

struct Outcome {
  std::size_t allocations = 0;
  bool succeeded = false;
  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool invariant_holds = false;
};

[[nodiscard]] Outcome RunOverflowSeek(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }

  const std::array<modern_sqlite::IndexColumnOrder, 1> columns{
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
  modern_sqlite::PageNumber root_page;
  std::array<modern_sqlite::SqlValue, 1> key{
      modern_sqlite::SqlValue::Blob(modern_sqlite::ByteBuffer{modern_sqlite::ByteCount{2'000}}),
  };
  {
    auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
    if (!session.has_value() || !session->InitializeDatabase().has_value()) {
      return {};
    }
    auto writer = session->CreateIndexBtree(columns);
    if (!writer.has_value()) {
      return {};
    }
    root_page = writer->root_page();
    if (!writer->Insert(key).has_value() || !pager->Commit().has_value()) {
      return {};
    }
  }
  if (!pager->BeginWrite().has_value()) {
    return {};
  }

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool invariant = false;
  std::size_t allocations = 0;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(owner, root_page, false);
    if (!cursor.has_value()) {
      return {};
    }
    std::vector<std::byte> scratch;
    Arm(failure);
    const auto found =
        cursor->SeekIndex(key, columns,
                          modern_sqlite::RecordCodecOptions{
                              .schema_format = modern_sqlite::RecordSchemaFormat::kFour,
                          },
                          scratch);
    allocations = Disarm();
    succeeded = found.has_value();
    error = found.has_value() ? modern_sqlite::ErrorCode::kGeneric : found.error().code();
    invariant =
        found.has_value()
            ? found->exact
            : cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kFault &&
                  owner.size() == 1U;
  }
  invariant = invariant && pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = invariant,
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
  Arm(std::nullopt);
  const auto baseline_workspace =
      modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(modern_sqlite::ByteCount{512});
  const std::size_t workspace_allocations = Disarm();
  if (!baseline_workspace.has_value() || workspace_allocations == 0U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < workspace_allocations; ++failure) {
    Arm(failure);
    const auto workspace =
        modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(modern_sqlite::ByteCount{512});
    (void)Disarm();
    if (workspace.has_value() ||
        workspace.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return 2;
    }
  }

  const Outcome baseline = RunOverflowSeek(std::nullopt);
  if (!baseline.succeeded || !baseline.invariant_holds || baseline.allocations == 0U) {
    return 3;
  }
  for (std::size_t failure = 0; failure < baseline.allocations; ++failure) {
    const Outcome outcome = RunOverflowSeek(failure);
    if (outcome.succeeded || outcome.error != modern_sqlite::ErrorCode::kOutOfMemory ||
        !outcome.invariant_holds) {
      return 4;
    }
  }
  return 0;
} catch (...) {
  failing_allocation.reset();
  return 5;
}
