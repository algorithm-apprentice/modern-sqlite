#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <variant>

#include "modern_sqlite/bytecode/program.hpp"

namespace {

bool fail_allocations = false;
std::size_t allocation_index = 0;
std::optional<std::size_t> failing_allocation{};

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index++;
  if (fail_allocations || failing_allocation == index) {
    throw std::bad_alloc{};
  }
  if (void* allocation = std::malloc(size == 0 ? 1 : size); allocation != nullptr) {
    return allocation;
  }
  throw std::bad_alloc{};
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }

int main() try {
  using namespace modern_sqlite;

  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 1, .generation = 2};
  input.statement_kind = ProgramStatementKind::kInsert;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.mutation_result = MutationResultMetadata{
      .publishes_changes = true,
      .publishes_last_insert_rowid = true,
  };
  input.constants.push_back(SqlValue::Text("payload"));
  input.symbols.emplace_back("BINARY");
  input.symbols.emplace_back("sum");
  input.aggregates.push_back(AggregateDescriptor{
      .function = SymbolId(1),
      .collation = SymbolId(0),
      .argument_count = 1,
  });
  input.sorters.push_back(OrderingRecordDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              OrderingColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
                  .null_placement = BytecodeNullPlacement::kFirst,
              },
          },
  });
  input.top_ns.push_back(OrderingRecordDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              OrderingColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kDescending,
                  .null_placement = BytecodeNullPlacement::kLast,
              },
          },
  });
  input.relations.push_back(OrderingRecordDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              OrderingColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
                  .null_placement = BytecodeNullPlacement::kFirst,
              },
          },
  });
  input.record_comparisons.push_back(OrderingRecordDescriptor{
      .field_count = 1,
      .key_field_count = 1,
      .key_columns =
          {
              OrderingColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
                  .null_placement = BytecodeNullPlacement::kFirst,
              },
          },
  });
  input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(2),
      .columns =
          {
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kText,
                  .not_null = false,
                  .rowid_alias = false,
                  .default_value = ConstantId(0),
              },
          },
      .rowid_alias = std::nullopt,
      .index_columns = {},
      .key_term_count = 0,
      .unique = false,
      .unique_not_null = false,
      .storage = WriteCursorStorageKind::kRowIdTable,
  });
  input.instructions = {HaltInstruction{}};

  auto created = BytecodeProgram::Create(input);
  if (!created.has_value()) {
    return 1;
  }
  const BytecodeProgram& program = *created;

  std::size_t checksum = 0;
  fail_allocations = true;
  for (const auto& instruction : program.instructions()) {
    checksum += static_cast<std::size_t>(InstructionKindOf(instruction));
    checksum += InstructionKindName(InstructionKindOf(instruction)).size();
    std::visit([&checksum](const auto&) { ++checksum; }, instruction);
  }
  for (const auto& constant : program.constants()) {
    checksum += static_cast<std::size_t>(constant.type());
  }
  for (const auto& symbol : program.symbols()) {
    checksum += symbol.size();
  }
  for (const WriteCursorDescriptor& cursor : program.write_cursors()) {
    checksum += cursor.root_page.value();
    checksum += cursor.columns.size();
  }
  for (const OrderingRecordDescriptor& sorter : program.sorters()) {
    checksum += sorter.field_count;
    checksum += sorter.key_field_count;
    checksum += sorter.key_columns.size();
  }
  for (const OrderingRecordDescriptor& top_n : program.top_ns()) {
    checksum += top_n.field_count;
    checksum += top_n.key_field_count;
    checksum += top_n.key_columns.size();
  }
  for (const OrderingRecordDescriptor& relation : program.relations()) {
    checksum += relation.field_count;
    checksum += relation.key_field_count;
    checksum += relation.key_columns.size();
  }
  for (const OrderingRecordDescriptor& comparison : program.record_comparisons()) {
    checksum += comparison.field_count;
    checksum += comparison.key_field_count;
    checksum += comparison.key_columns.size();
  }
  for (const AggregateDescriptor& aggregate : program.aggregates()) {
    checksum += aggregate.function.value();
    checksum += aggregate.collation.value();
    checksum += aggregate.argument_count;
  }
  checksum += program.sorter(SorterId(0)).field_count;
  checksum += program.top_n(TopNId(0)).field_count;
  checksum += program.relation(RelationId(0)).field_count;
  checksum += program.record_comparison(RecordComparisonId(0)).field_count;
  checksum += program.aggregate(AggregateId(0)).argument_count;
  checksum += static_cast<std::size_t>(program.statement_kind());
  checksum += static_cast<std::size_t>(program.transaction_access());
  checksum += static_cast<std::size_t>(program.rollback_mode());
  checksum += program.mutation_result().publishes_changes ? 1U : 0U;
  checksum += program.constant(ConstantId(0)).text_value().value_or(Utf8View{}).size_bytes();
  fail_allocations = false;

  ProgramInput null_input;
  null_input.register_count = 1;
  null_input.cursors.push_back(ReadCursorDescriptor{
      .root_page = RootPageNumber(2),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 1,
      .fields = {CursorFieldSource{.kind = CursorFieldSourceKind::kRowId, .record_field = 0}},
      .index_columns = {},
  });
  null_input.result_columns.push_back(ResultColumnMetadata{
      .name = "rowid",
      .declared_type = std::nullopt,
      .affinity = TypeAffinity::kInteger,
  });
  null_input.instructions = {
      OpenReadCursorInstruction{.cursor = CursorId(0)},
      NullRowInstruction{.cursor = CursorId(0)},
      ReadRowIdInstruction{.cursor = CursorId(0), .output = RegisterId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      CloseCursorInstruction{.cursor = CursorId(0)},
      HaltInstruction{},
  };
  allocation_index = 0;
  const auto null_program = BytecodeProgram::Create(null_input);
  if (!null_program.has_value()) {
    return 2;
  }
  const std::size_t null_allocations = allocation_index;
  if (null_allocations == 0U || null_allocations > 128U) {
    return 2;
  }
  for (std::size_t failure = 0; failure < null_allocations; ++failure) {
    allocation_index = 0;
    failing_allocation = failure;
    bool threw = false;
    try {
      [[maybe_unused]] const auto rejected = BytecodeProgram::Create(null_input);
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    failing_allocation.reset();
    if (!threw) {
      return 2;
    }
  }

  return checksum == 0 ? 1 : 0;
} catch (...) {
  return 1;
}
