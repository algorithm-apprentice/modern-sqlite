#include "modern_sqlite/bytecode/program.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace modern_sqlite {
namespace {

[[nodiscard]] constexpr RegisterId Reg(std::uint32_t value) { return RegisterId(value); }
[[nodiscard]] constexpr CursorId Cursor(std::uint32_t value) { return CursorId(value); }
[[nodiscard]] constexpr WriteCursorId WriteCursor(std::uint32_t value) {
  return WriteCursorId(value);
}
[[nodiscard]] constexpr SorterId Sorter(std::uint32_t value) { return SorterId(value); }
[[nodiscard]] constexpr TopNId TopN(std::uint32_t value) { return TopNId(value); }
[[nodiscard]] constexpr RelationId Relation(std::uint32_t value) { return RelationId(value); }
[[nodiscard]] constexpr AggregateId Aggregate(std::uint32_t value) { return AggregateId(value); }
[[nodiscard]] constexpr RecordComparisonId RecordComparison(std::uint32_t value) {
  return RecordComparisonId(value);
}
[[nodiscard]] constexpr ParameterId Parameter(std::uint32_t value) { return ParameterId(value); }
[[nodiscard]] constexpr ConstantId Constant(std::uint32_t value) { return ConstantId(value); }
[[nodiscard]] constexpr SymbolId Symbol(std::uint32_t value) { return SymbolId(value); }
[[nodiscard]] constexpr CursorFieldId Field(std::uint32_t value) { return CursorFieldId(value); }
[[nodiscard]] constexpr InstructionAddress Address(std::uint32_t value) {
  return InstructionAddress(value);
}
[[nodiscard]] constexpr ProgramResourceCounts Resources(std::uint32_t registers,
                                                        std::uint32_t parameters = 0) {
  return ProgramResourceCounts{
      .registers = registers,
      .parameters = parameters,
  };
}

[[nodiscard]] ResultColumnMetadata IntegerResultColumn(std::string name = "value") {
  return ResultColumnMetadata{
      .name = std::move(name),
      .declared_type = "INTEGER",
      .affinity = TypeAffinity::kInteger,
  };
}

[[nodiscard]] ProgramInput ScalarProgramInput() {
  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 7, .generation = 11};
  input.register_count = 1;
  input.constants.push_back(SqlValue::Integer(42));
  input.result_columns.push_back(IntegerResultColumn());
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      HaltInstruction{},
  };
  return input;
}

[[nodiscard]] ReadCursorDescriptor RowIdCursorDescriptor() {
  return ReadCursorDescriptor{
      .root_page = RootPageNumber(2),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 2,
      .fields =
          {
              CursorFieldSource{
                  .kind = CursorFieldSourceKind::kRecordField,
                  .record_field = 0,
              },
              CursorFieldSource{
                  .kind = CursorFieldSourceKind::kRowId,
                  .record_field = 0,
              },
          },
      .index_columns = {},
  };
}

[[nodiscard]] ReadCursorDescriptor IndexCursorDescriptor() {
  return ReadCursorDescriptor{
      .root_page = RootPageNumber(3),
      .storage = CursorStorageKind::kIndex,
      .record_field_count = 2,
      .fields =
          {
              CursorFieldSource{
                  .kind = CursorFieldSourceKind::kRecordField,
                  .record_field = 0,
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
                  .order = BytecodeSortOrder::kDescending,
              },
          },
  };
}

[[nodiscard]] ProgramInput SharedValueSubroutineInput() {
  ProgramInput input;
  input.register_count = 5;
  input.parameter_count = 1;
  input.constants.push_back(SqlValue::Integer(42));
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(2));
  input.result_columns.push_back(IntegerResultColumn());
  input.subroutines.push_back(SubroutineDescriptor{.entry = Address(11), .end = Address(13)});
  input.instructions = {
      LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      JumpIfInstruction{.condition = JumpCondition::kIfTrue, .input = Reg(0), .target = Address(7)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(2)},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      ResultRowInstruction{.first = Reg(2), .count = 1},
      JumpInstruction{.target = Address(10)},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(3)},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      ResultRowInstruction{.first = Reg(3), .count = 1},
      HaltInstruction{},
      CopyInstruction{.input = Reg(1), .output = Reg(4)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
  };
  return input;
}

TEST(BytecodeProgramTest, VerifiesSharedValueSubroutineWithoutMergingPrivateCallerRegisters) {
  const ProgramInput input = SharedValueSubroutineInput();
  auto program = BytecodeProgram::Create(input);
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(1U, program->subroutines().size());
  EXPECT_EQ(1U, program->verification_metrics().maximum_subroutine_depth);
  EXPECT_EQ(13U, program->verification_metrics().reachable_instruction_count);
}

TEST(BytecodeProgramTest, RejectsMalformedSubroutineRegionsAndReferences) {
  for (const SubroutineDescriptor region :
       {SubroutineDescriptor{.entry = Address(0), .end = Address(13)},
        SubroutineDescriptor{.entry = Address(11), .end = Address(11)},
        SubroutineDescriptor{.entry = Address(12), .end = Address(11)},
        SubroutineDescriptor{.entry = Address(11), .end = Address(14)}}) {
    auto input = SharedValueSubroutineInput();
    input.subroutines[0] = region;
    const auto rejected = BytecodeProgram::Create(input);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineDescriptor, rejected.error().code);
  }
  auto overlap = SharedValueSubroutineInput();
  overlap.subroutines.push_back(overlap.subroutines[0]);
  const auto overlapping = BytecodeProgram::Create(overlap);
  ASSERT_FALSE(overlapping.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineDescriptor, overlapping.error().code);
  for (const Instruction instruction :
       {Instruction{CallSubroutineInstruction{.subroutine = SubroutineId{1}}},
        Instruction{ReturnSubroutineInstruction{.subroutine = SubroutineId{1}}}}) {
    auto input = SharedValueSubroutineInput();
    input.instructions[4] = instruction;
    const auto rejected = BytecodeProgram::Create(input);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ProgramErrorCode::kInvalidSubroutine, rejected.error().code);
  }
}

TEST(BytecodeProgramTest, RejectsCrossRegionControlAndSubroutineCapabilityUse) {
  for (const Instruction instruction :
       {Instruction{JumpInstruction{.target = Address(10)}}, Instruction{HaltInstruction{}},
        Instruction{ResultRowInstruction{.first = Reg(1), .count = 1}},
        Instruction{OpenReadCursorInstruction{.cursor = Cursor(0)}}}) {
    auto input = SharedValueSubroutineInput();
    input.cursors.push_back(RowIdCursorDescriptor());
    input.instructions[11] = instruction;
    const auto rejected = BytecodeProgram::Create(input);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, rejected.error().code);
  }
  auto entered = SharedValueSubroutineInput();
  entered.instructions[6] = JumpInstruction{.target = Address(11)};
  const auto cross_entry = BytecodeProgram::Create(entered);
  ASSERT_FALSE(cross_entry.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, cross_entry.error().code);
  auto main_return = SharedValueSubroutineInput();
  main_return.instructions[4] = ReturnSubroutineInstruction{.subroutine = SubroutineId{0}};
  const auto returned = BytecodeProgram::Create(main_return);
  ASSERT_FALSE(returned.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, returned.error().code);
}

TEST(BytecodeProgramTest, RequiresSubroutineInputsAndOnlyPublishesDefiniteOutputs) {
  auto missing = SharedValueSubroutineInput();
  missing.instructions[1] = LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)};
  const auto input_rejected = BytecodeProgram::Create(missing);
  ASSERT_FALSE(input_rejected.has_value());
  EXPECT_EQ(ProgramErrorCode::kUninitializedRegister, input_rejected.error().code);
  EXPECT_EQ(1U, input_rejected.error().detail);

  auto conditional = SharedValueSubroutineInput();
  conditional.subroutines[0].end = Address(15);
  conditional.instructions[11] = JumpIfInstruction{
      .condition = JumpCondition::kIfTrue, .input = Reg(0), .target = Address(14)};
  conditional.instructions[12] = CopyInstruction{.input = Reg(1), .output = Reg(4)};
  conditional.instructions.push_back(ReturnSubroutineInstruction{.subroutine = SubroutineId{0}});
  conditional.instructions.push_back(ReturnSubroutineInstruction{.subroutine = SubroutineId{0}});
  ASSERT_TRUE(BytecodeProgram::Create(conditional).has_value());
  conditional.instructions[5] = ResultRowInstruction{.first = Reg(4), .count = 1};
  conditional.instructions[9] = ResultRowInstruction{.first = Reg(4), .count = 1};
  const auto output_rejected = BytecodeProgram::Create(conditional);
  ASSERT_FALSE(output_rejected.has_value());
  EXPECT_EQ(ProgramErrorCode::kUninitializedRegister, output_rejected.error().code);
  EXPECT_EQ(4U, output_rejected.error().detail);
}

TEST(BytecodeProgramTest, RejectsRecursiveUncalledAndNonreturningSubroutines) {
  auto recursive = SharedValueSubroutineInput();
  recursive.instructions[11] = CallSubroutineInstruction{.subroutine = SubroutineId{0}};
  const auto cycle = BytecodeProgram::Create(recursive);
  ASSERT_FALSE(cycle.has_value());
  EXPECT_EQ(ProgramErrorCode::kRecursiveSubroutine, cycle.error().code);
  auto uncalled = SharedValueSubroutineInput();
  uncalled.instructions[4] = CopyInstruction{.input = Reg(1), .output = Reg(4)};
  uncalled.instructions[8] = CopyInstruction{.input = Reg(1), .output = Reg(4)};
  const auto unused = BytecodeProgram::Create(uncalled);
  ASSERT_FALSE(unused.has_value());
  EXPECT_EQ(ProgramErrorCode::kUnreachableInstruction, unused.error().code);
  auto looping = SharedValueSubroutineInput();
  looping.subroutines[0].end = Address(12);
  looping.instructions.pop_back();
  looping.instructions[11] = JumpInstruction{.target = Address(11)};
  const auto no_return = BytecodeProgram::Create(looping);
  ASSERT_FALSE(no_return.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, no_return.error().code);
}

TEST(BytecodeProgramTest, ChargesSubroutineSummariesToExistingAnalysisBudgets) {
  const auto input = SharedValueSubroutineInput();
  const auto baseline = VerifyProgram(input);
  ASSERT_TRUE(baseline.has_value());
  ProgramLimits memory;
  memory.maximum_analysis_words = baseline->analysis_words;
  EXPECT_TRUE(VerifyProgram(input, memory).has_value());
  --memory.maximum_analysis_words;
  const auto exhausted_memory = VerifyProgram(input, memory);
  ASSERT_FALSE(exhausted_memory.has_value());
  EXPECT_EQ(ProgramErrorCode::kAnalysisLimitExceeded, exhausted_memory.error().code);
  ProgramLimits work;
  work.maximum_analysis_edge_words = baseline->processed_edge_words;
  EXPECT_TRUE(VerifyProgram(input, work).has_value());
  --work.maximum_analysis_edge_words;
  const auto exhausted_work = VerifyProgram(input, work);
  ASSERT_FALSE(exhausted_work.has_value());
  EXPECT_EQ(ProgramErrorCode::kAnalysisWorkLimitExceeded, exhausted_work.error().code);
}

TEST(BytecodeProgramTest, RejectsMutualCallsAndMismatchedReturns) {
  ProgramInput input;
  input.subroutines = {
      SubroutineDescriptor{.entry = Address(2), .end = Address(4)},
      SubroutineDescriptor{.entry = Address(4), .end = Address(6)},
  };
  input.instructions = {
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      HaltInstruction{},
      CallSubroutineInstruction{.subroutine = SubroutineId{1}},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{1}},
  };
  const auto cycle = BytecodeProgram::Create(input);
  ASSERT_FALSE(cycle.has_value());
  EXPECT_EQ(ProgramErrorCode::kRecursiveSubroutine, cycle.error().code);
  input.instructions[3] = ReturnSubroutineInstruction{.subroutine = SubroutineId{1}};
  const auto returned = BytecodeProgram::Create(input);
  ASSERT_FALSE(returned.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, returned.error().code);
}

TEST(BytecodeProgramTest, ResolvesSubroutineLabelsWithoutRelaxingBranchOwnership) {
  auto builder = ProgramBuilder::Create({}, Resources(1));
  ASSERT_TRUE(builder.has_value());
  const auto entry = builder->CreateLabel();
  const auto last = builder->CreateLabel();
  ASSERT_TRUE(entry.has_value());
  ASSERT_TRUE(last.has_value());
  const auto routine = builder->AddSubroutine(*entry, *last);
  ASSERT_TRUE(routine.has_value());
  const auto constant = builder->AddConstant(SqlValue::Integer(42));
  ASSERT_TRUE(constant.has_value());
  ASSERT_TRUE(builder->Append(CallSubroutineInstruction{.subroutine = *routine}).has_value());
  ASSERT_TRUE(builder->Append(ResultRowInstruction{.first = Reg(0), .count = 1}).has_value());
  ASSERT_TRUE(builder->Append(HaltInstruction{}).has_value());
  ASSERT_TRUE(builder->BindLabel(*entry).has_value());
  ASSERT_TRUE(builder->Append(LoadConstantInstruction{.constant = *constant, .output = Reg(0)})
                  .has_value());
  ASSERT_TRUE(builder->BindLabel(*last).has_value());
  ASSERT_TRUE(builder->Append(ReturnSubroutineInstruction{.subroutine = *routine}).has_value());
  const auto program = std::move(*builder).Build({IntegerResultColumn()});
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ((SubroutineDescriptor{.entry = Address(3), .end = Address(5)}),
            program->subroutine(*routine));
  EXPECT_EQ("call_subroutine", InstructionKindName(InstructionKind::kCallSubroutine));
  EXPECT_EQ("return_subroutine", InstructionKindName(InstructionKind::kReturnSubroutine));
  auto foreign_builder = ProgramBuilder::Create({}, Resources(0));
  ASSERT_TRUE(foreign_builder.has_value());
  const auto foreign = foreign_builder->CreateLabel();
  ASSERT_TRUE(foreign.has_value());
  const auto rejected = foreign_builder->AddSubroutine(*foreign, *entry);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ProgramErrorCode::kForeignLabel, rejected.error().code);
}

[[nodiscard]] ProgramInput PrivateReadSubroutineInput() {
  ProgramInput input;
  input.register_count = 3;
  input.constants.push_back(SqlValue::Integer(0));
  input.cursors.push_back(RowIdCursorDescriptor());
  input.cursors.push_back(RowIdCursorDescriptor());
  input.result_columns = {IntegerResultColumn("callee"), IntegerResultColumn("caller")};
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
  return input;
}

TEST(BytecodeProgramTest, VerifiesPrivateReadSubroutineWithoutChangingCallerCursorState) {
  const auto input = PrivateReadSubroutineInput();
  const auto program = BytecodeProgram::Create(input);
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(1U, program->verification_metrics().maximum_subroutine_depth);
}

TEST(BytecodeProgramTest, RejectsCrossOwnerCursorUse) {
  for (const std::size_t address : {7U, 9U}) {
    auto input = PrivateReadSubroutineInput();
    input.instructions[address] =
        address == 7U ? Instruction{OpenReadCursorInstruction{.cursor = Cursor(0)}}
                      : Instruction{ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(1)}};
    const auto rejected = BytecodeProgram::Create(input);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, rejected.error().code);
  }
  auto closed_caller = PrivateReadSubroutineInput();
  closed_caller.instructions[4] = CloseCursorInstruction{.cursor = Cursor(1)};
  const auto caller_use = BytecodeProgram::Create(closed_caller);
  ASSERT_FALSE(caller_use.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, caller_use.error().code);
}

TEST(BytecodeProgramTest, RequiresEveryReadSubroutineReturnToCloseItsCursor) {
  for (const std::size_t address : {10U, 13U}) {
    auto input = PrivateReadSubroutineInput();
    input.instructions[address] = CopyInstruction{.input = Reg(1), .output = Reg(1)};
    const auto rejected = BytecodeProgram::Create(input);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ProgramErrorCode::kCursorStateConflict, rejected.error().code);
  }
  auto unknown = PrivateReadSubroutineInput();
  unknown.subroutines[0].kind = static_cast<SubroutineKind>(255);
  const auto invalid_kind = BytecodeProgram::Create(unknown);
  ASSERT_FALSE(invalid_kind.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidEnumValue, invalid_kind.error().code);
  unknown.subroutines[0].kind = SubroutineKind::kValue;
  const auto not_opted_in = BytecodeProgram::Create(unknown);
  ASSERT_FALSE(not_opted_in.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, not_opted_in.error().code);
}

TEST(BytecodeProgramTest, RejectsIndirectReadEffectsThroughValueOnlySubroutines) {
  auto input = PrivateReadSubroutineInput();
  input.subroutines.push_back(SubroutineDescriptor{.entry = Address(15), .end = Address(17)});
  input.instructions[2] = CallSubroutineInstruction{.subroutine = SubroutineId{1}};
  input.instructions.push_back(CallSubroutineInstruction{.subroutine = SubroutineId{0}});
  input.instructions.push_back(ReturnSubroutineInstruction{.subroutine = SubroutineId{1}});
  const auto rejected = BytecodeProgram::Create(input);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, rejected.error().code);
}

TEST(BytecodeProgramTest, VerifiesNullRowsAsLogicalRowsAndRejectsClosedOrDisabledReads) {
  ProgramInput input;
  input.register_count = 1;
  input.cursors.push_back(RowIdCursorDescriptor());
  input.result_columns.push_back(IntegerResultColumn());
  input.instructions = {
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      NullRowInstruction{.cursor = Cursor(0)},
      ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(0)},
      ResultRowInstruction{.first = Reg(0), .count = 1},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  EXPECT_TRUE(BytecodeProgram::Create(input).has_value());
  EXPECT_EQ("null_row", InstructionKindName(InstructionKind::kNullRow));
  input.instructions[1] = NullRowInstruction{.cursor = Cursor(0), .enabled = false};
  const auto disabled = BytecodeProgram::Create(input);
  ASSERT_FALSE(disabled.has_value());
  EXPECT_EQ(ProgramErrorCode::kCursorNotPositioned, disabled.error().code);
  input.instructions[0] = NullRowInstruction{.cursor = Cursor(0)};
  const auto closed = BytecodeProgram::Create(input);
  ASSERT_FALSE(closed.has_value());
  EXPECT_EQ(ProgramErrorCode::kCursorNotOpen, closed.error().code);
  input.instructions[0] = OpenReadCursorInstruction{.cursor = Cursor(0)};
  input.instructions[1] = NullRowInstruction{.cursor = Cursor(99)};
  const auto invalid = BytecodeProgram::Create(input);
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidCursor, invalid.error().code);
}

TEST(BytecodeProgramTest, RejectsNullPublicationForAnyMutationCursorUse) {
  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kDelete;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.mutation_result.publishes_changes = true;
  input.cursors.push_back(RowIdCursorDescriptor());
  input.instructions = {
      OpenMutationCursorInstruction{.cursor = Cursor(0)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  ASSERT_TRUE(BytecodeProgram::Create(input).has_value());
  input.instructions.insert(input.instructions.begin() + 1,
                            NullRowInstruction{.cursor = Cursor(0)});
  const auto rejected = BytecodeProgram::Create(input);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidExecutionMetadata, rejected.error().code);
  input.cursors.push_back(RowIdCursorDescriptor());
  input.instructions = {
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      OpenMutationCursorInstruction{.cursor = Cursor(1)},
      NullRowInstruction{.cursor = Cursor(0)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      CloseCursorInstruction{.cursor = Cursor(1)},
      HaltInstruction{},
  };
  EXPECT_TRUE(BytecodeProgram::Create(input).has_value());
}

TEST(BytecodeProgramTest, MergesPhysicalAndPublishedNullRowsWithoutWeakeningUnpositionedChecks) {
  ProgramInput input;
  input.register_count = 2;
  input.parameter_count = 1;
  input.cursors.push_back(RowIdCursorDescriptor());
  input.result_columns.push_back(IntegerResultColumn());
  input.instructions = {
      LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      JumpIfInstruction{.condition = JumpCondition::kIfNull, .input = Reg(0), .target = Address(6)},
      RewindInstruction{.cursor = Cursor(0), .empty_target = Address(6)},
      ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(1)},
      JumpInstruction{.target = Address(8)},
      NullRowInstruction{.cursor = Cursor(0)},
      ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(1)},
      ResultRowInstruction{.first = Reg(1), .count = 1},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  EXPECT_TRUE(BytecodeProgram::Create(input).has_value());
  input.instructions[6] = NullRowInstruction{.cursor = Cursor(0), .enabled = false};
  const auto disabled = BytecodeProgram::Create(input);
  ASSERT_FALSE(disabled.has_value());
  EXPECT_EQ(ProgramErrorCode::kCursorNotPositioned, disabled.error().code);
}

[[nodiscard]] WriteCursorDescriptor IndexWriteCursorDescriptor() {
  return WriteCursorDescriptor{
      .root_page = RootPageNumber(3),
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
  };
}

[[nodiscard]] OrderingRecordDescriptor OrderingDescriptor() {
  return OrderingRecordDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              OrderingColumnMetadata{
                  .collation = Symbol(0),
                  .order = BytecodeSortOrder::kAscending,
                  .null_placement = BytecodeNullPlacement::kFirst,
              },
          },
  };
}

[[nodiscard]] ProgramInput IndexRefillProgramInput() {
  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kCreateIndex;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.write_effects = ProgramWriteEffects{.multi_write = true, .may_abort = true};
  input.register_count = 2;
  input.symbols.emplace_back("BINARY");
  input.constants.push_back(SqlValue::Integer(7));
  input.constants.push_back(SqlValue::Integer(1));
  WriteCursorDescriptor index = IndexWriteCursorDescriptor();
  index.root_page = RootPageNumber(0);
  index.pending_root = true;
  input.write_cursors.push_back(std::move(index));
  OrderingRecordDescriptor sorter = OrderingDescriptor();
  sorter.index_refill_cursor = WriteCursor(0);
  input.sorters.push_back(std::move(sorter));
  input.instructions = {
      CreateIndexRootInstruction{.cursor = WriteCursor(0), .output = Reg(0)},
      OpenSorterInstruction{.sorter = Sorter(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      InsertSorterInstruction{.sorter = Sorter(0), .first_value = Reg(0), .value_count = 2},
      RewindSorterInstruction{.sorter = Sorter(0), .empty_target = Address(8)},
      InsertSorterIndexInstruction{.sorter = Sorter(0)},
      NextSorterInstruction{.sorter = Sorter(0), .next_target = Address(6)},
      CloseWriteCursorInstruction{.cursor = WriteCursor(0)},
      CloseSorterInstruction{.sorter = Sorter(0)},
      HaltInstruction{},
  };
  return input;
}

[[nodiscard]] ProgramInput PrivateCapabilitySubroutineInput(
    std::span<const Instruction> body, std::span<const Instruction> caller_body = {}) {
  ProgramInput input;
  input.register_count = 1;
  input.constants.push_back(SqlValue::Integer(1));
  input.symbols = {"BINARY", "count"};
  input.sorters.push_back(OrderingDescriptor());
  input.top_ns.push_back(OrderingDescriptor());
  input.relations.push_back(OrderingDescriptor());
  input.aggregates.push_back(
      AggregateDescriptor{.function = Symbol(1), .collation = Symbol(0), .argument_count = 0});
  input.instructions.reserve(4U + caller_body.size() + body.size());
  input.instructions.push_back(LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)});
  input.instructions.insert(input.instructions.end(), caller_body.begin(), caller_body.end());
  input.instructions.push_back(CallSubroutineInstruction{.subroutine = SubroutineId{0}});
  input.instructions.push_back(HaltInstruction{});
  const auto entry = static_cast<std::uint32_t>(input.instructions.size());
  input.instructions.insert(input.instructions.end(), body.begin(), body.end());
  input.instructions.push_back(ReturnSubroutineInstruction{.subroutine = SubroutineId{0}});
  input.subroutines.push_back(SubroutineDescriptor{
      .entry = Address(entry),
      .end = Address(static_cast<std::uint32_t>(input.instructions.size())),
      .kind = SubroutineKind::kRead,
  });
  return input;
}

TEST(BytecodeProgramTest, AppliesPrivateOwnershipAndClosedReturnToEveryTemporaryCapability) {
  const std::array<std::vector<Instruction>, 4> bodies{
      std::vector<Instruction>{OpenSorterInstruction{.sorter = Sorter(0)},
                               CloseSorterInstruction{.sorter = Sorter(0)}},
      std::vector<Instruction>{OpenTopNInstruction{.top_n = TopN(0), .bound = Reg(0)},
                               CloseTopNInstruction{.top_n = TopN(0)}},
      std::vector<Instruction>{OpenRelationInstruction{.relation = Relation(0)},
                               CloseRelationInstruction{.relation = Relation(0)}},
      std::vector<Instruction>{
          OpenAggregateInstruction{.aggregate = Aggregate(0)},
          FinalizeAggregateInstruction{.aggregate = Aggregate(0), .output = Reg(0)},
          CloseAggregateInstruction{.aggregate = Aggregate(0)}},
  };
  for (const auto& body : bodies) {
    SCOPED_TRACE(InstructionKindName(InstructionKindOf(body.front())));
    const auto valid = PrivateCapabilitySubroutineInput(body);
    EXPECT_TRUE(BytecodeProgram::Create(valid).has_value());

    const auto shared = PrivateCapabilitySubroutineInput(body, body);
    const auto cross_owner = BytecodeProgram::Create(shared);
    ASSERT_FALSE(cross_owner.has_value());
    EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, cross_owner.error().code);

    const auto unclosed = PrivateCapabilitySubroutineInput(
        std::span<const Instruction>{body}.first(body.size() - 1U));
    const auto retained = BytecodeProgram::Create(unclosed);
    ASSERT_FALSE(retained.has_value());
    EXPECT_EQ(ProgramErrorCode::kCapabilityStateConflict, retained.error().code);
  }
}

TEST(BytecodeProgramTest, RejectsReadBodyGlobalAndClientControlOperations) {
  for (const Instruction instruction :
       {Instruction{ClearRowIdListInstruction{}}, Instruction{HaltInstruction{}},
        Instruction{ResultRowInstruction{.first = Reg(0), .count = 1}}}) {
    auto input = PrivateReadSubroutineInput();
    input.instructions[9] = instruction;
    if (std::holds_alternative<ResultRowInstruction>(instruction)) {
      input.result_columns.resize(1);
      input.instructions[5] = ResultRowInstruction{.first = Reg(1), .count = 1};
    }
    const auto rejected = BytecodeProgram::Create(input);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, rejected.error().code);
  }
}

TEST(BytecodeProgramTest, ChargesPrivateCapabilityOwnershipToExistingAnalysisLimits) {
  const auto input = PrivateReadSubroutineInput();
  const auto measured = VerifyProgram(input);
  ASSERT_TRUE(measured.has_value());
  ProgramLimits limits;
  limits.maximum_analysis_words = measured->analysis_words;
  EXPECT_TRUE(VerifyProgram(input, limits).has_value());
  --limits.maximum_analysis_words;
  const auto rejected = VerifyProgram(input, limits);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ProgramErrorCode::kAnalysisLimitExceeded, rejected.error().code);
}

TEST(BytecodeProgramTest, PublishesReadKindThroughBuilderAndDoesNotConsumeInvalidDeclarations) {
  auto builder = ProgramBuilder::Create({}, Resources(0));
  ASSERT_TRUE(builder.has_value());
  const auto entry = builder->CreateLabel();
  const auto last = builder->CreateLabel();
  ASSERT_TRUE(entry.has_value());
  ASSERT_TRUE(last.has_value());
  const auto invalid = builder->AddSubroutine(*entry, *last, static_cast<SubroutineKind>(255));
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidEnumValue, invalid.error().code);
  const auto routine = builder->AddSubroutine(*entry, *last, SubroutineKind::kRead);
  ASSERT_TRUE(routine.has_value());
  EXPECT_EQ(0U, routine->value());
  ASSERT_TRUE(builder->AddSymbol("BINARY").has_value());
  const auto sorter = builder->AddSorter(OrderingDescriptor());
  ASSERT_TRUE(sorter.has_value());
  ASSERT_TRUE(builder->Append(CallSubroutineInstruction{.subroutine = *routine}).has_value());
  ASSERT_TRUE(builder->Append(HaltInstruction{}).has_value());
  ASSERT_TRUE(builder->BindLabel(*entry).has_value());
  ASSERT_TRUE(builder->Append(OpenSorterInstruction{.sorter = *sorter}).has_value());
  ASSERT_TRUE(builder->Append(CloseSorterInstruction{.sorter = *sorter}).has_value());
  ASSERT_TRUE(builder->BindLabel(*last).has_value());
  ASSERT_TRUE(builder->Append(ReturnSubroutineInstruction{.subroutine = *routine}).has_value());
  const auto program = std::move(*builder).Build({});
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(SubroutineKind::kRead, program->subroutine(*routine).kind);
}

TEST(BytecodeProgramTest, RejectsReadBodyPersistentMutationEvenInAWritableMainProgram) {
  auto input = PrivateReadSubroutineInput();
  input.statement_kind = ProgramStatementKind::kDelete;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.mutation_result.publishes_changes = true;
  input.result_columns.clear();
  input.instructions[5] = CopyInstruction{.input = Reg(1), .output = Reg(2)};
  input.instructions[7] = OpenMutationCursorInstruction{.cursor = Cursor(1)};
  const auto rejected = BytecodeProgram::Create(input);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, rejected.error().code);
}

TEST(BytecodeProgramTest, RetainsMainHaltCleanupPolicyOutsidePrivateReadRegions) {
  ProgramInput input;
  input.symbols = {"BINARY"};
  input.sorters = {OrderingDescriptor(), OrderingDescriptor()};
  input.subroutines = {
      SubroutineDescriptor{.entry = Address(3), .end = Address(6), .kind = SubroutineKind::kRead}};
  input.instructions = {
      OpenSorterInstruction{.sorter = Sorter(0)},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      HaltInstruction{},
      OpenSorterInstruction{.sorter = Sorter(1)},
      CloseSorterInstruction{.sorter = Sorter(1)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
  };
  EXPECT_TRUE(BytecodeProgram::Create(input).has_value());
}

[[nodiscard]] OrderingRecordDescriptor RecordComparisonDescriptor() {
  OrderingRecordDescriptor descriptor = OrderingDescriptor();
  descriptor.key_field_count = 2;
  descriptor.key_columns.push_back(OrderingColumnMetadata{
      .collation = Symbol(0),
      .order = BytecodeSortOrder::kDescending,
      .null_placement = BytecodeNullPlacement::kLast,
  });
  return descriptor;
}

[[nodiscard]] ProgramErrorCode VerifyError(const ProgramInput& input, ProgramLimits limits = {}) {
  const auto verified = VerifyProgram(input, limits);
  EXPECT_FALSE(verified.has_value());
  return verified.error().code;
}

[[nodiscard]] OrderingRecordDescriptor LookupRelationDescriptor() {
  OrderingRecordDescriptor descriptor = RecordComparisonDescriptor();
  descriptor.relation_purpose = RelationPurpose::kLookup;
  return descriptor;
}

[[nodiscard]] ReadCursorDescriptor LookupCursorDescriptor() {
  return ReadCursorDescriptor{
      .root_page = RootPageNumber(0),
      .storage = CursorStorageKind::kEphemeralIndex,
      .record_field_count = 2,
      .fields =
          {
              CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0},
              CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 1},
          },
      .index_columns = {},
      .relation = Relation(0),
  };
}

[[nodiscard]] ProgramInput LookupRelationInput() {
  ProgramInput input;
  input.register_count = 3;
  input.constants.push_back(SqlValue::Integer(1));
  input.symbols.emplace_back("BINARY");
  input.relations.push_back(LookupRelationDescriptor());
  input.cursors.push_back(LookupCursorDescriptor());
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      OpenRelationInstruction{.relation = Relation(0)},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      CheckRelationLookupInstruction{
          .relation = Relation(0), .ready_target = Address(8), .empty_target = Address(12)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = RelationInsertMode::kKeepExisting,
          .duplicate_target = Address(6),
      },
      SealRelationInstruction{.relation = Relation(0)},
      JumpInstruction{.target = Address(4)},
      SeekRelationPrefixInstruction{
          .cursor = Cursor(0), .first_key = Reg(0), .key_count = 1, .missing_target = Address(12)},
      ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(2)},
      NextRelationPrefixInstruction{
          .cursor = Cursor(0), .first_key = Reg(0), .key_count = 1, .next_target = Address(9)},
      JumpInstruction{.target = Address(4)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      CloseRelationInstruction{.relation = Relation(0)},
      HaltInstruction{},
  };
  return input;
}

TEST(BytecodeProgramTest, DoesNotRequirePersistentSnapshotForLookupOnlyProgram) {
  auto program = BytecodeProgram::Create(LookupRelationInput());
  ASSERT_TRUE(program.has_value());
  EXPECT_FALSE(program->requires_database_snapshot());
  EXPECT_EQ(RelationPurpose::kLookup, program->relation(Relation(0)).relation_purpose);
  EXPECT_EQ(Relation(0), program->cursor(Cursor(0)).relation);
  EXPECT_EQ(Cursor(0), program->relation_borrower(Relation(0)));
  EXPECT_EQ("check_relation_lookup", InstructionKindName(InstructionKind::kCheckRelationLookup));
  EXPECT_EQ("seal_relation", InstructionKindName(InstructionKind::kSealRelation));
  EXPECT_EQ("seek_relation_prefix", InstructionKindName(InstructionKind::kSeekRelationPrefix));
  EXPECT_EQ("next_relation_prefix", InstructionKindName(InstructionKind::kNextRelationPrefix));
}

TEST(BytecodeProgramTest, KeepsBorrowedLookupDescriptorsAndCapabilitiesConsistent) {
  const auto reject = [](const ProgramInput& input) {
    const auto rejected = BytecodeProgram::Create(input);
    EXPECT_FALSE(rejected.has_value());
  };
  auto duplicate = LookupRelationInput();
  duplicate.cursors.push_back(LookupCursorDescriptor());
  reject(duplicate);

  auto root = LookupRelationInput();
  root.cursors[0].root_page = RootPageNumber(2);
  reject(root);

  auto owner = LookupRelationInput();
  owner.cursors[0].relation = Relation(1);
  reject(owner);
  owner.cursors[0].relation.reset();
  reject(owner);

  auto shape = LookupRelationInput();
  shape.cursors[0].record_field_count = 3;
  reject(shape);

  auto rowid = LookupRelationInput();
  rowid.cursors[0].fields[1].kind = CursorFieldSourceKind::kRowId;
  rowid.cursors[0].fields[1].record_field = 0;
  reject(rowid);

  auto metadata = LookupRelationInput();
  metadata.cursors[0].index_columns.push_back(
      IndexColumnMetadata{.collation = Symbol(0), .order = BytecodeSortOrder::kAscending});
  reject(metadata);

  auto purpose = LookupRelationInput();
  purpose.relations[0].relation_purpose = RelationPurpose::kMembership;
  reject(purpose);
  purpose.relations[0].relation_purpose = static_cast<RelationPurpose>(255);
  reject(purpose);

  auto prefix_only = LookupRelationInput();
  prefix_only.relations[0].key_field_count = 1;
  prefix_only.relations[0].key_columns.pop_back();
  reject(prefix_only);

  for (const CursorStorageKind kind : {CursorStorageKind::kRowIdTable, CursorStorageKind::kIndex}) {
    auto persistent = LookupRelationInput();
    persistent.cursors[0] =
        kind == CursorStorageKind::kRowIdTable ? RowIdCursorDescriptor() : IndexCursorDescriptor();
    persistent.cursors[0].relation = Relation(0);
    reject(persistent);
  }
  for (std::size_t kind = 0; kind < 3U; ++kind) {
    ProgramInput misplaced;
    misplaced.symbols.emplace_back("BINARY");
    misplaced.instructions = {HaltInstruction{}};
    if (kind == 0U) {
      misplaced.sorters.push_back(LookupRelationDescriptor());
    } else if (kind == 1U) {
      misplaced.top_ns.push_back(LookupRelationDescriptor());
    } else {
      misplaced.record_comparisons.push_back(LookupRelationDescriptor());
    }
    reject(misplaced);
  }
}

TEST(BytecodeProgramTest, RejectsUnsafeBorrowedLookupOperationsAndUninitializedKeys) {
  const std::array<std::pair<Instruction, ProgramErrorCode>, 10> invalid{
      std::pair{Instruction{ResetRelationInstruction{.relation = Relation(0)}},
                ProgramErrorCode::kCursorStateConflict},
      std::pair{Instruction{CloseRelationInstruction{.relation = Relation(0)}},
                ProgramErrorCode::kCursorStateConflict},
      std::pair{Instruction{SealRelationInstruction{.relation = Relation(0)}},
                ProgramErrorCode::kCapabilityNotWriting},
      std::pair{Instruction{ContainsRelationInstruction{.relation = Relation(0),
                                                        .first_key = Reg(0),
                                                        .key_count = 2,
                                                        .found_target = Address(12)}},
                ProgramErrorCode::kCapabilityNotWriting},
      std::pair{Instruction{DeleteRelationInstruction{
                    .relation = Relation(0), .first_key = Reg(0), .key_count = 2}},
                ProgramErrorCode::kCapabilityNotWriting},
      std::pair{Instruction{RewindRelationInstruction{.relation = Relation(0),
                                                      .empty_target = Address(12)}},
                ProgramErrorCode::kInvalidOrderingDescriptor},
      std::pair{Instruction{RewindInstruction{.cursor = Cursor(0), .empty_target = Address(12)}},
                ProgramErrorCode::kInvalidCursorDescriptor},
      std::pair{Instruction{
                    ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(2)}},
                ProgramErrorCode::kCursorNotPositioned},
      std::pair{
          Instruction{NextRelationPrefixInstruction{
              .cursor = Cursor(0), .first_key = Reg(0), .key_count = 1, .next_target = Address(9)}},
          ProgramErrorCode::kCursorNotPositioned},
      std::pair{Instruction{SeekRelationPrefixInstruction{.cursor = Cursor(0),
                                                          .first_key = Reg(2),
                                                          .key_count = 1,
                                                          .missing_target = Address(12)}},
                ProgramErrorCode::kUninitializedRegister},
  };
  for (const auto& [instruction, expected] : invalid) {
    SCOPED_TRACE(InstructionKindName(InstructionKindOf(instruction)));
    auto input = LookupRelationInput();
    input.instructions[8] = instruction;
    const auto rejected = VerifyProgram(input);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(expected, rejected.error().code);
  }
  for (const std::uint32_t width : {0U, 3U}) {
    auto input = LookupRelationInput();
    std::get<SeekRelationPrefixInstruction>(input.instructions[8]).key_count = width;
    const auto rejected = VerifyProgram(input);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ProgramErrorCode::kInvalidRegisterRange, rejected.error().code);
  }
  auto before_owner = LookupRelationInput();
  std::swap(before_owner.instructions[2], before_owner.instructions[3]);
  const auto rejected = VerifyProgram(before_owner);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ProgramErrorCode::kCapabilityNotOpen, rejected.error().code);
}

TEST(BytecodeProgramTest, VerifiesWritingLookupNullRowsAndRequiresOnePrivateOwner) {
  const std::array<Instruction, 7> body{
      OpenRelationInstruction{.relation = Relation(0)},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      NullRowInstruction{.cursor = Cursor(0)},
      ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(0)},
      NullRowInstruction{.cursor = Cursor(0), .enabled = false},
      CloseCursorInstruction{.cursor = Cursor(0)},
      CloseRelationInstruction{.relation = Relation(0)},
  };
  auto valid = PrivateCapabilitySubroutineInput(body);
  valid.relations[0] = LookupRelationDescriptor();
  valid.cursors.push_back(LookupCursorDescriptor());
  EXPECT_TRUE(BytecodeProgram::Create(valid).has_value());

  const std::array<Instruction, 2> relation{
      OpenRelationInstruction{.relation = Relation(0)},
      CloseRelationInstruction{.relation = Relation(0)},
  };
  const std::array<Instruction, 2> cursor{
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      CloseCursorInstruction{.cursor = Cursor(0)},
  };
  for (const bool caller_owns_relation : {false, true}) {
    auto input = caller_owns_relation ? PrivateCapabilitySubroutineInput(cursor, relation)
                                      : PrivateCapabilitySubroutineInput(relation, cursor);
    input.relations[0] = LookupRelationDescriptor();
    input.cursors.push_back(LookupCursorDescriptor());
    const auto rejected = BytecodeProgram::Create(input);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, rejected.error().code);
  }
}

TEST(BytecodeProgramTest, ChargesBorrowerMapAndLookupFlowToExistingAnalysisLimits) {
  const ProgramInput input = LookupRelationInput();
  const auto measured = VerifyProgram(input);
  ASSERT_TRUE(measured.has_value());
  ProgramLimits memory;
  memory.maximum_analysis_words = measured->analysis_words;
  EXPECT_TRUE(VerifyProgram(input, memory).has_value());
  --memory.maximum_analysis_words;
  EXPECT_EQ(ProgramErrorCode::kAnalysisLimitExceeded, VerifyError(input, memory));
  ProgramLimits work;
  work.maximum_analysis_edge_words = measured->processed_edge_words;
  EXPECT_TRUE(VerifyProgram(input, work).has_value());
  --work.maximum_analysis_edge_words;
  EXPECT_EQ(ProgramErrorCode::kAnalysisWorkLimitExceeded, VerifyError(input, work));
}

TEST(BytecodeProgramTest, BuilderResolvesLookupBranchesWithoutAcceptingNumericTargets) {
  auto builder = ProgramBuilder::Create({}, Resources(3));
  ASSERT_TRUE(builder.has_value());
  ASSERT_TRUE(builder->AddSymbol("BINARY").has_value());
  ASSERT_TRUE(builder->AddRelation(LookupRelationDescriptor()).has_value());
  ASSERT_TRUE(builder->AddCursor(LookupCursorDescriptor()).has_value());
  ASSERT_TRUE(builder->AddConstant(SqlValue::Integer(1)).has_value());
  const auto check = builder->CreateLabel();
  const auto inserted = builder->CreateLabel();
  const auto ready = builder->CreateLabel();
  const auto row = builder->CreateLabel();
  const auto done = builder->CreateLabel();
  ASSERT_TRUE(check.has_value() && inserted.has_value() && ready.has_value() && row.has_value() &&
              done.has_value());
  ASSERT_TRUE(builder->Append(LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)}));
  ASSERT_TRUE(builder->Append(LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)}));
  ASSERT_TRUE(builder->Append(OpenRelationInstruction{.relation = Relation(0)}));
  ASSERT_TRUE(builder->Append(OpenReadCursorInstruction{.cursor = Cursor(0)}));
  ASSERT_TRUE(builder->BindLabel(*check));
  ASSERT_TRUE(builder->EmitCheckRelationLookup(Relation(0), *ready, *done));
  ASSERT_TRUE(builder->EmitInsertRelation(Relation(0), Reg(0), 2, RelationInsertMode::kKeepExisting,
                                          *inserted));
  ASSERT_TRUE(builder->BindLabel(*inserted));
  ASSERT_TRUE(builder->Append(SealRelationInstruction{.relation = Relation(0)}));
  ASSERT_TRUE(builder->EmitJump(*check));
  ASSERT_TRUE(builder->BindLabel(*ready));
  ASSERT_TRUE(builder->EmitSeekRelationPrefix(Cursor(0), Reg(0), 1, *done));
  ASSERT_TRUE(builder->BindLabel(*row));
  ASSERT_TRUE(builder->Append(
      ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(2)}));
  ASSERT_TRUE(builder->EmitNextRelationPrefix(Cursor(0), Reg(0), 1, *row));
  ASSERT_TRUE(builder->BindLabel(*done));
  ASSERT_TRUE(builder->Append(CloseCursorInstruction{.cursor = Cursor(0)}));
  ASSERT_TRUE(builder->Append(CloseRelationInstruction{.relation = Relation(0)}));
  ASSERT_TRUE(builder->Append(HaltInstruction{}));
  auto program = std::move(*builder).Build({});
  ASSERT_TRUE(program.has_value());
  EXPECT_FALSE(program->requires_database_snapshot());
  const auto& checked = std::get<CheckRelationLookupInstruction>(program->instructions()[4]);
  EXPECT_EQ(Address(8), checked.ready_target);
  EXPECT_EQ(Address(11), checked.empty_target);
  EXPECT_EQ(Address(11),
            std::get<SeekRelationPrefixInstruction>(program->instructions()[8]).missing_target);
  EXPECT_EQ(Address(9),
            std::get<NextRelationPrefixInstruction>(program->instructions()[10]).next_target);

  for (const Instruction instruction :
       {Instruction{CheckRelationLookupInstruction{
            .relation = Relation(0), .ready_target = Address(0), .empty_target = Address(0)}},
        Instruction{SeekRelationPrefixInstruction{.cursor = Cursor(0),
                                                  .first_key = Reg(0),
                                                  .key_count = 1,
                                                  .missing_target = Address(0)}},
        Instruction{NextRelationPrefixInstruction{.cursor = Cursor(0),
                                                  .first_key = Reg(0),
                                                  .key_count = 1,
                                                  .next_target = Address(0)}}}) {
    auto numeric = ProgramBuilder::Create({}, Resources(1));
    ASSERT_TRUE(numeric.has_value());
    const auto rejected = numeric->Append(instruction);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ProgramErrorCode::kBranchRequiresLabel, rejected.error().code);
  }
}

TEST(BytecodeProgramTest, UsesStrongIdsAndStableInstructionMetadata) {
  static_assert(!std::is_convertible_v<std::uint32_t, RegisterId>);
  static_assert(!std::is_convertible_v<RegisterId, CursorId>);
  static_assert(!std::is_convertible_v<AggregateId, RelationId>);
  static_assert(sizeof(Instruction) <= 32);

  const std::array<Instruction, 25> instructions = {
      HaltInstruction{},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
      CopyInstruction{.input = Reg(0), .output = Reg(1)},
      UnaryInstruction{
          .operation = UnaryOperation::kNegate,
          .input = Reg(0),
          .output = Reg(1),
      },
      BinaryInstruction{
          .operation = BinaryOperation::kAdd,
          .left = Reg(0),
          .right = Reg(1),
          .output = Reg(2),
      },
      ApplyAffinityInstruction{
          .input = Reg(0),
          .affinity = TypeAffinity::kNumeric,
          .output = Reg(1),
      },
      MustBeIntegerInstruction{.input = Reg(0), .output = Reg(1)},
      RealAffinityInstruction{.input = Reg(0), .output = Reg(1)},
      RealStorageAffinityInstruction{.input = Reg(0), .output = Reg(1)},
      CastInstruction{
          .input = Reg(0),
          .target = CastTarget::kText,
          .output = Reg(1),
      },
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      RewindInstruction{.cursor = Cursor(0), .empty_target = Address(1)},
      NextInstruction{.cursor = Cursor(0), .next_target = Address(1)},
      LastInstruction{.cursor = Cursor(0), .empty_target = Address(1)},
      PreviousInstruction{.cursor = Cursor(0), .previous_target = Address(1)},
      SeekRowIdInstruction{
          .cursor = Cursor(0),
          .key = Reg(0),
          .missing_target = Address(1),
      },
      ReadFieldInstruction{
          .cursor = Cursor(0),
          .field = Field(0),
          .output = Reg(0),
      },
      ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(0)},
      CompareInstruction{
          .comparison = SqlComparison::kEqual,
          .affinity = TypeAffinity::kNumeric,
          .collation = Symbol(0),
          .left = Reg(0),
          .right = Reg(1),
          .output = Reg(2),
      },
      CallScalarInstruction{
          .function = Symbol(0),
          .collation = Symbol(1),
          .first_argument = Reg(0),
          .argument_count = 2,
          .output = Reg(2),
      },
      JumpInstruction{.target = Address(1)},
      JumpIfInstruction{
          .condition = JumpCondition::kIfTrue,
          .input = Reg(0),
          .target = Address(1),
      },
      ResultRowInstruction{.first = Reg(0), .count = 1},
  };
  const std::array<std::string_view, 25> names = {
      "halt",           "load_constant",
      "load_parameter", "copy",
      "unary",          "binary",
      "apply_affinity", "must_be_integer",
      "real_affinity",  "real_storage_affinity",
      "cast",           "open_read",
      "close",          "rewind",
      "next",           "last",
      "previous",       "seek_rowid",
      "read_field",     "read_rowid",
      "compare",        "call_scalar",
      "jump",           "jump_if",
      "result_row",
  };

  for (std::size_t index = 0; index < instructions.size(); ++index) {
    EXPECT_EQ(InstructionKindName(InstructionKindOf(instructions[index])), names[index]);
  }
}

TEST(BytecodeProgramTest, VerifiesTypedTableInsertInstructions) {
  const std::array<Instruction, 5> instructions{
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
  };
  const std::array<std::string_view, 5> names{
      "open_write", "resolve_insert_rowid", "build_table_record", "insert_table", "close_write",
  };
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    EXPECT_EQ(names[index], InstructionKindName(InstructionKindOf(instructions[index])));
  }

  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kInsert;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.mutation_result = MutationResultMetadata{
      .publishes_changes = true,
      .publishes_last_insert_rowid = true,
  };
  input.register_count = 4;
  input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(2),
      .columns =
          {
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kInteger,
                  .not_null = false,
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
  input.constants.emplace_back();
  input.constants.push_back(SqlValue::Text("value"));
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(2)},
      instructions[0],
      instructions[1],
      instructions[2],
      instructions[3],
      instructions[4],
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(input).has_value());

  input.instructions[2] = OpenWriteCursorInstruction{.cursor = WriteCursor(1)};
  EXPECT_EQ(ProgramErrorCode::kInvalidCursor, VerifyError(input));
  input.instructions[2] = instructions[0];

  std::get<BuildTableRecordInstruction>(input.instructions[4]).value_count = 1;
  EXPECT_EQ(ProgramErrorCode::kInvalidRegisterRange, VerifyError(input));
  input.instructions[4] = instructions[2];

  std::get<BuildTableRecordInstruction>(input.instructions[4]).affinity_mode =
      static_cast<TableRecordAffinityMode>(255);  // NOLINT
  EXPECT_EQ(ProgramErrorCode::kInvalidEnumValue, VerifyError(input));
  input.instructions[4] = instructions[2];

  std::get<ResolveInsertRowIdInstruction>(input.instructions[3]).input = Reg(3);
  EXPECT_EQ(ProgramErrorCode::kUninitializedRegister, VerifyError(input));
  input.instructions[3] = instructions[1];

  input.instructions.erase(input.instructions.begin() + 2);
  EXPECT_EQ(ProgramErrorCode::kCursorNotOpen, VerifyError(input));
}

TEST(BytecodeProgramTest, VerifiesTypedTableDeleteAndRowidSeekModes) {
  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kDelete;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.mutation_result.publishes_changes = true;
  input.register_count = 1;
  input.constants.push_back(SqlValue::Integer(7));
  input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(2),
      .columns =
          {
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kText,
                  .not_null = false,
                  .rowid_alias = false,
                  .default_value = std::nullopt,
              },
          },
      .rowid_alias = std::nullopt,
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
  EXPECT_TRUE(VerifyProgram(input).has_value());
  EXPECT_EQ("delete_table", InstructionKindName(InstructionKindOf(input.instructions[2])));

  input.instructions.erase(input.instructions.begin() + 1);
  EXPECT_EQ(ProgramErrorCode::kCursorNotOpen, VerifyError(input));

  ProgramInput scan;
  scan.statement_kind = ProgramStatementKind::kDelete;
  scan.transaction_access = ProgramTransactionAccess::kWrite;
  scan.rollback_mode = ProgramRollbackMode::kStatement;
  scan.mutation_result.publishes_changes = true;
  scan.cursors.push_back(RowIdCursorDescriptor());
  scan.instructions = {
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
  EXPECT_TRUE(VerifyProgram(scan).has_value());
  EXPECT_EQ("open_mutation", InstructionKindName(InstructionKindOf(scan.instructions[0])));
  EXPECT_EQ("delete_current_table", InstructionKindName(InstructionKindOf(scan.instructions[2])));

  scan.instructions.erase(scan.instructions.begin() + 1);
  std::get<DeleteCurrentTableInstruction>(scan.instructions[1]).exhausted_target = Address(3);
  EXPECT_EQ(ProgramErrorCode::kCursorNotPositioned, VerifyError(scan));

  ProgramInput seek;
  seek.register_count = 1;
  seek.constants.push_back(SqlValue::Integer(7));
  seek.cursors.push_back(RowIdCursorDescriptor());
  seek.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      SeekRowIdInstruction{
          .cursor = Cursor(0),
          .key = Reg(0),
          .missing_target = Address(5),
          .mode = RowIdSeekMode::kGreater,
      },
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  constexpr std::array modes{
      RowIdSeekMode::kEqual,       RowIdSeekMode::kGreaterOrEqual, RowIdSeekMode::kGreater,
      RowIdSeekMode::kLessOrEqual, RowIdSeekMode::kLess,
  };
  for (const RowIdSeekMode mode : modes) {
    std::get<SeekRowIdInstruction>(seek.instructions[2]).mode = mode;
    EXPECT_TRUE(VerifyProgram(seek).has_value());
  }

  std::get<SeekRowIdInstruction>(seek.instructions[2]).mode =
      static_cast<RowIdSeekMode>(255);  // NOLINT
  EXPECT_EQ(ProgramErrorCode::kInvalidEnumValue, VerifyError(seek));
}

TEST(BytecodeProgramTest, VerifiesTypedTableUpdateInstruction) {
  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kUpdate;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.mutation_result.publishes_changes = true;
  input.register_count = 3;
  input.constants.push_back(SqlValue::Integer(7));
  input.constants.push_back(SqlValue::Integer(8));
  const std::array record_bytes{std::byte{2}, std::byte{0}};
  input.constants.push_back(SqlValue::Blob(ByteBuffer::CopyOf(record_bytes)));
  input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(2),
      .columns =
          {
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kInteger,
                  .not_null = false,
                  .rowid_alias = true,
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
      UpdateTableInstruction{
          .cursor = WriteCursor(0),
          .old_rowid = Reg(0),
          .new_rowid = Reg(1),
          .record = Reg(2),
      },
      CloseWriteCursorInstruction{.cursor = WriteCursor(0)},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(input).has_value());
  EXPECT_EQ("check_update_rowid", InstructionKindName(InstructionKindOf(input.instructions[4])));
  EXPECT_EQ("update_table", InstructionKindName(InstructionKindOf(input.instructions[5])));

  input.instructions.erase(input.instructions.begin() + 3);
  EXPECT_EQ(ProgramErrorCode::kCursorNotOpen, VerifyError(input));

  ProgramInput scan;
  scan.statement_kind = ProgramStatementKind::kUpdate;
  scan.transaction_access = ProgramTransactionAccess::kWrite;
  scan.rollback_mode = ProgramRollbackMode::kStatement;
  scan.mutation_result.publishes_changes = true;
  scan.register_count = 1;
  scan.constants.push_back(SqlValue::Blob(ByteBuffer::CopyOf(record_bytes)));
  scan.cursors.push_back(RowIdCursorDescriptor());
  scan.instructions = {
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
  EXPECT_TRUE(VerifyProgram(scan).has_value());
  EXPECT_EQ("update_current_table", InstructionKindName(InstructionKindOf(scan.instructions[3])));

  scan.instructions.erase(scan.instructions.begin() + 2);
  EXPECT_EQ(ProgramErrorCode::kCursorNotPositioned, VerifyError(scan));
}

TEST(BytecodeProgramTest, VerifiesTypedIndexWriteInstructions) {
  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kInsert;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.mutation_result = MutationResultMetadata{
      .publishes_changes = true,
      .publishes_last_insert_rowid = true,
  };
  input.register_count = 2;
  input.constants.push_back(SqlValue::Text("key"));
  input.constants.push_back(SqlValue::Integer(7));
  input.symbols.emplace_back("BINARY");
  input.write_cursors.push_back(IndexWriteCursorDescriptor());
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      OpenWriteCursorInstruction{.cursor = WriteCursor(0)},
      CheckUniqueIndexInstruction{
          .cursor = WriteCursor(0),
          .first_key = Reg(0),
          .key_count = 1,
          .ignored_rowid = std::nullopt,
      },
      InsertIndexInstruction{
          .cursor = WriteCursor(0),
          .first_value = Reg(0),
          .value_count = 2,
      },
      CloseWriteCursorInstruction{.cursor = WriteCursor(0)},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(input).has_value());
  EXPECT_EQ("check_unique_index", InstructionKindName(InstructionKindOf(input.instructions[3])));
  EXPECT_EQ("insert_index", InstructionKindName(InstructionKindOf(input.instructions[4])));

  std::get<CheckUniqueIndexInstruction>(input.instructions[3]).key_count = 2;
  EXPECT_EQ(ProgramErrorCode::kInvalidRegisterRange, VerifyError(input));
  std::get<CheckUniqueIndexInstruction>(input.instructions[3]).key_count = 1;
  std::get<InsertIndexInstruction>(input.instructions[4]).value_count = 1;
  EXPECT_EQ(ProgramErrorCode::kInvalidRegisterRange, VerifyError(input));

  ProgramInput deletion;
  deletion.statement_kind = ProgramStatementKind::kDelete;
  deletion.transaction_access = ProgramTransactionAccess::kWrite;
  deletion.rollback_mode = ProgramRollbackMode::kStatement;
  deletion.mutation_result.publishes_changes = true;
  deletion.register_count = 2;
  deletion.constants.push_back(SqlValue::Text("key"));
  deletion.constants.push_back(SqlValue::Integer(7));
  deletion.symbols.emplace_back("BINARY");
  deletion.write_cursors.push_back(IndexWriteCursorDescriptor());
  deletion.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      OpenWriteCursorInstruction{.cursor = WriteCursor(0)},
      DeleteIndexInstruction{
          .cursor = WriteCursor(0),
          .first_value = Reg(0),
          .value_count = 2,
      },
      CloseWriteCursorInstruction{.cursor = WriteCursor(0)},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(deletion).has_value());
  EXPECT_EQ("delete_index", InstructionKindName(InstructionKindOf(deletion.instructions[3])));
}

TEST(BytecodeProgramTest, VerifiesTypedIndexSeekAndRangeInstructions) {
  ProgramInput input;
  input.register_count = 3;
  input.constants.push_back(SqlValue::Text("bin-05"));
  input.symbols.emplace_back("BINARY");
  auto descriptor = IndexCursorDescriptor();
  descriptor.fields.push_back(CursorFieldSource{
      .kind = CursorFieldSourceKind::kRecordField,
      .record_field = 1,
  });
  input.cursors.push_back(std::move(descriptor));
  input.result_columns = {
      IntegerResultColumn("key"),
      IntegerResultColumn("rowid"),
  };
  input.instructions = {
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
          .mode = IndexRangeEndMode::kInclusive,
      },
      ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(1)},
      ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(2)},
      ResultRowInstruction{.first = Reg(1), .count = 2},
      NextInstruction{.cursor = Cursor(0), .next_target = Address(3)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(input).has_value());
  EXPECT_EQ("seek_index", InstructionKindName(InstructionKindOf(input.instructions[2])));
  EXPECT_EQ("check_index_range", InstructionKindName(InstructionKindOf(input.instructions[3])));

  std::get<ReadFieldInstruction>(input.instructions[4]).missing_value_affinity =
      static_cast<TypeAffinity>(255);  // NOLINT
  EXPECT_EQ(ProgramErrorCode::kInvalidEnumValue, VerifyError(input));
  std::get<ReadFieldInstruction>(input.instructions[4]).missing_value_affinity = std::nullopt;

  std::get<SeekIndexInstruction>(input.instructions[2]).key_count = 0;
  EXPECT_EQ(ProgramErrorCode::kInvalidRegisterRange, VerifyError(input));
  std::get<SeekIndexInstruction>(input.instructions[2]).key_count = 3;
  EXPECT_EQ(ProgramErrorCode::kInvalidRegisterRange, VerifyError(input));
  std::get<SeekIndexInstruction>(input.instructions[2]).key_count = 1;
  std::get<SeekIndexInstruction>(input.instructions[2]).mode =
      static_cast<IndexSeekMode>(255);  // NOLINT
  EXPECT_EQ(ProgramErrorCode::kInvalidEnumValue, VerifyError(input));
  std::get<SeekIndexInstruction>(input.instructions[2]).mode = IndexSeekMode::kGreaterOrEqual;
  std::get<CheckIndexRangeInstruction>(input.instructions[3]).key_count = 3;
  EXPECT_EQ(ProgramErrorCode::kInvalidRegisterRange, VerifyError(input));
  std::get<CheckIndexRangeInstruction>(input.instructions[3]).key_count = 1;
  std::get<CheckIndexRangeInstruction>(input.instructions[3]).mode =
      static_cast<IndexRangeEndMode>(255);  // NOLINT
  EXPECT_EQ(ProgramErrorCode::kInvalidEnumValue, VerifyError(input));
  std::get<CheckIndexRangeInstruction>(input.instructions[3]).mode = IndexRangeEndMode::kInclusive;

  input.instructions.erase(input.instructions.begin() + 2);
  EXPECT_EQ(ProgramErrorCode::kCursorNotPositioned, VerifyError(input));
}

TEST(BytecodeProgramTest, TreatsIndexRangeEndAsUnpositioned) {
  ProgramInput invalid;
  invalid.register_count = 2;
  invalid.constants.push_back(SqlValue::Text("bin-05"));
  invalid.symbols.emplace_back("BINARY");
  invalid.cursors.push_back(IndexCursorDescriptor());
  invalid.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      NullRowInstruction{.cursor = Cursor(0)},
      CheckIndexRangeInstruction{
          .cursor = Cursor(0),
          .first_key = Reg(0),
          .key_count = 1,
          .end_target = Address(6),
          .mode = IndexRangeEndMode::kInclusive,
      },
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
      ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(1)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  const auto rejected = VerifyProgram(invalid);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ProgramErrorCode::kCursorNotPositioned, rejected.error().code);

  ProgramInput merged;
  merged.register_count = 2;
  merged.constants.push_back(SqlValue::Text("bin-05"));
  merged.symbols.emplace_back("BINARY");
  merged.cursors.push_back(IndexCursorDescriptor());
  merged.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      SeekIndexInstruction{
          .cursor = Cursor(0),
          .first_key = Reg(0),
          .key_count = 1,
          .missing_target = Address(7),
          .mode = IndexSeekMode::kGreaterOrEqual,
      },
      CheckIndexRangeInstruction{
          .cursor = Cursor(0),
          .first_key = Reg(0),
          .key_count = 1,
          .end_target = Address(7),
          .mode = IndexRangeEndMode::kInclusive,
      },
      ReadFieldInstruction{.cursor = Cursor(0), .field = Field(0), .output = Reg(1)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
      NullRowInstruction{.cursor = Cursor(0), .enabled = false},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(merged).has_value());
}

TEST(BytecodeProgramTest, VerifiesIndexRowidTableLookupInstructions) {
  ProgramInput input;
  input.register_count = 2;
  input.symbols.emplace_back("BINARY");
  auto index = IndexCursorDescriptor();
  index.fields.push_back(CursorFieldSource{
      .kind = CursorFieldSourceKind::kRecordField,
      .record_field = 1,
  });
  input.cursors.push_back(std::move(index));
  input.cursors.push_back(RowIdCursorDescriptor());
  input.result_columns.push_back(IntegerResultColumn());
  input.instructions = {
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      RewindInstruction{
          .cursor = Cursor(0),
          .empty_target = Address(8),
      },
      ReadFieldInstruction{.cursor = Cursor(0), .field = Field(1), .output = Reg(0)},
      OpenReadCursorInstruction{.cursor = Cursor(1)},
      SeekTableRowIdInstruction{.cursor = Cursor(1), .key = Reg(0)},
      ReadRowIdInstruction{.cursor = Cursor(1), .output = Reg(1)},
      ResultRowInstruction{.first = Reg(1), .count = 1},
      HaltInstruction{},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(input).has_value());
  EXPECT_EQ("seek_table_rowid", InstructionKindName(InstructionKindOf(input.instructions[4])));

  std::get<SeekTableRowIdInstruction>(input.instructions[4]).cursor = Cursor(0);
  EXPECT_EQ(ProgramErrorCode::kRowIdOperationRequiresRowIdTable, VerifyError(input));
}

TEST(BytecodeProgramTest, VerifiesRowidListLifecycleAndBranches) {
  ProgramInput input;
  input.register_count = 2;
  input.constants.push_back(SqlValue::Integer(7));
  input.instructions = {
      ClearRowIdListInstruction{},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      AppendRowIdListInstruction{.input = Reg(0)},
      RewindRowIdListInstruction{
          .output = Reg(1),
          .empty_target = Address(6),
      },
      NextRowIdListInstruction{
          .output = Reg(1),
          .next_target = Address(4),
      },
      HaltInstruction{},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(input).has_value());
  EXPECT_EQ("clear_rowid_list", InstructionKindName(InstructionKindOf(input.instructions[0])));
  EXPECT_EQ("append_rowid_list", InstructionKindName(InstructionKindOf(input.instructions[2])));
  EXPECT_EQ("rewind_rowid_list", InstructionKindName(InstructionKindOf(input.instructions[3])));
  EXPECT_EQ("next_rowid_list", InstructionKindName(InstructionKindOf(input.instructions[4])));

  input.instructions[0] = LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)};
  EXPECT_EQ(ProgramErrorCode::kCursorNotOpen, VerifyError(input));

  ProgramInput conflict;
  conflict.register_count = 1;
  conflict.constants.push_back(SqlValue::Integer(1));
  conflict.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      JumpIfInstruction{
          .condition = JumpCondition::kIfTrue,
          .input = Reg(0),
          .target = Address(3),
      },
      ClearRowIdListInstruction{},
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kCursorStateConflict, VerifyError(conflict));
}

TEST(BytecodeProgramTest, VerifiesCreateTableStorageInstructions) {
  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kCreateTable;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.register_count = 2;
  input.instructions = {
      EnsureDatabaseInitializedInstruction{},
      CreateTableRootInstruction{.output = Reg(0)},
      IncrementSchemaCookieInstruction{.output = Reg(1)},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(input).has_value());
  EXPECT_EQ("ensure_database_initialized",
            InstructionKindName(InstructionKindOf(input.instructions[0])));
  EXPECT_EQ("create_table_root", InstructionKindName(InstructionKindOf(input.instructions[1])));
  EXPECT_EQ("increment_schema_cookie",
            InstructionKindName(InstructionKindOf(input.instructions[2])));

  input.statement_kind = ProgramStatementKind::kSelect;
  input.transaction_access = ProgramTransactionAccess::kRead;
  input.rollback_mode = ProgramRollbackMode::kTransaction;
  EXPECT_EQ(ProgramErrorCode::kInvalidExecutionMetadata, VerifyError(input));
}

TEST(BytecodeProgramTest, VerifiesDynamicCreateIndexRootInstruction) {
  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kCreateIndex;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.register_count = 1;
  input.symbols.emplace_back("BINARY");
  auto descriptor = IndexWriteCursorDescriptor();
  descriptor.root_page = RootPageNumber(0);
  descriptor.pending_root = true;
  input.write_cursors.push_back(std::move(descriptor));
  input.instructions = {
      CreateIndexRootInstruction{
          .cursor = WriteCursor(0),
          .output = Reg(0),
      },
      CloseWriteCursorInstruction{.cursor = WriteCursor(0)},
      HaltInstruction{},
  };
  auto created = BytecodeProgram::Create(input);
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ("create_index_root",
            InstructionKindName(InstructionKindOf(created->instructions()[0])));
  EXPECT_TRUE(created->write_cursor(WriteCursor(0)).pending_root);

  input.write_cursors[0].pending_root = false;
  EXPECT_EQ(ProgramErrorCode::kInvalidRootPage, VerifyError(input));
  input.write_cursors[0].pending_root = true;
  input.write_cursors[0].root_page = RootPageNumber(3);
  EXPECT_EQ(ProgramErrorCode::kInvalidRootPage, VerifyError(input));
  input.write_cursors[0].root_page = RootPageNumber(0);
  input.instructions[0] = OpenWriteCursorInstruction{.cursor = WriteCursor(0)};
  EXPECT_EQ(ProgramErrorCode::kInvalidCursorDescriptor, VerifyError(input));
}

TEST(BytecodeProgramTest, VerifiesTypedAnalyzeInstructions) {
  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kAnalyze;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.register_count = 3;
  input.constants.push_back(SqlValue::Text("Items"));
  input.symbols.emplace_back("BINARY");
  input.cursors.push_back(IndexCursorDescriptor());
  input.cursors.push_back(RowIdCursorDescriptor());
  input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(0),
      .columns =
          {
              WriteColumnDescriptor{.affinity = TypeAffinity::kBlob},
              WriteColumnDescriptor{.affinity = TypeAffinity::kBlob},
              WriteColumnDescriptor{.affinity = TypeAffinity::kBlob},
          },
      .rowid_alias = std::nullopt,
      .index_columns = {},
      .key_term_count = 0,
      .unique = false,
      .unique_not_null = false,
      .pending_root = true,
      .storage = WriteCursorStorageKind::kRowIdTable,
  });
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      CreateTableRootInstruction{
          .cursor = WriteCursor(0),
          .output = Reg(1),
      },
      ClearStat1Instruction{
          .cursor = WriteCursor(0),
          .scope = Stat1ClearScope::kTable,
          .name = Reg(0),
      },
      ComputeIndexStat1Instruction{
          .cursor = Cursor(0),
          .key_term_count = 1,
          .emit_empty = false,
          .output = Reg(2),
      },
      ComputeTableStat1Instruction{
          .cursor = Cursor(1),
          .output = Reg(2),
      },
      CloseWriteCursorInstruction{.cursor = WriteCursor(0)},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(input).has_value());
  EXPECT_EQ("clear_stat1", InstructionKindName(InstructionKindOf(input.instructions[2])));
  EXPECT_EQ("compute_index_stat1", InstructionKindName(InstructionKindOf(input.instructions[3])));
  EXPECT_EQ("compute_table_stat1", InstructionKindName(InstructionKindOf(input.instructions[4])));

  std::get<ClearStat1Instruction>(input.instructions[2]).scope =
      static_cast<Stat1ClearScope>(255);  // NOLINT
  EXPECT_EQ(ProgramErrorCode::kInvalidExecutionMetadata, VerifyError(input));
}

TEST(BytecodeProgramTest, VerifiesSorterBackedIndexRefillAssociationAndAppendPhase) {
  const ProgramInput input = IndexRefillProgramInput();
  const auto program = BytecodeProgram::Create(input);
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(std::optional{WriteCursor(0)}, program->sorter(Sorter(0)).index_refill_cursor);
  EXPECT_EQ("insert_sorter_index", InstructionKindName(InstructionKindOf(input.instructions[6])));
  EXPECT_TRUE(program->write_effects().multi_write);
  EXPECT_TRUE(program->write_effects().may_abort);
}

TEST(BytecodeProgramTest, RejectsRefillAssociationsOutsideOneMatchingPendingIndex) {
  const auto rejects = [](const auto& mutate) {
    ProgramInput input = IndexRefillProgramInput();
    mutate(input);
    EXPECT_EQ(ProgramErrorCode::kInvalidOrderingDescriptor, VerifyError(input));
  };
  rejects([](ProgramInput& input) { input.sorters[0].index_refill_cursor = WriteCursor(1); });
  rejects([](ProgramInput& input) {
    input.write_cursors[0].pending_root = false;
    input.write_cursors[0].root_page = RootPageNumber(3);
  });
  rejects([](ProgramInput& input) { input.sorters[0].field_count = 3; });
  rejects([](ProgramInput& input) {
    input.symbols.emplace_back("NOCASE");
    input.sorters[0].key_columns[0].collation = Symbol(1);
  });
  rejects([](ProgramInput& input) {
    input.sorters[0].key_columns[0].order = BytecodeSortOrder::kDescending;
    input.sorters[0].key_columns[0].null_placement = BytecodeNullPlacement::kLast;
  });
  rejects([](ProgramInput& input) {
    input.sorters[0].key_columns[0].null_placement = BytecodeNullPlacement::kLast;
  });
  rejects([](ProgramInput& input) {
    input.write_cursors[0].index_columns.back().order = BytecodeSortOrder::kDescending;
  });
  rejects([](ProgramInput& input) { input.sorters.push_back(input.sorters[0]); });
  rejects([](ProgramInput& input) { input.top_ns.push_back(input.sorters[0]); });
  rejects([](ProgramInput& input) { input.relations.push_back(input.sorters[0]); });
  rejects([](ProgramInput& input) {
    input.sorters[0].key_field_count = input.sorters[0].field_count;
    input.sorters[0].key_columns.push_back(input.sorters[0].key_columns[0]);
    input.record_comparisons.push_back(std::move(input.sorters[0]));
    input.sorters.clear();
  });
  rejects([](ProgramInput& input) { input.statement_kind = ProgramStatementKind::kAnalyze; });
  rejects([](ProgramInput& input) { input.write_effects.multi_write = false; });
  rejects([](ProgramInput& input) { input.write_effects.may_abort = false; });
}

TEST(BytecodeProgramTest, RejectsIndexRefillSorterAndWriterPhaseViolations) {
  const auto rejects = [](ProgramErrorCode expected, const auto& mutate) {
    ProgramInput input = IndexRefillProgramInput();
    mutate(input);
    EXPECT_EQ(expected, VerifyError(input));
  };
  rejects(ProgramErrorCode::kInvalidOrderingDescriptor,
          [](ProgramInput& input) { input.sorters[0].index_refill_cursor.reset(); });
  rejects(ProgramErrorCode::kCursorNotOpen, [](ProgramInput& input) {
    input.instructions[0] = LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)};
  });
  rejects(ProgramErrorCode::kCapabilityNotPositioned, [](ProgramInput& input) {
    input.instructions[5] = InsertSorterIndexInstruction{.sorter = Sorter(0)};
  });
  rejects(ProgramErrorCode::kCursorStateConflict, [](ProgramInput& input) {
    input.instructions[6] =
        InsertIndexInstruction{.cursor = WriteCursor(0), .first_value = Reg(0), .value_count = 2};
  });
  rejects(ProgramErrorCode::kCursorStateConflict, [](ProgramInput& input) {
    input.instructions[6] =
        CheckUniqueIndexInstruction{.cursor = WriteCursor(0), .first_key = Reg(0), .key_count = 1};
  });
  rejects(ProgramErrorCode::kCursorStateConflict, [](ProgramInput& input) {
    input.instructions[6] = ResetSorterInstruction{.sorter = Sorter(0)};
  });
  rejects(ProgramErrorCode::kCursorStateConflict, [](ProgramInput& input) {
    input.instructions[6] = CloseWriteCursorInstruction{.cursor = WriteCursor(0)};
    input.instructions[7] = InsertSorterIndexInstruction{.sorter = Sorter(0)};
  });
}

TEST(BytecodeProgramTest, RejectsPrivateSorterRefillOfAnOuterIndexWriter) {
  ProgramInput input = IndexRefillProgramInput();
  input.subroutines.push_back(
      SubroutineDescriptor{.entry = Address(3), .end = Address(8), .kind = SubroutineKind::kRead});
  input.instructions = {
      CreateIndexRootInstruction{.cursor = WriteCursor(0), .output = Reg(0)},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      HaltInstruction{},
      OpenSorterInstruction{.sorter = Sorter(0)},
      RewindSorterInstruction{.sorter = Sorter(0), .empty_target = Address(6)},
      NextSorterInstruction{.sorter = Sorter(0), .next_target = Address(5)},
      CloseSorterInstruction{.sorter = Sorter(0)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
  };
  EXPECT_EQ(ProgramErrorCode::kInvalidSubroutineControl, VerifyError(input));
}

TEST(BytecodeProgramTest, VerifiesTypedSorterLifecycleAndDataflow) {
  ProgramInput input;
  input.register_count = 2;
  input.symbols.emplace_back("BINARY");
  input.constants.push_back(SqlValue::Integer(2));
  input.constants.push_back(SqlValue::Text("payload"));
  input.sorters.push_back(OrderingDescriptor());
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      OpenSorterInstruction{.sorter = Sorter(0)},
      InsertSorterInstruction{.sorter = Sorter(0), .first_value = Reg(0), .value_count = 2},
      RewindSorterInstruction{.sorter = Sorter(0), .empty_target = Address(7)},
      ReadSorterFieldInstruction{.sorter = Sorter(0), .field = 0, .output = Reg(0)},
      NextSorterInstruction{.sorter = Sorter(0), .next_target = Address(5)},
      ResetSorterInstruction{.sorter = Sorter(0)},
      CloseSorterInstruction{.sorter = Sorter(0)},
      HaltInstruction{},
  };

  ASSERT_TRUE(VerifyProgram(input).has_value());
  const auto created = BytecodeProgram::Create(input);
  ASSERT_TRUE(created.has_value());
  EXPECT_FALSE(created->requires_database_snapshot());
  const std::array<std::string_view, 7> names{
      "open_sorter", "insert_sorter", "rewind_sorter", "read_sorter_field",
      "next_sorter", "reset_sorter",  "close_sorter",
  };
  for (std::size_t index = 0; index < names.size(); ++index) {
    EXPECT_EQ(names[index], InstructionKindName(InstructionKindOf(input.instructions[index + 2U])));
  }
}

TEST(BytecodeProgramTest, VerifiesTypedAggregateLifecycleAndDataflow) {
  ProgramInput input;
  input.register_count = 3;
  input.symbols.emplace_back("sum");
  input.symbols.emplace_back("BINARY");
  input.constants.push_back(SqlValue::Integer(2));
  input.aggregates.push_back(AggregateDescriptor{
      .function = Symbol(0),
      .collation = Symbol(1),
      .argument_count = 1,
  });
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenAggregateInstruction{.aggregate = Aggregate(0)},
      StepAggregateInstruction{
          .aggregate = Aggregate(0),
          .first_argument = Reg(0),
          .effect_output = Reg(0),
      },
      FinalizeAggregateInstruction{.aggregate = Aggregate(0), .output = Reg(2)},
      ResetAggregateInstruction{.aggregate = Aggregate(0)},
      StepAggregateInstruction{
          .aggregate = Aggregate(0),
          .first_argument = Reg(0),
          .effect_output = std::nullopt,
      },
      FinalizeAggregateInstruction{.aggregate = Aggregate(0), .output = Reg(2)},
      CloseAggregateInstruction{.aggregate = Aggregate(0)},
      HaltInstruction{},
  };

  ASSERT_TRUE(VerifyProgram(input).has_value());
  const std::array<std::string_view, 5> names{
      "open_aggregate",  "step_aggregate",  "finalize_aggregate",
      "reset_aggregate", "close_aggregate",
  };
  EXPECT_EQ(names[0], InstructionKindName(InstructionKindOf(input.instructions[1])));
  EXPECT_EQ(names[1], InstructionKindName(InstructionKindOf(input.instructions[2])));
  EXPECT_EQ(names[2], InstructionKindName(InstructionKindOf(input.instructions[3])));
  EXPECT_EQ(names[3], InstructionKindName(InstructionKindOf(input.instructions[4])));
  EXPECT_EQ(names[1], InstructionKindName(InstructionKindOf(input.instructions[5])));
  EXPECT_EQ(names[2], InstructionKindName(InstructionKindOf(input.instructions[6])));
  EXPECT_EQ(names[4], InstructionKindName(InstructionKindOf(input.instructions[7])));

  auto created = ProgramBuilder::Create({}, Resources(1));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);
  ASSERT_TRUE(builder.AddSymbol("count").has_value());
  ASSERT_TRUE(builder.AddSymbol("BINARY").has_value());
  const auto aggregate = builder.AddAggregate(AggregateDescriptor{
      .function = Symbol(0),
      .collation = Symbol(1),
      .argument_count = 0,
  });
  ASSERT_TRUE(aggregate.has_value());
  ASSERT_TRUE(builder.Append(OpenAggregateInstruction{.aggregate = *aggregate}).has_value());
  ASSERT_TRUE(builder
                  .Append(FinalizeAggregateInstruction{
                      .aggregate = *aggregate,
                      .output = Reg(0),
                  })
                  .has_value());
  ASSERT_TRUE(builder.Append(CloseAggregateInstruction{.aggregate = *aggregate}).has_value());
  ASSERT_TRUE(builder.Append(HaltInstruction{}).has_value());
  auto built = std::move(builder).Build({});
  ASSERT_TRUE(built.has_value());
  EXPECT_EQ(0U, built->aggregate(*aggregate).argument_count);
}

TEST(BytecodeProgramTest, RejectsInvalidAggregateDescriptorsRangesAndLifecycle) {
  ProgramInput invalid_descriptor;
  invalid_descriptor.symbols.emplace_back("count");
  invalid_descriptor.aggregates.push_back(AggregateDescriptor{
      .function = Symbol(0),
      .collation = Symbol(1),
      .argument_count = 0,
  });
  invalid_descriptor.instructions = {HaltInstruction{}};
  EXPECT_EQ(ProgramErrorCode::kInvalidAggregateDescriptor, VerifyError(invalid_descriptor));

  ProgramInput invalid_id;
  invalid_id.instructions = {
      OpenAggregateInstruction{.aggregate = Aggregate(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kInvalidAggregate, VerifyError(invalid_id));

  ProgramInput uninitialized;
  uninitialized.register_count = 1;
  uninitialized.symbols = {"sum", "BINARY"};
  uninitialized.aggregates.push_back(AggregateDescriptor{
      .function = Symbol(0),
      .collation = Symbol(1),
      .argument_count = 1,
  });
  uninitialized.instructions = {
      OpenAggregateInstruction{.aggregate = Aggregate(0)},
      StepAggregateInstruction{
          .aggregate = Aggregate(0),
          .first_argument = Reg(0),
      },
      FinalizeAggregateInstruction{.aggregate = Aggregate(0), .output = Reg(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kUninitializedRegister, VerifyError(uninitialized));

  ProgramInput zero_argument;
  zero_argument.register_count = 1;
  zero_argument.symbols = {"count", "BINARY"};
  zero_argument.aggregates.push_back(AggregateDescriptor{
      .function = Symbol(0),
      .collation = Symbol(1),
      .argument_count = 0,
  });
  zero_argument.instructions = {
      OpenAggregateInstruction{.aggregate = Aggregate(0)},
      StepAggregateInstruction{
          .aggregate = Aggregate(0),
          .first_argument = Reg(1),
      },
      FinalizeAggregateInstruction{.aggregate = Aggregate(0), .output = Reg(0)},
      CloseAggregateInstruction{.aggregate = Aggregate(0)},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(zero_argument).has_value());

  const auto lifecycle_input = [] {
    ProgramInput input;
    input.register_count = 1;
    input.symbols = {"count", "BINARY"};
    input.aggregates.push_back(AggregateDescriptor{
        .function = Symbol(0),
        .collation = Symbol(1),
        .argument_count = 0,
    });
    return input;
  };

  ProgramInput reset_accumulating = lifecycle_input();
  reset_accumulating.instructions = {
      OpenAggregateInstruction{.aggregate = Aggregate(0)},
      ResetAggregateInstruction{.aggregate = Aggregate(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kCapabilityStateConflict, VerifyError(reset_accumulating));

  ProgramInput close_accumulating = lifecycle_input();
  close_accumulating.instructions = {
      OpenAggregateInstruction{.aggregate = Aggregate(0)},
      CloseAggregateInstruction{.aggregate = Aggregate(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kCapabilityStateConflict, VerifyError(close_accumulating));

  ProgramInput halt_accumulating = lifecycle_input();
  halt_accumulating.instructions = {
      OpenAggregateInstruction{.aggregate = Aggregate(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kCapabilityStateConflict, VerifyError(halt_accumulating));

  ProgramInput step_finalized = lifecycle_input();
  step_finalized.instructions = {
      OpenAggregateInstruction{.aggregate = Aggregate(0)},
      FinalizeAggregateInstruction{.aggregate = Aggregate(0), .output = Reg(0)},
      StepAggregateInstruction{
          .aggregate = Aggregate(0),
          .first_argument = Reg(1),
      },
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kCapabilityStateConflict, VerifyError(step_finalized));

  ProgramInput branch_conflict = lifecycle_input();
  branch_conflict.constants.push_back(SqlValue::Integer(1));
  branch_conflict.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenAggregateInstruction{.aggregate = Aggregate(0)},
      JumpIfInstruction{
          .condition = JumpCondition::kIfTrue,
          .input = Reg(0),
          .target = Address(5),
      },
      FinalizeAggregateInstruction{.aggregate = Aggregate(0), .output = Reg(0)},
      JumpInstruction{.target = Address(6)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kCapabilityStateConflict, VerifyError(branch_conflict));

  ProgramInput finalized_halt = lifecycle_input();
  finalized_halt.instructions = {
      OpenAggregateInstruction{.aggregate = Aggregate(0)},
      FinalizeAggregateInstruction{.aggregate = Aggregate(0), .output = Reg(0)},
      HaltInstruction{},
  };
  EXPECT_TRUE(VerifyProgram(finalized_halt).has_value());
}

TEST(BytecodeProgramTest, VerifiesTypedTopNLifecyclePendingCandidateAndBranches) {
  ProgramInput input;
  input.register_count = 3;
  input.symbols.emplace_back("BINARY");
  input.constants.push_back(SqlValue::Integer(4));
  input.constants.push_back(SqlValue::Integer(2));
  input.constants.push_back(SqlValue::Text("payload"));
  input.top_ns.push_back(OrderingDescriptor());
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(2)},
      OpenTopNInstruction{.top_n = TopN(0), .bound = Reg(0)},
      CheckTopNInstruction{
          .top_n = TopN(0),
          .first_key = Reg(1),
          .key_count = 1,
          .rejected_target = Address(6),
      },
      InsertTopNInstruction{.top_n = TopN(0), .first_value = Reg(1), .value_count = 2},
      RewindTopNInstruction{.top_n = TopN(0), .empty_target = Address(9)},
      ReadTopNFieldInstruction{.top_n = TopN(0), .field = 1, .output = Reg(2)},
      NextTopNInstruction{.top_n = TopN(0), .next_target = Address(7)},
      ResetTopNInstruction{.top_n = TopN(0)},
      CloseTopNInstruction{.top_n = TopN(0)},
      HaltInstruction{},
  };

  ASSERT_TRUE(VerifyProgram(input).has_value());
  const std::array<std::string_view, 8> names{
      "open_top_n",       "check_top_n", "insert_top_n", "rewind_top_n",
      "read_top_n_field", "next_top_n",  "reset_top_n",  "close_top_n",
  };
  for (std::size_t index = 0; index < names.size(); ++index) {
    EXPECT_EQ(names[index], InstructionKindName(InstructionKindOf(input.instructions[index + 3U])));
  }
}

TEST(BytecodeProgramTest, VerifiesTypedRelationLifecycleBranchesAndRecordComparison) {
  ProgramInput input;
  input.register_count = 5;
  input.symbols.emplace_back("BINARY");
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(10));
  input.constants.push_back(SqlValue::Integer(2));
  input.constants.push_back(SqlValue::Integer(20));
  input.relations.push_back(OrderingDescriptor());
  input.record_comparisons.push_back(RecordComparisonDescriptor());
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(2)},
      LoadConstantInstruction{.constant = Constant(3), .output = Reg(3)},
      OpenRelationInstruction{.relation = Relation(0)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = RelationInsertMode::kKeepExisting,
          .duplicate_target = Address(6),
      },
      ContainsRelationInstruction{
          .relation = Relation(0),
          .first_key = Reg(0),
          .key_count = 1,
          .found_target = Address(7),
      },
      DeleteRelationInstruction{
          .relation = Relation(0),
          .first_key = Reg(0),
          .key_count = 1,
      },
      RewindRelationInstruction{.relation = Relation(0), .empty_target = Address(11)},
      ReadRelationFieldInstruction{.relation = Relation(0), .field = 0, .output = Reg(4)},
      NextRelationInstruction{.relation = Relation(0), .next_target = Address(9)},
      ResetRelationInstruction{.relation = Relation(0)},
      CloseRelationInstruction{.relation = Relation(0)},
      CompareRecordsInstruction{
          .comparison = RecordComparison(0),
          .left_first = Reg(0),
          .right_first = Reg(2),
          .output = Reg(4),
      },
      HaltInstruction{},
  };

  ASSERT_TRUE(VerifyProgram(input).has_value());
  const std::array expected_names{
      std::string_view{"open_relation"},     std::string_view{"insert_relation"},
      std::string_view{"contains_relation"}, std::string_view{"delete_relation"},
      std::string_view{"rewind_relation"},   std::string_view{"read_relation_field"},
      std::string_view{"next_relation"},     std::string_view{"reset_relation"},
      std::string_view{"close_relation"},    std::string_view{"compare_records"},
  };
  for (std::size_t index = 0; index < expected_names.size(); ++index) {
    EXPECT_EQ(expected_names[index],
              InstructionKindName(InstructionKindOf(input.instructions[index + 4U])));
  }
}

TEST(BytecodeProgramTest, RejectsInvalidRelationDescriptorsRangesEnumsAndLifecycle) {
  ProgramInput invalid_relation;
  invalid_relation.instructions = {
      OpenRelationInstruction{.relation = Relation(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kInvalidRelation, VerifyError(invalid_relation));

  ProgramInput invalid_comparison;
  invalid_comparison.register_count = 1;
  invalid_comparison.instructions = {
      CompareRecordsInstruction{
          .comparison = RecordComparison(0),
          .left_first = Reg(0),
          .right_first = Reg(0),
          .output = Reg(0),
      },
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kInvalidRecordComparison, VerifyError(invalid_comparison));

  ProgramInput malformed_comparison;
  malformed_comparison.symbols.emplace_back("BINARY");
  malformed_comparison.record_comparisons.push_back(OrderingDescriptor());
  malformed_comparison.instructions = {HaltInstruction{}};
  EXPECT_EQ(ProgramErrorCode::kInvalidOrderingDescriptor, VerifyError(malformed_comparison));

  ProgramInput invalid_mode;
  invalid_mode.register_count = 2;
  invalid_mode.symbols.emplace_back("BINARY");
  invalid_mode.constants.push_back(SqlValue::Integer(1));
  invalid_mode.relations.push_back(OrderingDescriptor());
  invalid_mode.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      OpenRelationInstruction{.relation = Relation(0)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = static_cast<RelationInsertMode>(255),  // NOLINT
          .duplicate_target = Address(4),
      },
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kInvalidEnumValue, VerifyError(invalid_mode));

  ProgramInput read_before_rewind;
  read_before_rewind.register_count = 1;
  read_before_rewind.symbols.emplace_back("BINARY");
  read_before_rewind.relations.push_back(OrderingDescriptor());
  read_before_rewind.instructions = {
      OpenRelationInstruction{.relation = Relation(0)},
      ReadRelationFieldInstruction{.relation = Relation(0), .field = 0, .output = Reg(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kCapabilityNotPositioned, VerifyError(read_before_rewind));

  ProgramInput write_after_rewind;
  write_after_rewind.register_count = 2;
  write_after_rewind.symbols.emplace_back("BINARY");
  write_after_rewind.constants.push_back(SqlValue::Integer(1));
  write_after_rewind.relations.push_back(OrderingDescriptor());
  write_after_rewind.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      OpenRelationInstruction{.relation = Relation(0)},
      RewindRelationInstruction{.relation = Relation(0), .empty_target = Address(5)},
      InsertRelationInstruction{
          .relation = Relation(0),
          .first_value = Reg(0),
          .value_count = 2,
          .mode = RelationInsertMode::kKeepExisting,
          .duplicate_target = Address(5),
      },
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kCapabilityNotWriting, VerifyError(write_after_rewind));

  ProgramInput state_conflict;
  state_conflict.symbols.emplace_back("BINARY");
  state_conflict.relations.push_back(OrderingDescriptor());
  state_conflict.instructions = {
      OpenRelationInstruction{.relation = Relation(0)},
      RewindRelationInstruction{.relation = Relation(0), .empty_target = Address(3)},
      ResetRelationInstruction{.relation = Relation(0)},
      CloseRelationInstruction{.relation = Relation(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(ProgramErrorCode::kCapabilityStateConflict, VerifyError(state_conflict));
}

TEST(BytecodeProgramTest, RejectsInvalidOrderingDescriptorsAndIds) {
  ProgramInput invalid_descriptor;
  invalid_descriptor.symbols.emplace_back("BINARY");
  invalid_descriptor.sorters.push_back(OrderingRecordDescriptor{});
  invalid_descriptor.instructions = {HaltInstruction{}};
  EXPECT_EQ(VerifyError(invalid_descriptor), ProgramErrorCode::kInvalidOrderingDescriptor);

  ProgramInput invalid_column;
  invalid_column.symbols.emplace_back("BINARY");
  auto descriptor = OrderingDescriptor();
  descriptor.key_columns[0].null_placement = static_cast<BytecodeNullPlacement>(2);  // NOLINT
  invalid_column.top_ns.push_back(std::move(descriptor));
  invalid_column.instructions = {HaltInstruction{}};
  EXPECT_EQ(VerifyError(invalid_column), ProgramErrorCode::kInvalidOrderingDescriptor);

  ProgramInput invalid_sorter;
  invalid_sorter.instructions = {
      OpenSorterInstruction{.sorter = Sorter(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(invalid_sorter), ProgramErrorCode::kInvalidSorter);

  ProgramInput invalid_top_n;
  invalid_top_n.register_count = 1;
  invalid_top_n.constants.push_back(SqlValue::Integer(1));
  invalid_top_n.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenTopNInstruction{.top_n = TopN(0), .bound = Reg(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(invalid_top_n), ProgramErrorCode::kInvalidTopN);
}

TEST(BytecodeProgramTest, RejectsSorterAndTopNRegisterAndLifecycleViolations) {
  ProgramInput uninitialized_sorter;
  uninitialized_sorter.register_count = 2;
  uninitialized_sorter.symbols.emplace_back("BINARY");
  uninitialized_sorter.sorters.push_back(OrderingDescriptor());
  uninitialized_sorter.instructions = {
      OpenSorterInstruction{.sorter = Sorter(0)},
      InsertSorterInstruction{.sorter = Sorter(0), .first_value = Reg(0), .value_count = 2},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(uninitialized_sorter), ProgramErrorCode::kUninitializedRegister);

  ProgramInput wrong_sorter_range;
  wrong_sorter_range.register_count = 2;
  wrong_sorter_range.symbols.emplace_back("BINARY");
  wrong_sorter_range.sorters.push_back(OrderingDescriptor());
  wrong_sorter_range.constants.push_back(SqlValue::Integer(1));
  wrong_sorter_range.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      OpenSorterInstruction{.sorter = Sorter(0)},
      InsertSorterInstruction{.sorter = Sorter(0), .first_value = Reg(0), .value_count = 1},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(wrong_sorter_range), ProgramErrorCode::kInvalidRegisterRange);

  ProgramInput read_before_rewind;
  read_before_rewind.register_count = 1;
  read_before_rewind.symbols.emplace_back("BINARY");
  read_before_rewind.sorters.push_back(OrderingDescriptor());
  read_before_rewind.instructions = {
      OpenSorterInstruction{.sorter = Sorter(0)},
      ReadSorterFieldInstruction{.sorter = Sorter(0), .field = 0, .output = Reg(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(read_before_rewind), ProgramErrorCode::kCapabilityNotPositioned);

  ProgramInput pending_required;
  pending_required.register_count = 3;
  pending_required.symbols.emplace_back("BINARY");
  pending_required.constants.push_back(SqlValue::Integer(1));
  pending_required.top_ns.push_back(OrderingDescriptor());
  pending_required.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(2)},
      OpenTopNInstruction{.top_n = TopN(0), .bound = Reg(0)},
      InsertTopNInstruction{.top_n = TopN(0), .first_value = Reg(1), .value_count = 2},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(pending_required), ProgramErrorCode::kTopNCandidateRequired);

  ProgramInput pending_conflict = std::move(pending_required);
  pending_conflict.instructions[4] = CheckTopNInstruction{
      .top_n = TopN(0),
      .first_key = Reg(1),
      .key_count = 1,
      .rejected_target = Address(6),
  };
  pending_conflict.instructions[5] = JumpInstruction{.target = Address(6)};
  pending_conflict.instructions.emplace_back(HaltInstruction{});
  EXPECT_EQ(VerifyError(pending_conflict), ProgramErrorCode::kCapabilityStateConflict);

  ProgramInput sorter_open_twice;
  sorter_open_twice.symbols.emplace_back("BINARY");
  sorter_open_twice.sorters.push_back(OrderingDescriptor());
  sorter_open_twice.instructions = {
      OpenSorterInstruction{.sorter = Sorter(0)},
      OpenSorterInstruction{.sorter = Sorter(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(sorter_open_twice), ProgramErrorCode::kCapabilityAlreadyOpen);

  ProgramInput sorter_close_closed;
  sorter_close_closed.symbols.emplace_back("BINARY");
  sorter_close_closed.sorters.push_back(OrderingDescriptor());
  sorter_close_closed.instructions = {
      CloseSorterInstruction{.sorter = Sorter(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(sorter_close_closed), ProgramErrorCode::kCapabilityAlreadyClosed);

  ProgramInput top_n_pending;
  top_n_pending.register_count = 2;
  top_n_pending.symbols.emplace_back("BINARY");
  top_n_pending.constants.push_back(SqlValue::Integer(1));
  top_n_pending.top_ns.push_back(OrderingDescriptor());
  top_n_pending.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      OpenTopNInstruction{.top_n = TopN(0), .bound = Reg(0)},
      CheckTopNInstruction{
          .top_n = TopN(0),
          .first_key = Reg(1),
          .key_count = 1,
          .rejected_target = Address(5),
      },
      CheckTopNInstruction{
          .top_n = TopN(0),
          .first_key = Reg(1),
          .key_count = 1,
          .rejected_target = Address(5),
      },
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(top_n_pending), ProgramErrorCode::kTopNCandidatePending);
}

TEST(BytecodeProgramTest, BuilderResolvesSorterAndTopNBranchLabels) {
  auto sorter_created = ProgramBuilder::Create({}, Resources(0));
  ASSERT_TRUE(sorter_created.has_value());
  ProgramBuilder sorter_builder = std::move(*sorter_created);
  ASSERT_TRUE(sorter_builder.AddSymbol("BINARY").has_value());
  ASSERT_TRUE(sorter_builder.AddSorter(OrderingDescriptor()).has_value());
  const auto empty_sorter = sorter_builder.CreateLabel();
  const auto next_sorter = sorter_builder.CreateLabel();
  ASSERT_TRUE(empty_sorter.has_value());
  ASSERT_TRUE(next_sorter.has_value());
  ASSERT_TRUE(sorter_builder.Append(OpenSorterInstruction{.sorter = Sorter(0)}).has_value());
  ASSERT_TRUE(sorter_builder.EmitRewindSorter(Sorter(0), *empty_sorter).has_value());
  ASSERT_TRUE(sorter_builder.BindLabel(*next_sorter).has_value());
  ASSERT_TRUE(sorter_builder.EmitNextSorter(Sorter(0), *next_sorter).has_value());
  ASSERT_TRUE(sorter_builder.BindLabel(*empty_sorter).has_value());
  ASSERT_TRUE(sorter_builder.Append(ResetSorterInstruction{.sorter = Sorter(0)}).has_value());
  ASSERT_TRUE(sorter_builder.Append(CloseSorterInstruction{.sorter = Sorter(0)}).has_value());
  ASSERT_TRUE(sorter_builder.Append(HaltInstruction{}).has_value());

  auto sorter_program = std::move(sorter_builder).Build({});
  ASSERT_TRUE(sorter_program.has_value());
  EXPECT_EQ(Address(3),
            std::get<RewindSorterInstruction>(sorter_program->instructions()[1]).empty_target);
  EXPECT_EQ(Address(2),
            std::get<NextSorterInstruction>(sorter_program->instructions()[2]).next_target);

  auto top_n_created = ProgramBuilder::Create({}, Resources(1));
  ASSERT_TRUE(top_n_created.has_value());
  ProgramBuilder top_n_builder = std::move(*top_n_created);
  ASSERT_TRUE(top_n_builder.AddSymbol("BINARY").has_value());
  OrderingRecordDescriptor top_n_descriptor = OrderingDescriptor();
  top_n_descriptor.field_count = 1;
  ASSERT_TRUE(top_n_builder.AddTopN(std::move(top_n_descriptor)).has_value());
  ASSERT_TRUE(top_n_builder.AddConstant(SqlValue::Integer(1)).has_value());
  const auto rejected = top_n_builder.CreateLabel();
  const auto empty_top_n = top_n_builder.CreateLabel();
  const auto next_top_n = top_n_builder.CreateLabel();
  ASSERT_TRUE(rejected.has_value());
  ASSERT_TRUE(empty_top_n.has_value());
  ASSERT_TRUE(next_top_n.has_value());
  ASSERT_TRUE(
      top_n_builder.Append(LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)}));
  ASSERT_TRUE(top_n_builder.Append(OpenTopNInstruction{.top_n = TopN(0), .bound = Reg(0)}));
  ASSERT_TRUE(top_n_builder.EmitCheckTopN(TopN(0), Reg(0), 1, *rejected).has_value());
  ASSERT_TRUE(top_n_builder.Append(
      InsertTopNInstruction{.top_n = TopN(0), .first_value = Reg(0), .value_count = 1}));
  ASSERT_TRUE(top_n_builder.BindLabel(*rejected).has_value());
  ASSERT_TRUE(top_n_builder.EmitRewindTopN(TopN(0), *empty_top_n).has_value());
  ASSERT_TRUE(top_n_builder.BindLabel(*next_top_n).has_value());
  ASSERT_TRUE(top_n_builder.EmitNextTopN(TopN(0), *next_top_n).has_value());
  ASSERT_TRUE(top_n_builder.BindLabel(*empty_top_n).has_value());
  ASSERT_TRUE(top_n_builder.Append(ResetTopNInstruction{.top_n = TopN(0)}).has_value());
  ASSERT_TRUE(top_n_builder.Append(CloseTopNInstruction{.top_n = TopN(0)}).has_value());
  ASSERT_TRUE(top_n_builder.Append(HaltInstruction{}).has_value());

  auto top_n_program = std::move(top_n_builder).Build({});
  ASSERT_TRUE(top_n_program.has_value());
  EXPECT_EQ(Address(4),
            std::get<CheckTopNInstruction>(top_n_program->instructions()[2]).rejected_target);
  EXPECT_EQ(Address(6),
            std::get<RewindTopNInstruction>(top_n_program->instructions()[4]).empty_target);
  EXPECT_EQ(Address(5),
            std::get<NextTopNInstruction>(top_n_program->instructions()[5]).next_target);
}

TEST(BytecodeProgramTest, BuilderResolvesRelationBranchLabelsAndDescriptors) {
  auto created = ProgramBuilder::Create({}, Resources(2));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);
  ASSERT_TRUE(builder.AddSymbol("BINARY").has_value());
  ASSERT_TRUE(builder.AddRelation(OrderingDescriptor()).has_value());
  ASSERT_TRUE(builder.AddRecordComparison(RecordComparisonDescriptor()).has_value());
  ASSERT_TRUE(builder.AddConstant(SqlValue::Integer(1)).has_value());
  const auto duplicate = builder.CreateLabel();
  const auto found = builder.CreateLabel();
  const auto empty = builder.CreateLabel();
  const auto next = builder.CreateLabel();
  ASSERT_TRUE(duplicate.has_value());
  ASSERT_TRUE(found.has_value());
  ASSERT_TRUE(empty.has_value());
  ASSERT_TRUE(next.has_value());
  ASSERT_TRUE(builder.Append(LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)}));
  ASSERT_TRUE(builder.Append(LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)}));
  ASSERT_TRUE(builder.Append(OpenRelationInstruction{.relation = Relation(0)}));
  ASSERT_TRUE(
      builder
          .EmitInsertRelation(Relation(0), Reg(0), 2, RelationInsertMode::kKeepExisting, *duplicate)
          .has_value());
  ASSERT_TRUE(builder.BindLabel(*duplicate).has_value());
  ASSERT_TRUE(builder.EmitContainsRelation(Relation(0), Reg(0), 1, *found).has_value());
  ASSERT_TRUE(builder.BindLabel(*found).has_value());
  ASSERT_TRUE(builder.EmitRewindRelation(Relation(0), *empty).has_value());
  ASSERT_TRUE(builder.BindLabel(*next).has_value());
  ASSERT_TRUE(builder.EmitNextRelation(Relation(0), *next).has_value());
  ASSERT_TRUE(builder.BindLabel(*empty).has_value());
  ASSERT_TRUE(builder.Append(ResetRelationInstruction{.relation = Relation(0)}));
  ASSERT_TRUE(builder.Append(CloseRelationInstruction{.relation = Relation(0)}));
  ASSERT_TRUE(builder.Append(CompareRecordsInstruction{
      .comparison = RecordComparison(0),
      .left_first = Reg(0),
      .right_first = Reg(0),
      .output = Reg(0),
  }));
  ASSERT_TRUE(builder.Append(HaltInstruction{}));

  auto program = std::move(builder).Build({});
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(Address(4),
            std::get<InsertRelationInstruction>(program->instructions()[3]).duplicate_target);
  EXPECT_EQ(Address(5),
            std::get<ContainsRelationInstruction>(program->instructions()[4]).found_target);
  EXPECT_EQ(Address(7),
            std::get<RewindRelationInstruction>(program->instructions()[5]).empty_target);
  EXPECT_EQ(Address(6), std::get<NextRelationInstruction>(program->instructions()[6]).next_target);
  EXPECT_EQ(2U, program->relation(Relation(0)).field_count);
  EXPECT_EQ(2U, program->record_comparison(RecordComparison(0)).field_count);
}

TEST(BytecodeProgramTest, PublishesDirectInputAsImmutableContiguousStorage) {
  auto created = BytecodeProgram::Create(ScalarProgramInput());
  ASSERT_TRUE(created.has_value());

  const BytecodeProgram& program = *created;
  EXPECT_EQ(program.schema_version().schema_cookie, 7U);
  EXPECT_EQ(program.schema_version().generation, 11U);
  EXPECT_EQ(program.register_count(), 1U);
  EXPECT_EQ(program.parameter_count(), 0U);
  EXPECT_FALSE(program.requires_database_snapshot());
  EXPECT_EQ(program.constants().size(), 1U);
  EXPECT_EQ(program.instructions().size(), 3U);
  EXPECT_EQ(program.result_columns().front().name, "value");
  const auto integer = program.constant(Constant(0)).integer_value();
  EXPECT_EQ(integer.value_or(0), 42);
  EXPECT_EQ(InstructionKindOf(program.instruction(Address(1))), InstructionKind::kResultRow);
  EXPECT_GT(program.verification_metrics().owned_bytes, 0U);
  static_assert(std::is_same_v<decltype(program.instructions()), std::span<const Instruction>>);
}

TEST(BytecodeProgramTest, PublishesExecutionAndWriteMetadata) {
  static_assert(!std::is_convertible_v<CursorId, WriteCursorId>);

  ProgramInput input;
  input.statement_kind = ProgramStatementKind::kUpdate;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.rollback_mode = ProgramRollbackMode::kStatement;
  input.write_effects = {.multi_write = true, .may_abort = true};
  input.mutation_result = MutationResultMetadata{
      .publishes_changes = true,
      .publishes_last_insert_rowid = false,
  };
  input.constants.push_back(SqlValue::Text("default"));
  input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(2),
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
                  .default_value = Constant(0),
              },
          },
      .rowid_alias = 0,
      .index_columns = {},
      .key_term_count = 0,
      .unique = false,
      .unique_not_null = false,
      .storage = WriteCursorStorageKind::kRowIdTable,
  });
  input.instructions = {HaltInstruction{}};

  auto created = BytecodeProgram::Create(input);
  ASSERT_TRUE(created.has_value());
  const BytecodeProgram& program = *created;
  EXPECT_EQ(ProgramStatementKind::kUpdate, program.statement_kind());
  EXPECT_EQ(ProgramTransactionAccess::kWrite, program.transaction_access());
  EXPECT_EQ(ProgramRollbackMode::kStatement, program.rollback_mode());
  EXPECT_EQ(input.write_effects, program.write_effects());
  EXPECT_TRUE(program.uses_statement_journal());
  EXPECT_TRUE(program.requires_database_snapshot());
  EXPECT_TRUE(program.mutation_result().publishes_changes);
  ASSERT_EQ(1U, program.write_cursors().size());
  EXPECT_EQ(RootPageNumber(2), program.write_cursor(WriteCursor(0)).root_page);

  input.statement_kind = ProgramStatementKind::kSelect;
  EXPECT_EQ(ProgramErrorCode::kInvalidExecutionMetadata, VerifyError(input));

  input.statement_kind = ProgramStatementKind::kUpdate;
  input.write_cursors.front().columns[1].default_value = Constant(1);
  EXPECT_EQ(ProgramErrorCode::kInvalidCursorDescriptor, VerifyError(input));

  input.write_cursors.front().columns[1].default_value = Constant(0);
  input.transaction_access = static_cast<ProgramTransactionAccess>(255);  // NOLINT
  EXPECT_EQ(ProgramErrorCode::kInvalidEnumValue, VerifyError(input));
}

TEST(BytecodeProgramTest, RequiresCanonicalMutationResultMetadataForEachStatementKind) {
  const auto input_for = [](ProgramStatementKind kind, MutationResultMetadata mutation_result) {
    ProgramInput input;
    input.statement_kind = kind;
    input.transaction_access = kind == ProgramStatementKind::kSelect
                                   ? ProgramTransactionAccess::kRead
                                   : ProgramTransactionAccess::kWrite;
    input.mutation_result = mutation_result;
    input.instructions.emplace_back(HaltInstruction{});
    return input;
  };

  EXPECT_TRUE(VerifyProgram(input_for(ProgramStatementKind::kSelect, {})).has_value());
  EXPECT_TRUE(VerifyProgram(input_for(ProgramStatementKind::kInsert,
                                      MutationResultMetadata{
                                          .publishes_changes = true,
                                          .publishes_last_insert_rowid = true,
                                      }))
                  .has_value());
  EXPECT_TRUE(VerifyProgram(input_for(ProgramStatementKind::kUpdate,
                                      MutationResultMetadata{
                                          .publishes_changes = true,
                                          .publishes_last_insert_rowid = false,
                                      }))
                  .has_value());
  EXPECT_TRUE(VerifyProgram(input_for(ProgramStatementKind::kDelete,
                                      MutationResultMetadata{
                                          .publishes_changes = true,
                                          .publishes_last_insert_rowid = false,
                                      }))
                  .has_value());
  EXPECT_TRUE(VerifyProgram(input_for(ProgramStatementKind::kCreateTable, {})).has_value());
  EXPECT_TRUE(VerifyProgram(input_for(ProgramStatementKind::kCreateIndex, {})).has_value());
  EXPECT_TRUE(VerifyProgram(input_for(ProgramStatementKind::kAnalyze, {})).has_value());

  EXPECT_EQ(
      ProgramErrorCode::kInvalidExecutionMetadata,
      VerifyError(input_for(ProgramStatementKind::kSelect, MutationResultMetadata{
                                                               .publishes_changes = true,
                                                               .publishes_last_insert_rowid = false,
                                                           })));
  EXPECT_EQ(ProgramErrorCode::kInvalidExecutionMetadata,
            VerifyError(input_for(ProgramStatementKind::kInsert, {})));
  EXPECT_EQ(
      ProgramErrorCode::kInvalidExecutionMetadata,
      VerifyError(input_for(ProgramStatementKind::kUpdate, MutationResultMetadata{
                                                               .publishes_changes = true,
                                                               .publishes_last_insert_rowid = true,
                                                           })));
  EXPECT_EQ(ProgramErrorCode::kInvalidExecutionMetadata,
            VerifyError(input_for(ProgramStatementKind::kDelete, {})));
  EXPECT_EQ(ProgramErrorCode::kInvalidExecutionMetadata,
            VerifyError(input_for(ProgramStatementKind::kCreateTable,
                                  MutationResultMetadata{
                                      .publishes_changes = true,
                                      .publishes_last_insert_rowid = false,
                                  })));
  EXPECT_EQ(ProgramErrorCode::kInvalidExecutionMetadata,
            VerifyError(input_for(ProgramStatementKind::kCreateIndex,
                                  MutationResultMetadata{
                                      .publishes_changes = true,
                                      .publishes_last_insert_rowid = false,
                                  })));
  EXPECT_EQ(ProgramErrorCode::kInvalidExecutionMetadata,
            VerifyError(
                input_for(ProgramStatementKind::kAnalyze, MutationResultMetadata{
                                                              .publishes_changes = true,
                                                              .publishes_last_insert_rowid = false,
                                                          })));
}

TEST(BytecodeProgramTest, RejectsInstructionsOutsideTheirStatementFamily) {
  const auto expect_rejected = [](ProgramStatementKind kind, MutationResultMetadata mutation_result,
                                  Instruction instruction) {
    ProgramInput input;
    input.statement_kind = kind;
    input.transaction_access = kind == ProgramStatementKind::kSelect
                                   ? ProgramTransactionAccess::kRead
                                   : ProgramTransactionAccess::kWrite;
    input.mutation_result = mutation_result;
    input.register_count = 4;
    input.instructions.push_back(instruction);
    EXPECT_EQ(ProgramErrorCode::kInvalidExecutionMetadata, VerifyError(input));
  };
  constexpr MutationResultMetadata kInsertResults{
      .publishes_changes = true,
      .publishes_last_insert_rowid = true,
  };
  constexpr MutationResultMetadata kMutationResults{
      .publishes_changes = true,
      .publishes_last_insert_rowid = false,
  };

  expect_rejected(ProgramStatementKind::kDelete, kMutationResults,
                  ResolveInsertRowIdInstruction{
                      .cursor = WriteCursor(0),
                      .input = Reg(0),
                      .output = Reg(1),
                  });
  expect_rejected(ProgramStatementKind::kDelete, kMutationResults,
                  CheckUpdateRowIdInstruction{
                      .cursor = WriteCursor(0),
                      .old_rowid = Reg(0),
                      .new_rowid = Reg(1),
                  });
  expect_rejected(ProgramStatementKind::kCreateTable, {},
                  CreateIndexRootInstruction{
                      .cursor = WriteCursor(0),
                      .output = Reg(0),
                  });
  expect_rejected(ProgramStatementKind::kSelect, {},
                  BuildTableRecordInstruction{
                      .cursor = WriteCursor(0),
                      .first_value = Reg(0),
                      .value_count = 1,
                      .output = Reg(1),
                  });
  expect_rejected(ProgramStatementKind::kUpdate, kMutationResults,
                  InsertTableInstruction{
                      .cursor = WriteCursor(0),
                      .rowid = Reg(0),
                      .record = Reg(1),
                  });
  expect_rejected(ProgramStatementKind::kInsert, kInsertResults,
                  DeleteTableInstruction{.cursor = WriteCursor(0), .rowid = Reg(0)});
  expect_rejected(ProgramStatementKind::kDelete, kMutationResults,
                  UpdateTableInstruction{
                      .cursor = WriteCursor(0),
                      .old_rowid = Reg(0),
                      .new_rowid = Reg(1),
                      .record = Reg(2),
                  });
  expect_rejected(ProgramStatementKind::kUpdate, kMutationResults,
                  ResultRowInstruction{.first = Reg(0), .count = 1});
}

TEST(BytecodeProgramTest, CanonicalizesCallerStorageBeforePublication) {
  ProgramInput input = ScalarProgramInput();
  input.instructions.reserve(1024);
  input.symbols.emplace_back("BINARY");
  input.cursors.push_back(RowIdCursorDescriptor());
  const Instruction* source_instructions = input.instructions.data();
  const CursorFieldSource* source_fields = input.cursors.front().fields.data();

  auto created = BytecodeProgram::Create(input);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->cursors().size(), 1U);
  EXPECT_TRUE(created->requires_database_snapshot());
  EXPECT_NE(created->instructions().data(), source_instructions);
  EXPECT_NE(created->cursors().front().fields.data(), source_fields);

  input.instructions.front() = JumpInstruction{.target = Address(99)};
  input.constants.front() = SqlValue::Integer(99);
  input.symbols.front() = "changed";
  input.cursors.front().root_page = RootPageNumber(99);
  EXPECT_EQ(InstructionKindOf(created->instructions().front()), InstructionKind::kLoadConstant);
  EXPECT_EQ(created->cursors().front().root_page, RootPageNumber(2));
  EXPECT_EQ(created->constant(Constant(0)).integer_value().value_or(0), 42);
  EXPECT_EQ(created->symbol(Symbol(0)), "BINARY");
}

TEST(BytecodeProgramTest, CanonicalizationDropsExcessCallerCapacity) {
  ProgramInput input;
  input.instructions.reserve(1024);
  input.instructions.emplace_back(HaltInstruction{});

  ProgramLimits limits;
  limits.maximum_owned_bytes = sizeof(Instruction) * 8;
  EXPECT_EQ(VerifyError(input, limits), ProgramErrorCode::kOwnedBytesLimitExceeded);

  auto created = BytecodeProgram::Create(input, limits);
  ASSERT_TRUE(created.has_value());
  EXPECT_LE(created->verification_metrics().owned_bytes, limits.maximum_owned_bytes);
}

TEST(BytecodeProgramTest, BuilderResolvesOwnedForwardAndBackwardLabels) {
  auto created = ProgramBuilder::Create(
      SchemaVersionRequirement{.schema_cookie = 2, .generation = 3}, Resources(1));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);

  auto constant = builder.AddConstant(SqlValue::Integer(1));
  auto loop = builder.CreateLabel();
  auto done = builder.CreateLabel();
  ASSERT_TRUE(constant.has_value());
  ASSERT_TRUE(loop.has_value());
  ASSERT_TRUE(done.has_value());
  ASSERT_TRUE(builder.BindLabel(*loop).has_value());
  ASSERT_TRUE(builder.Append(LoadConstantInstruction{.constant = *constant, .output = Reg(0)}));
  ASSERT_TRUE(builder.EmitJumpIf(Reg(0), JumpCondition::kIfTrue, *done));
  ASSERT_TRUE(builder.EmitJump(*loop));
  ASSERT_TRUE(builder.BindLabel(*done).has_value());
  ASSERT_TRUE(builder.Append(ResultRowInstruction{.first = Reg(0), .count = 1}));
  ASSERT_TRUE(builder.Append(HaltInstruction{}));

  auto program = std::move(builder).Build({IntegerResultColumn()});
  ASSERT_TRUE(program.has_value());
  ASSERT_EQ(program->instructions().size(), 5U);
  EXPECT_EQ(std::get<JumpIfInstruction>(program->instructions()[1]).target, Address(3));
  EXPECT_EQ(std::get<JumpInstruction>(program->instructions()[2]).target, Address(0));
}

TEST(BytecodeProgramTest, BuilderPublishesExplicitDatabaseSnapshotRequirement) {
  auto created = ProgramBuilder::Create({}, Resources(0));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);
  ASSERT_TRUE(builder.RequireDatabaseSnapshot().has_value());
  ASSERT_TRUE(builder.Append(HaltInstruction{}).has_value());

  auto program = std::move(builder).Build({});

  ASSERT_TRUE(program.has_value());
  EXPECT_TRUE(program->requires_database_snapshot());
}

TEST(BytecodeProgramTest, BuilderPublishesWriteExecutionMetadata) {
  auto created = ProgramBuilder::Create({}, Resources(0));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);
  auto default_value = builder.AddConstant(SqlValue::Text("default"));
  ASSERT_TRUE(default_value.has_value());
  ASSERT_TRUE(builder
                  .SetExecutionMetadata(ProgramStatementKind::kInsert,
                                        ProgramTransactionAccess::kWrite,
                                        ProgramRollbackMode::kTransaction,
                                        MutationResultMetadata{
                                            .publishes_changes = true,
                                            .publishes_last_insert_rowid = true,
                                        },
                                        ProgramWriteEffects{
                                            .multi_write = true,
                                        })
                  .has_value());
  ASSERT_TRUE(builder.MarkMayAbort().has_value());
  auto cursor = builder.AddWriteCursor(WriteCursorDescriptor{
      .root_page = RootPageNumber(2),
      .columns =
          {
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kText,
                  .not_null = false,
                  .rowid_alias = false,
                  .default_value = *default_value,
              },
          },
      .rowid_alias = std::nullopt,
      .index_columns = {},
      .key_term_count = 0,
      .unique = false,
      .unique_not_null = false,
      .storage = WriteCursorStorageKind::kRowIdTable,
  });
  ASSERT_TRUE(cursor.has_value());
  ASSERT_TRUE(builder.Append(HaltInstruction{}).has_value());

  auto program = std::move(builder).Build({});
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(WriteCursorId(0), *cursor);
  EXPECT_EQ(ProgramStatementKind::kInsert, program->statement_kind());
  EXPECT_EQ(ProgramTransactionAccess::kWrite, program->transaction_access());
  EXPECT_TRUE(program->requires_database_snapshot());
  EXPECT_EQ(1U, program->write_cursors().size());
}

TEST(BytecodeProgramTest, BuilderRejectsDirectNumericBranchesAndForeignLabels) {
  auto first = ProgramBuilder::Create({}, Resources(0));
  auto second = ProgramBuilder::Create({}, Resources(0));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  auto label = first->CreateLabel();
  ASSERT_TRUE(label.has_value());
  auto direct = first->Append(JumpInstruction{.target = Address(0)});
  ASSERT_FALSE(direct.has_value());
  EXPECT_EQ(direct.error().code, ProgramErrorCode::kBranchRequiresLabel);
  auto direct_index = first->Append(SeekIndexInstruction{
      .cursor = Cursor(0),
      .first_key = Reg(0),
      .key_count = 1,
      .missing_target = Address(0),
      .mode = IndexSeekMode::kEqual,
  });
  ASSERT_FALSE(direct_index.has_value());
  EXPECT_EQ(direct_index.error().code, ProgramErrorCode::kBranchRequiresLabel);
  auto direct_index_range = first->Append(CheckIndexRangeInstruction{
      .cursor = Cursor(0),
      .first_key = Reg(0),
      .key_count = 1,
      .end_target = Address(0),
  });
  ASSERT_FALSE(direct_index_range.has_value());
  EXPECT_EQ(direct_index_range.error().code, ProgramErrorCode::kBranchRequiresLabel);
  const auto direct_last =
      first->Append(LastInstruction{.cursor = Cursor(0), .empty_target = Address(0)});
  ASSERT_FALSE(direct_last.has_value());
  EXPECT_EQ(direct_last.error().code, ProgramErrorCode::kBranchRequiresLabel);
  const auto direct_previous =
      first->Append(PreviousInstruction{.cursor = Cursor(0), .previous_target = Address(0)});
  ASSERT_FALSE(direct_previous.has_value());
  EXPECT_EQ(direct_previous.error().code, ProgramErrorCode::kBranchRequiresLabel);
  const auto direct_sorter =
      first->Append(RewindSorterInstruction{.sorter = Sorter(0), .empty_target = Address(0)});
  ASSERT_FALSE(direct_sorter.has_value());
  EXPECT_EQ(direct_sorter.error().code, ProgramErrorCode::kBranchRequiresLabel);
  const auto direct_top_n = first->Append(CheckTopNInstruction{
      .top_n = TopN(0),
      .first_key = Reg(0),
      .key_count = 1,
      .rejected_target = Address(0),
  });
  ASSERT_FALSE(direct_top_n.has_value());
  EXPECT_EQ(direct_top_n.error().code, ProgramErrorCode::kBranchRequiresLabel);

  auto foreign = second->EmitJump(*label);
  ASSERT_FALSE(foreign.has_value());
  EXPECT_EQ(foreign.error().code, ProgramErrorCode::kForeignLabel);
}

TEST(BytecodeProgramTest, BuilderRejectsDoubleBoundAndUnboundLabels) {
  auto created = ProgramBuilder::Create({}, Resources(0));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);

  auto bound = builder.CreateLabel();
  const auto unbound = builder.CreateLabel();
  ASSERT_TRUE(bound.has_value());
  ASSERT_TRUE(unbound.has_value());
  ASSERT_TRUE(builder.BindLabel(*bound).has_value());
  auto rebound = builder.BindLabel(*bound);
  ASSERT_FALSE(rebound.has_value());
  EXPECT_EQ(rebound.error().code, ProgramErrorCode::kLabelAlreadyBound);
  ASSERT_TRUE(builder.Append(HaltInstruction{}));

  auto program = std::move(builder).Build({});
  ASSERT_FALSE(program.has_value());
  EXPECT_EQ(program.error().code, ProgramErrorCode::kUnboundLabel);
}

TEST(BytecodeProgramTest, MovingBuilderTransfersAndInvalidatesLabelOwner) {
  auto created = ProgramBuilder::Create({}, Resources(0));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder source = std::move(*created);
  auto label = source.CreateLabel();
  ASSERT_TRUE(label.has_value());

  ProgramBuilder destination = std::move(source);
  // Moved-from rejection is part of the public builder contract.
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  auto source_label = source.CreateLabel();
  ASSERT_FALSE(source_label.has_value());
  EXPECT_EQ(source_label.error().code, ProgramErrorCode::kInvalidBuilder);
  auto source_append = source.Append(HaltInstruction{});
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(source_append.has_value());
  EXPECT_EQ(source_append.error().code, ProgramErrorCode::kInvalidBuilder);

  ASSERT_TRUE(destination.BindLabel(*label).has_value());
  ASSERT_TRUE(destination.Append(HaltInstruction{}));
  EXPECT_TRUE(std::move(destination).Build({}).has_value());
}

TEST(BytecodeProgramTest, BuilderResolvesCursorControlFlowLabels) {
  auto created = ProgramBuilder::Create({}, Resources(1));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);

  auto cursor = builder.AddCursor(RowIdCursorDescriptor());
  auto loop = builder.CreateLabel();
  auto done = builder.CreateLabel();
  ASSERT_TRUE(cursor.has_value());
  ASSERT_TRUE(loop.has_value());
  ASSERT_TRUE(done.has_value());
  ASSERT_TRUE(builder.Append(OpenReadCursorInstruction{.cursor = *cursor}));
  ASSERT_TRUE(builder.EmitRewind(*cursor, *done));
  ASSERT_TRUE(builder.BindLabel(*loop).has_value());
  ASSERT_TRUE(builder.Append(ReadRowIdInstruction{.cursor = *cursor, .output = Reg(0)}));
  ASSERT_TRUE(builder.EmitNext(*cursor, *loop));
  ASSERT_TRUE(builder.BindLabel(*done).has_value());
  ASSERT_TRUE(builder.Append(CloseCursorInstruction{.cursor = *cursor}));
  ASSERT_TRUE(builder.Append(HaltInstruction{}));

  auto program = std::move(builder).Build({});
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(std::get<RewindInstruction>(program->instructions()[1]).empty_target, Address(4));
  EXPECT_EQ(std::get<NextInstruction>(program->instructions()[3]).next_target, Address(2));
}

TEST(BytecodeProgramTest, BuilderResolvesReverseCursorControlFlowLabels) {
  auto created = ProgramBuilder::Create({}, Resources(1));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);

  auto cursor = builder.AddCursor(RowIdCursorDescriptor());
  auto loop = builder.CreateLabel();
  auto done = builder.CreateLabel();
  ASSERT_TRUE(cursor.has_value());
  ASSERT_TRUE(loop.has_value());
  ASSERT_TRUE(done.has_value());
  ASSERT_TRUE(builder.Append(OpenReadCursorInstruction{.cursor = *cursor}));
  ASSERT_TRUE(builder.EmitLast(*cursor, *done));
  ASSERT_TRUE(builder.BindLabel(*loop).has_value());
  ASSERT_TRUE(builder.Append(ReadRowIdInstruction{.cursor = *cursor, .output = Reg(0)}));
  ASSERT_TRUE(builder.EmitPrevious(*cursor, *loop));
  ASSERT_TRUE(builder.BindLabel(*done).has_value());
  ASSERT_TRUE(builder.Append(CloseCursorInstruction{.cursor = *cursor}));
  ASSERT_TRUE(builder.Append(HaltInstruction{}));

  auto program = std::move(builder).Build({});
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(std::get<LastInstruction>(program->instructions()[1]).empty_target, Address(4));
  EXPECT_EQ(std::get<PreviousInstruction>(program->instructions()[3]).previous_target, Address(2));
}

TEST(BytecodeProgramTest, BuilderResolvesSeekSuccessAndMissingPaths) {
  auto created = ProgramBuilder::Create({}, Resources(1));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);

  auto constant = builder.AddConstant(SqlValue::Integer(1));
  auto cursor = builder.AddCursor(RowIdCursorDescriptor());
  auto missing = builder.CreateLabel();
  auto done = builder.CreateLabel();
  ASSERT_TRUE(constant.has_value());
  ASSERT_TRUE(cursor.has_value());
  ASSERT_TRUE(missing.has_value());
  ASSERT_TRUE(done.has_value());
  ASSERT_TRUE(builder.Append(LoadConstantInstruction{.constant = *constant, .output = Reg(0)}));
  ASSERT_TRUE(builder.Append(OpenReadCursorInstruction{.cursor = *cursor}));
  ASSERT_TRUE(builder.EmitSeekRowId(*cursor, Reg(0), *missing, RowIdSeekMode::kGreater));
  ASSERT_TRUE(builder.Append(CloseCursorInstruction{.cursor = *cursor}));
  ASSERT_TRUE(builder.EmitJump(*done));
  ASSERT_TRUE(builder.BindLabel(*missing).has_value());
  ASSERT_TRUE(builder.Append(CloseCursorInstruction{.cursor = *cursor}));
  ASSERT_TRUE(builder.EmitJump(*done));
  ASSERT_TRUE(builder.BindLabel(*done).has_value());
  ASSERT_TRUE(builder.Append(HaltInstruction{}));

  auto program = std::move(builder).Build({});
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(std::get<SeekRowIdInstruction>(program->instructions()[2]).missing_target, Address(5));
  EXPECT_EQ(std::get<SeekRowIdInstruction>(program->instructions()[2]).mode,
            RowIdSeekMode::kGreater);
}

TEST(BytecodeProgramTest, BuilderResolvesIndexRangeControlFlow) {
  auto created = ProgramBuilder::Create({}, Resources(1));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);

  auto key = builder.AddConstant(SqlValue::Text("bin-05"));
  const auto binary = builder.AddSymbol("BINARY");
  ASSERT_TRUE(key.has_value());
  ASSERT_TRUE(binary.has_value());
  auto cursor = builder.AddCursor(IndexCursorDescriptor());
  auto loop = builder.CreateLabel();
  auto positioned_done = builder.CreateLabel();
  auto unpositioned_done = builder.CreateLabel();
  ASSERT_TRUE(cursor.has_value());
  ASSERT_TRUE(loop.has_value());
  ASSERT_TRUE(positioned_done.has_value());
  ASSERT_TRUE(unpositioned_done.has_value());

  ASSERT_TRUE(builder.Append(LoadConstantInstruction{.constant = *key, .output = Reg(0)}));
  ASSERT_TRUE(builder.Append(OpenReadCursorInstruction{.cursor = *cursor}));
  ASSERT_TRUE(builder.EmitSeekIndex(*cursor, Reg(0), 1, *unpositioned_done,
                                    IndexSeekMode::kGreaterOrEqual));
  ASSERT_TRUE(builder.BindLabel(*loop));
  ASSERT_TRUE(builder.EmitCheckIndexRange(*cursor, Reg(0), 1, *positioned_done,
                                          IndexRangeEndMode::kInclusive));
  ASSERT_TRUE(builder.EmitNext(*cursor, *loop));
  ASSERT_TRUE(builder.BindLabel(*unpositioned_done));
  ASSERT_TRUE(builder.Append(CloseCursorInstruction{.cursor = *cursor}));
  ASSERT_TRUE(builder.Append(HaltInstruction{}));
  ASSERT_TRUE(builder.BindLabel(*positioned_done));
  ASSERT_TRUE(builder.Append(CloseCursorInstruction{.cursor = *cursor}));
  ASSERT_TRUE(builder.Append(HaltInstruction{}));

  auto program = std::move(builder).Build({});
  ASSERT_TRUE(program.has_value());
  ASSERT_EQ(9U, program->instructions().size());
  const auto& seek = std::get<SeekIndexInstruction>(program->instructions()[2]);
  EXPECT_EQ(Address(5), seek.missing_target);
  EXPECT_EQ(IndexSeekMode::kGreaterOrEqual, seek.mode);
  const auto& range = std::get<CheckIndexRangeInstruction>(program->instructions()[3]);
  EXPECT_EQ(Address(7), range.end_target);
  EXPECT_EQ(IndexRangeEndMode::kInclusive, range.mode);
}

TEST(BytecodeProgramTest, BuilderResolvesRowidListIterationLabels) {
  auto created = ProgramBuilder::Create({}, Resources(2));
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);
  auto constant = builder.AddConstant(SqlValue::Integer(7));
  auto empty = builder.CreateLabel();
  auto loop = builder.CreateLabel();
  ASSERT_TRUE(constant.has_value());
  ASSERT_TRUE(empty.has_value());
  ASSERT_TRUE(loop.has_value());
  ASSERT_TRUE(builder.Append(ClearRowIdListInstruction{}));
  ASSERT_TRUE(builder.Append(LoadConstantInstruction{.constant = *constant, .output = Reg(0)}));
  ASSERT_TRUE(builder.Append(AppendRowIdListInstruction{.input = Reg(0)}));
  ASSERT_TRUE(builder.EmitRewindRowIdList(Reg(1), *empty));
  ASSERT_TRUE(builder.BindLabel(*loop).has_value());
  ASSERT_TRUE(builder.EmitNextRowIdList(Reg(1), *loop));
  ASSERT_TRUE(builder.Append(HaltInstruction{}));
  ASSERT_TRUE(builder.BindLabel(*empty).has_value());
  ASSERT_TRUE(builder.Append(HaltInstruction{}));

  auto program = std::move(builder).Build({});
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(Address(6),
            std::get<RewindRowIdListInstruction>(program->instructions()[3]).empty_target);
  EXPECT_EQ(Address(4), std::get<NextRowIdListInstruction>(program->instructions()[4]).next_target);
}

TEST(BytecodeProgramTest, VerifiesPositionedCursorScanAcrossLoopEdges) {
  ProgramInput input;
  input.register_count = 1;
  input.cursors.push_back(RowIdCursorDescriptor());
  input.result_columns.push_back(IntegerResultColumn());
  input.instructions = {
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      RewindInstruction{.cursor = Cursor(0), .empty_target = Address(5)},
      ReadFieldInstruction{
          .cursor = Cursor(0),
          .field = Field(0),
          .output = Reg(0),
      },
      ResultRowInstruction{.first = Reg(0), .count = 1},
      NextInstruction{.cursor = Cursor(0), .next_target = Address(2)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };

  auto verified = VerifyProgram(input);
  ASSERT_TRUE(verified.has_value());
  EXPECT_EQ(verified->reachable_instruction_count, input.instructions.size());
  EXPECT_GT(verified->processed_edge_words, 0U);
}

TEST(BytecodeProgramTest, RejectsReadFromUnpositionedCursor) {
  ProgramInput input;
  input.register_count = 1;
  input.cursors.push_back(RowIdCursorDescriptor());
  input.instructions = {
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      ReadRowIdInstruction{.cursor = Cursor(0), .output = Reg(0)},
      HaltInstruction{},
  };

  EXPECT_EQ(VerifyError(input), ProgramErrorCode::kCursorNotPositioned);
}

TEST(BytecodeProgramTest, RejectsIncompatibleCursorStatesAtControlFlowJoin) {
  ProgramInput input;
  input.cursors.push_back(RowIdCursorDescriptor());
  input.instructions = {
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      RewindInstruction{.cursor = Cursor(0), .empty_target = Address(3)},
      JumpInstruction{.target = Address(3)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };

  EXPECT_EQ(VerifyError(input), ProgramErrorCode::kCursorStateConflict);
}

TEST(BytecodeProgramTest, MergesRegisterInitializationByIntersection) {
  ProgramInput input;
  input.register_count = 2;
  input.constants.push_back(SqlValue::Integer(1));
  input.result_columns.push_back(IntegerResultColumn());
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      JumpIfInstruction{
          .condition = JumpCondition::kIfTrue,
          .input = Reg(0),
          .target = Address(3),
      },
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      ResultRowInstruction{.first = Reg(1), .count = 1},
      HaltInstruction{},
  };

  EXPECT_EQ(VerifyError(input), ProgramErrorCode::kUninitializedRegister);
}

TEST(BytecodeProgramTest, RejectsInvalidDescriptorAndStorageSpecificOperations) {
  ProgramInput bad_root = ScalarProgramInput();
  auto root_zero = RowIdCursorDescriptor();
  root_zero.root_page = RootPageNumber(0);
  bad_root.cursors.push_back(std::move(root_zero));
  EXPECT_EQ(VerifyError(bad_root), ProgramErrorCode::kInvalidRootPage);

  ProgramInput bad_descriptor = ScalarProgramInput();
  bad_descriptor.symbols.emplace_back("BINARY");
  auto descriptor = IndexCursorDescriptor();
  descriptor.index_columns.pop_back();
  bad_descriptor.cursors.push_back(std::move(descriptor));
  EXPECT_EQ(VerifyError(bad_descriptor), ProgramErrorCode::kInvalidCursorDescriptor);

  ProgramInput missing_constant = ScalarProgramInput();
  auto constant_descriptor = RowIdCursorDescriptor();
  constant_descriptor.fields[0].missing_value_kind = MissingFieldValueKind::kConstant;
  missing_constant.cursors.push_back(std::move(constant_descriptor));
  EXPECT_EQ(VerifyError(missing_constant), ProgramErrorCode::kInvalidCursorDescriptor);

  ProgramInput forbidden_rowid_default = ScalarProgramInput();
  auto rowid_descriptor = RowIdCursorDescriptor();
  rowid_descriptor.fields[1].missing_value_kind = MissingFieldValueKind::kUnsupported;
  forbidden_rowid_default.cursors.push_back(std::move(rowid_descriptor));
  EXPECT_EQ(VerifyError(forbidden_rowid_default), ProgramErrorCode::kInvalidCursorDescriptor);

  ProgramInput bad_seek;
  bad_seek.register_count = 1;
  bad_seek.symbols.emplace_back("BINARY");
  bad_seek.cursors.push_back(IndexCursorDescriptor());
  bad_seek.constants.push_back(SqlValue::Integer(1));
  bad_seek.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      SeekRowIdInstruction{
          .cursor = Cursor(0),
          .key = Reg(0),
          .missing_target = Address(4),
      },
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(bad_seek), ProgramErrorCode::kRowIdOperationRequiresRowIdTable);

  ProgramInput bad_index_seek;
  bad_index_seek.register_count = 1;
  bad_index_seek.constants.push_back(SqlValue::Integer(1));
  bad_index_seek.cursors.push_back(RowIdCursorDescriptor());
  bad_index_seek.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      SeekIndexInstruction{
          .cursor = Cursor(0),
          .first_key = Reg(0),
          .key_count = 1,
          .missing_target = Address(4),
      },
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(bad_index_seek), ProgramErrorCode::kIndexOperationRequiresIndex);

  bad_index_seek.instructions[2] = CheckIndexRangeInstruction{
      .cursor = Cursor(0),
      .first_key = Reg(0),
      .key_count = 1,
      .end_target = Address(4),
  };
  EXPECT_EQ(VerifyError(bad_index_seek), ProgramErrorCode::kIndexOperationRequiresIndex);
}

TEST(BytecodeProgramTest, RejectsInvalidOperandsResultShapeAndText) {
  auto invalid_register = ScalarProgramInput();
  invalid_register.instructions[0] =
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)};
  EXPECT_EQ(VerifyError(invalid_register), ProgramErrorCode::kInvalidRegister);

  auto invalid_target = ScalarProgramInput();
  invalid_target.instructions[0] = JumpInstruction{.target = Address(9)};
  EXPECT_EQ(VerifyError(invalid_target), ProgramErrorCode::kInvalidBranchTarget);

  auto invalid_condition = ScalarProgramInput();
  invalid_condition.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      JumpIfInstruction{
          .condition = static_cast<JumpCondition>(0xff),
          .input = Reg(0),
          .target = Address(3),
      },
      ResultRowInstruction{.first = Reg(0), .count = 1},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(invalid_condition), ProgramErrorCode::kInvalidEnumValue);

  auto invalid_shape = ScalarProgramInput();
  invalid_shape.instructions[1] = ResultRowInstruction{.first = Reg(0), .count = 0};
  EXPECT_EQ(VerifyError(invalid_shape), ProgramErrorCode::kResultShapeMismatch);

  auto invalid_symbol = ScalarProgramInput();
  invalid_symbol.symbols.emplace_back("bad\0symbol", 10);
  EXPECT_EQ(VerifyError(invalid_symbol), ProgramErrorCode::kInvalidText);
}

TEST(BytecodeProgramTest, RejectsUnreachableAndFallthroughPastEndInstructions) {
  ProgramInput unreachable;
  unreachable.instructions = {
      JumpInstruction{.target = Address(2)},
      HaltInstruction{},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(unreachable), ProgramErrorCode::kUnreachableInstruction);

  ProgramInput fallthrough;
  fallthrough.constants.push_back(SqlValue::Integer(1));
  fallthrough.instructions = {LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)}};
  fallthrough.register_count = 1;
  EXPECT_EQ(VerifyError(fallthrough), ProgramErrorCode::kFallthroughPastEnd);
}

TEST(BytecodeProgramTest, EnforcesBuilderCountAndOwnedByteLimitsIncrementally) {
  ProgramLimits limits;
  limits.maximum_instructions = 1;
  limits.maximum_symbols = 1;
  limits.maximum_labels = 1;
  limits.maximum_owned_bytes = sizeof(Instruction) + sizeof(std::string) + 3;

  auto created = ProgramBuilder::Create({}, Resources(0), limits);
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);

  ASSERT_TRUE(builder.AddSymbol("bin").has_value());
  auto second_symbol = builder.AddSymbol("more");
  ASSERT_FALSE(second_symbol.has_value());
  EXPECT_EQ(second_symbol.error().code, ProgramErrorCode::kSymbolLimitExceeded);

  ASSERT_TRUE(builder.CreateLabel().has_value());
  auto second_label = builder.CreateLabel();
  ASSERT_FALSE(second_label.has_value());
  EXPECT_EQ(second_label.error().code, ProgramErrorCode::kLabelLimitExceeded);

  ASSERT_TRUE(builder.Append(HaltInstruction{}));
  auto second_instruction = builder.Append(HaltInstruction{});
  ASSERT_FALSE(second_instruction.has_value());
  EXPECT_EQ(second_instruction.error().code, ProgramErrorCode::kInstructionLimitExceeded);

  ProgramLimits ordering_limits;
  ordering_limits.maximum_sorters = 0;
  ordering_limits.maximum_top_ns = 0;
  ordering_limits.maximum_relations = 0;
  ordering_limits.maximum_record_comparisons = 0;
  ordering_limits.maximum_aggregates = 0;
  auto ordering_created = ProgramBuilder::Create({}, Resources(0), ordering_limits);
  ASSERT_TRUE(ordering_created.has_value());
  ProgramBuilder ordering_builder = std::move(*ordering_created);
  const auto sorter_limit = ordering_builder.AddSorter(OrderingDescriptor());
  const auto top_n_limit = ordering_builder.AddTopN(OrderingDescriptor());
  const auto relation_limit = ordering_builder.AddRelation(OrderingDescriptor());
  const auto comparison_limit = ordering_builder.AddRecordComparison(RecordComparisonDescriptor());
  const auto aggregate_limit = ordering_builder.AddAggregate(AggregateDescriptor{
      .function = Symbol(0),
      .collation = Symbol(0),
      .argument_count = 0,
  });
  ASSERT_FALSE(sorter_limit.has_value());
  ASSERT_FALSE(top_n_limit.has_value());
  EXPECT_EQ(sorter_limit.error().code, ProgramErrorCode::kSorterLimitExceeded);
  EXPECT_EQ(top_n_limit.error().code, ProgramErrorCode::kTopNLimitExceeded);
  EXPECT_EQ(relation_limit.error().code, ProgramErrorCode::kRelationLimitExceeded);
  EXPECT_EQ(comparison_limit.error().code, ProgramErrorCode::kRecordComparisonLimitExceeded);
  EXPECT_EQ(aggregate_limit.error().code, ProgramErrorCode::kAggregateLimitExceeded);
}

TEST(BytecodeProgramTest, ReservesEmissionStorageWithoutPublishingResources) {
  ProgramLimits limits;
  limits.maximum_instructions = 8;
  limits.maximum_labels = 4;
  limits.maximum_owned_bytes = sizeof(Instruction);
  auto created = ProgramBuilder::Create({}, Resources(0), limits);
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);
  ASSERT_TRUE(builder.ReserveInstructionCapacity(limits.maximum_instructions).has_value());
  ASSERT_TRUE(builder.ReserveLabelCapacity(limits.maximum_labels).has_value());
  ASSERT_TRUE(builder.Append(HaltInstruction{}).has_value());
  const auto program = std::move(builder).Build({});
  ASSERT_TRUE(program.has_value());
  EXPECT_EQ(1U, program->instructions().size());
  EXPECT_EQ(sizeof(Instruction), program->verification_metrics().owned_bytes);
}

TEST(BytecodeProgramTest, RejectsInvalidEmissionCapacityAndMovedBuilders) {
  ProgramLimits limits;
  limits.maximum_instructions = 2;
  limits.maximum_labels = 2;
  auto created = ProgramBuilder::Create({}, Resources(0), limits);
  ASSERT_TRUE(created.has_value());
  ProgramBuilder builder = std::move(*created);
  const auto instructions = builder.ReserveInstructionCapacity(3);
  ASSERT_FALSE(instructions.has_value());
  EXPECT_EQ(ProgramErrorCode::kInstructionLimitExceeded, instructions.error().code);
  const auto labels = builder.ReserveLabelCapacity(3);
  ASSERT_FALSE(labels.has_value());
  EXPECT_EQ(ProgramErrorCode::kLabelLimitExceeded, labels.error().code);
  ASSERT_TRUE(builder.ReserveInstructionCapacity(2).has_value());
  ASSERT_TRUE(builder.ReserveLabelCapacity(2).has_value());
  ProgramBuilder moved = std::move(builder);
  const auto invalid_instructions = builder.ReserveInstructionCapacity(0);
  ASSERT_FALSE(invalid_instructions.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidBuilder, invalid_instructions.error().code);
  const auto invalid_labels = builder.ReserveLabelCapacity(0);
  ASSERT_FALSE(invalid_labels.has_value());
  EXPECT_EQ(ProgramErrorCode::kInvalidBuilder, invalid_labels.error().code);
  ASSERT_TRUE(moved.ReserveInstructionCapacity(0).has_value());
  ASSERT_TRUE(moved.ReserveLabelCapacity(0).has_value());

  limits.maximum_instructions = std::numeric_limits<std::size_t>::max();
  limits.maximum_labels = std::numeric_limits<std::size_t>::max();
  auto wide = ProgramBuilder::Create({}, Resources(0), limits);
  ASSERT_TRUE(wide.has_value());
  const auto too_many_instructions =
      wide->ReserveInstructionCapacity(std::numeric_limits<std::size_t>::max());
  ASSERT_FALSE(too_many_instructions.has_value());
  EXPECT_EQ(ProgramErrorCode::kInstructionLimitExceeded, too_many_instructions.error().code);
  const auto too_many_labels = wide->ReserveLabelCapacity(std::numeric_limits<std::size_t>::max());
  ASSERT_FALSE(too_many_labels.has_value());
  EXPECT_EQ(ProgramErrorCode::kLabelLimitExceeded, too_many_labels.error().code);
}

TEST(BytecodeProgramTest, EnforcesOwnedAndAnalysisMemoryForDirectInput) {
  const auto owned = ScalarProgramInput();
  ProgramLimits owned_limits;
  owned_limits.maximum_owned_bytes = 1;
  EXPECT_EQ(VerifyError(owned, owned_limits), ProgramErrorCode::kOwnedBytesLimitExceeded);

  const auto analysis = ScalarProgramInput();
  ProgramLimits analysis_limits;
  analysis_limits.maximum_analysis_words = 1;
  EXPECT_EQ(VerifyError(analysis, analysis_limits), ProgramErrorCode::kAnalysisLimitExceeded);

  const auto work = ScalarProgramInput();
  ProgramLimits work_limits;
  work_limits.maximum_analysis_edge_words = 1;
  EXPECT_EQ(VerifyError(work, work_limits), ProgramErrorCode::kAnalysisWorkLimitExceeded);

  ProgramInput maximum_register_id;
  maximum_register_id.register_count = std::numeric_limits<std::uint32_t>::max();
  maximum_register_id.constants.push_back(SqlValue::Integer(1));
  maximum_register_id.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      HaltInstruction{},
  };
  ProgramLimits overflow_limits;
  overflow_limits.maximum_registers = std::numeric_limits<std::uint32_t>::max();
  overflow_limits.maximum_analysis_words = 3;
  EXPECT_EQ(VerifyError(maximum_register_id, overflow_limits),
            ProgramErrorCode::kAnalysisLimitExceeded);
}

TEST(BytecodeProgramTest, MapsResourceAndValidationFailuresToBaseErrors) {
  EXPECT_EQ(ProgramError{.code = ProgramErrorCode::kOwnedBytesLimitExceeded}.base_error_code(),
            ErrorCode::kTooLarge);
  EXPECT_EQ(ProgramError{.code = ProgramErrorCode::kAggregateLimitExceeded}.base_error_code(),
            ErrorCode::kTooLarge);
  EXPECT_EQ(ProgramError{.code = ProgramErrorCode::kUninitializedRegister}.base_error_code(),
            ErrorCode::kMisuse);
  EXPECT_EQ(ProgramError{.code = ProgramErrorCode::kInvalidExecutionMetadata}.base_error_code(),
            ErrorCode::kMisuse);
  EXPECT_EQ(ProgramError{.code = ProgramErrorCode::kInvalidAggregate}.base_error_code(),
            ErrorCode::kMisuse);
}

TEST(BytecodeProgramTest, CarriesAffinityAndSelectedCollationInOperations) {
  ProgramInput input;
  input.register_count = 3;
  input.symbols = {"NOCASE", "lower"};
  input.constants.push_back(SqlValue::Text("A"));
  input.constants.push_back(SqlValue::Text("a"));
  input.result_columns.push_back(IntegerResultColumn());
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      CompareInstruction{
          .comparison = SqlComparison::kEqual,
          .affinity = TypeAffinity::kText,
          .collation = Symbol(0),
          .left = Reg(0),
          .right = Reg(1),
          .output = Reg(2),
      },
      CallScalarInstruction{
          .function = Symbol(1),
          .collation = Symbol(0),
          .first_argument = Reg(0),
          .argument_count = 1,
          .output = Reg(0),
      },
      ResultRowInstruction{.first = Reg(2), .count = 1},
      HaltInstruction{},
  };

  EXPECT_TRUE(VerifyProgram(input).has_value());
}

TEST(BytecodeProgramTest, VerifiesParameterAndScalarExpressionOperations) {
  ProgramInput input;
  input.register_count = 6;
  input.parameter_count = 1;
  input.symbols = {"BINARY", "scalar"};
  input.constants.push_back(SqlValue::Integer(2));
  input.result_columns.push_back(IntegerResultColumn());
  input.instructions = {
      LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)},
      UnaryInstruction{
          .operation = UnaryOperation::kNegate,
          .input = Reg(1),
          .output = Reg(2),
      },
      BinaryInstruction{
          .operation = BinaryOperation::kAdd,
          .left = Reg(0),
          .right = Reg(2),
          .output = Reg(3),
      },
      ApplyAffinityInstruction{
          .input = Reg(3),
          .affinity = TypeAffinity::kNumeric,
          .output = Reg(3),
      },
      CastInstruction{
          .input = Reg(3),
          .target = CastTarget::kInteger,
          .output = Reg(4),
      },
      CompareInstruction{
          .comparison = SqlComparison::kGreater,
          .affinity = TypeAffinity::kNumeric,
          .collation = Symbol(0),
          .left = Reg(4),
          .right = Reg(1),
          .output = Reg(5),
      },
      CallScalarInstruction{
          .function = Symbol(1),
          .collation = Symbol(0),
          .first_argument = Reg(0),
          .argument_count = 2,
          .output = Reg(4),
      },
      ResultRowInstruction{.first = Reg(5), .count = 1},
      HaltInstruction{},
  };

  EXPECT_TRUE(VerifyProgram(input).has_value());
}

TEST(BytecodeProgramTest, PreservesAbsentAndEmptyDeclaredTypes) {
  ProgramInput input;
  input.register_count = 2;
  input.constants.push_back(SqlValue::Integer(1));
  input.constants.push_back(SqlValue::Integer(2));
  input.result_columns = {
      ResultColumnMetadata{
          .name = "absent",
          .declared_type = std::nullopt,
          .affinity = TypeAffinity::kNone,
      },
      ResultColumnMetadata{
          .name = "empty",
          .declared_type = std::string{},
          .affinity = TypeAffinity::kNone,
      },
  };
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      ResultRowInstruction{.first = Reg(0), .count = 2},
      HaltInstruction{},
  };

  auto created = BytecodeProgram::Create(input);
  ASSERT_TRUE(created.has_value());
  EXPECT_FALSE(created->result_columns()[0].declared_type.has_value());
  EXPECT_TRUE(created->result_columns()[1]
                  .declared_type.transform([](const std::string& value) { return value.empty(); })
                  .value_or(false));
}

TEST(BytecodeProgramTest, RejectsInvalidCursorLifetimeTransitions) {
  ProgramInput close_closed;
  close_closed.cursors.push_back(RowIdCursorDescriptor());
  close_closed.instructions = {
      CloseCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(close_closed), ProgramErrorCode::kCursorAlreadyClosed);

  ProgramInput open_twice;
  open_twice.cursors.push_back(RowIdCursorDescriptor());
  open_twice.instructions = {
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(open_twice), ProgramErrorCode::kCursorAlreadyOpen);

  ProgramInput rewind_closed;
  rewind_closed.cursors.push_back(RowIdCursorDescriptor());
  rewind_closed.instructions = {
      RewindInstruction{.cursor = Cursor(0), .empty_target = Address(1)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(rewind_closed), ProgramErrorCode::kCursorNotOpen);

  ProgramInput next_unpositioned;
  next_unpositioned.cursors.push_back(RowIdCursorDescriptor());
  next_unpositioned.instructions = {
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      NextInstruction{.cursor = Cursor(0), .next_target = Address(1)},
      HaltInstruction{},
  };
  EXPECT_EQ(VerifyError(next_unpositioned), ProgramErrorCode::kCursorNotPositioned);
}

}  // namespace
}  // namespace modern_sqlite
