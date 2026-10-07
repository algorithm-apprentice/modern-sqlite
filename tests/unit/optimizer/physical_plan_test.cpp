#include "modern_sqlite/optimizer/physical_plan.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/syntax/parser.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<PhysicalPlan>);
static_assert(!std::is_copy_assignable_v<PhysicalPlan>);
static_assert(std::is_nothrow_move_constructible_v<PhysicalPlan>);
static_assert(std::is_nothrow_move_assignable_v<PhysicalPlan>);
static_assert(!std::is_copy_constructible_v<PhysicalMutationPlan>);
static_assert(std::is_nothrow_move_constructible_v<PhysicalMutationPlan>);
static_assert(!std::is_copy_constructible_v<PhysicalStatementPlan>);
static_assert(std::is_nothrow_move_constructible_v<PhysicalStatementPlan>);
static_assert(!std::is_convertible_v<PhysicalNodeId, LogicalNodeId>);
static_assert(!std::is_convertible_v<LogicalNodeId, PhysicalNodeId>);
static_assert(!std::is_convertible_v<PhysicalNodeId, BoundExpressionId>);

[[nodiscard]] SyntaxTree ParseTree(std::string_view sql) {
  ParseResult parsed = ParseOne(Utf8View{sql});
  if (!parsed.has_value()) {
    throw std::runtime_error{std::string{ParseErrorMessage(parsed.error())}};
  }
  if (!parsed->tree.has_value()) {
    throw std::runtime_error{"optimizer test SQL did not produce a statement"};
  }
  return std::move(*parsed->tree);
}

[[nodiscard]] CatalogIndexTerm ColumnTerm(std::size_t column) {
  return CatalogIndexTerm{
      .target = ColumnId{column},
      .collation_name = "BINARY",
      .order = SortOrder::kAscending,
  };
}

[[nodiscard]] CatalogSnapshotPtr TestCatalog(std::string item_name = "Items") {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 23, .generation = 11},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT COLLATE NOCASE, Value INT)"));
  input.definitions.push_back(ParseTree("CREATE TABLE Tiny(id INTEGER PRIMARY KEY)"));
  input.definitions.push_back(ParseTree("CREATE TABLE wr(key TEXT PRIMARY KEY) WITHOUT ROWID"));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = std::move(item_name),
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
                  .collation_name = "NOCASE",
              },
              CatalogColumnInput{
                  .name = "Value",
                  .declared_type = "INT",
              },
          },
      .rowid_alias = ColumnId{0},
      .statistics =
          TableStatistics{
              .has_stat1 = true,
              .estimated_rows = 100,
              .average_row_size = 24,
          },
  });
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{1},
      .name = "Tiny",
      .root_page = RootPageId{3},
      .columns =
          {
              CatalogColumnInput{
                  .name = "id",
                  .declared_type = "INTEGER",
                  .primary_key = true,
              },
          },
      .rowid_alias = ColumnId{0},
      .statistics =
          TableStatistics{
              .has_stat1 = true,
              .estimated_rows = 1,
              .average_row_size = 8,
          },
  });
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{2},
      .name = "wr",
      .root_page = RootPageId{4},
      .columns =
          {
              CatalogColumnInput{
                  .name = "key",
                  .declared_type = "TEXT",
                  .primary_key = true,
              },
          },
      .without_rowid = true,
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{2},
      .name = "sqlite_autoindex_wr_1",
      .table = TableId{2},
      .root_page = RootPageId{4},
      .origin = IndexOrigin::kPrimaryKey,
      .unique = true,
      .conflict_action = ConflictAction::kDefault,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(0),
          },
  });
  CatalogSnapshotResult created = CatalogSnapshot::Create(std::move(input));
  if (!created.has_value()) {
    throw std::runtime_error{created.error().detail};
  }
  return *std::move(created);
}

[[nodiscard]] Result<SqlValue> ReturnOne(const ScalarFunctionContext&, std::span<const SqlValue>) {
  return SqlValue::Integer(1);
}

[[nodiscard]] BindEnvironment TestEnvironment() {
  static const std::array<ScalarFunction, 2> functions{{
      ScalarFunction{"stable_guard", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ReturnOne},
      ScalarFunction{"volatile_key", FunctionArity::Exact(0),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     ReturnOne},
  }};
  static const FunctionRegistry registry{functions};
  static const std::array<const Collation*, 3> collations{
      &BinaryCollation(),
      &NoCaseCollation(),
      &RTrimCollation(),
  };
  return BindEnvironment{registry, collations, 7};
}

[[nodiscard]] BoundSelect BindOrThrow(std::string_view sql, const CatalogSnapshotPtr& catalog,
                                      BindEnvironment environment = BindEnvironment::Core()) {
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog, environment);
  if (!bound.has_value()) {
    throw std::runtime_error{bound.error().detail};
  }
  return std::move(*bound);
}

[[nodiscard]] PhysicalPlan OptimizeOrThrow(std::string_view sql, const CatalogSnapshotPtr& catalog,
                                           BindEnvironment environment = BindEnvironment::Core()) {
  BuildLogicalPlanResult logical = BuildLogicalPlan(BindOrThrow(sql, catalog, environment));
  if (!logical.has_value()) {
    throw std::runtime_error{logical.error().detail};
  }
  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical));
  if (!physical.has_value()) {
    throw std::runtime_error{physical.error().detail};
  }
  return std::move(*physical);
}

[[nodiscard]] PhysicalStatementPlan OptimizeStatementOrThrow(
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
  return std::move(*physical);
}

[[nodiscard]] std::string QuoteSqlIdentifier(std::string_view name) {
  std::string result{"\""};
  for (const char byte : name) {
    if (byte == '"') {
      result.push_back('"');
    }
    result.push_back(byte);
  }
  result.push_back('"');
  return result;
}

TEST(PhysicalPlanApi, ExposesStableKindsErrorsAndOwnership) {
  EXPECT_EQ("single_row", PhysicalAccessKindName(PhysicalAccessKind::kSingleRow));
  EXPECT_EQ("empty", PhysicalAccessKindName(PhysicalAccessKind::kEmpty));
  EXPECT_EQ("table_scan", PhysicalAccessKindName(PhysicalAccessKind::kTableScan));
  EXPECT_EQ("rowid_lookup", PhysicalAccessKindName(PhysicalAccessKind::kRowIdLookup));
  EXPECT_EQ("unknown",
            PhysicalAccessKindName(static_cast<PhysicalAccessKind>(255)));  // NOLINT
  EXPECT_EQ("empty", MutationAccessKindName(MutationAccessKind::kEmpty));
  EXPECT_EQ("rowid_lookup", MutationAccessKindName(MutationAccessKind::kRowIdLookup));
  EXPECT_EQ("unknown",
            MutationAccessKindName(static_cast<MutationAccessKind>(255)));  // NOLINT
  EXPECT_EQ("statement", MutationAtomicityName(MutationAtomicity::kStatement));
  EXPECT_EQ("unknown",
            MutationAtomicityName(static_cast<MutationAtomicity>(255)));  // NOLINT

  EXPECT_EQ("guard", PhysicalNodeKindName(PhysicalNodeKind::kGuard));
  EXPECT_EQ("projection", PhysicalNodeKindName(PhysicalNodeKind::kProjection));
  EXPECT_EQ("unknown",
            PhysicalNodeKindName(static_cast<PhysicalNodeKind>(255)));  // NOLINT

  EXPECT_EQ("invalid_input", OptimizerErrorCodeName(OptimizerErrorCode::kInvalidInput));
  EXPECT_EQ("internal_invariant", OptimizerErrorCodeName(OptimizerErrorCode::kInternalInvariant));
  EXPECT_EQ("unknown",
            OptimizerErrorCodeName(static_cast<OptimizerErrorCode>(255)));  // NOLINT
  EXPECT_EQ(ErrorCode::kMisuse,
            OptimizerError{.code = OptimizerErrorCode::kInvalidInput}.base_error_code());
  EXPECT_EQ(ErrorCode::kInternal,
            OptimizerError{.code = OptimizerErrorCode::kInternalInvariant}.base_error_code());
}

TEST(PhysicalMutationPlan, ChoosesDeterministicUpdateAndDeleteAccess) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  PhysicalStatementPlan exact_update_statement =
      OptimizeStatementOrThrow("UPDATE Items SET Name=?1 WHERE id=?2", catalog);
  const auto& exact_update_plan = std::get<PhysicalMutationPlan>(exact_update_statement);
  const auto& exact_update = std::get<PhysicalUpdateMutation>(exact_update_plan.payload());
  EXPECT_EQ(MutationAccessKind::kRowIdLookup, exact_update.access.kind);
  EXPECT_TRUE(exact_update.access.key.has_value());
  EXPECT_TRUE(exact_update.access.residuals.empty());
  EXPECT_EQ(MutationAtomicity::kStatement, exact_update.atomicity);
  EXPECT_FALSE(exact_update.collect_original_rowids);

  PhysicalStatementPlan exact_moving_update_statement =
      OptimizeStatementOrThrow("UPDATE Items SET id=id+1 WHERE id=?1", catalog);
  const auto& exact_moving_update = std::get<PhysicalUpdateMutation>(
      std::get<PhysicalMutationPlan>(exact_moving_update_statement).payload());
  EXPECT_EQ(MutationAccessKind::kRowIdLookup, exact_moving_update.access.kind);
  EXPECT_EQ(MutationAtomicity::kStatement, exact_moving_update.atomicity);
  EXPECT_FALSE(exact_moving_update.collect_original_rowids);

  PhysicalStatementPlan scan_update_statement =
      OptimizeStatementOrThrow("UPDATE Items SET Name=?1 WHERE Name=?2", catalog);
  const auto& scan_update = std::get<PhysicalUpdateMutation>(
      std::get<PhysicalMutationPlan>(scan_update_statement).payload());
  EXPECT_EQ(MutationAccessKind::kTableScan, scan_update.access.kind);
  EXPECT_EQ(MutationAtomicity::kStatement, scan_update.atomicity);
  EXPECT_FALSE(scan_update.collect_original_rowids);

  PhysicalStatementPlan moving_update_statement =
      OptimizeStatementOrThrow("UPDATE Items SET id=id+1 WHERE Name=?1", catalog);
  const auto& moving_update = std::get<PhysicalUpdateMutation>(
      std::get<PhysicalMutationPlan>(moving_update_statement).payload());
  EXPECT_EQ(MutationAccessKind::kTableScan, moving_update.access.kind);
  EXPECT_EQ(MutationAtomicity::kStatement, moving_update.atomicity);
  EXPECT_TRUE(moving_update.collect_original_rowids);

  PhysicalStatementPlan exact_delete_statement =
      OptimizeStatementOrThrow("DELETE FROM Items WHERE rowid=?1", catalog);
  const auto& exact_delete = std::get<PhysicalDeleteMutation>(
      std::get<PhysicalMutationPlan>(exact_delete_statement).payload());
  EXPECT_EQ(MutationAccessKind::kRowIdLookup, exact_delete.access.kind);
  EXPECT_EQ(MutationAtomicity::kStatement, exact_delete.atomicity);

  PhysicalStatementPlan scan_delete_statement =
      OptimizeStatementOrThrow("DELETE FROM Items WHERE Name=?1", catalog);
  const auto& scan_delete = std::get<PhysicalDeleteMutation>(
      std::get<PhysicalMutationPlan>(scan_delete_statement).payload());
  EXPECT_EQ(MutationAccessKind::kTableScan, scan_delete.access.kind);
  EXPECT_EQ(MutationAtomicity::kStatement, scan_delete.atomicity);
}

TEST(PhysicalMutationPlan, PreservesGuardsResidualsAndEmptyPredicates) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  PhysicalStatementPlan guarded_statement =
      OptimizeStatementOrThrow("DELETE FROM Items WHERE stable_guard(?1)=1 AND id=?2 AND Name=?3",
                               catalog, TestEnvironment());
  const auto& guarded =
      std::get<PhysicalDeleteMutation>(std::get<PhysicalMutationPlan>(guarded_statement).payload());
  EXPECT_EQ(MutationAccessKind::kRowIdLookup, guarded.access.kind);
  ASSERT_EQ(1U, guarded.access.guards.size());
  ASSERT_EQ(1U, guarded.access.residuals.size());

  PhysicalStatementPlan empty_statement =
      OptimizeStatementOrThrow("UPDATE Items SET Name='x' WHERE 0", catalog);
  const auto& empty =
      std::get<PhysicalUpdateMutation>(std::get<PhysicalMutationPlan>(empty_statement).payload());
  EXPECT_EQ(MutationAccessKind::kEmpty, empty.access.kind);
  EXPECT_EQ(MutationAtomicity::kTransaction, empty.atomicity);
}

TEST(PhysicalMutationPlan, PlansInsertCreateSelectAndTransactionStatements) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  PhysicalStatementPlan insert_statement =
      OptimizeStatementOrThrow("INSERT INTO Items(Name,id,Value) VALUES(?1,?2,?3)", catalog);
  const auto& insert =
      std::get<PhysicalInsertMutation>(std::get<PhysicalMutationPlan>(insert_statement).payload());
  EXPECT_EQ(MutationAtomicity::kStatement, insert.atomicity);

  PhysicalStatementPlan create_statement =
      OptimizeStatementOrThrow("CREATE TABLE NewItems(id INTEGER PRIMARY KEY)", catalog);
  const auto& create = std::get<PhysicalCreateTableMutation>(
      std::get<PhysicalMutationPlan>(create_statement).payload());
  EXPECT_FALSE(create.no_op);
  EXPECT_EQ(MutationAtomicity::kStatement, create.atomicity);

  PhysicalStatementPlan no_op_statement =
      OptimizeStatementOrThrow("CREATE TABLE IF NOT EXISTS Items(a UNIQUE)", catalog);
  const auto& no_op = std::get<PhysicalCreateTableMutation>(
      std::get<PhysicalMutationPlan>(no_op_statement).payload());
  EXPECT_TRUE(no_op.no_op);
  EXPECT_EQ(MutationAtomicity::kTransaction, no_op.atomicity);

  EXPECT_TRUE(std::holds_alternative<PhysicalPlan>(
      OptimizeStatementOrThrow("SELECT Name FROM Items", catalog)));
  EXPECT_TRUE(std::holds_alternative<BoundBeginTransaction>(
      OptimizeStatementOrThrow("BEGIN IMMEDIATE", catalog)));
}

TEST(PhysicalMutationPlan, RejectsMovedFromLogicalMutationAndMovesOwnership) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  BindStatementResult bound = BindStatement(ParseTree("DELETE FROM Items WHERE id=?1"), catalog);
  ASSERT_TRUE(bound.has_value());
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  ASSERT_TRUE(logical.has_value());

  LogicalStatementPlan retained = std::move(*logical);
  LogicalStatementPlan source = std::move(retained);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  OptimizeLogicalStatementPlanResult rejected = OptimizeLogicalStatementPlan(std::move(retained));
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(OptimizerErrorCode::kInvalidInput, rejected.error().code);

  OptimizeLogicalStatementPlanResult built = OptimizeLogicalStatementPlan(std::move(source));
  ASSERT_TRUE(built.has_value());
  PhysicalMutationPlan plan = std::get<PhysicalMutationPlan>(std::move(*built));
  const PhysicalMutationPlan moved = std::move(plan);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(plan.valid());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(moved.valid());
}

TEST(PhysicalPlan, BuildsConstantRowAndMovesRetainedLogicalPlan) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  LogicalPlan logical = *BuildLogicalPlan(BindOrThrow("SELECT 1 AS one, ? AS value", catalog));
  const CatalogSnapshot* identity = logical.bound_select().catalog();
  OptimizeLogicalPlanResult built = OptimizeLogicalPlan(std::move(logical));
  ASSERT_TRUE(built.has_value());
  PhysicalPlan plan = std::move(*built);

  ASSERT_TRUE(plan.valid());
  ASSERT_EQ(2U, plan.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalSingleRowNode>(plan.nodes()[0].payload));
  EXPECT_EQ(PhysicalNodeId{1}, plan.root());
  EXPECT_EQ(LogicalNodeId{1},
            std::get<PhysicalProjectionNode>(plan.nodes()[1].payload).logical_projection);
  ASSERT_EQ(1U, plan.candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kSingleRow, plan.selected_candidate().kind);
  EXPECT_EQ(
      (AccessPathCost{.estimated_input_rows = 1, .estimated_output_rows = 1, .work_units = 1}),
      plan.selected_candidate().cost);
  EXPECT_EQ("SCAN CONSTANT ROW", ExplainPhysicalPlan(plan));
  EXPECT_EQ(identity, plan.logical_plan().bound_select().catalog());

  const PhysicalPlan moved = std::move(plan);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(plan.valid());
  EXPECT_TRUE(plan.nodes().empty());
  EXPECT_TRUE(plan.candidates().empty());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(moved.valid());
}

TEST(PhysicalPlan, ChoosesFullScanWithExplicitCostAndResidualFilter) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan plan = OptimizeOrThrow("SELECT Name FROM Items WHERE Name=?", catalog);

  ASSERT_EQ(3U, plan.nodes().size());
  const auto& scan = std::get<PhysicalTableScanNode>(plan.nodes()[0].payload);
  EXPECT_EQ(BoundSourceKind::kCatalogTable, scan.source_kind);
  EXPECT_EQ(TableId{0}, scan.table);
  EXPECT_EQ(RootPageId{2}, scan.root_page);

  const auto& filter = std::get<PhysicalFilterNode>(plan.nodes()[1].payload);
  EXPECT_EQ(PhysicalNodeId{0}, filter.input);
  ASSERT_EQ(1U, filter.predicates.size());
  EXPECT_EQ(plan.logical_plan().bound_select().where_expression(), filter.predicates[0]);

  ASSERT_EQ(1U, plan.candidates().size());
  EXPECT_EQ(0U, plan.selected_candidate_index());
  EXPECT_EQ(PhysicalAccessKind::kTableScan, plan.selected_candidate().kind);
  EXPECT_EQ((AccessPathCost{
                .estimated_input_rows = 100, .estimated_output_rows = 100, .work_units = 100}),
            plan.selected_candidate().cost);
  EXPECT_EQ("SCAN \"Items\"", ExplainPhysicalPlan(plan));
}

TEST(PhysicalPlan, ChoosesRowIdLookupAndPublishesBothCandidates) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan plan = OptimizeOrThrow("SELECT Name FROM Items WHERE rowid=?", catalog);

  ASSERT_EQ(2U, plan.nodes().size());
  const auto& lookup = std::get<PhysicalRowIdLookupNode>(plan.nodes()[0].payload);
  EXPECT_EQ(BoundSourceKind::kCatalogTable, lookup.source_kind);
  EXPECT_EQ(TableId{0}, lookup.table);
  EXPECT_EQ(RootPageId{2}, lookup.root_page);
  EXPECT_TRUE(std::holds_alternative<BoundParameterExpression>(
      plan.logical_plan().bound_select().expression(lookup.key).payload));

  ASSERT_EQ(2U, plan.candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kTableScan, plan.candidates()[0].kind);
  EXPECT_EQ(PhysicalAccessKind::kRowIdLookup, plan.candidates()[1].kind);
  EXPECT_EQ(
      (AccessPathCost{.estimated_input_rows = 100, .estimated_output_rows = 1, .work_units = 7}),
      plan.candidates()[1].cost);
  EXPECT_EQ(1U, plan.selected_candidate_index());
  EXPECT_EQ(plan.candidates()[1], plan.selected_candidate());
  EXPECT_EQ("SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid=?)", ExplainPhysicalPlan(plan));
}

TEST(PhysicalPlan, RecognizesCompatibleRowIdSpellingsAndTieBreaksLookup) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const std::array<std::string_view, 7> queries{
      "SELECT Name FROM Items WHERE rowid IS ?",
      "SELECT Name FROM Items WHERE ?=rowid",
      "SELECT Name FROM Items WHERE id=?",
      "SELECT Name FROM Items WHERE likely(rowid=?)",
      "SELECT Name FROM Items WHERE rowid COLLATE NOCASE=?",
      "SELECT rowid AS key FROM Items WHERE key=?",
      "SELECT Name FROM Items WHERE rowid=NULL",
  };
  for (const std::string_view sql : queries) {
    const PhysicalPlan plan = OptimizeOrThrow(sql, catalog);
    EXPECT_TRUE(std::holds_alternative<PhysicalRowIdLookupNode>(plan.nodes()[0].payload)) << sql;
  }

  const PhysicalPlan tiny = OptimizeOrThrow("SELECT id FROM Tiny WHERE rowid=?", catalog);
  ASSERT_EQ(2U, tiny.candidates().size());
  EXPECT_EQ(1U, tiny.candidates()[0].cost.work_units);
  EXPECT_EQ(1U, tiny.candidates()[1].cost.work_units);
  EXPECT_EQ(PhysicalAccessKind::kRowIdLookup, tiny.selected_candidate().kind);
}

TEST(PhysicalPlan, PreservesFirstOriginalRowIdKeyAndResidualOccurrences) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan collated =
      OptimizeOrThrow("SELECT Name FROM Items WHERE rowid=(? COLLATE NOCASE)", catalog);
  const auto& collated_lookup = std::get<PhysicalRowIdLookupNode>(collated.nodes()[0].payload);
  const BoundSelect& collated_bound = collated.logical_plan().bound_select();
  const auto& collated_key =
      std::get<BoundCollateExpression>(collated_bound.expression(collated_lookup.key).payload);
  EXPECT_TRUE(std::holds_alternative<BoundParameterExpression>(
      collated_bound.expression(collated_key.operand).payload));

  const PhysicalPlan repeated =
      OptimizeOrThrow("SELECT Name FROM Items WHERE rowid=?1 AND rowid=?2", catalog);
  const auto& repeated_lookup = std::get<PhysicalRowIdLookupNode>(repeated.nodes()[0].payload);
  const BoundSelect& repeated_bound = repeated.logical_plan().bound_select();
  EXPECT_EQ(
      0U, std::get<BoundParameterExpression>(repeated_bound.expression(repeated_lookup.key).payload)
              .parameter.value());

  const auto& filter = std::get<PhysicalFilterNode>(repeated.nodes()[1].payload);
  ASSERT_EQ(1U, filter.predicates.size());
  const auto& residual =
      std::get<BoundComparisonExpression>(repeated_bound.expression(filter.predicates[0]).payload);
  EXPECT_EQ(1U,
            std::get<BoundParameterExpression>(repeated_bound.expression(residual.right).payload)
                .parameter.value());
}

TEST(PhysicalPlan, RejectsAffinityBarriersAndSourceDependentKeys) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const std::array<std::string_view, 4> queries{
      "SELECT Name FROM Items WHERE +rowid=?",
      "SELECT Name FROM Items WHERE likely(rowid)='1'",
      "SELECT Name FROM Items WHERE rowid=Name",
      "SELECT key FROM wr WHERE key=?",
  };
  for (const std::string_view sql : queries) {
    const PhysicalPlan plan = OptimizeOrThrow(sql, catalog);
    EXPECT_TRUE(std::holds_alternative<PhysicalTableScanNode>(plan.nodes()[0].payload)) << sql;
  }
}

TEST(PhysicalPlan, OrdersGuardLookupResidualLimitAndProjection) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT Name FROM Items "
      "WHERE stable_guard(?)=1 AND rowid=volatile_key() AND Name=? "
      "LIMIT 1 OFFSET 1",
      catalog, TestEnvironment());

  ASSERT_EQ(5U, plan.nodes().size());
  const auto& lookup = std::get<PhysicalRowIdLookupNode>(plan.nodes()[0].payload);
  const auto& key_call = std::get<BoundScalarCallExpression>(
      plan.logical_plan().bound_select().expression(lookup.key).payload);
  EXPECT_EQ("volatile_key",
            plan.logical_plan().bound_select().functions()[key_call.function.value()].name);

  const auto& guard = std::get<PhysicalGuardNode>(plan.nodes()[1].payload);
  EXPECT_EQ(PhysicalNodeId{0}, guard.input);
  ASSERT_EQ(1U, guard.predicates.size());
  const auto& guard_comparison = std::get<BoundComparisonExpression>(
      plan.logical_plan().bound_select().expression(guard.predicates[0]).payload);
  const auto& guard_call = std::get<BoundScalarCallExpression>(
      plan.logical_plan().bound_select().expression(guard_comparison.left).payload);
  EXPECT_EQ("stable_guard",
            plan.logical_plan().bound_select().functions()[guard_call.function.value()].name);

  const auto& filter = std::get<PhysicalFilterNode>(plan.nodes()[2].payload);
  EXPECT_EQ(PhysicalNodeId{1}, filter.input);
  ASSERT_EQ(1U, filter.predicates.size());

  const auto& limit = std::get<PhysicalLimitNode>(plan.nodes()[3].payload);
  EXPECT_EQ(PhysicalNodeId{2}, limit.input);
  ASSERT_NE(nullptr, plan.logical_plan().bound_select().limit());
  EXPECT_EQ(plan.logical_plan().bound_select().limit()->limit, limit.limit);
  EXPECT_EQ(plan.logical_plan().bound_select().limit()->offset, limit.offset);

  const auto& projection = std::get<PhysicalProjectionNode>(plan.nodes()[4].payload);
  EXPECT_EQ(PhysicalNodeId{3}, projection.input);
  EXPECT_EQ(plan.logical_plan().root(), projection.logical_projection);
}

TEST(PhysicalPlan, PreservesPriorGuardsWhenPredicateBecomesEmpty) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan guarded_empty = OptimizeOrThrow(
      "SELECT Name FROM Items "
      "WHERE stable_guard(?)=1 AND 0 AND rowid=volatile_key()",
      catalog, TestEnvironment());

  ASSERT_EQ(3U, guarded_empty.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalEmptyNode>(guarded_empty.nodes()[0].payload));
  const auto& guard = std::get<PhysicalGuardNode>(guarded_empty.nodes()[1].payload);
  ASSERT_EQ(1U, guard.predicates.size());
  EXPECT_TRUE(std::holds_alternative<PhysicalProjectionNode>(guarded_empty.nodes()[2].payload));
  EXPECT_EQ(PhysicalAccessKind::kEmpty, guarded_empty.selected_candidate().kind);
  EXPECT_EQ((AccessPathCost{}), guarded_empty.selected_candidate().cost);
  EXPECT_EQ("EMPTY RESULT", ExplainPhysicalPlan(guarded_empty));

  const PhysicalPlan immediate_empty = OptimizeOrThrow(
      "SELECT Name FROM Items WHERE 0 AND stable_guard(?)=1", catalog, TestEnvironment());
  ASSERT_EQ(2U, immediate_empty.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalEmptyNode>(immediate_empty.nodes()[0].payload));

  const PhysicalPlan null_empty = OptimizeOrThrow("SELECT Name FROM Items WHERE NULL", catalog);
  EXPECT_TRUE(std::holds_alternative<PhysicalEmptyNode>(null_empty.nodes()[0].payload));

  const PhysicalPlan true_scan = OptimizeOrThrow("SELECT Name FROM Items WHERE 1", catalog);
  ASSERT_EQ(2U, true_scan.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalTableScanNode>(true_scan.nodes()[0].payload));
}

TEST(PhysicalPlan, PreservesNoFromPredicateOrderAndNondeterministicEffects) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const PhysicalPlan guarded_empty = OptimizeOrThrow(
      "SELECT 1 WHERE volatile_key() AND stable_guard(?) AND 0", catalog, TestEnvironment());
  ASSERT_EQ(3U, guarded_empty.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalEmptyNode>(guarded_empty.nodes()[0].payload));
  const auto& guard = std::get<PhysicalGuardNode>(guarded_empty.nodes()[1].payload);
  ASSERT_EQ(2U, guard.predicates.size());
  const BoundSelect& bound = guarded_empty.logical_plan().bound_select();
  const auto& volatile_call =
      std::get<BoundScalarCallExpression>(bound.expression(guard.predicates[0]).payload);
  const auto& stable_call =
      std::get<BoundScalarCallExpression>(bound.expression(guard.predicates[1]).payload);
  EXPECT_EQ("volatile_key", bound.functions()[volatile_call.function.value()].name);
  EXPECT_EQ("stable_guard", bound.functions()[stable_call.function.value()].name);

  const PhysicalPlan immediate_empty =
      OptimizeOrThrow("SELECT 1 WHERE 0 AND volatile_key()", catalog, TestEnvironment());
  ASSERT_EQ(2U, immediate_empty.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalEmptyNode>(immediate_empty.nodes()[0].payload));

  const PhysicalPlan guarded_row =
      OptimizeOrThrow("SELECT 1 WHERE volatile_key()", catalog, TestEnvironment());
  ASSERT_EQ(3U, guarded_row.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalSingleRowNode>(guarded_row.nodes()[0].payload));
  EXPECT_TRUE(std::holds_alternative<PhysicalGuardNode>(guarded_row.nodes()[1].payload));
  EXPECT_TRUE(std::holds_alternative<PhysicalProjectionNode>(guarded_row.nodes()[2].payload));
}

TEST(PhysicalPlan, SplitsWrappedConjunctionsAndKeepsResidualOrder) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const std::array<std::string_view, 3> queries{
      "SELECT Name FROM Items WHERE likely(rowid=? AND Name=?)",
      "SELECT Name FROM Items WHERE (rowid=? AND Name=?) COLLATE NOCASE",
      "SELECT (rowid=?1 AND Name=?2) AS keep FROM Items WHERE keep",
  };
  for (const std::string_view sql : queries) {
    const PhysicalPlan plan = OptimizeOrThrow(sql, catalog);
    EXPECT_TRUE(std::holds_alternative<PhysicalRowIdLookupNode>(plan.nodes()[0].payload)) << sql;
    ASSERT_TRUE(std::holds_alternative<PhysicalFilterNode>(plan.nodes()[1].payload)) << sql;
    const auto& filter = std::get<PhysicalFilterNode>(plan.nodes()[1].payload);
    EXPECT_EQ(1U, filter.predicates.size()) << sql;
  }

  const PhysicalPlan ordered =
      OptimizeOrThrow("SELECT Name FROM Items WHERE Value>? AND rowid=? AND Name=?", catalog);
  const auto& filter = std::get<PhysicalFilterNode>(ordered.nodes()[1].payload);
  ASSERT_EQ(2U, filter.predicates.size());
  const BoundSelect& bound = ordered.logical_plan().bound_select();
  EXPECT_TRUE(std::holds_alternative<BoundComparisonExpression>(
      bound.expression(filter.predicates[0]).payload));
  EXPECT_TRUE(std::holds_alternative<BoundComparisonExpression>(
      bound.expression(filter.predicates[1]).payload));
  EXPECT_LT(filter.predicates[0].value(), filter.predicates[1].value());
}

TEST(PhysicalPlan, SupportsSchemaRowIdAndCanonicalExplainEscaping) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan schema =
      OptimizeOrThrow("SELECT name FROM main.sqlite_schema WHERE rowid=?", catalog);
  const auto& lookup = std::get<PhysicalRowIdLookupNode>(schema.nodes()[0].payload);
  EXPECT_EQ(BoundSourceKind::kSchemaTable, lookup.source_kind);
  EXPECT_FALSE(lookup.table.has_value());
  EXPECT_EQ(RootPageId{1}, lookup.root_page);
  EXPECT_EQ((AccessPathCost{
                .estimated_input_rows = 1U << 20U,
                .estimated_output_rows = 1,
                .work_units = 21,
            }),
            schema.selected_candidate().cost);
  EXPECT_EQ("SEARCH \"sqlite_schema\" USING INTEGER PRIMARY KEY (rowid=?)",
            ExplainPhysicalPlan(schema));

  std::string unusual{"A\"\\\n"};
  unusual.push_back(static_cast<char>(0xC0));
  unusual.append("\xC3\xA9");
  const CatalogSnapshotPtr unusual_catalog = TestCatalog(unusual);
  const std::string unusual_sql = "SELECT Name FROM " + QuoteSqlIdentifier(unusual);
  const PhysicalPlan unusual_plan = OptimizeOrThrow(unusual_sql, unusual_catalog);
  EXPECT_EQ("SCAN \"A\\\"\\\\\\x0A\\xC0\\xC3\\xA9\"", ExplainPhysicalPlan(unusual_plan));

  const CatalogSnapshotPtr empty_catalog = TestCatalog("");
  const PhysicalPlan empty_name = OptimizeOrThrow("SELECT Name FROM \"\"", empty_catalog);
  EXPECT_EQ("SCAN \"\"", ExplainPhysicalPlan(empty_name));
}

TEST(PhysicalPlan, RejectsMovedFromLogicalInput) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  BuildLogicalPlanResult built = BuildLogicalPlan(BindOrThrow("SELECT Name FROM Items", catalog));
  ASSERT_TRUE(built.has_value());
  LogicalPlan source = std::move(*built);
  LogicalPlan retained = std::move(source);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  OptimizeLogicalPlanResult rejected = OptimizeLogicalPlan(std::move(source));
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(OptimizerErrorCode::kInvalidInput, rejected.error().code);
  EXPECT_EQ(ErrorCode::kMisuse, rejected.error().base_error_code());
  EXPECT_EQ("logical plan is invalid", rejected.error().detail);

  const OptimizeLogicalPlanResult recovered = OptimizeLogicalPlan(std::move(retained));
  EXPECT_TRUE(recovered.has_value());
}

}  // namespace
}  // namespace modern_sqlite
