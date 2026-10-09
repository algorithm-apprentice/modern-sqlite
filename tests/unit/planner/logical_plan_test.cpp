#include "modern_sqlite/planner/logical_plan.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/syntax/parser.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<LogicalPlan>);
static_assert(!std::is_copy_assignable_v<LogicalPlan>);
static_assert(std::is_nothrow_move_constructible_v<LogicalPlan>);
static_assert(std::is_nothrow_move_assignable_v<LogicalPlan>);
static_assert(!std::is_copy_constructible_v<LogicalMutationPlan>);
static_assert(std::is_nothrow_move_constructible_v<LogicalMutationPlan>);
static_assert(!std::is_copy_constructible_v<LogicalStatementPlan>);
static_assert(std::is_nothrow_move_constructible_v<LogicalStatementPlan>);
static_assert(!std::is_convertible_v<LogicalNodeId, BoundExpressionId>);
static_assert(!std::is_convertible_v<BoundExpressionId, LogicalNodeId>);

[[nodiscard]] SyntaxTree ParseTree(std::string_view sql) {
  ParseResult parsed = ParseOne(Utf8View{sql});
  if (!parsed.has_value()) {
    throw std::runtime_error{std::string{ParseErrorMessage(parsed.error())}};
  }
  if (!parsed->tree.has_value()) {
    throw std::runtime_error{"logical-plan test SQL did not produce a statement"};
  }
  return std::move(*parsed->tree);
}

[[nodiscard]] CatalogSnapshotPtr TestCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 17, .generation = 9},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT COLLATE NOCASE)"));
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
                  .collation_name = "NOCASE",
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

[[nodiscard]] BoundSelect BindOrThrow(std::string_view sql, const CatalogSnapshotPtr& catalog) {
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{bound.error().detail};
  }
  return std::move(*bound);
}

[[nodiscard]] LogicalPlan PlanOrThrow(std::string_view sql, const CatalogSnapshotPtr& catalog) {
  BuildLogicalPlanResult plan = BuildLogicalPlan(BindOrThrow(sql, catalog));
  if (!plan.has_value()) {
    throw std::runtime_error{plan.error().detail};
  }
  return std::move(*plan);
}

[[nodiscard]] LogicalStatementPlan StatementPlanOrThrow(std::string_view sql,
                                                        const CatalogSnapshotPtr& catalog) {
  BindStatementResult bound = BindStatement(ParseTree(sql), catalog);
  if (!bound.has_value()) {
    throw std::runtime_error{bound.error().detail};
  }
  BuildLogicalStatementPlanResult plan = BuildLogicalStatementPlan(std::move(*bound));
  if (!plan.has_value()) {
    throw std::runtime_error{plan.error().detail};
  }
  return std::move(*plan);
}

TEST(LogicalPlanApi, ExposesStableKindsErrorsAndOwnership) {
  EXPECT_EQ("single_row", LogicalNodeKindName(LogicalNodeKind::kSingleRow));
  EXPECT_EQ("scan", LogicalNodeKindName(LogicalNodeKind::kScan));
  EXPECT_EQ("filter", LogicalNodeKindName(LogicalNodeKind::kFilter));
  EXPECT_EQ("limit", LogicalNodeKindName(LogicalNodeKind::kLimit));
  EXPECT_EQ("projection", LogicalNodeKindName(LogicalNodeKind::kProjection));
  EXPECT_EQ("order", LogicalNodeKindName(LogicalNodeKind::kOrder));
  EXPECT_EQ("output", LogicalNodeKindName(LogicalNodeKind::kOutput));
  EXPECT_EQ("unknown",
            LogicalNodeKindName(static_cast<LogicalNodeKind>(255)));  // NOLINT
  EXPECT_EQ("insert", LogicalMutationKindName(LogicalMutationKind::kInsert));
  EXPECT_EQ("create_table", LogicalMutationKindName(LogicalMutationKind::kCreateTable));
  EXPECT_EQ("create_index", LogicalMutationKindName(LogicalMutationKind::kCreateIndex));
  EXPECT_EQ("analyze", LogicalMutationKindName(LogicalMutationKind::kAnalyze));
  EXPECT_EQ("unknown",
            LogicalMutationKindName(static_cast<LogicalMutationKind>(255)));  // NOLINT

  EXPECT_EQ("invalid_input", LogicalPlanErrorCodeName(LogicalPlanErrorCode::kInvalidInput));
  EXPECT_EQ("unsupported_feature",
            LogicalPlanErrorCodeName(LogicalPlanErrorCode::kUnsupportedFeature));
  EXPECT_EQ("internal_invariant",
            LogicalPlanErrorCodeName(LogicalPlanErrorCode::kInternalInvariant));
  EXPECT_EQ("unknown",
            LogicalPlanErrorCodeName(static_cast<LogicalPlanErrorCode>(255)));  // NOLINT

  EXPECT_EQ(ErrorCode::kMisuse,
            LogicalPlanError{.code = LogicalPlanErrorCode::kInvalidInput}.base_error_code());
  EXPECT_EQ(ErrorCode::kInternal,
            LogicalPlanError{.code = LogicalPlanErrorCode::kInternalInvariant}.base_error_code());
}

TEST(LogicalStatementPlan, BuildsTypedMutationAndTransactionPayloads) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  LogicalStatementPlan insert_statement =
      StatementPlanOrThrow("INSERT INTO Items(Name,id) VALUES(?1,?2)", catalog);
  const auto& insert_plan = std::get<LogicalMutationPlan>(insert_statement);
  EXPECT_EQ(LogicalMutationKind::kInsert, LogicalMutationKindOf(insert_plan.payload()));
  EXPECT_TRUE(std::holds_alternative<LogicalInsertMutation>(insert_plan.payload()));
  EXPECT_TRUE(std::holds_alternative<BoundInsert>(insert_plan.bound_statement()));

  LogicalStatementPlan update_statement =
      StatementPlanOrThrow("UPDATE Items SET Name=?1, id=id+1 WHERE id=?2", catalog);
  const auto& update_plan = std::get<LogicalMutationPlan>(update_statement);
  const auto& update = std::get<LogicalUpdateMutation>(update_plan.payload());
  const auto& bound_update = std::get<BoundUpdate>(update_plan.bound_statement());
  EXPECT_EQ(bound_update.where_expression(), update.predicate);
  EXPECT_TRUE(update.changes_rowid);

  LogicalStatementPlan delete_statement =
      StatementPlanOrThrow("DELETE FROM Items WHERE Name=?1", catalog);
  const auto& delete_plan = std::get<LogicalMutationPlan>(delete_statement);
  const auto& delete_mutation = std::get<LogicalDeleteMutation>(delete_plan.payload());
  EXPECT_EQ(std::get<BoundDelete>(delete_plan.bound_statement()).where_expression(),
            delete_mutation.predicate);

  LogicalStatementPlan create_statement =
      StatementPlanOrThrow("CREATE TABLE NewItems(id INTEGER PRIMARY KEY)", catalog);
  const auto& create_plan = std::get<LogicalMutationPlan>(create_statement);
  EXPECT_FALSE(std::get<LogicalCreateTableMutation>(create_plan.payload()).no_op);

  LogicalStatementPlan create_index_statement =
      StatementPlanOrThrow("CREATE INDEX items_name ON Items(Name)", catalog);
  const auto& create_index_plan = std::get<LogicalMutationPlan>(create_index_statement);
  EXPECT_FALSE(std::get<LogicalCreateIndexMutation>(create_index_plan.payload()).no_op);
  EXPECT_TRUE(std::holds_alternative<BoundCreateIndex>(create_index_plan.bound_statement()));

  LogicalStatementPlan analyze_statement = StatementPlanOrThrow("ANALYZE Items", catalog);
  const auto& analyze_plan = std::get<LogicalMutationPlan>(analyze_statement);
  EXPECT_TRUE(std::get<LogicalAnalyzeMutation>(analyze_plan.payload()).creates_stat1);
  EXPECT_TRUE(std::holds_alternative<BoundAnalyze>(analyze_plan.bound_statement()));

  EXPECT_TRUE(std::holds_alternative<BoundBeginTransaction>(
      StatementPlanOrThrow("BEGIN IMMEDIATE", catalog)));
  EXPECT_TRUE(
      std::holds_alternative<BoundSavepoint>(StatementPlanOrThrow("SAVEPOINT name", catalog)));
}

TEST(LogicalStatementPlan, RetainsSelectAndRejectsMovedFromMutationInput) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  EXPECT_TRUE(
      std::holds_alternative<LogicalPlan>(StatementPlanOrThrow("SELECT Name FROM Items", catalog)));

  BindStatementResult bound = BindStatement(ParseTree("DELETE FROM Items WHERE id=?1"), catalog);
  ASSERT_TRUE(bound.has_value());
  BoundStatement retained = std::move(*bound);
  BoundStatement moved = std::move(retained);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  BuildLogicalStatementPlanResult rejected = BuildLogicalStatementPlan(std::move(retained));
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(LogicalPlanErrorCode::kInvalidInput, rejected.error().code);

  const BuildLogicalStatementPlanResult recovered = BuildLogicalStatementPlan(std::move(moved));
  EXPECT_TRUE(recovered.has_value());
}

TEST(LogicalPlan, BuildsSingleRowProjectionAndRetainsBoundState) {
  CatalogSnapshotPtr catalog = TestCatalog();
  const CatalogSnapshot* catalog_identity = catalog.get();
  LogicalPlan plan = PlanOrThrow("SELECT 1 AS one, ? AS value", catalog);
  catalog.reset();

  ASSERT_TRUE(plan.valid());
  ASSERT_EQ(2U, plan.nodes().size());
  EXPECT_EQ(LogicalNodeId{1}, plan.root());
  EXPECT_EQ(LogicalNodeKind::kSingleRow, LogicalNodeKindOf(plan.nodes()[0]));
  EXPECT_TRUE(std::holds_alternative<LogicalSingleRowNode>(plan.nodes()[0].payload));

  EXPECT_EQ(LogicalNodeKind::kProjection, LogicalNodeKindOf(plan.nodes()[1]));
  const auto& projection = std::get<LogicalProjectionNode>(plan.nodes()[1].payload);
  EXPECT_EQ(LogicalNodeId{0}, projection.input);
  ASSERT_EQ(2U, projection.expressions.size());

  const BoundSelect& bound = plan.bound_select();
  EXPECT_EQ(catalog_identity, bound.catalog());
  EXPECT_EQ("SELECT 1 AS one, ? AS value", bound.source().bytes());
  EXPECT_EQ("one", bound.result_columns()[0].name);
  EXPECT_EQ("value", bound.result_columns()[1].name);
  EXPECT_EQ(bound.result_columns()[0].expression, projection.expressions[0]);
  EXPECT_EQ(bound.result_columns()[1].expression, projection.expressions[1]);

  const LogicalPlan moved = std::move(plan);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(plan.valid());
  EXPECT_TRUE(plan.nodes().empty());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(moved.valid());
  EXPECT_EQ(LogicalNodeId{1}, moved.root());
}

TEST(LogicalPlan, BuildsExactScanFilterLimitProjectionChain) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const LogicalPlan plan =
      PlanOrThrow("SELECT Name, id + ? FROM Items WHERE id > ? LIMIT ?, ?", catalog);

  ASSERT_EQ(4U, plan.nodes().size());
  EXPECT_EQ(LogicalNodeId{3}, plan.root());

  EXPECT_EQ(LogicalNodeKind::kScan, LogicalNodeKindOf(plan.nodes()[0]));
  const auto& scan = std::get<LogicalScanNode>(plan.nodes()[0].payload);
  EXPECT_EQ(BoundSourceKind::kCatalogTable, scan.source_kind);
  EXPECT_EQ(TableId{0}, scan.table);

  const BoundSelect& bound = plan.bound_select();
  ASSERT_NE(nullptr, bound.table_source());
  EXPECT_EQ(bound.table_source()->kind, scan.source_kind);
  EXPECT_EQ(bound.table_source()->table, scan.table);

  EXPECT_EQ(LogicalNodeKind::kFilter, LogicalNodeKindOf(plan.nodes()[1]));
  const auto& filter = std::get<LogicalFilterNode>(plan.nodes()[1].payload);
  EXPECT_EQ(LogicalNodeId{0}, filter.input);
  ASSERT_TRUE(bound.where_expression().has_value());
  EXPECT_EQ(*bound.where_expression(), filter.predicate);

  EXPECT_EQ(LogicalNodeKind::kLimit, LogicalNodeKindOf(plan.nodes()[2]));
  const auto& limit = std::get<LogicalLimitNode>(plan.nodes()[2].payload);
  EXPECT_EQ(LogicalNodeId{1}, limit.input);
  ASSERT_NE(nullptr, bound.limit());
  EXPECT_EQ(bound.limit()->limit, limit.limit);
  EXPECT_EQ(bound.limit()->offset, limit.offset);

  EXPECT_EQ(LogicalNodeKind::kProjection, LogicalNodeKindOf(plan.nodes()[3]));
  const auto& projection = std::get<LogicalProjectionNode>(plan.nodes()[3].payload);
  EXPECT_EQ(LogicalNodeId{2}, projection.input);
  ASSERT_EQ(bound.result_columns().size(), projection.expressions.size());
  for (std::size_t index = 0; index < projection.expressions.size(); ++index) {
    EXPECT_EQ(bound.result_columns()[index].expression, projection.expressions[index]);
  }

  for (std::uint32_t index = 0; index < plan.nodes().size(); ++index) {
    EXPECT_EQ(&plan.nodes()[index], &plan.node(LogicalNodeId{index}));
  }
}

TEST(LogicalPlan, PreservesSchemaSourceAndOptionalOperators) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const LogicalPlan schema =
      PlanOrThrow("SELECT name FROM main.sqlite_schema WHERE rootpage > 0", catalog);

  ASSERT_EQ(3U, schema.nodes().size());
  const auto& schema_scan = std::get<LogicalScanNode>(schema.nodes()[0].payload);
  EXPECT_EQ(BoundSourceKind::kSchemaTable, schema_scan.source_kind);
  EXPECT_FALSE(schema_scan.table.has_value());
  EXPECT_TRUE(std::holds_alternative<LogicalFilterNode>(schema.nodes()[1].payload));
  EXPECT_TRUE(std::holds_alternative<LogicalProjectionNode>(schema.nodes()[2].payload));

  const LogicalPlan table = PlanOrThrow("SELECT Name FROM Items", catalog);
  ASSERT_EQ(2U, table.nodes().size());
  EXPECT_TRUE(std::holds_alternative<LogicalScanNode>(table.nodes()[0].payload));
  const auto& projection = std::get<LogicalProjectionNode>(table.nodes()[1].payload);
  EXPECT_EQ(LogicalNodeId{0}, projection.input);

  const LogicalPlan limited_single_row =
      PlanOrThrow("SELECT abs(?) WHERE ? LIMIT 0 OFFSET abs(?)", catalog);
  ASSERT_EQ(4U, limited_single_row.nodes().size());
  EXPECT_TRUE(std::holds_alternative<LogicalSingleRowNode>(limited_single_row.nodes()[0].payload));
  EXPECT_TRUE(std::holds_alternative<LogicalFilterNode>(limited_single_row.nodes()[1].payload));
  EXPECT_TRUE(std::holds_alternative<LogicalLimitNode>(limited_single_row.nodes()[2].payload));
  EXPECT_TRUE(std::holds_alternative<LogicalProjectionNode>(limited_single_row.nodes()[3].payload));
  EXPECT_EQ(LogicalNodeId{3}, limited_single_row.root());
}

TEST(LogicalPlan, PreservesAliasIdentityAndRejectsMovedFromInput) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const LogicalPlan plan =
      PlanOrThrow("SELECT abs(?) AS magnitude FROM Items WHERE magnitude > 0 LIMIT 1", catalog);

  const BoundSelect& bound = plan.bound_select();
  ASSERT_TRUE(bound.where_expression().has_value());
  const auto& comparison =
      std::get<BoundComparisonExpression>(bound.expression(*bound.where_expression()).payload);
  const auto& alias =
      std::get<BoundAliasReferenceExpression>(bound.expression(comparison.left).payload);
  EXPECT_EQ(bound.result_columns()[0].expression, alias.target);
  const auto& filter = std::get<LogicalFilterNode>(plan.nodes()[1].payload);
  EXPECT_EQ(*bound.where_expression(), filter.predicate);

  BoundSelect source = BindOrThrow("SELECT 1", catalog);
  BoundSelect retained = std::move(source);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  BuildLogicalPlanResult rejected = BuildLogicalPlan(std::move(source));
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(LogicalPlanErrorCode::kInvalidInput, rejected.error().code);
  EXPECT_EQ(ErrorCode::kMisuse, rejected.error().base_error_code());
  EXPECT_EQ("bound select is invalid", rejected.error().detail);

  const BuildLogicalPlanResult recovered = BuildLogicalPlan(std::move(retained));
  EXPECT_TRUE(recovered.has_value());
}

TEST(LogicalPlan, BuildsOrderedOutputMappingsAndPayloadFirstSchedule) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const LogicalPlan plan = PlanOrThrow(
      "SELECT Name, id + 1, abs(?) FROM Items WHERE id > ? "
      "ORDER BY Name, id + 1, Name DESC NULLS FIRST",
      catalog);

  ASSERT_EQ(4U, plan.nodes().size());
  EXPECT_TRUE(std::holds_alternative<LogicalScanNode>(plan.nodes()[0].payload));
  EXPECT_TRUE(std::holds_alternative<LogicalFilterNode>(plan.nodes()[1].payload));

  const auto& order = std::get<LogicalOrderNode>(plan.nodes()[2].payload);
  EXPECT_EQ(LogicalNodeId{1}, order.input);
  EXPECT_EQ(OrderEvaluationSchedule::kPayloadThenKeys, order.schedule);
  ASSERT_EQ(3U, order.terms.size());
  ASSERT_EQ(1U, order.payload_expressions.size());
  ASSERT_EQ(3U, order.output_fields.size());

  const BoundSelect& bound = plan.bound_select();
  ASSERT_EQ(3U, bound.order_by().size());
  EXPECT_EQ(bound.order_by()[0].expression, order.terms[0].expression);
  EXPECT_EQ(bound.order_by()[1].expression, order.terms[1].expression);
  EXPECT_EQ(bound.order_by()[2].expression, order.terms[2].expression);
  EXPECT_EQ(0U, order.terms[0].result_column);
  EXPECT_EQ(1U, order.terms[1].result_column);
  EXPECT_EQ(0U, order.terms[2].result_column);
  EXPECT_EQ(bound.result_columns()[2].expression, order.payload_expressions[0]);
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kKey, .field_index = 2}),
            order.output_fields[0]);
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kKey, .field_index = 1}),
            order.output_fields[1]);
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kPayload, .field_index = 0}),
            order.output_fields[2]);

  const auto& output = std::get<LogicalOutputNode>(plan.nodes()[3].payload);
  EXPECT_EQ(LogicalNodeId{2}, output.input);
  EXPECT_EQ(LogicalNodeId{3}, plan.root());
}

TEST(LogicalPlan, PlacesOrderedLimitAfterOrderAndUsesKeyFirstSchedule) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const LogicalPlan plan = PlanOrThrow(
      "SELECT Name, abs(?) FROM Items WHERE id > ? "
      "ORDER BY Name LIMIT ? OFFSET ?",
      catalog);

  ASSERT_EQ(5U, plan.nodes().size());
  const auto& order = std::get<LogicalOrderNode>(plan.nodes()[2].payload);
  EXPECT_EQ(LogicalNodeId{1}, order.input);
  EXPECT_EQ(OrderEvaluationSchedule::kKeysThenAdmissionThenPayload, order.schedule);
  ASSERT_EQ(1U, order.payload_expressions.size());
  EXPECT_EQ(plan.bound_select().result_columns()[1].expression, order.payload_expressions[0]);
  ASSERT_EQ(2U, order.output_fields.size());
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kKey, .field_index = 0}),
            order.output_fields[0]);
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kPayload, .field_index = 0}),
            order.output_fields[1]);

  const auto& limit = std::get<LogicalLimitNode>(plan.nodes()[3].payload);
  EXPECT_EQ(LogicalNodeId{2}, limit.input);
  const auto& output = std::get<LogicalOutputNode>(plan.nodes()[4].payload);
  EXPECT_EQ(LogicalNodeId{3}, output.input);
  EXPECT_EQ(LogicalNodeId{4}, plan.root());
}

TEST(LogicalPlan, OrdersSourceFreeQueriesWithoutPayloadDuplication) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const LogicalPlan plan = PlanOrThrow("SELECT abs(?) AS x, ? AS y ORDER BY x, y LIMIT ?", catalog);

  ASSERT_EQ(4U, plan.nodes().size());
  EXPECT_TRUE(std::holds_alternative<LogicalSingleRowNode>(plan.nodes()[0].payload));
  const auto& order = std::get<LogicalOrderNode>(plan.nodes()[1].payload);
  EXPECT_EQ(LogicalNodeId{0}, order.input);
  EXPECT_TRUE(order.payload_expressions.empty());
  ASSERT_EQ(2U, order.output_fields.size());
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kKey, .field_index = 0}),
            order.output_fields[0]);
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kKey, .field_index = 1}),
            order.output_fields[1]);
  EXPECT_TRUE(std::holds_alternative<LogicalLimitNode>(plan.nodes()[2].payload));
  EXPECT_TRUE(std::holds_alternative<LogicalOutputNode>(plan.nodes()[3].payload));
}

}  // namespace
}  // namespace modern_sqlite
