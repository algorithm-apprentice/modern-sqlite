#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/syntax/parser.hpp"

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

namespace {

[[nodiscard]] int RunAllocationTest() {
  auto parsed = modern_sqlite::ParseOne(modern_sqlite::Utf8View{"CREATE TABLE items(value TEXT)"});
  if (!parsed.has_value() || !parsed->tree.has_value()) {
    return 1;
  }

  modern_sqlite::CatalogInput input{
      .schema_name = "main",
      .version =
          modern_sqlite::CatalogVersion{
              .schema_cookie = 1,
              .generation = 1,
          },
  };
  input.definitions.push_back(std::move(*parsed->tree));
  input.tables.push_back(modern_sqlite::CatalogTableInput{
      .definition = modern_sqlite::SchemaDefinitionId{0},
      .name = "items",
      .root_page = modern_sqlite::RootPageId{2},
      .columns =
          {
              modern_sqlite::CatalogColumnInput{
                  .name = "value",
                  .declared_type = "TEXT",
              },
          },
  });
  auto snapshot_result = modern_sqlite::CatalogSnapshot::Create(std::move(input));
  if (!snapshot_result.has_value()) {
    return 1;
  }
  const modern_sqlite::CatalogSnapshot& snapshot = **snapshot_result;

  const std::size_t before = allocation_count.load(std::memory_order_relaxed);
  std::uint64_t checksum = 0;
  for (std::size_t iteration = 0; iteration < 100'000; ++iteration) {
    const auto table = snapshot.FindTable("ITEMS");
    const auto column = table.has_value() ? snapshot.FindColumn(*table, "VaLuE")
                                          : std::optional<modern_sqlite::ColumnId>{};
    const auto missing = snapshot.FindIndex("missing");
    if (!table.has_value() || !column.has_value() || missing.has_value()) {
      return 1;
    }
    checksum += static_cast<std::uint64_t>(table->value + column->value + 1U);
  }
  const std::size_t after = allocation_count.load(std::memory_order_relaxed);
  return after == before && checksum != 0 ? 0 : 1;
}

}  // namespace

int main() {
  try {
    return RunAllocationTest();
  } catch (const std::exception& error) {
    std::fputs("catalog allocation test failed: ", stderr);
    std::fputs(error.what(), stderr);
    std::fputc('\n', stderr);
  } catch (...) {
    std::fputs("catalog allocation test failed with an unknown exception\n", stderr);
  }
  return 1;
}
