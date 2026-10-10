#include "modern_sqlite/optimizer/physical_plan.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
  static const FunctionRegistry registry{functions};
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

TEST(PhysicalPlan, ChoosesConservativeAdvancedAccessAndMembership) {
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
  EXPECT_TRUE(std::holds_alternative<PhysicalTableScanNode>(distinct.nodes()[0].payload));
  EXPECT_TRUE(std::holds_alternative<PhysicalFilterNode>(distinct.nodes()[1].payload));
  EXPECT_TRUE(std::holds_alternative<PhysicalProjectionNode>(distinct.nodes()[2].payload));
  const auto& node = std::get<PhysicalDistinctNode>(distinct.nodes()[3].payload);
  EXPECT_EQ(PhysicalDistinctStrategy::kEphemeralMembership, node.strategy);
  EXPECT_TRUE(distinct.candidates().empty());
  EXPECT_FALSE(distinct.selected_candidate_index().has_value());
  EXPECT_EQ(nullptr, distinct.selected_candidate());
  EXPECT_EQ(
      "CORE 0 SCAN \"Items\"\n"
      "DISTINCT CORE 0 USING EPHEMERAL RELATION",
      ExplainPhysicalPlan(distinct));

  const PhysicalPlan guarded = OptimizeOrThrow(
      "SELECT DISTINCT Name FROM Items WHERE id<0 AND abs(-9223372036854775808)", catalog);
  const auto filter = std::ranges::find_if(guarded.nodes(), [](const PhysicalNode& physical) {
    return std::holds_alternative<PhysicalFilterNode>(physical.payload);
  });
  ASSERT_NE(guarded.nodes().end(), filter);
  const auto& guarded_filter = std::get<PhysicalFilterNode>(filter->payload);
  ASSERT_EQ(1U, guarded_filter.predicates.size());
  ASSERT_EQ(1U, guarded_filter.guards.size());
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

  ASSERT_EQ(5U, plan.nodes().size());
  EXPECT_TRUE(std::holds_alternative<PhysicalTableScanNode>(plan.nodes()[0].payload));
  EXPECT_TRUE(std::holds_alternative<PhysicalFilterNode>(plan.nodes()[1].payload));

  const auto& sort = std::get<PhysicalSortNode>(plan.nodes()[2].payload);
  EXPECT_EQ(PhysicalNodeId{1}, sort.input);
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

  const auto& limit = std::get<PhysicalLimitNode>(plan.nodes()[3].payload);
  EXPECT_EQ(PhysicalNodeId{2}, limit.input);
  const auto& output = std::get<PhysicalOutputNode>(plan.nodes()[4].payload);
  EXPECT_EQ(PhysicalNodeId{3}, output.input);
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

  ASSERT_EQ(2U, plan.candidates().size());
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

  ASSERT_EQ(3U, plan.candidates().size());
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
  ASSERT_EQ(2U, full.candidates().size());
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
  ASSERT_EQ(2U, selective_noncovering.candidates().size());
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
  ASSERT_EQ(2U, unselective_noncovering.candidates().size());
  EXPECT_EQ(PhysicalAccessKind::kTableScan, unselective_noncovering.selected_candidate()->kind);
  EXPECT_EQ(IndexId{2}, unselective_noncovering.candidates()[1].index);
  EXPECT_FALSE(unselective_noncovering.candidates()[1].covering);
}

TEST(PhysicalPlan, PreservesCandidateOrderAndRowidPreference) {
  const CatalogSnapshotPtr catalog = IndexedCatalog();
  const PhysicalPlan plan =
      OptimizeOrThrow("SELECT id, Score FROM Items WHERE id=?1 AND Category=?2", catalog);

  ASSERT_EQ(3U, plan.candidates().size());
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
