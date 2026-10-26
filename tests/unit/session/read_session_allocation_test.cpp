#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "tests/support/aligned_allocation.hpp"

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
  if (modern_sqlite::test_support::AllocateAligned(&memory, alignment,
                                                   size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "catalog_loader" / "sqlite-3.54.0-catalog.db";
}

[[nodiscard]] modern_sqlite::ReadStatement Prepare(modern_sqlite::ReadSession& session,
                                                   std::string_view sql) {
  auto prepared = session.Prepare(modern_sqlite::Utf8View{sql});
  if (!prepared.has_value() || !prepared->statement.has_value()) {
    throw std::runtime_error{"unable to prepare session allocation statement"};
  }
  return std::move(*prepared->statement);
}

void ResetCount() { allocation_count.store(0, std::memory_order_relaxed); }

[[nodiscard]] bool NoAllocations() {
  return allocation_count.load(std::memory_order_relaxed) == 0U;
}

[[nodiscard]] bool AtMostAllocations(std::size_t maximum) {
  return allocation_count.load(std::memory_order_relaxed) <= maximum;
}

[[nodiscard]] int FailedCheck(
    std::source_location location = std::source_location::current()) noexcept {
  std::fprintf(stderr, "read-session allocation check failed at line %u (allocations=%zu)\n",
               static_cast<unsigned>(location.line()),
               allocation_count.load(std::memory_order_relaxed));
  return 1;
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

  auto opened = ReadSession::Open(FixturePath().string());
  if (!opened.has_value()) {
    return FailedCheck();
  }
  ReadSession session = std::move(*opened);
  ReadStatement constant = Prepare(session, "SELECT 1");
  ReadStatement lookup = Prepare(session, "SELECT id FROM items WHERE rowid=?1");
  ReadStatement ordered = Prepare(session, "SELECT id FROM items ORDER BY id DESC");
  ReadStatement compound =
      Prepare(session, "SELECT id FROM items UNION SELECT id FROM items ORDER BY 1");
  ReadStatement aggregate =
      Prepare(session, "SELECT count(),min(category),group_concat(name) FROM items");
  ReadStatement grouped = Prepare(
      session, "SELECT score IS NULL,count(),min(id) FROM items GROUP BY score IS NULL ORDER BY 1");
  ReadStatement modifiers =
      Prepare(session,
              "SELECT score IS NULL,group_concat(name ORDER BY id DESC),count(DISTINCT category) "
              "FROM items GROUP BY score IS NULL ORDER BY 1");
  ReadStatement aggregate_distinct =
      Prepare(session, "SELECT DISTINCT count() FROM items GROUP BY score IS NULL ORDER BY 1");
  ReadStatement aggregate_compound =
      Prepare(session, "SELECT count() FROM items UNION ALL SELECT sum(id) FROM items ORDER BY 1");

  ResetCount();
  const auto constant_row = constant.Step();
  if (!constant_row.has_value() || *constant_row != ReadStep::kRow || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  if (constant.row().size() != 1U || constant.result_columns().size() != 1U || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  const auto constant_done = constant.Step();
  if (!constant_done.has_value() || *constant_done != ReadStep::kDone || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  if (!constant.Reset().has_value() || !NoAllocations()) {
    return FailedCheck();
  }

  const SqlValue rowid = SqlValue::Integer(2);
  if (!lookup.Bind(1, rowid).has_value()) {
    return FailedCheck();
  }
  auto lookup_row = lookup.Step();
  if (!lookup_row.has_value() || *lookup_row != ReadStep::kRow) {
    return FailedCheck();
  }
  auto lookup_done = lookup.Step();
  if (!lookup_done.has_value() || *lookup_done != ReadStep::kDone || !lookup.Reset().has_value()) {
    return FailedCheck();
  }

  ResetCount();
  if (!lookup.Bind(1, rowid).has_value() || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  lookup_row = lookup.Step();
  if (!lookup_row.has_value() || *lookup_row != ReadStep::kRow || !AtMostAllocations(3U)) {
    return FailedCheck();
  }
  ResetCount();
  if (lookup.row().size() != 1U || lookup.parameter_count() != 1U ||
      lookup.parameter_name(1) != "?1" || lookup.parameter_index("?1") != 1U || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  lookup_done = lookup.Step();
  if (!lookup_done.has_value() || *lookup_done != ReadStep::kDone || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  if (!lookup.Reset().has_value() || !NoAllocations()) {
    return FailedCheck();
  }

  ResetCount();
  const auto ordered_first = ordered.Step();
  if (!ordered_first.has_value() || *ordered_first != ReadStep::kRow ||
      ordered.row()[0].integer_value().value_or(0) != 3 || !AtMostAllocations(32U)) {
    return FailedCheck();
  }
  ResetCount();
  const auto ordered_second = ordered.Step();
  if (!ordered_second.has_value() || *ordered_second != ReadStep::kRow ||
      ordered.row()[0].integer_value().value_or(0) != 2 || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  const auto ordered_third = ordered.Step();
  if (!ordered_third.has_value() || *ordered_third != ReadStep::kRow ||
      ordered.row()[0].integer_value().value_or(0) != 1 || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  const auto ordered_done = ordered.Step();
  if (!ordered_done.has_value() || *ordered_done != ReadStep::kDone || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  if (!ordered.Reset().has_value() || !NoAllocations()) {
    return FailedCheck();
  }

  ResetCount();
  const auto compound_first = compound.Step();
  if (!compound_first.has_value() || *compound_first != ReadStep::kRow ||
      compound.row()[0].integer_value().value_or(0) != 1 || !AtMostAllocations(256U)) {
    return FailedCheck();
  }
  ResetCount();
  const auto compound_second = compound.Step();
  if (!compound_second.has_value() || *compound_second != ReadStep::kRow ||
      compound.row()[0].integer_value().value_or(0) != 2 || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  const auto compound_third = compound.Step();
  if (!compound_third.has_value() || *compound_third != ReadStep::kRow ||
      compound.row()[0].integer_value().value_or(0) != 3 || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  const auto compound_done = compound.Step();
  if (!compound_done.has_value() || *compound_done != ReadStep::kDone || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  if (!compound.Reset().has_value() || !NoAllocations()) {
    return FailedCheck();
  }

  ResetCount();
  const auto aggregate_row = aggregate.Step();
  if (!aggregate_row.has_value() || *aggregate_row != ReadStep::kRow ||
      aggregate.row().size() != 3U ||
      aggregate.row()[0].integer_value() != std::optional<std::int64_t>{3} ||
      !AtMostAllocations(64U)) {
    return FailedCheck();
  }
  ResetCount();
  const auto aggregate_done = aggregate.Step();
  if (!aggregate_done.has_value() || *aggregate_done != ReadStep::kDone || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  if (!aggregate.Reset().has_value() || !NoAllocations()) {
    return FailedCheck();
  }

  ResetCount();
  const auto grouped_first = grouped.Step();
  if (!grouped_first.has_value() || *grouped_first != ReadStep::kRow ||
      grouped.row().size() != 3U ||
      grouped.row()[0].integer_value() != std::optional<std::int64_t>{0} ||
      !AtMostAllocations(256U)) {
    return FailedCheck();
  }
  ResetCount();
  const auto grouped_second = grouped.Step();
  if (!grouped_second.has_value() || *grouped_second != ReadStep::kRow ||
      grouped.row()[0].integer_value() != std::optional<std::int64_t>{1} ||
      !AtMostAllocations(2U)) {
    return FailedCheck();
  }
  ResetCount();
  const auto grouped_done = grouped.Step();
  if (!grouped_done.has_value() || *grouped_done != ReadStep::kDone || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  if (!grouped.Reset().has_value() || !NoAllocations()) {
    return FailedCheck();
  }

  ResetCount();
  const auto modifier_first = modifiers.Step();
  if (!modifier_first.has_value() || *modifier_first != ReadStep::kRow ||
      modifiers.row().size() != 3U || !AtMostAllocations(256U)) {
    return FailedCheck();
  }
  ResetCount();
  const auto modifier_second = modifiers.Step();
  if (!modifier_second.has_value() || *modifier_second != ReadStep::kRow ||
      !AtMostAllocations(64U)) {
    return FailedCheck();
  }
  ResetCount();
  const auto modifier_done = modifiers.Step();
  if (!modifier_done.has_value() || *modifier_done != ReadStep::kDone || !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  if (!modifiers.Reset().has_value() || !NoAllocations()) {
    return FailedCheck();
  }

  ResetCount();
  const auto aggregate_distinct_first = aggregate_distinct.Step();
  if (!aggregate_distinct_first.has_value() || *aggregate_distinct_first != ReadStep::kRow ||
      aggregate_distinct.row().size() != 1U || !AtMostAllocations(256U)) {
    return FailedCheck();
  }
  ResetCount();
  const auto aggregate_distinct_second = aggregate_distinct.Step();
  if (!aggregate_distinct_second.has_value() || *aggregate_distinct_second != ReadStep::kRow ||
      !AtMostAllocations(2U)) {
    return FailedCheck();
  }
  ResetCount();
  const auto aggregate_distinct_done = aggregate_distinct.Step();
  if (!aggregate_distinct_done.has_value() || *aggregate_distinct_done != ReadStep::kDone ||
      !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  if (!aggregate_distinct.Reset().has_value() || !NoAllocations()) {
    return FailedCheck();
  }

  ResetCount();
  const auto aggregate_compound_first = aggregate_compound.Step();
  if (!aggregate_compound_first.has_value() || *aggregate_compound_first != ReadStep::kRow ||
      aggregate_compound.row().size() != 1U || !AtMostAllocations(256U)) {
    return FailedCheck();
  }
  ResetCount();
  const auto aggregate_compound_second = aggregate_compound.Step();
  if (!aggregate_compound_second.has_value() || *aggregate_compound_second != ReadStep::kRow ||
      !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  const auto aggregate_compound_done = aggregate_compound.Step();
  if (!aggregate_compound_done.has_value() || *aggregate_compound_done != ReadStep::kDone ||
      !NoAllocations()) {
    return FailedCheck();
  }
  ResetCount();
  if (!aggregate_compound.Reset().has_value() || !NoAllocations()) {
    return FailedCheck();
  }

  return 0;
} catch (...) {
  return FailedCheck();
}
