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
#include "modern_sqlite/binder/bound_statement.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/lowering/plan_lowering.hpp"
#include "modern_sqlite/planner/logical_plan.hpp"
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
    throw std::runtime_error{"failed to parse plan lowering allocation fixture"};
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
    throw std::runtime_error{"failed to create lowering allocation catalog"};
  }
  return *std::move(created);
}

[[nodiscard]] modern_sqlite::PhysicalPlan PhysicalFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog, std::string_view sql) {
  using namespace modern_sqlite;
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind lowering allocation fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan lowering allocation fixture"};
  }
  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical));
  if (!physical.has_value()) {
    throw std::runtime_error{"failed to optimize lowering allocation fixture"};
  }
  return std::move(*physical);
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan MutationFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound =
      BindStatement(ParseTree("INSERT INTO Items(Name,id) VALUES(?1,?2)"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind mutation lowering allocation fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan mutation lowering allocation fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize mutation lowering allocation fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan DeleteFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound = BindStatement(ParseTree("DELETE FROM Items WHERE Name=?1"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind DELETE lowering allocation fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan DELETE lowering allocation fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize DELETE lowering allocation fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan UpdateFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound =
      BindStatement(ParseTree("UPDATE Items SET id=id+10 WHERE Name=?1"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind UPDATE lowering allocation fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan UPDATE lowering allocation fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize UPDATE lowering allocation fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan StableUpdateFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound =
      BindStatement(ParseTree("UPDATE Items SET Name=?1 WHERE Name<>?2"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind stable UPDATE lowering allocation fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan stable UPDATE lowering allocation fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize stable UPDATE lowering allocation fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan CreateFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound = BindStatement(
      ParseTree("CREATE TABLE NewTable(id INTEGER PRIMARY KEY, Name TEXT DEFAULT 'seed')"),
      catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind CREATE TABLE lowering allocation fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan CREATE TABLE lowering allocation fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize CREATE TABLE lowering allocation fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan CreateIndexFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound =
      BindStatement(ParseTree("CREATE UNIQUE INDEX items_name ON Items(Name DESC)"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind CREATE INDEX lowering allocation fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan CREATE INDEX lowering allocation fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize CREATE INDEX lowering allocation fixture"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan AnalyzeFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound = BindStatement(ParseTree("ANALYZE items_name"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind ANALYZE lowering allocation fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan ANALYZE lowering allocation fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize ANALYZE lowering allocation fixture"};
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
  const std::array<PhysicalPlan, 8> physical_plans{
      PhysicalFixture(catalog, "SELECT 1"),
      PhysicalFixture(catalog, "SELECT Name FROM Items"),
      PhysicalFixture(catalog, "SELECT Name FROM Items WHERE rowid=?1"),
      PhysicalFixture(catalog, "SELECT Name FROM Items LIMIT ?1 OFFSET ?2"),
      PhysicalFixture(indexed_catalog, "SELECT id, Name FROM Items WHERE Name=?1"),
      PhysicalFixture(indexed_catalog, "SELECT Payload FROM Items WHERE Name=?1"),
      PhysicalFixture(indexed_catalog,
                      "SELECT Payload, Name FROM Items WHERE Name=?1 ORDER BY id DESC"),
      PhysicalFixture(indexed_catalog,
                      "SELECT Payload, Name FROM Items WHERE Name=?1 "
                      "ORDER BY id DESC LIMIT ?2 OFFSET ?3"),
  };
  constexpr std::array<std::size_t, 8> kExpectedAllocations{17U, 28U, 27U, 34U, 34U, 39U, 46U, 59U};
  std::unique_ptr<BytecodeProgram> published;
  for (std::size_t plan_index = 0; plan_index < physical_plans.size(); ++plan_index) {
    for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
      allocation_count.store(0, std::memory_order_relaxed);
      count_allocations = true;
      const LowerPlanResult lowered = LowerPlan(physical_plans[plan_index]);
      count_allocations = false;
      if (!lowered.has_value()) {
        return 1;
      }
      const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
      if (allocations != kExpectedAllocations[plan_index]) {
        return 1;
      }
    }
  }

  const std::array<PhysicalPlan, 2> distinct_plans{
      PhysicalFixture(catalog, "SELECT DISTINCT Name FROM Items LIMIT ?1"),
      PhysicalFixture(catalog, "SELECT DISTINCT Name FROM Items ORDER BY id LIMIT ?1 OFFSET ?2"),
  };
  std::array<std::size_t, 2> distinct_allocations{};
  std::unique_ptr<BytecodeProgram> distinct_published;
  for (std::size_t plan_index = 0; plan_index < distinct_plans.size(); ++plan_index) {
    for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
      allocation_count.store(0, std::memory_order_relaxed);
      count_allocations = true;
      LowerPlanResult lowered = LowerPlan(distinct_plans[plan_index]);
      count_allocations = false;
      if (!lowered.has_value()) {
        return 1;
      }
      const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
      if (allocations == 0U || allocations > 128U ||
          (distinct_allocations[plan_index] != 0U &&
           allocations != distinct_allocations[plan_index])) {
        return 1;
      }
      distinct_allocations[plan_index] = allocations;
      if (plan_index == 1U && distinct_published == nullptr) {
        distinct_published = std::make_unique<BytecodeProgram>(std::move(*lowered));
      }
    }
  }

  const std::array<PhysicalPlan, 3> streaming_plans{
      PhysicalFixture(catalog, "VALUES(?1,?2),(?3,?4) UNION ALL SELECT ?5,?6 LIMIT ?7 OFFSET ?8"),
      PhysicalFixture(catalog, "SELECT Name FROM Items UNION ALL SELECT 'tail' LIMIT ?1 OFFSET ?2"),
      PhysicalFixture(catalog, "SELECT DISTINCT Name FROM Items UNION ALL SELECT 'tail' LIMIT ?1"),
  };
  std::array<std::size_t, 3> streaming_allocations{};
  std::unique_ptr<BytecodeProgram> streaming_published;
  for (std::size_t plan_index = 0; plan_index < streaming_plans.size(); ++plan_index) {
    for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
      allocation_count.store(0, std::memory_order_relaxed);
      count_allocations = true;
      LowerPlanResult lowered = LowerPlan(streaming_plans[plan_index]);
      count_allocations = false;
      if (!lowered.has_value()) {
        return 1;
      }
      const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
      if (allocations == 0U || allocations > 192U ||
          (streaming_allocations[plan_index] != 0U &&
           allocations != streaming_allocations[plan_index])) {
        return 1;
      }
      streaming_allocations[plan_index] = allocations;
      if (plan_index == 2U && streaming_published == nullptr) {
        streaming_published = std::make_unique<BytecodeProgram>(std::move(*lowered));
      }
    }
  }

  const PhysicalMutationPlan mutation = MutationFixture(catalog);
  constexpr std::size_t kExpectedMutationAllocations = 21U;
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    LowerPlanResult lowered = LowerPlan(mutation);
    count_allocations = false;
    if (!lowered.has_value()) {
      return 1;
    }
    const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
    if (allocations != kExpectedMutationAllocations) {
      return 1;
    }
    if (published == nullptr) {
      published = std::make_unique<BytecodeProgram>(std::move(*lowered));
    }
  }

  const PhysicalMutationPlan indexed_insert = MutationFixture(indexed_catalog);
  constexpr std::size_t kExpectedIndexedInsertAllocations = 29U;
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    const LowerPlanResult lowered = LowerPlan(indexed_insert);
    count_allocations = false;
    if (!lowered.has_value()) {
      return 1;
    }
    const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
    if (allocations != kExpectedIndexedInsertAllocations) {
      return 1;
    }
  }

  const auto verify_mutation_allocations = [](const PhysicalMutationPlan& plan,
                                              std::size_t expected) {
    for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
      allocation_count.store(0, std::memory_order_relaxed);
      count_allocations = true;
      const LowerPlanResult lowered = LowerPlan(plan);
      count_allocations = false;
      if (!lowered.has_value() || allocation_count.load(std::memory_order_relaxed) != expected) {
        return false;
      }
    }
    return true;
  };
  const PhysicalMutationPlan indexed_deletion = DeleteFixture(indexed_catalog);
  const PhysicalMutationPlan indexed_update = UpdateFixture(indexed_catalog);
  const PhysicalMutationPlan indexed_stable_update = StableUpdateFixture(indexed_catalog);
  constexpr std::size_t kExpectedIndexedDeleteAllocations = 38U;
  constexpr std::size_t kExpectedIndexedUpdateAllocations = 40U;
  constexpr std::size_t kExpectedIndexedStableUpdateAllocations = 38U;
  if (!verify_mutation_allocations(indexed_deletion, kExpectedIndexedDeleteAllocations) ||
      !verify_mutation_allocations(indexed_update, kExpectedIndexedUpdateAllocations) ||
      !verify_mutation_allocations(indexed_stable_update,
                                   kExpectedIndexedStableUpdateAllocations)) {
    return 1;
  }

  const PhysicalMutationPlan deletion = DeleteFixture(catalog);
  constexpr std::size_t kExpectedDeleteAllocations = 27U;
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    const LowerPlanResult lowered = LowerPlan(deletion);
    count_allocations = false;
    if (!lowered.has_value()) {
      return 1;
    }
    const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
    if (allocations != kExpectedDeleteAllocations) {
      return 1;
    }
  }

  const PhysicalMutationPlan update = UpdateFixture(catalog);
  constexpr std::size_t kExpectedUpdateAllocations = 36U;
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    const LowerPlanResult lowered = LowerPlan(update);
    count_allocations = false;
    if (!lowered.has_value()) {
      return 1;
    }
    const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
    if (allocations != kExpectedUpdateAllocations) {
      return 1;
    }
  }

  const PhysicalMutationPlan stable_update = StableUpdateFixture(catalog);
  constexpr std::size_t kExpectedStableUpdateAllocations = 32U;
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    const LowerPlanResult lowered = LowerPlan(stable_update);
    count_allocations = false;
    if (!lowered.has_value()) {
      return 1;
    }
    const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
    if (allocations != kExpectedStableUpdateAllocations) {
      return 1;
    }
  }

  const PhysicalMutationPlan create = CreateFixture(catalog);
  constexpr std::size_t kExpectedCreateAllocations = 23U;
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    const LowerPlanResult lowered = LowerPlan(create);
    count_allocations = false;
    if (!lowered.has_value()) {
      return 1;
    }
    const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
    if (allocations != kExpectedCreateAllocations) {
      return 1;
    }
  }

  const PhysicalMutationPlan create_index = CreateIndexFixture(catalog);
  constexpr std::size_t kExpectedCreateIndexAllocations = 36U;
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    const LowerPlanResult lowered = LowerPlan(create_index);
    count_allocations = false;
    if (!lowered.has_value() ||
        allocation_count.load(std::memory_order_relaxed) != kExpectedCreateIndexAllocations) {
      return 1;
    }
  }

  const PhysicalMutationPlan analyze = AnalyzeFixture(indexed_catalog);
  constexpr std::size_t kExpectedAnalyzeAllocations = 37U;
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    const LowerPlanResult lowered = LowerPlan(analyze);
    count_allocations = false;
    if (!lowered.has_value() ||
        allocation_count.load(std::memory_order_relaxed) != kExpectedAnalyzeAllocations) {
      return 1;
    }
  }
  if (published == nullptr || distinct_published == nullptr || streaming_published == nullptr) {
    return 1;
  }

  std::uint64_t checksum = 0;
  fail_allocations = true;
  checksum += published->register_count();
  checksum += published->parameter_count();
  checksum += published->constants().size();
  checksum += published->symbols().size();
  checksum += published->cursors().size();
  checksum += published->write_cursors().size();
  checksum += published->result_columns().size();
  checksum += published->instructions().size();
  checksum += published->verification_metrics().reachable_instruction_count;
  for (const Instruction& instruction : published->instructions()) {
    const InstructionKind kind = InstructionKindOf(instruction);
    checksum += static_cast<std::uint64_t>(kind);
    checksum += InstructionKindName(kind).size();
  }
  checksum += distinct_published->relations().size();
  checksum += distinct_published->sorters().size();
  checksum += distinct_published->instructions().size();
  checksum += distinct_published->relation(RelationId{0}).field_count;
  checksum += distinct_published->sorter(SorterId{0}).field_count;
  checksum += streaming_published->cursors().size();
  checksum += streaming_published->relations().size();
  checksum += streaming_published->instructions().size();
  fail_allocations = false;

  return checksum == 0 ? 1 : 0;
} catch (...) {
  count_allocations = false;
  fail_allocations = false;
  return 1;
}
