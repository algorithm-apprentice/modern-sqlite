#include "modern_sqlite/vm/vm.hpp"

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
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/transaction/transaction_coordinator.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<Vm>);
static_assert(!std::is_copy_assignable_v<Vm>);
static_assert(std::is_nothrow_move_constructible_v<Vm>);
static_assert(std::is_nothrow_move_assignable_v<Vm>);

constexpr std::uint64_t kCatalogGeneration = 17;

[[nodiscard]] constexpr RegisterId Reg(std::uint32_t value) { return RegisterId(value); }
[[nodiscard]] constexpr CursorId Cursor(std::uint32_t value) { return CursorId(value); }
[[nodiscard]] constexpr WriteCursorId WriteCursor(std::uint32_t value) {
  return WriteCursorId(value);
}
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

[[nodiscard]] Vm CreateAttachedVm(const BytecodeProgram& program, Pager& pager,
                                  std::uint64_t catalog_generation,
                                  VmEnvironment environment = VmEnvironment::Core(),
                                  VmLimits limits = {}) {
  Vm vm = TakeValue(Vm::Create(program, environment, limits));
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{pager, catalog_generation}));
  return vm;
}

[[nodiscard]] TransactionCoordinator OpenWriteCoordinator(test::WritePagerFixedVfs& vfs) {
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error{"failed to open VM write-test pager"};
  }
  return TakeValue(TransactionCoordinator::Open(std::move(pager)));
}

[[nodiscard]] ByteBuffer InitializedWriteDatabase(test::WritePagerFixedVfs& vfs) {
  TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
  TransactionStatement statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  RequireStatus(statement.writer()->InitializeDatabase());
  RequireStatus(statement.Succeed());
  return ByteBuffer::CopyOf(vfs.database_bytes());
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
  });
  input.instructions = {
      LoadConstantInstruction{.constant = Constant(0), .output = Reg(0)},
      LoadConstantInstruction{.constant = Constant(1), .output = Reg(1)},
      LoadConstantInstruction{.constant = Constant(2), .output = Reg(2)},
      OpenWriteCursorInstruction{.cursor = WriteCursor(0)},
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
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error{"failed to reopen VM write-test pager"};
  }
  RequireStatus(pager->BeginRead());
  std::vector<std::pair<std::int64_t, std::vector<SqlValue>>> rows;
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
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
  };
}

class VmTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pager_ = TakeValue(
        Pager::Open(vfs_, FixturePath().string(), PagerOptions{.cache_capacity_pages = 256}));
    RequireStatus(pager_->BeginRead());
  }

  void TearDown() override {
    if (pager_ != nullptr && pager_->in_read_transaction()) {
      const Status ended = pager_->EndRead();
      EXPECT_TRUE(ended.has_value()) << ended.error().ToString();
    }
  }

  [[nodiscard]] Vm CreateCoreVm(const BytecodeProgram& program, VmLimits limits = {}) {
    return CreateAttachedVm(program, *pager_, kCatalogGeneration, VmEnvironment::Core(), limits);
  }

  PosixVfs vfs_;
  std::unique_ptr<Pager> pager_;
};

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
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration}));
  EXPECT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ExpectInteger(OnlyRowValue(vm), 42);

  RequireStatus(vm.Reset());
  RequireStatus(vm.ClearBindings());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration}));
  EXPECT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  EXPECT_EQ(OnlyRowValue(vm).type(), SqlValueType::kNull);
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
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration}));
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

  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration}));
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
  write_input.instructions.emplace_back(HaltInstruction{});
  const BytecodeProgram write_program = TakeProgramValue(BytecodeProgram::Create(write_input));
  Vm write_vm = TakeValue(Vm::Create(write_program, VmEnvironment::Core()));
  const Status missing_writer =
      write_vm.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration});
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
    std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    const BytecodeProgram program = TableSeekProgram(SqlValue::Integer(1), RowIdSeekMode::kGreater);
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager, 0}));
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
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration}));
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ExpectInteger(OnlyRowValue(vm), 7);
}

[[nodiscard]] SqlValue EvaluateUnary(Pager& pager, UnaryOperation operation, SqlValue input) {
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
  Vm vm = CreateAttachedVm(program, pager, kCatalogGeneration);
  if (TakeValue(vm.Step()) != VmStep::kRow) {
    throw std::runtime_error("unary VM program did not yield a row");
  }
  return OnlyRowValue(vm).Clone();
}

[[nodiscard]] SqlValue EvaluateBinary(Pager& pager, BinaryOperation operation, SqlValue left,
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
  Vm vm = CreateAttachedVm(program, pager, kCatalogGeneration);
  if (TakeValue(vm.Step()) != VmStep::kRow) {
    throw std::runtime_error("binary VM program did not yield a row");
  }
  return OnlyRowValue(vm).Clone();
}

TEST_F(VmTest, MatchesPinnedSqliteUnaryAndArithmeticSemantics) {
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

TEST_F(VmTest, MatchesPinnedSqliteConcatenationBitwiseAndLogicalSemantics) {
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

[[nodiscard]] SqlValue EvaluateJump(Pager& pager, JumpCondition condition, SqlValue input) {
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
  Vm vm = CreateAttachedVm(program, pager, kCatalogGeneration);
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

  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfTrue, SqlValue::Integer(1)), 1);
  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfTrue, SqlValue{}), 0);
  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfFalse, SqlValue::Integer(0)), 1);
  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfNull, SqlValue{}), 1);
  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfNotNull, SqlValue::Integer(0)), 1);
  ExpectInteger(EvaluateJump(*pager_, JumpCondition::kIfNotNull, SqlValue{}), 0);
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
  Vm custom_vm = CreateAttachedVm(custom, *pager_, kCatalogGeneration,
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
      VmExecutionContext{*pager_, kCatalogGeneration + 1U});
  ASSERT_FALSE(generation_attach.has_value());
  EXPECT_EQ(generation_attach.error().code(), ErrorCode::kSchemaChanged);

  SchemaVersionRequirement wrong_cookie = CurrentSchema(*pager_);
  ++wrong_cookie.schema_cookie;
  const BytecodeProgram wrong_schema =
      BuildProgram(*pager_, 0, 0, {}, {}, {}, {}, {HaltInstruction{}}, wrong_cookie);
  Vm cookie_mismatch = TakeValue(Vm::Create(wrong_schema, VmEnvironment::Core()));
  const Status cookie_attach =
      cookie_mismatch.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration});
  ASSERT_FALSE(cookie_attach.has_value());
  EXPECT_EQ(cookie_attach.error().code(), ErrorCode::kSchemaChanged);

  RequireStatus(pager_->EndRead());
  Vm inactive = TakeValue(Vm::Create(halt, VmEnvironment::Core()));
  const Status inactive_attach =
      inactive.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration});
  ASSERT_FALSE(inactive_attach.has_value());
  EXPECT_EQ(inactive_attach.error().code(), ErrorCode::kMisuse);
  Vm inactive_stale = TakeValue(Vm::Create(halt, VmEnvironment::Core()));
  const Status inactive_stale_attach =
      inactive_stale.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration + 1U});
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
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, database.path().string()));
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
  Vm vm = CreateAttachedVm(program, *pager, kCatalogGeneration);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kRow);
  ASSERT_EQ(TakeValue(vm.Step()), VmStep::kDone);
  RequireStatus(vm.Reset());
  RequireStatus(pager->EndRead());

  const std::uint32_t next_change = Read32(bytes, 24) + 1U;
  Write32(bytes, 24, next_change);
  Write32(bytes, 92, next_change);
  database.Rewrite(bytes);

  RequireStatus(pager->BeginRead());
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager, kCatalogGeneration}));
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
  RequireStatus(missing_vm.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration}));
  EXPECT_EQ(TakeValue(missing_vm.Step()), VmStep::kDone);

  RequireStatus(missing_vm.Reset());
  const SqlValue rounded_past_max =
      SqlValue::Real(static_cast<double>(std::numeric_limits<std::int64_t>::max()));
  RequireStatus(missing_vm.Bind(Parameter(0), rounded_past_max));
  RequireStatus(missing_vm.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration}));
  EXPECT_EQ(TakeValue(missing_vm.Step()), VmStep::kDone);

  RequireStatus(missing_vm.Reset());
  const SqlValue minimum =
      SqlValue::Real(static_cast<double>(std::numeric_limits<std::int64_t>::min()));
  RequireStatus(missing_vm.Bind(Parameter(0), minimum));
  RequireStatus(missing_vm.AttachExecutionContext(VmExecutionContext{*pager_, kCatalogGeneration}));
  ASSERT_EQ(TakeValue(missing_vm.Step()), VmStep::kRow);
  EXPECT_EQ(OnlyRowValue(missing_vm).type(), SqlValueType::kNull);
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

[[nodiscard]] Result<SqlValue> ThrowingFunction(const ScalarFunctionContext&,
                                                std::span<const SqlValue>) {
  throw std::runtime_error("expected scalar callback failure");
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
  Vm vm =
      CreateAttachedVm(program, *pager_, kCatalogGeneration, VmEnvironment{registry, collations});

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
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, database.path().string()));
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
  Vm vm = CreateAttachedVm(program, *pager, kCatalogGeneration);
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
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
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
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*pager, kCatalogGeneration}));

  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ExpectInteger(OnlyRowValue(vm), 41);
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(vm.Reset());
  ASSERT_EQ(1U, vm.bindings().size());
  ExpectInteger(vm.bindings().front(), 41);
}

TEST(Vm, RejectsSnapshotRequiredProgramsWithoutAReadTransaction) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  ProgramInput input;
  input.schema_version = CurrentSchema(*pager);
  input.requires_database_snapshot = true;
  input.instructions.emplace_back(HaltInstruction{});
  const BytecodeProgram program = TakeProgramValue(BytecodeProgram::Create(input));
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(pager->EndRead());

  const Status attached = vm.AttachExecutionContext(VmExecutionContext{*pager, kCatalogGeneration});
  ASSERT_FALSE(attached.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, attached.error().code());
  EXPECT_EQ(VmState::kReady, vm.state());
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
