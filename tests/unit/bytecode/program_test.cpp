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

[[nodiscard]] ProgramErrorCode VerifyError(const ProgramInput& input, ProgramLimits limits = {}) {
  const auto verified = VerifyProgram(input, limits);
  EXPECT_FALSE(verified.has_value());
  return verified.error().code;
}

TEST(BytecodeProgramTest, UsesStrongIdsAndStableInstructionMetadata) {
  static_assert(!std::is_convertible_v<std::uint32_t, RegisterId>);
  static_assert(!std::is_convertible_v<RegisterId, CursorId>);
  static_assert(sizeof(Instruction) <= 32);

  const std::array<Instruction, 23> instructions = {
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
  const std::array<std::string_view, 23> names = {
      "halt",       "load_constant",  "load_parameter",  "copy",          "unary",
      "binary",     "apply_affinity", "must_be_integer", "real_affinity", "real_storage_affinity",
      "cast",       "open_read",      "close",           "rewind",        "next",
      "seek_rowid", "read_field",     "read_rowid",      "compare",       "call_scalar",
      "jump",       "jump_if",        "result_row",
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
  EXPECT_TRUE(VerifyProgram(seek).has_value());

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
                                        })
                  .has_value());
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
  auto ordering_created = ProgramBuilder::Create({}, Resources(0), ordering_limits);
  ASSERT_TRUE(ordering_created.has_value());
  ProgramBuilder ordering_builder = std::move(*ordering_created);
  const auto sorter_limit = ordering_builder.AddSorter(OrderingDescriptor());
  const auto top_n_limit = ordering_builder.AddTopN(OrderingDescriptor());
  ASSERT_FALSE(sorter_limit.has_value());
  ASSERT_FALSE(top_n_limit.has_value());
  EXPECT_EQ(sorter_limit.error().code, ProgramErrorCode::kSorterLimitExceeded);
  EXPECT_EQ(top_n_limit.error().code, ProgramErrorCode::kTopNLimitExceeded);
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
  EXPECT_EQ(ProgramError{.code = ProgramErrorCode::kInvalidExecutionMetadata}.base_error_code(),
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
