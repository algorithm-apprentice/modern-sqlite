#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/planner/logical_plan.hpp"
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
    throw std::runtime_error{"failed to parse logical-plan OOM fixture"};
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
    throw std::runtime_error{"failed to create logical-plan OOM catalog"};
  }
  return *std::move(created);
}

[[nodiscard]] modern_sqlite::BoundSelect BindFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  modern_sqlite::BindSelectResult bound = modern_sqlite::BindSelectStatement(
      ParseTree("SELECT Name, abs(?), id FROM Items WHERE id > ? LIMIT ?, ?"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind logical-plan OOM fixture"};
  }
  return std::move(*bound);
}

[[nodiscard]] modern_sqlite::BoundSelect BindOrderedFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  modern_sqlite::BindSelectResult bound = modern_sqlite::BindSelectStatement(
      ParseTree("SELECT Name, abs(?), id FROM Items WHERE id > ? "
                "ORDER BY Name, id, Name DESC LIMIT ?"),
      catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind ordered logical-plan OOM fixture"};
  }
  return std::move(*bound);
}

[[nodiscard]] modern_sqlite::BoundSelect BindCompoundFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog,
    std::string_view sql =
        "VALUES(1) UNION ALL SELECT DISTINCT id FROM Items "
        "EXCEPT SELECT 3 ORDER BY 1 LIMIT 2") {
  modern_sqlite::BindSelectResult bound =
      modern_sqlite::BindSelectStatement(ParseTree(sql), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind compound logical-plan OOM fixture"};
  }
  return std::move(*bound);
}

[[nodiscard]] modern_sqlite::BoundSelect BindAggregateFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  modern_sqlite::BindSelectResult bound = modern_sqlite::BindSelectStatement(
      ParseTree("SELECT Name,count() AS total FROM Items WHERE id>0 "
                "GROUP BY Name HAVING total>0 ORDER BY Name DESC LIMIT ?1"),
      catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind aggregate logical-plan OOM fixture"};
  }
  return std::move(*bound);
}

[[nodiscard]] modern_sqlite::BoundStatement BindMutationFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  modern_sqlite::BindStatementResult bound = modern_sqlite::BindStatement(
      ParseTree("UPDATE Items SET Name=?1, id=id+1 WHERE id=?2"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind logical mutation OOM fixture"};
  }
  return std::move(*bound);
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
  BoundSelect baseline_bound = BindFixture(catalog);
  allocation_index.store(0, std::memory_order_relaxed);
  const BuildLogicalPlanResult baseline = BuildLogicalPlan(std::move(baseline_bound));
  if (!baseline.has_value()) {
    return 1;
  }
  const std::size_t allocation_count = allocation_index.load(std::memory_order_relaxed);
  if (allocation_count != 3U) {
    return 1;
  }

  for (std::size_t failure = 0; failure < allocation_count; ++failure) {
    BoundSelect bound = BindFixture(catalog);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool threw = false;
    try {
      [[maybe_unused]] const BuildLogicalPlanResult unexpected = BuildLogicalPlan(std::move(bound));
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    inject_failure = false;
    if (!threw) {
      return 1;
    }
  }

  BoundSelect recovered_bound = BindFixture(catalog);
  const BuildLogicalPlanResult recovered = BuildLogicalPlan(std::move(recovered_bound));
  if (!recovered.has_value()) {
    return 1;
  }

  BoundSelect ordered_baseline_bound = BindOrderedFixture(catalog);
  allocation_index.store(0, std::memory_order_relaxed);
  const BuildLogicalPlanResult ordered_baseline =
      BuildLogicalPlan(std::move(ordered_baseline_bound));
  if (!ordered_baseline.has_value()) {
    return 1;
  }
  const std::size_t ordered_allocation_count = allocation_index.load(std::memory_order_relaxed);
  if (ordered_allocation_count != 5U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < ordered_allocation_count; ++failure) {
    BoundSelect bound = BindOrderedFixture(catalog);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool threw = false;
    try {
      [[maybe_unused]] const BuildLogicalPlanResult unexpected = BuildLogicalPlan(std::move(bound));
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    inject_failure = false;
    if (!threw) {
      return 1;
    }
  }
  if (!BuildLogicalPlan(BindOrderedFixture(catalog)).has_value()) {
    return 1;
  }

  const std::array<std::string_view, 7> advanced_sqls{
      "VALUES(1) UNION ALL SELECT DISTINCT id FROM Items EXCEPT SELECT 3 ORDER BY 1 LIMIT 2",
      "SELECT DISTINCT a.Name FROM Items a CROSS JOIN Items b "
      "WHERE a.id>?1 AND b.id>?2 ORDER BY 1 LIMIT ?3",
      "SELECT b.id,count(),max(a.id) FROM Items a CROSS JOIN Items b "
      "GROUP BY b.id HAVING count()>?1 ORDER BY 1 LIMIT ?2",
      "SELECT a.id,count(b.id) FROM Items a LEFT JOIN Items b ON a.id=b.id+?1 "
      "GROUP BY a.id ORDER BY 1",
      "SELECT id,count(b.id) FROM Items a LEFT JOIN Items b USING(id) GROUP BY id ORDER BY 1",
      "SELECT id,count(a.id) FROM Items a RIGHT JOIN Items b USING(id) GROUP BY id ORDER BY 1",
      "SELECT id,count(a.id) FROM Items a FULL JOIN Items b USING(id) GROUP BY id ORDER BY 1",
  };
  for (const std::string_view sql : advanced_sqls) {
    BoundSelect compound_baseline_bound = BindCompoundFixture(catalog, sql);
    allocation_index.store(0, std::memory_order_relaxed);
    const BuildLogicalPlanResult compound_baseline =
        BuildLogicalPlan(std::move(compound_baseline_bound));
    if (!compound_baseline.has_value()) {
      return 1;
    }
    const std::size_t compound_allocation_count = allocation_index.load(std::memory_order_relaxed);
    if (compound_allocation_count == 0U || compound_allocation_count > 64U) {
      return 1;
    }
    for (std::size_t failure = 0; failure < compound_allocation_count; ++failure) {
      BoundSelect bound = BindCompoundFixture(catalog, sql);
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = failure;
      inject_failure = true;
      bool threw = false;
      try {
        [[maybe_unused]] const BuildLogicalPlanResult unexpected =
            BuildLogicalPlan(std::move(bound));
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      inject_failure = false;
      if (!threw) {
        return 1;
      }
    }
    if (!BuildLogicalPlan(BindCompoundFixture(catalog, sql)).has_value()) {
      return 1;
    }
  }

  BoundSelect aggregate_baseline_bound = BindAggregateFixture(catalog);
  allocation_index.store(0, std::memory_order_relaxed);
  const BuildLogicalPlanResult aggregate_baseline =
      BuildLogicalPlan(std::move(aggregate_baseline_bound));
  if (!aggregate_baseline.has_value()) {
    return 1;
  }
  const std::size_t aggregate_allocation_count = allocation_index.load(std::memory_order_relaxed);
  if (aggregate_allocation_count == 0U || aggregate_allocation_count > 64U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < aggregate_allocation_count; ++failure) {
    BoundSelect bound = BindAggregateFixture(catalog);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool threw = false;
    try {
      [[maybe_unused]] const BuildLogicalPlanResult unexpected = BuildLogicalPlan(std::move(bound));
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    inject_failure = false;
    if (!threw) {
      return 1;
    }
  }
  if (!BuildLogicalPlan(BindAggregateFixture(catalog)).has_value()) {
    return 1;
  }

  BoundStatement mutation_baseline = BindMutationFixture(catalog);
  allocation_index.store(0, std::memory_order_relaxed);
  const BuildLogicalStatementPlanResult mutation =
      BuildLogicalStatementPlan(std::move(mutation_baseline));
  if (!mutation.has_value()) {
    return 1;
  }
  const std::size_t mutation_allocations = allocation_index.load(std::memory_order_relaxed);
  if (mutation_allocations == 0U || mutation_allocations > 8U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < mutation_allocations; ++failure) {
    BoundStatement bound = BindMutationFixture(catalog);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool threw = false;
    try {
      [[maybe_unused]] const BuildLogicalStatementPlanResult unexpected =
          BuildLogicalStatementPlan(std::move(bound));
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    inject_failure = false;
    if (!threw) {
      return 1;
    }
  }
  return BuildLogicalStatementPlan(BindMutationFixture(catalog)).has_value() ? 0 : 1;
} catch (...) {
  inject_failure = false;
  return 1;
}
