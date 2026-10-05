#include "modern_sqlite/binder/bound_select.hpp"

#include <gtest/gtest.h>

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

static_assert(!std::is_copy_constructible_v<BoundSelect>);
static_assert(!std::is_copy_assignable_v<BoundSelect>);
static_assert(std::is_nothrow_move_constructible_v<BoundSelect>);
static_assert(std::is_nothrow_move_assignable_v<BoundSelect>);

const BindEnvironment kStaticInitializationEnvironment = BindEnvironment::Core();

[[nodiscard]] SourceSpan Span(std::size_t begin, std::size_t end) {
  const auto span = SourceSpan::FromBounds(ByteOffset{begin}, ByteOffset{end});
  if (!span.has_value()) {
    throw std::logic_error{"invalid binder test source span"};
  }
  return *span;
}

[[nodiscard]] SyntaxTree ParseTree(std::string_view sql) {
  ParseResult parsed = ParseOne(Utf8View{sql});
  if (!parsed.has_value()) {
    throw std::runtime_error{std::string{ParseErrorMessage(parsed.error())}};
  }
  if (!parsed->tree.has_value()) {
    throw std::runtime_error{"binder test SQL did not produce a statement"};
  }
  return std::move(*parsed->tree);
}

[[nodiscard]] CatalogIndexTerm ColumnTerm(std::size_t column, std::string collation = "BINARY") {
  return CatalogIndexTerm{
      .target = ColumnId{column},
      .collation_name = std::move(collation),
      .order = SortOrder::kAscending,
  };
}

[[nodiscard]] CatalogSnapshotPtr TestCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 17, .generation = 9},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Items("
                "id INTEGER PRIMARY KEY, "
                "Name TEXT COLLATE NOCASE, "
                "Score REAL, "
                "rowid TEXT, "
                "TRUE INTEGER"
                ")"));
  input.definitions.push_back(
      ParseTree("CREATE TABLE CustomCollation(Value TEXT COLLATE missing)"));
  input.definitions.push_back(ParseTree("CREATE TABLE wr(key TEXT PRIMARY KEY) WITHOUT ROWID"));
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
              CatalogColumnInput{
                  .name = "Score",
                  .declared_type = "REAL",
              },
              CatalogColumnInput{
                  .name = "rowid",
                  .declared_type = "TEXT",
              },
              CatalogColumnInput{
                  .name = "TRUE",
                  .declared_type = "INTEGER",
              },
          },
      .rowid_alias = ColumnId{0},
  });
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{1},
      .name = "CustomCollation",
      .root_page = RootPageId{3},
      .columns =
          {
              CatalogColumnInput{
                  .name = "Value",
                  .declared_type = "TEXT",
                  .collation_name = "missing",
              },
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

[[nodiscard]] BoundSelect BindOrThrow(std::string_view sql, const CatalogSnapshotPtr& catalog,
                                      BindEnvironment environment = BindEnvironment::Core(),
                                      BindOptions options = {}) {
  BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog, environment, options);
  if (!bound.has_value()) {
    throw std::runtime_error{bound.error().detail};
  }
  return std::move(*bound);
}

void ExpectBindError(std::string_view sql, const CatalogSnapshotPtr& catalog, BindErrorCode code,
                     std::string_view detail, BindEnvironment environment = BindEnvironment::Core(),
                     BindOptions options = {}) {
  const BindSelectResult bound = BindSelectStatement(ParseTree(sql), catalog, environment, options);
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(code, bound.error().code);
  EXPECT_EQ(detail, bound.error().detail);
}

[[nodiscard]] const BoundExpression& ResultExpression(const BoundSelect& select,
                                                      std::size_t index) {
  return select.expression(select.result_columns()[index].expression);
}

template <typename T>
[[nodiscard]] T RequiredOptional(std::optional<T> value) {
  if (!value.has_value()) {
    throw std::runtime_error{"binder test expected an optional value"};
  }
  return std::move(*value);
}

[[nodiscard]] std::string_view CollationName(const BoundSelect& select, BoundCollationId id) {
  return select.collations()[id.value()].name;
}

[[nodiscard]] std::string_view FunctionName(const BoundSelect& select, BoundFunctionId id) {
  return select.functions()[id.value()].name;
}

[[nodiscard]] Result<SqlValue> StubScalar(const ScalarFunctionContext&, std::span<const SqlValue>) {
  return SqlValue{};
}

TEST(BinderApi, ExposesStableKindsErrorsAndOwnership) {
  EXPECT_EQ("literal", BoundExpressionKindName(BoundExpressionKind::kLiteral));
  EXPECT_EQ("truth_test", BoundExpressionKindName(BoundExpressionKind::kTruthTest));
  EXPECT_EQ("unknown",
            BoundExpressionKindName(static_cast<BoundExpressionKind>(255)));  // NOLINT

  EXPECT_EQ("no_such_table", BindErrorCodeName(BindErrorCode::kNoSuchTable));
  EXPECT_EQ("invalid_variable_number", BindErrorCodeName(BindErrorCode::kInvalidVariableNumber));
  EXPECT_EQ("unknown", BindErrorCodeName(static_cast<BindErrorCode>(255)));  // NOLINT

  EXPECT_EQ(ErrorCode::kGeneric, BindError{.code = BindErrorCode::kNoSuchColumn}.base_error_code());
  EXPECT_EQ(ErrorCode::kTooLarge,
            BindError{.code = BindErrorCode::kResultColumnLimitExceeded}.base_error_code());
  EXPECT_EQ(ErrorCode::kMisuse, BindError{.code = BindErrorCode::kInvalidInput}.base_error_code());
  EXPECT_EQ(0U, BindEnvironment::Core().registration_generation());
}

TEST(Binder, ExpandsWildcardsAndPublishesDirectColumnMetadata) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  BoundSelect select =
      BindOrThrow("SELECT *, q._rowid_ AS hidden, (q.Name) AS n, q.rowid FROM Items AS q", catalog);

  ASSERT_TRUE(select.valid());
  EXPECT_EQ("SELECT *, q._rowid_ AS hidden, (q.Name) AS n, q.rowid FROM Items AS q",
            select.source().bytes());
  ASSERT_EQ(catalog.get(), select.catalog());
  ASSERT_NE(nullptr, select.table_source());
  EXPECT_EQ(BoundSourceKind::kCatalogTable, select.table_source()->kind);
  EXPECT_EQ(TableId{0}, select.table_source()->table);

  ASSERT_EQ(5U, select.source_columns().size());
  EXPECT_EQ("Name", select.source_columns()[1].name);
  EXPECT_EQ(TypeAffinity::kText, select.source_columns()[1].affinity);
  EXPECT_EQ("NOCASE", select.source_columns()[1].collation_name);

  ASSERT_EQ(8U, select.result_columns().size());
  EXPECT_EQ("id", select.result_columns()[0].name);
  EXPECT_EQ(std::optional<std::string>{"INTEGER"}, select.result_columns()[0].declared_type);
  EXPECT_EQ("Name", select.result_columns()[1].name);
  EXPECT_EQ("Score", select.result_columns()[2].name);
  EXPECT_EQ("rowid", select.result_columns()[3].name);
  EXPECT_EQ("TRUE", select.result_columns()[4].name);

  EXPECT_EQ("hidden", select.result_columns()[5].name);
  EXPECT_EQ(std::optional<std::string>{"INTEGER"}, select.result_columns()[5].declared_type);
  EXPECT_EQ(TypeAffinity::kInteger, select.result_columns()[5].affinity);
  EXPECT_TRUE(std::holds_alternative<BoundRowIdExpression>(ResultExpression(select, 5).payload));

  EXPECT_EQ("n", select.result_columns()[6].name);
  EXPECT_EQ(std::optional<std::string>{"TEXT"}, select.result_columns()[6].declared_type);
  const auto& parenthesized_column =
      std::get<BoundColumnExpression>(ResultExpression(select, 6).payload);
  EXPECT_EQ(BoundSourceColumnId{1}, parenthesized_column.column);

  EXPECT_EQ("rowid", select.result_columns()[7].name);
  EXPECT_EQ(std::optional<std::string>{"TEXT"}, select.result_columns()[7].declared_type);
  const auto& declared_rowid = std::get<BoundColumnExpression>(ResultExpression(select, 7).payload);
  EXPECT_EQ(BoundSourceColumnId{3}, declared_rowid.column);

  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  const BoundSelect moved = std::move(select);
  EXPECT_FALSE(select.valid());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(moved.valid());
}

TEST(Binder, PublishesIntegerPrimaryKeyMetadataForHiddenRowIdNames) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const BoundSelect aliased = BindOrThrow("SELECT _rowid_, oid FROM Items", catalog);
  ASSERT_EQ(2U, aliased.result_columns().size());
  for (const BoundResultColumn& result : aliased.result_columns()) {
    EXPECT_EQ("id", result.name);
    EXPECT_EQ(std::optional<std::string>{"INTEGER"}, result.declared_type);
    EXPECT_EQ(TypeAffinity::kInteger, result.affinity);
  }

  const BoundSelect ordinary =
      BindOrThrow("SELECT rowid, _rowid_, oid FROM CustomCollation", catalog);
  ASSERT_EQ(3U, ordinary.result_columns().size());
  for (const BoundResultColumn& result : ordinary.result_columns()) {
    EXPECT_EQ("rowid", result.name);
    EXPECT_EQ(std::optional<std::string>{"INTEGER"}, result.declared_type);
    EXPECT_EQ(TypeAffinity::kInteger, result.affinity);
  }
}

TEST(Binder, BindsSyntheticSchemaTableAndLegacyWildcardQualifier) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select =
      BindOrThrow("SELECT sqlite_master.*, rowid FROM main.sqlite_schema", catalog);

  ASSERT_NE(nullptr, select.table_source());
  EXPECT_EQ(BoundSourceKind::kSchemaTable, select.table_source()->kind);
  EXPECT_FALSE(select.table_source()->table.has_value());
  ASSERT_EQ(5U, select.source_columns().size());
  EXPECT_EQ("type", select.source_columns()[0].name);
  EXPECT_EQ("INT", select.source_columns()[3].declared_type);
  ASSERT_EQ(6U, select.result_columns().size());
  EXPECT_EQ("sql", select.result_columns()[4].name);
  EXPECT_EQ("rowid", select.result_columns()[5].name);
  EXPECT_EQ(std::optional<std::string>{"INTEGER"}, select.result_columns()[5].declared_type);

  const BoundSelect alias = BindOrThrow("SELECT main.q.name FROM sqlite_schema AS q", catalog);
  EXPECT_EQ("name", alias.result_columns().front().name);

  ExpectBindError("SELECT sqlite_schema.* FROM sqlite_schema", catalog, BindErrorCode::kNoSuchTable,
                  "no such table: sqlite_schema");
}

TEST(Binder, AppliesAliasPrecedenceAndPreservesPerReferenceEvaluation) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow(
      "SELECT abs(?) AS x, FALSE AS flag FROM Items "
      "WHERE x > 0 OR 0.5 IS flag",
      catalog);

  ASSERT_TRUE(select.where_expression().has_value());
  const auto& logical_or = std::get<BoundBinaryExpression>(
      select.expression(RequiredOptional(select.where_expression())).payload);
  EXPECT_EQ(BoundBinaryOperation::kLogicalOr, logical_or.operation);

  const auto& comparison =
      std::get<BoundComparisonExpression>(select.expression(logical_or.left).payload);
  const auto& alias =
      std::get<BoundAliasReferenceExpression>(select.expression(comparison.left).payload);
  EXPECT_EQ(select.result_columns()[0].expression, alias.target);

  const auto& truth =
      std::get<BoundTruthTestExpression>(select.expression(logical_or.right).payload);
  EXPECT_EQ(BoundTruthValue::kFalse, truth.expected);
  EXPECT_FALSE(truth.negated);

  const BoundSelect source_precedence =
      BindOrThrow("SELECT Score AS Name FROM Items WHERE Name=0", catalog);
  const auto& source_comparison = std::get<BoundComparisonExpression>(
      source_precedence.expression(RequiredOptional(source_precedence.where_expression())).payload);
  const auto& source_column =
      std::get<BoundColumnExpression>(source_precedence.expression(source_comparison.left).payload);
  EXPECT_EQ(BoundSourceColumnId{1}, source_column.column);

  const BoundSelect first_alias =
      BindOrThrow("SELECT Score AS x, Name AS x FROM Items WHERE x=0", catalog);
  const auto& duplicate_comparison = std::get<BoundComparisonExpression>(
      first_alias.expression(RequiredOptional(first_alias.where_expression())).payload);
  const auto& duplicate_alias = std::get<BoundAliasReferenceExpression>(
      first_alias.expression(duplicate_comparison.left).payload);
  EXPECT_EQ(first_alias.result_columns()[0].expression, duplicate_alias.target);
}

TEST(Binder, DefersUnusedCollationsAndResolvesSelectedCollations) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect unused = BindOrThrow(
      "SELECT Value, Value COLLATE missing, abs(Value COLLATE missing) "
      "FROM CustomCollation",
      catalog);

  const BoundExpression& natural = ResultExpression(unused, 0);
  ASSERT_TRUE(natural.properties.collation.has_value());
  EXPECT_EQ("missing", CollationName(unused, *natural.properties.collation));
  EXPECT_FALSE(natural.properties.has_explicit_collation);

  const BoundExpression& explicit_collation = ResultExpression(unused, 1);
  ASSERT_TRUE(explicit_collation.properties.collation.has_value());
  EXPECT_EQ("missing", CollationName(unused, *explicit_collation.properties.collation));
  EXPECT_TRUE(explicit_collation.properties.has_explicit_collation);

  const auto& abs_call = std::get<BoundScalarCallExpression>(ResultExpression(unused, 2).payload);
  EXPECT_EQ("abs", FunctionName(unused, abs_call.function));
  EXPECT_EQ("BINARY", CollationName(unused, abs_call.collation));
  EXPECT_TRUE(ResultExpression(unused, 2).properties.has_explicit_collation);

  const BoundSelect comparisons =
      BindOrThrow("SELECT Name='A', 'A'=Name, Name COLLATE BINARY='A' FROM Items", catalog);
  const auto& left_column =
      std::get<BoundComparisonExpression>(ResultExpression(comparisons, 0).payload);
  const auto& right_column =
      std::get<BoundComparisonExpression>(ResultExpression(comparisons, 1).payload);
  const auto& explicit_binary =
      std::get<BoundComparisonExpression>(ResultExpression(comparisons, 2).payload);
  EXPECT_EQ("NOCASE", CollationName(comparisons, left_column.collation));
  EXPECT_EQ("NOCASE", CollationName(comparisons, right_column.collation));
  EXPECT_EQ("BINARY", CollationName(comparisons, explicit_binary.collation));

  const BoundSelect null_tests =
      BindOrThrow("SELECT Value IS NULL, Value IS NOT (NULL) FROM CustomCollation", catalog);
  for (std::size_t index = 0; index < null_tests.result_columns().size(); ++index) {
    const auto& comparison =
        std::get<BoundComparisonExpression>(ResultExpression(null_tests, index).payload);
    EXPECT_EQ(TypeAffinity::kNone, comparison.affinity);
    EXPECT_EQ("BINARY", CollationName(null_tests, comparison.collation));
  }

  ExpectBindError("SELECT Value='x' FROM CustomCollation", catalog, BindErrorCode::kNoSuchCollation,
                  "no such collation sequence: missing");
  ExpectBindError("SELECT nullif(Value COLLATE missing,'x') FROM CustomCollation", catalog,
                  BindErrorCode::kNoSuchCollation, "no such collation sequence: missing");
  ExpectBindError("SELECT Value IS +NULL FROM CustomCollation", catalog,
                  BindErrorCode::kNoSuchCollation, "no such collation sequence: missing");
  ExpectBindError("SELECT Value IS (NULL COLLATE missing) FROM CustomCollation", catalog,
                  BindErrorCode::kNoSuchCollation, "no such collation sequence: missing");
  ExpectBindError("SELECT NULL AS n FROM CustomCollation WHERE Value IS n", catalog,
                  BindErrorCode::kNoSuchCollation, "no such collation sequence: missing");
}

TEST(Binder, ResolvesEagerLazyConditionalAndLikelihoodFunctions) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow(
      "SELECT min(Name,'z'), coalesce(NULL,Name), iif(0,1,0,2,3), "
      "likely(Score), likelihood(Score,0.25) FROM Items",
      catalog);

  const auto& minimum = std::get<BoundScalarCallExpression>(ResultExpression(select, 0).payload);
  EXPECT_EQ("min", FunctionName(select, minimum.function));
  EXPECT_EQ("NOCASE", CollationName(select, minimum.collation));
  EXPECT_TRUE(select.functions()[minimum.function.value()].deterministic);

  EXPECT_TRUE(std::holds_alternative<BoundCoalesceExpression>(ResultExpression(select, 1).payload));
  const auto& conditional =
      std::get<BoundConditionalExpression>(ResultExpression(select, 2).payload);
  EXPECT_EQ(5U, conditional.arguments.size());

  const auto& likely = std::get<BoundLikelihoodExpression>(ResultExpression(select, 3).payload);
  EXPECT_DOUBLE_EQ(0.9375, likely.probability);
  const auto& likelihood = std::get<BoundLikelihoodExpression>(ResultExpression(select, 4).payload);
  EXPECT_DOUBLE_EQ(0.25, likelihood.probability);
  EXPECT_EQ(TypeAffinity::kNone, ResultExpression(select, 3).properties.affinity);
  EXPECT_FALSE(ResultExpression(select, 3).properties.collation.has_value());
  EXPECT_EQ(TypeAffinity::kNone, ResultExpression(select, 4).properties.affinity);
  EXPECT_FALSE(ResultExpression(select, 4).properties.collation.has_value());

  const BoundSelect hint_comparisons =
      BindOrThrow("SELECT likely(Name)='x', likely(Name COLLATE NOCASE)='x' FROM Items", catalog);
  const auto& natural_hint =
      std::get<BoundComparisonExpression>(ResultExpression(hint_comparisons, 0).payload);
  const auto& explicit_hint =
      std::get<BoundComparisonExpression>(ResultExpression(hint_comparisons, 1).payload);
  EXPECT_EQ(TypeAffinity::kNone, natural_hint.affinity);
  EXPECT_EQ("BINARY", CollationName(hint_comparisons, natural_hint.collation));
  EXPECT_EQ("NOCASE", CollationName(hint_comparisons, explicit_hint.collation));
  const BoundExpression& explicit_likelihood = hint_comparisons.expression(explicit_hint.left);
  EXPECT_EQ(TypeAffinity::kNone, explicit_likelihood.properties.affinity);
  EXPECT_TRUE(explicit_likelihood.properties.has_explicit_collation);

  const BoundSelect parenthesized_probability =
      BindOrThrow("SELECT likelihood(Score,((0.5))) FROM Items", catalog);
  EXPECT_DOUBLE_EQ(0.5, std::get<BoundLikelihoodExpression>(
                            ResultExpression(parenthesized_probability, 0).payload)
                            .probability);

  constexpr std::array<ScalarFunction, 1> kOverrideFunctions{
      ScalarFunction{"coalesce", FunctionArity::Exact(2), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, StubScalar},
  };
  const FunctionRegistry registry{kOverrideFunctions};
  const std::array<const Collation*, 3> collations{
      &BinaryCollation(),
      &NoCaseCollation(),
      &RTrimCollation(),
  };
  const BoundSelect overridden =
      BindOrThrow("SELECT coalesce(NULL,1)", catalog, BindEnvironment{registry, collations, 42});
  EXPECT_EQ(42U, overridden.registration_generation());
  const auto& call = std::get<BoundScalarCallExpression>(ResultExpression(overridden, 0).payload);
  EXPECT_EQ("coalesce", FunctionName(overridden, call.function));

  ExpectBindError("SELECT abs()", catalog, BindErrorCode::kWrongFunctionArity,
                  "wrong number of arguments to function abs()");
  ExpectBindError("SELECT unknown(1)", catalog, BindErrorCode::kNoSuchFunction,
                  "no such function: unknown");
  ExpectBindError("SELECT count(*)", catalog, BindErrorCode::kUnsupportedFeature,
                  "aggregate functions are not supported");
  ExpectBindError("SELECT min(Name) FROM Items", catalog, BindErrorCode::kUnsupportedFeature,
                  "aggregate functions are not supported");
  ExpectBindError("SELECT likelihood(1,1)", catalog, BindErrorCode::kInvalidLiteral,
                  "second argument to likelihood() must be a constant between 0.0 and 1.0");
  ExpectBindError("SELECT likelihood(1,(+0.5))", catalog, BindErrorCode::kInvalidLiteral,
                  "second argument to likelihood() must be a constant between 0.0 and 1.0");
}

TEST(Binder, AssignsParametersInLexicalOrderAndRetainsFirstSpellings) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow("SELECT ?, ?5, :x, :x, @x, $x, ?2 LIMIT ?10, ?", catalog);

  ASSERT_EQ(11U, select.parameters().size());
  EXPECT_FALSE(select.parameters()[0].name.has_value());
  EXPECT_EQ(std::optional<std::string>{"?2"}, select.parameters()[1].name);
  EXPECT_FALSE(select.parameters()[2].name.has_value());
  EXPECT_FALSE(select.parameters()[3].name.has_value());
  EXPECT_EQ(std::optional<std::string>{"?5"}, select.parameters()[4].name);
  EXPECT_EQ(std::optional<std::string>{":x"}, select.parameters()[5].name);
  EXPECT_EQ(std::optional<std::string>{"@x"}, select.parameters()[6].name);
  EXPECT_EQ(std::optional<std::string>{"$x"}, select.parameters()[7].name);
  EXPECT_FALSE(select.parameters()[8].name.has_value());
  EXPECT_EQ(std::optional<std::string>{"?10"}, select.parameters()[9].name);
  EXPECT_FALSE(select.parameters()[10].name.has_value());

  ASSERT_NE(nullptr, select.limit());
  const auto& bound_limit =
      std::get<BoundParameterExpression>(select.expression(select.limit()->limit).payload);
  ASSERT_TRUE(select.limit()->offset.has_value());
  const auto& bound_offset =
      std::get<BoundParameterExpression>(select.expression(*select.limit()->offset).payload);
  EXPECT_EQ(BoundParameterId{10}, bound_limit.parameter);
  EXPECT_EQ(BoundParameterId{9}, bound_offset.parameter);

  const BoundSelect first_spelling = BindOrThrow("SELECT ?0001, ?1, ?2, ?0002", catalog);
  ASSERT_EQ(2U, first_spelling.parameters().size());
  EXPECT_EQ(std::optional<std::string>{"?0001"}, first_spelling.parameters()[0].name);
  EXPECT_EQ(std::optional<std::string>{"?2"}, first_spelling.parameters()[1].name);

  ExpectBindError("SELECT ?6", catalog, BindErrorCode::kInvalidVariableNumber,
                  "variable number must be between ?1 and ?5", BindEnvironment::Core(),
                  BindOptions{.maximum_parameters = 5});
  ExpectBindError("SELECT ?,?,?,?,?,?", catalog, BindErrorCode::kParameterLimitExceeded,
                  "too many SQL variables", BindEnvironment::Core(),
                  BindOptions{.maximum_parameters = 5});
}

TEST(Binder, MaterializesPinnedLiteralBoundariesAndDqsFallback) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow(
      "SELECT NULL, 1_2, 0xffffffffffffffff, 9223372036854775808, "
      "-9223372036854775808, 1_2.5e1, 'it''s', x'CAFE', TRUE, FALSE, \"missing\"",
      catalog);

  ASSERT_EQ(11U, select.result_columns().size());
  EXPECT_EQ(SqlValueType::kNull,
            std::get<BoundLiteralExpression>(ResultExpression(select, 0).payload).value.type());
  EXPECT_EQ(
      12,
      std::get<BoundLiteralExpression>(ResultExpression(select, 1).payload).value.integer_value());
  EXPECT_EQ(
      -1,
      std::get<BoundLiteralExpression>(ResultExpression(select, 2).payload).value.integer_value());
  EXPECT_EQ(SqlValueType::kReal,
            std::get<BoundLiteralExpression>(ResultExpression(select, 3).payload).value.type());
  EXPECT_EQ(
      std::numeric_limits<std::int64_t>::min(),
      std::get<BoundLiteralExpression>(ResultExpression(select, 4).payload).value.integer_value());
  EXPECT_DOUBLE_EQ(125.0, std::get<BoundLiteralExpression>(ResultExpression(select, 5).payload)
                              .value.real_value()
                              .value_or(0.0));
  EXPECT_EQ(
      "it's",
      RequiredOptional(
          std::get<BoundLiteralExpression>(ResultExpression(select, 6).payload).value.text_value())
          .bytes());
  const ByteView blob = RequiredOptional(
      std::get<BoundLiteralExpression>(ResultExpression(select, 7).payload).value.blob_value());
  ASSERT_EQ(2U, blob.size());
  EXPECT_EQ(std::byte{0xca}, blob.data()[0]);
  EXPECT_EQ(std::byte{0xfe}, blob.data()[1]);
  EXPECT_EQ(
      1,
      std::get<BoundLiteralExpression>(ResultExpression(select, 8).payload).value.integer_value());
  EXPECT_EQ(
      0,
      std::get<BoundLiteralExpression>(ResultExpression(select, 9).payload).value.integer_value());
  EXPECT_EQ(
      "missing",
      RequiredOptional(
          std::get<BoundLiteralExpression>(ResultExpression(select, 10).payload).value.text_value())
          .bytes());
  EXPECT_EQ("\"missing\"", select.result_columns()[10].name);

  const BoundSelect signed_minimums = BindOrThrow(
      "SELECT -09223372036854775808, -09_223_372_036_854_775_808, "
      "-(9223372036854775808), -(+9223372036854775808), 0xdead_beef",
      catalog);
  for (std::size_t index = 0; index < 4U; ++index) {
    const auto& literal =
        std::get<BoundLiteralExpression>(ResultExpression(signed_minimums, index).payload);
    EXPECT_EQ(SqlValueType::kInteger, literal.value.type());
    EXPECT_EQ(std::numeric_limits<std::int64_t>::min(), literal.value.integer_value());
  }
  EXPECT_EQ(3'735'928'559,
            std::get<BoundLiteralExpression>(ResultExpression(signed_minimums, 4).payload)
                .value.integer_value());

  ExpectBindError("SELECT \"missing\"", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: \"missing\"", BindEnvironment::Core(),
                  BindOptions{.enable_double_quoted_strings = false});
  ExpectBindError("SELECT 0x10000000000000000", catalog, BindErrorCode::kInvalidLiteral,
                  "hex literal too big: 0x10000000000000000");
  ExpectBindError("SELECT CURRENT_DATE", catalog, BindErrorCode::kUnsupportedFeature,
                  "current-time literals are not supported");
}

TEST(Binder, ResolvesRowidBooleanAndAliasQualificationRules) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select =
      BindOrThrow("SELECT TRUE, rowid, _rowid_, oid, \"Name\" FROM Items", catalog);

  const auto& truth_column = std::get<BoundColumnExpression>(ResultExpression(select, 0).payload);
  EXPECT_EQ(BoundSourceColumnId{4}, truth_column.column);
  const auto& declared_rowid = std::get<BoundColumnExpression>(ResultExpression(select, 1).payload);
  EXPECT_EQ(BoundSourceColumnId{3}, declared_rowid.column);
  EXPECT_TRUE(std::holds_alternative<BoundRowIdExpression>(ResultExpression(select, 2).payload));
  EXPECT_TRUE(std::holds_alternative<BoundRowIdExpression>(ResultExpression(select, 3).payload));
  EXPECT_EQ("Name", select.result_columns()[4].name);

  const BoundSelect qualified = BindOrThrow("SELECT main.q.Name FROM Items AS q", catalog);
  EXPECT_EQ("Name", qualified.result_columns().front().name);
  ExpectBindError("SELECT Items.Name FROM Items AS q", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: Items.Name");
  ExpectBindError("SELECT rowid FROM wr", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: rowid");
  ExpectBindError("SELECT `TRUE`", catalog, BindErrorCode::kNoSuchColumn, "no such column: `TRUE`");
  ExpectBindError("SELECT [FALSE]", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: [FALSE]");
}

TEST(Binder, BindsSupportedOperatorsAndComparisonAffinity) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow(
      "SELECT +Name, -Score, ~id, NOT id, "
      "id+1, id-1, id*2, id/2, id%2, id<<1, id>>1, id&1, id|1, "
      "Name||'x', id AND Score, id OR Score, id=Score, Name='x', 1=2 "
      "FROM Items",
      catalog);

  constexpr std::array<BoundUnaryOperation, 4> kUnaryOperations{
      BoundUnaryOperation::kPositive,
      BoundUnaryOperation::kNegative,
      BoundUnaryOperation::kBitwiseNot,
      BoundUnaryOperation::kLogicalNot,
  };
  for (std::size_t index = 0; index < kUnaryOperations.size(); ++index) {
    const auto& unary = std::get<BoundUnaryExpression>(ResultExpression(select, index).payload);
    EXPECT_EQ(kUnaryOperations[index], unary.operation);
  }
  EXPECT_EQ(TypeAffinity::kNone, ResultExpression(select, 0).properties.affinity);
  ASSERT_TRUE(ResultExpression(select, 0).properties.collation.has_value());
  EXPECT_EQ(
      "NOCASE",
      CollationName(select, RequiredOptional(ResultExpression(select, 0).properties.collation)));

  constexpr std::array<BoundBinaryOperation, 12> kBinaryOperations{
      BoundBinaryOperation::kAdd,        BoundBinaryOperation::kSubtract,
      BoundBinaryOperation::kMultiply,   BoundBinaryOperation::kDivide,
      BoundBinaryOperation::kRemainder,  BoundBinaryOperation::kShiftLeft,
      BoundBinaryOperation::kShiftRight, BoundBinaryOperation::kBitwiseAnd,
      BoundBinaryOperation::kBitwiseOr,  BoundBinaryOperation::kConcatenate,
      BoundBinaryOperation::kLogicalAnd, BoundBinaryOperation::kLogicalOr,
  };
  for (std::size_t index = 0; index < kBinaryOperations.size(); ++index) {
    const auto& binary =
        std::get<BoundBinaryExpression>(ResultExpression(select, index + 4U).payload);
    EXPECT_EQ(kBinaryOperations[index], binary.operation);
  }

  const auto& numeric = std::get<BoundComparisonExpression>(ResultExpression(select, 16).payload);
  const auto& text = std::get<BoundComparisonExpression>(ResultExpression(select, 17).payload);
  const auto& none = std::get<BoundComparisonExpression>(ResultExpression(select, 18).payload);
  EXPECT_EQ(TypeAffinity::kNumeric, numeric.affinity);
  EXPECT_EQ(TypeAffinity::kText, text.affinity);
  EXPECT_EQ(TypeAffinity::kNone, none.affinity);
  EXPECT_EQ(TypeAffinity::kNone, ResultExpression(select, 16).properties.affinity);
}

TEST(Binder, PublishesExpressionMetadataAndTruthTestTransparency) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect metadata =
      BindOrThrow("SELECT Name, (Name), Name COLLATE BINARY, +Name, Name||'' FROM Items", catalog);

  EXPECT_EQ(std::optional<std::string>{"TEXT"}, metadata.result_columns()[0].declared_type);
  EXPECT_EQ(std::optional<std::string>{"TEXT"}, metadata.result_columns()[1].declared_type);
  EXPECT_FALSE(metadata.result_columns()[2].declared_type.has_value());
  EXPECT_FALSE(metadata.result_columns()[3].declared_type.has_value());
  EXPECT_FALSE(metadata.result_columns()[4].declared_type.has_value());
  EXPECT_EQ("Name COLLATE BINARY", metadata.result_columns()[2].name);
  EXPECT_EQ("+Name", metadata.result_columns()[3].name);
  EXPECT_EQ("Name||''", metadata.result_columns()[4].name);
  EXPECT_EQ(TypeAffinity::kText, metadata.result_columns()[2].affinity);
  EXPECT_EQ(TypeAffinity::kNone, metadata.result_columns()[3].affinity);

  const BoundSelect truth = BindOrThrow(
      "SELECT 0.5 IS TRUE, 0.5 IS (FALSE), "
      "0.5 IS TRUE COLLATE missing, 0.5 IS likely(TRUE), "
      "0.5 IS likelihood(TRUE,0.5), 0.5 IS NOT FALSE",
      catalog);
  EXPECT_TRUE(std::holds_alternative<BoundTruthTestExpression>(ResultExpression(truth, 0).payload));
  EXPECT_TRUE(std::holds_alternative<BoundTruthTestExpression>(ResultExpression(truth, 1).payload));
  EXPECT_TRUE(std::holds_alternative<BoundTruthTestExpression>(ResultExpression(truth, 2).payload));
  EXPECT_TRUE(
      std::holds_alternative<BoundComparisonExpression>(ResultExpression(truth, 3).payload));
  EXPECT_TRUE(
      std::holds_alternative<BoundComparisonExpression>(ResultExpression(truth, 4).payload));
  const auto& negated = std::get<BoundTruthTestExpression>(ResultExpression(truth, 5).payload);
  EXPECT_EQ(BoundTruthValue::kFalse, negated.expected);
  EXPECT_TRUE(negated.negated);
  ASSERT_TRUE(ResultExpression(truth, 2).properties.collation.has_value());
  EXPECT_EQ(
      "missing",
      CollationName(truth, RequiredOptional(ResultExpression(truth, 2).properties.collation)));
}

TEST(Binder, PublishesOnlyPinnedSqliteScalarTruthHints) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow(
      "SELECT TRUE, FALSE, 0, 1, (0), 0x10, 0x7fffffff, "
      "0x80000000, 0x8000000000000000, 0xffffffffffffffff, "
      "-1, +1, 2147483648, 1_0, 0.0, NULL, "
      "1 IS NULL, 1 IS NOT NULL, +1 IS NULL, "
      "1 COLLATE BINARY",
      catalog);

  const std::array<BoundTruthHint, 20> expected{
      BoundTruthHint::kAlwaysTrue,  BoundTruthHint::kAlwaysFalse, BoundTruthHint::kAlwaysFalse,
      BoundTruthHint::kAlwaysTrue,  BoundTruthHint::kAlwaysFalse, BoundTruthHint::kAlwaysTrue,
      BoundTruthHint::kAlwaysTrue,  BoundTruthHint::kNone,        BoundTruthHint::kNone,
      BoundTruthHint::kNone,        BoundTruthHint::kNone,        BoundTruthHint::kNone,
      BoundTruthHint::kNone,        BoundTruthHint::kNone,        BoundTruthHint::kNone,
      BoundTruthHint::kNone,        BoundTruthHint::kAlwaysFalse, BoundTruthHint::kAlwaysTrue,
      BoundTruthHint::kAlwaysFalse, BoundTruthHint::kNone,
  };
  ASSERT_EQ(expected.size(), select.result_columns().size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(expected[index], ResultExpression(select, index).properties.truth_hint) << index;
  }

  const BoundSelect alias = BindOrThrow("SELECT 0 AS never WHERE never", catalog);
  ASSERT_TRUE(alias.where_expression().has_value());
  EXPECT_EQ(BoundTruthHint::kAlwaysFalse,
            alias.expression(RequiredOptional(alias.where_expression())).properties.truth_hint);
}

TEST(Binder, PreservesConditionalParityAndScalarOverrideRules) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect conditionals = BindOrThrow(
      "SELECT iif(1,2), iif(1,2,3), iif(0,1,1,2), iif(0,1,0,2,3), "
      "abs(DISTINCT 1)",
      catalog);
  for (std::size_t index = 0; index < 4U; ++index) {
    const auto& conditional =
        std::get<BoundConditionalExpression>(ResultExpression(conditionals, index).payload);
    EXPECT_EQ(index + 2U, conditional.arguments.size());
  }
  EXPECT_TRUE(
      std::holds_alternative<BoundScalarCallExpression>(ResultExpression(conditionals, 4).payload));

  constexpr std::array<ScalarFunction, 2> kOverloads{
      ScalarFunction{"choose", FunctionArity::AtLeast(1), FunctionDeterminism::kNonDeterministic,
                     FunctionCollationUse::kNone, StubScalar},
      ScalarFunction{"choose", FunctionArity::Exact(2), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, StubScalar},
  };
  const FunctionRegistry overload_registry{kOverloads};
  const std::array<const Collation*, 3> collations{
      &BinaryCollation(),
      &NoCaseCollation(),
      &RTrimCollation(),
  };
  const BoundSelect exact =
      BindOrThrow("SELECT choose(1,2)", catalog, BindEnvironment{overload_registry, collations, 7});
  const auto& exact_call = std::get<BoundScalarCallExpression>(ResultExpression(exact, 0).payload);
  EXPECT_TRUE(exact.functions()[exact_call.function.value()].deterministic);

  constexpr std::array<ScalarFunction, 1> kNonmatchingOverride{
      ScalarFunction{"coalesce", FunctionArity::Exact(3), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, StubScalar},
  };
  const FunctionRegistry nonmatching_registry{kNonmatchingOverride};
  const BoundSelect lazy = BindOrThrow("SELECT coalesce(1,2)", catalog,
                                       BindEnvironment{nonmatching_registry, collations, 8});
  EXPECT_TRUE(std::holds_alternative<BoundCoalesceExpression>(ResultExpression(lazy, 0).payload));
}

TEST(Binder, OwnsPublishedStateAndRejectsInvalidEnvironments) {
  ASSERT_EQ(3U, kStaticInitializationEnvironment.collations().size());
  for (const Collation* collation : kStaticInitializationEnvironment.collations()) {
    EXPECT_NE(nullptr, collation);
  }

  CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect owned = [&catalog] {
    constexpr std::array<ScalarFunction, 1> kFunctions{
        ScalarFunction{"ephemeral", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                       FunctionCollationUse::kNone, StubScalar},
    };
    const FunctionRegistry registry{kFunctions};
    const std::array<const Collation*, 3> collations{
        &BinaryCollation(),
        &NoCaseCollation(),
        &RTrimCollation(),
    };
    return BindOrThrow("SELECT ephemeral(Name) FROM Items", catalog,
                       BindEnvironment{registry, collations, 99});
  }();
  catalog.reset();
  EXPECT_EQ(99U, owned.registration_generation());
  EXPECT_NE(nullptr, owned.catalog());
  EXPECT_EQ("Name", owned.source_columns()[1].name);
  EXPECT_EQ("ephemeral", owned.functions().front().name);

  const CatalogSnapshotPtr validation_catalog = TestCatalog();
  const FunctionRegistry empty_registry{std::span<const ScalarFunction>{}};
  const std::array<const Collation*, 2> missing_binary{
      &NoCaseCollation(),
      &RTrimCollation(),
  };
  BindSelectResult missing =
      BindSelectStatement(ParseTree("SELECT 1"), validation_catalog,
                          BindEnvironment{empty_registry, missing_binary, 0});
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(BindErrorCode::kInvalidInput, missing.error().code);

  const std::array<const Collation*, 2> null_descriptor{
      &BinaryCollation(),
      nullptr,
  };
  BindSelectResult invalid =
      BindSelectStatement(ParseTree("SELECT 1"), validation_catalog,
                          BindEnvironment{empty_registry, null_descriptor, 0});
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(BindErrorCode::kInvalidInput, invalid.error().code);

  BindSelectResult null_catalog = BindSelectStatement(ParseTree("SELECT 1"), CatalogSnapshotPtr{});
  ASSERT_FALSE(null_catalog.has_value());
  EXPECT_EQ(BindErrorCode::kInvalidInput, null_catalog.error().code);

  BindSelectResult non_select =
      BindSelectStatement(ParseTree("CREATE TABLE x(a)"), validation_catalog);
  ASSERT_FALSE(non_select.has_value());
  EXPECT_EQ(BindErrorCode::kInvalidInput, non_select.error().code);
}

TEST(Binder, RejectsLiteralTokensThatDoNotMatchTheirPublicAstKinds) {
  struct Case {
    std::string_view token;
    LiteralKind kind;
  };
  constexpr std::array cases{
      Case{.token = "x", .kind = LiteralKind::kInteger},
      Case{.token = "1", .kind = LiteralKind::kReal},
      Case{.token = "1.0", .kind = LiteralKind::kInteger},
      Case{.token = "1__2", .kind = LiteralKind::kInteger},
      Case{.token = "1_.0", .kind = LiteralKind::kReal},
  };
  const CatalogSnapshotPtr catalog = TestCatalog();

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.token);
    std::string source{"SELECT "};
    source.append(test_case.token);
    const SourceSpan statement_span = Span(0, source.size());
    const SourceSpan token_span = Span(7, source.size());
    std::vector<Expression> expressions{
        Expression{
            .span = token_span,
            .payload =
                LiteralExpression{
                    .kind = test_case.kind,
                    .token = token_span,
                },
        },
    };
    Statement statement = SelectStatement{
        .span = statement_span,
        .result_columns =
            {
                ResultColumn{
                    .span = token_span,
                    .expression = ExpressionId{0},
                },
            },
    };
    Result<SyntaxTree> tree =
        SyntaxTree::Create(std::move(source), std::move(expressions), std::move(statement));
    ASSERT_TRUE(tree.has_value()) << tree.error().ToString();

    BindSelectResult bound = BindSelectStatement(std::move(*tree), catalog);
    ASSERT_FALSE(bound.has_value());
    EXPECT_EQ(BindErrorCode::kInternalInvariant, bound.error().code);
    EXPECT_EQ("literal token does not match its AST kind: " + std::string{test_case.token},
              bound.error().detail);
  }

  std::string source{"SELECT likelihood(1,x)"};
  const SourceSpan statement_span = Span(0, source.size());
  const SourceSpan call_span = Span(7, source.size());
  const SourceSpan name_span = Span(7, 17);
  const SourceSpan value_span = Span(18, 19);
  const SourceSpan probability_span = Span(20, 21);
  std::vector<Expression> expressions{
      Expression{
          .span = value_span,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = value_span,
              },
      },
      Expression{
          .span = probability_span,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kReal,
                  .token = probability_span,
              },
      },
      Expression{
          .span = call_span,
          .payload =
              FunctionCallExpression{
                  .name =
                      QualifiedName{
                          .span = name_span,
                          .parts = {name_span},
                      },
                  .arguments = {ExpressionId{0}, ExpressionId{1}},
              },
      },
  };
  Statement statement = SelectStatement{
      .span = statement_span,
      .result_columns =
          {
              ResultColumn{
                  .span = call_span,
                  .expression = ExpressionId{2},
              },
          },
  };
  Result<SyntaxTree> tree =
      SyntaxTree::Create(std::move(source), std::move(expressions), std::move(statement));
  ASSERT_TRUE(tree.has_value()) << tree.error().ToString();

  BindSelectResult bound = BindSelectStatement(std::move(*tree), catalog);
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(BindErrorCode::kInternalInvariant, bound.error().code);
  EXPECT_EQ("literal token does not match its AST kind: x", bound.error().detail);
}

TEST(Binder, RejectsUnsupportedScopesAndConfiguredLimits) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  ExpectBindError("SELECT *", catalog, BindErrorCode::kNoTablesSpecified, "no tables specified");
  ExpectBindError("SELECT DISTINCT Name FROM Items", catalog, BindErrorCode::kUnsupportedFeature,
                  "SELECT DISTINCT is not supported");
  ExpectBindError("SELECT Name LIKE 'a%' FROM Items", catalog, BindErrorCode::kUnsupportedFeature,
                  "pattern operators are not supported");
  ExpectBindError("SELECT Name FROM Items LIMIT Name", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: Name");
  ExpectBindError("SELECT 1 AS x, x", catalog, BindErrorCode::kNoSuchColumn, "no such column: x");
  ExpectBindError("SELECT 1 AS x LIMIT x", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: x");
  ExpectBindError("SELECT * FROM Items", catalog, BindErrorCode::kResultColumnLimitExceeded,
                  "too many columns in result set", BindEnvironment::Core(),
                  BindOptions{.maximum_result_columns = 4});
  ExpectBindError("SELECT coalesce(1,2,3)", catalog, BindErrorCode::kFunctionArgumentLimitExceeded,
                  "too many arguments on function coalesce", BindEnvironment::Core(),
                  BindOptions{.maximum_function_arguments = 2});
  ExpectBindError("SELECT missing FROM Items", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: missing");
  ExpectBindError("SELECT 1 FROM Missing", catalog, BindErrorCode::kNoSuchTable,
                  "no such table: Missing");
  ExpectBindError("SELECT -0x10000000000000000", catalog, BindErrorCode::kInvalidLiteral,
                  "hex literal too big: -0x10000000000000000");
}

}  // namespace
}  // namespace modern_sqlite
