#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "modern_sqlite/binder/bound_statement.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/syntax/parser.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::size_t failing_allocation = 0;
bool inject_failure = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && index == failing_allocation) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && index == failing_allocation) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] modern_sqlite::SyntaxTree ParseTree(std::string_view sql) {
  modern_sqlite::ParseResult parsed = modern_sqlite::ParseOne(modern_sqlite::Utf8View{sql});
  if (!parsed.has_value() || !parsed->tree.has_value()) {
    throw std::runtime_error{"failed to parse binder OOM fixture"};
  }
  return std::move(*parsed->tree);
}

[[nodiscard]] modern_sqlite::CatalogSnapshotPtr TestCatalog() {
  using namespace modern_sqlite;

  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 1, .generation = 1},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT COLLATE NOCASE)"));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = "Items",
      .root_page = RootPageId{2},
      .columns =
          {
              CatalogColumnInput{
                  .name = "id",
                  .declared_type = "INTEGER",
                  .primary_key = true,
              },
              CatalogColumnInput{
                  .name = "Name",
                  .declared_type = "TEXT",
                  .collation_name = "NOCASE",
              },
          },
      .rowid_alias = ColumnId{0},
  });
  CatalogSnapshotResult created = CatalogSnapshot::Create(std::move(input));
  if (!created.has_value()) {
    throw std::runtime_error{"failed to create binder OOM catalog"};
  }
  return *std::move(created);
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

  constexpr std::array kSql{
      std::string_view{"SELECT Name COLLATE NOCASE, abs(?), coalesce(NULL,Name), "
                       "x'00112233445566778899AABBCCDDEEFF' AS payload "
                       "FROM Items WHERE Name=? AND id>0 LIMIT ?"},
      std::string_view{"INSERT INTO Items(Name,id) VALUES(abs(?1),?2)"},
      std::string_view{"UPDATE Items SET Name=coalesce(?1,Name), id=id+1 WHERE Name=?2"},
      std::string_view{"DELETE FROM Items WHERE id=?1"},
      std::string_view{"CREATE TABLE NewItems(id INTEGER PRIMARY KEY, name TEXT DEFAULT 'x')"},
      std::string_view{"CREATE UNIQUE INDEX items_name ON Items(Name COLLATE NOCASE DESC)"},
      std::string_view{"ANALYZE Items"},
  };
  const CatalogSnapshotPtr catalog = TestCatalog();

  for (const std::string_view sql : kSql) {
    SyntaxTree baseline_tree = ParseTree(sql);
    allocation_index.store(0, std::memory_order_relaxed);
    const BindStatementResult baseline = BindStatement(std::move(baseline_tree), catalog);
    if (!baseline.has_value()) {
      return 1;
    }
    const std::size_t allocation_count = allocation_index.load(std::memory_order_relaxed);
    if (allocation_count == 0 || allocation_count > 1024U) {
      return 1;
    }

    const std::array<std::size_t, 5> failures{
        0,
        allocation_count / 4U,
        allocation_count / 2U,
        (allocation_count * 3U) / 4U,
        allocation_count - 1U,
    };
    std::optional<std::size_t> previous_failure;
    for (const std::size_t failure : failures) {
      if (previous_failure == failure) {
        continue;
      }
      previous_failure = failure;
      SyntaxTree tree = ParseTree(sql);
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = failure;
      inject_failure = true;
      bool threw = false;
      try {
        [[maybe_unused]] const BindStatementResult unexpected =
            BindStatement(std::move(tree), catalog);
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      inject_failure = false;
      if (!threw) {
        return 1;
      }
    }

    const BindStatementResult recovered = BindStatement(ParseTree(sql), catalog);
    if (!recovered.has_value()) {
      return 1;
    }
  }

  return 0;
} catch (...) {
  inject_failure = false;
  return 1;
}
