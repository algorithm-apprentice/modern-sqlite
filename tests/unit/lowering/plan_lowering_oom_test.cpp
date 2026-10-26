#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
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
#include "tests/support/aligned_allocation.hpp"

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
  if (modern_sqlite::test_support::AllocateAligned(&memory, alignment,
                                                   size == 0 ? alignment : size) == 0) {
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

[[nodiscard]] modern_sqlite::PhysicalPlan DistinctFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog, std::string_view sql,
    modern_sqlite::OptimizerOptions options = {}) {
  using namespace modern_sqlite;
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog, TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind DISTINCT lowering OOM fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan DISTINCT lowering OOM fixture"};
  }
  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical), options);
  if (!physical.has_value()) {
    throw std::runtime_error{"failed to optimize DISTINCT lowering OOM fixture"};
  }
  return std::move(*physical);
}

[[nodiscard]] modern_sqlite::PhysicalPlan AggregateFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog, std::string_view sql,
    modern_sqlite::OptimizerOptions options = {}) {
  using namespace modern_sqlite;
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind aggregate lowering OOM fixture"};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan aggregate lowering OOM fixture"};
  }
  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical), options);
  if (!physical.has_value()) {
    throw std::runtime_error{"failed to optimize aggregate lowering OOM fixture"};
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
  if (!physical.has_value() || physical->selected_candidate() == nullptr ||
      physical->selected_candidate()->kind != PhysicalAccessKind::kIndexScan) {
    throw std::runtime_error{"failed to optimize covering index lowering OOM fixture"};
  }
  return std::move(*physical);
}

[[nodiscard]] modern_sqlite::PhysicalMutationPlan MutationFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog,
    std::string_view sql =
        "INSERT INTO Items(Name,id) VALUES(coalesce(?1,'fallback'),stable_guard(?2))",
    modern_sqlite::BindEnvironment environment = TestEnvironment()) {
  using namespace modern_sqlite;
  BindStatementResult bound = BindStatement(ParseTree(sql), catalog, environment);
  if (!bound.has_value()) {
    throw std::runtime_error{bound.error().detail};
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

[[nodiscard]] modern_sqlite::PhysicalMutationPlan AnalyzeFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  using namespace modern_sqlite;
  BindStatementResult bound =
      BindStatement(ParseTree("ANALYZE items_name"), catalog, TestEnvironment());
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind ANALYZE lowering OOM fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to plan ANALYZE lowering OOM fixture"};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value() || !std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"failed to optimize ANALYZE lowering OOM fixture"};
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
void operator delete(void* memory, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}

int main() try {
  using namespace modern_sqlite;
  const CatalogSnapshotPtr catalog = TestCatalog();
  const CatalogSnapshotPtr indexed_catalog = TestCatalog(true);
  const PhysicalPlan physical = PhysicalFixture(catalog);
  const PhysicalPlan index = PhysicalIndexFixture(indexed_catalog);
  const PhysicalPlan noncovering_index =
      PhysicalIndexFixture(indexed_catalog, "SELECT Payload FROM Items WHERE Name=?1");
  const PhysicalPlan ordered_index = PhysicalIndexFixture(
      indexed_catalog, "SELECT Payload, Name FROM Items WHERE Name=?1 ORDER BY id DESC");
  const PhysicalPlan ordered_limit_index = PhysicalIndexFixture(
      indexed_catalog,
      "SELECT Payload, Name FROM Items WHERE Name=?1 ORDER BY id DESC LIMIT ?2 OFFSET ?3");
  const std::array<PhysicalPlan, 20> aggregate_plans{
      AggregateFixture(catalog, "SELECT count(),sum(id) FROM Items WHERE Name<>?1"),
      AggregateFixture(catalog, "SELECT sum((SELECT 1)) AS x FROM Items ORDER BY x"),
      AggregateFixture(catalog,
                       "SELECT sum((SELECT 1)) AS x FROM Items "
                       "HAVING sum((SELECT 2))>0 "
                       "UNION ALL SELECT 0 ORDER BY x"),
      AggregateFixture(catalog, "SELECT (SELECT sum(o.id)) FROM Items o"),
      AggregateFixture(catalog,
                       "SELECT sum((SELECT 1)) AS x FROM Items "
                       "HAVING (SELECT x)>0 ORDER BY (SELECT x)"),
      AggregateFixture(catalog, "SELECT EXISTS(SELECT 1 ORDER BY sum(o.id)) FROM Items o"),
      AggregateFixture(catalog,
                       "SELECT ("
                       "SELECT sum((SELECT 1)+0*o.id)+sum((SELECT 1)+0*i.id) AS x "
                       "FROM Items i UNION ALL SELECT 0 ORDER BY x"
                       ") FROM Items o"),
      AggregateFixture(catalog, "SELECT max(id) FROM Items"),
      AggregateFixture(indexed_catalog, "SELECT min(Name) FROM Items"),
      AggregateFixture(indexed_catalog, "SELECT min(Name) FROM Items WHERE Payload=?1"),
      AggregateFixture(indexed_catalog, "SELECT Payload,min(Name) FROM Items"),
      AggregateFixture(indexed_catalog,
                       "SELECT Name,count(),min(id) FROM Items "
                       "GROUP BY Name ORDER BY Name LIMIT ?1 OFFSET ?2"),
      AggregateFixture(catalog,
                       "SELECT Name||'',count(),min(id) FROM Items "
                       "GROUP BY Name||'' ORDER BY count() DESC LIMIT ?1"),
      AggregateFixture(catalog,
                       "SELECT count(DISTINCT Name) FILTER (WHERE id>?1),"
                       "group_concat(Name ORDER BY id DESC) FROM Items"),
      AggregateFixture(catalog,
                       "SELECT Name||'',group_concat(DISTINCT Name ORDER BY Name) "
                       "FROM Items GROUP BY Name||''"),
      AggregateFixture(catalog,
                       "SELECT Name||'',group_concat(DISTINCT Name ORDER BY id),"
                       "count(DISTINCT id) FILTER (WHERE id>?1) "
                       "FROM Items GROUP BY Name||'' ORDER BY 1"),
      AggregateFixture(catalog, "SELECT DISTINCT count() FROM Items GROUP BY Name ORDER BY 1"),
      AggregateFixture(catalog,
                       "SELECT count() FROM Items UNION ALL SELECT sum(id) FROM Items ORDER BY 1"),
      AggregateFixture(catalog,
                       "SELECT count() FROM Items GROUP BY Name "
                       "UNION SELECT count() FROM Items LIMIT 1"),
      AggregateFixture(catalog,
                       "SELECT count() FROM Items UNION ALL SELECT sum(id) FROM Items "
                       "LIMIT ?1 OFFSET ?2"),
  };
  const PhysicalPlan distinct =
      DistinctFixture(catalog, "SELECT DISTINCT Name, stable_guard(?) FROM Items LIMIT ?");
  const PhysicalPlan ordered_distinct = DistinctFixture(
      catalog, "SELECT DISTINCT Name FROM Items ORDER BY stable_guard(?) LIMIT ? OFFSET ?");
  const PhysicalPlan values_union =
      DistinctFixture(catalog, "VALUES(?1,?2),(?3,?4) UNION ALL SELECT ?5,?6 LIMIT ?7 OFFSET ?8");
  const PhysicalPlan table_union =
      DistinctFixture(catalog, "SELECT Name FROM Items UNION ALL SELECT 'tail' LIMIT ? OFFSET ?");
  const PhysicalPlan distinct_union =
      DistinctFixture(catalog, "SELECT DISTINCT Name FROM Items UNION ALL SELECT 'tail' LIMIT ?");
  const std::array<PhysicalPlan, 5> set_plans{
      DistinctFixture(catalog, "SELECT Name FROM Items UNION SELECT ?1"),
      DistinctFixture(catalog, "SELECT Name FROM Items EXCEPT SELECT ?1"),
      DistinctFixture(catalog, "SELECT Name FROM Items INTERSECT SELECT ?1"),
      DistinctFixture(catalog,
                      "VALUES(?1),(?2) UNION ALL SELECT ?3 EXCEPT SELECT ?4 UNION SELECT ?5"),
      DistinctFixture(catalog, "SELECT Name FROM Items UNION SELECT ?1 LIMIT 1"),
  };
  const std::array<PhysicalPlan, 5> ordered_compound_plans{
      DistinctFixture(catalog,
                      "SELECT Name FROM Items UNION ALL SELECT ?1 ORDER BY 1 LIMIT ?2 OFFSET ?3"),
      DistinctFixture(catalog, "SELECT Name FROM Items UNION SELECT ?1 ORDER BY 1 LIMIT 1"),
      DistinctFixture(catalog,
                      "SELECT Name FROM Items EXCEPT SELECT ?1 INTERSECT SELECT ?2 ORDER BY 1"),
      DistinctFixture(catalog,
                      "SELECT Name FROM Items UNION SELECT ?1 "
                      "ORDER BY 1 COLLATE BINARY LIMIT ?2"),
      DistinctFixture(catalog,
                      "VALUES(?1,?2),(?3,?4) UNION ALL SELECT ?5,?6 ORDER BY 1 LIMIT ?7 OFFSET ?8"),
  };
  const std::array<PhysicalPlan, 38> joined_plans{
      DistinctFixture(catalog,
                      "SELECT a.id,b.id FROM Items a JOIN Items b ON a.id>?1 AND b.id=a.id"),
      DistinctFixture(catalog,
                      "SELECT a.id,b.Name FROM Items a CROSS JOIN Items b "
                      "WHERE a.id>?1 AND b.id>?2 LIMIT ?3 OFFSET ?4"),
      DistinctFixture(catalog, "SELECT DISTINCT b.Name FROM Items a,Items b ORDER BY 1"),
      DistinctFixture(catalog,
                      "SELECT a.Name FROM Items a CROSS JOIN Items b "
                      "UNION SELECT ?1 ORDER BY 1"),
      DistinctFixture(catalog,
                      "SELECT a.Name FROM Items a CROSS JOIN Items b "
                      "UNION SELECT ?1 LIMIT 1"),
      DistinctFixture(catalog,
                      "SELECT id,a.Name FROM Items a RIGHT JOIN Items b USING(id) "
                      "ORDER BY id LIMIT ?1"),
      AggregateFixture(catalog,
                       "SELECT b.id,count(a.id) FROM Items a RIGHT JOIN Items b ON a.id=b.id "
                       "RIGHT JOIN Items c ON b.id=c.id GROUP BY b.id ORDER BY b.id"),
      DistinctFixture(catalog,
                      "SELECT id,a.Name FROM Items a FULL JOIN Items b USING(id) "
                      "ORDER BY id LIMIT ?1"),
      AggregateFixture(catalog,
                       "SELECT b.id,count(a.id) FROM Items a FULL JOIN Items b ON 0 "
                       "FULL JOIN Items c ON b.id=c.id GROUP BY b.id ORDER BY b.id"),
      AggregateFixture(catalog,
                       "SELECT a.id,a._rowid_,b.id,b._rowid_,max(b.id-a.id) "
                       "FROM Items a CROSS JOIN Items b"),
      AggregateFixture(catalog,
                       "SELECT b.id,group_concat(a.Name ORDER BY a.id) "
                       "FROM Items a CROSS JOIN Items b GROUP BY b.id ORDER BY 1"),
      AggregateFixture(catalog,
                       "SELECT count() FROM Items a CROSS JOIN Items b "
                       "UNION SELECT count() FROM Items LIMIT 1"),
      DistinctFixture(catalog,
                      "SELECT a.Name,b.id FROM Items a LEFT JOIN Items b "
                      "ON b.id=a.id+1 WHERE b.id IS NULL OR a.id>?1 "
                      "ORDER BY a.Name LIMIT ?2"),
      AggregateFixture(catalog,
                       "SELECT a.id,count(b.id),group_concat(b.Name ORDER BY b.id) "
                       "FROM Items a LEFT JOIN Items b ON b.id=a.id+1 "
                       "GROUP BY a.id HAVING count()>?1 ORDER BY 1 LIMIT ?2"),
      DistinctFixture(catalog,
                      "SELECT id,b.Name FROM Items a LEFT JOIN Items b USING(id) "
                      "ORDER BY id LIMIT ?1"),
      AggregateFixture(catalog,
                       "SELECT count() FROM Items a NATURAL LEFT JOIN Items b "
                       "UNION SELECT ?1 LIMIT 1"),
      DistinctFixture(indexed_catalog,
                      "SELECT a.id,b.id,c.id FROM Items a JOIN Items b ON b.Name=a.Name "
                      "JOIN Items c ON c.Name=b.Name ORDER BY a.id,b.id,c.id"),
      AggregateFixture(indexed_catalog,
                       "SELECT b.Name,count(a.id) FROM Items a FULL JOIN Items b "
                       "ON b.Name IS a.Name GROUP BY b.Name ORDER BY b.Name"),
      DistinctFixture(indexed_catalog,
                      "SELECT a.id,b.id,c.id FROM Items a LEFT JOIN Items b ON b.Name=a.Name "
                      "FULL JOIN Items c ON c.Name=b.Name"),
      DistinctFixture(indexed_catalog,
                      "SELECT (SELECT b.Name FROM Items a JOIN Items b ON b.Name=a.Name "
                      "ORDER BY b.id LIMIT 1)"),
      DistinctFixture(indexed_catalog,
                      "SELECT b.Payload FROM Items a JOIN Items b ON b.Name=a.Name"),
      AggregateFixture(indexed_catalog,
                       "SELECT count(b.Payload) FROM Items a FULL JOIN Items b "
                       "ON b.Name IS a.Name"),
      DistinctFixture(indexed_catalog,
                      "SELECT a.id,b.Payload,c.Payload FROM Items a LEFT JOIN Items b "
                      "ON b.Name=a.Name FULL JOIN Items c ON c.Name=b.Name"),
      DistinctFixture(indexed_catalog,
                      "SELECT (SELECT b.Payload FROM Items a JOIN Items b ON b.Name=a.Name "
                      "LIMIT 1)"),
      DistinctFixture(indexed_catalog, "SELECT b.Name FROM Items a JOIN Items b ON b.Name>a.Name"),
      AggregateFixture(indexed_catalog,
                       "SELECT count(b.Payload) FROM Items a FULL JOIN Items b "
                       "ON b.Name>=a.Name AND b.Name<=?1"),
      DistinctFixture(indexed_catalog,
                      "SELECT a.id,b.Payload,c.Payload FROM Items a LEFT JOIN Items b "
                      "ON b.Name<=a.Name FULL JOIN Items c ON c.Name>b.Name"),
      DistinctFixture(indexed_catalog,
                      "SELECT (SELECT b.Payload FROM Items a JOIN Items b ON b.Name<a.Name "
                      "LIMIT 1)"),
      DistinctFixture(catalog,
                      "SELECT a.id,b.id,(SELECT coalesce(a.id,b.id)),(SELECT a.Name),"
                      "(SELECT b.Name) FROM Items a FULL JOIN Items b "
                      "ON b.id=a.id+?3 AND a.id<=?1 AND b.id<=?2 "
                      "ORDER BY a.id IS NULL,a.id,b.id"),
      AggregateFixture(catalog,
                       "SELECT count() FROM Items a LEFT JOIN Items b "
                       "ON abs(-9223372036854775808) AND b.id=a.id+1000"),
      AggregateFixture(catalog,
                       "SELECT count() FROM Items a JOIN Items b "
                       "ON a.id<0 AND b.id=abs(a.id-a.id-9223372036854775807-1)"),
      AggregateFixture(catalog,
                       "SELECT count() FROM Items a JOIN Items x "
                       "ON abs(a.id-a.id-9223372036854775807-1) "
                       "RIGHT JOIN Items b ON x.id=b.id"),
      DistinctFixture(catalog,
                      "SELECT a.id,b.id,c.id FROM Items a CROSS JOIN Items b CROSS JOIN Items c "
                      "ORDER BY a.id DESC,b.id,c.id DESC LIMIT ?1"),
      DistinctFixture(indexed_catalog,
                      "SELECT a.id,b.Payload FROM Items a CROSS JOIN Items b WHERE a.id=1 "
                      "AND b.Name>=?1 AND b.Name<=?2 ORDER BY b.Name DESC LIMIT 2"),
      DistinctFixture(indexed_catalog,
                      "SELECT Name,Payload FROM Items ORDER BY Name,id+0 LIMIT ?1 OFFSET ?2"),
      DistinctFixture(indexed_catalog,
                      "SELECT a.id,b.Name FROM Items a CROSS JOIN Items b WHERE a.id=1 "
                      "ORDER BY b.Name,b.id+0 LIMIT ?1 OFFSET ?2"),
      DistinctFixture(catalog,
                      "SELECT a.id,b.id FROM Items a CROSS JOIN Items b "
                      "ORDER BY b.id DESC LIMIT ?1 OFFSET ?2"),
      DistinctFixture(catalog,
                      "SELECT a.id,b.Name FROM Items a FULL JOIN Items b "
                      "ON b.id>a.id AND b.id<=a.id+?1 ORDER BY b.id DESC LIMIT ?2"),
  };
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
  const PhysicalMutationPlan analyze = AnalyzeFixture(indexed_catalog);
  const std::array mutation_query_plans{
      MutationFixture(catalog, "INSERT INTO Items VALUES(?1,(SELECT Name FROM Items WHERE id=?2))"),
      MutationFixture(catalog,
                      "UPDATE Items SET Name=(SELECT (SELECT q.Name) FROM Items q "
                      "ORDER BY q.id DESC) WHERE EXISTS(SELECT ?1)"),
      MutationFixture(catalog, "DELETE FROM Items WHERE Name=(SELECT min(Name) FROM Items)",
                      BindEnvironment::Core()),
  };
  constexpr OptimizerOptions kAutomaticOptions{.allow_file_backed_automatic_indexes = true};
  const std::array<PhysicalPlan, 7> automatic_plans{
      DistinctFixture(catalog,
                      "SELECT a.id,b.Name,c.Name FROM Items a JOIN Items b ON b.Name=a.Name "
                      "JOIN Items c ON c.Name=b.Name",
                      kAutomaticOptions),
      DistinctFixture(catalog,
                      "SELECT a.id,b.Name FROM Items a LEFT JOIN Items b ON b.Name IS a.Name "
                      "ORDER BY a.id,b.id LIMIT ?1 OFFSET ?2",
                      kAutomaticOptions),
      DistinctFixture(catalog,
                      "SELECT a.id,b.Name FROM Items a LEFT JOIN Items b "
                      "ON b.Name IS a.Name AND b.id>1",
                      kAutomaticOptions),
      DistinctFixture(catalog, "SELECT DISTINCT b.Name FROM Items a JOIN Items b ON b.Name=a.Name",
                      kAutomaticOptions),
      DistinctFixture(catalog,
                      "SELECT b.Name FROM Items a JOIN Items b ON b.Name=a.Name "
                      "UNION SELECT ?1 ORDER BY 1",
                      kAutomaticOptions),
      DistinctFixture(catalog,
                      "SELECT (SELECT b.Name FROM Items a JOIN Items b ON b.Name=a.Name "
                      "ORDER BY b.id LIMIT 1),"
                      "EXISTS(SELECT b.Name FROM Items a JOIN Items b ON b.Name IS a.Name)",
                      kAutomaticOptions),
      AggregateFixture(catalog,
                       "SELECT a.id,count(DISTINCT b.Name),group_concat(b.Name ORDER BY b.id) "
                       "FROM Items a LEFT JOIN Items b ON b.Name IS a.Name "
                       "GROUP BY a.id ORDER BY a.id",
                       kAutomaticOptions),
  };
  const std::array<PhysicalPlan, 8> correlated_plans{
      AggregateFixture(catalog,
                       "SELECT (SELECT (SELECT o.Name)),"
                       "EXISTS(SELECT 1 ORDER BY (SELECT o.Name)) FROM Items o"),
      AggregateFixture(catalog, "SELECT count(*),EXISTS(SELECT o.Name ORDER BY o.id) FROM Items o"),
      AggregateFixture(catalog,
                       "SELECT (SELECT group_concat(i.Name ORDER BY i.id) FROM Items i "
                       "WHERE i.id<=o.id GROUP BY i.Name ORDER BY 1 LIMIT ?1) AS x "
                       "FROM Items o WHERE x IS NOT NULL"),
      AggregateFixture(indexed_catalog,
                       "SELECT (SELECT i.Payload FROM Items i WHERE i.Name=o.Name) FROM Items o"),
      AggregateFixture(catalog,
                       "SELECT (SELECT b.Name||o.Name FROM Items a JOIN Items b ON b.Name=a.Name "
                       "WHERE a.id=o.id ORDER BY b.id LIMIT ?1) FROM Items o",
                       kAutomaticOptions),
      AggregateFixture(catalog, "SELECT 1 AS x FROM Items WHERE (SELECT (SELECT x))>0"),
      AggregateFixture(catalog,
                       "SELECT Name AS x FROM Items WHERE "
                       "EXISTS(SELECT x ORDER BY (SELECT x) LIMIT 1)"),
      AggregateFixture(catalog,
                       "SELECT Name AS x,count(*) FROM Items GROUP BY Name "
                       "HAVING (SELECT x) IS NOT NULL"),
  };
  const std::array<PhysicalPlan, 8> null_range_plans{
      PhysicalIndexFixture(indexed_catalog, "SELECT Name FROM Items WHERE Name<?1"),
      PhysicalIndexFixture(indexed_catalog, "SELECT Payload FROM Items WHERE Name<?1"),
      AggregateFixture(indexed_catalog, "SELECT count(*) FROM Items WHERE Name<?1"),
      AggregateFixture(indexed_catalog, "SELECT max(Name) FROM Items WHERE Name<?1"),
      AggregateFixture(indexed_catalog,
                       "SELECT Name,count(*) FROM Items WHERE Name<?1 GROUP BY Name"),
      DistinctFixture(indexed_catalog,
                      "SELECT (SELECT Payload FROM Items WHERE Name<?1),"
                      "EXISTS(SELECT 1 FROM Items WHERE Name<?2)"),
      DistinctFixture(indexed_catalog,
                      "SELECT a.id,b.Payload FROM Items a LEFT JOIN Items b ON b.Name<a.Name"),
      AggregateFixture(indexed_catalog,
                       "SELECT count(b.Payload) FROM Items a FULL JOIN Items b ON b.Name<a.Name"),
  };
  const std::array<PhysicalPlan, 7> rowid_range_plans{
      DistinctFixture(indexed_catalog, "SELECT Payload FROM Items WHERE id<?1"),
      AggregateFixture(indexed_catalog, "SELECT count(*) FROM Items WHERE id>=?1"),
      AggregateFixture(indexed_catalog, "SELECT id,count(*) FROM Items WHERE id>?1 GROUP BY id"),
      DistinctFixture(indexed_catalog, "SELECT DISTINCT Name FROM Items WHERE id<=?1"),
      DistinctFixture(indexed_catalog,
                      "SELECT id FROM Items WHERE id<?1 "
                      "UNION ALL SELECT id FROM Items WHERE id>?2"),
      DistinctFixture(indexed_catalog,
                      "SELECT (SELECT Payload FROM Items WHERE id<?1),"
                      "EXISTS(SELECT 1 FROM Items WHERE id>?2)"),
      DistinctFixture(indexed_catalog, "SELECT id FROM Items WHERE id=?1 UNION SELECT 99 LIMIT 1"),
  };
  const std::array null_order_plans{
      PhysicalIndexFixture(indexed_catalog, "SELECT Name FROM Items ORDER BY Name NULLS LAST"),
      PhysicalIndexFixture(indexed_catalog,
                           "SELECT Payload FROM Items ORDER BY Name DESC NULLS FIRST"),
      DistinctFixture(indexed_catalog,
                      "SELECT (SELECT Payload FROM Items ORDER BY Name NULLS LAST LIMIT 1)"),
      DistinctFixture(indexed_catalog,
                      "SELECT Name FROM Items UNION ALL SELECT ?1 ORDER BY 1 NULLS LAST"),
      DistinctFixture(indexed_catalog,
                      "SELECT b.Payload FROM Items a LEFT JOIN Items b ON b.Payload=a.Payload "
                      "ORDER BY a.id,b.Name NULLS LAST"),
  };

  const auto verify_oom = [](const auto& plan, std::size_t maximum_allocations = 128U,
                             bool require_lookup = false) {
    allocation_index.store(0, std::memory_order_relaxed);
    const LowerPlanResult baseline = LowerPlan(plan);
    if (!baseline.has_value()) {
      std::fprintf(stderr, "lowering OOM baseline failed: %s\n", baseline.error().detail.c_str());
      return false;
    }
    if (require_lookup &&
        !std::ranges::any_of(baseline->relations(), [](const OrderingRecordDescriptor& relation) {
          return relation.relation_purpose == RelationPurpose::kLookup;
        })) {
      return false;
    }
    const std::size_t allocation_count = allocation_index.load(std::memory_order_relaxed);
    if (allocation_count == 0U || allocation_count > maximum_allocations) {
      std::fprintf(stderr, "lowering OOM baseline allocations=%zu limit=%zu\n", allocation_count,
                   maximum_allocations);
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
        std::fprintf(stderr, "lowering OOM allocation prefix=%zu/%zu did not throw\n", failure,
                     allocation_count);
        return false;
      }
    }

    return LowerPlan(plan).has_value();
  };

  if (!verify_oom(values_union)) {
    return 1;
  }
  if (!verify_oom(table_union)) {
    return 1;
  }
  if (!verify_oom(distinct_union)) {
    return 1;
  }
  for (const PhysicalPlan& set_plan : set_plans) {
    if (!verify_oom(set_plan, 320U)) {
      return 1;
    }
  }
  for (const PhysicalPlan& ordered_plan : ordered_compound_plans) {
    if (!verify_oom(ordered_plan, 512U)) {
      return 1;
    }
  }
  for (const PhysicalPlan& aggregate_plan : aggregate_plans) {
    if (!verify_oom(aggregate_plan, 256U)) {
      return 1;
    }
  }
  for (const PhysicalPlan& joined_plan : joined_plans) {
    if (!verify_oom(joined_plan, 256U)) {
      return 1;
    }
  }
  for (const PhysicalPlan& automatic_plan : automatic_plans) {
    if (!verify_oom(automatic_plan, 256U, true)) {
      return 2;
    }
  }
  for (const PhysicalPlan& correlated_plan : correlated_plans) {
    if (!verify_oom(correlated_plan, 256U)) {
      return 3;
    }
  }
  for (const PhysicalPlan& range_plan : null_range_plans) {
    if (!verify_oom(range_plan, 256U)) {
      return 1;
    }
  }
  for (const PhysicalPlan& range_plan : rowid_range_plans) {
    if (!verify_oom(range_plan, 256U)) {
      return 1;
    }
  }
  for (const PhysicalPlan& plan : null_order_plans) {
    if (!verify_oom(plan)) {
      return 1;
    }
  }
  for (const PhysicalMutationPlan& plan : mutation_query_plans) {
    if (!verify_oom(plan, 256U)) {
      return 1;
    }
  }

  return verify_oom(physical) && verify_oom(index) && verify_oom(noncovering_index) &&
                 verify_oom(ordered_index) && verify_oom(ordered_limit_index) &&
                 verify_oom(distinct) && verify_oom(ordered_distinct) && verify_oom(mutation) &&
                 verify_oom(indexed_insert) && verify_oom(deletion) &&
                 verify_oom(indexed_deletion) && verify_oom(update) && verify_oom(indexed_update) &&
                 verify_oom(stable_update) && verify_oom(indexed_stable_update) &&
                 verify_oom(create) && verify_oom(create_index) && verify_oom(analyze)
             ? 0
             : 1;
} catch (const std::exception& error) {
  inject_failure = false;
  std::fprintf(stderr, "lowering OOM setup failed: %s\n", error.what());
  return 1;
} catch (...) {
  inject_failure = false;
  std::fprintf(stderr, "lowering OOM setup failed with a nonstandard exception\n");
  return 1;
}
