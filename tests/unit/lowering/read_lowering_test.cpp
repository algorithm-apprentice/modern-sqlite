#include "modern_sqlite/lowering/read_lowering.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/catalog/catalog_loader.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/planner/logical_plan.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/syntax/parser.hpp"
#include "modern_sqlite/vm/read_vm.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<LowerReadPlanResult>);
static_assert(std::is_move_constructible_v<LowerReadPlanResult>);

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

template <typename T>
[[nodiscard]] T TakeOptional(std::optional<T> value, std::string_view message) {
  if (!value.has_value()) {
    throw std::runtime_error{std::string{message}};
  }
  return *value;
}

[[nodiscard]] std::string_view TextBytes(const SqlValue& value) {
  return TakeOptional(value.text_value(), "expected SQL text").bytes();
}

[[nodiscard]] ByteView BlobBytes(const SqlValue& value) {
  return TakeOptional(value.blob_value(), "expected SQL blob");
}

void RequireStatus(Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

[[nodiscard]] SyntaxTree ParseTree(std::string_view sql) {
  ParseResult parsed = ParseOne(Utf8View{sql});
  if (!parsed.has_value()) {
    throw std::runtime_error{std::string{ParseErrorMessage(parsed.error())}};
  }
  if (!parsed->tree.has_value()) {
    throw std::runtime_error{"lowering test SQL did not produce a statement"};
  }
  return std::move(*parsed->tree);
}

[[nodiscard]] CatalogSnapshotPtr TestCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 31, .generation = 17},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT, score REAL)"));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = "items",
      .root_page = RootPageId{2},
      .columns =
          {
              CatalogColumnInput{
                  .name = "id",
                  .declared_type = "INTEGER",
                  .primary_key = true,
              },
              CatalogColumnInput{
                  .name = "name",
                  .declared_type = "TEXT",
              },
              CatalogColumnInput{
                  .name = "score",
                  .declared_type = "REAL",
              },
          },
      .rowid_alias = ColumnId{0},
  });
  CatalogSnapshotResult created = CatalogSnapshot::Create(std::move(input));
  if (!created.has_value()) {
    throw std::runtime_error{created.error().detail};
  }
  return *std::move(created);
}

[[nodiscard]] PhysicalPlan OptimizeOrThrow(std::string_view sql, const CatalogSnapshotPtr& catalog,
                                           BindEnvironment environment = BindEnvironment::Core()) {
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog, environment);
  if (!bound.has_value()) {
    throw std::runtime_error{bound.error().detail};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{logical.error().detail};
  }
  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical));
  if (!physical.has_value()) {
    throw std::runtime_error{physical.error().detail};
  }
  return std::move(*physical);
}

[[nodiscard]] BytecodeProgram LowerOrThrow(std::string_view sql, const CatalogSnapshotPtr& catalog,
                                           BindEnvironment environment = BindEnvironment::Core()) {
  const PhysicalPlan plan = OptimizeOrThrow(sql, catalog, environment);
  LowerReadPlanResult lowered = LowerReadPlan(plan);
  if (!lowered.has_value()) {
    throw std::runtime_error(lowered.error().detail);
  }
  return std::move(*lowered);
}

[[nodiscard]] std::vector<InstructionKind> InstructionKinds(const BytecodeProgram& program) {
  std::vector<InstructionKind> result;
  result.reserve(program.instructions().size());
  for (const Instruction& instruction : program.instructions()) {
    result.push_back(InstructionKindOf(instruction));
  }
  return result;
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "catalog_loader" / "sqlite-3.54.0-catalog.db";
}

[[nodiscard]] std::filesystem::path AlterDefaultsFixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "lowering" / "sqlite-3.54.0-alter-defaults.db";
}

[[nodiscard]] std::vector<std::vector<SqlValue>> ExecuteRows(const BytecodeProgram& program,
                                                             ReadVmEnvironment environment) {
  ReadVm vm = TakeValue(ReadVm::Create(program, environment));
  std::vector<std::vector<SqlValue>> rows;
  while (true) {
    const ReadVmStep step = TakeValue(vm.Step());
    if (step == ReadVmStep::kDone) {
      return rows;
    }
    std::vector<SqlValue> row;
    row.reserve(vm.row().size());
    for (const SqlValue& value : vm.row()) {
      row.push_back(value.Clone());
    }
    rows.push_back(std::move(row));
  }
}

std::size_t callback_count = 0;

[[nodiscard]] Result<SqlValue> ReturnOne(const ScalarFunctionContext&, std::span<const SqlValue>) {
  return SqlValue::Integer(1);
}

[[nodiscard]] Result<SqlValue> ReturnTwo(const ScalarFunctionContext&, std::span<const SqlValue>) {
  return SqlValue::Integer(2);
}

[[nodiscard]] Result<SqlValue> CountCall(const ScalarFunctionContext&, std::span<const SqlValue>) {
  ++callback_count;
  return SqlValue::Integer(1);
}

[[nodiscard]] Result<SqlValue> FailCall(const ScalarFunctionContext&, std::span<const SqlValue>) {
  ++callback_count;
  return std::unexpected(Error::Create(ErrorCode::kGeneric, "failing function executed"));
}

struct CustomEnvironment {
  std::array<ScalarFunction, 4> functions{{
      ScalarFunction{"stable_guard", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ReturnOne},
      ScalarFunction{"volatile_key", FunctionArity::Exact(0),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     ReturnTwo},
      ScalarFunction{"volatile_counter", FunctionArity::Exact(0),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     CountCall},
      ScalarFunction{"failing", FunctionArity::Exact(0), FunctionDeterminism::kNonDeterministic,
                     FunctionCollationUse::kNone, FailCall},
  }};
  FunctionRegistry registry{functions};
  std::array<const Collation*, 3> collations{
      &BinaryCollation(),
      &NoCaseCollation(),
      &RTrimCollation(),
  };

  [[nodiscard]] BindEnvironment Binder() const noexcept {
    return BindEnvironment{registry, collations, 29};
  }

  [[nodiscard]] ReadVmEnvironment Vm(Pager& pager,
                                     std::uint64_t catalog_generation) const noexcept {
    return ReadVmEnvironment{pager, catalog_generation, registry, collations};
  }
};

TEST(ReadLoweringApi, ExposesStableErrorsAndBaseMappings) {
  EXPECT_EQ("invalid_input", ReadLoweringErrorCodeName(ReadLoweringErrorCode::kInvalidInput));
  EXPECT_EQ("resource_limit", ReadLoweringErrorCodeName(ReadLoweringErrorCode::kResourceLimit));
  EXPECT_EQ("internal_invariant",
            ReadLoweringErrorCodeName(ReadLoweringErrorCode::kInternalInvariant));
  EXPECT_EQ("unknown",
            ReadLoweringErrorCodeName(static_cast<ReadLoweringErrorCode>(255)));  // NOLINT

  EXPECT_EQ(ErrorCode::kMisuse,
            ReadLoweringError{.code = ReadLoweringErrorCode::kInvalidInput}.base_error_code());
  EXPECT_EQ(ErrorCode::kTooLarge,
            ReadLoweringError{.code = ReadLoweringErrorCode::kResourceLimit}.base_error_code());
  EXPECT_EQ(ErrorCode::kInternal,
            ReadLoweringError{.code = ReadLoweringErrorCode::kInternalInvariant}.base_error_code());
}

TEST(ReadLowering, LowersConstantRowsIntoVerifiedOwnedPrograms) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan plan = OptimizeOrThrow("SELECT 1 AS answer", catalog);

  LowerReadPlanResult lowered = LowerReadPlan(plan);
  ASSERT_TRUE(lowered.has_value()) << lowered.error().detail;
  EXPECT_EQ((SchemaVersionRequirement{.schema_cookie = 31, .generation = 17}),
            lowered->schema_version());
  ASSERT_EQ(1U, lowered->result_columns().size());
  EXPECT_EQ("answer", lowered->result_columns()[0].name);
  EXPECT_EQ(TypeAffinity::kNone, lowered->result_columns()[0].affinity);
  ASSERT_EQ(1U, lowered->constants().size());
  EXPECT_EQ(1, lowered->constants()[0].integer_value());
  EXPECT_EQ((std::vector<InstructionKind>{
                InstructionKind::kLoadConstant,
                InstructionKind::kResultRow,
                InstructionKind::kHalt,
            }),
            InstructionKinds(*lowered));
  EXPECT_EQ(lowered->instructions().size(),
            lowered->verification_metrics().reachable_instruction_count);
}

TEST(ReadLowering, EmitsStableCanonicalAccessPathShapes) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const BytecodeProgram empty = LowerOrThrow("SELECT name FROM items WHERE 0", catalog);
  EXPECT_EQ((std::vector<InstructionKind>{InstructionKind::kHalt}), InstructionKinds(empty));

  const BytecodeProgram scan = LowerOrThrow("SELECT name FROM items", catalog);
  EXPECT_EQ((std::vector<InstructionKind>{
                InstructionKind::kOpenRead,
                InstructionKind::kRewind,
                InstructionKind::kReadField,
                InstructionKind::kResultRow,
                InstructionKind::kNext,
                InstructionKind::kHalt,
            }),
            InstructionKinds(scan));

  const BytecodeProgram lookup = LowerOrThrow("SELECT name FROM items WHERE rowid=2", catalog);
  EXPECT_EQ((std::vector<InstructionKind>{
                InstructionKind::kOpenRead,
                InstructionKind::kLoadConstant,
                InstructionKind::kSeekRowId,
                InstructionKind::kReadField,
                InstructionKind::kResultRow,
                InstructionKind::kHalt,
                InstructionKind::kHalt,
            }),
            InstructionKinds(lookup));
}

TEST(ReadLowering, RetainsExactProgramLimitFailuresAndRejectsMovedFromPlans) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  PhysicalPlan plan = OptimizeOrThrow("SELECT 1", catalog);

  ProgramLimits limits;
  limits.maximum_instructions = 2;
  LowerReadPlanResult limited = LowerReadPlan(plan, limits);
  ASSERT_FALSE(limited.has_value());
  EXPECT_EQ(ReadLoweringErrorCode::kResourceLimit, limited.error().code);
  ASSERT_TRUE(limited.error().program_error.has_value());
  EXPECT_EQ(ProgramErrorCode::kInstructionLimitExceeded,
            TakeOptional(limited.error().program_error, "missing nested program error").code);

  const PhysicalPlan moved = std::move(plan);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  ASSERT_FALSE(LowerReadPlan(plan).has_value());
  EXPECT_TRUE(LowerReadPlan(moved).has_value());
}

TEST(ReadLowering, PreservesNestedProgramResourceLimitCodes) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const auto expect_limit = [&](std::string_view sql, ProgramLimits limits,
                                ProgramErrorCode expected) {
    SCOPED_TRACE(sql);
    const PhysicalPlan plan = OptimizeOrThrow(sql, catalog);
    LowerReadPlanResult lowered = LowerReadPlan(plan, limits);
    ASSERT_FALSE(lowered.has_value());
    EXPECT_EQ(ReadLoweringErrorCode::kResourceLimit, lowered.error().code);
    ASSERT_TRUE(lowered.error().program_error.has_value());
    EXPECT_EQ(expected,
              TakeOptional(lowered.error().program_error, "missing nested program error").code);
  };

  ProgramLimits register_limit;
  register_limit.maximum_registers = 0;
  expect_limit("SELECT 1", register_limit, ProgramErrorCode::kRegisterLimitExceeded);

  ProgramLimits parameter_limit;
  parameter_limit.maximum_parameters = 0;
  expect_limit("SELECT ?1", parameter_limit, ProgramErrorCode::kParameterLimitExceeded);

  ProgramLimits constant_limit;
  constant_limit.maximum_constants = 0;
  expect_limit("SELECT 1", constant_limit, ProgramErrorCode::kConstantLimitExceeded);

  ProgramLimits symbol_limit;
  symbol_limit.maximum_symbols = 0;
  expect_limit("SELECT name FROM items", symbol_limit, ProgramErrorCode::kSymbolLimitExceeded);

  ProgramLimits cursor_limit;
  cursor_limit.maximum_cursors = 0;
  expect_limit("SELECT name FROM items", cursor_limit, ProgramErrorCode::kCursorLimitExceeded);

  ProgramLimits result_limit;
  result_limit.maximum_result_columns = 0;
  expect_limit("SELECT 1", result_limit, ProgramErrorCode::kResultColumnLimitExceeded);

  ProgramLimits label_limit;
  label_limit.maximum_labels = 0;
  expect_limit("SELECT 1", label_limit, ProgramErrorCode::kLabelLimitExceeded);

  ProgramLimits owned_bytes_limit;
  owned_bytes_limit.maximum_owned_bytes = 0;
  expect_limit("SELECT 1", owned_bytes_limit, ProgramErrorCode::kOwnedBytesLimitExceeded);

  ProgramLimits analysis_limit;
  analysis_limit.maximum_analysis_words = 0;
  expect_limit("SELECT 1", analysis_limit, ProgramErrorCode::kAnalysisLimitExceeded);

  ProgramLimits analysis_work_limit;
  analysis_work_limit.maximum_analysis_edge_words = 0;
  expect_limit("SELECT 1", analysis_work_limit, ProgramErrorCode::kAnalysisWorkLimitExceeded);
}

TEST(ReadLowering, LowersCursorDescriptorsAndSourceOrdering) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog = TakeValue(LoadCatalog(*pager));

  const BytecodeProgram table = LowerOrThrow("SELECT id, name, score FROM items", catalog);
  EXPECT_TRUE(table.requires_read_transaction());
  ASSERT_EQ(1U, table.cursors().size());
  EXPECT_EQ(CursorStorageKind::kRowIdTable, table.cursors()[0].storage);
  ASSERT_EQ(4U, table.cursors()[0].fields.size());
  EXPECT_EQ(CursorFieldSourceKind::kRowId, table.cursors()[0].fields[0].kind);
  EXPECT_EQ(CursorFieldSourceKind::kRecordField, table.cursors()[0].fields[2].kind);
  const std::vector<InstructionKind> table_kinds = InstructionKinds(table);
  EXPECT_NE(std::ranges::find(table_kinds, InstructionKind::kRealAffinity), table_kinds.end());

  const BytecodeProgram without_rowid = LowerOrThrow("SELECT a, b, c, payload FROM wr", catalog);
  ASSERT_EQ(1U, without_rowid.cursors().size());
  const ReadCursorDescriptor& descriptor = without_rowid.cursors()[0];
  EXPECT_EQ(CursorStorageKind::kIndex, descriptor.storage);
  EXPECT_EQ(4U, descriptor.record_field_count);
  ASSERT_EQ(4U, descriptor.index_columns.size());
  ASSERT_EQ(4U, descriptor.fields.size());
  for (std::uint32_t index = 0; index < 4U; ++index) {
    EXPECT_EQ(index, descriptor.fields[index].record_field);
  }

  const CustomEnvironment custom;
  const BytecodeProgram lookup = LowerOrThrow(
      "SELECT name FROM items "
      "WHERE stable_guard(?)=1 AND rowid=volatile_key()",
      catalog, custom.Binder());
  std::optional<std::size_t> guard_call;
  std::optional<std::size_t> open;
  std::optional<std::size_t> key_call;
  std::optional<std::size_t> seek;
  for (std::size_t index = 0; index < lookup.instructions().size(); ++index) {
    const Instruction& instruction = lookup.instructions()[index];
    if (const auto* call = std::get_if<CallScalarInstruction>(&instruction); call != nullptr) {
      if (lookup.symbol(call->function) == "stable_guard") {
        guard_call = index;
      } else if (lookup.symbol(call->function) == "volatile_key") {
        key_call = index;
      }
    } else if (std::holds_alternative<OpenReadCursorInstruction>(instruction)) {
      open = index;
    } else if (std::holds_alternative<SeekRowIdInstruction>(instruction)) {
      seek = index;
    }
  }
  ASSERT_TRUE(guard_call.has_value());
  ASSERT_TRUE(open.has_value());
  ASSERT_TRUE(key_call.has_value());
  ASSERT_TRUE(seek.has_value());
  EXPECT_LT(*guard_call, *open);
  EXPECT_LT(*open, *key_call);
  EXPECT_LT(*key_call, *seek);
}

TEST(ReadLowering, MarksTableDependentEmptyPlansButNotConstantRowsAsTransactional) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const BytecodeProgram constant = LowerOrThrow("SELECT 1", catalog);
  const BytecodeProgram empty = LowerOrThrow("SELECT name FROM items WHERE 0", catalog);

  EXPECT_FALSE(constant.requires_read_transaction());
  EXPECT_TRUE(empty.requires_read_transaction());
  EXPECT_TRUE(empty.cursors().empty());
}

TEST(ReadLowering, ExecutesScansLookupsLimitsAndRealAffinity) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 73}));

  const BytecodeProgram scan = LowerOrThrow("SELECT id, name, score FROM items", catalog);
  const auto rows =
      ExecuteRows(scan, ReadVmEnvironment::Core(*pager, catalog->version().generation));
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ("alpha", TextBytes(rows[0][1]));
  EXPECT_EQ(1.5, rows[0][2].real_value());
  EXPECT_EQ(2, rows[1][0].integer_value());
  EXPECT_EQ(3.0, rows[1][2].real_value());
  EXPECT_EQ(SqlValueType::kNull, rows[2][2].type());

  const BytecodeProgram lookup = LowerOrThrow("SELECT name FROM items WHERE rowid=2", catalog);
  const auto lookup_rows =
      ExecuteRows(lookup, ReadVmEnvironment::Core(*pager, catalog->version().generation));
  ASSERT_EQ(1U, lookup_rows.size());
  EXPECT_EQ("beta", TextBytes(lookup_rows[0][0]));

  const BytecodeProgram limited =
      LowerOrThrow("SELECT name FROM items LIMIT ?1 OFFSET ?2", catalog);
  ReadVm limited_vm = TakeValue(
      ReadVm::Create(limited, ReadVmEnvironment::Core(*pager, catalog->version().generation)));
  const SqlValue one = SqlValue::Integer(1);
  RequireStatus(limited_vm.Bind(ParameterId{0}, one));
  RequireStatus(limited_vm.Bind(ParameterId{1}, one));
  ASSERT_EQ(ReadVmStep::kRow, TakeValue(limited_vm.Step()));
  EXPECT_EQ("beta", TextBytes(limited_vm.row()[0]));
  EXPECT_EQ(ReadVmStep::kDone, TakeValue(limited_vm.Step()));

  const BytecodeProgram negative =
      LowerOrThrow("SELECT name FROM items LIMIT -1 OFFSET -2", catalog);
  const auto negative_rows =
      ExecuteRows(negative, ReadVmEnvironment::Core(*pager, catalog->version().generation));
  ASSERT_EQ(3U, negative_rows.size());
  EXPECT_EQ("alpha", TextBytes(negative_rows[0][0]));

  const BytecodeProgram without_rowid = LowerOrThrow("SELECT a, b, c, payload FROM wr", catalog);
  const auto wr_rows =
      ExecuteRows(without_rowid, ReadVmEnvironment::Core(*pager, catalog->version().generation));
  ASSERT_EQ(2U, wr_rows.size());
  EXPECT_EQ("right", TextBytes(wr_rows[0][0]));
  EXPECT_EQ(2, wr_rows[0][1].integer_value());
  EXPECT_EQ("second", TextBytes(wr_rows[0][2]));
  EXPECT_EQ("left", TextBytes(wr_rows[1][0]));
}

TEST(ReadLowering, PreservesLazyExpressionsAndNoFromEffects) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 79}));
  const CustomEnvironment custom;

  callback_count = 0;
  const BytecodeProgram guarded =
      LowerOrThrow("SELECT 1 WHERE volatile_counter() AND 0", catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(guarded, custom.Vm(*pager, catalog->version().generation)).empty());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram lazy = LowerOrThrow(
      "SELECT 0 AND failing(), coalesce('ok', failing()), "
      "iif(0, failing(), 'chosen')",
      catalog, custom.Binder());
  const auto rows = ExecuteRows(lazy, custom.Vm(*pager, catalog->version().generation));
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(0, rows[0][0].integer_value());
  EXPECT_EQ("ok", TextBytes(rows[0][1]));
  EXPECT_EQ("chosen", TextBytes(rows[0][2]));
  EXPECT_EQ(0U, callback_count);

  callback_count = 0;
  const BytecodeProgram zero_limit =
      LowerOrThrow("SELECT 1 LIMIT 0 OFFSET failing()", catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(zero_limit, custom.Vm(*pager, catalog->version().generation)).empty());
  EXPECT_EQ(0U, callback_count);

  const BytecodeProgram barrier = LowerOrThrow("SELECT +0 AND failing()", catalog, custom.Binder());
  ReadVm barrier_vm =
      TakeValue(ReadVm::Create(barrier, custom.Vm(*pager, catalog->version().generation)));
  const auto failed = barrier_vm.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram high_bit_hex =
      LowerOrThrow("SELECT 0xffffffffffffffff OR failing()", catalog, custom.Binder());
  ReadVm high_bit_hex_vm =
      TakeValue(ReadVm::Create(high_bit_hex, custom.Vm(*pager, catalog->version().generation)));
  const auto high_bit_hex_failed = high_bit_hex_vm.Step();
  ASSERT_FALSE(high_bit_hex_failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, high_bit_hex_failed.error().code());
  EXPECT_EQ(1U, callback_count);
}

TEST(ReadLowering, ReportsStrictLimitTypeMismatchBeforeRowWork) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 83}));

  const BytecodeProgram program =
      LowerOrThrow("SELECT name FROM items LIMIT 'not-an-integer'", catalog);
  ReadVm vm = TakeValue(
      ReadVm::Create(program, ReadVmEnvironment::Core(*pager, catalog->version().generation)));
  const auto stepped = vm.Step();
  ASSERT_FALSE(stepped.has_value());
  EXPECT_EQ(ErrorCode::kTypeMismatch, stepped.error().code());
  EXPECT_EQ("datatype mismatch", stepped.error().message());
}

TEST(ReadLowering, SubstitutesAlterDefaultsOnlyForPhysicallyMissingFields) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, AlterDefaultsFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 89}));

  const BytecodeProgram program = LowerOrThrow(
      "SELECT id, existing, text_default, real_default, null_default, "
      "blob_default, negative_default FROM altered",
      catalog);
  ASSERT_EQ(1U, program.cursors().size());
  const ReadCursorDescriptor& descriptor = program.cursors()[0];
  ASSERT_EQ(7U, descriptor.fields.size());
  EXPECT_EQ(MissingFieldValueKind::kConstant, descriptor.fields[2].missing_value_kind);
  EXPECT_EQ(MissingFieldValueKind::kConstant, descriptor.fields[3].missing_value_kind);
  EXPECT_EQ(MissingFieldValueKind::kConstant, descriptor.fields[4].missing_value_kind);
  EXPECT_EQ(MissingFieldValueKind::kConstant, descriptor.fields[5].missing_value_kind);
  EXPECT_EQ(MissingFieldValueKind::kConstant, descriptor.fields[6].missing_value_kind);

  const auto rows =
      ExecuteRows(program, ReadVmEnvironment::Core(*pager, catalog->version().generation));
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ("old", TextBytes(rows[0][1]));
  EXPECT_EQ("legacy", TextBytes(rows[0][2]));
  EXPECT_EQ(7.0, rows[0][3].real_value());
  EXPECT_EQ(SqlValueType::kNull, rows[0][4].type());
  const ByteView blob = BlobBytes(rows[0][5]);
  ASSERT_EQ(2U, blob.size());
  EXPECT_EQ(std::byte{0x01}, blob[0]);
  EXPECT_EQ(std::byte{0x02}, blob[1]);
  EXPECT_EQ(-5, rows[0][6].integer_value());

  EXPECT_EQ(2, rows[1][0].integer_value());
  for (std::size_t index = 2; index < rows[1].size(); ++index) {
    EXPECT_EQ(SqlValueType::kNull, rows[1][index].type()) << index;
  }

  const BytecodeProgram without_rowid =
      LowerOrThrow("SELECT key, existing, added FROM wr", catalog);
  const auto wr_rows =
      ExecuteRows(without_rowid, ReadVmEnvironment::Core(*pager, catalog->version().generation));
  ASSERT_EQ(2U, wr_rows.size());
  EXPECT_EQ("new", TextBytes(wr_rows[0][0]));
  EXPECT_EQ(SqlValueType::kNull, wr_rows[0][2].type());
  EXPECT_EQ("old", TextBytes(wr_rows[1][0]));
  EXPECT_EQ("wr-default", TextBytes(wr_rows[1][2]));
}

}  // namespace
}  // namespace modern_sqlite
