#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <stdexcept>

#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace {

std::atomic<bool> fail_allocations = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  if (fail_allocations.load(std::memory_order_relaxed)) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  if (fail_allocations.load(std::memory_order_relaxed)) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
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

  std::array<SqlValue, 3> left_values{
      SqlValue::Text("Alpha"),
      SqlValue::Integer(9007199254740993LL),
      SqlValue::Integer(10),
  };
  std::array<SqlValue, 3> right_values{
      SqlValue::Text("alpha"),
      SqlValue::Real(9007199254740992.0),
      SqlValue::Integer(20),
  };
  Result<ByteBuffer> left_encoded = EncodeRecord(left_values);
  Result<ByteBuffer> right_encoded = EncodeRecord(right_values);
  if (!left_encoded.has_value() || !right_encoded.has_value()) {
    return 1;
  }
  Result<RecordView> left = RecordView::Parse(left_encoded->view());
  Result<RecordView> right = RecordView::Parse(right_encoded->view());
  if (!left.has_value() || !right.has_value()) {
    return 1;
  }

  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{NoCaseCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  std::array<SqlValue, 2> search_key{
      SqlValue::Text("alpha"),
      SqlValue::Real(9007199254740992.0),
  };

  fail_allocations.store(true, std::memory_order_relaxed);
  for (std::size_t iteration = 0; iteration < 1024U; ++iteration) {
    const Result<std::weak_ordering> records = CompareRecordPrefixes(*left, *right, columns);
    if (!records.has_value() || *records != std::weak_ordering::greater) {
      fail_allocations.store(false, std::memory_order_relaxed);
      return 1;
    }
    const Result<IndexKeyComparison> index = CompareIndexRecord(*left, search_key, columns);
    if (!index.has_value() || index->ordering != std::weak_ordering::greater ||
        index->equivalent_prefix) {
      fail_allocations.store(false, std::memory_order_relaxed);
      return 1;
    }
  }
  fail_allocations.store(false, std::memory_order_relaxed);
  return 0;
} catch (...) {
  fail_allocations.store(false, std::memory_order_relaxed);
  return 1;
}
