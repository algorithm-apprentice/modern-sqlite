#include <algorithm>
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

[[nodiscard]] modern_sqlite::LogicalPlan LogicalIndexFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog,
    std::string_view sql = "SELECT id, Name FROM Items WHERE Name=?1") {
  using namespace modern_sqlite;
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind optimizer index OOM fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to build optimizer index OOM fixture"};
  }
  return std::move(*logical);
}

[[nodiscard]] modern_sqlite::LogicalPlan LogicalOrderFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindSelectResult bound = BindSelectStatement(
      ParseTree("SELECT Name, Payload FROM Items ORDER BY Name LIMIT ?"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind optimizer ORDER BY OOM fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to build optimizer ORDER BY OOM fixture"};
  }
  return std::move(*logical);
}

[[nodiscard]] modern_sqlite::LogicalPlan LogicalCompoundFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog,
    std::string_view sql =
        "SELECT DISTINCT id,Name FROM Items "
        "UNION ALL SELECT id,Name FROM Items ORDER BY 1 LIMIT +1") {
  using namespace modern_sqlite;
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind optimizer compound OOM fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to build optimizer compound OOM fixture"};
  }
  return std::move(*logical);
}

[[nodiscard]] modern_sqlite::LogicalPlan LogicalAggregateFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindSelectResult bound =
      BindSelectStatement(ParseTree("SELECT Name,count() AS total FROM Items WHERE id>0 "
                                    "GROUP BY Name HAVING total>0 ORDER BY Name DESC LIMIT ?1"),
                          catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind optimizer aggregate OOM fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to build optimizer aggregate OOM fixture"};
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
  if (allocation_count != 9U) {
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

  const CatalogSnapshotPtr indexed_catalog = TestCatalog(true);
  LogicalPlan index_baseline_logical = LogicalIndexFixture(indexed_catalog);
  allocation_index.store(0, std::memory_order_relaxed);
  const OptimizeLogicalPlanResult index_baseline =
      OptimizeLogicalPlan(std::move(index_baseline_logical));
  if (!index_baseline.has_value() || index_baseline->selected_candidate() == nullptr ||
      index_baseline->selected_candidate()->kind != PhysicalAccessKind::kIndexScan) {
    return 1;
  }
  const std::size_t index_allocation_count = allocation_index.load(std::memory_order_relaxed);
  if (index_allocation_count == 0U || index_allocation_count > 64U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < index_allocation_count; ++failure) {
    LogicalPlan logical = LogicalIndexFixture(indexed_catalog);
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

  LogicalPlan noncovering_baseline_logical =
      LogicalIndexFixture(indexed_catalog, "SELECT Payload FROM Items WHERE Name=?1");
  allocation_index.store(0, std::memory_order_relaxed);
  const OptimizeLogicalPlanResult noncovering_baseline =
      OptimizeLogicalPlan(std::move(noncovering_baseline_logical));
  if (!noncovering_baseline.has_value() || noncovering_baseline->selected_candidate() == nullptr ||
      noncovering_baseline->selected_candidate()->kind != PhysicalAccessKind::kIndexScan ||
      noncovering_baseline->selected_candidate()->covering) {
    return 1;
  }
  const std::size_t noncovering_allocation_count = allocation_index.load(std::memory_order_relaxed);
  if (noncovering_allocation_count == 0U || noncovering_allocation_count > 64U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < noncovering_allocation_count; ++failure) {
    LogicalPlan logical =
        LogicalIndexFixture(indexed_catalog, "SELECT Payload FROM Items WHERE Name=?1");
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

  LogicalPlan order_baseline_logical = LogicalOrderFixture(catalog);
  allocation_index.store(0, std::memory_order_relaxed);
  const OptimizeLogicalPlanResult order_baseline =
      OptimizeLogicalPlan(std::move(order_baseline_logical));
  if (!order_baseline.has_value()) {
    return 1;
  }
  const std::size_t order_allocation_count = allocation_index.load(std::memory_order_relaxed);
  if (order_allocation_count != 7U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < order_allocation_count; ++failure) {
    LogicalPlan logical = LogicalOrderFixture(catalog);
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
  if (!OptimizeLogicalPlan(LogicalOrderFixture(catalog)).has_value()) {
    return 1;
  }

  const std::array<std::string_view, 35> advanced_sqls{
      "SELECT DISTINCT id,Name FROM Items UNION ALL SELECT id,Name FROM Items ORDER BY 1 LIMIT +1",
      "SELECT DISTINCT a.Name FROM Items a CROSS JOIN Items b "
      "WHERE a.id>?1 AND b.id>?2 ORDER BY 1 LIMIT ?3",
      "SELECT b.id,count(),max(a.id) FROM Items a CROSS JOIN Items b "
      "GROUP BY b.id HAVING count()>?1 ORDER BY 1 LIMIT ?2",
      "SELECT a.id,count(b.id) FROM Items a LEFT JOIN Items b ON a.id=b.id+?1 "
      "GROUP BY a.id ORDER BY 1",
      "SELECT id,count(b.id) FROM Items a LEFT JOIN Items b USING(id) GROUP BY id ORDER BY 1",
      "SELECT id,count(a.id) FROM Items a RIGHT JOIN Items b USING(id) GROUP BY id ORDER BY 1",
      "SELECT id,count(a.id) FROM Items a FULL JOIN Items b USING(id) GROUP BY id ORDER BY 1",
      "SELECT a.id,b.id FROM Items a JOIN Items b ON a.id>?1 AND b.id=a.id",
      "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Name=a.Name",
      "SELECT b.Name,count(a.id) FROM Items a FULL JOIN Items b ON b.Name IS a.Name "
      "GROUP BY b.Name ORDER BY b.Name",
      "SELECT a.id,b.id,c.id FROM Items a LEFT JOIN Items b ON b.Name=a.Name "
      "FULL JOIN Items c ON c.Name=b.Name",
      "SELECT (SELECT b.Name FROM Items a JOIN Items b ON b.Name=a.Name ORDER BY b.id LIMIT 1)",
      "SELECT b.Payload FROM Items a JOIN Items b ON b.Name=a.Name",
      "SELECT count(b.Payload) FROM Items a FULL JOIN Items b ON b.Name IS a.Name",
      "SELECT a.id,b.Payload,c.Payload FROM Items a LEFT JOIN Items b ON b.Name=a.Name "
      "FULL JOIN Items c ON c.Name=b.Name",
      "SELECT (SELECT b.Payload FROM Items a JOIN Items b ON b.Name=a.Name LIMIT 1)",
      "SELECT b.Name FROM Items a JOIN Items b ON b.Name>a.Name",
      "SELECT count(b.Payload) FROM Items a FULL JOIN Items b "
      "ON b.Name>=a.Name AND b.Name<=?1",
      "SELECT a.id,b.Payload,c.Payload FROM Items a LEFT JOIN Items b ON b.Name<=a.Name "
      "FULL JOIN Items c ON c.Name>b.Name",
      "SELECT (SELECT b.Payload FROM Items a JOIN Items b ON b.Name<a.Name LIMIT 1)",
      "SELECT (SELECT i.Payload FROM Items i WHERE i.Name=o.Name) FROM Items o",
      "SELECT (SELECT (SELECT o.Payload)) FROM Items o",
      "SELECT count(*),EXISTS(SELECT o.Payload ORDER BY o.Name) FROM Items o",
      "SELECT (SELECT group_concat(i.Name ORDER BY i.id) FROM Items i "
      "WHERE i.id<=o.id GROUP BY i.Name ORDER BY 1) FROM Items o",
      "SELECT 1 AS x FROM Items WHERE (SELECT (SELECT x))>0",
      "SELECT o.Name AS x FROM Items o WHERE EXISTS(SELECT x ORDER BY x LIMIT 1)",
      "SELECT o.Name AS x,count(*) FROM Items o GROUP BY o.Name HAVING (SELECT x) IS NOT NULL",
      "SELECT a.id,b.id,(SELECT coalesce(a.id,b.id)) FROM Items a FULL JOIN Items b "
      "ON b.id=a.id+?3 AND a.id<=?1 AND b.id<=?2 ORDER BY a.id IS NULL,a.id,b.id",
      "SELECT a.id,b.id,c.id FROM Items a JOIN Items b "
      "ON c.id>0 AND abs(a.id)>0 JOIN Items c ON c.id=a.id+1000",
      "SELECT a.id,b.id,c.id FROM Items a CROSS JOIN Items b CROSS JOIN Items c "
      "ORDER BY a.id DESC,b.id,c.id DESC LIMIT ?1",
      "SELECT a.id,b.Payload FROM Items a CROSS JOIN Items b WHERE a.id=1 "
      "AND b.Name>=?1 AND b.Name<=?2 ORDER BY b.Name DESC LIMIT 2",
      "SELECT Name,Payload FROM Items ORDER BY Name,id+0 LIMIT ?1 OFFSET ?2",
      "SELECT a.id,b.Name FROM Items a CROSS JOIN Items b WHERE a.id=1 "
      "ORDER BY b.Name,b.id+0 LIMIT ?1 OFFSET ?2",
      "SELECT a.id,b.id FROM Items a CROSS JOIN Items b ORDER BY b.id DESC LIMIT ?1 OFFSET ?2",
      "SELECT a.id,b.Name FROM Items a FULL JOIN Items b "
      "ON b.id>a.id AND b.id<=a.id+?1 ORDER BY b.id DESC LIMIT ?2",
  };
  for (const auto& fixture_catalog : {catalog, indexed_catalog}) {
    for (const std::string_view sql : advanced_sqls) {
      LogicalPlan compound_baseline_logical = LogicalCompoundFixture(fixture_catalog, sql);
      allocation_index.store(0, std::memory_order_relaxed);
      const OptimizeLogicalPlanResult compound_baseline =
          OptimizeLogicalPlan(std::move(compound_baseline_logical));
      if (!compound_baseline.has_value()) {
        return 1;
      }
      const std::size_t compound_allocation_count =
          allocation_index.load(std::memory_order_relaxed);
      if (compound_allocation_count == 0U || compound_allocation_count > 128U) {
        return 1;
      }
      for (std::size_t failure = 0; failure < compound_allocation_count; ++failure) {
        LogicalPlan logical = LogicalCompoundFixture(fixture_catalog, sql);
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
      if (!OptimizeLogicalPlan(LogicalCompoundFixture(fixture_catalog, sql)).has_value()) {
        return 1;
      }
    }
  }

  constexpr OptimizerOptions kAutomaticOptions{.allow_file_backed_automatic_indexes = true};
  constexpr std::array<std::string_view, 5> kAutomaticSqls{
      "SELECT a.id,b.Payload,c.Payload FROM Items a JOIN Items b ON b.Name=a.Name "
      "JOIN Items c ON c.Name=b.Name",
      "SELECT a.id,count(b.id),max(b.Payload) FROM Items a LEFT JOIN Items b "
      "ON b.Name IS a.Name GROUP BY a.id ORDER BY 1 LIMIT ?1",
      "SELECT a.id,b.Payload FROM Items a LEFT JOIN Items b "
      "ON b.Name IS a.Name AND b.id>1",
      "SELECT DISTINCT b.Name,b.Payload FROM Items a JOIN Items b ON b.Name=a.Name",
      "SELECT b.Payload FROM Items a JOIN Items b ON b.Name=a.Name "
      "UNION SELECT ?1 ORDER BY 1 LIMIT ?2 OFFSET ?3",
  };
  for (const std::string_view sql : kAutomaticSqls) {
    LogicalPlan automatic_logical = LogicalCompoundFixture(catalog, sql);
    allocation_index.store(0, std::memory_order_relaxed);
    const OptimizeLogicalPlanResult automatic =
        OptimizeLogicalPlan(std::move(automatic_logical), kAutomaticOptions);
    if (!automatic.has_value() ||
        !std::ranges::any_of(automatic->nodes(), [](const PhysicalNode& node) {
          return std::holds_alternative<PhysicalAutomaticIndexNode>(node.payload);
        })) {
      return 2;
    }
    const std::size_t automatic_allocation_count = allocation_index.load(std::memory_order_relaxed);
    if (automatic_allocation_count == 0U || automatic_allocation_count > 128U) {
      return 2;
    }
    for (std::size_t failure = 0; failure < automatic_allocation_count; ++failure) {
      LogicalPlan logical = LogicalCompoundFixture(catalog, sql);
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = failure;
      inject_failure = true;
      bool threw = false;
      try {
        [[maybe_unused]] const OptimizeLogicalPlanResult unexpected =
            OptimizeLogicalPlan(std::move(logical), kAutomaticOptions);
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      inject_failure = false;
      if (!threw) {
        return 2;
      }
    }
    if (!OptimizeLogicalPlan(LogicalCompoundFixture(catalog, sql), kAutomaticOptions).has_value()) {
      return 2;
    }
  }

  LogicalPlan aggregate_baseline_logical = LogicalAggregateFixture(indexed_catalog);
  allocation_index.store(0, std::memory_order_relaxed);
  const OptimizeLogicalPlanResult aggregate_baseline =
      OptimizeLogicalPlan(std::move(aggregate_baseline_logical));
  if (!aggregate_baseline.has_value()) {
    return 1;
  }
  const std::size_t aggregate_allocation_count = allocation_index.load(std::memory_order_relaxed);
  if (aggregate_allocation_count == 0U || aggregate_allocation_count > 256U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < aggregate_allocation_count; ++failure) {
    LogicalPlan logical = LogicalAggregateFixture(indexed_catalog);
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
  if (!OptimizeLogicalPlan(LogicalAggregateFixture(indexed_catalog)).has_value()) {
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
