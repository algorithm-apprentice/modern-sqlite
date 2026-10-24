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

[[nodiscard]] bool CheckReusableFieldCopies(std::span<const modern_sqlite::SqlValue> values) {
  using namespace modern_sqlite;
  const auto encoded = EncodeRecord(values);
  if (!encoded.has_value()) {
    return false;
  }
  const auto record = RecordView::Parse(encoded->view());
  if (!record.has_value()) {
    return false;
  }
  for (std::size_t index = 0; index < values.size(); ++index) {
    const auto field = record->field(index);
    if (!field.has_value()) {
      return false;
    }
    const bool bytes = field->type() == SqlValueType::kText || field->type() == SqlValueType::kBlob;
    ByteBuffer storage{ByteCount{1}};
    const auto* original_address = storage.view().data();
    const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
    allocation_index.store(0, std::memory_order_relaxed);
    fail_allocations.store(true, std::memory_order_relaxed);
    const auto rejected = field->CopyTo(storage);
    fail_allocations.store(false, std::memory_order_relaxed);
    if (bytes ? rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory
              : !rejected.has_value()) {
      return false;
    }
    if (storage.view().data() != original_address || storage.size() != ByteCount{1} ||
        live_allocations.load(std::memory_order_relaxed) != live_before) {
      return false;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    const auto copied = field->CopyTo(storage);
    if (!copied.has_value() ||
        allocation_index.load(std::memory_order_relaxed) != static_cast<std::size_t>(bytes)) {
      return false;
    }
    const auto* retained_address = storage.view().data();
    allocation_index.store(0, std::memory_order_relaxed);
    fail_allocations.store(true, std::memory_order_relaxed);
    bool valid = true;
    for (std::size_t iteration = 0; iteration < 1024U; ++iteration) {
      const auto reused = field->CopyTo(storage);
      if (!reused.has_value() ||
          CompareRecordFields(*field, *reused, BinaryCollation()) !=
              std::weak_ordering::equivalent ||
          retained_address != storage.view().data()) {
        valid = false;
        break;
      }
    }
    fail_allocations.store(false, std::memory_order_relaxed);
    if (!valid || allocation_index.load(std::memory_order_relaxed) != 0U) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool CheckColumnLayoutFailuresAndReuse() {
  using namespace modern_sqlite;
  const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
  bool correct = true;
  {
    RecordColumnLayout layout;
    fail_allocations.store(true, std::memory_order_relaxed);
    const auto rejected = layout.Prepare(2);
    fail_allocations.store(false, std::memory_order_relaxed);
    if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory ||
        !layout.Prepare(2).has_value()) {
      return false;
    }
    constexpr std::array record{std::byte{2}, std::byte{1}, std::byte{42}};
    allocation_index.store(0, std::memory_order_relaxed);
    fail_allocations.store(true, std::memory_order_relaxed);
    const auto prepared = layout.Prepare(2);
    const auto reset = layout.Reset(record, ByteCount{record.size()});
    const auto field = layout.field(0, record);
    const auto cached = layout.field(0, {});
    const auto decoded = RecordFieldView::ParseColumn(1, ByteView{record}.last(1));
    fail_allocations.store(false, std::memory_order_relaxed);
    correct = prepared.has_value() && reset.has_value() && field.has_value() &&
              field->has_value() && cached.has_value() && cached->has_value() &&
              decoded.has_value() && decoded->integer_value() == 42 &&
              allocation_index.load(std::memory_order_relaxed) == 0U;
    fail_allocations.store(true, std::memory_order_relaxed);
    const auto bad_index = layout.field(2, {});
    const auto bad_size = RecordFieldView::ParseColumn(1, {});
    fail_allocations.store(false, std::memory_order_relaxed);
    correct = correct && !bad_index.has_value() &&
              bad_index.error().code() == ErrorCode::kOutOfMemory && !bad_size.has_value() &&
              bad_size.error().code() == ErrorCode::kOutOfMemory;
  }
  return correct && live_before == live_allocations.load(std::memory_order_relaxed);
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
  const std::array<RecordValueComparisonKey, 2> comparison_keys{
      RecordValueComparisonKey{0, columns[0]}, RecordValueComparisonKey{1, columns[1]}};
  const std::array<RecordValueComparisonKey, 3> permuted_keys{
      RecordValueComparisonKey{1, IndexColumnOrder{BinaryCollation()}},
      RecordValueComparisonKey{0, IndexColumnOrder{NoCaseCollation()}},
      RecordValueComparisonKey{1, IndexColumnOrder{BinaryCollation()}},
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
    const auto values = CompareRecordValues(left_values, right_values, comparison_keys);
    if (!values.has_value() || *values != std::weak_ordering::greater) {
      fail_allocations.store(false, std::memory_order_relaxed);
      return 1;
    }
    const auto permuted = CompareRecordValues(left_values, right_values, permuted_keys);
    if (!permuted.has_value() || *permuted != std::weak_ordering::greater) {
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
  const bool retained_fields = CheckReusableFieldCopies(owned_values);
  const bool column_layout = CheckColumnLayoutFailuresAndReuse();
  return field_failure && decode_failures && retained_fields && column_layout ? 0 : 1;
} catch (...) {
  fail_allocations.store(false, std::memory_order_relaxed);
  failing_allocation.reset();
  return 1;
}
