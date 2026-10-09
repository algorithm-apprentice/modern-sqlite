#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/optimizer/physical_plan.hpp"
#include "modern_sqlite/planner/logical_plan.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
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
    throw std::runtime_error{"failed to parse optimizer allocation fixture"};
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
      ParseTree("CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT, Payload BLOB)"));
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
              CatalogColumnInput{
                  .name = "Payload",
                  .declared_type = "BLOB",
              },
          },
      .rowid_alias = ColumnId{0},
  });
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
    throw std::runtime_error{"failed to create optimizer allocation catalog"};
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
    std::string_view sql, const modern_sqlite::CatalogSnapshotPtr& catalog,
    modern_sqlite::BindEnvironment environment = modern_sqlite::BindEnvironment::Core()) {
  using namespace modern_sqlite;
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog, environment);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind optimizer allocation fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to build optimizer allocation fixture"};
  }
  return std::move(*logical);
}

[[nodiscard]] modern_sqlite::LogicalStatementPlan LogicalMutationFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound =
      BindStatement(ParseTree("UPDATE Items SET id=id+1 "
                              "WHERE stable_guard(?1)=1 AND rowid=?2 AND Name=?3"),
                    catalog, TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind optimizer mutation allocation fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to build optimizer mutation allocation fixture"};
  }
  return std::move(*logical);
}

[[nodiscard]] std::size_t OptimizeAllocationCount(
    std::string_view sql, const modern_sqlite::CatalogSnapshotPtr& catalog,
    modern_sqlite::BindEnvironment environment = modern_sqlite::BindEnvironment::Core()) {
  modern_sqlite::LogicalPlan logical = LogicalFixture(sql, catalog, environment);
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  const modern_sqlite::OptimizeLogicalPlanResult physical =
      modern_sqlite::OptimizeLogicalPlan(std::move(logical));
  count_allocations = false;
  if (!physical.has_value()) {
    throw std::runtime_error{"failed to optimize allocation fixture"};
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
  const CatalogSnapshotPtr catalog = TestCatalog();

  if (OptimizeAllocationCount("SELECT Name FROM Items", catalog) != 4U) {
    return 1;
  }
  if (OptimizeAllocationCount("SELECT Name FROM Items WHERE Name=?", catalog) != 9U) {
    return 1;
  }
  if (OptimizeAllocationCount("SELECT Name FROM Items WHERE stable_guard(?)=1", catalog,
                              TestEnvironment()) != 7U) {
    return 1;
  }
  constexpr std::string_view kMixedSql =
      "SELECT Name FROM Items "
      "WHERE stable_guard(?)=1 AND rowid=volatile_key() AND Name=?";
  if (OptimizeAllocationCount(kMixedSql, catalog, TestEnvironment()) != 9U) {
    return 1;
  }
  const CatalogSnapshotPtr indexed_catalog = TestCatalog(true);
  const std::size_t index_allocations =
      OptimizeAllocationCount("SELECT id, Name FROM Items WHERE Name=?1", indexed_catalog);
  if (index_allocations != 14U) {
    return 1;
  }
  const std::size_t noncovering_allocations =
      OptimizeAllocationCount("SELECT Payload FROM Items WHERE Name=?1", indexed_catalog);
  if (noncovering_allocations != 14U) {
    return 1;
  }
  const std::size_t ordered_allocations =
      OptimizeAllocationCount("SELECT Name, Payload FROM Items ORDER BY Name LIMIT ?1", catalog);
  if (ordered_allocations != 7U) {
    return 1;
  }

  LogicalStatementPlan mutation_logical = LogicalMutationFixture(catalog);
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  OptimizeLogicalStatementPlanResult mutation_result =
      OptimizeLogicalStatementPlan(std::move(mutation_logical));
  count_allocations = false;
  if (!mutation_result.has_value()) {
    return 1;
  }
  const std::size_t mutation_allocations = allocation_count.load(std::memory_order_relaxed);
  if (mutation_allocations == 0U || mutation_allocations > 16U) {
    return 1;
  }
  PhysicalStatementPlan mutation_plan = std::move(*mutation_result);

  LogicalPlan logical = LogicalFixture(kMixedSql, catalog, TestEnvironment());
  OptimizeLogicalPlanResult built = OptimizeLogicalPlan(std::move(logical));
  if (!built.has_value()) {
    return 1;
  }
  std::unique_ptr<PhysicalPlan> published = std::make_unique<PhysicalPlan>(std::move(*built));
  LogicalPlan ordered_logical =
      LogicalFixture("SELECT Name, Payload FROM Items ORDER BY Name LIMIT ?1", catalog);
  OptimizeLogicalPlanResult ordered_built = OptimizeLogicalPlan(std::move(ordered_logical));
  if (!ordered_built.has_value()) {
    return 1;
  }
  std::unique_ptr<PhysicalPlan> ordered = std::make_unique<PhysicalPlan>(std::move(*ordered_built));

  std::uint64_t checksum = 0;
  fail_allocations = true;
  checksum += published->logical_plan().bound_select().source().size_bytes();
  checksum += published->nodes().size();
  checksum += published->candidates().size();
  checksum += published->root().value();
  checksum += published->selected_candidate_index();
  checksum += published->selected_candidate().cost.work_units;
  for (std::uint32_t index = 0; index < published->nodes().size(); ++index) {
    const PhysicalNode& node = published->node(PhysicalNodeId{index});
    checksum += static_cast<std::uint64_t>(PhysicalNodeKindOf(node));
    checksum += PhysicalNodeKindName(PhysicalNodeKindOf(node)).size();
    std::visit([&checksum](const auto&) { ++checksum; }, node.payload);
  }
  const auto& sort = std::get<PhysicalSortNode>(ordered->nodes()[1].payload);
  checksum += sort.terms.size();
  checksum += sort.payload_expressions.size();
  checksum += sort.output_fields.size();
  const PhysicalMutationPlan& mutation = std::get<PhysicalMutationPlan>(mutation_plan);
  const auto& update = std::get<PhysicalUpdateMutation>(mutation.payload());
  checksum += static_cast<std::uint64_t>(update.access.kind);
  checksum += update.access.guards.size();
  checksum += update.access.residuals.size();
  checksum += update.collect_original_rowids ? 1U : 0U;
  fail_allocations = false;

  return checksum == 0 ? 1 : 0;
} catch (...) {
  count_allocations = false;
  fail_allocations = false;
  return 1;
}
