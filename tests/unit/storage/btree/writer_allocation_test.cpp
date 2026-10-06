#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <string>

#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "write_pager_test_support.hpp"

namespace {

std::atomic<std::size_t> allocation_count = 0;

[[nodiscard]] void* Allocate(std::size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

struct LocalMutationAllocations {
  std::size_t insert;
  std::size_t replace;
  std::size_t erase;
  std::size_t index_insert;
  std::size_t index_erase;
  std::size_t overflow_index_probe;
};

[[nodiscard]] std::optional<LocalMutationAllocations> MeasureLocalMutations() {
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 8);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return std::nullopt;
  }
  const std::array<modern_sqlite::IndexColumnOrder, 1> columns{
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
  modern_sqlite::PageNumber index_root;
  modern_sqlite::PageNumber overflow_index_root;
  {
    auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
    if (!session.has_value() || !session->InitializeDatabase().has_value()) {
      return std::nullopt;
    }
    auto index = session->CreateIndexBtree(columns);
    if (!index.has_value()) {
      return std::nullopt;
    }
    index_root = index->root_page();
    auto overflow_index = session->CreateIndexBtree(columns);
    if (!overflow_index.has_value()) {
      return std::nullopt;
    }
    overflow_index_root = overflow_index->root_page();
  }
  if (!pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return std::nullopt;
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return std::nullopt;
  }
  auto writer = session->OpenTableBtree(modern_sqlite::PageNumber{1});
  auto index = session->OpenIndexBtree(index_root, columns);
  auto overflow_index = session->OpenIndexBtree(overflow_index_root, columns);
  if (!writer.has_value() || !index.has_value() || !overflow_index.has_value()) {
    return std::nullopt;
  }
  constexpr std::array<std::byte, 8> kPayload{std::byte{0x5a}};
  constexpr std::array<std::byte, 8> kReplacement{std::byte{0x6b}};
  const std::array<modern_sqlite::SqlValue, 1> key_one{
      modern_sqlite::SqlValue::Integer(1),
  };
  const std::array<modern_sqlite::SqlValue, 1> key_two{
      modern_sqlite::SqlValue::Integer(2),
  };
  const std::array<modern_sqlite::SqlValue, 1> overflow_key{
      modern_sqlite::SqlValue::Text(std::string(1'000U, 'x')),
  };
  const std::array<modern_sqlite::SqlValue, 1> missing_overflow_key{
      modern_sqlite::SqlValue::Text(std::string(1'000U, 'y')),
  };

  if (!writer->Insert(1, kPayload).has_value() ||
      !writer->Insert(1, kReplacement, modern_sqlite::BtreeInsertMode::kReplace).has_value() ||
      !writer->Insert(2, kPayload).has_value() || !writer->Delete(2).has_value() ||
      !writer->Insert(2, kPayload).has_value() || !writer->Delete(2).has_value() ||
      !index->Insert(key_one).has_value() || !index->Insert(key_two).has_value() ||
      !index->Delete(key_two).has_value() || !index->Insert(key_two).has_value() ||
      !index->Delete(key_two).has_value() || !overflow_index->Insert(overflow_key).has_value()) {
    return std::nullopt;
  }
  const auto warmed_overflow_probe = overflow_index->Delete(missing_overflow_key);
  if (!warmed_overflow_probe.has_value() || *warmed_overflow_probe) {
    return std::nullopt;
  }

  allocation_count.store(0, std::memory_order_relaxed);
  const auto inserted = writer->Insert(2, kPayload);
  const std::size_t insert_allocations = allocation_count.load(std::memory_order_relaxed);
  if (!inserted.has_value()) {
    return std::nullopt;
  }

  allocation_count.store(0, std::memory_order_relaxed);
  const auto replaced = writer->Insert(1, kPayload, modern_sqlite::BtreeInsertMode::kReplace);
  const std::size_t replace_allocations = allocation_count.load(std::memory_order_relaxed);
  if (!replaced.has_value()) {
    return std::nullopt;
  }

  allocation_count.store(0, std::memory_order_relaxed);
  const auto erased = writer->Delete(2);
  const std::size_t erase_allocations = allocation_count.load(std::memory_order_relaxed);
  if (!erased.has_value() || !*erased) {
    return std::nullopt;
  }

  allocation_count.store(0, std::memory_order_relaxed);
  const auto index_inserted = index->Insert(key_two);
  const std::size_t index_insert_allocations = allocation_count.load(std::memory_order_relaxed);
  if (!index_inserted.has_value()) {
    return std::nullopt;
  }

  allocation_count.store(0, std::memory_order_relaxed);
  const auto index_erased = index->Delete(key_two);
  const std::size_t index_erase_allocations = allocation_count.load(std::memory_order_relaxed);
  if (!index_erased.has_value() || !*index_erased) {
    return std::nullopt;
  }

  allocation_count.store(0, std::memory_order_relaxed);
  const auto overflow_probe = overflow_index->Delete(missing_overflow_key);
  const std::size_t overflow_probe_allocations = allocation_count.load(std::memory_order_relaxed);
  if (!overflow_probe.has_value() || *overflow_probe) {
    return std::nullopt;
  }
  return LocalMutationAllocations{
      .insert = insert_allocations,
      .replace = replace_allocations,
      .erase = erase_allocations,
      .index_insert = index_insert_allocations,
      .index_erase = index_erase_allocations,
      .overflow_index_probe = overflow_probe_allocations,
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
  const std::optional<LocalMutationAllocations> measured = MeasureLocalMutations();
  if (!measured.has_value()) {
    return 1;
  }
  if (measured->insert != 0U) {
    return 2;
  }
  if (measured->replace != 0U) {
    return 3;
  }
  if (measured->erase != 0U) {
    return 4;
  }
  if (measured->index_insert != 0U) {
    return 5;
  }
  if (measured->index_erase != 0U) {
    return 6;
  }
  if (measured->overflow_index_probe != 0U) {
    return 7;
  }
  return 0;
} catch (...) {
  return 8;
}
