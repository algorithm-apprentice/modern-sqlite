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

#include "modern_sqlite/binder/bound_statement.hpp"
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
static_assert(!std::is_copy_constructible_v<BoundInsert>);
static_assert(!std::is_copy_assignable_v<BoundInsert>);
static_assert(std::is_nothrow_move_constructible_v<BoundInsert>);
static_assert(std::is_nothrow_move_assignable_v<BoundInsert>);
static_assert(!std::is_copy_constructible_v<BoundUpdate>);
static_assert(!std::is_copy_constructible_v<BoundDelete>);
static_assert(!std::is_copy_constructible_v<BoundCreateTable>);
static_assert(!std::is_copy_constructible_v<BoundCreateIndex>);
static_assert(!std::is_copy_constructible_v<BoundAnalyze>);
static_assert(!std::is_copy_constructible_v<BoundStatement>);
static_assert(std::is_nothrow_move_constructible_v<BoundStatement>);
static_assert(std::is_nothrow_move_assignable_v<BoundStatement>);

const BindEnvironment kStaticInitializationEnvironment = BindEnvironment::Core();

static_assert(!std::is_convertible_v<BoundSourceId, BoundSourceColumnId>);
static_assert(!std::is_convertible_v<BoundSourceColumnId, BoundSourceId>);
static_assert(!std::is_convertible_v<BoundQueryId, BoundSourceId>);
static_assert(!std::is_convertible_v<BoundQueryId, BoundExpressionId>);

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

[[nodiscard]] CatalogSnapshotPtr CollatedRowIdCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 19, .generation = 10},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Items(id INTEGER PRIMARY KEY COLLATE NOCASE)"));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = "Items",
      .root_page = RootPageId{2},
      .columns =
          {
              CatalogColumnInput{
                  .name = "id",
                  .declared_type = "INTEGER",
                  .collation_name = "NOCASE",
                  .primary_key = true,
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
      .version = CatalogVersion{.schema_cookie = 23, .generation = 11},
  };

  SyntaxTree items = ParseTree(
      "CREATE TABLE Items("
      "id INTEGER PRIMARY KEY, "
      "Name TEXT NOT NULL DEFAULT 'seed', "
      "Score REAL"
      ")");
  const ExpressionId name_default = ColumnDefaultExpression(items, 1);
  input.definitions.push_back(std::move(items));
  input.definitions.push_back(
      ParseTree("CREATE TABLE IndexedItems(id INTEGER PRIMARY KEY, Value TEXT)"));
  input.definitions.push_back(ParseTree("CREATE TABLE wr(key TEXT PRIMARY KEY) WITHOUT ROWID"));
  input.definitions.push_back(ParseTree("CREATE INDEX idx_items_value ON IndexedItems(Value)"));
  input.definitions.push_back(ParseTree("CREATE TABLE Shadowed(rowid TEXT, Value TEXT)"));
  SyntaxTree dynamic_default =
      ParseTree("CREATE TABLE DynamicDefault(Value TEXT DEFAULT CURRENT_DATE)");
  const ExpressionId dynamic_default_expression = ColumnDefaultExpression(dynamic_default, 0);
  input.definitions.push_back(std::move(dynamic_default));

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
      .name = "IndexedItems",
      .root_page = RootPageId{3},
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
              },
          },
      .rowid_alias = ColumnId{0},
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
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{4},
      .name = "Shadowed",
      .root_page = RootPageId{6},
      .columns =
          {
              CatalogColumnInput{
                  .name = "rowid",
                  .declared_type = "TEXT",
              },
              CatalogColumnInput{
                  .name = "Value",
                  .declared_type = "TEXT",
              },
          },
  });
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{5},
      .name = "DynamicDefault",
      .root_page = RootPageId{7},
      .columns =
          {
              CatalogColumnInput{
                  .name = "Value",
                  .declared_type = "TEXT",
                  .default_expression =
                      SchemaExpression{
                          .definition = SchemaDefinitionId{5},
                          .expression = dynamic_default_expression,
                      },
              },
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{3},
      .name = "idx_items_value",
      .table = TableId{1},
      .root_page = RootPageId{5},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(1),
              CatalogIndexTerm{
                  .target = RowIdIndexTerm{},
                  .collation_name = "BINARY",
                  .order = SortOrder::kAscending,
              },
          },
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

[[nodiscard]] BoundStatement BindStatementOrThrow(
    std::string_view sql, const CatalogSnapshotPtr& catalog,
    BindEnvironment environment = BindEnvironment::Core(), BindOptions options = {}) {
  BindStatementResult bound = BindStatement(ParseTree(sql), catalog, environment, options);
  if (!bound.has_value()) {
    throw std::runtime_error{bound.error().detail};
  }
  return std::move(*bound);
}

void ExpectStatementBindError(std::string_view sql, const CatalogSnapshotPtr& catalog,
                              BindErrorCode code, ErrorCode base_code,
                              std::string_view detail_fragment) {
  const BindStatementResult bound = BindStatement(ParseTree(sql), catalog);
  ASSERT_FALSE(bound.has_value()) << sql;
  EXPECT_EQ(code, bound.error().code) << sql;
  EXPECT_EQ(base_code, bound.error().base_error_code()) << sql;
  EXPECT_NE(std::string_view::npos, bound.error().detail.find(detail_fragment)) << sql;
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
  EXPECT_EQ("aggregate_call", BoundExpressionKindName(BoundExpressionKind::kAggregateCall));
  EXPECT_EQ("unknown",
            BoundExpressionKindName(static_cast<BoundExpressionKind>(255)));  // NOLINT

  EXPECT_EQ("no_such_table", BindErrorCodeName(BindErrorCode::kNoSuchTable));
  EXPECT_EQ("invalid_variable_number", BindErrorCodeName(BindErrorCode::kInvalidVariableNumber));
  EXPECT_EQ("indexed_table_unsupported",
            BindErrorCodeName(BindErrorCode::kIndexedTableUnsupported));
  EXPECT_EQ("order_by_term_out_of_range", BindErrorCodeName(BindErrorCode::kOrderByTermOutOfRange));
  EXPECT_EQ("group_by_term_out_of_range", BindErrorCodeName(BindErrorCode::kGroupByTermOutOfRange));
  EXPECT_EQ("aggregate_misuse", BindErrorCodeName(BindErrorCode::kAggregateMisuse));
  EXPECT_EQ("having_without_aggregate", BindErrorCodeName(BindErrorCode::kHavingWithoutAggregate));
  EXPECT_EQ("aggregate_term_limit_exceeded",
            BindErrorCodeName(BindErrorCode::kAggregateTermLimitExceeded));
  EXPECT_EQ("compound_order_by_term", BindErrorCodeName(BindErrorCode::kCompoundOrderByTerm));
  EXPECT_EQ("compound_column_count", BindErrorCodeName(BindErrorCode::kCompoundColumnCount));
  EXPECT_EQ("values_column_count", BindErrorCodeName(BindErrorCode::kValuesColumnCount));
  EXPECT_EQ("index_already_exists", BindErrorCodeName(BindErrorCode::kIndexAlreadyExists));
  EXPECT_EQ("unknown", BindErrorCodeName(static_cast<BindErrorCode>(255)));  // NOLINT

  EXPECT_EQ(ErrorCode::kGeneric, BindError{.code = BindErrorCode::kNoSuchColumn}.base_error_code());
  EXPECT_EQ(ErrorCode::kTooLarge,
            BindError{.code = BindErrorCode::kResultColumnLimitExceeded}.base_error_code());
  EXPECT_EQ(ErrorCode::kTooLarge,
            BindError{.code = BindErrorCode::kAggregateTermLimitExceeded}.base_error_code());
  EXPECT_EQ(ErrorCode::kProtocol,
            BindError{.code = BindErrorCode::kIndexedTableUnsupported}.base_error_code());
  EXPECT_EQ(ErrorCode::kMisuse, BindError{.code = BindErrorCode::kInvalidInput}.base_error_code());
  EXPECT_EQ(0U, BindEnvironment::Core().registration_generation());
}

TEST(Binder, PublishesOneCanonicalRootQueryMetadataOwner) {
  const auto catalog = TestCatalog();
  for (const std::string_view sql : {
           "SELECT id FROM Items ORDER BY id LIMIT ?1",
           "VALUES(1),(2) UNION ALL SELECT id FROM Items ORDER BY 1 LIMIT ?1 OFFSET ?2",
           "SELECT count() FROM Items a FULL JOIN Items b USING(id)",
       }) {
    SCOPED_TRACE(sql);
    const BoundSelect bound = BindOrThrow(sql, catalog);
    const BoundQuery& query = bound.root_query();
    EXPECT_EQ(BoundQueryId{0}, query.id);
    EXPECT_FALSE(query.parent.has_value());
    EXPECT_TRUE(query.captures.empty());
    EXPECT_EQ(0U, query.core_begin);
    EXPECT_EQ(bound.query_cores().size(), query.core_count);
    EXPECT_EQ(bound.compound_operators().data(), query.compound_operators.data());
    EXPECT_EQ(bound.compound_collations().data(), query.compound_collations.data());
    EXPECT_EQ(bound.result_columns().data(), query.output_columns.data());
    EXPECT_EQ(bound.order_by().data(), query.order_by.data());
    EXPECT_EQ(bound.limit(), query.limit.has_value() ? &*query.limit : nullptr);
  }
}

TEST(Binder, PublishesRegisteredCollationsForUnindexedJoinedScopes) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  for (const std::string_view sql :
       {"SELECT b.Name FROM Items a JOIN Items b ON b.Name=a.Name",
        "SELECT (SELECT b.Name FROM Items a JOIN Items b ON b.Name=a.Name LIMIT 1)"}) {
    SCOPED_TRACE(sql);
    const BoundSelect bound = BindOrThrow(sql, catalog);
    const std::span<const std::string> registered = bound.registered_collations();
    ASSERT_EQ(3U, registered.size());
    EXPECT_EQ("BINARY", registered[0]);
    EXPECT_EQ("NOCASE", registered[1]);
    EXPECT_EQ("RTRIM", registered[2]);
  }
  const BoundSelect single = BindOrThrow("SELECT Name FROM Items", catalog);
  EXPECT_TRUE(single.registered_collations().empty());
}

TEST(Binder, KeepsNestedQueriesInReservedCoreRangesAndStatementGlobalArenas) {
  const BoundSelect bound = BindOrThrow(
      "SELECT (SELECT id FROM Items WHERE id=:one) AS x,(SELECT 7) AS y FROM Items AS parent "
      "UNION ALL SELECT (VALUES(8)),EXISTS(SELECT sum(id) FROM Items)",
      TestCatalog());
  ASSERT_EQ(5U, bound.query_count());
  ASSERT_EQ(6U, bound.query_cores().size());
  EXPECT_EQ(0U, bound.root_query().core_begin);
  EXPECT_EQ(2U, bound.root_query().core_count);
  ASSERT_EQ(1U, bound.parameters().size());
  EXPECT_EQ(":one", bound.parameters()[0].name);
  for (std::uint32_t index = 1; index < 5U; ++index) {
    const BoundQueryView query{bound, BoundQueryId{index}};
    EXPECT_TRUE(query.valid());
    EXPECT_EQ(index + 1U, query.query().core_begin);
    EXPECT_EQ(1U, query.query().core_count);
    EXPECT_EQ(bound.expressions().data(), query.expressions().data());
    EXPECT_EQ(bound.source_columns().data(), query.source_columns().data());
    EXPECT_EQ(bound.parameters().data(), query.parameters().data());
    ASSERT_NE(nullptr, query.limit());
  }
  const auto& parent = std::get<BoundSelectCore>(bound.query_cores()[0]);
  const auto& first_child = std::get<BoundSelectCore>(bound.query_cores()[2]);
  const auto& last_child = std::get<BoundSelectCore>(bound.query_cores()[5]);
  EXPECT_EQ(BoundSourceId{0}, parent.table_sources[0].id);
  EXPECT_EQ(BoundSourceId{1}, first_child.table_sources[0].id);
  EXPECT_EQ(BoundSourceId{2}, last_child.table_sources[0].id);
  EXPECT_FALSE(parent.aggregate.has_value());
  ASSERT_TRUE(last_child.aggregate.has_value());
  EXPECT_EQ(BoundQueryDestination::kExists, bound.query(BoundQueryId{4}).destination);
}

TEST(Binder, PublishesScalarAffinityAndTypeWithoutBorrowingChildCollation) {
  const BoundSelect bound = BindOrThrow(
      "SELECT (SELECT Name FROM Items),(SELECT id FROM Items WHERE 0),EXISTS(SELECT id,Name FROM "
      "Items)",
      TestCatalog());
  ASSERT_EQ(3U, bound.result_columns().size());
  EXPECT_EQ("TEXT", bound.result_columns()[0].declared_type);
  EXPECT_EQ(TypeAffinity::kText, bound.result_columns()[0].affinity);
  EXPECT_EQ("INTEGER", bound.result_columns()[1].declared_type);
  EXPECT_EQ(TypeAffinity::kInteger, bound.result_columns()[1].affinity);
  EXPECT_EQ(std::nullopt, ResultExpression(bound, 0).properties.collation);
  EXPECT_EQ(std::nullopt, bound.result_columns()[2].declared_type);
  EXPECT_FALSE(ResultExpression(bound, 2).properties.contains_aggregate);
  const BoundSelect values = BindOrThrow("VALUES((SELECT id FROM Items))", TestCatalog());
  EXPECT_EQ("INTEGER", values.result_columns()[0].declared_type);
}

TEST(Binder, DerivesResultValueDemandFromDestinationAndSetConsumers) {
  const auto catalog = TestCatalog();
  struct Case {
    std::string_view suffix;
    bool compares_values;
  };
  for (const bool scalar : {false, true}) {
    SCOPED_TRACE(scalar);
    for (const Case& test_case :
         {Case{"", false}, Case{" LIMIT 1 OFFSET 1", false}, Case{" UNION SELECT 'other'", false},
          Case{" UNION ALL SELECT 'other'", false}, Case{" INTERSECT SELECT 'other'", true},
          Case{" EXCEPT SELECT 'other'", true},
          Case{" UNION SELECT 'other' LIMIT 1 OFFSET 1", true},
          Case{" UNION ALL SELECT 'other' LIMIT 1 OFFSET 1", false}}) {
      SCOPED_TRACE(test_case.suffix);
      const BoundSelect bound =
          BindOrThrow(std::string{scalar ? "SELECT (" : "SELECT EXISTS("} +
                          "SELECT Name AS x FROM Items WHERE EXISTS(SELECT x)" +
                          std::string{test_case.suffix} + ")",
                      catalog);
      EXPECT_TRUE(BoundQueryRequiresResultValues(bound.root_query(), bound.expressions()));
      EXPECT_EQ(scalar || test_case.compares_values,
                BoundQueryRequiresResultValues(bound.query(BoundQueryId{1}), bound.expressions()));
    }
  }
}

TEST(Binder, RejectsScalarWidthParentAliasesAndStagedQueryFeatures) {
  const auto catalog = TestCatalog();
  ExpectBindError("SELECT (SELECT id,Name FROM Items)", catalog,
                  BindErrorCode::kColumnCountMismatch, "sub-select returns 2 columns - expected 1");
  ExpectBindError("SELECT id AS x,(SELECT x) FROM Items", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: x");
  ExpectBindError("SELECT (SELECT x),1 AS x", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: x");
  ExpectBindError("SELECT (SELECT Name) AS x FROM Items WHERE (SELECT x) IS NOT NULL", catalog,
                  BindErrorCode::kUnsupportedFeature,
                  "outer result-alias definitions containing queries are not supported");
  ExpectBindError("SELECT (SELECT sum(parent.Score)) FROM Items parent", catalog,
                  BindErrorCode::kUnsupportedFeature, "outer-owned aggregates are not supported");
  ExpectStatementBindError("UPDATE Items SET Score=(SELECT 1)", catalog,
                           BindErrorCode::kUnsupportedFeature, ErrorCode::kGeneric,
                           "subqueries in mutation statements");
}

TEST(Binder, PublishesTransitiveCapturesAndPreservesDiscardedBindingDependencies) {
  const BoundSelect bound = BindOrThrow(
      "SELECT (SELECT (SELECT o.Name||i.Name) FROM Items i WHERE i.id=o.id),"
      "EXISTS(SELECT o.Score ORDER BY (SELECT o.Name)),(VALUES(o._rowid_)) FROM Items o",
      TestCatalog());
  ASSERT_EQ(5U, bound.query_count());
  EXPECT_TRUE(bound.root_query().captures.empty());
  const std::array parents{BoundQueryId{0}, BoundQueryId{1}, BoundQueryId{0}, BoundQueryId{0}};
  const std::array<std::size_t, 4> counts{2, 2, 2, 1};
  for (std::uint32_t index = 1; index < bound.query_count(); ++index) {
    const BoundQuery& query = bound.query(BoundQueryId{index});
    EXPECT_EQ(parents[index - 1U], query.parent);
    ASSERT_EQ(counts[index - 1U], query.captures.size());
    for (const BoundQueryCapture& dependency : query.captures) {
      const auto* captured = std::get_if<BoundSourceCapture>(&dependency);
      ASSERT_NE(nullptr, captured);
      const BoundSourceCapture& capture = *captured;
      if (index == 3U) {
        EXPECT_FALSE(capture.value.has_value());
      } else {
        ASSERT_TRUE(capture.value.has_value());
        EXPECT_EQ(capture.identity, BoundSourceLeafIdentityOf(bound.expression(*capture.value),
                                                              bound.source_columns()));
      }
    }
  }
  EXPECT_EQ(BoundSourceId{0},
            std::get<BoundSourceCapture>(bound.query(BoundQueryId{1}).captures[0]).identity.source);
  EXPECT_EQ(BoundSourceId{1},
            std::get<BoundSourceCapture>(bound.query(BoundQueryId{2}).captures[1]).identity.source);
  EXPECT_TRUE(bound.query(BoundQueryId{3}).order_by.empty());
  EXPECT_TRUE(
      std::get<BoundSourceCapture>(bound.query(BoundQueryId{4}).captures[0]).identity.is_rowid());

  const auto captures =
      [&bound](BoundQueryId query) -> std::optional<std::span<const BoundQueryCapture>> {
    if (query.value() >= bound.query_count()) {
      return std::nullopt;
    }
    return std::span<const BoundQueryCapture>{bound.query(query).captures};
  };
  for (const BoundSourceDependency dependency :
       {BoundSourceDependency::kBinding, BoundSourceDependency::kValue}) {
    std::size_t visited = 0;
    auto visitor = [&](const BoundSourceLeafIdentity&, std::optional<BoundExpressionId>) {
      ++visited;
      return true;
    };
    EXPECT_TRUE(binder_detail::VisitSourceLeaves(bound.expressions(), bound.source_columns(),
                                                 bound.result_columns()[1].expression, captures,
                                                 dependency, visitor));
    EXPECT_EQ(dependency == BoundSourceDependency::kBinding ? 2U : 0U, visited);
    EXPECT_FALSE(binder_detail::VisitSourceLeaves(
        bound.expressions(), bound.source_columns(),
        BoundExpressionId{static_cast<std::uint32_t>(bound.expressions().size())}, captures,
        dependency, visitor));
  }
}

TEST(Binder, PropagatesFieldFreeAliasContextWithoutCapturingItsDefiningQuery) {
  const auto catalog = TestCatalog();
  const BoundSelect transitive =
      BindOrThrow("SELECT 1 AS x FROM Items WHERE (SELECT (SELECT x))>0", catalog);
  ASSERT_EQ(3U, transitive.query_count());
  EXPECT_TRUE(transitive.root_query().captures.empty());
  EXPECT_EQ(BoundQueryId{0}, transitive.query(BoundQueryId{1}).parent);
  EXPECT_EQ(BoundQueryId{1}, transitive.query(BoundQueryId{2}).parent);
  ASSERT_EQ(1U, transitive.query(BoundQueryId{1}).captures.size());
  ASSERT_EQ(1U, transitive.query(BoundQueryId{2}).captures.size());
  for (const BoundQueryId query : {BoundQueryId{1}, BoundQueryId{2}}) {
    const auto* context =
        std::get_if<BoundQueryContextCapture>(&transitive.query(query).captures[0]);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(BoundQueryId{0}, context->query);
  }

  const BoundSelect private_owner =
      BindOrThrow("SELECT (SELECT 1 AS x WHERE (SELECT x)>0)", catalog);
  ASSERT_EQ(3U, private_owner.query_count());
  EXPECT_TRUE(private_owner.root_query().captures.empty());
  EXPECT_TRUE(private_owner.query(BoundQueryId{1}).captures.empty());
  ASSERT_EQ(1U, private_owner.query(BoundQueryId{2}).captures.size());
  const auto* context =
      std::get_if<BoundQueryContextCapture>(&private_owner.query(BoundQueryId{2}).captures[0]);
  ASSERT_NE(nullptr, context);
  EXPECT_EQ(BoundQueryId{1}, context->query);
}

TEST(Binder, ReusesOriginalAliasSourcesAndRetainsDiscardedContextWithoutValueIds) {
  const auto catalog = TestCatalog();
  struct Case {
    std::string_view sql;
    BoundQueryId child;
  };
  for (const Case& test_case : {
           Case{"SELECT o.Name AS x FROM Items o "
                "WHERE (SELECT x FROM Items o LIMIT 1) IS NOT NULL",
                BoundQueryId{1}},
           Case{"SELECT o.Name||o.Name AS x FROM Items o WHERE (SELECT x) IS NOT NULL",
                BoundQueryId{1}},
           Case{"SELECT (SELECT 1),o.Name AS x FROM Items o WHERE (SELECT x) IS NOT NULL",
                BoundQueryId{2}},
           Case{"SELECT o.*,(SELECT 1),o.Name AS x FROM Items o "
                "WHERE (SELECT x) IS NOT NULL",
                BoundQueryId{2}},
           Case{"SELECT o.*,(SELECT 1),o.Name AS x FROM Items o "
                "WHERE (SELECT x) IS NOT NULL "
                "UNION ALL SELECT *,1,Name FROM Items ORDER BY 7",
                BoundQueryId{2}},
       }) {
    SCOPED_TRACE(test_case.sql);
    const BoundSelect bound = BindOrThrow(test_case.sql, catalog);
    const BoundQuery& query = bound.query(test_case.child);
    ASSERT_EQ(2U, query.captures.size());
    const auto& context = std::get<BoundQueryContextCapture>(query.captures.at(0));
    const auto& source = std::get<BoundSourceCapture>(query.captures.at(1));
    EXPECT_EQ(BoundQueryId{0}, context.query);
    EXPECT_EQ(BoundSourceId{0}, source.identity.source);
    EXPECT_TRUE(source.value.has_value());
  }
  const BoundSelect discarded = BindOrThrow(
      "SELECT o.Name AS x FROM Items o WHERE EXISTS(SELECT x ORDER BY (SELECT x) LIMIT 1)",
      catalog);
  ASSERT_EQ(2U, discarded.query_count());
  const BoundQuery& query = discarded.query(BoundQueryId{1});
  ASSERT_EQ(2U, query.captures.size());
  EXPECT_TRUE(query.order_by.empty());
  const auto& context = std::get<BoundQueryContextCapture>(query.captures.at(0));
  const auto& source = std::get<BoundSourceCapture>(query.captures.at(1));
  EXPECT_EQ(BoundQueryId{0}, context.query);
  EXPECT_EQ(BoundSourceId{0}, source.identity.source);
  EXPECT_FALSE(source.value.has_value());
}

TEST(Binder, PreservesOuterAliasVisibilityPrecedenceAndScopeBarriers) {
  const auto catalog = TestCatalog();
  for (const std::string_view sql : {
           "SELECT Name AS x,id AS x FROM Items WHERE (SELECT x) IS NOT NULL",
           "SELECT 1 AS x FROM Items WHERE (SELECT 2 AS id WHERE (SELECT id)=2)=2",
           "SELECT \"later\" AS x,id AS later FROM Items WHERE (SELECT x)='later'",
           "SELECT id AS x FROM Items WHERE EXISTS(SELECT x ORDER BY x LIMIT 1)",
       }) {
    SCOPED_TRACE(sql);
    EXPECT_TRUE(BindSelectStatement(ParseTree(sql), catalog).has_value());
  }
  for (const std::string_view sql : {
           "SELECT Name AS x FROM Items o WHERE (SELECT o.x) IS NOT NULL",
           "SELECT Name AS x FROM Items WHERE (SELECT 1 LIMIT x)>0",
           "SELECT Name AS x FROM Items WHERE (SELECT 1 LIMIT (SELECT x))>0",
           "SELECT Name AS x FROM Items WHERE (SELECT 1 LIMIT 1 OFFSET (SELECT x))>0",
       }) {
    SCOPED_TRACE(sql);
    const auto rejected = BindSelectStatement(ParseTree(sql), catalog);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(BindErrorCode::kNoSuchColumn, rejected.error().code);
  }
}

TEST(Binder, KeepsOuterAggregateAliasErrorsInTheirDefiningClause) {
  const auto catalog = TestCatalog();
  for (const std::string_view sql : {
           "SELECT sum(Score) AS x FROM Items WHERE (SELECT x)>0",
           "SELECT sum(Score) AS x FROM Items GROUP BY (SELECT x)",
           "SELECT sum(a.Score) AS x FROM Items a JOIN Items b ON (SELECT x)=b.id",
       }) {
    SCOPED_TRACE(sql);
    ExpectBindError(sql, catalog, BindErrorCode::kAggregateMisuse,
                    "misuse of aggregate function through result alias");
  }
  for (const std::string_view sql : {
           "SELECT sum(Score) AS x FROM Items HAVING (SELECT x)>0",
           "SELECT sum(Score) AS x FROM Items ORDER BY (SELECT x)",
       }) {
    SCOPED_TRACE(sql);
    ExpectBindError(sql, catalog, BindErrorCode::kUnsupportedFeature,
                    "outer-owned aggregates are not supported");
  }
}

TEST(Binder, ChecksAliasTargetWidthBeforeTheQueryDefinitionBoundary) {
  const auto catalog = TestCatalog();
  ExpectBindError("SELECT EXISTS(SELECT (SELECT 1,2) AS x WHERE EXISTS(SELECT x))", catalog,
                  BindErrorCode::kColumnCountMismatch, "row value misused");
  ExpectBindError("SELECT EXISTS(SELECT *,(SELECT 1,2) AS x FROM Items WHERE EXISTS(SELECT x))",
                  catalog, BindErrorCode::kColumnCountMismatch, "row value misused");
  for (const std::string_view sql : {
           "SELECT (SELECT 1) AS x WHERE (SELECT x)>0",
           "SELECT coalesce(1,(SELECT 1)) AS x WHERE (SELECT x)>0",
           "SELECT o.*,(SELECT 1),coalesce(1,(SELECT 2)) AS x FROM Items o "
           "WHERE (SELECT x)>0",
           "SELECT *,EXISTS(SELECT 1) AS x FROM Items WHERE (SELECT x)>0",
           "SELECT EXISTS(SELECT coalesce(1,(SELECT 1,2)) AS x WHERE EXISTS(SELECT x))",
           "SELECT EXISTS(SELECT coalesce(1,(SELECT 1,2)) AS x WHERE (SELECT x)>0)",
       }) {
    SCOPED_TRACE(sql);
    ExpectBindError(sql, catalog, BindErrorCode::kUnsupportedFeature,
                    "outer result-alias definitions containing queries are not supported");
  }
}

TEST(Binder, ResolvesOuterSourcesWithCanonicalQualificationAndFallbackRules) {
  struct Case {
    std::string_view sql;
    std::size_t captures;
  };
  const auto catalog = TestCatalog();
  for (const Case& test_case : {
           Case{"SELECT (SELECT Name FROM Items i) FROM Items o", 0},
           Case{"SELECT (SELECT o.Name FROM CustomCollation o) FROM Items o", 1},
           Case{"SELECT (SELECT main.o.Name) FROM Items o", 1},
           Case{"SELECT (SELECT \"Name\") FROM Items o", 1},
           Case{"SELECT (SELECT TRUE) FROM Items o", 1},
           Case{"SELECT (VALUES(o.Name),(o.Name)) FROM Items o", 1},
           Case{"SELECT (SELECT o.rowid), (SELECT o._rowid_) FROM Items o", 1},
           Case{"SELECT (SELECT id) FROM Items a FULL JOIN Items b USING(id)", 2},
       }) {
    SCOPED_TRACE(test_case.sql);
    const BoundSelect bound = BindOrThrow(test_case.sql, catalog);
    EXPECT_EQ(test_case.captures, bound.query(BoundQueryId{1}).captures.size());
    EXPECT_TRUE(bound.root_query().captures.empty());
  }
  ExpectBindError("SELECT (SELECT Items.Name) FROM Items o", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: Items.Name");
  ExpectBindError("SELECT (SELECT z.Name FROM Items i) FROM Items o", catalog,
                  BindErrorCode::kNoSuchColumn, "no such column: z.Name");
  ExpectBindError("SELECT (SELECT (SELECT Name) FROM Items a JOIN Items b) FROM Items o", catalog,
                  BindErrorCode::kAmbiguousColumn, "ambiguous column name: Name");
  for (const std::string_view sql : {
           "SELECT (SELECT 1 LIMIT o.id) FROM Items o",
           "SELECT (SELECT 1 LIMIT (SELECT o.id)) FROM Items o",
           "SELECT (SELECT 1 LIMIT 1 OFFSET (SELECT o.id)) FROM Items o",
       }) {
    SCOPED_TRACE(sql);
    ExpectBindError(sql, catalog, BindErrorCode::kNoSuchColumn, "no such column: o.id");
  }
}

TEST(Binder, RetainsOwnAggregateBindingLeavesWithoutSnapshottingAncestorInputs) {
  const auto catalog = TestCatalog();
  const BoundSelect representative = BindOrThrow(
      "SELECT count(*),(SELECT o.Name),EXISTS(SELECT o.Score ORDER BY o.rowid) FROM Items o",
      catalog);
  const auto& root = std::get<BoundSelectCore>(representative.query_cores()[0]);
  ASSERT_TRUE(root.aggregate.has_value());
  EXPECT_TRUE(root.aggregate->source_leaves.empty());
  ASSERT_EQ(3U, root.aggregate->representative_leaves.size());
  const std::array columns{BoundSourceColumnId{1}, BoundSourceColumnId{2}, BoundSourceColumnId{3}};
  for (std::size_t index = 0; index < columns.size(); ++index) {
    EXPECT_EQ((BoundSourceLeafIdentity{.source = BoundSourceId{0}, .column = columns[index]}),
              BoundSourceLeafIdentityOf(
                  representative.expression(root.aggregate->representative_leaves[index]),
                  representative.source_columns()));
  }
  const BoundSelect incoming = BindOrThrow(
      "SELECT (SELECT sum(i.Score+o.Score) FROM Items i GROUP BY i.id) FROM Items o", catalog);
  const BoundQueryView child{incoming, BoundQueryId{1}};
  ASSERT_EQ(1U, child.query().captures.size());
  const auto& inner = std::get<BoundSelectCore>(child.query_cores()[0]);
  ASSERT_TRUE(inner.aggregate.has_value());
  ASSERT_EQ(2U, inner.aggregate->source_leaves.size());
  EXPECT_TRUE(inner.aggregate->representative_leaves.empty());
  for (const BoundExpressionId leaf : inner.aggregate->source_leaves) {
    const auto identity =
        BoundSourceLeafIdentityOf(incoming.expression(leaf), incoming.source_columns());
    ASSERT_TRUE(identity.has_value());
    EXPECT_EQ(inner.table_sources[0].id, identity->source);
  }
  ExpectBindError("SELECT (SELECT sum(o.Score) FROM Items i) FROM Items o", catalog,
                  BindErrorCode::kUnsupportedFeature, "outer-owned aggregates are not supported");
}

TEST(Binder, PublishesGlobalOrderingRetentionWithTheNativeSimpleCountException) {
  struct Case {
    std::string_view sql;
    std::size_t representatives;
  };
  const auto catalog = TestCatalog();
  for (const Case& test_case :
       {Case{"SELECT count(*) FROM Items o ORDER BY o.Name", 0},
        Case{"SELECT count() FROM Items o ORDER BY (SELECT o.Name)", 0},
        Case{"SELECT DISTINCT count(*) FROM Items o ORDER BY (SELECT o.Name)", 0},
        Case{"SELECT count(*) FROM sqlite_schema s ORDER BY (SELECT s.sql)", 0},
        Case{"SELECT count(*) FROM Items o WHERE o.id>0 ORDER BY (SELECT o.Name)", 1},
        Case{"SELECT sum(o.id) FROM Items o ORDER BY (SELECT o.Name)", 1},
        Case{"SELECT count(*) FROM Items o ORDER BY sum(o.id),(SELECT o.Name)", 1},
        Case{"SELECT count(*) FILTER(WHERE 1) FROM Items o ORDER BY (SELECT o.Name)", 1},
        Case{"SELECT count(o.id) FROM Items o ORDER BY (SELECT o.Name)", 1},
        Case{"SELECT count(*) COLLATE BINARY FROM Items o ORDER BY (SELECT o.Name)", 1},
        Case{"SELECT +count(*) FROM Items o ORDER BY (SELECT o.Name)", 1},
        Case{"SELECT likely(count(*)) FROM Items o ORDER BY (SELECT o.Name)", 1}}) {
    SCOPED_TRACE(test_case.sql);
    const BoundSelect bound = BindOrThrow(test_case.sql, catalog);
    const auto& core = std::get<BoundSelectCore>(bound.query_cores()[0]);
    ASSERT_TRUE(core.aggregate.has_value());
    EXPECT_EQ(test_case.representatives, core.aggregate->representative_leaves.size());
  }
  constexpr std::string_view limited =
      "SELECT count(*) FROM Items o ORDER BY (SELECT o.Name),(SELECT o.Score)";
  ExpectBindError(limited, catalog, BindErrorCode::kAggregateTermLimitExceeded,
                  "too many aggregate terms", BindEnvironment::Core(),
                  BindOptions{.maximum_aggregate_terms = 1});
  const BoundSelect accepted = BindOrThrow(limited, catalog, BindEnvironment::Core(),
                                           BindOptions{.maximum_aggregate_terms = 2});
  const auto& core = std::get<BoundSelectCore>(accepted.query_cores()[0]);
  ASSERT_TRUE(core.aggregate.has_value());
  EXPECT_TRUE(core.aggregate->representative_leaves.empty());
}

TEST(Binder, KeepsOnlyBindingCapturesForElidedGlobalOrdering) {
  struct Case {
    std::string_view sql;
    bool value;
  };
  const auto catalog = TestCatalog();
  for (const Case& test_case :
       {Case{"SELECT (SELECT count(*) FROM Items i ORDER BY o.Name) FROM Items o", false},
        Case{"SELECT (SELECT sum(i.id) FROM Items i ORDER BY o.Name) FROM Items o", false},
        Case{"SELECT (SELECT sum(i.id) FROM Items i GROUP BY i.id ORDER BY o.Name) "
             "FROM Items o",
             true}}) {
    SCOPED_TRACE(test_case.sql);
    const BoundSelect bound = BindOrThrow(test_case.sql, catalog);
    const BoundQuery& child = bound.query(BoundQueryId{1});
    ASSERT_EQ(1U, child.captures.size());
    const auto* capture = std::get_if<BoundSourceCapture>(&child.captures[0]);
    ASSERT_NE(nullptr, capture);
    EXPECT_EQ(BoundSourceId{0}, capture->identity.source);
    EXPECT_EQ(test_case.value, capture->value.has_value());
  }
}

TEST(Binder, CountsNestedResolutionAndNormalizedJoinHeightsTogether) {
  const auto catalog = TestCatalog();
  struct Case {
    std::string_view prefix;
    std::string_view suffix;
    std::size_t accepted_terms;
    bool joined = true;
  };
  const std::array cases{
      Case{.prefix = "SELECT (", .suffix = ")", .accepted_terms = 998},
      Case{.prefix = "SELECT 0+(", .suffix = ")", .accepted_terms = 997},
      Case{.prefix = "SELECT (SELECT (", .suffix = "))", .accepted_terms = 995},
      Case{.prefix = "SELECT (", .suffix = ")", .accepted_terms = 499, .joined = false},
  };
  for (const Case& test_case : cases) {
    for (const std::size_t extra : {0U, 1U}) {
      SCOPED_TRACE(::testing::Message{} << test_case.prefix << '/' << test_case.joined << '/'
                                        << extra);
      std::string sql{test_case.prefix};
      sql.append(test_case.joined ? "SELECT 1 FROM Items a JOIN Items b ON " : "SELECT 1 WHERE ");
      sql.append("1");
      for (std::size_t term = 1; term < test_case.accepted_terms + extra; ++term) {
        sql.append(" AND 1");
      }
      sql.append(test_case.suffix);
      const auto bound = BindSelectStatement(ParseTree(sql), catalog);
      if (extra == 0U) {
        ASSERT_TRUE(bound.has_value()) << bound.error().detail;
      } else {
        ASSERT_FALSE(bound.has_value());
        EXPECT_EQ(BindErrorCode::kExpressionDepthExceeded, bound.error().code);
      }
    }
  }
}

TEST(Binder, AppliesThePinnedCumulativeNestedSelectResolutionLimit) {
  std::string sql = "SELECT 7";
  for (std::size_t depth = 0; depth < 44U; ++depth) {
    sql = "SELECT (" + sql + ")";
  }
  const auto bound = BindSelectStatement(ParseTree(sql), TestCatalog());
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(BindErrorCode::kExpressionDepthExceeded, bound.error().code);
}

TEST(Binder, AppliesThePinnedCorrelatedSourceResolutionHeightBoundary) {
  for (const std::size_t depth : {42U, 43U}) {
    SCOPED_TRACE(depth);
    std::string expression = "o.id";
    for (std::size_t index = 0; index < depth; ++index) {
      expression = "(SELECT " + expression + ")";
    }
    const auto bound =
        BindSelectStatement(ParseTree("SELECT " + expression + " FROM Items o"), TestCatalog());
    if (depth == 42U) {
      ASSERT_TRUE(bound.has_value()) << bound.error().detail;
    } else {
      ASSERT_FALSE(bound.has_value());
      EXPECT_EQ(BindErrorCode::kExpressionDepthExceeded, bound.error().code);
    }
  }
}

TEST(Binder, ExpandsWildcardsAndPublishesDirectColumnMetadata) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  BoundSelect select =
      BindOrThrow("SELECT *, q._rowid_ AS hidden, (q.Name) AS n, q.rowid FROM Items AS q", catalog);

  ASSERT_TRUE(select.valid());
  EXPECT_EQ("SELECT *, q._rowid_ AS hidden, (q.Name) AS n, q.rowid FROM Items AS q",
            select.source().bytes());
  ASSERT_EQ(catalog.get(), select.catalog());
  ASSERT_EQ(1U, select.table_sources().size());
  EXPECT_EQ(BoundSourceKind::kCatalogTable, select.table_sources().front().kind);
  EXPECT_EQ(TableId{0}, select.table_sources().front().table);

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

TEST(Binder, PublishesDistinctOccurrenceOwnersAcrossCompoundSourceScopes) {
  const BoundSelect select = BindOrThrow(
      "SELECT a.id,a._rowid_ FROM Items AS a "
      "UNION ALL VALUES(0,0) "
      "UNION ALL SELECT b.id,b._rowid_ FROM Items AS b",
      TestCatalog());
  ASSERT_EQ(3U, select.query_cores().size());
  const auto* left = std::get_if<BoundSelectCore>(&select.query_cores()[0]);
  const auto* right = std::get_if<BoundSelectCore>(&select.query_cores()[2]);
  ASSERT_NE(nullptr, left);
  ASSERT_NE(nullptr, right);
  ASSERT_EQ(1U, left->table_sources.size());
  ASSERT_EQ(1U, right->table_sources.size());
  EXPECT_EQ(left->table_sources.front().table, right->table_sources.front().table);
  EXPECT_EQ(BoundSourceId{0}, left->table_sources.front().id);
  EXPECT_EQ(BoundSourceId{1}, right->table_sources.front().id);
  EXPECT_NE(left->table_sources.front().id, right->table_sources.front().id);
  for (const BoundSelectCore* core : std::array{left, right}) {
    const auto source_columns =
        select.source_columns().subspan(core->source_column_begin, core->source_column_count);
    ASSERT_EQ(5U, source_columns.size());
    for (const BoundSourceColumn& column : source_columns) {
      EXPECT_EQ(core->table_sources.front().id, column.source);
    }
    EXPECT_TRUE(source_columns[0].rowid_alias);
    EXPECT_FALSE(source_columns[3].rowid_alias);
    const auto* rowid = std::get_if<BoundRowIdExpression>(
        &select.expression(core->result_columns[1].expression).payload);
    ASSERT_NE(nullptr, rowid);
    EXPECT_EQ(core->table_sources.front().id, rowid->source);
  }
}

TEST(Binder, NormalizesOwnedRowIdAliasesThroughOneSourceLeafContract) {
  const BoundSelect select = BindOrThrow(
      "SELECT id,_rowid_,rowid,Name FROM Items "
      "UNION ALL SELECT id,_rowid_,rowid,Name FROM Items",
      TestCatalog());
  std::array<BoundSourceLeafIdentity, 2> rowids;
  for (std::size_t index = 0; index < rowids.size(); ++index) {
    const auto& core = std::get<BoundSelectCore>(select.query_cores()[index]);
    const auto identity = [&](std::size_t result) {
      return RequiredOptional(BoundSourceLeafIdentityOf(
          select.expression(core.result_columns[result].expression), select.source_columns()));
    };
    rowids[index] = identity(1);
    EXPECT_EQ(identity(0), rowids[index]);
    EXPECT_TRUE(rowids[index].is_rowid());
    EXPECT_EQ(core.table_sources.front().id, rowids[index].source);
    const BoundSourceLeafIdentity declared_rowid = identity(2);
    EXPECT_FALSE(declared_rowid.is_rowid());
    EXPECT_EQ(BoundSourceColumnId{static_cast<std::uint32_t>(core.source_column_begin + 3U)},
              declared_rowid.column);
    EXPECT_NE(rowids[index], declared_rowid);
    EXPECT_EQ(core.table_sources.front().id, identity(3).source);
  }
  EXPECT_NE(rowids[0], rowids[1]);

  const BoundSelect constant = BindOrThrow("SELECT 1", TestCatalog());
  EXPECT_FALSE(BoundSourceLeafIdentityOf(ResultExpression(constant, 0), constant.source_columns())
                   .has_value());
}

TEST(Binder, AllocatesOnlyRealOccurrencesAndSeparatesSchemaSources) {
  const BoundSelect select = BindOrThrow(
      "SELECT 0,'zero' UNION ALL VALUES(1,'one') "
      "UNION ALL SELECT rowid,name FROM sqlite_schema "
      "UNION ALL SELECT _rowid_,Name FROM Items",
      TestCatalog());
  ASSERT_EQ(4U, select.query_cores().size());
  EXPECT_TRUE(std::get<BoundSelectCore>(select.query_cores()[0]).table_sources.empty());
  EXPECT_TRUE(std::holds_alternative<BoundValuesCore>(select.query_cores()[1]));
  for (std::size_t index = 2; index < select.query_cores().size(); ++index) {
    const auto& core = std::get<BoundSelectCore>(select.query_cores()[index]);
    ASSERT_EQ(1U, core.table_sources.size());
    EXPECT_EQ(BoundSourceId{static_cast<std::uint32_t>(index - 2U)}, core.table_sources.front().id);
    const auto* rowid = std::get_if<BoundRowIdExpression>(
        &select.expression(core.result_columns[0].expression).payload);
    ASSERT_NE(nullptr, rowid);
    EXPECT_EQ(core.table_sources.front().id, rowid->source);
    for (const BoundSourceColumn& column :
         select.source_columns().subspan(core.source_column_begin, core.source_column_count)) {
      EXPECT_EQ(core.table_sources.front().id, column.source);
      if (core.table_sources.front().kind == BoundSourceKind::kSchemaTable) {
        EXPECT_FALSE(column.rowid_alias);
        EXPECT_FALSE(column.catalog_column.has_value());
      }
    }
  }
}

TEST(StatementBinder, PreservesSingleTargetSourceOwnershipAndRowIdFacts) {
  const std::array queries{
      "INSERT INTO Items(id) VALUES(1)",
      "UPDATE Items SET Score=Score+1 WHERE _rowid_=?1",
      "DELETE FROM Items WHERE oid=?1",
  };
  for (const std::string_view sql : queries) {
    SCOPED_TRACE(sql);
    const BoundStatement statement = BindStatementOrThrow(sql, MutationCatalog());
    std::visit(
        [&](const auto& bound) {
          using Bound = std::decay_t<decltype(bound)>;
          if constexpr (std::is_same_v<Bound, BoundUpdate> || std::is_same_v<Bound, BoundDelete>) {
            ASSERT_FALSE(bound.source_columns().empty());
            for (const BoundSourceColumn& column : bound.source_columns()) {
              EXPECT_EQ(BoundSourceId{0}, column.source);
              EXPECT_EQ(column.catalog_column == bound.target().rowid_alias, column.rowid_alias);
            }
            for (const BoundExpression& expression : bound.expressions()) {
              if (const auto* rowid = std::get_if<BoundRowIdExpression>(&expression.payload);
                  rowid != nullptr) {
                EXPECT_EQ(BoundSourceId{0}, rowid->source);
              }
            }
          } else if constexpr (std::is_same_v<Bound, BoundInsert>) {
            for (const BoundExpression& expression : bound.expressions()) {
              EXPECT_FALSE(BoundSourceLeafIdentityOf(expression, {}).has_value());
            }
          } else {
            ADD_FAILURE() << "expected a single-target mutation";
          }
        },
        statement);
  }
}

TEST(Binder, RejectsInvalidTrustedColumnIdsInsteadOfHidingThemAsNonLeaves) {
  const BoundSelect select = BindOrThrow("SELECT id FROM Items", TestCatalog());
  const BoundExpression invalid{
      .payload =
          BoundColumnExpression{
              .column =
                  BoundSourceColumnId{static_cast<std::uint32_t>(select.source_columns().size())},
          },
  };
  EXPECT_DEATH(static_cast<void>(BoundSourceLeafIdentityOf(invalid, select.source_columns())),
               "terminat");
}

TEST(Binder, ResolvesJoinedAliasesActualMatchesAndWildcardOrder) {
  const BoundSelect select =
      BindOrThrow("SELECT q.id,q.key,q.* FROM Items AS q CROSS JOIN wr AS q", TestCatalog());
  ASSERT_EQ(2U, select.table_sources().size());
  ASSERT_EQ(8U, select.result_columns().size());
  EXPECT_EQ("id", select.result_columns()[2].name);
  EXPECT_EQ("Name", select.result_columns()[3].name);
  EXPECT_EQ("key", select.result_columns()[7].name);
  const auto identity = [&](std::size_t result) {
    return RequiredOptional(
        BoundSourceLeafIdentityOf(ResultExpression(select, result), select.source_columns()));
  };
  EXPECT_EQ(BoundSourceId{0}, identity(0).source);
  EXPECT_EQ(BoundSourceId{1}, identity(1).source);
  EXPECT_EQ(SourceJoinKind::kCross, select.table_sources()[1].join);
  EXPECT_EQ(5U, select.table_sources()[1].source_column_begin);
  EXPECT_EQ(1U, select.table_sources()[1].source_column_count);

  const BoundSelect hidden = BindOrThrow("SELECT q.oid FROM wr AS q,Items AS q", TestCatalog());
  const auto* rowid = std::get_if<BoundRowIdExpression>(&ResultExpression(hidden, 0).payload);
  ASSERT_NE(nullptr, rowid);
  EXPECT_EQ(BoundSourceId{1}, rowid->source);
}

TEST(Binder, RejectsJoinedAmbiguityBeforeAliasBooleanOrDqsFallback) {
  const std::array queries{
      "SELECT id FROM Items a,Items b",
      "SELECT a.id FROM Items a,Items a",
      "SELECT oid FROM Items a,Items b",
      "SELECT \"id\" FROM Items a,Items b",
      "SELECT a.Name AS id FROM Items a,Items b WHERE id>0",
      "SELECT TRUE FROM Items a,Items b",
  };
  for (const std::string_view sql : queries) {
    SCOPED_TRACE(sql);
    const auto bound = BindSelectStatement(ParseTree(sql), TestCatalog());
    ASSERT_FALSE(bound.has_value());
    EXPECT_EQ(BindErrorCode::kAmbiguousColumn, bound.error().code);
    EXPECT_TRUE(bound.error().detail.starts_with("ambiguous column name: "));
  }
  const BoundSelect fallback =
      BindOrThrow("SELECT \"missing\" FROM Items a,Items b", TestCatalog());
  EXPECT_EQ("missing", std::get<BoundLiteralExpression>(ResultExpression(fallback, 0).payload)
                           .value.text_value()
                           ->bytes());
  const auto aggregate = BindSelectStatement(
      ParseTree("SELECT count() FROM Items a CROSS JOIN Items b"), TestCatalog());
  ASSERT_TRUE(aggregate.has_value());
  const auto& core = std::get<BoundSelectCore>(aggregate->query_cores().front());
  ASSERT_TRUE(core.aggregate.has_value());
  EXPECT_EQ(2U, core.table_sources.size());
  EXPECT_EQ(1U, core.aggregate->invocations.size());
}

TEST(Binder, ResolvesGeneratedWildcardColumnsAndRejectsDuplicateQualifierAmbiguity) {
  const std::array queries{
      "SELECT q.* FROM Items q,Items q",
      "SELECT * FROM Items,Items",
  };
  for (const std::string_view sql : queries) {
    SCOPED_TRACE(sql);
    const auto rejected = BindSelectStatement(ParseTree(sql), TestCatalog());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(BindErrorCode::kAmbiguousColumn, rejected.error().code);
    EXPECT_TRUE(rejected.error().detail.starts_with("ambiguous column name: main."));
    EXPECT_TRUE(rejected.error().detail.ends_with(".id"));
  }
}

TEST(Binder, BindsOnAliasesAndInnerForwardSourcesButRejectsLeftForwardScope) {
  const auto catalog = TestCatalog();
  const BoundSelect aliased =
      BindOrThrow("SELECT a.id AS key FROM Items a LEFT JOIN Items b ON key=b.id", catalog);
  ASSERT_EQ(2U, aliased.table_sources().size());
  EXPECT_TRUE(aliased.table_sources()[1].on.has_value());
  const BoundSelect forward = BindOrThrow(
      "SELECT a.id FROM Items a JOIN Items b ON c.id=b.id JOIN Items c ON c.id=a.id", catalog);
  EXPECT_EQ(3U, forward.table_sources().size());
  const auto invalid = BindSelectStatement(
      ParseTree(
          "SELECT a.id FROM Items a LEFT JOIN Items b ON c.id=b.id JOIN Items c ON c.id=a.id"),
      catalog);
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(BindErrorCode::kJoinConditionScope, invalid.error().code);
  EXPECT_EQ("ON clause references tables to its right", invalid.error().detail);
  const auto first = BindSelectStatement(ParseTree("SELECT id FROM Items ON id=1"), catalog);
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(BindErrorCode::kJoinConditionScope, first.error().code);
  EXPECT_EQ("a JOIN clause is required before ON", first.error().detail);
  const auto aggregate = BindSelectStatement(
      ParseTree("SELECT count() FROM Items a JOIN Items b ON count()>0"), catalog);
  ASSERT_FALSE(aggregate.has_value());
  EXPECT_EQ(BindErrorCode::kAggregateMisuse, aggregate.error().code);
}

TEST(Binder, PublishesMergedColumnProvenanceWithoutChangingPhysicalOwnership) {
  const BoundSelect select =
      BindOrThrow("SELECT id,a.id,b.id FROM Items a LEFT JOIN Items b USING(id,id)", TestCatalog());
  const auto sources = select.table_sources();
  ASSERT_EQ(2U, sources.size());
  const auto right =
      BoundSourceColumnId{static_cast<std::uint32_t>(sources[1].source_column_begin)};
  EXPECT_FALSE(select.source_columns()[0].merged_into.has_value());
  EXPECT_EQ(BoundSourceColumnId{0}, select.source_columns()[right.value()].merged_into);
  EXPECT_EQ(sources[1].id, select.source_columns()[right.value()].source);
  ASSERT_TRUE(sources[1].on.has_value());
  EXPECT_TRUE(
      std::holds_alternative<BoundBinaryExpression>(select.expression(*sources[1].on).payload));
  EXPECT_EQ(BoundSourceColumnId{0},
            std::get<BoundColumnExpression>(ResultExpression(select, 0).payload).column);
  EXPECT_EQ(BoundSourceColumnId{0},
            std::get<BoundColumnExpression>(ResultExpression(select, 1).payload).column);
  EXPECT_EQ(right, std::get<BoundColumnExpression>(ResultExpression(select, 2).payload).column);
  const BoundSelect collated =
      BindOrThrow("SELECT Name FROM Items a JOIN Items b USING(Name)", TestCatalog());
  const auto& comparison = std::get<BoundComparisonExpression>(
      collated.expression(*collated.table_sources()[1].on).payload);
  EXPECT_EQ(TypeAffinity::kBlob, comparison.affinity);
  EXPECT_EQ("NOCASE", collated.collations()[comparison.collation.value()].name);
}

TEST(Binder, ResolvesMergedDuplicateQualifiersThroughTheCanonicalWildcardResolver) {
  const auto catalog = TestCatalog();
  const BoundSelect natural =
      BindOrThrow("SELECT q.* FROM Items q NATURAL LEFT JOIN Items q", catalog);
  const std::size_t columns = natural.table_sources()[0].source_column_count;
  ASSERT_EQ(columns * 2U, natural.result_columns().size());
  for (std::size_t index = 0; index < natural.result_columns().size(); ++index) {
    EXPECT_EQ(BoundSourceColumnId{static_cast<std::uint32_t>(index % columns)},
              std::get<BoundColumnExpression>(ResultExpression(natural, index).payload).column);
  }
  const BoundSelect qualified =
      BindOrThrow("SELECT q.id FROM Items q JOIN Items q USING(id)", catalog);
  EXPECT_EQ(BoundSourceColumnId{0},
            std::get<BoundColumnExpression>(ResultExpression(qualified, 0).payload).column);
  const auto unrelated =
      BindSelectStatement(ParseTree("SELECT * FROM Items q JOIN Items q USING(id)"), catalog);
  ASSERT_FALSE(unrelated.has_value());
  EXPECT_EQ(BindErrorCode::kAmbiguousColumn, unrelated.error().code);
  const auto rowid =
      BindSelectStatement(ParseTree("SELECT q.rowid FROM Items q JOIN Items q USING(id)"), catalog);
  ASSERT_FALSE(rowid.has_value());
  EXPECT_EQ(BindErrorCode::kAmbiguousColumn, rowid.error().code);
}

TEST(Binder, UsesFirstVisibleLeftUsingColumnAndRejectsMissingOrIllegalClauses) {
  const auto catalog = TestCatalog();
  const BoundSelect prefix =
      BindOrThrow("SELECT a.id,b.id,c.id FROM Items a,Items b JOIN Items c USING(id)", catalog);
  const auto sources = prefix.table_sources();
  ASSERT_EQ(3U, sources.size());
  const auto third_begin = sources[2].source_column_begin;
  EXPECT_EQ(BoundSourceColumnId{0}, prefix.source_columns()[third_begin].merged_into);
  EXPECT_FALSE(prefix.source_columns()[sources[1].source_column_begin].merged_into.has_value());
  const auto ambiguous = BindSelectStatement(
      ParseTree("SELECT id FROM Items a,Items b JOIN Items c USING(id)"), catalog);
  ASSERT_FALSE(ambiguous.has_value());
  EXPECT_EQ(BindErrorCode::kAmbiguousColumn, ambiguous.error().code);
  struct Case {
    std::string_view sql;
    std::string_view detail;
  };
  const std::array cases{
      Case{.sql = "SELECT * FROM Items USING(id)",
           .detail = "a JOIN clause is required before USING"},
      Case{.sql = "SELECT * FROM Items a JOIN Items b USING(_rowid_)",
           .detail = "cannot join using column _rowid_ - column not present in both tables"},
      Case{.sql = "SELECT * FROM Items a JOIN Items b USING(absent)",
           .detail = "cannot join using column absent - column not present in both tables"},
      Case{.sql = "SELECT * FROM Items a NATURAL JOIN Items b ON 1",
           .detail = "a NATURAL join may not have an ON or USING clause"},
      Case{.sql = "SELECT * FROM Items a NATURAL JOIN Items b USING(id)",
           .detail = "a NATURAL join may not have an ON or USING clause"},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    const auto rejected = BindSelectStatement(ParseTree(test_case.sql), catalog);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(BindErrorCode::kJoinConditionScope, rejected.error().code);
    EXPECT_EQ(test_case.detail, rejected.error().detail);
  }
  const BoundSelect explicit_rowid =
      BindOrThrow("SELECT rowid FROM Items a JOIN Items b USING(rowid)", catalog);
  EXPECT_EQ(BoundSourceColumnId{3},
            std::get<BoundColumnExpression>(ResultExpression(explicit_rowid, 0).payload).column);
}

TEST(Binder, PublishesRightMergedValuesWithoutChangingUniquePhysicalQualifiers) {
  const BoundSelect bound =
      BindOrThrow("SELECT id,a.id,b.id FROM Items a RIGHT JOIN Items b USING(id)", TestCatalog());
  const auto right_begin = bound.table_sources()[1].source_column_begin;
  ASSERT_TRUE(bound.source_columns()[0].joined_value.has_value());
  const auto& merged = std::get<BoundAliasReferenceExpression>(ResultExpression(bound, 0).payload);
  EXPECT_EQ(BoundSourceColumnId{static_cast<std::uint32_t>(right_begin)},
            std::get<BoundColumnExpression>(bound.expression(merged.target).payload).column);
  EXPECT_EQ(BoundSourceColumnId{0},
            std::get<BoundColumnExpression>(ResultExpression(bound, 1).payload).column);
  EXPECT_EQ(BoundSourceColumnId{static_cast<std::uint32_t>(right_begin)},
            std::get<BoundColumnExpression>(ResultExpression(bound, 2).payload).column);
  EXPECT_EQ("INTEGER", bound.result_columns()[0].declared_type);
  const BoundSelect duplicate =
      BindOrThrow("SELECT q.id FROM Items q RIGHT JOIN Items q USING(id)", TestCatalog());
  EXPECT_EQ(BoundSourceColumnId{static_cast<std::uint32_t>(right_begin)},
            std::get<BoundColumnExpression>(ResultExpression(duplicate, 0).payload).column);
}

TEST(Binder, PublishesFullMergedCoalescesWithScopedProvenanceAndDeferredProperties) {
  const auto catalog = TestCatalog();
  const BoundSelect bound =
      BindOrThrow("SELECT id,a.id,b.id FROM Items a FULL JOIN Items b USING(id)", catalog);
  const auto& merged = std::get<BoundAliasReferenceExpression>(ResultExpression(bound, 0).payload);
  const BoundExpression& value = bound.expression(merged.target);
  EXPECT_TRUE(std::holds_alternative<BoundCoalesceExpression>(value.payload));
  EXPECT_EQ(TypeAffinity::kInteger, value.properties.affinity);
  EXPECT_EQ(std::nullopt, bound.result_columns()[0].declared_type);
  EXPECT_EQ(BoundSourceColumnId{0},
            std::get<BoundColumnExpression>(ResultExpression(bound, 1).payload).column);
  EXPECT_EQ(BoundSourceColumnId{5},
            std::get<BoundColumnExpression>(ResultExpression(bound, 2).payload).column);
  const BoundSelect subgroup = BindOrThrow(
      "SELECT q.id,c.id FROM Items q FULL JOIN Items q USING(id) FULL JOIN Items c USING(id)",
      catalog);
  const auto& scoped = std::get<BoundCoalesceExpression>(ResultExpression(subgroup, 0).payload);
  ASSERT_EQ(2U, scoped.arguments.size());
  EXPECT_EQ("q.id", subgroup.result_columns()[0].name);
  EXPECT_EQ(std::nullopt, subgroup.result_columns()[0].declared_type);
  EXPECT_EQ(BoundSourceColumnId{10},
            std::get<BoundColumnExpression>(ResultExpression(subgroup, 1).payload).column);
  const BoundSelect replaced = BindOrThrow(
      "SELECT q.id FROM Items q FULL JOIN Items q USING(id) RIGHT JOIN Items q USING(id)", catalog);
  EXPECT_EQ(BoundSourceColumnId{10},
            std::get<BoundColumnExpression>(ResultExpression(replaced, 0).payload).column);
  EXPECT_EQ("INTEGER", replaced.result_columns()[0].declared_type);
}

TEST(Binder, RejectsForwardOnThroughoutRightCoresWithoutAffectingOtherArms) {
  const auto catalog = TestCatalog();
  for (const std::string_view sql : {
           "SELECT a.id FROM Items a JOIN Items b ON c.id=b.id "
           "JOIN Items c ON a.id=c.id RIGHT JOIN Items d ON d.id=a.id",
           "SELECT a.id FROM Items a RIGHT JOIN Items b ON a.id=b.id "
           "JOIN Items c ON d.id=c.id JOIN Items d ON d.id=b.id",
       }) {
    const auto rejected = BindSelectStatement(ParseTree(sql), catalog);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(BindErrorCode::kJoinConditionScope, rejected.error().code);
    EXPECT_EQ("ON clause references tables to its right", rejected.error().detail);
  }
  const auto independent = BindOrThrow(
      "SELECT a.id FROM Items a JOIN Items b ON c.id=b.id JOIN Items c ON c.id=a.id "
      "UNION ALL SELECT a.id FROM Items a RIGHT JOIN Items b ON a.id=b.id",
      catalog);
  EXPECT_EQ(2U, independent.query_cores().size());
}

TEST(Binder, NormalizesRightUsingPredicatesFromAllPriorPhysicalContributors) {
  const auto catalog = TestCatalog();
  const BoundSelect bound = BindOrThrow(
      "SELECT a.id FROM Items a LEFT JOIN Items b USING(id) RIGHT JOIN Items c USING(id)", catalog);
  const auto on = bound.table_sources()[2].on;
  ASSERT_TRUE(on.has_value());
  const auto& equality = std::get<BoundComparisonExpression>(bound.expression(*on).payload);
  const auto* coalesce =
      std::get_if<BoundCoalesceExpression>(&bound.expression(equality.left).payload);
  ASSERT_NE(nullptr, coalesce);
  ASSERT_EQ(2U, coalesce->arguments.size());
  EXPECT_EQ(
      BoundSourceColumnId{0},
      std::get<BoundColumnExpression>(bound.expression(coalesce->arguments[0]).payload).column);
  EXPECT_EQ(
      BoundSourceColumnId{5},
      std::get<BoundColumnExpression>(bound.expression(coalesce->arguments[1]).payload).column);
  const auto ambiguous = BindSelectStatement(
      ParseTree("SELECT a.id FROM Items a,Items b RIGHT JOIN Items c USING(id)"), catalog);
  ASSERT_FALSE(ambiguous.has_value());
  EXPECT_EQ(BindErrorCode::kJoinConditionScope, ambiguous.error().code);
  EXPECT_EQ("ambiguous reference to id in USING()", ambiguous.error().detail);
}

TEST(Binder, EnforcesCombinedGeneratedJoinHeightAcrossSourcesAndWhere) {
  struct Case {
    std::size_t sources;
    std::size_t names;
    std::string_view where;
    bool accepted;
    std::string_view leading_on{};
    std::string_view trailing_on{};
  };
  const std::array cases{
      Case{.sources = 2, .names = 999, .where = "", .accepted = true},
      Case{.sources = 2, .names = 1000, .where = "", .accepted = false},
      Case{.sources = 3, .names = 499, .where = "", .accepted = true},
      Case{.sources = 3, .names = 500, .where = "", .accepted = false},
      Case{.sources = 2, .names = 998, .where = " WHERE 0", .accepted = true},
      Case{.sources = 2, .names = 999, .where = " WHERE 0", .accepted = false},
      Case{.sources = 2, .names = 997, .where = " WHERE t0.id>0", .accepted = true},
      Case{.sources = 2, .names = 998, .where = " WHERE t0.id>0", .accepted = false},
      Case{.sources = 2, .names = 998, .where = " WHERE id COLLATE BINARY>0", .accepted = true},
      Case{.sources = 2, .names = 999, .where = " WHERE id COLLATE BINARY>0", .accepted = false},
      Case{.sources = 10, .names = 111, .where = "", .accepted = true},
      Case{.sources = 64, .names = 16, .where = "", .accepted = false},
      Case{.sources = 2,
           .names = 997,
           .where = "",
           .accepted = true,
           .leading_on = " JOIN Items extra ON t0.id=extra.id"},
      Case{.sources = 2,
           .names = 998,
           .where = "",
           .accepted = false,
           .leading_on = " JOIN Items extra ON t0.id=extra.id"},
      Case{.sources = 2,
           .names = 998,
           .where = "",
           .accepted = true,
           .trailing_on = " JOIN Items extra ON t0.id=extra.id"},
      Case{.sources = 2,
           .names = 999,
           .where = "",
           .accepted = false,
           .trailing_on = " JOIN Items extra ON t0.id=extra.id"},
      Case{.sources = 2,
           .names = 998,
           .where = "",
           .accepted = false,
           .leading_on = " RIGHT JOIN Items extra USING(id)"},
  };
  const auto catalog = TestCatalog();
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sources);
    SCOPED_TRACE(test_case.names);
    SCOPED_TRACE(test_case.where);
    SCOPED_TRACE(test_case.leading_on);
    SCOPED_TRACE(test_case.trailing_on);
    std::string sql = "SELECT count() FROM Items t0";
    sql += test_case.leading_on;
    for (std::size_t source = 1; source < test_case.sources; ++source) {
      sql += " JOIN Items t" + std::to_string(source) + " USING(";
      for (std::size_t name = 0; name < test_case.names; ++name) {
        sql += name == 0U ? "id" : ",id";
      }
      sql += ')';
    }
    sql += test_case.trailing_on;
    sql += test_case.where;
    const auto bound = BindSelectStatement(ParseTree(sql), catalog);
    if (test_case.accepted) {
      ASSERT_TRUE(bound.has_value()) << bound.error().detail;
    } else {
      ASSERT_FALSE(bound.has_value());
      EXPECT_EQ(BindErrorCode::kExpressionDepthExceeded, bound.error().code);
      EXPECT_EQ(ErrorCode::kGeneric, bound.error().base_error_code());
      EXPECT_EQ("Expression tree is too large (maximum depth 1000)", bound.error().detail);
    }
  }
}

TEST(Binder, BindsSyntheticSchemaTableAndLegacyWildcardQualifier) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select =
      BindOrThrow("SELECT sqlite_master.*, rowid FROM main.sqlite_schema", catalog);

  ASSERT_EQ(1U, select.table_sources().size());
  EXPECT_EQ(BoundSourceKind::kSchemaTable, select.table_sources().front().kind);
  EXPECT_FALSE(select.table_sources().front().table.has_value());
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

TEST(StatementBinder, BindsInsertUpdateAndDeleteAgainstOneCatalogSnapshot) {
  const CatalogSnapshotPtr catalog = MutationCatalog();

  BoundStatement insert_statement =
      BindStatementOrThrow("INSERT INTO main.Items(Name, id, name) VALUES(?1, ?2, ?3)", catalog);
  const auto& insert = std::get<BoundInsert>(insert_statement);
  EXPECT_EQ(catalog.get(), insert.catalog());
  EXPECT_EQ((CatalogVersion{.schema_cookie = 23, .generation = 11}),
            insert.required_catalog_version());
  EXPECT_EQ(TableId{0}, insert.target().table);
  EXPECT_EQ(RootPageId{2}, insert.target().root_page);
  EXPECT_EQ(std::optional<ColumnId>{ColumnId{0}}, insert.target().rowid_alias);
  ASSERT_EQ(3U, insert.target().columns.size());
  EXPECT_EQ("Name", insert.target().columns[1].name);
  ASSERT_NE(nullptr, insert.target().columns[1].default_value);
  EXPECT_EQ("seed",
            RequiredOptional(insert.target().columns[1].default_value->text_value()).bytes());
  ASSERT_EQ(3U, insert.values().size());
  EXPECT_EQ(std::optional<ColumnId>{ColumnId{1}}, insert.values()[0].target.column);
  EXPECT_FALSE(insert.values()[0].target.rowid);
  EXPECT_TRUE(insert.values()[0].effective);
  EXPECT_EQ(std::optional<ColumnId>{ColumnId{0}}, insert.values()[1].target.column);
  EXPECT_TRUE(insert.values()[1].target.rowid);
  EXPECT_TRUE(insert.values()[1].effective);
  EXPECT_EQ(std::optional<ColumnId>{ColumnId{1}}, insert.values()[2].target.column);
  EXPECT_FALSE(insert.values()[2].effective);
  EXPECT_EQ(3U, insert.parameters().size());
  EXPECT_FALSE(insert.default_values());
  EXPECT_TRUE(insert.explicit_columns());

  BoundStatement update_statement =
      BindStatementOrThrow("UPDATE Items SET Name=?1, name=?2, rowid=id+1 WHERE Score>?3", catalog);
  const auto& update = std::get<BoundUpdate>(update_statement);
  ASSERT_EQ(3U, update.assignments().size());
  EXPECT_FALSE(update.assignments()[0].effective);
  EXPECT_TRUE(update.assignments()[1].effective);
  EXPECT_TRUE(update.assignments()[2].effective);
  EXPECT_TRUE(update.assignments()[2].target.rowid);
  EXPECT_TRUE(update.changes_rowid());
  EXPECT_TRUE(update.where_expression().has_value());
  EXPECT_EQ(3U, update.parameters().size());

  BoundStatement delete_statement = BindStatementOrThrow("DELETE FROM Items WHERE id=?1", catalog);
  const auto& delete_bound = std::get<BoundDelete>(delete_statement);
  EXPECT_EQ(TableId{0}, delete_bound.target().table);
  EXPECT_TRUE(delete_bound.where_expression().has_value());
  EXPECT_EQ(1U, delete_bound.parameters().size());

  BoundStatement indexed_statement =
      BindStatementOrThrow("INSERT INTO IndexedItems(Value) VALUES(?1)", catalog);
  const auto& indexed = std::get<BoundInsert>(indexed_statement);
  ASSERT_EQ(1U, indexed.target().indexes.size());
  const BoundIndexMaintenance& maintenance = indexed.target().indexes[0];
  EXPECT_EQ(IndexId{0}, maintenance.index);
  EXPECT_EQ(RootPageId{5}, maintenance.root_page);
  EXPECT_FALSE(maintenance.unique);
  EXPECT_EQ(1U, maintenance.key_term_count);
  ASSERT_EQ(2U, maintenance.terms.size());
  EXPECT_EQ(ColumnId{1}, maintenance.terms[0].column);
  EXPECT_FALSE(maintenance.terms[0].rowid);
  EXPECT_FALSE(maintenance.terms[1].column.has_value());
  EXPECT_TRUE(maintenance.terms[1].rowid);

  BoundStatement indexed_update_statement =
      BindStatementOrThrow("UPDATE IndexedItems SET Value=?1 WHERE id=?2", catalog);
  const auto& indexed_update = std::get<BoundUpdate>(indexed_update_statement);
  ASSERT_EQ(1U, indexed_update.target().indexes.size());
  EXPECT_EQ(IndexId{0}, indexed_update.target().indexes[0].index);

  BoundStatement indexed_delete_statement =
      BindStatementOrThrow("DELETE FROM IndexedItems WHERE id=?1", catalog);
  const auto& indexed_delete = std::get<BoundDelete>(indexed_delete_statement);
  ASSERT_EQ(1U, indexed_delete.target().indexes.size());
  EXPECT_EQ(IndexId{0}, indexed_delete.target().indexes[0].index);
}

TEST(StatementBinder, RejectsInvalidOrUnsafeMutationTargetsBeforePlanning) {
  const CatalogSnapshotPtr catalog = MutationCatalog();

  ExpectStatementBindError("INSERT INTO missing VALUES(1)", catalog, BindErrorCode::kNoSuchTable,
                           ErrorCode::kGeneric, "no such table");
  ExpectStatementBindError("INSERT INTO Items(missing) VALUES(1)", catalog,
                           BindErrorCode::kNoSuchColumn, ErrorCode::kGeneric, "no such column");
  ExpectStatementBindError("INSERT INTO Items VALUES(1, 2)", catalog,
                           BindErrorCode::kColumnCountMismatch, ErrorCode::kGeneric, "values for");
  ExpectStatementBindError("INSERT INTO Items(Name) DEFAULT VALUES", catalog,
                           BindErrorCode::kColumnCountMismatch, ErrorCode::kGeneric,
                           "0 values for 1 columns");
  ExpectStatementBindError("INSERT INTO Items(Name) VALUES(Name)", catalog,
                           BindErrorCode::kNoSuchColumn, ErrorCode::kGeneric, "no such column");
  ExpectStatementBindError("UPDATE IndexedItems SET Value=no_such_function()", catalog,
                           BindErrorCode::kNoSuchFunction, ErrorCode::kGeneric, "no such function");
  ExpectStatementBindError("DELETE FROM wr", catalog, BindErrorCode::kUnsupportedFeature,
                           ErrorCode::kGeneric, "table shape");
  ExpectStatementBindError("INSERT INTO DynamicDefault DEFAULT VALUES", catalog,
                           BindErrorCode::kUnsupportedFeature, ErrorCode::kGeneric,
                           "non-constant column defaults");
}

TEST(StatementBinder, PreservesPinnedDuplicateTargetAndDefaultValuesRules) {
  const CatalogSnapshotPtr catalog = MutationCatalog();

  BoundStatement insert_statement = BindStatementOrThrow(
      "INSERT INTO Items(Name, name, rowid, id) VALUES(?1, ?2, ?3, ?4)", catalog);
  const auto& insert = std::get<BoundInsert>(insert_statement);
  ASSERT_EQ(4U, insert.values().size());
  EXPECT_TRUE(insert.values()[0].effective);
  EXPECT_FALSE(insert.values()[1].effective);
  EXPECT_FALSE(insert.values()[2].effective);
  EXPECT_TRUE(insert.values()[3].effective);
  EXPECT_TRUE(insert.values()[2].target.rowid);
  EXPECT_TRUE(insert.values()[3].target.rowid);
  EXPECT_EQ(4U, insert.parameters().size());

  BoundStatement update_statement =
      BindStatementOrThrow("UPDATE Items SET Name=?1, name=?2, rowid=?3, id=?4", catalog);
  const auto& update = std::get<BoundUpdate>(update_statement);
  ASSERT_EQ(4U, update.assignments().size());
  EXPECT_FALSE(update.assignments()[0].effective);
  EXPECT_TRUE(update.assignments()[1].effective);
  EXPECT_FALSE(update.assignments()[2].effective);
  EXPECT_TRUE(update.assignments()[3].effective);
  EXPECT_TRUE(update.changes_rowid());

  BoundStatement defaults_statement =
      BindStatementOrThrow("INSERT INTO Items DEFAULT VALUES", catalog);
  const auto& defaults = std::get<BoundInsert>(defaults_statement);
  EXPECT_TRUE(defaults.default_values());
  EXPECT_FALSE(defaults.explicit_columns());
  EXPECT_TRUE(defaults.values().empty());

  BoundStatement positional_statement =
      BindStatementOrThrow("INSERT INTO Items VALUES(?1, ?2, ?3)", catalog);
  const auto& positional = std::get<BoundInsert>(positional_statement);
  ASSERT_EQ(3U, positional.values().size());
  EXPECT_FALSE(positional.explicit_columns());
  EXPECT_TRUE(positional.values()[0].target.rowid);
  EXPECT_EQ(std::optional<ColumnId>{ColumnId{2}}, positional.values()[2].target.column);

  BoundStatement shadowed_statement =
      BindStatementOrThrow("UPDATE Shadowed SET rowid=?1, _rowid_=?2", catalog);
  const auto& shadowed = std::get<BoundUpdate>(shadowed_statement);
  ASSERT_EQ(2U, shadowed.assignments().size());
  EXPECT_EQ(std::optional<ColumnId>{ColumnId{0}}, shadowed.assignments()[0].target.column);
  EXPECT_FALSE(shadowed.assignments()[0].target.rowid);
  EXPECT_FALSE(shadowed.assignments()[1].target.column.has_value());
  EXPECT_TRUE(shadowed.assignments()[1].target.rowid);
  EXPECT_TRUE(shadowed.assignments()[0].effective);
  EXPECT_TRUE(shadowed.assignments()[1].effective);
}

TEST(StatementBinder, BindsCreateTableAndTransactionControlMetadata) {
  const CatalogSnapshotPtr catalog = MutationCatalog();

  BoundStatement create_statement = BindStatementOrThrow(
      "CREATE TABLE IF NOT EXISTS main.\"New Table\" ("
      "id INTEGER PRIMARY KEY, "
      "name TEXT NOT NULL DEFAULT 'x', "
      "score REAL"
      ")",
      catalog);
  const auto& create = std::get<BoundCreateTable>(create_statement);
  EXPECT_EQ("New Table", create.table_name());
  EXPECT_TRUE(create.if_not_exists());
  EXPECT_FALSE(create.no_op());
  EXPECT_EQ(
      "CREATE TABLE \"New Table\" (id INTEGER PRIMARY KEY, name TEXT NOT NULL DEFAULT 'x', "
      "score REAL)",
      create.canonical_sql());
  ASSERT_EQ(3U, create.columns().size());
  EXPECT_EQ(std::optional<ColumnId>{ColumnId{0}}, create.rowid_alias());
  EXPECT_TRUE(create.columns()[1].not_null);
  ASSERT_NE(nullptr, create.columns()[1].default_value);
  EXPECT_EQ("x", RequiredOptional(create.columns()[1].default_value->text_value()).bytes());

  BoundStatement no_op_statement =
      BindStatementOrThrow("CREATE TABLE IF NOT EXISTS Items(a UNIQUE)", catalog);
  const auto& no_op = std::get<BoundCreateTable>(no_op_statement);
  EXPECT_TRUE(no_op.no_op());

  BoundStatement begin_statement = BindStatementOrThrow("BEGIN IMMEDIATE", catalog);
  const auto& begin = std::get<BoundBeginTransaction>(begin_statement);
  EXPECT_EQ(BeginTransactionMode::kImmediate, begin.mode);
  BoundStatement savepoint_statement = BindStatementOrThrow("SAVEPOINT \"CaseName\"", catalog);
  const auto& savepoint = std::get<BoundSavepoint>(savepoint_statement);
  EXPECT_EQ("CaseName", savepoint.name);
  EXPECT_TRUE(std::holds_alternative<BoundCommitTransaction>(
      BindStatementOrThrow("END TRANSACTION", catalog)));
  EXPECT_TRUE(std::holds_alternative<BoundRollbackToSavepoint>(
      BindStatementOrThrow("ROLLBACK TO SAVEPOINT CaseName", catalog)));
}

TEST(StatementBinder, MaterializesSupportedCreateDefaultsWithColumnAffinity) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  BoundStatement statement = BindStatementOrThrow(
      "CREATE TABLE Defaults("
      "a TEXT DEFAULT word, "
      "b INTEGER DEFAULT -2, "
      "c BLOB DEFAULT X'0A', "
      "d TEXT DEFAULT NULL, "
      "e NUMERIC DEFAULT true"
      ")",
      catalog);
  const auto& create = std::get<BoundCreateTable>(statement);
  ASSERT_EQ(5U, create.columns().size());

  ASSERT_NE(nullptr, create.columns()[0].default_value);
  EXPECT_EQ("word", RequiredOptional(create.columns()[0].default_value->text_value()).bytes());
  ASSERT_NE(nullptr, create.columns()[1].default_value);
  EXPECT_EQ(-2, RequiredOptional(create.columns()[1].default_value->integer_value()));
  ASSERT_NE(nullptr, create.columns()[2].default_value);
  EXPECT_EQ(1U, RequiredOptional(create.columns()[2].default_value->blob_value()).size_bytes());
  ASSERT_NE(nullptr, create.columns()[3].default_value);
  EXPECT_EQ(SqlValueType::kNull, create.columns()[3].default_value->type());
  ASSERT_NE(nullptr, create.columns()[4].default_value);
  EXPECT_EQ(1, RequiredOptional(create.columns()[4].default_value->integer_value()));
}

TEST(StatementBinder, BindsSimpleCreateIndexMetadata) {
  const CatalogSnapshotPtr catalog = MutationCatalog();
  BoundStatement statement = BindStatementOrThrow(
      "CREATE UNIQUE INDEX IF NOT EXISTS main.items_name_score "
      "ON Items((Name COLLATE NOCASE) DESC,Score,id)",
      catalog);
  const auto& create = std::get<BoundCreateIndex>(statement);
  EXPECT_EQ(catalog.get(), create.catalog());
  EXPECT_EQ(TableId{0}, create.table());
  EXPECT_EQ(RootPageId{2}, create.table_root_page());
  EXPECT_EQ("items_name_score", create.index_name());
  EXPECT_EQ("Items", create.table_name());
  EXPECT_TRUE(create.unique());
  EXPECT_FALSE(create.unique_not_null());
  EXPECT_TRUE(create.if_not_exists());
  EXPECT_FALSE(create.no_op());
  EXPECT_EQ("CREATE UNIQUE INDEX items_name_score ON Items((Name COLLATE NOCASE) DESC,Score,id)",
            create.canonical_sql());
  ASSERT_EQ(3U, create.terms().size());
  EXPECT_EQ(ColumnId{1}, create.terms()[0].column);
  EXPECT_EQ("NOCASE", create.terms()[0].collation_name);
  EXPECT_EQ(SortOrder::kDescending, create.terms()[0].order);
  EXPECT_FALSE(create.terms()[0].rowid);
  EXPECT_EQ(TypeAffinity::kReal, create.terms()[1].affinity);
  EXPECT_TRUE(create.terms()[2].rowid);

  BoundStatement no_op_statement = BindStatementOrThrow(
      "CREATE INDEX IF NOT EXISTS idx_items_value ON IndexedItems(no_such_column)", catalog);
  EXPECT_TRUE(std::get<BoundCreateIndex>(no_op_statement).no_op());

  const CatalogSnapshotPtr collated_catalog = TestCatalog();
  BoundStatement derived_statement =
      BindStatementOrThrow("CREATE INDEX items_name ON Items(Name)", collated_catalog);
  const auto& derived = std::get<BoundCreateIndex>(derived_statement);
  ASSERT_EQ(1U, derived.terms().size());
  EXPECT_EQ("NOCASE", derived.terms()[0].collation_name);
}

TEST(StatementBinder, DispatchesSelectAndValidatesTransactionBindingInputs) {
  const CatalogSnapshotPtr catalog = MutationCatalog();

  EXPECT_TRUE(
      std::holds_alternative<BoundSelect>(BindStatementOrThrow("SELECT Name FROM Items", catalog)));
  EXPECT_TRUE(std::holds_alternative<BoundReleaseSavepoint>(
      BindStatementOrThrow("RELEASE SAVEPOINT name", catalog)));

  const BindStatementResult null_catalog = BindStatement(ParseTree("BEGIN"), CatalogSnapshotPtr{});
  ASSERT_FALSE(null_catalog.has_value());
  EXPECT_EQ(BindErrorCode::kInvalidInput, null_catalog.error().code);

  EXPECT_TRUE(std::holds_alternative<BoundCreateIndex>(
      BindStatementOrThrow("CREATE INDEX new_index ON Items(Name)", catalog)));
}

TEST(StatementBinder, RejectsUnsupportedCreateIndexShapes) {
  const CatalogSnapshotPtr catalog = MutationCatalog();

  ExpectStatementBindError("CREATE INDEX missing_table ON nope(value)", catalog,
                           BindErrorCode::kNoSuchTable, ErrorCode::kGeneric, "no such table");
  ExpectStatementBindError("CREATE INDEX missing_column ON Items(nope)", catalog,
                           BindErrorCode::kNoSuchColumn, ErrorCode::kGeneric, "no such column");
  ExpectStatementBindError("CREATE INDEX expression_index ON Items(Name||'x')", catalog,
                           BindErrorCode::kUnsupportedFeature, ErrorCode::kGeneric, "expressions");
  ExpectStatementBindError("CREATE INDEX partial_index ON Items(Name) WHERE Score>0", catalog,
                           BindErrorCode::kUnsupportedFeature, ErrorCode::kGeneric, "partial");
  ExpectStatementBindError("CREATE INDEX bad_collation ON Items(Name COLLATE missing)", catalog,
                           BindErrorCode::kNoSuchCollation, ErrorCode::kGeneric, "missing");
  ExpectStatementBindError("CREATE INDEX sqlite_private ON Items(Name)", catalog,
                           BindErrorCode::kObjectNameReserved, ErrorCode::kGeneric, "reserved");
  ExpectStatementBindError("CREATE INDEX Items ON Items(Name)", catalog,
                           BindErrorCode::kTableAlreadyExists, ErrorCode::kGeneric,
                           "already a table");
  ExpectStatementBindError("CREATE INDEX idx_items_value ON IndexedItems(Value)", catalog,
                           BindErrorCode::kIndexAlreadyExists, ErrorCode::kGeneric,
                           "already exists");
  ExpectStatementBindError("CREATE INDEX wr_index ON wr(key)", catalog,
                           BindErrorCode::kUnsupportedFeature, ErrorCode::kGeneric, "table shape");
}

TEST(StatementBinder, ResolvesAnalyzeDatabaseTableAndIndexScopes) {
  const CatalogSnapshotPtr catalog = MutationCatalog();

  BoundStatement database_statement = BindStatementOrThrow("ANALYZE main", catalog);
  const auto& database = std::get<BoundAnalyze>(database_statement);
  EXPECT_EQ(BoundAnalyzeScope::kDatabase, database.scope());
  EXPECT_FALSE(database.stat1_root_page().has_value());
  EXPECT_FALSE(database.indexes().empty());
  EXPECT_FALSE(database.tables().empty());

  BoundStatement table_statement = BindStatementOrThrow("ANALYZE IndexedItems", catalog);
  const auto& table = std::get<BoundAnalyze>(table_statement);
  EXPECT_EQ(BoundAnalyzeScope::kTable, table.scope());
  EXPECT_EQ("IndexedItems", table.scope_name());
  ASSERT_EQ(1U, table.indexes().size());
  EXPECT_EQ("idx_items_value", table.indexes()[0].index_name);
  EXPECT_TRUE(table.tables().empty());

  BoundStatement index_statement = BindStatementOrThrow("ANALYZE main.idx_items_value", catalog);
  const auto& index = std::get<BoundAnalyze>(index_statement);
  EXPECT_EQ(BoundAnalyzeScope::kIndex, index.scope());
  EXPECT_EQ("idx_items_value", index.scope_name());
  ASSERT_EQ(1U, index.indexes().size());
  EXPECT_EQ(1U, index.indexes()[0].key_term_count);
  ASSERT_EQ(2U, index.indexes()[0].columns.size());

  ExpectStatementBindError("ANALYZE missing", catalog, BindErrorCode::kNoSuchTable,
                           ErrorCode::kGeneric, "no such table or index");
  ExpectStatementBindError("ANALYZE temp.Items", catalog, BindErrorCode::kUnsupportedFeature,
                           ErrorCode::kGeneric, "main schema");
}

TEST(StatementBinder, RejectsUnsupportedCreateTableShapes) {
  const CatalogSnapshotPtr catalog = MutationCatalog();

  ExpectStatementBindError("CREATE TABLE sqlite_private(id)", catalog,
                           BindErrorCode::kObjectNameReserved, ErrorCode::kGeneric,
                           "reserved for internal use");
  ExpectStatementBindError("CREATE TABLE idx_items_value(id)", catalog,
                           BindErrorCode::kTableAlreadyExists, ErrorCode::kGeneric,
                           "already an index");
  ExpectStatementBindError("CREATE TABLE bad(A, a)", catalog, BindErrorCode::kDuplicateColumn,
                           ErrorCode::kGeneric, "duplicate column");

  for (const std::string_view sql : {
           "CREATE TEMP TABLE temp_items(id)",
           "CREATE TABLE bad(id TEXT PRIMARY KEY)",
           "CREATE TABLE bad(id INTEGER PRIMARY KEY AUTOINCREMENT)",
           "CREATE TABLE bad(value TEXT UNIQUE)",
           "CREATE TABLE bad(value TEXT COLLATE NOCASE)",
           "CREATE TABLE bad(value TEXT CHECK(value <> ''))",
           "CREATE TABLE bad(value TEXT NOT NULL ON CONFLICT FAIL)",
           "CREATE TABLE bad(value TEXT DEFAULT (1 + 2))",
           "CREATE TABLE bad(a, UNIQUE(a))",
           "CREATE TABLE bad(value TEXT) WITHOUT ROWID",
           "CREATE TABLE bad(value TEXT) STRICT",
       }) {
    const BindStatementResult bound = BindStatement(ParseTree(sql), catalog);
    ASSERT_FALSE(bound.has_value()) << sql;
    EXPECT_EQ(BindErrorCode::kUnsupportedFeature, bound.error().code) << sql;
  }
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
  ExpectBindError("SELECT likelihood(1,1)", catalog, BindErrorCode::kInvalidLiteral,
                  "second argument to likelihood() must be a constant between 0.0 and 1.0");
  ExpectBindError("SELECT likelihood(1,(+0.5))", catalog, BindErrorCode::kInvalidLiteral,
                  "second argument to likelihood() must be a constant between 0.0 and 1.0");
}

TEST(Binder, EnforcesAggregateClauseLegality) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  ExpectBindError("SELECT Name FROM Items HAVING count()", catalog,
                  BindErrorCode::kHavingWithoutAggregate, "HAVING clause on a non-aggregate query");
  ExpectBindError("SELECT Name FROM Items ORDER BY count()", catalog,
                  BindErrorCode::kAggregateMisuse, "misuse of aggregate function count()");
  ExpectBindError("SELECT count() FROM Items WHERE count()", catalog,
                  BindErrorCode::kAggregateMisuse, "misuse of aggregate function count()");
  ExpectBindError("SELECT count() FROM Items WHERE sum(missing)", catalog,
                  BindErrorCode::kNoSuchColumn, "no such column: missing");
  ExpectBindError("SELECT count() AS total FROM Items WHERE total", catalog,
                  BindErrorCode::kAggregateMisuse,
                  "misuse of aggregate function through result alias");
  ExpectBindError("SELECT count() FROM Items GROUP BY count()", catalog,
                  BindErrorCode::kAggregateMisuse,
                  "aggregate functions are not allowed in the GROUP BY clause");
  ExpectBindError("SELECT sum(count()) FROM Items", catalog, BindErrorCode::kAggregateMisuse,
                  "misuse of aggregate function count()");
}

TEST(Binder, BindsAggregateModifiersAndPreservesScalarModifierErrors) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect aggregate = BindOrThrow(
      "SELECT group_concat(DISTINCT Name ORDER BY Score DESC NULLS LAST) "
      "FILTER (WHERE Id>0) FROM Items",
      catalog);
  ASSERT_EQ(1U, aggregate.aggregate_invocations().size());
  const BoundAggregateInvocation& invocation = aggregate.aggregate_invocations().front();
  EXPECT_TRUE(invocation.distinct);
  ASSERT_EQ(1U, invocation.arguments.size());
  ASSERT_EQ(1U, invocation.order_by.size());
  EXPECT_EQ(SortOrder::kDescending, invocation.order_by[0].order);
  EXPECT_EQ(BoundNullPlacement::kLast, invocation.order_by[0].null_placement);
  ASSERT_TRUE(invocation.filter.has_value());

  const BoundSelect zero_order = BindOrThrow("SELECT count(ORDER BY missing) FROM Items", catalog);
  EXPECT_TRUE(zero_order.aggregate_invocations().front().order_by.empty());

  const BoundSelect scalar_distinct =
      BindOrThrow("SELECT min(DISTINCT Score,Id) FROM Items", catalog);
  EXPECT_TRUE(std::holds_alternative<BoundScalarCallExpression>(
      ResultExpression(scalar_distinct, 0).payload));

  ExpectBindError("SELECT abs(1) FILTER (WHERE missing)", catalog, BindErrorCode::kAggregateMisuse,
                  "FILTER may not be used with non-aggregate abs()");
  ExpectBindError("SELECT abs(1 ORDER BY missing)", catalog, BindErrorCode::kAggregateMisuse,
                  "ORDER BY may not be used with non-aggregate abs()");
  ExpectBindError("SELECT coalesce(1,2 ORDER BY missing)", catalog, BindErrorCode::kAggregateMisuse,
                  "ORDER BY may not be used with non-aggregate coalesce()");
  ExpectBindError("SELECT iif(1,2) FILTER (WHERE missing)", catalog,
                  BindErrorCode::kAggregateMisuse,
                  "FILTER may not be used with non-aggregate iif()");
  ExpectBindError("SELECT abs(ORDER BY missing)", catalog, BindErrorCode::kWrongFunctionArity,
                  "wrong number of arguments to function abs()");
  ExpectBindError("SELECT likely(*)", catalog, BindErrorCode::kWrongFunctionArity,
                  "wrong number of arguments to function likely()");
  ExpectBindError("SELECT count(DISTINCT) FROM Items", catalog, BindErrorCode::kWrongFunctionArity,
                  "DISTINCT aggregates must have exactly one argument");
  ExpectBindError("SELECT count(DISTINCT) FILTER (WHERE missing) FROM Items", catalog,
                  BindErrorCode::kNoSuchColumn, "no such column: missing");
  ExpectBindError("SELECT group_concat(Name ORDER BY sum(Score)) FROM Items", catalog,
                  BindErrorCode::kAggregateMisuse, "misuse of aggregate function sum()");
  ExpectBindError("SELECT sum(Score) FILTER (WHERE count()) FROM Items", catalog,
                  BindErrorCode::kAggregateMisuse, "misuse of aggregate function count()");
}

TEST(Binder, PublishesAggregateArgumentOrderKeyReuse) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect reused =
      BindOrThrow("SELECT group_concat(Name ORDER BY Name) FROM Items", catalog);
  ASSERT_EQ(1U, reused.aggregate_invocations().size());
  EXPECT_TRUE(reused.aggregate_invocations().front().argument_reuses_single_order_key);

  const BoundSelect rowid =
      BindOrThrow("SELECT group_concat(id ORDER BY _rowid_) FROM Items", catalog);
  ASSERT_EQ(1U, rowid.aggregate_invocations().size());
  EXPECT_TRUE(rowid.aggregate_invocations().front().argument_reuses_single_order_key);

  const BoundSelect different =
      BindOrThrow("SELECT group_concat(Name ORDER BY Score) FROM Items", catalog);
  ASSERT_EQ(1U, different.aggregate_invocations().size());
  EXPECT_FALSE(different.aggregate_invocations().front().argument_reuses_single_order_key);

  const BoundSelect multiple =
      BindOrThrow("SELECT group_concat(Name ORDER BY Name,Score) FROM Items", catalog);
  ASSERT_EQ(1U, multiple.aggregate_invocations().size());
  EXPECT_FALSE(multiple.aggregate_invocations().front().argument_reuses_single_order_key);
}

TEST(Binder, LetsArgumentErrorsReplaceProvisionalFunctionLookupErrors) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  for (const std::string_view sql : {
           "SELECT unknown(missing)",
           "SELECT abs(missing,1)",
           "SELECT coalesce(missing)",
           "SELECT sum(missing,1)",
       }) {
    SCOPED_TRACE(sql);
    ExpectBindError(sql, catalog, BindErrorCode::kNoSuchColumn, "no such column: missing");
  }
}

TEST(Binder, BindsCoreAggregateInvocationsIntoOneProgram) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow("SELECT count(),sum(Score),min(Name) FROM Items", catalog);
  ASSERT_EQ(1U, select.query_cores().size());
  const auto& core = std::get<BoundSelectCore>(select.query_cores().front());
  ASSERT_TRUE(core.aggregate.has_value());
  ASSERT_EQ(3U, core.aggregate->invocations.size());
  ASSERT_EQ(3U, select.aggregate_invocations().size());

  const auto count = std::get<BoundAggregateCallExpression>(
      select.expression(core.result_columns[0].expression).payload);
  const auto sum = std::get<BoundAggregateCallExpression>(
      select.expression(core.result_columns[1].expression).payload);
  const auto minimum = std::get<BoundAggregateCallExpression>(
      select.expression(core.result_columns[2].expression).payload);
  EXPECT_EQ(core.aggregate->invocations[0], count.invocation);
  EXPECT_EQ(core.aggregate->invocations[1], sum.invocation);
  EXPECT_EQ(core.aggregate->invocations[2], minimum.invocation);

  ASSERT_EQ(3U, select.aggregate_functions().size());
  EXPECT_EQ("count", select.aggregate_functions()[0].name);
  EXPECT_EQ("sum", select.aggregate_functions()[1].name);
  EXPECT_EQ("min", select.aggregate_functions()[2].name);
  EXPECT_EQ(BoundAggregateExtremumKind::kMinimum, select.aggregate_functions()[2].extremum);
}

TEST(Binder, ResolvesEveryCoreAggregateArityAndCountStar) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow(
      "SELECT count(),count(*),count(Name),sum(Score),total(Score),avg(Score),"
      "min(Name),max(Name),group_concat(Name),group_concat(Name,','),"
      "string_agg(Name,',') FROM Items",
      catalog);
  const auto& core = std::get<BoundSelectCore>(select.query_cores().front());
  ASSERT_TRUE(core.aggregate.has_value());
  EXPECT_EQ(10U, core.aggregate->invocations.size());
  EXPECT_EQ(std::get<BoundAggregateCallExpression>(ResultExpression(select, 0).payload).invocation,
            std::get<BoundAggregateCallExpression>(ResultExpression(select, 1).payload).invocation);

  ExpectBindError("SELECT sum(*) FROM Items", catalog, BindErrorCode::kWrongFunctionArity,
                  "wrong number of arguments to function sum()");
  ExpectBindError("SELECT string_agg(Name) FROM Items", catalog, BindErrorCode::kWrongFunctionArity,
                  "wrong number of arguments to function string_agg()");
}

TEST(Binder, BindsAggregateClausesAliasesOrdinalsAndMetadata) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow(
      "SELECT Name AS label,sum(Score) AS total FROM Items "
      "WHERE Score>0 GROUP BY 1 COLLATE NOCASE "
      "HAVING total>0 ORDER BY sum(Score)",
      catalog);
  const auto& core = std::get<BoundSelectCore>(select.query_cores().front());
  ASSERT_TRUE(core.aggregate.has_value());
  ASSERT_EQ(1U, core.aggregate->invocations.size());
  ASSERT_EQ(1U, core.aggregate->group_by.size());
  ASSERT_TRUE(core.aggregate->having.has_value());

  const BoundGroupingTerm& grouping = core.aggregate->group_by.front();
  EXPECT_EQ(std::optional<std::size_t>{0}, grouping.result_column);
  EXPECT_EQ("NOCASE", CollationName(select, grouping.collation));
  const auto* grouping_alias =
      std::get_if<BoundCollateExpression>(&select.expression(grouping.expression).payload);
  ASSERT_NE(nullptr, grouping_alias);
  EXPECT_TRUE(std::holds_alternative<BoundAliasReferenceExpression>(
      select.expression(grouping_alias->operand).payload));

  ASSERT_EQ(1U, select.aggregate_invocations().size());
  const BoundAggregateInvocation& invocation = select.aggregate_invocations().front();
  EXPECT_EQ("sum", select.aggregate_functions()[invocation.function.value()].name);
  ASSERT_EQ(1U, invocation.arguments.size());
  EXPECT_FALSE(invocation.distinct);
  EXPECT_TRUE(invocation.order_by.empty());
  EXPECT_FALSE(invocation.filter.has_value());

  ASSERT_EQ(1U, select.order_by().size());
  const auto order_call = std::get<BoundAggregateCallExpression>(
      select.expression(select.order_by()[0].expression).payload);
  EXPECT_EQ(invocation.function,
            select.aggregate_invocations()[order_call.invocation.value()].function);

  const BoundResultColumn& aggregate_result = core.result_columns[1];
  EXPECT_EQ(TypeAffinity::kNone, aggregate_result.affinity);
  EXPECT_FALSE(aggregate_result.declared_type.has_value());
  EXPECT_EQ("total", aggregate_result.name);
}

TEST(Binder, SeparatesAggregateInvocationAndResultCollations) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect sum = BindOrThrow("SELECT sum(Score COLLATE unavailable) FROM Items", catalog);
  ASSERT_EQ(1U, sum.aggregate_invocations().size());
  EXPECT_EQ("BINARY", CollationName(sum, sum.aggregate_invocations().front().collation));
  const BoundExpressionProperties& properties = ResultExpression(sum, 0).properties;
  ASSERT_TRUE(properties.collation.has_value());
  EXPECT_EQ("unavailable", CollationName(sum, *properties.collation));
  EXPECT_TRUE(properties.has_explicit_collation);
  EXPECT_TRUE(properties.contains_aggregate);

  ExpectBindError("SELECT sum(DISTINCT Score COLLATE unavailable) FROM Items", catalog,
                  BindErrorCode::kNoSuchCollation, "no such collation sequence: unavailable");
  ExpectBindError("SELECT min(Score COLLATE unavailable) FROM Items", catalog,
                  BindErrorCode::kNoSuchCollation, "no such collation sequence: unavailable");
}

TEST(Binder, DeduplicatesAggregateInvocationsInClauseOrderPerCore) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow(
      "SELECT sum(Score),sum(Score) FROM Items "
      "HAVING min(Name) IS NOT NULL ORDER BY max(Id),sum(Score)",
      catalog);
  const auto& core = std::get<BoundSelectCore>(select.query_cores().front());
  ASSERT_TRUE(core.aggregate.has_value());
  ASSERT_EQ(3U, core.aggregate->invocations.size());
  ASSERT_EQ(3U, select.aggregate_invocations().size());

  const auto first_sum =
      std::get<BoundAggregateCallExpression>(ResultExpression(select, 0).payload);
  const auto second_sum =
      std::get<BoundAggregateCallExpression>(ResultExpression(select, 1).payload);
  EXPECT_EQ(first_sum.invocation, second_sum.invocation);
  EXPECT_EQ("sum", select
                       .aggregate_functions()
                           [select.aggregate_invocations()[core.aggregate->invocations[0].value()]
                                .function.value()]
                       .name);
  EXPECT_EQ("max", select
                       .aggregate_functions()
                           [select.aggregate_invocations()[core.aggregate->invocations[1].value()]
                                .function.value()]
                       .name);
  EXPECT_EQ("min", select
                       .aggregate_functions()
                           [select.aggregate_invocations()[core.aggregate->invocations[2].value()]
                                .function.value()]
                       .name);

  const BoundSelect compound =
      BindOrThrow("SELECT sum(Score) FROM Items UNION ALL SELECT sum(Score) FROM Items", catalog);
  ASSERT_EQ(2U, compound.query_cores().size());
  ASSERT_EQ(2U, compound.aggregate_invocations().size());
  const auto& left = std::get<BoundSelectCore>(compound.query_cores()[0]);
  const auto& right = std::get<BoundSelectCore>(compound.query_cores()[1]);
  ASSERT_TRUE(left.aggregate.has_value());
  ASSERT_TRUE(right.aggregate.has_value());
  EXPECT_NE(left.aggregate->invocations[0], right.aggregate->invocations[0]);

  const BoundSelect compound_order = BindOrThrow(
      "SELECT sum(Score) FROM Items UNION ALL "
      "SELECT sum(Score) FROM Items ORDER BY sum(Score)",
      catalog);
  EXPECT_EQ(2U, compound_order.aggregate_invocations().size());
  EXPECT_EQ(1U, compound_order.aggregate_functions().size());
  ASSERT_EQ(1U, compound_order.order_by().size());
  EXPECT_EQ(std::optional<std::size_t>{0}, compound_order.order_by()[0].result_column);
  ExpectBindError(
      "SELECT sum(Score) FROM Items UNION ALL "
      "SELECT sum(Score) FROM Items ORDER BY sum(Id)",
      catalog, BindErrorCode::kCompoundOrderByTerm,
      "1 ORDER BY term does not match any column in the result set");

  ExpectBindError("SELECT sum(Score),max(Id) FROM Items", catalog,
                  BindErrorCode::kAggregateTermLimitExceeded, "too many aggregate terms",
                  BindEnvironment::Core(), BindOptions{.maximum_aggregate_terms = 1});
}

TEST(Binder, CanonicalizesIntegerPrimaryKeyAliasesForAggregateIdentity) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect deduplicated = BindOrThrow("SELECT sum(Id),sum(_rowid_) FROM Items", catalog);
  const auto& deduplicated_core = std::get<BoundSelectCore>(deduplicated.query_cores().front());
  ASSERT_TRUE(deduplicated_core.aggregate.has_value());
  ASSERT_EQ(1U, deduplicated_core.aggregate->invocations.size());
  const auto id_sum =
      std::get<BoundAggregateCallExpression>(ResultExpression(deduplicated, 0).payload);
  const auto rowid_sum =
      std::get<BoundAggregateCallExpression>(ResultExpression(deduplicated, 1).payload);
  EXPECT_EQ(id_sum.invocation, rowid_sum.invocation);

  const BoundSelect grouped =
      BindOrThrow("SELECT Id,count() FROM Items GROUP BY _rowid_ HAVING Id=1", catalog,
                  BindEnvironment::Core(), BindOptions{.maximum_aggregate_terms = 1});
  const auto& grouped_core = std::get<BoundSelectCore>(grouped.query_cores().front());
  ASSERT_TRUE(grouped_core.aggregate.has_value());
  EXPECT_FALSE(grouped_core.aggregate->having.has_value());
  EXPECT_TRUE(grouped_core.where.has_value());

  const CatalogSnapshotPtr collated_catalog = CollatedRowIdCatalog();
  const BoundSelect minimum =
      BindOrThrow("SELECT min(Id),min(_rowid_) FROM Items", collated_catalog);
  const auto& minimum_core = std::get<BoundSelectCore>(minimum.query_cores().front());
  ASSERT_TRUE(minimum_core.aggregate.has_value());
  ASSERT_EQ(1U, minimum_core.aggregate->invocations.size());
  EXPECT_EQ("BINARY", CollationName(minimum, minimum.aggregate_invocations().front().collation));

  const BoundSelect distinct =
      BindOrThrow("SELECT count(DISTINCT Id),count(DISTINCT _rowid_) FROM Items", collated_catalog,
                  BindEnvironment::Core(), BindOptions{.maximum_aggregate_terms = 1});
  const auto& distinct_core = std::get<BoundSelectCore>(distinct.query_cores().front());
  ASSERT_TRUE(distinct_core.aggregate.has_value());
  EXPECT_EQ(1U, distinct_core.aggregate->invocations.size());

  const BoundSelect explicit_collation =
      BindOrThrow("SELECT min(Id COLLATE NOCASE),min(_rowid_) FROM Items", collated_catalog);
  const auto& explicit_core = std::get<BoundSelectCore>(explicit_collation.query_cores().front());
  ASSERT_TRUE(explicit_core.aggregate.has_value());
  ASSERT_EQ(2U, explicit_core.aggregate->invocations.size());
  EXPECT_EQ("NOCASE", CollationName(explicit_collation,
                                    explicit_collation.aggregate_invocations()[0].collation));
  EXPECT_EQ("BINARY", CollationName(explicit_collation,
                                    explicit_collation.aggregate_invocations()[1].collation));
}

TEST(Binder, ResolvesGroupByOrdinalsAndRightmostResultMatches) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const BoundSelect ordinal =
      BindOrThrow("SELECT Name,Name,count() FROM Items GROUP BY 1", catalog);
  const auto& ordinal_core = std::get<BoundSelectCore>(ordinal.query_cores().front());
  ASSERT_TRUE(ordinal_core.aggregate.has_value());
  ASSERT_EQ(1U, ordinal_core.aggregate->group_by.size());
  EXPECT_EQ(std::optional<std::size_t>{0}, ordinal_core.aggregate->group_by[0].result_column);

  const BoundSelect ordinary =
      BindOrThrow("SELECT Name,Name,count() FROM Items GROUP BY Name", catalog);
  const auto& ordinary_core = std::get<BoundSelectCore>(ordinary.query_cores().front());
  ASSERT_TRUE(ordinary_core.aggregate.has_value());
  EXPECT_EQ(std::optional<std::size_t>{1}, ordinary_core.aggregate->group_by[0].result_column);

  const BoundSelect collated =
      BindOrThrow("SELECT Name,count() FROM Items GROUP BY 1 COLLATE NOCASE", catalog);
  const auto& collated_core = std::get<BoundSelectCore>(collated.query_cores().front());
  ASSERT_TRUE(collated_core.aggregate.has_value());
  EXPECT_EQ(std::optional<std::size_t>{0}, collated_core.aggregate->group_by[0].result_column);
  EXPECT_EQ("NOCASE", CollationName(collated, collated_core.aggregate->group_by[0].collation));

  const BoundSelect large =
      BindOrThrow("SELECT Name,count() FROM Items GROUP BY 2147483648", catalog);
  const auto& large_core = std::get<BoundSelectCore>(large.query_cores().front());
  ASSERT_TRUE(large_core.aggregate.has_value());
  EXPECT_FALSE(large_core.aggregate->group_by[0].result_column.has_value());

  ExpectBindError("SELECT Name,count() FROM Items GROUP BY 0", catalog,
                  BindErrorCode::kGroupByTermOutOfRange,
                  "1 GROUP BY term out of range - should be between 1 and 2");
  ExpectBindError("SELECT Name,count() FROM Items GROUP BY -1", catalog,
                  BindErrorCode::kGroupByTermOutOfRange,
                  "1 GROUP BY term out of range - should be between 1 and 2");
  ExpectBindError("SELECT sum(Score),count() FROM Items GROUP BY 1", catalog,
                  BindErrorCode::kAggregateMisuse,
                  "aggregate functions are not allowed in the GROUP BY clause");
}

TEST(Binder, GivesSourceColumnsPrecedenceOverAggregateAliases) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select =
      BindOrThrow("SELECT Id AS Score,count() FROM Items GROUP BY Score HAVING Score>0", catalog);
  const auto& core = std::get<BoundSelectCore>(select.query_cores().front());
  ASSERT_TRUE(core.aggregate.has_value());
  const auto* grouping = std::get_if<BoundColumnExpression>(
      &select.expression(core.aggregate->group_by[0].expression).payload);
  ASSERT_NE(nullptr, grouping);
  EXPECT_EQ(BoundSourceColumnId{2}, grouping->column);

  EXPECT_FALSE(core.aggregate->having.has_value());
  ASSERT_TRUE(core.where.has_value());
  const auto& comparison =
      std::get<BoundComparisonExpression>(select.expression(*core.where).payload);
  const auto* having_column =
      std::get_if<BoundColumnExpression>(&select.expression(comparison.left).payload);
  ASSERT_NE(nullptr, having_column);
  EXPECT_EQ(BoundSourceColumnId{2}, having_column->column);
}

TEST(Binder, TransfersEligibleHavingLeavesAndPublishesAggregateLeafSets) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const BoundSelect transferred = BindOrThrow(
      "SELECT Id,count() FROM Items WHERE Id>0 "
      "GROUP BY Id HAVING Id=1 AND 1",
      catalog);
  const auto& transferred_core = std::get<BoundSelectCore>(transferred.query_cores().front());
  ASSERT_TRUE(transferred_core.aggregate.has_value());
  EXPECT_FALSE(transferred_core.aggregate->having.has_value());
  EXPECT_TRUE(transferred_core.where.has_value());
  ASSERT_EQ(1U, transferred_core.aggregate->source_leaves.size());
  ASSERT_EQ(1U, transferred_core.aggregate->representative_leaves.size());
  const auto* source = std::get_if<BoundColumnExpression>(
      &transferred.expression(transferred_core.aggregate->source_leaves[0]).payload);
  const auto* representative = std::get_if<BoundColumnExpression>(
      &transferred.expression(transferred_core.aggregate->representative_leaves[0]).payload);
  ASSERT_NE(nullptr, source);
  ASSERT_NE(nullptr, representative);
  EXPECT_EQ(BoundSourceColumnId{0}, source->column);
  EXPECT_EQ(BoundSourceColumnId{0}, representative->column);

  const BoundSelect collated = BindOrThrow(
      "SELECT Name,count() FROM Items "
      "GROUP BY Name COLLATE NOCASE HAVING Name='x' AND 1",
      catalog);
  const auto& collated_core = std::get<BoundSelectCore>(collated.query_cores().front());
  ASSERT_TRUE(collated_core.aggregate.has_value());
  ASSERT_TRUE(collated_core.aggregate->having.has_value());
  EXPECT_TRUE(collated_core.where.has_value());

  const BoundSelect always_false =
      BindOrThrow("SELECT Name,count() FROM Items GROUP BY Name HAVING 0", catalog);
  const auto& false_core = std::get<BoundSelectCore>(always_false.query_cores().front());
  ASSERT_TRUE(false_core.aggregate.has_value());
  EXPECT_TRUE(false_core.aggregate->having.has_value());

  const BoundSelect aggregate =
      BindOrThrow("SELECT Name,count() FROM Items GROUP BY Name HAVING count()>0", catalog);
  const auto& aggregate_core = std::get<BoundSelectCore>(aggregate.query_cores().front());
  ASSERT_TRUE(aggregate_core.aggregate.has_value());
  EXPECT_TRUE(aggregate_core.aggregate->having.has_value());
}

TEST(Binder, SeparatesAggregateSourceAndRepresentativeLeaves) {
  const CatalogSnapshotPtr catalog = TestCatalog();

  const BoundSelect ordered = BindOrThrow(
      "SELECT Name,group_concat(Score ORDER BY Id) FILTER (WHERE Score>0) "
      "FROM Items GROUP BY Name",
      catalog);
  const auto& ordered_core = std::get<BoundSelectCore>(ordered.query_cores().front());
  ASSERT_TRUE(ordered_core.aggregate.has_value());
  EXPECT_EQ(3U, ordered_core.aggregate->source_leaves.size());
  ASSERT_EQ(1U, ordered_core.aggregate->representative_leaves.size());

  const BoundSelect minimum = BindOrThrow("SELECT Name,min(Score ORDER BY Id) FROM Items", catalog);
  const auto& minimum_core = std::get<BoundSelectCore>(minimum.query_cores().front());
  ASSERT_TRUE(minimum_core.aggregate.has_value());
  ASSERT_EQ(1U, minimum_core.aggregate->source_leaves.size());
  const auto* source = std::get_if<BoundColumnExpression>(
      &minimum.expression(minimum_core.aggregate->source_leaves[0]).payload);
  ASSERT_NE(nullptr, source);
  EXPECT_EQ(BoundSourceColumnId{2}, source->column);
  ASSERT_EQ(1U, minimum_core.aggregate->representative_leaves.size());

  ExpectBindError("SELECT Id,sum(Score) FROM Items GROUP BY Name", catalog,
                  BindErrorCode::kAggregateTermLimitExceeded, "too many aggregate terms",
                  BindEnvironment::Core(), BindOptions{.maximum_aggregate_terms = 2});
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

TEST(Binder, ResolvesOrderByAliasesOrdinalsExpressionsAndMetadata) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect select = BindOrThrow(
      "SELECT Name AS x,Score AS x,Name FROM Items "
      "ORDER BY x DESC NULLS FIRST,2 COLLATE nocase,Name,+1,2147483648",
      catalog);
  ASSERT_EQ(5U, select.order_by().size());

  const BoundOrderingTerm& alias = select.order_by()[0];
  EXPECT_EQ(0U, alias.result_column);
  EXPECT_EQ(SortOrder::kDescending, alias.order);
  EXPECT_EQ(BoundNullPlacement::kFirst, alias.null_placement);
  EXPECT_EQ("NOCASE", CollationName(select, alias.collation));
  const auto& alias_expression =
      std::get<BoundAliasReferenceExpression>(select.expression(alias.expression).payload);
  EXPECT_EQ(select.result_columns()[0].expression, alias_expression.target);

  const BoundOrderingTerm& ordinal = select.order_by()[1];
  EXPECT_EQ(1U, ordinal.result_column);
  EXPECT_EQ(SortOrder::kAscending, ordinal.order);
  EXPECT_EQ(BoundNullPlacement::kFirst, ordinal.null_placement);
  EXPECT_EQ("NOCASE", CollationName(select, ordinal.collation));
  EXPECT_TRUE(select.expression(ordinal.expression).properties.has_explicit_collation);

  const BoundOrderingTerm& identical = select.order_by()[2];
  EXPECT_EQ(2U, identical.result_column);
  EXPECT_EQ("NOCASE", CollationName(select, identical.collation));

  const BoundOrderingTerm& unary = select.order_by()[3];
  EXPECT_EQ(0U, unary.result_column);

  const BoundOrderingTerm& large = select.order_by()[4];
  EXPECT_FALSE(large.result_column.has_value());
  EXPECT_EQ(BoundExpressionKind::kLiteral,
            BoundExpressionKindOf(select.expression(large.expression)));

  ExpectBindError("SELECT Name FROM Items ORDER BY 0", catalog,
                  BindErrorCode::kOrderByTermOutOfRange,
                  "1 ORDER BY term out of range - should be between 1 and 1");
  ExpectBindError("SELECT Name FROM Items ORDER BY -1", catalog,
                  BindErrorCode::kOrderByTermOutOfRange,
                  "1 ORDER BY term out of range - should be between 1 and 1");
  ExpectBindError("SELECT Name FROM Items ORDER BY 2", catalog,
                  BindErrorCode::kOrderByTermOutOfRange,
                  "1 ORDER BY term out of range - should be between 1 and 1");

  const BoundSelect quoted =
      BindOrThrow("SELECT Score AS x,Name AS y FROM Items ORDER BY \"x\"", catalog);
  ASSERT_EQ(1U, quoted.order_by().size());
  EXPECT_EQ(0U, quoted.order_by()[0].result_column);

  const BoundSelect distinct_calls = BindOrThrow(
      "SELECT ifnull(Name,'x'),coalesce(Name,'x') FROM Items "
      "ORDER BY ifnull(Name,'x')",
      catalog);
  ASSERT_EQ(1U, distinct_calls.order_by().size());
  EXPECT_EQ(0U, distinct_calls.order_by()[0].result_column);

  const BoundSelect anonymous = BindOrThrow("SELECT ?,? FROM Items ORDER BY ?", catalog);
  EXPECT_FALSE(anonymous.order_by()[0].result_column.has_value());

  const BoundSelect wildcard = BindOrThrow("SELECT * FROM Items ORDER BY Name", catalog);
  EXPECT_EQ(1U, wildcard.order_by()[0].result_column);
  const BoundSelect explicit_then_wildcard =
      BindOrThrow("SELECT Name,* FROM Items ORDER BY Name", catalog);
  EXPECT_EQ(2U, explicit_then_wildcard.order_by()[0].result_column);

  const BoundSelect signed_ordinals =
      BindOrThrow("SELECT Name,Score FROM Items ORDER BY ++2,-2147483648", catalog);
  EXPECT_EQ(1U, signed_ordinals.order_by()[0].result_column);
  EXPECT_FALSE(signed_ordinals.order_by()[1].result_column.has_value());
  const BoundSelect collated_unary =
      BindOrThrow("SELECT Name,Score FROM Items ORDER BY +(2 COLLATE nocase)", catalog);
  EXPECT_FALSE(collated_unary.order_by()[0].result_column.has_value());

  const BoundSelect normalized =
      BindOrThrow("SELECT abs(Score)+01,Name FROM Items ORDER BY abs(Score)+1", catalog);
  EXPECT_EQ(0U, normalized.order_by()[0].result_column);
  const BoundSelect qualified = BindOrThrow("SELECT Name FROM Items ORDER BY Items.Name", catalog);
  EXPECT_EQ(0U, qualified.order_by()[0].result_column);
  const BoundSelect dqs = BindOrThrow("SELECT \"Name\" FROM Items ORDER BY Name", catalog);
  EXPECT_EQ(0U, dqs.order_by()[0].result_column);
  const BoundSelect dqs_text = BindOrThrow("SELECT 'missing' ORDER BY \"missing\"", catalog);
  EXPECT_EQ(0U, dqs_text.order_by()[0].result_column);

  const BoundSelect real_spelling = BindOrThrow(
      "SELECT abs(Score)+1.0,abs(Score)+1e0 FROM Items ORDER BY abs(Score)+1.0", catalog);
  EXPECT_EQ(0U, real_spelling.order_by()[0].result_column);
  const BoundSelect large_integer_spelling = BindOrThrow(
      "SELECT abs(Score)+2147483648,abs(Score)+02147483648 FROM Items "
      "ORDER BY abs(Score)+2147483648",
      catalog);
  EXPECT_EQ(0U, large_integer_spelling.order_by()[0].result_column);

  ExpectBindError("SELECT Name FROM Items ORDER BY Name COLLATE missing", catalog,
                  BindErrorCode::kNoSuchCollation, "no such collation sequence: missing");
}

TEST(Binder, BindsValuesAndCompoundQueryCores) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect bound = BindOrThrow(
      "VALUES(?1,?2 IS NULL),(?3,?4) "
      "UNION SELECT ?5 AS right_name,?6 COLLATE nocase "
      "ORDER BY right_name COLLATE binary DESC NULLS FIRST LIMIT ?7 OFFSET ?8",
      catalog);

  EXPECT_EQ(8U, bound.parameters().size());
  ASSERT_EQ(2U, bound.query_cores().size());
  ASSERT_EQ(1U, bound.compound_operators().size());
  EXPECT_EQ(CompoundOperator::kUnion, bound.compound_operators()[0]);

  const auto& values = std::get<BoundValuesCore>(bound.query_cores()[0]);
  EXPECT_EQ(BoundValuesEvaluationSchedule::kConsumerFiltered, values.schedule);
  ASSERT_EQ(2U, values.rows.size());
  ASSERT_EQ(2U, values.result_columns.size());
  EXPECT_EQ("column1", values.result_columns[0].name);
  EXPECT_EQ("column2", values.result_columns[1].name);

  const auto& right = std::get<BoundSelectCore>(bound.query_cores()[1]);
  EXPECT_EQ(SelectQuantifier::kDefault, right.quantifier);
  EXPECT_EQ(0U, right.source_column_count);
  ASSERT_EQ(2U, right.result_columns.size());
  EXPECT_EQ("right_name", right.result_columns[0].name);

  ASSERT_EQ(2U, bound.compound_collations().size());
  EXPECT_EQ("BINARY", bound.collations()[bound.compound_collations()[0].value()].name);
  EXPECT_EQ("nocase", bound.collations()[bound.compound_collations()[1].value()].name);

  ASSERT_EQ(1U, bound.order_by().size());
  EXPECT_EQ(std::optional<std::size_t>{0}, bound.order_by()[0].result_column);
  EXPECT_EQ(SortOrder::kDescending, bound.order_by()[0].order);
  EXPECT_EQ(BoundNullPlacement::kFirst, bound.order_by()[0].null_placement);
  EXPECT_EQ("BINARY", bound.collations()[bound.order_by()[0].collation.value()].name);

  ASSERT_NE(nullptr, bound.limit());
  const auto& limit =
      std::get<BoundParameterExpression>(bound.expression(bound.limit()->limit).payload);
  const auto& offset = std::get<BoundParameterExpression>(
      bound.expression(RequiredOptional(bound.limit()->offset)).payload);
  EXPECT_EQ(BoundParameterId{6}, limit.parameter);
  EXPECT_EQ(BoundParameterId{7}, offset.parameter);
}

TEST(Binder, PublishesSetThenOrderMetadataFromTheRightmostCore) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect ordinary = BindOrThrow(
      "SELECT id,'left' FROM Items "
      "UNION SELECT Score,Name FROM Items "
      "ORDER BY 2",
      catalog);
  ASSERT_EQ(2U, ordinary.result_columns().size());
  EXPECT_EQ("id", ordinary.result_columns()[0].name);
  EXPECT_EQ("'left'", ordinary.result_columns()[1].name);
  EXPECT_EQ(std::optional<std::string>{"INTEGER"}, ordinary.result_columns()[0].declared_type);
  EXPECT_EQ(std::nullopt, ordinary.result_columns()[1].declared_type);
  EXPECT_EQ(TypeAffinity::kInteger, ordinary.result_columns()[0].affinity);
  EXPECT_EQ(TypeAffinity::kNone, ordinary.result_columns()[1].affinity);

  const BoundSelect set_then_order = BindOrThrow(
      "SELECT id,'left' FROM Items "
      "UNION SELECT Score,Name FROM Items "
      "ORDER BY 2 COLLATE binary",
      catalog);
  ASSERT_EQ(2U, set_then_order.result_columns().size());
  EXPECT_EQ("id", set_then_order.result_columns()[0].name);
  EXPECT_EQ("'left'", set_then_order.result_columns()[1].name);
  EXPECT_EQ(std::optional<std::string>{"REAL"}, set_then_order.result_columns()[0].declared_type);
  EXPECT_EQ(std::optional<std::string>{"TEXT"}, set_then_order.result_columns()[1].declared_type);
  EXPECT_EQ(TypeAffinity::kReal, set_then_order.result_columns()[0].affinity);
  EXPECT_EQ(TypeAffinity::kText, set_then_order.result_columns()[1].affinity);

  const auto& left = std::get<BoundSelectCore>(set_then_order.query_cores().front());
  EXPECT_EQ(std::optional<std::string>{"INTEGER"}, left.result_columns[0].declared_type);
  EXPECT_EQ(std::nullopt, left.result_columns[1].declared_type);
  EXPECT_EQ(TypeAffinity::kInteger, left.result_columns[0].affinity);
  EXPECT_EQ(TypeAffinity::kNone, left.result_columns[1].affinity);
}

TEST(Binder, ClassifiesValuesSchedulesAndBindsDistinctCores) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect eager = BindOrThrow("VALUES(?1,abs(?2)),(?3,?4)", catalog);
  EXPECT_EQ(BoundValuesEvaluationSchedule::kProducerEager,
            std::get<BoundValuesCore>(eager.query_cores()[0]).schedule);

  const BoundSelect truth = BindOrThrow("VALUES(?1 IS NULL),(2)", catalog);
  EXPECT_EQ(BoundValuesEvaluationSchedule::kConsumerFiltered,
            std::get<BoundValuesCore>(truth.query_cores()[0]).schedule);

  constexpr std::array<ScalarFunction, 1> kFunctions{
      ScalarFunction{"volatile_value", FunctionArity::Exact(0),
                     FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                     StubScalar},
  };
  const FunctionRegistry registry{kFunctions};
  const std::array<const Collation*, 3> collations{
      &BinaryCollation(),
      &NoCaseCollation(),
      &RTrimCollation(),
  };
  const BoundSelect nondeterministic = BindOrThrow("VALUES(volatile_value()),(2)", catalog,
                                                   BindEnvironment{registry, collations, 17});
  EXPECT_EQ(BoundValuesEvaluationSchedule::kConsumerFiltered,
            std::get<BoundValuesCore>(nondeterministic.query_cores()[0]).schedule);

  const BoundSelect distinct = BindOrThrow("SELECT DISTINCT Name FROM Items", catalog);
  ASSERT_EQ(1U, distinct.query_cores().size());
  EXPECT_EQ(SelectQuantifier::kDistinct,
            std::get<BoundSelectCore>(distinct.query_cores()[0]).quantifier);
}

TEST(Binder, IsolatesCompoundSourceScopesAndPublishesGlobalCollations) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  const BoundSelect sources =
      BindOrThrow("SELECT Name FROM Items UNION SELECT Score FROM Items", catalog);
  ASSERT_EQ(2U, sources.query_cores().size());
  const auto& left = std::get<BoundSelectCore>(sources.query_cores()[0]);
  const auto& right = std::get<BoundSelectCore>(sources.query_cores()[1]);
  EXPECT_EQ(0U, left.source_column_begin);
  EXPECT_EQ(5U, left.source_column_count);
  EXPECT_EQ(5U, right.source_column_begin);
  EXPECT_EQ(5U, right.source_column_count);
  const auto& left_column = std::get<BoundColumnExpression>(
      sources.expression(left.result_columns[0].expression).payload);
  const auto& right_column = std::get<BoundColumnExpression>(
      sources.expression(right.result_columns[0].expression).payload);
  EXPECT_EQ(BoundSourceColumnId{1}, left_column.column);
  EXPECT_EQ(BoundSourceColumnId{7}, right_column.column);

  ExpectBindError("SELECT Name FROM Items UNION SELECT missing FROM Items", catalog,
                  BindErrorCode::kNoSuchColumn, "no such column: missing");

  const BoundSelect late = BindOrThrow("SELECT 'a' UNION SELECT 'A' COLLATE nocase", catalog);
  ASSERT_EQ(1U, late.compound_collations().size());
  EXPECT_EQ("nocase", late.collations()[late.compound_collations()[0].value()].name);

  const BoundSelect explicit_binary =
      BindOrThrow("SELECT 'a' COLLATE binary UNION SELECT 'A' COLLATE nocase", catalog);
  ASSERT_EQ(1U, explicit_binary.compound_collations().size());
  EXPECT_EQ("binary",
            explicit_binary.collations()[explicit_binary.compound_collations()[0].value()].name);

  const BoundSelect unused =
      BindOrThrow("SELECT 'a' COLLATE missing UNION ALL SELECT 'b'", catalog);
  EXPECT_EQ("missing", unused.collations()[unused.compound_collations()[0].value()].name);
  static_cast<void>(
      BindOrThrow("SELECT 1,'a' COLLATE missing UNION ALL SELECT 2,'b' ORDER BY 1", catalog));
  ExpectBindError("SELECT DISTINCT 'a' COLLATE missing", catalog, BindErrorCode::kNoSuchCollation,
                  "no such collation sequence: missing");
  ExpectBindError("SELECT DISTINCT 'a' COLLATE missing ORDER BY no_match", catalog,
                  BindErrorCode::kNoSuchColumn, "no such column: no_match");
  ExpectBindError("SELECT 'a' COLLATE missing UNION SELECT 'b'", catalog,
                  BindErrorCode::kNoSuchCollation, "no such collation sequence: missing");
  ExpectBindError("SELECT 'a' COLLATE missing UNION SELECT 'b' ORDER BY no_match", catalog,
                  BindErrorCode::kCompoundOrderByTerm,
                  "1 ORDER BY term does not match any column in the result set");
}

TEST(Binder, RejectsCompoundWidthsAndResolvesCompoundOrderTerms) {
  const CatalogSnapshotPtr catalog = TestCatalog();
  ExpectBindError("VALUES(1),(2,3)", catalog, BindErrorCode::kValuesColumnCount,
                  "all VALUES must have the same number of terms");
  ExpectBindError("SELECT 1 UNION ALL SELECT 2,3", catalog, BindErrorCode::kCompoundColumnCount,
                  "SELECTs to the left and right of UNION ALL "
                  "do not have the same number of result columns");

  const BoundSelect right_alias =
      BindOrThrow("SELECT 1 UNION SELECT 2 AS right_name ORDER BY right_name", catalog);
  EXPECT_EQ(std::optional<std::size_t>{0}, right_alias.order_by()[0].result_column);

  const BoundSelect right_expression =
      BindOrThrow("SELECT 1 UNION SELECT abs(2) ORDER BY abs(2)", catalog);
  EXPECT_EQ(std::optional<std::size_t>{0}, right_expression.order_by()[0].result_column);

  ExpectBindError("SELECT 1 UNION SELECT abs(2) ORDER BY abs(3)", catalog,
                  BindErrorCode::kCompoundOrderByTerm,
                  "1 ORDER BY term does not match any column in the result set");
  ExpectBindError("SELECT 'a' UNION SELECT 1 ORDER BY 'z'", catalog,
                  BindErrorCode::kCompoundOrderByTerm,
                  "1 ORDER BY term does not match any column in the result set");

  const BoundSelect arm_precedence =
      BindOrThrow(R"(SELECT "x",10 UNION SELECT 20 AS y,30 AS x ORDER BY "x")", catalog);
  EXPECT_EQ(std::optional<std::size_t>{0}, arm_precedence.order_by()[0].result_column);

  const BoundSelect wildcard =
      BindOrThrow("SELECT * FROM Items UNION SELECT * FROM Items ORDER BY Items.Name", catalog);
  EXPECT_EQ(std::optional<std::size_t>{1}, wildcard.order_by()[0].result_column);

  const BoundSelect output_collation = BindOrThrow(
      "SELECT 'a' COLLATE binary AS l "
      "UNION ALL SELECT 'B' COLLATE nocase AS r ORDER BY r",
      catalog);
  EXPECT_EQ("binary",
            output_collation.collations()[output_collation.order_by()[0].collation.value()].name);

  ExpectBindError("VALUES(no_such_function()),(1,2)", catalog, BindErrorCode::kNoSuchFunction,
                  "no such function: no_such_function");
  ExpectBindError("SELECT no_such_function() UNION SELECT 2,3 UNION SELECT 4", catalog,
                  BindErrorCode::kCompoundColumnCount,
                  "SELECTs to the left and right of UNION "
                  "do not have the same number of result columns");
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
        .first =
            SelectCore{
                .span = statement_span,
                .result_columns =
                    {
                        ResultColumn{
                            .span = token_span,
                            .expression = ExpressionId{0},
                        },
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
      .first =
          SelectCore{
              .span = statement_span,
              .result_columns =
                  {
                      ResultColumn{
                          .span = call_span,
                          .expression = ExpressionId{2},
                      },
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
  ExpectBindError("VALUES(1,2)", catalog, BindErrorCode::kResultColumnLimitExceeded,
                  "too many columns in result set", BindEnvironment::Core(),
                  BindOptions{.maximum_result_columns = 1});
  ExpectBindError("SELECT coalesce(1,2,3)", catalog, BindErrorCode::kFunctionArgumentLimitExceeded,
                  "too many arguments on function coalesce", BindEnvironment::Core(),
                  BindOptions{.maximum_function_arguments = 2});
  ExpectBindError("SELECT count()", catalog, BindErrorCode::kAggregateTermLimitExceeded,
                  "too many aggregate terms", BindEnvironment::Core(),
                  BindOptions{.maximum_aggregate_terms = 0});
  static_cast<void>(BindOrThrow("SELECT count() UNION ALL SELECT count()", catalog,
                                BindEnvironment::Core(),
                                BindOptions{.maximum_aggregate_terms = 1}));

  BindSelectResult unrepresentable = BindSelectStatement(
      ParseTree("SELECT 1"), catalog, BindEnvironment::Core(),
      BindOptions{
          .maximum_aggregate_terms =
              static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U,
      });
  ASSERT_FALSE(unrepresentable.has_value());
  EXPECT_EQ(BindErrorCode::kInvalidInput, unrepresentable.error().code);
  ExpectBindError("SELECT missing FROM Items", catalog, BindErrorCode::kNoSuchColumn,
                  "no such column: missing");
  ExpectBindError("SELECT 1 FROM Missing", catalog, BindErrorCode::kNoSuchTable,
                  "no such table: Missing");
  ExpectBindError("SELECT -0x10000000000000000", catalog, BindErrorCode::kInvalidLiteral,
                  "hex literal too big: -0x10000000000000000");
}

}  // namespace
}  // namespace modern_sqlite
