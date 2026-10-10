#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
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
    throw std::runtime_error{"failed to parse logical-plan allocation fixture"};
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
    throw std::runtime_error{"failed to create logical-plan allocation catalog"};
  }
  return *std::move(created);
}

[[nodiscard]] modern_sqlite::BoundSelect BindFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  modern_sqlite::BindSelectResult bound = modern_sqlite::BindSelectStatement(
      ParseTree("SELECT Name, abs(?), id FROM Items WHERE id > ? LIMIT ?, ?"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind logical-plan allocation fixture"};
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
    throw std::runtime_error{"failed to bind ordered logical-plan allocation fixture"};
  }
  return std::move(*bound);
}

[[nodiscard]] modern_sqlite::BoundSelect BindCompoundFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  modern_sqlite::BindSelectResult bound = modern_sqlite::BindSelectStatement(
      ParseTree("VALUES(1) UNION ALL SELECT DISTINCT id FROM Items "
                "EXCEPT SELECT 3 ORDER BY 1 LIMIT 2"),
      catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind compound logical-plan allocation fixture"};
  }
  return std::move(*bound);
}

[[nodiscard]] modern_sqlite::BoundStatement BindMutationFixture(
    const modern_sqlite::CatalogSnapshotPtr& catalog) {
  modern_sqlite::BindStatementResult bound =
      modern_sqlite::BindStatement(ParseTree("DELETE FROM Items WHERE id=?1 AND Name=?2"), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{"failed to bind logical mutation allocation fixture"};
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
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

int main() try {
  using namespace modern_sqlite;

  const CatalogSnapshotPtr catalog = TestCatalog();
  std::unique_ptr<LogicalPlan> published;

  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    BoundSelect bound = BindFixture(catalog);
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations = true;
    BuildLogicalPlanResult built = BuildLogicalPlan(std::move(bound));
    count_allocations = false;
    if (!built.has_value()) {
      return 1;
    }
    if (allocation_count.load(std::memory_order_relaxed) != 3U) {
      return 1;
    }
    if (published == nullptr) {
      published = std::make_unique<LogicalPlan>(std::move(*built));
    }
  }
  if (published == nullptr) {
    return 1;
  }

  BoundSelect ordered_bound = BindOrderedFixture(catalog);
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  BuildLogicalPlanResult ordered_result = BuildLogicalPlan(std::move(ordered_bound));
  count_allocations = false;
  if (!ordered_result.has_value() || allocation_count.load(std::memory_order_relaxed) != 5U) {
    return 1;
  }
  std::unique_ptr<LogicalPlan> ordered = std::make_unique<LogicalPlan>(std::move(*ordered_result));

  BoundSelect compound_bound = BindCompoundFixture(catalog);
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  BuildLogicalPlanResult compound_result = BuildLogicalPlan(std::move(compound_bound));
  count_allocations = false;
  const std::size_t compound_allocations = allocation_count.load(std::memory_order_relaxed);
  if (!compound_result.has_value() || compound_allocations == 0U || compound_allocations > 32U) {
    return 1;
  }
  std::unique_ptr<LogicalPlan> compound =
      std::make_unique<LogicalPlan>(std::move(*compound_result));

  const LogicalPlan& plan = *published;
  BoundStatement mutation_bound = BindMutationFixture(catalog);
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  BuildLogicalStatementPlanResult mutation_result =
      BuildLogicalStatementPlan(std::move(mutation_bound));
  count_allocations = false;
  if (!mutation_result.has_value()) {
    return 1;
  }
  const std::size_t mutation_allocations = allocation_count.load(std::memory_order_relaxed);
  if (mutation_allocations == 0U || mutation_allocations > 8U) {
    return 1;
  }
  LogicalStatementPlan mutation_plan = std::move(*mutation_result);

  std::uint64_t checksum = 0;
  fail_allocations = true;
  checksum += plan.bound_select().source().size_bytes();
  checksum += plan.bound_select().expressions().size();
  checksum += plan.bound_select().result_columns().size();
  checksum += plan.nodes().size();
  checksum += plan.root().value();
  for (std::uint32_t index = 0; index < plan.nodes().size(); ++index) {
    const LogicalNode& node = plan.node(LogicalNodeId{index});
    checksum += static_cast<std::uint64_t>(LogicalNodeKindOf(node));
    checksum += LogicalNodeKindName(LogicalNodeKindOf(node)).size();
    std::visit([&checksum](const auto&) { ++checksum; }, node.payload);
  }
  checksum += ordered->nodes().size();
  const auto& order = std::get<LogicalOrderNode>(ordered->nodes()[2].payload);
  checksum += order.terms.size();
  checksum += order.payload_expressions.size();
  checksum += order.output_fields.size();
  checksum += compound->nodes().size();
  checksum += compound->root().value();
  const LogicalMutationPlan& mutation = std::get<LogicalMutationPlan>(mutation_plan);
  checksum += static_cast<std::uint64_t>(LogicalMutationKindOf(mutation.payload()));
  checksum += LogicalMutationKindName(LogicalMutationKindOf(mutation.payload())).size();
  checksum += std::get<BoundDelete>(mutation.bound_statement()).expressions().size();
  fail_allocations = false;

  return checksum == 0 ? 1 : 0;
} catch (...) {
  count_allocations = false;
  fail_allocations = false;
  return 1;
}
