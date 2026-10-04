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

TEST(LogicalPlanApi, ExposesStableKindsErrorsAndOwnership) {
  EXPECT_EQ("single_row", LogicalNodeKindName(LogicalNodeKind::kSingleRow));
  EXPECT_EQ("scan", LogicalNodeKindName(LogicalNodeKind::kScan));
  EXPECT_EQ("filter", LogicalNodeKindName(LogicalNodeKind::kFilter));
  EXPECT_EQ("limit", LogicalNodeKindName(LogicalNodeKind::kLimit));
  EXPECT_EQ("projection", LogicalNodeKindName(LogicalNodeKind::kProjection));
  EXPECT_EQ("unknown",
            LogicalNodeKindName(static_cast<LogicalNodeKind>(255)));  // NOLINT

  EXPECT_EQ("invalid_input", LogicalPlanErrorCodeName(LogicalPlanErrorCode::kInvalidInput));
  EXPECT_EQ("internal_invariant",
            LogicalPlanErrorCodeName(LogicalPlanErrorCode::kInternalInvariant));
  EXPECT_EQ("unknown",
            LogicalPlanErrorCodeName(static_cast<LogicalPlanErrorCode>(255)));  // NOLINT

  EXPECT_EQ(ErrorCode::kMisuse,
            LogicalPlanError{.code = LogicalPlanErrorCode::kInvalidInput}.base_error_code());
  EXPECT_EQ(ErrorCode::kInternal,
            LogicalPlanError{.code = LogicalPlanErrorCode::kInternalInvariant}.base_error_code());
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

}  // namespace
}  // namespace modern_sqlite
