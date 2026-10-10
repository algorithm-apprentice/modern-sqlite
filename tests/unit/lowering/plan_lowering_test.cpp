#include "modern_sqlite/lowering/plan_lowering.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
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
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
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

[[nodiscard]] CatalogSnapshotPtr IndexedMutationCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 0, .generation = 17},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT, Score REAL)"));
  input.definitions.push_back(ParseTree("CREATE UNIQUE INDEX items_name ON Items(Name)"));
  input.definitions.push_back(ParseTree("CREATE INDEX items_score ON Items(Score DESC)"));
  input.definitions.push_back(ParseTree("CREATE INDEX items_id ON Items(id)"));
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
              CatalogColumnInput{
                  .name = "Score",
                  .declared_type = "REAL",
              },
          },
      .rowid_alias = ColumnId{0},
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{1},
      .name = "items_name",
      .table = TableId{0},
      .root_page = RootPageId{3},
      .origin = IndexOrigin::kCreateIndex,
      .unique = true,
      .conflict_action = ConflictAction::kDefault,
      .key_term_count = 1,
      .terms =
          {
              CatalogIndexTerm{
                  .target = ColumnId{1},
                  .collation_name = "BINARY",
                  .order = SortOrder::kAscending,
              },
              CatalogIndexTerm{
                  .target = RowIdIndexTerm{},
                  .collation_name = "BINARY",
                  .order = SortOrder::kAscending,
              },
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{2},
      .name = "items_score",
      .table = TableId{0},
      .root_page = RootPageId{4},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 1,
      .terms =
          {
              CatalogIndexTerm{
                  .target = ColumnId{2},
                  .collation_name = "BINARY",
                  .order = SortOrder::kDescending,
              },
              CatalogIndexTerm{
                  .target = RowIdIndexTerm{},
                  .collation_name = "BINARY",
                  .order = SortOrder::kAscending,
              },
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{3},
      .name = "items_id",
      .table = TableId{0},
      .root_page = RootPageId{5},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 1,
      .terms =
          {
              CatalogIndexTerm{
                  .target = ColumnId{0},
                  .collation_name = "BINARY",
                  .order = SortOrder::kAscending,
              },
              CatalogIndexTerm{
                  .target = RowIdIndexTerm{},
                  .collation_name = "BINARY",
                  .order = SortOrder::kAscending,
              },
          },
  });
  CatalogSnapshotResult created = CatalogSnapshot::Create(std::move(input));
  if (!created.has_value()) {
    throw std::runtime_error{created.error().detail};
  }
  return *std::move(created);
}

[[nodiscard]] CatalogSnapshotPtr CreateIndexCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 0, .generation = 19},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT, Score REAL)"));
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
              CatalogColumnInput{
                  .name = "Score",
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

[[nodiscard]] CatalogSnapshotPtr EmptyCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 0, .generation = 11},
  };
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
    std::string detail = lowered.error().detail;
    if (lowered.error().program_error.has_value()) {
      const ProgramError program_error =
          TakeOptional(lowered.error().program_error, "missing nested program error");
      detail.append(": ");
      detail.append(std::to_string(static_cast<unsigned>(program_error.code)));
      detail.append(" at ");
      detail.append(std::to_string(program_error.instruction));
      detail.append(" detail ");
      detail.append(std::to_string(program_error.detail));
    }
    throw std::runtime_error(std::string{sql} + ": " + detail);
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

[[nodiscard]] std::filesystem::path IndexFixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "index_performance" / "indexed.db";
}

[[nodiscard]] std::filesystem::path AlterDefaultsFixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "lowering" / "sqlite-3.54.0-alter-defaults.db";
}

[[nodiscard]] std::vector<std::vector<SqlValue>> ExecuteRows(
    const BytecodeProgram& program, Pager& pager, std::uint64_t catalog_generation,
    VmEnvironment environment = VmEnvironment::Core(),
    const TemporaryStorageFactory* temporary_storage = nullptr,
    std::span<const SqlValue> parameters = {}) {
  Vm vm = TakeValue(Vm::Create(program, environment));
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    RequireStatus(vm.Bind(ParameterId{static_cast<std::uint32_t>(index)}, parameters[index]));
  }
  if (temporary_storage == nullptr) {
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{pager, catalog_generation}));
  } else {
    RequireStatus(vm.AttachExecutionContext(
        VmExecutionContext{pager, catalog_generation, *temporary_storage}));
  }
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

void InitializeIndexedMutationDatabase(test::WritePagerFixedVfs& vfs) {
  TransactionCoordinator coordinator = OpenWriteCoordinator(vfs);
  TransactionStatement statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  RequireStatus(statement.writer()->InitializeDatabase());
  const TableBtreeWriter table = TakeValue(statement.writer()->CreateTableBtree());
  if (table.root_page() != PageNumber{2}) {
    throw std::runtime_error{"indexed mutation table root is not page 2"};
  }
  const std::array<IndexColumnOrder, 2> name_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  const IndexBtreeWriter name = TakeValue(statement.writer()->CreateIndexBtree(name_columns));
  if (name.root_page() != PageNumber{3}) {
    throw std::runtime_error{"indexed mutation name root is not page 3"};
  }
  const std::array<IndexColumnOrder, 2> score_columns{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
      IndexColumnOrder{BinaryCollation()},
  };
  const IndexBtreeWriter score = TakeValue(statement.writer()->CreateIndexBtree(score_columns));
  if (score.root_page() != PageNumber{4}) {
    throw std::runtime_error{"indexed mutation score root is not page 4"};
  }
  const std::array<IndexColumnOrder, 2> id_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  const IndexBtreeWriter id = TakeValue(statement.writer()->CreateIndexBtree(id_columns));
  if (id.root_page() != PageNumber{5}) {
    throw std::runtime_error{"indexed mutation id root is not page 5"};
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

[[nodiscard]] std::vector<std::vector<SqlValue>> ReadIndexRows(
    test::WritePagerFixedVfs& vfs, PageNumber root_page,
    std::span<const IndexColumnOrder> columns) {
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error{"failed to reopen indexed lowering test pager"};
  }
  RequireStatus(pager->BeginRead());
  std::vector<std::vector<SqlValue>> rows;
  {
    IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager, root_page, columns));
    bool has_row = TakeValue(cursor.First());
    while (has_row) {
      const ByteBuffer payload = TakeValue(cursor.CopyPayload());
      rows.push_back(TakeValue(DecodeRecord(payload.view())));
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

TEST(ReadLowering, DefersDistinctValuesAndCompoundPlans) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  constexpr std::array<std::string_view, 3> cases{
      "SELECT DISTINCT id FROM items",
      "VALUES(1),(2)",
      "SELECT 1 UNION SELECT 2",
  };
  for (const std::string_view sql : cases) {
    SCOPED_TRACE(sql);
    const PhysicalPlan plan = OptimizeOrThrow(sql, catalog);
    LowerPlanResult lowered = LowerPlan(plan);
    ASSERT_FALSE(lowered.has_value());
    EXPECT_EQ(PlanLoweringErrorCode::kUnsupportedPlan, lowered.error().code);
    EXPECT_EQ("DISTINCT, VALUES, and compound SELECT lowering is not supported",
              lowered.error().detail);
  }
}

TEST(ReadLowering, LowersRuntimeLimitOrderByIntoTopNAndExternalFallbackBytecode) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BytecodeProgram program =
      LowerOrThrow("SELECT id, name FROM items ORDER BY name LIMIT ?1 OFFSET ?2", catalog);

  ASSERT_EQ(1U, program.sorters().size());
  ASSERT_EQ(1U, program.top_ns().size());
  EXPECT_EQ(program.sorters()[0].field_count, program.top_ns()[0].field_count);
  EXPECT_EQ(program.sorters()[0].key_field_count, program.top_ns()[0].key_field_count);
  const std::vector<InstructionKind> kinds = InstructionKinds(program);
  EXPECT_EQ(1U, static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kOpenSorter)));
  EXPECT_EQ(1U, static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kOpenTopN)));
  EXPECT_NE(std::ranges::find(kinds, InstructionKind::kCheckTopN), kinds.end());
  EXPECT_NE(std::ranges::find(kinds, InstructionKind::kInsertTopN), kinds.end());
  EXPECT_NE(std::ranges::find(kinds, InstructionKind::kRewindTopN), kinds.end());
  EXPECT_NE(std::ranges::find(kinds, InstructionKind::kRewindSorter), kinds.end());
}

TEST(ReadLowering, LowersExternalOrderByIntoVerifiedSorterBytecode) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BytecodeProgram program =
      LowerOrThrow("SELECT id, name FROM items ORDER BY name DESC NULLS FIRST", catalog);

  ASSERT_EQ(1U, program.sorters().size());
  const OrderingRecordDescriptor& sorter = program.sorters().front();
  EXPECT_EQ(2U, sorter.field_count);
  EXPECT_EQ(1U, sorter.key_field_count);
  ASSERT_EQ(1U, sorter.key_columns.size());
  EXPECT_EQ("BINARY", program.symbol(sorter.key_columns[0].collation));
  EXPECT_EQ(BytecodeSortOrder::kDescending, sorter.key_columns[0].order);
  EXPECT_EQ(BytecodeNullPlacement::kFirst, sorter.key_columns[0].null_placement);

  const std::vector<InstructionKind> kinds = InstructionKinds(program);
  EXPECT_EQ(1U, static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kOpenSorter)));
  EXPECT_EQ(1U,
            static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kInsertSorter)));
  EXPECT_EQ(1U,
            static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kRewindSorter)));
  EXPECT_EQ(2U,
            static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kReadSorterField)));
  EXPECT_EQ(1U, static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kNextSorter)));
  EXPECT_EQ(1U, static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kCloseSorter)));

  const auto insert = std::ranges::find_if(program.instructions(), [](const Instruction& value) {
    return std::holds_alternative<InsertSorterInstruction>(value);
  });
  ASSERT_NE(program.instructions().end(), insert);
  const auto& insert_operation = std::get<InsertSorterInstruction>(*insert);
  std::optional<std::size_t> payload_copy;
  std::optional<std::size_t> key_copy;
  std::size_t insert_index = 0;
  for (auto current = program.instructions().begin(); current != insert;
       ++current, ++insert_index) {
    const auto* copy = std::get_if<CopyInstruction>(&*current);
    if (copy == nullptr) {
      continue;
    }
    if (copy->output.value() == insert_operation.first_value.value()) {
      key_copy = insert_index;
    } else if (copy->output.value() == insert_operation.first_value.value() + 1U) {
      payload_copy = insert_index;
    }
  }
  ASSERT_TRUE(payload_copy.has_value());
  ASSERT_TRUE(key_copy.has_value());
  EXPECT_LT(*payload_copy, *key_copy);
  const auto rewind = std::ranges::find(kinds, InstructionKind::kRewindSorter) - kinds.begin();
  EXPECT_LT(insert_index, static_cast<std::size_t>(rewind));

  std::vector<std::uint32_t> output_fields;
  for (const Instruction& instruction : program.instructions()) {
    if (const auto* read = std::get_if<ReadSorterFieldInstruction>(&instruction); read != nullptr) {
      output_fields.push_back(read->field);
    }
  }
  EXPECT_EQ((std::vector<std::uint32_t>{1, 0}), output_fields);
}

TEST(ReadLowering, ExecutesExternalOrderByWithPmaSpillAndEmptyInputs) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 83}));
  const TemporaryStorageFactory temporary_storage =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{1},
                                                }));

  const BytecodeProgram ordered =
      LowerOrThrow("SELECT id, name FROM items ORDER BY name DESC", catalog);
  const auto rows = ExecuteRows(ordered, *pager, catalog->version().generation,
                                VmEnvironment::Core(), &temporary_storage);
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
  EXPECT_EQ("gamma", TextBytes(rows[0][1]));
  EXPECT_EQ(2, rows[1][0].integer_value());
  EXPECT_EQ("beta", TextBytes(rows[1][1]));
  EXPECT_EQ(1, rows[2][0].integer_value());
  EXPECT_EQ("alpha", TextBytes(rows[2][1]));

  const BytecodeProgram empty =
      LowerOrThrow("SELECT name FROM items WHERE 0 ORDER BY name", catalog);
  EXPECT_TRUE(ExecuteRows(empty, *pager, catalog->version().generation, VmEnvironment::Core(),
                          &temporary_storage)
                  .empty());
}

TEST(ReadLowering, ExecutesExternalOrderByForSourceFreeGuardFilterAndLookupPlans) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 89}));
  const TemporaryStorageFactory temporary_storage =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                    .sorter_memory_threshold = ByteCount{1U << 20U},
                                                }));
  const CustomEnvironment custom;

  callback_count = 0;
  const BytecodeProgram source_free = LowerOrThrow(
      "SELECT volatile_counter() AS value ORDER BY value, value DESC", catalog, custom.Binder());
  ASSERT_FALSE(source_free.requires_database_snapshot());
  ASSERT_EQ(1U, source_free.sorters().size());
  EXPECT_EQ(2U, source_free.sorters()[0].field_count);
  std::vector<std::uint32_t> source_free_output_fields;
  for (const Instruction& instruction : source_free.instructions()) {
    if (const auto* read = std::get_if<ReadSorterFieldInstruction>(&instruction); read != nullptr) {
      source_free_output_fields.push_back(read->field);
    }
  }
  EXPECT_EQ((std::vector<std::uint32_t>{1}), source_free_output_fields);
  const auto source_free_rows = ExecuteRows(source_free, *pager, catalog->version().generation,
                                            custom.Vm(), &temporary_storage);
  ASSERT_EQ(1U, source_free_rows.size());
  EXPECT_EQ(2, source_free_rows[0][0].integer_value());
  EXPECT_EQ(2U, callback_count);

  callback_count = 0;
  const BytecodeProgram source_free_limited = LowerOrThrow(
      "SELECT volatile_counter() AS value ORDER BY value LIMIT 1", catalog, custom.Binder());
  const auto source_free_limited_rows = ExecuteRows(
      source_free_limited, *pager, catalog->version().generation, custom.Vm(), &temporary_storage);
  ASSERT_EQ(1U, source_free_limited_rows.size());
  EXPECT_EQ(1, source_free_limited_rows[0][0].integer_value());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram evaluation_order = LowerOrThrow(
      "SELECT volatile_counter() AS payload FROM items "
      "WHERE rowid=1 ORDER BY volatile_counter()+0",
      catalog, custom.Binder());
  const auto evaluation_rows = ExecuteRows(evaluation_order, *pager, catalog->version().generation,
                                           custom.Vm(), &temporary_storage);
  ASSERT_EQ(1U, evaluation_rows.size());
  EXPECT_EQ(1, evaluation_rows[0][0].integer_value());
  EXPECT_EQ(2U, callback_count);

  const BytecodeProgram rejected_guard = LowerOrThrow(
      "SELECT name FROM items WHERE stable_guard(1)=0 ORDER BY name", catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(rejected_guard, *pager, catalog->version().generation, custom.Vm(),
                          &temporary_storage)
                  .empty());
  const BytecodeProgram rejected_guard_limited =
      LowerOrThrow("SELECT name FROM items WHERE stable_guard(1)=0 ORDER BY name LIMIT 1", catalog,
                   custom.Binder());
  EXPECT_TRUE(ExecuteRows(rejected_guard_limited, *pager, catalog->version().generation,
                          custom.Vm(), &temporary_storage)
                  .empty());

  const BytecodeProgram rejected_filter =
      LowerOrThrow("SELECT name FROM items WHERE id<0 ORDER BY name", catalog);
  EXPECT_TRUE(ExecuteRows(rejected_filter, *pager, catalog->version().generation,
                          VmEnvironment::Core(), &temporary_storage)
                  .empty());
  const BytecodeProgram rejected_filter_limited =
      LowerOrThrow("SELECT name FROM items WHERE id<0 ORDER BY name LIMIT 1", catalog);
  EXPECT_TRUE(ExecuteRows(rejected_filter_limited, *pager, catalog->version().generation,
                          VmEnvironment::Core(), &temporary_storage)
                  .empty());

  const BytecodeProgram lookup =
      LowerOrThrow("SELECT id, name FROM items WHERE rowid=2 ORDER BY name DESC", catalog);
  const auto lookup_rows = ExecuteRows(lookup, *pager, catalog->version().generation,
                                       VmEnvironment::Core(), &temporary_storage);
  ASSERT_EQ(1U, lookup_rows.size());
  EXPECT_EQ(2, lookup_rows[0][0].integer_value());
  EXPECT_EQ("beta", TextBytes(lookup_rows[0][1]));
  const BytecodeProgram limited_lookup =
      LowerOrThrow("SELECT id, name FROM items WHERE rowid=2 ORDER BY name DESC LIMIT 1", catalog);
  const auto limited_lookup_rows =
      ExecuteRows(limited_lookup, *pager, catalog->version().generation, VmEnvironment::Core(),
                  &temporary_storage);
  ASSERT_EQ(1U, limited_lookup_rows.size());
  EXPECT_EQ("beta", TextBytes(limited_lookup_rows[0][1]));
}

TEST(ReadLowering, ExecutesExternalOrderByAcrossCoveringAndNoncoveringIndexScans) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 101}));
  const TemporaryStorageFactory temporary_storage =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                    .sorter_memory_threshold = ByteCount{1U << 20U},
                                                }));

  const BytecodeProgram covering = LowerOrThrow(
      "SELECT id, score FROM items "
      "WHERE category>='category-000a' AND category<'category-000e' "
      "ORDER BY id DESC",
      catalog);
  ASSERT_EQ(1U, covering.cursors().size());
  const auto covering_rows = ExecuteRows(covering, *pager, catalog->version().generation,
                                         VmEnvironment::Core(), &temporary_storage);
  ASSERT_EQ(4U, covering_rows.size());
  for (std::size_t index = 0; index < covering_rows.size(); ++index) {
    const std::int64_t expected = 13 - static_cast<std::int64_t>(index);
    EXPECT_EQ(expected, covering_rows[index][0].integer_value());
    EXPECT_EQ(expected, covering_rows[index][1].integer_value());
  }

  const BytecodeProgram noncovering = LowerOrThrow(
      "SELECT id, payload FROM items "
      "WHERE category>='category-000a' AND category<'category-000e' "
      "ORDER BY id DESC",
      catalog);
  ASSERT_EQ(2U, noncovering.cursors().size());
  const auto noncovering_rows = ExecuteRows(noncovering, *pager, catalog->version().generation,
                                            VmEnvironment::Core(), &temporary_storage);
  ASSERT_EQ(4U, noncovering_rows.size());
  for (std::size_t index = 0; index < noncovering_rows.size(); ++index) {
    EXPECT_EQ(13 - static_cast<std::int64_t>(index), noncovering_rows[index][0].integer_value());
    EXPECT_EQ(128U, BlobBytes(noncovering_rows[index][1]).size());
  }

  const BytecodeProgram limited_covering = LowerOrThrow(
      "SELECT id, score FROM items "
      "WHERE category>='category-000a' AND category<'category-000e' "
      "ORDER BY id DESC LIMIT 2 OFFSET 1",
      catalog);
  ASSERT_EQ(1U, limited_covering.cursors().size());
  const auto limited_covering_rows =
      ExecuteRows(limited_covering, *pager, catalog->version().generation, VmEnvironment::Core(),
                  &temporary_storage);
  ASSERT_EQ(2U, limited_covering_rows.size());
  EXPECT_EQ(12, limited_covering_rows[0][0].integer_value());
  EXPECT_EQ(11, limited_covering_rows[1][0].integer_value());

  const BytecodeProgram limited_noncovering = LowerOrThrow(
      "SELECT id, payload FROM items "
      "WHERE category>='category-000a' AND category<'category-000e' "
      "ORDER BY id DESC LIMIT 2 OFFSET 1",
      catalog);
  ASSERT_EQ(2U, limited_noncovering.cursors().size());
  const auto limited_noncovering_rows =
      ExecuteRows(limited_noncovering, *pager, catalog->version().generation, VmEnvironment::Core(),
                  &temporary_storage);
  ASSERT_EQ(2U, limited_noncovering_rows.size());
  EXPECT_EQ(12, limited_noncovering_rows[0][0].integer_value());
  EXPECT_EQ(11, limited_noncovering_rows[1][0].integer_value());
}

TEST(ReadLowering, ExecutesRuntimeLimitOrderByStrategiesAndLazyPayloadEvaluation) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 103}));
  const TemporaryStorageFactory memory_storage =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                    .sorter_memory_threshold = ByteCount{1U << 20U},
                                                }));
  const CustomEnvironment custom;
  const BytecodeProgram program = LowerOrThrow(
      "SELECT volatile_counter() AS payload, name FROM items "
      "ORDER BY name LIMIT ?1 OFFSET ?2",
      catalog, custom.Binder());

  callback_count = 0;
  std::array<SqlValue, 2> bounded_parameters{
      SqlValue::Integer(1),
      SqlValue::Integer(1),
  };
  const auto bounded = ExecuteRows(program, *pager, catalog->version().generation, custom.Vm(),
                                   &memory_storage, bounded_parameters);
  ASSERT_EQ(1U, bounded.size());
  EXPECT_EQ(2, bounded[0][0].integer_value());
  EXPECT_EQ("beta", TextBytes(bounded[0][1]));
  EXPECT_EQ(2U, callback_count);

  callback_count = 0;
  std::array<SqlValue, 2> negative_parameters{
      SqlValue::Integer(-1),
      SqlValue::Integer(1),
  };
  const auto negative = ExecuteRows(program, *pager, catalog->version().generation, custom.Vm(),
                                    &memory_storage, negative_parameters);
  ASSERT_EQ(2U, negative.size());
  EXPECT_EQ(2, negative[0][0].integer_value());
  EXPECT_EQ("beta", TextBytes(negative[0][1]));
  EXPECT_EQ(3, negative[1][0].integer_value());
  EXPECT_EQ("gamma", TextBytes(negative[1][1]));
  EXPECT_EQ(3U, callback_count);

  callback_count = 0;
  std::array<SqlValue, 2> overflow_parameters{
      SqlValue::Integer(std::numeric_limits<std::int64_t>::max()),
      SqlValue::Integer(1),
  };
  const auto overflow = ExecuteRows(program, *pager, catalog->version().generation, custom.Vm(),
                                    &memory_storage, overflow_parameters);
  ASSERT_EQ(2U, overflow.size());
  EXPECT_EQ("beta", TextBytes(overflow[0][1]));
  EXPECT_EQ("gamma", TextBytes(overflow[1][1]));
  EXPECT_EQ(3U, callback_count);

  const BytecodeProgram zero =
      LowerOrThrow("SELECT failing() FROM items ORDER BY name LIMIT 0 OFFSET failing()", catalog,
                   custom.Binder());
  callback_count = 0;
  EXPECT_TRUE(ExecuteRows(zero, *pager, catalog->version().generation, custom.Vm(), &memory_storage)
                  .empty());
  EXPECT_EQ(0U, callback_count);
  Vm zero_without_storage = TakeValue(Vm::Create(zero, custom.Vm()));
  RequireStatus(zero_without_storage.AttachExecutionContext(
      VmExecutionContext{*pager, catalog->version().generation}));
  EXPECT_EQ(VmStep::kDone, TakeValue(zero_without_storage.Step()));
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
  EXPECT_EQ(ProgramRollbackMode::kStatement, lowered->rollback_mode());
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
                InstructionKind::kCheckInsertRowId,
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
  EXPECT_EQ(7, rows[0].second[2].integer_value());

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

TEST(InsertLowering, MaintainsIndexesAndEnforcesUniquePrefixes) {
  const CatalogSnapshotPtr catalog = IndexedMutationCatalog();
  test::WritePagerFixedVfs vfs{false};
  InitializeIndexedMutationDatabase(vfs);
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const std::vector<InstructionKind> kinds = InstructionKinds(insert);
  EXPECT_NE(std::ranges::find(kinds, InstructionKind::kCheckInsertRowId), kinds.end());
  EXPECT_NE(std::ranges::find(kinds, InstructionKind::kCheckUniqueIndex), kinds.end());
  EXPECT_EQ(3U, static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kInsertIndex)));
  const auto table_insert = std::ranges::find(kinds, InstructionKind::kInsertTable);
  const auto first_index_insert = std::ranges::find(kinds, InstructionKind::kInsertIndex);
  ASSERT_NE(kinds.end(), table_insert);
  ASSERT_NE(kinds.end(), first_index_insert);
  EXPECT_LT(first_index_insert, table_insert);

  const auto execute = [&](std::int64_t rowid, SqlValue name, SqlValue score) {
    std::vector<SqlValue> parameters;
    parameters.push_back(SqlValue::Integer(rowid));
    parameters.push_back(std::move(name));
    parameters.push_back(std::move(score));
    return ExecuteMutationProgram(insert, vfs, parameters);
  };

  EXPECT_EQ(1U, TakeValue(execute(1, SqlValue::Text("alpha"), SqlValue::Integer(7))).changes);
  const auto duplicate = execute(2, SqlValue::Text("alpha"), SqlValue::Integer(8));
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate.error().code());
  EXPECT_EQ(1U, ReadMutationRows(vfs).size());

  EXPECT_EQ(1U, TakeValue(execute(2, SqlValue{}, SqlValue::Integer(8))).changes);
  EXPECT_EQ(1U, TakeValue(execute(3, SqlValue{}, SqlValue::Integer(9))).changes);
  const auto table_rows = ReadMutationRows(vfs);
  ASSERT_EQ(3U, table_rows.size());
  EXPECT_EQ(7, table_rows[0].second[2].integer_value());

  const std::array<IndexColumnOrder, 2> name_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  const auto name_rows = ReadIndexRows(vfs, PageNumber{3}, name_columns);
  ASSERT_EQ(3U, name_rows.size());
  EXPECT_EQ(SqlValueType::kNull, name_rows[0][0].type());
  EXPECT_EQ(SqlValueType::kNull, name_rows[1][0].type());
  EXPECT_EQ("alpha", TextBytes(name_rows[2][0]));
  EXPECT_EQ(1, name_rows[2][1].integer_value());

  const std::array<IndexColumnOrder, 2> score_columns{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
      IndexColumnOrder{BinaryCollation()},
  };
  const auto score_rows = ReadIndexRows(vfs, PageNumber{4}, score_columns);
  ASSERT_EQ(3U, score_rows.size());
  EXPECT_EQ(9, score_rows[0][0].integer_value());
  EXPECT_EQ(8, score_rows[1][0].integer_value());
  EXPECT_EQ(7, score_rows[2][0].integer_value());

  const std::array<IndexColumnOrder, 2> id_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  const auto id_rows = ReadIndexRows(vfs, PageNumber{5}, id_columns);
  ASSERT_EQ(3U, id_rows.size());
  EXPECT_EQ(1, id_rows[0][0].integer_value());
  EXPECT_EQ(1, id_rows[0][1].integer_value());
  EXPECT_EQ(2, id_rows[1][0].integer_value());
  EXPECT_EQ(2, id_rows[1][1].integer_value());
  EXPECT_EQ(3, id_rows[2][0].integer_value());
  EXPECT_EQ(3, id_rows[2][1].integer_value());
}

TEST(IndexedMutationLowering, MaintainsIndexesForExactUpdateAndDelete) {
  const CatalogSnapshotPtr catalog = IndexedMutationCatalog();
  test::WritePagerFixedVfs vfs{false};
  InitializeIndexedMutationDatabase(vfs);

  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const auto execute_insert = [&](std::int64_t rowid, std::string name, std::int64_t score) {
    const std::array parameters{
        SqlValue::Integer(rowid),
        SqlValue::Text(std::move(name)),
        SqlValue::Integer(score),
    };
    return ExecuteMutationProgram(insert, vfs, parameters);
  };
  EXPECT_EQ(1U, TakeValue(execute_insert(1, "alpha", 7)).changes);
  EXPECT_EQ(1U, TakeValue(execute_insert(2, "beta", 8)).changes);

  const BytecodeProgram update =
      LowerMutationOrThrow("UPDATE Items SET id=?1,Name=?2,Score=?3 WHERE id=?4", catalog);
  const std::vector<InstructionKind> update_kinds = InstructionKinds(update);
  EXPECT_EQ(3U, static_cast<std::size_t>(
                    std::ranges::count(update_kinds, InstructionKind::kDeleteIndex)));
  EXPECT_EQ(3U, static_cast<std::size_t>(
                    std::ranges::count(update_kinds, InstructionKind::kInsertIndex)));
  const auto rowid_check = std::ranges::find(update_kinds, InstructionKind::kCheckUpdateRowId);
  const auto first_index_delete = std::ranges::find(update_kinds, InstructionKind::kDeleteIndex);
  const auto last_unique_check = std::ranges::find(update_kinds.rbegin(), update_kinds.rend(),
                                                   InstructionKind::kCheckUniqueIndex);
  ASSERT_NE(update_kinds.end(), rowid_check);
  ASSERT_NE(update_kinds.end(), first_index_delete);
  ASSERT_NE(update_kinds.rend(), last_unique_check);
  EXPECT_LT(rowid_check, first_index_delete);
  EXPECT_LT(std::prev(last_unique_check.base()), first_index_delete);
  const auto table_update = std::ranges::find(update_kinds, InstructionKind::kUpdateTable);
  const auto last_index_insert =
      std::ranges::find(update_kinds.rbegin(), update_kinds.rend(), InstructionKind::kInsertIndex);
  ASSERT_NE(update_kinds.end(), table_update);
  ASSERT_NE(update_kinds.rend(), last_index_insert);
  EXPECT_LT(std::distance(update_kinds.begin(), std::prev(last_index_insert.base())),
            std::distance(update_kinds.begin(), table_update));

  const std::array rowid_conflict_parameters{
      SqlValue::Integer(2),
      SqlValue::Text("zeta"),
      SqlValue::Integer(12),
      SqlValue::Integer(1),
  };
  const Result<MutationOutcome> rowid_conflict =
      ExecuteMutationProgram(update, vfs, rowid_conflict_parameters);
  ASSERT_FALSE(rowid_conflict.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, rowid_conflict.error().code());

  const std::array update_parameters{
      SqlValue::Integer(4),
      SqlValue::Text("delta"),
      SqlValue::Integer(11),
      SqlValue::Integer(1),
  };
  EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(update, vfs, update_parameters)).changes);

  const BytecodeProgram duplicate =
      LowerMutationOrThrow("UPDATE Items SET Name=?1 WHERE id=?2", catalog);
  const std::array duplicate_parameters{
      SqlValue::Text("delta"),
      SqlValue::Integer(2),
  };
  const Result<MutationOutcome> duplicate_result =
      ExecuteMutationProgram(duplicate, vfs, duplicate_parameters);
  ASSERT_FALSE(duplicate_result.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate_result.error().code());

  auto table_rows = ReadMutationRows(vfs);
  ASSERT_EQ(2U, table_rows.size());
  EXPECT_EQ(2, table_rows[0].first);
  EXPECT_EQ("beta", TextBytes(table_rows[0].second[1]));
  EXPECT_EQ(8, table_rows[0].second[2].integer_value());
  EXPECT_EQ(4, table_rows[1].first);
  EXPECT_EQ("delta", TextBytes(table_rows[1].second[1]));
  EXPECT_EQ(11, table_rows[1].second[2].integer_value());

  const std::array<IndexColumnOrder, 2> name_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  auto name_rows = ReadIndexRows(vfs, PageNumber{3}, name_columns);
  ASSERT_EQ(2U, name_rows.size());
  EXPECT_EQ("beta", TextBytes(name_rows[0][0]));
  EXPECT_EQ(2, name_rows[0][1].integer_value());
  EXPECT_EQ("delta", TextBytes(name_rows[1][0]));
  EXPECT_EQ(4, name_rows[1][1].integer_value());

  const std::array<IndexColumnOrder, 2> score_columns{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
      IndexColumnOrder{BinaryCollation()},
  };
  auto score_rows = ReadIndexRows(vfs, PageNumber{4}, score_columns);
  ASSERT_EQ(2U, score_rows.size());
  EXPECT_EQ(11, score_rows[0][0].integer_value());
  EXPECT_EQ(4, score_rows[0][1].integer_value());
  EXPECT_EQ(8, score_rows[1][0].integer_value());
  EXPECT_EQ(2, score_rows[1][1].integer_value());

  const std::array<IndexColumnOrder, 2> id_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  auto id_rows = ReadIndexRows(vfs, PageNumber{5}, id_columns);
  ASSERT_EQ(2U, id_rows.size());
  EXPECT_EQ(2, id_rows[0][0].integer_value());
  EXPECT_EQ(2, id_rows[0][1].integer_value());
  EXPECT_EQ(4, id_rows[1][0].integer_value());
  EXPECT_EQ(4, id_rows[1][1].integer_value());

  const BytecodeProgram delete_program =
      LowerMutationOrThrow("DELETE FROM Items WHERE id=?1", catalog);
  const std::vector<InstructionKind> delete_kinds = InstructionKinds(delete_program);
  EXPECT_EQ(3U, static_cast<std::size_t>(
                    std::ranges::count(delete_kinds, InstructionKind::kDeleteIndex)));
  const auto table_delete = std::ranges::find(delete_kinds, InstructionKind::kDeleteTable);
  const auto last_index_delete =
      std::ranges::find(delete_kinds.rbegin(), delete_kinds.rend(), InstructionKind::kDeleteIndex);
  ASSERT_NE(delete_kinds.end(), table_delete);
  ASSERT_NE(delete_kinds.rend(), last_index_delete);
  EXPECT_LT(std::distance(delete_kinds.begin(), std::prev(last_index_delete.base())),
            std::distance(delete_kinds.begin(), table_delete));

  const std::array delete_parameters{SqlValue::Integer(4)};
  EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(delete_program, vfs, delete_parameters)).changes);
  table_rows = ReadMutationRows(vfs);
  ASSERT_EQ(1U, table_rows.size());
  EXPECT_EQ(2, table_rows[0].first);
  EXPECT_EQ("beta", TextBytes(table_rows[0].second[1]));

  name_rows = ReadIndexRows(vfs, PageNumber{3}, name_columns);
  ASSERT_EQ(1U, name_rows.size());
  EXPECT_EQ("beta", TextBytes(name_rows[0][0]));
  EXPECT_EQ(2, name_rows[0][1].integer_value());
  score_rows = ReadIndexRows(vfs, PageNumber{4}, score_columns);
  ASSERT_EQ(1U, score_rows.size());
  EXPECT_EQ(8, score_rows[0][0].integer_value());
  EXPECT_EQ(2, score_rows[0][1].integer_value());
  id_rows = ReadIndexRows(vfs, PageNumber{5}, id_columns);
  ASSERT_EQ(1U, id_rows.size());
  EXPECT_EQ(2, id_rows[0][0].integer_value());
  EXPECT_EQ(2, id_rows[0][1].integer_value());
}

TEST(IndexedMutationLowering, CollectsScanRowidsAndRollsBackPartialIndexWork) {
  const CatalogSnapshotPtr catalog = IndexedMutationCatalog();
  test::WritePagerFixedVfs vfs{false};
  InitializeIndexedMutationDatabase(vfs);

  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const auto execute_insert = [&](std::int64_t rowid, SqlValue name, std::int64_t score) {
    const std::array parameters{
        SqlValue::Integer(rowid),
        std::move(name),
        SqlValue::Integer(score),
    };
    return ExecuteMutationProgram(insert, vfs, parameters);
  };
  EXPECT_EQ(1U, TakeValue(execute_insert(1, SqlValue::Text("alpha"), 7)).changes);
  EXPECT_EQ(1U, TakeValue(execute_insert(2, SqlValue::Text("beta"), 8)).changes);
  EXPECT_EQ(1U, TakeValue(execute_insert(3, SqlValue::Text("gamma"), 9)).changes);
  EXPECT_EQ(1U, TakeValue(execute_insert(4, SqlValue{}, 10)).changes);
  EXPECT_EQ(1U, TakeValue(execute_insert(5, SqlValue{}, 11)).changes);

  const BytecodeProgram conflicting =
      LowerMutationOrThrow("UPDATE Items SET Name='same' WHERE Score>=7", catalog);
  const Result<MutationOutcome> conflict = ExecuteMutationProgram(conflicting, vfs);
  ASSERT_FALSE(conflict.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, conflict.error().code());
  auto table_rows = ReadMutationRows(vfs);
  ASSERT_EQ(5U, table_rows.size());
  EXPECT_EQ("alpha", TextBytes(table_rows[0].second[1]));
  EXPECT_EQ("beta", TextBytes(table_rows[1].second[1]));

  const BytecodeProgram stable_scan =
      LowerMutationOrThrow("UPDATE Items SET Score=Score+10 WHERE Score>=9", catalog);
  const std::vector<InstructionKind> update_kinds = InstructionKinds(stable_scan);
  EXPECT_NE(std::ranges::find(update_kinds, InstructionKind::kClearRowIdList), update_kinds.end());
  EXPECT_NE(std::ranges::find(update_kinds, InstructionKind::kAppendRowIdList), update_kinds.end());
  EXPECT_NE(std::ranges::find(update_kinds, InstructionKind::kUpdateTable), update_kinds.end());
  EXPECT_EQ(std::ranges::find(update_kinds, InstructionKind::kOpenMutation), update_kinds.end());
  EXPECT_EQ(std::ranges::find(update_kinds, InstructionKind::kUpdateCurrentTable),
            update_kinds.end());
  EXPECT_EQ(3U, TakeValue(ExecuteMutationProgram(stable_scan, vfs)).changes);

  const BytecodeProgram moving_scan =
      LowerMutationOrThrow("UPDATE Items SET id=id+10 WHERE Score>=20", catalog);
  EXPECT_EQ(2U, TakeValue(ExecuteMutationProgram(moving_scan, vfs)).changes);
  table_rows = ReadMutationRows(vfs);
  ASSERT_EQ(5U, table_rows.size());
  EXPECT_EQ(1, table_rows[0].first);
  EXPECT_EQ(2, table_rows[1].first);
  EXPECT_EQ(3, table_rows[2].first);
  EXPECT_EQ(14, table_rows[3].first);
  EXPECT_EQ(15, table_rows[4].first);

  const std::array<IndexColumnOrder, 2> name_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  auto name_rows = ReadIndexRows(vfs, PageNumber{3}, name_columns);
  ASSERT_EQ(5U, name_rows.size());
  EXPECT_EQ(SqlValueType::kNull, name_rows[0][0].type());
  EXPECT_EQ(14, name_rows[0][1].integer_value());
  EXPECT_EQ(SqlValueType::kNull, name_rows[1][0].type());
  EXPECT_EQ(15, name_rows[1][1].integer_value());

  const std::array<IndexColumnOrder, 2> id_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  auto id_rows = ReadIndexRows(vfs, PageNumber{5}, id_columns);
  ASSERT_EQ(5U, id_rows.size());
  EXPECT_EQ(1, id_rows[0][0].integer_value());
  EXPECT_EQ(2, id_rows[1][0].integer_value());
  EXPECT_EQ(3, id_rows[2][0].integer_value());
  EXPECT_EQ(14, id_rows[3][0].integer_value());
  EXPECT_EQ(15, id_rows[4][0].integer_value());
  for (const auto& row : id_rows) {
    EXPECT_EQ(row[0].integer_value(), row[1].integer_value());
  }

  const BytecodeProgram delete_scan =
      LowerMutationOrThrow("DELETE FROM Items WHERE Score>=20", catalog);
  const std::vector<InstructionKind> delete_kinds = InstructionKinds(delete_scan);
  EXPECT_NE(std::ranges::find(delete_kinds, InstructionKind::kClearRowIdList), delete_kinds.end());
  EXPECT_NE(std::ranges::find(delete_kinds, InstructionKind::kAppendRowIdList), delete_kinds.end());
  EXPECT_NE(std::ranges::find(delete_kinds, InstructionKind::kDeleteTable), delete_kinds.end());
  EXPECT_EQ(std::ranges::find(delete_kinds, InstructionKind::kOpenMutation), delete_kinds.end());
  EXPECT_EQ(std::ranges::find(delete_kinds, InstructionKind::kDeleteCurrentTable),
            delete_kinds.end());
  EXPECT_EQ(2U, TakeValue(ExecuteMutationProgram(delete_scan, vfs)).changes);

  table_rows = ReadMutationRows(vfs);
  ASSERT_EQ(3U, table_rows.size());
  EXPECT_EQ(1, table_rows[0].first);
  EXPECT_EQ("alpha", TextBytes(table_rows[0].second[1]));
  EXPECT_EQ(7, table_rows[0].second[2].integer_value());
  EXPECT_EQ(2, table_rows[1].first);
  EXPECT_EQ("beta", TextBytes(table_rows[1].second[1]));
  EXPECT_EQ(8, table_rows[1].second[2].integer_value());
  EXPECT_EQ(3, table_rows[2].first);
  EXPECT_EQ("gamma", TextBytes(table_rows[2].second[1]));
  EXPECT_EQ(19, table_rows[2].second[2].integer_value());

  name_rows = ReadIndexRows(vfs, PageNumber{3}, name_columns);
  ASSERT_EQ(3U, name_rows.size());
  EXPECT_EQ("alpha", TextBytes(name_rows[0][0]));
  EXPECT_EQ(1, name_rows[0][1].integer_value());
  EXPECT_EQ("beta", TextBytes(name_rows[1][0]));
  EXPECT_EQ(2, name_rows[1][1].integer_value());
  EXPECT_EQ("gamma", TextBytes(name_rows[2][0]));
  EXPECT_EQ(3, name_rows[2][1].integer_value());

  const std::array<IndexColumnOrder, 2> score_columns{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
      IndexColumnOrder{BinaryCollation()},
  };
  const auto score_rows = ReadIndexRows(vfs, PageNumber{4}, score_columns);
  ASSERT_EQ(3U, score_rows.size());
  EXPECT_EQ(19, score_rows[0][0].integer_value());
  EXPECT_EQ(3, score_rows[0][1].integer_value());
  EXPECT_EQ(8, score_rows[1][0].integer_value());
  EXPECT_EQ(2, score_rows[1][1].integer_value());
  EXPECT_EQ(7, score_rows[2][0].integer_value());
  EXPECT_EQ(1, score_rows[2][1].integer_value());
  id_rows = ReadIndexRows(vfs, PageNumber{5}, id_columns);
  ASSERT_EQ(3U, id_rows.size());
  EXPECT_EQ(1, id_rows[0][0].integer_value());
  EXPECT_EQ(2, id_rows[1][0].integer_value());
  EXPECT_EQ(3, id_rows[2][0].integer_value());
  for (const auto& row : id_rows) {
    EXPECT_EQ(row[0].integer_value(), row[1].integer_value());
  }
}

TEST(IndexedMutationLowering, RoundsRealAffinityBeforeBuildingPhysicalKeys) {
  constexpr std::int64_t kInput = INT64_C(9007199254740993);
  constexpr std::int64_t kRounded = INT64_C(9007199254740992);
  const CatalogSnapshotPtr catalog = IndexedMutationCatalog();
  test::WritePagerFixedVfs vfs{false};
  InitializeIndexedMutationDatabase(vfs);

  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const std::array insert_parameters{
      SqlValue::Integer(1),
      SqlValue::Text("large"),
      SqlValue::Integer(kInput),
  };
  EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(insert, vfs, insert_parameters)).changes);

  const BytecodeProgram select =
      LowerOrThrow("SELECT id FROM Items WHERE Score=9007199254740993", catalog);
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const auto rows = ExecuteRows(select, *pager, catalog->version().generation);
  RequireStatus(pager->EndRead());
  EXPECT_TRUE(rows.empty());

  const BytecodeProgram rounded_select =
      LowerOrThrow("SELECT id FROM Items WHERE Score=9007199254740992", catalog);
  RequireStatus(pager->BeginRead());
  const auto rounded_rows = ExecuteRows(rounded_select, *pager, catalog->version().generation);
  RequireStatus(pager->EndRead());
  ASSERT_EQ(1U, rounded_rows.size());
  EXPECT_EQ(1, rounded_rows[0][0].integer_value());

  const BytecodeProgram update =
      LowerMutationOrThrow("UPDATE Items SET id=2,Name='updated' WHERE id=1", catalog);
  EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(update, vfs)).changes);
  const std::array<IndexColumnOrder, 2> score_columns{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
      IndexColumnOrder{BinaryCollation()},
  };
  const auto score_rows = ReadIndexRows(vfs, PageNumber{4}, score_columns);
  ASSERT_EQ(1U, score_rows.size());
  EXPECT_EQ(kRounded, score_rows[0][0].integer_value());
  EXPECT_EQ(2, score_rows[0][1].integer_value());

  const BytecodeProgram delete_program =
      LowerMutationOrThrow("DELETE FROM Items WHERE id=2", catalog);
  EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(delete_program, vfs)).changes);
  EXPECT_TRUE(ReadIndexRows(vfs, PageNumber{4}, score_columns).empty());
}

TEST(InsertLowering, RetainsLimitsAndRejectsMovedFromPlans) {
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
  EXPECT_EQ(ProgramRollbackMode::kStatement, exact.rollback_mode());
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
  ASSERT_EQ(1U, scan.cursors().size());
  EXPECT_TRUE(scan.write_cursors().empty());
  EXPECT_TRUE(std::ranges::any_of(scan.instructions(), [](const Instruction& instruction) {
    return std::holds_alternative<RewindInstruction>(instruction);
  }));
  bool mutation_open = false;
  bool scan_delete = false;
  std::optional<std::size_t> scan_close;
  std::optional<std::size_t> scan_delete_index;
  for (std::size_t index = 0; index < scan.instructions().size(); ++index) {
    const Instruction& instruction = scan.instructions()[index];
    if (std::holds_alternative<OpenMutationCursorInstruction>(instruction)) {
      mutation_open = true;
    }
    if (!scan_close.has_value() && std::holds_alternative<CloseCursorInstruction>(instruction)) {
      scan_close = index;
    }
    if (std::holds_alternative<DeleteCurrentTableInstruction>(instruction)) {
      scan_delete = true;
      scan_delete_index = index;
    }
    EXPECT_FALSE(std::holds_alternative<OpenWriteCursorInstruction>(instruction));
    EXPECT_FALSE(std::holds_alternative<DeleteTableInstruction>(instruction));
    if (const auto* seek = std::get_if<SeekRowIdInstruction>(&instruction); seek != nullptr) {
      EXPECT_NE(RowIdSeekMode::kGreater, seek->mode);
    }
  }
  EXPECT_TRUE(mutation_open);
  EXPECT_TRUE(scan_delete);
  ASSERT_TRUE(scan_close.has_value());
  ASSERT_TRUE(scan_delete_index.has_value());
  EXPECT_LT(TakeOptional(scan_delete_index, "missing scan delete"),
            TakeOptional(scan_close, "missing scan cursor close"));

  const PhysicalMutationPlan limited_plan =
      OptimizeMutationOrThrow("DELETE FROM Items WHERE Score>=?1", catalog);
  ProgramLimits limits;
  limits.maximum_cursors = 0;
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
        std::holds_alternative<DeleteCurrentTableInstruction>(instruction)) {
      function_delete = index;
    }
  }
  EXPECT_LT(TakeOptional(function_call, "missing function scan call"),
            TakeOptional(function_delete, "missing function scan delete"));
  EXPECT_LT(TakeOptional(function_delete, "missing function scan delete"),
            TakeOptional(function_close, "missing function scan cursor close"));
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
  EXPECT_EQ(ProgramRollbackMode::kStatement, exact.rollback_mode());
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
  bool mutation_open = false;
  bool update = false;
  for (const Instruction& instruction : scan.instructions()) {
    mutation_open =
        mutation_open || std::holds_alternative<OpenMutationCursorInstruction>(instruction);
    if (const auto* seek = std::get_if<SeekRowIdInstruction>(&instruction); seek != nullptr) {
      EXPECT_NE(RowIdSeekMode::kGreater, seek->mode);
    }
    EXPECT_FALSE(std::holds_alternative<UpdateTableInstruction>(instruction));
    update = update || std::holds_alternative<UpdateCurrentTableInstruction>(instruction);
  }
  EXPECT_TRUE(mutation_open);
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
    if (!update_index.has_value() &&
        std::holds_alternative<UpdateCurrentTableInstruction>(instruction)) {
      update_index = index;
    }
  }
  EXPECT_LT(TakeOptional(call, "missing UPDATE assignment call"),
            TakeOptional(update_index, "missing UPDATE current mutation"));
  EXPECT_LT(TakeOptional(update_index, "missing UPDATE current mutation"),
            TakeOptional(close, "missing UPDATE mutation cursor close"));

  const BytecodeProgram moving_scan =
      LowerMutationOrThrow("UPDATE Items SET id=id+10 WHERE Score>=?1", catalog);
  EXPECT_EQ(ProgramRollbackMode::kStatement, moving_scan.rollback_mode());
  const auto kinds = InstructionKinds(moving_scan);
  const auto has_kind = [&](InstructionKind kind) {
    return std::ranges::find(kinds, kind) != kinds.end();
  };
  EXPECT_TRUE(has_kind(InstructionKind::kClearRowIdList));
  EXPECT_TRUE(has_kind(InstructionKind::kAppendRowIdList));
  EXPECT_TRUE(has_kind(InstructionKind::kRewindRowIdList));
  EXPECT_TRUE(has_kind(InstructionKind::kNextRowIdList));
  EXPECT_TRUE(has_kind(InstructionKind::kUpdateTable));
  EXPECT_FALSE(has_kind(InstructionKind::kUpdateCurrentTable));
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
  EXPECT_EQ(1, rows[0].second[2].integer_value());
  EXPECT_EQ(2, rows[1].first);
  EXPECT_EQ("42", TextBytes(rows[1].second[1]));
  EXPECT_EQ(12, rows[1].second[2].integer_value());
  EXPECT_EQ(5, rows[2].first);
  EXPECT_EQ("gamma", TextBytes(rows[2].second[1]));
  EXPECT_EQ(13, rows[2].second[2].integer_value());

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

  test::WritePagerFixedVfs moving_vfs{false};
  InitializeMutationDatabase(moving_vfs);
  static_cast<void>(insert_item(moving_vfs, 1, "one", 1));
  static_cast<void>(insert_item(moving_vfs, 2, "two", 2));
  static_cast<void>(insert_item(moving_vfs, 3, "three", 3));
  const BytecodeProgram moving_scan =
      LowerMutationOrThrow("UPDATE Items SET id=id+10,Name=Name||'x' WHERE Score>=1", catalog);
  EXPECT_EQ(3U, TakeValue(ExecuteMutationProgram(moving_scan, moving_vfs)).changes);
  const auto moved_rows = ReadMutationRows(moving_vfs);
  ASSERT_EQ(3U, moved_rows.size());
  EXPECT_EQ(11, moved_rows[0].first);
  EXPECT_EQ("onex", TextBytes(moved_rows[0].second[1]));
  EXPECT_EQ(12, moved_rows[1].first);
  EXPECT_EQ("twox", TextBytes(moved_rows[1].second[1]));
  EXPECT_EQ(13, moved_rows[2].first);
  EXPECT_EQ("threex", TextBytes(moved_rows[2].second[1]));

  test::WritePagerFixedVfs conflict_vfs{false};
  InitializeMutationDatabase(conflict_vfs);
  static_cast<void>(insert_item(conflict_vfs, 1, "one", 1));
  static_cast<void>(insert_item(conflict_vfs, 2, "two", 2));
  const BytecodeProgram moving_conflict = LowerMutationOrThrow("UPDATE Items SET id=id+1", catalog);
  const Result<MutationOutcome> moving_conflict_result =
      ExecuteMutationProgram(moving_conflict, conflict_vfs);
  ASSERT_FALSE(moving_conflict_result.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, moving_conflict_result.error().code());
  const auto conflict_rows = ReadMutationRows(conflict_vfs);
  ASSERT_EQ(2U, conflict_rows.size());
  EXPECT_EQ(1, conflict_rows[0].first);
  EXPECT_EQ(2, conflict_rows[1].first);

  test::WritePagerFixedVfs collection_failure_vfs{false};
  InitializeMutationDatabase(collection_failure_vfs);
  static_cast<void>(insert_item(collection_failure_vfs, 1, "one", 1));
  static_cast<void>(insert_item(collection_failure_vfs, 2, "two", 2));
  callback_count = 0;
  const BytecodeProgram collection_failure = LowerMutationOrThrow(
      "UPDATE Items SET id=id+10 WHERE fail_after_one(Score)", catalog, custom.Binder());
  const Result<MutationOutcome> collection_failed =
      ExecuteMutationProgram(collection_failure, collection_failure_vfs, {}, custom.Vm());
  ASSERT_FALSE(collection_failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, collection_failed.error().code());
  EXPECT_EQ(2U, callback_count);
  const auto collection_restored = ReadMutationRows(collection_failure_vfs);
  ASSERT_EQ(2U, collection_restored.size());
  EXPECT_EQ(1, collection_restored[0].first);
  EXPECT_EQ(2, collection_restored[1].first);

  test::WritePagerFixedVfs moving_failure_vfs{false};
  InitializeMutationDatabase(moving_failure_vfs);
  static_cast<void>(insert_item(moving_failure_vfs, 1, "one", 1));
  static_cast<void>(insert_item(moving_failure_vfs, 2, "two", 2));
  callback_count = 0;
  const BytecodeProgram moving_failure = LowerMutationOrThrow(
      "UPDATE Items SET id=id+10,Name=fail_after_one(Name)", catalog, custom.Binder());
  const Result<MutationOutcome> moving_failed =
      ExecuteMutationProgram(moving_failure, moving_failure_vfs, {}, custom.Vm());
  ASSERT_FALSE(moving_failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, moving_failed.error().code());
  EXPECT_EQ(2U, callback_count);
  const auto moving_restored = ReadMutationRows(moving_failure_vfs);
  ASSERT_EQ(2U, moving_restored.size());
  EXPECT_EQ(1, moving_restored[0].first);
  EXPECT_EQ("one", TextBytes(moving_restored[0].second[1]));
  EXPECT_EQ(2, moving_restored[1].first);
  EXPECT_EQ("two", TextBytes(moving_restored[1].second[1]));

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

TEST(CreateTableLowering, EmitsSchemaMutationAndNoOpPrograms) {
  const CatalogSnapshotPtr catalog = EmptyCatalog();
  const BytecodeProgram program = LowerMutationOrThrow(
      "CREATE TABLE main.NewItems("
      "id INTEGER PRIMARY KEY, "
      "Name TEXT NOT NULL DEFAULT 'seed'"
      ")",
      catalog);
  EXPECT_EQ(ProgramStatementKind::kCreateTable, program.statement_kind());
  EXPECT_EQ(ProgramTransactionAccess::kWrite, program.transaction_access());
  EXPECT_EQ(ProgramRollbackMode::kStatement, program.rollback_mode());
  EXPECT_FALSE(program.mutation_result().publishes_changes);
  EXPECT_FALSE(program.mutation_result().publishes_last_insert_rowid);
  EXPECT_EQ(8U, program.register_count());
  ASSERT_EQ(1U, program.write_cursors().size());
  const WriteCursorDescriptor& schema = program.write_cursor(WriteCursorId{0});
  EXPECT_EQ(RootPageNumber{1}, schema.root_page);
  ASSERT_EQ(5U, schema.columns.size());
  EXPECT_EQ(TypeAffinity::kInteger, schema.columns[3].affinity);
  EXPECT_FALSE(schema.rowid_alias.has_value());
  EXPECT_EQ((std::vector{
                InstructionKind::kEnsureDatabaseInitialized,
                InstructionKind::kCreateTableRoot,
                InstructionKind::kLoadConstant,
                InstructionKind::kLoadConstant,
                InstructionKind::kLoadConstant,
                InstructionKind::kLoadConstant,
                InstructionKind::kLoadConstant,
                InstructionKind::kOpenWrite,
                InstructionKind::kResolveInsertRowId,
                InstructionKind::kBuildTableRecord,
                InstructionKind::kInsertTable,
                InstructionKind::kCloseWrite,
                InstructionKind::kIncrementSchemaCookie,
                InstructionKind::kHalt,
            }),
            InstructionKinds(program));

  const CatalogSnapshotPtr existing = MutationCatalog();
  const BytecodeProgram no_op =
      LowerMutationOrThrow("CREATE TABLE IF NOT EXISTS Items(a UNIQUE)", existing);
  EXPECT_EQ(ProgramStatementKind::kCreateTable, no_op.statement_kind());
  EXPECT_EQ(ProgramRollbackMode::kTransaction, no_op.rollback_mode());
  EXPECT_EQ(0U, no_op.register_count());
  EXPECT_TRUE(no_op.constants().empty());
  EXPECT_TRUE(no_op.write_cursors().empty());
  EXPECT_EQ((std::vector{InstructionKind::kHalt}), InstructionKinds(no_op));
}

TEST(CreateTableLowering, WritesLoadableCanonicalSchema) {
  const CatalogSnapshotPtr catalog = EmptyCatalog();
  test::WritePagerFixedVfs vfs{false};
  const BytecodeProgram program = LowerMutationOrThrow(
      "CREATE TABLE main.NewItems("
      "id INTEGER PRIMARY KEY, "
      "Name TEXT NOT NULL DEFAULT 'seed'"
      ")",
      catalog);
  CatalogSnapshotPtr candidate;
  {
    std::unique_ptr<Pager> active_pager = test::OpenWritePager(vfs, 64U);
    ASSERT_NE(nullptr, active_pager);
    Pager* const pager_identity = active_pager.get();
    TransactionCoordinator coordinator =
        TakeValue(TransactionCoordinator::Open(std::move(active_pager)));
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kStatement,
        }));
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 11}));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_EQ(0U, vm.change_count());
    EXPECT_FALSE(vm.last_insert_rowid_event().has_value());
    RequireStatus(vm.DetachExecutionContext());
    candidate = TakeValue(LoadCatalog(*pager_identity, CatalogLoadOptions{.generation = 12}));
    RequireStatus(statement.Succeed());
  }

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr durable =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 12}));
  const auto expect_catalog = [&](const CatalogSnapshotPtr& loaded) {
    EXPECT_EQ((CatalogVersion{.schema_cookie = 1, .generation = 12}), loaded->version());
    const std::optional<TableId> table_id = loaded->FindTable("newitems");
    ASSERT_TRUE(table_id.has_value());
    const CatalogTable& table = loaded->table(TakeOptional(table_id, "missing created table"));
    EXPECT_EQ(RootPageId{2}, table.root_page);
    ASSERT_EQ(2U, table.columns.size());
    EXPECT_EQ(std::optional<ColumnId>{ColumnId{0}}, table.rowid_alias);
    ASSERT_NE(nullptr, table.columns[1].missing_record_value);
    EXPECT_EQ("seed", TextBytes(*table.columns[1].missing_record_value));
    EXPECT_EQ("CREATE TABLE NewItems(id INTEGER PRIMARY KEY, Name TEXT NOT NULL DEFAULT 'seed')",
              loaded->definition(table.definition).source().bytes());
  };
  expect_catalog(candidate);
  expect_catalog(durable);
  RequireStatus(pager->EndRead());
}

TEST(CreateIndexLowering, EmitsSchemaPopulationProgram) {
  const CatalogSnapshotPtr catalog = CreateIndexCatalog();
  const BytecodeProgram program = LowerMutationOrThrow(
      "CREATE UNIQUE INDEX items_name_score ON Items(Name COLLATE NOCASE DESC,Score)", catalog);
  EXPECT_EQ(ProgramStatementKind::kCreateIndex, program.statement_kind());
  EXPECT_EQ(ProgramRollbackMode::kStatement, program.rollback_mode());
  EXPECT_TRUE(program.requires_database_snapshot());
  EXPECT_FALSE(program.mutation_result().publishes_changes);
  EXPECT_EQ(11U, program.register_count());
  ASSERT_EQ(1U, program.cursors().size());
  ASSERT_EQ(2U, program.write_cursors().size());
  EXPECT_EQ(CursorStorageKind::kRowIdTable, program.cursors()[0].storage);
  EXPECT_EQ(WriteCursorStorageKind::kRowIdTable, program.write_cursors()[0].storage);
  EXPECT_EQ(WriteCursorStorageKind::kIndex, program.write_cursors()[1].storage);
  EXPECT_TRUE(program.write_cursors()[1].pending_root);
  EXPECT_EQ(RootPageNumber{0}, program.write_cursors()[1].root_page);
  EXPECT_EQ(2U, program.write_cursors()[1].key_term_count);
  EXPECT_TRUE(program.write_cursors()[1].unique);
  ASSERT_EQ(3U, program.write_cursors()[1].index_columns.size());
  EXPECT_EQ(BytecodeSortOrder::kDescending, program.write_cursors()[1].index_columns[0].order);

  const std::vector<InstructionKind> kinds = InstructionKinds(program);
  const auto create_root = std::ranges::find(kinds, InstructionKind::kCreateIndexRoot);
  const auto schema_insert = std::ranges::find(kinds, InstructionKind::kInsertTable);
  const auto index_insert = std::ranges::find(kinds, InstructionKind::kInsertIndex);
  const auto cookie = std::ranges::find(kinds, InstructionKind::kIncrementSchemaCookie);
  ASSERT_NE(kinds.end(), create_root);
  ASSERT_NE(kinds.end(), schema_insert);
  ASSERT_NE(kinds.end(), index_insert);
  ASSERT_NE(kinds.end(), cookie);
  EXPECT_LT(create_root, schema_insert);
  EXPECT_LT(schema_insert, index_insert);
  EXPECT_LT(index_insert, cookie);

  const CatalogSnapshotPtr indexed = IndexedMutationCatalog();
  const BytecodeProgram no_op = LowerMutationOrThrow(
      "CREATE INDEX IF NOT EXISTS items_name ON Items(no_such_column)", indexed);
  EXPECT_EQ(ProgramRollbackMode::kTransaction, no_op.rollback_mode());
  EXPECT_EQ(0U, no_op.register_count());
  EXPECT_TRUE(no_op.cursors().empty());
  EXPECT_TRUE(no_op.write_cursors().empty());
  EXPECT_EQ((std::vector{InstructionKind::kHalt}), InstructionKinds(no_op));
}

TEST(CreateIndexLowering, PopulatesPhysicalRecordsAndRollsBackUniqueFailure) {
  const CatalogSnapshotPtr catalog = CreateIndexCatalog();
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const BytecodeProgram create = LowerMutationOrThrow(
      "CREATE UNIQUE INDEX items_name_score ON Items(Name COLLATE NOCASE DESC,Score)", catalog);
  const auto insert_row = [&](test::WritePagerFixedVfs& vfs, std::int64_t rowid, SqlValue name,
                              std::int64_t score) {
    const std::array parameters{
        SqlValue::Integer(rowid),
        std::move(name),
        SqlValue::Integer(score),
    };
    return ExecuteMutationProgram(insert, vfs, parameters);
  };

  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);
  EXPECT_EQ(1U, TakeValue(insert_row(vfs, 1, SqlValue::Text("alpha"), 7)).changes);
  EXPECT_EQ(1U, TakeValue(insert_row(vfs, 2, SqlValue::Text("beta"), 8)).changes);
  EXPECT_EQ(1U, TakeValue(insert_row(vfs, 3, SqlValue{}, 9)).changes);
  EXPECT_EQ(1U, TakeValue(insert_row(vfs, 4, SqlValue{}, 9)).changes);
  EXPECT_EQ(0U, TakeValue(ExecuteMutationProgram(create, vfs)).changes);

  const auto schema_rows = ReadMutationRows(vfs, PageNumber{1});
  ASSERT_EQ(1U, schema_rows.size());
  ASSERT_EQ(5U, schema_rows[0].second.size());
  EXPECT_EQ("index", TextBytes(schema_rows[0].second[0]));
  EXPECT_EQ("items_name_score", TextBytes(schema_rows[0].second[1]));
  EXPECT_EQ("Items", TextBytes(schema_rows[0].second[2]));
  EXPECT_EQ(4, schema_rows[0].second[3].integer_value());
  EXPECT_EQ("CREATE UNIQUE INDEX items_name_score ON Items(Name COLLATE NOCASE DESC,Score)",
            TextBytes(schema_rows[0].second[4]));

  const std::array<IndexColumnOrder, 3> columns{
      IndexColumnOrder{NoCaseCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  const auto index_rows = ReadIndexRows(vfs, PageNumber{4}, columns);
  ASSERT_EQ(4U, index_rows.size());
  EXPECT_EQ("beta", TextBytes(index_rows[0][0]));
  EXPECT_EQ(8, index_rows[0][1].integer_value());
  EXPECT_EQ(2, index_rows[0][2].integer_value());
  EXPECT_EQ("alpha", TextBytes(index_rows[1][0]));
  EXPECT_EQ(7, index_rows[1][1].integer_value());
  EXPECT_EQ(1, index_rows[1][2].integer_value());
  EXPECT_EQ(SqlValueType::kNull, index_rows[2][0].type());
  EXPECT_EQ(3, index_rows[2][2].integer_value());
  EXPECT_EQ(SqlValueType::kNull, index_rows[3][0].type());
  EXPECT_EQ(4, index_rows[3][2].integer_value());

  {
    std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    ASSERT_NE(nullptr, pager->header());
    EXPECT_EQ(1U, pager->header()->schema_cookie());
    RequireStatus(pager->EndRead());
  }

  test::WritePagerFixedVfs conflict_vfs{false};
  InitializeMutationDatabase(conflict_vfs);
  EXPECT_EQ(1U, TakeValue(insert_row(conflict_vfs, 1, SqlValue::Text("alpha"), 7)).changes);
  EXPECT_EQ(1U, TakeValue(insert_row(conflict_vfs, 2, SqlValue::Text("ALPHA"), 7)).changes);
  const Result<MutationOutcome> conflict = ExecuteMutationProgram(create, conflict_vfs);
  ASSERT_FALSE(conflict.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, conflict.error().code());
  EXPECT_TRUE(ReadMutationRows(conflict_vfs, PageNumber{1}).empty());

  const BytecodeProgram nonunique = LowerMutationOrThrow(
      "CREATE INDEX items_name_score_nonunique "
      "ON Items(Name COLLATE NOCASE DESC,Score)",
      catalog);
  EXPECT_EQ(0U, TakeValue(ExecuteMutationProgram(nonunique, conflict_vfs)).changes);
  const auto restored_schema = ReadMutationRows(conflict_vfs, PageNumber{1});
  ASSERT_EQ(1U, restored_schema.size());
  EXPECT_EQ(4, restored_schema[0].second[3].integer_value());

  test::WritePagerFixedVfs empty_vfs{false};
  InitializeMutationDatabase(empty_vfs);
  EXPECT_EQ(0U, TakeValue(ExecuteMutationProgram(nonunique, empty_vfs)).changes);
  EXPECT_TRUE(ReadIndexRows(empty_vfs, PageNumber{4}, columns).empty());
}

TEST(CreateIndexLowering, PreservesTableScanAcrossIndexPageSplits) {
  const CatalogSnapshotPtr catalog = CreateIndexCatalog();
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  for (std::int64_t index = 0; index < 96; ++index) {
    const std::array parameters{
        SqlValue::Integer(index + 1),
        SqlValue::Text("name-" + std::to_string(196 - index)),
        SqlValue::Integer(index),
    };
    EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(insert, vfs, parameters)).changes);
  }

  const BytecodeProgram create =
      LowerMutationOrThrow("CREATE INDEX items_name ON Items(Name)", catalog);
  EXPECT_EQ(0U, TakeValue(ExecuteMutationProgram(create, vfs)).changes);
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  const auto schema_rows = ReadMutationRows(vfs, PageNumber{1});
  ASSERT_EQ(1U, schema_rows.size());
  const std::optional<std::int64_t> root_page = schema_rows[0].second[3].integer_value();
  ASSERT_TRUE(root_page.has_value());
  ASSERT_GT(*root_page, 0);
  const auto rows = ReadIndexRows(vfs, PageNumber{static_cast<std::uint32_t>(*root_page)}, columns);
  ASSERT_EQ(96U, rows.size());
  EXPECT_EQ("name-101", TextBytes(rows.front()[0]));
  EXPECT_EQ("name-196", TextBytes(rows.back()[0]));
  for (std::size_t index = 1; index < rows.size(); ++index) {
    EXPECT_LT(TextBytes(rows[index - 1U][0]), TextBytes(rows[index][0]));
  }
}

TEST(CreateIndexLowering, PopulatesIntegerPrimaryKeyTermsFromRowids) {
  const CatalogSnapshotPtr catalog = CreateIndexCatalog();
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  for (const std::int64_t rowid : std::array<std::int64_t, 3>{1, 3, 2}) {
    const std::array parameters{
        SqlValue::Integer(rowid),
        SqlValue::Text("row"),
        SqlValue::Integer(rowid),
    };
    EXPECT_EQ(1U, TakeValue(ExecuteMutationProgram(insert, vfs, parameters)).changes);
  }

  const BytecodeProgram create =
      LowerMutationOrThrow("CREATE INDEX items_id ON Items(id DESC)", catalog);
  EXPECT_EQ(0U, TakeValue(ExecuteMutationProgram(create, vfs)).changes);
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
      IndexColumnOrder{BinaryCollation()},
  };
  const auto rows = ReadIndexRows(vfs, PageNumber{4}, columns);
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
  EXPECT_EQ(3, rows[0][1].integer_value());
  EXPECT_EQ(2, rows[1][0].integer_value());
  EXPECT_EQ(2, rows[1][1].integer_value());
  EXPECT_EQ(1, rows[2][0].integer_value());
  EXPECT_EQ(1, rows[2][1].integer_value());
}

TEST(CreateIndexLowering, RetainsResourceLimitsAndMoveSafety) {
  const CatalogSnapshotPtr catalog = CreateIndexCatalog();
  PhysicalMutationPlan plan =
      OptimizeMutationOrThrow("CREATE INDEX items_name ON Items(Name)", catalog);

  ProgramLimits limits;
  limits.maximum_cursors = 2;
  LowerPlanResult limited = LowerPlan(plan, limits);
  ASSERT_FALSE(limited.has_value());
  EXPECT_EQ(PlanLoweringErrorCode::kResourceLimit, limited.error().code);
  ASSERT_TRUE(limited.error().program_error.has_value());
  EXPECT_EQ(ProgramErrorCode::kCursorLimitExceeded,
            TakeOptional(limited.error().program_error, "missing CREATE INDEX cursor limit").code);

  const PhysicalMutationPlan moved = std::move(plan);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  LowerPlanResult invalid = LowerPlan(plan);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(PlanLoweringErrorCode::kInvalidInput, invalid.error().code);
  EXPECT_TRUE(LowerPlan(moved).has_value());
}

TEST(AnalyzeLowering, CreatesStat1AndComputesIndexPrefixes) {
  const CatalogSnapshotPtr catalog = IndexedMutationCatalog();
  const BytecodeProgram program = LowerMutationOrThrow("ANALYZE items_name", catalog);
  EXPECT_EQ(ProgramStatementKind::kAnalyze, program.statement_kind());
  const std::vector<InstructionKind> kinds = InstructionKinds(program);
  EXPECT_NE(std::ranges::find(kinds, InstructionKind::kCreateTableRoot), kinds.end());
  EXPECT_NE(std::ranges::find(kinds, InstructionKind::kClearStat1), kinds.end());
  EXPECT_NE(std::ranges::find(kinds, InstructionKind::kComputeIndexStat1), kinds.end());

  test::WritePagerFixedVfs vfs{false};
  InitializeIndexedMutationDatabase(vfs);
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const auto insert_row = [&](std::int64_t rowid, SqlValue name, std::int64_t score) {
    const std::array parameters{
        SqlValue::Integer(rowid),
        std::move(name),
        SqlValue::Integer(score),
    };
    return ExecuteMutationProgram(insert, vfs, parameters);
  };
  EXPECT_EQ(1U, TakeValue(insert_row(1, SqlValue::Text("alpha"), 7)).changes);
  EXPECT_EQ(1U, TakeValue(insert_row(2, SqlValue::Text("beta"), 8)).changes);
  EXPECT_EQ(1U, TakeValue(insert_row(3, SqlValue{}, 9)).changes);
  EXPECT_EQ(1U, TakeValue(insert_row(4, SqlValue{}, 10)).changes);
  EXPECT_EQ(0U, TakeValue(ExecuteMutationProgram(program, vfs)).changes);

  const auto schema_rows = ReadMutationRows(vfs, PageNumber{1});
  ASSERT_EQ(1U, schema_rows.size());
  EXPECT_EQ("sqlite_stat1", TextBytes(schema_rows[0].second[1]));
  EXPECT_EQ(6, schema_rows[0].second[3].integer_value());
  EXPECT_EQ("CREATE TABLE sqlite_stat1(tbl,idx,stat)", TextBytes(schema_rows[0].second[4]));
  const auto stat_rows = ReadMutationRows(vfs, PageNumber{6});
  ASSERT_EQ(1U, stat_rows.size());
  EXPECT_EQ("Items", TextBytes(stat_rows[0].second[0]));
  EXPECT_EQ("items_name", TextBytes(stat_rows[0].second[1]));
  EXPECT_EQ("4 2", TextBytes(stat_rows[0].second[2]));
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

  ProgramLimits sorter_limit;
  sorter_limit.maximum_sorters = 0;
  expect_limit("SELECT name FROM items ORDER BY name", sorter_limit,
               ProgramErrorCode::kSorterLimitExceeded);

  ProgramLimits top_n_limit;
  top_n_limit.maximum_top_ns = 0;
  expect_limit("SELECT name FROM items ORDER BY name LIMIT 1", top_n_limit,
               ProgramErrorCode::kTopNLimitExceeded);

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

TEST(ReadLowering, LowersAndExecutesIndexRanges) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 97}));

  const BytecodeProgram equality =
      LowerOrThrow("SELECT id, score FROM items WHERE category='category-000a'", catalog);
  ASSERT_EQ(1U, equality.cursors().size());
  const ReadCursorDescriptor& equality_cursor = equality.cursors()[0];
  EXPECT_EQ(CursorStorageKind::kIndex, equality_cursor.storage);
  const IndexId category_index =
      TakeOptional(catalog->FindIndex("items_category_score"), "missing category index");
  EXPECT_EQ(catalog->index(category_index).root_page.value, equality_cursor.root_page.value());
  EXPECT_EQ(3U, equality_cursor.record_field_count);
  ASSERT_EQ(3U, equality_cursor.fields.size());
  ASSERT_EQ(3U, equality_cursor.index_columns.size());
  const std::vector<InstructionKind> equality_kinds = InstructionKinds(equality);
  EXPECT_NE(std::ranges::find(equality_kinds, InstructionKind::kApplyAffinity),
            equality_kinds.end());
  EXPECT_NE(std::ranges::find(equality_kinds, InstructionKind::kSeekIndex), equality_kinds.end());
  EXPECT_NE(std::ranges::find(equality_kinds, InstructionKind::kCheckIndexRange),
            equality_kinds.end());

  const auto equality_rows = ExecuteRows(equality, *pager, catalog->version().generation);
  ASSERT_EQ(1U, equality_rows.size());
  EXPECT_EQ(10, equality_rows[0][0].integer_value());
  EXPECT_EQ(10, equality_rows[0][1].integer_value());

  const BytecodeProgram is_equality =
      LowerOrThrow("SELECT id, score FROM items WHERE category IS 'category-000a'", catalog);
  const auto is_rows = ExecuteRows(is_equality, *pager, catalog->version().generation);
  ASSERT_EQ(1U, is_rows.size());
  EXPECT_EQ(10, is_rows[0][0].integer_value());

  const BytecodeProgram is_null =
      LowerOrThrow("SELECT id FROM items WHERE category IS NULL", catalog);
  EXPECT_TRUE(ExecuteRows(is_null, *pager, catalog->version().generation).empty());

  const BytecodeProgram range = LowerOrThrow(
      "SELECT id, score FROM items "
      "WHERE category>='category-000a' AND category<'category-000e'",
      catalog);
  const auto range_rows = ExecuteRows(range, *pager, catalog->version().generation);
  ASSERT_EQ(4U, range_rows.size());
  for (std::size_t index = 0; index < range_rows.size(); ++index) {
    const auto expected = static_cast<std::int64_t>(index) + 10;
    EXPECT_EQ(expected, range_rows[index][0].integer_value());
    EXPECT_EQ(expected, range_rows[index][1].integer_value());
  }

  const BytecodeProgram reordered =
      LowerOrThrow("SELECT id FROM items WHERE score=?2 AND category=?1", catalog);
  std::vector<std::uint32_t> key_parameter_order;
  for (const Instruction& instruction : reordered.instructions()) {
    if (std::holds_alternative<OpenReadCursorInstruction>(instruction)) {
      break;
    }
    if (const auto* parameter = std::get_if<LoadParameterInstruction>(&instruction);
        parameter != nullptr) {
      key_parameter_order.push_back(parameter->parameter.value());
    }
  }
  EXPECT_EQ((std::vector<std::uint32_t>{1, 0}), key_parameter_order);
  const auto reordered_seek = std::ranges::find_if(
      reordered.instructions(),
      [](const Instruction& value) { return std::holds_alternative<SeekIndexInstruction>(value); });
  ASSERT_NE(reordered.instructions().end(), reordered_seek);
  EXPECT_EQ(2U, std::get<SeekIndexInstruction>(*reordered_seek).key_count);

  const BytecodeProgram descending =
      LowerOrThrow("SELECT id, score FROM items WHERE score>=250 AND score<252", catalog);
  const auto seek = std::ranges::find_if(descending.instructions(), [](const Instruction& value) {
    return std::holds_alternative<SeekIndexInstruction>(value);
  });
  ASSERT_NE(descending.instructions().end(), seek);
  EXPECT_EQ(IndexSeekMode::kGreater, std::get<SeekIndexInstruction>(*seek).mode);
  const auto end = std::ranges::find_if(descending.instructions(), [](const Instruction& value) {
    return std::holds_alternative<CheckIndexRangeInstruction>(value);
  });
  ASSERT_NE(descending.instructions().end(), end);
  EXPECT_EQ(IndexRangeEndMode::kInclusive, std::get<CheckIndexRangeInstruction>(*end).mode);

  const auto descending_rows = ExecuteRows(descending, *pager, catalog->version().generation);
  ASSERT_EQ(32U, descending_rows.size());
  EXPECT_EQ(251, descending_rows.front()[1].integer_value());
  EXPECT_EQ(251, descending_rows[15][1].integer_value());
  EXPECT_EQ(250, descending_rows[16][1].integer_value());
  EXPECT_EQ(250, descending_rows.back()[1].integer_value());

  const BytecodeProgram reversed =
      LowerOrThrow("SELECT id, score FROM items WHERE 249<score AND 251>=score", catalog);
  const auto reversed_rows = ExecuteRows(reversed, *pager, catalog->version().generation);
  ASSERT_EQ(32U, reversed_rows.size());
  EXPECT_EQ(251, reversed_rows.front()[1].integer_value());
  EXPECT_EQ(250, reversed_rows.back()[1].integer_value());

  const BytecodeProgram affinity = LowerOrThrow("SELECT id FROM items WHERE score='250'", catalog);
  const auto affinity_rows = ExecuteRows(affinity, *pager, catalog->version().generation);
  ASSERT_EQ(16U, affinity_rows.size());
  EXPECT_EQ(250, affinity_rows.front()[0].integer_value());

  const BytecodeProgram residual =
      LowerOrThrow("SELECT id FROM items WHERE flag=0 AND id>10", catalog);
  const auto residual_rows = ExecuteRows(residual, *pager, catalog->version().generation);
  ASSERT_EQ(2043U, residual_rows.size());
  EXPECT_EQ(12, residual_rows.front()[0].integer_value());
  EXPECT_EQ(4096, residual_rows.back()[0].integer_value());

  const BytecodeProgram limited =
      LowerOrThrow("SELECT id FROM items WHERE flag=0 LIMIT 2 OFFSET 1", catalog);
  const auto limited_rows = ExecuteRows(limited, *pager, catalog->version().generation);
  ASSERT_EQ(2U, limited_rows.size());
  EXPECT_EQ(4, limited_rows[0][0].integer_value());
  EXPECT_EQ(6, limited_rows[1][0].integer_value());

  const BytecodeProgram hidden_rowid =
      LowerOrThrow("SELECT rowid FROM items WHERE flag=0 LIMIT 2", catalog);
  const auto hidden_rowid_rows = ExecuteRows(hidden_rowid, *pager, catalog->version().generation);
  ASSERT_EQ(2U, hidden_rowid_rows.size());
  EXPECT_EQ(2, hidden_rowid_rows[0][0].integer_value());
  EXPECT_EQ(4, hidden_rowid_rows[1][0].integer_value());

  const BytecodeProgram full = LowerOrThrow("SELECT id, score FROM items LIMIT 3", catalog);
  const auto full_rows = ExecuteRows(full, *pager, catalog->version().generation);
  ASSERT_EQ(3U, full_rows.size());
  EXPECT_EQ(255, full_rows[0][0].integer_value());
  EXPECT_EQ(255, full_rows[0][1].integer_value());
  EXPECT_EQ(511, full_rows[1][0].integer_value());
  EXPECT_EQ(767, full_rows[2][0].integer_value());

  const BytecodeProgram nullable = LowerOrThrow("SELECT id FROM items WHERE category=?1", catalog);
  Vm null_vm = TakeValue(Vm::Create(nullable, VmEnvironment::Core()));
  RequireStatus(
      null_vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  RequireStatus(null_vm.Bind(ParameterId{0}, SqlValue{}));
  EXPECT_EQ(VmStep::kDone, TakeValue(null_vm.Step()));

  const BytecodeProgram missing =
      LowerOrThrow("SELECT id FROM items WHERE category='missing'", catalog);
  EXPECT_TRUE(ExecuteRows(missing, *pager, catalog->version().generation).empty());

  const BytecodeProgram noncovering =
      LowerOrThrow("SELECT payload FROM items WHERE category='category-000a'", catalog);
  ASSERT_EQ(2U, noncovering.cursors().size());
  EXPECT_EQ(CursorStorageKind::kIndex, noncovering.cursors()[0].storage);
  EXPECT_EQ(CursorStorageKind::kRowIdTable, noncovering.cursors()[1].storage);
  const std::vector<InstructionKind> noncovering_kinds = InstructionKinds(noncovering);
  EXPECT_EQ(2U, static_cast<std::size_t>(
                    std::ranges::count(noncovering_kinds, InstructionKind::kOpenRead)));
  EXPECT_NE(std::ranges::find(noncovering_kinds, InstructionKind::kReadField),
            noncovering_kinds.end());
  EXPECT_NE(std::ranges::find(noncovering_kinds, InstructionKind::kSeekTableRowId),
            noncovering_kinds.end());
  const auto noncovering_rows = ExecuteRows(noncovering, *pager, catalog->version().generation);
  ASSERT_EQ(1U, noncovering_rows.size());
  EXPECT_EQ(128U, BlobBytes(noncovering_rows[0][0]).size());

  const BytecodeProgram noncovering_range = LowerOrThrow(
      "SELECT payload FROM items "
      "WHERE category>='category-000a' AND category<'category-000e'",
      catalog);
  const auto noncovering_range_rows =
      ExecuteRows(noncovering_range, *pager, catalog->version().generation);
  ASSERT_EQ(4U, noncovering_range_rows.size());
  for (const auto& row : noncovering_range_rows) {
    EXPECT_EQ(128U, BlobBytes(row[0]).size());
  }

  const BytecodeProgram upper_only =
      LowerOrThrow("SELECT payload FROM items WHERE category<'category-0003'", catalog);
  EXPECT_EQ(2U, ExecuteRows(upper_only, *pager, catalog->version().generation).size());

  const BytecodeProgram descending_noncovering =
      LowerOrThrow("SELECT payload FROM items WHERE score>=250", catalog);
  EXPECT_EQ(96U, ExecuteRows(descending_noncovering, *pager, catalog->version().generation).size());

  const BytecodeProgram noncovering_residual = LowerOrThrow(
      "SELECT payload FROM items "
      "WHERE category>='category-000a' AND category<'category-000e' AND flag=0",
      catalog);
  EXPECT_EQ(2U, ExecuteRows(noncovering_residual, *pager, catalog->version().generation).size());

  const BytecodeProgram noncovering_limit = LowerOrThrow(
      "SELECT payload FROM items "
      "WHERE category>='category-000a' AND category<'category-000e' LIMIT 2 OFFSET 1",
      catalog);
  EXPECT_EQ(2U, ExecuteRows(noncovering_limit, *pager, catalog->version().generation).size());

  const BytecodeProgram unselective_noncovering =
      LowerOrThrow("SELECT payload FROM items WHERE flag=0", catalog);
  ASSERT_EQ(1U, unselective_noncovering.cursors().size());
  EXPECT_EQ(CursorStorageKind::kRowIdTable, unselective_noncovering.cursors()[0].storage);

  const CustomEnvironment custom;
  callback_count = 0;
  const BytecodeProgram ordered_keys = LowerOrThrow(
      "SELECT id, score FROM items "
      "WHERE score>=volatile_counter() AND score<volatile_counter()",
      catalog, custom.Binder());
  const auto ordered_rows =
      ExecuteRows(ordered_keys, *pager, catalog->version().generation, custom.Vm());
  EXPECT_EQ(2U, callback_count);
  ASSERT_EQ(16U, ordered_rows.size());
  for (const auto& row : ordered_rows) {
    EXPECT_EQ(1, row[1].integer_value());
  }

  callback_count = 0;
  const BytecodeProgram zero_limit = LowerOrThrow(
      "SELECT id FROM items WHERE score=volatile_counter() LIMIT 0", catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(zero_limit, *pager, catalog->version().generation, custom.Vm()).empty());
  EXPECT_EQ(0U, callback_count);

  callback_count = 0;
  const BytecodeProgram rejected_guard = LowerOrThrow(
      "SELECT id FROM items "
      "WHERE stable_guard(1)=0 AND score=volatile_counter()",
      catalog, custom.Binder());
  EXPECT_TRUE(
      ExecuteRows(rejected_guard, *pager, catalog->version().generation, custom.Vm()).empty());
  EXPECT_EQ(0U, callback_count);
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
