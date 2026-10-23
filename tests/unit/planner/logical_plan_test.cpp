#include "modern_sqlite/planner/logical_plan.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
  EXPECT_EQ("values", LogicalNodeKindName(LogicalNodeKind::kValues));
  EXPECT_EQ("distinct", LogicalNodeKindName(LogicalNodeKind::kDistinct));
  EXPECT_EQ("compound", LogicalNodeKindName(LogicalNodeKind::kCompound));
  EXPECT_EQ("advanced_order", LogicalNodeKindName(LogicalNodeKind::kAdvancedOrder));
  EXPECT_EQ("aggregate", LogicalNodeKindName(LogicalNodeKind::kAggregate));
  EXPECT_EQ("having", LogicalNodeKindName(LogicalNodeKind::kHaving));
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

TEST(LogicalPlan, MapsUnsatisfiedKeysAndPrefixPayloadsThroughOneFieldContract) {
  const BoundSelect bound =
      BindOrThrow("SELECT id,Name FROM Items ORDER BY id,Name,id", TestCatalog());
  auto fields = planner_detail::BuildSortOutputFields(bound.order_by(), 1U, bound.result_columns());
  ASSERT_TRUE(fields.has_value());
  ASSERT_EQ(2U, fields->size());
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kKey, .field_index = 1}), (*fields)[0]);
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kKey, .field_index = 0}), (*fields)[1]);
  fields = planner_detail::BuildSortOutputFields(bound.order_by(), 2U, bound.result_columns());
  ASSERT_TRUE(fields.has_value());
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kKey, .field_index = 0}), (*fields)[0]);
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kPayload, .field_index = 0}),
            (*fields)[1]);
  const auto oversized =
      planner_detail::BuildSortOutputFields(bound.order_by(), 4U, bound.result_columns());
  ASSERT_FALSE(oversized.has_value());
  EXPECT_EQ(LogicalPlanErrorCode::kInternalInvariant, oversized.error().code);
}

TEST(LogicalPlan, PublishesFlatQueryPlansThroughOneBoundStatementOwner) {
  const LogicalPlan plan = PlanOrThrow(
      "SELECT (SELECT id FROM Items WHERE id=2),EXISTS(SELECT Name FROM Items)", TestCatalog());
  ASSERT_EQ(3U, plan.bound_select().query_count());
  const LogicalQueryView root{plan};
  EXPECT_EQ(plan.nodes().data(), root.nodes().data());
  EXPECT_EQ(plan.root(), root.root());
  for (std::uint32_t index = 1; index < 3U; ++index) {
    const BoundQueryId id{index};
    const LogicalQueryView child{plan, id};
    EXPECT_TRUE(child.valid());
    EXPECT_EQ(id, plan.query_plan(id).query);
    EXPECT_EQ(&plan.bound_select(), &child.bound_select().owner());
    EXPECT_EQ(plan.bound_select().expressions().data(), child.bound_select().expressions().data());
    EXPECT_NE(plan.nodes().data(), child.nodes().data());
    ASSERT_FALSE(child.nodes().empty());
    EXPECT_LT(child.root().value(), child.nodes().size());
  }
}

TEST(LogicalPlan, BuildsAggregateQueries) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  BuildLogicalPlanResult plan =
      BuildLogicalPlan(BindOrThrow("SELECT Name,count() AS total FROM Items WHERE id>0 "
                                   "GROUP BY Name HAVING total>0 LIMIT 1 OFFSET 1",
                                   catalog));

  ASSERT_TRUE(plan.has_value()) << plan.error().detail;
  ASSERT_EQ(6U, plan->nodes().size());
  EXPECT_EQ(LogicalNodeKind::kScan, LogicalNodeKindOf(plan->nodes()[0]));
  EXPECT_EQ(LogicalNodeKind::kFilter, LogicalNodeKindOf(plan->nodes()[1]));
  EXPECT_EQ(LogicalNodeKind::kAggregate, LogicalNodeKindOf(plan->nodes()[2]));
  EXPECT_EQ(LogicalNodeKind::kHaving, LogicalNodeKindOf(plan->nodes()[3]));
  EXPECT_EQ(LogicalNodeKind::kLimit, LogicalNodeKindOf(plan->nodes()[4]));
  EXPECT_EQ(LogicalNodeKind::kProjection, LogicalNodeKindOf(plan->nodes()[5]));
  const auto& aggregate = std::get<LogicalAggregateNode>(plan->nodes()[2].payload);
  EXPECT_EQ(LogicalNodeId{1}, aggregate.input);
  EXPECT_EQ(0U, aggregate.core_index);
  const auto& having = std::get<LogicalHavingNode>(plan->nodes()[3].payload);
  EXPECT_EQ(LogicalNodeId{2}, having.input);
  EXPECT_EQ(0U, having.core_index);
  const auto& core = std::get<BoundSelectCore>(plan->bound_select().query_cores().front());
  ASSERT_TRUE(core.aggregate.has_value());
  EXPECT_EQ(core.aggregate->having, std::optional<BoundExpressionId>{having.predicate});
  EXPECT_EQ(LogicalNodeId{5}, plan->root());

  const LogicalPlan ordered =
      PlanOrThrow("SELECT count() FROM Items ORDER BY abs(?) NULLS LAST LIMIT 0", catalog);
  ASSERT_EQ(5U, ordered.nodes().size());
  EXPECT_EQ(LogicalNodeKind::kScan, LogicalNodeKindOf(ordered.nodes()[0]));
  EXPECT_EQ(LogicalNodeKind::kAggregate, LogicalNodeKindOf(ordered.nodes()[1]));
  EXPECT_EQ(LogicalNodeKind::kOrder, LogicalNodeKindOf(ordered.nodes()[2]));
  EXPECT_EQ(LogicalNodeKind::kLimit, LogicalNodeKindOf(ordered.nodes()[3]));
  EXPECT_EQ(LogicalNodeKind::kOutput, LogicalNodeKindOf(ordered.nodes()[4]));
}

TEST(LogicalPlan, BuildsAggregateDistinctAndCompoundCorePhases) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const LogicalPlan distinct = PlanOrThrow("SELECT DISTINCT count() FROM Items", catalog);
  ASSERT_EQ(4U, distinct.nodes().size());
  EXPECT_EQ(LogicalNodeKind::kScan, LogicalNodeKindOf(distinct.nodes()[0]));
  EXPECT_EQ(LogicalNodeKind::kAggregate, LogicalNodeKindOf(distinct.nodes()[1]));
  EXPECT_EQ(LogicalNodeKind::kProjection, LogicalNodeKindOf(distinct.nodes()[2]));
  EXPECT_EQ(LogicalNodeKind::kDistinct, LogicalNodeKindOf(distinct.nodes()[3]));

  const LogicalPlan compound = PlanOrThrow(
      "SELECT count() FROM Items "
      "UNION ALL SELECT count() FROM Items GROUP BY Name ORDER BY 1",
      catalog);
  ASSERT_EQ(9U, compound.nodes().size());
  EXPECT_EQ(LogicalNodeKind::kAggregate, LogicalNodeKindOf(compound.nodes()[1]));
  EXPECT_EQ(LogicalNodeKind::kProjection, LogicalNodeKindOf(compound.nodes()[2]));
  EXPECT_EQ(LogicalNodeKind::kAggregate, LogicalNodeKindOf(compound.nodes()[4]));
  EXPECT_EQ(LogicalNodeKind::kProjection, LogicalNodeKindOf(compound.nodes()[5]));
  EXPECT_EQ(LogicalNodeKind::kCompound, LogicalNodeKindOf(compound.nodes()[6]));
  EXPECT_EQ(LogicalNodeKind::kAdvancedOrder, LogicalNodeKindOf(compound.nodes()[7]));
  EXPECT_EQ(LogicalNodeKind::kOutput, LogicalNodeKindOf(compound.nodes()[8]));
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

TEST(LogicalPlan, BuildsDistinctValuesAndCompoundQueries) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  constexpr std::array<std::string_view, 3> cases{
      "SELECT DISTINCT Name FROM Items",
      "VALUES(1),(2)",
      "SELECT 1 UNION SELECT 2",
  };
  for (const std::string_view sql : cases) {
    SCOPED_TRACE(sql);
    BuildLogicalPlanResult plan = BuildLogicalPlan(BindOrThrow(sql, catalog));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
  }
}

TEST(LogicalPlan, BuildsValuesAndDistinctCoreSubgraphs) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const LogicalPlan values = PlanOrThrow("VALUES(1),(2)", catalog);
  ASSERT_EQ(1U, values.nodes().size());
  EXPECT_EQ(LogicalNodeId{0}, values.root());
  EXPECT_EQ(LogicalNodeKind::kValues, LogicalNodeKindOf(values.nodes()[0]));
  EXPECT_EQ(0U, std::get<LogicalValuesNode>(values.nodes()[0].payload).core_index);

  const LogicalPlan distinct = PlanOrThrow("SELECT DISTINCT Name FROM Items WHERE id>0", catalog);
  ASSERT_EQ(4U, distinct.nodes().size());
  EXPECT_EQ(LogicalNodeKind::kScan, LogicalNodeKindOf(distinct.nodes()[0]));
  EXPECT_EQ(0U, std::get<LogicalScanNode>(distinct.nodes()[0].payload).core_index);
  EXPECT_EQ(LogicalNodeKind::kFilter, LogicalNodeKindOf(distinct.nodes()[1]));
  EXPECT_EQ(LogicalNodeKind::kProjection, LogicalNodeKindOf(distinct.nodes()[2]));
  const auto& projection = std::get<LogicalProjectionNode>(distinct.nodes()[2].payload);
  EXPECT_EQ(0U, projection.core_index);
  EXPECT_EQ(LogicalNodeId{1}, projection.input);
  const auto& node = std::get<LogicalDistinctNode>(distinct.nodes()[3].payload);
  EXPECT_EQ(LogicalNodeId{2}, node.input);
  EXPECT_EQ(0U, node.core_index);
  ASSERT_EQ(1U, node.collations.size());
  EXPECT_EQ("NOCASE", distinct.bound_select().collations()[node.collations[0].value()].name);
  EXPECT_EQ(LogicalNodeId{3}, distinct.root());
}

TEST(LogicalPlan, PreservesLeftAssociatedCompoundsAndOuterClauses) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const LogicalPlan plan = PlanOrThrow(
      "VALUES(1) UNION ALL SELECT DISTINCT id FROM Items "
      "EXCEPT SELECT 3 ORDER BY 1 LIMIT 2 OFFSET 1",
      catalog);

  ASSERT_EQ(11U, plan.nodes().size());
  EXPECT_EQ(LogicalNodeKind::kValues, LogicalNodeKindOf(plan.nodes()[0]));
  EXPECT_EQ(LogicalNodeKind::kScan, LogicalNodeKindOf(plan.nodes()[1]));
  EXPECT_EQ(LogicalNodeKind::kProjection, LogicalNodeKindOf(plan.nodes()[2]));
  EXPECT_EQ(LogicalNodeKind::kDistinct, LogicalNodeKindOf(plan.nodes()[3]));
  EXPECT_EQ(LogicalNodeKind::kSingleRow, LogicalNodeKindOf(plan.nodes()[4]));
  EXPECT_EQ(LogicalNodeKind::kProjection, LogicalNodeKindOf(plan.nodes()[5]));

  const auto& union_all = std::get<LogicalCompoundNode>(plan.nodes()[6].payload);
  EXPECT_EQ(LogicalNodeId{0}, union_all.left);
  EXPECT_EQ(LogicalNodeId{3}, union_all.right);
  EXPECT_EQ(CompoundOperator::kUnionAll, union_all.operation);

  const auto& except = std::get<LogicalCompoundNode>(plan.nodes()[7].payload);
  EXPECT_EQ(LogicalNodeId{6}, except.left);
  EXPECT_EQ(LogicalNodeId{5}, except.right);
  EXPECT_EQ(CompoundOperator::kExcept, except.operation);
  ASSERT_EQ(1U, except.collations.size());

  const auto& order = std::get<LogicalAdvancedOrderNode>(plan.nodes()[8].payload);
  EXPECT_EQ(LogicalNodeId{7}, order.input);
  ASSERT_EQ(3U, order.core_layouts.size());
  for (const LogicalCoreOrderLayout& layout : order.core_layouts) {
    EXPECT_EQ(OrderEvaluationSchedule::kPayloadThenKeys, layout.schedule);
    ASSERT_EQ(1U, layout.key_values.size());
    EXPECT_EQ(LogicalOrderValueKind::kInputField, layout.key_values[0].kind);
  }
  EXPECT_FALSE(order.set_then_order);
  EXPECT_EQ(LogicalNodeKind::kLimit, LogicalNodeKindOf(plan.nodes()[9]));
  EXPECT_EQ(LogicalNodeKind::kOutput, LogicalNodeKindOf(plan.nodes()[10]));
  EXPECT_EQ(LogicalNodeId{10}, plan.root());
}

TEST(LogicalPlan, UsesKeyFirstOrderOnlyForPureUnionAll) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const LogicalPlan union_all = PlanOrThrow(
      "SELECT id FROM Items UNION ALL SELECT id FROM Items ORDER BY 1 LIMIT 1", catalog);
  const auto order_iterator = std::ranges::find_if(union_all.nodes(), [](const LogicalNode& node) {
    return std::holds_alternative<LogicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(union_all.nodes().end(), order_iterator);
  const auto& union_order = std::get<LogicalAdvancedOrderNode>(order_iterator->payload);
  ASSERT_EQ(2U, union_order.core_layouts.size());
  for (const LogicalCoreOrderLayout& layout : union_order.core_layouts) {
    EXPECT_EQ(OrderEvaluationSchedule::kKeysThenAdmissionThenPayload, layout.schedule);
    EXPECT_EQ(LogicalOrderValueKind::kExpression, layout.key_values[0].kind);
  }

  const LogicalPlan distinct =
      PlanOrThrow("SELECT DISTINCT id FROM Items ORDER BY 1 LIMIT 1", catalog);
  const auto distinct_order = std::ranges::find_if(distinct.nodes(), [](const LogicalNode& node) {
    return std::holds_alternative<LogicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(distinct.nodes().end(), distinct_order);
  const auto& distinct_layout =
      std::get<LogicalAdvancedOrderNode>(distinct_order->payload).core_layouts[0];
  EXPECT_EQ(OrderEvaluationSchedule::kPayloadThenKeys, distinct_layout.schedule);
  EXPECT_EQ(LogicalOrderValueKind::kInputField, distinct_layout.key_values[0].kind);

  const LogicalPlan hidden =
      PlanOrThrow("SELECT DISTINCT Name FROM Items ORDER BY id LIMIT 1", catalog);
  const auto hidden_order = std::ranges::find_if(hidden.nodes(), [](const LogicalNode& node) {
    return std::holds_alternative<LogicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(hidden.nodes().end(), hidden_order);
  const auto& hidden_layout =
      std::get<LogicalAdvancedOrderNode>(hidden_order->payload).core_layouts[0];
  EXPECT_EQ(LogicalOrderValueKind::kExpression, hidden_layout.key_values[0].kind);
  EXPECT_EQ(LogicalOrderValueKind::kInputField, hidden_layout.payload_values[0].kind);
}

TEST(LogicalPlan, PublishesArmLocalOrderSchedulesAndSetThenOrder) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const LogicalPlan mixed = PlanOrThrow(
      "SELECT DISTINCT id,abs(id) FROM Items "
      "UNION ALL SELECT id,abs(id+1) FROM Items ORDER BY 1 LIMIT 1",
      catalog);
  const auto mixed_order = std::ranges::find_if(mixed.nodes(), [](const LogicalNode& node) {
    return std::holds_alternative<LogicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(mixed.nodes().end(), mixed_order);
  const auto& layouts = std::get<LogicalAdvancedOrderNode>(mixed_order->payload).core_layouts;
  ASSERT_EQ(2U, layouts.size());
  EXPECT_EQ(OrderEvaluationSchedule::kPayloadThenKeys, layouts[0].schedule);
  EXPECT_EQ(LogicalOrderValueKind::kInputField, layouts[0].key_values[0].kind);
  EXPECT_EQ(LogicalOrderValueKind::kInputField, layouts[0].payload_values[0].kind);
  EXPECT_EQ(OrderEvaluationSchedule::kKeysThenAdmissionThenPayload, layouts[1].schedule);
  EXPECT_EQ(LogicalOrderValueKind::kExpression, layouts[1].key_values[0].kind);
  EXPECT_EQ(LogicalOrderValueKind::kExpression, layouts[1].payload_values[0].kind);

  const LogicalPlan collated = PlanOrThrow(
      "SELECT Name FROM Items UNION SELECT Name FROM Items "
      "ORDER BY 1 COLLATE binary LIMIT 1",
      catalog);
  const auto collated_order = std::ranges::find_if(collated.nodes(), [](const LogicalNode& node) {
    return std::holds_alternative<LogicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(collated.nodes().end(), collated_order);
  const auto& order = std::get<LogicalAdvancedOrderNode>(collated_order->payload);
  EXPECT_TRUE(order.set_then_order);
  EXPECT_TRUE(order.terms[0].explicit_collation);
  for (const LogicalCoreOrderLayout& layout : order.core_layouts) {
    EXPECT_EQ(OrderEvaluationSchedule::kPayloadThenKeys, layout.schedule);
    EXPECT_EQ(LogicalOrderValueKind::kInputField, layout.key_values[0].kind);
  }
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
  ASSERT_EQ(1U, bound.table_sources().size());
  EXPECT_EQ(bound.table_sources().front().kind, scan.source_kind);
  EXPECT_EQ(bound.table_sources().front().table, scan.table);

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
