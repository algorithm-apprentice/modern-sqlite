#include "modern_sqlite/optimizer/physical_plan.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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

[[nodiscard]] CatalogIndexTerm ColumnTerm(std::size_t column, std::string collation,
                                          SortOrder order = SortOrder::kAscending) {
  return CatalogIndexTerm{
      .target = ColumnId{column},
      .collation_name = std::move(collation),
      .order = order,
  };
}

[[nodiscard]] CatalogIndexTerm RowIdTerm() {
  return CatalogIndexTerm{
      .target = RowIdIndexTerm{},
      .collation_name = "BINARY",
      .order = SortOrder::kAscending,
  };
}

[[nodiscard]] CatalogSnapshotPtr TestCatalog(std::string item_name = "Items",
                                             std::uint64_t item_rows = 100) {
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
              .estimated_rows = item_rows,
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

[[nodiscard]] CatalogSnapshotPtr IndexedMutationCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 31, .generation = 17},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT, Value INT)"));
  input.definitions.push_back(ParseTree("CREATE INDEX items_name ON Items(Name)"));
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
                  .name = "Value",
                  .declared_type = "INT",
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
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(1),
              RowIdTerm(),
          },
  });
  CatalogSnapshotResult created = CatalogSnapshot::Create(std::move(input));
  if (!created.has_value()) {
    throw std::runtime_error{created.error().detail};
  }
  return *std::move(created);
}

[[nodiscard]] CatalogSnapshotPtr IndexedCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 29, .generation = 13},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Items("
                "id INTEGER PRIMARY KEY, "
                "Category TEXT COLLATE NOCASE, "
                "Score INT, "
                "Flag INT, "
                "Code TEXT NOT NULL, "
                "Payload BLOB"
                ")"));
  input.definitions.push_back(
      ParseTree("CREATE INDEX items_category_score ON Items(Category COLLATE NOCASE, Score DESC)"));
  input.definitions.push_back(ParseTree("CREATE INDEX items_score_desc ON Items(Score DESC)"));
  input.definitions.push_back(ParseTree("CREATE INDEX items_flag ON Items(Flag)"));
  input.definitions.push_back(ParseTree("CREATE UNIQUE INDEX items_code_unique ON Items(Code)"));
  SyntaxTree partial_definition =
      ParseTree("CREATE INDEX items_partial ON Items(Category) WHERE Flag=1");
  const auto& partial_statement = std::get<CreateIndexStatement>(partial_definition.statement());
  if (!partial_statement.where.has_value()) {
    throw std::runtime_error{"partial index test definition has no predicate"};
  }
  const ExpressionId partial_predicate = *partial_statement.where;
  input.definitions.push_back(std::move(partial_definition));
  SyntaxTree expression_definition =
      ParseTree("CREATE INDEX items_expression ON Items(lower(Category))");
  const auto& expression_statement =
      std::get<CreateIndexStatement>(expression_definition.statement());
  const ExpressionId expression_term = expression_statement.terms[0].expression;
  input.definitions.push_back(std::move(expression_definition));
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
                  .name = "Category",
                  .declared_type = "TEXT",
                  .collation_name = "NOCASE",
              },
              CatalogColumnInput{
                  .name = "Score",
                  .declared_type = "INT",
              },
              CatalogColumnInput{
                  .name = "Flag",
                  .declared_type = "INT",
              },
              CatalogColumnInput{
                  .name = "Code",
                  .declared_type = "TEXT",
                  .not_null_conflict = ConflictAction::kDefault,
              },
              CatalogColumnInput{
                  .name = "Payload",
                  .declared_type = "BLOB",
              },
          },
      .rowid_alias = ColumnId{0},
      .statistics =
          TableStatistics{
              .has_stat1 = true,
              .estimated_rows = 4096,
              .average_row_size = 512,
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{1},
      .name = "items_category_score",
      .table = TableId{0},
      .root_page = RootPageId{10},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 2,
      .terms =
          {
              ColumnTerm(1, "NOCASE"),
              ColumnTerm(2, "BINARY", SortOrder::kDescending),
              RowIdTerm(),
          },
      .statistics =
          IndexStatistics{
              .has_stat1 = true,
              .rows_per_prefix = {4096, 16, 1},
              .average_row_size = 28,
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{2},
      .name = "items_score_desc",
      .table = TableId{0},
      .root_page = RootPageId{11},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(2, "BINARY", SortOrder::kDescending),
              RowIdTerm(),
          },
      .statistics =
          IndexStatistics{
              .has_stat1 = true,
              .rows_per_prefix = {4096},
              .average_row_size = 8,
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{3},
      .name = "items_flag",
      .table = TableId{0},
      .root_page = RootPageId{12},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(3),
              RowIdTerm(),
          },
      .statistics =
          IndexStatistics{
              .has_stat1 = true,
              .rows_per_prefix = {4096, 4096},
              .average_row_size = 8,
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{4},
      .name = "items_code_unique",
      .table = TableId{0},
      .root_page = RootPageId{13},
      .origin = IndexOrigin::kCreateIndex,
      .unique = true,
      .conflict_action = ConflictAction::kDefault,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(4),
              RowIdTerm(),
          },
      .statistics =
          IndexStatistics{
              .has_stat1 = true,
              .rows_per_prefix = {4096},
              .average_row_size = 24,
              .unordered = true,
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{5},
      .name = "items_partial",
      .table = TableId{0},
      .root_page = RootPageId{14},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(1, "NOCASE"),
              RowIdTerm(),
          },
      .partial_predicate =
          SchemaExpression{
              .definition = SchemaDefinitionId{5},
              .expression = partial_predicate,
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{6},
      .name = "items_expression",
      .table = TableId{0},
      .root_page = RootPageId{15},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 1,
      .terms =
          {
              CatalogIndexTerm{
                  .target =
                      SchemaExpression{
                          .definition = SchemaDefinitionId{6},
                          .expression = expression_term,
                      },
                  .collation_name = "BINARY",
                  .order = SortOrder::kAscending,
              },
              RowIdTerm(),
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
  static const FunctionRegistry registry{functions, CoreFunctionRegistry().aggregate_functions()};
  static const std::array<const Collation*, 3> collations{
      &BinaryCollation(),
      &NoCaseCollation(),
      &RTrimCollation(),
  };
  return BindEnvironment{registry, collations, 7};
}

class ReverseCollation final : public Collation {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "REVERSE"; }

  [[nodiscard]] std::weak_ordering Compare(Utf8View left, Utf8View right) const noexcept override {
    const std::weak_ordering order = BinaryCollation().Compare(left, right);
    if (order == std::weak_ordering::less) {
      return std::weak_ordering::greater;
    }
    if (order == std::weak_ordering::greater) {
      return std::weak_ordering::less;
    }
    return std::weak_ordering::equivalent;
  }
};

[[nodiscard]] BindEnvironment CustomCollationEnvironment() {
  static const ReverseCollation reverse;
  static const std::array<const Collation*, 4> collations{
      &BinaryCollation(),
      &NoCaseCollation(),
      &RTrimCollation(),
      &reverse,
  };
  return BindEnvironment{CoreFunctionRegistry(), collations, 19};
}

[[nodiscard]] CatalogSnapshotPtr CustomCollationCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 31, .generation = 17},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Custom(id INTEGER PRIMARY KEY, Value TEXT COLLATE REVERSE)"));
  input.definitions.push_back(
      ParseTree("CREATE INDEX custom_value ON Custom(Value COLLATE REVERSE)"));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = "Custom",
      .root_page = RootPageId{2},
      .columns =
          {
              CatalogColumnInput{
                  .name = "id",
                  .declared_type = "INTEGER",
                  .primary_key = true,
              },
              CatalogColumnInput{
                  .name = "Value",
                  .declared_type = "TEXT",
                  .collation_name = "REVERSE",
              },
          },
      .rowid_alias = ColumnId{0},
      .statistics =
          TableStatistics{
              .has_stat1 = true,
              .estimated_rows = 100,
              .average_row_size = 100,
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{1},
      .name = "custom_value",
      .table = TableId{0},
      .root_page = RootPageId{3},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(1, "REVERSE"),
              RowIdTerm(),
          },
      .statistics =
          IndexStatistics{
              .has_stat1 = true,
              .rows_per_prefix = {100, 1},
              .average_row_size = 20,
          },
  });
  CatalogSnapshotResult created = CatalogSnapshot::Create(std::move(input));
  if (!created.has_value()) {
    throw std::runtime_error{created.error().detail};
  }
  return *std::move(created);
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
                                           BindEnvironment environment = BindEnvironment::Core(),
                                           OptimizerOptions options = {}) {
  BuildLogicalPlanResult logical = BuildLogicalPlan(BindOrThrow(sql, catalog, environment));
  if (!logical.has_value()) {
    throw std::runtime_error{logical.error().detail};
  }
  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical), options);
  if (!physical.has_value()) {
    throw std::runtime_error{physical.error().detail};
  }
  return std::move(*physical);
}

[[nodiscard]] const PhysicalAggregateNode& AggregateNode(const PhysicalPlan& plan) {
  const auto node = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& candidate) {
    return std::holds_alternative<PhysicalAggregateNode>(candidate.payload);
  });
  if (node == plan.nodes().end()) {
    throw std::runtime_error{"physical plan has no aggregate node"};
  }
  return std::get<PhysicalAggregateNode>(node->payload);
}

[[nodiscard]] const PhysicalSortNode& SortNode(const PhysicalPlan& plan) {
  const auto node = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& candidate) {
    return std::holds_alternative<PhysicalSortNode>(candidate.payload);
  });
  if (node == plan.nodes().end()) {
    throw std::runtime_error{"physical plan has no sort node"};
  }
  return std::get<PhysicalSortNode>(node->payload);
}

[[nodiscard]] PhysicalStatementPlan OptimizeStatementOrThrow(
    std::string_view sql, const CatalogSnapshotPtr& catalog,
    BindEnvironment environment = BindEnvironment::Core(), OptimizerOptions options = {}) {
  BindStatementResult bound = BindStatement(ParseTree(sql), catalog, environment);
  if (!bound.has_value()) {
    throw std::runtime_error{bound.error().detail};
  }
  BuildLogicalStatementPlanResult logical = BuildLogicalStatementPlan(std::move(*bound));
  if (!logical.has_value()) {
    throw std::runtime_error{logical.error().detail};
  }
  OptimizeLogicalStatementPlanResult physical =
      OptimizeLogicalStatementPlan(std::move(*logical), options);
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
  EXPECT_EQ("index_scan", PhysicalAccessKindName(PhysicalAccessKind::kIndexScan));
  EXPECT_EQ("unknown",
            PhysicalAccessKindName(static_cast<PhysicalAccessKind>(255)));  // NOLINT
  EXPECT_EQ("empty", MutationAccessKindName(MutationAccessKind::kEmpty));
  EXPECT_EQ("rowid_lookup", MutationAccessKindName(MutationAccessKind::kRowIdLookup));
  EXPECT_EQ("unknown",
            MutationAccessKindName(static_cast<MutationAccessKind>(255)));  // NOLINT
  EXPECT_EQ("statement", MutationAtomicityName(MutationAtomicity::kStatement));
  EXPECT_EQ("unknown",
            MutationAtomicityName(static_cast<MutationAtomicity>(255)));  // NOLINT

  EXPECT_EQ("index_scan", PhysicalNodeKindName(PhysicalNodeKind::kIndexScan));
  EXPECT_EQ("guard", PhysicalNodeKindName(PhysicalNodeKind::kGuard));
  EXPECT_EQ("projection", PhysicalNodeKindName(PhysicalNodeKind::kProjection));
  EXPECT_EQ("sort", PhysicalNodeKindName(PhysicalNodeKind::kSort));
  EXPECT_EQ("output", PhysicalNodeKindName(PhysicalNodeKind::kOutput));
  EXPECT_EQ("values", PhysicalNodeKindName(PhysicalNodeKind::kValues));
  EXPECT_EQ("distinct", PhysicalNodeKindName(PhysicalNodeKind::kDistinct));
  EXPECT_EQ("compound", PhysicalNodeKindName(PhysicalNodeKind::kCompound));
  EXPECT_EQ("advanced_order", PhysicalNodeKindName(PhysicalNodeKind::kAdvancedOrder));
  EXPECT_EQ("aggregate", PhysicalNodeKindName(PhysicalNodeKind::kAggregate));
  EXPECT_EQ("having", PhysicalNodeKindName(PhysicalNodeKind::kHaving));
  EXPECT_EQ("unknown",
            PhysicalNodeKindName(static_cast<PhysicalNodeKind>(255)));  // NOLINT

  EXPECT_EQ("invalid_input", OptimizerErrorCodeName(OptimizerErrorCode::kInvalidInput));
  EXPECT_EQ("unsupported_feature", OptimizerErrorCodeName(OptimizerErrorCode::kUnsupportedFeature));
  EXPECT_EQ("internal_invariant", OptimizerErrorCodeName(OptimizerErrorCode::kInternalInvariant));
  EXPECT_EQ("unknown",
            OptimizerErrorCodeName(static_cast<OptimizerErrorCode>(255)));  // NOLINT
  EXPECT_EQ(ErrorCode::kMisuse,
            OptimizerError{.code = OptimizerErrorCode::kInvalidInput}.base_error_code());
  EXPECT_EQ(ErrorCode::kGeneric,
            OptimizerError{.code = OptimizerErrorCode::kUnsupportedFeature}.base_error_code());
  EXPECT_EQ(ErrorCode::kInternal,
            OptimizerError{.code = OptimizerErrorCode::kInternalInvariant}.base_error_code());
}

TEST(PhysicalPlan, PlansAndExplainsLexicallyOwnedScalarAndExistsQueries) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT (SELECT id FROM Items WHERE id=2),EXISTS(SELECT Name FROM Items)", TestCatalog());
  const PhysicalQueryView scalar{plan, BoundQueryId{1}};
  const PhysicalQueryView exists{plan, BoundQueryId{2}};
  ASSERT_TRUE(scalar.valid());
  ASSERT_TRUE(exists.valid());
  EXPECT_EQ(BoundQueryId{1}, plan.query_plan(BoundQueryId{1}).query);
  EXPECT_EQ(BoundQueryId{2}, plan.query_plan(BoundQueryId{2}).query);
  EXPECT_EQ(plan.logical_plan().bound_select().expressions().data(),
            scalar.logical_plan().bound_select().expressions().data());
  EXPECT_TRUE(std::ranges::any_of(scalar.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalRowIdLookupNode>(node.payload);
  }));
  EXPECT_TRUE(std::ranges::any_of(exists.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalTableScanNode>(node.payload) ||
           std::holds_alternative<PhysicalIndexScanNode>(node.payload);
  }));
  const std::string explanation = ExplainPhysicalPlan(plan);
  EXPECT_NE(std::string::npos, explanation.find("SCALAR SUBQUERY 1"));
  EXPECT_NE(std::string::npos, explanation.find("EXISTS SUBQUERY 2"));
  EXPECT_NE(std::string::npos, explanation.find("USING INTEGER PRIMARY KEY"));
}

TEST(PhysicalPlan, AdmitsAncestorProbeKeysAndGuardsButNotTheProducersOwnRow) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT (SELECT i.Name FROM Items i WHERE o.Value>0 AND i.id=o.id) FROM Items o",
      TestCatalog());
  const PhysicalQueryView child{plan, BoundQueryId{1}};
  EXPECT_TRUE(std::ranges::any_of(child.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalRowIdLookupNode>(node.payload);
  }));
  const auto guard = std::ranges::find_if(child.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalGuardNode>(node.payload);
  });
  ASSERT_NE(child.nodes().end(), guard);
  EXPECT_EQ(1U, std::get<PhysicalGuardNode>(guard->payload).predicates.size());

  for (const std::string_view sql :
       {"SELECT o.id FROM Items o WHERE o.id=(SELECT o.id)",
        "SELECT o.id FROM Items o WHERE o.id=(SELECT i.id FROM Items i WHERE i.id=o.id)"}) {
    SCOPED_TRACE(sql);
    const PhysicalPlan self = OptimizeOrThrow(sql, TestCatalog());
    ASSERT_NE(nullptr, self.selected_candidate());
    EXPECT_EQ(PhysicalAccessKind::kTableScan, self.selected_candidate()->kind);
  }
}

TEST(PhysicalPlan, PlacesTransitiveAndInactiveCapturePredicatesAfterTheirSources) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT a.id,b.id FROM Items a CROSS JOIN Items b "
      "WHERE (SELECT (SELECT b.Value))>a.Value AND EXISTS(SELECT b.Name)",
      TestCatalog());
  const auto filter = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalFilterNode>(node.payload);
  });
  ASSERT_NE(plan.nodes().end(), filter);
  const auto& predicates = std::get<PhysicalFilterNode>(filter->payload);
  EXPECT_TRUE(predicates.guards.empty());
  EXPECT_EQ((std::vector{BoundSourceId{1}, BoundSourceId{1}}), predicates.predicate_sources);
}

TEST(PhysicalPlan, DoesNotTreatAliasContextAsAnActualSourceDependency) {
  const auto catalog = TestCatalog();
  const PhysicalPlan point =
      OptimizeOrThrow("SELECT 1 AS x FROM Items WHERE id=(SELECT x)", catalog);
  EXPECT_TRUE(std::ranges::any_of(point.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalRowIdLookupNode>(node.payload);
  }));
  struct Case {
    std::string_view definition;
    BoundSourceId ready;
  };
  for (const Case& test_case : {Case{"1", BoundSourceId{0}}, Case{"b.Value", BoundSourceId{1}}}) {
    SCOPED_TRACE(test_case.definition);
    const PhysicalPlan plan =
        OptimizeOrThrow("SELECT " + std::string{test_case.definition} +
                            " AS x FROM Items a CROSS JOIN Items b WHERE (SELECT x)>0",
                        catalog);
    const auto filter = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
      return std::holds_alternative<PhysicalFilterNode>(node.payload);
    });
    ASSERT_NE(plan.nodes().end(), filter);
    const auto& predicates = std::get<PhysicalFilterNode>(filter->payload);
    EXPECT_TRUE(predicates.guards.empty());
    EXPECT_EQ(std::vector{test_case.ready}, predicates.predicate_sources);
  }
}

TEST(PhysicalPlan, DemandsOnlyExecutableAliasFieldsFromPrivateStoredIndexes) {
  const auto catalog = IndexedCatalog();
  for (const bool alias : {false, true}) {
    SCOPED_TRACE(alias);
    for (const bool live : {false, true}) {
      SCOPED_TRACE(live);
      const std::string value = alias ? "x" : "o.Payload";
      const std::string predicate =
          live ? "(SELECT length(" + value + "))>0" : "EXISTS(SELECT " + value + ")";
      const PhysicalPlan plan = OptimizeOrThrow(
          "SELECT EXISTS(SELECT o.Payload AS x FROM Items o "
          "WHERE o.Code=?1 AND " +
              predicate + ")",
          catalog);
      const PhysicalQueryView owner{plan, BoundQueryId{1}};
      const auto index = std::ranges::find_if(owner.nodes(), [](const PhysicalNode& node) {
        return std::holds_alternative<PhysicalIndexScanNode>(node.payload);
      });
      ASSERT_NE(owner.nodes().end(), index);
      EXPECT_EQ(!live, std::get<PhysicalIndexScanNode>(index->payload).covering);
    }
  }
}

TEST(PhysicalPlan, RetainsOnlyExecutableCapturedFieldsInStoredIndexCoverage) {
  struct Case {
    std::string_view sql;
    bool parent_covering;
    bool indexed_child = false;
  };
  for (const Case& test_case :
       {Case{.sql = "SELECT (SELECT o.Payload) FROM Items o WHERE o.Code=?1",
             .parent_covering = false},
        Case{.sql = "SELECT EXISTS(SELECT o.Payload) FROM Items o WHERE o.Code=?1",
             .parent_covering = true},
        Case{.sql = "SELECT (SELECT i.Payload FROM Items i WHERE i.Code=o.Code) "
                    "FROM Items o WHERE o.Code=?1",
             .parent_covering = true,
             .indexed_child = true}}) {
    SCOPED_TRACE(test_case.sql);
    const PhysicalPlan plan = OptimizeOrThrow(test_case.sql, IndexedCatalog());
    const auto index = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
      return std::holds_alternative<PhysicalIndexScanNode>(node.payload);
    });
    ASSERT_NE(plan.nodes().end(), index);
    EXPECT_EQ(test_case.parent_covering, std::get<PhysicalIndexScanNode>(index->payload).covering);
    if (test_case.indexed_child) {
      const PhysicalQueryView child{plan, BoundQueryId{1}};
      const auto child_index = std::ranges::find_if(child.nodes(), [](const PhysicalNode& node) {
        return std::holds_alternative<PhysicalIndexScanNode>(node.payload);
      });
      ASSERT_NE(child.nodes().end(), child_index);
      const auto& access = std::get<PhysicalIndexScanNode>(child_index->payload);
      EXPECT_FALSE(access.covering);
      ASSERT_EQ(1U, access.equalities.size());
      EXPECT_EQ(ColumnId{4}, access.equalities.front().column);
    }
  }
}

TEST(PhysicalPlan, IncludesExecutableCapturesInAutomaticCoveringPayloads) {
  for (const bool inactive : {false, true}) {
    SCOPED_TRACE(inactive);
    const std::string sql =
        std::string{inactive ? "SELECT EXISTS(SELECT b.Value)" : "SELECT (SELECT b.Value)"} +
        " FROM Items a JOIN Items b ON b.Name=a.Name";
    const PhysicalPlan plan =
        OptimizeOrThrow(sql, TestCatalog(), BindEnvironment::Core(),
                        OptimizerOptions{.allow_file_backed_automatic_indexes = true});
    const auto automatic = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
      return std::holds_alternative<PhysicalAutomaticIndexNode>(node.payload);
    });
    ASSERT_NE(plan.nodes().end(), automatic);
    EXPECT_EQ(inactive ? std::vector<ColumnId>{} : std::vector{ColumnId{2}},
              std::get<PhysicalAutomaticIndexNode>(automatic->payload).covering_columns);
  }
}

TEST(PhysicalPlan, RetainsAggregateSnapshotsButSkipsInactiveGlobalInputsInIndexCoverage) {
  struct Case {
    std::string_view sql;
    bool covering;
  };
  const CatalogSnapshotPtr catalog = IndexedCatalog();
  for (const Case& test_case :
       {Case{.sql = "SELECT count(*) FROM Items o WHERE o.Code=?1 ORDER BY o.Payload",
             .covering = false},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Code=?1 "
                    "ORDER BY (SELECT o.Payload)",
             .covering = false},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Code=?1 "
                    "ORDER BY EXISTS(SELECT o.Payload)",
             .covering = false},
        Case{.sql = "SELECT count(*) FROM Items o CROSS JOIN Items i "
                    "WHERE o.Code=?1 AND i.Code=?1 ORDER BY (SELECT o.Payload)",
             .covering = false},
        Case{.sql = "SELECT sum(EXISTS(SELECT o.Payload)) FROM Items o WHERE o.Code=?1",
             .covering = true},
        Case{.sql = "SELECT count(*) FILTER(WHERE EXISTS(SELECT o.Payload)) "
                    "FROM Items o WHERE o.Code=?1",
             .covering = true},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Code=?1 "
                    "ORDER BY sum(EXISTS(SELECT o.Payload))",
             .covering = true},
        Case{.sql = "SELECT (SELECT count(*) FROM Items i WHERE i.Code=?1 "
                    "ORDER BY o.Payload) FROM Items o WHERE o.Code=?1",
             .covering = true},
        Case{.sql = "SELECT count(*),EXISTS(SELECT o.Payload) FROM Items o WHERE o.Code=?1",
             .covering = false},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Code=?1 "
                    "HAVING EXISTS(SELECT o.Payload)",
             .covering = false},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Code=?1 ORDER BY sum(o.Payload)",
             .covering = false},
        Case{.sql = "SELECT sum(EXISTS(SELECT o.Payload)) FROM Items o WHERE o.Code=?1 "
                    "GROUP BY o.Code",
             .covering = false},
        Case{.sql = "SELECT (SELECT count(*) FROM Items i WHERE i.Code=?1 "
                    "GROUP BY i.Code ORDER BY o.Payload) FROM Items o WHERE o.Code=?1",
             .covering = false},
        Case{.sql = "SELECT count(*) FROM Items o WHERE o.Code=?1 "
                    "GROUP BY o.Code ORDER BY (SELECT o.Payload)",
             .covering = false}}) {
    SCOPED_TRACE(test_case.sql);
    const PhysicalPlan plan = OptimizeOrThrow(test_case.sql, catalog);
    const auto index = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
      const auto* access = std::get_if<PhysicalIndexScanNode>(&node.payload);
      return access != nullptr && access->source == BoundSourceId{0};
    });
    ASSERT_NE(plan.nodes().end(), index);
    EXPECT_EQ(test_case.covering, std::get<PhysicalIndexScanNode>(index->payload).covering);
  }
}

TEST(PhysicalPlan, PlansGlobalAggregate) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  BuildLogicalPlanResult logical =
      BuildLogicalPlan(BindOrThrow("SELECT count() FROM Items", catalog));
  ASSERT_TRUE(logical.has_value()) << logical.error().detail;

  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical));
  ASSERT_TRUE(physical.has_value()) << physical.error().detail;
  ASSERT_EQ(3U, physical->nodes().size());
  EXPECT_EQ(PhysicalNodeKind::kTableScan, PhysicalNodeKindOf(physical->nodes()[0]));
  EXPECT_EQ(PhysicalNodeKind::kAggregate, PhysicalNodeKindOf(physical->nodes()[1]));
  EXPECT_EQ(PhysicalNodeKind::kProjection, PhysicalNodeKindOf(physical->nodes()[2]));
  const auto& aggregate = std::get<PhysicalAggregateNode>(physical->nodes()[1].payload);
  EXPECT_EQ(PhysicalNodeId{0}, aggregate.input);
  EXPECT_EQ(0U, aggregate.core_index);
  EXPECT_EQ(PhysicalAggregateStrategy::kGlobal, aggregate.strategy);
  EXPECT_FALSE(aggregate.min_max.has_value());
  EXPECT_TRUE(aggregate.grouping_order.empty());
  EXPECT_TRUE(aggregate.at_most_one_row);
  ASSERT_EQ(1U, physical->candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kTableScan, physical->selected_candidate()->kind);
  EXPECT_EQ(
      "CORE 0 SCAN \"Items\"\n"
      "AGGREGATE CORE 0 GLOBAL",
      ExplainPhysicalPlan(*physical));

  const PhysicalPlan having =
      OptimizeOrThrow("SELECT count() FROM Items HAVING count()>0", catalog);
  const auto having_node = std::ranges::find_if(having.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalHavingNode>(node.payload);
  });
  ASSERT_NE(having.nodes().end(), having_node);
  EXPECT_EQ(0U, std::get<PhysicalHavingNode>(having_node->payload).core_index);
}

TEST(PhysicalPlan, PlansStableSortedGrouping) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  BuildLogicalPlanResult logical = BuildLogicalPlan(
      BindOrThrow("SELECT Name || '',count() FROM Items GROUP BY Name || ''", catalog));
  ASSERT_TRUE(logical.has_value()) << logical.error().detail;

  OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical));
  ASSERT_TRUE(physical.has_value()) << physical.error().detail;
  ASSERT_EQ(3U, physical->nodes().size());
  const auto& aggregate = std::get<PhysicalAggregateNode>(physical->nodes()[1].payload);
  EXPECT_EQ(PhysicalAggregateStrategy::kSortedGroup, aggregate.strategy);
  EXPECT_FALSE(aggregate.min_max.has_value());
  ASSERT_EQ(1U, aggregate.grouping_order.size());
  EXPECT_EQ(0U, aggregate.grouping_order[0].group_term_index);
  EXPECT_EQ(SortOrder::kAscending, aggregate.grouping_order[0].order);
  EXPECT_EQ(BoundNullPlacement::kFirst, aggregate.grouping_order[0].null_placement);
  EXPECT_FALSE(aggregate.at_most_one_row);
  EXPECT_EQ(
      "CORE 0 SCAN \"Items\"\n"
      "AGGREGATE CORE 0 GROUPS USING TEMP SORTER",
      ExplainPhysicalPlan(*physical));
}

TEST(PhysicalPlan, PreservesEmptyAndSingleRowAggregateInputs) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const PhysicalPlan empty_global = OptimizeOrThrow("SELECT count() FROM Items WHERE 0", catalog);
  EXPECT_TRUE(std::holds_alternative<PhysicalEmptyNode>(empty_global.nodes().front().payload));
  EXPECT_EQ(PhysicalAggregateStrategy::kGlobal, AggregateNode(empty_global).strategy);
  EXPECT_TRUE(AggregateNode(empty_global).at_most_one_row);

  const PhysicalPlan empty_group =
      OptimizeOrThrow("SELECT Name,count() FROM Items WHERE 0 GROUP BY Name", catalog);
  EXPECT_TRUE(std::holds_alternative<PhysicalEmptyNode>(empty_group.nodes().front().payload));
  EXPECT_EQ(PhysicalAggregateStrategy::kOrderedGroup, AggregateNode(empty_group).strategy);
  EXPECT_TRUE(AggregateNode(empty_group).at_most_one_row);

  const PhysicalPlan single = OptimizeOrThrow("SELECT 1,count() GROUP BY 1", catalog);
  EXPECT_TRUE(std::holds_alternative<PhysicalSingleRowNode>(single.nodes().front().payload));
  EXPECT_EQ(PhysicalAggregateStrategy::kOrderedGroup, AggregateNode(single).strategy);
  EXPECT_TRUE(AggregateNode(single).at_most_one_row);

  const PhysicalPlan ordered_single =
      OptimizeOrThrow("SELECT abs(?1),count() GROUP BY abs(?1) ORDER BY abs(?1) DESC", catalog);
  EXPECT_TRUE(AggregateNode(ordered_single).at_most_one_row);
  EXPECT_TRUE(SortNode(ordered_single).input_order_satisfied);
}

TEST(PhysicalPlan, PlansAggregateDistinctAndCompoundCores) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan compound = OptimizeOrThrow(
      "SELECT count() FROM Items "
      "UNION ALL SELECT count() FROM Items GROUP BY Name ORDER BY 1",
      catalog);
  std::vector<PhysicalAggregateStrategy> strategies;
  for (const PhysicalNode& node : compound.nodes()) {
    if (const auto* aggregate = std::get_if<PhysicalAggregateNode>(&node.payload);
        aggregate != nullptr) {
      strategies.push_back(aggregate->strategy);
    }
  }
  EXPECT_EQ((std::vector{
                PhysicalAggregateStrategy::kGlobal,
                PhysicalAggregateStrategy::kSortedGroup,
            }),
            strategies);

  const PhysicalPlan distinct =
      OptimizeOrThrow("SELECT DISTINCT count() FROM Items ORDER BY 1", catalog);
  const auto order = std::ranges::find_if(distinct.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(distinct.nodes().end(), order);
  EXPECT_TRUE(std::get<PhysicalAdvancedOrderNode>(order->payload).input_order_satisfied);

  const CatalogSnapshotPtr indexed = IndexedCatalog();
  const PhysicalPlan constrained = OptimizeOrThrow(
      "SELECT count() "
      "UNION ALL SELECT count() FROM Items WHERE Category=?1 GROUP BY Score",
      indexed);
  const auto index_scan = std::ranges::find_if(constrained.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalIndexScanNode>(node.payload);
  });
  ASSERT_NE(constrained.nodes().end(), index_scan);
  EXPECT_EQ(1U, std::get<PhysicalIndexScanNode>(index_scan->payload).equalities.size());
  const auto second_aggregate =
      std::ranges::find_if(constrained.nodes(), [](const PhysicalNode& node) {
        const auto* aggregate = std::get_if<PhysicalAggregateNode>(&node.payload);
        return aggregate != nullptr && aggregate->core_index == 1U;
      });
  ASSERT_NE(constrained.nodes().end(), second_aggregate);
  EXPECT_EQ(PhysicalAggregateStrategy::kOrderedGroup,
            std::get<PhysicalAggregateNode>(second_aggregate->payload).strategy);
}

TEST(PhysicalPlan, PreservesOccurrenceIdentityAcrossAllAccessKinds) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "VALUES(0) UNION ALL SELECT id FROM Items "
      "UNION ALL SELECT count() FROM Items WHERE rowid=?1 "
      "UNION ALL SELECT min(Score) FROM Items",
      IndexedCatalog());
  const auto cores = plan.logical_plan().bound_select().query_cores();
  std::size_t logical_sources = 0;
  for (const LogicalNode& node : plan.logical_plan().nodes()) {
    if (const auto* scan = std::get_if<LogicalScanNode>(&node.payload); scan != nullptr) {
      const auto& core = std::get<BoundSelectCore>(cores[scan->core_index]);
      ASSERT_EQ(1U, core.table_sources.size());
      EXPECT_EQ(core.table_sources.front().id, scan->source);
      EXPECT_EQ(BoundSourceId{static_cast<std::uint32_t>(logical_sources++)}, scan->source);
    }
  }
  EXPECT_EQ(3U, logical_sources);
  std::size_t scans = 0;
  std::size_t lookups = 0;
  std::size_t indexes = 0;
  for (const PhysicalNode& node : plan.nodes()) {
    std::visit(
        [&](const auto& access) {
          using Access = std::decay_t<decltype(access)>;
          if constexpr (std::is_same_v<Access, PhysicalTableScanNode> ||
                        std::is_same_v<Access, PhysicalRowIdLookupNode> ||
                        std::is_same_v<Access, PhysicalIndexScanNode>) {
            const auto& core = std::get<BoundSelectCore>(cores[access.core_index]);
            ASSERT_EQ(1U, core.table_sources.size());
            EXPECT_EQ(core.table_sources.front().id, access.source);
            EXPECT_EQ(BoundSourceId{static_cast<std::uint32_t>(access.core_index - 1U)},
                      access.source);
            if constexpr (std::is_same_v<Access, PhysicalTableScanNode>) {
              ++scans;
            } else if constexpr (std::is_same_v<Access, PhysicalRowIdLookupNode>) {
              ++lookups;
            } else {
              ++indexes;
            }
          }
        },
        node.payload);
  }
  EXPECT_EQ(1U, scans);
  EXPECT_EQ(1U, lookups);
  EXPECT_EQ(1U, indexes);
}

TEST(PhysicalPlan, PublishesDeclaredJoinOrderAndEarliestReadyPredicateOwners) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT a.id,b.id FROM Items a CROSS JOIN Items b "
      "WHERE a.id>1 AND b.id>2 AND a.id+b.id>3",
      TestCatalog());
  std::size_t scans = 0;
  std::size_t joins = 0;
  const PhysicalFilterNode* filter = nullptr;
  for (const PhysicalNode& node : plan.nodes()) {
    if (const auto* scan = std::get_if<PhysicalTableScanNode>(&node.payload); scan != nullptr) {
      EXPECT_EQ(BoundSourceId{static_cast<std::uint32_t>(scans++)}, scan->source);
    } else if (const auto* join = std::get_if<PhysicalNestedLoopJoinNode>(&node.payload);
               join != nullptr) {
      EXPECT_EQ(SourceJoinKind::kCross, join->join);
      EXPECT_EQ(PhysicalNodeId{0}, join->left);
      EXPECT_EQ(PhysicalNodeId{1}, join->right);
      ++joins;
    } else if (const auto* predicates = std::get_if<PhysicalFilterNode>(&node.payload);
               predicates != nullptr) {
      filter = predicates;
    }
  }
  EXPECT_EQ(2U, scans);
  EXPECT_EQ(1U, joins);
  ASSERT_NE(nullptr, filter);
  EXPECT_EQ((std::vector{BoundSourceId{0}, BoundSourceId{1}, BoundSourceId{1}}),
            filter->predicate_sources);
  EXPECT_EQ(filter->predicates.size(), filter->predicate_sources.size());
  EXPECT_EQ("nested_loop_join", PhysicalNodeKindName(PhysicalNodeKind::kNestedLoopJoin));
  EXPECT_EQ("join", LogicalNodeKindName(LogicalNodeKind::kJoin));
  EXPECT_EQ(
      "CORE 0 SCAN \"Items\" SOURCE 0\nCORE 0 SCAN \"Items\" SOURCE 1\n"
      "CORE 0 NESTED LOOP CROSS JOIN",
      ExplainPhysicalPlan(plan));
}

TEST(PhysicalPlan, PlansJoinedAggregationWithoutClaimingFirstSourceAccessOrOrder) {
  const PhysicalPlan global = OptimizeOrThrow(
      "SELECT min(a.id),count() FROM Items a CROSS JOIN Items b ORDER BY 1", TestCatalog());
  EXPECT_EQ(PhysicalAggregateStrategy::kGlobal, AggregateNode(global).strategy);
  EXPECT_TRUE(AggregateNode(global).at_most_one_row);
  EXPECT_FALSE(AggregateNode(global).min_max.has_value());
  EXPECT_TRUE(global.candidates().empty());
  const PhysicalPlan grouped = OptimizeOrThrow(
      "SELECT b.id,count() FROM Items a CROSS JOIN Items b GROUP BY b.id ORDER BY b.id",
      TestCatalog());
  EXPECT_EQ(PhysicalAggregateStrategy::kSortedGroup, AggregateNode(grouped).strategy);
  EXPECT_FALSE(AggregateNode(grouped).at_most_one_row);
  EXPECT_TRUE(grouped.candidates().empty());
}

TEST(PhysicalPlan, SelectsSourceOwnedRowidSeeksForAlreadyPositionedJoinPrefixes) {
  for (const std::string_view operation : {"JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL JOIN"}) {
    SCOPED_TRACE(operation);
    const PhysicalPlan plan = OptimizeOrThrow(
        "SELECT a.id,b.id FROM Items a " + std::string{operation} + " Items b ON b.id=a.id",
        TestCatalog());
    ASSERT_GE(plan.nodes().size(), 2U);
    EXPECT_TRUE(std::holds_alternative<PhysicalTableScanNode>(plan.nodes()[0].payload));
    const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&plan.nodes()[1].payload);
    ASSERT_NE(nullptr, lookup);
    EXPECT_EQ(BoundSourceId{1}, lookup->source);
    EXPECT_EQ(TableId{0}, lookup->table);
    const BoundSelect& bound = plan.logical_plan().bound_select();
    const auto& key = std::get<BoundColumnExpression>(bound.expression(lookup->key).payload);
    EXPECT_EQ(BoundSourceId{0}, bound.source_columns()[key.column.value()].source);
  }
}

TEST(PhysicalPlan, SelectsAutomaticCoveringEqualityWithCompleteSourceMetadata) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT b.Name,b.Value,b.id FROM Items a JOIN Items b ON b.Name=a.Name", TestCatalog(),
      BindEnvironment::Core(), OptimizerOptions{.allow_file_backed_automatic_indexes = true});
  ASSERT_GE(plan.nodes().size(), 2U);
  EXPECT_TRUE(std::holds_alternative<PhysicalTableScanNode>(plan.nodes()[0].payload));
  const auto* automatic = std::get_if<PhysicalAutomaticIndexNode>(&plan.nodes()[1].payload);
  ASSERT_NE(nullptr, automatic);
  EXPECT_EQ(0U, automatic->core_index);
  EXPECT_EQ(BoundSourceId{1}, automatic->source);
  EXPECT_EQ(TableId{0}, automatic->table);
  EXPECT_EQ(RootPageId{2}, automatic->table_root_page);
  EXPECT_EQ(PhysicalScanDirection::kForward, automatic->direction);
  ASSERT_EQ(1U, automatic->equalities.size());
  const PhysicalIndexEquality& equality = automatic->equalities.front();
  EXPECT_EQ(ColumnId{1}, equality.column);
  EXPECT_EQ(TypeAffinity::kText, equality.affinity);
  EXPECT_EQ("NOCASE", equality.collation_name);
  EXPECT_EQ(SortOrder::kAscending, equality.order);
  EXPECT_TRUE(equality.reject_null);
  EXPECT_EQ((std::vector{ColumnId{2}}), automatic->covering_columns);
  EXPECT_EQ("automatic_index", PhysicalNodeKindName(PhysicalNodeKindOf(plan.nodes()[1])));
  EXPECT_EQ("automatic_index", PhysicalAccessKindName(PhysicalAccessKind::kAutomaticIndex));
  EXPECT_NE(std::string::npos, ExplainPhysicalPlan(plan).find("USING AUTOMATIC COVERING INDEX"));
}

TEST(PhysicalPlan, ThreadsAutomaticBackendPermissionThroughEveryQueryAndStatementOwner) {
  for (const bool enabled : {false, true}) {
    const OptimizerOptions options{.allow_file_backed_automatic_indexes = enabled};
    for (const std::string_view sql :
         {"SELECT b.Value FROM Items a JOIN Items b ON b.Name=a.Name",
          "SELECT (SELECT count(b.Value) FROM Items a JOIN Items b ON b.Name=a.Name)"}) {
      SCOPED_TRACE(sql);
      SCOPED_TRACE(enabled);
      const PhysicalPlan plan =
          OptimizeOrThrow(sql, TestCatalog(), BindEnvironment::Core(), options);
      EXPECT_EQ(options, plan.options());
      std::size_t automatic_sources = 0;
      for (std::size_t query = 0; query < plan.logical_plan().bound_select().query_count();
           ++query) {
        const PhysicalQueryView view{plan, BoundQueryId{static_cast<std::uint32_t>(query)}};
        EXPECT_EQ(options, view.options());
        automatic_sources += static_cast<std::size_t>(
            std::ranges::count_if(view.nodes(), [](const PhysicalNode& node) {
              return std::holds_alternative<PhysicalAutomaticIndexNode>(node.payload);
            }));
      }
      EXPECT_EQ(enabled ? 1U : 0U, automatic_sources);
      const PhysicalStatementPlan statement =
          OptimizeStatementOrThrow(sql, TestCatalog(), BindEnvironment::Core(), options);
      EXPECT_EQ(options, std::get<PhysicalPlan>(statement).options());
    }
  }
}

TEST(PhysicalPlan, DoesNotAssumeTemporaryStoragePermissionForDefaultOptimization) {
  constexpr std::string_view sql = "SELECT b.Value FROM Items a JOIN Items b ON b.Name=a.Name";
  const PhysicalPlan plan = OptimizeOrThrow(sql, TestCatalog());
  EXPECT_FALSE(plan.options().allow_file_backed_automatic_indexes);
  EXPECT_FALSE(std::ranges::any_of(plan.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalAutomaticIndexNode>(node.payload);
  }));
  const PhysicalStatementPlan statement = OptimizeStatementOrThrow(sql, TestCatalog());
  EXPECT_FALSE(std::get<PhysicalPlan>(statement).options().allow_file_backed_automatic_indexes);
}

TEST(PhysicalPlan, KeepsAutomaticSourcesBehindCostReadinessAndPreservationBarriers) {
  for (const std::string_view sql :
       {"SELECT b.Value FROM Items a JOIN Items b ON b.Name=a.Name WHERE a.id=1",
        "SELECT b.Value FROM Tiny a JOIN Items b ON b.Name='x'",
        "SELECT b.Value FROM Items a JOIN Items b ON b.Name>a.Name",
        "SELECT b.Value FROM Items a JOIN Items b ON b.Name=upper(a.Name)",
        "SELECT b.Value FROM Items a JOIN Items b ON +b.Name=a.Name",
        "SELECT b.Value FROM Items a JOIN Items b ON b.Name=b.Value",
        "SELECT b.Value FROM Items a LEFT JOIN Items b ON 1 WHERE b.Name=a.Name",
        "SELECT b.Value FROM Items a RIGHT JOIN Items b ON b.Name=a.Name",
        "SELECT b.Value FROM Items a FULL JOIN Items b ON b.Name=a.Name",
        "SELECT b.Value FROM Items a JOIN Items b ON b.Name=c.Name CROSS JOIN Items c",
        "SELECT c.Value FROM Items a JOIN Items b ON b.id=a.id "
        "JOIN Items c ON c.Name=b.Name WHERE a.id=1"}) {
    SCOPED_TRACE(sql);
    const PhysicalPlan plan =
        OptimizeOrThrow(sql, TestCatalog(), BindEnvironment::Core(),
                        OptimizerOptions{.allow_file_backed_automatic_indexes = true});
    EXPECT_FALSE(std::ranges::any_of(plan.nodes(), [](const PhysicalNode& node) {
      return std::holds_alternative<PhysicalAutomaticIndexNode>(node.payload);
    }));
  }
  for (const std::uint64_t rows : {std::uint64_t{10}, std::numeric_limits<std::uint64_t>::max()}) {
    SCOPED_TRACE(rows);
    const PhysicalPlan plan = OptimizeOrThrow(
        "SELECT b.Value FROM Items a JOIN Items b ON b.Name=a.Name", TestCatalog("Items", rows),
        BindEnvironment::Core(), OptimizerOptions{.allow_file_backed_automatic_indexes = true});
    EXPECT_FALSE(std::ranges::any_of(plan.nodes(), [](const PhysicalNode& node) {
      return std::holds_alternative<PhysicalAutomaticIndexNode>(node.payload);
    }));
  }
}

TEST(PhysicalPlan, DiscoversAutomaticCoveringValuesInTheirActualCompoundCore) {
  struct Case {
    std::string_view sql;
    std::vector<ColumnId> covering;
  };
  for (const Case& test_case : {
           Case{.sql = "SELECT Value FROM Items WHERE id=1 UNION ALL "
                       "SELECT b.Name FROM Items a JOIN Items b ON b.Name=a.Name",
                .covering = {}},
           Case{.sql = "SELECT Name FROM Items WHERE id=1 UNION ALL "
                       "SELECT b.Value FROM Items a JOIN Items b ON b.Name=a.Name",
                .covering = {ColumnId{2}}},
           Case{.sql = "SELECT Value FROM Items WHERE id=1 UNION ALL "
                       "SELECT b.id FROM Items a JOIN Items b "
                       "ON b.Name=a.Name AND b.Value>0",
                .covering = {ColumnId{2}}},
       }) {
    SCOPED_TRACE(test_case.sql);
    const PhysicalPlan plan =
        OptimizeOrThrow(test_case.sql, TestCatalog(), BindEnvironment::Core(),
                        OptimizerOptions{.allow_file_backed_automatic_indexes = true});
    std::size_t count = 0;
    for (const PhysicalNode& node : plan.nodes()) {
      if (const auto* automatic = std::get_if<PhysicalAutomaticIndexNode>(&node.payload);
          automatic != nullptr) {
        EXPECT_EQ(1U, automatic->core_index);
        EXPECT_EQ(test_case.covering, automatic->covering_columns);
        ++count;
      }
    }
    EXPECT_EQ(1U, count);
  }
}

TEST(PhysicalPlan, PreservesStoredAndRowidPriorityOverAutomaticSources) {
  const PhysicalPlan indexed = OptimizeOrThrow(
      "SELECT a.id,b.Payload FROM Items a JOIN Items b ON b.Category=a.Category", IndexedCatalog(),
      BindEnvironment::Core(), OptimizerOptions{.allow_file_backed_automatic_indexes = true});
  EXPECT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(indexed.nodes()[1].payload));
  EXPECT_FALSE(std::get<PhysicalIndexScanNode>(indexed.nodes()[1].payload).covering);
  const PhysicalPlan rowid = OptimizeOrThrow(
      "SELECT a.id,b.Value FROM Items a JOIN Items b ON b.id=a.id AND b.Name=a.Name", TestCatalog(),
      BindEnvironment::Core(), OptimizerOptions{.allow_file_backed_automatic_indexes = true});
  EXPECT_TRUE(std::holds_alternative<PhysicalRowIdLookupNode>(rowid.nodes()[1].payload));
}

TEST(PhysicalPlan, SelectsCoveringStoredIndexPrefixesForJoinedSources) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT a.id,b.id,b.Category,b.Score FROM Items a JOIN Items b "
      "ON b.Category=a.Category AND b.Score=a.Score WHERE a.id=?1",
      IndexedCatalog());
  ASSERT_GE(plan.nodes().size(), 2U);
  const auto* index = std::get_if<PhysicalIndexScanNode>(&plan.nodes()[1].payload);
  ASSERT_NE(nullptr, index);
  EXPECT_EQ(0U, index->core_index);
  EXPECT_EQ(BoundSourceId{1}, index->source);
  EXPECT_EQ(TableId{0}, index->table);
  EXPECT_EQ(IndexId{0}, index->index);
  EXPECT_EQ(RootPageId{10}, index->index_root_page);
  EXPECT_TRUE(index->covering);
  EXPECT_FALSE(index->range.has_value());
  ASSERT_EQ(2U, index->equalities.size());
  EXPECT_EQ(ColumnId{1}, index->equalities[0].column);
  EXPECT_EQ(ColumnId{2}, index->equalities[1].column);
  EXPECT_EQ(SortOrder::kDescending, index->equalities[1].order);
  const BoundSelect& bound = plan.logical_plan().bound_select();
  for (const PhysicalIndexEquality& equality : index->equalities) {
    const auto& key = std::get<BoundColumnExpression>(bound.expression(equality.key).payload);
    EXPECT_EQ(BoundSourceId{0}, bound.source_columns()[key.column.value()].source);
    EXPECT_TRUE(equality.reject_null);
  }
  EXPECT_NE(std::string::npos,
            ExplainPhysicalPlan(plan).find("SEARCH \"Items\" USING COVERING INDEX "
                                           "\"items_category_score\""));
}

TEST(PhysicalPlan, DoesNotConstrainTextIndexesWithNumericComparisonAffinity) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT id,Category FROM Items "
      "WHERE Category=(SELECT Score FROM Items WHERE id=1)",
      IndexedCatalog());
  ASSERT_FALSE(plan.candidates().empty());
  for (const AccessPathCandidate& candidate : plan.candidates()) {
    EXPECT_EQ(0U, candidate.equality_term_count);
    EXPECT_EQ(0U, candidate.range_bound_count);
  }
}

TEST(PhysicalPlan, SelectsNoncoveringStoredIndexPrefixesForJoinedSources) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT a.id,b.id,b.Payload FROM Items a JOIN Items b "
      "ON b.Category=a.Category AND b.Score=a.Score WHERE a.id=?1",
      IndexedCatalog());
  ASSERT_GE(plan.nodes().size(), 2U);
  const auto* index = std::get_if<PhysicalIndexScanNode>(&plan.nodes()[1].payload);
  ASSERT_NE(nullptr, index);
  EXPECT_EQ(0U, index->core_index);
  EXPECT_EQ(BoundSourceId{1}, index->source);
  EXPECT_EQ(TableId{0}, index->table);
  EXPECT_EQ(IndexId{0}, index->index);
  EXPECT_EQ(RootPageId{10}, index->index_root_page);
  EXPECT_FALSE(index->covering);
  EXPECT_FALSE(index->range.has_value());
  ASSERT_EQ(2U, index->equalities.size());
  EXPECT_EQ(ColumnId{1}, index->equalities[0].column);
  EXPECT_EQ(ColumnId{2}, index->equalities[1].column);
  EXPECT_NE(std::string::npos, ExplainPhysicalPlan(plan).find("SEARCH \"Items\" USING INDEX "
                                                              "\"items_category_score\""));
}

TEST(PhysicalPlan, SelectsParameterizedJoinedRangesWithAndWithoutEqualityPrefixes) {
  struct Case {
    std::string_view predicate;
    IndexId index;
    ColumnId column;
    std::size_t equalities;
    SortOrder order;
    std::optional<bool> lower;
    std::optional<bool> upper;
  };
  const std::array cases{
      Case{.predicate = "b.Category>=a.Category AND b.Category<=a.Category",
           .index = IndexId{0},
           .column = ColumnId{1},
           .equalities = 0,
           .order = SortOrder::kAscending,
           .lower = true,
           .upper = true},
      Case{.predicate = "a.Score<b.Score",
           .index = IndexId{1},
           .column = ColumnId{2},
           .equalities = 0,
           .order = SortOrder::kDescending,
           .lower = false,
           .upper = std::nullopt},
      Case{.predicate = "b.Score<=a.Score",
           .index = IndexId{1},
           .column = ColumnId{2},
           .equalities = 0,
           .order = SortOrder::kDescending,
           .lower = std::nullopt,
           .upper = true},
      Case{.predicate = "b.Category IS a.Category AND a.Score<b.Score AND b.Score<=?2",
           .index = IndexId{0},
           .column = ColumnId{2},
           .equalities = 1,
           .order = SortOrder::kDescending,
           .lower = false,
           .upper = true},
      Case{.predicate = "b.Category IS a.Category AND b.Score<?2",
           .index = IndexId{0},
           .column = ColumnId{2},
           .equalities = 1,
           .order = SortOrder::kDescending,
           .lower = std::nullopt,
           .upper = false},
  };
  for (const bool covering : {true, false}) {
    for (const std::string_view join : {"JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL JOIN"}) {
      for (const Case& test_case : cases) {
        SCOPED_TRACE(::testing::Message()
                     << join << " covering=" << covering << " " << test_case.predicate);
        const PhysicalPlan plan =
            OptimizeOrThrow("SELECT a.id,b.id" + std::string{covering ? "" : ",b.Payload"} +
                                " FROM Items a " + std::string{join} + " Items b ON " +
                                std::string{test_case.predicate} + " WHERE a.id=?1",
                            IndexedCatalog());
        ASSERT_GE(plan.nodes().size(), 2U);
        const auto* index = std::get_if<PhysicalIndexScanNode>(&plan.nodes()[1].payload);
        ASSERT_NE(nullptr, index);
        EXPECT_EQ(BoundSourceId{1}, index->source);
        EXPECT_EQ(test_case.index, index->index);
        EXPECT_EQ(covering, index->covering);
        EXPECT_EQ(PhysicalScanDirection::kForward, index->direction);
        EXPECT_EQ(test_case.equalities, index->equalities.size());
        ASSERT_TRUE(index->range.has_value());
        const PhysicalIndexRange& range = *index->range;
        EXPECT_EQ(test_case.column, range.column);
        EXPECT_EQ(test_case.order, range.order);
        ASSERT_EQ(test_case.lower.has_value(), range.lower.has_value());
        ASSERT_EQ(test_case.upper.has_value(), range.upper.has_value());
        if (range.lower.has_value()) {
          EXPECT_EQ(test_case.lower, range.lower->inclusive);
        }
        if (range.upper.has_value()) {
          EXPECT_EQ(test_case.upper, range.upper->inclusive);
        }
      }
    }
  }
}

TEST(PhysicalPlan, KeepsJoinedEqualityAccessWithinSourceAndPreservationBarriers) {
  struct Case {
    std::string_view sql;
    std::vector<BoundSourceId> indexes;
    std::vector<BoundSourceId> noncovering{};
    std::vector<BoundSourceId> ranges{};
  };
  const std::array cases{
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Category IS a.Category",
           .indexes = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON a.Category=b.Category COLLATE NOCASE",
           .indexes = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a LEFT JOIN Items b ON b.Category=?1",
           .indexes = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a RIGHT JOIN Items b ON b.Category='alpha'",
           .indexes = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a FULL JOIN Items b "
                  "ON b.Category=coalesce(a.Category,?1)",
           .indexes = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a CROSS JOIN Items b "
                  "WHERE a.Category=?1 AND b.Category=a.Category",
           .indexes = {BoundSourceId{0}, BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a LEFT JOIN Items b "
                  "ON 1 WHERE b.Category=?1",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a RIGHT JOIN Items b "
                  "ON 1 WHERE b.Category=?1",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a FULL JOIN Items b "
                  "ON 1 WHERE a.Category=?1",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id,c.id FROM Items a JOIN Items b ON b.Category=c.Category "
                  "LEFT JOIN Items c ON c.Category=a.Category",
           .indexes = {BoundSourceId{2}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Category=b.Code", .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON +b.Category=a.Category",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.Category=abs(a.Score)",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.Category COLLATE BINARY=a.Category",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Category=a.Score",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.Payload FROM Items a JOIN Items b "
                  "ON b.Category=a.Category",
           .indexes = {BoundSourceId{1}},
           .noncovering = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Category=a.Category "
                  "ORDER BY b.Payload",
           .indexes = {BoundSourceId{1}},
           .noncovering = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id,c.id FROM Items a JOIN Items b ON b.Category=a.Category "
                  "JOIN Items c ON c.Code=b.Payload",
           .indexes = {BoundSourceId{1}, BoundSourceId{2}},
           .noncovering = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.Category=a.Category AND b.Score>a.Score",
           .indexes = {BoundSourceId{1}},
           .ranges = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.id=a.id AND b.Category=a.Category",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Code=a.Code",
           .indexes = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Flag=a.Flag",
           .indexes = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.Payload FROM Items a JOIN Items b ON b.Flag=a.Flag",
           .indexes = {}},
  };
  const auto catalog = IndexedCatalog();
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    const PhysicalPlan plan = OptimizeOrThrow(test_case.sql, catalog);
    std::vector<BoundSourceId> indexes;
    for (const PhysicalNode& node : plan.nodes()) {
      if (const auto* index = std::get_if<PhysicalIndexScanNode>(&node.payload); index != nullptr) {
        EXPECT_EQ(!std::ranges::contains(test_case.noncovering, index->source), index->covering);
        EXPECT_EQ(std::ranges::contains(test_case.ranges, index->source), index->range.has_value());
        EXPECT_FALSE(index->equalities.empty());
        indexes.push_back(index->source);
      }
    }
    EXPECT_EQ(test_case.indexes, indexes);
  }
}

TEST(PhysicalPlan, KeepsJoinedRangesWithinCanonicalAdmissionBarriers) {
  struct Case {
    std::string_view sql;
    std::vector<BoundSourceId> indexes;
    std::vector<BoundSourceId> ranges{};
  };
  const std::array cases{
      Case{.sql = "SELECT a.id,b.id FROM Items a CROSS JOIN Items b "
                  "WHERE a.Score>?1 AND b.Score>=a.Score AND b.Score<=?2",
           .indexes = {BoundSourceId{0}, BoundSourceId{1}},
           .ranges = {BoundSourceId{0}, BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Category>a.Category",
           .indexes = {BoundSourceId{1}},
           .ranges = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a LEFT JOIN Items b ON 1 WHERE b.Score>?1",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a RIGHT JOIN Items b ON 1 WHERE b.Score>?1",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a FULL JOIN Items b ON 1 WHERE a.Score>?1",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id,c.id FROM Items a JOIN Items b ON b.Score>c.Score "
                  "LEFT JOIN Items c ON c.Score>=a.Score",
           .indexes = {BoundSourceId{2}},
           .ranges = {BoundSourceId{2}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Score>b.Flag", .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON +b.Score>a.Score", .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Score>abs(a.Score)",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Category>a.Score",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.Category COLLATE BINARY>a.Category",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Code>a.Code", .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.id=a.id AND b.Score>a.Score",
           .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON 1", .indexes = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.Score>coalesce(a.Score,?1)",
           .indexes = {BoundSourceId{1}},
           .ranges = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.Score>a.rowid",
           .indexes = {BoundSourceId{1}},
           .ranges = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.Category=a.Category AND b.Score>abs(a.Score)",
           .indexes = {BoundSourceId{1}}},
  };
  const auto catalog = IndexedCatalog();
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    const PhysicalPlan plan = OptimizeOrThrow(test_case.sql, catalog);
    std::vector<BoundSourceId> indexes;
    for (const PhysicalNode& node : plan.nodes()) {
      if (const auto* index = std::get_if<PhysicalIndexScanNode>(&node.payload); index != nullptr) {
        EXPECT_EQ(std::ranges::contains(test_case.ranges, index->source), index->range.has_value());
        EXPECT_TRUE(!index->equalities.empty() || index->range.has_value());
        indexes.push_back(index->source);
      }
    }
    EXPECT_EQ(test_case.indexes, indexes);
  }
}

TEST(PhysicalPlan, DiscoversJoinedIndexFieldsInTheirActualCompoundCore) {
  struct Case {
    std::string_view sql;
    std::size_t indexes;
    bool covering;
    bool range = false;
  };
  const std::array cases{
      Case{.sql = "SELECT Payload FROM Items WHERE id=1 UNION ALL "
                  "SELECT b.Category FROM Items a JOIN Items b ON b.Category=a.Category",
           .indexes = 1,
           .covering = true},
      Case{.sql = "SELECT Category FROM Items WHERE id=1 UNION ALL "
                  "SELECT b.Payload FROM Items a JOIN Items b ON b.Category=a.Category",
           .indexes = 1,
           .covering = false},
      Case{.sql = "SELECT Payload FROM Items WHERE id=1 UNION ALL "
                  "SELECT b.Category FROM Items a JOIN Items b ON b.Category>a.Category",
           .indexes = 1,
           .covering = true,
           .range = true},
      Case{.sql = "SELECT Category FROM Items WHERE id=1 UNION ALL "
                  "SELECT b.Payload FROM Items a JOIN Items b ON b.Category>a.Category",
           .indexes = 1,
           .covering = false,
           .range = true},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    const PhysicalPlan plan = OptimizeOrThrow(test_case.sql, IndexedCatalog());
    std::size_t indexes = 0;
    for (const PhysicalNode& node : plan.nodes()) {
      if (const auto* index = std::get_if<PhysicalIndexScanNode>(&node.payload); index != nullptr) {
        EXPECT_EQ(1U, index->core_index);
        EXPECT_EQ(test_case.covering, index->covering);
        EXPECT_EQ(test_case.range, index->range.has_value());
        ++indexes;
      }
    }
    EXPECT_EQ(test_case.indexes, indexes);
  }
}

TEST(PhysicalPlan, KeepsJoinedRowidKeysBehindForwardAndOuterPreservationBarriers) {
  struct Case {
    std::string_view sql;
    std::vector<BoundSourceId> lookups;
  };
  const std::array cases{
      Case{.sql = "SELECT a.id,b.id FROM Items a LEFT JOIN Items b ON b.Name=a.Name "
                  "WHERE b.id IS NULL",
           .lookups = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a FULL JOIN Items b ON 1 WHERE a.id=1",
           .lookups = {}},
      Case{.sql = "SELECT a.id,b.id,c.id FROM Items a JOIN Items b ON b.id=c.id "
                  "LEFT JOIN Items c ON c.id=a.id",
           .lookups = {BoundSourceId{2}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON +b.id=a.id", .lookups = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.id=b.Value", .lookups = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.id=abs(a.id)", .lookups = {}},
      Case{.sql = "SELECT a.id,b.key FROM Items a JOIN wr b ON b.key=a.id", .lookups = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a CROSS JOIN Items b "
                  "WHERE a.id=2 AND b.id=a.id",
           .lookups = {BoundSourceId{0}, BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a RIGHT JOIN Items b ON b.id=a.id WHERE a.id=2",
           .lookups = {BoundSourceId{1}}},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    const PhysicalPlan plan = OptimizeOrThrow(test_case.sql, TestCatalog());
    std::vector<BoundSourceId> lookups;
    for (const PhysicalNode& node : plan.nodes()) {
      if (const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&node.payload);
          lookup != nullptr) {
        lookups.push_back(lookup->source);
      }
    }
    EXPECT_EQ(test_case.lookups, lookups);
  }
}

TEST(PhysicalPlan, SelectsComputedJoinedRowidProbesInValueOnlyOnContexts) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT a.id,b.id FROM Items a FULL JOIN Items b "
      "ON b.id=a.id+?3 AND a.id<=?1 AND b.id<=?2 "
      "WHERE (a.id<=?1 OR a.id IS NULL) AND (b.id<=?2 OR b.id IS NULL)",
      TestCatalog());
  const auto lookup = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
    const auto* point = std::get_if<PhysicalRowIdLookupNode>(&node.payload);
    return point != nullptr && point->source == BoundSourceId{1};
  });
  ASSERT_NE(plan.nodes().end(), lookup);
  const auto key = std::get<PhysicalRowIdLookupNode>(lookup->payload).key;
  const auto* arithmetic = std::get_if<BoundBinaryExpression>(
      &plan.logical_plan().bound_select().expression(key).payload);
  ASSERT_NE(nullptr, arithmetic);
  EXPECT_EQ(BoundBinaryOperation::kAdd, arithmetic->operation);
}

TEST(PhysicalPlan, KeepsComputedJoinedKeysBehindCallbackAndPreservationBoundaries) {
  struct Case {
    std::string_view sql;
    std::vector<BoundSourceId> lookups;
  };
  const std::array cases{
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.id=a.id+1 AND abs(a.Value)>0",
           .lookups = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a FULL JOIN Items b "
                  "ON b.id=a.id+1 AND (SELECT a.id)>0",
           .lookups = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.id=a.id+1 AND a.Name||a.Name!=''",
           .lookups = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.id=a.id+1 AND iif(a.id>0,1,0)",
           .lookups = {}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b "
                  "ON b.id=a.id AND abs(a.Value)>0",
           .lookups = {BoundSourceId{1}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a JOIN Items b ON b.id=b.Value+1", .lookups = {}},
      Case{.sql = "SELECT a.id,b.id,c.id FROM Items a JOIN Items b ON b.id=c.id+1 "
                  "LEFT JOIN Items c ON c.id=a.id",
           .lookups = {BoundSourceId{2}}},
      Case{.sql = "SELECT a.id,b.id FROM Items a LEFT JOIN Items b ON 1 "
                  "WHERE b.id=a.id+1",
           .lookups = {}},
      Case{.sql = "SELECT a.id,b.id,c.id FROM Items a JOIN Items b "
                  "ON c.id>0 AND abs(a.Value)>0 JOIN Items c ON c.id=a.id+1000",
           .lookups = {}},
      Case{.sql = "SELECT a.id,b.id,c.id FROM Items a JOIN Items b ON b.id=a.id+1000 "
                  "JOIN Items c ON b.id>0 AND abs(a.Value)>0",
           .lookups = {}},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    const auto plan = OptimizeOrThrow(test_case.sql, TestCatalog());
    std::vector<BoundSourceId> lookups;
    for (const auto& node : plan.nodes()) {
      if (const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&node.payload);
          lookup != nullptr) {
        lookups.push_back(lookup->source);
      }
    }
    EXPECT_EQ(test_case.lookups, lookups);
  }
}

TEST(PhysicalPlan, AdmitsNumericJoinedRowidShapesWithoutChangingValueKeyPreference) {
  for (const std::string_view join : {"JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL JOIN"}) {
    for (const std::string_view key :
         {"a.id+?1", "?1+a.id", "a.Value-1", "a.id*2", "a.id/2", "a.id%2", "a.id<<1", "a.id>>1",
          "a.id&?1", "a.id|?1", "-a.id", "+a.id", "~a.id", "coalesce(a.id,?1)+1",
          "(a.id+?1) COLLATE NOCASE"}) {
      SCOPED_TRACE(::testing::Message() << join << " " << key);
      const auto plan = OptimizeOrThrow("SELECT a.id,b.id FROM Items a " + std::string{join} +
                                            " Items b ON b.id=" + std::string{key},
                                        TestCatalog());
      EXPECT_TRUE(std::ranges::any_of(plan.nodes(), [](const PhysicalNode& node) {
        const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&node.payload);
        return lookup != nullptr && lookup->source == BoundSourceId{1};
      }));
    }
  }
  const auto plan = OptimizeOrThrow(
      "SELECT a.id,b.id FROM Items a JOIN Items b ON b.id=a.id+1 AND b.id=?1", TestCatalog());
  const auto lookup = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
    const auto* point = std::get_if<PhysicalRowIdLookupNode>(&node.payload);
    return point != nullptr && point->source == BoundSourceId{1};
  });
  ASSERT_NE(plan.nodes().end(), lookup);
  EXPECT_TRUE(std::holds_alternative<BoundParameterExpression>(
      plan.logical_plan()
          .bound_select()
          .expression(std::get<PhysicalRowIdLookupNode>(lookup->payload).key)
          .payload));
}

TEST(PhysicalPlan, PreservesNonjoinedFunctionKeysAndJoinedIndexValueOnlyAdmission) {
  const auto point = OptimizeOrThrow("SELECT Name FROM Items WHERE id=abs(?1)+?2", TestCatalog());
  EXPECT_TRUE(std::holds_alternative<PhysicalRowIdLookupNode>(point.nodes().front().payload));
  const auto index =
      OptimizeOrThrow("SELECT Category FROM Items WHERE Category=lower(?1)", IndexedCatalog());
  EXPECT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(index.nodes().front().payload));
  for (const bool automatic : {false, true}) {
    const auto joined =
        OptimizeOrThrow("SELECT a.id,b.Score FROM Items a JOIN Items b ON b.Score=a.Score+?1",
                        IndexedCatalog(), BindEnvironment::Core(),
                        OptimizerOptions{.allow_file_backed_automatic_indexes = automatic});
    EXPECT_FALSE(std::ranges::any_of(joined.nodes(), [](const PhysicalNode& node) {
      return std::holds_alternative<PhysicalIndexScanNode>(node.payload) ||
             std::holds_alternative<PhysicalAutomaticIndexNode>(node.payload);
    }));
  }
}

TEST(PhysicalPlan, RetainsOnConditionsAndPinsLeftReadinessToItsOwnSource) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT a.id FROM Items a JOIN Items b ON c.id=b.id "
      "LEFT JOIN Items c ON a.id>0 INNER JOIN Items d ON 1",
      TestCatalog());
  const std::array kinds{SourceJoinKind::kInner, SourceJoinKind::kLeft, SourceJoinKind::kInner};
  const std::array ready{BoundSourceId{2}, BoundSourceId{2}, BoundSourceId{0}};
  const auto cores = plan.logical_plan().bound_select().query_cores();
  const auto& sources = std::get<BoundSelectCore>(cores.front()).table_sources;
  std::size_t index = 0;
  for (std::size_t node_index = 0; node_index < plan.nodes().size(); ++node_index) {
    const PhysicalNode& node = plan.nodes()[node_index];
    if (const auto* join = std::get_if<PhysicalNestedLoopJoinNode>(&node.payload);
        join != nullptr) {
      ASSERT_LT(index, kinds.size());
      EXPECT_EQ(kinds[index], join->join);
      EXPECT_EQ(ready[index], join->condition_source);
      ASSERT_TRUE(join->on.has_value());
      EXPECT_EQ(sources[index + 1U].on, join->on);
      const auto* logical =
          std::get_if<LogicalJoinNode>(&plan.logical_plan().nodes()[node_index].payload);
      ASSERT_NE(nullptr, logical);
      EXPECT_EQ(logical->on, join->on);
      ++index;
    }
  }
  EXPECT_EQ(kinds.size(), index);
  EXPECT_NE(std::string::npos, ExplainPhysicalPlan(plan).find("NESTED LOOP LEFT JOIN"));
  EXPECT_NE(std::string::npos, ExplainPhysicalPlan(plan).find("NESTED LOOP INNER JOIN"));
}

TEST(PhysicalPlan, KeepsWhereAndSuffixOnAfterTheirRightNullExtensionBarriers) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT a.id,b.id FROM Items a RIGHT JOIN Items b ON a.id=b.id "
      "JOIN Items c ON a.id>0 WHERE a.id>1 AND b.id>0",
      TestCatalog());
  std::size_t joins = 0;
  for (const PhysicalNode& node : plan.nodes()) {
    if (const auto* join = std::get_if<PhysicalNestedLoopJoinNode>(&node.payload);
        join != nullptr) {
      EXPECT_EQ(BoundSourceId{1}, join->condition_source);
      ++joins;
    } else if (const auto* filter = std::get_if<PhysicalFilterNode>(&node.payload);
               filter != nullptr) {
      EXPECT_EQ((std::vector{BoundSourceId{1}, BoundSourceId{1}}), filter->predicate_sources);
    }
  }
  EXPECT_EQ(2U, joins);
  EXPECT_NE(std::string::npos, ExplainPhysicalPlan(plan).find("NESTED LOOP RIGHT JOIN"));
}

TEST(PhysicalPlan, PreservesEmptyAggregateAccessCoreIndex) {
  const PhysicalPlan plan =
      OptimizeOrThrow("SELECT count() UNION ALL SELECT count() FROM Items WHERE 0", TestCatalog());
  const auto empty = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalEmptyNode>(node.payload);
  });
  ASSERT_NE(plan.nodes().end(), empty);
  EXPECT_EQ(1U, std::get<PhysicalEmptyNode>(empty->payload).core_index);
}

TEST(PhysicalPlan, PreservesRowIdAggregateAccessCoreIndex) {
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT count() UNION ALL SELECT count() FROM Items WHERE rowid=?1", TestCatalog());
  const auto lookup = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalRowIdLookupNode>(node.payload);
  });
  ASSERT_NE(plan.nodes().end(), lookup);
  EXPECT_EQ(1U, std::get<PhysicalRowIdLookupNode>(lookup->payload).core_index);
}

TEST(PhysicalPlan, PreservesIndexedAggregateAccessCoreIndex) {
  const PhysicalPlan plan =
      OptimizeOrThrow("SELECT count() UNION ALL SELECT min(Score) FROM Items", IndexedCatalog());
  const auto scan = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalIndexScanNode>(node.payload);
  });
  ASSERT_NE(plan.nodes().end(), scan);
  EXPECT_EQ(1U, std::get<PhysicalIndexScanNode>(scan->payload).core_index);
}

TEST(PhysicalPlan, SelectsIndexedMinMaxDirectionsAndFallbacks) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();

  const PhysicalPlan minimum = OptimizeOrThrow("SELECT min(Score) FROM Items", catalog);
  ASSERT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(minimum.nodes().front().payload));
  const auto& minimum_scan =
      std::get<PhysicalIndexScanNode>(minimum.node(PhysicalNodeId{0}).payload);
  EXPECT_EQ(IndexId{1}, minimum_scan.index);
  EXPECT_TRUE(minimum_scan.covering);
  EXPECT_EQ(PhysicalScanDirection::kReverse, minimum_scan.direction);
  const PhysicalAggregateNode& minimum_aggregate = AggregateNode(minimum);
  EXPECT_EQ(PhysicalAggregateStrategy::kIndexedMinMax, minimum_aggregate.strategy);
  ASSERT_TRUE(minimum_aggregate.min_max.has_value());
  EXPECT_EQ(PhysicalMinMaxNullStrategy::kNonNullFirstWithSingleNullFallback,
            minimum_aggregate.min_max->null_strategy);
  EXPECT_FALSE(minimum_aggregate.min_max->argument_fixed_by_equality);
  ASSERT_NE(nullptr, minimum.selected_candidate());
  EXPECT_EQ(std::optional<IndexId>{IndexId{1}}, minimum.selected_candidate()->index);
  EXPECT_EQ(
      "SCAN \"Items\" USING COVERING INDEX \"items_score_desc\" REVERSE\n"
      "AGGREGATE CORE 0 INDEXED MIN/MAX",
      ExplainPhysicalPlan(minimum));

  const PhysicalPlan maximum = OptimizeOrThrow("SELECT max(Score) FROM Items", catalog);
  EXPECT_EQ(PhysicalScanDirection::kForward,
            std::get<PhysicalIndexScanNode>(maximum.nodes().front().payload).direction);

  const PhysicalPlan noncovering = OptimizeOrThrow("SELECT min(Score),Payload FROM Items", catalog);
  EXPECT_FALSE(std::get<PhysicalIndexScanNode>(noncovering.nodes().front().payload).covering);
  EXPECT_EQ(PhysicalAggregateStrategy::kIndexedMinMax, AggregateNode(noncovering).strategy);

  const PhysicalPlan fixed =
      OptimizeOrThrow("SELECT min(Score) FROM Items WHERE Score=?1", catalog);
  const PhysicalAggregateNode& fixed_aggregate = AggregateNode(fixed);
  ASSERT_TRUE(fixed_aggregate.min_max.has_value());
  EXPECT_TRUE(fixed_aggregate.min_max->argument_fixed_by_equality);

  const PhysicalPlan fixed_null =
      OptimizeOrThrow("SELECT min(Score) FROM Items WHERE Score IS NULL", catalog);
  const PhysicalAggregateNode& fixed_null_aggregate = AggregateNode(fixed_null);
  ASSERT_TRUE(fixed_null_aggregate.min_max.has_value());
  EXPECT_TRUE(fixed_null_aggregate.min_max->argument_fixed_by_equality);
  EXPECT_EQ(PhysicalMinMaxNullStrategy::kNonNullFirstWithSingleNullFallback,
            fixed_null_aggregate.min_max->null_strategy);

  const PhysicalPlan ranged =
      OptimizeOrThrow("SELECT min(Score) FROM Items WHERE Score>=?1 AND Score<?2", catalog);
  const auto& ranged_scan = std::get<PhysicalIndexScanNode>(ranged.node(PhysicalNodeId{0}).payload);
  EXPECT_EQ(PhysicalScanDirection::kReverse, ranged_scan.direction);
  ASSERT_TRUE(ranged_scan.range.has_value());
  EXPECT_TRUE(ranged_scan.range->lower.has_value());
  EXPECT_TRUE(ranged_scan.range->upper.has_value());

  for (const std::string_view sql : {
           "SELECT min(Score),max(Score) FROM Items",
           "SELECT min(Score) FILTER (WHERE Flag) FROM Items",
           "SELECT min(Score) FROM Items HAVING min(Score)>0",
           "SELECT min(Category COLLATE BINARY) FROM Items",
       }) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(PhysicalAggregateStrategy::kGlobal,
              AggregateNode(OptimizeOrThrow(sql, catalog)).strategy);
  }

  const CatalogSnapshotPtr rowid_catalog = TestCatalog();
  const PhysicalPlan rowid_min = OptimizeOrThrow("SELECT min(id) FROM Items", rowid_catalog);
  EXPECT_EQ(PhysicalScanDirection::kForward,
            std::get<PhysicalTableScanNode>(rowid_min.nodes().front().payload).direction);
  const PhysicalAggregateNode& rowid_min_aggregate = AggregateNode(rowid_min);
  ASSERT_TRUE(rowid_min_aggregate.min_max.has_value());
  EXPECT_EQ(PhysicalMinMaxNullStrategy::kNone, rowid_min_aggregate.min_max->null_strategy);
  const PhysicalPlan rowid_max = OptimizeOrThrow("SELECT max(_rowid_) FROM Items", rowid_catalog);
  EXPECT_EQ(PhysicalScanDirection::kReverse,
            std::get<PhysicalTableScanNode>(rowid_max.nodes().front().payload).direction);
}

TEST(PhysicalPlan, SelectsOrderedGroupingAndPreservesDeliveredTermOrder) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();
  const PhysicalPlan permuted =
      OptimizeOrThrow("SELECT Score,Category,count() FROM Items GROUP BY Score,Category", catalog);
  ASSERT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(permuted.nodes().front().payload));
  const auto& scan = std::get<PhysicalIndexScanNode>(permuted.node(PhysicalNodeId{0}).payload);
  EXPECT_EQ(IndexId{0}, scan.index);
  const PhysicalAggregateNode& aggregate = AggregateNode(permuted);
  EXPECT_EQ(PhysicalAggregateStrategy::kOrderedGroup, aggregate.strategy);
  ASSERT_EQ(2U, aggregate.grouping_order.size());
  EXPECT_EQ(1U, aggregate.grouping_order[0].group_term_index);
  EXPECT_EQ(SortOrder::kAscending, aggregate.grouping_order[0].order);
  EXPECT_EQ(BoundNullPlacement::kFirst, aggregate.grouping_order[0].null_placement);
  EXPECT_EQ(0U, aggregate.grouping_order[1].group_term_index);
  EXPECT_EQ(SortOrder::kDescending, aggregate.grouping_order[1].order);
  EXPECT_EQ(BoundNullPlacement::kLast, aggregate.grouping_order[1].null_placement);

  const PhysicalPlan equality_prefix =
      OptimizeOrThrow("SELECT Score,count() FROM Items WHERE Category=?1 GROUP BY Score", catalog);
  EXPECT_EQ(PhysicalAggregateStrategy::kOrderedGroup, AggregateNode(equality_prefix).strategy);
  ASSERT_EQ(1U, AggregateNode(equality_prefix).grouping_order.size());
  EXPECT_EQ(SortOrder::kDescending, AggregateNode(equality_prefix).grouping_order[0].order);

  const PhysicalPlan noncovering =
      OptimizeOrThrow("SELECT Category,Payload,count() FROM Items GROUP BY Category", catalog);
  EXPECT_FALSE(std::get<PhysicalIndexScanNode>(noncovering.nodes().front().payload).covering);
  EXPECT_EQ(PhysicalAggregateStrategy::kOrderedGroup, AggregateNode(noncovering).strategy);

  const PhysicalPlan mismatch = OptimizeOrThrow(
      "SELECT Category COLLATE BINARY,count() FROM Items "
      "GROUP BY Category COLLATE BINARY",
      catalog);
  EXPECT_EQ(PhysicalAggregateStrategy::kSortedGroup, AggregateNode(mismatch).strategy);

  const PhysicalPlan equality_mismatch = OptimizeOrThrow(
      "SELECT Category COLLATE BINARY,count() FROM Items WHERE Category=?1 "
      "GROUP BY Category COLLATE BINARY",
      catalog);
  EXPECT_EQ(PhysicalAggregateStrategy::kSortedGroup, AggregateNode(equality_mismatch).strategy);

  const PhysicalPlan unordered =
      OptimizeOrThrow("SELECT Code,count() FROM Items GROUP BY Code", catalog);
  EXPECT_EQ(PhysicalAggregateStrategy::kSortedGroup, AggregateNode(unordered).strategy);

  const PhysicalPlan expression = OptimizeOrThrow(
      "SELECT lower(Category),count() FROM Items GROUP BY lower(Category)", catalog);
  EXPECT_EQ(PhysicalAggregateStrategy::kSortedGroup, AggregateNode(expression).strategy);

  const PhysicalPlan volatile_group =
      OptimizeOrThrow("SELECT volatile_key(),count() FROM Items GROUP BY volatile_key()", catalog,
                      TestEnvironment());
  EXPECT_EQ(PhysicalAggregateStrategy::kSortedGroup, AggregateNode(volatile_group).strategy);
}

TEST(PhysicalPlan, EliminatesOnlyProvenAggregateOrdering) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();

  const PhysicalPlan global =
      OptimizeOrThrow("SELECT count() FROM Items ORDER BY abs(?) ASC NULLS LAST", catalog);
  EXPECT_TRUE(SortNode(global).input_order_satisfied);

  const CatalogSnapshotPtr unordered_catalog = TestCatalog();
  const PhysicalPlan hidden_order =
      OptimizeOrThrow("SELECT count() FROM Items GROUP BY ?1 ORDER BY volatile_key()",
                      unordered_catalog, TestEnvironment());
  EXPECT_TRUE(AggregateNode(hidden_order).at_most_one_row);
  EXPECT_FALSE(SortNode(hidden_order).input_order_satisfied);

  const PhysicalPlan fixed_group_order = OptimizeOrThrow(
      "SELECT abs(?1),count() FROM Items "
      "GROUP BY abs(?1) ORDER BY abs(?1) DESC",
      unordered_catalog);
  EXPECT_TRUE(AggregateNode(fixed_group_order).at_most_one_row);
  EXPECT_TRUE(SortNode(fixed_group_order).input_order_satisfied);

  const PhysicalPlan sorted = OptimizeOrThrow(
      "SELECT Name || '',count() FROM Items "
      "GROUP BY Name || '' ORDER BY Name || '' DESC",
      unordered_catalog);
  EXPECT_EQ(PhysicalAggregateStrategy::kSortedGroup, AggregateNode(sorted).strategy);
  ASSERT_EQ(1U, AggregateNode(sorted).grouping_order.size());
  EXPECT_EQ(SortOrder::kDescending, AggregateNode(sorted).grouping_order[0].order);
  EXPECT_EQ(BoundNullPlacement::kLast, AggregateNode(sorted).grouping_order[0].null_placement);
  EXPECT_TRUE(SortNode(sorted).input_order_satisfied);

  const PhysicalPlan nondefault_nulls = OptimizeOrThrow(
      "SELECT Name || '',count() FROM Items "
      "GROUP BY Name || '' ORDER BY Name || '' ASC NULLS LAST",
      unordered_catalog);
  EXPECT_FALSE(SortNode(nondefault_nulls).input_order_satisfied);

  const PhysicalPlan reversed = OptimizeOrThrow(
      "SELECT Category,count() FROM Items GROUP BY Category ORDER BY Category DESC", catalog);
  EXPECT_EQ(PhysicalAggregateStrategy::kOrderedGroup, AggregateNode(reversed).strategy);
  EXPECT_EQ(PhysicalScanDirection::kReverse,
            std::get<PhysicalIndexScanNode>(reversed.nodes().front().payload).direction);
  EXPECT_EQ(SortOrder::kDescending, AggregateNode(reversed).grouping_order[0].order);
  EXPECT_EQ(BoundNullPlacement::kLast, AggregateNode(reversed).grouping_order[0].null_placement);
  EXPECT_TRUE(SortNode(reversed).input_order_satisfied);

  const PhysicalPlan permutation = OptimizeOrThrow(
      "SELECT Score,Category,count() FROM Items "
      "GROUP BY Score,Category ORDER BY Score,Category",
      catalog);
  EXPECT_FALSE(SortNode(permutation).input_order_satisfied);

  const PhysicalPlan fixed_prefix = OptimizeOrThrow(
      "SELECT Category,Score,count() FROM Items WHERE Category=?1 "
      "GROUP BY Category,Score ORDER BY Category,Score DESC",
      catalog);
  EXPECT_TRUE(SortNode(fixed_prefix).input_order_satisfied);

  const CatalogSnapshotPtr rowid_catalog = TestCatalog();
  const PhysicalPlan rowid_alias = OptimizeOrThrow(
      "SELECT id,count() FROM Items GROUP BY _rowid_ ORDER BY id DESC", rowid_catalog);
  EXPECT_EQ(PhysicalScanDirection::kReverse,
            std::get<PhysicalTableScanNode>(rowid_alias.nodes().front().payload).direction);
  EXPECT_TRUE(SortNode(rowid_alias).input_order_satisfied);
}

TEST(PhysicalPlan, OptimizesDistinctValuesAndCompoundQueries) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  constexpr std::array<std::string_view, 3> cases{
      "SELECT DISTINCT Name FROM Items",
      "VALUES(1),(2)",
      "SELECT 1 UNION SELECT 2",
  };
  for (const std::string_view sql : cases) {
    SCOPED_TRACE(sql);
    BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog);
    ASSERT_TRUE(bound.has_value()) << bound.error().detail;
    BuildLogicalPlanResult logical = BuildLogicalPlan(std::move(*bound));
    ASSERT_TRUE(logical.has_value()) << logical.error().detail;
    OptimizeLogicalPlanResult physical = OptimizeLogicalPlan(std::move(*logical));
    ASSERT_TRUE(physical.has_value()) << physical.error().detail;
  }
}

TEST(PhysicalPlan, PublishesCanonicalAdvancedAccessAndMembership) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const PhysicalPlan values = OptimizeOrThrow("VALUES(1),(2)", catalog);
  ASSERT_EQ(1U, values.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalValuesNode>(values.nodes()[0].payload));
  EXPECT_TRUE(values.candidates().empty());
  EXPECT_FALSE(values.selected_candidate_index().has_value());
  EXPECT_EQ(nullptr, values.selected_candidate());
  EXPECT_EQ("VALUES CORE 0", ExplainPhysicalPlan(values));

  const PhysicalPlan distinct =
      OptimizeOrThrow("SELECT DISTINCT Name FROM Items WHERE id=1", catalog);
  ASSERT_EQ(4U, distinct.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalRowIdLookupNode>(distinct.nodes()[0].payload));
  EXPECT_TRUE(std::holds_alternative<PhysicalFilterNode>(distinct.nodes()[1].payload));
  EXPECT_TRUE(std::holds_alternative<PhysicalProjectionNode>(distinct.nodes()[2].payload));
  const auto& node = std::get<PhysicalDistinctNode>(distinct.nodes()[3].payload);
  EXPECT_EQ(PhysicalDistinctStrategy::kEphemeralMembership, node.strategy);
  ASSERT_EQ(1U, distinct.core_access_plans().size());
  EXPECT_EQ(0U, distinct.core_access_plans()[0].core_index);
  ASSERT_NE(nullptr, distinct.selected_candidate());
  EXPECT_EQ(PhysicalAccessKind::kRowIdLookup, distinct.selected_candidate()->kind);
  EXPECT_EQ(
      "SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid=?)\n"
      "DISTINCT CORE 0 USING EPHEMERAL RELATION",
      ExplainPhysicalPlan(distinct));

  const PhysicalPlan guarded = OptimizeOrThrow(
      "SELECT DISTINCT Name FROM Items WHERE id<0 AND abs(-9223372036854775808)", catalog);
  const auto filter = std::ranges::find_if(guarded.nodes(), [](const PhysicalNode& physical) {
    return std::holds_alternative<PhysicalFilterNode>(physical.payload);
  });
  ASSERT_NE(guarded.nodes().end(), filter);
  const auto& guarded_filter = std::get<PhysicalFilterNode>(filter->payload);
  EXPECT_TRUE(guarded_filter.predicates.empty());
  ASSERT_EQ(1U, guarded_filter.guards.size());
  EXPECT_TRUE(std::holds_alternative<PhysicalRowIdRangeNode>(guarded.nodes()[0].payload));
}

TEST(PhysicalPlan, SelectsUnorderedCompoundStrategies) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  struct Case {
    std::string_view sql;
    PhysicalCompoundStrategy strategy;
  };
  constexpr std::array cases{
      Case{.sql = "SELECT 1 UNION ALL SELECT 2",
           .strategy = PhysicalCompoundStrategy::kConcatenate},
      Case{.sql = "SELECT 1 UNION SELECT 2", .strategy = PhysicalCompoundStrategy::kEphemeralSet},
      Case{.sql = "SELECT 1 EXCEPT SELECT 2", .strategy = PhysicalCompoundStrategy::kEphemeralSet},
      Case{.sql = "SELECT 1 INTERSECT SELECT 2",
           .strategy = PhysicalCompoundStrategy::kEphemeralSet},
      Case{.sql = "SELECT 1 UNION SELECT 2 LIMIT +1",
           .strategy = PhysicalCompoundStrategy::kUnionLimitOne},
      Case{.sql = "SELECT 1 UNION SELECT 2 LIMIT (1)",
           .strategy = PhysicalCompoundStrategy::kUnionLimitOne},
      Case{.sql = "SELECT 1 UNION SELECT 2 LIMIT -(-1)",
           .strategy = PhysicalCompoundStrategy::kUnionLimitOne},
      Case{.sql = "SELECT 1 UNION SELECT 2 LIMIT 1+0",
           .strategy = PhysicalCompoundStrategy::kEphemeralSet},
      Case{.sql = "SELECT 1 UNION SELECT 2 LIMIT 1.0",
           .strategy = PhysicalCompoundStrategy::kEphemeralSet},
      Case{.sql = "SELECT 1 UNION SELECT 2 LIMIT ?1",
           .strategy = PhysicalCompoundStrategy::kEphemeralSet},
      Case{.sql = "SELECT 1 UNION SELECT 2 LIMIT TRUE",
           .strategy = PhysicalCompoundStrategy::kEphemeralSet},
      Case{.sql = "SELECT 1 UNION SELECT 2 LIMIT +TRUE",
           .strategy = PhysicalCompoundStrategy::kEphemeralSet},
      Case{.sql = "SELECT 1 UNION SELECT 2 LIMIT -(-TRUE)",
           .strategy = PhysicalCompoundStrategy::kEphemeralSet},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    const PhysicalPlan plan = OptimizeOrThrow(test_case.sql, catalog);
    const auto compound = std::ranges::find_if(plan.nodes(), [](const PhysicalNode& node) {
      return std::holds_alternative<PhysicalCompoundNode>(node.payload);
    });
    ASSERT_NE(plan.nodes().end(), compound);
    EXPECT_EQ(test_case.strategy, std::get<PhysicalCompoundNode>(compound->payload).strategy);
  }

  const PhysicalPlan blocked =
      OptimizeOrThrow("SELECT 2 UNION SELECT 1 EXCEPT SELECT 2 LIMIT 1", catalog);
  std::vector<PhysicalCompoundStrategy> blocked_strategies;
  for (const PhysicalNode& physical : blocked.nodes()) {
    if (const auto* compound = std::get_if<PhysicalCompoundNode>(&physical.payload);
        compound != nullptr) {
      blocked_strategies.push_back(compound->strategy);
    }
  }
  EXPECT_EQ((std::vector{
                PhysicalCompoundStrategy::kEphemeralSet,
                PhysicalCompoundStrategy::kEphemeralSet,
            }),
            blocked_strategies);

  const PhysicalPlan resumed =
      OptimizeOrThrow("SELECT 2 EXCEPT SELECT 1 UNION SELECT 3 LIMIT 1", catalog);
  std::vector<PhysicalCompoundStrategy> resumed_strategies;
  for (const PhysicalNode& physical : resumed.nodes()) {
    if (const auto* compound = std::get_if<PhysicalCompoundNode>(&physical.payload);
        compound != nullptr) {
      resumed_strategies.push_back(compound->strategy);
    }
  }
  EXPECT_EQ((std::vector{
                PhysicalCompoundStrategy::kEphemeralSet,
                PhysicalCompoundStrategy::kUnionLimitOne,
            }),
            resumed_strategies);
}

TEST(PhysicalPlan, SelectsArmLocalOrderingAndSetThenOrder) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan mixed = OptimizeOrThrow(
      "SELECT DISTINCT id,abs(id) FROM Items "
      "UNION ALL SELECT id,abs(id+1) FROM Items ORDER BY 1 LIMIT 1",
      catalog);
  const auto mixed_order = std::ranges::find_if(mixed.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(mixed.nodes().end(), mixed_order);
  const auto& mixed_plans = std::get<PhysicalAdvancedOrderNode>(mixed_order->payload).core_plans;
  ASSERT_EQ(2U, mixed_plans.size());
  EXPECT_EQ(PhysicalSortStrategy::kRuntimeLimit, mixed_plans[0].strategy);
  EXPECT_EQ(PhysicalSortStrategy::kRuntimeLimit, mixed_plans[1].strategy);

  const PhysicalPlan ordered_union =
      OptimizeOrThrow("SELECT 3 UNION SELECT 1 ORDER BY 1 LIMIT +1", catalog);
  const auto union_order =
      std::ranges::find_if(ordered_union.nodes(), [](const PhysicalNode& node) {
        return std::holds_alternative<PhysicalAdvancedOrderNode>(node.payload);
      });
  ASSERT_NE(ordered_union.nodes().end(), union_order);
  for (const PhysicalCoreOrderPlan& core :
       std::get<PhysicalAdvancedOrderNode>(union_order->payload).core_plans) {
    EXPECT_EQ(PhysicalSortStrategy::kRuntimeLimit, core.strategy);
  }

  const PhysicalPlan ordered_mixed = OptimizeOrThrow(
      "SELECT id FROM Items EXCEPT SELECT id FROM Items WHERE id=1 "
      "UNION SELECT 99 WHERE 0 ORDER BY 1 LIMIT 1",
      catalog);
  const auto ordered_mixed_order =
      std::ranges::find_if(ordered_mixed.nodes(), [](const PhysicalNode& node) {
        return std::holds_alternative<PhysicalAdvancedOrderNode>(node.payload);
      });
  ASSERT_NE(ordered_mixed.nodes().end(), ordered_mixed_order);
  const auto& ordered_mixed_plans =
      std::get<PhysicalAdvancedOrderNode>(ordered_mixed_order->payload).core_plans;
  ASSERT_EQ(3U, ordered_mixed_plans.size());
  EXPECT_EQ(PhysicalSortStrategy::kExternal, ordered_mixed_plans[0].strategy);
  EXPECT_EQ(PhysicalSortStrategy::kExternal, ordered_mixed_plans[1].strategy);
  EXPECT_EQ(PhysicalSortStrategy::kRuntimeLimit, ordered_mixed_plans[2].strategy);

  const PhysicalPlan collated = OptimizeOrThrow(
      "SELECT Name FROM Items UNION SELECT Name FROM Items "
      "ORDER BY 1 COLLATE binary LIMIT 1",
      catalog);
  const auto set_node = std::ranges::find_if(collated.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalCompoundNode>(node.payload);
  });
  const auto set_order = std::ranges::find_if(collated.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalAdvancedOrderNode>(node.payload);
  });
  ASSERT_NE(collated.nodes().end(), set_node);
  ASSERT_NE(collated.nodes().end(), set_order);
  EXPECT_EQ(PhysicalCompoundStrategy::kSetThenOrder,
            std::get<PhysicalCompoundNode>(set_node->payload).strategy);
  const auto& final_order = std::get<PhysicalAdvancedOrderNode>(set_order->payload);
  EXPECT_TRUE(final_order.set_then_order);
  EXPECT_EQ(PhysicalSortStrategy::kRuntimeLimit, final_order.final_strategy);
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
  EXPECT_FALSE(scan_delete.collect_original_rowids);
}

TEST(PhysicalMutationPlan, CollectsIndexedTableScansBeforeMutation) {
  const CatalogSnapshotPtr catalog = IndexedMutationCatalog();

  PhysicalStatementPlan exact_update_statement =
      OptimizeStatementOrThrow("UPDATE Items SET Name=?1 WHERE id=?2", catalog);
  const auto& exact_update = std::get<PhysicalUpdateMutation>(
      std::get<PhysicalMutationPlan>(exact_update_statement).payload());
  EXPECT_EQ(MutationAccessKind::kRowIdLookup, exact_update.access.kind);
  EXPECT_FALSE(exact_update.collect_original_rowids);

  PhysicalStatementPlan scan_update_statement =
      OptimizeStatementOrThrow("UPDATE Items SET Value=Value+1 WHERE Name>=?1", catalog);
  const auto& scan_update = std::get<PhysicalUpdateMutation>(
      std::get<PhysicalMutationPlan>(scan_update_statement).payload());
  EXPECT_EQ(MutationAccessKind::kTableScan, scan_update.access.kind);
  EXPECT_TRUE(scan_update.collect_original_rowids);

  PhysicalStatementPlan exact_delete_statement =
      OptimizeStatementOrThrow("DELETE FROM Items WHERE id=?1", catalog);
  const auto& exact_delete = std::get<PhysicalDeleteMutation>(
      std::get<PhysicalMutationPlan>(exact_delete_statement).payload());
  EXPECT_EQ(MutationAccessKind::kRowIdLookup, exact_delete.access.kind);
  EXPECT_FALSE(exact_delete.collect_original_rowids);

  PhysicalStatementPlan scan_delete_statement =
      OptimizeStatementOrThrow("DELETE FROM Items WHERE Name>=?1", catalog);
  const auto& scan_delete = std::get<PhysicalDeleteMutation>(
      std::get<PhysicalMutationPlan>(scan_delete_statement).payload());
  EXPECT_EQ(MutationAccessKind::kTableScan, scan_delete.access.kind);
  EXPECT_TRUE(scan_delete.collect_original_rowids);
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

  PhysicalStatementPlan create_index_statement =
      OptimizeStatementOrThrow("CREATE UNIQUE INDEX items_value ON Items(Value DESC)", catalog);
  const auto& create_index = std::get<PhysicalCreateIndexMutation>(
      std::get<PhysicalMutationPlan>(create_index_statement).payload());
  EXPECT_FALSE(create_index.no_op);
  EXPECT_EQ(MutationAtomicity::kStatement, create_index.atomicity);

  const CatalogSnapshotPtr indexed = IndexedMutationCatalog();
  PhysicalStatementPlan no_op_index_statement =
      OptimizeStatementOrThrow("CREATE INDEX IF NOT EXISTS items_name ON Items(nope)", indexed);
  const auto& no_op_index = std::get<PhysicalCreateIndexMutation>(
      std::get<PhysicalMutationPlan>(no_op_index_statement).payload());
  EXPECT_TRUE(no_op_index.no_op);
  EXPECT_EQ(MutationAtomicity::kTransaction, no_op_index.atomicity);

  PhysicalStatementPlan analyze_statement = OptimizeStatementOrThrow("ANALYZE Items", catalog);
  const auto& analyze = std::get<PhysicalAnalyzeMutation>(
      std::get<PhysicalMutationPlan>(analyze_statement).payload());
  EXPECT_TRUE(analyze.creates_stat1);
  EXPECT_EQ(MutationAtomicity::kStatement, analyze.atomicity);

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
  EXPECT_EQ(PhysicalAccessKind::kSingleRow, plan.selected_candidate()->kind);
  EXPECT_EQ(
      (AccessPathCost{.estimated_input_rows = 1, .estimated_output_rows = 1, .work_units = 1}),
      plan.selected_candidate()->cost);
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
  ASSERT_TRUE(plan.selected_candidate_index().has_value());
  EXPECT_EQ(0U, *plan.selected_candidate_index());
  ASSERT_NE(nullptr, plan.selected_candidate());
  EXPECT_EQ(PhysicalAccessKind::kTableScan, plan.selected_candidate()->kind);
  EXPECT_EQ((AccessPathCost{
                .estimated_input_rows = 100, .estimated_output_rows = 100, .work_units = 2400}),
            plan.selected_candidate()->cost);
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
  ASSERT_TRUE(plan.selected_candidate_index().has_value());
  EXPECT_EQ(1U, *plan.selected_candidate_index());
  ASSERT_NE(nullptr, plan.selected_candidate());
  EXPECT_EQ(plan.candidates()[1], *plan.selected_candidate());
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
  EXPECT_EQ(8U, tiny.candidates()[0].cost.work_units);
  EXPECT_EQ(1U, tiny.candidates()[1].cost.work_units);
  EXPECT_EQ(PhysicalAccessKind::kRowIdLookup, tiny.selected_candidate()->kind);
}

TEST(PhysicalPlan, SelectsRowIdRangeBeforeResidualPredicates) {
  const PhysicalPlan plan =
      OptimizeOrThrow("SELECT Name FROM Items WHERE Name=?1 AND id<?2", TestCatalog());
  EXPECT_EQ("SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid<?)", ExplainPhysicalPlan(plan));
}

TEST(PhysicalPlan, NormalizesAndPublishesRowIdRangeBounds) {
  struct Case {
    std::string_view sql;
    bool lower;
    bool inclusive;
    std::string_view explain;
  };
  constexpr std::array cases{
      Case{.sql = "SELECT Name FROM Items WHERE id>?1",
           .lower = true,
           .inclusive = false,
           .explain = "SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid>?)"},
      Case{.sql = "SELECT Name FROM Items WHERE ?1<id",
           .lower = true,
           .inclusive = false,
           .explain = "SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid>?)"},
      Case{.sql = "SELECT Name FROM Items WHERE id>=?1",
           .lower = true,
           .inclusive = true,
           .explain = "SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid>=?)"},
      Case{.sql = "SELECT Name FROM Items WHERE ?1<=id",
           .lower = true,
           .inclusive = true,
           .explain = "SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid>=?)"},
      Case{.sql = "SELECT Name FROM Items WHERE id<?1",
           .lower = false,
           .inclusive = false,
           .explain = "SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid<?)"},
      Case{.sql = "SELECT Name FROM Items WHERE ?1>id",
           .lower = false,
           .inclusive = false,
           .explain = "SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid<?)"},
      Case{.sql = "SELECT Name FROM Items WHERE id<=?1",
           .lower = false,
           .inclusive = true,
           .explain = "SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid<=?)"},
      Case{.sql = "SELECT Name FROM Items WHERE ?1>=id",
           .lower = false,
           .inclusive = true,
           .explain = "SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid<=?)"},
  };
  for (const Case& test : cases) {
    SCOPED_TRACE(test.sql);
    const PhysicalPlan plan = OptimizeOrThrow(test.sql, TestCatalog());
    const auto& range = std::get<PhysicalRowIdRangeNode>(plan.nodes()[0].payload);
    const std::optional<PhysicalRangeBound>& bound = test.lower ? range.lower : range.upper;
    ASSERT_TRUE(bound.has_value());
    EXPECT_EQ(test.inclusive, bound->inclusive);
    EXPECT_EQ(test.lower, range.lower.has_value());
    EXPECT_EQ(!test.lower, range.upper.has_value());
    ASSERT_NE(nullptr, plan.selected_candidate());
    EXPECT_EQ(PhysicalAccessKind::kRowIdRange, plan.selected_candidate()->kind);
    EXPECT_EQ(1U, plan.selected_candidate()->range_bound_count);
    EXPECT_EQ(test.explain, ExplainPhysicalPlan(plan));
    ASSERT_EQ(1U, plan.core_access_plans().size());
    EXPECT_EQ(plan.selected_candidate_index(),
              std::optional{plan.core_access_plans()[0].selected_candidate});
  }

  const PhysicalPlan bounded =
      OptimizeOrThrow("SELECT Name FROM Items WHERE id>?1 AND id<=?2", TestCatalog());
  const auto& range = std::get<PhysicalRowIdRangeNode>(bounded.nodes()[0].payload);
  ASSERT_TRUE(range.lower.has_value());
  ASSERT_TRUE(range.upper.has_value());
  EXPECT_FALSE(range.lower->inclusive);
  EXPECT_TRUE(range.upper->inclusive);
  EXPECT_EQ(2U, bounded.selected_candidate()->range_bound_count);
  EXPECT_LT(bounded.selected_candidate()->cost.estimated_output_rows,
            bounded.candidates()[0].cost.estimated_output_rows);
  EXPECT_LT(bounded.selected_candidate()->cost.work_units, bounded.candidates()[0].cost.work_units);
  EXPECT_EQ("SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid>? AND rowid<=?)",
            ExplainPhysicalPlan(bounded));

  const PhysicalPlan zero_estimate =
      OptimizeOrThrow("SELECT Name FROM Items WHERE id<1", TestCatalog("Items", 0));
  ASSERT_EQ(2U, zero_estimate.candidates().size());
  EXPECT_EQ(AccessPathCost{}, zero_estimate.candidates()[0].cost);
  EXPECT_EQ(AccessPathCost{}, zero_estimate.candidates()[1].cost);
  ASSERT_NE(nullptr, zero_estimate.selected_candidate());
  EXPECT_EQ(PhysicalAccessKind::kRowIdRange, zero_estimate.selected_candidate()->kind);
}

TEST(PhysicalPlan, KeepsLaterRowIdBoundsResidualAndRejectsAccessBarriers) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan repeated =
      OptimizeOrThrow("SELECT Name FROM Items WHERE id>?1 AND id>=?2", catalog);
  const auto& range = std::get<PhysicalRowIdRangeNode>(repeated.nodes()[0].payload);
  ASSERT_TRUE(range.lower.has_value());
  const auto& first = std::get<BoundParameterExpression>(
      repeated.logical_plan().bound_select().expression(range.lower->key).payload);
  EXPECT_EQ(0U, first.parameter.value());
  const auto& filter = std::get<PhysicalFilterNode>(repeated.nodes()[1].payload);
  ASSERT_EQ(1U, filter.predicates.size());
  const auto& residual = std::get<BoundComparisonExpression>(
      repeated.logical_plan().bound_select().expression(filter.predicates[0]).payload);
  const auto& second = std::get<BoundParameterExpression>(
      repeated.logical_plan().bound_select().expression(residual.right).payload);
  EXPECT_EQ(1U, second.parameter.value());

  constexpr std::array<std::string_view, 3> barriers{
      "SELECT Name FROM Items WHERE +id>?1",
      "SELECT Name FROM Items WHERE id>Name",
      "SELECT key FROM wr WHERE key>'a'",
  };
  for (const std::string_view sql : barriers) {
    SCOPED_TRACE(sql);
    const PhysicalPlan plan = OptimizeOrThrow(sql, catalog);
    EXPECT_FALSE(std::holds_alternative<PhysicalRowIdRangeNode>(plan.nodes()[0].payload));
  }
}

TEST(PhysicalPlan, PublishesRowIdRangesForEveryNonjoinedCoreAndAggregateDirection) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan compound = OptimizeOrThrow(
      "SELECT id FROM Items WHERE id<2 UNION ALL SELECT id FROM Items WHERE id>=2", catalog);
  ASSERT_EQ(2U, compound.core_access_plans().size());
  for (std::size_t core = 0; core < compound.core_access_plans().size(); ++core) {
    EXPECT_EQ(core, compound.core_access_plans()[core].core_index);
    const auto& selected = compound.core_access_plans()[core];
    ASSERT_LT(selected.selected_candidate, selected.candidates.size());
    EXPECT_EQ(PhysicalAccessKind::kRowIdRange,
              selected.candidates[selected.selected_candidate].kind);
  }
  EXPECT_TRUE(
      std::ranges::equal(compound.core_access_plans()[0].candidates, compound.candidates()));

  const PhysicalPlan maximum = OptimizeOrThrow("SELECT max(id) FROM Items WHERE id<3", catalog);
  const auto& reverse = std::get<PhysicalRowIdRangeNode>(maximum.nodes()[0].payload);
  EXPECT_EQ(PhysicalScanDirection::kReverse, reverse.direction);
  EXPECT_EQ(
      "SEARCH \"Items\" USING INTEGER PRIMARY KEY (rowid<?) REVERSE\n"
      "AGGREGATE CORE 0 INDEXED MIN/MAX",
      ExplainPhysicalPlan(maximum));

  const PhysicalPlan grouped = OptimizeOrThrow(
      "SELECT id,count() FROM Items "
      "WHERE (NULL OR abs(id-id-9223372036854775807-1)) AND id<0 "
      "GROUP BY id ORDER BY id",
      catalog);
  EXPECT_TRUE(std::holds_alternative<PhysicalRowIdRangeNode>(grouped.nodes()[0].payload));
  EXPECT_EQ(PhysicalAggregateStrategy::kOrderedGroup, AggregateNode(grouped).strategy);

  const PhysicalPlan schema =
      OptimizeOrThrow("SELECT name FROM main.sqlite_schema WHERE rowid<3", catalog);
  EXPECT_TRUE(std::holds_alternative<PhysicalRowIdRangeNode>(schema.nodes()[0].payload));
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

TEST(PhysicalPlan, MapsOrderedLogicalMetadataAndRuntimeLimitStrategy) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT Name, abs(?) FROM Items WHERE id > ? "
      "ORDER BY Name DESC NULLS FIRST LIMIT ? OFFSET ?",
      catalog);

  ASSERT_EQ(4U, plan.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalRowIdRangeNode>(plan.nodes()[0].payload));

  const auto& sort = std::get<PhysicalSortNode>(plan.nodes()[1].payload);
  EXPECT_EQ(PhysicalNodeId{0}, sort.input);
  EXPECT_EQ(OrderEvaluationSchedule::kKeysThenAdmissionThenPayload, sort.schedule);
  EXPECT_EQ(PhysicalSortStrategy::kRuntimeLimit, sort.strategy);
  EXPECT_FALSE(sort.input_order_satisfied);
  ASSERT_EQ(1U, sort.terms.size());
  EXPECT_EQ(SortOrder::kDescending, sort.terms[0].order);
  EXPECT_EQ(BoundNullPlacement::kFirst, sort.terms[0].null_placement);
  ASSERT_EQ(1U, sort.payload_expressions.size());
  ASSERT_EQ(2U, sort.output_fields.size());
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kKey, .field_index = 0}),
            sort.output_fields[0]);
  EXPECT_EQ((SortOutputField{.kind = SortOutputFieldKind::kPayload, .field_index = 0}),
            sort.output_fields[1]);

  const auto& logical_order = std::get<LogicalOrderNode>(plan.logical_plan().nodes()[2].payload);
  EXPECT_EQ(logical_order.terms[0].expression, sort.terms[0].expression);
  EXPECT_EQ(logical_order.payload_expressions, sort.payload_expressions);
  EXPECT_EQ(logical_order.output_fields, sort.output_fields);

  const auto& limit = std::get<PhysicalLimitNode>(plan.nodes()[2].payload);
  EXPECT_EQ(PhysicalNodeId{1}, limit.input);
  const auto& output = std::get<PhysicalOutputNode>(plan.nodes()[3].payload);
  EXPECT_EQ(PhysicalNodeId{2}, output.input);
  EXPECT_EQ(plan.logical_plan().root(), output.logical_output);
}

TEST(PhysicalPlan, KeepsSorterForMatchingIndexOrderAndExplainsTerms) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT Category, Score FROM Items WHERE Category=?1 "
      "ORDER BY Score DESC LIMIT ?2",
      catalog);

  EXPECT_EQ(PhysicalAccessKind::kIndexScan, plan.selected_candidate()->kind);
  ASSERT_EQ(4U, plan.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalIndexScanNode>(plan.nodes()[0].payload));
  const auto& sort = std::get<PhysicalSortNode>(plan.nodes()[1].payload);
  EXPECT_EQ(PhysicalNodeId{0}, sort.input);
  EXPECT_EQ(PhysicalSortStrategy::kRuntimeLimit, sort.strategy);
  EXPECT_FALSE(sort.input_order_satisfied);
  EXPECT_EQ(
      "SEARCH \"Items\" USING COVERING INDEX \"items_category_score\" (\"Category\"=?)\n"
      "SORT 1 TERM (COLLATE \"BINARY\" DESC NULLS LAST) USING SORTER",
      ExplainPhysicalPlan(plan));
}

TEST(PhysicalPlan, UsesExternalStrategyWithoutSyntacticLimit) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT Name, id FROM Items "
      "ORDER BY Name COLLATE NOCASE DESC NULLS FIRST, id",
      catalog);

  ASSERT_EQ(3U, plan.nodes().size());
  const auto& sort = std::get<PhysicalSortNode>(plan.nodes()[1].payload);
  EXPECT_EQ(PhysicalSortStrategy::kExternal, sort.strategy);
  EXPECT_EQ(OrderEvaluationSchedule::kPayloadThenKeys, sort.schedule);
  EXPECT_FALSE(sort.input_order_satisfied);
  EXPECT_TRUE(std::holds_alternative<PhysicalOutputNode>(plan.nodes()[2].payload));
  EXPECT_EQ(
      "SCAN \"Items\"\n"
      "SORT 2 TERMS (COLLATE \"NOCASE\" DESC NULLS FIRST, "
      "COLLATE \"BINARY\" ASC NULLS FIRST) USING SORTER",
      ExplainPhysicalPlan(plan));
}

TEST(PhysicalPlan, IncludesOrderingExpressionsInCoveringIndexRequirements) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();
  const PhysicalPlan plan =
      OptimizeOrThrow("SELECT id,Score FROM Items ORDER BY Category,id", catalog);

  ASSERT_EQ(4U, plan.candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, plan.selected_candidate()->kind);
  EXPECT_EQ(IndexId{0}, plan.selected_candidate()->index);
  EXPECT_TRUE(plan.selected_candidate()->covering);
  EXPECT_EQ(
      "SCAN \"Items\" USING COVERING INDEX \"items_category_score\"\n"
      "SORT 2 TERMS (COLLATE \"NOCASE\" ASC NULLS FIRST, "
      "COLLATE \"BINARY\" ASC NULLS FIRST) USING SORTER",
      ExplainPhysicalPlan(plan));
}

TEST(PhysicalPlan, SelectsCoveringIndexRangesWithStat1Costs) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();
  const PhysicalPlan plan = OptimizeOrThrow(
      "SELECT Category, Score FROM Items "
      "WHERE Category=?1 AND Score>=?2 AND Score<?3",
      catalog);

  ASSERT_EQ(4U, plan.candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kTableScan, plan.candidates()[0].kind);
  EXPECT_EQ((AccessPathCost{
                .estimated_input_rows = 4096,
                .estimated_output_rows = 4096,
                .work_units = 2'097'152,
            }),
            plan.candidates()[0].cost);
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, plan.candidates()[1].kind);
  EXPECT_EQ(IndexId{0}, plan.candidates()[1].index);
  EXPECT_TRUE(plan.candidates()[1].covering);
  EXPECT_EQ(1U, plan.candidates()[1].equality_term_count);
  EXPECT_EQ(2U, plan.candidates()[1].range_bound_count);
  EXPECT_EQ((AccessPathCost{
                .estimated_input_rows = 4096,
                .estimated_output_rows = 1,
                .work_units = 41,
            }),
            plan.candidates()[1].cost);
  ASSERT_TRUE(plan.selected_candidate_index().has_value());
  EXPECT_EQ(1U, *plan.selected_candidate_index());

  const auto& index = std::get<PhysicalIndexScanNode>(plan.nodes()[0].payload);
  EXPECT_EQ(TableId{0}, index.table);
  EXPECT_EQ(RootPageId{2}, index.table_root_page);
  EXPECT_EQ(IndexId{0}, index.index);
  EXPECT_EQ(RootPageId{10}, index.index_root_page);
  EXPECT_TRUE(index.covering);
  ASSERT_EQ(1U, index.equalities.size());
  EXPECT_EQ(ColumnId{1}, index.equalities[0].column);
  EXPECT_TRUE(index.equalities[0].reject_null);
  EXPECT_EQ(SortOrder::kAscending, index.equalities[0].order);
  ASSERT_TRUE(index.range.has_value());
  EXPECT_EQ(ColumnId{2}, index.range->column);
  EXPECT_EQ(SortOrder::kDescending, index.range->order);
  ASSERT_TRUE(index.range->lower.has_value());
  EXPECT_TRUE(index.range->lower->inclusive);
  ASSERT_TRUE(index.range->upper.has_value());
  EXPECT_FALSE(index.range->upper->inclusive);
  EXPECT_FALSE(std::ranges::any_of(plan.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalFilterNode>(node.payload);
  }));
  EXPECT_EQ(
      "SEARCH \"Items\" USING COVERING INDEX \"items_category_score\" "
      "(\"Category\"=? AND \"Score\">=? AND \"Score\"<?)",
      ExplainPhysicalPlan(plan));

  const PhysicalPlan reversed =
      OptimizeOrThrow("SELECT id, Score FROM Items WHERE ?1<Score AND ?2>=Score", catalog);
  const auto& reversed_index = std::get<PhysicalIndexScanNode>(reversed.nodes()[0].payload);
  ASSERT_TRUE(reversed_index.range.has_value());
  ASSERT_TRUE(reversed_index.range->lower.has_value());
  EXPECT_FALSE(reversed_index.range->lower->inclusive);
  ASSERT_TRUE(reversed_index.range->upper.has_value());
  EXPECT_TRUE(reversed_index.range->upper->inclusive);
  EXPECT_EQ(
      "SEARCH \"Items\" USING COVERING INDEX \"items_score_desc\" "
      "(\"Score\">? AND \"Score\"<=?)",
      ExplainPhysicalPlan(reversed));
}

TEST(PhysicalPlan, CostsCoveringAndNoncoveringIndexes) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();

  const PhysicalPlan full = OptimizeOrThrow("SELECT Category, Score FROM Items", catalog);
  ASSERT_EQ(4U, full.candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, full.selected_candidate()->kind);
  EXPECT_EQ(IndexId{0}, full.selected_candidate()->index);
  EXPECT_EQ((AccessPathCost{
                .estimated_input_rows = 4096,
                .estimated_output_rows = 4096,
                .work_units = 114'688,
            }),
            full.selected_candidate()->cost);
  EXPECT_EQ("SCAN \"Items\" USING COVERING INDEX \"items_category_score\"",
            ExplainPhysicalPlan(full));

  const PhysicalPlan unique = OptimizeOrThrow("SELECT Code FROM Items WHERE Code IS ?1", catalog);
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, unique.selected_candidate()->kind);
  EXPECT_EQ(IndexId{3}, unique.selected_candidate()->index);
  EXPECT_EQ(1U, unique.selected_candidate()->cost.estimated_output_rows);
  EXPECT_EQ(37U, unique.selected_candidate()->cost.work_units);
  const auto& unique_node = std::get<PhysicalIndexScanNode>(unique.nodes()[0].payload);
  ASSERT_EQ(1U, unique_node.equalities.size());
  EXPECT_FALSE(unique_node.equalities[0].reject_null);
  EXPECT_EQ("SEARCH \"Items\" USING COVERING INDEX \"items_code_unique\" (\"Code\" IS ?)",
            ExplainPhysicalPlan(unique));

  const PhysicalPlan unordered_full = OptimizeOrThrow("SELECT Code FROM Items", catalog);
  EXPECT_EQ(PhysicalAccessKind::kTableScan, unordered_full.selected_candidate()->kind);

  const PhysicalPlan unselective = OptimizeOrThrow("SELECT id FROM Items WHERE Flag=?1", catalog);
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, unselective.selected_candidate()->kind);
  EXPECT_EQ(IndexId{2}, unselective.selected_candidate()->index);
  EXPECT_EQ(4096U, unselective.selected_candidate()->cost.estimated_output_rows);
  EXPECT_EQ(32'781U, unselective.selected_candidate()->cost.work_units);

  const PhysicalPlan selective_noncovering =
      OptimizeOrThrow("SELECT Payload FROM Items WHERE Category=?1", catalog);
  ASSERT_EQ(4U, selective_noncovering.candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, selective_noncovering.selected_candidate()->kind);
  EXPECT_EQ(IndexId{0}, selective_noncovering.selected_candidate()->index);
  EXPECT_FALSE(selective_noncovering.selected_candidate()->covering);
  EXPECT_EQ((AccessPathCost{
                .estimated_input_rows = 4096,
                .estimated_output_rows = 16,
                .work_units = 8861,
            }),
            selective_noncovering.selected_candidate()->cost);
  const auto& selective_node =
      std::get<PhysicalIndexScanNode>(selective_noncovering.nodes()[0].payload);
  EXPECT_FALSE(selective_node.covering);
  EXPECT_EQ("SEARCH \"Items\" USING INDEX \"items_category_score\" (\"Category\"=?)",
            ExplainPhysicalPlan(selective_noncovering));

  const PhysicalPlan unselective_noncovering =
      OptimizeOrThrow("SELECT Payload FROM Items WHERE Flag=?1", catalog);
  ASSERT_EQ(4U, unselective_noncovering.candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kTableScan, unselective_noncovering.selected_candidate()->kind);
  const auto flag_index = std::ranges::find(unselective_noncovering.candidates(),
                                            std::optional{IndexId{2}}, &AccessPathCandidate::index);
  ASSERT_NE(unselective_noncovering.candidates().end(), flag_index);
  EXPECT_FALSE(flag_index->covering);
}

TEST(PhysicalPlan, PreservesCandidateOrderAndRowidPreference) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();
  const PhysicalPlan plan =
      OptimizeOrThrow("SELECT id, Score FROM Items WHERE id=?1 AND Category=?2", catalog);

  ASSERT_EQ(5U, plan.candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kTableScan, plan.candidates()[0].kind);
  EXPECT_EQ(PhysicalAccessKind::kRowIdLookup, plan.candidates()[1].kind);
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, plan.candidates()[2].kind);
  EXPECT_EQ(IndexId{0}, plan.candidates()[2].index);
  ASSERT_TRUE(plan.selected_candidate_index().has_value());
  EXPECT_EQ(1U, *plan.selected_candidate_index());
  EXPECT_EQ(PhysicalAccessKind::kRowIdLookup, plan.selected_candidate()->kind);
}

TEST(PhysicalPlan, AppliesDefaultNullAndRangeEstimates) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();

  const PhysicalPlan collated_null =
      OptimizeOrThrow("SELECT id, Category FROM Items WHERE Category IS NULL", catalog);
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, collated_null.selected_candidate()->kind);
  EXPECT_EQ(IndexId{0}, collated_null.selected_candidate()->index);
  EXPECT_EQ(16U, collated_null.selected_candidate()->cost.estimated_output_rows);

  const PhysicalPlan null_equality =
      OptimizeOrThrow("SELECT id, Score FROM Items WHERE Score IS NULL", catalog);
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, null_equality.selected_candidate()->kind);
  EXPECT_EQ(IndexId{1}, null_equality.selected_candidate()->index);
  EXPECT_EQ(20U, null_equality.selected_candidate()->cost.estimated_output_rows);
  EXPECT_EQ(173U, null_equality.selected_candidate()->cost.work_units);

  const PhysicalPlan two_sided_range =
      OptimizeOrThrow("SELECT id, Score FROM Items WHERE Score>=?1 AND Score<?2", catalog);
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, two_sided_range.selected_candidate()->kind);
  EXPECT_EQ(IndexId{1}, two_sided_range.selected_candidate()->index);
  EXPECT_EQ(64U, two_sided_range.selected_candidate()->cost.estimated_output_rows);
  EXPECT_EQ(525U, two_sided_range.selected_candidate()->cost.work_units);
}

TEST(PhysicalPlan, KeepsIneligibleConstraintsAsCoveringScanResiduals) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();
  const PhysicalPlan alternate =
      OptimizeOrThrow("SELECT Category, Score FROM Items WHERE Score=?1", catalog);
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, alternate.selected_candidate()->kind);
  EXPECT_EQ(IndexId{1}, alternate.selected_candidate()->index);
  EXPECT_FALSE(alternate.selected_candidate()->covering);
  EXPECT_EQ(1U, alternate.selected_candidate()->equality_term_count);
  EXPECT_FALSE(std::ranges::any_of(alternate.nodes(), [](const PhysicalNode& node) {
    return std::holds_alternative<PhysicalFilterNode>(node.payload);
  }));
  EXPECT_EQ("SEARCH \"Items\" USING INDEX \"items_score_desc\" (\"Score\"=?)",
            ExplainPhysicalPlan(alternate));

  const std::array<std::string_view, 2> queries{
      "SELECT Category, Score FROM Items WHERE Category COLLATE BINARY=?1",
      "SELECT Category, Score FROM Items WHERE +Category=?1",
  };
  for (const std::string_view sql : queries) {
    SCOPED_TRACE(sql);
    const PhysicalPlan plan = OptimizeOrThrow(sql, catalog);
    EXPECT_EQ(PhysicalAccessKind::kIndexScan, plan.selected_candidate()->kind);
    EXPECT_EQ(IndexId{0}, plan.selected_candidate()->index);
    EXPECT_EQ(0U, plan.selected_candidate()->equality_term_count);
    EXPECT_EQ(0U, plan.selected_candidate()->range_bound_count);
    EXPECT_TRUE(std::holds_alternative<PhysicalFilterNode>(plan.nodes()[1].payload));
    EXPECT_EQ("SCAN \"Items\" USING COVERING INDEX \"items_category_score\"",
              ExplainPhysicalPlan(plan));
  }
}

TEST(PhysicalPlan, FollowsTransparentIndexPredicateWrappers) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();
  const std::array<std::string_view, 3> queries{
      "SELECT Category FROM Items WHERE likely(Category=?1)",
      "SELECT Category FROM Items WHERE (Category=?1) COLLATE NOCASE",
      "SELECT Category AS selected FROM Items WHERE selected=?1",
  };
  for (const std::string_view sql : queries) {
    SCOPED_TRACE(sql);
    const PhysicalPlan plan = OptimizeOrThrow(sql, catalog);
    EXPECT_EQ(PhysicalAccessKind::kIndexScan, plan.selected_candidate()->kind);
    EXPECT_EQ(IndexId{0}, plan.selected_candidate()->index);
    EXPECT_EQ(1U, plan.selected_candidate()->equality_term_count);
    EXPECT_FALSE(std::ranges::any_of(plan.nodes(), [](const PhysicalNode& node) {
      return std::holds_alternative<PhysicalFilterNode>(node.payload);
    }));
  }
}

TEST(PhysicalPlan, SelectsOnlyIndexesWhoseCollationsAreRegistered) {
  const CatalogSnapshotPtr catalog = CustomCollationCatalog();

  const PhysicalPlan unavailable = OptimizeOrThrow("SELECT id, Value FROM Custom", catalog);
  ASSERT_EQ(1U, unavailable.candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kTableScan, unavailable.selected_candidate()->kind);

  const PhysicalPlan full =
      OptimizeOrThrow("SELECT id, Value FROM Custom", catalog, CustomCollationEnvironment());
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, full.selected_candidate()->kind);
  EXPECT_EQ(IndexId{0}, full.selected_candidate()->index);

  const PhysicalPlan constrained = OptimizeOrThrow("SELECT id FROM Custom WHERE Value=?1", catalog,
                                                   CustomCollationEnvironment());
  EXPECT_EQ(PhysicalAccessKind::kIndexScan, constrained.selected_candidate()->kind);
  EXPECT_EQ(27U, constrained.selected_candidate()->cost.work_units);
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
  EXPECT_EQ(PhysicalAccessKind::kEmpty, guarded_empty.selected_candidate()->kind);
  EXPECT_EQ((AccessPathCost{}), guarded_empty.selected_candidate()->cost);
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
            schema.selected_candidate()->cost);
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
