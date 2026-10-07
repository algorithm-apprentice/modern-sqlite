#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/optimizer/physical_plan.hpp"
#include "modern_sqlite/planner/logical_plan.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
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
    throw std::runtime_error{"failed to parse optimizer OOM fixture"};
  }
  return std::move(*parsed->tree);
}

[[nodiscard]] modern_sqlite::CatalogSnapshotPtr TestCatalog() {
  using namespace modern_sqlite;
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 1, .generation = 1},
  };
  input.definitions.push_back(ParseTree("CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)"));
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
              },
          },
      .rowid_alias = ColumnId{0},
  });
  CatalogSnapshotResult created = CatalogSnapshot::Create(std::move(input));
  if (!created.has_value()) {
    throw std::runtime_error{"failed to create optimizer OOM catalog"};
  }
  return *std::move(created);
}

[[nodiscard]] modern_sqlite::Result<modern_sqlite::SqlValue> ReturnOne(
    const modern_sqlite::ScalarFunctionContext&, std::span<const modern_sqlite::SqlValue>) {
  return modern_sqlite::SqlValue::Integer(1);
}

[[nodiscard]] modern_sqlite::BindEnvironment TestEnvironment() {
  using namespace modern_sqlite;
  static const std::array<ScalarFunction, 2> functions{{
      ScalarFunction{"stable_guard", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ReturnOne},
      ScalarFunction{"volatile_key", FunctionArity::Exact(0),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     ReturnOne},
  }};
  static const FunctionRegistry registry{functions};
  static const std::array<const Collation*, 1> collations{&BinaryCollation()};
  return BindEnvironment{registry, collations, 1};
}

[[nodiscard]] modern_sqlite::LogicalPlan LogicalFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindSelectResult bound =
      BindSelectStatement(ParseTree("SELECT Name FROM Items "
                                    "WHERE stable_guard(?)=1 AND rowid=volatile_key() AND Name=?"),
                          catalog, TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind optimizer OOM fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to build optimizer OOM fixture"};
  }
  return std::move(*logical);
}

[[nodiscard]] modern_sqlite::LogicalStatementPlan LogicalMutationFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound =
      BindStatement(ParseTree("DELETE FROM Items "
                              "WHERE stable_guard(?)=1 AND rowid=volatile_key() AND Name=?"),
                    catalog, TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind optimizer mutation OOM fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to build optimizer mutation OOM fixture"};
  }
  return std::move(*logical);
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
  const CatalogSnapshotPtr catalog = TestCatalog();

  LogicalPlan baseline_logical = LogicalFixture(catalog);
  allocation_index.store(0, std::memory_order_relaxed);
  const OptimizeLogicalPlanResult baseline = OptimizeLogicalPlan(std::move(baseline_logical));
  if (!baseline.has_value()) {
    return 1;
  }
  const std::size_t allocation_count = allocation_index.load(std::memory_order_relaxed);
  if (allocation_count != 4U) {
    return 1;
  }

  for (std::size_t failure = 0; failure < allocation_count; ++failure) {
    LogicalPlan logical = LogicalFixture(catalog);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool threw = false;
    try {
      [[maybe_unused]] const OptimizeLogicalPlanResult unexpected =
          OptimizeLogicalPlan(std::move(logical));
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    inject_failure = false;
    if (!threw) {
      return 1;
    }
  }

  LogicalPlan recovered_logical = LogicalFixture(catalog);
  const OptimizeLogicalPlanResult recovered = OptimizeLogicalPlan(std::move(recovered_logical));
  if (!recovered.has_value()) {
    return 1;
  }

  LogicalStatementPlan mutation_baseline = LogicalMutationFixture(catalog);
  allocation_index.store(0, std::memory_order_relaxed);
  const OptimizeLogicalStatementPlanResult mutation =
      OptimizeLogicalStatementPlan(std::move(mutation_baseline));
  if (!mutation.has_value()) {
    return 1;
  }
  const std::size_t mutation_allocations = allocation_index.load(std::memory_order_relaxed);
  if (mutation_allocations == 0U || mutation_allocations > 16U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < mutation_allocations; ++failure) {
    LogicalStatementPlan logical = LogicalMutationFixture(catalog);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool threw = false;
    try {
      [[maybe_unused]] const OptimizeLogicalStatementPlanResult unexpected =
          OptimizeLogicalStatementPlan(std::move(logical));
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    inject_failure = false;
    if (!threw) {
      return 1;
    }
  }
  return OptimizeLogicalStatementPlan(LogicalMutationFixture(catalog)).has_value() ? 0 : 1;
} catch (...) {
  inject_failure = false;
  return 1;
}
