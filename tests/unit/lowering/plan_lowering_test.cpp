#include "modern_sqlite/lowering/plan_lowering.hpp"

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
#include "modern_sqlite/binder/bound_statement.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/catalog/catalog_loader.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/planner/logical_plan.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/syntax/parser.hpp"
#include "modern_sqlite/transaction/transaction_coordinator.hpp"
#include "modern_sqlite/vm/vm.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<LowerPlanResult>);
static_assert(std::is_move_constructible_v<LowerPlanResult>);

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

[[nodiscard]] ExpressionId ColumnDefaultExpression(const SyntaxTree& tree,
                                                   std::size_t column_index) {
  const auto& table = std::get<CreateTableStatement>(tree.statement());
  for (const ColumnConstraint& constraint : table.columns.at(column_index).constraints) {
    if (const auto* default_value = std::get_if<DefaultColumnConstraint>(&constraint.payload);
        default_value != nullptr) {
      return default_value->expression;
    }
  }
  throw std::runtime_error{"test column has no default expression"};
}

[[nodiscard]] CatalogSnapshotPtr MutationCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 0, .generation = 11},
  };
  SyntaxTree definition = ParseTree(
      "CREATE TABLE Items("
      "id INTEGER PRIMARY KEY NOT NULL DEFAULT 9, "
      "Name TEXT NOT NULL DEFAULT 'seed', "
      "Score REAL"
      ")");
  const ExpressionId id_default = ColumnDefaultExpression(definition, 0);
  const ExpressionId name_default = ColumnDefaultExpression(definition, 1);
  input.definitions.push_back(std::move(definition));
  SyntaxTree plain_definition =
      ParseTree("CREATE TABLE Plain(Value TEXT NOT NULL DEFAULT 'plain')");
  const ExpressionId plain_default = ColumnDefaultExpression(plain_definition, 0);
  input.definitions.push_back(std::move(plain_definition));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = "Items",
      .root_page = RootPageId{2},
      .columns =
          {
              CatalogColumnInput{
                  .name = "id",
                  .declared_type = "INTEGER",
                  .not_null_conflict = ConflictAction::kDefault,
                  .primary_key = true,
                  .default_expression =
                      SchemaExpression{
                          .definition = SchemaDefinitionId{0},
                          .expression = id_default,
                      },
                  .missing_record_value = std::make_shared<const SqlValue>(SqlValue::Integer(9)),
              },
              CatalogColumnInput{
                  .name = "Name",
                  .declared_type = "TEXT",
                  .not_null_conflict = ConflictAction::kDefault,
                  .default_expression =
                      SchemaExpression{
                          .definition = SchemaDefinitionId{0},
                          .expression = name_default,
                      },
                  .missing_record_value = std::make_shared<const SqlValue>(SqlValue::Text("seed")),
              },
              CatalogColumnInput{
                  .name = "Score",
                  .declared_type = "REAL",
              },
          },
      .rowid_alias = ColumnId{0},
  });
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{1},
      .name = "Plain",
      .root_page = RootPageId{3},
      .columns =
          {
              CatalogColumnInput{
                  .name = "Value",
                  .declared_type = "TEXT",
                  .not_null_conflict = ConflictAction::kDefault,
                  .default_expression =
                      SchemaExpression{
                          .definition = SchemaDefinitionId{1},
                          .expression = plain_default,
                      },
                  .missing_record_value = std::make_shared<const SqlValue>(SqlValue::Text("plain")),
              },
          },
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

[[nodiscard]] PhysicalMutationPlan OptimizeMutationOrThrow(
    std::string_view sql, const CatalogSnapshotPtr& catalog,
    BindEnvironment environment = BindEnvironment::Core()) {
  BindStatementResult bound = BindStatement(ParseTree(sql), catalog, environment);
  if (!bound.has_value()) {
    throw std::runtime_error{bound.error().detail};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{logical.error().detail};
  }
  OptimizeLogicalStatementPlanResult physical = OptimizeLogicalStatementPlan(std::move(*logical));
  if (!physical.has_value()) {
    throw std::runtime_error{physical.error().detail};
  }
  if (!std::holds_alternative<PhysicalMutationPlan>(*physical)) {
    throw std::runtime_error{"lowering test statement did not produce a mutation plan"};
  }
  return std::get<PhysicalMutationPlan>(std::move(*physical));
}

[[nodiscard]] BytecodeProgram LowerOrThrow(std::string_view sql, const CatalogSnapshotPtr& catalog,
                                           BindEnvironment environment = BindEnvironment::Core()) {
  const PhysicalPlan plan = OptimizeOrThrow(sql, catalog, environment);
  LowerPlanResult lowered = LowerPlan(plan);
  if (!lowered.has_value()) {
    throw std::runtime_error(lowered.error().detail);
  }
  return std::move(*lowered);
}

[[nodiscard]] BytecodeProgram LowerMutationOrThrow(
    std::string_view sql, const CatalogSnapshotPtr& catalog,
    BindEnvironment environment = BindEnvironment::Core()) {
  const PhysicalMutationPlan plan = OptimizeMutationOrThrow(sql, catalog, environment);
  LowerPlanResult lowered = LowerPlan(plan);
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

[[nodiscard]] std::vector<std::vector<SqlValue>> ExecuteRows(
    const BytecodeProgram& program, Pager& pager, std::uint64_t catalog_generation,
    VmEnvironment environment = VmEnvironment::Core()) {
  Vm vm = TakeValue(Vm::Create(program, environment));
  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{pager, catalog_generation}));
  std::vector<std::vector<SqlValue>> rows;
  while (true) {
    const VmStep step = TakeValue(vm.Step());
    if (step == VmStep::kDone) {
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

[[nodiscard]] TransactionCoordinator OpenWriteCoordinator(test::WritePagerFixedVfs& vfs) {
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error{"failed to open lowering write-test pager"};
  }
  return TakeValue(TransactionCoordinator::Open(std::move(pager)));
}

void InitializeMutationDatabase(test::WritePagerFixedVfs& vfs) {
  TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
  TransactionStatement statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  RequireStatus(statement.writer()->InitializeDatabase());
  const TableBtreeWriter table = TakeValue(statement.writer()->CreateTableBtree());
  if (table.root_page() != PageNumber{2}) {
    throw std::runtime_error{"lowering write-test table root is not page 2"};
  }
  const TableBtreeWriter plain = TakeValue(statement.writer()->CreateTableBtree());
  if (plain.root_page() != PageNumber{3}) {
    throw std::runtime_error{"lowering write-test table root is not page 3"};
  }
  RequireStatus(statement.Succeed());
}

struct MutationOutcome {
  std::uint64_t changes = 0;
  std::optional<std::int64_t> last_insert_rowid{};
};

[[nodiscard]] Result<MutationOutcome> ExecuteMutationProgram(
    const BytecodeProgram& program, test::WritePagerFixedVfs& vfs,
    std::span<const SqlValue> parameters = {}, VmEnvironment environment = VmEnvironment::Core()) {
  TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
  const StatementRollbackMode rollback = program.rollback_mode() == ProgramRollbackMode::kStatement
                                             ? StatementRollbackMode::kStatement
                                             : StatementRollbackMode::kTransaction;
  TransactionStatement statement = TakeValue(coordinator.BeginStatement(
      TransactionStatementOptions{.access = StatementAccess::kWrite, .rollback = rollback}));
  Vm vm = TakeValue(Vm::Create(program, environment));
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    RequireStatus(vm.Bind(ParameterId{static_cast<std::uint32_t>(index)}, parameters[index]));
  }
  RequireStatus(vm.AttachExecutionContext(
      VmExecutionContext{*statement.writer(), program.schema_version().generation}));
  Result<VmStep> stepped = vm.Step();
  if (!stepped.has_value()) {
    const Error error = std::move(stepped.error());
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(statement.Rollback());
    return std::unexpected(error);
  }
  if (*stepped != VmStep::kDone) {
    throw std::runtime_error{"mutation program unexpectedly produced a row"};
  }
  MutationOutcome outcome{
      .changes = vm.change_count(),
      .last_insert_rowid = vm.last_insert_rowid_event(),
  };
  RequireStatus(vm.DetachExecutionContext());
  RequireStatus(statement.Succeed());
  return outcome;
}

[[nodiscard]] std::vector<std::pair<std::int64_t, std::vector<SqlValue>>> ReadMutationRows(
    test::WritePagerFixedVfs& vfs, PageNumber root_page = PageNumber{2}) {
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error{"failed to reopen lowering write-test pager"};
  }
  RequireStatus(pager->BeginRead());
  std::vector<std::pair<std::int64_t, std::vector<SqlValue>>> rows;
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, root_page));
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

std::size_t callback_count = 0;

[[nodiscard]] Result<SqlValue> ReturnOne(const ScalarFunctionContext&, std::span<const SqlValue>) {
  return SqlValue::Integer(1);
}

[[nodiscard]] Result<SqlValue> ReturnTwo(const ScalarFunctionContext&, std::span<const SqlValue>) {
  return SqlValue::Integer(2);
}

[[nodiscard]] Result<SqlValue> CountCall(const ScalarFunctionContext&, std::span<const SqlValue>) {
  ++callback_count;
  return SqlValue::Integer(static_cast<std::int64_t>(callback_count));
}

[[nodiscard]] Result<SqlValue> FailCall(const ScalarFunctionContext&, std::span<const SqlValue>) {
  ++callback_count;
  return std::unexpected(Error::Create(ErrorCode::kGeneric, "failing function executed"));
}

[[nodiscard]] Result<SqlValue> FailAfterOne(const ScalarFunctionContext&,
                                            std::span<const SqlValue>) {
  ++callback_count;
  if (callback_count > 1U) {
    return std::unexpected(Error::Create(ErrorCode::kGeneric, "second function call failed"));
  }
  return SqlValue::Integer(1);
}

struct CustomEnvironment {
  std::array<ScalarFunction, 5> functions{{
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
      ScalarFunction{"fail_after_one", FunctionArity::Exact(1),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     FailAfterOne},
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

  [[nodiscard]] VmEnvironment Vm() const noexcept { return VmEnvironment{registry, collations}; }
};

TEST(PlanLoweringApi, ExposesStableErrorsAndBaseMappings) {
  EXPECT_EQ("invalid_input", PlanLoweringErrorCodeName(PlanLoweringErrorCode::kInvalidInput));
  EXPECT_EQ("unsupported_plan", PlanLoweringErrorCodeName(PlanLoweringErrorCode::kUnsupportedPlan));
  EXPECT_EQ("resource_limit", PlanLoweringErrorCodeName(PlanLoweringErrorCode::kResourceLimit));
  EXPECT_EQ("internal_invariant",
            PlanLoweringErrorCodeName(PlanLoweringErrorCode::kInternalInvariant));
  EXPECT_EQ("unknown",
            PlanLoweringErrorCodeName(static_cast<PlanLoweringErrorCode>(255)));  // NOLINT
  EXPECT_EQ(ErrorCode::kMisuse,
            PlanLoweringError{.code = PlanLoweringErrorCode::kInvalidInput}.base_error_code());
  EXPECT_EQ(ErrorCode::kGeneric,
            PlanLoweringError{.code = PlanLoweringErrorCode::kUnsupportedPlan}.base_error_code());
  EXPECT_EQ(ErrorCode::kTooLarge,
            PlanLoweringError{.code = PlanLoweringErrorCode::kResourceLimit}.base_error_code());
  EXPECT_EQ(ErrorCode::kInternal,
            PlanLoweringError{.code = PlanLoweringErrorCode::kInternalInvariant}.base_error_code());
}

TEST(ReadLowering, LowersConstantRowsIntoVerifiedOwnedPrograms) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan plan = OptimizeOrThrow("SELECT 1 AS answer", catalog);

  LowerPlanResult lowered = LowerPlan(plan);
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

TEST(InsertLowering, EmitsVerifiedWriteProgramAndPreservesDuplicateEvaluationOrder) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  const PhysicalMutationPlan plan = OptimizeMutationOrThrow(
      "INSERT INTO Items(Name,name,rowid,id,Score) VALUES(?1,?2,?3,?4,?5)", catalog);

  LowerPlanResult lowered = LowerPlan(plan);
  ASSERT_TRUE(lowered.has_value()) << lowered.error().detail;
  EXPECT_EQ((SchemaVersionRequirement{.schema_cookie = 0, .generation = 11}),
            lowered->schema_version());
  EXPECT_EQ(ProgramStatementKind::kInsert, lowered->statement_kind());
  EXPECT_EQ(ProgramTransactionAccess::kWrite, lowered->transaction_access());
  EXPECT_EQ(ProgramRollbackMode::kTransaction, lowered->rollback_mode());
  EXPECT_TRUE(lowered->mutation_result().publishes_changes);
  EXPECT_TRUE(lowered->mutation_result().publishes_last_insert_rowid);
  EXPECT_TRUE(lowered->requires_database_snapshot());
  EXPECT_EQ(5U, lowered->parameter_count());
  EXPECT_TRUE(lowered->cursors().empty());
  EXPECT_TRUE(lowered->result_columns().empty());

  ASSERT_EQ(1U, lowered->write_cursors().size());
  const WriteCursorDescriptor& cursor = lowered->write_cursor(WriteCursorId{0});
  EXPECT_EQ(RootPageNumber{2}, cursor.root_page);
  ASSERT_EQ(3U, cursor.columns.size());
  EXPECT_EQ(std::optional<std::uint32_t>{0}, cursor.rowid_alias);
  EXPECT_TRUE(cursor.columns[0].rowid_alias);
  EXPECT_TRUE(cursor.columns[0].not_null);
  EXPECT_FALSE(cursor.columns[0].default_value.has_value());
  ASSERT_TRUE(cursor.columns[1].default_value.has_value());
  const ConstantId name_default =
      TakeOptional(cursor.columns[1].default_value, "missing name default");
  EXPECT_EQ("seed", TextBytes(lowered->constant(name_default)));
  EXPECT_EQ(TypeAffinity::kReal, cursor.columns[2].affinity);

  std::vector<std::uint32_t> loaded_parameters;
  for (const Instruction& instruction : lowered->instructions()) {
    if (const auto* load = std::get_if<LoadParameterInstruction>(&instruction); load != nullptr) {
      loaded_parameters.push_back(load->parameter.value());
    }
  }
  EXPECT_EQ((std::vector<std::uint32_t>{0, 1, 2, 3, 4}), loaded_parameters);
  EXPECT_EQ((std::vector{
                InstructionKind::kLoadConstant,
                InstructionKind::kLoadConstant,
                InstructionKind::kLoadConstant,
                InstructionKind::kLoadConstant,
                InstructionKind::kLoadParameter,
                InstructionKind::kLoadParameter,
                InstructionKind::kLoadParameter,
                InstructionKind::kLoadParameter,
                InstructionKind::kLoadParameter,
                InstructionKind::kOpenWrite,
                InstructionKind::kResolveInsertRowId,
                InstructionKind::kBuildTableRecord,
                InstructionKind::kInsertTable,
                InstructionKind::kCloseWrite,
                InstructionKind::kHalt,
            }),
            InstructionKinds(*lowered));
}

TEST(InsertLowering, ExecutesDefaultsAffinityDuplicateTargetsAndRowidAliases) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  const CustomEnvironment custom;
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);

  callback_count = 0;
  const BytecodeProgram duplicate = LowerMutationOrThrow(
      "INSERT INTO Items(Name,name,Score) "
      "VALUES(volatile_counter(),volatile_counter(),?1)",
      catalog, custom.Binder());
  std::vector<SqlValue> duplicate_parameters;
  duplicate_parameters.push_back(SqlValue::Integer(7));
  const MutationOutcome duplicate_outcome =
      TakeValue(ExecuteMutationProgram(duplicate, vfs, duplicate_parameters, custom.Vm()));
  EXPECT_EQ(2U, callback_count);
  EXPECT_EQ(1U, duplicate_outcome.changes);
  EXPECT_EQ(std::optional<std::int64_t>{1}, duplicate_outcome.last_insert_rowid);

  const BytecodeProgram defaults =
      LowerMutationOrThrow("INSERT INTO Items DEFAULT VALUES", catalog);
  const MutationOutcome default_outcome = TakeValue(ExecuteMutationProgram(defaults, vfs));
  EXPECT_EQ(1U, default_outcome.changes);
  EXPECT_EQ(std::optional<std::int64_t>{2}, default_outcome.last_insert_rowid);

  const BytecodeProgram rowid =
      LowerMutationOrThrow("INSERT INTO Items(rowid,id,Name) VALUES(?1,?2,?3)", catalog);
  std::vector<SqlValue> rowid_parameters;
  rowid_parameters.push_back(SqlValue::Integer(10));
  rowid_parameters.push_back(SqlValue::Text("11"));
  rowid_parameters.push_back(SqlValue::Text("alias"));
  const MutationOutcome rowid_outcome =
      TakeValue(ExecuteMutationProgram(rowid, vfs, rowid_parameters));
  EXPECT_EQ(1U, rowid_outcome.changes);
  EXPECT_EQ(std::optional<std::int64_t>{11}, rowid_outcome.last_insert_rowid);

  const BytecodeProgram plain_default =
      LowerMutationOrThrow("INSERT INTO Plain DEFAULT VALUES", catalog);
  const MutationOutcome plain_default_outcome =
      TakeValue(ExecuteMutationProgram(plain_default, vfs));
  EXPECT_EQ(std::optional<std::int64_t>{1}, plain_default_outcome.last_insert_rowid);

  const BytecodeProgram plain_explicit =
      LowerMutationOrThrow("INSERT INTO Plain(rowid,Value) VALUES(?1,?2)", catalog);
  std::vector<SqlValue> plain_parameters;
  plain_parameters.push_back(SqlValue::Text("20"));
  plain_parameters.push_back(SqlValue::Integer(42));
  const MutationOutcome plain_explicit_outcome =
      TakeValue(ExecuteMutationProgram(plain_explicit, vfs, plain_parameters));
  EXPECT_EQ(std::optional<std::int64_t>{20}, plain_explicit_outcome.last_insert_rowid);

  const BytecodeProgram null_plain =
      LowerMutationOrThrow("INSERT INTO Plain(Value) VALUES(NULL)", catalog);
  const Result<MutationOutcome> null_failed = ExecuteMutationProgram(null_plain, vfs);
  ASSERT_FALSE(null_failed.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, null_failed.error().code());

  callback_count = 0;
  const BytecodeProgram failing = LowerMutationOrThrow(
      "INSERT INTO Items(Name,name) VALUES('kept',failing())", catalog, custom.Binder());
  const Result<MutationOutcome> failed = ExecuteMutationProgram(failing, vfs, {}, custom.Vm());
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_EQ(1U, callback_count);

  const auto rows = ReadMutationRows(vfs);
  ASSERT_EQ(3U, rows.size());

  EXPECT_EQ(1, rows[0].first);
  ASSERT_EQ(3U, rows[0].second.size());
  EXPECT_EQ(SqlValueType::kNull, rows[0].second[0].type());
  EXPECT_EQ("1", TextBytes(rows[0].second[1]));
  EXPECT_DOUBLE_EQ(7.0, TakeOptional(rows[0].second[2].real_value(), "expected REAL score"));

  EXPECT_EQ(2, rows[1].first);
  EXPECT_EQ(SqlValueType::kNull, rows[1].second[0].type());
  EXPECT_EQ("seed", TextBytes(rows[1].second[1]));
  EXPECT_EQ(SqlValueType::kNull, rows[1].second[2].type());

  EXPECT_EQ(11, rows[2].first);
  EXPECT_EQ(SqlValueType::kNull, rows[2].second[0].type());
  EXPECT_EQ("alias", TextBytes(rows[2].second[1]));
  EXPECT_EQ(SqlValueType::kNull, rows[2].second[2].type());

  const auto plain_rows = ReadMutationRows(vfs, PageNumber{3});
  ASSERT_EQ(2U, plain_rows.size());
  EXPECT_EQ(1, plain_rows[0].first);
  ASSERT_EQ(1U, plain_rows[0].second.size());
  EXPECT_EQ("plain", TextBytes(plain_rows[0].second[0]));
  EXPECT_EQ(20, plain_rows[1].first);
  EXPECT_EQ("42", TextBytes(plain_rows[1].second[0]));
}

TEST(InsertLowering, RetainsLimitsRejectsUnsupportedMutationsAndMovedFromPlans) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  PhysicalMutationPlan plan = OptimizeMutationOrThrow("INSERT INTO Items DEFAULT VALUES", catalog);

  ProgramLimits limits;
  limits.maximum_constants = 1;
  LowerPlanResult limited = LowerPlan(plan, limits);
  ASSERT_FALSE(limited.has_value());
  EXPECT_EQ(PlanLoweringErrorCode::kResourceLimit, limited.error().code);
  ASSERT_TRUE(limited.error().program_error.has_value());
  EXPECT_EQ(ProgramErrorCode::kConstantLimitExceeded,
            TakeOptional(limited.error().program_error, "missing nested program error").code);

  const PhysicalMutationPlan create =
      OptimizeMutationOrThrow("CREATE TABLE Other(id INTEGER PRIMARY KEY)", catalog);
  LowerPlanResult unsupported = LowerPlan(create);
  ASSERT_FALSE(unsupported.has_value());
  EXPECT_EQ(PlanLoweringErrorCode::kUnsupportedPlan, unsupported.error().code);

  const PhysicalMutationPlan moved = std::move(plan);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  LowerPlanResult invalid = LowerPlan(plan);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(PlanLoweringErrorCode::kInvalidInput, invalid.error().code);
  EXPECT_TRUE(LowerPlan(moved).has_value());
}

TEST(DeleteLowering, EmitsEmptyExactAndSafeScanPrograms) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  const CustomEnvironment custom;

  const BytecodeProgram empty = LowerMutationOrThrow("DELETE FROM Items WHERE 0", catalog);
  EXPECT_EQ(ProgramStatementKind::kDelete, empty.statement_kind());
  EXPECT_EQ(ProgramTransactionAccess::kWrite, empty.transaction_access());
  EXPECT_EQ(ProgramRollbackMode::kTransaction, empty.rollback_mode());
  EXPECT_TRUE(empty.mutation_result().publishes_changes);
  EXPECT_FALSE(empty.mutation_result().publishes_last_insert_rowid);
  EXPECT_TRUE(empty.cursors().empty());
  EXPECT_TRUE(empty.write_cursors().empty());
  EXPECT_EQ((std::vector{InstructionKind::kHalt}), InstructionKinds(empty));

  const BytecodeProgram exact =
      LowerMutationOrThrow("DELETE FROM Items WHERE id=?1 AND Name=?2", catalog);
  EXPECT_EQ(ProgramRollbackMode::kTransaction, exact.rollback_mode());
  ASSERT_EQ(1U, exact.cursors().size());
  ASSERT_EQ(1U, exact.write_cursors().size());
  EXPECT_EQ(RootPageNumber{2}, exact.cursor(CursorId{0}).root_page);
  EXPECT_EQ(RootPageNumber{2}, exact.write_cursor(WriteCursorId{0}).root_page);
  bool exact_seek = false;
  bool exact_delete = false;
  std::optional<std::size_t> exact_close;
  std::optional<std::size_t> exact_delete_index;
  for (std::size_t index = 0; index < exact.instructions().size(); ++index) {
    const Instruction& instruction = exact.instructions()[index];
    if (const auto* seek = std::get_if<SeekRowIdInstruction>(&instruction); seek != nullptr) {
      exact_seek = true;
      EXPECT_EQ(RowIdSeekMode::kEqual, seek->mode);
    }
    if (!exact_close.has_value() && std::holds_alternative<CloseCursorInstruction>(instruction)) {
      exact_close = index;
    }
    if (std::holds_alternative<DeleteTableInstruction>(instruction)) {
      exact_delete = true;
      exact_delete_index = index;
    }
  }
  EXPECT_TRUE(exact_seek);
  EXPECT_TRUE(exact_delete);
  ASSERT_TRUE(exact_close.has_value());
  ASSERT_TRUE(exact_delete_index.has_value());
  EXPECT_LT(TakeOptional(exact_close, "missing exact read close"),
            TakeOptional(exact_delete_index, "missing exact delete"));

  const BytecodeProgram scan = LowerMutationOrThrow("DELETE FROM Items WHERE Score>=?1", catalog);
  EXPECT_EQ(ProgramRollbackMode::kStatement, scan.rollback_mode());
  EXPECT_TRUE(std::ranges::any_of(scan.instructions(), [](const Instruction& instruction) {
    return std::holds_alternative<RewindInstruction>(instruction);
  }));
  bool greater_seek = false;
  bool scan_delete = false;
  std::optional<std::size_t> scan_close;
  std::optional<std::size_t> scan_delete_index;
  for (std::size_t index = 0; index < scan.instructions().size(); ++index) {
    const Instruction& instruction = scan.instructions()[index];
    if (const auto* seek = std::get_if<SeekRowIdInstruction>(&instruction); seek != nullptr) {
      greater_seek = greater_seek || seek->mode == RowIdSeekMode::kGreater;
    }
    if (!scan_close.has_value() && std::holds_alternative<CloseCursorInstruction>(instruction)) {
      scan_close = index;
    }
    if (std::holds_alternative<DeleteTableInstruction>(instruction)) {
      scan_delete = true;
      scan_delete_index = index;
    }
  }
  EXPECT_TRUE(greater_seek);
  EXPECT_TRUE(scan_delete);
  ASSERT_TRUE(scan_close.has_value());
  ASSERT_TRUE(scan_delete_index.has_value());
  EXPECT_LT(TakeOptional(scan_close, "missing scan read close"),
            TakeOptional(scan_delete_index, "missing scan delete"));

  const PhysicalMutationPlan limited_plan =
      OptimizeMutationOrThrow("DELETE FROM Items WHERE Score>=?1", catalog);
  ProgramLimits limits;
  limits.maximum_cursors = 1;
  LowerPlanResult limited = LowerPlan(limited_plan, limits);
  ASSERT_FALSE(limited.has_value());
  EXPECT_EQ(PlanLoweringErrorCode::kResourceLimit, limited.error().code);
  ASSERT_TRUE(limited.error().program_error.has_value());
  EXPECT_EQ(ProgramErrorCode::kCursorLimitExceeded,
            TakeOptional(limited.error().program_error, "missing DELETE cursor limit").code);

  const BytecodeProgram function_scan = LowerMutationOrThrow(
      "DELETE FROM Items WHERE fail_after_one(Score)", catalog, custom.Binder());
  std::optional<std::size_t> function_close;
  std::optional<std::size_t> function_call;
  std::optional<std::size_t> function_delete;
  for (std::size_t index = 0; index < function_scan.instructions().size(); ++index) {
    const Instruction& instruction = function_scan.instructions()[index];
    if (!function_close.has_value() &&
        std::holds_alternative<CloseCursorInstruction>(instruction)) {
      function_close = index;
    }
    if (!function_call.has_value() && std::holds_alternative<CallScalarInstruction>(instruction)) {
      function_call = index;
    }
    if (!function_delete.has_value() &&
        std::holds_alternative<DeleteTableInstruction>(instruction)) {
      function_delete = index;
    }
  }
  EXPECT_LT(TakeOptional(function_close, "missing function scan read close"),
            TakeOptional(function_call, "missing function scan call"));
  EXPECT_LT(TakeOptional(function_call, "missing function scan call"),
            TakeOptional(function_delete, "missing function scan delete"));
}

TEST(DeleteLowering, ExecutesExactAndScanDeletesWithStatementRollback) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  const CustomEnvironment custom;
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);

  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const auto insert_item = [&](std::int64_t rowid, std::string name, std::int64_t score) {
    std::vector<SqlValue> parameters;
    parameters.push_back(SqlValue::Integer(rowid));
    parameters.push_back(SqlValue::Text(std::move(name)));
    parameters.push_back(SqlValue::Integer(score));
    return TakeValue(ExecuteMutationProgram(insert, vfs, parameters));
  };
  static_cast<void>(insert_item(1, "alpha", 1));
  static_cast<void>(insert_item(2, "beta", 2));
  static_cast<void>(insert_item(3, "gamma", 3));
  static_cast<void>(insert_item(4, "beta", 4));

  const BytecodeProgram exact =
      LowerMutationOrThrow("DELETE FROM Items WHERE id=?1 AND Name=?2", catalog);
  std::vector<SqlValue> exact_parameters;
  exact_parameters.push_back(SqlValue::Integer(2));
  exact_parameters.push_back(SqlValue::Text("beta"));
  const MutationOutcome exact_outcome =
      TakeValue(ExecuteMutationProgram(exact, vfs, exact_parameters));
  EXPECT_EQ(1U, exact_outcome.changes);
  EXPECT_FALSE(exact_outcome.last_insert_rowid.has_value());

  std::vector<SqlValue> rejected_parameters;
  rejected_parameters.push_back(SqlValue::Integer(3));
  rejected_parameters.push_back(SqlValue::Text("not-gamma"));
  const MutationOutcome rejected =
      TakeValue(ExecuteMutationProgram(exact, vfs, rejected_parameters));
  EXPECT_EQ(0U, rejected.changes);

  std::vector<SqlValue> invalid_key_parameters;
  invalid_key_parameters.push_back(SqlValue::Text("not-rowid"));
  invalid_key_parameters.push_back(SqlValue::Text("alpha"));
  const MutationOutcome invalid_key =
      TakeValue(ExecuteMutationProgram(exact, vfs, invalid_key_parameters));
  EXPECT_EQ(0U, invalid_key.changes);

  const BytecodeProgram scan = LowerMutationOrThrow("DELETE FROM Items WHERE Score>=?1", catalog);
  std::vector<SqlValue> scan_parameters;
  scan_parameters.push_back(SqlValue::Integer(3));
  const MutationOutcome scan_outcome =
      TakeValue(ExecuteMutationProgram(scan, vfs, scan_parameters));
  EXPECT_EQ(2U, scan_outcome.changes);

  callback_count = 0;
  const BytecodeProgram guarded = LowerMutationOrThrow(
      "DELETE FROM Items WHERE stable_guard(?1)=0 AND failing()", catalog, custom.Binder());
  std::vector<SqlValue> guard_parameters;
  guard_parameters.push_back(SqlValue::Integer(1));
  const MutationOutcome guard_outcome =
      TakeValue(ExecuteMutationProgram(guarded, vfs, guard_parameters, custom.Vm()));
  EXPECT_EQ(0U, guard_outcome.changes);
  EXPECT_EQ(0U, callback_count);

  const auto remaining = ReadMutationRows(vfs);
  ASSERT_EQ(1U, remaining.size());
  EXPECT_EQ(1, remaining[0].first);
  EXPECT_EQ("alpha", TextBytes(remaining[0].second[1]));

  const BytecodeProgram plain_insert =
      LowerMutationOrThrow("INSERT INTO Plain(rowid,Value) VALUES(?1,?2)", catalog);
  const auto insert_plain = [&](std::int64_t rowid, std::string value) {
    std::vector<SqlValue> parameters;
    parameters.push_back(SqlValue::Integer(rowid));
    parameters.push_back(SqlValue::Text(std::move(value)));
    return TakeValue(ExecuteMutationProgram(plain_insert, vfs, parameters));
  };
  static_cast<void>(insert_plain(5, "five"));
  static_cast<void>(insert_plain(6, "six"));

  const BytecodeProgram plain_exact =
      LowerMutationOrThrow("DELETE FROM Plain WHERE rowid=?1", catalog);
  std::vector<SqlValue> plain_key;
  plain_key.push_back(SqlValue::Integer(5));
  EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(plain_exact, vfs, plain_key)).changes);
  const BytecodeProgram plain_scan = LowerMutationOrThrow("DELETE FROM Plain", catalog);
  EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(plain_scan, vfs)).changes);
  EXPECT_TRUE(ReadMutationRows(vfs, PageNumber{3}).empty());

  test::WritePagerFixedVfs rollback_vfs{false};
  InitializeMutationDatabase(rollback_vfs);
  const auto seed_rollback = [&](std::int64_t rowid, std::string name) {
    std::vector<SqlValue> parameters;
    parameters.push_back(SqlValue::Integer(rowid));
    parameters.push_back(SqlValue::Text(std::move(name)));
    parameters.push_back(SqlValue::Integer(rowid));
    return TakeValue(ExecuteMutationProgram(insert, rollback_vfs, parameters));
  };
  static_cast<void>(seed_rollback(1, "first"));
  static_cast<void>(seed_rollback(2, "second"));

  callback_count = 0;
  const BytecodeProgram failing = LowerMutationOrThrow(
      "DELETE FROM Items WHERE fail_after_one(Score)", catalog, custom.Binder());
  const Result<MutationOutcome> failed =
      ExecuteMutationProgram(failing, rollback_vfs, {}, custom.Vm());
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_EQ(2U, callback_count);

  const auto restored = ReadMutationRows(rollback_vfs);
  ASSERT_EQ(2U, restored.size());
  EXPECT_EQ(1, restored[0].first);
  EXPECT_EQ(2, restored[1].first);
}

TEST(UpdateLowering, EmitsEmptyExactAndSafeStableRowidScanPrograms) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  const CustomEnvironment custom;

  const BytecodeProgram empty =
      LowerMutationOrThrow("UPDATE Items SET Name='unused' WHERE 0", catalog);
  EXPECT_EQ(ProgramStatementKind::kUpdate, empty.statement_kind());
  EXPECT_EQ(ProgramRollbackMode::kTransaction, empty.rollback_mode());
  EXPECT_TRUE(empty.cursors().empty());
  EXPECT_TRUE(empty.write_cursors().empty());
  EXPECT_EQ((std::vector{InstructionKind::kHalt}), InstructionKinds(empty));

  const BytecodeProgram exact =
      LowerMutationOrThrow("UPDATE Items SET Name=?1 WHERE id=?2", catalog);
  EXPECT_EQ(ProgramRollbackMode::kTransaction, exact.rollback_mode());
  EXPECT_TRUE(std::ranges::any_of(exact.instructions(), [](const Instruction& instruction) {
    return std::holds_alternative<UpdateTableInstruction>(instruction);
  }));

  const BytecodeProgram moving =
      LowerMutationOrThrow("UPDATE Items SET id=?1 WHERE id=?2", catalog);
  EXPECT_EQ(ProgramRollbackMode::kStatement, moving.rollback_mode());
  EXPECT_TRUE(std::ranges::any_of(moving.instructions(), [](const Instruction& instruction) {
    return std::holds_alternative<MustBeIntegerInstruction>(instruction);
  }));

  const BytecodeProgram scan =
      LowerMutationOrThrow("UPDATE Items SET Name=Name||?1 WHERE Score>=?2", catalog);
  EXPECT_EQ(ProgramRollbackMode::kStatement, scan.rollback_mode());
  bool greater_seek = false;
  bool update = false;
  for (const Instruction& instruction : scan.instructions()) {
    if (const auto* seek = std::get_if<SeekRowIdInstruction>(&instruction); seek != nullptr) {
      greater_seek = greater_seek || seek->mode == RowIdSeekMode::kGreater;
    }
    update = update || std::holds_alternative<UpdateTableInstruction>(instruction);
  }
  EXPECT_TRUE(greater_seek);
  EXPECT_TRUE(update);

  const BytecodeProgram function_scan = LowerMutationOrThrow(
      "UPDATE Items SET Name=volatile_counter() WHERE Score>=0", catalog, custom.Binder());
  std::optional<std::size_t> close;
  std::optional<std::size_t> call;
  std::optional<std::size_t> update_index;
  for (std::size_t index = 0; index < function_scan.instructions().size(); ++index) {
    const Instruction& instruction = function_scan.instructions()[index];
    if (!close.has_value() && std::holds_alternative<CloseCursorInstruction>(instruction)) {
      close = index;
    }
    if (!call.has_value() && std::holds_alternative<CallScalarInstruction>(instruction)) {
      call = index;
    }
    if (!update_index.has_value() && std::holds_alternative<UpdateTableInstruction>(instruction)) {
      update_index = index;
    }
  }
  EXPECT_LT(TakeOptional(close, "missing UPDATE read close"),
            TakeOptional(call, "missing UPDATE assignment call"));
  EXPECT_LT(TakeOptional(call, "missing UPDATE assignment call"),
            TakeOptional(update_index, "missing UPDATE point mutation"));

  const PhysicalMutationPlan moving_scan =
      OptimizeMutationOrThrow("UPDATE Items SET id=id+10 WHERE Score>=?1", catalog);
  LowerPlanResult unsupported = LowerPlan(moving_scan);
  ASSERT_FALSE(unsupported.has_value());
  EXPECT_EQ(PlanLoweringErrorCode::kUnsupportedPlan, unsupported.error().code);
}

TEST(UpdateLowering, ExecutesAssignmentsRowidMovesAndStatementRollback) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  const CustomEnvironment custom;
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);

  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const auto insert_item = [&](test::WritePagerFixedVfs& target_vfs, std::int64_t rowid,
                               std::string name, std::int64_t score) {
    std::vector<SqlValue> parameters;
    parameters.push_back(SqlValue::Integer(rowid));
    parameters.push_back(SqlValue::Text(std::move(name)));
    parameters.push_back(SqlValue::Integer(score));
    return TakeValue(ExecuteMutationProgram(insert, target_vfs, parameters));
  };
  static_cast<void>(insert_item(vfs, 1, "alpha", 1));
  static_cast<void>(insert_item(vfs, 2, "beta", 2));
  static_cast<void>(insert_item(vfs, 3, "gamma", 3));

  const BytecodeProgram exact =
      LowerMutationOrThrow("UPDATE Items SET Name=?1 WHERE id=?2", catalog);
  std::vector<SqlValue> exact_parameters;
  exact_parameters.push_back(SqlValue::Integer(42));
  exact_parameters.push_back(SqlValue::Integer(2));
  const MutationOutcome exact_outcome =
      TakeValue(ExecuteMutationProgram(exact, vfs, exact_parameters));
  EXPECT_EQ(1U, exact_outcome.changes);
  EXPECT_FALSE(exact_outcome.last_insert_rowid.has_value());

  const BytecodeProgram move = LowerMutationOrThrow("UPDATE Items SET id=?1 WHERE id=?2", catalog);
  std::vector<SqlValue> move_parameters;
  move_parameters.push_back(SqlValue::Text("5"));
  move_parameters.push_back(SqlValue::Integer(3));
  EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(move, vfs, move_parameters)).changes);

  callback_count = 0;
  const BytecodeProgram duplicates = LowerMutationOrThrow(
      "UPDATE Items SET Name=volatile_counter(),name=volatile_counter() WHERE id=1", catalog,
      custom.Binder());
  EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(duplicates, vfs, {}, custom.Vm())).changes);
  EXPECT_EQ(2U, callback_count);

  const BytecodeProgram scan =
      LowerMutationOrThrow("UPDATE Items SET Score=Score+10 WHERE Score>=?1", catalog);
  std::vector<SqlValue> scan_parameters;
  scan_parameters.push_back(SqlValue::Integer(2));
  EXPECT_EQ(2U, TakeValue(ExecuteMutationProgram(scan, vfs, scan_parameters)).changes);

  std::vector<SqlValue> conflict_parameters;
  conflict_parameters.push_back(SqlValue::Integer(2));
  conflict_parameters.push_back(SqlValue::Integer(1));
  const Result<MutationOutcome> conflict = ExecuteMutationProgram(move, vfs, conflict_parameters);
  ASSERT_FALSE(conflict.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, conflict.error().code());

  std::vector<SqlValue> null_parameters;
  null_parameters.emplace_back();
  null_parameters.push_back(SqlValue::Integer(1));
  const Result<MutationOutcome> null_rowid = ExecuteMutationProgram(move, vfs, null_parameters);
  ASSERT_FALSE(null_rowid.has_value());
  EXPECT_EQ(ErrorCode::kTypeMismatch, null_rowid.error().code());

  std::vector<SqlValue> missing_parameters;
  missing_parameters.push_back(SqlValue::Text("missing"));
  missing_parameters.push_back(SqlValue::Integer(99));
  EXPECT_EQ(0U, TakeValue(ExecuteMutationProgram(exact, vfs, missing_parameters)).changes);

  const auto rows = ReadMutationRows(vfs);
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(1, rows[0].first);
  EXPECT_EQ("2", TextBytes(rows[0].second[1]));
  EXPECT_EQ(1.0, TakeOptional(rows[0].second[2].real_value(), "missing row 1 score"));
  EXPECT_EQ(2, rows[1].first);
  EXPECT_EQ("42", TextBytes(rows[1].second[1]));
  EXPECT_EQ(12.0, TakeOptional(rows[1].second[2].real_value(), "missing row 2 score"));
  EXPECT_EQ(5, rows[2].first);
  EXPECT_EQ("gamma", TextBytes(rows[2].second[1]));
  EXPECT_EQ(13.0, TakeOptional(rows[2].second[2].real_value(), "missing row 5 score"));

  const BytecodeProgram plain_insert =
      LowerMutationOrThrow("INSERT INTO Plain(rowid,Value) VALUES(?1,?2)", catalog);
  std::vector<SqlValue> plain_insert_parameters;
  plain_insert_parameters.push_back(SqlValue::Integer(5));
  plain_insert_parameters.push_back(SqlValue::Text("five"));
  static_cast<void>(TakeValue(ExecuteMutationProgram(plain_insert, vfs, plain_insert_parameters)));
  const BytecodeProgram plain_update =
      LowerMutationOrThrow("UPDATE Plain SET Value=?1 WHERE rowid=?2", catalog);
  std::vector<SqlValue> plain_update_parameters;
  plain_update_parameters.push_back(SqlValue::Integer(42));
  plain_update_parameters.push_back(SqlValue::Integer(5));
  EXPECT_EQ(1U,
            TakeValue(ExecuteMutationProgram(plain_update, vfs, plain_update_parameters)).changes);
  const BytecodeProgram plain_move =
      LowerMutationOrThrow("UPDATE Plain SET rowid=?1 WHERE rowid=?2", catalog);
  std::vector<SqlValue> plain_move_parameters;
  plain_move_parameters.push_back(SqlValue::Integer(6));
  plain_move_parameters.push_back(SqlValue::Integer(5));
  EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(plain_move, vfs, plain_move_parameters)).changes);
  const auto plain_rows = ReadMutationRows(vfs, PageNumber{3});
  ASSERT_EQ(1U, plain_rows.size());
  EXPECT_EQ(6, plain_rows[0].first);
  EXPECT_EQ("42", TextBytes(plain_rows[0].second[0]));

  test::WritePagerFixedVfs rollback_vfs{false};
  InitializeMutationDatabase(rollback_vfs);
  static_cast<void>(insert_item(rollback_vfs, 1, "first", 1));
  static_cast<void>(insert_item(rollback_vfs, 2, "second", 2));
  callback_count = 0;
  const BytecodeProgram failing =
      LowerMutationOrThrow("UPDATE Items SET Name=fail_after_one(Name)", catalog, custom.Binder());
  const Result<MutationOutcome> failed =
      ExecuteMutationProgram(failing, rollback_vfs, {}, custom.Vm());
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_EQ(2U, callback_count);
  const auto restored = ReadMutationRows(rollback_vfs);
  ASSERT_EQ(2U, restored.size());
  EXPECT_EQ("first", TextBytes(restored[0].second[1]));
  EXPECT_EQ("second", TextBytes(restored[1].second[1]));
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
  LowerPlanResult limited = LowerPlan(plan, limits);
  ASSERT_FALSE(limited.has_value());
  EXPECT_EQ(PlanLoweringErrorCode::kResourceLimit, limited.error().code);
  ASSERT_TRUE(limited.error().program_error.has_value());
  EXPECT_EQ(ProgramErrorCode::kInstructionLimitExceeded,
            TakeOptional(limited.error().program_error, "missing nested program error").code);

  const PhysicalPlan moved = std::move(plan);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  ASSERT_FALSE(LowerPlan(plan).has_value());
  EXPECT_TRUE(LowerPlan(moved).has_value());
}

TEST(ReadLowering, PreservesNestedProgramResourceLimitCodes) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const auto expect_limit = [&](std::string_view sql, ProgramLimits limits,
                                ProgramErrorCode expected) {
    SCOPED_TRACE(sql);
    const PhysicalPlan plan = OptimizeOrThrow(sql, catalog);
    LowerPlanResult lowered = LowerPlan(plan, limits);
    ASSERT_FALSE(lowered.has_value());
    EXPECT_EQ(PlanLoweringErrorCode::kResourceLimit, lowered.error().code);
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
  EXPECT_TRUE(table.requires_database_snapshot());
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

TEST(ReadLowering, MarksTableDependentEmptyPlansButNotConstantRowsAsSnapshotRequired) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const BytecodeProgram constant = LowerOrThrow("SELECT 1", catalog);
  const BytecodeProgram empty = LowerOrThrow("SELECT name FROM items WHERE 0", catalog);

  EXPECT_FALSE(constant.requires_database_snapshot());
  EXPECT_TRUE(empty.requires_database_snapshot());
  EXPECT_TRUE(empty.cursors().empty());
}

TEST(ReadLowering, ExecutesScansLookupsLimitsAndRealAffinity) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 73}));

  const BytecodeProgram scan = LowerOrThrow("SELECT id, name, score FROM items", catalog);
  const auto rows = ExecuteRows(scan, *pager, catalog->version().generation);
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ("alpha", TextBytes(rows[0][1]));
  EXPECT_EQ(1.5, rows[0][2].real_value());
  EXPECT_EQ(2, rows[1][0].integer_value());
  EXPECT_EQ(3.0, rows[1][2].real_value());
  EXPECT_EQ(SqlValueType::kNull, rows[2][2].type());

  const BytecodeProgram lookup = LowerOrThrow("SELECT name FROM items WHERE rowid=2", catalog);
  const auto lookup_rows = ExecuteRows(lookup, *pager, catalog->version().generation);
  ASSERT_EQ(1U, lookup_rows.size());
  EXPECT_EQ("beta", TextBytes(lookup_rows[0][0]));

  const BytecodeProgram limited =
      LowerOrThrow("SELECT name FROM items LIMIT ?1 OFFSET ?2", catalog);
  Vm limited_vm = TakeValue(Vm::Create(limited, VmEnvironment::Core()));
  RequireStatus(
      limited_vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  const SqlValue one = SqlValue::Integer(1);
  RequireStatus(limited_vm.Bind(ParameterId{0}, one));
  RequireStatus(limited_vm.Bind(ParameterId{1}, one));
  ASSERT_EQ(VmStep::kRow, TakeValue(limited_vm.Step()));
  EXPECT_EQ("beta", TextBytes(limited_vm.row()[0]));
  EXPECT_EQ(VmStep::kDone, TakeValue(limited_vm.Step()));

  const BytecodeProgram negative =
      LowerOrThrow("SELECT name FROM items LIMIT -1 OFFSET -2", catalog);
  const auto negative_rows = ExecuteRows(negative, *pager, catalog->version().generation);
  ASSERT_EQ(3U, negative_rows.size());
  EXPECT_EQ("alpha", TextBytes(negative_rows[0][0]));

  const BytecodeProgram without_rowid = LowerOrThrow("SELECT a, b, c, payload FROM wr", catalog);
  const auto wr_rows = ExecuteRows(without_rowid, *pager, catalog->version().generation);
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
  EXPECT_TRUE(ExecuteRows(guarded, *pager, catalog->version().generation, custom.Vm()).empty());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram lazy = LowerOrThrow(
      "SELECT 0 AND failing(), coalesce('ok', failing()), "
      "iif(0, failing(), 'chosen')",
      catalog, custom.Binder());
  const auto rows = ExecuteRows(lazy, *pager, catalog->version().generation, custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(0, rows[0][0].integer_value());
  EXPECT_EQ("ok", TextBytes(rows[0][1]));
  EXPECT_EQ("chosen", TextBytes(rows[0][2]));
  EXPECT_EQ(0U, callback_count);

  callback_count = 0;
  const BytecodeProgram zero_limit =
      LowerOrThrow("SELECT 1 LIMIT 0 OFFSET failing()", catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(zero_limit, *pager, catalog->version().generation, custom.Vm()).empty());
  EXPECT_EQ(0U, callback_count);

  const BytecodeProgram barrier = LowerOrThrow("SELECT +0 AND failing()", catalog, custom.Binder());
  Vm barrier_vm = TakeValue(Vm::Create(barrier, custom.Vm()));
  RequireStatus(
      barrier_vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  const auto failed = barrier_vm.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram high_bit_hex =
      LowerOrThrow("SELECT 0xffffffffffffffff OR failing()", catalog, custom.Binder());
  Vm high_bit_hex_vm = TakeValue(Vm::Create(high_bit_hex, custom.Vm()));
  RequireStatus(high_bit_hex_vm.AttachExecutionContext(
      VmExecutionContext{*pager, catalog->version().generation}));
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
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
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

  const auto rows = ExecuteRows(program, *pager, catalog->version().generation);
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
  const auto wr_rows = ExecuteRows(without_rowid, *pager, catalog->version().generation);
  ASSERT_EQ(2U, wr_rows.size());
  EXPECT_EQ("new", TextBytes(wr_rows[0][0]));
  EXPECT_EQ(SqlValueType::kNull, wr_rows[0][2].type());
  EXPECT_EQ("old", TextBytes(wr_rows[1][0]));
  EXPECT_EQ("wr-default", TextBytes(wr_rows[1][2]));
}

}  // namespace
}  // namespace modern_sqlite
