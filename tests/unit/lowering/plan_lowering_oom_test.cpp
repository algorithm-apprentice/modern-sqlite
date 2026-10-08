#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/binder/bound_statement.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/lowering/plan_lowering.hpp"
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
    throw std::runtime_error{"failed to parse plan lowering OOM fixture"};
  }
  return std::move(*parsed->tree);
}

[[nodiscard]] modern_sqlite::CatalogSnapshotPtr TestCatalog(bool indexed = false) {
  using namespace modern_sqlite;
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 1, .generation = 1},
  };
  input.definitions.push_back(
      ParseTree(indexed ? "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT, Payload BLOB)"
                        : "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)"));
  if (indexed) {
    input.definitions.push_back(ParseTree("CREATE INDEX items_name ON Items(Name)"));
  }
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
  if (indexed) {
    input.tables.back().columns.push_back(CatalogColumnInput{
        .name = "Payload",
        .declared_type = "BLOB",
    });
  }
  if (indexed) {
    input.indexes.push_back(CatalogIndexInput{
        .definition = SchemaDefinitionId{1},
        .name = "items_name",
        .table = TableId{0},
        .root_page = RootPageId{3},
        .origin = IndexOrigin::kCreateIndex,
        .key_term_count = 1,
        .terms =
            {
                CatalogIndexTerm{
                    .target = ColumnId{1},
                    .collation_name = "BINARY",
                    .order = SortOrder::kAscending,
                },
                CatalogIndexTerm{
                    .target = RowIdIndexTerm{},
                    .collation_name = "BINARY",
                    .order = SortOrder::kAscending,
                },
            },
    });
  }
  CatalogSnapshotResult created = CatalogSnapshot::Create(std::move(input));
  if (!created.has_value()) {
    throw std::runtime_error{"failed to create lowering OOM catalog"};
  }
  return *std::move(created);
}

[[nodiscard]] modern_sqlite::Result<modern_sqlite::SqlValue> ReturnOne(
    const modern_sqlite::ScalarFunctionContext&, std::span<const modern_sqlite::SqlValue>) {
  return modern_sqlite::SqlValue::Integer(1);
}

[[nodiscard]] modern_sqlite::BindEnvironment TestEnvironment() {
  using namespace modern_sqlite;
  static const std::array<ScalarFunction, 1> functions{{
      ScalarFunction{"stable_guard", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ReturnOne},
  }};
  static const FunctionRegistry registry{functions};
  static const std::array<const Collation*, 1> collations{
      &BinaryCollation(),
  };
  return BindEnvironment{registry, collations, 1};
}

[[nodiscard]] modern_sqlite::PhysicalPlan PhysicalFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindSelectResult bound =
      BindSelectStatement(ParseTree("SELECT Name, ?, id+1 FROM Items "
                                    "WHERE stable_guard(?)=1 AND Name<>? LIMIT ? OFFSET ?"),
                          catalog, TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind lowering OOM fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan lowering OOM fixture"};
  }
  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical));
  if (!physical.has_value()) {
    throw std::runtime_error{"failed to optimize lowering OOM fixture"};
  }
  return std::move(*physical);
}

[[nodiscard]] modern_sqlite::PhysicalPlan PhysicalIndexFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog,
    std::string_view sql = "SELECT id, Name FROM Items WHERE Name=?1") {
  using namespace modern_sqlite;
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind covering index lowering OOM fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan covering index lowering OOM fixture"};
  }
  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical));
  if (!physical.has_value() ||
      physical->selected_candidate().kind != PhysicalAccessKind::kIndexScan) {
    throw std::runtime_error{"failed to optimize covering index lowering OOM fixture"};
  }
  return std::move(*physical);
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan MutationFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound = BindStatement(
      ParseTree("INSERT INTO Items(Name,id) VALUES(coalesce(?1,'fallback'),stable_guard(?2))"),
      catalog, TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind mutation lowering OOM fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan mutation lowering OOM fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize mutation lowering OOM fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan DeleteFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound =
      BindStatement(ParseTree("DELETE FROM Items WHERE stable_guard(?1)=1 AND Name<>?2"), catalog,
                    TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind DELETE lowering OOM fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan DELETE lowering OOM fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize DELETE lowering OOM fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan UpdateFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound =
      BindStatement(ParseTree("UPDATE Items SET id=id+10, Name=coalesce(?1,Name) "
                              "WHERE stable_guard(?2)=1 AND Name<>?3"),
                    catalog, TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind UPDATE lowering OOM fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan UPDATE lowering OOM fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize UPDATE lowering OOM fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan StableUpdateFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound = BindStatement(ParseTree("UPDATE Items SET Name=coalesce(?1,Name) "
                                                      "WHERE stable_guard(?2)=1 AND Name<>?3"),
                                            catalog, TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind stable UPDATE lowering OOM fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan stable UPDATE lowering OOM fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize stable UPDATE lowering OOM fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan CreateFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound = BindStatement(
      ParseTree("CREATE TABLE NewTable(id INTEGER PRIMARY KEY, Name TEXT DEFAULT 'seed')"), catalog,
      TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind CREATE TABLE lowering OOM fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan CREATE TABLE lowering OOM fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize CREATE TABLE lowering OOM fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan CreateIndexFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound =
      BindStatement(ParseTree("CREATE UNIQUE INDEX items_name ON Items(Name COLLATE BINARY DESC)"),
                    catalog, TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind CREATE INDEX lowering OOM fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan CREATE INDEX lowering OOM fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize CREATE INDEX lowering OOM fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
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
  const CatalogSnapshotPtr indexed_catalog = TestCatalog(true);
  const PhysicalPlan physical = PhysicalFixture(catalog);
  const PhysicalPlan index = PhysicalIndexFixture(indexed_catalog);
  const PhysicalPlan noncovering_index =
      PhysicalIndexFixture(indexed_catalog, "SELECT Payload FROM Items WHERE Name=?1");
  const PhysicalMutationPlan mutation = MutationFixture(catalog);
  const PhysicalMutationPlan indexed_insert = MutationFixture(indexed_catalog);
  const PhysicalMutationPlan deletion = DeleteFixture(catalog);
  const PhysicalMutationPlan indexed_deletion = DeleteFixture(indexed_catalog);
  const PhysicalMutationPlan update = UpdateFixture(catalog);
  const PhysicalMutationPlan indexed_update = UpdateFixture(indexed_catalog);
  const PhysicalMutationPlan stable_update = StableUpdateFixture(catalog);
  const PhysicalMutationPlan indexed_stable_update = StableUpdateFixture(indexed_catalog);
  const PhysicalMutationPlan create = CreateFixture(catalog);
  const PhysicalMutationPlan create_index = CreateIndexFixture(catalog);

  const auto verify_oom = [](const auto& plan) {
    allocation_index.store(0, std::memory_order_relaxed);
    const LowerPlanResult baseline = LowerPlan(plan);
    if (!baseline.has_value()) {
      return false;
    }
    const std::size_t allocation_count = allocation_index.load(std::memory_order_relaxed);
    if (allocation_count == 0U || allocation_count > 128U) {
      return false;
    }

    for (std::size_t failure = 0; failure < allocation_count; ++failure) {
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = failure;
      inject_failure = true;
      bool threw = false;
      try {
        [[maybe_unused]] const LowerPlanResult unexpected = LowerPlan(plan);
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      inject_failure = false;
      if (!threw) {
        return false;
      }
    }

    return LowerPlan(plan).has_value();
  };

  return verify_oom(physical) && verify_oom(index) && verify_oom(noncovering_index) &&
                 verify_oom(mutation) && verify_oom(indexed_insert) && verify_oom(deletion) &&
                 verify_oom(indexed_deletion) && verify_oom(update) && verify_oom(indexed_update) &&
                 verify_oom(stable_update) && verify_oom(indexed_stable_update) &&
                 verify_oom(create) && verify_oom(create_index)
             ? 0
             : 1;
} catch (...) {
  inject_failure = false;
  return 1;
}
