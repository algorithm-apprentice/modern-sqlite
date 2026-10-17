#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace {

std::atomic<bool> fail_allocations = false;
std::atomic<std::size_t> allocation_index = 0;
std::atomic<std::size_t> live_allocations = 0;
std::optional<std::size_t> failing_allocation;

void BeforeAllocation() {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (fail_allocations.load(std::memory_order_relaxed) || failing_allocation == index) {
    throw std::bad_alloc{};
  }
}

[[nodiscard]] void* Allocate(std::size_t size) {
  BeforeAllocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    live_allocations.fetch_add(1, std::memory_order_relaxed);
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  BeforeAllocation();
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
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

[[nodiscard]] bool ReportsTypedFieldRangeFailure(const modern_sqlite::RecordView& view) {
  fail_allocations.store(true, std::memory_order_relaxed);
  try {
    const auto field = view.field(view.field_count());
    fail_allocations.store(false, std::memory_order_relaxed);
    if (field.has_value() || field.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      std::fprintf(stderr, "record field diagnostic did not return typed OOM\n");
      return false;
    }
  } catch (const std::bad_alloc&) {
    fail_allocations.store(false, std::memory_order_relaxed);
    std::fprintf(stderr, "record field diagnostic escaped std::bad_alloc\n");
    return false;
  }
  return true;
}

[[nodiscard]] bool MatchesOwnedValues(std::span<const modern_sqlite::SqlValue> actual,
                                      std::span<const modern_sqlite::SqlValue> expected) {
  using namespace modern_sqlite;
  if (actual.size() != expected.size()) {
    return false;
  }
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (actual[index].type() != expected[index].type() ||
        CompareSqlValues(actual[index], expected[index]) != std::strong_ordering::equal) {
      return false;
    }
    if (actual[index].type() == SqlValueType::kText &&
        actual[index].text_value()->data() == expected[index].text_value()->data()) {
      return false;
    }
    if (actual[index].type() == SqlValueType::kBlob &&
        actual[index].blob_value()->data() == expected[index].blob_value()->data()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool ExhaustDecodeFailures(std::span<const modern_sqlite::SqlValue> values) {
  using namespace modern_sqlite;
  const auto encoded = EncodeRecord(values);
  if (!encoded.has_value()) {
    return false;
  }
  const ByteBuffer original = ByteBuffer::CopyOf(encoded->view());
  bool success = true;
  for (std::size_t attempt = 0; attempt < 4U; ++attempt) {
    const std::optional<std::size_t> failure =
        attempt == 0 ? std::nullopt : std::optional<std::size_t>{attempt - 1U};
    const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    bool correct = false;
    try {
      const auto decoded = DecodeRecord(encoded->view());
      failing_allocation.reset();
      correct = failure.has_value()
                    ? !decoded.has_value() && decoded.error().code() == ErrorCode::kOutOfMemory
                    : decoded.has_value() && MatchesOwnedValues(*decoded, values) &&
                          allocation_index.load(std::memory_order_relaxed) == 3U;
    } catch (const std::bad_alloc&) {
      failing_allocation.reset();
      std::fprintf(stderr, "record decode allocation %zu escaped std::bad_alloc\n", attempt - 1U);
    }
    correct = correct && live_before == live_allocations.load(std::memory_order_relaxed) &&
              std::ranges::equal(original.view(), encoded->view());
    {
      const auto retry = DecodeRecord(encoded->view());
      correct = correct && retry.has_value() && MatchesOwnedValues(*retry, values);
    }
    correct = correct && live_before == live_allocations.load(std::memory_order_relaxed);
    success = correct && success;
  }
  return success;
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
    const auto header_size =
        RecordPrefixView::RequiredHeaderSize(left_encoded->view().first(1), left_encoded->size());
    const auto prefix_size = RecordPrefixView::RequiredPrefixSize(
        left_encoded->view(), left_encoded->size(), search_key.size());
    if (!header_size.has_value() || !prefix_size.has_value()) {
      fail_allocations.store(false, std::memory_order_relaxed);
      return 1;
    }
    const auto prefix = RecordPrefixView::Parse(left_encoded->view().first(prefix_size->value()),
                                                left_encoded->size(), search_key.size());
    if (!prefix.has_value()) {
      fail_allocations.store(false, std::memory_order_relaxed);
      return 1;
    }
    const auto prefix_index = CompareIndexRecord(*prefix, search_key, columns);
    if (!prefix_index.has_value() || prefix_index->ordering != std::weak_ordering::greater ||
        prefix_index->equivalent_prefix) {
      fail_allocations.store(false, std::memory_order_relaxed);
      return 1;
    }
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
  const std::array owned_values{
      SqlValue::Integer(17),
      SqlValue::Text(std::string(256, 't')),
      SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(std::string(256, 'b')))),
  };
  const bool field_failure = ReportsTypedFieldRangeFailure(*left);
  const bool decode_failures = ExhaustDecodeFailures(owned_values);
  return field_failure && decode_failures ? 0 : 1;
} catch (...) {
  fail_allocations.store(false, std::memory_order_relaxed);
  failing_allocation.reset();
  return 1;
}
