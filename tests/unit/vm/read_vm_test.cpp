#include "modern_sqlite/vm/read_vm.hpp"

#include <gtest/gtest.h>

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
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/pager/read_pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<ReadVm>);
static_assert(!std::is_copy_assignable_v<ReadVm>);
static_assert(std::is_nothrow_move_constructible_v<ReadVm>);
static_assert(std::is_nothrow_move_assignable_v<ReadVm>);

constexpr std::uint64_t kCatalogGeneration = 17;

[[nodiscard]] constexpr RegisterId Reg(std::uint32_t value) { return RegisterId(value); }
[[nodiscard]] constexpr CursorId Cursor(std::uint32_t value) { return CursorId(value); }
[[nodiscard]] constexpr ParameterId Parameter(std::uint32_t value) { return ParameterId(value); }
[[nodiscard]] constexpr ConstantId Constant(std::uint32_t value) { return ConstantId(value); }
[[nodiscard]] constexpr SymbolId Symbol(std::uint32_t value) { return SymbolId(value); }
[[nodiscard]] constexpr CursorFieldId Field(std::uint32_t value) { return CursorFieldId(value); }
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
[[nodiscard]] T TakeProgramValue(ProgramResult<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error("bytecode program construction failed: code=" +
                             std::to_string(static_cast<int>(result.error().code)) +
                             " instruction=" + std::to_string(result.error().instruction) +
                             " detail=" + std::to_string(result.error().detail));
  }
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
            ("modern-sqlite-read-vm-" + std::to_string(timestamp) + "-" +
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
    const ReadPager& pager, std::uint64_t generation = kCatalogGeneration) {
  const DatabaseHeader* header = pager.header();
  return SchemaVersionRequirement{
      .schema_cookie = header == nullptr ? 0U : header->schema_cookie(),
      .generation = generation,
  };
}

// The adjacent counts mirror ProgramInput and remain explicit at each test call site.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] BytecodeProgram BuildProgram(
    const ReadPager& pager, std::uint32_t register_count, std::uint32_t parameter_count,
    std::vector<SqlValue> constants, std::vector<std::string> symbols,
    std::vector<ReadCursorDescriptor> cursors, std::vector<ResultColumnMetadata> result_columns,
    std::vector<Instruction> instructions,
    std::optional<SchemaVersionRequirement> schema = std::nullopt,
    bool requires_read_transaction = false) {
  ProgramInput input;
  input.schema_version = schema.value_or(CurrentSchema(pager));
  input.register_count = register_count;
  input.parameter_count = parameter_count;
  input.requires_read_transaction = requires_read_transaction;
  input.constants = std::move(constants);
  input.symbols = std::move(symbols);
  input.cursors = std::move(cursors);
  input.result_columns = std::move(result_columns);
  input.instructions = std::move(instructions);
  return TakeProgramValue(BytecodeProgram::Create(input));
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] const SqlValue& OnlyRowValue(const ReadVm& vm) {
  const std::span<const SqlValue> row = vm.row();
  if (row.size() != 1U) {
    throw std::runtime_error("expected exactly one VM result value");
  }
  return row.front();
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
  };
}

class ReadVmTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pager_ = TakeValue(ReadPager::Open(vfs_, FixturePath().string(),
                                       ReadPagerOptions{.cache_capacity_pages = 256}));
    RequireStatus(pager_->BeginRead());
  }

  void TearDown() override {
    if (pager_ != nullptr && pager_->in_read_transaction()) {
      const Status ended = pager_->EndRead();
      EXPECT_TRUE(ended.has_value()) << ended.error().ToString();
    }
  }

  [[nodiscard]] ReadVm CreateCoreVm(const BytecodeProgram& program, ReadVmLimits limits = {}) {
    return TakeValue(
        ReadVm::Create(program, ReadVmEnvironment::Core(*pager_, kCatalogGeneration), limits));
  }

  PosixVfs vfs_;
  std::unique_ptr<ReadPager> pager_;
};

TEST_F(ReadVmTest, ExecutesBindingsRowsHaltAndReset) {
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

  ReadVm vm = CreateCoreVm(program);
  EXPECT_EQ(vm.state(), ReadVmState::kReady);

  const SqlValue binding = SqlValue::Integer(2);
  RequireStatus(vm.Bind(Parameter(0), binding));
  EXPECT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
  EXPECT_EQ(vm.state(), ReadVmState::kRow);
  ExpectInteger(OnlyRowValue(vm), 42);
  EXPECT_EQ(vm.executed_instruction_count(), 4U);
  const Status rebound_while_suspended = vm.Bind(Parameter(0), binding);
  ASSERT_FALSE(rebound_while_suspended.has_value());
  EXPECT_EQ(rebound_while_suspended.error().code(), ErrorCode::kMisuse);

  EXPECT_EQ(TakeValue(vm.Step()), ReadVmStep::kDone);
  EXPECT_EQ(vm.state(), ReadVmState::kDone);
  EXPECT_TRUE(vm.row().empty());
  EXPECT_EQ(vm.executed_instruction_count(), 5U);
  const auto repeated = vm.Step();
  ASSERT_FALSE(repeated.has_value());
  EXPECT_EQ(repeated.error().code(), ErrorCode::kMisuse);

  RequireStatus(vm.Reset());
  EXPECT_EQ(vm.state(), ReadVmState::kReady);
  EXPECT_EQ(vm.executed_instruction_count(), 0U);
  EXPECT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
  ExpectInteger(OnlyRowValue(vm), 42);

  RequireStatus(vm.Reset());
  RequireStatus(vm.ClearBindings());
  EXPECT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
  EXPECT_EQ(OnlyRowValue(vm).type(), SqlValueType::kNull);
}

TEST_F(ReadVmTest, InvalidatesSuspendedRowsWhenTheReadSnapshotEnds) {
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
  ReadVm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
  ExpectInteger(OnlyRowValue(vm), 7);
  RequireStatus(pager_->EndRead());

  const auto resumed = vm.Step();
  ASSERT_FALSE(resumed.has_value());
  EXPECT_EQ(resumed.error().code(), ErrorCode::kMisuse);
  EXPECT_EQ(vm.state(), ReadVmState::kError);
  EXPECT_TRUE(vm.row().empty());

  RequireStatus(vm.Reset());
  RequireStatus(pager_->BeginRead());
  ASSERT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
  ExpectInteger(OnlyRowValue(vm), 7);
}

[[nodiscard]] SqlValue EvaluateUnary(ReadPager& pager, UnaryOperation operation, SqlValue input) {
  std::vector<SqlValue> constants;
  constants.push_back(std::move(input));
  const BytecodeProgram program =
      BuildProgram(pager, 2, 0, std::move(constants), {}, {}, {ResultColumn()},
                   {
                       LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
                       UnaryInstruction{.operation = operation, .input = Reg(0), .output = Reg(1)},
                       ResultRowInstruction{.first = Reg(1), .count = 1},
                       HaltInstruction{},
                   });
  ReadVm vm =
      TakeValue(ReadVm::Create(program, ReadVmEnvironment::Core(pager, kCatalogGeneration)));
  if (TakeValue(vm.Step()) != ReadVmStep::kRow) {
    throw std::runtime_error("unary VM program did not yield a row");
  }
  return OnlyRowValue(vm).Clone();
}

[[nodiscard]] SqlValue EvaluateBinary(ReadPager& pager, BinaryOperation operation, SqlValue left,
                                      SqlValue right) {
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
                           .output = Reg(2),
                       },
                       ResultRowInstruction{.first = Reg(2), .count = 1},
                       HaltInstruction{},
                   });
  ReadVm vm =
      TakeValue(ReadVm::Create(program, ReadVmEnvironment::Core(pager, kCatalogGeneration)));
  if (TakeValue(vm.Step()) != ReadVmStep::kRow) {
    throw std::runtime_error("binary VM program did not yield a row");
  }
  return OnlyRowValue(vm).Clone();
}

TEST_F(ReadVmTest, MatchesPinnedSqliteUnaryAndArithmeticSemantics) {
  ExpectInteger(EvaluateUnary(*pager_, UnaryOperation::kNegate, SqlValue::Text("5")), -5);
  ExpectReal(EvaluateUnary(*pager_, UnaryOperation::kNegate, SqlValue::Text("1.5")), -1.5);
  ExpectInteger(EvaluateUnary(*pager_, UnaryOperation::kNegate, SqlValue::Text("abc")), 0);
  ExpectReal(EvaluateUnary(*pager_, UnaryOperation::kNegate,
                           SqlValue::Integer(std::numeric_limits<std::int64_t>::min())),
             9223372036854775808.0);
  ExpectInteger(EvaluateUnary(*pager_, UnaryOperation::kBitwiseNot, SqlValue::Text("3.9")), -4);
  ExpectInteger(EvaluateUnary(*pager_, UnaryOperation::kLogicalNot, SqlValue::Text("2x")), 0);
  ExpectInteger(EvaluateUnary(*pager_, UnaryOperation::kLogicalNot, SqlValue::Text("abc")), 1);
  EXPECT_EQ(EvaluateUnary(*pager_, UnaryOperation::kLogicalNot, SqlValue{}).type(),
            SqlValueType::kNull);

  ExpectInteger(
      EvaluateBinary(*pager_, BinaryOperation::kAdd, SqlValue::Integer(2), SqlValue::Integer(3)),
      5);
  ExpectReal(
      EvaluateBinary(*pager_, BinaryOperation::kAdd, SqlValue::Text("1.0"), SqlValue::Integer(2)),
      3.0);
  ExpectInteger(
      EvaluateBinary(*pager_, BinaryOperation::kAdd, SqlValue::Text("abc"), SqlValue::Integer(2)),
      2);
  ExpectReal(EvaluateBinary(*pager_, BinaryOperation::kAdd,
                            SqlValue::Integer(std::numeric_limits<std::int64_t>::max()),
                            SqlValue::Integer(1)),
             9223372036854775808.0);
  ExpectReal(EvaluateBinary(*pager_, BinaryOperation::kMultiply, SqlValue::Integer(3037000500),
                            SqlValue::Integer(3037000500)),
             9.2233720370002493e18);
  ExpectInteger(
      EvaluateBinary(*pager_, BinaryOperation::kDivide, SqlValue::Integer(7), SqlValue::Integer(2)),
      3);
  ExpectReal(
      EvaluateBinary(*pager_, BinaryOperation::kDivide, SqlValue::Integer(7), SqlValue::Real(2.0)),
      3.5);
  EXPECT_EQ(
      EvaluateBinary(*pager_, BinaryOperation::kDivide, SqlValue::Integer(7), SqlValue::Integer(0))
          .type(),
      SqlValueType::kNull);
  ExpectReal(EvaluateBinary(*pager_, BinaryOperation::kRemainder, SqlValue::Real(7.9),
                            SqlValue::Real(2.1)),
             1.0);
  ExpectInteger(EvaluateBinary(*pager_, BinaryOperation::kRemainder,
                               SqlValue::Integer(std::numeric_limits<std::int64_t>::min()),
                               SqlValue::Integer(-1)),
                0);
}

TEST_F(ReadVmTest, MatchesPinnedSqliteConcatenationBitwiseAndLogicalSemantics) {
  ExpectText(EvaluateBinary(*pager_, BinaryOperation::kConcatenate, SqlValue::Integer(12),
                            SqlValue::Real(3.5)),
             "123.5");
  ExpectInteger(EvaluateBinary(*pager_, BinaryOperation::kBitwiseAnd, SqlValue::Integer(6),
                               SqlValue::Integer(3)),
                2);
  ExpectInteger(EvaluateBinary(*pager_, BinaryOperation::kShiftLeft, SqlValue::Integer(8),
                               SqlValue::Integer(-1)),
                4);
  ExpectInteger(EvaluateBinary(*pager_, BinaryOperation::kShiftRight, SqlValue::Integer(8),
                               SqlValue::Integer(-1)),
                16);
  ExpectInteger(EvaluateBinary(*pager_, BinaryOperation::kShiftLeft, SqlValue::Integer(-1),
                               SqlValue::Integer(64)),
                0);
  ExpectInteger(EvaluateBinary(*pager_, BinaryOperation::kShiftRight, SqlValue::Integer(-1),
                               SqlValue::Integer(64)),
                -1);
  ExpectInteger(EvaluateBinary(*pager_, BinaryOperation::kShiftLeft, SqlValue::Integer(1),
                               SqlValue::Integer(std::numeric_limits<std::int64_t>::min())),
                0);

  ExpectInteger(
      EvaluateBinary(*pager_, BinaryOperation::kLogicalAnd, SqlValue{}, SqlValue::Integer(0)), 0);
  EXPECT_EQ(EvaluateBinary(*pager_, BinaryOperation::kLogicalAnd, SqlValue{}, SqlValue::Integer(1))
                .type(),
            SqlValueType::kNull);
  EXPECT_EQ(
      EvaluateBinary(*pager_, BinaryOperation::kLogicalOr, SqlValue{}, SqlValue::Integer(0)).type(),
      SqlValueType::kNull);
  ExpectInteger(
      EvaluateBinary(*pager_, BinaryOperation::kLogicalOr, SqlValue{}, SqlValue::Integer(1)), 1);
}

[[nodiscard]] SqlValue EvaluateJump(ReadPager& pager, JumpCondition condition, SqlValue input) {
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
  ReadVm vm =
      TakeValue(ReadVm::Create(program, ReadVmEnvironment::Core(pager, kCatalogGeneration)));
  if (TakeValue(vm.Step()) != ReadVmStep::kRow) {
    throw std::runtime_error("conditional VM program did not yield a row");
  }
  return OnlyRowValue(vm).Clone();
}

TEST_F(ReadVmTest, ExecutesCopyAffinityCastAndConditionalJumps) {
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
  ReadVm conversion_vm = CreateCoreVm(conversion);
  ASSERT_EQ(TakeValue(conversion_vm.Step()), ReadVmStep::kRow);
  ASSERT_EQ(conversion_vm.row().size(), 3U);
  ExpectText(conversion_vm.row()[0], "42.5");
  ExpectReal(conversion_vm.row()[1], 42.5);
  ExpectInteger(conversion_vm.row()[2], 42);

  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfTrue, SqlValue::Integer(1)), 1);
  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfTrue, SqlValue{}), 0);
  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfFalse, SqlValue::Integer(0)), 1);
  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfNull, SqlValue{}), 1);
  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfNotNull, SqlValue::Integer(0)), 1);
  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfNotNull, SqlValue{}), 0);
}

TEST_F(ReadVmTest, ExecutesStrictIntegerAndIntegerOnlyRealAffinity) {
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
  ReadVm conversion_vm = CreateCoreVm(conversion);
  ASSERT_EQ(TakeValue(conversion_vm.Step()), ReadVmStep::kRow);
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
  ReadVm invalid_vm = CreateCoreVm(invalid);
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
  ReadVm nan_vm = CreateCoreVm(nan);
  const auto nan_failed = nan_vm.Step();
  ASSERT_FALSE(nan_failed.has_value());
  EXPECT_EQ(ErrorCode::kTypeMismatch, nan_failed.error().code());
}

TEST_F(ReadVmTest, AppliesComparisonAndScalarCallsWithAliasedOutputs) {
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

  ReadVm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
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
  ReadVm comparison_vm = CreateCoreVm(text_comparison);
  ASSERT_EQ(TakeValue(comparison_vm.Step()), ReadVmStep::kRow);
  ExpectInteger(OnlyRowValue(comparison_vm), 0);
}

class LengthCollation final : public Collation {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "LENGTH"; }

  [[nodiscard]] std::weak_ordering Compare(Utf8View left, Utf8View right) const noexcept override {
    return left.bytes().size() <=> right.bytes().size();
  }
};

TEST_F(ReadVmTest, ResolvesCustomCollationsAndRejectsMissingRuntimeRegistrations) {
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
  ReadVm custom_vm = TakeValue(ReadVm::Create(
      custom, ReadVmEnvironment{*pager_, kCatalogGeneration, CoreFunctionRegistry(), collations}));
  ASSERT_EQ(TakeValue(custom_vm.Step()), ReadVmStep::kRow);
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
  const auto unresolved_collation =
      ReadVm::Create(missing_collation, ReadVmEnvironment::Core(*pager_, kCatalogGeneration));
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
  const auto unresolved_function =
      ReadVm::Create(missing_function, ReadVmEnvironment::Core(*pager_, kCatalogGeneration));
  ASSERT_FALSE(unresolved_function.has_value());
  EXPECT_EQ(unresolved_function.error().code(), ErrorCode::kGeneric);
}

TEST_F(ReadVmTest, EnforcesSchemaIdentityAndInstructionBudgets) {
  const BytecodeProgram halt =
      BuildProgram(*pager_, 0, 0, {}, {}, {}, {}, {HaltInstruction{}}, std::nullopt, true);

  const auto generation_mismatch =
      ReadVm::Create(halt, ReadVmEnvironment::Core(*pager_, kCatalogGeneration + 1U));
  ASSERT_FALSE(generation_mismatch.has_value());
  EXPECT_EQ(generation_mismatch.error().code(), ErrorCode::kSchemaChanged);

  SchemaVersionRequirement wrong_cookie = CurrentSchema(*pager_);
  ++wrong_cookie.schema_cookie;
  const BytecodeProgram wrong_schema =
      BuildProgram(*pager_, 0, 0, {}, {}, {}, {}, {HaltInstruction{}}, wrong_cookie);
  const auto cookie_mismatch =
      ReadVm::Create(wrong_schema, ReadVmEnvironment::Core(*pager_, kCatalogGeneration));
  ASSERT_FALSE(cookie_mismatch.has_value());
  EXPECT_EQ(cookie_mismatch.error().code(), ErrorCode::kSchemaChanged);

  RequireStatus(pager_->EndRead());
  const auto inactive = ReadVm::Create(halt, ReadVmEnvironment::Core(*pager_, kCatalogGeneration));
  ASSERT_FALSE(inactive.has_value());
  EXPECT_EQ(inactive.error().code(), ErrorCode::kMisuse);
  const auto inactive_stale =
      ReadVm::Create(halt, ReadVmEnvironment::Core(*pager_, kCatalogGeneration + 1U));
  ASSERT_FALSE(inactive_stale.has_value());
  EXPECT_EQ(inactive_stale.error().code(), ErrorCode::kMisuse);
  RequireStatus(pager_->BeginRead());

  const BytecodeProgram loop =
      BuildProgram(*pager_, 0, 0, {}, {}, {}, {}, {JumpInstruction{.target = Address(0)}});
  ReadVm vm = CreateCoreVm(loop, ReadVmLimits{
                                     .maximum_value_bytes = 1'000'000'000,
                                     .maximum_instructions_per_step = 10,
                                 });
  const auto interrupted = vm.Step();
  ASSERT_FALSE(interrupted.has_value());
  EXPECT_EQ(interrupted.error().code(), ErrorCode::kInterrupted);
  EXPECT_EQ(vm.state(), ReadVmState::kError);
  EXPECT_EQ(vm.executed_instruction_count(), 10U);
}

TEST(ReadVmSnapshotTest, ResetCanAttachToANewDataOnlySnapshot) {
  std::vector<std::byte> bytes = ReadBytes(FixturePath());
  TemporaryDatabase database(bytes);
  PosixVfs vfs;
  std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, database.path().string()));
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
  ReadVm vm =
      TakeValue(ReadVm::Create(program, ReadVmEnvironment::Core(*pager, kCatalogGeneration)));
  ASSERT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
  ASSERT_EQ(TakeValue(vm.Step()), ReadVmStep::kDone);
  RequireStatus(vm.Reset());
  RequireStatus(pager->EndRead());

  const std::uint32_t next_change = Read32(bytes, 24) + 1U;
  Write32(bytes, 24, next_change);
  Write32(bytes, 92, next_change);
  database.Rewrite(bytes);

  RequireStatus(pager->BeginRead());
  ASSERT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
  ExpectInteger(OnlyRowValue(vm), 9);
  RequireStatus(vm.Reset());
  RequireStatus(pager->EndRead());
}

[[nodiscard]] BytecodeProgram TableScanProgram(const ReadPager& pager) {
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

TEST_F(ReadVmTest, ScansTableRowsAndClosesCursorsAtHalt) {
  const BytecodeProgram program = TableScanProgram(*pager_);
  ReadVm vm = CreateCoreVm(program);

  std::size_t rows = 0;
  std::int64_t first_rowid = 0;
  std::int64_t last_rowid = 0;
  std::string first_key;
  while (true) {
    const ReadVmStep step = TakeValue(vm.Step());
    if (step == ReadVmStep::kDone) {
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

TEST_F(ReadVmTest, KeepsCursorPinsAcrossRowsAndReleasesThemOnReset) {
  const BytecodeProgram program = TableScanProgram(*pager_);
  ReadVm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);

  const Status pinned = pager_->EndRead();
  ASSERT_FALSE(pinned.has_value());
  EXPECT_EQ(pinned.error().code(), ErrorCode::kBusy);

  RequireStatus(vm.Reset());
  RequireStatus(pager_->EndRead());

  RequireStatus(pager_->BeginRead());
  {
    ReadVm scoped_vm = CreateCoreVm(program);
    ASSERT_EQ(TakeValue(scoped_vm.Step()), ReadVmStep::kRow);
    const Status scoped_pin = pager_->EndRead();
    ASSERT_FALSE(scoped_pin.has_value());
    EXPECT_EQ(scoped_pin.error().code(), ErrorCode::kBusy);
  }
  RequireStatus(pager_->EndRead());
}

[[nodiscard]] BytecodeProgram SeekFieldProgram(const ReadPager& pager, CursorFieldSource source,
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

TEST_F(ReadVmTest, SeeksRowidsReadsOverflowFieldsAndReturnsNullForMissingFields) {
  const BytecodeProgram payload_program =
      SeekFieldProgram(*pager_, CursorFieldSource{
                                    .kind = CursorFieldSourceKind::kRecordField,
                                    .record_field = 6,
                                });
  ReadVm payload_vm = CreateCoreVm(payload_program);
  const SqlValue key = SqlValue::Text("44.0");
  RequireStatus(payload_vm.Bind(Parameter(0), key));
  ASSERT_EQ(TakeValue(payload_vm.Step()), ReadVmStep::kRow);
  ASSERT_EQ(OnlyRowValue(payload_vm).type(), SqlValueType::kBlob);
  EXPECT_EQ(OnlyRowValue(payload_vm).blob_value().value_or(ByteView{}).size(), 3000U);
  EXPECT_EQ(TakeValue(payload_vm.Step()), ReadVmStep::kDone);

  const BytecodeProgram missing_program =
      SeekFieldProgram(*pager_,
                       CursorFieldSource{
                           .kind = CursorFieldSourceKind::kRecordField,
                           .record_field = 7,
                       },
                       8);
  ReadVm missing_vm = CreateCoreVm(missing_program);
  const SqlValue integer_key = SqlValue::Integer(44);
  RequireStatus(missing_vm.Bind(Parameter(0), integer_key));
  ASSERT_EQ(TakeValue(missing_vm.Step()), ReadVmStep::kRow);
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
  ReadVm default_vm = CreateCoreVm(default_program);
  RequireStatus(default_vm.Bind(Parameter(0), integer_key));
  ASSERT_EQ(TakeValue(default_vm.Step()), ReadVmStep::kRow);
  ExpectText(OnlyRowValue(default_vm), "legacy");

  const BytecodeProgram unsupported_program =
      SeekFieldProgram(*pager_,
                       CursorFieldSource{
                           .kind = CursorFieldSourceKind::kRecordField,
                           .record_field = 7,
                           .missing_value_kind = MissingFieldValueKind::kUnsupported,
                       },
                       8);
  ReadVm unsupported_vm = CreateCoreVm(unsupported_program);
  RequireStatus(unsupported_vm.Bind(Parameter(0), integer_key));
  const auto unsupported = unsupported_vm.Step();
  ASSERT_FALSE(unsupported.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, unsupported.error().code());

  RequireStatus(missing_vm.Reset());
  const SqlValue fractional = SqlValue::Real(44.5);
  RequireStatus(missing_vm.Bind(Parameter(0), fractional));
  EXPECT_EQ(TakeValue(missing_vm.Step()), ReadVmStep::kDone);

  RequireStatus(missing_vm.Reset());
  const SqlValue rounded_past_max =
      SqlValue::Real(static_cast<double>(std::numeric_limits<std::int64_t>::max()));
  RequireStatus(missing_vm.Bind(Parameter(0), rounded_past_max));
  EXPECT_EQ(TakeValue(missing_vm.Step()), ReadVmStep::kDone);

  RequireStatus(missing_vm.Reset());
  const SqlValue minimum =
      SqlValue::Real(static_cast<double>(std::numeric_limits<std::int64_t>::min()));
  RequireStatus(missing_vm.Bind(Parameter(0), minimum));
  ASSERT_EQ(TakeValue(missing_vm.Step()), ReadVmStep::kRow);
  EXPECT_EQ(OnlyRowValue(missing_vm).type(), SqlValueType::kNull);
}

TEST_F(ReadVmTest, ScansIndexRecordsWithResolvedOrderingMetadata) {
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
  ReadVm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
  ASSERT_EQ(vm.row().size(), 2U);
  ExpectText(vm.row()[0], "bin-00");
  ExpectInteger(vm.row()[1], 13);
}

TEST_F(ReadVmTest, ReadsRowidsThroughDedicatedAndDescriptorInstructions) {
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
  ReadVm vm = CreateCoreVm(program);
  ASSERT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
  ASSERT_EQ(vm.row().size(), 2U);
  ExpectInteger(vm.row()[0], std::numeric_limits<std::int64_t>::min());
  ExpectInteger(vm.row()[1], std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(TakeValue(vm.Step()), ReadVmStep::kDone);
}

[[nodiscard]] Result<SqlValue> ThrowingFunction(const ScalarFunctionContext&,
                                                std::span<const SqlValue>) {
  throw std::runtime_error("expected scalar callback failure");
}

TEST_F(ReadVmTest, CleansUpCursorsBeforeRethrowingForeignExceptions) {
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
  ReadVm vm = TakeValue(ReadVm::Create(
      program, ReadVmEnvironment{*pager_, kCatalogGeneration, registry, collations}));

  EXPECT_THROW(
      {
        const auto result = vm.Step();
        EXPECT_FALSE(result.has_value());
      },
      std::runtime_error);
  EXPECT_EQ(vm.state(), ReadVmState::kError);
  RequireStatus(pager_->EndRead());
}

TEST_F(ReadVmTest, EnforcesValueLimitsAtCreationBindingAndExecution) {
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
      ReadVm::Create(constant_program, ReadVmEnvironment::Core(*pager_, kCatalogGeneration),
                     ReadVmLimits{.maximum_value_bytes = 3});
  ASSERT_FALSE(oversized_constant.has_value());
  EXPECT_EQ(oversized_constant.error().code(), ErrorCode::kTooLarge);

  const BytecodeProgram parameter_program =
      BuildProgram(*pager_, 1, 1, {}, {}, {}, {ResultColumn()},
                   {
                       LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   });
  ReadVm parameter_vm = CreateCoreVm(parameter_program, ReadVmLimits{.maximum_value_bytes = 3});
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
  ReadVm concat_vm = CreateCoreVm(concat_program, ReadVmLimits{.maximum_value_bytes = 3});
  const auto concatenated = concat_vm.Step();
  ASSERT_FALSE(concatenated.has_value());
  EXPECT_EQ(concatenated.error().code(), ErrorCode::kTooLarge);
  EXPECT_EQ(concat_vm.state(), ReadVmState::kError);
}

TEST_F(ReadVmTest, UsesSharedDatabaseFormatNormalization) {
  std::vector<std::byte> bytes = ReadBytes(FixturePath());
  Write32(bytes, 44, 0);
  Write32(bytes, 56, 0);
  const TemporaryDatabase database(bytes);
  PosixVfs vfs;
  std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, database.path().string()));
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
  ReadVm vm =
      TakeValue(ReadVm::Create(program, ReadVmEnvironment::Core(*pager, kCatalogGeneration)));
  EXPECT_EQ(TakeValue(vm.Step()), ReadVmStep::kDone);
  RequireStatus(pager->EndRead());
}

TEST_F(ReadVmTest, MovedFromMachinesRejectOperations) {
  const BytecodeProgram program = BuildProgram(*pager_, 0, 0, {}, {}, {}, {}, {HaltInstruction{}});
  ReadVm source = CreateCoreVm(program);
  ReadVm destination = std::move(source);

  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(source.state(), ReadVmState::kInvalid);
  const auto stepped = source.Step();
  ASSERT_FALSE(stepped.has_value());
  EXPECT_EQ(stepped.error().code(), ErrorCode::kMisuse);
  const Status reset = source.Reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(reset.error().code(), ErrorCode::kMisuse);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)

  EXPECT_EQ(TakeValue(destination.Step()), ReadVmStep::kDone);
}

TEST(ReadVm, ExecutesTransactionFreeProgramsAndPublishesBindingsWithoutAPagerSnapshot) {
  PosixVfs vfs;
  std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const BytecodeProgram program =
      BuildProgram(*pager, 1, 1, {}, {}, {}, {ResultColumn()},
                   {
                       LoadParameterInstruction{.parameter = Parameter(0), .output = Reg(0)},
                       ResultRowInstruction{.first = Reg(0), .count = 1},
                       HaltInstruction{},
                   });
  ASSERT_FALSE(program.requires_read_transaction());
  ReadVm vm =
      TakeValue(ReadVm::Create(program, ReadVmEnvironment::Core(*pager, kCatalogGeneration)));
  const SqlValue value = SqlValue::Integer(41);
  RequireStatus(vm.Bind(Parameter(0), value));
  ASSERT_EQ(1U, vm.bindings().size());
  ExpectInteger(vm.bindings().front(), 41);
  RequireStatus(pager->EndRead());

  EXPECT_EQ(ReadVmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 41);
  EXPECT_EQ(ReadVmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(vm.Reset());
  ASSERT_EQ(1U, vm.bindings().size());
  ExpectInteger(vm.bindings().front(), 41);
}

TEST(ReadVm, RejectsTransactionRequiredProgramsWithoutAPagerSnapshot) {
  PosixVfs vfs;
  std::unique_ptr<ReadPager> pager = TakeValue(ReadPager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager);
  input.requires_read_transaction = true;
  input.instructions.emplace_back(HaltInstruction{});
  const BytecodeProgram program = TakeProgramValue(BytecodeProgram::Create(input));
  ReadVm vm =
      TakeValue(ReadVm::Create(program, ReadVmEnvironment::Core(*pager, kCatalogGeneration)));
  RequireStatus(pager->EndRead());

  const auto stepped = vm.Step();

  ASSERT_FALSE(stepped.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, stepped.error().code());
  EXPECT_EQ(ReadVmState::kError, vm.state());
}

#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
TEST_F(ReadVmTest, RecordsExecutedInstructionsWhenInstrumentationIsEnabled) {
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
  ReadVm vm = CreateCoreVm(program);
  instrumentation::CounterCollection counters;
  const instrumentation::ScopedCounterCollection collection(counters);

  EXPECT_EQ(TakeValue(vm.Step()), ReadVmStep::kRow);
  EXPECT_EQ(counters.Value(instrumentation::Counter::kVmInstructions), 2U);
  EXPECT_EQ(TakeValue(vm.Step()), ReadVmStep::kDone);
  EXPECT_EQ(counters.Value(instrumentation::Counter::kVmInstructions), 3U);
}
#endif

}  // namespace
}  // namespace modern_sqlite
