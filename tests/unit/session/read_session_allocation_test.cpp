#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"

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

  auto opened = ReadSession::Open(FixturePath().string());
  if (!opened.has_value()) {
    return 1;
  }
  ReadSession session = std::move(*opened);
  ReadStatement constant = Prepare(session, "SELECT 1");
  ReadStatement lookup = Prepare(session, "SELECT id FROM items WHERE rowid=?1");

  ResetCount();
  const auto constant_row = constant.Step();
  if (!constant_row.has_value() || *constant_row != ReadStep::kRow || !NoAllocations()) {
    return 1;
  }
  ResetCount();
  if (constant.row().size() != 1U || constant.result_columns().size() != 1U || !NoAllocations()) {
    return 1;
  }
  ResetCount();
  const auto constant_done = constant.Step();
  if (!constant_done.has_value() || *constant_done != ReadStep::kDone || !NoAllocations()) {
    return 1;
  }
  ResetCount();
  if (!constant.Reset().has_value() || !NoAllocations()) {
    return 1;
  }

  const SqlValue rowid = SqlValue::Integer(2);
  if (!lookup.Bind(1, rowid).has_value()) {
    return 1;
  }
  auto lookup_row = lookup.Step();
  if (!lookup_row.has_value() || *lookup_row != ReadStep::kRow) {
    return 1;
  }
  auto lookup_done = lookup.Step();
  if (!lookup_done.has_value() || *lookup_done != ReadStep::kDone || !lookup.Reset().has_value()) {
    return 1;
  }

  ResetCount();
  if (!lookup.Bind(1, rowid).has_value() || !NoAllocations()) {
    return 1;
  }
  ResetCount();
  lookup_row = lookup.Step();
  if (!lookup_row.has_value() || *lookup_row != ReadStep::kRow || !AtMostAllocations(3U)) {
    return 1;
  }
  ResetCount();
  if (lookup.row().size() != 1U || lookup.parameter_count() != 1U ||
      lookup.parameter_name(1) != "?1" || lookup.parameter_index("?1") != 1U || !NoAllocations()) {
    return 1;
  }
  ResetCount();
  lookup_done = lookup.Step();
  if (!lookup_done.has_value() || *lookup_done != ReadStep::kDone || !NoAllocations()) {
    return 1;
  }
  ResetCount();
  if (!lookup.Reset().has_value() || !NoAllocations()) {
    return 1;
  }

  return 0;
} catch (...) {
  return 1;
}
