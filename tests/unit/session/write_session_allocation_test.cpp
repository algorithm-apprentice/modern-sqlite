#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <utility>

#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

std::atomic<std::size_t> allocation_count = 0;
bool count_allocations = false;
bool fail_allocations = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  if (count_allocations) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  if (count_allocations) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
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
  auto vfs = std::make_unique<test::WritePagerFixedVfs>(false);
  auto opened = WriteSession::Open(std::move(vfs), test::kWritePagerDatabasePath);
  if (!opened.has_value()) {
    return 1;
  }
  WriteSession& session = *opened;
  auto create = session.Prepare(Utf8View{"CREATE TABLE Items(id INTEGER PRIMARY KEY)"});
  if (!create.has_value() || !create->statement.has_value()) {
    return 1;
  }
  auto created = create->statement->Step();
  if (!created.has_value() || *created != WriteStep::kDone) {
    return 1;
  }

  constexpr std::size_t kExpectedPrepareAllocations = 22U;
  std::optional<WriteStatement> published;
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    auto prepared = session.Prepare(Utf8View{"CREATE TABLE IF NOT EXISTS Items(a UNIQUE)"});
    count_allocations = false;
    if (!prepared.has_value() || !prepared->statement.has_value()) {
      return 1;
    }
    const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
    if (allocations != kExpectedPrepareAllocations) {
      return 1;
    }
    if (!published.has_value()) {
      published.emplace(std::move(*prepared->statement));
    }
  }
  if (!published.has_value()) {
    return 1;
  }

  fail_allocations = true;
  const std::size_t parameters = published->parameter_count();
  const std::size_t columns = published->result_columns().size();
  const bool valid = published->valid();
  fail_allocations = false;
  return parameters == 0U && columns == 0U && valid ? 0 : 1;
} catch (...) {
  count_allocations = false;
  fail_allocations = false;
  return 1;
}
