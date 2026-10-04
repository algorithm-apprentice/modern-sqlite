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

[[nodiscard]] ProgramErrorCode VerifyError(const ProgramInput& input, ProgramLimits limits = {}) {
  const auto verified = VerifyProgram(input, limits);
  EXPECT_FALSE(verified.has_value());
  return verified.error().code;
}

TEST(BytecodeProgramTest, UsesStrongIdsAndStableInstructionMetadata) {
  static_assert(!std::is_convertible_v<std::uint32_t, RegisterId>);
  static_assert(!std::is_convertible_v<RegisterId, CursorId>);
  static_assert(sizeof(Instruction) <= 32);

  const std::array<Instruction, 20> instructions = {
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
      CastInstruction{
          .input = Reg(0),
          .target = CastTarget::kText,
          .output = Reg(1),
      },
      OpenReadCursorInstruction{.cursor = Cursor(0)},
      CloseCursorInstruction{.cursor = Cursor(0)},
      RewindInstruction{.cursor = Cursor(0), .empty_target = Address(1)},
      NextInstruction{.cursor = Cursor(0), .next_target = Address(1)},
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
  const std::array<std::string_view, 20> names = {
      "halt",    "load_constant",  "load_parameter", "copy",       "unary",
      "binary",  "apply_affinity", "cast",           "open_read",  "close",
      "rewind",  "next",           "seek_rowid",     "read_field", "read_rowid",
      "compare", "call_scalar",    "jump",           "jump_if",    "result_row",
  };

  for (std::size_t index = 0; index < instructions.size(); ++index) {
    EXPECT_EQ(InstructionKindName(InstructionKindOf(instructions[index])), names[index]);
  }
}

TEST(BytecodeProgramTest, PublishesDirectInputAsImmutableContiguousStorage) {
  auto created = BytecodeProgram::Create(ScalarProgramInput());
  ASSERT_TRUE(created.has_value());

  const BytecodeProgram& program = *created;
  EXPECT_EQ(program.schema_version().schema_cookie, 7U);
  EXPECT_EQ(program.schema_version().generation, 11U);
  EXPECT_EQ(program.register_count(), 1U);
  EXPECT_EQ(program.parameter_count(), 0U);
  EXPECT_EQ(program.constants().size(), 1U);
  EXPECT_EQ(program.instructions().size(), 3U);
  EXPECT_EQ(program.result_columns().front().name, "value");
  const auto integer = program.constant(Constant(0)).integer_value();
  EXPECT_EQ(integer.value_or(0), 42);
  EXPECT_EQ(InstructionKindOf(program.instruction(Address(1))), InstructionKind::kResultRow);
  EXPECT_GT(program.verification_metrics().owned_bytes, 0U);
  static_assert(std::is_same_v<decltype(program.instructions()), std::span<const Instruction>>);
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
  ASSERT_TRUE(builder.EmitSeekRowId(*cursor, Reg(0), *missing));
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
}

TEST(BytecodeProgramTest, RejectsInvalidOperandsResultShapeAndText) {
  auto invalid_register = ScalarProgramInput();
  invalid_register.instructions[0] =
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(1)};
  EXPECT_EQ(VerifyError(invalid_register), ProgramErrorCode::kInvalidRegister);

  auto invalid_target = ScalarProgramInput();
  invalid_target.instructions[0] = JumpInstruction{.target = Address(9)};
  EXPECT_EQ(VerifyError(invalid_target), ProgramErrorCode::kInvalidBranchTarget);

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
  EXPECT_EQ(ProgramError{.code = ProgramErrorCode::kUninitializedRegister}.base_error_code(),
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
