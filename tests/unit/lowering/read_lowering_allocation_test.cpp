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

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/lowering/read_lowering.hpp"
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
    throw std::runtime_error{"failed to parse lowering allocation fixture"};
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
  const std::array<PhysicalPlan, 4> physical_plans{
      PhysicalFixture(catalog, "SELECT 1"),
      PhysicalFixture(catalog, "SELECT Name FROM Items"),
      PhysicalFixture(catalog, "SELECT Name FROM Items WHERE rowid=?1"),
      PhysicalFixture(catalog, "SELECT Name FROM Items LIMIT ?1 OFFSET ?2"),
  };
  constexpr std::array<std::size_t, 4> kExpectedAllocations{17U, 27U, 26U, 33U};
  std::unique_ptr<BytecodeProgram> published;
  for (std::size_t plan_index = 0; plan_index < physical_plans.size(); ++plan_index) {
    for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
      allocation_count.store(0, std::memory_order_relaxed);
      count_allocations = true;
      LowerReadPlanResult lowered = LowerReadPlan(physical_plans[plan_index]);
      count_allocations = false;
      if (!lowered.has_value()) {
        return 1;
      }
      const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
      if (allocations != kExpectedAllocations[plan_index]) {
        return 1;
      }
      if (plan_index + 1U == physical_plans.size() && published == nullptr) {
        published = std::make_unique<BytecodeProgram>(std::move(*lowered));
      }
    }
  }
  if (published == nullptr) {
    return 1;
  }

  std::uint64_t checksum = 0;
  fail_allocations = true;
  checksum += published->register_count();
  checksum += published->parameter_count();
  checksum += published->constants().size();
  checksum += published->symbols().size();
  checksum += published->cursors().size();
  checksum += published->result_columns().size();
  checksum += published->instructions().size();
  checksum += published->verification_metrics().reachable_instruction_count;
  for (const Instruction& instruction : published->instructions()) {
    const InstructionKind kind = InstructionKindOf(instruction);
    checksum += static_cast<std::uint64_t>(kind);
    checksum += InstructionKindName(kind).size();
  }
  fail_allocations = false;

  return checksum == 0 ? 1 : 0;
} catch (...) {
  count_allocations = false;
  fail_allocations = false;
  return 1;
}
