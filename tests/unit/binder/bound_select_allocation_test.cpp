#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "modern_sqlite/binder/bound_statement.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/syntax/parser.hpp"

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

[[nodiscard]] modern_sqlite::SyntaxTree ParseTree(std::string_view sql) {
  modern_sqlite::ParseResult parsed = modern_sqlite::ParseOne(modern_sqlite::Utf8View{sql});
  if (!parsed.has_value() || !parsed->tree.has_value()) {
    throw std::runtime_error{"failed to parse binder allocation fixture"};
  }
  return std::move(*parsed->tree);
}

[[nodiscard]] modern_sqlite::CatalogSnapshotPtr TestCatalog() {
  using namespace modern_sqlite;

  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 1, .generation = 1},
  };
  input.definitions.push_back(ParseTree(
      "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT COLLATE NOCASE, Score REAL)"));
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
              CatalogColumnInput{
                  .name = "Score",
                  .declared_type = "REAL",
              },
          },
      .rowid_alias = ColumnId{0},
  });
  CatalogSnapshotResult created = CatalogSnapshot::Create(std::move(input));
  if (!created.has_value()) {
    throw std::runtime_error{"failed to create binder allocation catalog"};
  }
  return *std::move(created);
}

[[nodiscard]] std::size_t BindAllocationCount(std::string_view sql,
                                              const modern_sqlite::CatalogSnapshotPtr& catalog) {
  modern_sqlite::SyntaxTree tree = ParseTree(sql);
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  const modern_sqlite::BindSelectResult bound =
      modern_sqlite::BindSelectStatement(std::move(tree), catalog);
  count_allocations = false;
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind allocation fixture"};
  }
  return allocation_count.load(std::memory_order_relaxed);
}

[[nodiscard]] std::size_t BindStatementAllocationCount(
    std::string_view sql, const modern_sqlite::CatalogSnapshotPtr& catalog) {
  modern_sqlite::SyntaxTree tree = ParseTree(sql);
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  const modern_sqlite::BindStatementResult bound =
      modern_sqlite::BindStatement(std::move(tree), catalog);
  count_allocations = false;
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind statement allocation fixture"};
  }
  return allocation_count.load(std::memory_order_relaxed);
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

  constexpr std::string_view kSql =
      "SELECT *, abs(?1) AS magnitude, coalesce(NULL,Name), "
      "iif(Score>0,Name,'none'), likelihood(Score,0.25) "
      "FROM Items WHERE Name=?2 OR id=?3 LIMIT ?4";
  const CatalogSnapshotPtr catalog = TestCatalog();

  const std::size_t one_name_allocations = BindAllocationCount("SELECT Name FROM Items", catalog);
  const std::size_t repeated_name_allocations = BindAllocationCount(
      "SELECT Name,Name,Name,Name,Name,Name,Name,Name,"
      "Name,Name,Name,Name,Name,Name,Name,Name,"
      "Name,Name,Name,Name,Name,Name,Name,Name,"
      "Name,Name,Name,Name,Name,Name,Name,Name "
      "FROM Items",
      catalog);
  if (repeated_name_allocations > one_name_allocations + 16U) {
    return 1;
  }
  const std::size_t update_allocations = BindStatementAllocationCount(
      "UPDATE Items SET Name=coalesce(?1,Name), id=id+1 WHERE Score>?2", catalog);
  const std::size_t create_allocations = BindStatementAllocationCount(
      "CREATE TABLE NewItems(id INTEGER PRIMARY KEY, name TEXT DEFAULT 'x')", catalog);
  const std::size_t create_index_allocations = BindStatementAllocationCount(
      "CREATE UNIQUE INDEX items_name ON Items(Name COLLATE NOCASE DESC)", catalog);
  const std::size_t analyze_allocations = BindStatementAllocationCount("ANALYZE Items", catalog);
  if (update_allocations == 0 || update_allocations > 256U || create_allocations == 0 ||
      create_allocations > 256U || create_index_allocations == 0 ||
      create_index_allocations > 256U || analyze_allocations == 0 || analyze_allocations > 256U) {
    return 1;
  }

  std::optional<std::size_t> expected_allocations;
  std::unique_ptr<BoundSelect> published;

  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    SyntaxTree tree = ParseTree(kSql);
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    BindSelectResult bound = BindSelectStatement(std::move(tree), catalog);
    count_allocations = false;
    if (!bound.has_value()) {
      return 1;
    }
    const std::size_t current = allocation_count.load(std::memory_order_relaxed);
    if (current == 0 || current > 256U) {
      return 1;
    }
    if (expected_allocations.has_value() && current != *expected_allocations) {
      return 1;
    }
    expected_allocations = current;
    if (published == nullptr) {
      published = std::make_unique<BoundSelect>(std::move(*bound));
    }
  }
  if (published == nullptr) {
    return 1;
  }
  const BoundSelect& select = *published;
  BindStatementResult update_result =
      BindStatement(ParseTree("UPDATE Items SET Name=?1, id=id+1 WHERE Score>?2"), catalog);
  if (!update_result.has_value() || !std::holds_alternative<BoundUpdate>(*update_result)) {
    return 1;
  }
  BoundStatement update_statement = std::move(*update_result);
  const BoundUpdate& update = std::get<BoundUpdate>(update_statement);

  std::uint64_t checksum = 0;
  fail_allocations = true;
  checksum += select.source().size_bytes();
  checksum += select.source_columns().size();
  checksum += select.collations().size();
  checksum += select.functions().size();
  checksum += select.parameters().size();
  checksum += select.result_columns().size();
  for (const BoundExpression& expression : select.expressions()) {
    checksum += static_cast<std::uint64_t>(BoundExpressionKindOf(expression));
    checksum += BoundExpressionKindName(BoundExpressionKindOf(expression)).size();
    std::visit([&checksum](const auto&) { ++checksum; }, expression.payload);
  }
  checksum += update.source().size_bytes();
  checksum += update.target().columns.size();
  checksum += update.assignments().size();
  checksum += update.parameters().size();
  checksum += update.expressions().size();
  checksum += update.where_expression().has_value() ? 1U : 0U;
  fail_allocations = false;

  return checksum == 0 ? 1 : 0;
} catch (...) {
  count_allocations = false;
  fail_allocations = false;
  return 1;
}
