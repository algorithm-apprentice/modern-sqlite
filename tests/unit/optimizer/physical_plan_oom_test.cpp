#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/optimizer/physical_plan.hpp"
#include "modern_sqlite/planner/logical_plan.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/syntax/parser.hpp"
#include "tests/support/aligned_allocation.hpp"
#include "tests/support/compiler_allocation_fault.hpp"

// Keep allocation-failure diagnostics independent of the C++ allocator under test.
// NOLINTBEGIN(modernize-use-std-print)

namespace {

namespace allocation_fault = modern_sqlite::test_support::compiler_allocation_fault;

using allocation_fault::Allocate;
using allocation_fault::AllocateAligned;
using allocation_fault::allocation_index;
using allocation_fault::allocation_tracking_active;
using allocation_fault::failing_allocation;
using allocation_fault::inject_failure;
using allocation_fault::registered_allocation_thread;
using allocation_fault::unregistered_allocation;

struct FixturePhaseTiming {
  std::chrono::steady_clock::duration parse{};
  std::chrono::steady_clock::duration bind{};
  std::chrono::steady_clock::duration logical{};
  std::size_t fixtures = 0;
};

thread_local FixturePhaseTiming compound_fixture_timing;

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
  ++compound_fixture_timing.fixtures;
  auto phase_started = std::chrono::steady_clock::now();
  SyntaxTree tree = ParseTree(sql);
  compound_fixture_timing.parse += std::chrono::steady_clock::now() - phase_started;
  phase_started = std::chrono::steady_clock::now();
  BindSelectResult bound = BindSelectStatement(std::move(tree), catalog);
  compound_fixture_timing.bind += std::chrono::steady_clock::now() - phase_started;
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind optimizer compound OOM fixture"};
  }
  phase_started = std::chrono::steady_clock::now();
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  compound_fixture_timing.logical += std::chrono::steady_clock::now() - phase_started;
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
    const modern_sqlite::CatalogSnapshotPtr& catalog,
    std::string_view sql =
        "DELETE FROM Items WHERE stable_guard(?)=1 AND rowid=volatile_key() AND Name=?",
    modern_sqlite::BindEnvironment environment = TestEnvironment()) {
  using namespace modern_sqlite;
  BindStatementResult bound = BindStatement(ParseTree(sql), catalog, environment);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind optimizer mutation OOM fixture"};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{"failed to build optimizer mutation OOM fixture"};
  }
  return std::move(*logical);
}

[[nodiscard]] bool VerifyAllocationThreadDomains() {
  const auto* main_timing = &compound_fixture_timing;
  return allocation_fault::VerifyThreadDomains(
      [main_timing] { return &compound_fixture_timing != main_timing; });
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

int main(int argc, char* argv[]) try {
  using namespace modern_sqlite;
  allocation_fault::owner_name = "optimizer";
  registered_allocation_thread = true;
  allocation_tracking_active.store(true, std::memory_order_relaxed);
  if (argc == 2 && std::string_view{argv[1]} == "--verify-thread-domains") {
    return VerifyAllocationThreadDomains() ? 0 : 1;
  }
  if (argc != 1) {
    std::fprintf(stderr, "optimizer OOM unexpected command-line arguments\n");
    return 1;
  }
  const auto started = std::chrono::steady_clock::now();
  const auto report_stage = [started](const char* stage) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    std::fprintf(stderr, "optimizer OOM stage=%s elapsed-ms=%lld\n", stage,
                 static_cast<long long>(elapsed));
  };
  const CatalogSnapshotPtr catalog = TestCatalog();

  LogicalPlan baseline_logical = LogicalFixture(catalog);
  allocation_index.store(0, std::memory_order_relaxed);
  const OptimizeLogicalPlanResult baseline = OptimizeLogicalPlan(std::move(baseline_logical));
  if (!baseline.has_value()) {
    std::fprintf(stderr, "optimizer OOM baseline failed: %s\n", baseline.error().detail.c_str());
    return 1;
  }
  const std::size_t allocation_count = allocation_index.load(std::memory_order_relaxed);
  if (allocation_count != 9U) {
    std::fprintf(stderr, "optimizer OOM baseline expected 9 allocations, got %zu\n",
                 allocation_count);
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
  report_stage("simple-and-order-complete");

  std::string proof_depth_sql =
      "SELECT a.id FROM Items a LEFT JOIN Items b ON b.id=a.id WHERE b.id>0";
  for (std::size_t term = 1; term < 256U; ++term) {
    proof_depth_sql += " OR b.id>0";
  }
  proof_depth_sql += " LIMIT 0";
  const std::array<std::string_view, 48> advanced_sqls{
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
      "SELECT Name FROM Items ORDER BY Name NULLS LAST",
      "SELECT Payload FROM Items ORDER BY Name DESC NULLS FIRST",
      "SELECT Name FROM Items UNION ALL SELECT ?1 ORDER BY 1 NULLS LAST",
      "SELECT b.Payload FROM Items a LEFT JOIN Items b ON b.Payload=a.Payload "
      "ORDER BY a.id,b.Name NULLS LAST",
      "SELECT 0 AS no FROM Items a FULL JOIN Items b ON no AND 1",
      "SELECT (SELECT c.id) AS later FROM Items a LEFT JOIN Items b ON later CROSS JOIN Items c",
      "SELECT a.id,b.id FROM Items a FULL JOIN Items b ON b.id=a.id "
      "WHERE a.id IS NOT NULL AND b.id IS NOT NULL",
      "SELECT a.id,b.id FROM Items a FULL JOIN Items b ON b.id=a.id WHERE a.id>0 AND b.id>0",
      "SELECT b.id,count() FROM Items a LEFT JOIN Items b ON b.id=a.id "
      "GROUP BY b.id HAVING b.id IS NOT NULL",
      "SELECT a.id FROM Items a LEFT JOIN Items b ON abs(-9223372036854775808) OR 1 "
      "WHERE b.id IS NOT NULL",
      proof_depth_sql,
      "SELECT 1 FROM Items a LEFT JOIN Items b "
      "ON 1 IN (abs(-9223372036854775808) OR 1,2) WHERE b.id IS NOT NULL LIMIT 1",
      "SELECT 1 FROM Items a LEFT JOIN Items b "
      "ON 1 IN (abs(-9223372036854775808) OR 1) WHERE b.id IS NOT NULL LIMIT 1",
  };
  struct AdvancedSweepResult {
    bool passed = false;
    std::size_t cases = 0;
    std::size_t prefixes = 0;
    std::size_t expected_prefixes = 0;
    std::size_t recoveries = 0;
  };
  std::array<AdvancedSweepResult, 2> advanced_results;
  const std::array<CatalogSnapshotPtr, 2> fixture_catalogs{catalog, indexed_catalog};
  const auto verify_advanced = [&](std::size_t catalog_index) {
    const auto& fixture_catalog = fixture_catalogs[catalog_index];
    auto& result = advanced_results[catalog_index];
    std::size_t case_index = 0;
    for (const std::string_view sql : advanced_sqls) {
      compound_fixture_timing = {};
      std::chrono::steady_clock::duration fixture_time{};
      std::chrono::steady_clock::duration optimize_time{};
      LogicalPlan compound_baseline_logical = LogicalCompoundFixture(fixture_catalog, sql);
      allocation_index.store(0, std::memory_order_relaxed);
      const OptimizeLogicalPlanResult compound_baseline =
          OptimizeLogicalPlan(std::move(compound_baseline_logical));
      if (!compound_baseline.has_value()) {
        std::fprintf(stderr, "advanced optimizer OOM baseline failed: %s; SQL=%.*s\n",
                     compound_baseline.error().detail.c_str(), static_cast<int>(sql.size()),
                     sql.data());
        return false;
      }
      const std::size_t compound_allocation_count =
          allocation_index.load(std::memory_order_relaxed);
      if (compound_allocation_count == 0U || compound_allocation_count > 128U) {
        std::fprintf(stderr, "advanced optimizer OOM allocations=%zu limit=128; SQL=%.*s\n",
                     compound_allocation_count, static_cast<int>(sql.size()), sql.data());
        return false;
      }
      result.expected_prefixes += compound_allocation_count;
      std::fprintf(stderr, "optimizer OOM catalog=%zu advanced-case=%zu allocations=%zu begin\n",
                   catalog_index, case_index, compound_allocation_count);
      for (std::size_t failure = 0; failure < compound_allocation_count; ++failure) {
        const auto fixture_started = std::chrono::steady_clock::now();
        LogicalPlan logical = LogicalCompoundFixture(fixture_catalog, sql);
        fixture_time += std::chrono::steady_clock::now() - fixture_started;
        allocation_index.store(0, std::memory_order_relaxed);
        failing_allocation = failure;
        inject_failure = true;
        bool threw = false;
        const auto optimize_started = std::chrono::steady_clock::now();
        try {
          [[maybe_unused]] const OptimizeLogicalPlanResult unexpected =
              OptimizeLogicalPlan(std::move(logical));
        } catch (const std::bad_alloc&) {
          threw = true;
        }
        inject_failure = false;
        optimize_time += std::chrono::steady_clock::now() - optimize_started;
        if (!threw) {
          std::fprintf(stderr, "advanced optimizer OOM prefix=%zu/%zu did not throw; SQL=%.*s\n",
                       failure, compound_allocation_count, static_cast<int>(sql.size()),
                       sql.data());
          return false;
        }
        ++result.prefixes;
      }
      if (!OptimizeLogicalPlan(LogicalCompoundFixture(fixture_catalog, sql)).has_value()) {
        std::fprintf(stderr, "advanced optimizer OOM recovery failed; SQL=%.*s\n",
                     static_cast<int>(sql.size()), sql.data());
        return false;
      }
      if (compound_fixture_timing.fixtures != compound_allocation_count + 2U) {
        std::fprintf(stderr, "advanced optimizer OOM fixture count did not match\n");
        return false;
      }
      ++result.recoveries;
      std::fprintf(
          stderr,
          "optimizer OOM catalog=%zu advanced-case=%zu fixture-us=%lld optimize-us=%lld "
          "parse-us=%lld bind-us=%lld logical-us=%lld complete\n",
          catalog_index, case_index,
          static_cast<long long>(
              std::chrono::duration_cast<std::chrono::microseconds>(fixture_time).count()),
          static_cast<long long>(
              std::chrono::duration_cast<std::chrono::microseconds>(optimize_time).count()),
          static_cast<long long>(
              std::chrono::duration_cast<std::chrono::microseconds>(compound_fixture_timing.parse)
                  .count()),
          static_cast<long long>(
              std::chrono::duration_cast<std::chrono::microseconds>(compound_fixture_timing.bind)
                  .count()),
          static_cast<long long>(
              std::chrono::duration_cast<std::chrono::microseconds>(compound_fixture_timing.logical)
                  .count()));
      ++case_index;
    }
    result.cases = case_index;
    return result.cases == advanced_sqls.size() && result.recoveries == advanced_sqls.size() &&
           result.prefixes == result.expected_prefixes;
  };
  [[maybe_unused]] const BindEnvironment core_environment = BindEnvironment::Core();
  {
    std::array<std::jthread, 2> workers;
    for (std::size_t catalog_index = 0; catalog_index < workers.size(); ++catalog_index) {
      workers[catalog_index] = std::jthread([&, catalog_index] {
        registered_allocation_thread = true;
        try {
          advanced_results[catalog_index].passed = verify_advanced(catalog_index);
        } catch (const std::exception& error) {
          inject_failure = false;
          std::fprintf(stderr, "optimizer OOM catalog=%zu unexpected exception: %s\n",
                       catalog_index, error.what());
        } catch (...) {
          inject_failure = false;
          std::fprintf(stderr, "optimizer OOM catalog=%zu unexpected nonstandard exception\n",
                       catalog_index);
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
  }
  if (unregistered_allocation.load(std::memory_order_relaxed)) {
    return 1;
  }

  for (std::size_t catalog_index = 0; catalog_index < advanced_results.size(); ++catalog_index) {
    const auto& result = advanced_results[catalog_index];
    std::fprintf(stderr,
                 "optimizer OOM catalog=%zu cases=%zu prefixes=%zu recoveries=%zu passed=%d\n",
                 catalog_index, result.cases, result.prefixes, result.recoveries,
                 static_cast<int>(result.passed));
    if (!result.passed) {
      return 1;
    }
  }
  report_stage("advanced-complete");

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
    report_stage("automatic-complete");
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
  report_stage("aggregate-complete");

  const auto verify_mutation = [&](std::string_view sql, std::size_t maximum,
                                   BindEnvironment environment) {
    LogicalStatementPlan mutation_baseline = LogicalMutationFixture(catalog, sql, environment);
    allocation_index.store(0, std::memory_order_relaxed);
    const OptimizeLogicalStatementPlanResult mutation =
        OptimizeLogicalStatementPlan(std::move(mutation_baseline));
    if (!mutation.has_value()) {
      return false;
    }
    const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
    if (allocations == 0U || allocations > maximum) {
      return false;
    }
    for (std::size_t failure = 0; failure < allocations; ++failure) {
      LogicalStatementPlan logical = LogicalMutationFixture(catalog, sql, environment);
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
        return false;
      }
    }
    return OptimizeLogicalStatementPlan(LogicalMutationFixture(catalog, sql, environment))
        .has_value();
  };
  if (!verify_mutation(
          "DELETE FROM Items WHERE stable_guard(?)=1 AND rowid=volatile_key() AND Name=?", 16U,
          TestEnvironment())) {
    return 1;
  }
  for (const std::string_view sql : {
           "INSERT INTO Items(id,Name) VALUES(?1,(SELECT Name FROM Items WHERE id=?2))",
           "UPDATE Items SET Name=(SELECT (SELECT q.Name) FROM Items q ORDER BY q.id DESC) "
           "WHERE EXISTS(SELECT ?1)",
           "DELETE FROM Items WHERE Name=(SELECT min(Name) FROM Items)",
           "DELETE FROM Items WHERE abs(abs(abs(id+0+0+0+0)))",
           "UPDATE Items SET Name='x' WHERE "
           "abs(abs(abs(id+((SELECT ?1) COLLATE BINARY)+0+0+0+0)))",
       }) {
    if (!verify_mutation(sql, 128U, BindEnvironment::Core())) {
      return 1;
    }
  }
  report_stage("mutations-complete");
  return unregistered_allocation.load(std::memory_order_relaxed) ? 1 : 0;
} catch (const std::exception& error) {
  inject_failure = false;
  std::fprintf(stderr, "optimizer OOM unexpected exception: %s\n", error.what());
  return 1;
} catch (...) {
  inject_failure = false;
  std::fprintf(stderr, "optimizer OOM unexpected nonstandard exception\n");
  return 1;
}

// NOLINTEND(modernize-use-std-print)
