#include "modern_sqlite/lowering/plan_lowering.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
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
#include "modern_sqlite/storage/btree/page.hpp"
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
                                           BindEnvironment environment = BindEnvironment::Core(),
                                           OptimizerOptions options = {}) {
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog, environment);
  if (!bound.has_value()) {
    throw std::runtime_error{bound.error().detail};
  }
  BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{logical.error().detail};
  }
  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical), options);
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
                                           BindEnvironment environment = BindEnvironment::Core(),
                                           OptimizerOptions options = {}) {
  const PhysicalPlan plan = OptimizeOrThrow(sql, catalog, environment, options);
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

[[nodiscard]] std::vector<IndexSeekMode> IndexSeekModes(const BytecodeProgram& program) {
  std::vector<IndexSeekMode> result;
  for (const Instruction& instruction : program.instructions()) {
    if (const auto* seek = std::get_if<SeekIndexInstruction>(&instruction); seek != nullptr) {
      result.push_back(seek->mode);
    }
  }
  return result;
}

[[nodiscard]] std::vector<IndexRangeEndMode> IndexRangeEndModes(const BytecodeProgram& program) {
  std::vector<IndexRangeEndMode> result;
  for (const Instruction& instruction : program.instructions()) {
    if (const auto* check = std::get_if<CheckIndexRangeInstruction>(&instruction);
        check != nullptr) {
      result.push_back(check->mode);
    }
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

[[nodiscard]] std::filesystem::path JoinedIndexFixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "joined_index" / "sqlite-3.54.0-joined-index.db";
}

[[nodiscard]] std::filesystem::path ReadCompatibilityFixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "read_compatibility" / "sqlite-3.54.0-read-compatibility.db";
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

[[nodiscard]] CatalogSnapshotPtr InitializeCorrelatedSourceDatabase(test::WritePagerFixedVfs& vfs) {
  InitializeMutationDatabase(vfs);
  const CatalogSnapshotPtr catalog = MutationCatalog();
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name) VALUES(?1,?2)", catalog);
  for (std::int64_t id = 1; id <= 3; ++id) {
    ByteBuffer payload{ByteCount{4096}};
    payload.mutable_view()[0] = static_cast<std::byte>(id);
    const std::array parameters{SqlValue::Integer(id), SqlValue::Blob(std::move(payload))};
    [[maybe_unused]] const auto inserted =
        TakeValue(ExecuteMutationProgram(insert, vfs, parameters));
  }
  [[maybe_unused]] const auto inserted = TakeValue(ExecuteMutationProgram(
      LowerMutationOrThrow("INSERT INTO Plain(Value) VALUES('left')", catalog), vfs));
  return catalog;
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
std::vector<std::string> callback_trace;

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

[[nodiscard]] Result<SqlValue> NumericRepresentative(const ScalarFunctionContext&,
                                                     std::span<const SqlValue>) {
  ++callback_count;
  return callback_count == 1U ? SqlValue::Integer(1) : SqlValue::Real(1.0);
}

[[nodiscard]] Result<SqlValue> TextRepresentative(const ScalarFunctionContext&,
                                                  std::span<const SqlValue>) {
  ++callback_count;
  if (callback_count == 1U) {
    return SqlValue::Text("A");
  }
  if (callback_count == 2U) {
    return SqlValue::Text("a");
  }
  return SqlValue::Text("B");
}

[[nodiscard]] Result<SqlValue> ReverseTextRepresentative(const ScalarFunctionContext&,
                                                         std::span<const SqlValue>) {
  ++callback_count;
  if (callback_count == 1U) {
    return SqlValue::Text("a");
  }
  if (callback_count == 2U) {
    return SqlValue::Text("A");
  }
  return SqlValue::Text("B");
}

[[nodiscard]] Result<SqlValue> FailCall(const ScalarFunctionContext&, std::span<const SqlValue>) {
  ++callback_count;
  return std::unexpected(Error::Create(ErrorCode::kGeneric, "failing function executed"));
}

[[nodiscard]] Result<SqlValue> DeterministicFailCall(const ScalarFunctionContext&,
                                                     std::span<const SqlValue>) {
  ++callback_count;
  return std::unexpected(
      Error::Create(ErrorCode::kGeneric, "deterministic failing function executed"));
}

[[nodiscard]] Result<SqlValue> FailAfterOne(const ScalarFunctionContext&,
                                            std::span<const SqlValue>) {
  ++callback_count;
  if (callback_count > 1U) {
    return std::unexpected(Error::Create(ErrorCode::kGeneric, "second function call failed"));
  }
  return SqlValue::Integer(1);
}

[[nodiscard]] Result<SqlValue> TraceFilter(const ScalarFunctionContext&,
                                           std::span<const SqlValue> arguments) {
  callback_trace.emplace_back("filter");
  return arguments.front().Clone();
}

[[nodiscard]] Result<SqlValue> TraceKey(const ScalarFunctionContext&,
                                        std::span<const SqlValue> arguments) {
  callback_trace.emplace_back("key");
  return arguments.front().Clone();
}

[[nodiscard]] Result<SqlValue> TraceArgument(const ScalarFunctionContext&,
                                             std::span<const SqlValue> arguments) {
  callback_trace.emplace_back("argument");
  return arguments.front().Clone();
}

struct CustomEnvironment {
  std::array<ScalarFunction, 12> functions{{
      ScalarFunction{"stable_guard", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ReturnOne},
      ScalarFunction{"volatile_key", FunctionArity::Exact(0),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     ReturnTwo},
      ScalarFunction{"volatile_counter", FunctionArity::Exact(0),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     CountCall},
      ScalarFunction{"numeric_representative", FunctionArity::Exact(0),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     NumericRepresentative},
      ScalarFunction{"text_representative", FunctionArity::Exact(0),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     TextRepresentative},
      ScalarFunction{"reverse_text_representative", FunctionArity::Exact(0),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     ReverseTextRepresentative},
      ScalarFunction{"failing", FunctionArity::Exact(0), FunctionDeterminism::kNonDeterministic,
                     FunctionCollationUse::kNone, FailCall},
      ScalarFunction{"deterministic_failing", FunctionArity::Exact(0),
                     FunctionDeterminism::kDeterministic, FunctionCollationUse::kNone,
                     DeterministicFailCall},
      ScalarFunction{"fail_after_one", FunctionArity::Exact(1),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     FailAfterOne},
      ScalarFunction{"trace_filter", FunctionArity::Exact(1),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     TraceFilter},
      ScalarFunction{"trace_key", FunctionArity::Exact(1), FunctionDeterminism::kNonDeterministic,
                     FunctionCollationUse::kNone, TraceKey},
      ScalarFunction{"trace_argument", FunctionArity::Exact(1),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     TraceArgument},
  }};
  FunctionRegistry registry{functions, CoreFunctionRegistry().aggregate_functions()};
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

class ReadLoweringAggregateIntegration : public ::testing::Test {
 protected:
  void SetUp() override { OpenFixture(FixturePath()); }

  void OpenFixture(const std::filesystem::path& path) {
    pager_ = TakeValue(Pager::Open(vfs_, path.string()));
    RequireStatus(pager_->BeginRead());
    catalog_ = TakeValue(LoadCatalog(*pager_, CatalogLoadOptions{.generation = 141}));
    UseTemporaryStorage(TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory});
    callback_count = 0;
  }

  [[nodiscard]] std::vector<std::vector<SqlValue>> Execute(
      std::string_view sql, BindEnvironment binding = BindEnvironment::Core(),
      VmEnvironment environment = VmEnvironment::Core(),
      std::span<const SqlValue> parameters = {}) {
    const BytecodeProgram program = LowerOrThrow(
        sql, catalog_, binding,
        OptimizerOptions{.allow_file_backed_automatic_indexes = temporary_->file_spill_enabled()});
    if (temporary_->file_spill_enabled()) {
      EXPECT_TRUE(std::ranges::any_of(program.relations(), [](const auto& relation) {
        return relation.relation_purpose == RelationPurpose::kLookup;
      })) << sql;
    }
    return ExecuteRows(program, *pager_, catalog_->version().generation, environment, &*temporary_,
                       parameters);
  }

  void UseTemporaryStorage(TemporaryStorageOptions options) {
    temporary_.emplace(TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_, options)));
  }

  [[nodiscard]] const CatalogSnapshotPtr& catalog() const noexcept { return catalog_; }

 private:
  PosixVfs vfs_;
  std::unique_ptr<Pager> pager_;
  CatalogSnapshotPtr catalog_;
  std::optional<TemporaryStorageFactory> temporary_;
};

class ReadLoweringJoin : public ReadLoweringAggregateIntegration {};

class ReadLoweringAutomaticJoin : public ReadLoweringAggregateIntegration {
 protected:
  void SetUp() override {
    OpenFixture(ReadCompatibilityFixturePath());
    UseTemporaryStorage(TemporaryStorageOptions{.mode = TemporaryStoreMode::kFile,
                                                .sorter_memory_threshold = ByteCount{1024}});
  }
};

TEST_F(ReadLoweringAutomaticJoin,
       PreservesAutomaticSourcesAcrossOrderingDistinctAndAggregateConsumers) {
  const auto ordered = Execute(
      "SELECT a.id,b.id,c.id,b.score,c.name FROM items a JOIN items b ON b.score IS a.score "
      "JOIN items c ON c.score IS b.score WHERE a.id<=3 ORDER BY a.id DESC LIMIT 2 OFFSET 1");
  ASSERT_EQ(2U, ordered.size());
  for (std::size_t row = 0; row < ordered.size(); ++row) {
    const auto id = static_cast<std::int64_t>(2U - row);
    for (std::size_t field = 0; field < 3U; ++field) {
      EXPECT_EQ(id, ordered[row][field].integer_value());
    }
    EXPECT_EQ(SqlValueType::kReal, ordered[row][3].type());
    EXPECT_EQ(row == 0U ? 3.0 : 1.5, ordered[row][3].real_value());
    EXPECT_EQ(row == 0U ? "beta" : "alpha", TextBytes(ordered[row][4]));
  }
  const auto distinct = Execute(
      "SELECT DISTINCT b.name FROM items a JOIN items b ON b.score IS a.score WHERE a.id<=3 "
      "UNION SELECT 'tail' ORDER BY 1");
  ASSERT_EQ(4U, distinct.size());
  const std::array<std::string_view, 4> names{"alpha", "beta", "gamma", "tail"};
  for (std::size_t row = 0; row < distinct.size(); ++row) {
    EXPECT_EQ(names[row], TextBytes(distinct[row][0]));
  }
  const auto grouped = Execute(
      "SELECT a.id,count(DISTINCT b.score),group_concat(c.name ORDER BY c.id) "
      "FROM items a LEFT JOIN items b ON b.score IS a.score "
      "JOIN items c ON c.score IS b.score WHERE a.id<=3 GROUP BY a.id ORDER BY a.id");
  ASSERT_EQ(3U, grouped.size());
  for (std::size_t row = 0; row < grouped.size(); ++row) {
    EXPECT_EQ(static_cast<std::int64_t>(row + 1U), grouped[row][0].integer_value());
    EXPECT_EQ(row == 2U ? 0 : 1, grouped[row][1].integer_value());
    EXPECT_EQ(names[row], TextBytes(grouped[row][2]));
  }
  const auto global = Execute(
      "SELECT count(b.id),sum(b.score) FROM items a JOIN items b ON b.score IS a.score "
      "WHERE a.id<=3");
  ASSERT_EQ(1U, global.size());
  EXPECT_EQ(3, global[0][0].integer_value());
  EXPECT_EQ(4.5, global[0][1].real_value());
}

TEST_F(ReadLoweringAutomaticJoin, KeepsAutomaticPrivateDemandAndUnenteredSourcesLazy) {
  const CustomEnvironment custom;
  const auto rows = Execute(
      "SELECT (SELECT b.name FROM items a JOIN items b ON b.score IS a.score "
      "WHERE a.id<=3 ORDER BY b.id DESC LIMIT 1 OFFSET 1),"
      "EXISTS(SELECT failing() FROM items a JOIN items b ON b.score IS a.score),"
      "coalesce(9,(SELECT failing() FROM items a JOIN items b ON b.score IS a.score))",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("beta", TextBytes(rows[0][0]));
  EXPECT_EQ(1, rows[0][1].integer_value());
  EXPECT_EQ(9, rows[0][2].integer_value());
  EXPECT_EQ(0U, callback_count);
  EXPECT_TRUE(Execute("SELECT failing() FROM items a JOIN items b ON b.score IS a.score "
                      "LIMIT 0 OFFSET failing()",
                      custom.Binder(), custom.Vm())
                  .empty());
  EXPECT_TRUE(Execute("SELECT failing() FROM items a JOIN items b ON b.score IS a.score "
                      "WHERE a.id<0 AND failing()",
                      custom.Binder(), custom.Vm())
                  .empty());
  EXPECT_EQ(0U, callback_count);
}

TEST(PlanLowering, UsesTypedReadRegionsForFlatNestedQueryCalls) {
  const BytecodeProgram program = LowerOrThrow(
      "SELECT (SELECT (SELECT id FROM items)),EXISTS(SELECT 1 FROM items)", TestCatalog());
  ASSERT_EQ(3U, program.subroutines().size());
  EXPECT_EQ(2U, program.cursors().size());
  EXPECT_EQ(2U, program.verification_metrics().maximum_subroutine_depth);
  std::size_t result_rows = 0;
  for (const Instruction& instruction : program.instructions()) {
    result_rows += std::holds_alternative<ResultRowInstruction>(instruction) ? 1U : 0U;
  }
  EXPECT_EQ(1U, result_rows);
  for (std::size_t index = 0; index < program.subroutines().size(); ++index) {
    const SubroutineDescriptor& subroutine = program.subroutines()[index];
    EXPECT_EQ(SubroutineKind::kRead, subroutine.kind);
    ASSERT_LT(subroutine.entry.value(), subroutine.end.value());
    for (std::uint32_t address = subroutine.entry.value(); address < subroutine.end.value();
         ++address) {
      EXPECT_FALSE(std::holds_alternative<HaltInstruction>(program.instructions()[address]));
      EXPECT_FALSE(std::holds_alternative<ResultRowInstruction>(program.instructions()[address]));
    }
    const auto* returned = std::get_if<ReturnSubroutineInstruction>(
        &program.instructions()[subroutine.end.value() - 1U]);
    ASSERT_NE(nullptr, returned);
    EXPECT_EQ(SubroutineId{static_cast<std::uint32_t>(index)}, returned->subroutine);
  }
}

TEST_F(ReadLoweringAggregateIntegration, LazilyCachesVolatileUncorrelatedQueriesPerExecution) {
  const CustomEnvironment custom;
  const auto rows =
      Execute("SELECT (SELECT volatile_counter()) FROM Items", custom.Binder(), custom.Vm());
  ASSERT_GT(rows.size(), 1U);
  EXPECT_EQ(1U, callback_count);
  for (const auto& row : rows) {
    ASSERT_EQ(1U, row.size());
    EXPECT_EQ(1, row[0].integer_value());
  }
  const auto next_execution =
      Execute("SELECT (SELECT volatile_counter()) FROM Items", custom.Binder(), custom.Vm());
  ASSERT_EQ(rows.size(), next_execution.size());
  EXPECT_EQ(2U, callback_count);
  for (const auto& row : next_execution) {
    EXPECT_EQ(2, row[0].integer_value());
  }
  callback_count = 0;
  const auto exists =
      Execute("SELECT EXISTS(SELECT failing() FROM Items)", custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, exists.size());
  EXPECT_EQ(1, exists[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);
  const auto lazy =
      Execute("SELECT coalesce(7,(SELECT failing())) FROM Items", custom.Binder(), custom.Vm());
  ASSERT_EQ(rows.size(), lazy.size());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, ReevaluatesConstantAliasContextWithoutSourceFields) {
  const CustomEnvironment custom;
  const auto rows = Execute("SELECT 1 AS x FROM Items WHERE (SELECT volatile_counter()+x)>0",
                            custom.Binder(), custom.Vm());
  ASSERT_GT(rows.size(), 1U);
  EXPECT_EQ(rows.size(), callback_count);
  for (const auto& row : rows) {
    ASSERT_EQ(1U, row.size());
    EXPECT_EQ(1, row[0].integer_value());
  }
}

TEST_F(ReadLoweringAggregateIntegration, PreservesAliasContextFrequencyAndSourceFreeCaching) {
  const CustomEnvironment custom;
  const std::size_t count = Execute("SELECT id FROM Items").size();
  struct Case {
    std::string_view sql;
    std::size_t calls;
    std::size_t rows;
  };
  for (const Case& test_case : {
           Case{"SELECT 1 AS x FROM Items ORDER BY (SELECT volatile_counter()+x)", count, count},
           Case{"SELECT 1 AS x FROM Items WHERE (SELECT (SELECT volatile_counter()+x))>0", count,
                count},
           Case{"SELECT 1 AS x FROM Items WHERE "
                "EXISTS(SELECT x FROM Items WHERE volatile_counter()>0 LIMIT 1)",
                count, count},
           Case{"SELECT 1 AS x FROM Items WHERE "
                "EXISTS(SELECT 1 FROM Items WHERE volatile_counter()>0 ORDER BY x LIMIT 1)",
                count, count},
           Case{"SELECT 1 AS x FROM Items WHERE "
                "EXISTS(SELECT 1 FROM Items WHERE volatile_counter()>0 LIMIT 1)",
                1, count},
           Case{"SELECT 1 AS x FROM Items a CROSS JOIN Items b "
                "WHERE (SELECT volatile_counter()+x)>0",
                count, count * count},
           Case{"SELECT 1 AS x FROM Items WHERE id=(SELECT volatile_counter()+x)", 1, 1},
       }) {
    SCOPED_TRACE(test_case.sql);
    callback_count = 0;
    const auto rows = Execute(test_case.sql, custom.Binder(), custom.Vm());
    ASSERT_EQ(test_case.rows, rows.size());
    EXPECT_EQ(test_case.calls, callback_count);
    for (const auto& row : rows) {
      ASSERT_EQ(1U, row.size());
      EXPECT_EQ(1, row[0].integer_value());
    }
  }
}

TEST_F(ReadLoweringAggregateIntegration, EvaluatesVolatileAliasesOnlyAtReachedSites) {
  const CustomEnvironment custom;
  const std::size_t count = Execute("SELECT id FROM Items").size();
  struct Case {
    std::string_view sql;
    std::size_t calls;
    std::size_t rows;
  };
  for (const Case& test_case : {
           Case{"SELECT volatile_counter() AS x FROM Items WHERE (SELECT x)>0", 2U * count, count},
           Case{"SELECT volatile_counter() AS x FROM Items WHERE (SELECT coalesce(1,x))=1", count,
                count},
           Case{"SELECT volatile_counter() AS x FROM Items WHERE (SELECT sum(x) FROM Items)>0",
                count * (count + 1U), count},
           Case{"SELECT (SELECT volatile_counter() AS x WHERE (SELECT x)>0) FROM Items", 2, count},
           Case{"SELECT failing() AS x FROM Items WHERE (SELECT coalesce(1,x))=0", 0, 0},
       }) {
    SCOPED_TRACE(test_case.sql);
    callback_count = 0;
    const auto rows = Execute(test_case.sql, custom.Binder(), custom.Vm());
    EXPECT_EQ(test_case.rows, rows.size());
    EXPECT_EQ(test_case.calls, callback_count);
  }
}

TEST(PlanLowering, KeepsAliasContextsInsideTheirNearestPrivateOwner) {
  struct Case {
    std::string_view sql;
    std::size_t private_queries;
  };
  for (const Case& test_case : {
           Case{"SELECT 1 AS x FROM Items WHERE (SELECT x)>0", 0},
           Case{"SELECT (SELECT 1 AS x WHERE (SELECT x)>0)", 1},
           Case{"SELECT 1 AS x FROM Items WHERE (SELECT (SELECT x))>0", 0},
       }) {
    SCOPED_TRACE(test_case.sql);
    const BytecodeProgram program = LowerOrThrow(test_case.sql, TestCatalog());
    EXPECT_EQ(test_case.private_queries, program.subroutines().size());
    EXPECT_EQ(test_case.private_queries, program.verification_metrics().maximum_subroutine_depth);
    EXPECT_FALSE(std::ranges::any_of(program.instructions(), [](const Instruction& instruction) {
      return std::holds_alternative<ReadFieldInstruction>(instruction);
    }));
  }
}

TEST_F(ReadLoweringAggregateIntegration, UsesCanonicalPrivatePointAccessAndStatementGuards) {
  for (const std::int64_t allowed : {0, 1}) {
    const std::array parameters{SqlValue::Integer(allowed)};
    const auto rows = Execute(
        "SELECT (SELECT id FROM Items WHERE ?1 AND id=1),"
        "EXISTS(SELECT Name FROM Items WHERE ?1)",
        BindEnvironment::Core(), VmEnvironment::Core(), parameters);
    ASSERT_EQ(1U, rows.size());
    ASSERT_EQ(2U, rows[0].size());
    if (allowed == 0) {
      EXPECT_EQ(SqlValueType::kNull, rows[0][0].type());
    } else {
      EXPECT_EQ(1, rows[0][0].integer_value());
    }
    EXPECT_EQ(allowed, rows[0][1].integer_value());
  }
}

TEST(PlanLowering, KeepsInlineCapturesInsideTheirNearestSourceFreeReadRegion) {
  struct Case {
    std::string_view sql;
    std::size_t private_queries;
  };
  for (const Case& test_case : {
           Case{"SELECT (SELECT (SELECT o.name)) FROM items o", 0},
           Case{"SELECT (SELECT (SELECT a.name) FROM items a)", 1},
           Case{"SELECT (SELECT (SELECT o.name)||(SELECT '!')) FROM items o", 1},
       }) {
    SCOPED_TRACE(test_case.sql);
    const BytecodeProgram program = LowerOrThrow(test_case.sql, TestCatalog());
    EXPECT_EQ(test_case.private_queries, program.subroutines().size());
    EXPECT_EQ(test_case.private_queries, program.verification_metrics().maximum_subroutine_depth);
    std::size_t output_rows = 0;
    for (const Instruction& instruction : program.instructions()) {
      output_rows += std::holds_alternative<ResultRowInstruction>(instruction) ? 1U : 0U;
    }
    EXPECT_EQ(1U, output_rows);
  }
}

TEST(PlanLowering, ReadsFullUsingCaptureBranchesOnlyAfterTheirNullGuard) {
  const BytecodeProgram program = LowerOrThrow(
      "SELECT (SELECT name) FROM items a FULL JOIN items b USING(name)", TestCatalog());
  EXPECT_TRUE(program.subroutines().empty());
  const auto instructions = program.instructions();
  bool guarded_source_read = false;
  for (std::size_t index = 2; index < instructions.size(); ++index) {
    const auto* left = std::get_if<ReadFieldInstruction>(&instructions[index - 2U]);
    const auto* guard = std::get_if<JumpIfInstruction>(&instructions[index - 1U]);
    const auto* right = std::get_if<ReadFieldInstruction>(&instructions[index]);
    if (left != nullptr && guard != nullptr && right != nullptr &&
        guard->condition == JumpCondition::kIfNotNull && guard->input == left->output &&
        right->output == left->output && right->cursor != left->cursor &&
        guard->target.value() > index) {
      guarded_source_read = true;
    }
  }
  EXPECT_TRUE(guarded_source_read);
}

TEST_F(ReadLoweringAggregateIntegration,
       ReevaluatesBindingCorrelationsButCachesSourceFreeChildren) {
  const CustomEnvironment custom;
  for (const std::string_view expression : {
           "(SELECT volatile_counter()+o.id)",
           "(SELECT volatile_counter() WHERE o.id IS NOT NULL)",
           "EXISTS(SELECT o.name WHERE volatile_counter()>0)",
           "EXISTS(SELECT 1 WHERE volatile_counter()>0 ORDER BY o.name)",
           "EXISTS(SELECT 1 WHERE volatile_counter()>0 ORDER BY (SELECT o.name))",
           "EXISTS(SELECT (SELECT o.name) WHERE volatile_counter()>0)",
       }) {
    SCOPED_TRACE(expression);
    callback_count = 0;
    const auto rows = Execute("SELECT o.id," + std::string{expression} + " FROM items o",
                              custom.Binder(), custom.Vm());
    ASSERT_GT(rows.size(), 1U);
    EXPECT_EQ(rows.size(), callback_count);
    for (std::size_t index = 0; index < rows.size(); ++index) {
      if (expression.starts_with("EXISTS")) {
        EXPECT_EQ(1, rows[index][1].integer_value());
      } else {
        const std::int64_t rowid = TakeOptional(rows[index][0].integer_value(), "expected rowid");
        const auto expected = static_cast<std::int64_t>(index + 1U) +
                              (expression.find("+o.id") != std::string_view::npos ? rowid : 0);
        EXPECT_EQ(expected, rows[index][1].integer_value());
      }
    }
  }
  callback_count = 0;
  const auto rows = Execute("SELECT o.id,(SELECT o.id+(SELECT volatile_counter())) FROM items o",
                            custom.Binder(), custom.Vm());
  ASSERT_GT(rows.size(), 1U);
  EXPECT_EQ(1U, callback_count);
  for (const auto& row : rows) {
    EXPECT_EQ(TakeOptional(row[0].integer_value(), "expected rowid") + 1, row[1].integer_value());
  }
}

TEST_F(ReadLoweringAggregateIntegration, ReemitsEachCorrelatedProducerAtLocalAliasSites) {
  const std::array<std::string_view, 20> bodies{
      "SELECT o.id",
      "VALUES(o.id),(o.id+1)",
      "SELECT i.id FROM items i WHERE i.id=o.id",
      "SELECT o.id FROM items i ORDER BY i.name DESC LIMIT ?1",
      "SELECT DISTINCT o.id FROM items i",
      "SELECT DISTINCT o.id FROM items i ORDER BY i.name",
      "SELECT o.id UNION SELECT o.id",
      "SELECT o.id EXCEPT SELECT -1",
      "SELECT o.id UNION ALL SELECT o.id ORDER BY 1 DESC",
      "SELECT max(i.id) FROM items i WHERE i.id=o.id",
      "SELECT i.id+count(*)-1 FROM items i WHERE i.id=o.id",
      "SELECT i.id FROM items i WHERE i.id=o.id GROUP BY i.id",
      "SELECT i.id FROM items i WHERE i.id=o.id GROUP BY i.name",
      "SELECT DISTINCT max(i.id) FROM items i WHERE i.id=o.id",
      "SELECT max(i.id) FROM items i WHERE i.id=o.id UNION ALL SELECT o.id",
      "SELECT max(i.id) FROM items i WHERE i.id=o.id EXCEPT SELECT -1",
      "SELECT o.id UNION SELECT max(i.id) FROM items i WHERE i.id=o.id",
      "SELECT b.id FROM items a JOIN items b ON b.id=a.id WHERE a.id=o.id",
      "SELECT max(b.id) FROM items a JOIN items b ON b.id=a.id WHERE a.id=o.id",
      "SELECT id FROM items a FULL JOIN items b USING(id) WHERE a.id=o.id",
  };
  const std::size_t expected_rows = Execute("SELECT id FROM items").size();
  for (const std::string_view body : bodies) {
    SCOPED_TRACE(body);
    const bool parameterized = body.find("?1") != std::string_view::npos;
    for (const std::int64_t limit : {1, -1}) {
      SCOPED_TRACE(limit);
      const std::string sql =
          "SELECT o.id,(" + std::string{body} + ") AS x FROM items o WHERE x IS o.id ORDER BY o.id";
      const std::array parameters{SqlValue::Integer(limit)};
      const auto rows = Execute(
          sql, BindEnvironment::Core(), VmEnvironment::Core(),
          parameterized ? std::span<const SqlValue>{parameters} : std::span<const SqlValue>{});
      ASSERT_EQ(expected_rows, rows.size());
      for (const auto& row : rows) {
        ASSERT_EQ(2U, row.size());
        EXPECT_EQ(row[0].integer_value(), row[1].integer_value());
      }
      if (!parameterized) {
        break;
      }
    }
  }
}

TEST(PlanLowering, PreservesLazyCapturedFieldsAndNativeAggregateSnapshotPhases) {
  test::WritePagerFixedVfs vfs{false};
  const CatalogSnapshotPtr catalog = InitializeCorrelatedSourceDatabase(vfs);
  auto pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const auto temporary = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  struct Case {
    std::string_view sql;
    std::size_t rows = 0;
    bool too_large = false;
    std::optional<std::int64_t> integer{};
    std::optional<std::string_view> text{};
  };
  const std::array cases{
      Case{.sql = "SELECT (SELECT coalesce(1,o.Name)) FROM Items o", .rows = 3, .integer = 1},
      Case{.sql = "SELECT (SELECT coalesce(NULL,o.Name)) FROM Items o", .too_large = true},
      Case{.sql = "SELECT (SELECT iif(1,1,o.Name)) FROM Items o", .rows = 3, .integer = 1},
      Case{.sql = "SELECT (SELECT iif(0,1,o.Name)) FROM Items o", .too_large = true},
      Case{.sql = "SELECT (SELECT o.Name WHERE 0) FROM Items o", .rows = 3},
      Case{.sql = "SELECT (SELECT o.Name FROM Plain WHERE 0) FROM Items o", .rows = 3},
      Case{.sql = "SELECT (SELECT o.Name LIMIT 0) FROM Items o", .rows = 3},
      Case{.sql = "SELECT EXISTS(SELECT o.Name) FROM Items o", .rows = 3, .integer = 1},
      Case{.sql = "SELECT EXISTS(SELECT 1 ORDER BY o.Name) FROM Items o", .rows = 3, .integer = 1},
      Case{.sql = "SELECT (SELECT coalesce(t.Value,o.Name)) FROM Plain t FULL JOIN Items o "
                  "ON t.rowid=o.id WHERE o.id=1",
           .rows = 1,
           .text = "left"},
      Case{.sql = "SELECT (SELECT coalesce(t.Value,o.Name)) FROM Plain t FULL JOIN Items o "
                  "ON t.rowid=o.id WHERE o.id=2",
           .too_large = true},
      Case{.sql = "SELECT count(*),(SELECT coalesce(1,o.Name)) FROM Items o", .too_large = true},
      Case{.sql = "SELECT count(*),(SELECT coalesce(1,o.Name)) FROM Items o GROUP BY o.id%2",
           .too_large = true},
      Case{.sql = "SELECT sum((SELECT coalesce(1,o.Name))) FROM Items o", .rows = 1, .integer = 3},
      Case{.sql = "SELECT sum((SELECT coalesce(1,o.Name))) FROM Items o GROUP BY o.id%2",
           .too_large = true},
      Case{.sql = "SELECT count(*),(SELECT o.Name LIMIT 0) FROM Items o", .too_large = true},
      Case{.sql = "SELECT count(*),(SELECT o.Name EXCEPT SELECT NULL UNION SELECT 1) FROM Items o",
           .too_large = true},
      Case{.sql = "SELECT count(*),EXISTS(SELECT o.Name) FROM Items o", .too_large = true},
      Case{.sql = "SELECT count(*),EXISTS(SELECT o.Name) FROM Items o GROUP BY o.id%2",
           .too_large = true},
      Case{.sql = "SELECT count(*),EXISTS(SELECT 1 ORDER BY o.Name) FROM Items o",
           .too_large = true},
      Case{.sql = "SELECT sum(EXISTS(SELECT o.Name)) FROM Items o", .rows = 1, .integer = 3},
      Case{.sql = "SELECT sum(EXISTS(SELECT o.Name)) FROM Items o GROUP BY o.id%2",
           .too_large = true},
      Case{.sql = "SELECT count(*),EXISTS(SELECT (SELECT o.Name)) FROM Items o", .too_large = true},
      Case{.sql = "SELECT count(*),EXISTS(SELECT o.Name UNION SELECT 1) FROM Items o",
           .too_large = true},
      Case{.sql = "SELECT min(o.id),EXISTS(SELECT o.Name) FROM Items o", .too_large = true},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    const BytecodeProgram program = LowerOrThrow(test_case.sql, catalog);
    Vm vm =
        TakeValue(Vm::Create(program, VmEnvironment::Core(), VmLimits{.maximum_value_bytes = 256}));
    RequireStatus(vm.AttachExecutionContext(
        VmExecutionContext{*pager, catalog->version().generation, temporary}));
    for (std::size_t index = 0; index < test_case.rows; ++index) {
      ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
      ASSERT_EQ(1U, vm.row().size());
      if (test_case.integer.has_value()) {
        EXPECT_EQ(SqlValueType::kInteger, vm.row()[0].type());
        EXPECT_EQ(*test_case.integer, vm.row()[0].integer_value());
      } else if (test_case.text.has_value()) {
        EXPECT_EQ(*test_case.text, TextBytes(vm.row()[0]));
      } else {
        EXPECT_EQ(SqlValueType::kNull, vm.row()[0].type());
      }
    }
    const auto terminal = vm.Step();
    if (test_case.too_large) {
      ASSERT_FALSE(terminal.has_value());
      EXPECT_EQ(ErrorCode::kTooLarge, terminal.error().code());
      EXPECT_EQ(VmState::kError, vm.state());
      EXPECT_TRUE(vm.row().empty());
    } else {
      ASSERT_TRUE(terminal.has_value()) << terminal.error().ToString();
      EXPECT_EQ(VmStep::kDone, *terminal);
    }
    RequireStatus(vm.Reset());
  }
  RequireStatus(pager->EndRead());
}

TEST(PlanLowering, OwnsCapturedOverflowValuesBeyondOnePageCacheAndSourceLifetimes) {
  test::WritePagerFixedVfs vfs{false};
  const CatalogSnapshotPtr catalog = InitializeCorrelatedSourceDatabase(vfs);
  struct Case {
    std::string_view sql;
    std::size_t fields;
  };
  for (const Case& test_case : {
           Case{"SELECT o.id,(SELECT o.Name),(SELECT (SELECT o.Name)),"
                "(SELECT i.Name FROM Items i WHERE i.id=o.id) FROM Items o ORDER BY o.id",
                4},
           Case{"SELECT o.id,o.Name AS payload FROM Items o "
                "WHERE (SELECT (SELECT length(payload)))=4096 ORDER BY o.id",
                2},
       }) {
    SCOPED_TRACE(test_case.sql);
    auto pager = test::OpenWritePager(vfs, 1U);
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    const auto temporary = TakeValue(TemporaryStorageFactory::Create(
        vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
    const BytecodeProgram program = LowerOrThrow(test_case.sql, catalog);
    const auto rows = ExecuteRows(program, *pager, catalog->version().generation,
                                  VmEnvironment::Core(), &temporary);
    RequireStatus(pager->EndRead());
    pager.reset();
    ASSERT_EQ(3U, rows.size());
    for (std::size_t row = 0; row < rows.size(); ++row) {
      ASSERT_EQ(test_case.fields, rows[row].size());
      for (std::size_t field = 1; field < test_case.fields; ++field) {
        const ByteView blob = BlobBytes(rows[row][field]);
        ASSERT_EQ(4096U, blob.size());
        EXPECT_EQ(static_cast<std::byte>(row + 1U), blob[0]);
        EXPECT_TRUE(std::ranges::equal(BlobBytes(rows[row][1]), blob));
      }
    }
  }
}

TEST(PlanLowering, SkipsUnreachedOuterOverflowReadsAndReportsTakenIoAndCorruption) {
  test::WritePagerFixedVfs vfs{false};
  const CatalogSnapshotPtr catalog = InitializeCorrelatedSourceDatabase(vfs);
  const ByteBuffer valid = ByteBuffer::CopyOf(vfs.database_bytes());
  ByteBuffer corrupt = valid.Clone();
  const auto geometry = TakeValue(BtreePageGeometry::Create(ByteCount{test::kWritePagerPageSize},
                                                            ByteCount{test::kWritePagerPageSize}));
  const auto page = TakeValue(BtreePageView::Parse(
      valid.view().subspan(test::kWritePagerPageSize, test::kWritePagerPageSize), PageNumber{2},
      geometry));
  ASSERT_EQ(BtreePageType::kLeafTable, page.type());
  const auto cell = TakeValue(page.cell(0));
  ASSERT_TRUE(cell.first_overflow_page().has_value());
  const auto overflow_offset = static_cast<std::size_t>(cell.first_overflow_page()->value() - 1U) *
                               test::kWritePagerPageSize;
  std::ranges::fill(corrupt.mutable_view().subspan(overflow_offset, 4U), std::byte{0});
  const std::array<std::string_view, 5> queries{
      "SELECT (SELECT coalesce(?1,o.Name)) FROM Items o",
      "SELECT (SELECT iif(?1 IS NULL,o.Name,1)) FROM Items o",
      "SELECT (SELECT coalesce(t.Value,o.Name)) FROM Plain t FULL JOIN Items o "
      "ON o.id=t.rowid-?1 WHERE o.id=1",
      "SELECT EXISTS(SELECT o.Name AS x FROM Items o WHERE "
      "(SELECT coalesce(?1,x)) IS 1)",
      "SELECT EXISTS(SELECT o.Name AS x FROM Items o WHERE "
      "(SELECT iif(?1 IS NULL,x,1)) IS 1)",
  };
  for (std::size_t query = 0; query < queries.size(); ++query) {
    for (const bool corrupt_input : {false, true}) {
      for (const bool taken : {false, true}) {
        const bool full_join = query == 2U;
        const bool existence = queries[query].starts_with("SELECT EXISTS(");
        SCOPED_TRACE(queries[query]);
        SCOPED_TRACE(::testing::Message{} << "corrupt=" << corrupt_input << " taken=" << taken);
        vfs.LoadDatabase(corrupt_input ? corrupt.view() : valid.view());
        auto pager = test::OpenWritePager(vfs, 64U);
        ASSERT_NE(nullptr, pager);
        RequireStatus(pager->BeginRead());
        const auto temporary = TakeValue(TemporaryStorageFactory::Create(
            vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
        const BytecodeProgram program = LowerOrThrow(queries[query], catalog);
        Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
        RequireStatus(vm.Bind(ParameterId{0}, full_join ? SqlValue::Integer(taken ? 1 : 0)
                                              : taken   ? SqlValue{}
                                                        : SqlValue::Integer(1)));
        RequireStatus(vm.AttachExecutionContext(
            VmExecutionContext{*pager, catalog->version().generation, temporary}));
        if (!corrupt_input) {
          vfs.FailDatabaseReadAfter(full_join ? 2U : 1U, ErrorCode::kIo);
        }
        if (taken) {
          const auto failed = vm.Step();
          ASSERT_FALSE(failed.has_value());
          EXPECT_EQ(corrupt_input ? ErrorCode::kCorruption : ErrorCode::kIo, failed.error().code());
          EXPECT_TRUE(vm.row().empty());
          EXPECT_EQ(VmState::kError, vm.state());
        } else {
          for (std::size_t row = 0; row < (full_join || existence ? 1U : 3U); ++row) {
            ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
            if (full_join) {
              EXPECT_EQ("left", TextBytes(vm.row()[0]));
            } else {
              EXPECT_EQ(1, vm.row()[0].integer_value());
            }
          }
          EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
        }
        RequireStatus(vm.Reset());
        RequireStatus(pager->EndRead());
      }
    }
  }
}

TEST_F(ReadLoweringAggregateIntegration, ExecutesDeepFlatQueryCallGraphs) {
  for (const std::uint32_t depth : {1U, 8U, 43U}) {
    SCOPED_TRACE(depth);
    std::string sql = "SELECT 7";
    for (std::uint32_t index = 0; index < depth; ++index) {
      sql = "SELECT (" + sql + ")";
    }
    const BytecodeProgram program = LowerOrThrow(sql, catalog());
    EXPECT_EQ(depth, program.subroutines().size());
    EXPECT_EQ(depth, program.verification_metrics().maximum_subroutine_depth);
    const auto rows = Execute(sql);
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ(7, rows[0][0].integer_value());
  }
  for (const std::uint32_t depth : {1U, 8U, 42U}) {
    SCOPED_TRACE(depth);
    std::string sql = "o.id";
    for (std::uint32_t index = 0; index < depth; ++index) {
      sql = "(SELECT " + sql + ")";
    }
    sql = "SELECT o.id," + sql + " FROM items o";
    const BytecodeProgram correlated = LowerOrThrow(sql, catalog());
    EXPECT_TRUE(correlated.subroutines().empty());
    const auto correlated_rows = Execute(sql);
    ASSERT_GT(correlated_rows.size(), 1U);
    for (const auto& row : correlated_rows) {
      EXPECT_EQ(row[0].integer_value(), row[1].integer_value());
    }
  }
}

TEST_F(ReadLoweringAggregateIntegration, PreservesThePinnedOuterAliasDepthLimit) {
  for (const std::string_view definition : {"1", "o.id"}) {
    std::string expression = "x";
    for (std::size_t depth = 0; depth < 43U; ++depth) {
      expression = "(SELECT " + expression + ")";
    }
    const std::string prefix = "SELECT " + std::string{definition} + " AS x FROM Items o ORDER BY ";
    const auto rows = Execute(prefix + expression);
    ASSERT_GT(rows.size(), 1U);
    for (std::size_t row = 0; row < rows.size(); ++row) {
      EXPECT_EQ(definition == "1" ? 1 : static_cast<std::int64_t>(row + 1U),
                rows[row][0].integer_value());
    }
    const auto rejected =
        BindSelectStatement(ParseTree(prefix + "(SELECT " + expression + ")"), catalog());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(BindErrorCode::kExpressionDepthExceeded, rejected.error().code);
  }
}

TEST(PlanLowering, RecoversCorrelatedQueryIoFailureWithAParentCursorOpen) {
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);
  const auto catalog = MutationCatalog();
  ASSERT_TRUE(
      ExecuteMutationProgram(
          LowerMutationOrThrow("INSERT INTO Items(id,Name) VALUES(9,'inline-')", catalog), vfs)
          .has_value());
  ASSERT_TRUE(ExecuteMutationProgram(
                  LowerMutationOrThrow("INSERT INTO Plain(Value) VALUES('parent')", catalog), vfs)
                  .has_value());
  for (const std::string_view sql : {
           "SELECT p.Value,(SELECT Name||p.Value FROM Items WHERE id=p.rowid+8) FROM Plain p",
           "SELECT p.Value AS suffix,(SELECT Name||p.Value FROM Items WHERE id=p.rowid+8) "
           "FROM Plain p WHERE "
           "(SELECT Name||suffix FROM Items WHERE id=p.rowid+8) IS NOT NULL",
       }) {
    SCOPED_TRACE(sql);
    auto pager = test::OpenWritePager(vfs, 64U);
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    const BytecodeProgram program = LowerOrThrow(sql, catalog);
    EXPECT_TRUE(program.subroutines().empty());
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(
        vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
    vfs.FailDatabaseReadAfter(1U, ErrorCode::kIo);
    const auto failed = vm.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_EQ(VmState::kError, vm.state());
    EXPECT_TRUE(vm.row().empty());
    RequireStatus(vm.Reset());
    RequireStatus(
        vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    EXPECT_EQ("parent", TextBytes(vm.row()[0]));
    EXPECT_EQ("inline-parent", TextBytes(vm.row()[1]));
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    RequireStatus(pager->EndRead());
  }
}

TEST(PlanLowering, RecoversPrivateQueryIoFailureWithAParentCursorOpen) {
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);
  const auto catalog = MutationCatalog();
  ASSERT_TRUE(
      ExecuteMutationProgram(
          LowerMutationOrThrow("INSERT INTO Items(id,Name) VALUES(9,'private')", catalog), vfs)
          .has_value());
  ASSERT_TRUE(ExecuteMutationProgram(
                  LowerMutationOrThrow("INSERT INTO Plain(Value) VALUES('parent')", catalog), vfs)
                  .has_value());
  auto pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const BytecodeProgram program =
      LowerOrThrow("SELECT Value,(SELECT Name FROM Items WHERE id=9) FROM Plain", catalog);
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  vfs.FailDatabaseReadAfter(1U, ErrorCode::kIo);
  const auto failed = vm.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_EQ(VmState::kError, vm.state());
  EXPECT_TRUE(vm.row().empty());
  RequireStatus(vm.Reset());
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  ASSERT_EQ(2U, vm.row().size());
  EXPECT_EQ("parent", TextBytes(vm.row()[0]));
  EXPECT_EQ("private", TextBytes(vm.row()[1]));
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(pager->EndRead());
}

TEST(PlanLowering, RecoversPrivateAndInlineAggregatePmaWriteFailures) {
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);
  const auto catalog = MutationCatalog();
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name) VALUES(?1,?2)", catalog);
  for (std::int64_t id = 1; id <= 32; ++id) {
    const std::array parameters{SqlValue::Integer(id), SqlValue::Text("row" + std::to_string(id))};
    ASSERT_TRUE(ExecuteMutationProgram(insert, vfs, parameters).has_value());
  }
  ASSERT_TRUE(ExecuteMutationProgram(
                  LowerMutationOrThrow("INSERT INTO Plain(Value) VALUES('parent')", catalog), vfs)
                  .has_value());
  auto pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const auto factory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager,
      TemporaryStorageOptions{.mode = TemporaryStoreMode::kFile,
                              .sorter_memory_threshold = ByteCount{32U << 10U}}));
  for (const std::string_view sql : {
           "SELECT (SELECT group_concat(a.Name ORDER BY b.id,a.id) "
           "FROM Items a CROSS JOIN Items b)",
           "SELECT (SELECT group_concat(a.Name||p.Value ORDER BY b.id,a.id) "
           "FROM Items a CROSS JOIN Items b) FROM Plain p",
           "SELECT p.Value AS suffix FROM Plain p WHERE "
           "(SELECT group_concat(a.Name||suffix ORDER BY b.id,a.id) "
           "FROM Items a CROSS JOIN Items b) IS NOT NULL",
       }) {
    SCOPED_TRACE(sql);
    const BytecodeProgram program = LowerOrThrow(sql, catalog);
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(vm.AttachExecutionContext(
        VmExecutionContext{*pager, catalog->version().generation, factory}));
    vfs.FailPathlessWriteAfter(0U, ErrorCode::kIo);
    const auto failed = vm.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_EQ(VmState::kError, vm.state());
    EXPECT_GT(vfs.pathless_open_count(), 0U);
    EXPECT_FALSE(vfs.pathless_file_present());
    RequireStatus(vm.Reset());
    RequireStatus(vm.AttachExecutionContext(
        VmExecutionContext{*pager, catalog->version().generation, factory}));
    ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    ASSERT_EQ(1U, vm.row().size());
    EXPECT_FALSE(TextBytes(vm.row()[0]).empty());
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    EXPECT_FALSE(vfs.pathless_file_present());
  }
  RequireStatus(pager->EndRead());
}

TEST_F(ReadLoweringJoin, PreservesEarliestReadyPredicateCallSchedules) {
  const CustomEnvironment custom;
  struct Case {
    std::string_view predicate;
    std::size_t calls;
  };
  const std::array cases{
      Case{.predicate = "volatile_counter()", .calls = 2U},
      Case{.predicate = "trace_filter(a.id)", .calls = 2U},
      Case{.predicate = "trace_filter(b.id)", .calls = 6U},
      Case{.predicate = "trace_filter(a.id+b.id)", .calls = 6U},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.predicate);
    callback_count = 0;
    callback_trace.clear();
    const std::string sql =
        "SELECT a.id,b.id FROM items a CROSS JOIN items b "
        "WHERE a.id<=2 AND b.id<=3 AND " +
        std::string{test_case.predicate};
    const auto rows = Execute(sql, custom.Binder(), custom.Vm());
    ASSERT_EQ(6U, rows.size());
    EXPECT_EQ(test_case.calls, callback_count + callback_trace.size());
  }
}

TEST_F(ReadLoweringJoin, SkipsProjectionAndLaterWorkForLimitOffsetAndRejectedPrefixes) {
  const CustomEnvironment custom;
  callback_count = 0;
  const auto rows = Execute(
      "SELECT volatile_counter(),a.id,b.id FROM items a CROSS JOIN items b LIMIT 1 OFFSET 2",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1U, callback_count);
  EXPECT_EQ(1, rows[0][1].integer_value());
  EXPECT_EQ(3, rows[0][2].integer_value());
  EXPECT_TRUE(
      Execute("SELECT failing() FROM items a CROSS JOIN items b WHERE a.id>99 AND failing()",
              custom.Binder(), custom.Vm())
          .empty());
  EXPECT_TRUE(Execute("SELECT failing() FROM items a CROSS JOIN items b LIMIT 0 OFFSET failing()",
                      custom.Binder(), custom.Vm())
                  .empty());
  EXPECT_EQ(1U, callback_count);
}

TEST_F(ReadLoweringJoin, SendsJoinedRowsThroughExistingOrderMembershipAndSetDestinations) {
  const auto distinct = Execute(
      "SELECT DISTINCT b.id FROM items a CROSS JOIN items b ORDER BY 1 DESC LIMIT 2 OFFSET 1");
  ASSERT_EQ(2U, distinct.size());
  EXPECT_EQ(2, distinct[0][0].integer_value());
  EXPECT_EQ(1, distinct[1][0].integer_value());
  const auto united = Execute(
      "SELECT a.id+b.id FROM items a CROSS JOIN items b WHERE a.id=1 "
      "UNION SELECT 99 ORDER BY 1");
  ASSERT_EQ(4U, united.size());
  for (std::size_t index = 0; index < 3U; ++index) {
    EXPECT_EQ(static_cast<std::int64_t>(index + 2U), united[index][0].integer_value());
  }
  EXPECT_EQ(99, united[3][0].integer_value());
  const auto except = Execute(
      "SELECT b.id FROM items a CROSS JOIN items b "
      "EXCEPT SELECT 2 ORDER BY 1");
  ASSERT_EQ(2U, except.size());
  EXPECT_EQ(1, except[0][0].integer_value());
  EXPECT_EQ(3, except[1][0].integer_value());
}

TEST_F(ReadLoweringJoin, StopsDemandOneWithoutAnUnreachableInnerAdvance) {
  const CustomEnvironment custom;
  callback_count = 0;
  const auto rows = Execute(
      "SELECT a.id FROM items a CROSS JOIN items b "
      "UNION SELECT failing() LIMIT 1",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);
  const auto filtered = Execute(
      "SELECT b.id FROM items a CROSS JOIN items b WHERE b.id>2 "
      "UNION SELECT failing() LIMIT 1",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, filtered.size());
  EXPECT_EQ(3, filtered[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringJoin, EmitsThreeOwnedSourcesWithoutRowIdOrFieldAliasing) {
  const auto rows = Execute(
      "SELECT a.oid,b._rowid_,c.id FROM items a CROSS JOIN items b,items c "
      "WHERE a.id=1 AND b.id<=2 AND c.id<=3 ORDER BY 1,2,3");
  ASSERT_EQ(6U, rows.size());
  for (std::size_t index = 0; index < rows.size(); ++index) {
    EXPECT_EQ(1, rows[index][0].integer_value());
    EXPECT_EQ(static_cast<std::int64_t>(index / 3U + 1U), rows[index][1].integer_value());
    EXPECT_EQ(static_cast<std::int64_t>(index % 3U + 1U), rows[index][2].integer_value());
  }
}

TEST_F(ReadLoweringJoin, RetainsSeparateRowIdsAndPrimaryKeyAliasesForJoinedRepresentatives) {
  const auto global = Execute(
      "SELECT a.id,a._rowid_,b.id,b._rowid_,max(b.id*10-a.id) "
      "FROM items a CROSS JOIN items b");
  ASSERT_EQ(1U, global.size());
  const std::array<std::int64_t, 5> expected{1, 1, 3, 3, 29};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(expected[index], global[0][index].integer_value());
  }
  const auto grouped = Execute(
      "SELECT a.id,a._rowid_,b.id,b._rowid_,max(a.id) "
      "FROM items a CROSS JOIN items b GROUP BY b.id ORDER BY b.id");
  ASSERT_EQ(3U, grouped.size());
  for (std::size_t index = 0; index < grouped.size(); ++index) {
    EXPECT_EQ(3, grouped[index][0].integer_value());
    EXPECT_EQ(3, grouped[index][1].integer_value());
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), grouped[index][2].integer_value());
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), grouped[index][3].integer_value());
    EXPECT_EQ(3, grouped[index][4].integer_value());
  }
}

TEST_F(ReadLoweringJoin, FinalizesEmptyJoinedGlobalsButProducesNoEmptyGroups) {
  const auto empty = Execute(
      "SELECT count(),sum(a.id),total(b.id),min(a.id) "
      "FROM items a CROSS JOIN items b WHERE a.id>99");
  ASSERT_EQ(1U, empty.size());
  EXPECT_EQ(0, empty[0][0].integer_value());
  EXPECT_EQ(SqlValueType::kNull, empty[0][1].type());
  EXPECT_EQ(0.0, empty[0][2].real_value());
  EXPECT_EQ(SqlValueType::kNull, empty[0][3].type());
  EXPECT_TRUE(Execute("SELECT b.id,count() FROM items a CROSS JOIN items b WHERE 0 "
                      "GROUP BY b.id ORDER BY b.id")
                  .empty());
  const auto rejected = Execute("SELECT count() FROM items a CROSS JOIN items b WHERE 0");
  ASSERT_EQ(1U, rejected.size());
  EXPECT_EQ(0, rejected[0][0].integer_value());
}

TEST_F(ReadLoweringJoin, ReusesJoinedModifierGroupingAndCompletedRowConsumers) {
  const auto rows = Execute(
      "SELECT b.id,count(DISTINCT a.id) FILTER (WHERE a.id>1),"
      "group_concat(a.id ORDER BY a.id DESC) "
      "FROM items a CROSS JOIN items b GROUP BY b.id HAVING count()>1 "
      "ORDER BY b.id DESC LIMIT 2 OFFSET 1");
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ(2, rows[0][0].integer_value());
  EXPECT_EQ(1, rows[1][0].integer_value());
  for (const auto& row : rows) {
    EXPECT_EQ(2, row[1].integer_value());
    EXPECT_EQ("3,2,1", TextBytes(row[2]));
  }
}

TEST_F(ReadLoweringJoin, RoutesJoinedAggregateSetAndDemandOneArmsThroughTheirLifecycle) {
  for (const std::string_view sql : {
           "SELECT count() FROM items a CROSS JOIN items b UNION SELECT 99 ORDER BY 1",
           "SELECT 99 UNION SELECT count() FROM items a CROSS JOIN items b ORDER BY 1",
       }) {
    SCOPED_TRACE(sql);
    const auto rows = Execute(sql);
    ASSERT_EQ(2U, rows.size());
    EXPECT_EQ(9, rows[0][0].integer_value());
    EXPECT_EQ(99, rows[1][0].integer_value());
  }
  const auto grouped = Execute(
      "SELECT a.id FROM items a CROSS JOIN items b GROUP BY a.id "
      "UNION SELECT 99 ORDER BY 1");
  ASSERT_EQ(4U, grouped.size());
  const CustomEnvironment custom;
  callback_count = 0;
  for (const std::string_view sql : {
           "SELECT count() FROM items a CROSS JOIN items b UNION SELECT failing() LIMIT 1",
           "SELECT max(a.id) FROM items a CROSS JOIN items b GROUP BY b.id "
           "UNION SELECT failing() LIMIT 1",
       }) {
    SCOPED_TRACE(sql);
    const auto rows = Execute(sql, custom.Binder(), custom.Vm());
    ASSERT_EQ(1U, rows.size());
  }
  const auto reverse =
      Execute("SELECT 99 UNION SELECT count() FROM items a CROSS JOIN items b LIMIT 1");
  ASSERT_EQ(1U, reverse.size());
  EXPECT_EQ(99, reverse[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringJoin, KeepsAggregateFiltersAndOrderingAtCompleteJoinedRowPhase) {
  const CustomEnvironment custom;
  callback_trace.clear();
  const auto rows = Execute(
      "SELECT group_concat(trace_argument(a.id) ORDER BY trace_key(b.id)) "
      "FILTER (WHERE trace_filter(a.id)>1) FROM items a CROSS JOIN items b",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(9U, std::ranges::count(callback_trace, "filter"));
  EXPECT_EQ(6U, std::ranges::count(callback_trace, "argument"));
  EXPECT_EQ(6U, std::ranges::count(callback_trace, "key"));
  callback_count = 0;
  EXPECT_TRUE(
      Execute("SELECT count(failing()) FROM items a CROSS JOIN items b LIMIT 0 OFFSET failing()",
              custom.Binder(), custom.Vm())
          .empty());
  const auto empty = Execute("SELECT count(failing()) FROM items a CROSS JOIN items b WHERE 0",
                             custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, empty.size());
  EXPECT_EQ(0, empty[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringJoin, ExecutesLeftChainsAndNeverExtendsWhereRejectedMatches) {
  const auto chain = Execute(
      "SELECT a.id,b.id,c.id FROM items a LEFT JOIN items b ON a.id=b.id AND b.id<2 "
      "LEFT JOIN items c ON b.id=c.id ORDER BY a.id");
  ASSERT_EQ(3U, chain.size());
  for (std::size_t index = 0; index < chain.size(); ++index) {
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), chain[index][0].integer_value());
    if (index == 0U) {
      EXPECT_EQ(1, chain[index][1].integer_value());
      EXPECT_EQ(1, chain[index][2].integer_value());
    } else {
      EXPECT_EQ(SqlValueType::kNull, chain[index][1].type());
      EXPECT_EQ(SqlValueType::kNull, chain[index][2].type());
    }
  }
  EXPECT_TRUE(
      Execute("SELECT a.id FROM items a LEFT JOIN items b ON a.id=b.id WHERE b.id>99").empty());
  const auto unmatched = Execute(
      "SELECT a.id,b.id FROM items a LEFT JOIN items b ON 0 WHERE b.id IS NULL ORDER BY a.id");
  ASSERT_EQ(3U, unmatched.size());
  for (const auto& row : unmatched) {
    EXPECT_EQ(SqlValueType::kNull, row[1].type());
  }
}

TEST_F(ReadLoweringJoin, ReplaysRightRowsAfterAnUnmatchedLeftPrefix) {
  const auto rows = Execute(
      "SELECT a.id,b.id,c.id FROM items a LEFT JOIN items b ON 0 "
      "RIGHT JOIN items c ON b.id=c.id ORDER BY c.id");
  ASSERT_EQ(3U, rows.size());
  for (std::size_t index = 0; index < rows.size(); ++index) {
    EXPECT_EQ(SqlValueType::kNull, rows[index][0].type());
    EXPECT_EQ(SqlValueType::kNull, rows[index][1].type());
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), rows[index][2].integer_value());
  }
}

TEST_F(ReadLoweringJoin, SharesOneReadableBodyAcrossSixtyFourRightReplays) {
  for (const std::size_t sources : {2U, 8U, 32U, 64U}) {
    SCOPED_TRACE(sources);
    std::string sql = "SELECT 1 FROM Items t0";
    for (std::size_t index = 1; index < sources; ++index) {
      sql += " RIGHT JOIN Items t" + std::to_string(index) + " ON 0";
    }
    sql += " LIMIT 0";
    const BytecodeProgram program = LowerOrThrow(sql, catalog());
    const auto kinds = InstructionKinds(program);
    EXPECT_EQ(sources,
              static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kOpenRead)));
    EXPECT_EQ(sources - 1U,
              static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kOpenRelation)));
    EXPECT_EQ(1, std::ranges::count(kinds, InstructionKind::kResultRow));
    EXPECT_LE(program.instructions().size(), sources * sources * 2U + sources * 64U + 32U);
    RecordProperty("instructions_for_" + std::to_string(sources) + "_right_sources",
                   std::to_string(program.instructions().size()));
  }
}

TEST_F(ReadLoweringJoin, ComposesFullExtensionsThroughSixtyFourSourcesWithoutCopyingConsumers) {
  for (const std::size_t sources : {2U, 8U, 64U}) {
    SCOPED_TRACE(sources);
    std::string sql = "SELECT count() FROM Items t0";
    for (std::size_t index = 1; index < sources; ++index) {
      sql += " FULL JOIN Items t" + std::to_string(index) + " ON 0";
    }
    const auto rows = Execute(sql);
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ(static_cast<std::int64_t>(sources * 3U), rows[0][0].integer_value());
    const BytecodeProgram program = LowerOrThrow(sql, catalog());
    const auto kinds = InstructionKinds(program);
    EXPECT_EQ(1, std::ranges::count(kinds, InstructionKind::kResultRow));
    EXPECT_EQ(sources - 1U,
              static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kOpenRelation)));
    EXPECT_LE(program.instructions().size(), sources * sources * 2U + sources * 96U + 128U);
  }
}

TEST_F(ReadLoweringJoin, AppliesInnerForwardConditionsAndConsumesLeftRowsInAllDestinations) {
  const auto forward = Execute(
      "SELECT a.id,b.id,c.id FROM items a JOIN items b ON c.id=b.id "
      "JOIN items c ON a.id=c.id ORDER BY a.id");
  ASSERT_EQ(3U, forward.size());
  const auto grouped = Execute(
      "SELECT a.id,count(),count(b.id),sum(b.id),b._rowid_ "
      "FROM items a LEFT JOIN items b ON a.id=b.id AND b.id<2 "
      "GROUP BY a.id ORDER BY a.id");
  ASSERT_EQ(3U, grouped.size());
  for (std::size_t index = 0; index < grouped.size(); ++index) {
    EXPECT_EQ(1, grouped[index][1].integer_value());
    EXPECT_EQ(index == 0U ? 1 : 0, grouped[index][2].integer_value());
    if (index == 0U) {
      EXPECT_EQ(1, grouped[index][3].integer_value());
      EXPECT_EQ(1, grouped[index][4].integer_value());
    } else {
      EXPECT_EQ(SqlValueType::kNull, grouped[index][3].type());
      EXPECT_EQ(SqlValueType::kNull, grouped[index][4].type());
    }
  }
  const auto distinct = Execute(
      "SELECT DISTINCT b.id FROM items a LEFT JOIN items b ON a.id=b.id AND b.id<2 "
      "ORDER BY b.id");
  ASSERT_EQ(2U, distinct.size());
  EXPECT_EQ(SqlValueType::kNull, distinct[0][0].type());
  EXPECT_EQ(1, distinct[1][0].integer_value());
  const auto demanded = Execute(
      "SELECT a.id,b.id FROM items a LEFT JOIN items b ON 0 "
      "UNION SELECT 99,99 LIMIT 1");
  ASSERT_EQ(1U, demanded.size());
  EXPECT_EQ(1, demanded[0][0].integer_value());
  EXPECT_EQ(SqlValueType::kNull, demanded[0][1].type());
  const auto unconditional =
      Execute("SELECT a.id,b.id FROM items a LEFT JOIN items b UNION SELECT 99,99 LIMIT 1");
  ASSERT_EQ(1U, unconditional.size());
}

TEST_F(ReadLoweringJoin, EvaluatesLeftOnOnlyForPhysicalCandidates) {
  const CustomEnvironment custom;
  callback_trace.clear();
  const auto rows = Execute(
      "SELECT trace_argument(a.id),b.id FROM items a LEFT JOIN items b "
      "ON trace_filter(b.id)>99",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(9U, std::ranges::count(callback_trace, "filter"));
  EXPECT_EQ(3U, std::ranges::count(callback_trace, "argument"));
  for (const auto& row : rows) {
    EXPECT_EQ(SqlValueType::kNull, row[1].type());
  }
  callback_count = 0;
  EXPECT_TRUE(Execute("SELECT failing() FROM items a LEFT JOIN items b ON failing() "
                      "LIMIT 0 OFFSET failing()",
                      custom.Binder(), custom.Vm())
                  .empty());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringJoin, EvaluatesFullOnOnceAndNeverMarksVirtualRightRows) {
  const CustomEnvironment custom;
  callback_trace.clear();
  const auto rows = Execute(
      "SELECT trace_argument(coalesce(a.id,b.id)),a.id,b.id FROM items a "
      "FULL JOIN items b ON trace_filter(b.id)>99",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(6U, rows.size());
  EXPECT_EQ(9U, std::ranges::count(callback_trace, "filter"));
  EXPECT_EQ(6U, std::ranges::count(callback_trace, "argument"));
  std::size_t left_only = 0;
  std::size_t right_only = 0;
  for (const auto& row : rows) {
    if (row[1].type() == SqlValueType::kNull) {
      ++right_only;
      EXPECT_NE(SqlValueType::kNull, row[2].type());
    } else {
      ++left_only;
      EXPECT_EQ(SqlValueType::kNull, row[2].type());
    }
  }
  EXPECT_EQ(3U, left_only);
  EXPECT_EQ(3U, right_only);
}

TEST_F(ReadLoweringJoin, KeepsComputedJoinKeysInTheirExistingCandidatePhase) {
  const auto rows = Execute(
      "SELECT count() FROM items a JOIN items b ON 0 "
      "AND b.id=abs(a.id-a.id-9223372036854775807-1)");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(0, rows[0][0].integer_value());
  EXPECT_THROW(static_cast<void>(Execute("SELECT count() FROM items a JOIN items b ON a.id<0 "
                                         "AND b.id=abs(a.id-a.id-9223372036854775807-1)")),
               std::runtime_error);
}

TEST_F(ReadLoweringJoin, KeepsComputedProbesBehindForwardOnCallbacks) {
  EXPECT_THROW(static_cast<void>(Execute("SELECT count() FROM items a JOIN items b "
                                         "ON c.id>0 AND abs(a.id-a.id-9223372036854775807-1)>0 "
                                         "JOIN items c ON c.id=a.id+1000")),
               std::runtime_error);
}

TEST_F(ReadLoweringJoin, KeepsNondeterministicJoinKeysInTheirPhysicalCandidatePhase) {
  const CustomEnvironment custom;
  for (const std::string_view key : {"trace_argument(a.id)", "trace_argument(2)"}) {
    SCOPED_TRACE(key);
    callback_trace.clear();
    const auto rows =
        Execute("SELECT a.id,b.id FROM items a JOIN items b ON b.id=" + std::string{key},
                custom.Binder(), custom.Vm());
    EXPECT_EQ(3U, rows.size());
    EXPECT_EQ(9U, std::ranges::count(callback_trace, "argument"));
  }
}

TEST_F(ReadLoweringJoin, PreservesMergedScopesAcrossGroupingSetsAndValuesCores) {
  const auto grouped = Execute(
      "SELECT id,count(),count(b.id) FROM items a LEFT JOIN items b USING(id) "
      "GROUP BY id HAVING count()>0 ORDER BY id");
  ASSERT_EQ(3U, grouped.size());
  for (std::size_t index = 0; index < grouped.size(); ++index) {
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), grouped[index][0].integer_value());
    EXPECT_EQ(1, grouped[index][1].integer_value());
    EXPECT_EQ(1, grouped[index][2].integer_value());
  }
  struct Case {
    std::string_view sql;
    std::array<std::int64_t, 4> expected;
  };
  const std::array cases{
      Case{.sql = "SELECT id FROM items a LEFT JOIN items b USING(id) UNION SELECT 99 ORDER BY 1",
           .expected = {1, 2, 3, 99}},
      Case{.sql = "VALUES(0) UNION ALL SELECT id FROM items a NATURAL LEFT JOIN items b ORDER BY 1",
           .expected = {0, 1, 2, 3}},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    const auto rows = Execute(test_case.sql);
    ASSERT_EQ(4U, rows.size());
    for (std::size_t index = 0; index < rows.size(); ++index) {
      EXPECT_EQ(test_case.expected[index], rows[index][0].integer_value());
    }
  }
  const CustomEnvironment custom;
  callback_count = 0;
  const auto demanded = Execute(
      "SELECT id FROM items a LEFT JOIN items b USING(id) "
      "UNION SELECT failing() LIMIT 1",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, demanded.size());
  EXPECT_EQ(1, demanded[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringJoin, KeepsUsingRepresentativesInTheirCoreAcrossSixtyFourSources) {
  for (const std::size_t sources : {2U, 8U, 64U}) {
    SCOPED_TRACE(sources);
    std::string sql = "SELECT id,t" + std::to_string(sources - 1U) + ".id FROM items t0";
    for (std::size_t index = 1; index < sources; ++index) {
      sql += " LEFT JOIN items t" + std::to_string(index) + " USING(id)";
    }
    sql += " ORDER BY id";
    const auto rows = Execute(sql);
    ASSERT_EQ(3U, rows.size());
    for (std::size_t index = 0; index < rows.size(); ++index) {
      const auto id = static_cast<std::int64_t>(index + 1U);
      EXPECT_EQ(id, rows[index][0].integer_value());
      EXPECT_EQ(id, rows[index][1].integer_value());
    }
  }
}

TEST_F(ReadLoweringJoin, SharesOneReadableBodyAcrossSixtyFourLeftSources) {
  for (const std::size_t sources : {2U, 8U, 32U, 64U}) {
    SCOPED_TRACE(sources);
    std::string sql = "SELECT 1 FROM Items t0";
    for (std::size_t index = 1; index < sources; ++index) {
      sql += " LEFT JOIN Items t" + std::to_string(index) + " ON 0";
    }
    sql += " LIMIT 0";
    const BytecodeProgram program = LowerOrThrow(sql, catalog());
    const auto kinds = InstructionKinds(program);
    EXPECT_EQ(sources,
              static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kOpenRead)));
    EXPECT_EQ(sources - 1U,
              static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kNullRow)));
    EXPECT_EQ(1, std::ranges::count(kinds, InstructionKind::kResultRow));
    EXPECT_LE(program.instructions().size(), sources * 48U + 32U);
    RecordProperty("instructions_for_" + std::to_string(sources) + "_sources",
                   std::to_string(program.instructions().size()));
  }
}

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

TEST(PlanLowering, LowersGlobalAggregatePlans) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan plan = OptimizeOrThrow("SELECT count() FROM Items", catalog);

  LowerPlanResult lowered = LowerPlan(plan);
  ASSERT_TRUE(lowered.has_value()) << lowered.error().detail;
  const std::vector<InstructionKind> kinds = InstructionKinds(*lowered);
  EXPECT_EQ(1U, std::ranges::count(kinds, InstructionKind::kOpenAggregate));
  EXPECT_EQ(1U, std::ranges::count(kinds, InstructionKind::kStepAggregate));
  EXPECT_EQ(1U, std::ranges::count(kinds, InstructionKind::kFinalizeAggregate));
  EXPECT_EQ(1U, std::ranges::count(kinds, InstructionKind::kCloseAggregate));
}

TEST(PlanLowering, RunsOrderOnlyAggregatesAndSkipsGlobalScalarOrderKeys) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const CustomEnvironment custom;
  const BytecodeProgram program = LowerOrThrow(
      "SELECT count() FROM Items ORDER BY sum(id), failing()", catalog, custom.Binder());
  const std::vector<InstructionKind> kinds = InstructionKinds(program);

  EXPECT_EQ(2U, std::ranges::count(kinds, InstructionKind::kOpenAggregate));
  EXPECT_EQ(2U, std::ranges::count(kinds, InstructionKind::kStepAggregate));
  EXPECT_EQ(2U, std::ranges::count(kinds, InstructionKind::kFinalizeAggregate));
  EXPECT_EQ(0U, std::ranges::count(kinds, InstructionKind::kCallScalar));
  EXPECT_EQ(0U, std::ranges::count(kinds, InstructionKind::kOpenSorter));
  EXPECT_EQ(0U, std::ranges::count(kinds, InstructionKind::kOpenTopN));
}

TEST(PlanLowering, RetainsGlobalOrderSnapshotsWithoutReadingInactiveGlobalInputCaptures) {
  test::WritePagerFixedVfs vfs{false};
  InitializeIndexedMutationDatabase(vfs);
  const CatalogSnapshotPtr catalog = IndexedMutationCatalog();
  ASSERT_TRUE(
      ExecuteMutationProgram(
          LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(1,'hit',7.0)", catalog),
          vfs)
          .has_value());
  const ByteBuffer valid = ByteBuffer::CopyOf(vfs.database_bytes());
  ByteBuffer corrupt = valid.Clone();
  corrupt.mutable_view()[test::kWritePagerPageSize] = std::byte{0};
  struct Case {
    std::string_view sql;
    bool requires_table = false;
  };
  for (const Case& test_case :
       {Case{.sql = "SELECT count(*) FROM Items o WHERE o.Name=?1"},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Name=?1 ORDER BY (SELECT o.Score)",
             .requires_table = true},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Name=?1 "
                    "ORDER BY EXISTS(SELECT o.Score)",
             .requires_table = true},
        Case{.sql = "SELECT count(*) FROM Items o CROSS JOIN Items i "
                    "WHERE o.Name=?1 AND i.Name=?1 ORDER BY (SELECT o.Score)",
             .requires_table = true},
        Case{.sql = "SELECT sum(EXISTS(SELECT o.Score)) FROM Items o WHERE o.Name=?1"},
        Case{.sql = "SELECT count(*) FILTER(WHERE EXISTS(SELECT o.Score)) "
                    "FROM Items o WHERE o.Name=?1"},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Name=?1 "
                    "ORDER BY sum(EXISTS(SELECT o.Score))"},
        Case{.sql = "SELECT (SELECT count(*) FROM Items i WHERE i.Name=?1 "
                    "ORDER BY o.Score) FROM Items o WHERE o.Name=?1"},
        Case{.sql = "SELECT count(*),EXISTS(SELECT o.Score) FROM Items o WHERE o.Name=?1",
             .requires_table = true},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Name=?1 "
                    "HAVING EXISTS(SELECT o.Score)",
             .requires_table = true},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Name=?1 ORDER BY sum(o.Score)",
             .requires_table = true}}) {
    for (const bool corrupt_input : {false, true}) {
      SCOPED_TRACE(test_case.sql);
      SCOPED_TRACE(corrupt_input);
      vfs.LoadDatabase(corrupt_input ? corrupt.view() : valid.view());
      auto pager = test::OpenWritePager(vfs, 1U);
      ASSERT_NE(nullptr, pager);
      RequireStatus(pager->BeginRead());
      const BytecodeProgram program = LowerOrThrow(test_case.sql, catalog);
      Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
      RequireStatus(vm.Bind(ParameterId{0}, SqlValue::Text("hit")));
      RequireStatus(
          vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
      if (!corrupt_input) {
        vfs.FailDatabaseReadAfter(1U, ErrorCode::kIo);
      }
      const auto stepped = vm.Step();
      if (test_case.requires_table) {
        ASSERT_FALSE(stepped.has_value());
        EXPECT_EQ(corrupt_input ? ErrorCode::kCorruption : ErrorCode::kIo, stepped.error().code());
        EXPECT_TRUE(vm.row().empty());
      } else {
        ASSERT_TRUE(stepped.has_value()) << stepped.error().ToString();
        EXPECT_EQ(VmStep::kRow, *stepped);
        ASSERT_EQ(1U, vm.row().size());
        EXPECT_EQ(1, vm.row()[0].integer_value());
        EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
      }
      RequireStatus(vm.Reset());
      RequireStatus(pager->EndRead());
    }
  }
}

TEST(PlanLowering, RetainsGlobalOrderBindingLeavesBeforeElidingScalarKeys) {
  test::WritePagerFixedVfs vfs{false};
  const CatalogSnapshotPtr catalog = InitializeCorrelatedSourceDatabase(vfs);
  const ByteBuffer valid = ByteBuffer::CopyOf(vfs.database_bytes());
  struct Case {
    std::string_view sql;
    bool retains_value = true;
    bool joined = false;
  };
  for (const Case& test_case :
       {Case{.sql = "SELECT count(*) FROM Items o WHERE o.id>0 ORDER BY o.Name"},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.id>0 ORDER BY coalesce(1,o.Name)"},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.id>0 ORDER BY (SELECT o.Name)"},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.id>0 ORDER BY EXISTS(SELECT o.Name)"},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.id>0 "
                    "ORDER BY EXISTS(SELECT 1 ORDER BY o.Name)"},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.id>0 ORDER BY (SELECT o.Name LIMIT 0)"},
        Case{.sql = "SELECT count(*) FROM Items o CROSS JOIN Plain p ORDER BY (SELECT o.Name)",
             .joined = true},
        Case{.sql = "SELECT sum(EXISTS(SELECT o.Name)) FROM Items o", .retains_value = false},
        Case{.sql = "SELECT count(*) FROM Items o ORDER BY (SELECT o.Name)",
             .retains_value = false}}) {
    for (const bool io_failure : {false, true}) {
      SCOPED_TRACE(test_case.sql);
      SCOPED_TRACE(io_failure);
      vfs.LoadDatabase(valid.view());
      auto pager = test::OpenWritePager(vfs, 64U);
      ASSERT_NE(nullptr, pager);
      RequireStatus(pager->BeginRead());
      const BytecodeProgram program = LowerOrThrow(test_case.sql, catalog);
      VmLimits limits{};
      if (!io_failure) {
        limits.maximum_value_bytes = 256;
      }
      Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core(), limits));
      RequireStatus(
          vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
      if (io_failure) {
        vfs.FailDatabaseReadAfter(test_case.joined ? 2U : 1U, ErrorCode::kIo);
      }
      const auto stepped = vm.Step();
      if (test_case.retains_value) {
        ASSERT_FALSE(stepped.has_value());
        EXPECT_EQ(io_failure ? ErrorCode::kIo : ErrorCode::kTooLarge, stepped.error().code());
        EXPECT_TRUE(vm.row().empty());
      } else {
        ASSERT_TRUE(stepped.has_value()) << stepped.error().ToString();
        EXPECT_EQ(VmStep::kRow, *stepped);
        ASSERT_EQ(1U, vm.row().size());
        EXPECT_EQ(3, vm.row()[0].integer_value());
        EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
      }
      RequireStatus(vm.Reset());
      RequireStatus(pager->EndRead());
    }
  }
}

TEST(PlanLowering, LowersIndexedMinMaxDirectionsAndReverseRanges) {
  const CatalogSnapshotPtr catalog = IndexedMutationCatalog();

  const BytecodeProgram minimum = LowerOrThrow("SELECT min(Score) FROM Items", catalog);
  const std::vector<InstructionKind> minimum_kinds = InstructionKinds(minimum);
  EXPECT_EQ(0U, std::ranges::count(minimum_kinds, InstructionKind::kLast));
  EXPECT_EQ(1U, std::ranges::count(minimum_kinds, InstructionKind::kPrevious));
  EXPECT_EQ(2U, std::ranges::count(minimum_kinds, InstructionKind::kSeekIndex));
  EXPECT_EQ(2U, std::ranges::count(minimum_kinds, InstructionKind::kStepAggregate));

  const BytecodeProgram maximum = LowerOrThrow("SELECT max(Score) FROM Items", catalog);
  const std::vector<InstructionKind> maximum_kinds = InstructionKinds(maximum);
  EXPECT_EQ(1U, std::ranges::count(maximum_kinds, InstructionKind::kRewind));
  EXPECT_EQ(1U, std::ranges::count(maximum_kinds, InstructionKind::kNext));
  EXPECT_EQ(1U, std::ranges::count(maximum_kinds, InstructionKind::kSeekIndex));
  EXPECT_EQ(2U, std::ranges::count(maximum_kinds, InstructionKind::kStepAggregate));

  const BytecodeProgram ranged =
      LowerOrThrow("SELECT min(Score) FROM Items WHERE Score>=?1 AND Score<?2", catalog);
  const auto seek = std::ranges::find_if(ranged.instructions(), [](const Instruction& instruction) {
    return std::holds_alternative<SeekIndexInstruction>(instruction);
  });
  const auto end = std::ranges::find_if(ranged.instructions(), [](const Instruction& instruction) {
    return std::holds_alternative<CheckIndexRangeInstruction>(instruction);
  });
  ASSERT_NE(ranged.instructions().end(), seek);
  ASSERT_NE(ranged.instructions().end(), end);
  EXPECT_EQ(IndexSeekMode::kLessOrEqual, std::get<SeekIndexInstruction>(*seek).mode);
  EXPECT_EQ(IndexRangeEndMode::kReverseExclusive, std::get<CheckIndexRangeInstruction>(*end).mode);
}

TEST(PlanLowering, SeeksPastNullableMinMaxNullRegions) {
  const CatalogSnapshotPtr catalog = IndexedMutationCatalog();

  const BytecodeProgram ascending_min = LowerOrThrow("SELECT min(Name) FROM Items", catalog);
  const std::vector<IndexSeekMode> ascending_min_seeks = IndexSeekModes(ascending_min);
  ASSERT_FALSE(ascending_min_seeks.empty());
  EXPECT_EQ(IndexSeekMode::kGreater, ascending_min_seeks.front());

  const BytecodeProgram descending_min = LowerOrThrow("SELECT min(Score) FROM Items", catalog);
  const std::vector<IndexSeekMode> descending_min_seeks = IndexSeekModes(descending_min);
  ASSERT_FALSE(descending_min_seeks.empty());
  EXPECT_EQ(IndexSeekMode::kLess, descending_min_seeks.front());

  const BytecodeProgram ascending_max = LowerOrThrow("SELECT max(Name) FROM Items", catalog);
  const std::vector<IndexRangeEndMode> ascending_max_ends = IndexRangeEndModes(ascending_max);
  ASSERT_FALSE(ascending_max_ends.empty());
  EXPECT_EQ(IndexRangeEndMode::kReverseExclusive, ascending_max_ends.front());

  const BytecodeProgram descending_max = LowerOrThrow("SELECT max(Score) FROM Items", catalog);
  const std::vector<IndexRangeEndMode> descending_max_ends = IndexRangeEndModes(descending_max);
  ASSERT_FALSE(descending_max_ends.empty());
  EXPECT_EQ(IndexRangeEndMode::kExclusive, descending_max_ends.front());
}

TEST(PlanLowering, LowersGroupedAggregateModifiers) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BytecodeProgram distinct =
      LowerOrThrow("SELECT name,count(DISTINCT id) FROM items GROUP BY name", catalog);
  EXPECT_EQ(1U, distinct.relations().size());
  EXPECT_EQ(1U, std::ranges::count(InstructionKinds(distinct), InstructionKind::kOpenRelation));

  const BytecodeProgram filtered =
      LowerOrThrow("SELECT name,count() FILTER (WHERE id>1) FROM items GROUP BY name", catalog);
  EXPECT_TRUE(filtered.relations().empty());
  EXPECT_EQ(1U, filtered.sorters().size());

  const BytecodeProgram ordered =
      LowerOrThrow("SELECT name,group_concat(id ORDER BY id) FROM items GROUP BY name", catalog);
  EXPECT_EQ(2U, ordered.sorters().size());
  EXPECT_EQ(2U, std::ranges::count(InstructionKinds(ordered), InstructionKind::kOpenSorter));
}

TEST(ReadLowering, PreservesGlobalAggregateSkippingAndErrors) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 103}));
  const CustomEnvironment custom;

  const BytecodeProgram zero =
      LowerOrThrow("SELECT sum(failing()) FROM items LIMIT 0", catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(zero, *pager, catalog->version().generation, custom.Vm()).empty());

  const BytecodeProgram offset = LowerOrThrow(
      "SELECT failing(),count() FROM items LIMIT -1 OFFSET 1", catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(offset, *pager, catalog->version().generation, custom.Vm()).empty());

  const BytecodeProgram immediate =
      LowerOrThrow("SELECT sum(failing()) FROM items", catalog, custom.Binder());
  Vm immediate_vm = TakeValue(Vm::Create(immediate, custom.Vm()));
  RequireStatus(immediate_vm.AttachExecutionContext(
      VmExecutionContext{*pager, catalog->version().generation}));
  const auto immediate_error = immediate_vm.Step();
  ASSERT_FALSE(immediate_error.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, immediate_error.error().code());

  const BytecodeProgram delayed =
      LowerOrThrow("SELECT group_concat(name) FROM items", catalog, custom.Binder());
  Vm delayed_vm = TakeValue(Vm::Create(delayed, custom.Vm(), VmLimits{.maximum_value_bytes = 5}));
  RequireStatus(
      delayed_vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  const auto delayed_error = delayed_vm.Step();
  ASSERT_FALSE(delayed_error.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, delayed_error.error().code());
}

TEST(ReadLowering, PreservesGlobalAggregateRepresentatives) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const auto add_row = [&](std::int64_t id, std::string name, SqlValue score) {
    const std::array parameters{
        SqlValue::Integer(id),
        SqlValue::Text(std::move(name)),
        std::move(score),
    };
    ASSERT_TRUE(ExecuteMutationProgram(insert, vfs, parameters).has_value());
  };
  add_row(1, "alpha", SqlValue::Real(1.5));
  add_row(2, "beta", SqlValue::Real(3.0));
  add_row(3, "gamma", SqlValue{});

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());

  const BytecodeProgram ordinary = LowerOrThrow("SELECT id,count() FROM Items", catalog);
  const auto ordinary_rows = ExecuteRows(ordinary, *pager, catalog->version().generation);
  ASSERT_EQ(1U, ordinary_rows.size());
  EXPECT_EQ(1, ordinary_rows[0][0].integer_value());
  EXPECT_EQ(3, ordinary_rows[0][1].integer_value());

  const BytecodeProgram minimum_then_maximum =
      LowerOrThrow("SELECT id,min(Score),max(Score) FROM Items", catalog);
  const auto minimum_then_maximum_rows =
      ExecuteRows(minimum_then_maximum, *pager, catalog->version().generation);
  ASSERT_EQ(1U, minimum_then_maximum_rows.size());
  EXPECT_EQ(2, minimum_then_maximum_rows[0][0].integer_value());
  EXPECT_EQ(1.5, minimum_then_maximum_rows[0][1].real_value());
  EXPECT_EQ(3.0, minimum_then_maximum_rows[0][2].real_value());

  const BytecodeProgram maximum_then_minimum =
      LowerOrThrow("SELECT id,max(Score),min(Score) FROM Items", catalog);
  const auto maximum_then_minimum_rows =
      ExecuteRows(maximum_then_minimum, *pager, catalog->version().generation);
  ASSERT_EQ(1U, maximum_then_minimum_rows.size());
  EXPECT_EQ(1, maximum_then_minimum_rows[0][0].integer_value());
  EXPECT_EQ(3.0, maximum_then_minimum_rows[0][1].real_value());
  EXPECT_EQ(1.5, maximum_then_minimum_rows[0][2].real_value());

  const BytecodeProgram all_null =
      LowerOrThrow("SELECT id,min(Score) FROM Items WHERE Score IS NULL", catalog);
  const auto all_null_rows = ExecuteRows(all_null, *pager, catalog->version().generation);
  ASSERT_EQ(1U, all_null_rows.size());
  EXPECT_EQ(3, all_null_rows[0][0].integer_value());
  EXPECT_EQ(SqlValueType::kNull, all_null_rows[0][1].type());

  const BytecodeProgram empty = LowerOrThrow("SELECT id,count() FROM Items WHERE 0", catalog);
  const auto empty_rows = ExecuteRows(empty, *pager, catalog->version().generation);
  ASSERT_EQ(1U, empty_rows.size());
  EXPECT_EQ(SqlValueType::kNull, empty_rows[0][0].type());
  EXPECT_EQ(0, empty_rows[0][1].integer_value());

  const BytecodeProgram representative_having =
      LowerOrThrow("SELECT count() FROM Items HAVING id=1", catalog);
  EXPECT_EQ(1U, ExecuteRows(representative_having, *pager, catalog->version().generation).size());

  RequireStatus(pager->EndRead());
  PosixVfs indexed_vfs;
  pager = TakeValue(Pager::Open(indexed_vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr indexed =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 107}));
  const BytecodeProgram indexed_covering = LowerOrThrow("SELECT id,min(score) FROM items", indexed);
  EXPECT_TRUE(
      std::ranges::any_of(indexed_covering.instructions(), [](const Instruction& instruction) {
        const auto* step = std::get_if<StepAggregateInstruction>(&instruction);
        return step != nullptr && step->effect_output.has_value();
      }));
  const auto indexed_covering_rows =
      ExecuteRows(indexed_covering, *pager, indexed->version().generation);
  ASSERT_EQ(1U, indexed_covering_rows.size());
  EXPECT_EQ(4096, indexed_covering_rows[0][0].integer_value());
  EXPECT_EQ(0, indexed_covering_rows[0][1].integer_value());

  const BytecodeProgram indexed_noncovering =
      LowerOrThrow("SELECT payload,min(score) FROM items", indexed);
  const auto indexed_noncovering_rows =
      ExecuteRows(indexed_noncovering, *pager, indexed->version().generation);
  ASSERT_EQ(1U, indexed_noncovering_rows.size());
  EXPECT_FALSE(BlobBytes(indexed_noncovering_rows[0][0]).empty());
  EXPECT_EQ(0, indexed_noncovering_rows[0][1].integer_value());
}

TEST(ReadLowering, ExecutesAggregateFiltersDistinctAndLocalOrdering) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 109}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));

  const BytecodeProgram program = LowerOrThrow(
      "SELECT count(DISTINCT score) FILTER (WHERE id>1),"
      "group_concat(name ORDER BY id DESC),"
      "group_concat(DISTINCT category ORDER BY category) "
      "FROM items",
      catalog);
  const auto rows =
      ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);

  ASSERT_EQ(1U, rows.size());
  ASSERT_EQ(3U, rows[0].size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ("gamma,beta,alpha", TextBytes(rows[0][1]));
  EXPECT_EQ("one,two", TextBytes(rows[0][2]));
}

TEST(ReadLowering, PreservesAggregateModifierEvaluationSchedules) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 111}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const CustomEnvironment custom;

  callback_trace.clear();
  const BytecodeProgram filtered = LowerOrThrow(
      "SELECT group_concat(trace_argument(name) ORDER BY trace_key(id)) "
      "FILTER (WHERE trace_filter(id>1)) FROM items",
      catalog, custom.Binder());
  const auto filtered_rows =
      ExecuteRows(filtered, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, filtered_rows.size());
  EXPECT_EQ("beta,gamma", TextBytes(filtered_rows[0][0]));
  EXPECT_EQ((std::vector<std::string>{
                "filter",
                "filter",
                "key",
                "argument",
                "filter",
                "key",
                "argument",
            }),
            callback_trace);

  callback_trace.clear();
  const BytecodeProgram minimum =
      LowerOrThrow("SELECT min(trace_argument(score) ORDER BY trace_key(id)) FROM items", catalog,
                   custom.Binder());
  const auto minimum_rows =
      ExecuteRows(minimum, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, minimum_rows.size());
  EXPECT_EQ(1.5, minimum_rows[0][0].real_value());
  EXPECT_EQ((std::vector<std::string>{"argument", "argument", "argument"}), callback_trace);

  callback_count = 0;
  const BytecodeProgram reused =
      LowerOrThrow("SELECT group_concat(volatile_counter() ORDER BY volatile_counter()) FROM items",
                   catalog, custom.Binder());
  const auto reused_rows =
      ExecuteRows(reused, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, reused_rows.size());
  EXPECT_EQ("1,2,3", TextBytes(reused_rows[0][0]));
  EXPECT_EQ(3U, callback_count);

  const BytecodeProgram separator = LowerOrThrow(
      "SELECT group_concat(name,'-' ORDER BY id DESC),"
      "string_agg(name,'/' ORDER BY id DESC),"
      "count(name ORDER BY id DESC),"
      "sum(id ORDER BY name DESC),"
      "total(id ORDER BY name DESC),"
      "avg(id ORDER BY name DESC) "
      "FROM items",
      catalog);
  const auto separator_rows =
      ExecuteRows(separator, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, separator_rows.size());
  EXPECT_EQ("gamma-beta-alpha", TextBytes(separator_rows[0][0]));
  EXPECT_EQ("gamma/beta/alpha", TextBytes(separator_rows[0][1]));
  EXPECT_EQ(3, separator_rows[0][2].integer_value());
  EXPECT_EQ(6, separator_rows[0][3].integer_value());
  EXPECT_EQ(6.0, separator_rows[0][4].real_value());
  EXPECT_EQ(2.0, separator_rows[0][5].real_value());

  callback_count = 0;
  const BytecodeProgram zero = LowerOrThrow(
      "SELECT group_concat(failing() ORDER BY failing()) "
      "FILTER (WHERE failing()) FROM items LIMIT 0",
      catalog, custom.Binder());
  EXPECT_TRUE(
      ExecuteRows(zero, *pager, catalog->version().generation, custom.Vm(), &memory).empty());
  EXPECT_EQ(0U, callback_count);

  callback_count = 0;
  const BytecodeProgram numeric_distinct = LowerOrThrow(
      "SELECT count(DISTINCT numeric_representative()) FROM items", catalog, custom.Binder());
  const auto numeric_distinct_rows =
      ExecuteRows(numeric_distinct, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, numeric_distinct_rows.size());
  EXPECT_EQ(1, numeric_distinct_rows[0][0].integer_value());
  EXPECT_EQ(3U, callback_count);
}

TEST(ReadLowering, PreservesAggregateDistinctOrderedRepresentatives) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const auto add_row = [&](std::int64_t id, std::string name, std::int64_t score) {
    const std::array parameters{
        SqlValue::Integer(id),
        SqlValue::Text(std::move(name)),
        SqlValue::Integer(score),
    };
    ASSERT_TRUE(ExecuteMutationProgram(insert, vfs, parameters).has_value());
  };
  add_row(1, "A", 30);
  add_row(2, "a", 10);
  add_row(3, "B", 20);
  add_row(4, "b", 40);

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));

  const BytecodeProgram unique = LowerOrThrow(
      "SELECT group_concat(DISTINCT Name COLLATE NOCASE "
      "ORDER BY Name COLLATE NOCASE) FROM Items",
      catalog);
  const auto unique_rows =
      ExecuteRows(unique, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, unique_rows.size());
  EXPECT_EQ("a,b", TextBytes(unique_rows[0][0]));

  const BytecodeProgram ordinary = LowerOrThrow(
      "SELECT group_concat(DISTINCT Name COLLATE NOCASE ORDER BY Score) FROM Items", catalog);
  const auto ordinary_rows =
      ExecuteRows(ordinary, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, ordinary_rows.size());
  EXPECT_EQ("B,A", TextBytes(ordinary_rows[0][0]));

  const BytecodeProgram grouped = LowerOrThrow(
      "SELECT id<=3,group_concat(DISTINCT Name COLLATE NOCASE ORDER BY Score) "
      "FROM Items GROUP BY id<=3 ORDER BY 1",
      catalog);
  const auto grouped_rows =
      ExecuteRows(grouped, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(2U, grouped_rows.size());
  EXPECT_EQ(0, grouped_rows[0][0].integer_value());
  EXPECT_EQ("b", TextBytes(grouped_rows[0][1]));
  EXPECT_EQ(1, grouped_rows[1][0].integer_value());
  EXPECT_EQ("B,A", TextBytes(grouped_rows[1][1]));
}

TEST(ReadLowering, PreservesFilteredAndDistinctMinMaxRepresentativeGates) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const auto add_row = [&](std::int64_t id, std::string name, std::int64_t score) {
    const std::array parameters{
        SqlValue::Integer(id),
        SqlValue::Text(std::move(name)),
        SqlValue::Integer(score),
    };
    ASSERT_TRUE(ExecuteMutationProgram(insert, vfs, parameters).has_value());
  };
  add_row(1, "replace", 2);
  add_row(2, "replace", 1);
  add_row(3, "replace", 1);
  add_row(4, "keep", 1);
  add_row(5, "keep", 2);
  add_row(6, "keep", 2);
  add_row(7, "filtered", 2);
  add_row(8, "filtered", 1);

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));

  const BytecodeProgram stale_replace =
      LowerOrThrow("SELECT id,min(DISTINCT Score) FROM Items WHERE id<=3", catalog);
  const auto stale_replace_rows = ExecuteRows(stale_replace, *pager, catalog->version().generation,
                                              VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, stale_replace_rows.size());
  EXPECT_EQ(3, stale_replace_rows[0][0].integer_value());
  EXPECT_EQ(1.0, stale_replace_rows[0][1].real_value());

  const BytecodeProgram stale_keep =
      LowerOrThrow("SELECT id,min(DISTINCT Score) FROM Items WHERE id>=4 AND id<=6", catalog);
  const auto stale_keep_rows = ExecuteRows(stale_keep, *pager, catalog->version().generation,
                                           VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, stale_keep_rows.size());
  EXPECT_EQ(4, stale_keep_rows[0][0].integer_value());
  EXPECT_EQ(1.0, stale_keep_rows[0][1].real_value());

  const BytecodeProgram grouped = LowerOrThrow(
      "SELECT Name,id,min(DISTINCT Score) FROM Items WHERE id<=6 GROUP BY Name ORDER BY Name",
      catalog);
  const auto grouped_rows =
      ExecuteRows(grouped, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(2U, grouped_rows.size());
  EXPECT_EQ("keep", TextBytes(grouped_rows[0][0]));
  EXPECT_EQ(4, grouped_rows[0][1].integer_value());
  EXPECT_EQ("replace", TextBytes(grouped_rows[1][0]));
  EXPECT_EQ(3, grouped_rows[1][1].integer_value());

  const BytecodeProgram filtered =
      LowerOrThrow("SELECT id,min(Score) FILTER (WHERE id=3) FROM Items WHERE id<=3", catalog);
  const auto filtered_rows =
      ExecuteRows(filtered, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, filtered_rows.size());
  EXPECT_EQ(3, filtered_rows[0][0].integer_value());

  const BytecodeProgram rejected =
      LowerOrThrow("SELECT id,min(Score) FILTER (WHERE 0) FROM Items WHERE id<=3", catalog);
  const auto rejected_rows =
      ExecuteRows(rejected, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, rejected_rows.size());
  EXPECT_EQ(1, rejected_rows[0][0].integer_value());
  EXPECT_EQ(SqlValueType::kNull, rejected_rows[0][1].type());

  const BytecodeProgram global_final_skip = LowerOrThrow(
      "SELECT id,min(Score),max(id) FILTER (WHERE 0) "
      "FROM Items WHERE Name='filtered'",
      catalog);
  const auto global_final_skip_rows = ExecuteRows(
      global_final_skip, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, global_final_skip_rows.size());
  EXPECT_EQ(8, global_final_skip_rows[0][0].integer_value());

  const BytecodeProgram grouped_final_skip = LowerOrThrow(
      "SELECT Name,id,min(Score),max(id) FILTER (WHERE 0) "
      "FROM Items WHERE Name='filtered' GROUP BY Name",
      catalog);
  const auto grouped_final_skip_rows = ExecuteRows(
      grouped_final_skip, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, grouped_final_skip_rows.size());
  EXPECT_EQ(7, grouped_final_skip_rows[0][1].integer_value());
}

TEST(ReadLowering, SharesTemporaryStorageAcrossAggregateModifiers) {
  constexpr ByteCount kModifierSpillThreshold{64U * 1024U};
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 139}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const TemporaryStorageFactory file = TakeValue(
      TemporaryStorageFactory::Create(vfs, *pager,
                                      TemporaryStorageOptions{
                                          .mode = TemporaryStoreMode::kFile,
                                          .sorter_memory_threshold = kModifierSpillThreshold,
                                      }));
  const BytecodeProgram program = LowerOrThrow(
      "SELECT flag+0,"
      "count(DISTINCT category),"
      "group_concat(category ORDER BY score DESC),"
      "group_concat(DISTINCT category ORDER BY category DESC) "
      "FROM items GROUP BY flag+0 ORDER BY 1",
      catalog);

  const auto memory_rows =
      ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  const auto file_rows =
      ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(), &file);
  ASSERT_EQ(memory_rows.size(), file_rows.size());
  ASSERT_EQ(2U, memory_rows.size());
  for (std::size_t row = 0; row < memory_rows.size(); ++row) {
    ASSERT_EQ(4U, memory_rows[row].size());
    ASSERT_EQ(4U, file_rows[row].size());
    EXPECT_EQ(memory_rows[row][0].integer_value(), file_rows[row][0].integer_value());
    EXPECT_EQ(memory_rows[row][1].integer_value(), file_rows[row][1].integer_value());
    EXPECT_EQ(TextBytes(memory_rows[row][2]), TextBytes(file_rows[row][2]));
    EXPECT_EQ(TextBytes(memory_rows[row][3]), TextBytes(file_rows[row][3]));
  }
}

TEST_F(ReadLoweringAggregateIntegration, DeduplicatesCompletedGroups) {
  const auto rows = Execute("SELECT DISTINCT count() FROM items GROUP BY id");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, StreamsGlobalCompoundArms) {
  const auto rows = Execute("SELECT count() FROM items UNION ALL SELECT sum(id) FROM items");
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
  EXPECT_EQ(6, rows[1][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, DeduplicatesGlobalUnionRows) {
  const auto rows = Execute("SELECT count() FROM items UNION SELECT count() FROM items");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, OrdersMixedCompoundArms) {
  const auto rows = Execute(
      "SELECT count() FROM items UNION ALL SELECT id FROM items ORDER BY 1 LIMIT 3 OFFSET 1");
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(2, rows[0][0].integer_value());
  EXPECT_EQ(3, rows[1][0].integer_value());
  EXPECT_EQ(3, rows[2][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, OrdersDistinctGroupedRows) {
  const auto rows = Execute("SELECT DISTINCT count() FROM items GROUP BY score IS NULL ORDER BY 1");
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(2, rows[1][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, ExecutesIndexedMinMaxCompoundArms) {
  const auto rows = Execute("SELECT min(name) FROM items UNION ALL SELECT max(name) FROM items");
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ("alpha", TextBytes(rows[0][0]));
  EXPECT_EQ("gamma", TextBytes(rows[1][0]));
}

TEST_F(ReadLoweringAggregateIntegration, CompletesNoncoveringGroupedIndexArms) {
  constexpr std::string_view kSql =
      "SELECT name,count(score) FROM items WHERE name='alpha' GROUP BY name "
      "UNION ALL SELECT 'fallback',0";
  const PhysicalPlan plan = OptimizeOrThrow(kSql, catalog());
  ASSERT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(plan.nodes().front().payload));
  EXPECT_FALSE(std::get<PhysicalIndexScanNode>(plan.nodes().front().payload).covering);

  const auto rows = Execute(kSql);
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ("alpha", TextBytes(rows[0][0]));
  EXPECT_EQ(1, rows[0][1].integer_value());
  EXPECT_EQ("fallback", TextBytes(rows[1][0]));
  EXPECT_EQ(0, rows[1][1].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, ClosesGroupedIndexOnMissingSeekWithoutOpeningTable) {
  constexpr std::string_view kSql =
      "SELECT name,count(score) FROM items WHERE name='missing' GROUP BY name "
      "UNION ALL SELECT 'fallback',0";
  const PhysicalPlan plan = OptimizeOrThrow(kSql, catalog());
  ASSERT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(plan.nodes().front().payload));
  EXPECT_FALSE(std::get<PhysicalIndexScanNode>(plan.nodes().front().payload).covering);

  const auto rows = Execute(kSql);
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("fallback", TextBytes(rows[0][0]));
  EXPECT_EQ(0, rows[0][1].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, ClosesGroupedIndexOnRangeEndWithoutOpeningTable) {
  constexpr std::string_view kSql =
      "SELECT name,count(score) FROM items WHERE name<'a' GROUP BY name "
      "UNION ALL SELECT 'fallback',0";
  const PhysicalPlan plan = OptimizeOrThrow(kSql, catalog());
  ASSERT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(plan.nodes().front().payload));
  EXPECT_FALSE(std::get<PhysicalIndexScanNode>(plan.nodes().front().payload).covering);

  const auto rows = Execute(kSql);
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("fallback", TextBytes(rows[0][0]));
  EXPECT_EQ(0, rows[0][1].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, CompletesNullGroupedIndexKeysBeforeOpeningCursors) {
  constexpr std::string_view kSql =
      "SELECT name,count(score) FROM items WHERE name=?1 GROUP BY name "
      "UNION ALL SELECT 'fallback',0";
  const PhysicalPlan plan = OptimizeOrThrow(kSql, catalog());
  ASSERT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(plan.nodes().front().payload));
  EXPECT_FALSE(std::get<PhysicalIndexScanNode>(plan.nodes().front().payload).covering);

  const std::array parameters{SqlValue{}};
  const auto rows = Execute(kSql, BindEnvironment::Core(), VmEnvironment::Core(), parameters);
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("fallback", TextBytes(rows[0][0]));
  EXPECT_EQ(0, rows[0][1].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, StopsCoveringGroupedIndexUnionAfterOneRow) {
  constexpr std::string_view kSql =
      "SELECT count() FROM items GROUP BY name UNION SELECT 0 LIMIT 1";
  const PhysicalPlan plan = OptimizeOrThrow(kSql, catalog());
  ASSERT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(plan.nodes().front().payload));
  EXPECT_TRUE(std::get<PhysicalIndexScanNode>(plan.nodes().front().payload).covering);

  const auto rows = Execute(kSql);
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, StopsCoveringGroupedIndexUnionAllAfterOneRow) {
  constexpr std::string_view kSql =
      "SELECT count() FROM items GROUP BY name UNION ALL SELECT 0 LIMIT 1";
  const PhysicalPlan plan = OptimizeOrThrow(kSql, catalog());
  ASSERT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(plan.nodes().front().payload));
  EXPECT_TRUE(std::get<PhysicalIndexScanNode>(plan.nodes().front().payload).covering);

  const auto rows = Execute(kSql);
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, SkipsSatisfiedGlobalDistinctOrderOnlyExpressions) {
  constexpr std::string_view kSql =
      "SELECT DISTINCT count() FROM items ORDER BY abs(-9223372036854775808)";
  const PhysicalPlan plan = OptimizeOrThrow(kSql, catalog());
  const auto order = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(plan.nodes().end(), order);
  ASSERT_TRUE(std::get<PhysicalAdvancedOrderNode>(order->payload).input_order_satisfied);

  const BytecodeProgram program = LowerOrThrow(kSql, catalog());
  const auto kinds = InstructionKinds(program);
  EXPECT_EQ(0, std::ranges::count(kinds, InstructionKind::kOpenSorter));
  EXPECT_EQ(0, std::ranges::count(kinds, InstructionKind::kOpenTopN));
  const auto rows = Execute(kSql);
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, CoalescesSatisfiedGroupedDistinctOrdering) {
  constexpr std::string_view kSql =
      "SELECT DISTINCT name,count() FROM items GROUP BY name ORDER BY name LIMIT 1";
  const PhysicalPlan plan = OptimizeOrThrow(kSql, catalog());
  const auto order = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(plan.nodes().end(), order);
  ASSERT_TRUE(std::get<PhysicalAdvancedOrderNode>(order->payload).input_order_satisfied);

  const BytecodeProgram program = LowerOrThrow(kSql, catalog());
  const auto kinds = InstructionKinds(program);
  EXPECT_EQ(0, std::ranges::count(kinds, InstructionKind::kOpenSorter));
  EXPECT_EQ(0, std::ranges::count(kinds, InstructionKind::kOpenTopN));
  const auto rows = Execute(kSql);
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("alpha", TextBytes(rows[0][0]));
  EXPECT_EQ(1, rows[0][1].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, StopsSatisfiedSortedDistinctOrderBeforeLaterArguments) {
  const CustomEnvironment custom;
  constexpr std::string_view kSql =
      "SELECT DISTINCT score IS NULL,count(fail_after_one(name)) FROM items "
      "GROUP BY score IS NULL ORDER BY score IS NULL DESC LIMIT 1";
  const PhysicalPlan plan = OptimizeOrThrow(kSql, catalog(), custom.Binder());
  const auto order = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(plan.nodes().end(), order);
  ASSERT_TRUE(std::get<PhysicalAdvancedOrderNode>(order->payload).input_order_satisfied);

  const BytecodeProgram program = LowerOrThrow(kSql, catalog(), custom.Binder());
  const auto kinds = InstructionKinds(program);
  EXPECT_EQ(1, std::ranges::count(kinds, InstructionKind::kOpenSorter));
  EXPECT_EQ(0, std::ranges::count(kinds, InstructionKind::kOpenTopN));
  const auto rows = Execute(kSql, custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(1, rows[0][1].integer_value());
  EXPECT_EQ(1U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, OrdersSingletonDistinctAggregatePayloadColumns) {
  constexpr std::string_view kSql =
      "SELECT DISTINCT name,count() FROM items GROUP BY name ORDER BY count()";
  const auto rows = Execute(kSql);
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ("alpha", TextBytes(rows[0][0]));
  EXPECT_EQ("beta", TextBytes(rows[1][0]));
  EXPECT_EQ("gamma", TextBytes(rows[2][0]));
  for (const auto& row : rows) {
    EXPECT_EQ(1, row[1].integer_value());
  }
}

TEST_F(ReadLoweringAggregateIntegration, ExecutesGroupedModifierCompoundArms) {
  const auto rows = Execute(
      "SELECT score IS NULL,count(DISTINCT category) FROM items GROUP BY score IS NULL "
      "UNION ALL SELECT 2,count() FROM items ORDER BY 1");
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(0, rows[0][0].integer_value());
  EXPECT_EQ(2, rows[0][1].integer_value());
  EXPECT_EQ(1, rows[1][0].integer_value());
  EXPECT_EQ(0, rows[1][1].integer_value());
  EXPECT_EQ(2, rows[2][0].integer_value());
  EXPECT_EQ(3, rows[2][1].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, PreservesMixedSetBarriers) {
  const auto rows = Execute(
      "SELECT count() FROM items EXCEPT SELECT sum(id) FROM items "
      "UNION SELECT count() FROM items INTERSECT SELECT 3");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, ContinuesAfterSuppressedAggregateArm) {
  const auto rows =
      Execute("SELECT count() FROM items HAVING 0 UNION ALL SELECT id FROM items WHERE rowid=2");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(2, rows[0][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, AcceptsEmptyGroupedCompoundArms) {
  const auto rows =
      Execute("SELECT count() FROM items WHERE 0 GROUP BY id UNION ALL SELECT count() FROM items");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, AcceptsSourceFreeGroupedCompoundArms) {
  const auto rows = Execute("SELECT 1,count() GROUP BY 1 UNION ALL SELECT 2,count() FROM items");
  ASSERT_EQ(2U, rows.size());
  ASSERT_EQ(2U, rows[0].size());
  ASSERT_EQ(2U, rows[1].size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(1, rows[0][1].integer_value());
  EXPECT_EQ(2, rows[1][0].integer_value());
  EXPECT_EQ(3, rows[1][1].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, AcceptsRowIdGroupedCompoundArms) {
  const auto rows = Execute(
      "SELECT count() FROM items WHERE rowid=2 GROUP BY id "
      "UNION ALL SELECT count() FROM items WHERE rowid=3 GROUP BY id");
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(1, rows[1][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, DeduplicatesSourceFreeGroupedRows) {
  const auto rows = Execute("SELECT DISTINCT 1,count() GROUP BY 1");
  ASSERT_EQ(1U, rows.size());
  ASSERT_EQ(2U, rows[0].size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(1, rows[0][1].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, EmitsEmptyGlobalAndSourceFreeAggregateRows) {
  const auto rows = Execute("SELECT count() FROM items WHERE 0 UNION ALL SELECT count()");
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ(0, rows[0][0].integer_value());
  EXPECT_EQ(1, rows[1][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, SkipsGlobalProjectionBeforeOffset) {
  const CustomEnvironment custom;
  const auto rows = Execute(
      "SELECT failing()+count() FROM items "
      "UNION ALL SELECT sum(id) FROM items LIMIT 1 OFFSET 1",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(6, rows[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, SkipsGroupedProjectionBeforeOffset) {
  const CustomEnvironment custom;
  const auto rows = Execute(
      "SELECT failing()+count() FROM items GROUP BY id "
      "UNION ALL SELECT sum(id) FROM items LIMIT 1 OFFSET 3",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(6, rows[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, StopsGlobalUnionAfterOneRow) {
  const CustomEnvironment custom;
  const auto rows = Execute("SELECT count() FROM items UNION SELECT failing() LIMIT 1",
                            custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, StopsGroupedUnionAfterOneRow) {
  const CustomEnvironment custom;
  const auto rows = Execute(
      "SELECT count(fail_after_one(name)) FROM items GROUP BY id "
      "UNION SELECT failing() LIMIT 1",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(1U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, StopsSortedGroupedUnionAfterOneRow) {
  const CustomEnvironment custom;
  const auto rows = Execute(
      "SELECT count(fail_after_one(name)) FROM items GROUP BY id+0 "
      "UNION SELECT failing() LIMIT 1",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(1U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, SkipsHavingRejectedGroupsBeforeDemandOneOutput) {
  const CustomEnvironment custom;
  constexpr std::string_view kSql =
      "SELECT volatile_counter()+count() FROM items GROUP BY id HAVING count()+id>2 "
      "UNION SELECT failing() LIMIT 1";
  const PhysicalPlan plan = OptimizeOrThrow(kSql, catalog(), custom.Binder());
  ASSERT_TRUE(std::ranges::any_of(plan.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalHavingNode>(node.payload);
  }));
  const auto rows = Execute(kSql, custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(2, rows[0][0].integer_value());
  EXPECT_EQ(1U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, StopsGroupedUnionAllAfterOneRow) {
  const CustomEnvironment custom;
  const auto rows = Execute(
      "SELECT count(fail_after_one(name)) FROM items GROUP BY id "
      "UNION ALL SELECT failing() LIMIT 1",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(1U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, StopsAfterFinalGroupedUnionRow) {
  const CustomEnvironment custom;
  const auto rows = Execute(
      "SELECT count(fail_after_one(name)) FROM items WHERE rowid=2 GROUP BY id "
      "UNION SELECT failing() LIMIT 1",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(1U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, StopsAfterFinalGroupedUnionAllRow) {
  const CustomEnvironment custom;
  const auto rows =
      Execute("SELECT count() FROM items GROUP BY id UNION ALL SELECT failing() LIMIT 3",
              custom.Binder(), custom.Vm());
  ASSERT_EQ(3U, rows.size());
  for (const auto& row : rows) {
    EXPECT_EQ(1, row[0].integer_value());
  }
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, SharesOffsetAcrossAggregateArms) {
  const auto rows = Execute(
      "SELECT count() FROM items UNION ALL SELECT sum(id) FROM items "
      "UNION ALL SELECT max(id)+100 FROM items LIMIT 1 OFFSET 2");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(103, rows[0][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, EvaluatesCompoundLimitOnce) {
  const CustomEnvironment custom;
  const auto rows = Execute(
      "SELECT count() FROM items UNION ALL SELECT sum(id) FROM items LIMIT volatile_counter()",
      custom.Binder(), custom.Vm());
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
  EXPECT_EQ(1U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, BypassesEveryAggregateArmAndOffsetForLimitZero) {
  const CustomEnvironment custom;
  EXPECT_TRUE(Execute("SELECT count(failing()) FROM items GROUP BY id "
                      "UNION ALL SELECT count(failing()) FROM items LIMIT 0 OFFSET failing()",
                      custom.Binder(), custom.Vm())
                  .empty());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, ContinuesDemandOneAfterSuppressedAggregateArm) {
  const auto rows =
      Execute("SELECT count() FROM items HAVING 0 UNION SELECT count() FROM items LIMIT 1");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
}

TEST_F(ReadLoweringAggregateIntegration, SkipsAggregateRightArmOfEmptyExcept) {
  const CustomEnvironment custom;
  EXPECT_TRUE(Execute("SELECT 1 WHERE 0 EXCEPT SELECT count(failing()) FROM items", custom.Binder(),
                      custom.Vm())
                  .empty());
  EXPECT_EQ(0U, callback_count);
}

TEST_F(ReadLoweringAggregateIntegration, SkipsAggregateRightArmOfEmptyIntersect) {
  const CustomEnvironment custom;
  EXPECT_TRUE(Execute("SELECT 1 WHERE 0 INTERSECT SELECT count(failing()) FROM items",
                      custom.Binder(), custom.Vm())
                  .empty());
  EXPECT_EQ(0U, callback_count);
}

TEST(ReadLowering, BuildsCanonicalLookupAndRetainedProbeForEachAutomaticJoinedSource) {
  const auto catalog = TestCatalog();
  const PhysicalPlan physical = OptimizeOrThrow(
      "SELECT a.id,b.id,c.id,b.score,c.score FROM items a JOIN items b ON b.name=a.name "
      "JOIN items c ON c.name=b.name",
      catalog, BindEnvironment::Core(),
      OptimizerOptions{.allow_file_backed_automatic_indexes = true});
  ASSERT_EQ(2, std::ranges::count_if(physical.nodes(), [](const PhysicalNode& node) {
              return std::holds_alternative<PhysicalAutomaticIndexNode>(node.payload);
            }));
  auto lowered = LowerPlan(physical);
  ASSERT_TRUE(lowered.has_value());
  const BytecodeProgram& program = *lowered;
  ASSERT_EQ(2U, program.relations().size());
  ASSERT_EQ(5U, program.cursors().size());
  const auto kinds = InstructionKinds(program);
  for (const InstructionKind kind :
       {InstructionKind::kCheckRelationLookup, InstructionKind::kSealRelation,
        InstructionKind::kSeekRelationPrefix, InstructionKind::kNextRelationPrefix}) {
    EXPECT_EQ(2, std::ranges::count(kinds, kind));
  }
  std::vector<const SeekRelationPrefixInstruction*> probes;
  for (const Instruction& instruction : program.instructions()) {
    if (const auto* seek = std::get_if<SeekRelationPrefixInstruction>(&instruction);
        seek != nullptr) {
      probes.push_back(seek);
    }
    if (const auto* rowid = std::get_if<ReadRowIdInstruction>(&instruction); rowid != nullptr) {
      EXPECT_EQ(CursorStorageKind::kRowIdTable, program.cursor(rowid->cursor).storage);
    }
  }

  ASSERT_EQ(2U, probes.size());
  EXPECT_LE(probes[0]->first_key.value() + probes[0]->key_count, probes[1]->first_key.value());
  for (const SeekRelationPrefixInstruction* probe : probes) {
    EXPECT_EQ(1U, probe->key_count);
    const ReadCursorDescriptor& view = program.cursor(probe->cursor);
    EXPECT_EQ(CursorStorageKind::kEphemeralIndex, view.storage);
    EXPECT_EQ(RootPageNumber(0), view.root_page);
    ASSERT_TRUE(view.relation.has_value());
    EXPECT_EQ(probe->cursor, program.relation_borrower(*view.relation));
    const OrderingRecordDescriptor& owner = program.relation(*view.relation);
    EXPECT_EQ(RelationPurpose::kLookup, owner.relation_purpose);
    EXPECT_EQ(3U, owner.field_count);
    EXPECT_EQ(owner.field_count, owner.key_field_count);
    EXPECT_EQ(owner.field_count, owner.key_columns.size());
    EXPECT_TRUE(view.index_columns.empty());
    const auto next =
        std::ranges::find_if(program.instructions(), [&](const Instruction& instruction) {
          const auto* operation = std::get_if<NextRelationPrefixInstruction>(&instruction);
          return operation != nullptr && operation->cursor == probe->cursor;
        });
    ASSERT_NE(program.instructions().end(), next);
    const auto& advance = std::get<NextRelationPrefixInstruction>(*next);
    EXPECT_EQ(probe->first_key, advance.first_key);
    EXPECT_EQ(probe->key_count, advance.key_count);
  }
}

TEST(ReadLowering, PreservesLiveOuterAutomaticRecordsWithOneSharedCachePage) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, ReadCompatibilityFixturePath().string(),
                                     PagerOptions{.cache_capacity_pages = 1}));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 142}));
  const auto factory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager,
      TemporaryStorageOptions{.mode = TemporaryStoreMode::kFile,
                              .sorter_memory_threshold = ByteCount{1024}}));
  const BytecodeProgram program = LowerOrThrow(
      "SELECT a.id,b.name,c.name,b.score,c.score,b.name FROM items a "
      "JOIN items b ON b.score IS a.score JOIN items c ON c.score IS b.score "
      "WHERE a.id<=3 ORDER BY a.id",
      catalog, BindEnvironment::Core(),
      OptimizerOptions{.allow_file_backed_automatic_indexes = factory.file_spill_enabled()});
  ASSERT_EQ(2U, program.relations().size());
  const auto rows =
      ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(), &factory);
  ASSERT_EQ(3U, rows.size());
  const std::array<std::string_view, 3> names{"alpha", "beta", "gamma"};
  for (std::size_t row = 0; row < rows.size(); ++row) {
    EXPECT_EQ(static_cast<std::int64_t>(row + 1U), rows[row][0].integer_value());
    for (const std::size_t field : {1U, 2U, 5U}) {
      EXPECT_EQ(names[row], TextBytes(rows[row][field]));
    }
    for (const std::size_t field : {3U, 4U}) {
      EXPECT_EQ(row == 2U ? SqlValueType::kNull : SqlValueType::kReal, rows[row][field].type());
    }
  }
}

TEST(ReadLowering, CompilesSixtyFourDeclaredSourcesWithBoundedAutomaticAdmission) {
  std::string sql = "SELECT t0.id,t63.name FROM items t0";
  for (std::size_t source = 1; source < 64U; ++source) {
    sql += " JOIN items t" + std::to_string(source) + " ON t" + std::to_string(source) + ".name=t" +
           std::to_string(source - 1U) + ".name";
  }
  sql += " LIMIT 1";
  const PhysicalPlan physical =
      OptimizeOrThrow(sql, TestCatalog(), BindEnvironment::Core(),
                      OptimizerOptions{.allow_file_backed_automatic_indexes = true});
  const auto automatic = static_cast<std::size_t>(
      std::ranges::count_if(physical.nodes(), [](const PhysicalNode& node) {
        return std::holds_alternative<PhysicalAutomaticIndexNode>(node.payload);
      }));
  ASSERT_GT(automatic, 0U);
  const auto lowered = LowerPlan(physical);
  ASSERT_TRUE(lowered.has_value());
  EXPECT_EQ(automatic, lowered->relations().size());
  EXPECT_EQ(64U + automatic, lowered->cursors().size());
}

TEST(ReadLowering, KeepsNestedJoinedIndexPrefixesInDisjointRegisterBlocks) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 149}));
  const BytecodeProgram program = LowerOrThrow(
      "SELECT a.id,b.id,c.id FROM items a JOIN items b ON b.category=a.category "
      "JOIN items c ON c.category=b.category AND c.score=b.score WHERE a.id=1",
      catalog);
  std::vector<const SeekIndexInstruction*> seeks;
  for (const Instruction& instruction : program.instructions()) {
    if (const auto* seek = std::get_if<SeekIndexInstruction>(&instruction); seek != nullptr) {
      seeks.push_back(seek);
    }
  }
  ASSERT_EQ(2U, seeks.size());
  EXPECT_NE(seeks[0]->cursor, seeks[1]->cursor);
  EXPECT_EQ(1U, seeks[0]->key_count);
  EXPECT_EQ(2U, seeks[1]->key_count);
  EXPECT_LE(seeks[0]->first_key.value() + seeks[0]->key_count, seeks[1]->first_key.value());
  ASSERT_EQ(3U, program.cursors().size());
  EXPECT_EQ(CursorStorageKind::kIndex, program.cursors()[1].storage);
  EXPECT_EQ(CursorStorageKind::kIndex, program.cursors()[2].storage);
  const auto rows = ExecuteRows(program, *pager, catalog->version().generation);
  ASSERT_EQ(1U, rows.size());
  ASSERT_EQ(3U, rows[0].size());
  for (const SqlValue& value : rows[0]) {
    EXPECT_EQ(1, value.integer_value());
  }
}

TEST(ReadLowering, RetainsNestedRangeEndsBeforeBackedgesInDisjointSourceBlocks) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, JoinedIndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 173}));
  const auto memory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  const BytecodeProgram program = LowerOrThrow(
      "SELECT p.id,b.id,c.id,b.payload,c.payload FROM probe_rows p JOIN key_rows b "
      "ON b.numeric_key>=p.numeric_key AND b.numeric_key<=3 "
      "JOIN key_rows c ON c.numeric_key>b.numeric_key AND c.numeric_key<=4 "
      "ORDER BY p.id,b.id,c.id",
      catalog);
  std::vector<const SeekIndexInstruction*> seeks;
  for (const Instruction& instruction : program.instructions()) {
    if (const auto* seek = std::get_if<SeekIndexInstruction>(&instruction); seek != nullptr) {
      seeks.push_back(seek);
    }
  }
  ASSERT_EQ(2U, seeks.size());
  ASSERT_EQ(5U, program.cursors().size());
  EXPECT_LE(seeks[0]->first_key.value() + seeks[0]->key_count, seeks[1]->first_key.value());
  for (const SeekIndexInstruction* seek : seeks) {
    ASSERT_EQ(1U, seek->key_count);
    const auto start =
        std::ranges::find_if(program.instructions(), [&](const Instruction& instruction) {
          const auto* candidate = std::get_if<SeekIndexInstruction>(&instruction);
          return candidate != nullptr && candidate->cursor == seek->cursor;
        });
    const auto end =
        std::ranges::find_if(program.instructions(), [&](const Instruction& instruction) {
          const auto* candidate = std::get_if<CheckIndexRangeInstruction>(&instruction);
          return candidate != nullptr && candidate->cursor == seek->cursor;
        });
    ASSERT_NE(program.instructions().end(), end);
    const auto& check = std::get<CheckIndexRangeInstruction>(*end);
    EXPECT_EQ(seek->first_key, check.first_key);
    EXPECT_EQ(1U, check.key_count);
    const auto start_position =
        static_cast<std::size_t>(std::ranges::distance(program.instructions().begin(), start));
    const auto end_position =
        static_cast<std::size_t>(std::ranges::distance(program.instructions().begin(), end));
    ASSERT_LT(start_position, end_position);
    EXPECT_TRUE(std::ranges::any_of(
        program.instructions().subspan(start_position + 1U, end_position - start_position - 1U),
        [&](const Instruction& instruction) {
          const auto* copy = std::get_if<CopyInstruction>(&instruction);
          return copy != nullptr && copy->output == seek->first_key;
        }));
    const auto next =
        std::ranges::find_if(program.instructions(), [&](const Instruction& instruction) {
          const auto* candidate = std::get_if<NextInstruction>(&instruction);
          return candidate != nullptr && candidate->cursor == seek->cursor;
        });
    ASSERT_NE(program.instructions().end(), next);
    EXPECT_EQ(end_position, std::get<NextInstruction>(*next).next_target.value());
  }
  constexpr std::array<std::array<std::int64_t, 3>, 15> kExpected{{
      {1, 1, 4},
      {1, 1, 5},
      {1, 1, 7},
      {1, 2, 4},
      {1, 2, 5},
      {1, 2, 7},
      {1, 3, 4},
      {1, 3, 5},
      {1, 3, 7},
      {1, 4, 5},
      {1, 4, 7},
      {1, 5, 7},
      {2, 4, 5},
      {2, 4, 7},
      {2, 5, 7},
  }};
  const auto rows =
      ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(kExpected.size(), rows.size());
  for (std::size_t row = 0; row < rows.size(); ++row) {
    ASSERT_EQ(5U, rows[row].size());
    for (std::size_t column = 0; column < kExpected[row].size(); ++column) {
      EXPECT_EQ(kExpected[row][column], rows[row][column].integer_value());
    }
    EXPECT_EQ(SqlValueType::kText, rows[row][3].type());
    EXPECT_EQ(SqlValueType::kText, rows[row][4].type());
  }
}

TEST(ReadLowering, ExecutesJoinedCoveringPrefixesAndCompletePreservedReplay) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, JoinedIndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 151}));
  const auto memory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  struct Case {
    std::string_view join;
    std::int64_t rows;
    std::int64_t left;
    std::int64_t right;
  };
  const std::array cases{
      Case{.join = "JOIN", .rows = 5, .left = 5, .right = 5},
      Case{.join = "LEFT JOIN", .rows = 8, .left = 8, .right = 5},
      Case{.join = "RIGHT JOIN", .rows = 12, .left = 5, .right = 12},
      Case{.join = "FULL JOIN", .rows = 15, .left = 8, .right = 12},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.join);
    const BytecodeProgram program =
        LowerOrThrow("SELECT count(),count(p.rowid),count(b.rowid) FROM probe_rows p " +
                         std::string{test_case.join} + " key_rows b ON b.text_key=p.text_key",
                     catalog);
    ASSERT_EQ(2U, program.cursors().size());
    EXPECT_EQ(CursorStorageKind::kIndex, program.cursors()[1].storage);
    const auto rows =
        ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
    ASSERT_EQ(1U, rows.size());
    ASSERT_EQ(3U, rows[0].size());
    EXPECT_EQ(test_case.rows, rows[0][0].integer_value());
    EXPECT_EQ(test_case.left, rows[0][1].integer_value());
    EXPECT_EQ(test_case.right, rows[0][2].integer_value());
  }
}

TEST(ReadLowering, KeepsNestedNoncoveringSourcesAndTheirRetainedPrefixesDistinct) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 157}));
  const BytecodeProgram program = LowerOrThrow(
      "SELECT a.id,b.payload,c.payload FROM items a JOIN items b ON b.category=a.category "
      "JOIN items c ON c.category=b.category AND c.score=b.score WHERE a.id=1",
      catalog);
  std::vector<const SeekIndexInstruction*> seeks;
  std::vector<const SeekTableRowIdInstruction*> lookups;
  for (const Instruction& instruction : program.instructions()) {
    if (const auto* seek = std::get_if<SeekIndexInstruction>(&instruction); seek != nullptr) {
      seeks.push_back(seek);
    } else if (const auto* lookup = std::get_if<SeekTableRowIdInstruction>(&instruction);
               lookup != nullptr) {
      lookups.push_back(lookup);
    }
  }
  ASSERT_EQ(2U, seeks.size());
  EXPECT_EQ(1U, seeks[0]->key_count);
  EXPECT_EQ(2U, seeks[1]->key_count);
  EXPECT_LE(seeks[0]->first_key.value() + seeks[0]->key_count, seeks[1]->first_key.value());
  ASSERT_EQ(2U, lookups.size());
  EXPECT_NE(lookups[0]->cursor, lookups[1]->cursor);
  EXPECT_EQ(lookups[0]->key, lookups[1]->key);
  ASSERT_EQ(5U, program.cursors().size());
  EXPECT_EQ(CursorStorageKind::kIndex, program.cursors()[1].storage);
  EXPECT_EQ(CursorStorageKind::kRowIdTable, program.cursors()[2].storage);
  EXPECT_EQ(CursorStorageKind::kIndex, program.cursors()[3].storage);
  EXPECT_EQ(CursorStorageKind::kRowIdTable, program.cursors()[4].storage);
  const auto rows = ExecuteRows(program, *pager, catalog->version().generation);
  ASSERT_EQ(1U, rows.size());
  ASSERT_EQ(3U, rows[0].size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(128U, BlobBytes(rows[0][1]).size());
  EXPECT_EQ(128U, BlobBytes(rows[0][2]).size());
}

TEST(ReadLowering, ExecutesJoinedNoncoveringPrefixesAndCompleteTableReplay) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, JoinedIndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 159}));
  const auto memory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  struct Case {
    std::string_view join;
    std::int64_t rows;
    std::int64_t left;
    std::int64_t right;
    bool replay = false;
  };
  const std::array cases{
      Case{.join = "JOIN", .rows = 5, .left = 5, .right = 5},
      Case{.join = "LEFT JOIN", .rows = 8, .left = 8, .right = 5},
      Case{.join = "RIGHT JOIN", .rows = 12, .left = 5, .right = 12, .replay = true},
      Case{.join = "FULL JOIN", .rows = 15, .left = 8, .right = 12, .replay = true},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.join);
    const BytecodeProgram program = LowerOrThrow(
        "SELECT count(),count(p.rowid),count(b.rowid),count(b.payload) "
        "FROM probe_rows p " +
            std::string{test_case.join} + " key_rows b ON b.text_key=p.text_key",
        catalog);
    ASSERT_EQ(3U, program.cursors().size());
    EXPECT_EQ(CursorStorageKind::kIndex, program.cursors()[1].storage);
    EXPECT_EQ(CursorStorageKind::kRowIdTable, program.cursors()[2].storage);
    const auto table_rewinds =
        std::ranges::count_if(program.instructions(), [](const Instruction& instruction) {
          const auto* rewind = std::get_if<RewindInstruction>(&instruction);
          return rewind != nullptr && rewind->cursor == CursorId{2};
        });
    EXPECT_EQ(test_case.replay ? 1 : 0, table_rewinds);
    const auto rows =
        ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
    ASSERT_EQ(1U, rows.size());
    ASSERT_EQ(4U, rows[0].size());
    EXPECT_EQ(test_case.rows, rows[0][0].integer_value());
    EXPECT_EQ(test_case.left, rows[0][1].integer_value());
    EXPECT_EQ(test_case.right, rows[0][2].integer_value());
    EXPECT_EQ(test_case.right, rows[0][3].integer_value());
  }
}

TEST(ReadLowering, ExecutesEmptyJoinedNoncoveringSourcesAndReplay) {
  test::WritePagerFixedVfs vfs{false};
  InitializeIndexedMutationDatabase(vfs);
  auto pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  RequireStatus(pager->BeginRead());
  const auto catalog = IndexedMutationCatalog();
  const auto memory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  for (const std::string_view join : {"JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL JOIN"}) {
    for (const std::string_view operation : {"=", ">", "<"}) {
      SCOPED_TRACE(::testing::Message() << join << " " << operation);
      const BytecodeProgram program =
          LowerOrThrow("SELECT a.Name,b.Score FROM Items a " + std::string{join} +
                           " Items b ON b.Name" + std::string{operation} + "a.Name",
                       catalog);
      ASSERT_EQ(3U, program.cursors().size());
      EXPECT_EQ(CursorStorageKind::kIndex, program.cursors()[1].storage);
      EXPECT_EQ(CursorStorageKind::kRowIdTable, program.cursors()[2].storage);
      EXPECT_TRUE(ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(),
                              &memory)
                      .empty());
    }
  }
}

TEST(ReadLowering, ReplaysNoncoveringTablesInNativeOrderForPrivateAndAggregateConsumers) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, JoinedIndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 161}));
  const auto memory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  for (const std::string_view predicate : {"b.text_key=p.text_key", "b.numeric_key>p.id"}) {
    for (const std::string_view join : {"RIGHT JOIN", "FULL JOIN"}) {
      SCOPED_TRACE(::testing::Message() << join << " " << predicate);
      const std::string source = " FROM empty_probe_rows p " + std::string{join} +
                                 " key_rows b ON " + std::string{predicate};
      const BytecodeProgram aggregate =
          LowerOrThrow("SELECT group_concat(b.payload)" + source, catalog);
      const auto rows = ExecuteRows(aggregate, *pager, catalog->version().generation,
                                    VmEnvironment::Core(), &memory);
      ASSERT_EQ(1U, rows.size());
      EXPECT_EQ(
          "one,zero-one,zeros-one,alpha,upper-alpha,null-one,null-two,"
          "beta-null,beta-five,same-one,same-two,null-three",
          TextBytes(rows[0][0]));
      const BytecodeProgram scalar =
          LowerOrThrow("SELECT (SELECT b.payload" + source + ")", catalog);
      const auto first = ExecuteRows(scalar, *pager, catalog->version().generation,
                                     VmEnvironment::Core(), &memory);
      ASSERT_EQ(1U, first.size());
      EXPECT_EQ("one", TextBytes(first[0][0]));
    }
  }
}

TEST(ReadLowering, ReplaysUnboundedCoveringRangeInputForPrivateAndAggregateConsumers) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, JoinedIndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 175}));
  const auto memory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  for (const std::string_view join : {"RIGHT JOIN", "FULL JOIN"}) {
    const std::string source =
        " FROM empty_probe_rows p " + std::string{join} + " key_rows b ON b.numeric_key>p.id";
    const BytecodeProgram aggregate = LowerOrThrow("SELECT group_concat(b.id)" + source, catalog);
    ASSERT_EQ(2U, aggregate.cursors().size());
    EXPECT_EQ(CursorStorageKind::kIndex, aggregate.cursors()[1].storage);
    const auto rows = ExecuteRows(aggregate, *pager, catalog->version().generation,
                                  VmEnvironment::Core(), &memory);
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ("10,11,9,7,5,4,1,2,3,12,6,8", TextBytes(rows[0][0]));
    const BytecodeProgram scalar = LowerOrThrow("SELECT (SELECT b.id" + source + ")", catalog);
    const auto first =
        ExecuteRows(scalar, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
    ASSERT_EQ(1U, first.size());
    EXPECT_EQ(10, first[0][0].integer_value());
  }
}

TEST(ReadLowering, ReusesOneRowidScratchAcrossJoinedAndOrdinaryNoncoveringAggregateCores) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, JoinedIndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 163}));
  const BytecodeProgram program = LowerOrThrow(
      "SELECT count(b.payload) FROM probe_rows p JOIN key_rows b ON b.text_key=p.text_key "
      "UNION ALL SELECT count(payload) FROM key_rows WHERE text_key='alpha'",
      catalog);
  std::optional<RegisterId> rowid;
  std::size_t lookups = 0;
  for (const Instruction& instruction : program.instructions()) {
    if (const auto* lookup = std::get_if<SeekTableRowIdInstruction>(&instruction);
        lookup != nullptr) {
      if (rowid.has_value()) {
        EXPECT_EQ(*rowid, lookup->key);
      } else {
        rowid = lookup->key;
      }
      ++lookups;
    }
  }
  EXPECT_EQ(3U, lookups);
  const auto rows = ExecuteRows(program, *pager, catalog->version().generation);
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ(5, rows[0][0].integer_value());
  EXPECT_EQ(2, rows[1][0].integer_value());
}

TEST(ReadLowering, SharesRowidScratchAcrossJoinedRangeOrdinaryRangeAndIndexedMinMax) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, JoinedIndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 177}));
  const BytecodeProgram program = LowerOrThrow(
      "SELECT count(b.payload) FROM probe_rows p JOIN key_rows b "
      "ON b.numeric_key>=p.numeric_key AND b.numeric_key<=5 "
      "UNION ALL SELECT count(payload) FROM key_rows WHERE text_key>'alpha' "
      "UNION ALL SELECT max(real_key) FROM key_rows WHERE numeric_key=1 AND payload!='none'",
      catalog);
  std::optional<RegisterId> rowid;
  std::size_t lookups = 0;
  for (const Instruction& instruction : program.instructions()) {
    if (const auto* lookup = std::get_if<SeekTableRowIdInstruction>(&instruction);
        lookup != nullptr) {
      if (rowid.has_value()) {
        EXPECT_EQ(*rowid, lookup->key);
      } else {
        rowid = lookup->key;
      }
      ++lookups;
    }
  }
  EXPECT_EQ(7U, lookups);
  const auto rows = ExecuteRows(program, *pager, catalog->version().generation);
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(12, rows[0][0].integer_value());
  EXPECT_EQ(4, rows[1][0].integer_value());
  EXPECT_EQ(SqlValueType::kReal, rows[2][0].type());
  EXPECT_EQ(1.0, rows[2][0].real_value());
}

TEST(ReadLowering, ExecutesSixtyFourNoncoveringSourcesWithOneConsumerAndRowidScratch) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 165}));
  constexpr std::size_t kSources = 64;
  for (const bool range : {false, true}) {
    SCOPED_TRACE(range);
    std::string sql = "SELECT t0.id,t63.id,t63.payload FROM items t0";
    for (std::size_t source = 1; source < kSources; ++source) {
      const std::string current = "t" + std::to_string(source);
      const std::string previous = "t" + std::to_string(source - 1U);
      sql += " JOIN items " + current + " ON " + current + ".category" + (range ? ">=" : "=") +
             previous + ".category";
      if (range) {
        sql += " AND " + current + ".category<=" + previous + ".category";
      }
      sql += " AND " + current + ".payload=" + previous + ".payload";
    }
    sql += " WHERE t0.id=1";
    const BytecodeProgram program = LowerOrThrow(sql, catalog);
    const auto kinds = InstructionKinds(program);
    EXPECT_EQ(kSources * 2U - 1U, program.cursors().size());
    EXPECT_EQ(kSources - 1U, std::ranges::count(kinds, InstructionKind::kSeekIndex));
    EXPECT_EQ(kSources - 1U, std::ranges::count(kinds, InstructionKind::kSeekTableRowId));
    EXPECT_EQ(1, std::ranges::count(kinds, InstructionKind::kResultRow));
    EXPECT_LE(program.instructions().size(), kSources * 32U + 128U);
    std::optional<RegisterId> rowid;
    for (const Instruction& instruction : program.instructions()) {
      if (const auto* lookup = std::get_if<SeekTableRowIdInstruction>(&instruction);
          lookup != nullptr) {
        if (rowid.has_value()) {
          EXPECT_EQ(*rowid, lookup->key);
        } else {
          rowid = lookup->key;
        }
      }
    }
    const auto rows = ExecuteRows(program, *pager, catalog->version().generation);
    ASSERT_EQ(1U, rows.size());
    ASSERT_EQ(3U, rows[0].size());
    EXPECT_EQ(1, rows[0][0].integer_value());
    EXPECT_EQ(1, rows[0][1].integer_value());
    EXPECT_EQ(128U, BlobBytes(rows[0][2]).size());
  }
}

void ExerciseJoinedIndexSuffixFailures(std::string_view predicate) {
  PosixVfs source_vfs;
  auto source = TakeValue(Pager::Open(source_vfs, JoinedIndexFixturePath().string()));
  RequireStatus(source->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*source, CatalogLoadOptions{.generation = 167}));
  const BytecodeProgram program = LowerOrThrow(
      "SELECT p.id,b.payload FROM probe_rows p JOIN key_rows b "
      "ON " +
          std::string{predicate} + " WHERE p.id=2",
      catalog);
  ASSERT_EQ(3U, program.cursors().size());
  ASSERT_EQ(CursorStorageKind::kIndex, program.cursors()[1].storage);
  const PageNumber root{program.cursors()[1].root_page.value()};
  ASSERT_EQ(ByteCount{512}, source->page_size());
  const auto geometry = TakeValue(BtreePageGeometry::Create(ByteCount{512}, ByteCount{512}));
  const auto size = std::filesystem::file_size(JoinedIndexFixturePath());
  ASSERT_LE(size, test::kWritePagerFileCapacity);
  ByteBuffer image{ByteCount{static_cast<std::size_t>(size)}};
  {
    std::ifstream input(JoinedIndexFixturePath(), std::ios::binary);
    ASSERT_TRUE(input);
    input.read(reinterpret_cast<char*>(image.mutable_view().data()),
               static_cast<std::streamsize>(size));
    ASSERT_TRUE(input);
  }
  enum class Fault {
    None,
    DanglingRowid,
    NonintegerRowid,
    MalformedRecord,
  };
  for (const Fault fault :
       {Fault::None, Fault::DanglingRowid, Fault::NonintegerRowid, Fault::MalformedRecord}) {
    SCOPED_TRACE(static_cast<int>(fault));
    ByteBuffer corrupted = image.Clone();
    const std::size_t page_offset = (root.value() - 1U) * 512U;
    const MutableByteView bytes = corrupted.mutable_view().subspan(page_offset, 512U);
    const auto page = TakeValue(BtreePageView::Parse(bytes, root, geometry));
    ASSERT_EQ(BtreePageType::kLeafIndex, page.type());
    bool patched = false;
    for (std::size_t index = 0; index < page.cell_count(); ++index) {
      const auto cell = TakeValue(page.cell(index));
      const ByteView payload = cell.local_payload();
      const auto record = TakeValue(RecordView::Parse(payload));
      ASSERT_EQ(3U, record.field_count());
      if (TakeValue(record.field(2)).integer_value() != 4) {
        continue;
      }
      const auto offset = static_cast<std::size_t>(payload.data() - bytes.data());
      ASSERT_EQ(std::byte{1}, payload[record.header_size().value() - 1U]);
      ASSERT_EQ(std::byte{4}, payload.back());
      if (fault == Fault::DanglingRowid) {
        bytes[offset + payload.size() - 1U] = std::byte{127};
      } else if (fault == Fault::NonintegerRowid) {
        bytes[offset + record.header_size().value() - 1U] = std::byte{15};
      } else if (fault == Fault::MalformedRecord) {
        bytes[offset + record.header_size().value() - 1U] = std::byte{7};
      }
      patched = true;
      break;
    }
    ASSERT_TRUE(patched);
    test::WritePagerFixedVfs vfs{false};
    vfs.LoadDatabase(corrupted.view());
    auto pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
    RequireStatus(pager->BeginRead());
    Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
    RequireStatus(
        vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
    if (fault == Fault::None) {
      for (const std::string_view payload : {"alpha", "upper-alpha"}) {
        ASSERT_EQ(VmStep::kRow, TakeValue(vm.Step()));
        EXPECT_EQ(2, vm.row()[0].integer_value());
        EXPECT_EQ(payload, TextBytes(vm.row()[1]));
      }
      EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
    } else {
      const auto failed = vm.Step();
      ASSERT_FALSE(failed.has_value());
      EXPECT_EQ(ErrorCode::kCorruption, failed.error().code());
      EXPECT_TRUE(vm.row().empty());
    }
    RequireStatus(vm.DetachExecutionContext());
    RequireStatus(pager->EndRead());
  }
}

TEST(ReadLowering, RejectsDanglingNonintegerAndMalformedJoinedIndexSuffixes) {
  for (const std::string_view predicate :
       {"b.text_key=p.text_key", "b.text_key>=p.text_key AND b.text_key<=p.text_key"}) {
    SCOPED_TRACE(predicate);
    ExerciseJoinedIndexSuffixFailures(predicate);
  }
}

TEST(ReadLowering, SharesOneConsumerAcrossSixtyFourIndexedFullSources) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 153}));
  constexpr std::size_t kSources = 64;
  std::string sql = "SELECT 1 FROM items t0";
  for (std::size_t source = 1; source < kSources; ++source) {
    sql += " FULL JOIN items t" + std::to_string(source) + " ON t" + std::to_string(source) +
           ".category=t" + std::to_string(source - 1U) + ".category";
  }
  sql += " LIMIT 0";
  const BytecodeProgram program = LowerOrThrow(sql, catalog);
  const auto kinds = InstructionKinds(program);
  EXPECT_EQ(kSources - 1U,
            static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kSeekIndex)));
  EXPECT_EQ(kSources - 1U,
            static_cast<std::size_t>(std::ranges::count(kinds, InstructionKind::kOpenRelation)));
  EXPECT_EQ(1, std::ranges::count(kinds, InstructionKind::kResultRow));
  EXPECT_LE(program.instructions().size(), kSources * kSources * 2U + kSources * 96U + 128U);
}

TEST(ReadLowering, ExecutesStableSortedGroupedAggregates) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 107}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));

  const BytecodeProgram program = LowerOrThrow(
      "SELECT score IS NULL,count(),min(id) FROM items "
      "GROUP BY score IS NULL ORDER BY 1",
      catalog);
  const std::vector<std::vector<SqlValue>> rows =
      ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);

  ASSERT_EQ(2U, rows.size());
  ASSERT_EQ(3U, rows[0].size());
  EXPECT_EQ(0, rows[0][0].integer_value());
  EXPECT_EQ(2, rows[0][1].integer_value());
  EXPECT_EQ(1, rows[0][2].integer_value());
  ASSERT_EQ(3U, rows[1].size());
  EXPECT_EQ(1, rows[1][0].integer_value());
  EXPECT_EQ(1, rows[1][1].integer_value());
  EXPECT_EQ(3, rows[1][2].integer_value());

  const BytecodeProgram keys_only =
      LowerOrThrow("SELECT score IS NULL FROM items GROUP BY score IS NULL ORDER BY 1", catalog);
  const auto key_rows =
      ExecuteRows(keys_only, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(2U, key_rows.size());
  EXPECT_EQ(0, key_rows[0][0].integer_value());
  EXPECT_EQ(1, key_rows[1][0].integer_value());
}

TEST(ReadLowering, ExecutesOrderedGroupedAggregatesAndStopsAfterLimit) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 109}));
  const CustomEnvironment custom;

  callback_count = 0;
  const BytecodeProgram program = LowerOrThrow(
      "SELECT flag,count(volatile_counter()) FROM items "
      "GROUP BY flag ORDER BY flag LIMIT 1",
      catalog, custom.Binder());
  const std::vector<std::vector<SqlValue>> rows =
      ExecuteRows(program, *pager, catalog->version().generation, custom.Vm());

  ASSERT_EQ(1U, rows.size());
  ASSERT_EQ(2U, rows[0].size());
  EXPECT_EQ(0, rows[0][0].integer_value());
  EXPECT_EQ(2048, rows[0][1].integer_value());
  EXPECT_EQ(2048U, callback_count);
}

TEST(ReadLowering, ClosesPrivateOrderedGroupIndexesOnEarlyOutput) {
  PosixVfs vfs;
  auto pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const auto catalog = TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 109}));
  const CustomEnvironment custom;
  callback_count = 0;
  const BytecodeProgram program = LowerOrThrow(
      "SELECT (SELECT count(volatile_counter()) FROM items GROUP BY flag ORDER BY flag LIMIT 1)",
      catalog, custom.Binder());
  const auto rows = ExecuteRows(program, *pager, catalog->version().generation, custom.Vm());
  ASSERT_EQ(1U, rows.size());
  ASSERT_EQ(1U, rows[0].size());
  EXPECT_EQ(2048, rows[0][0].integer_value());
  EXPECT_EQ(2048U, callback_count);
}

TEST(ReadLowering, PreservesGroupedRepresentativesAndSelectedKeys) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  test::WritePagerFixedVfs vfs{false};
  InitializeMutationDatabase(vfs);
  const BytecodeProgram insert =
      LowerMutationOrThrow("INSERT INTO Items(id,Name,Score) VALUES(?1,?2,?3)", catalog);
  const auto add_row = [&](std::int64_t id, std::string name, SqlValue score) {
    const std::array parameters{
        SqlValue::Integer(id),
        SqlValue::Text(std::move(name)),
        std::move(score),
    };
    const MutationOutcome outcome = TakeValue(ExecuteMutationProgram(insert, vfs, parameters));
    EXPECT_EQ(1U, outcome.changes);
  };
  add_row(1, "g", SqlValue::Integer(1));
  add_row(2, "g", SqlValue::Integer(0));
  add_row(3, "n", SqlValue{});
  add_row(4, "n", SqlValue{});
  add_row(5, "t", SqlValue::Integer(5));
  add_row(6, "t", SqlValue::Integer(5));
  add_row(7, "A", SqlValue::Integer(2));
  add_row(8, "a", SqlValue::Integer(1));

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));

  const BytecodeProgram ordinary = LowerOrThrow(
      "SELECT Name,id,count() FROM Items WHERE id<=6 GROUP BY Name ORDER BY Name", catalog);
  const auto ordinary_rows =
      ExecuteRows(ordinary, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(3U, ordinary_rows.size());
  EXPECT_EQ("g", TextBytes(ordinary_rows[0][0]));
  EXPECT_EQ(1, ordinary_rows[0][1].integer_value());
  EXPECT_EQ("n", TextBytes(ordinary_rows[1][0]));
  EXPECT_EQ(3, ordinary_rows[1][1].integer_value());
  EXPECT_EQ("t", TextBytes(ordinary_rows[2][0]));
  EXPECT_EQ(5, ordinary_rows[2][1].integer_value());

  const BytecodeProgram minimum_then_maximum = LowerOrThrow(
      "SELECT Name,id,min(Score),max(Score) FROM Items "
      "WHERE id<=6 GROUP BY Name ORDER BY Name",
      catalog);
  const auto minimum_then_maximum_rows = ExecuteRows(
      minimum_then_maximum, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(3U, minimum_then_maximum_rows.size());
  EXPECT_EQ(1, minimum_then_maximum_rows[0][1].integer_value());
  EXPECT_EQ(4, minimum_then_maximum_rows[1][1].integer_value());
  EXPECT_EQ(5, minimum_then_maximum_rows[2][1].integer_value());

  const BytecodeProgram maximum_then_minimum = LowerOrThrow(
      "SELECT Name,id,max(Score),min(Score) FROM Items "
      "WHERE id<=6 GROUP BY Name ORDER BY Name",
      catalog);
  const auto maximum_then_minimum_rows = ExecuteRows(
      maximum_then_minimum, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(3U, maximum_then_minimum_rows.size());
  EXPECT_EQ(2, maximum_then_minimum_rows[0][1].integer_value());
  EXPECT_EQ(4, maximum_then_minimum_rows[1][1].integer_value());
  EXPECT_EQ(5, maximum_then_minimum_rows[2][1].integer_value());

  const BytecodeProgram selected_key = LowerOrThrow(
      "SELECT Name,(Name||'') COLLATE NOCASE,id,min(Score) FROM Items "
      "WHERE id>=7 GROUP BY (Name||'') COLLATE NOCASE",
      catalog);
  const auto selected_key_rows = ExecuteRows(selected_key, *pager, catalog->version().generation,
                                             VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, selected_key_rows.size());
  ASSERT_EQ(4U, selected_key_rows[0].size());
  EXPECT_EQ("a", TextBytes(selected_key_rows[0][0]));
  EXPECT_EQ("A", TextBytes(selected_key_rows[0][1]));
  EXPECT_EQ(8, selected_key_rows[0][2].integer_value());
  EXPECT_EQ(1.0, selected_key_rows[0][3].real_value());

  const BytecodeProgram ordinary_having =
      LowerOrThrow("SELECT Name,count() FROM Items WHERE id<=2 GROUP BY Name HAVING id=2", catalog);
  EXPECT_TRUE(ExecuteRows(ordinary_having, *pager, catalog->version().generation,
                          VmEnvironment::Core(), &memory)
                  .empty());

  const BytecodeProgram minimum_having = LowerOrThrow(
      "SELECT Name,min(Score) FROM Items WHERE id<=2 GROUP BY Name HAVING id=2", catalog);
  const auto minimum_having_rows = ExecuteRows(
      minimum_having, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, minimum_having_rows.size());
  EXPECT_EQ("g", TextBytes(minimum_having_rows[0][0]));
  EXPECT_EQ(0.0, minimum_having_rows[0][1].real_value());
}

TEST(ReadLowering, PreservesGroupedHavingOffsetAndTopNSkipping) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 113}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const CustomEnvironment custom;

  callback_count = 0;
  const BytecodeProgram zero = LowerOrThrow(
      "SELECT volatile_counter(),count(volatile_counter()) FROM items "
      "GROUP BY score IS NULL LIMIT 0",
      catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(zero, *pager, catalog->version().generation, custom.Vm()).empty());
  EXPECT_EQ(0U, callback_count);

  callback_count = 0;
  const BytecodeProgram numeric = LowerOrThrow(
      "SELECT count() FROM items GROUP BY numeric_representative()", catalog, custom.Binder());
  const auto numeric_rows =
      ExecuteRows(numeric, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, numeric_rows.size());
  EXPECT_EQ(3, numeric_rows[0][0].integer_value());
  EXPECT_EQ(3U, callback_count);

  callback_count = 0;
  const BytecodeProgram offset = LowerOrThrow(
      "SELECT volatile_counter(),score IS NULL,count() FROM items "
      "GROUP BY score IS NULL ORDER BY 2 LIMIT 1 OFFSET 1",
      catalog, custom.Binder());
  const auto offset_rows =
      ExecuteRows(offset, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, offset_rows.size());
  EXPECT_EQ(1, offset_rows[0][0].integer_value());
  EXPECT_EQ(1, offset_rows[0][1].integer_value());
  EXPECT_EQ(1, offset_rows[0][2].integer_value());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram having = LowerOrThrow(
      "SELECT volatile_counter(),score IS NULL,count() FROM items "
      "GROUP BY score IS NULL HAVING count()>1 ORDER BY 2",
      catalog, custom.Binder());
  const auto having_rows =
      ExecuteRows(having, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, having_rows.size());
  EXPECT_EQ(1, having_rows[0][0].integer_value());
  EXPECT_EQ(0, having_rows[0][1].integer_value());
  EXPECT_EQ(2, having_rows[0][2].integer_value());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram sorted_limit =
      LowerOrThrow("SELECT count(fail_after_one(name)) FROM items GROUP BY id+0 LIMIT 1", catalog,
                   custom.Binder());
  const auto sorted_limit_rows =
      ExecuteRows(sorted_limit, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, sorted_limit_rows.size());
  EXPECT_EQ(1, sorted_limit_rows[0][0].integer_value());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram top_n = LowerOrThrow(
      "SELECT volatile_counter(),score IS NULL,count() FROM items "
      "GROUP BY score IS NULL ORDER BY count() DESC LIMIT 1",
      catalog, custom.Binder());
  const auto top_n_rows =
      ExecuteRows(top_n, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, top_n_rows.size());
  EXPECT_EQ(1, top_n_rows[0][0].integer_value());
  EXPECT_EQ(0, top_n_rows[0][1].integer_value());
  EXPECT_EQ(2, top_n_rows[0][2].integer_value());
  EXPECT_EQ(1U, callback_count);
}

TEST(ReadLowering, ExecutesGroupedEmptySingleLookupAndIndexAccess) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 127}));

  const BytecodeProgram empty =
      LowerOrThrow("SELECT count() FROM items WHERE 0 GROUP BY score IS NULL", catalog);
  EXPECT_TRUE(ExecuteRows(empty, *pager, catalog->version().generation).empty());

  const BytecodeProgram single = LowerOrThrow("SELECT 1,count() GROUP BY 1", catalog);
  const auto single_rows = ExecuteRows(single, *pager, catalog->version().generation);
  ASSERT_EQ(1U, single_rows.size());
  EXPECT_EQ(1, single_rows[0][0].integer_value());
  EXPECT_EQ(1, single_rows[0][1].integer_value());

  const BytecodeProgram lookup =
      LowerOrThrow("SELECT name,count() FROM items WHERE id=2 GROUP BY name", catalog);
  const auto lookup_rows = ExecuteRows(lookup, *pager, catalog->version().generation);
  ASSERT_EQ(1U, lookup_rows.size());
  EXPECT_EQ("beta", TextBytes(lookup_rows[0][0]));
  EXPECT_EQ(1, lookup_rows[0][1].integer_value());

  RequireStatus(pager->EndRead());
  pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr indexed =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 131}));

  const BytecodeProgram reverse = LowerOrThrow(
      "SELECT flag,count() FROM items GROUP BY flag ORDER BY flag DESC LIMIT 1", indexed);
  const auto reverse_rows = ExecuteRows(reverse, *pager, indexed->version().generation);
  ASSERT_EQ(1U, reverse_rows.size());
  EXPECT_EQ(1, reverse_rows[0][0].integer_value());
  EXPECT_EQ(2048, reverse_rows[0][1].integer_value());

  const BytecodeProgram ranged = LowerOrThrow(
      "SELECT score,count() FROM items WHERE score>=10 AND score<13 "
      "GROUP BY score ORDER BY score DESC",
      indexed);
  const auto ranged_rows = ExecuteRows(ranged, *pager, indexed->version().generation);
  ASSERT_EQ(3U, ranged_rows.size());
  EXPECT_EQ(12, ranged_rows[0][0].integer_value());
  EXPECT_EQ(11, ranged_rows[1][0].integer_value());
  EXPECT_EQ(10, ranged_rows[2][0].integer_value());

  const BytecodeProgram noncovering = LowerOrThrow(
      "SELECT category,payload,count() FROM items "
      "GROUP BY category ORDER BY category LIMIT 1",
      indexed);
  const auto noncovering_rows = ExecuteRows(noncovering, *pager, indexed->version().generation);
  ASSERT_EQ(1U, noncovering_rows.size());
  EXPECT_EQ("category-0001", TextBytes(noncovering_rows[0][0]));
  EXPECT_FALSE(BlobBytes(noncovering_rows[0][1]).empty());
  EXPECT_EQ(1, noncovering_rows[0][2].integer_value());
}

TEST(ReadLowering, ExecutesSpilledSortedGroupingWithOuterOrder) {
  constexpr ByteCount kGroupedSpillThreshold{64U * 1024U};
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, IndexFixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 137}));
  const TemporaryStorageFactory temporary_storage = TakeValue(
      TemporaryStorageFactory::Create(vfs, *pager,
                                      TemporaryStorageOptions{
                                          .mode = TemporaryStoreMode::kFile,
                                          .sorter_memory_threshold = kGroupedSpillThreshold,
                                      }));

  const BytecodeProgram program = LowerOrThrow(
      "SELECT flag+0,count() FROM items "
      "GROUP BY flag+0 ORDER BY count() DESC,1",
      catalog);
  const auto rows = ExecuteRows(program, *pager, catalog->version().generation,
                                VmEnvironment::Core(), &temporary_storage);

  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ(0, rows[0][0].integer_value());
  EXPECT_EQ(2048, rows[0][1].integer_value());
  EXPECT_EQ(1, rows[1][0].integer_value());
  EXPECT_EQ(2048, rows[1][1].integer_value());
}

TEST(ReadLowering, LowersAndExecutesValuesAndStreamingUnionAll) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 103}));
  const CustomEnvironment custom;

  const BytecodeProgram values = LowerOrThrow("VALUES(1,'a'),(2,'b')", catalog);
  EXPECT_TRUE(values.cursors().empty());
  const auto value_rows = ExecuteRows(values, *pager, catalog->version().generation);
  ASSERT_EQ(2U, value_rows.size());
  EXPECT_EQ(1, value_rows[0][0].integer_value());
  EXPECT_EQ("a", TextBytes(value_rows[0][1]));
  EXPECT_EQ(2, value_rows[1][0].integer_value());
  EXPECT_EQ("b", TextBytes(value_rows[1][1]));

  const BytecodeProgram table_values =
      LowerOrThrow("SELECT name FROM items UNION ALL VALUES('tail')", catalog);
  const auto table_value_rows = ExecuteRows(table_values, *pager, catalog->version().generation);
  ASSERT_EQ(4U, table_value_rows.size());
  EXPECT_EQ("alpha", TextBytes(table_value_rows[0][0]));
  EXPECT_EQ("beta", TextBytes(table_value_rows[1][0]));
  EXPECT_EQ("gamma", TextBytes(table_value_rows[2][0]));
  EXPECT_EQ("tail", TextBytes(table_value_rows[3][0]));

  const BytecodeProgram two_tables =
      LowerOrThrow("SELECT name FROM items UNION ALL SELECT name FROM items WHERE id=2", catalog);
  ASSERT_EQ(2U, two_tables.cursors().size());
  const auto two_table_rows = ExecuteRows(two_tables, *pager, catalog->version().generation);
  ASSERT_EQ(4U, two_table_rows.size());
  EXPECT_EQ("alpha", TextBytes(two_table_rows[0][0]));
  EXPECT_EQ("beta", TextBytes(two_table_rows[1][0]));
  EXPECT_EQ("gamma", TextBytes(two_table_rows[2][0]));
  EXPECT_EQ("beta", TextBytes(two_table_rows[3][0]));

  const BytecodeProgram without_rowid =
      LowerOrThrow("SELECT a FROM wr UNION ALL SELECT a FROM wr", catalog);
  const auto without_rowid_rows = ExecuteRows(without_rowid, *pager, catalog->version().generation);
  ASSERT_EQ(4U, without_rowid_rows.size());
  EXPECT_EQ("right", TextBytes(without_rowid_rows[0][0]));
  EXPECT_EQ("left", TextBytes(without_rowid_rows[1][0]));
  EXPECT_EQ("right", TextBytes(without_rowid_rows[2][0]));
  EXPECT_EQ("left", TextBytes(without_rowid_rows[3][0]));

  callback_count = 0;
  const BytecodeProgram row_major = LowerOrThrow(
      "VALUES(volatile_counter(),volatile_counter()),"
      "(volatile_counter(),volatile_counter())",
      catalog, custom.Binder());
  const auto row_major_rows =
      ExecuteRows(row_major, *pager, catalog->version().generation, custom.Vm());
  ASSERT_EQ(2U, row_major_rows.size());
  EXPECT_EQ(1, row_major_rows[0][0].integer_value());
  EXPECT_EQ(2, row_major_rows[0][1].integer_value());
  EXPECT_EQ(3, row_major_rows[1][0].integer_value());
  EXPECT_EQ(4, row_major_rows[1][1].integer_value());
  EXPECT_EQ(4U, callback_count);

  callback_count = 0;
  const BytecodeProgram limited = LowerOrThrow(
      "SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT failing() LIMIT 1", catalog, custom.Binder());
  const auto limited_rows =
      ExecuteRows(limited, *pager, catalog->version().generation, custom.Vm());
  ASSERT_EQ(1U, limited_rows.size());
  EXPECT_EQ(1, limited_rows[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);

  const BytecodeProgram table_limited =
      LowerOrThrow("SELECT name FROM items UNION ALL SELECT 'tail' LIMIT 2", catalog);
  const auto table_limited_rows = ExecuteRows(table_limited, *pager, catalog->version().generation);
  ASSERT_EQ(2U, table_limited_rows.size());
  EXPECT_EQ("alpha", TextBytes(table_limited_rows[0][0]));
  EXPECT_EQ("beta", TextBytes(table_limited_rows[1][0]));

  const BytecodeProgram source_free_offset = LowerOrThrow(
      "SELECT abs(-9223372036854775808) "
      "UNION ALL SELECT 2 LIMIT 1 OFFSET 1",
      catalog);
  const auto source_free_offset_rows =
      ExecuteRows(source_free_offset, *pager, catalog->version().generation);
  ASSERT_EQ(1U, source_free_offset_rows.size());
  EXPECT_EQ(2, source_free_offset_rows[0][0].integer_value());

  const BytecodeProgram table_offset = LowerOrThrow(
      "SELECT abs(-9223372036854775808) FROM items WHERE id=1 "
      "UNION ALL SELECT 2 LIMIT 1 OFFSET 1",
      catalog);
  const auto table_offset_rows = ExecuteRows(table_offset, *pager, catalog->version().generation);
  ASSERT_EQ(1U, table_offset_rows.size());
  EXPECT_EQ(2, table_offset_rows[0][0].integer_value());

  callback_count = 0;
  const BytecodeProgram eager = LowerOrThrow(
      "VALUES(deterministic_failing()),(2) "
      "UNION ALL SELECT 3 LIMIT 1 OFFSET 1",
      catalog, custom.Binder());
  Vm eager_vm = TakeValue(Vm::Create(eager, custom.Vm()));
  RequireStatus(
      eager_vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  const auto eager_failure = eager_vm.Step();
  ASSERT_FALSE(eager_failure.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, eager_failure.error().code());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram filtered = LowerOrThrow(
      "VALUES(deterministic_failing() IS NULL),(2) "
      "UNION ALL SELECT 3 LIMIT 1 OFFSET 1",
      catalog, custom.Binder());
  const auto filtered_rows =
      ExecuteRows(filtered, *pager, catalog->version().generation, custom.Vm());
  ASSERT_EQ(1U, filtered_rows.size());
  EXPECT_EQ(2, filtered_rows[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);

  callback_count = 0;
  const BytecodeProgram zero =
      LowerOrThrow("VALUES(failing()),(2) UNION ALL SELECT 3 LIMIT 0 OFFSET failing()", catalog,
                   custom.Binder());
  EXPECT_TRUE(ExecuteRows(zero, *pager, catalog->version().generation, custom.Vm()).empty());
  EXPECT_EQ(0U, callback_count);

  callback_count = 0;
  const BytecodeProgram later =
      LowerOrThrow("SELECT 1 UNION ALL SELECT failing()", catalog, custom.Binder());
  Vm later_vm = TakeValue(Vm::Create(later, custom.Vm()));
  RequireStatus(
      later_vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  EXPECT_EQ(VmStep::kRow, TakeValue(later_vm.Step()));
  EXPECT_EQ(1, later_vm.row()[0].integer_value());
  const auto later_failure = later_vm.Step();
  ASSERT_FALSE(later_failure.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, later_failure.error().code());
  EXPECT_EQ(1U, callback_count);

  const TemporaryStorageFactory temporary_storage =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const BytecodeProgram distinct_arm =
      LowerOrThrow("SELECT DISTINCT 1 FROM items UNION ALL SELECT 2", catalog);
  const auto distinct_arm_rows = ExecuteRows(distinct_arm, *pager, catalog->version().generation,
                                             VmEnvironment::Core(), &temporary_storage);
  ASSERT_EQ(2U, distinct_arm_rows.size());
  EXPECT_EQ(1, distinct_arm_rows[0][0].integer_value());
  EXPECT_EQ(2, distinct_arm_rows[1][0].integer_value());

  const BytecodeProgram distinct_limited =
      LowerOrThrow("SELECT DISTINCT 1 FROM items UNION ALL SELECT 2 LIMIT 1", catalog);
  const auto distinct_limited_rows =
      ExecuteRows(distinct_limited, *pager, catalog->version().generation, VmEnvironment::Core(),
                  &temporary_storage);
  ASSERT_EQ(1U, distinct_limited_rows.size());
  EXPECT_EQ(1, distinct_limited_rows[0][0].integer_value());

  const BytecodeProgram distinct_zero = LowerOrThrow(
      "SELECT DISTINCT failing() FROM items UNION ALL SELECT 2 LIMIT 0", catalog, custom.Binder());
  callback_count = 0;
  EXPECT_TRUE(
      ExecuteRows(distinct_zero, *pager, catalog->version().generation, custom.Vm()).empty());
  EXPECT_EQ(0U, callback_count);

  const BytecodeProgram guarded = LowerOrThrow(
      "SELECT 1 FROM items WHERE id<0 AND abs(-9223372036854775808) "
      "UNION ALL SELECT 2",
      catalog);
  Vm guarded_vm = TakeValue(Vm::Create(guarded, VmEnvironment::Core()));
  RequireStatus(
      guarded_vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  const auto guard_failure = guarded_vm.Step();
  ASSERT_FALSE(guard_failure.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, guard_failure.error().code());
  RequireStatus(pager->EndRead());
}

TEST(ReadLowering, ResetsAndRebindsValuesUnionAllParameters) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 107}));
  const BytecodeProgram program =
      LowerOrThrow("VALUES(?1),(?2) UNION ALL SELECT ?3 LIMIT ?4 OFFSET ?5", catalog);
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  const auto run = [&](std::int64_t first, std::int64_t second, std::int64_t third) {
    RequireStatus(vm.Bind(ParameterId{0}, SqlValue::Integer(first)));
    RequireStatus(vm.Bind(ParameterId{1}, SqlValue::Integer(second)));
    RequireStatus(vm.Bind(ParameterId{2}, SqlValue::Integer(third)));
    RequireStatus(vm.Bind(ParameterId{3}, SqlValue::Integer(2)));
    RequireStatus(vm.Bind(ParameterId{4}, SqlValue::Integer(1)));
    RequireStatus(
        vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    EXPECT_EQ(second, vm.row()[0].integer_value());
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    EXPECT_EQ(third, vm.row()[0].integer_value());
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  };
  run(1, 2, 3);
  RequireStatus(vm.Reset());
  run(4, 5, 6);
  RequireStatus(pager->EndRead());
}

TEST(ReadLowering, ExecutesOrderedUnionAllWithStablePerArmTopN) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 137}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const CustomEnvironment custom;

  const BytecodeProgram stable =
      LowerOrThrow("SELECT 1,'left' UNION ALL SELECT 1,'right' ORDER BY 1", catalog);
  const auto stable_rows =
      ExecuteRows(stable, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(2U, stable_rows.size());
  EXPECT_EQ("left", TextBytes(stable_rows[0][1]));
  EXPECT_EQ("right", TextBytes(stable_rows[1][1]));
  EXPECT_GT(std::ranges::count(InstructionKinds(stable), InstructionKind::kCompareRecords), 0);

  const BytecodeProgram grouped_stable = LowerOrThrow(
      "SELECT 1,'left' UNION ALL SELECT 1,'middle' UNION ALL SELECT 1,'right' "
      "ORDER BY 1",
      catalog);
  const auto grouped_stable_rows = ExecuteRows(
      grouped_stable, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(3U, grouped_stable_rows.size());
  EXPECT_EQ("left", TextBytes(grouped_stable_rows[0][1]));
  EXPECT_EQ("middle", TextBytes(grouped_stable_rows[1][1]));
  EXPECT_EQ("right", TextBytes(grouped_stable_rows[2][1]));

  callback_count = 0;
  const BytecodeProgram bounded = LowerOrThrow(
      "SELECT id,volatile_counter() FROM items "
      "UNION ALL SELECT id,volatile_counter() FROM items "
      "ORDER BY 1 LIMIT 1",
      catalog, custom.Binder());
  const auto bounded_rows =
      ExecuteRows(bounded, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, bounded_rows.size());
  EXPECT_EQ(1, bounded_rows[0][0].integer_value());
  EXPECT_EQ(1, bounded_rows[0][1].integer_value());
  EXPECT_EQ(2U, callback_count);

  const BytecodeProgram bounded_distinct = LowerOrThrow(
      "SELECT DISTINCT id FROM items "
      "UNION ALL SELECT id FROM items "
      "ORDER BY 1 LIMIT 1",
      catalog);
  EXPECT_EQ(2, std::ranges::count(InstructionKinds(bounded_distinct), InstructionKind::kOpenTopN));
  const auto bounded_distinct_rows = ExecuteRows(
      bounded_distinct, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, bounded_distinct_rows.size());
  EXPECT_EQ(1, bounded_distinct_rows[0][0].integer_value());

  const BytecodeProgram bounded_values =
      LowerOrThrow("VALUES(3),(1),(2) UNION ALL SELECT 4 ORDER BY 1 LIMIT 1", catalog);
  EXPECT_EQ(2, std::ranges::count(InstructionKinds(bounded_values), InstructionKind::kOpenTopN));
  const auto bounded_value_rows = ExecuteRows(bounded_values, *pager, catalog->version().generation,
                                              VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, bounded_value_rows.size());
  EXPECT_EQ(1, bounded_value_rows[0][0].integer_value());

  const BytecodeProgram global_limit = LowerOrThrow(
      "SELECT 3 UNION ALL SELECT 1 UNION ALL SELECT 2 "
      "ORDER BY 1 LIMIT 2 OFFSET 1",
      catalog);
  const auto global_rows = ExecuteRows(global_limit, *pager, catalog->version().generation,
                                       VmEnvironment::Core(), &memory);
  ASSERT_EQ(2U, global_rows.size());
  EXPECT_EQ(2, global_rows[0][0].integer_value());
  EXPECT_EQ(3, global_rows[1][0].integer_value());

  callback_count = 0;
  const BytecodeProgram unlimited = LowerOrThrow(
      "SELECT id,volatile_counter() FROM items "
      "UNION ALL SELECT id,volatile_counter() FROM items "
      "ORDER BY 1 LIMIT -1",
      catalog, custom.Binder());
  const auto unlimited_rows =
      ExecuteRows(unlimited, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(6U, unlimited_rows.size());
  EXPECT_EQ(1, unlimited_rows[0][1].integer_value());
  EXPECT_EQ(4, unlimited_rows[1][1].integer_value());
  EXPECT_EQ(6U, callback_count);

  callback_count = 0;
  const BytecodeProgram overflow = LowerOrThrow(
      "SELECT id,volatile_counter() FROM items "
      "UNION ALL SELECT id,volatile_counter() FROM items "
      "ORDER BY 1 LIMIT ?1 OFFSET ?2",
      catalog, custom.Binder());
  Vm overflow_vm = TakeValue(Vm::Create(overflow, custom.Vm()));
  RequireStatus(overflow_vm.Bind(ParameterId{0},
                                 SqlValue::Integer(std::numeric_limits<std::int64_t>::max())));
  RequireStatus(overflow_vm.Bind(ParameterId{1}, SqlValue::Integer(1)));
  RequireStatus(overflow_vm.AttachExecutionContext(
      VmExecutionContext{*pager, catalog->version().generation, memory}));
  std::size_t overflow_rows = 0;
  while (TakeValue(overflow_vm.Step()) == VmStep::kRow) {
    ++overflow_rows;
  }
  EXPECT_EQ(5U, overflow_rows);
  EXPECT_EQ(6U, callback_count);

  callback_count = 0;
  const BytecodeProgram zero = LowerOrThrow(
      "SELECT failing() UNION ALL SELECT failing() ORDER BY 1 LIMIT 0", catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(zero, *pager, catalog->version().generation, custom.Vm()).empty());
  EXPECT_EQ(0U, callback_count);

  const TemporaryStorageFactory file =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{256},
                                                }));
  const BytecodeProgram file_program = LowerOrThrow(
      "SELECT 8 UNION ALL SELECT 3 UNION ALL SELECT 6 UNION ALL SELECT 1 "
      "UNION ALL SELECT 7 UNION ALL SELECT 2 UNION ALL SELECT 5 UNION ALL SELECT 4 "
      "ORDER BY 1",
      catalog);
  const auto file_rows = ExecuteRows(file_program, *pager, catalog->version().generation,
                                     VmEnvironment::Core(), &file);
  ASSERT_EQ(8U, file_rows.size());
  for (std::size_t index = 0; index < file_rows.size(); ++index) {
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), file_rows[index][0].integer_value());
  }
  RequireStatus(pager->EndRead());
}

TEST(ReadLowering, ExecutesOrderedSetMergesAndSetThenOrder) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 139}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const CustomEnvironment custom;
  const auto execute = [&](std::string_view sql) {
    const BytecodeProgram program = LowerOrThrow(sql, catalog);
    return ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(),
                       &memory);
  };

  const auto union_rows = execute("SELECT 1.0 UNION SELECT 1 ORDER BY 1");
  ASSERT_EQ(1U, union_rows.size());
  EXPECT_EQ(SqlValueType::kInteger, union_rows[0][0].type());
  EXPECT_EQ(1, union_rows[0][0].integer_value());

  const auto grouped_union = execute("SELECT 1.0 UNION SELECT 1 UNION SELECT 1.0 ORDER BY 1");
  ASSERT_EQ(1U, grouped_union.size());
  EXPECT_EQ(SqlValueType::kReal, grouped_union[0][0].type());
  EXPECT_EQ(1.0, grouped_union[0][0].real_value());

  callback_count = 0;
  const BytecodeProgram grouped_union_arm = LowerOrThrow(
      "SELECT 'x' COLLATE NOCASE "
      "UNION SELECT 'y' "
      "UNION SELECT reverse_text_representative() FROM items "
      "ORDER BY 1",
      catalog, custom.Binder());
  const auto grouped_union_arm_rows =
      ExecuteRows(grouped_union_arm, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(4U, grouped_union_arm_rows.size());
  EXPECT_EQ("a", TextBytes(grouped_union_arm_rows[0][0]));
  EXPECT_EQ("B", TextBytes(grouped_union_arm_rows[1][0]));
  EXPECT_EQ("x", TextBytes(grouped_union_arm_rows[2][0]));
  EXPECT_EQ("y", TextBytes(grouped_union_arm_rows[3][0]));
  EXPECT_EQ(3U, callback_count);

  const auto intersect_rows = execute("SELECT 1 INTERSECT SELECT 1.0 ORDER BY 1");
  ASSERT_EQ(1U, intersect_rows.size());
  EXPECT_EQ(SqlValueType::kInteger, intersect_rows[0][0].type());
  EXPECT_EQ(1, intersect_rows[0][0].integer_value());

  const auto except_rows = execute("SELECT 2 UNION SELECT 1 EXCEPT SELECT 2 ORDER BY 1");
  ASSERT_EQ(1U, except_rows.size());
  EXPECT_EQ(1, except_rows[0][0].integer_value());

  const auto set_then_order = execute(
      "SELECT 'A' COLLATE NOCASE "
      "UNION SELECT 'B' "
      "UNION SELECT 'a' "
      "ORDER BY 1 COLLATE BINARY");
  ASSERT_EQ(2U, set_then_order.size());
  EXPECT_EQ("B", TextBytes(set_then_order[0][0]));
  EXPECT_EQ("a", TextBytes(set_then_order[1][0]));

  const auto set_then_order_limit = execute(
      "SELECT 'A' COLLATE NOCASE "
      "UNION SELECT 'a' "
      "UNION ALL SELECT 'B' "
      "ORDER BY 1 COLLATE BINARY LIMIT 2");
  ASSERT_EQ(2U, set_then_order_limit.size());
  EXPECT_EQ("B", TextBytes(set_then_order_limit[0][0]));
  EXPECT_EQ("a", TextBytes(set_then_order_limit[1][0]));

  callback_count = 0;
  const BytecodeProgram distinct_union = LowerOrThrow(
      "SELECT 'x' COLLATE NOCASE "
      "UNION SELECT DISTINCT reverse_text_representative() FROM items "
      "ORDER BY 1",
      catalog, custom.Binder());
  const auto distinct_union_rows =
      ExecuteRows(distinct_union, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(3U, distinct_union_rows.size());
  EXPECT_EQ("a", TextBytes(distinct_union_rows[0][0]));
  EXPECT_EQ("B", TextBytes(distinct_union_rows[1][0]));
  EXPECT_EQ("x", TextBytes(distinct_union_rows[2][0]));
  EXPECT_EQ(3U, callback_count);

  callback_count = 0;
  const BytecodeProgram empty_left = LowerOrThrow(
      "SELECT 1 WHERE 0 INTERSECT SELECT failing() ORDER BY 1", catalog, custom.Binder());
  EXPECT_TRUE(
      ExecuteRows(empty_left, *pager, catalog->version().generation, custom.Vm(), &memory).empty());
  EXPECT_EQ(0U, callback_count);

  callback_count = 0;
  const BytecodeProgram empty_except =
      LowerOrThrow("SELECT 1 WHERE 0 EXCEPT SELECT failing() ORDER BY 1", catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(empty_except, *pager, catalog->version().generation, custom.Vm(), &memory)
                  .empty());
  EXPECT_EQ(0U, callback_count);

  callback_count = 0;
  const BytecodeProgram materialized =
      LowerOrThrow("SELECT 1 UNION SELECT failing() ORDER BY 1 LIMIT 1", catalog, custom.Binder());
  Vm materialized_vm = TakeValue(Vm::Create(materialized, custom.Vm()));
  RequireStatus(materialized_vm.AttachExecutionContext(
      VmExecutionContext{*pager, catalog->version().generation, memory}));
  const auto materialized_failure = materialized_vm.Step();
  ASSERT_FALSE(materialized_failure.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, materialized_failure.error().code());
  EXPECT_EQ(1U, callback_count);

  const auto limit_one = execute("SELECT 3 UNION SELECT 1 ORDER BY 1 LIMIT +1");
  ASSERT_EQ(1U, limit_one.size());
  EXPECT_EQ(1, limit_one[0][0].integer_value());

  callback_count = 0;
  const BytecodeProgram bounded_union = LowerOrThrow(
      "SELECT volatile_counter()%2 FROM items "
      "UNION SELECT volatile_counter()%2 FROM items "
      "ORDER BY 1 LIMIT 1",
      catalog, custom.Binder());
  const auto bounded_union_rows =
      ExecuteRows(bounded_union, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, bounded_union_rows.size());
  EXPECT_EQ(0, bounded_union_rows[0][0].integer_value());
  EXPECT_EQ(6U, callback_count);
  RequireStatus(pager->EndRead());
}

TEST(ReadLowering, ResetsAndRebindsOrderedCompoundParameters) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 149}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const BytecodeProgram program =
      LowerOrThrow("VALUES(?1),(?2) UNION ALL SELECT ?3 ORDER BY 1 LIMIT ?4 OFFSET ?5", catalog);
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  const auto run = [&](std::int64_t first, std::int64_t second, std::int64_t third) {
    RequireStatus(vm.Bind(ParameterId{0}, SqlValue::Integer(first)));
    RequireStatus(vm.Bind(ParameterId{1}, SqlValue::Integer(second)));
    RequireStatus(vm.Bind(ParameterId{2}, SqlValue::Integer(third)));
    RequireStatus(vm.Bind(ParameterId{3}, SqlValue::Integer(2)));
    RequireStatus(vm.Bind(ParameterId{4}, SqlValue::Integer(1)));
    RequireStatus(vm.AttachExecutionContext(
        VmExecutionContext{*pager, catalog->version().generation, memory}));
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    EXPECT_EQ(second, vm.row()[0].integer_value());
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    EXPECT_EQ(third, vm.row()[0].integer_value());
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  };
  run(1, 2, 3);
  RequireStatus(vm.Reset());
  run(4, 5, 6);
  RequireStatus(pager->EndRead());
}

TEST(ReadLowering, ExecutesUnorderedSetOperatorsAndRepresentatives) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 109}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const CustomEnvironment custom;

  const auto execute = [&](std::string_view sql) {
    const BytecodeProgram program = LowerOrThrow(sql, catalog);
    return ExecuteRows(program, *pager, catalog->version().generation, VmEnvironment::Core(),
                       &memory);
  };

  const auto union_rows = execute("SELECT 1 UNION SELECT 1.0");
  ASSERT_EQ(1U, union_rows.size());
  EXPECT_EQ(SqlValueType::kReal, union_rows[0][0].type());
  EXPECT_EQ(1.0, union_rows[0][0].real_value());

  const auto intersect_rows = execute("SELECT 1 INTERSECT SELECT 1.0");
  ASSERT_EQ(1U, intersect_rows.size());
  EXPECT_EQ(SqlValueType::kInteger, intersect_rows[0][0].type());
  EXPECT_EQ(1, intersect_rows[0][0].integer_value());

  EXPECT_TRUE(execute("SELECT 1 EXCEPT SELECT 1.0").empty());
  const auto null_rows = execute("SELECT NULL UNION SELECT NULL");
  ASSERT_EQ(1U, null_rows.size());
  EXPECT_EQ(SqlValueType::kNull, null_rows[0][0].type());

  const auto nocase = execute("SELECT 'a' UNION SELECT 'A' COLLATE NOCASE");
  ASSERT_EQ(1U, nocase.size());
  EXPECT_EQ("A", TextBytes(nocase[0][0]));

  const auto binary = execute("SELECT 'a' COLLATE BINARY UNION SELECT 'A' COLLATE NOCASE");
  ASSERT_EQ(2U, binary.size());
  EXPECT_EQ("A", TextBytes(binary[0][0]));
  EXPECT_EQ("a", TextBytes(binary[1][0]));

  const auto mixed = execute("SELECT 1 UNION SELECT 2 EXCEPT SELECT 1");
  ASSERT_EQ(1U, mixed.size());
  EXPECT_EQ(2, mixed[0][0].integer_value());

  const auto sorted = execute("SELECT 3 UNION SELECT 1 UNION SELECT 2");
  ASSERT_EQ(3U, sorted.size());
  EXPECT_EQ(1, sorted[0][0].integer_value());
  EXPECT_EQ(2, sorted[1][0].integer_value());
  EXPECT_EQ(3, sorted[2][0].integer_value());

  const auto distinct_arm = execute("SELECT DISTINCT 1 FROM items UNION SELECT 2");
  ASSERT_EQ(2U, distinct_arm.size());
  EXPECT_EQ(1, distinct_arm[0][0].integer_value());
  EXPECT_EQ(2, distinct_arm[1][0].integer_value());

  callback_count = 0;
  const BytecodeProgram distinct_union = LowerOrThrow(
      "SELECT 'x' COLLATE NOCASE "
      "UNION SELECT DISTINCT reverse_text_representative() FROM items",
      catalog, custom.Binder());
  const auto distinct_union_rows =
      ExecuteRows(distinct_union, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(3U, distinct_union_rows.size());
  EXPECT_EQ("a", TextBytes(distinct_union_rows[0][0]));
  EXPECT_EQ("B", TextBytes(distinct_union_rows[1][0]));
  EXPECT_EQ("x", TextBytes(distinct_union_rows[2][0]));
  EXPECT_EQ(3U, callback_count);

  callback_count = 0;
  const BytecodeProgram distinct_intersect = LowerOrThrow(
      "SELECT DISTINCT reverse_text_representative() FROM items "
      "INTERSECT SELECT 'a' COLLATE NOCASE",
      catalog, custom.Binder());
  const auto distinct_intersect_rows =
      ExecuteRows(distinct_intersect, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, distinct_intersect_rows.size());
  EXPECT_EQ("a", TextBytes(distinct_intersect_rows[0][0]));
  EXPECT_EQ(3U, callback_count);

  callback_count = 0;
  const BytecodeProgram distinct_except = LowerOrThrow(
      "SELECT DISTINCT reverse_text_representative() FROM items "
      "EXCEPT SELECT 'B' COLLATE NOCASE",
      catalog, custom.Binder());
  const auto distinct_except_rows =
      ExecuteRows(distinct_except, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, distinct_except_rows.size());
  EXPECT_EQ("a", TextBytes(distinct_except_rows[0][0]));
  EXPECT_EQ(3U, callback_count);

  const auto mixed_union_all = execute("SELECT 1 UNION SELECT 2 UNION ALL SELECT 2");
  ASSERT_EQ(3U, mixed_union_all.size());
  EXPECT_EQ(1, mixed_union_all[0][0].integer_value());
  EXPECT_EQ(2, mixed_union_all[1][0].integer_value());
  EXPECT_EQ(2, mixed_union_all[2][0].integer_value());

  const auto global_limit = execute("SELECT 1 UNION SELECT 2 UNION ALL SELECT 3 LIMIT 2 OFFSET 1");
  ASSERT_EQ(2U, global_limit.size());
  EXPECT_EQ(2, global_limit[0][0].integer_value());
  EXPECT_EQ(3, global_limit[1][0].integer_value());

  const auto collapsed_union_all = execute("SELECT 1.0 UNION ALL SELECT 1 UNION SELECT 2");
  ASSERT_EQ(2U, collapsed_union_all.size());
  EXPECT_EQ(SqlValueType::kReal, collapsed_union_all[0][0].type());
  EXPECT_EQ(1.0, collapsed_union_all[0][0].real_value());
  EXPECT_EQ(2, collapsed_union_all[1][0].integer_value());

  const auto table_rows = execute("SELECT name FROM items UNION SELECT name FROM items WHERE id=2");
  ASSERT_EQ(3U, table_rows.size());
  EXPECT_EQ("alpha", TextBytes(table_rows[0][0]));
  EXPECT_EQ("beta", TextBytes(table_rows[1][0]));
  EXPECT_EQ("gamma", TextBytes(table_rows[2][0]));

  const auto without_rowid_rows = execute("SELECT a FROM wr INTERSECT SELECT a FROM wr");
  ASSERT_EQ(2U, without_rowid_rows.size());
  EXPECT_EQ("left", TextBytes(without_rowid_rows[0][0]));
  EXPECT_EQ("right", TextBytes(without_rowid_rows[1][0]));

  const TemporaryStorageFactory file =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                }));
  const BytecodeProgram file_program =
      LowerOrThrow("SELECT 3 UNION SELECT 1 UNION SELECT 2", catalog);
  const auto file_rows = ExecuteRows(file_program, *pager, catalog->version().generation,
                                     VmEnvironment::Core(), &file);
  ASSERT_EQ(3U, file_rows.size());
  EXPECT_EQ(1, file_rows[0][0].integer_value());
  EXPECT_EQ(2, file_rows[1][0].integer_value());
  EXPECT_EQ(3, file_rows[2][0].integer_value());

  const BytecodeProgram file_intersect =
      LowerOrThrow("SELECT 1 UNION SELECT 2 INTERSECT SELECT 2 INTERSECT SELECT 2", catalog);
  const auto file_intersect_rows = ExecuteRows(
      file_intersect, *pager, catalog->version().generation, VmEnvironment::Core(), &file);
  ASSERT_EQ(1U, file_intersect_rows.size());
  EXPECT_EQ(2, file_intersect_rows[0][0].integer_value());

  const BytecodeProgram missing_program = LowerOrThrow("SELECT 1 UNION SELECT 2", catalog);
  Vm missing = TakeValue(Vm::Create(missing_program, VmEnvironment::Core()));
  RequireStatus(
      missing.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  const auto missing_storage = missing.Step();
  ASSERT_FALSE(missing_storage.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing_storage.error().code());
  RequireStatus(pager->EndRead());
}

TEST(ReadLowering, SuppressesEmptySetRightArmsAndMaterializesOrdinaryUnion) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 113}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const CustomEnvironment custom;

  for (const std::string_view sql : {
           "SELECT 1 WHERE 0 EXCEPT SELECT failing()",
           "SELECT 1 WHERE 0 INTERSECT SELECT failing()",
       }) {
    SCOPED_TRACE(sql);
    callback_count = 0;
    const BytecodeProgram program = LowerOrThrow(sql, catalog, custom.Binder());
    EXPECT_TRUE(
        ExecuteRows(program, *pager, catalog->version().generation, custom.Vm(), &memory).empty());
    EXPECT_EQ(0U, callback_count);
  }

  const BytecodeProgram empty_intersect =
      LowerOrThrow("SELECT 1 WHERE 0 INTERSECT SELECT failing()", catalog, custom.Binder());
  const std::vector<InstructionKind> empty_intersect_kinds = InstructionKinds(empty_intersect);
  const RegisterId accumulator_count{empty_intersect.register_count() - 3U};
  std::optional<std::size_t> empty_branch;
  for (std::size_t index = 0; index < empty_intersect.instructions().size(); ++index) {
    const auto* jump = std::get_if<JumpIfInstruction>(&empty_intersect.instructions()[index]);
    if (jump != nullptr && jump->input == accumulator_count) {
      empty_branch = index;
      break;
    }
  }
  ASSERT_TRUE(empty_branch.has_value());
  EXPECT_EQ(1,
            std::count(empty_intersect_kinds.begin(),
                       empty_intersect_kinds.begin() + static_cast<std::ptrdiff_t>(*empty_branch),
                       InstructionKind::kOpenRelation));

  const BytecodeProgram distinct_right = LowerOrThrow(
      "SELECT 1 INTERSECT "
      "SELECT DISTINCT reverse_text_representative() FROM items",
      catalog, custom.Binder());
  const std::vector<InstructionKind> distinct_right_kinds = InstructionKinds(distinct_right);
  std::vector<std::size_t> relation_opens;
  for (std::size_t index = 0; index < distinct_right_kinds.size(); ++index) {
    if (distinct_right_kinds[index] == InstructionKind::kOpenRelation) {
      relation_opens.push_back(index);
    }
  }
  const auto first_relation_close =
      std::ranges::find(distinct_right_kinds, InstructionKind::kCloseRelation);
  ASSERT_GE(relation_opens.size(), 4U);
  ASSERT_NE(distinct_right_kinds.end(), first_relation_close);
  EXPECT_LT(
      static_cast<std::size_t>(std::distance(distinct_right_kinds.begin(), first_relation_close)),
      relation_opens[3]);

  callback_count = 0;
  const BytecodeProgram ordinary =
      LowerOrThrow("SELECT 1 UNION SELECT failing()", catalog, custom.Binder());
  Vm ordinary_vm = TakeValue(Vm::Create(ordinary, custom.Vm()));
  RequireStatus(ordinary_vm.AttachExecutionContext(
      VmExecutionContext{*pager, catalog->version().generation, memory}));
  const auto failed = ordinary_vm.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram zero =
      LowerOrThrow("SELECT failing() UNION SELECT failing() LIMIT 0", catalog, custom.Binder());
  EXPECT_TRUE(ExecuteRows(zero, *pager, catalog->version().generation, custom.Vm()).empty());
  EXPECT_EQ(0U, callback_count);
  RequireStatus(pager->EndRead());
}

TEST(ReadLowering, PreservesExactUnionLimitOneShortCircuitClassifier) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 127}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const CustomEnvironment custom;

  for (const std::string_view limit : {"1", "+1", "(1)", "-(-1)"}) {
    SCOPED_TRACE(limit);
    callback_count = 0;
    const std::string sql =
        "SELECT volatile_counter() FROM items UNION SELECT failing() LIMIT " + std::string{limit};
    const BytecodeProgram program = LowerOrThrow(sql, catalog, custom.Binder());
    const auto rows = ExecuteRows(program, *pager, catalog->version().generation, custom.Vm());
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ(1, rows[0][0].integer_value());
    EXPECT_EQ(1U, callback_count);
  }

  const BytecodeProgram representative = LowerOrThrow("SELECT 1.0 UNION SELECT 1 LIMIT 1", catalog);
  const auto representative_rows =
      ExecuteRows(representative, *pager, catalog->version().generation);
  ASSERT_EQ(1U, representative_rows.size());
  EXPECT_EQ(SqlValueType::kReal, representative_rows[0][0].type());
  EXPECT_EQ(1.0, representative_rows[0][0].real_value());

  for (const std::string_view limit : {"1+0", "1.0", "TRUE", "+TRUE", "-(-TRUE)"}) {
    SCOPED_TRACE(limit);
    callback_count = 0;
    const std::string sql =
        "SELECT volatile_counter() FROM items UNION SELECT failing() LIMIT " + std::string{limit};
    const BytecodeProgram program = LowerOrThrow(sql, catalog, custom.Binder());
    Vm vm = TakeValue(Vm::Create(program, custom.Vm()));
    RequireStatus(vm.AttachExecutionContext(
        VmExecutionContext{*pager, catalog->version().generation, memory}));
    const auto failed = vm.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
    EXPECT_GT(callback_count, 1U);
  }

  callback_count = 0;
  const BytecodeProgram parameter =
      LowerOrThrow("SELECT volatile_counter() FROM items UNION SELECT failing() LIMIT ?1", catalog,
                   custom.Binder());
  Vm parameter_vm = TakeValue(Vm::Create(parameter, custom.Vm()));
  RequireStatus(parameter_vm.Bind(ParameterId{0}, SqlValue::Integer(1)));
  RequireStatus(parameter_vm.AttachExecutionContext(
      VmExecutionContext{*pager, catalog->version().generation, memory}));
  const auto parameter_failure = parameter_vm.Step();
  ASSERT_FALSE(parameter_failure.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, parameter_failure.error().code());
  EXPECT_GT(callback_count, 1U);

  callback_count = 0;
  const BytecodeProgram empty_left = LowerOrThrow(
      "SELECT 1 WHERE 0 UNION SELECT volatile_counter() LIMIT 1", catalog, custom.Binder());
  const auto empty_left_rows =
      ExecuteRows(empty_left, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, empty_left_rows.size());
  EXPECT_EQ(1, empty_left_rows[0][0].integer_value());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram resumed = LowerOrThrow(
      "SELECT 2 EXCEPT SELECT 1 UNION SELECT failing() LIMIT 1", catalog, custom.Binder());
  const auto resumed_rows =
      ExecuteRows(resumed, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, resumed_rows.size());
  EXPECT_EQ(2, resumed_rows[0][0].integer_value());
  EXPECT_EQ(0U, callback_count);
  RequireStatus(pager->EndRead());
}

TEST(ReadLowering, ResetsAndRebindsUnorderedSetParameters) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 131}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const BytecodeProgram program =
      LowerOrThrow("VALUES(?1),(?2) UNION SELECT ?3 LIMIT 2 OFFSET 1", catalog);
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));
  const auto run = [&](std::int64_t first, std::int64_t second, std::int64_t third) {
    RequireStatus(vm.Bind(ParameterId{0}, SqlValue::Integer(first)));
    RequireStatus(vm.Bind(ParameterId{1}, SqlValue::Integer(second)));
    RequireStatus(vm.Bind(ParameterId{2}, SqlValue::Integer(third)));
    RequireStatus(vm.AttachExecutionContext(
        VmExecutionContext{*pager, catalog->version().generation, memory}));
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    EXPECT_EQ(second, vm.row()[0].integer_value());
    EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
    EXPECT_EQ(third, vm.row()[0].integer_value());
    EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  };
  run(1, 2, 3);
  RequireStatus(vm.Reset());
  run(4, 5, 6);
  RequireStatus(pager->EndRead());
}

TEST(ReadLowering, LowersSimpleDistinctIntoMembershipAndOptionalOrderingBytecode) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BytecodeProgram simple = LowerOrThrow("SELECT DISTINCT name FROM items", catalog);
  ASSERT_EQ(1U, simple.relations().size());
  EXPECT_TRUE(simple.sorters().empty());
  const OrderingRecordDescriptor& membership = simple.relations().front();
  EXPECT_EQ(1U, membership.field_count);
  EXPECT_EQ(1U, membership.key_field_count);
  ASSERT_EQ(1U, membership.key_columns.size());
  EXPECT_EQ("BINARY", simple.symbol(membership.key_columns[0].collation));
  const std::vector<InstructionKind> simple_kinds = InstructionKinds(simple);
  EXPECT_EQ(1U, static_cast<std::size_t>(
                    std::ranges::count(simple_kinds, InstructionKind::kOpenRelation)));
  EXPECT_EQ(1U, static_cast<std::size_t>(
                    std::ranges::count(simple_kinds, InstructionKind::kInsertRelation)));
  EXPECT_EQ(1U, static_cast<std::size_t>(
                    std::ranges::count(simple_kinds, InstructionKind::kCloseRelation)));

  const CustomEnvironment custom;
  const BytecodeProgram ordered = LowerOrThrow(
      "SELECT DISTINCT 1 FROM items ORDER BY volatile_counter()", catalog, custom.Binder());
  ASSERT_EQ(1U, ordered.relations().size());
  ASSERT_EQ(1U, ordered.sorters().size());
  EXPECT_TRUE(ordered.top_ns().empty());
  const auto relation_insert =
      std::ranges::find_if(ordered.instructions(), [](const Instruction& instruction) {
        return std::holds_alternative<InsertRelationInstruction>(instruction);
      });
  const auto hidden_call =
      std::ranges::find_if(ordered.instructions(), [](const Instruction& instruction) {
        return std::holds_alternative<CallScalarInstruction>(instruction);
      });
  const auto sorter_insert =
      std::ranges::find_if(ordered.instructions(), [](const Instruction& instruction) {
        return std::holds_alternative<InsertSorterInstruction>(instruction);
      });
  ASSERT_NE(ordered.instructions().end(), relation_insert);
  ASSERT_NE(ordered.instructions().end(), hidden_call);
  ASSERT_NE(ordered.instructions().end(), sorter_insert);
  EXPECT_LT(relation_insert, hidden_call);
  EXPECT_LT(hidden_call, sorter_insert);
}

TEST(ReadLowering, ExecutesSimpleDistinctRepresentativesLimitsOrderingAndCleanup) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 97}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const CustomEnvironment custom;

  callback_count = 0;
  const BytecodeProgram representative =
      LowerOrThrow("SELECT DISTINCT numeric_representative() FROM items", catalog, custom.Binder());
  const auto representative_rows =
      ExecuteRows(representative, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, representative_rows.size());
  EXPECT_EQ(SqlValueType::kInteger, representative_rows[0][0].type());
  EXPECT_EQ(1, representative_rows[0][0].integer_value());
  EXPECT_EQ(3U, callback_count);

  const BytecodeProgram nulls = LowerOrThrow("SELECT DISTINCT NULL FROM items", catalog);
  const auto null_rows =
      ExecuteRows(nulls, *pager, catalog->version().generation, VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, null_rows.size());
  EXPECT_EQ(SqlValueType::kNull, null_rows[0][0].type());

  const BytecodeProgram offset_after_membership =
      LowerOrThrow("SELECT DISTINCT 1 FROM items LIMIT 1 OFFSET 1", catalog);
  EXPECT_TRUE(ExecuteRows(offset_after_membership, *pager, catalog->version().generation,
                          VmEnvironment::Core(), &memory)
                  .empty());

  const BytecodeProgram source_free = LowerOrThrow("SELECT DISTINCT 7", catalog);
  const auto source_free_rows = ExecuteRows(source_free, *pager, catalog->version().generation,
                                            VmEnvironment::Core(), &memory);
  ASSERT_EQ(1U, source_free_rows.size());
  EXPECT_EQ(7, source_free_rows[0][0].integer_value());

  callback_count = 0;
  const BytecodeProgram collated =
      LowerOrThrow("SELECT DISTINCT text_representative() COLLATE NOCASE FROM items ORDER BY 1",
                   catalog, custom.Binder());
  const auto collated_rows =
      ExecuteRows(collated, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(2U, collated_rows.size());
  EXPECT_EQ("A", TextBytes(collated_rows[0][0]));
  EXPECT_EQ("B", TextBytes(collated_rows[1][0]));
  EXPECT_EQ(3U, callback_count);

  callback_count = 0;
  const BytecodeProgram zero = LowerOrThrow(
      "SELECT DISTINCT failing() FROM items LIMIT 0 OFFSET failing()", catalog, custom.Binder());
  Vm zero_without_storage = TakeValue(Vm::Create(zero, custom.Vm()));
  RequireStatus(zero_without_storage.AttachExecutionContext(
      VmExecutionContext{*pager, catalog->version().generation}));
  EXPECT_EQ(VmStep::kDone, TakeValue(zero_without_storage.Step()));
  EXPECT_EQ(0U, callback_count);

  callback_count = 0;
  const BytecodeProgram limited = LowerOrThrow(
      "SELECT DISTINCT volatile_counter() FROM items LIMIT 1", catalog, custom.Binder());
  const auto limited_rows =
      ExecuteRows(limited, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, limited_rows.size());
  EXPECT_EQ(1, limited_rows[0][0].integer_value());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram hidden = LowerOrThrow(
      "SELECT DISTINCT 1 FROM items ORDER BY volatile_counter()", catalog, custom.Binder());
  const auto hidden_rows =
      ExecuteRows(hidden, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, hidden_rows.size());
  EXPECT_EQ(1U, callback_count);

  callback_count = 0;
  const BytecodeProgram ordered_limit =
      LowerOrThrow("SELECT DISTINCT volatile_counter()%2 FROM items ORDER BY 1 LIMIT 1", catalog,
                   custom.Binder());
  const auto ordered_limit_rows =
      ExecuteRows(ordered_limit, *pager, catalog->version().generation, custom.Vm(), &memory);
  ASSERT_EQ(1U, ordered_limit_rows.size());
  EXPECT_EQ(0, ordered_limit_rows[0][0].integer_value());
  EXPECT_EQ(3U, callback_count);

  callback_count = 0;
  const BytecodeProgram later_error =
      LowerOrThrow("SELECT DISTINCT fail_after_one(name) FROM items", catalog, custom.Binder());
  Vm later_error_vm = TakeValue(Vm::Create(later_error, custom.Vm()));
  RequireStatus(later_error_vm.AttachExecutionContext(
      VmExecutionContext{*pager, catalog->version().generation, memory}));
  EXPECT_EQ(VmStep::kRow, TakeValue(later_error_vm.Step()));
  EXPECT_EQ(1, later_error_vm.row()[0].integer_value());
  const auto failed_after_row = later_error_vm.Step();
  ASSERT_FALSE(failed_after_row.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed_after_row.error().code());
  EXPECT_EQ(2U, callback_count);

  const TemporaryStorageFactory file =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{256},
                                                }));
  const BytecodeProgram file_program =
      LowerOrThrow("SELECT DISTINCT name FROM items ORDER BY name", catalog);
  EXPECT_EQ(3U, ExecuteRows(file_program, *pager, catalog->version().generation,
                            VmEnvironment::Core(), &file)
                    .size());

  Vm missing = TakeValue(Vm::Create(representative, custom.Vm()));
  RequireStatus(
      missing.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation}));
  const auto missing_storage = missing.Step();
  ASSERT_FALSE(missing_storage.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing_storage.error().code());

  const BytecodeProgram empty_guard = LowerOrThrow(
      "SELECT DISTINCT 1 FROM items WHERE id<0 AND abs(-9223372036854775808)", catalog);
  Vm empty_guard_vm = TakeValue(Vm::Create(empty_guard, VmEnvironment::Core()));
  RequireStatus(empty_guard_vm.AttachExecutionContext(
      VmExecutionContext{*pager, catalog->version().generation, memory}));
  const auto guard_failure = empty_guard_vm.Step();
  ASSERT_FALSE(guard_failure.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, guard_failure.error().code());
  EXPECT_EQ("integer overflow", guard_failure.error().message());
  RequireStatus(pager->EndRead());
}

TEST(ReadLowering, ResetsAndRebindsSimpleDistinctParameters) {
  PosixVfs vfs;
  std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, FixturePath().string()));
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 101}));
  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const BytecodeProgram program = LowerOrThrow("SELECT DISTINCT ?1 FROM items LIMIT ?2", catalog);
  Vm vm = TakeValue(Vm::Create(program, VmEnvironment::Core()));

  RequireStatus(vm.Bind(ParameterId{0}, SqlValue::Integer(5)));
  RequireStatus(vm.Bind(ParameterId{1}, SqlValue::Integer(1)));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation, memory}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(5, vm.row()[0].integer_value());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));

  RequireStatus(vm.Reset());
  RequireStatus(vm.Bind(ParameterId{0}, SqlValue::Integer(7)));
  RequireStatus(vm.Bind(ParameterId{1}, SqlValue::Integer(1)));
  RequireStatus(
      vm.AttachExecutionContext(VmExecutionContext{*pager, catalog->version().generation, memory}));
  EXPECT_EQ(VmStep::kRow, TakeValue(vm.Step()));
  EXPECT_EQ(7, vm.row()[0].integer_value());
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(pager->EndRead());
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
                                                    .sorter_memory_threshold = ByteCount{256},
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

  ProgramLimits relation_limit;
  relation_limit.maximum_relations = 0;
  expect_limit("SELECT DISTINCT name FROM items", relation_limit,
               ProgramErrorCode::kRelationLimitExceeded);

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
