#include "modern_sqlite/vm/vm.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/database_format.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "modern_sqlite/transaction/transaction_coordinator.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"
#include "tests/unit/storage/btree/database_test_support.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<Vm>);
static_assert(!std::is_copy_assignable_v<Vm>);
static_assert(std::is_nothrow_move_constructible_v<Vm>);
static_assert(std::is_nothrow_move_assignable_v<Vm>);

constexpr std::uint64_t kCatalogGeneration = 17;
constexpr int kSqliteAbortRollback = 516;

[[nodiscard]] constexpr RegisterId Reg(std::uint32_t value) { return RegisterId(value); }
[[nodiscard]] constexpr CursorId Cursor(std::uint32_t value) { return CursorId(value); }
[[nodiscard]] constexpr WriteCursorId WriteCursor(std::uint32_t value) {
  return WriteCursorId(value);
}
[[nodiscard]] constexpr ParameterId Parameter(std::uint32_t value) { return ParameterId(value); }
[[nodiscard]] constexpr ConstantId Constant(std::uint32_t value) { return ConstantId(value); }
[[nodiscard]] constexpr SymbolId Symbol(std::uint32_t value) { return SymbolId(value); }
[[nodiscard]] constexpr CursorFieldId Field(std::uint32_t value) { return CursorFieldId(value); }
[[nodiscard]] constexpr RelationId Relation(std::uint32_t value) { return RelationId(value); }
[[nodiscard]] constexpr RecordComparisonId RecordComparison(std::uint32_t value) {
  return RecordComparisonId(value);
}
[[nodiscard]] constexpr InstructionAddress Address(std::uint32_t value) {
  return InstructionAddress(value);
}

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

template <typename T>
void RequireProgramResult(const ProgramResult<T>& result) {
  if (!result.has_value()) {
    throw std::runtime_error("bytecode program construction failed: code=" +
                             std::to_string(static_cast<int>(result.error().code)) +
                             " instruction=" + std::to_string(result.error().instruction) +
                             " detail=" + std::to_string(result.error().detail));
  }
}

template <typename T>
[[nodiscard]] T TakeProgramValue(ProgramResult<T> result) {
  RequireProgramResult(result);
  return std::move(*result);
}

void RequireStatus(Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "btree_read" / "sqlite-3.54.0-btree-read.db";
}

[[nodiscard]] std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("unable to open VM test fixture");
  }
  const std::vector<char> characters{std::istreambuf_iterator<char>{stream},
                                     std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes(characters.size());
  if (!characters.empty()) {
    std::memcpy(bytes.data(), characters.data(), characters.size());
  }
  return bytes;
}

void Write32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + offset, sizeof(std::uint32_t)},
      value);
}

[[nodiscard]] std::uint32_t Read32(const std::vector<std::byte>& bytes, std::size_t offset) {
  return LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
      bytes.data() + offset, sizeof(std::uint32_t)});
}

class TemporaryDatabase final {
 public:
  explicit TemporaryDatabase(const std::vector<std::byte>& bytes) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-vm-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(1)) + ".db");
    journal_path_ = path_;
    journal_path_ += "-journal";
    wal_path_ = path_;
    wal_path_ += "-wal";
    Rewrite(bytes);
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  ~TemporaryDatabase() noexcept {
    std::error_code error;
    std::filesystem::remove(path_, error);
    std::filesystem::remove(journal_path_, error);
    std::filesystem::remove(wal_path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  void Rewrite(const std::vector<std::byte>& bytes) {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("unable to create temporary VM database");
    }
    if (!bytes.empty()) {
      stream.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
      throw std::runtime_error("unable to write temporary VM database");
    }
  }

 private:
  std::filesystem::path path_;
  std::filesystem::path journal_path_;
  std::filesystem::path wal_path_;
};

[[nodiscard]] ResultColumnMetadata ResultColumn(std::string name = "value",
                                                TypeAffinity affinity = TypeAffinity::kNone) {
  return ResultColumnMetadata{
      .name = std::move(name),
      .declared_type = std::nullopt,
      .affinity = affinity,
  };
}

[[nodiscard]] SchemaVersionRequirement CurrentSchema(
    const Pager& pager, std::uint64_t generation = kCatalogGeneration) {
  const DatabaseHeader* header = pager.header();
  return SchemaVersionRequirement{
      .schema_cookie = header == nullptr ? 0U : header->schema_cookie(),
      .generation = generation,
  };
}

// The adjacent counts mirror ProgramInput and remain explicit at each test call site.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] BytecodeProgram BuildProgram(
    const Pager& pager, std::uint32_t register_count, std::uint32_t parameter_count,
    std::vector<SqlValue> constants, std::vector<std::string> symbols,
    std::vector<ReadCursorDescriptor> cursors, std::vector<ResultColumnMetadata> result_columns,
    std::vector<Instruction> instructions,
    std::optional<SchemaVersionRequirement> schema = std::nullopt,
    bool requires_database_snapshot = false) {
  ProgramInput input;
  input.schema_version = schema.value_or(CurrentSchema(pager));
  input.register_count = register_count;
  input.parameter_count = parameter_count;
  input.requires_database_snapshot = requires_database_snapshot;
  input.constants = std::move(constants);
  input.symbols = std::move(symbols);
  input.cursors = std::move(cursors);
  input.result_columns = std::move(result_columns);
  input.instructions = std::move(instructions);
  return TakeProgramValue(BytecodeProgram::Create(input));
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] const SqlValue& OnlyRowValue(const Vm& vm) {
  const std::span<const SqlValue> row = vm.row();
  if (row.size() != 1U) {
    throw std::runtime_error("expected exactly one VM result value");
  }
  return row.front();
}

[[nodiscard]] Vm CreateAttachedVm(const BytecodeProgram& program, BtreeDatabase& database,
                                  std::uint64_t catalog_generation,
                                  VmEnvironment environment = VmEnvironment::Core(),
                                  VmLimits limits = {}) {
  Vm vm = TakeValue(Vm::Create(program, environment, limits));
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{database, catalog_generation}));
  return vm;
}

[[nodiscard]] OrderingRecordDescriptor SorterDescriptor(
    SymbolId collation = Symbol(0), BytecodeSortOrder order = BytecodeSortOrder::kAscending) {
  return OrderingRecordDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              OrderingColumnMetadata{
                  .collation = collation,
                  .order = order,
                  .null_placement = order == BytecodeSortOrder::kDescending
                                        ? BytecodeNullPlacement::kLast
                                        : BytecodeNullPlacement::kFirst,
              },
          },
  };
}

[[nodiscard]] AggregateDescriptor AggregateDescriptorFor(SymbolId function, SymbolId collation,
                                                         std::uint32_t argument_count) {
  return AggregateDescriptor{
      .function = function,
      .collation = collation,
      .argument_count = argument_count,
  };
}

[[nodiscard]] BytecodeProgram BuildAggregateSumProgram(const Pager& pager) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager);
  input.register_count = 3;
  input.symbols = {"sum", "BINARY"};
  input.constants.push_back(SqlValue::Integer(2));
  input.constants.push_back(SqlValue::Integer(3));
  input.constants.push_back(SqlValue::Integer(4));
  input.aggregates.push_back(AggregateDescriptorFor(Symbol(0), Symbol(1), 1));
  input.result_columns = {
      ResultColumnMetadata{
          .name = "effect",
          .declared_type = "INTEGER",
          .affinity = TypeAffinity::kInteger,
      },
      ResultColumnMetadata{
          .name = "sum",
          .declared_type = std::nullopt,
          .affinity = TypeAffinity::kNone,
      },
  };
  input.instructions = {
      OpenAggregateInstruction{.aggregate = AggregateId(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      StepAggregateInstruction{
          .aggregate = AggregateId(0),
          .first_argument = Reg(0),
          .effect_output = Reg(1),
      },
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(0)},
      StepAggregateInstruction{
          .aggregate = AggregateId(0),
          .first_argument = Reg(0),
          .effect_output = Reg(1),
      },
      FinalizeAggregateInstruction{.aggregate = AggregateId(0), .output = Reg(2)},
      ResultRowInstruction{.first = Reg(1), .count = 2},
      ResetAggregateInstruction{.aggregate = AggregateId(0)},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(0)},
      StepAggregateInstruction{
          .aggregate = AggregateId(0),
          .first_argument = Reg(0),
          .effect_output = std::nullopt,
      },
      FinalizeAggregateInstruction{.aggregate = AggregateId(0), .output = Reg(2)},
      ResultRowInstruction{.first = Reg(1), .count = 2},
      CloseAggregateInstruction{.aggregate = AggregateId(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram BuildAggregateMinEffectProgram(const Pager& pager) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager);
  input.register_count = 3;
  input.symbols = {"min", "BINARY"};
  input.constants.push_back(SqlValue::Integer(3));
  input.constants.push_back(SqlValue::Integer(5));
  input.constants.push_back(SqlValue::Integer(1));
  input.aggregates.push_back(AggregateDescriptorFor(Symbol(0), Symbol(1), 1));
  input.result_columns = {
      ResultColumnMetadata{
          .name = "value",
          .declared_type = std::nullopt,
          .affinity = TypeAffinity::kNone,
      },
  };
  input.instructions = {
      OpenAggregateInstruction{.aggregate = AggregateId(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      StepAggregateInstruction{
          .aggregate = AggregateId(0),
          .first_argument = Reg(0),
          .effect_output = Reg(1),
      },
      ResultRowInstruction{.first = Reg(1), .count = 1},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(0)},
      StepAggregateInstruction{
          .aggregate = AggregateId(0),
          .first_argument = Reg(0),
          .effect_output = Reg(1),
      },
      ResultRowInstruction{.first = Reg(1), .count = 1},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(0)},
      StepAggregateInstruction{
          .aggregate = AggregateId(0),
          .first_argument = Reg(0),
          .effect_output = Reg(1),
      },
      ResultRowInstruction{.first = Reg(1), .count = 1},
      FinalizeAggregateInstruction{.aggregate = AggregateId(0), .output = Reg(2)},
      ResultRowInstruction{.first = Reg(2), .count = 1},
      CloseAggregateInstruction{.aggregate = AggregateId(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram BuildAggregateConcatErrorProgram(const Pager& pager) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager);
  input.register_count = 2;
  input.symbols = {"group_concat", "BINARY"};
  input.constants.push_back(SqlValue::Text("abc"));
  input.constants.push_back(SqlValue::Text("def"));
  input.aggregates.push_back(AggregateDescriptorFor(Symbol(0), Symbol(1), 1));
  input.result_columns = {
      ResultColumnMetadata{
          .name = "stepped",
          .declared_type = "TEXT",
          .affinity = TypeAffinity::kText,
      },
  };
  input.instructions = {
      OpenAggregateInstruction{.aggregate = AggregateId(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      StepAggregateInstruction{
          .aggregate = AggregateId(0),
          .first_argument = Reg(0),
      },
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(0)},
      StepAggregateInstruction{
          .aggregate = AggregateId(0),
          .first_argument = Reg(0),
      },
      ResultRowInstruction{.first = Reg(0), .count = 1},
      FinalizeAggregateInstruction{.aggregate = AggregateId(0), .output = Reg(1)},
      CloseAggregateInstruction{.aggregate = AggregateId(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram BuildSorterProgram(
    const Pager& pager, BytecodeSortOrder order = BytecodeSortOrder::kAscending) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager);
  input.register_count = 2;
  input.symbols.emplace_back("BINARY");
  input.constants.push_back(SqlValue::Integer(3));
  input.constants.push_back(SqlValue::Text("three"));
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Text("one"));
  input.constants.push_back(SqlValue::Integer(2));
  input.constants.push_back(SqlValue::Text("two"));
  input.sorters.push_back(SorterDescriptor(Symbol(0), order));
  input.result_columns = {
      ResultColumnMetadata{
          .name = "key",
          .declared_type = "INTEGER",
          .affinity = TypeAffinity::kInteger,
      },
      ResultColumnMetadata{
          .name = "payload",
          .declared_type = "TEXT",
          .affinity = TypeAffinity::kText,
      },
  };
  input.instructions = {
      OpenSorterInstruction{.sorter = SorterId(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      InsertSorterInstruction{.sorter = SorterId(0), .first_value = Reg(0), .value_count = 2},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(3), .output = Reg(1)},
      InsertSorterInstruction{.sorter = SorterId(0), .first_value = Reg(0), .value_count = 2},
      LoadConstantInstruction{.constant = Constant(4), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(5), .output = Reg(1)},
      InsertSorterInstruction{.sorter = SorterId(0), .first_value = Reg(0), .value_count = 2},
      RewindSorterInstruction{.sorter = SorterId(0), .empty_target = Address(15)},
      ReadSorterFieldInstruction{.sorter = SorterId(0), .field = 0, .output = Reg(0)},
      ReadSorterFieldInstruction{.sorter = SorterId(0), .field = 1, .output = Reg(1)},
      ResultRowInstruction{.first = Reg(0), .count = 2},
      NextSorterInstruction{.sorter = SorterId(0), .next_target = Address(11)},
      ResetSorterInstruction{.sorter = SorterId(0)},
      CloseSorterInstruction{.sorter = SorterId(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram BuildTopNProgram(const Pager& pager, std::int64_t bound = 2) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager);
  input.register_count = 3;
  input.symbols.emplace_back("BINARY");
  input.constants.push_back(SqlValue::Integer(bound));
  input.constants.push_back(SqlValue::Integer(3));
  input.constants.push_back(SqlValue::Integer(30));
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(10));
  input.constants.push_back(SqlValue::Integer(4));
  input.constants.push_back(SqlValue::Integer(40));
  input.constants.push_back(SqlValue::Integer(2));
  input.constants.push_back(SqlValue::Integer(20));
  input.top_ns.push_back(SorterDescriptor());
  input.result_columns = {
      ResultColumnMetadata{
          .name = "key",
          .declared_type = "INTEGER",
          .affinity = TypeAffinity::kInteger,
      },
      ResultColumnMetadata{
          .name = "payload",
          .declared_type = "INTEGER",
          .affinity = TypeAffinity::kInteger,
      },
  };
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenTopNInstruction{.top_n = TopNId(0), .bound = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      CheckTopNInstruction{
          .top_n = TopNId(0),
          .first_key = Reg(1),
          .key_count = 1,
          .rejected_target = Address(6),
      },
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(2)},
      InsertTopNInstruction{.top_n = TopNId(0), .first_value = Reg(1), .value_count = 2},
      LoadConstantInstruction{.constant = Constant(3), .output = Reg(1)},
      CheckTopNInstruction{
          .top_n = TopNId(0),
          .first_key = Reg(1),
          .key_count = 1,
          .rejected_target = Address(10),
      },
      LoadConstantInstruction{.constant = Constant(4), .output = Reg(2)},
      InsertTopNInstruction{.top_n = TopNId(0), .first_value = Reg(1), .value_count = 2},
      LoadConstantInstruction{.constant = Constant(5), .output = Reg(1)},
      CheckTopNInstruction{
          .top_n = TopNId(0),
          .first_key = Reg(1),
          .key_count = 1,
          .rejected_target = Address(14),
      },
      LoadConstantInstruction{.constant = Constant(6), .output = Reg(2)},
      InsertTopNInstruction{.top_n = TopNId(0), .first_value = Reg(1), .value_count = 2},
      LoadConstantInstruction{.constant = Constant(7), .output = Reg(1)},
      CheckTopNInstruction{
          .top_n = TopNId(0),
          .first_key = Reg(1),
          .key_count = 1,
          .rejected_target = Address(18),
      },
      LoadConstantInstruction{.constant = Constant(8), .output = Reg(2)},
      InsertTopNInstruction{.top_n = TopNId(0), .first_value = Reg(1), .value_count = 2},
      RewindTopNInstruction{.top_n = TopNId(0), .empty_target = Address(23)},
      ReadTopNFieldInstruction{.top_n = TopNId(0), .field = 0, .output = Reg(1)},
      ReadTopNFieldInstruction{.top_n = TopNId(0), .field = 1, .output = Reg(2)},
      ResultRowInstruction{.first = Reg(1), .count = 2},
      NextTopNInstruction{.top_n = TopNId(0), .next_target = Address(19)},
      ResetTopNInstruction{.top_n = TopNId(0)},
      CloseTopNInstruction{.top_n = TopNId(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram BuildLookupRelationProgram(const Pager& pager, bool empty = false) {
  auto builder = TakeProgramValue(ProgramBuilder::Create(
      CurrentSchema(pager), ProgramResourceCounts{.registers = 6, .parameters = 1}));
  const SymbolId binary = TakeProgramValue(builder.AddSymbol("BINARY"));
  const SymbolId nocase = TakeProgramValue(builder.AddSymbol("NOCASE"));
  const RelationId relation = TakeProgramValue(builder.AddRelation(OrderingRecordDescriptor{
      .field_count = 3,
      .key_field_count = 3,
      .key_columns =
          {
              {.collation = nocase},
              {.collation = binary},
              {.collation = binary},
          },
      .relation_purpose = RelationPurpose::kLookup,
  }));
  const CursorId cursor = TakeProgramValue(builder.AddCursor(ReadCursorDescriptor{
      .root_page = RootPageNumber(0),
      .storage = CursorStorageKind::kEphemeralIndex,
      .record_field_count = 3,
      .fields =
          {
              CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 1},
              CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 2},
          },
      .index_columns = {},
      .relation = relation,
  }));
  const auto check = TakeProgramValue(builder.CreateLabel());
  const auto ready = TakeProgramValue(builder.CreateLabel());
  const auto done = TakeProgramValue(builder.CreateLabel());
  const auto emit_row = [&] {
    RequireProgramResult(builder.Append(
        ReadFieldInstruction{.cursor = cursor, .field = Field(0), .output = Reg(4)}));
    RequireProgramResult(builder.Append(
        ReadFieldInstruction{.cursor = cursor, .field = Field(1), .output = Reg(5)}));
    RequireProgramResult(builder.Append(ResultRowInstruction{.first = Reg(4), .count = 2}));
  };
  RequireProgramResult(builder.Append(OpenRelationInstruction{.relation = relation}));
  RequireProgramResult(builder.Append(OpenReadCursorInstruction{.cursor = cursor}));
  RequireProgramResult(builder.Append(NullRowInstruction{.cursor = cursor}));
  emit_row();
  RequireProgramResult(builder.Append(NullRowInstruction{.cursor = cursor, .enabled = false}));
  RequireProgramResult(builder.BindLabel(check));
  RequireProgramResult(builder.EmitCheckRelationLookup(relation, ready, done));
  if (!empty) {
    const std::array<std::string_view, 3> keys{"a", "A", "b"};
    const std::array<std::int64_t, 4> rowids{2, 1, 3, 4};
    for (std::size_t index = 0; index < rowids.size(); ++index) {
      std::array<SqlValue, 3> values{
          index < keys.size() ? SqlValue::Text(std::string{keys[index]}) : SqlValue{},
          SqlValue::Text(std::string(2048, 'p') + std::to_string(rowids[index])),
          SqlValue::Integer(rowids[index]),
      };
      for (std::size_t field = 0; field < values.size(); ++field) {
        const ConstantId value = TakeProgramValue(builder.AddConstant(std::move(values[field])));
        RequireProgramResult(builder.Append(LoadConstantInstruction{
            .constant = value, .output = Reg(static_cast<std::uint32_t>(field))}));
      }
      const auto inserted = TakeProgramValue(builder.CreateLabel());
      RequireProgramResult(builder.EmitInsertRelation(relation, Reg(0), 3,
                                                      RelationInsertMode::kKeepExisting, inserted));
      RequireProgramResult(builder.BindLabel(inserted));
    }
  }
  RequireProgramResult(builder.Append(SealRelationInstruction{.relation = relation}));
  RequireProgramResult(builder.EmitJump(check));
  RequireProgramResult(builder.BindLabel(ready));
  for (std::size_t probe = 0; probe < 2U; ++probe) {
    const auto row = TakeProgramValue(builder.CreateLabel());
    const auto exhausted = TakeProgramValue(builder.CreateLabel());
    RequireProgramResult(
        builder.Append(LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(3)}));
    RequireProgramResult(builder.EmitSeekRelationPrefix(cursor, Reg(3), 1, exhausted));
    RequireProgramResult(builder.BindLabel(row));
    emit_row();
    RequireProgramResult(builder.EmitNextRelationPrefix(cursor, Reg(3), 1, row));
    RequireProgramResult(builder.BindLabel(exhausted));
    if (probe == 0U) {
      RequireProgramResult(builder.Append(NullRowInstruction{.cursor = cursor}));
      emit_row();
      RequireProgramResult(builder.Append(NullRowInstruction{.cursor = cursor, .enabled = false}));
    }
  }
  RequireProgramResult(builder.BindLabel(done));
  RequireProgramResult(builder.Append(CloseCursorInstruction{.cursor = cursor}));
  RequireProgramResult(builder.Append(ResetRelationInstruction{.relation = relation}));
  RequireProgramResult(builder.Append(CloseRelationInstruction{.relation = relation}));
  RequireProgramResult(builder.Append(HaltInstruction{}));
  return TakeProgramValue(
      std::move(builder).Build({ResultColumn("payload"), ResultColumn("rowid")}));
}

[[nodiscard]] BytecodeProgram BuildRelationProgram(const Pager& pager) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager);
  input.register_count = 3;
  input.symbols.emplace_back("BINARY");
  input.constants.push_back(SqlValue::Integer(2));
  input.constants.push_back(SqlValue::Integer(20));
  input.constants.push_back(SqlValue::Real(2.0));
  input.constants.push_back(SqlValue::Integer(21));
  input.constants.push_back(SqlValue::Integer(22));
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(10));
  input.constants.push_back(SqlValue::Integer(3));
  input.constants.push_back(SqlValue::Integer(30));
  input.constants.push_back(SqlValue::Integer(4));
  input.relations.push_back(SorterDescriptor());
  input.result_columns = {
      ResultColumnMetadata{
          .name = "key",
          .declared_type = "NUMERIC",
          .affinity = TypeAffinity::kNumeric,
      },
      ResultColumnMetadata{
          .name = "payload",
          .declared_type = "INTEGER",
          .affinity = TypeAffinity::kInteger,
      },
  };
  input.instructions = {
      OpenRelationInstruction{.relation = Relation(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = RelationInsertMode::kKeepExisting,
          .duplicate_target = Address(4),
      },
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(3), .output = Reg(1)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = RelationInsertMode::kKeepExisting,
          .duplicate_target = Address(8),
      },
      HaltInstruction{},
      LoadConstantInstruction{.constant = Constant(4), .output = Reg(1)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = RelationInsertMode::kReplaceExisting,
          .duplicate_target = Address(10),
      },
      LoadConstantInstruction{.constant = Constant(5), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(6), .output = Reg(1)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = RelationInsertMode::kKeepExisting,
          .duplicate_target = Address(13),
      },
      LoadConstantInstruction{.constant = Constant(7), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(8), .output = Reg(1)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = RelationInsertMode::kKeepExisting,
          .duplicate_target = Address(16),
      },
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      ContainsRelationInstruction{
          .relation = Relation(0),
          .first_key = Reg(0),
          .key_count = 1,
          .found_target = Address(19),
      },
      HaltInstruction{},
      LoadConstantInstruction{.constant = Constant(9), .output = Reg(0)},
      DeleteRelationInstruction{
          .relation = Relation(0),
          .first_key = Reg(0),
          .key_count = 1,
      },
      LoadConstantInstruction{.constant = Constant(5), .output = Reg(0)},
      DeleteRelationInstruction{
          .relation = Relation(0),
          .first_key = Reg(0),
          .key_count = 1,
      },
      RewindRelationInstruction{.relation = Relation(0), .empty_target = Address(28)},
      ReadRelationFieldInstruction{.relation = Relation(0), .field = 0, .output = Reg(0)},
      ReadRelationFieldInstruction{.relation = Relation(0), .field = 1, .output = Reg(1)},
      ResultRowInstruction{.first = Reg(0), .count = 2},
      NextRelationInstruction{.relation = Relation(0), .next_target = Address(24)},
      ResetRelationInstruction{.relation = Relation(0)},
      CloseRelationInstruction{.relation = Relation(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram BuildRelationDrainProgram(const Pager& pager) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager);
  input.register_count = 2;
  input.symbols.emplace_back("BINARY");
  ByteBuffer payload{ByteCount{900}};
  std::ranges::fill(payload.mutable_view(), std::byte{0x5a});
  input.constants.push_back(SqlValue::Blob(std::move(payload)));
  constexpr std::uint32_t kRowCount = 24;
  for (std::uint32_t key = 0; key < kRowCount; ++key) {
    input.constants.push_back(SqlValue::Integer(key));
  }
  input.relations.push_back(SorterDescriptor());
  input.result_columns = {
      ResultColumnMetadata{
          .name = "key",
          .declared_type = "INTEGER",
          .affinity = TypeAffinity::kInteger,
      },
  };
  input.instructions.emplace_back(OpenRelationInstruction{.relation = Relation(0)});
  input.instructions.emplace_back(
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)});
  for (std::uint32_t key = 0; key < kRowCount; ++key) {
    input.instructions.emplace_back(
        LoadConstantInstruction{.constant = Constant(key + 1U), .output = Reg(0)});
    const auto next =
        InstructionAddress(static_cast<std::uint32_t>(input.instructions.size() + 1U));
    input.instructions.emplace_back(InsertRelationInstruction{
        .relation = Relation(0),
        .first_value = Reg(0),
        .value_count = 2,
        .mode = RelationInsertMode::kKeepExisting,
        .duplicate_target = next,
    });
  }
  const auto rewind_address = static_cast<std::uint32_t>(input.instructions.size());
  input.instructions.emplace_back(RewindRelationInstruction{
      .relation = Relation(0),
      .empty_target = Address(rewind_address + 4U),
  });
  input.instructions.emplace_back(
      ReadRelationFieldInstruction{.relation = Relation(0), .field = 0, .output = Reg(0)});
  input.instructions.emplace_back(ResultRowInstruction{.first = Reg(0), .count = 1});
  input.instructions.emplace_back(NextRelationInstruction{
      .relation = Relation(0), .next_target = Address(rewind_address + 1U)});
  input.instructions.emplace_back(ResetRelationInstruction{.relation = Relation(0)});
  input.instructions.emplace_back(CloseRelationInstruction{.relation = Relation(0)});
  input.instructions.emplace_back(HaltInstruction{});
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram BuildRecordComparisonProgram(const Pager& pager) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager);
  input.register_count = 15;
  input.symbols.emplace_back("BINARY");
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(3));
  input.constants.push_back(SqlValue::Real(1.0));
  input.constants.push_back(SqlValue::Integer(2));
  input.constants.push_back(SqlValue::Real(1.0));
  input.constants.push_back(SqlValue::Integer(2));
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(3));
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.emplace_back();
  input.constants.push_back(SqlValue::Real(1.0));
  input.constants.emplace_back();
  OrderingRecordDescriptor descriptor = SorterDescriptor();
  descriptor.key_field_count = 2;
  descriptor.key_columns.push_back(OrderingColumnMetadata{
      .collation = Symbol(0),
      .order = BytecodeSortOrder::kDescending,
      .null_placement = BytecodeNullPlacement::kFirst,
  });
  input.record_comparisons.push_back(std::move(descriptor));
  for (std::uint32_t index = 0; index < 12; ++index) {
    input.instructions.emplace_back(
        LoadConstantInstruction{.constant = Constant(index), .output = Reg(index)});
  }
  input.instructions.emplace_back(CompareRecordsInstruction{
      .comparison = RecordComparison(0),
      .left_first = Reg(0),
      .right_first = Reg(2),
      .output = Reg(12),
  });
  input.instructions.emplace_back(CompareRecordsInstruction{
      .comparison = RecordComparison(0),
      .left_first = Reg(4),
      .right_first = Reg(6),
      .output = Reg(13),
  });
  input.instructions.emplace_back(CompareRecordsInstruction{
      .comparison = RecordComparison(0),
      .left_first = Reg(8),
      .right_first = Reg(10),
      .output = Reg(14),
  });
  input.result_columns = {
      ResultColumnMetadata{
          .name = "less",
          .declared_type = "INTEGER",
          .affinity = TypeAffinity::kInteger,
      },
      ResultColumnMetadata{
          .name = "greater",
          .declared_type = "INTEGER",
          .affinity = TypeAffinity::kInteger,
      },
      ResultColumnMetadata{
          .name = "equal",
          .declared_type = "INTEGER",
          .affinity = TypeAffinity::kInteger,
      },
  };
  input.instructions.emplace_back(ResultRowInstruction{.first = Reg(12), .count = 3});
  input.instructions.emplace_back(HaltInstruction{});
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram BuildReplaceBranchProgram(const Pager& pager) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager);
  input.register_count = 3;
  input.symbols.emplace_back("BINARY");
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(10));
  input.constants.push_back(SqlValue::Integer(20));
  input.constants.push_back(SqlValue::Integer(0));
  input.constants.push_back(SqlValue::Integer(1));
  input.relations.push_back(SorterDescriptor());
  input.result_columns = {
      ResultColumnMetadata{
          .name = "replaced",
          .declared_type = "INTEGER",
          .affinity = TypeAffinity::kInteger,
      },
  };
  input.instructions = {
      OpenRelationInstruction{.relation = Relation(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = RelationInsertMode::kKeepExisting,
          .duplicate_target = Address(4),
      },
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(1)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = RelationInsertMode::kReplaceExisting,
          .duplicate_target = Address(8),
      },
      LoadConstantInstruction{.constant = Constant(3), .output = Reg(2)},
      JumpInstruction{.target = Address(9)},
      LoadConstantInstruction{.constant = Constant(4), .output = Reg(2)},
      CloseRelationInstruction{.relation = Relation(0)},
      ResultRowInstruction{.first = Reg(2), .count = 1},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] TransactionCoordinator OpenWriteCoordinator(test::WritePagerFixedVfs& vfs) {
  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  if (pager == nullptr) {
    throw std::runtime_error{"failed to open VM write-test pager"};
  }
  return TakeValue(TransactionCoordinator::Open(std::move(pager_database)));
}

[[nodiscard]] ByteBuffer InitializedWriteDatabase(test::WritePagerFixedVfs& vfs) {
  TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
  TransactionStatement statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  RequireStatus(statement.writer()->InitializeDatabase());
  RequireStatus(statement.Succeed());
  return ByteBuffer::CopyOf(vfs.database_bytes());
}

[[nodiscard]] BytecodeProgram IndexRefillProgram(
    std::span<const std::array<SqlValue, 2>> records, bool unique = false,
    BytecodeSortOrder order = BytecodeSortOrder::kAscending) {
  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kCreateIndex;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.write_effects = ProgramWriteEffects{.multi_write = true, .may_abort = true};
  input.register_count = 2;
  input.symbols.emplace_back("BINARY");
  input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(0),
      .columns = {},
      .index_columns =
          {
              IndexColumnMetadata{.collation = Symbol(0), .order = order},
              IndexColumnMetadata{.collation = Symbol(0), .order = BytecodeSortOrder::kAscending},
          },
      .key_term_count = 1,
      .unique = unique,
      .pending_root = true,
      .storage = WriteCursorStorageKind::kIndex,
  });
  OrderingRecordDescriptor sorter = SorterDescriptor(Symbol(0), order);
  sorter.index_refill_cursor = WriteCursor(0);
  input.sorters.push_back(std::move(sorter));
  input.instructions = {
      CreateIndexRootInstruction{.cursor = WriteCursor(0), .output = Reg(0)},
      OpenSorterInstruction{.sorter = SorterId(0)},
  };
  for (const auto& record : records) {
    for (std::uint32_t field = 0; field < record.size(); ++field) {
      const ConstantId constant{static_cast<std::uint32_t>(input.constants.size())};
      input.constants.push_back(record[field].Clone());
      input.instructions.emplace_back(
          LoadConstantInstruction{.constant = constant, .output = Reg(field)});
    }
    input.instructions.emplace_back(
        InsertSorterInstruction{.sorter = SorterId(0), .first_value = Reg(0), .value_count = 2});
  }
  const auto rewind = static_cast<std::uint32_t>(input.instructions.size());
  input.instructions.emplace_back(
      RewindSorterInstruction{.sorter = SorterId(0), .empty_target = Address(rewind + 3U)});
  input.instructions.emplace_back(InsertSorterIndexInstruction{.sorter = SorterId(0)});
  input.instructions.emplace_back(
      NextSorterInstruction{.sorter = SorterId(0), .next_target = Address(rewind + 1U)});
  input.instructions.emplace_back(CloseWriteCursorInstruction{.cursor = WriteCursor(0)});
  input.instructions.emplace_back(CloseSorterInstruction{.sorter = SorterId(0)});
  input.instructions.emplace_back(HaltInstruction{});
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] Result<VmStep> ExecuteIndexRefill(
    const BytecodeProgram& program, test::WritePagerFixedVfs& vfs,
    std::optional<TemporaryStorageOptions> temporary_options =
        TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory},
    VmLimits limits = {}) {
  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  if (pager == nullptr) {
    throw std::runtime_error{"failed to open VM refill-test pager"};
  }
  Pager& execution_pager = *pager;
  TransactionCoordinator coordinator =
      TakeValue(TransactionCoordinator::Open(std::move(pager_database)));
  TransactionStatement statement = TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
      .access = StatementAccess::kWrite, .rollback = StatementRollbackMode::kStatement}));
  std::optional<TemporaryStorageFactory> temporary;
  if (temporary_options.has_value()) {
    temporary.emplace(
        TakeValue(TemporaryStorageFactory::Create(vfs, execution_pager, *temporary_options)));
  }
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core(), limits));
  RequireStatus(vm.AttachExecutionContext(
      temporary.has_value() ? VmExecutionContext{*statement.writer(), 0, *temporary}
                            : VmExecutionContext{*statement.writer(), 0}));
  Result<VmStep> stepped = vm.Step();
  RequireStatus(vm.DetachExecutionContext());
  if (stepped.has_value()) {
    RequireStatus(statement.Succeed());
  } else {
    RequireStatus(statement.Rollback());
  }
  return stepped;
}

[[nodiscard]] BytecodeProgram TableInsertProgram(SqlValue rowid, SqlValue value,
                                                 bool not_null = true) {
  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  input.statement_kind = ProgramStatementKind::kInsert;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.mutation_result = MutationResultMetadata{
      .publishes_changes = true,
      .publishes_last_insert_rowid = true,
  };
  input.register_count = 4;
  input.constants.push_back(std::move(rowid));
  input.constants.push_back(std::move(value));
  input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(1),
      .columns =
          {
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kInteger,
                  .not_null = true,
                  .rowid_alias = true,
                  .default_value = std::nullopt,
              },
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kText,
                  .not_null = not_null,
                  .rowid_alias = false,
                  .default_value = std::nullopt,
              },
          },
      .rowid_alias = 0,
      .index_columns = {},
      .key_term_count = 0,
      .unique = false,
      .unique_not_null = false,
      .storage = WriteCursorStorageKind::kRowIdTable,
  });
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(2)},
      OpenWriteCursorInstruction{.cursor = WriteCursor(0)},
      ResolveInsertRowIdInstruction{
          .cursor = WriteCursor(0),
          .input = Reg(0),
          .output = Reg(1),
      },
      BuildTableRecordInstruction{
          .cursor = WriteCursor(0),
          .first_value = Reg(1),
          .value_count = 2,
          .output = Reg(3),
      },
      InsertTableInstruction{
          .cursor = WriteCursor(0),
          .rowid = Reg(1),
          .record = Reg(3),
      },
      CloseWriteCursorInstruction{.cursor = WriteCursor(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram TableDeleteProgram(SqlValue rowid) {
  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  input.statement_kind = ProgramStatementKind::kDelete;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.mutation_result.publishes_changes = true;
  input.register_count = 1;
  input.constants.push_back(std::move(rowid));
  input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(1),
      .columns =
          {
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kInteger,
                  .not_null = true,
                  .rowid_alias = true,
                  .default_value = std::nullopt,
              },
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kText,
                  .not_null = false,
                  .rowid_alias = false,
                  .default_value = std::nullopt,
              },
          },
      .rowid_alias = 0,
      .index_columns = {},
      .key_term_count = 0,
      .unique = false,
      .unique_not_null = false,
      .storage = WriteCursorStorageKind::kRowIdTable,
  });
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenWriteCursorInstruction{.cursor = WriteCursor(0)},
      DeleteTableInstruction{.cursor = WriteCursor(0), .rowid = Reg(0)},
      CloseWriteCursorInstruction{.cursor = WriteCursor(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram TableDeleteScanProgram() {
  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  input.statement_kind = ProgramStatementKind::kDelete;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.mutation_result.publishes_changes = true;
  input.cursors.push_back(ReadCursorDescriptor{
      .root_page = RootPageNumber(1),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 2,
      .fields = {},
      .index_columns = {},
  });
  input.instructions = {
      OpenMutationCursorInstruction{.cursor = Cursor(0)},
      RewindInstruction{
          .cursor = Cursor(0),
          .empty_target = Address(4),
      },
      DeleteCurrentTableInstruction{
          .cursor = Cursor(0),
          .exhausted_target = Address(4),
      },
      JumpInstruction{.target = Address(2)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram TableUpdateProgram(SqlValue old_rowid, SqlValue new_rowid,
                                                 SqlValue value) {
  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  input.statement_kind = ProgramStatementKind::kUpdate;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.mutation_result.publishes_changes = true;
  input.register_count = 4;
  input.constants.push_back(std::move(old_rowid));
  input.constants.push_back(std::move(new_rowid));
  input.constants.push_back(std::move(value));
  input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(1),
      .columns =
          {
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kInteger,
                  .not_null = true,
                  .rowid_alias = true,
                  .default_value = std::nullopt,
              },
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kText,
                  .not_null = true,
                  .rowid_alias = false,
                  .default_value = std::nullopt,
              },
          },
      .rowid_alias = 0,
      .index_columns = {},
      .key_term_count = 0,
      .unique = false,
      .unique_not_null = false,
      .storage = WriteCursorStorageKind::kRowIdTable,
  });
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(2)},
      OpenWriteCursorInstruction{.cursor = WriteCursor(0)},
      CheckUpdateRowIdInstruction{
          .cursor = WriteCursor(0),
          .old_rowid = Reg(0),
          .new_rowid = Reg(1),
      },
      BuildTableRecordInstruction{
          .cursor = WriteCursor(0),
          .first_value = Reg(1),
          .value_count = 2,
          .output = Reg(3),
      },
      UpdateTableInstruction{
          .cursor = WriteCursor(0),
          .old_rowid = Reg(0),
          .new_rowid = Reg(1),
          .record = Reg(3),
      },
      CloseWriteCursorInstruction{.cursor = WriteCursor(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram TableUpdateScanProgram(SqlValue record) {
  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  input.statement_kind = ProgramStatementKind::kUpdate;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.mutation_result.publishes_changes = true;
  input.register_count = 1;
  input.constants.push_back(std::move(record));
  input.cursors.push_back(ReadCursorDescriptor{
      .root_page = RootPageNumber(1),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 2,
      .fields = {},
      .index_columns = {},
  });
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenMutationCursorInstruction{.cursor = Cursor(0)},
      RewindInstruction{
          .cursor = Cursor(0),
          .empty_target = Address(5),
      },
      UpdateCurrentTableInstruction{
          .cursor = Cursor(0),
          .record = Reg(0),
      },
      NextInstruction{
          .cursor = Cursor(0),
          .next_target = Address(3),
      },
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram CreateTablePrimitiveProgram(bool fail_after_cookie) {
  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  input.statement_kind = ProgramStatementKind::kCreateTable;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.register_count = fail_after_cookie ? 3U : 2U;
  if (fail_after_cookie) {
    input.constants.emplace_back();
  }
  input.instructions = {
      EnsureDatabaseInitializedInstruction{},
      CreateTableRootInstruction{.output = Reg(0)},
      IncrementSchemaCookieInstruction{.output = Reg(1)},
  };
  if (fail_after_cookie) {
    input.instructions.emplace_back(
        LoadConstantInstruction{.constant = Constant(0), .output = Reg(2)});
    input.instructions.emplace_back(MustBeIntegerInstruction{
        .input = Reg(2),
        .output = Reg(2),
    });
  }
  input.instructions.emplace_back(HaltInstruction{});
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] BytecodeProgram TableSeekProgram(SqlValue key, RowIdSeekMode mode) {
  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  input.register_count = 2;
  input.constants.push_back(std::move(key));
  input.cursors.push_back(ReadCursorDescriptor{
      .root_page = RootPageNumber(1),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 2,
      .fields = {},
      .index_columns = {},
  });
  input.result_columns.push_back(ResultColumnMetadata{
      .name = "rowid",
      .declared_type = "INTEGER",
      .affinity = TypeAffinity::kInteger,
  });
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      SeekRowIdInstruction{
          .cursor = Cursor(0),
          .key = Reg(0),
          .missing_target = Address(7),
          .mode = mode,
      },
      ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(1)},
      ResultRowInstruction{.first = Reg(1), .count = 1},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

[[nodiscard]] std::optional<std::int64_t> ExecuteTableSeek(test::WritePagerFixedVfs& vfs,
                                                           SqlValue key, RowIdSeekMode mode) {
  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  if (pager == nullptr) {
    throw std::runtime_error("failed to open rowid seek test pager");
  }
  RequireStatus(pager->BeginRead());
  const BytecodeProgram program = TableSeekProgram(std::move(key), mode);
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_database, 0}));
  const VmStep first = TakeValue(vm.Step());
  std::optional<std::int64_t> rowid;
  if (first == VmStep::kRow) {
    rowid = vm.row()[0].integer_value();
    if (!rowid.has_value() || TakeValue(vm.Step()) != VmStep::kDone) {
      throw std::runtime_error("rowid seek test returned an invalid row");
    }
  }
  RequireStatus(vm.DetachExecutionContext());
  RequireStatus(pager->EndRead());
  return rowid;
}

void InsertDirectWriteRow(test::WritePagerFixedVfs& vfs, std::int64_t rowid, std::string value) {
  TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
  TransactionStatement statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
  std::vector<SqlValue> fields;
  fields.emplace_back();
  fields.push_back(SqlValue::Text(std::move(value)));
  const ByteBuffer record = TakeValue(EncodeRecord(fields));
  RequireStatus(table.Insert(rowid, record.view()));
  RequireStatus(statement.Succeed());
}

[[nodiscard]] std::vector<std::pair<std::int64_t, std::vector<SqlValue>>> ReadWriteTableRows(
    test::WritePagerFixedVfs& vfs) {
  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  if (pager == nullptr) {
    throw std::runtime_error{"failed to reopen VM write-test pager"};
  }
  RequireStatus(pager->BeginRead());
  std::vector<std::pair<std::int64_t, std::vector<SqlValue>>> rows;
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    bool has_row = TakeValue(cursor.First());
    while (has_row) {
      const std::int64_t rowid = TakeValue(cursor.rowid());
      const ByteBuffer payload = TakeValue(cursor.CopyPayload());
      rows.emplace_back(rowid, TakeValue(DecodeRecord(payload.view())));
      has_row = TakeValue(cursor.Next());
    }
  }
  RequireStatus(pager->EndRead());
  return rows;
}

void ExpectInteger(const SqlValue& value, std::int64_t expected) {
  EXPECT_EQ(value.type(), SqlValueType::kInteger);
  EXPECT_EQ(value.integer_value(), expected);
}

void ExpectReal(const SqlValue& value, double expected) {
  EXPECT_EQ(value.type(), SqlValueType::kReal);
  ASSERT_TRUE(value.real_value().has_value());
  EXPECT_DOUBLE_EQ(*value.real_value(), expected);
}

void ExpectText(const SqlValue& value, std::string_view expected) {
  EXPECT_EQ(value.type(), SqlValueType::kText);
  ASSERT_TRUE(value.text_value().has_value());
  EXPECT_EQ(value.text_value()->bytes(), expected);
}

[[nodiscard]] ReadCursorDescriptor TableDescriptor(std::vector<CursorFieldSource> fields,
                                                   std::uint32_t record_field_count = 7) {
  return ReadCursorDescriptor{
      .root_page = RootPageNumber(2),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = record_field_count,
      .fields = std::move(fields),
      .index_columns = {},
  };
}

[[nodiscard]] ReadCursorDescriptor BinaryIndexDescriptor() {
  return ReadCursorDescriptor{
      .root_page = RootPageNumber(76),
      .storage = CursorStorageKind::kIndex,
      .record_field_count = 2,
      .fields =
          {
              CursorFieldSource{
                  .kind = CursorFieldSourceKind::kRecordField,
                  .record_field = 0,
              },
              CursorFieldSource{
                  .kind = CursorFieldSourceKind::kRecordField,
                  .record_field = 1,
              },
          },
      .index_columns =
          {
              IndexColumnMetadata{
                  .collation = Symbol(0),
                  .order = BytecodeSortOrder::kAscending,
              },
              IndexColumnMetadata{
                  .collation = Symbol(0),
                  .order = BytecodeSortOrder::kAscending,
              },
          },
      .index_key_field_count = 2,
  };
}

class VmTest : public ::testing::Test {
 protected:
  void SetUp() override {
    database_ = TakeValue(BtreeDatabase::Open(vfs_, FixturePath().string(),
                                              PagerOptions{.cache_capacity_pages = 256}));
    pager_ = &database_->pager();
    RequireStatus(pager_->BeginRead());
  }

  void TearDown() override {
    if (pager_ != nullptr && pager_->in_read_transaction()) {
      const Status ended = pager_->EndRead();
      EXPECT_TRUE(ended.has_value()) << ended.error().ToString();
    }
  }

  [[nodiscard]] Vm CreateCoreVm(const BytecodeProgram& program, VmLimits limits = {}) {
    return CreateAttachedVm(program, *database_, kCatalogGeneration, VmEnvironment::Core(), limits);
  }

  PosixVfs vfs_;
  std::unique_ptr<BtreeDatabase> database_;
  Pager* pager_ = nullptr;
};

[[nodiscard]] BytecodeProgram PrivateReadSubroutineProgram(const Pager& pager) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager, 0);
  input.register_count = 3;
  input.constants.push_back(SqlValue::Integer(0));
  const ReadCursorDescriptor cursor{
      .root_page = RootPageNumber(1),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 2,
      .fields = {},
      .index_columns = {},
  };
  input.cursors = {cursor, cursor};
  input.result_columns = {ResultColumn("callee"), ResultColumn("caller")};
  input.subroutines.push_back(
      SubroutineDescriptor{.entry = Address(7), .end = Address(15), .kind = SubroutineKind::kRead});
  input.instructions = {
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      NullRowInstruction{.cursor = Cursor(0)},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(2)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      ResultRowInstruction{.first = Reg(1), .count = 2},
      HaltInstruction{},
      OpenReadCursorInstruction{.cursor = Cursor(1)},
      RewindInstruction{.cursor = Cursor(1), .empty_target = Address(12)},
      ReadRowIdInstruction{.cursor = Cursor(1), .output = Reg(1)},
      CloseCursorInstruction{.cursor = Cursor(1)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      CloseCursorInstruction{.cursor = Cursor(1)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
  };
  return TakeProgramValue(BytecodeProgram::Create(input));
}

TEST(VmReadSubroutine, PreservesCallerRowsAcrossNormalEmptyAndRepeatedPrivateReads) {
  for (const bool populated : {false, true}) {
    SCOPED_TRACE(populated);
    test::WritePagerFixedVfs vfs{false};
    [[maybe_unused]] const auto initialized = InitializedWriteDatabase(vfs);
    if (populated) {
      InsertDirectWriteRow(vfs, 9, "private");
    }
    std::unique_ptr<BtreeDatabase> pager_database =
        test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    const auto program = PrivateReadSubroutineProgram(*pager);
    Vm vm = CreateAttachedVm(program, *pager_database, 0);
    for (std::size_t iteration = 0; iteration < 3U; ++iteration) {
      ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
      ASSERT_EQ(2U, vm.row().size());
      ExpectInteger(vm.row()[0], populated ? 9 : 0);
      EXPECT_EQ(SqlValueType::kNull, vm.row()[1].type());
      EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
      RequireStatus(vm.Reset());
      RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_database, 0}));
    }
    RequireStatus(pager->EndRead());
  }
}

TEST(VmReadSubroutine, RecoversPrivateCursorIoFailureWithoutRetainingContinuations) {
  test::WritePagerFixedVfs vfs{false};
  [[maybe_unused]] const auto initialized = InitializedWriteDatabase(vfs);
  InsertDirectWriteRow(vfs, 9, "private");
  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const auto program = PrivateReadSubroutineProgram(*pager);
  Vm vm = CreateAttachedVm(program, *pager_database, 0);
  vfs.FailDatabaseReadAfter(0U, ErrorCode::kIo);
  const auto failed = vm.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_EQ(VmState::kError, vm.state());
  RequireStatus(vm.Reset());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_database, 0}));
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(vm.row()[0], 9);
  EXPECT_EQ(SqlValueType::kNull, vm.row()[1].type());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(pager->EndRead());
}

TEST(VmReadSubroutine, PreservesParentCapabilitiesAcrossNestedReadCalls) {
  test::WritePagerFixedVfs vfs{false};
  [[maybe_unused]] const auto initialized = InitializedWriteDatabase(vfs);
  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const ReadCursorDescriptor cursor{
      .root_page = RootPageNumber(1),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 2,
      .fields = {},
      .index_columns = {},
  };
  for (const std::uint32_t count : {1U, 8U, 33U, 64U}) {
    SCOPED_TRACE(count);
    ProgramInput input;
    input.schema_version = CurrentSchema(*pager, 0);
    input.register_count = 1;
    input.cursors = std::vector<ReadCursorDescriptor>(count, cursor);
    input.result_columns = {ResultColumn()};
    input.instructions = {
        CallSubroutineInstruction{.subroutine = SubroutineId{0}},
        ResultRowInstruction{.first = Reg(0), .count = 1},
        HaltInstruction{},
    };
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto entry = static_cast<std::uint32_t>(input.instructions.size());
      input.instructions.push_back(OpenReadCursorInstruction{.cursor = Cursor(index)});
      input.instructions.push_back(NullRowInstruction{.cursor = Cursor(index)});
      if (index + 1U < count) {
        input.instructions.push_back(
            CallSubroutineInstruction{.subroutine = SubroutineId{index + 1U}});
      }
      input.instructions.push_back(ReadRowIdInstruction{.cursor = Cursor(index), .output = Reg(0)});
      input.instructions.push_back(CloseCursorInstruction{.cursor = Cursor(index)});
      input.instructions.push_back(ReturnSubroutineInstruction{.subroutine = SubroutineId{index}});
      input.subroutines.push_back(SubroutineDescriptor{
          .entry = Address(entry),
          .end = Address(static_cast<std::uint32_t>(input.instructions.size())),
          .kind = SubroutineKind::kRead,
      });
    }
    const auto program = TakeProgramValue(BytecodeProgram::Create(input));
    EXPECT_EQ(count, program.verification_metrics().maximum_subroutine_depth);
    Vm vm = CreateAttachedVm(program, *pager_database, 0);
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    EXPECT_EQ(SqlValueType::kNull, OnlyRowValue(vm).type());
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  }
  RequireStatus(pager->EndRead());
}

TEST_F(VmTest, ExecutesPrivateReadTemporaryAndAggregateLifecycles) {
  const TemporaryStorageFactory factory = TakeValue(TemporaryStorageFactory::Create(
      vfs_, *pager_,
      TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory,
                              .sorter_memory_threshold = ByteCount{1U << 20U}}));
  for (const std::uint32_t family : {0U, 1U, 2U, 3U}) {
    SCOPED_TRACE(family);
    ProgramInput input;
    input.schema_version = CurrentSchema(*pager_);
    input.register_count = 2;
    input.parameter_count = 1;
    input.symbols = {"BINARY", "sum"};
    input.result_columns = {ResultColumn()};
    input.instructions = {
        LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
        CallSubroutineInstruction{.subroutine = SubroutineId{0}},
        ResultRowInstruction{.first = Reg(1), .count = 1},
        HaltInstruction{},
    };
    if (family == 0U) {
      input.sorters.push_back(SorterDescriptor());
      input.instructions.push_back(OpenSorterInstruction{.sorter = SorterId{0}});
      input.instructions.push_back(CloseSorterInstruction{.sorter = SorterId{0}});
    } else if (family == 1U) {
      input.top_ns.push_back(SorterDescriptor());
      input.instructions.push_back(OpenTopNInstruction{.top_n = TopNId{0}, .bound = Reg(0)});
      input.instructions.push_back(CloseTopNInstruction{.top_n = TopNId{0}});
    } else if (family == 2U) {
      input.relations.push_back(SorterDescriptor());
      input.instructions.push_back(OpenRelationInstruction{.relation = RelationId{0}});
      input.instructions.push_back(CloseRelationInstruction{.relation = RelationId{0}});
    } else {
      input.aggregates.push_back(AggregateDescriptorFor(Symbol(1), Symbol(0), 1));
      input.instructions.push_back(OpenAggregateInstruction{.aggregate = AggregateId{0}});
      input.instructions.push_back(
          StepAggregateInstruction{.aggregate = AggregateId{0}, .first_argument = Reg(0)});
      input.instructions.push_back(
          FinalizeAggregateInstruction{.aggregate = AggregateId{0}, .output = Reg(1)});
      input.instructions.push_back(CloseAggregateInstruction{.aggregate = AggregateId{0}});
    }
    if (family != 3U) {
      input.instructions.push_back(CopyInstruction{.input = Reg(0), .output = Reg(1)});
    }
    input.instructions.push_back(ReturnSubroutineInstruction{.subroutine = SubroutineId{0}});
    input.subroutines.push_back(SubroutineDescriptor{
        .entry = Address(4),
        .end = Address(static_cast<std::uint32_t>(input.instructions.size())),
        .kind = SubroutineKind::kRead,
    });
    const auto program = TakeProgramValue(BytecodeProgram::Create(input));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(
        vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));
    for (const std::int64_t value : {1, 3}) {
      RequireStatus(vm.Bind(Parameter(0), SqlValue::Integer(value)));
      ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
      ExpectInteger(OnlyRowValue(vm), value);
      EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
      RequireStatus(vm.Reset());
      RequireStatus(
          vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));
    }
  }
}

TEST(VmReadSubroutine, RecoversPrivateSorterFactoryAndPmaWriteFailures) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, test::kWritePagerInputPath));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager);
  input.register_count = 2;
  input.constants.push_back(SqlValue::Integer(4096));
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(0));
  input.symbols = {"BINARY"};
  input.result_columns = {ResultColumn()};
  input.sorters.push_back(OrderingRecordDescriptor{
      .field_count = 1,
      .key_field_count = 1,
      .key_columns = {OrderingColumnMetadata{.collation = Symbol(0)}},
  });
  input.subroutines.push_back(
      SubroutineDescriptor{.entry = Address(5), .end = Address(16), .kind = SubroutineKind::kRead});
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      HaltInstruction{},
      OpenSorterInstruction{.sorter = SorterId{0}},
      InsertSorterInstruction{.sorter = SorterId{0}, .first_value = Reg(0), .value_count = 1},
      BinaryInstruction{.operation = BinaryOperation::kSubtract,
                        .left = Reg(0),
                        .right = Reg(1),
                        .output = Reg(0)},
      JumpIfInstruction{.condition = JumpCondition::kIfTrue, .input = Reg(0), .target = Address(6)},
      RewindSorterInstruction{.sorter = SorterId{0}, .empty_target = Address(13)},
      ReadSorterFieldInstruction{.sorter = SorterId{0}, .field = 0, .output = Reg(0)},
      CloseSorterInstruction{.sorter = SorterId{0}},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(0)},
      CloseSorterInstruction{.sorter = SorterId{0}},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
  };
  const auto program = TakeProgramValue(BytecodeProgram::Create(input));
  const auto factory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager,
      TemporaryStorageOptions{.mode = TemporaryStoreMode::kFile,
                              .sorter_memory_threshold = ByteCount{32U << 10U}}));
  Vm vm = CreateAttachedVm(program, *pager_database, kCatalogGeneration);
  const auto missing = vm.Step();
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing.error().code());
  RequireStatus(vm.Reset());
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  vfs.FailPathlessWriteAfter(0U, ErrorCode::kIo);
  const auto failed = vm.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_GT(vfs.pathless_open_count(), 0U);
  EXPECT_FALSE(vfs.pathless_file_present());
  RequireStatus(vm.Reset());
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 1);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  EXPECT_FALSE(vfs.pathless_file_present());
  RequireStatus(pager->EndRead());
}

TEST_F(VmTest, ExecutesBindingsRowsHaltAndReset) {
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Integer(40));
  const BytecodeProgram program =
      BuildProgram(*pager_, 3, 1, std::move(constants), {}, {}, {ResultColumn("sum")},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(1)},
                       BinaryInstruction{
                           .operation = BinaryOperation::kAdd,
                           .left = Reg(0),
                           .right = Reg(1),
                           .output = Reg(2),
                       },
                       ResultRowInstruction{.first = Reg(2), .count = 1},
                       HaltInstruction{},
                   });

  Vm vm = CreateCoreVm(program);
  EXPECT_EQ(vm.state(), VmState::kReady);

  const SqlValue binding = SqlValue::Integer(2);
  RequireStatus(vm.Bind(Parameter(0), binding));
  EXPECT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  EXPECT_EQ(vm.state(), VmState::kRow);
  ExpectInteger(OnlyRowValue(vm), 42);
  EXPECT_EQ(vm.executed_instruction_count(), 4U);
  const Status rebound_while_suspended = vm.Bind(Parameter(0), binding);
  ASSERT_FALSE(rebound_while_suspended.has_value());
  EXPECT_EQ(rebound_while_suspended.error().code(), ErrorCode::kMisuse);

  EXPECT_EQ(TakeValue(vm.Step()), VmStep::kDone);
  EXPECT_EQ(vm.state(), VmState::kDone);
  EXPECT_TRUE(vm.row().empty());
  EXPECT_EQ(vm.executed_instruction_count(), 5U);
  const auto repeated = vm.Step();
  ASSERT_FALSE(repeated.has_value());
  EXPECT_EQ(repeated.error().code(), ErrorCode::kMisuse);

  RequireStatus(vm.Reset());
  EXPECT_EQ(vm.state(), VmState::kReady);
  EXPECT_EQ(vm.executed_instruction_count(), 0U);
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ExpectInteger(OnlyRowValue(vm), 42);

  RequireStatus(vm.Reset());
  RequireStatus(vm.ClearBindings());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  EXPECT_EQ(OnlyRowValue(vm).type(), SqlValueType::kNull);
}

TEST_F(VmTest, ExecutesNestedTypedSubroutinesAndRebindsWithoutLosingContinuations) {
  for (const std::uint32_t count : {1U, 8U, 64U}) {
    SCOPED_TRACE(count);
    ProgramInput input;
    input.schema_version = CurrentSchema(*pager_);
    input.register_count = 2;
    input.parameter_count = 1;
    input.constants.push_back(SqlValue::Integer(1));
    input.result_columns.push_back(ResultColumn());
    input.instructions = {
        LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
        LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
        CallSubroutineInstruction{.subroutine = SubroutineId{0}},
        ResultRowInstruction{.first = Reg(0), .count = 1},
        HaltInstruction{},
    };
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto entry = static_cast<std::uint32_t>(input.instructions.size());
      if (index + 1U < count) {
        input.instructions.push_back(
            CallSubroutineInstruction{.subroutine = SubroutineId{index + 1U}});
      }
      input.instructions.push_back(BinaryInstruction{
          .operation = BinaryOperation::kAdd, .left = Reg(0), .right = Reg(1), .output = Reg(0)});
      input.instructions.push_back(ReturnSubroutineInstruction{.subroutine = SubroutineId{index}});
      input.subroutines.push_back(SubroutineDescriptor{
          .entry = Address(entry),
          .end = Address(static_cast<std::uint32_t>(input.instructions.size())),
      });
    }
    const auto program = TakeProgramValue(BytecodeProgram::Create(input));
    EXPECT_EQ(count, program.verification_metrics().maximum_subroutine_depth);
    Vm vm = CreateCoreVm(program);
    RequireStatus(vm.Bind(Parameter(0), SqlValue::Integer(10)));
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ExpectInteger(OnlyRowValue(vm), 10 + count);
    EXPECT_EQ(3U * count + 3U, vm.executed_instruction_count());
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    RequireStatus(vm.Reset());
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ExpectInteger(OnlyRowValue(vm), 10 + count);
    RequireStatus(vm.Reset());
    RequireStatus(vm.Bind(Parameter(0), SqlValue::Integer(20)));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ExpectInteger(OnlyRowValue(vm), 20 + count);
  }
}

TEST_F(VmTest, ResetsInterruptedSubroutineContinuationsBeforeAnotherExecution) {
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager_);
  input.register_count = 2;
  input.parameter_count = 1;
  input.constants.push_back(SqlValue::Integer(42));
  input.result_columns.push_back(ResultColumn());
  input.subroutines.push_back(SubroutineDescriptor{.entry = Address(8), .end = Address(11)});
  input.instructions = {
      LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
      JumpIfInstruction{.condition = JumpCondition::kIfTrue, .input = Reg(0), .target = Address(5)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      ResultRowInstruction{.first = Reg(1), .count = 1},
      HaltInstruction{},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      ResultRowInstruction{.first = Reg(1), .count = 1},
      HaltInstruction{},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      CopyInstruction{.input = Reg(1), .output = Reg(1)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
  };
  const auto program = TakeProgramValue(BytecodeProgram::Create(input));
  Vm vm = CreateCoreVm(program, VmLimits{.maximum_instructions_per_step = 4});
  RequireStatus(vm.Bind(Parameter(0), SqlValue::Integer(1)));
  const auto interrupted = vm.Step();
  ASSERT_FALSE(interrupted.has_value());
  EXPECT_EQ(ErrorCode::kInterrupted, interrupted.error().code());
  EXPECT_EQ(VmState::kError, vm.state());
  RequireStatus(vm.Reset());
  RequireStatus(vm.Bind(Parameter(0), SqlValue::Integer(0)));
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 42);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, RecoversTypedScalarErrorsFromSubroutinesWithoutLosingBindings) {
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager_);
  input.register_count = 1;
  input.parameter_count = 1;
  input.symbols = {"BINARY", "abs"};
  input.result_columns.push_back(ResultColumn());
  input.subroutines.push_back(SubroutineDescriptor{.entry = Address(4), .end = Address(6)});
  input.instructions = {
      LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      HaltInstruction{},
      CallScalarInstruction{.function = Symbol(1),
                            .collation = Symbol(0),
                            .first_argument = Reg(0),
                            .argument_count = 1,
                            .output = Reg(0)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
  };
  const auto program = TakeProgramValue(BytecodeProgram::Create(input));
  Vm vm = CreateCoreVm(program);
  RequireStatus(vm.Bind(Parameter(0), SqlValue::Integer(std::numeric_limits<std::int64_t>::min())));
  const auto failed = vm.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_EQ(VmState::kError, vm.state());
  RequireStatus(vm.Reset());
  RequireStatus(vm.Bind(Parameter(0), SqlValue::Integer(-7)));
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 7);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, MatchesIndependentSharedSubroutineDagModel) {
  std::minstd_rand random{0x53756231U};
  for (std::uint32_t example = 0; example < 32U; ++example) {
    const std::uint32_t count = example < 3U ? std::array{2U, 8U, 16U}[example]
                                             : 2U + static_cast<std::uint32_t>(random() % 15U);
    SCOPED_TRACE(example);
    SCOPED_TRACE(count);
    std::vector<std::optional<std::uint32_t>> extra_targets(count);
    for (std::uint32_t index = 0; index + 2U < count; ++index) {
      if (random() % 2U == 0U) {
        extra_targets[index] =
            index + 2U + static_cast<std::uint32_t>(random() % (count - index - 2U));
      }
    }
    std::vector<std::int64_t> contributions(count, 1);
    for (std::size_t index = count; index-- > 0U;) {
      if (index + 1U < count) {
        contributions[index] += contributions[index + 1U];
      }
      if (extra_targets[index].has_value()) {
        contributions[index] += contributions[*extra_targets[index]];
      }
    }
    ProgramInput input;
    input.schema_version = CurrentSchema(*pager_);
    input.register_count = 2;
    input.parameter_count = 1;
    input.constants.push_back(SqlValue::Integer(1));
    input.result_columns.push_back(ResultColumn());
    input.instructions = {
        LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
        LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
        CallSubroutineInstruction{.subroutine = SubroutineId{count - 1U}},
        CallSubroutineInstruction{.subroutine = SubroutineId{0}},
        ResultRowInstruction{.first = Reg(0), .count = 1},
        HaltInstruction{},
    };
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto entry = static_cast<std::uint32_t>(input.instructions.size());
      if (index + 1U < count) {
        input.instructions.push_back(
            CallSubroutineInstruction{.subroutine = SubroutineId{index + 1U}});
      }
      if (extra_targets[index].has_value()) {
        input.instructions.push_back(
            CallSubroutineInstruction{.subroutine = SubroutineId{*extra_targets[index]}});
      }
      input.instructions.push_back(BinaryInstruction{
          .operation = BinaryOperation::kAdd, .left = Reg(0), .right = Reg(1), .output = Reg(0)});
      input.instructions.push_back(ReturnSubroutineInstruction{.subroutine = SubroutineId{index}});
      input.subroutines.push_back(SubroutineDescriptor{
          .entry = Address(entry),
          .end = Address(static_cast<std::uint32_t>(input.instructions.size())),
      });
    }
    const auto program = TakeProgramValue(BytecodeProgram::Create(input));
    EXPECT_EQ(count, program.verification_metrics().maximum_subroutine_depth);
    Vm vm = CreateCoreVm(program);
    for (const std::int64_t seed : {-1, 0, 10}) {
      RequireStatus(vm.Bind(Parameter(0), SqlValue::Integer(seed)));
      ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
      ExpectInteger(OnlyRowValue(vm), seed + 1 + contributions.front());
      EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
      RequireStatus(vm.Reset());
      RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
    }
  }
}

TEST_F(VmTest, ExecutesLoopingSubroutineThroughTheCanonicalRegisterLattice) {
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager_);
  input.register_count = 4;
  input.constants.push_back(SqlValue::Integer(0));
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(3));
  input.symbols = {"BINARY"};
  input.result_columns.push_back(ResultColumn());
  input.subroutines.push_back(SubroutineDescriptor{.entry = Address(6), .end = Address(10)});
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(2)},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      HaltInstruction{},
      BinaryInstruction{
          .operation = BinaryOperation::kAdd, .left = Reg(0), .right = Reg(1), .output = Reg(0)},
      CompareInstruction{.comparison = SqlComparison::kLess,
                         .affinity = TypeAffinity::kNumeric,
                         .collation = Symbol(0),
                         .left = Reg(0),
                         .right = Reg(2),
                         .output = Reg(3)},
      JumpIfInstruction{.condition = JumpCondition::kIfTrue, .input = Reg(3), .target = Address(6)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
  };
  const auto program = TakeProgramValue(BytecodeProgram::Create(input));
  Vm vm = CreateCoreVm(program);
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 3);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, CollectsAndIteratesBoundedRowidLists) {
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Integer(3));
  constants.push_back(SqlValue::Integer(1));
  constants.push_back(SqlValue::Integer(2));
  const BytecodeProgram program =
      BuildProgram(*pager_, 2, 0, std::move(constants), {}, {}, {ResultColumn("rowid")},
                   {
                       ClearRowIdListInstruction{},
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       AppendRowIdListInstruction{.input = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(1), .output = Reg(0)},
                       AppendRowIdListInstruction{.input = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(2), .output = Reg(0)},
                       AppendRowIdListInstruction{.input = Reg(0)},
                       RewindRowIdListInstruction{
                           .output = Reg(1),
                           .empty_target = Address(11),
                       },
                       ResultRowInstruction{.first = Reg(1), .count = 1},
                       NextRowIdListInstruction{
                           .output = Reg(1),
                           .next_target = Address(8),
                       },
                       HaltInstruction{},
                       HaltInstruction{},
                   });

  Vm vm = CreateCoreVm(program);
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(3, OnlyRowValue(vm).integer_value());
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(1, OnlyRowValue(vm).integer_value());
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(2, OnlyRowValue(vm).integer_value());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));

  RequireStatus(vm.Reset());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(3, OnlyRowValue(vm).integer_value());
  RequireStatus(vm.Reset());

  std::vector<SqlValue> overflow_constants;
  overflow_constants.push_back(SqlValue::Integer(1));
  overflow_constants.push_back(SqlValue::Integer(2));
  overflow_constants.push_back(SqlValue::Integer(3));
  const BytecodeProgram overflow =
      BuildProgram(*pager_, 1, 0, std::move(overflow_constants), {}, {}, {},
                   {
                       ClearRowIdListInstruction{},
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       AppendRowIdListInstruction{.input = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(1), .output = Reg(0)},
                       AppendRowIdListInstruction{.input = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(2), .output = Reg(0)},
                       AppendRowIdListInstruction{.input = Reg(0)},
                       HaltInstruction{},
                   });
  Vm limited = CreateCoreVm(overflow, VmLimits{.maximum_value_bytes = 16});
  const auto too_large = limited.Step();
  ASSERT_FALSE(too_large.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, too_large.error().code());
  RequireStatus(limited.DetachExecutionContext());

  std::vector<SqlValue> invalid_constants;
  invalid_constants.push_back(SqlValue::Text("1.5"));
  const BytecodeProgram invalid =
      BuildProgram(*pager_, 1, 0, std::move(invalid_constants), {}, {}, {},
                   {
                       ClearRowIdListInstruction{},
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       AppendRowIdListInstruction{.input = Reg(0)},
                       HaltInstruction{},
                   });
  Vm invalid_vm = CreateCoreVm(invalid);
  const auto mismatch = invalid_vm.Step();
  ASSERT_FALSE(mismatch.has_value());
  EXPECT_EQ(ErrorCode::kTypeMismatch, mismatch.error().code());
  RequireStatus(invalid_vm.DetachExecutionContext());
}

TEST_F(VmTest, AttachesAndDetachesExecutionContextsExplicitly) {
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Integer(7));
  const BytecodeProgram program =
      BuildProgram(*pager_, 1, 0, std::move(constants), {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   });

  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  EXPECT_FALSE(vm.has_execution_context());
  const auto detached_step = vm.Step();
  ASSERT_FALSE(detached_step.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, detached_step.error().code());
  EXPECT_EQ(VmState::kReady, vm.state());

  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_TRUE(vm.has_execution_context());
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  const Status suspended_detach = vm.DetachExecutionContext();
  ASSERT_FALSE(suspended_detach.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, suspended_detach.error().code());

  RequireStatus(vm.Reset());
  EXPECT_FALSE(vm.has_execution_context());
  EXPECT_EQ(VmState::kReady, vm.state());

  ProgramInput write_input;
  write_input.schema_version = CurrentSchema(*pager_);
  write_input.statement_kind = ProgramStatementKind::kUpdate;
  write_input.transaction_access = ProgramTransactionAccess::kWrite;
  write_input.mutation_result.publishes_changes = true;
  write_input.instructions.emplace_back(HaltInstruction{});
  const BytecodeProgram write_program = TakeProgramValue(BytecodeProgram::Create(write_input));
  Vm write_vm = TakeValue(Vm::Create(write_program, VmEnvironment::Core()));
  const Status missing_writer =
      write_vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration});
  ASSERT_FALSE(missing_writer.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing_writer.error().code());
  EXPECT_FALSE(write_vm.has_execution_context());
}

TEST(VmWriteTest, InsertsGeneratedRowidRecordAndPublishesMutationResults) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(InitializedWriteDatabase(vfs));
  TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
  TransactionStatement statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  ASSERT_NE(nullptr, statement.writer());

  const BytecodeProgram program = TableInsertProgram(SqlValue{}, SqlValue::Integer(42));
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  EXPECT_EQ(1U, vm.change_count());
  EXPECT_EQ(std::optional<std::int64_t>{1}, vm.last_insert_rowid_event());
  RequireStatus(vm.DetachExecutionContext());
  RequireStatus(statement.Succeed());

  const auto rows = ReadWriteTableRows(vfs);
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0].first);
  ASSERT_EQ(2U, rows[0].second.size());
  EXPECT_EQ(SqlValueType::kNull, rows[0].second[0].type());
  ExpectText(rows[0].second[1], "42");
}

TEST(VmWriteTest, RejectsDuplicateRowidAndNotNullBeforePublishingChanges) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(InitializedWriteDatabase(vfs));
  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    const BytecodeProgram seed = TableInsertProgram(SqlValue::Integer(1), SqlValue::Text("seed"));
    Vm vm = TakeValue(Vm::Create(seed, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }

  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    const BytecodeProgram duplicate =
        TableInsertProgram(SqlValue::Text("1"), SqlValue::Text("duplicate"));
    Vm vm = TakeValue(Vm::Create(duplicate, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    const auto inserted = vm.Step();
    ASSERT_FALSE(inserted.has_value());
    EXPECT_EQ(ErrorCode::kConstraint, inserted.error().code());
    EXPECT_EQ(0U, vm.change_count());
    EXPECT_FALSE(vm.last_insert_rowid_event().has_value());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Rollback());
  }

  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    const BytecodeProgram fractional =
        TableInsertProgram(SqlValue::Text("1.5"), SqlValue::Text("fractional"));
    Vm vm = TakeValue(Vm::Create(fractional, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    const auto inserted = vm.Step();
    ASSERT_FALSE(inserted.has_value());
    EXPECT_EQ(ErrorCode::kTypeMismatch, inserted.error().code());
    EXPECT_EQ(0U, vm.change_count());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Rollback());
  }

  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    const BytecodeProgram not_null = TableInsertProgram(SqlValue{}, SqlValue{});
    Vm vm = TakeValue(Vm::Create(not_null, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    const auto inserted = vm.Step();
    ASSERT_FALSE(inserted.has_value());
    EXPECT_EQ(ErrorCode::kConstraint, inserted.error().code());
    EXPECT_EQ(0U, vm.change_count());
    EXPECT_FALSE(vm.last_insert_rowid_event().has_value());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Rollback());
  }

  const auto rows = ReadWriteTableRows(vfs);
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0].first);
  ExpectText(rows[0].second[1], "seed");
}

TEST(VmWriteTest, RetainsEquivalentAnalyzePrefixesAndNativeComparedWidths) {
  class TraceCollation final : public Collation {
   public:
    explicit TraceCollation(std::string_view name) : name_(name) {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] std::weak_ordering Compare(Utf8View left,
                                             Utf8View right) const noexcept override {
      if (comparisons < arguments.size()) {
        arguments[comparisons] = {left.bytes().empty() ? '\0' : left.bytes().front(),
                                  right.bytes().empty() ? '\0' : right.bytes().front()};
      }
      ++comparisons;
      return NoCaseCollation().Compare(left, right);
    }

    void Reset() const noexcept {
      comparisons = 0;
      arguments = {};
    }

    mutable std::size_t comparisons = 0;
    mutable std::array<std::pair<char, char>, 2> arguments{};

   private:
    std::string_view name_;
  };
  const TraceCollation head{"STAT1_HEAD"};
  const TraceCollation tail{"STAT1_TAIL"};
  const std::array<const Collation*, 3> collations{&BinaryCollation(), &head, &tail};
  const std::array<ScalarFunction, 1> functions{
      ScalarFunction{"check_stat1", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone,
                     [](const ScalarFunctionContext&,
                        std::span<const SqlValue> arguments) -> Result<SqlValue> {
                       ExpectText(arguments[0], "3 3 1");
                       return SqlValue{};
                     }},
  };
  const FunctionRegistry registry{functions};
  test::WritePagerFixedVfs vfs{false};
  PageNumber root{0};
  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(statement.writer()->InitializeDatabase());
    const std::array<IndexColumnOrder, 3> columns{IndexColumnOrder{head}, IndexColumnOrder{tail},
                                                  IndexColumnOrder{BinaryCollation()}};
    IndexBtreeWriter index = TakeValue(modern_sqlite::test::CreateIndexBtree(
        *(statement.writer()), columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));
    root = index.root_page();
    const std::array<std::pair<std::string_view, std::string_view>, 3> keys{
        std::pair{"A", "x"}, std::pair{"a", "y"}, std::pair{"a", "z"}};
    std::int64_t rowid = 0;
    for (const auto& [first, second] : keys) {
      const std::array values{SqlValue::Text(std::string{first}),
                              SqlValue::Text(std::string{second}), SqlValue::Integer(++rowid)};
      RequireStatus(index.Insert(values));
    }
    RequireStatus(statement.Succeed());
  }
  for (const Stat1IndexPolicy policy : {Stat1IndexPolicy::Ordinary, Stat1IndexPolicy::UniqueNotNull,
                                        Stat1IndexPolicy::PrimaryWithoutRowid}) {
    head.Reset();
    tail.Reset();
    ProgramInput input;
    input.statement_kind = ProgramStatementKind::kAnalyze;
    input.transaction_access = ProgramTransactionAccess::kWrite;
    input.rollback_mode = ProgramRollbackMode::kStatement;
    input.register_count = 2;
    input.symbols = {"BINARY", "STAT1_HEAD", "STAT1_TAIL", "check_stat1"};
    input.cursors.push_back(ReadCursorDescriptor{
        .root_page = RootPageNumber(root.value()),
        .storage = CursorStorageKind::kIndex,
        .record_field_count = 3,
        .fields = {},
        .index_columns =
            {
                IndexColumnMetadata{.collation = Symbol(1), .order = BytecodeSortOrder::kAscending},
                IndexColumnMetadata{.collation = Symbol(2), .order = BytecodeSortOrder::kAscending},
                IndexColumnMetadata{.collation = Symbol(0), .order = BytecodeSortOrder::kAscending},
            },
        .index_key_field_count = policy == Stat1IndexPolicy::UniqueNotNull ||
                                         policy == Stat1IndexPolicy::PrimaryWithoutRowid
                                     ? 2U
                                     : 3U,
    });
    input.instructions = {
        ComputeIndexStat1Instruction{
            .cursor = Cursor(0), .key_term_count = 2, .policy = policy, .output = Reg(0)},
        CallScalarInstruction{.function = Symbol(3),
                              .collation = Symbol(0),
                              .first_argument = Reg(0),
                              .argument_count = 1,
                              .output = Reg(1)},
        HaltInstruction{},
    };
    const BytecodeProgram program = TakeProgramValue(BytecodeProgram::Create(input));
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite, .rollback = StatementRollbackMode::kStatement}));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment{registry, collations}));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(2U, head.comparisons);
    EXPECT_EQ((std::array{std::pair{'A', 'a'}, std::pair{'A', 'a'}}), head.arguments);
    EXPECT_EQ(policy == Stat1IndexPolicy::Ordinary ? 2U : 0U, tail.comparisons);
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }
}

TEST(VmWriteTest, PreflightsMaintainsAndValidatesIndexEntries) {
  test::WritePagerFixedVfs vfs{false};
  PageNumber index_root{0};
  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(statement.writer()->InitializeDatabase());
    const std::array<IndexColumnOrder, 2> columns{
        IndexColumnOrder{BinaryCollation()},
        IndexColumnOrder{BinaryCollation()},
    };
    IndexBtreeWriter index = TakeValue(modern_sqlite::test::CreateIndexBtree(
        *(statement.writer()), columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));
    index_root = index.root_page();
    std::array<SqlValue, 2> seed{SqlValue::Text("key"), SqlValue::Integer(7)};
    RequireStatus(index.Insert(seed));
    RequireStatus(statement.Succeed());
  }

  enum class IndexAction : std::uint8_t {
    kCheck,
    kInsert,
    kInsertUnchecked,
    kDelete,
  };
  const auto program = [&](ProgramStatementKind statement_kind, SqlValue key, std::int64_t rowid,
                           IndexAction action,
                           std::optional<std::int64_t> ignored_rowid = std::nullopt) {
    ProgramInput input;
    input.schema_version = SchemaVersionRequirement{};
    input.statement_kind = statement_kind;
    input.transaction_access = ProgramTransactionAccess::kWrite;
    input.rollback_mode = ProgramRollbackMode::kStatement;
    input.mutation_result =
        statement_kind == ProgramStatementKind::kInsert
            ? MutationResultMetadata{
                  .publishes_changes = true,
                  .publishes_last_insert_rowid = true,
              }
            : MutationResultMetadata{
                  .publishes_changes = true,
                  .publishes_last_insert_rowid = false,
              };
    input.register_count = 2;
    input.constants.push_back(std::move(key));
    input.constants.push_back(SqlValue::Integer(rowid));
    input.symbols.emplace_back("BINARY");
    input.write_cursors.push_back(WriteCursorDescriptor{
        .root_page = RootPageNumber(index_root.value()),
        .columns = {},
        .rowid_alias = std::nullopt,
        .index_columns =
            {
                IndexColumnMetadata{
                    .collation = Symbol(0),
                    .order = BytecodeSortOrder::kAscending,
                },
                IndexColumnMetadata{
                    .collation = Symbol(0),
                    .order = BytecodeSortOrder::kAscending,
                },
            },
        .key_term_count = 1,
        .unique = true,
        .unique_not_null = false,
        .storage = WriteCursorStorageKind::kIndex,
    });
    input.instructions = {
        LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
        LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
        OpenWriteCursorInstruction{.cursor = WriteCursor(0)},
    };
    if (action != IndexAction::kDelete && action != IndexAction::kInsertUnchecked) {
      input.instructions.emplace_back(CheckUniqueIndexInstruction{
          .cursor = WriteCursor(0),
          .first_key = Reg(0),
          .key_count = 1,
          .ignored_rowid =
              ignored_rowid.has_value() ? std::optional<RegisterId>{Reg(1)} : std::nullopt,
      });
    }
    if (action == IndexAction::kInsert || action == IndexAction::kInsertUnchecked) {
      input.instructions.emplace_back(InsertIndexInstruction{
          .cursor = WriteCursor(0),
          .first_value = Reg(0),
          .value_count = 2,
      });
    } else if (action == IndexAction::kDelete) {
      input.instructions.emplace_back(DeleteIndexInstruction{
          .cursor = WriteCursor(0),
          .first_value = Reg(0),
          .value_count = 2,
      });
    }
    input.instructions.emplace_back(CloseWriteCursorInstruction{.cursor = WriteCursor(0)});
    input.instructions.emplace_back(HaltInstruction{});
    return TakeProgramValue(BytecodeProgram::Create(input));
  };

  const auto execute = [&](const BytecodeProgram& bytecode) {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kStatement,
        }));
    Vm vm = TakeValue(Vm::Create(bytecode, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    Result<VmStep> stepped = vm.Step();
    RequireStatus(vm.DetachExecutionContext());
    if (stepped.has_value()) {
      RequireStatus(statement.Succeed());
    } else {
      RequireStatus(statement.Rollback());
    }
    return stepped;
  };

  const BytecodeProgram duplicate =
      program(ProgramStatementKind::kInsert, SqlValue::Text("key"), 9, IndexAction::kInsert);
  const auto duplicate_result = execute(duplicate);
  ASSERT_FALSE(duplicate_result.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate_result.error().code());
  const BytecodeProgram unchecked_duplicate = program(
      ProgramStatementKind::kInsert, SqlValue::Text("key"), 9, IndexAction::kInsertUnchecked);
  const auto unchecked_result = execute(unchecked_duplicate);
  ASSERT_FALSE(unchecked_result.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, unchecked_result.error().code());

  const BytecodeProgram ignored =
      program(ProgramStatementKind::kUpdate, SqlValue::Text("key"), 7, IndexAction::kCheck, 7);
  EXPECT_EQ(VmStep::kDone, TakeValue(execute(ignored)));

  const BytecodeProgram first_null =
      program(ProgramStatementKind::kInsert, SqlValue{}, 8, IndexAction::kInsert);
  const BytecodeProgram second_null =
      program(ProgramStatementKind::kInsert, SqlValue{}, 9, IndexAction::kInsert);
  EXPECT_EQ(VmStep::kDone, TakeValue(execute(first_null)));
  EXPECT_EQ(VmStep::kDone, TakeValue(execute(second_null)));

  const BytecodeProgram deletion =
      program(ProgramStatementKind::kDelete, SqlValue::Text("key"), 7, IndexAction::kDelete);
  EXPECT_EQ(VmStep::kDone, TakeValue(execute(deletion)));
  const auto missing_delete = execute(deletion);
  ASSERT_FALSE(missing_delete.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, missing_delete.error().code());

  TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
  TransactionStatement statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  IndexBtreeWriter index = TakeValue(modern_sqlite::test::OpenIndexBtree(
      *(statement.writer()), index_root, columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                   .all_field_count = (columns).size()}));
  std::array<SqlValue, 1> key{SqlValue::Text("key")};
  EXPECT_FALSE(TakeValue(index.FindPrefixRowId(key)).has_value());
  std::array<SqlValue, 1> null_key{};
  EXPECT_EQ(std::optional<std::int64_t>{8}, TakeValue(index.FindPrefixRowId(null_key)));
  RequireStatus(statement.Succeed());
}

TEST(VmWriteTest, RefillsStablePhysicalKeysWithReusableRecordsInMemoryAndFileModes) {
  const std::string large_key(1400, 'a');
  const std::array records{
      std::array{SqlValue::Text("b"), SqlValue::Integer(-7)},
      std::array{SqlValue::Text(large_key),
                 SqlValue::Integer(std::numeric_limits<std::int64_t>::min())},
      std::array{SqlValue::Text("b"), SqlValue::Integer(std::numeric_limits<std::int64_t>::max())},
      std::array{SqlValue::Text("c"), SqlValue::Integer(0)},
  };
  const BytecodeProgram program = IndexRefillProgram(records);
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<int>(mode));
    test::WritePagerFixedVfs vfs{false};
    static_cast<void>(InitializedWriteDatabase(vfs));
    const TemporaryStorageOptions options{
        .mode = mode,
        .sorter_memory_threshold =
            ByteCount{mode == TemporaryStoreMode::kMemory ? 1U << 20U : 128U},
    };
    EXPECT_EQ(VmStep::kDone, TakeValue(ExecuteIndexRefill(program, vfs, options)));
    EXPECT_FALSE(vfs.pathless_file_present());
    if (mode == TemporaryStoreMode::kFile) {
      EXPECT_GT(vfs.pathless_open_count(), 0U);
    }
    std::unique_ptr<BtreeDatabase> pager_database =
        test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    {
      const std::array columns{IndexColumnOrder{BinaryCollation()},
                               IndexColumnOrder{BinaryCollation()}};
      IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
          *pager_database, PageNumber{2}, columns,
          modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                       .all_field_count = (columns).size()}));
      bool positioned = TakeValue(cursor.First());
      for (const std::size_t ordinal : {1U, 0U, 2U, 3U}) {
        ASSERT_TRUE(positioned);
        const ByteBuffer payload = TakeValue(cursor.CopyPayload());
        const auto fields = TakeValue(DecodeRecord(payload.view()));
        ASSERT_EQ(2U, fields.size());
        EXPECT_EQ(records[ordinal][0].text_value(), fields[0].text_value());
        EXPECT_EQ(records[ordinal][1].integer_value(), fields[1].integer_value());
        positioned = TakeValue(cursor.Next());
      }
      EXPECT_FALSE(positioned);
    }
    RequireStatus(pager->EndRead());
  }
}

TEST(VmWriteTest, RejectsInvalidPhysicalRefillOrderIncludingNullableUniqueKeys) {
  const auto rejects = [](SqlValue key, SqlValue first_rowid, SqlValue second_rowid, bool unique,
                          ErrorCode expected) {
    test::WritePagerFixedVfs vfs{false};
    const ByteBuffer original = InitializedWriteDatabase(vfs);
    const std::array records{
        std::array{key.Clone(), std::move(first_rowid)},
        std::array{std::move(key), std::move(second_rowid)},
    };
    const BytecodeProgram program = IndexRefillProgram(records, unique);
    const auto rejected = ExecuteIndexRefill(program, vfs);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(expected, rejected.error().code());
    EXPECT_TRUE(std::ranges::equal(original.view(), vfs.database_bytes()));
    EXPECT_FALSE(vfs.pathless_file_present());
  };
  rejects(SqlValue::Text("same"), SqlValue::Integer(9), SqlValue::Integer(3), false,
          ErrorCode::kMisuse);
  rejects(SqlValue::Text("same"), SqlValue::Integer(3), SqlValue::Integer(3), false,
          ErrorCode::kMisuse);
  rejects(SqlValue{}, SqlValue::Integer(9), SqlValue::Integer(3), true, ErrorCode::kMisuse);
  rejects(SqlValue::Text("same"), SqlValue::Integer(9), SqlValue::Integer(3), true,
          ErrorCode::kConstraint);
  rejects(SqlValue::Text("same"), SqlValue::Integer(1), SqlValue::Real(2.0), true,
          ErrorCode::kConstraint);
}

TEST(VmWriteTest, EnforcesIntegerSuffixAndEncodedRefillValueLimit) {
  const auto rejects = [](const std::array<SqlValue, 2>& record, VmLimits limits,
                          ErrorCode expected) {
    test::WritePagerFixedVfs vfs{false};
    const ByteBuffer original = InitializedWriteDatabase(vfs);
    const BytecodeProgram program =
        IndexRefillProgram(std::span<const std::array<SqlValue, 2>>{&record, 1});
    const auto rejected = ExecuteIndexRefill(
        program, vfs, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}, limits);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(expected, rejected.error().code());
    EXPECT_TRUE(std::ranges::equal(original.view(), vfs.database_bytes()));
  };
  rejects({SqlValue::Text("key"), SqlValue::Real(1.0)}, {}, ErrorCode::kMisuse);
  rejects({SqlValue::Text(std::string(32, 'x')),
           SqlValue::Integer(std::numeric_limits<std::int64_t>::max())},
          VmLimits{.maximum_value_bytes = 33}, ErrorCode::kTooLarge);
}

TEST(VmWriteTest, RollsBackRefillFactoryAndSpillFailuresWithoutRetainedPins) {
  const std::array records{
      std::array{SqlValue::Text(std::string(1400, 'a')), SqlValue::Integer(1)},
      std::array{SqlValue::Text("b"), SqlValue::Integer(2)},
  };
  const BytecodeProgram program = IndexRefillProgram(records);
  for (const bool fail_read : {false, true}) {
    test::WritePagerFixedVfs vfs{false};
    const ByteBuffer original = InitializedWriteDatabase(vfs);
    if (fail_read) {
      vfs.FailPathlessReadAfter(0, ErrorCode::kIo);
    } else {
      vfs.FailPathlessWriteAfter(0, ErrorCode::kIo);
    }
    const auto rejected =
        ExecuteIndexRefill(program, vfs,
                           TemporaryStorageOptions{.mode = TemporaryStoreMode::kFile,
                                                   .sorter_memory_threshold = ByteCount{128}});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kIo, rejected.error().code());
    EXPECT_GT(vfs.pathless_open_count(), 0U);
    EXPECT_FALSE(vfs.pathless_file_present());
    EXPECT_TRUE(std::ranges::equal(original.view(), vfs.database_bytes()));
  }
  test::WritePagerFixedVfs vfs{false};
  const ByteBuffer original = InitializedWriteDatabase(vfs);
  const auto missing_factory = ExecuteIndexRefill(program, vfs, std::nullopt);
  ASSERT_FALSE(missing_factory.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing_factory.error().code());
  EXPECT_TRUE(std::ranges::equal(original.view(), vfs.database_bytes()));
}

TEST(VmWriteTest, IgnoresLegacyIndexDescendingWithoutChangingQueryOrdering) {
  constexpr std::size_t kSchemaFormatOffset = 44;
  test::WritePagerFixedVfs vfs{false};
  ByteBuffer original = InitializedWriteDatabase(vfs);
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{
          original.mutable_view().data() + kSchemaFormatOffset, sizeof(std::uint32_t)},
      static_cast<std::uint32_t>(DatabaseSchemaFormat::kThree));
  vfs.LoadDatabase(original.view());
  const std::array records{
      std::array{SqlValue::Integer(3), SqlValue::Integer(1)},
      std::array{SqlValue{}, SqlValue::Integer(2)},
      std::array{SqlValue::Integer(1), SqlValue::Integer(3)},
  };
  const BytecodeProgram refill = IndexRefillProgram(records, false, BytecodeSortOrder::kDescending);
  EXPECT_EQ(VmStep::kDone, TakeValue(ExecuteIndexRefill(refill, vfs)));
  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    const std::array columns{IndexColumnOrder{BinaryCollation()},
                             IndexColumnOrder{BinaryCollation()}};
    IndexBtreeCursor cursor = TakeValue(modern_sqlite::test::OpenIndexCursor(
        *pager_database, PageNumber{2}, columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    ByteBuffer payload = TakeValue(cursor.CopyPayload());
    auto fields = TakeValue(DecodeRecord(
        payload.view(), RecordCodecOptions{.schema_format = DatabaseSchemaFormat::kThree}));
    EXPECT_EQ(SqlValueType::kNull, fields[0].type());
    ASSERT_TRUE(TakeValue(cursor.Next()));
    payload = TakeValue(cursor.CopyPayload());
    fields = TakeValue(DecodeRecord(
        payload.view(), RecordCodecOptions{.schema_format = DatabaseSchemaFormat::kThree}));
    ExpectInteger(fields[0], 1);
    ASSERT_TRUE(TakeValue(cursor.Next()));
    payload = TakeValue(cursor.CopyPayload());
    fields = TakeValue(DecodeRecord(
        payload.view(), RecordCodecOptions{.schema_format = DatabaseSchemaFormat::kThree}));
    ExpectInteger(fields[0], 3);
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }
  const auto query = BuildSorterProgram(*pager, BytecodeSortOrder::kDescending);
  const auto temporary = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  Vm vm = TakeValue(Vm::Create(query, VmEnvironment::Core()));
  RequireStatus(vm.AttachExecutionContext(
      VmExecutionContext{*pager_database, kCatalogGeneration, temporary}));
  for (std::int64_t expected = 3; expected > 0; --expected) {
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ExpectInteger(vm.row()[0], expected);
  }
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(pager->EndRead());
}

TEST(VmWriteTest, GeneratesRowidsAcrossNegativeAndRandomBoundaries) {
  {
    test::WritePagerFixedVfs vfs{false};
    static_cast<void>(InitializedWriteDatabase(vfs));
    InsertDirectWriteRow(vfs, -1, "negative");
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    const BytecodeProgram program = TableInsertProgram(SqlValue{}, SqlValue::Text("zero"));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(std::optional<std::int64_t>{0}, vm.last_insert_rowid_event());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }

  {
    test::WritePagerFixedVfs vfs{false};
    static_cast<void>(InitializedWriteDatabase(vfs));
    InsertDirectWriteRow(vfs, std::numeric_limits<std::int64_t>::max(), "maximum");
    vfs.SetRandomByte(std::byte{0});
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    const BytecodeProgram program = TableInsertProgram(SqlValue{}, SqlValue::Text("random"));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    const std::size_t random_calls_before = vfs.random_call_count();
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(std::optional<std::int64_t>{1}, vm.last_insert_rowid_event());
    EXPECT_GE(vfs.random_call_count() - random_calls_before, 1U);
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }

  {
    test::WritePagerFixedVfs vfs{false};
    static_cast<void>(InitializedWriteDatabase(vfs));
    InsertDirectWriteRow(vfs, 1, "collision");
    InsertDirectWriteRow(vfs, std::numeric_limits<std::int64_t>::max(), "maximum");
    vfs.SetRandomByte(std::byte{0});
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    const BytecodeProgram program = TableInsertProgram(SqlValue{}, SqlValue::Text("full"));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    const std::size_t random_calls_before = vfs.random_call_count();
    const auto inserted = vm.Step();
    ASSERT_FALSE(inserted.has_value());
    EXPECT_EQ(ErrorCode::kFull, inserted.error().code());
    EXPECT_EQ(100U, vfs.random_call_count() - random_calls_before);
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Rollback());
  }
}

TEST(VmWriteTest, DeletesRowsAndSeeksStrictlyGreaterRowids) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(InitializedWriteDatabase(vfs));
  InsertDirectWriteRow(vfs, 1, "first");
  InsertDirectWriteRow(vfs, 3, "third");

  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    const BytecodeProgram program = TableDeleteProgram(SqlValue::Integer(1));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(1U, vm.change_count());
    EXPECT_FALSE(vm.last_insert_rowid_event().has_value());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }

  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    const BytecodeProgram program = TableDeleteProgram(SqlValue::Integer(2));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(0U, vm.change_count());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }

  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    const BytecodeProgram program = TableDeleteProgram(SqlValue::Text("not-rowid"));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    const auto deleted = vm.Step();
    ASSERT_FALSE(deleted.has_value());
    EXPECT_EQ(ErrorCode::kTypeMismatch, deleted.error().code());
    EXPECT_EQ(0U, vm.change_count());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Rollback());
  }

  {
    std::unique_ptr<BtreeDatabase> pager_database =
        test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    const BytecodeProgram program = TableSeekProgram(SqlValue::Integer(1), RowIdSeekMode::kGreater);
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_database, 0}));
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ASSERT_EQ(1U, vm.row().size());
    EXPECT_EQ(3, vm.row()[0].integer_value());
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(pager->EndRead());
  }

  const auto rows = ReadWriteTableRows(vfs);
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(3, rows[0].first);
  ExpectText(rows[0].second[1], "third");
}

TEST(VmWriteTest, SeeksRowIdRangesUsingSqliteNumericBoundaryRules) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(InitializedWriteDatabase(vfs));
  InsertDirectWriteRow(vfs, -3, "negative");
  InsertDirectWriteRow(vfs, 1, "first");
  InsertDirectWriteRow(vfs, 3, "third");

  const auto expect = [&](SqlValue key, RowIdSeekMode mode, std::optional<std::int64_t> expected) {
    EXPECT_EQ(expected, ExecuteTableSeek(vfs, std::move(key), mode));
  };
  expect(SqlValue::Integer(1), RowIdSeekMode::kGreaterOrEqual, 1);
  expect(SqlValue::Integer(1), RowIdSeekMode::kGreater, 3);
  expect(SqlValue::Integer(1), RowIdSeekMode::kLessOrEqual, 1);
  expect(SqlValue::Integer(1), RowIdSeekMode::kLess, -3);
  expect(SqlValue::Real(1.5), RowIdSeekMode::kGreater, 3);
  expect(SqlValue::Real(1.5), RowIdSeekMode::kLessOrEqual, 1);
  expect(SqlValue::Real(-1.5), RowIdSeekMode::kGreaterOrEqual, 1);
  expect(SqlValue::Real(-1.5), RowIdSeekMode::kLess, -3);
  expect(SqlValue::Text("1.5"), RowIdSeekMode::kGreaterOrEqual, 3);
  expect(SqlValue::Text("not-numeric"), RowIdSeekMode::kGreaterOrEqual, std::nullopt);
  expect(SqlValue::Text("not-numeric"), RowIdSeekMode::kLessOrEqual, 3);
  expect(SqlValue::Blob(ByteBuffer{ByteCount{1}}), RowIdSeekMode::kGreater, std::nullopt);
  expect(SqlValue::Blob(ByteBuffer{ByteCount{1}}), RowIdSeekMode::kLess, 3);
  expect(SqlValue{}, RowIdSeekMode::kLessOrEqual, std::nullopt);
}

TEST(VmWriteTest, SeeksRowIdRangeSignedEndpointsAndInfinities) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(InitializedWriteDatabase(vfs));
  InsertDirectWriteRow(vfs, std::numeric_limits<std::int64_t>::min(), "minimum");
  InsertDirectWriteRow(vfs, std::numeric_limits<std::int64_t>::max(), "maximum");

  const double infinity = std::numeric_limits<double>::infinity();
  EXPECT_EQ(std::numeric_limits<std::int64_t>::min(),
            ExecuteTableSeek(vfs, SqlValue::Real(-infinity), RowIdSeekMode::kGreaterOrEqual));
  EXPECT_EQ(std::numeric_limits<std::int64_t>::max(),
            ExecuteTableSeek(vfs, SqlValue::Real(infinity), RowIdSeekMode::kLessOrEqual));
  EXPECT_EQ(std::numeric_limits<std::int64_t>::max(),
            ExecuteTableSeek(vfs, SqlValue::Text("not-numeric"), RowIdSeekMode::kLess));
  EXPECT_EQ(std::numeric_limits<std::int64_t>::min(),
            ExecuteTableSeek(
                vfs, SqlValue::Real(static_cast<double>(std::numeric_limits<std::int64_t>::min())),
                RowIdSeekMode::kGreaterOrEqual));
  EXPECT_EQ(std::numeric_limits<std::int64_t>::max(),
            ExecuteTableSeek(
                vfs, SqlValue::Real(static_cast<double>(std::numeric_limits<std::int64_t>::max())),
                RowIdSeekMode::kLessOrEqual));

  const auto expect_two_to_63 = [&](SqlValue key) {
    EXPECT_EQ(std::nullopt, ExecuteTableSeek(vfs, key.Clone(), RowIdSeekMode::kGreaterOrEqual));
    EXPECT_EQ(std::nullopt, ExecuteTableSeek(vfs, key.Clone(), RowIdSeekMode::kGreater));
    EXPECT_EQ(std::numeric_limits<std::int64_t>::max(),
              ExecuteTableSeek(vfs, key.Clone(), RowIdSeekMode::kLessOrEqual));
    EXPECT_EQ(std::numeric_limits<std::int64_t>::max(),
              ExecuteTableSeek(vfs, std::move(key), RowIdSeekMode::kLess));
  };
  expect_two_to_63(SqlValue::Real(9223372036854775808.0));
  expect_two_to_63(SqlValue::Text("9223372036854775808.0"));
}

TEST(VmWriteTest, DeletesAllRowsThroughOnePassMutationCursor) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(InitializedWriteDatabase(vfs));
  InsertDirectWriteRow(vfs, -3, "negative");
  InsertDirectWriteRow(vfs, 1, "first");
  InsertDirectWriteRow(vfs, 3, "third");

  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
        }));
    const BytecodeProgram program = TableDeleteScanProgram();
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(3U, vm.change_count());
    EXPECT_FALSE(vm.last_insert_rowid_event().has_value());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }

  EXPECT_TRUE(ReadWriteTableRows(vfs).empty());

  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
        }));
    const BytecodeProgram program = TableDeleteScanProgram();
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(0U, vm.change_count());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }
}

TEST(VmWriteTest, UpdatesAllRowsThroughOnePassMutationCursor) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(InitializedWriteDatabase(vfs));
  InsertDirectWriteRow(vfs, -3, "negative");
  InsertDirectWriteRow(vfs, 1, "first");
  InsertDirectWriteRow(vfs, 3, "third");

  std::vector<SqlValue> fields;
  fields.emplace_back();
  fields.push_back(SqlValue::Text("updated"));
  const ByteBuffer record = TakeValue(EncodeRecord(fields));
  const BytecodeProgram program =
      TableUpdateScanProgram(SqlValue::Blob(ByteBuffer::CopyOf(record.view())));

  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kStatement,
        }));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(3U, vm.change_count());
    EXPECT_FALSE(vm.last_insert_rowid_event().has_value());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }

  const auto rows = ReadWriteTableRows(vfs);
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(-3, rows[0].first);
  EXPECT_EQ(1, rows[1].first);
  EXPECT_EQ(3, rows[2].first);
  for (const auto& [rowid, values] : rows) {
    static_cast<void>(rowid);
    ASSERT_EQ(2U, values.size());
    ExpectText(values[1], "updated");
  }

  test::WritePagerFixedVfs empty_vfs{false};
  static_cast<void>(InitializedWriteDatabase(empty_vfs));
  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(empty_vfs);
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kStatement,
        }));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(0U, vm.change_count());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }

  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kStatement,
        }));
    const BytecodeProgram malformed = TableUpdateScanProgram(SqlValue::Text("not-a-record"));
    Vm vm = TakeValue(Vm::Create(malformed, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    const auto updated = vm.Step();
    ASSERT_FALSE(updated.has_value());
    EXPECT_EQ(ErrorCode::kTypeMismatch, updated.error().code());
    EXPECT_EQ(0U, vm.change_count());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Rollback());
  }
}

TEST(VmWriteTest, ReplacesAndMovesUpdatedRowsAtomically) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(InitializedWriteDatabase(vfs));
  InsertDirectWriteRow(vfs, 1, "first");
  InsertDirectWriteRow(vfs, 3, "third");

  const auto execute = [&](const BytecodeProgram& program) {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kStatement,
        }));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    Result<VmStep> stepped = vm.Step();
    if (!stepped.has_value()) {
      const Error error = std::move(stepped.error());
      RequireStatus(vm.DetachExecutionContext());
      RequireStatus(statement.Rollback());
      return Result<std::uint64_t>{std::unexpected(error)};
    }
    const std::uint64_t changes = vm.change_count();
    EXPECT_FALSE(vm.last_insert_rowid_event().has_value());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
    return Result<std::uint64_t>{changes};
  };

  EXPECT_EQ(1U, TakeValue(execute(TableUpdateProgram(SqlValue::Integer(1), SqlValue::Integer(1),
                                                     SqlValue::Text("same")))));
  EXPECT_EQ(1U, TakeValue(execute(TableUpdateProgram(SqlValue::Integer(1), SqlValue::Text("2"),
                                                     SqlValue::Integer(42)))));

  const auto duplicate = execute(
      TableUpdateProgram(SqlValue::Integer(2), SqlValue::Integer(3), SqlValue::Text("duplicate")));
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate.error().code());

  EXPECT_EQ(0U, TakeValue(execute(TableUpdateProgram(SqlValue::Integer(99), SqlValue::Integer(100),
                                                     SqlValue::Text("missing")))));

  const auto null_rowid =
      execute(TableUpdateProgram(SqlValue::Integer(2), SqlValue{}, SqlValue::Text("invalid")));
  ASSERT_FALSE(null_rowid.has_value());
  EXPECT_EQ(ErrorCode::kTypeMismatch, null_rowid.error().code());

  const auto rows = ReadWriteTableRows(vfs);
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ(2, rows[0].first);
  ExpectText(rows[0].second[1], "42");
  EXPECT_EQ(3, rows[1].first);
  ExpectText(rows[1].second[1], "third");
}

TEST(VmWriteTest, CreatesTableRootsAndIncrementsSchemaCookieAtomically) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kStatement,
        }));
    const BytecodeProgram program = CreateTablePrimitiveProgram(false);
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(0U, vm.change_count());
    EXPECT_FALSE(vm.last_insert_rowid_event().has_value());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }

  {
    std::unique_ptr<BtreeDatabase> pager_database =
        test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    ASSERT_NE(nullptr, pager->header());
    EXPECT_EQ(1U, pager->header()->schema_cookie());
    EXPECT_EQ(2U, pager->page_count());
    {
      TableBtreeCursor table = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{2}));
      EXPECT_FALSE(TakeValue(table.First()));
    }
    RequireStatus(pager->EndRead());
  }

  test::WritePagerFixedVfs rollback_vfs{false};
  {
    TransactionCoordinator coordinator = OpenWriteCoordinator(rollback_vfs);
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kStatement,
        }));
    const BytecodeProgram program = CreateTablePrimitiveProgram(true);
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    const auto failed = vm.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kTypeMismatch, failed.error().code());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Rollback());
  }
  EXPECT_TRUE(rollback_vfs.database_bytes().empty());
}

TEST_F(VmTest, InvalidatesSuspendedRowsWhenTheReadSnapshotEnds) {
  const BytecodeProgram program = BuildProgram(
      *pager_, 1, 0,
      [] {
        std::vector<SqlValue> values;
        values.push_back(SqlValue::Integer(7));
        return values;
      }(),
      {}, {}, {ResultColumn()},
      {
          LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
          ResultRowInstruction{.first = Reg(0), .count = 1},
          HaltInstruction{},
      },
      std::nullopt, true);
  Vm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ExpectInteger(OnlyRowValue(vm), 7);
  RequireStatus(pager_->EndRead());

  const auto resumed = vm.Step();
  ASSERT_FALSE(resumed.has_value());
  EXPECT_EQ(resumed.error().code(), ErrorCode::kMisuse);
  EXPECT_EQ(vm.state(), VmState::kError);
  EXPECT_TRUE(vm.row().empty());

  RequireStatus(vm.Reset());
  RequireStatus(pager_->BeginRead());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ExpectInteger(OnlyRowValue(vm), 7);
}

[[nodiscard]] SqlValue EvaluateUnary(BtreeDatabase& database, UnaryOperation operation,
                                     SqlValue input, RegisterId output = Reg(1)) {
  Pager& pager = database.pager();
  std::vector<SqlValue> constants;
  constants.push_back(std::move(input));
  const BytecodeProgram program =
      BuildProgram(pager, 2, 0, std::move(constants), {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       UnaryInstruction{.operation = operation, .input = Reg(0), .output = output},
                       ResultRowInstruction{.first = output, .count = 1},
                       HaltInstruction{},
                   });
  Vm vm = CreateAttachedVm(program, database, kCatalogGeneration);
  if (TakeValue(vm.Step()) != VmStep::kRow) {
    throw std::runtime_error("unary VM program did not yield a row");
  }
  return OnlyRowValue(vm).Clone();
}

[[nodiscard]] SqlValue EvaluateBinary(BtreeDatabase& database, BinaryOperation operation,
                                      SqlValue left, SqlValue right, RegisterId output = Reg(2)) {
  Pager& pager = database.pager();
  std::vector<SqlValue> constants;
  constants.push_back(std::move(left));
  constants.push_back(std::move(right));
  const BytecodeProgram program =
      BuildProgram(pager, 3, 0, std::move(constants), {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
                       BinaryInstruction{
                           .operation = operation,
                           .left = Reg(0),
                           .right = Reg(1),
                           .output = output,
                       },
                       ResultRowInstruction{.first = output, .count = 1},
                       HaltInstruction{},
                   });
  Vm vm = CreateAttachedVm(program, database, kCatalogGeneration);
  if (TakeValue(vm.Step()) != VmStep::kRow) {
    throw std::runtime_error("binary VM program did not yield a row");
  }
  return OnlyRowValue(vm).Clone();
}

[[nodiscard]] BytecodeProgram ValueCoroutineProgram(const Pager& pager, bool reinitialize) {
  ProgramInput input;
  input.schema_version = CurrentSchema(pager);
  input.register_count = 1;
  input.constants.push_back(SqlValue::Integer(42));
  input.constants.push_back(SqlValue::Integer(43));
  input.result_columns.push_back(ResultColumn());
  if (reinitialize) {
    input.coroutines.push_back(CoroutineDescriptor{
        .entry = Address(9), .end = Address(14), .first_output = Reg(0), .output_count = 1});
    input.instructions = {
        InitCoroutineInstruction{.coroutine = CoroutineId{0}},
        ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(8)},
        ResultRowInstruction{.first = Reg(0), .count = 1},
        CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
        InitCoroutineInstruction{.coroutine = CoroutineId{0}},
        ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(8)},
        ResultRowInstruction{.first = Reg(0), .count = 1},
        CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
        HaltInstruction{},
    };
  } else {
    input.coroutines.push_back(CoroutineDescriptor{
        .entry = Address(5), .end = Address(10), .first_output = Reg(0), .output_count = 1});
    input.instructions = {
        InitCoroutineInstruction{.coroutine = CoroutineId{0}},
        ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(4)},
        ResultRowInstruction{.first = Reg(0), .count = 1},
        JumpInstruction{.target = Address(1)},
        HaltInstruction{},
    };
  }
  input.instructions.emplace_back(
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)});
  input.instructions.emplace_back(YieldCoroutineInstruction{.coroutine = CoroutineId{0}});
  input.instructions.emplace_back(
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(0)});
  input.instructions.emplace_back(YieldCoroutineInstruction{.coroutine = CoroutineId{0}});
  input.instructions.emplace_back(EndCoroutineInstruction{.coroutine = CoroutineId{0}});
  return TakeProgramValue(BytecodeProgram::Create(input));
}

TEST_F(VmTest, ResumesValueCoroutinesAcrossClientRowsAndReset) {
  const auto program = ValueCoroutineProgram(*pager_, false);
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 42);
  RequireStatus(vm.Reset());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 42);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 43);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, CancelsSuspendedValueCoroutinesBeforeReinitialization) {
  const auto program = ValueCoroutineProgram(*pager_, true);
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 42);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 42);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, KeepsIndependentValueCoroutineContinuationSlots) {
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager_);
  input.register_count = 2;
  for (const std::int64_t value : {10, 11, 20, 21}) {
    input.constants.push_back(SqlValue::Integer(value));
  }
  input.result_columns.push_back(ResultColumn());
  input.coroutines = {
      CoroutineDescriptor{Address(13), Address(18), Reg(0), 1},
      CoroutineDescriptor{Address(18), Address(23), Reg(1), 1},
  };
  input.instructions = {
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      InitCoroutineInstruction{.coroutine = CoroutineId{1}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(10)},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{1}, .exhausted_target = Address(10)},
      ResultRowInstruction{.first = Reg(1), .count = 1},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(10)},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{1}, .exhausted_target = Address(10)},
      ResultRowInstruction{.first = Reg(1), .count = 1},
      CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
      CancelCoroutineInstruction{.coroutine = CoroutineId{1}},
      HaltInstruction{},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{0}},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(0)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{0}},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(1)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{1}},
      LoadConstantInstruction{.constant = Constant(3), .output = Reg(1)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{1}},
      EndCoroutineInstruction{.coroutine = CoroutineId{1}},
  };
  const auto program = TakeProgramValue(BytecodeProgram::Create(input));
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration);
  for (const std::int64_t expected : {10, 20, 11, 21}) {
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ExpectInteger(OnlyRowValue(vm), expected);
  }
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, CancelsAndReinitializesReadCoroutineWithItsOwnedCursor) {
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager_);
  input.register_count = 1;
  input.cursors.push_back(TableDescriptor({}));
  input.result_columns.push_back(ResultColumn());
  input.coroutines.push_back(CoroutineDescriptor{
      .entry = Address(9),
      .end = Address(18),
      .first_output = Reg(0),
      .output_count = 1,
      .kind = CoroutineKind::Read,
  });
  input.instructions = {
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(8)},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(8)},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
      HaltInstruction{},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      RewindInstruction{.cursor = Cursor(0), .empty_target = Address(16)},
      ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(0)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{0}},
      NextInstruction{.cursor = Cursor(0), .next_target = Address(11)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
      CloseCursorInstruction{.cursor = Cursor(0)},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
  };
  const auto program = TakeProgramValue(BytecodeProgram::Create(input));
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  const auto first = OnlyRowValue(vm).integer_value();
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), *first);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, CoroutineCancellationPreservesCallerLogicalNullCursor) {
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager_);
  input.register_count = 3;
  input.cursors = {TableDescriptor({}), TableDescriptor({})};
  input.result_columns = {ResultColumn(), ResultColumn()};
  input.coroutines.push_back(CoroutineDescriptor{
      .entry = Address(10),
      .end = Address(19),
      .first_output = Reg(0),
      .output_count = 1,
      .kind = CoroutineKind::Read,
  });
  input.instructions = {
      OpenReadCursorInstruction{.cursor = Cursor(1)},
      NullRowInstruction{.cursor = Cursor(1)},
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(8)},
      CopyInstruction{.input = Reg(0), .output = Reg(2)},
      ReadRowIdInstruction{.cursor = Cursor(1), .output = Reg(1)},
      CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResultRowInstruction{.first = Reg(1), .count = 2},
      CloseCursorInstruction{.cursor = Cursor(1)},
      HaltInstruction{},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      RewindInstruction{.cursor = Cursor(0), .empty_target = Address(17)},
      ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(0)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{0}},
      NextInstruction{.cursor = Cursor(0), .next_target = Address(12)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
      CloseCursorInstruction{.cursor = Cursor(0)},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
  };
  const auto program = TakeProgramValue(BytecodeProgram::Create(input));
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_EQ(2U, vm.row().size());
  EXPECT_EQ(SqlValueType::kNull, vm.row()[0].type());
  EXPECT_TRUE(vm.row()[1].integer_value().has_value());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, CancelsNestedReadCoroutineSubtreeBeforeReinitialization) {
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager_);
  input.register_count = 2;
  input.cursors = {TableDescriptor({}), TableDescriptor({})};
  input.result_columns.push_back(ResultColumn());
  input.coroutines = {
      CoroutineDescriptor{Address(9), Address(19), Reg(0), 1, CoroutineKind::Read},
      CoroutineDescriptor{Address(19), Address(27), Reg(1), 1, CoroutineKind::Read},
  };
  input.instructions = {
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(8)},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(8)},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
      HaltInstruction{},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      InitCoroutineInstruction{.coroutine = CoroutineId{1}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{1}, .exhausted_target = Address(17)},
      CopyInstruction{.input = Reg(1), .output = Reg(0)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{0}},
      CancelCoroutineInstruction{.coroutine = CoroutineId{1}},
      CloseCursorInstruction{.cursor = Cursor(0)},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
      CloseCursorInstruction{.cursor = Cursor(0)},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
      OpenReadCursorInstruction{.cursor = Cursor(1)},
      RewindInstruction{.cursor = Cursor(1), .empty_target = Address(25)},
      ReadRowIdInstruction{.cursor = Cursor(1), .output = Reg(1)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{1}},
      CloseCursorInstruction{.cursor = Cursor(1)},
      EndCoroutineInstruction{.coroutine = CoroutineId{1}},
      CloseCursorInstruction{.cursor = Cursor(1)},
      EndCoroutineInstruction{.coroutine = CoroutineId{1}},
  };
  const auto program = TakeProgramValue(BytecodeProgram::Create(input));
  EXPECT_EQ(CoroutineId{0}, program.coroutine_parent(CoroutineId{1}));
  EXPECT_EQ(CoroutineId{1}, program.coroutine_owner(Cursor(1)));
  EXPECT_TRUE(program.coroutine_contains(CoroutineId{0}, CoroutineId{1}));
  EXPECT_FALSE(program.coroutine_contains(CoroutineId{1}, CoroutineId{0}));
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  const auto first = OnlyRowValue(vm).integer_value();
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), *first);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, CancelsCoroutineTemporaryAndAggregateOwnersThroughCanonicalCleanup) {
  const auto factory = TakeValue(TemporaryStorageFactory::Create(
      vfs_, *pager_,
      TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory,
                              .sorter_memory_threshold = ByteCount{1U << 20U}}));
  for (const std::uint32_t family : {0U, 1U, 2U, 3U}) {
    SCOPED_TRACE(family);
    ProgramInput input;
    input.schema_version = CurrentSchema(*pager_);
    input.register_count = 2;
    input.symbols = {"BINARY", "sum"};
    input.constants.push_back(SqlValue::Integer(42));
    input.constants.push_back(SqlValue::Integer(1));
    input.result_columns.push_back(ResultColumn());
    input.instructions = {
        InitCoroutineInstruction{.coroutine = CoroutineId{0}},
        ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(8)},
        ResultRowInstruction{.first = Reg(0), .count = 1},
        CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
        InitCoroutineInstruction{.coroutine = CoroutineId{0}},
        ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(8)},
        ResultRowInstruction{.first = Reg(0), .count = 1},
        CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
        HaltInstruction{},
        LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
        LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
    };
    if (family == 0U) {
      input.sorters.push_back(SorterDescriptor());
      input.instructions.emplace_back(OpenSorterInstruction{.sorter = SorterId{0}});
    } else if (family == 1U) {
      input.top_ns.push_back(SorterDescriptor());
      input.instructions.emplace_back(OpenTopNInstruction{.top_n = TopNId{0}, .bound = Reg(1)});
    } else if (family == 2U) {
      auto relation = SorterDescriptor();
      relation.field_count = 1;
      input.relations.push_back(std::move(relation));
      input.instructions.emplace_back(OpenRelationInstruction{.relation = Relation(0)});
    } else {
      input.aggregates.push_back(AggregateDescriptorFor(Symbol(1), Symbol(0), 1));
      input.instructions.emplace_back(OpenAggregateInstruction{.aggregate = AggregateId{0}});
      input.instructions.emplace_back(
          StepAggregateInstruction{.aggregate = AggregateId{0}, .first_argument = Reg(0)});
    }
    input.instructions.emplace_back(YieldCoroutineInstruction{.coroutine = CoroutineId{0}});
    if (family == 0U) {
      input.instructions.emplace_back(CloseSorterInstruction{.sorter = SorterId{0}});
    } else if (family == 1U) {
      input.instructions.emplace_back(CloseTopNInstruction{.top_n = TopNId{0}});
    } else if (family == 2U) {
      input.instructions.emplace_back(CloseRelationInstruction{.relation = Relation(0)});
    } else {
      input.instructions.emplace_back(
          FinalizeAggregateInstruction{.aggregate = AggregateId{0}, .output = Reg(1)});
      input.instructions.emplace_back(CloseAggregateInstruction{.aggregate = AggregateId{0}});
    }
    input.instructions.emplace_back(EndCoroutineInstruction{.coroutine = CoroutineId{0}});
    input.coroutines.push_back(CoroutineDescriptor{
        .entry = Address(9),
        .end = Address(static_cast<std::uint32_t>(input.instructions.size())),
        .first_output = Reg(0),
        .output_count = 1,
        .kind = CoroutineKind::Read,
    });
    const auto program = TakeProgramValue(BytecodeProgram::Create(input));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(
        vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ExpectInteger(OnlyRowValue(vm), 42);
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ExpectInteger(OnlyRowValue(vm), 42);
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  }
}

TEST_F(VmTest, CoroutineInstructionFailureClosesOwnedCursorAndPermitsReset) {
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager_);
  input.register_count = 2;
  input.cursors.push_back(TableDescriptor({}));
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(42));
  input.result_columns.push_back(ResultColumn());
  input.coroutines.push_back(CoroutineDescriptor{
      .entry = Address(5),
      .end = Address(12),
      .first_output = Reg(0),
      .output_count = 1,
      .kind = CoroutineKind::Read,
  });
  input.instructions = {
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0}, .exhausted_target = Address(4)},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      JumpInstruction{.target = Address(1)},
      HaltInstruction{},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(0)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{0}},
      JumpIfInstruction{.condition = JumpCondition::kIfTrue, .input = Reg(1), .target = Address(9)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
  };
  const auto program = TakeProgramValue(BytecodeProgram::Create(input));
  VmLimits limits;
  limits.maximum_instructions_per_step = 9;
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core(), limits));
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 42);
  const auto rejected = vm.Step();
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kInterrupted, rejected.error().code());
  EXPECT_EQ(VmState::kError, vm.state());
  EXPECT_TRUE(vm.row().empty());
  RequireStatus(vm.Reset());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 42);
}

TEST_F(VmTest, MatchesPinnedSqliteUnaryAndArithmeticSemantics) {
  ExpectInteger(EvaluateUnary(*database_, UnaryOperation::kNegate, SqlValue::Text("5")), -5);
  ExpectReal(EvaluateUnary(*database_, UnaryOperation::kNegate, SqlValue::Text("1.5")), -1.5);
  ExpectInteger(EvaluateUnary(*database_, UnaryOperation::kNegate, SqlValue::Text("abc")), 0);
  ExpectReal(EvaluateUnary(*database_, UnaryOperation::kNegate,
                           SqlValue::Integer(std::numeric_limits<std::int64_t>::min())),
             9223372036854775808.0);
  ExpectInteger(EvaluateUnary(*database_, UnaryOperation::kBitwiseNot, SqlValue::Text("3.9")), -4);
  ExpectInteger(EvaluateUnary(*database_, UnaryOperation::kLogicalNot, SqlValue::Text("2x")), 0);
  ExpectInteger(EvaluateUnary(*database_, UnaryOperation::kLogicalNot, SqlValue::Text("abc")), 1);
  EXPECT_EQ(EvaluateUnary(*database_, UnaryOperation::kLogicalNot, SqlValue{}).type(),
            SqlValueType::kNull);

  ExpectInteger(
      EvaluateBinary(*database_, BinaryOperation::kAdd, SqlValue::Integer(2), SqlValue::Integer(3)),
      5);
  ExpectReal(EvaluateBinary(*database_, BinaryOperation::kAdd, SqlValue::Text("1.0"),
                            SqlValue::Integer(2)),
             3.0);
  ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kAdd, SqlValue::Text("abc"),
                               SqlValue::Integer(2)),
                2);
  ExpectReal(EvaluateBinary(*database_, BinaryOperation::kAdd,
                            SqlValue::Integer(std::numeric_limits<std::int64_t>::max()),
                            SqlValue::Integer(1)),
             9223372036854775808.0);
  ExpectReal(EvaluateBinary(*database_, BinaryOperation::kMultiply, SqlValue::Integer(3037000500),
                            SqlValue::Integer(3037000500)),
             9.2233720370002493e18);
  ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kDivide, SqlValue::Integer(7),
                               SqlValue::Integer(2)),
                3);
  ExpectReal(EvaluateBinary(*database_, BinaryOperation::kDivide, SqlValue::Integer(7),
                            SqlValue::Real(2.0)),
             3.5);
  EXPECT_EQ(EvaluateBinary(*database_, BinaryOperation::kDivide, SqlValue::Integer(7),
                           SqlValue::Integer(0))
                .type(),
            SqlValueType::kNull);
  ExpectReal(EvaluateBinary(*database_, BinaryOperation::kRemainder, SqlValue::Real(7.9),
                            SqlValue::Real(2.1)),
             1.0);
  ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kRemainder,
                               SqlValue::Integer(std::numeric_limits<std::int64_t>::min()),
                               SqlValue::Integer(-1)),
                0);
}

TEST_F(VmTest, ComputesBorrowedArithmeticBeforePublishingAliasedOutputs) {
  for (const auto output : {Reg(0), Reg(1), Reg(2)}) {
    SCOPED_TRACE(output.value());
    ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kAdd, SqlValue::Integer(2),
                                 SqlValue::Integer(3), output),
                  5);
    ExpectReal(EvaluateBinary(*database_, BinaryOperation::kAdd, SqlValue::Text("1.0"),
                              SqlValue::Integer(2), output),
               3.0);
    ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kSubtract,
                                 SqlValue::Blob(ByteBuffer::CopyOf(AsBytes("8tail"))),
                                 SqlValue::Integer(3), output),
                  5);
    ExpectReal(EvaluateBinary(*database_, BinaryOperation::kDivide,
                              SqlValue::Integer(std::numeric_limits<std::int64_t>::min()),
                              SqlValue::Integer(-1), output),
               9223372036854775808.0);
    ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kRemainder,
                                 SqlValue::Integer(std::numeric_limits<std::int64_t>::min()),
                                 SqlValue::Integer(-1), output),
                  0);
    EXPECT_EQ(SqlValueType::kNull, EvaluateBinary(*database_, BinaryOperation::kAdd, SqlValue{},
                                                  SqlValue::Text("not numeric"), output)
                                       .type());
    EXPECT_EQ(SqlValueType::kNull, EvaluateBinary(*database_, BinaryOperation::kAdd,
                                                  SqlValue::Text("not numeric"), SqlValue{}, output)
                                       .type());
  }
  for (const auto output : {Reg(0), Reg(1)}) {
    ExpectReal(EvaluateUnary(*database_, UnaryOperation::kNegate, SqlValue::Text("1.5"), output),
               -1.5);
    ExpectReal(EvaluateUnary(*database_, UnaryOperation::kNegate,
                             SqlValue::Integer(std::numeric_limits<std::int64_t>::min()), output),
               9223372036854775808.0);
    EXPECT_EQ(SqlValueType::kNull,
              EvaluateUnary(*database_, UnaryOperation::kNegate, SqlValue{}, output).type());
  }
}

TEST_F(VmTest, PublishesPrimitiveResultsWithAliasedInputsAndNativeNullRules) {
  for (const auto output : {Reg(0), Reg(1), Reg(2)}) {
    SCOPED_TRACE(output.value());
    const SqlValue zero = EvaluateBinary(*database_, BinaryOperation::kMultiply,
                                         SqlValue::Real(-0.0), SqlValue::Integer(2), output);
    ExpectReal(zero, -0.0);
    ASSERT_TRUE(zero.real_value().has_value());
    EXPECT_TRUE(std::signbit(*zero.real_value()));
    EXPECT_EQ(SqlValueType::kNull,
              EvaluateBinary(*database_, BinaryOperation::kAdd,
                             SqlValue::Real(std::numeric_limits<double>::infinity()),
                             SqlValue::Real(-std::numeric_limits<double>::infinity()), output)
                  .type());
    ExpectReal(EvaluateBinary(*database_, BinaryOperation::kAdd,
                              SqlValue::Integer(std::numeric_limits<std::int64_t>::max()),
                              SqlValue::Integer(1), output),
               9223372036854775808.0);
    EXPECT_EQ(SqlValueType::kNull,
              EvaluateBinary(*database_, BinaryOperation::kDivide, SqlValue::Integer(7),
                             SqlValue::Integer(0), output)
                  .type());
    ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kBitwiseAnd, SqlValue::Integer(6),
                                 SqlValue::Integer(3), output),
                  2);
    ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kShiftRight, SqlValue::Integer(8),
                                 SqlValue::Integer(-1), output),
                  16);
    ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kLogicalAnd, SqlValue{},
                                 SqlValue::Integer(0), output),
                  0);
    ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kLogicalOr, SqlValue{},
                                 SqlValue::Integer(1), output),
                  1);
    EXPECT_EQ(SqlValueType::kNull, EvaluateBinary(*database_, BinaryOperation::kLogicalOr,
                                                  SqlValue{}, SqlValue::Integer(0), output)
                                       .type());
    EXPECT_EQ(SqlValueType::kNull, EvaluateBinary(*database_, BinaryOperation::kConcatenate,
                                                  SqlValue{}, SqlValue::Text("unused"), output)
                                       .type());
  }
  for (const auto output : {Reg(0), Reg(1)}) {
    ExpectInteger(
        EvaluateUnary(*database_, UnaryOperation::kBitwiseNot, SqlValue::Text("3.9"), output), -4);
    ExpectInteger(
        EvaluateUnary(*database_, UnaryOperation::kLogicalNot, SqlValue::Text("abc"), output), 1);
  }
}

TEST_F(VmTest, MatchesPinnedSqliteConcatenationBitwiseAndLogicalSemantics) {
  ExpectText(EvaluateBinary(*database_, BinaryOperation::kConcatenate, SqlValue::Integer(12),
                            SqlValue::Real(3.5)),
             "123.5");
  ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kBitwiseAnd, SqlValue::Integer(6),
                               SqlValue::Integer(3)),
                2);
  ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kShiftLeft, SqlValue::Integer(8),
                               SqlValue::Integer(-1)),
                4);
  ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kShiftRight, SqlValue::Integer(8),
                               SqlValue::Integer(-1)),
                16);
  ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kShiftLeft, SqlValue::Integer(-1),
                               SqlValue::Integer(64)),
                0);
  ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kShiftRight, SqlValue::Integer(-1),
                               SqlValue::Integer(64)),
                -1);
  ExpectInteger(EvaluateBinary(*database_, BinaryOperation::kShiftLeft, SqlValue::Integer(1),
                               SqlValue::Integer(std::numeric_limits<std::int64_t>::min())),
                0);

  ExpectInteger(
      EvaluateBinary(*database_, BinaryOperation::kLogicalAnd, SqlValue{}, SqlValue::Integer(0)),
      0);
  EXPECT_EQ(
      EvaluateBinary(*database_, BinaryOperation::kLogicalAnd, SqlValue{}, SqlValue::Integer(1))
          .type(),
      SqlValueType::kNull);
  EXPECT_EQ(
      EvaluateBinary(*database_, BinaryOperation::kLogicalOr, SqlValue{}, SqlValue::Integer(0))
          .type(),
      SqlValueType::kNull);
  ExpectInteger(
      EvaluateBinary(*database_, BinaryOperation::kLogicalOr, SqlValue{}, SqlValue::Integer(1)), 1);
}

[[nodiscard]] SqlValue EvaluateJump(BtreeDatabase& database, JumpCondition condition,
                                    SqlValue input) {
  Pager& pager = database.pager();
  std::vector<SqlValue> constants;
  constants.push_back(std::move(input));
  constants.push_back(SqlValue::Integer(0));
  constants.push_back(SqlValue::Integer(1));
  const BytecodeProgram program =
      BuildProgram(pager, 2, 0, std::move(constants), {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
                       JumpIfInstruction{
                           .condition = condition,
                           .input = Reg(0),
                           .target = Address(4),
                       },
                       JumpInstruction{.target = Address(5)},
                       LoadConstantInstruction{.constant = Constant(2), .output = Reg(1)},
                       ResultRowInstruction{.first = Reg(1), .count = 1},
                       HaltInstruction{},
                   });
  Vm vm = CreateAttachedVm(program, database, kCatalogGeneration);
  if (TakeValue(vm.Step()) != VmStep::kRow) {
    throw std::runtime_error("conditional VM program did not yield a row");
  }
  return OnlyRowValue(vm).Clone();
}

TEST_F(VmTest, ExecutesCopyAffinityCastAndConditionalJumps) {
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Text("42.5"));
  const BytecodeProgram conversion =
      BuildProgram(*pager_, 3, 0, std::move(constants), {}, {},
                   {ResultColumn("source"), ResultColumn("numeric"), ResultColumn("integer")},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       CopyInstruction{.input = Reg(0), .output = Reg(1)},
                       ApplyAffinityInstruction{
                           .input = Reg(1),
                           .affinity = TypeAffinity::kNumeric,
                           .output = Reg(1),
                       },
                       CastInstruction{
                           .input = Reg(1),
                           .target = CastTarget::kInteger,
                           .output = Reg(2),
                       },
                       ResultRowInstruction{.first = Reg(0), .count = 3},
                       HaltInstruction{},
                   });
  Vm conversion_vm = CreateCoreVm(conversion);
  ASSERT_EQ(TakeValue(conversion_vm.Step()), VmStep::kRow);
  ASSERT_EQ(conversion_vm.row().size(), 3U);
  ExpectText(conversion_vm.row()[0], "42.5");
  ExpectReal(conversion_vm.row()[1], 42.5);
  ExpectInteger(conversion_vm.row()[2], 42);

  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfTrue, SqlValue::Integer(1)), 1);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfTrue, SqlValue{}), 0);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfFalse, SqlValue::Integer(0)), 1);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfTrueOrNull, SqlValue::Integer(1)), 1);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfTrueOrNull, SqlValue::Integer(0)), 0);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfTrueOrNull, SqlValue{}), 1);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfFalseOrNull, SqlValue::Integer(0)), 1);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfFalseOrNull, SqlValue::Integer(1)), 0);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfFalseOrNull, SqlValue{}), 1);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfNull, SqlValue{}), 1);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfNotNull, SqlValue::Integer(0)), 1);
  ExpectInteger(EvaluateJump(*database_, JumpCondition::kIfNotNull, SqlValue{}), 0);
}

TEST_F(VmTest, ExecutesStrictIntegerAndIntegerOnlyRealAffinity) {
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Text("7.0"));
  constants.push_back(SqlValue::Integer(8));
  constants.push_back(SqlValue::Text("9"));
  const BytecodeProgram conversion = BuildProgram(
      *pager_, 6, 0, std::move(constants), {}, {},
      {ResultColumn("strict"), ResultColumn("integer_real"), ResultColumn("text_real")},
      {
          LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
          MustBeIntegerInstruction{.input = Reg(0), .output = Reg(3)},
          LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
          RealAffinityInstruction{.input = Reg(1), .output = Reg(4)},
          LoadConstantInstruction{.constant = Constant(2), .output = Reg(2)},
          RealAffinityInstruction{.input = Reg(2), .output = Reg(5)},
          ResultRowInstruction{.first = Reg(3), .count = 3},
          HaltInstruction{},
      });
  Vm conversion_vm = CreateCoreVm(conversion);
  ASSERT_EQ(TakeValue(conversion_vm.Step()), VmStep::kRow);
  ExpectInteger(conversion_vm.row()[0], 7);
  ExpectReal(conversion_vm.row()[1], 8.0);
  ExpectText(conversion_vm.row()[2], "9");

  std::vector<SqlValue> invalid_constants;
  invalid_constants.push_back(SqlValue::Text("7.5"));
  const BytecodeProgram invalid =
      BuildProgram(*pager_, 1, 0, std::move(invalid_constants), {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       MustBeIntegerInstruction{.input = Reg(0), .output = Reg(0)},
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   });
  Vm invalid_vm = CreateCoreVm(invalid);
  const auto failed = invalid_vm.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kTypeMismatch, failed.error().code());
  EXPECT_EQ("datatype mismatch", failed.error().message());

  std::vector<SqlValue> nan_constants;
  nan_constants.push_back(SqlValue::Real(std::numeric_limits<double>::quiet_NaN()));
  const BytecodeProgram nan =
      BuildProgram(*pager_, 1, 0, std::move(nan_constants), {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       MustBeIntegerInstruction{.input = Reg(0), .output = Reg(0)},
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   });
  Vm nan_vm = CreateCoreVm(nan);
  const auto nan_failed = nan_vm.Step();
  ASSERT_FALSE(nan_failed.has_value());
  EXPECT_EQ(ErrorCode::kTypeMismatch, nan_failed.error().code());
}

TEST_F(VmTest, NormalizesRealStorageAffinityBeforePersistentRecords) {
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Integer(INT64_C(9007199254740993)));
  constants.push_back(SqlValue::Real(1.5));
  constants.push_back(SqlValue::Text("9007199254740993"));
  constants.push_back(SqlValue::Integer(42));
  constants.push_back(SqlValue::Real(-0.0));
  constants.push_back(SqlValue::Text("42"));
  const BytecodeProgram conversion = BuildProgram(
      *pager_, 12, 0, std::move(constants), {}, {},
      {ResultColumn("integer"), ResultColumn("fractional"), ResultColumn("text"),
       ResultColumn("compact"), ResultColumn("negative_zero"), ResultColumn("compact_text")},
      {
          LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
          RealStorageAffinityInstruction{.input = Reg(0), .output = Reg(6)},
          LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
          RealStorageAffinityInstruction{.input = Reg(1), .output = Reg(7)},
          LoadConstantInstruction{.constant = Constant(2), .output = Reg(2)},
          RealStorageAffinityInstruction{.input = Reg(2), .output = Reg(8)},
          LoadConstantInstruction{.constant = Constant(3), .output = Reg(3)},
          RealStorageAffinityInstruction{.input = Reg(3), .output = Reg(9)},
          LoadConstantInstruction{.constant = Constant(4), .output = Reg(4)},
          RealStorageAffinityInstruction{.input = Reg(4), .output = Reg(10)},
          LoadConstantInstruction{.constant = Constant(5), .output = Reg(5)},
          RealStorageAffinityInstruction{.input = Reg(5), .output = Reg(11)},
          ResultRowInstruction{.first = Reg(6), .count = 6},
          HaltInstruction{},
      });
  Vm vm = CreateCoreVm(conversion);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ExpectReal(vm.row()[0], 9007199254740992.0);
  ExpectReal(vm.row()[1], 1.5);
  ExpectReal(vm.row()[2], 9007199254740992.0);
  ExpectReal(vm.row()[3], 42.0);
  ExpectReal(vm.row()[4], 0.0);
  ASSERT_TRUE(vm.row()[4].real_value().has_value());
  EXPECT_FALSE(std::signbit(*vm.row()[4].real_value()));
  ExpectReal(vm.row()[5], 42.0);
}

TEST_F(VmTest, AppliesComparisonAndScalarCallsWithAliasedOutputs) {
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Integer(2));
  constants.push_back(SqlValue::Integer(10));
  constants.push_back(SqlValue::Text("A"));
  constants.push_back(SqlValue::Text("a"));
  const BytecodeProgram program =
      BuildProgram(*pager_, 4, 0, std::move(constants), {"BINARY", "NOCASE", "nullif"}, {},
                   {ResultColumn("integer_order"), ResultColumn("nullif")},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
                       CompareInstruction{
                           .comparison = SqlComparison::kLess,
                           .affinity = TypeAffinity::kText,
                           .collation = Symbol(0),
                           .left = Reg(0),
                           .right = Reg(1),
                           .output = Reg(0),
                       },
                       LoadConstantInstruction{.constant = Constant(2), .output = Reg(1)},
                       LoadConstantInstruction{.constant = Constant(3), .output = Reg(2)},
                       CallScalarInstruction{
                           .function = Symbol(2),
                           .collation = Symbol(1),
                           .first_argument = Reg(1),
                           .argument_count = 2,
                           .output = Reg(1),
                       },
                       ResultRowInstruction{.first = Reg(0), .count = 2},
                       HaltInstruction{},
                   });

  Vm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ASSERT_EQ(vm.row().size(), 2U);
  ExpectInteger(vm.row()[0], 1);
  EXPECT_EQ(vm.row()[1].type(), SqlValueType::kNull);

  const BytecodeProgram text_comparison =
      BuildProgram(*pager_, 3, 0,
                   [] {
                     std::vector<SqlValue> values;
                     values.push_back(SqlValue::Text("2"));
                     values.push_back(SqlValue::Integer(10));
                     return values;
                   }(),
                   {"BINARY"}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
                       CompareInstruction{
                           .comparison = SqlComparison::kLess,
                           .affinity = TypeAffinity::kText,
                           .collation = Symbol(0),
                           .left = Reg(0),
                           .right = Reg(1),
                           .output = Reg(2),
                       },
                       ResultRowInstruction{.first = Reg(2), .count = 1},
                       HaltInstruction{},
                   });
  Vm comparison_vm = CreateCoreVm(text_comparison);
  ASSERT_EQ(TakeValue(comparison_vm.Step()), VmStep::kRow);
  ExpectInteger(OnlyRowValue(comparison_vm), 0);
}

class LengthCollation final : public Collation {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "LENGTH"; }

  [[nodiscard]] std::weak_ordering Compare(Utf8View left, Utf8View right) const noexcept override {
    return left.bytes().size() <=> right.bytes().size();
  }
};

TEST_F(VmTest, ResolvesCustomCollationsAndRejectsMissingRuntimeRegistrations) {
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Text("a"));
  constants.push_back(SqlValue::Text("bb"));
  const BytecodeProgram custom =
      BuildProgram(*pager_, 3, 0, std::move(constants), {"length"}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
                       CompareInstruction{
                           .comparison = SqlComparison::kLess,
                           .affinity = TypeAffinity::kNone,
                           .collation = Symbol(0),
                           .left = Reg(0),
                           .right = Reg(1),
                           .output = Reg(2),
                       },
                       ResultRowInstruction{.first = Reg(2), .count = 1},
                       HaltInstruction{},
                   });
  const LengthCollation length;
  const std::array<const Collation*, 1> collations{&length};
  Vm custom_vm = CreateAttachedVm(custom, *database_, kCatalogGeneration,
                                  VmEnvironment{CoreFunctionRegistry(), collations});
  ASSERT_EQ(TakeValue(custom_vm.Step()), VmStep::kRow);
  ExpectInteger(OnlyRowValue(custom_vm), 1);

  const BytecodeProgram missing_collation =
      BuildProgram(*pager_, 3, 0,
                   [] {
                     std::vector<SqlValue> values;
                     values.push_back(SqlValue::Integer(1));
                     values.push_back(SqlValue::Integer(2));
                     return values;
                   }(),
                   {"missing"}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
                       CompareInstruction{
                           .comparison = SqlComparison::kLess,
                           .affinity = TypeAffinity::kNone,
                           .collation = Symbol(0),
                           .left = Reg(0),
                           .right = Reg(1),
                           .output = Reg(2),
                       },
                       ResultRowInstruction{.first = Reg(2), .count = 1},
                       HaltInstruction{},
                   });
  const auto unresolved_collation = Vm::Create(missing_collation, VmEnvironment::Core());
  ASSERT_FALSE(unresolved_collation.has_value());
  EXPECT_EQ(unresolved_collation.error().code(), ErrorCode::kGeneric);

  const BytecodeProgram missing_function =
      BuildProgram(*pager_, 1, 0, {}, {"missing", "BINARY"}, {}, {ResultColumn()},
                   {
                       CallScalarInstruction{
                           .function = Symbol(0),
                           .collation = Symbol(1),
                           .first_argument = Reg(0),
                           .argument_count = 0,
                           .output = Reg(0),
                       },
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   });
  const auto unresolved_function = Vm::Create(missing_function, VmEnvironment::Core());
  ASSERT_FALSE(unresolved_function.has_value());
  EXPECT_EQ(unresolved_function.error().code(), ErrorCode::kGeneric);
}

TEST_F(VmTest, EnforcesSchemaIdentityAndInstructionBudgets) {
  const BytecodeProgram halt =
      BuildProgram(*pager_, 0, 0, {}, {}, {}, {}, {HaltInstruction{}}, std::nullopt, true);

  Vm generation_mismatch = TakeValue(Vm::Create(halt, VmEnvironment::Core()));
  const Status generation_attach = generation_mismatch.AttachExecutionContext(
      VmExecutionContext{*database_, kCatalogGeneration + 1U});
  ASSERT_FALSE(generation_attach.has_value());
  EXPECT_EQ(generation_attach.error().code(), ErrorCode::kSchemaChanged);

  SchemaVersionRequirement wrong_cookie = CurrentSchema(*pager_);
  ++wrong_cookie.schema_cookie;
  const BytecodeProgram wrong_schema =
      BuildProgram(*pager_, 0, 0, {}, {}, {}, {}, {HaltInstruction{}}, wrong_cookie);
  Vm cookie_mismatch = TakeValue(Vm::Create(wrong_schema, VmEnvironment::Core()));
  const Status cookie_attach =
      cookie_mismatch.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration});
  ASSERT_FALSE(cookie_attach.has_value());
  EXPECT_EQ(cookie_attach.error().code(), ErrorCode::kSchemaChanged);

  RequireStatus(pager_->EndRead());
  Vm inactive = TakeValue(Vm::Create(halt, VmEnvironment::Core()));
  const Status inactive_attach =
      inactive.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration});
  ASSERT_FALSE(inactive_attach.has_value());
  EXPECT_EQ(inactive_attach.error().code(), ErrorCode::kMisuse);
  Vm inactive_stale = TakeValue(Vm::Create(halt, VmEnvironment::Core()));
  const Status inactive_stale_attach = inactive_stale.AttachExecutionContext(
      VmExecutionContext{*database_, kCatalogGeneration + 1U});
  ASSERT_FALSE(inactive_stale_attach.has_value());
  EXPECT_EQ(inactive_stale_attach.error().code(), ErrorCode::kMisuse);
  RequireStatus(pager_->BeginRead());

  const BytecodeProgram loop =
      BuildProgram(*pager_, 0, 0, {}, {}, {}, {}, {JumpInstruction{.target = Address(0)}});
  Vm vm = CreateCoreVm(loop, VmLimits{
                                 .maximum_value_bytes = 1'000'000'000,
                                 .maximum_instructions_per_step = 10,
                             });
  const auto interrupted = vm.Step();
  ASSERT_FALSE(interrupted.has_value());
  EXPECT_EQ(interrupted.error().code(), ErrorCode::kInterrupted);
  EXPECT_EQ(vm.state(), VmState::kError);
  EXPECT_EQ(vm.executed_instruction_count(), 10U);
}

TEST(VmSnapshotTest, ResetCanAttachToANewDataOnlySnapshot) {
  std::vector<std::byte> bytes = ReadBytes(FixturePath());
  TemporaryDatabase database(bytes);
  PosixVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());

  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Integer(9));
  const BytecodeProgram program =
      BuildProgram(*pager, 1, 0, std::move(constants), {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   },
                   std::nullopt, true);
  Vm vm = CreateAttachedVm(program, *pager_database, kCatalogGeneration);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kDone);
  RequireStatus(vm.Reset());
  RequireStatus(pager->EndRead());

  const std::uint32_t next_change = Read32(bytes, 24) + 1U;
  Write32(bytes, 24, next_change);
  Write32(bytes, 92, next_change);
  database.Rewrite(bytes);

  RequireStatus(pager->BeginRead());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_database, kCatalogGeneration}));
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ExpectInteger(OnlyRowValue(vm), 9);
  RequireStatus(vm.Reset());
  RequireStatus(pager->EndRead());
}

[[nodiscard]] BytecodeProgram TableScanProgram(const Pager& pager) {
  return BuildProgram(
      pager, 2, 0, {}, {},
      {
          TableDescriptor({
              CursorFieldSource{
                  .kind = CursorFieldSourceKind::kRowId,
                  .record_field = 0,
              },
              CursorFieldSource{
                  .kind = CursorFieldSourceKind::kRecordField,
                  .record_field = 1,
              },
          }),
      },
      {ResultColumn("id", TypeAffinity::kInteger), ResultColumn("binary_key", TypeAffinity::kText)},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(6)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          NextInstruction{.cursor = Cursor(0), .next_target = Address(2)},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
      });
}

TEST_F(VmTest, RetainsFieldStorageAcrossScalarAndNullRegisterStores) {
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Integer(7));
  constants.push_back(SqlValue::Real(1.25));
  constants.emplace_back();
  const BytecodeProgram program = BuildProgram(
      *pager_, 1, 0, std::move(constants), {},
      {TableDescriptor({
          CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 1},
      })},
      {ResultColumn()},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(14)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          ResultRowInstruction{.first = Reg(0), .count = 1},
          LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
          ResultRowInstruction{.first = Reg(0), .count = 1},
          LoadConstantInstruction{.constant = Constant(1), .output = Reg(0)},
          ResultRowInstruction{.first = Reg(0), .count = 1},
          LoadConstantInstruction{.constant = Constant(2), .output = Reg(0)},
          ResultRowInstruction{.first = Reg(0), .count = 1},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          ResultRowInstruction{.first = Reg(0), .count = 1},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
      });
  Vm vm = CreateCoreVm(program);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_TRUE(OnlyRowValue(vm).text_value().has_value());
  const std::string text{OnlyRowValue(vm).text_value()->bytes()};
  const std::size_t capacity = OnlyRowValue(vm).owned_capacity_bytes();
  ASSERT_GT(capacity, 0U);
  for (const SqlValueType expected :
       {SqlValueType::kInteger, SqlValueType::kReal, SqlValueType::kNull}) {
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    EXPECT_EQ(expected, OnlyRowValue(vm).type());
    EXPECT_GE(OnlyRowValue(vm).owned_capacity_bytes(), capacity);
  }
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectText(OnlyRowValue(vm), text);
  EXPECT_GE(OnlyRowValue(vm).owned_capacity_bytes(), capacity);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, PublishesNullRowsThenRepositionsTheOwnedTableCursor) {
  const BytecodeProgram program = BuildProgram(
      *pager_, 2, 0, {}, {},
      {TableDescriptor({
          CursorFieldSource{.kind = CursorFieldSourceKind::kRowId, .record_field = 0},
          CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 1},
      })},
      {ResultColumn("id"), ResultColumn("key")},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          NullRowInstruction{.cursor = Cursor(0)},
          ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(0)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          NullRowInstruction{.cursor = Cursor(0), .enabled = false},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(12)},
          ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(0)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
      });
  Vm vm = CreateCoreVm(program);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(SqlValueType::kNull, vm.row()[0].type());
  EXPECT_EQ(SqlValueType::kNull, vm.row()[1].type());
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(SqlValueType::kInteger, vm.row()[0].type());
  EXPECT_EQ(SqlValueType::kText, vm.row()[1].type());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, NullPublicationDiscardsCachedRecordsAndIgnoresMissingFieldDefaults) {
  CursorFieldSource missing{
      .kind = CursorFieldSourceKind::kRecordField,
      .record_field = 31,
      .missing_value_kind = MissingFieldValueKind::kConstant,
      .missing_value = Constant(0),
  };
  auto descriptor = TableDescriptor(
      {
          CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 1},
          missing,
      },
      32);
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Integer(42));
  const BytecodeProgram program = BuildProgram(
      *pager_, 2, 0, std::move(constants), {}, {std::move(descriptor)},
      {ResultColumn("key"), ResultColumn("default")},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(12)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          NullRowInstruction{.cursor = Cursor(0)},
          NullRowInstruction{.cursor = Cursor(0)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
      });
  Vm vm = CreateCoreVm(program);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(SqlValueType::kText, vm.row()[0].type());
  ExpectInteger(vm.row()[1], 42);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(SqlValueType::kNull, vm.row()[0].type());
  EXPECT_EQ(SqlValueType::kNull, vm.row()[1].type());
  RequireStatus(vm.Reset());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(SqlValueType::kText, vm.row()[0].type());
}

TEST_F(VmTest, ConsumesVirtualRowsWithoutAdvancingPhysicalTableOrIndexStorage) {
  for (const bool index : {false, true}) {
    for (const bool previous : {false, true}) {
      auto descriptor = index ? BinaryIndexDescriptor()
                              : TableDescriptor({
                                    CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField,
                                                      .record_field = 1},
                                });
      const std::vector<std::string> symbols =
          index ? std::vector<std::string>{"BINARY"} : std::vector<std::string>{};
      const Instruction advance =
          previous
              ? Instruction{PreviousInstruction{.cursor = Cursor(0), .previous_target = Address(4)}}
              : Instruction{NextInstruction{.cursor = Cursor(0), .next_target = Address(4)}};
      const BytecodeProgram program = BuildProgram(
          *pager_, 1, 0, {}, symbols, {std::move(descriptor)}, {ResultColumn()},
          {
              OpenReadCursorInstruction{.cursor = Cursor(0)},
              NullRowInstruction{.cursor = Cursor(0)},
              ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
              ResultRowInstruction{.first = Reg(0), .count = 1},
              advance,
              CloseCursorInstruction{.cursor = Cursor(0)},
              HaltInstruction{},
          });
      Vm vm = CreateCoreVm(program);
      ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
      EXPECT_EQ(SqlValueType::kNull, OnlyRowValue(vm).type());
      EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
      EXPECT_EQ(7U, vm.executed_instruction_count());
    }
  }
}

TEST_F(VmTest, ScansTableRowsAndClosesCursorsAtHalt) {
  const BytecodeProgram program = TableScanProgram(*pager_);
  Vm vm = CreateCoreVm(program);

  std::size_t rows = 0;
  std::int64_t first_rowid = 0;
  std::int64_t last_rowid = 0;
  std::string first_key;
  while (true) {
    const VmStep step = TakeValue(vm.Step());
    if (step == VmStep::kDone) {
      break;
    }
    ASSERT_EQ(vm.row().size(), 2U);
    ASSERT_EQ(vm.row()[0].type(), SqlValueType::kInteger);
    const std::int64_t rowid = vm.row()[0].integer_value().value_or(0);
    if (rows == 0) {
      first_rowid = rowid;
      ASSERT_EQ(vm.row()[1].type(), SqlValueType::kText);
      first_key = std::string{vm.row()[1].text_value().value_or(Utf8View{}).bytes()};
    }
    last_rowid = rowid;
    ++rows;
  }

  EXPECT_EQ(rows, 166U);
  EXPECT_EQ(first_rowid, std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(first_key, "bin-min");
  EXPECT_EQ(last_rowid, std::numeric_limits<std::int64_t>::max());
  RequireStatus(pager_->EndRead());
}

TEST_F(VmTest, KeepsCursorPinsAcrossRowsAndReleasesThemOnReset) {
  const BytecodeProgram program = TableScanProgram(*pager_);
  Vm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);

  const Status pinned = pager_->EndRead();
  ASSERT_FALSE(pinned.has_value());
  EXPECT_EQ(pinned.error().code(), ErrorCode::kBusy);

  RequireStatus(vm.Reset());
  RequireStatus(pager_->EndRead());

  RequireStatus(pager_->BeginRead());
  {
    Vm scoped_vm = CreateCoreVm(program);
    ASSERT_EQ(TakeValue(scoped_vm.Step()), VmStep::kRow);
    const Status scoped_pin = pager_->EndRead();
    ASSERT_FALSE(scoped_pin.has_value());
    EXPECT_EQ(scoped_pin.error().code(), ErrorCode::kBusy);
  }
  RequireStatus(pager_->EndRead());
}

[[nodiscard]] BytecodeProgram SeekFieldProgram(const Pager& pager, CursorFieldSource source,
                                               std::uint32_t record_field_count = 7,
                                               std::vector<SqlValue> constants = {}) {
  return BuildProgram(
      pager, 2, 1, std::move(constants), {},
      {
          TableDescriptor({source}, record_field_count),
      },
      {ResultColumn()},
      {
          LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          SeekRowIdInstruction{
              .cursor = Cursor(0),
              .key = Reg(0),
              .missing_target = Address(7),
          },
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(1), .count = 1},
          CloseCursorInstruction{.cursor = Cursor(0)},
          JumpInstruction{.target = Address(9)},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
          HaltInstruction{},
      });
}

TEST_F(VmTest, SeeksRowidsReadsOverflowFieldsAndReturnsNullForMissingFields) {
  const BytecodeProgram payload_program =
      SeekFieldProgram(*pager_, CursorFieldSource{
                                    .kind = CursorFieldSourceKind::kRecordField,
                                    .record_field = 6,
                                });
  Vm payload_vm = CreateCoreVm(payload_program);
  const SqlValue key = SqlValue::Text("44.0");
  RequireStatus(payload_vm.Bind(Parameter(0), key));
  ASSERT_EQ(TakeValue(payload_vm.Step()), VmStep::kRow);
  ASSERT_EQ(OnlyRowValue(payload_vm).type(), SqlValueType::kBlob);
  EXPECT_EQ(OnlyRowValue(payload_vm).blob_value().value_or(ByteView{}).size(), 3000U);
  EXPECT_EQ(TakeValue(payload_vm.Step()), VmStep::kDone);

  const BytecodeProgram missing_program =
      SeekFieldProgram(*pager_,
                       CursorFieldSource{
                           .kind = CursorFieldSourceKind::kRecordField,
                           .record_field = 7,
                       },
                       8);
  Vm missing_vm = CreateCoreVm(missing_program);
  const SqlValue integer_key = SqlValue::Integer(44);
  RequireStatus(missing_vm.Bind(Parameter(0), integer_key));
  ASSERT_EQ(TakeValue(missing_vm.Step()), VmStep::kRow);
  EXPECT_EQ(OnlyRowValue(missing_vm).type(), SqlValueType::kNull);

  std::vector<SqlValue> default_constants;
  default_constants.push_back(SqlValue::Text("legacy"));
  const BytecodeProgram default_program =
      SeekFieldProgram(*pager_,
                       CursorFieldSource{
                           .kind = CursorFieldSourceKind::kRecordField,
                           .record_field = 7,
                           .missing_value_kind = MissingFieldValueKind::kConstant,
                           .missing_value = Constant(0),
                       },
                       8, std::move(default_constants));
  Vm default_vm = CreateCoreVm(default_program);
  RequireStatus(default_vm.Bind(Parameter(0), integer_key));
  ASSERT_EQ(TakeValue(default_vm.Step()), VmStep::kRow);
  ExpectText(OnlyRowValue(default_vm), "legacy");

  const BytecodeProgram unsupported_program =
      SeekFieldProgram(*pager_,
                       CursorFieldSource{
                           .kind = CursorFieldSourceKind::kRecordField,
                           .record_field = 7,
                           .missing_value_kind = MissingFieldValueKind::kUnsupported,
                       },
                       8);
  Vm unsupported_vm = CreateCoreVm(unsupported_program);
  RequireStatus(unsupported_vm.Bind(Parameter(0), integer_key));
  const auto unsupported = unsupported_vm.Step();
  ASSERT_FALSE(unsupported.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, unsupported.error().code());

  RequireStatus(missing_vm.Reset());
  const SqlValue fractional = SqlValue::Real(44.5);
  RequireStatus(missing_vm.Bind(Parameter(0), fractional));
  RequireStatus(
      missing_vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(TakeValue(missing_vm.Step()), VmStep::kDone);

  RequireStatus(missing_vm.Reset());
  const SqlValue rounded_past_max =
      SqlValue::Real(static_cast<double>(std::numeric_limits<std::int64_t>::max()));
  RequireStatus(missing_vm.Bind(Parameter(0), rounded_past_max));
  RequireStatus(
      missing_vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(TakeValue(missing_vm.Step()), VmStep::kDone);

  RequireStatus(missing_vm.Reset());
  const SqlValue minimum =
      SqlValue::Real(static_cast<double>(std::numeric_limits<std::int64_t>::min()));
  RequireStatus(missing_vm.Bind(Parameter(0), minimum));
  RequireStatus(
      missing_vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  ASSERT_EQ(TakeValue(missing_vm.Step()), VmStep::kRow);
  EXPECT_EQ(OnlyRowValue(missing_vm).type(), SqlValueType::kNull);
}

TEST_F(VmTest, AppliesValueLimitsToTheSelectedColumnInsteadOfTheOverflowRecord) {
  constexpr std::size_t kValueLimit = 64U;
  const BytecodeProgram small_program =
      SeekFieldProgram(*pager_, CursorFieldSource{
                                    .kind = CursorFieldSourceKind::kRecordField,
                                    .record_field = 1,
                                });
  Vm small_vm = CreateCoreVm(small_program, VmLimits{.maximum_value_bytes = kValueLimit});
  const SqlValue key = SqlValue::Integer(44);
  RequireStatus(small_vm.Bind(Parameter(0), key));
  const auto small = small_vm.Step();
  ASSERT_TRUE(small.has_value()) << small.error().ToString();
  EXPECT_EQ(VmStep::kRow, *small);
  ExpectText(OnlyRowValue(small_vm), "bin-05");
  EXPECT_EQ(VmStep::kDone, TakeValue(small_vm.Step()));

  const BytecodeProgram large_program =
      SeekFieldProgram(*pager_, CursorFieldSource{
                                    .kind = CursorFieldSourceKind::kRecordField,
                                    .record_field = 6,
                                });
  Vm large_vm = CreateCoreVm(large_program, VmLimits{.maximum_value_bytes = kValueLimit});
  RequireStatus(large_vm.Bind(Parameter(0), key));
  const auto large = large_vm.Step();
  ASSERT_FALSE(large.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, large.error().code());
}

[[nodiscard]] BytecodeProgram RepeatedColumnProgram(const Pager& pager,
                                                    ReadCursorDescriptor descriptor,
                                                    std::vector<std::string> symbols = {}) {
  return BuildProgram(
      pager, 2, 0, {}, std::move(symbols), {std::move(descriptor)},
      {ResultColumn("first"), ResultColumn("second")},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(7)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
      });
}

TEST(VmColumn, SharesRepeatedNonlocalTableBytesOnlyAboveTheNativeThreshold) {
  for (const bool blob : {false, true}) {
    SCOPED_TRACE(blob);
    for (const std::size_t size : {3'999U, 4'000U, 4'001U}) {
      SCOPED_TRACE(size);
      test::WritePagerFixedVfs vfs{false};
      auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
      ASSERT_NE(nullptr, database);
      Pager& pager = database->pager();
      RequireStatus(pager.BeginRead());
      RequireStatus(pager.BeginWrite());
      auto session = TakeValue(BtreeWriteSession::Open(*database));
      RequireStatus(session.InitializeDatabase());
      auto table = TakeValue(session.CreateTableBtree());
      ByteBuffer bytes{ByteCount{size}};
      std::ranges::fill(bytes.mutable_view(), std::byte{0x5a});
      const std::array values{blob ? SqlValue::Blob(bytes.Clone())
                                   : SqlValue::Text(std::string(size, 'Z'))};
      const ByteBuffer record = TakeValue(EncodeRecord(values));
      RequireStatus(table.Insert(1, record.view()));
      auto descriptor = TableDescriptor(
          {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0}}, 1);
      descriptor.root_page = RootPageNumber(table.root_page().value());
      const BytecodeProgram program = RepeatedColumnProgram(pager, descriptor);
      Vm vm = CreateAttachedVm(program, *database, kCatalogGeneration);
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
      instrumentation::CounterCollection work;
      {
        const instrumentation::ScopedCounterCollection collect{work};
#endif
        ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
      }
      EXPECT_EQ(size * (size > 4'000U ? 1U : 2U),
                work.Value(instrumentation::Counter::kBytesCopied));
#endif
      const auto row = vm.row();
      ASSERT_EQ(2U, row.size());
      const auto type = blob ? SqlValueType::kBlob : SqlValueType::kText;
      ASSERT_EQ(type, row[0].type());
      ASSERT_EQ(type, row[1].type());
      const ByteView first = blob ? *row[0].blob_value() : AsBytes(row[0].text_value()->bytes());
      const ByteView second = blob ? *row[1].blob_value() : AsBytes(row[1].text_value()->bytes());
      EXPECT_TRUE(std::ranges::equal(bytes.view(), first));
      EXPECT_TRUE(std::ranges::equal(bytes.view(), second));
      EXPECT_EQ(size > 4'000U, first.data() == second.data());
      EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
      RequireStatus(pager.Rollback());
    }
  }
}

TEST(VmColumn, DoesNotPublishSharedBytesAfterALatePayloadReadFailure) {
  constexpr std::size_t kBytes = 5'001U;
  test::WritePagerFixedVfs vfs{false};
  PageNumber root;
  ByteBuffer bytes{ByteCount{kBytes}};
  std::ranges::fill(bytes.mutable_view(), std::byte{0x5a});
  {
    auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    ASSERT_NE(nullptr, database);
    Pager& pager = database->pager();
    RequireStatus(pager.BeginRead());
    RequireStatus(pager.BeginWrite());
    {
      auto session = TakeValue(BtreeWriteSession::Open(*database));
      RequireStatus(session.InitializeDatabase());
      auto table = TakeValue(session.CreateTableBtree());
      root = table.root_page();
      const auto record = TakeValue(EncodeRecord(std::array{SqlValue::Blob(bytes.Clone())}));
      RequireStatus(table.Insert(1, record.view()));
    }
    RequireStatus(pager.Commit());
    RequireStatus(pager.EndRead());
  }
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 4U);
  ASSERT_NE(nullptr, database);
  Pager& pager = database->pager();
  RequireStatus(pager.BeginRead());
  auto descriptor = TableDescriptor(
      {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0}}, 1);
  descriptor.root_page = RootPageNumber(root.value());
  const auto program = RepeatedColumnProgram(pager, descriptor);
  Vm vm = CreateAttachedVm(program, *database, kCatalogGeneration);
  const auto pins = pager.cache_pin_count();
  const auto reads = vfs.total_database_reads();
  vfs.FailDatabaseReadAfter(2U, ErrorCode::kIo);
  const auto failed = vm.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_EQ(VmState::kError, vm.state());
  EXPECT_TRUE(vm.row().empty());
  EXPECT_EQ(pins, pager.cache_pin_count());
  EXPECT_EQ(reads + 3U, vfs.total_database_reads());
  RequireStatus(vm.Reset());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database, kCatalogGeneration}));
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  const auto row = vm.row();
  ASSERT_EQ(2U, row.size());
  ASSERT_TRUE(row[0].blob_value().has_value());
  ASSERT_TRUE(row[1].blob_value().has_value());
  EXPECT_TRUE(std::ranges::equal(bytes.view(), *row[0].blob_value()));
  EXPECT_TRUE(std::ranges::equal(bytes.view(), *row[1].blob_value()));
  EXPECT_EQ(row[0].blob_value()->data(), row[1].blob_value()->data());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(vm.DetachExecutionContext());
  RequireStatus(pager.EndRead());
}

TEST(VmColumn, DoesNotShareNonlocalIndexValuesOrFullyLocalLargeTableValues) {
  constexpr std::size_t kBytes = 5'001U;
  for (const bool index_source : {false, true}) {
    SCOPED_TRACE(index_source);
    test::WritePagerFixedVfs vfs{false};
    auto database = index_source
                        ? test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U)
                        : TakeValue(BtreeDatabase::OpenEphemeralMemory(
                              vfs, PagerOptions{.empty_database_page_size = ByteCount{65'536U},
                                                .cache_capacity_pages = 8U}));
    ASSERT_NE(nullptr, database);
    Pager& pager = database->pager();
    RequireStatus(pager.BeginRead());
    RequireStatus(pager.BeginWrite());
    auto session = TakeValue(BtreeWriteSession::Open(*database));
    RequireStatus(session.InitializeDatabase());
    ByteBuffer bytes{ByteCount{kBytes}};
    std::ranges::fill(bytes.mutable_view(), std::byte{0x5a});
    auto descriptor = TableDescriptor(
        {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0}}, 1);
    std::vector<std::string> symbols;
    if (index_source) {
      const std::array orders{IndexColumnOrder{}, IndexColumnOrder{}};
      auto index = TakeValue(session.CreateIndexBtree(TakeValue(IndexComparisonInfo::CopyOf(
          orders, IndexKeyShape{.key_field_count = 1U, .all_field_count = 2U}))));
      const std::array values{SqlValue::Blob(bytes.Clone()), SqlValue::Integer(1)};
      RequireStatus(index.Insert(values));
      descriptor.root_page = RootPageNumber(index.root_page().value());
      descriptor.storage = CursorStorageKind::kIndex;
      descriptor.record_field_count = 2U;
      descriptor.index_key_field_count = 1U;
      descriptor.index_columns = {
          IndexColumnMetadata{.collation = Symbol(0), .order = BytecodeSortOrder::kAscending},
          IndexColumnMetadata{.collation = Symbol(0), .order = BytecodeSortOrder::kAscending},
      };
      symbols = {"BINARY"};
    } else {
      auto table = TakeValue(session.CreateTableBtree());
      const auto record = TakeValue(EncodeRecord(std::array{SqlValue::Blob(bytes.Clone())}));
      RequireStatus(table.Insert(1, record.view()));
      descriptor.root_page = RootPageNumber(table.root_page().value());
    }
    const auto program = RepeatedColumnProgram(pager, descriptor, std::move(symbols));
    Vm vm = CreateAttachedVm(program, *database, kCatalogGeneration);
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    const auto row = vm.row();
    ASSERT_EQ(2U, row.size());
    ASSERT_TRUE(row[0].blob_value().has_value());
    ASSERT_TRUE(row[1].blob_value().has_value());
    EXPECT_TRUE(std::ranges::equal(bytes.view(), *row[0].blob_value()));
    EXPECT_TRUE(std::ranges::equal(bytes.view(), *row[1].blob_value()));
    EXPECT_NE(row[0].blob_value()->data(), row[1].blob_value()->data());
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    if (index_source) {
      RequireStatus(pager.Rollback());
    }
  }
}

TEST(VmColumn, KeysSharedValuesByPhysicalPositionAndResultSuspension) {
  enum class Transition { Reseek, Move, Suspend };
  constexpr std::size_t kBytes = 5'001U;
  for (const auto transition : {Transition::Reseek, Transition::Move, Transition::Suspend}) {
    SCOPED_TRACE(static_cast<int>(transition));
    test::WritePagerFixedVfs vfs{false};
    auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    ASSERT_NE(nullptr, database);
    Pager& pager = database->pager();
    RequireStatus(pager.BeginRead());
    RequireStatus(pager.BeginWrite());
    auto session = TakeValue(BtreeWriteSession::Open(*database));
    RequireStatus(session.InitializeDatabase());
    auto table = TakeValue(session.CreateTableBtree());
    ByteBuffer first{ByteCount{kBytes}};
    ByteBuffer second{ByteCount{kBytes}};
    std::ranges::fill(first.mutable_view(), std::byte{0x11});
    std::ranges::fill(second.mutable_view(), std::byte{0x22});
    const auto first_record = TakeValue(EncodeRecord(std::array{SqlValue::Blob(first.Clone())}));
    const auto second_record = TakeValue(EncodeRecord(std::array{SqlValue::Blob(second.Clone())}));
    RequireStatus(table.Insert(1, first_record.view()));
    RequireStatus(table.Insert(2, second_record.view()));
    auto descriptor = TableDescriptor(
        {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0}}, 1);
    descriptor.root_page = RootPageNumber(table.root_page().value());
    Instruction between = RewindInstruction{.cursor = Cursor(0), .empty_target = Address(10)};
    if (transition == Transition::Move) {
      between =
          SeekRowIdInstruction{.cursor = Cursor(0), .key = Reg(2), .missing_target = Address(10)};
    } else if (transition == Transition::Suspend) {
      between = ResultRowInstruction{.first = Reg(0), .count = 2};
    }
    std::vector<SqlValue> constants;
    constants.push_back(SqlValue::Integer(2));
    constants.emplace_back();
    const auto program = BuildProgram(
        pager, 3, 0, std::move(constants), {}, {descriptor},
        {ResultColumn("first"), ResultColumn("second")},
        {
            LoadConstantInstruction{.constant = Constant(0), .output = Reg(2)},
            LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
            OpenReadCursorInstruction{.cursor = Cursor(0)},
            RewindInstruction{.cursor = Cursor(0), .empty_target = Address(10)},
            ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
            between,
            ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(1)},
            ResultRowInstruction{.first = Reg(0), .count = 2},
            CloseCursorInstruction{.cursor = Cursor(0)},
            HaltInstruction{},
            CloseCursorInstruction{.cursor = Cursor(0)},
            HaltInstruction{},
        });
    Vm vm = CreateAttachedVm(program, *database, kCatalogGeneration);
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    if (transition == Transition::Suspend) {
      EXPECT_EQ(SqlValueType::kNull, vm.row()[1].type());
      ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    }
    const auto row = vm.row();
    ASSERT_EQ(2U, row.size());
    ASSERT_TRUE(row[0].blob_value().has_value());
    ASSERT_TRUE(row[1].blob_value().has_value());
    EXPECT_TRUE(std::ranges::equal(first.view(), *row[0].blob_value()));
    EXPECT_TRUE(std::ranges::equal(transition == Transition::Move ? second.view() : first.view(),
                                   *row[1].blob_value()));
    EXPECT_EQ(transition == Transition::Reseek,
              row[0].blob_value()->data() == row[1].blob_value()->data());
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    RequireStatus(pager.Rollback());
  }
}

class ColumnWriteCacheProbe final {
 public:
  ColumnWriteCacheProbe(Vm& vm, ByteView expected) noexcept {
    vm_ = &vm;
    expected_ = expected;
    nested_rejected_ = false;
    observed_ = false;
  }
  ColumnWriteCacheProbe(const ColumnWriteCacheProbe&) = delete;
  ColumnWriteCacheProbe& operator=(const ColumnWriteCacheProbe&) = delete;
  ~ColumnWriteCacheProbe() {
    vm_ = nullptr;
    expected_ = {};
  }

  [[nodiscard]] bool valid() const noexcept { return nested_rejected_ && observed_; }

  [[nodiscard]] static Result<SqlValue> Nested(const ScalarFunctionContext&,
                                               std::span<const SqlValue>) {
    if (vm_ == nullptr) {
      return std::unexpected(Error::Create(ErrorCode::kInternal, "column cache probe is inactive"));
    }
    const auto nested = vm_->Step();
    nested_rejected_ = !nested.has_value() && nested.error().code() == ErrorCode::kMisuse &&
                       vm_->state() == VmState::Running;
    return SqlValue::Integer(1);
  }

  [[nodiscard]] static Result<SqlValue> Observe(const ScalarFunctionContext&,
                                                std::span<const SqlValue> arguments) {
    if (arguments.size() != 3U) {
      return std::unexpected(
          Error::Create(ErrorCode::kInternal, "column cache probe width is invalid"));
    }
    const auto before = arguments[0].blob_value();
    const auto after = arguments[1].blob_value();
    const auto nested = arguments[2].blob_value();
    observed_ = before.has_value() && after.has_value() && nested.has_value() &&
                std::ranges::equal(*before, expected_) && std::ranges::equal(*after, expected_) &&
                std::ranges::equal(*nested, expected_) && before->data() != after->data() &&
                after->data() == nested->data();
    return SqlValue::Integer(1);
  }

 private:
  static inline Vm* vm_ = nullptr;
  static inline ByteView expected_{};
  static inline bool nested_rejected_ = false;
  static inline bool observed_ = false;
};

TEST(VmColumn, InvalidatesSharedValuesAfterWritesToAnUnrelatedTable) {
  constexpr std::size_t kBytes = 5'001U;
  for (const bool mutation_reader : {false, true}) {
    SCOPED_TRACE(mutation_reader);
    test::WritePagerFixedVfs vfs{false};
    PageNumber source_root;
    PageNumber destination_root;
    ByteBuffer bytes{ByteCount{kBytes}};
    std::ranges::fill(bytes.mutable_view(), std::byte{0x5a});
    {
      auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
      ASSERT_NE(nullptr, database);
      RequireStatus(database->pager().BeginRead());
      RequireStatus(database->pager().BeginWrite());
      {
        auto session = TakeValue(BtreeWriteSession::Open(*database));
        RequireStatus(session.InitializeDatabase());
        auto source = TakeValue(session.CreateTableBtree());
        auto destination = TakeValue(session.CreateTableBtree());
        source_root = source.root_page();
        destination_root = destination.root_page();
        const auto record = TakeValue(EncodeRecord(std::array{SqlValue::Blob(bytes.Clone())}));
        RequireStatus(source.Insert(1, record.view()));
        const auto initial = TakeValue(EncodeRecord(std::array{SqlValue::Integer(5)}));
        RequireStatus(destination.Insert(1, initial.view()));
      }
      RequireStatus(database->pager().Commit());
      RequireStatus(database->pager().EndRead());
    }
    auto coordinator = OpenWriteCoordinator(vfs);
    auto statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    ProgramInput input;
    input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
    input.statement_kind = ProgramStatementKind::kUpdate;
    input.transaction_access = ProgramTransactionAccess::kWrite;
    input.mutation_result.publishes_changes = true;
    input.register_count = 5;
    input.constants.push_back(SqlValue::Integer(1));
    input.constants.push_back(
        SqlValue::Blob(TakeValue(EncodeRecord(std::array{SqlValue::Integer(7)}))));
    auto source = TableDescriptor(
        {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0}}, 1);
    source.root_page = RootPageNumber(source_root.value());
    input.cursors.push_back(std::move(source));
    input.write_cursors.push_back(WriteCursorDescriptor{
        .root_page = RootPageNumber(destination_root.value()),
        .columns = {WriteColumnDescriptor{}},
        .rowid_alias = std::nullopt,
        .index_columns = {},
    });
    input.symbols = {"nested_column_step", "observe_column_cache", "BINARY"};
    input.instructions = {
        LoadConstantInstruction{.constant = Constant(0), .output = Reg(2)},
        LoadConstantInstruction{.constant = Constant(1), .output = Reg(3)},
        mutation_reader ? Instruction{OpenMutationCursorInstruction{.cursor = Cursor(0)}}
                        : Instruction{OpenReadCursorInstruction{.cursor = Cursor(0)}},
        RewindInstruction{.cursor = Cursor(0), .empty_target = Address(14)},
        ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
        OpenWriteCursorInstruction{.cursor = WriteCursor(0)},
        UpdateTableInstruction{
            .cursor = WriteCursor(0), .old_rowid = Reg(2), .new_rowid = Reg(2), .record = Reg(3)},
        CloseWriteCursorInstruction{.cursor = WriteCursor(0)},
        ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(1)},
        CallScalarInstruction{.function = Symbol(0),
                              .collation = Symbol(2),
                              .first_argument = Reg(0),
                              .argument_count = 0,
                              .output = Reg(4)},
        ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(2)},
        CallScalarInstruction{.function = Symbol(1),
                              .collation = Symbol(2),
                              .first_argument = Reg(0),
                              .argument_count = 3,
                              .output = Reg(4)},
        CloseCursorInstruction{.cursor = Cursor(0)},
        HaltInstruction{},
        CloseCursorInstruction{.cursor = Cursor(0)},
        HaltInstruction{},
    };
    const auto program = TakeProgramValue(BytecodeProgram::Create(input));
    const std::array functions{
        ScalarFunction{"nested_column_step", FunctionArity::Exact(0),
                       FunctionDeterminism::kDeterministic, FunctionCollationUse::kNone,
                       ColumnWriteCacheProbe::Nested},
        ScalarFunction{"observe_column_cache", FunctionArity::Exact(3),
                       FunctionDeterminism::kDeterministic, FunctionCollationUse::kNone,
                       ColumnWriteCacheProbe::Observe},
    };
    const FunctionRegistry registry{functions};
    const std::array<const Collation*, 1> collations{&BinaryCollation()};
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment{registry, collations}));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
    const ColumnWriteCacheProbe probe{vm, bytes.view()};
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_TRUE(probe.valid());
    EXPECT_EQ(1U, vm.change_count());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Succeed());
  }
}

TEST(VmColumn, KeepsDifferentSharedColumnsAliveAfterTheirCursorCloses) {
  constexpr std::size_t kBytes = 5'001U;
  test::WritePagerFixedVfs vfs{false};
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  ASSERT_NE(nullptr, database);
  Pager& pager = database->pager();
  RequireStatus(pager.BeginRead());
  RequireStatus(pager.BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*database));
  RequireStatus(session.InitializeDatabase());
  auto table = TakeValue(session.CreateTableBtree());
  ByteBuffer first{ByteCount{kBytes}};
  ByteBuffer second{ByteCount{kBytes}};
  std::ranges::fill(first.mutable_view(), std::byte{0x11});
  std::ranges::fill(second.mutable_view(), std::byte{0x22});
  const auto record = TakeValue(
      EncodeRecord(std::array{SqlValue::Blob(first.Clone()), SqlValue::Blob(second.Clone())}));
  RequireStatus(table.Insert(1, record.view()));
  const auto pins = pager.cache_pin_count();
  auto descriptor = TableDescriptor(
      {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0},
       CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 1}},
      2);
  descriptor.root_page = RootPageNumber(table.root_page().value());
  const auto program = BuildProgram(
      pager, 2, 0, {}, {}, {descriptor}, {ResultColumn("first"), ResultColumn("second")},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(7)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
          CloseCursorInstruction{.cursor = Cursor(0)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          HaltInstruction{},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
      });
  Vm vm = CreateAttachedVm(program, *database, kCatalogGeneration);
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  const auto row = vm.row();
  ASSERT_EQ(2U, row.size());
  ASSERT_TRUE(row[0].blob_value().has_value());
  ASSERT_TRUE(row[1].blob_value().has_value());
  EXPECT_TRUE(std::ranges::equal(first.view(), *row[0].blob_value()));
  EXPECT_TRUE(std::ranges::equal(second.view(), *row[1].blob_value()));
  EXPECT_NE(row[0].blob_value()->data(), row[1].blob_value()->data());
  EXPECT_EQ(pins, pager.cache_pin_count());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(pager.Rollback());
}

TEST_F(VmTest, ScansIndexRecordsWithResolvedOrderingMetadata) {
  const BytecodeProgram program = BuildProgram(
      *pager_, 2, 0, {}, {"BINARY"}, {BinaryIndexDescriptor()},
      {ResultColumn("binary_key"), ResultColumn("id")},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(6)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          NextInstruction{.cursor = Cursor(0), .next_target = Address(2)},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
      });
  Vm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ASSERT_EQ(vm.row().size(), 2U);
  ExpectText(vm.row()[0], "bin-00");
  ExpectInteger(vm.row()[1], 13);
}

TEST_F(VmTest, ScansIndexRecordsInReverse) {
  const BytecodeProgram program = BuildProgram(
      *pager_, 2, 0, {}, {"BINARY"}, {BinaryIndexDescriptor()},
      {ResultColumn("binary_key"), ResultColumn("id")},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          LastInstruction{.cursor = Cursor(0), .empty_target = Address(6)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          PreviousInstruction{.cursor = Cursor(0), .previous_target = Address(2)},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
      });
  Vm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ASSERT_EQ(vm.row().size(), 2U);
  ExpectText(vm.row()[0], "bin-zero");
  ExpectInteger(vm.row()[1], 0);
}

TEST_F(VmTest, SeeksAndBoundsDuplicateIndexPrefixes) {
  const auto build = [&](std::string_view key, IndexRangeEndMode end_mode) {
    std::vector<SqlValue> constants;
    constants.push_back(SqlValue::Text(std::string(key)));
    return BuildProgram(
        *pager_, 3, 0, std::move(constants), {"BINARY"}, {BinaryIndexDescriptor()},
        {ResultColumn("binary_key"), ResultColumn("id")},
        {
            LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
            OpenReadCursorInstruction{.cursor = Cursor(0)},
            SeekIndexInstruction{
                .cursor = Cursor(0),
                .first_key = Reg(0),
                .key_count = 1,
                .missing_target = Address(8),
                .mode = IndexSeekMode::kGreaterOrEqual,
            },
            CheckIndexRangeInstruction{
                .cursor = Cursor(0),
                .first_key = Reg(0),
                .key_count = 1,
                .end_target = Address(10),
                .mode = end_mode,
            },
            ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(1)},
            ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(2)},
            ResultRowInstruction{.first = Reg(1), .count = 2},
            NextInstruction{.cursor = Cursor(0), .next_target = Address(3)},
            CloseCursorInstruction{.cursor = Cursor(0)},
            HaltInstruction{},
            CloseCursorInstruction{.cursor = Cursor(0)},
            HaltInstruction{},
        });
  };

  {
    const BytecodeProgram inclusive = build("bin-05", IndexRangeEndMode::kInclusive);
    Vm vm = CreateCoreVm(inclusive);
    constexpr std::array<std::int64_t, 12> kExpectedRowIds{
        5, 18, 31, 44, 57, 70, 83, 96, 109, 122, 135, 148,
    };
    for (const std::int64_t expected_rowid : kExpectedRowIds) {
      ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
      ASSERT_EQ(2U, vm.row().size());
      ExpectText(vm.row()[0], "bin-05");
      ExpectInteger(vm.row()[1], expected_rowid);
    }
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  }

  {
    const BytecodeProgram exclusive = build("bin-05", IndexRangeEndMode::kExclusive);
    Vm vm = CreateCoreVm(exclusive);
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  }

  {
    const BytecodeProgram missing = build("zzz", IndexRangeEndMode::kInclusive);
    Vm vm = CreateCoreVm(missing);
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  }

  const auto build_reverse = [&](IndexRangeEndMode end_mode) {
    std::vector<SqlValue> constants;
    constants.push_back(SqlValue::Text("bin-05"));
    return BuildProgram(
        *pager_, 3, 0, std::move(constants), {"BINARY"}, {BinaryIndexDescriptor()},
        {ResultColumn("binary_key"), ResultColumn("id")},
        {
            LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
            OpenReadCursorInstruction{.cursor = Cursor(0)},
            SeekIndexInstruction{
                .cursor = Cursor(0),
                .first_key = Reg(0),
                .key_count = 1,
                .missing_target = Address(8),
                .mode = IndexSeekMode::kLessOrEqual,
            },
            CheckIndexRangeInstruction{
                .cursor = Cursor(0),
                .first_key = Reg(0),
                .key_count = 1,
                .end_target = Address(10),
                .mode = end_mode,
            },
            ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(1)},
            ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(2)},
            ResultRowInstruction{.first = Reg(1), .count = 2},
            PreviousInstruction{.cursor = Cursor(0), .previous_target = Address(3)},
            CloseCursorInstruction{.cursor = Cursor(0)},
            HaltInstruction{},
            CloseCursorInstruction{.cursor = Cursor(0)},
            HaltInstruction{},
        });
  };

  {
    const BytecodeProgram inclusive = build_reverse(IndexRangeEndMode::kReverseInclusive);
    Vm vm = CreateCoreVm(inclusive);
    constexpr std::array<std::int64_t, 12> kExpectedRowIds{
        148, 135, 122, 109, 96, 83, 70, 57, 44, 31, 18, 5,
    };
    for (const std::int64_t expected_rowid : kExpectedRowIds) {
      ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
      ASSERT_EQ(2U, vm.row().size());
      ExpectText(vm.row()[0], "bin-05");
      ExpectInteger(vm.row()[1], expected_rowid);
    }
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  }

  {
    const BytecodeProgram exclusive = build_reverse(IndexRangeEndMode::kReverseExclusive);
    Vm vm = CreateCoreVm(exclusive);
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  }
}

TEST_F(VmTest, ReadsIndexRowidsAndRequiresMatchingTableRows) {
  const BytecodeProgram lookup = BuildProgram(
      *pager_, 2, 0, {}, {"BINARY"},
      {
          BinaryIndexDescriptor(),
          TableDescriptor({
              CursorFieldSource{
                  .kind = CursorFieldSourceKind::kRowId,
                  .record_field = 0,
              },
          }),
      },
      {ResultColumn("index_rowid"), ResultColumn("table_rowid")},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(8)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(0)},
          OpenReadCursorInstruction{.cursor = Cursor(1)},
          SeekTableRowIdInstruction{.cursor = Cursor(1), .key = Reg(0)},
          ReadRowIdInstruction{.cursor = Cursor(1), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          HaltInstruction{},
          HaltInstruction{},
      });
  Vm lookup_vm = CreateCoreVm(lookup);
  ASSERT_EQ(VmStep::kRow, TakeValue(lookup_vm.Step()));
  ASSERT_EQ(2U, lookup_vm.row().size());
  ExpectInteger(lookup_vm.row()[0], 13);
  ExpectInteger(lookup_vm.row()[1], 13);
  EXPECT_EQ(VmStep::kDone, TakeValue(lookup_vm.Step()));

  const auto failing_lookup = [&](SqlValue key) {
    std::vector<SqlValue> constants;
    constants.push_back(std::move(key));
    return BuildProgram(*pager_, 1, 0, std::move(constants), {},
                        {
                            TableDescriptor({
                                CursorFieldSource{
                                    .kind = CursorFieldSourceKind::kRowId,
                                    .record_field = 0,
                                },
                            }),
                        },
                        {},
                        {
                            LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                            OpenReadCursorInstruction{.cursor = Cursor(0)},
                            SeekTableRowIdInstruction{.cursor = Cursor(0), .key = Reg(0)},
                            HaltInstruction{},
                        });
  };

  const BytecodeProgram invalid_type_program = failing_lookup(SqlValue::Text("42"));
  Vm invalid_type = CreateCoreVm(invalid_type_program);
  const auto invalid_type_step = invalid_type.Step();
  ASSERT_FALSE(invalid_type_step.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, invalid_type_step.error().code());

  const BytecodeProgram missing_program =
      failing_lookup(SqlValue::Integer(std::numeric_limits<std::int64_t>::max() - 1));
  Vm missing = CreateCoreVm(missing_program);
  const auto missing_step = missing.Step();
  ASSERT_FALSE(missing_step.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, missing_step.error().code());
}

TEST_F(VmTest, ReadsRowidsThroughDedicatedAndDescriptorInstructions) {
  const BytecodeProgram program = BuildProgram(
      *pager_, 2, 0, {}, {},
      {
          TableDescriptor({
              CursorFieldSource{
                  .kind = CursorFieldSourceKind::kRowId,
                  .record_field = 0,
              },
          }),
      },
      {ResultColumn("mapped_rowid"), ResultColumn("direct_rowid")},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(6)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          HaltInstruction{},
          HaltInstruction{},
      });
  Vm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ASSERT_EQ(vm.row().size(), 2U);
  ExpectInteger(vm.row()[0], std::numeric_limits<std::int64_t>::min());
  ExpectInteger(vm.row()[1], std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(TakeValue(vm.Step()), VmStep::kDone);
}

enum class IndexPeerMutation : std::uint8_t {
  Erase,
  MoveKey,
  ReplaceSameKey,
};

class IndexPeerMutationProbe final {
 public:
  IndexPeerMutationProbe(TableBtreeWriter& table, IndexBtreeWriter& index,
                         IndexPeerMutation mutation) noexcept {
    table_ = &table;
    index_ = &index;
    mutation_ = mutation;
    calls_ = 0U;
  }
  IndexPeerMutationProbe(const IndexPeerMutationProbe&) = delete;
  IndexPeerMutationProbe& operator=(const IndexPeerMutationProbe&) = delete;
  ~IndexPeerMutationProbe() {
    table_ = nullptr;
    index_ = nullptr;
  }

  [[nodiscard]] static std::size_t calls() noexcept { return calls_; }

  [[nodiscard]] static Result<SqlValue> Invoke(const ScalarFunctionContext&,
                                               std::span<const SqlValue>) {
    if (table_ == nullptr || index_ == nullptr) {
      return std::unexpected(Error::Create(ErrorCode::kInternal, "index peer probe is not active"));
    }
    if (calls_++ != 0U) {
      return SqlValue::Integer(0);
    }
    auto removed = index_->Delete(std::array{SqlValue::Integer(2), SqlValue::Integer(1)});
    if (!removed.has_value()) {
      return std::unexpected(std::move(removed.error()));
    }
    if (!*removed) {
      return std::unexpected(Error::Create(ErrorCode::kNotFound, "index peer key is absent"));
    }
    if (mutation_ == IndexPeerMutation::Erase) {
      auto erased = table_->Delete(1);
      if (!erased.has_value()) {
        return std::unexpected(std::move(erased.error()));
      }
      if (!*erased) {
        return std::unexpected(
            Error::Create(ErrorCode::kNotFound, "index peer table row is absent"));
      }
    } else {
      const std::int64_t key = mutation_ == IndexPeerMutation::MoveKey ? 3 : 2;
      auto record =
          EncodeRecord(std::array{SqlValue{}, SqlValue::Integer(key), SqlValue::Integer(20)});
      if (!record.has_value()) {
        return std::unexpected(std::move(record.error()));
      }
      auto replaced = table_->Insert(1, record->view(), BtreeInsertMode::kReplace);
      if (!replaced.has_value()) {
        return std::unexpected(std::move(replaced.error()));
      }
      auto inserted = index_->Insert(std::array{SqlValue::Integer(key), SqlValue::Integer(1)});
      if (!inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
    }
    return SqlValue::Integer(1);
  }

 private:
  static inline TableBtreeWriter* table_ = nullptr;
  static inline IndexBtreeWriter* index_ = nullptr;
  static inline IndexPeerMutation mutation_ = IndexPeerMutation::Erase;
  static inline std::size_t calls_ = 0U;
};

TEST(VmIndexPeer, MatchesNativeCallbackRecordAndRowidTraces) {
  using Row = std::array<std::optional<std::int64_t>, 4>;
  struct Case {
    IndexPeerMutation mutation;
    bool covering;
    std::vector<Row> rows;
  };
  const std::array cases{
      Case{.mutation = IndexPeerMutation::Erase,
           .covering = true,
           .rows = {{2, 1, std::nullopt, std::nullopt}, {4, 0, 4, 2}, {6, 0, 6, 3}}},
      Case{.mutation = IndexPeerMutation::MoveKey,
           .covering = true,
           .rows = {{2, 1, std::nullopt, std::nullopt}, {3, 0, 3, 1}, {4, 0, 4, 2}, {6, 0, 6, 3}}},
      Case{.mutation = IndexPeerMutation::ReplaceSameKey,
           .covering = true,
           .rows = {{2, 1, 2, 1}, {4, 0, 4, 2}, {6, 0, 6, 3}}},
      Case{.mutation = IndexPeerMutation::Erase,
           .covering = false,
           .rows = {{20, 1, std::nullopt, std::nullopt}, {40, 0, 40, 2}, {60, 0, 60, 3}}},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.covering);
    SCOPED_TRACE(static_cast<std::uint8_t>(item.mutation));
    test::WritePagerFixedVfs vfs{false};
    auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    ASSERT_NE(nullptr, database);
    Pager& pager = database->pager();
    RequireStatus(pager.BeginRead());
    RequireStatus(pager.BeginWrite());
    auto session = TakeValue(BtreeWriteSession::Open(*database));
    RequireStatus(session.InitializeDatabase());
    auto table = TakeValue(session.CreateTableBtree());
    const std::array orders{IndexColumnOrder{}, IndexColumnOrder{}};
    auto index =
        TakeValue(session.CreateIndexBtree(TakeValue(IndexComparisonInfo::CopyOf(orders))));
    for (std::int64_t rowid = 1; rowid <= 3; ++rowid) {
      const auto record = TakeValue(EncodeRecord(
          std::array{SqlValue{}, SqlValue::Integer(rowid * 2), SqlValue::Integer(rowid * 20)}));
      RequireStatus(table.Insert(rowid, record.view()));
      RequireStatus(
          index.Insert(std::array{SqlValue::Integer(rowid * 2), SqlValue::Integer(rowid)}));
    }
    const std::array functions{
        ScalarFunction{"mutate_index_peer", FunctionArity::Exact(0),
                       FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                       IndexPeerMutationProbe::Invoke},
    };
    const FunctionRegistry registry{functions};
    const std::array<const Collation*, 1> collations{&BinaryCollation()};
    std::vector<ReadCursorDescriptor> cursors{
        ReadCursorDescriptor{
            .root_page = RootPageNumber(index.root_page().value()),
            .storage = CursorStorageKind::kIndex,
            .record_field_count = 2,
            .fields =
                {
                    CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField,
                                      .record_field = 0},
                    CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField,
                                      .record_field = 1},
                },
            .index_columns =
                {
                    IndexColumnMetadata{.collation = Symbol(1),
                                        .order = BytecodeSortOrder::kAscending},
                    IndexColumnMetadata{.collation = Symbol(1),
                                        .order = BytecodeSortOrder::kAscending},
                },
            .index_key_field_count = 2,
        },
    };
    std::vector<Instruction> instructions{OpenReadCursorInstruction{.cursor = Cursor(0)}};
    if (!item.covering) {
      auto descriptor = TableDescriptor(
          {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 2}}, 3);
      descriptor.root_page = RootPageNumber(table.root_page().value());
      cursors.push_back(std::move(descriptor));
      instructions.emplace_back(OpenReadCursorInstruction{.cursor = Cursor(1)});
    }
    const auto rewind_position = static_cast<std::uint32_t>(instructions.size());
    instructions.emplace_back(RewindInstruction{.cursor = Cursor(0), .empty_target = Address(0)});
    const auto loop = static_cast<std::uint32_t>(instructions.size());
    if (!item.covering) {
      instructions.emplace_back(
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(4)});
      instructions.emplace_back(SeekTableRowIdInstruction{.cursor = Cursor(1), .key = Reg(4)});
    }
    const CursorId value_cursor = item.covering ? Cursor(0) : Cursor(1);
    instructions.emplace_back(
        ReadFieldInstruction{.cursor = value_cursor, .field = Field(0), .output = Reg(0)});
    instructions.emplace_back(CallScalarInstruction{.function = Symbol(0),
                                                    .collation = Symbol(1),
                                                    .first_argument = Reg(0),
                                                    .argument_count = 0,
                                                    .output = Reg(1)});
    instructions.emplace_back(
        ReadFieldInstruction{.cursor = value_cursor, .field = Field(0), .output = Reg(2)});
    instructions.emplace_back(
        ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(3)});
    instructions.emplace_back(ResultRowInstruction{.first = Reg(0), .count = 4});
    if (!item.covering) {
      instructions.emplace_back(NullRowInstruction{.cursor = Cursor(1), .enabled = false});
    }
    instructions.emplace_back(NextInstruction{.cursor = Cursor(0), .next_target = Address(loop)});
    const auto end = static_cast<std::uint32_t>(instructions.size());
    instructions.emplace_back(HaltInstruction{});
    std::get<RewindInstruction>(instructions[rewind_position]).empty_target = Address(end);
    const auto program =
        BuildProgram(pager, 5, 0, {}, {"mutate_index_peer", "BINARY"}, std::move(cursors),
                     {ResultColumn("before"), ResultColumn("mutated"), ResultColumn("after"),
                      ResultColumn("rowid")},
                     std::move(instructions));
    Vm vm = CreateAttachedVm(program, *database, kCatalogGeneration,
                             VmEnvironment{registry, collations});
    const IndexPeerMutationProbe probe{table, index, item.mutation};
    for (const auto& expected : item.rows) {
      ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
      ASSERT_EQ(expected.size(), vm.row().size());
      for (std::size_t field = 0; field < expected.size(); ++field) {
        if (expected[field].has_value()) {
          ExpectInteger(vm.row()[field], *expected[field]);
        } else {
          EXPECT_EQ(SqlValueType::kNull, vm.row()[field].type());
        }
      }
    }
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(item.rows.size(), IndexPeerMutationProbe::calls());
    RequireStatus(pager.Rollback());
  }
}

class CursorTripProbe final {
 public:
  explicit CursorTripProbe(BtreeDatabase& database) noexcept { database_ = &database; }
  CursorTripProbe(const CursorTripProbe&) = delete;
  CursorTripProbe& operator=(const CursorTripProbe&) = delete;
  ~CursorTripProbe() { database_ = nullptr; }

  [[nodiscard]] static Result<SqlValue> Invoke(const ScalarFunctionContext&,
                                               std::span<const SqlValue>) {
    if (database_ == nullptr) {
      return std::unexpected(
          Error::Create(ErrorCode::kInternal, "cursor trip probe is not active"));
    }
    auto tripped = database_->TripCursors(BtreeCursorTripMode::All);
    if (!tripped.has_value()) {
      return std::unexpected(std::move(tripped.error()));
    }
    return SqlValue::Integer(1);
  }

 private:
  static inline BtreeDatabase* database_ = nullptr;
};

TEST(VmCursorTrip, PropagatesNativeAbortBeforeReusingCachedFieldsOrRowids) {
  for (const bool index_cursor : {false, true}) {
    for (const bool rowid : {false, true}) {
      SCOPED_TRACE(index_cursor);
      SCOPED_TRACE(rowid);
      test::WritePagerFixedVfs vfs{false};
      auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
      ASSERT_NE(nullptr, database);
      Pager& pager = database->pager();
      RequireStatus(pager.BeginRead());
      RequireStatus(pager.BeginWrite());
      auto session = TakeValue(BtreeWriteSession::Open(*database));
      RequireStatus(session.InitializeDatabase());
      auto table = TakeValue(session.CreateTableBtree());
      const auto record = TakeValue(EncodeRecord(std::array{SqlValue::Text("original")}));
      RequireStatus(table.Insert(1, record.view()));
      const std::array columns{IndexColumnOrder{}, IndexColumnOrder{}};
      auto index =
          TakeValue(session.CreateIndexBtree(TakeValue(IndexComparisonInfo::CopyOf(columns))));
      RequireStatus(index.Insert(std::array{SqlValue::Text("first"), SqlValue::Integer(1)}));
      const std::array functions{
          ScalarFunction{"trip_cursors", FunctionArity::Exact(0),
                         FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                         CursorTripProbe::Invoke},
      };
      const FunctionRegistry registry{functions};
      const std::array<const Collation*, 1> collations{&BinaryCollation()};
      auto descriptor = TableDescriptor(
          {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0}}, 1);
      descriptor.root_page = RootPageNumber(table.root_page().value());
      if (index_cursor) {
        descriptor.root_page = RootPageNumber(index.root_page().value());
        descriptor.storage = CursorStorageKind::kIndex;
        descriptor.record_field_count = 2;
        descriptor.fields.front().record_field = rowid ? 1U : 0U;
        descriptor.index_columns = {
            IndexColumnMetadata{.collation = Symbol(1), .order = BytecodeSortOrder::kAscending},
            IndexColumnMetadata{.collation = Symbol(1), .order = BytecodeSortOrder::kAscending},
        };
        descriptor.index_key_field_count = 2;
      }
      std::vector<Instruction> instructions{
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(7)},
      };
      const auto emit_read = [&](RegisterId output) {
        if (rowid && !index_cursor) {
          instructions.emplace_back(ReadRowIdInstruction{.cursor = Cursor(0), .output = output});
        } else {
          instructions.emplace_back(
              ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = output});
        }
      };
      emit_read(Reg(0));
      instructions.emplace_back(CallScalarInstruction{.function = Symbol(0),
                                                      .collation = Symbol(1),
                                                      .first_argument = Reg(0),
                                                      .argument_count = 0,
                                                      .output = Reg(1)});
      emit_read(Reg(2));
      instructions.emplace_back(ResultRowInstruction{.first = Reg(0), .count = 3});
      instructions.emplace_back(NextInstruction{.cursor = Cursor(0), .next_target = Address(2)});
      instructions.emplace_back(HaltInstruction{});
      const auto program =
          BuildProgram(pager, 3, 0, {}, {"trip_cursors", "BINARY"}, {std::move(descriptor)},
                       {ResultColumn("before"), ResultColumn("tripped"), ResultColumn("after")},
                       std::move(instructions));
      Vm vm = CreateAttachedVm(program, *database, kCatalogGeneration,
                               VmEnvironment{registry, collations});
      const CursorTripProbe probe{*database};

      const auto fault = vm.Step();

      ASSERT_FALSE(fault.has_value());
      EXPECT_EQ(ErrorCode::kAborted, fault.error().code());
      EXPECT_EQ(kSqliteAbortRollback, fault.error().sqlite_code());
      EXPECT_TRUE(vm.row().empty());
      EXPECT_EQ(0U, pager.cache_pin_count());
      RequireStatus(pager.Rollback());
    }
  }
}

class TablePeerMutationProbe final {
 public:
  TablePeerMutationProbe(TableBtreeWriter& table, ByteView replacement,
                         bool erase = false) noexcept {
    table_ = &table;
    replacement_ = replacement;
    erase_ = erase;
  }
  TablePeerMutationProbe(const TablePeerMutationProbe&) = delete;
  TablePeerMutationProbe& operator=(const TablePeerMutationProbe&) = delete;
  ~TablePeerMutationProbe() { table_ = nullptr; }

  [[nodiscard]] static Result<SqlValue> Invoke(const ScalarFunctionContext&,
                                               std::span<const SqlValue>) {
    if (table_ == nullptr) {
      return std::unexpected(Error::Create(ErrorCode::kInternal, "table peer probe is not active"));
    }
    if (erase_) {
      auto removed = table_->Delete(1);
      if (!removed.has_value()) {
        return std::unexpected(std::move(removed.error()));
      }
      if (!*removed) {
        return std::unexpected(
            Error::Create(ErrorCode::kNotFound, "table peer probe row is absent"));
      }
    } else {
      auto replaced = table_->Insert(1, replacement_, BtreeInsertMode::kReplace);
      if (!replaced.has_value()) {
        return std::unexpected(std::move(replaced.error()));
      }
    }
    return SqlValue::Integer(1);
  }

 private:
  static inline TableBtreeWriter* table_ = nullptr;
  static inline ByteView replacement_{};
  static inline bool erase_ = false;
};

void ExpectPeerSavedCurrentMutation(bool erase_current) {
  for (const bool erase_peer : {false, true}) {
    SCOPED_TRACE(erase_peer);
    test::WritePagerFixedVfs vfs{false};
    static_cast<void>(InitializedWriteDatabase(vfs));
    InsertDirectWriteRow(vfs, 1, "original");
    InsertDirectWriteRow(vfs, 3, "other");
    const ByteBuffer peer = TakeValue(EncodeRecord(std::array{SqlValue{}, SqlValue::Text("peer")}));
    const ByteBuffer final =
        TakeValue(EncodeRecord(std::array{SqlValue{}, SqlValue::Text("final")}));
    {
      TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
      TransactionStatement statement =
          TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
              .access = StatementAccess::kWrite, .rollback = StatementRollbackMode::kStatement}));
      TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
      const std::array functions{
          ScalarFunction{"mutate_peer", FunctionArity::Exact(0),
                         FunctionDeterminism::kDeterministic, FunctionCollationUse::kNone,
                         TablePeerMutationProbe::Invoke},
      };
      const FunctionRegistry registry{functions};
      const std::array<const Collation*, 1> collations{&BinaryCollation()};
      ProgramInput input;
      input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
      input.statement_kind =
          erase_current ? ProgramStatementKind::kDelete : ProgramStatementKind::kUpdate;
      input.transaction_access = ProgramTransactionAccess::kWrite;
      input.rollback_mode = ProgramRollbackMode::kStatement;
      input.mutation_result.publishes_changes = true;
      input.register_count = 2;
      input.constants.push_back(SqlValue::Blob(ByteBuffer::CopyOf(final.view())));
      input.symbols = {"mutate_peer", "BINARY"};
      input.cursors.push_back(ReadCursorDescriptor{
          .root_page = RootPageNumber(1),
          .storage = CursorStorageKind::kRowIdTable,
          .record_field_count = 2,
          .fields = {},
          .index_columns = {},
      });
      input.instructions = {
          LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
          OpenMutationCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(7)},
          CallScalarInstruction{.function = Symbol(0),
                                .collation = Symbol(1),
                                .first_argument = Reg(0),
                                .argument_count = 0,
                                .output = Reg(1)},
          erase_current
              ? Instruction{DeleteCurrentTableInstruction{.cursor = Cursor(0),
                                                          .exhausted_target = Address(7)}}
              : Instruction{UpdateCurrentTableInstruction{.cursor = Cursor(0), .record = Reg(0)}},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
          CloseCursorInstruction{.cursor = Cursor(0)},
          HaltInstruction{},
      };
      const BytecodeProgram program = TakeProgramValue(BytecodeProgram::Create(input));
      Vm vm = TakeValue(Vm::Create(program, VmEnvironment{registry, collations}));
      RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 0}));
      const TablePeerMutationProbe probe{table, peer.view(), erase_peer};
      const auto mutated = vm.Step();
      if (erase_peer) {
        ASSERT_FALSE(mutated.has_value());
        EXPECT_EQ(ErrorCode::kNotFound, mutated.error().code());
        EXPECT_EQ(0U, vm.change_count());
        RequireStatus(vm.DetachExecutionContext());
        RequireStatus(statement.Rollback());
      } else {
        ASSERT_TRUE(mutated.has_value()) << mutated.error().ToString();
        EXPECT_EQ(VmStep::kDone, *mutated);
        EXPECT_EQ(1U, vm.change_count());
        RequireStatus(vm.DetachExecutionContext());
        RequireStatus(statement.Succeed());
      }
    }
    const auto rows = ReadWriteTableRows(vfs);
    if (erase_current && !erase_peer) {
      ASSERT_EQ(1U, rows.size());
      EXPECT_EQ(3, rows[0].first);
    } else {
      ASSERT_EQ(2U, rows.size());
      EXPECT_EQ(1, rows[0].first);
      ExpectText(rows[0].second[1], erase_peer ? "original" : "final");
      EXPECT_EQ(3, rows[1].first);
    }
    ExpectText(rows.back().second[1], "other");
  }
}

TEST(VmColumn, RetiresSharedBytesWhenAPeerRestoresTheSamePhysicalRow) {
  constexpr std::size_t kBytes = 5'001U;
  test::WritePagerFixedVfs vfs{false};
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  ASSERT_NE(nullptr, database);
  Pager& pager = database->pager();
  RequireStatus(pager.BeginRead());
  RequireStatus(pager.BeginWrite());
  auto session = TakeValue(BtreeWriteSession::Open(*database));
  RequireStatus(session.InitializeDatabase());
  auto table = TakeValue(session.CreateTableBtree());
  ByteBuffer before{ByteCount{kBytes}};
  ByteBuffer after{ByteCount{kBytes}};
  std::ranges::fill(before.mutable_view(), std::byte{0x11});
  std::ranges::fill(after.mutable_view(), std::byte{0x22});
  const auto original = TakeValue(EncodeRecord(std::array{SqlValue::Blob(before.Clone())}));
  const auto replacement = TakeValue(EncodeRecord(std::array{SqlValue::Blob(after.Clone())}));
  RequireStatus(table.Insert(1, original.view()));
  const std::array functions{
      ScalarFunction{"mutate_peer", FunctionArity::Exact(0), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, TablePeerMutationProbe::Invoke},
  };
  const FunctionRegistry registry{functions};
  const std::array<const Collation*, 1> collations{&BinaryCollation()};
  auto descriptor = TableDescriptor(
      {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0}}, 1);
  descriptor.root_page = RootPageNumber(table.root_page().value());
  const auto program = BuildProgram(
      pager, 3, 0, {}, {"mutate_peer", "BINARY"}, {descriptor},
      {ResultColumn("before"), ResultColumn("after")},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(7)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          CallScalarInstruction{.function = Symbol(0),
                                .collation = Symbol(1),
                                .first_argument = Reg(0),
                                .argument_count = 0,
                                .output = Reg(2)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          HaltInstruction{},
          HaltInstruction{},
      });
  Vm vm =
      CreateAttachedVm(program, *database, kCatalogGeneration, VmEnvironment{registry, collations});
  const TablePeerMutationProbe probe{table, replacement.view()};
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  const auto row = vm.row();
  ASSERT_EQ(2U, row.size());
  ASSERT_TRUE(row[0].blob_value().has_value());
  ASSERT_TRUE(row[1].blob_value().has_value());
  EXPECT_TRUE(std::ranges::equal(before.view(), *row[0].blob_value()));
  EXPECT_TRUE(std::ranges::equal(after.view(), *row[1].blob_value()));
  EXPECT_NE(row[0].blob_value()->data(), row[1].blob_value()->data());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(pager.Rollback());
}

TEST(VmTablePeer, RestoresSavedWriterBeforeCurrentUpdate) { ExpectPeerSavedCurrentMutation(false); }

TEST(VmTablePeer, RestoresSavedWriterBeforeCurrentDelete) { ExpectPeerSavedCurrentMutation(true); }

TEST(VmTablePeer, InvalidatesDecodedFieldsAfterARealCallbackReplacesTheCurrentRow) {
  test::WritePagerFixedVfs vfs{false};
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  ASSERT_NE(nullptr, database);
  Pager& pager = database->pager();
  RequireStatus(pager.BeginRead());
  RequireStatus(pager.BeginWrite());
  BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*database));
  RequireStatus(session.InitializeDatabase());
  TableBtreeWriter table = TakeValue(session.CreateTableBtree());
  const ByteBuffer original =
      TakeValue(EncodeRecord(std::array{SqlValue::Integer(10), SqlValue::Integer(20)}));
  const ByteBuffer replacement =
      TakeValue(EncodeRecord(std::array{SqlValue::Integer(100), SqlValue::Integer(200)}));
  RequireStatus(table.Insert(1, original.view()));
  const std::array functions{
      ScalarFunction{"mutate_peer", FunctionArity::Exact(0), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, TablePeerMutationProbe::Invoke},
  };
  const FunctionRegistry registry{functions};
  const std::array<const Collation*, 1> collations{&BinaryCollation()};
  const BytecodeProgram program = BuildProgram(
      pager, 3, 0, {}, {"mutate_peer", "BINARY"},
      {TableDescriptor(
          {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0},
           CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 1}},
          2)},
      {ResultColumn("before"), ResultColumn("after")},
      {
          OpenReadCursorInstruction{.cursor = Cursor(0)},
          RewindInstruction{.cursor = Cursor(0), .empty_target = Address(7)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
          CallScalarInstruction{.function = Symbol(0),
                                .collation = Symbol(1),
                                .first_argument = Reg(0),
                                .argument_count = 0,
                                .output = Reg(2)},
          ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
          ResultRowInstruction{.first = Reg(0), .count = 2},
          HaltInstruction{},
          HaltInstruction{},
      });
  Vm vm =
      CreateAttachedVm(program, *database, kCatalogGeneration, VmEnvironment{registry, collations});
  const TablePeerMutationProbe probe{table, replacement.view()};
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_EQ(2U, vm.row().size());
  ExpectInteger(vm.row()[0], 10);
  ExpectInteger(vm.row()[1], 200);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(pager.Rollback());
}

TEST(VmTablePeer, PublishesDeletedOriginalNullsThenResumesAtTheSuccessor) {
  const std::array<Instruction, 3> moved_reads{
      ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
      ReadFieldInstruction{.cursor = Cursor(0), .field = Field(2), .output = Reg(2)},
      ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(3)},
  };
  for (std::size_t first = 0; first < moved_reads.size(); ++first) {
    SCOPED_TRACE(first);
    test::WritePagerFixedVfs vfs{false};
    auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    ASSERT_NE(nullptr, database);
    Pager& pager = database->pager();
    RequireStatus(pager.BeginRead());
    RequireStatus(pager.BeginWrite());
    BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*database));
    RequireStatus(session.InitializeDatabase());
    TableBtreeWriter table = TakeValue(session.CreateTableBtree());
    const ByteBuffer original =
        TakeValue(EncodeRecord(std::array{SqlValue::Integer(10), SqlValue::Integer(20)}));
    const ByteBuffer successor =
        TakeValue(EncodeRecord(std::array{SqlValue::Integer(30), SqlValue::Integer(40)}));
    RequireStatus(table.Insert(1, original.view()));
    RequireStatus(table.Insert(3, successor.view()));
    const std::array functions{
        ScalarFunction{"mutate_peer", FunctionArity::Exact(0), FunctionDeterminism::kDeterministic,
                       FunctionCollationUse::kNone, TablePeerMutationProbe::Invoke},
    };
    const FunctionRegistry registry{functions};
    const std::array<const Collation*, 1> collations{&BinaryCollation()};
    const BytecodeProgram program = BuildProgram(
        pager, 5, 0, {}, {"mutate_peer", "BINARY"},
        {TableDescriptor(
            {CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0},
             CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 1},
             CursorFieldSource{.kind = CursorFieldSourceKind::kRowId, .record_field = 0}},
            2)},
        {ResultColumn("before"), ResultColumn("after"), ResultColumn("mapped_rowid"),
         ResultColumn("direct_rowid")},
        {
            OpenReadCursorInstruction{.cursor = Cursor(0)},
            RewindInstruction{.cursor = Cursor(0), .empty_target = Address(16)},
            ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
            CallScalarInstruction{.function = Symbol(0),
                                  .collation = Symbol(1),
                                  .first_argument = Reg(0),
                                  .argument_count = 0,
                                  .output = Reg(4)},
            moved_reads[first],
            moved_reads[(first + 1U) % moved_reads.size()],
            moved_reads[(first + 2U) % moved_reads.size()],
            ResultRowInstruction{.first = Reg(0), .count = 4},
            NextInstruction{.cursor = Cursor(0), .next_target = Address(10)},
            HaltInstruction{},
            ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(0)},
            ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(1)},
            ReadFieldInstruction{.cursor = Cursor(0), .field = Field(2), .output = Reg(2)},
            ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(3)},
            ResultRowInstruction{.first = Reg(0), .count = 4},
            HaltInstruction{},
            HaltInstruction{},
        });
    Vm vm = CreateAttachedVm(program, *database, kCatalogGeneration,
                             VmEnvironment{registry, collations});
    const TablePeerMutationProbe probe{table, {}, true};
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ASSERT_EQ(4U, vm.row().size());
    ExpectInteger(vm.row()[0], 10);
    EXPECT_EQ(SqlValueType::kNull, vm.row()[1].type());
    EXPECT_EQ(SqlValueType::kNull, vm.row()[2].type());
    EXPECT_EQ(SqlValueType::kNull, vm.row()[3].type());
    RequireStatus(table.Insert(2, original.view()));
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ASSERT_EQ(4U, vm.row().size());
    ExpectInteger(vm.row()[0], 30);
    ExpectInteger(vm.row()[1], 40);
    ExpectInteger(vm.row()[2], 3);
    ExpectInteger(vm.row()[3], 3);
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    RequireStatus(pager.Rollback());
  }
}

[[nodiscard]] Result<SqlValue> ThrowingFunction(const ScalarFunctionContext&,
                                                std::span<const SqlValue>) {
  throw std::runtime_error("expected scalar callback failure");
}

class BindingMutationProbe final {
 public:
  enum class Operation { Bindings, Reset, Detach, Step };

  explicit BindingMutationProbe(Vm& vm, Operation operation = Operation::Bindings) noexcept {
    active_vm_ = &vm;
    operation_ = operation;
  }
  BindingMutationProbe(const BindingMutationProbe&) = delete;
  BindingMutationProbe& operator=(const BindingMutationProbe&) = delete;
  ~BindingMutationProbe() { active_vm_ = nullptr; }

  [[nodiscard]] static Result<SqlValue> Invoke(const ScalarFunctionContext&,
                                               std::span<const SqlValue>) {
    if (active_vm_ == nullptr) {
      return std::unexpected(Error::Create(ErrorCode::kInternal, "binding probe has no VM"));
    }
    if (operation_ != Operation::Bindings) {
      if (operation_ == Operation::Step) {
        const auto stepped = active_vm_->Step();
        if (!stepped.has_value() && stepped.error().code() == ErrorCode::kMisuse) {
          return SqlValue::Integer(1);
        }
      } else {
        const auto status = operation_ == Operation::Reset ? active_vm_->Reset()
                                                           : active_vm_->DetachExecutionContext();
        if (!status.has_value() && status.error().code() == ErrorCode::kMisuse) {
          return SqlValue::Integer(1);
        }
      }
      return std::unexpected(
          Error::Create(ErrorCode::kAborted, "active callback mutation was not rejected"));
    }
    const SqlValue replacement = SqlValue::Text("changed");
    const auto bound = active_vm_->Bind(Parameter(0), replacement);
    const auto cleared = active_vm_->ClearBindings();
    const bool rejected = !bound.has_value() && bound.error().code() == ErrorCode::kMisuse &&
                          !cleared.has_value() && cleared.error().code() == ErrorCode::kMisuse;
    return SqlValue::Integer(rejected ? 1 : 0);
  }

 private:
  static inline Vm* active_vm_ = nullptr;
  static inline Operation operation_ = Operation::Bindings;
};

TEST_F(VmTest, RejectsBindingMutationFromActiveCallback) {
  const std::array functions{
      ScalarFunction{"mutate_bindings", FunctionArity::Exact(0),
                     FunctionDeterminism::kDeterministic, FunctionCollationUse::kNone,
                     BindingMutationProbe::Invoke},
  };
  const FunctionRegistry registry{functions};
  const std::array<const Collation*, 1> collations{&BinaryCollation()};
  const BytecodeProgram program =
      BuildProgram(*pager_, 2, 1, {}, {"mutate_bindings", "BINARY"}, {},
                   {ResultColumn("binding"), ResultColumn("blocked")},
                   {
                       LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
                       CallScalarInstruction{.function = Symbol(0),
                                             .collation = Symbol(1),
                                             .first_argument = Reg(0),
                                             .argument_count = 0,
                                             .output = Reg(1)},
                       ResultRowInstruction{.first = Reg(0), .count = 2},
                       HaltInstruction{},
                   });
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration,
                           VmEnvironment{registry, collations});
  const std::string original(256, 'b');
  RequireStatus(vm.Bind(Parameter(0), SqlValue::Text(original)));
  const BindingMutationProbe probe{vm};
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_EQ(2U, vm.row().size());
  ExpectText(vm.row()[0], original);
  ExpectInteger(vm.row()[1], 1);
  ASSERT_TRUE(vm.bindings()[0].text_value().has_value());
  EXPECT_EQ(original, vm.bindings()[0].text_value()->bytes());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, RejectsLifecycleMutationFromActiveCallbacks) {
  const std::array functions{
      ScalarFunction{"mutate_vm", FunctionArity::Exact(0), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, BindingMutationProbe::Invoke},
  };
  const FunctionRegistry registry{functions};
  const std::array<const Collation*, 1> collations{&BinaryCollation()};
  std::vector<SqlValue> constants;
  constants.push_back(SqlValue::Integer(42));
  const BytecodeProgram program =
      BuildProgram(*pager_, 2, 0, std::move(constants), {"mutate_vm", "BINARY"}, {},
                   {ResultColumn("value"), ResultColumn("blocked")},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       CallScalarInstruction{.function = Symbol(0),
                                             .collation = Symbol(1),
                                             .first_argument = Reg(0),
                                             .argument_count = 0,
                                             .output = Reg(1)},
                       ResultRowInstruction{.first = Reg(0), .count = 2},
                       HaltInstruction{},
                   });
  for (const auto operation :
       {BindingMutationProbe::Operation::Reset, BindingMutationProbe::Operation::Detach,
        BindingMutationProbe::Operation::Step}) {
    Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration,
                             VmEnvironment{registry, collations});
    const BindingMutationProbe probe{vm, operation};
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ASSERT_EQ(2U, vm.row().size());
    ExpectInteger(vm.row()[0], 42);
    ExpectInteger(vm.row()[1], 1);
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  }
}

TEST_F(VmTest, CleansUpCursorsBeforeRethrowingForeignExceptions) {
  const std::array<ScalarFunction, 1> functions{
      ScalarFunction{"throwing", FunctionArity::Exact(0), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ThrowingFunction},
  };
  const FunctionRegistry registry{functions};
  const std::array<const Collation*, 1> collations{&BinaryCollation()};
  const BytecodeProgram program =
      BuildProgram(*pager_, 1, 0, {}, {"throwing", "BINARY"},
                   {
                       TableDescriptor({}),
                   },
                   {},
                   {
                       OpenReadCursorInstruction{.cursor = Cursor(0)},
                       RewindInstruction{.cursor = Cursor(0), .empty_target = Address(5)},
                       CallScalarInstruction{
                           .function = Symbol(0),
                           .collation = Symbol(1),
                           .first_argument = Reg(0),
                           .argument_count = 0,
                           .output = Reg(0),
                       },
                       CloseCursorInstruction{.cursor = Cursor(0)},
                       HaltInstruction{},
                       CloseCursorInstruction{.cursor = Cursor(0)},
                       HaltInstruction{},
                   });
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration,
                           VmEnvironment{registry, collations});

  EXPECT_THROW(
      {
        const auto result = vm.Step();
        EXPECT_FALSE(result.has_value());
      },
      std::runtime_error);
  EXPECT_EQ(vm.state(), VmState::kError);
  RequireStatus(pager_->EndRead());
}

TEST_F(VmTest, EnforcesValueLimitsAtCreationBindingAndExecution) {
  std::vector<SqlValue> constant;
  constant.push_back(SqlValue::Text("abcd"));
  const BytecodeProgram constant_program =
      BuildProgram(*pager_, 1, 0, std::move(constant), {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   });
  const auto oversized_constant =
      Vm::Create(constant_program, VmEnvironment::Core(), VmLimits{.maximum_value_bytes = 3});
  ASSERT_FALSE(oversized_constant.has_value());
  EXPECT_EQ(oversized_constant.error().code(), ErrorCode::kTooLarge);

  const BytecodeProgram parameter_program =
      BuildProgram(*pager_, 1, 1, {}, {}, {}, {ResultColumn()},
                   {
                       LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   });
  Vm parameter_vm = CreateCoreVm(parameter_program, VmLimits{.maximum_value_bytes = 3});
  const SqlValue oversized_binding = SqlValue::Text("abcd");
  const Status bound = parameter_vm.Bind(Parameter(0), oversized_binding);
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(bound.error().code(), ErrorCode::kTooLarge);

  std::vector<SqlValue> concat_constants;
  concat_constants.push_back(SqlValue::Text("ab"));
  concat_constants.push_back(SqlValue::Text("cd"));
  const BytecodeProgram concat_program =
      BuildProgram(*pager_, 3, 0, std::move(concat_constants), {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
                       BinaryInstruction{
                           .operation = BinaryOperation::kConcatenate,
                           .left = Reg(0),
                           .right = Reg(1),
                           .output = Reg(2),
                       },
                       ResultRowInstruction{.first = Reg(2), .count = 1},
                       HaltInstruction{},
                   });
  Vm concat_vm = CreateCoreVm(concat_program, VmLimits{.maximum_value_bytes = 3});
  const auto concatenated = concat_vm.Step();
  ASSERT_FALSE(concatenated.has_value());
  EXPECT_EQ(concatenated.error().code(), ErrorCode::kTooLarge);
  EXPECT_EQ(concat_vm.state(), VmState::kError);
}

TEST_F(VmTest, UsesSharedDatabaseFormatNormalization) {
  std::vector<std::byte> bytes = ReadBytes(FixturePath());
  Write32(bytes, 44, 0);
  Write32(bytes, 56, 0);
  const TemporaryDatabase database(bytes);
  PosixVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());

  const BytecodeProgram program = BuildProgram(*pager, 0, 0, {}, {},
                                               {
                                                   TableDescriptor({}),
                                               },
                                               {},
                                               {
                                                   OpenReadCursorInstruction{.cursor = Cursor(0)},
                                                   CloseCursorInstruction{.cursor = Cursor(0)},
                                                   HaltInstruction{},
                                               });
  Vm vm = CreateAttachedVm(program, *pager_database, kCatalogGeneration);
  EXPECT_EQ(TakeValue(vm.Step()), VmStep::kDone);
  RequireStatus(pager->EndRead());
}

TEST_F(VmTest, MovedFromMachinesRejectOperations) {
  const BytecodeProgram program = BuildProgram(*pager_, 0, 0, {}, {}, {}, {}, {HaltInstruction{}});
  Vm source = CreateCoreVm(program);
  Vm destination = std::move(source);

  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(source.state(), VmState::kInvalid);
  const auto stepped = source.Step();
  ASSERT_FALSE(stepped.has_value());
  EXPECT_EQ(stepped.error().code(), ErrorCode::kMisuse);
  const Status reset = source.Reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(reset.error().code(), ErrorCode::kMisuse);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)

  EXPECT_EQ(TakeValue(destination.Step()), VmStep::kDone);
}

TEST(Vm, ExecutesSnapshotFreeProgramsAndPublishesBindingsWithoutAReadTransaction) {
  PosixVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, FixturePath().string()));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  const BytecodeProgram program =
      BuildProgram(*pager, 1, 1, {}, {}, {}, {ResultColumn()},
                   {
                       LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   });
  ASSERT_FALSE(program.requires_database_snapshot());
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  const SqlValue value = SqlValue::Integer(41);
  RequireStatus(vm.Bind(Parameter(0), value));
  ASSERT_EQ(1U, vm.bindings().size());
  ExpectInteger(vm.bindings().front(), 41);
  RequireStatus(pager->EndRead());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_database, kCatalogGeneration}));

  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 41);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(vm.Reset());
  ASSERT_EQ(1U, vm.bindings().size());
  ExpectInteger(vm.bindings().front(), 41);
}

TEST(Vm, RejectsSnapshotRequiredProgramsWithoutAReadTransaction) {
  PosixVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, FixturePath().string()));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager);
  input.requires_database_snapshot = true;
  input.instructions.emplace_back(HaltInstruction{});
  const BytecodeProgram program = TakeProgramValue(BytecodeProgram::Create(input));
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(pager->EndRead());

  const Status attached =
      vm.AttachExecutionContext(VmExecutionContext{*pager_database, kCatalogGeneration});
  ASSERT_FALSE(attached.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, attached.error().code());
  EXPECT_EQ(VmState::kReady, vm.state());
}

TEST(Vm, RejectsTemporaryStorageFactoryForDifferentPager) {
  PosixVfs first_vfs;
  PosixVfs second_vfs;
  std::unique_ptr<BtreeDatabase> first_database =
      TakeValue(BtreeDatabase::Open(first_vfs, FixturePath().string()));
  Pager* first = first_database == nullptr ? nullptr : &first_database->pager();
  std::unique_ptr<BtreeDatabase> second_database =
      TakeValue(BtreeDatabase::Open(second_vfs, FixturePath().string()));
  Pager* second = second_database == nullptr ? nullptr : &second_database->pager();
  const TemporaryStorageFactory second_factory =
      TakeValue(TemporaryStorageFactory::Create(second_vfs, *second));
  const BytecodeProgram program = BuildProgram(*first, 0, 0, {}, {}, {}, {}, {HaltInstruction{}});
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));

  const Status attached = vm.AttachExecutionContext(
      VmExecutionContext{*first_database, kCatalogGeneration, second_factory});
  ASSERT_FALSE(attached.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, attached.error().code());
  EXPECT_FALSE(vm.has_execution_context());
}

TEST_F(VmTest, ExecutesAggregateLifecycleAndResetReuse) {
  const BytecodeProgram program = BuildAggregateSumProgram(*pager_);
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration);

  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_EQ(2U, vm.row().size());
  ExpectInteger(vm.row()[0], static_cast<std::int64_t>(
                                 BytecodeAggregateStepEffect::kNoRepresentativePreference));
  ExpectInteger(vm.row()[1], 5);

  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_EQ(2U, vm.row().size());
  ExpectInteger(vm.row()[0], static_cast<std::int64_t>(
                                 BytecodeAggregateStepEffect::kNoRepresentativePreference));
  ExpectInteger(vm.row()[1], 4);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));

  RequireStatus(vm.Reset());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(vm.row()[1], 5);
}

TEST_F(VmTest, PublishesMinMaxRepresentativeEffectsAndCleansAbortedState) {
  const BytecodeProgram program = BuildAggregateMinEffectProgram(*pager_);
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration);

  const std::array expected{
      BytecodeAggregateStepEffect::kReplaceRepresentative,
      BytecodeAggregateStepEffect::kKeepRepresentative,
      BytecodeAggregateStepEffect::kReplaceRepresentative,
  };
  for (const BytecodeAggregateStepEffect effect : expected) {
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ExpectInteger(OnlyRowValue(vm), static_cast<std::int64_t>(effect));
  }
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 1);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));

  RequireStatus(vm.Reset());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  RequireStatus(vm.Reset());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm),
                static_cast<std::int64_t>(BytecodeAggregateStepEffect::kReplaceRepresentative));
}

TEST_F(VmTest, ResolvesAggregateDescriptorsAtCreation) {
  const auto program_with = [&](std::string function, std::uint32_t argument_count,
                                std::string collation) {
    ProgramInput input;
    input.schema_version = CurrentSchema(*pager_);
    input.symbols.push_back(std::move(function));
    input.symbols.push_back(std::move(collation));
    input.aggregates.push_back(AggregateDescriptorFor(Symbol(0), Symbol(1), argument_count));
    input.instructions = {HaltInstruction{}};
    return TakeProgramValue(BytecodeProgram::Create(input));
  };

  const BytecodeProgram unknown = program_with("missing", 1, "BINARY");
  const auto unknown_vm = Vm::Create(unknown, VmEnvironment::Core());
  ASSERT_FALSE(unknown_vm.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, unknown_vm.error().code());

  const BytecodeProgram wrong_arity = program_with("sum", 0, "BINARY");
  const auto wrong_arity_vm = Vm::Create(wrong_arity, VmEnvironment::Core());
  ASSERT_FALSE(wrong_arity_vm.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, wrong_arity_vm.error().code());

  const BytecodeProgram missing_collation = program_with("sum", 1, "missing");
  const auto missing_collation_vm = Vm::Create(missing_collation, VmEnvironment::Core());
  ASSERT_FALSE(missing_collation_vm.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, missing_collation_vm.error().code());
}

TEST_F(VmTest, FinalizesEmptyAggregateAndReportsDelayedErrors) {
  ProgramInput empty_input;
  empty_input.schema_version = CurrentSchema(*pager_);
  empty_input.register_count = 1;
  empty_input.symbols = {"count", "BINARY"};
  empty_input.aggregates.push_back(AggregateDescriptorFor(Symbol(0), Symbol(1), 0));
  empty_input.result_columns = {
      ResultColumnMetadata{
          .name = "count",
          .declared_type = std::nullopt,
          .affinity = TypeAffinity::kNone,
      },
  };
  empty_input.instructions = {
      OpenAggregateInstruction{.aggregate = AggregateId(0)},
      FinalizeAggregateInstruction{.aggregate = AggregateId(0), .output = Reg(0)},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      HaltInstruction{},
  };
  const BytecodeProgram empty_program = TakeProgramValue(BytecodeProgram::Create(empty_input));
  Vm empty = CreateAttachedVm(empty_program, *database_, kCatalogGeneration);
  EXPECT_EQ(VmStep::kRow, TakeValue(empty.Step()));
  ExpectInteger(OnlyRowValue(empty), 0);
  EXPECT_EQ(VmStep::kDone, TakeValue(empty.Step()));

  const BytecodeProgram delayed_program = BuildAggregateConcatErrorProgram(*pager_);
  Vm delayed = TakeValue(
      Vm::Create(delayed_program, VmEnvironment::Core(), VmLimits{.maximum_value_bytes = 5}));
  RequireStatus(delayed.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(VmStep::kRow, TakeValue(delayed.Step()));
  EXPECT_EQ("def", OnlyRowValue(delayed).text_value().value_or(Utf8View{}).bytes());
  const auto finalized = delayed.Step();
  ASSERT_FALSE(finalized.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, finalized.error().code());
  EXPECT_EQ(VmState::kError, delayed.state());

  RequireStatus(delayed.Reset());
  RequireStatus(delayed.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  EXPECT_EQ(VmStep::kRow, TakeValue(delayed.Step()));
}

TEST_F(VmTest, ExecutesSorterBytecodeAndCanResetForAnotherRun) {
  const BytecodeProgram program = BuildSorterProgram(*pager_);
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                    .sorter_memory_threshold = ByteCount{1U << 20U},
                                                }));
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));

  const std::array<std::string_view, 3> payloads{"one", "two", "three"};
  for (std::int64_t expected = 1; expected <= 3; ++expected) {
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ASSERT_EQ(2U, vm.row().size());
    EXPECT_EQ(expected, vm.row()[0].integer_value());
    EXPECT_EQ(payloads[static_cast<std::size_t>(expected - 1)],
              vm.row()[1].text_value().value_or(Utf8View{}).bytes());
  }
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));

  RequireStatus(vm.Reset());
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(1, vm.row()[0].integer_value());
}

TEST(Vm, SpillsSorterBytecodeAndDeletesTemporaryFilesAtHalt) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, test::kWritePagerInputPath));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  const BytecodeProgram program = BuildSorterProgram(*pager);
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{128},
                                                }));
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*pager_database, kCatalogGeneration, factory}));

  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_TRUE(vfs.pathless_file_present());
  while (TakeValue(vm.Step()) == VmStep::kRow) {
  }
  EXPECT_FALSE(vfs.pathless_file_present());
  RequireStatus(pager->EndRead());
}

TEST(Vm, SorterBytecodeRequiresTemporaryStorageFactoryAndCleansIoFailures) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, test::kWritePagerInputPath));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  const BytecodeProgram program = BuildSorterProgram(*pager);

  Vm missing = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      missing.AttachExecutionContext(VmExecutionContext{*pager_database, kCatalogGeneration}));
  const auto missing_factory = missing.Step();
  ASSERT_FALSE(missing_factory.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing_factory.error().code());

  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{128},
                                                }));
  vfs.FailPathlessWriteAfter(0, ErrorCode::kIo);
  Vm failing = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(failing.AttachExecutionContext(
      VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  const auto failed = failing.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_FALSE(vfs.pathless_file_present());
  RequireStatus(pager->EndRead());
}

TEST_F(VmTest, ResolvesOrderingCapabilityCollations) {
  ProgramInput missing_collation;
  missing_collation.schema_version = CurrentSchema(*pager_);
  missing_collation.symbols.emplace_back("missing");
  missing_collation.sorters.push_back(SorterDescriptor());
  missing_collation.instructions = {
      OpenSorterInstruction{.sorter = SorterId(0)},
      HaltInstruction{},
  };
  const BytecodeProgram missing_program =
      TakeProgramValue(BytecodeProgram::Create(missing_collation));
  const auto unresolved = Vm::Create(missing_program, VmEnvironment::Core());
  ASSERT_FALSE(unresolved.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, unresolved.error().code());

  ProgramInput missing_relation;
  missing_relation.schema_version = CurrentSchema(*pager_);
  missing_relation.symbols.emplace_back("missing");
  missing_relation.relations.push_back(SorterDescriptor());
  missing_relation.instructions = {
      OpenRelationInstruction{.relation = Relation(0)},
      HaltInstruction{},
  };
  const BytecodeProgram missing_relation_program =
      TakeProgramValue(BytecodeProgram::Create(missing_relation));
  const auto unresolved_relation = Vm::Create(missing_relation_program, VmEnvironment::Core());
  ASSERT_FALSE(unresolved_relation.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, unresolved_relation.error().code());

  ProgramInput missing_comparison;
  missing_comparison.schema_version = CurrentSchema(*pager_);
  missing_comparison.symbols.emplace_back("missing");
  OrderingRecordDescriptor comparison = SorterDescriptor();
  comparison.key_field_count = 2;
  comparison.key_columns.push_back(OrderingColumnMetadata{
      .collation = Symbol(0),
      .order = BytecodeSortOrder::kAscending,
      .null_placement = BytecodeNullPlacement::kFirst,
  });
  missing_comparison.record_comparisons.push_back(std::move(comparison));
  missing_comparison.instructions = {HaltInstruction{}};
  const BytecodeProgram missing_comparison_program =
      TakeProgramValue(BytecodeProgram::Create(missing_comparison));
  const auto unresolved_comparison = Vm::Create(missing_comparison_program, VmEnvironment::Core());
  ASSERT_FALSE(unresolved_comparison.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, unresolved_comparison.error().code());
}

TEST_F(VmTest, ExecutesBoundedTopNAdmissionAndSkipsRejectedPayloadWork) {
  const BytecodeProgram program = BuildTopNProgram(*pager_);
  const TemporaryStorageFactory factory = TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_));
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));

  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_EQ(2U, vm.row().size());
  EXPECT_EQ(1, vm.row()[0].integer_value());
  EXPECT_EQ(10, vm.row()[1].integer_value());
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(2, vm.row()[0].integer_value());
  EXPECT_EQ(20, vm.row()[1].integer_value());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  EXPECT_EQ(28U, vm.executed_instruction_count());
}

TEST(Vm, ExecutesFileBackedTopNAndDeletesTransientFilesOnHaltAndFailure) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database = TakeValue(
      BtreeDatabase::Open(vfs, test::kWritePagerInputPath,
                          PagerOptions{
                              .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                              .cache_capacity_pages = 64,
                          }));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  const BytecodeProgram program = BuildTopNProgram(*pager);
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{256},
                                                }));

  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_TRUE(vfs.pathless_file_present());
  while (TakeValue(vm.Step()) == VmStep::kRow) {
  }
  EXPECT_FALSE(vfs.pathless_file_present());

  {
    Vm abandoned = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(abandoned.AttachExecutionContext(
        VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
    EXPECT_EQ(VmStep::kRow, TakeValue(abandoned.Step()));
    EXPECT_TRUE(vfs.pathless_file_present());
  }
  EXPECT_FALSE(vfs.pathless_file_present());

  vfs.FailPathlessWriteAfter(0, ErrorCode::kIo);
  Vm failing = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(failing.AttachExecutionContext(
      VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  const auto failed = failing.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_FALSE(vfs.pathless_file_present());
  RequireStatus(pager->EndRead());
}

TEST_F(VmTest, HandlesZeroBoundAndTopNMemoryFailures) {
  const BytecodeProgram zero_program = BuildTopNProgram(*pager_, 0);
  const TemporaryStorageFactory normal = TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_));
  Vm missing = TakeValue(Vm::Create(zero_program, VmEnvironment::Core()));
  RequireStatus(missing.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration}));
  const auto missing_factory = missing.Step();
  ASSERT_FALSE(missing_factory.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing_factory.error().code());

  Vm zero = TakeValue(Vm::Create(zero_program, VmEnvironment::Core()));
  RequireStatus(
      zero.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, normal}));
  EXPECT_EQ(VmStep::kDone, TakeValue(zero.Step()));

  const BytecodeProgram bounded_program = BuildTopNProgram(*pager_);
  const TemporaryStorageFactory tiny =
      TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                    .sorter_memory_threshold = ByteCount{1},
                                                }));
  Vm failing = TakeValue(Vm::Create(bounded_program, VmEnvironment::Core()));
  RequireStatus(
      failing.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, tiny}));
  const auto failed = failing.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, failed.error().code());

  const BytecodeProgram negative_program = BuildTopNProgram(*pager_, -1);
  Vm negative = TakeValue(Vm::Create(negative_program, VmEnvironment::Core()));
  RequireStatus(
      negative.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, normal}));
  const auto rejected_bound = negative.Step();
  ASSERT_FALSE(rejected_bound.has_value());
  EXPECT_EQ(ErrorCode::kTypeMismatch, rejected_bound.error().code());
}

TEST_F(VmTest, ExecutesRelationBytecodeAndCanResetForAnotherRun) {
  const BytecodeProgram program = BuildRelationProgram(*pager_);
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));

  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_EQ(2U, vm.row().size());
  EXPECT_EQ(SqlValueType::kReal, vm.row()[0].type());
  EXPECT_EQ(2.0, vm.row()[0].real_value());
  EXPECT_EQ(22, vm.row()[1].integer_value());
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(3, vm.row()[0].integer_value());
  EXPECT_EQ(30, vm.row()[1].integer_value());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));

  RequireStatus(vm.Reset());
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(2.0, vm.row()[0].real_value());
}

TEST_F(VmTest, ReprobesBorrowedLookupRecordsAndInvalidatesDecodedViewsBeforeMovement) {
  const BytecodeProgram program = BuildLookupRelationProgram(*pager_);
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    const auto factory = TakeValue(
        TemporaryStorageFactory::Create(vfs_, *pager_, TemporaryStorageOptions{.mode = mode}));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    for (std::size_t run = 0; run < 2U; ++run) {
      RequireStatus(
          vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));
      RequireStatus(vm.Bind(Parameter(0), SqlValue::Text("A")));
      for (const std::optional<std::int64_t> rowid :
           {std::optional<std::int64_t>{}, std::optional<std::int64_t>{1},
            std::optional<std::int64_t>{2}, std::optional<std::int64_t>{},
            std::optional<std::int64_t>{1}, std::optional<std::int64_t>{2}}) {
        ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
        ASSERT_EQ(2U, vm.row().size());
        if (rowid.has_value()) {
          ASSERT_TRUE(vm.row()[0].text_value().has_value());
          EXPECT_EQ(std::string(2048, 'p') + std::to_string(*rowid),
                    vm.row()[0].text_value()->bytes());
          EXPECT_EQ(*rowid, vm.row()[1].integer_value());
        } else {
          EXPECT_EQ(SqlValueType::kNull, vm.row()[0].type());
          EXPECT_EQ(SqlValueType::kNull, vm.row()[1].type());
        }
      }
      EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
      RequireStatus(vm.Reset());
    }
    RequireStatus(
        vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));
    RequireStatus(vm.Bind(Parameter(0), SqlValue{}));
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    EXPECT_EQ(4, vm.row()[1].integer_value());
    RequireStatus(vm.Reset());
  }
}

TEST(Vm, EmptyLookupRemainsFileLazyAndPositionedResetDropsBorrowedViewsFirst) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database = TakeValue(BtreeDatabase::Open(
      vfs, test::kWritePagerInputPath,
      PagerOptions{.empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                   .cache_capacity_pages = 1}));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  const auto factory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager,
      TemporaryStorageOptions{.mode = TemporaryStoreMode::kFile,
                              .sorter_memory_threshold = ByteCount{64}}));
  const BytecodeProgram empty = BuildLookupRelationProgram(*pager, true);
  Vm empty_vm = TakeValue(Vm::Create(empty, VmEnvironment::Core()));
  RequireStatus(empty_vm.AttachExecutionContext(
      VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  EXPECT_EQ(VmStep::kRow, TakeValue(empty_vm.Step()));
  EXPECT_EQ(SqlValueType::kNull, empty_vm.row()[0].type());
  EXPECT_FALSE(vfs.pathless_file_present());
  EXPECT_EQ(VmStep::kDone, TakeValue(empty_vm.Step()));
  EXPECT_FALSE(vfs.pathless_file_present());

  const BytecodeProgram populated = BuildLookupRelationProgram(*pager);
  Vm positioned = TakeValue(Vm::Create(populated, VmEnvironment::Core()));
  RequireStatus(positioned.AttachExecutionContext(
      VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  RequireStatus(positioned.Bind(Parameter(0), SqlValue::Text("a")));
  EXPECT_EQ(VmStep::kRow, TakeValue(positioned.Step()));
  EXPECT_EQ(VmStep::kRow, TakeValue(positioned.Step()));
  EXPECT_TRUE(vfs.pathless_file_present());
  EXPECT_EQ(1, positioned.row()[1].integer_value());
  RequireStatus(positioned.Reset());
  EXPECT_FALSE(vfs.pathless_file_present());
  RequireStatus(pager->EndRead());
}

TEST(Vm, BorrowedLookupPropagatesConstructionAndPositionedReadFailuresWithCleanup) {
  for (const bool after_position : {false, true}) {
    test::WritePagerFixedVfs vfs;
    std::unique_ptr<BtreeDatabase> pager_database = TakeValue(BtreeDatabase::Open(
        vfs, test::kWritePagerInputPath,
        PagerOptions{.empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                     .cache_capacity_pages = 1}));
    Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
    RequireStatus(pager->BeginRead());
    const auto factory = TakeValue(TemporaryStorageFactory::Create(
        vfs, *pager,
        TemporaryStorageOptions{.mode = TemporaryStoreMode::kFile,
                                .sorter_memory_threshold = ByteCount{64}}));
    const BytecodeProgram program = BuildLookupRelationProgram(*pager);
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(
        VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
    RequireStatus(vm.Bind(Parameter(0), SqlValue::Text("A")));
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    if (after_position) {
      EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
      EXPECT_EQ(1, vm.row()[1].integer_value());
      vfs.FailPathlessReadAfter(0, ErrorCode::kIo);
    } else {
      vfs.FailPathlessWriteAfter(0, ErrorCode::kIo);
    }
    const auto failed = vm.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_FALSE(vfs.pathless_file_present());
    RequireStatus(pager->EndRead());
  }
}

TEST(Vm, FileBackedRelationCleansUpAtHaltResetAndIoFailure) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database = TakeValue(
      BtreeDatabase::Open(vfs, test::kWritePagerInputPath,
                          PagerOptions{
                              .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                              .cache_capacity_pages = 1,
                          }));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  const BytecodeProgram program = BuildRelationProgram(*pager);
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{64},
                                                }));

  Vm reset = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(reset.AttachExecutionContext(
      VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  EXPECT_EQ(VmStep::kRow, TakeValue(reset.Step()));
  EXPECT_TRUE(vfs.pathless_file_present());
  RequireStatus(reset.Reset());
  EXPECT_FALSE(vfs.pathless_file_present());

  Vm halted = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(halted.AttachExecutionContext(
      VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  while (TakeValue(halted.Step()) == VmStep::kRow) {
  }
  EXPECT_FALSE(vfs.pathless_file_present());

  Vm missing = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      missing.AttachExecutionContext(VmExecutionContext{*pager_database, kCatalogGeneration}));
  const auto missing_factory = missing.Step();
  ASSERT_FALSE(missing_factory.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing_factory.error().code());

  vfs.FailPathlessWriteAfter(0, ErrorCode::kIo);
  Vm failing = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(failing.AttachExecutionContext(
      VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  const auto failed = failing.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_FALSE(vfs.pathless_file_present());
  RequireStatus(pager->EndRead());
}

TEST(Vm, RelationDrainCanFailAfterRowsAndStillDeletesItsTransientFile) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database = TakeValue(
      BtreeDatabase::Open(vfs, test::kWritePagerInputPath,
                          PagerOptions{
                              .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                              .cache_capacity_pages = 1,
                          }));
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  RequireStatus(pager->BeginRead());
  const BytecodeProgram program = BuildRelationDrainProgram(*pager);
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{1024},
                                                }));
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*pager_database, kCatalogGeneration, factory}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(0, vm.row()[0].integer_value());
  EXPECT_TRUE(vfs.pathless_file_present());

  vfs.FailPathlessReadAfter(0, ErrorCode::kIo);
  std::size_t rows_seen = 1;
  Result<VmStep> stepped = VmStep::kRow;
  while (stepped.has_value() && *stepped == VmStep::kRow) {
    stepped = vm.Step();
    if (stepped.has_value() && *stepped == VmStep::kRow) {
      ++rows_seen;
    }
  }
  ASSERT_FALSE(stepped.has_value());
  EXPECT_EQ(ErrorCode::kIo, stepped.error().code());
  EXPECT_GT(rows_seen, 0U);
  EXPECT_LT(rows_seen, 24U);
  EXPECT_FALSE(vfs.pathless_file_present());
  RequireStatus(pager->EndRead());
}

TEST_F(VmTest, ComparesConsecutiveRecordRangesWithOrderingMetadata) {
  const BytecodeProgram program = BuildRecordComparisonProgram(*pager_);
  Vm vm = CreateAttachedVm(program, *database_, kCatalogGeneration);
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_EQ(3U, vm.row().size());
  EXPECT_EQ(-1, vm.row()[0].integer_value());
  EXPECT_EQ(1, vm.row()[1].integer_value());
  EXPECT_EQ(0, vm.row()[2].integer_value());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

TEST_F(VmTest, ReplaceRelationBranchesWhenAnEquivalentKeyAlreadyExists) {
  const BytecodeProgram program = BuildReplaceBranchProgram(*pager_);
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*database_, kCatalogGeneration, factory}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_EQ(1U, vm.row().size());
  EXPECT_EQ(1, vm.row()[0].integer_value());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
}

#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
TEST_F(VmTest, RecordsExecutedInstructionsWhenInstrumentationIsEnabled) {
  const BytecodeProgram program =
      BuildProgram(*pager_, 1, 0,
                   [] {
                     std::vector<SqlValue> values;
                     values.push_back(SqlValue::Integer(1));
                     return values;
                   }(),
                   {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   });
  Vm vm = CreateCoreVm(program);
  instrumentation::CounterCollection counters;
  const instrumentation::ScopedCounterCollection collection(counters);

  EXPECT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  EXPECT_EQ(counters.Value(instrumentation::Counter::kVmInstructions), 2U);
  EXPECT_EQ(TakeValue(vm.Step()), VmStep::kDone);
  EXPECT_EQ(counters.Value(instrumentation::Counter::kVmInstructions), 3U);
}
#endif

}  // namespace
}  // namespace modern_sqlite
