#include "modern_sqlite/catalog/catalog.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/syntax/ast.hpp"
#include "modern_sqlite/syntax/parser.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<CatalogSnapshot>);
static_assert(!std::is_copy_assignable_v<CatalogSnapshot>);
static_assert(!std::is_move_constructible_v<CatalogSnapshot>);
static_assert(!std::is_move_assignable_v<CatalogSnapshot>);

[[nodiscard]] SyntaxTree ParseDefinition(std::string_view sql) {
  auto result = ParseOne(Utf8View{sql});
  if (!result.has_value() || !result->tree.has_value()) {
    throw std::logic_error{"catalog test DDL did not parse"};
  }
  return std::move(*result->tree);
}

[[nodiscard]] SchemaExpression ExpressionRef(SchemaDefinitionId definition,
                                             ExpressionId expression) {
  return SchemaExpression{
      .definition = definition,
      .expression = expression,
  };
}

[[nodiscard]] CatalogIndexTerm ColumnTerm(std::size_t column, std::string collation = "BINARY",
                                          SortOrder order = SortOrder::kAscending) {
  return CatalogIndexTerm{
      .target = ColumnId{column},
      .collation_name = std::move(collation),
      .order = order,
  };
}

[[nodiscard]] CatalogIndexTerm ExpressionTerm(SchemaDefinitionId definition,
                                              ExpressionId expression,
                                              std::string collation = "BINARY",
                                              SortOrder order = SortOrder::kAscending) {
  return CatalogIndexTerm{
      .target = ExpressionRef(definition, expression),
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

[[nodiscard]] CatalogSnapshotPtr Build(CatalogInput input) {
  auto result = CatalogSnapshot::Create(std::move(input));
  if (!result.has_value()) {
    throw std::logic_error{result.error().detail};
  }
  return *std::move(result);
}

void ExpectValidationError(const CatalogSnapshotResult& result, CatalogValidationCode code,
                           CatalogObjectKind owner_kind, std::size_t owner_index,
                           CatalogMemberKind member_kind = CatalogMemberKind::kNone,
                           std::size_t member_index = 0) {
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(code, result.error().code);
  EXPECT_EQ(owner_kind, result.error().location.owner_kind);
  EXPECT_EQ(owner_index, result.error().location.owner_index);
  EXPECT_EQ(member_kind, result.error().location.member_kind);
  EXPECT_EQ(member_index, result.error().location.member_index);
  EXPECT_FALSE(result.error().detail.empty());
}

[[nodiscard]] CatalogInput MakeSimpleCatalog(std::string table_name = "items",
                                             std::string column_name = "value") {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 7, .generation = 1},
  };
  input.definitions.push_back(ParseDefinition("CREATE TABLE items(value TEXT)"));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = std::move(table_name),
      .root_page = RootPageId{2},
      .columns =
          {
              CatalogColumnInput{
                  .name = std::move(column_name),
                  .declared_type = "TEXT",
              },
          },
  });
  return input;
}

[[nodiscard]] CatalogInput MakeRowidCatalog() {
  SyntaxTree table_tree = ParseDefinition(
      "CREATE TABLE Items("
      "id INTEGER PRIMARY KEY, "
      "Name TEXT COLLATE NOCASE NOT NULL DEFAULT 'x', "
      "Score REAL, "
      "CHECK(Score > 0), "
      "UNIQUE(Name)"
      ")");
  const auto& table_statement = std::get<CreateTableStatement>(table_tree.statement());
  const auto& name_constraints = table_statement.columns[1].constraints;
  const auto default_iterator = std::ranges::find_if(name_constraints, [](const auto& constraint) {
    return std::holds_alternative<DefaultColumnConstraint>(constraint.payload);
  });
  if (default_iterator == name_constraints.end()) {
    throw std::logic_error{"default constraint missing"};
  }
  const ExpressionId default_expression =
      std::get<DefaultColumnConstraint>(default_iterator->payload).expression;
  const ExpressionId check_expression =
      std::get<CheckTableConstraint>(table_statement.constraints[0].payload).expression;

  SyntaxTree index_tree =
      ParseDefinition("CREATE INDEX score_idx ON Items(lower(Name), Score DESC) WHERE Score > 10");
  const auto& index_statement = std::get<CreateIndexStatement>(index_tree.statement());
  if (!index_statement.where.has_value()) {
    throw std::logic_error{"partial predicate missing"};
  }
  const ExpressionId indexed_expression = index_statement.terms[0].expression;
  const ExpressionId partial_predicate = index_statement.where.value();

  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 19, .generation = 4},
  };
  input.definitions.push_back(std::move(table_tree));
  input.definitions.push_back(std::move(index_tree));
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
                  .not_null_conflict = ConflictAction::kDefault,
                  .default_expression = ExpressionRef(SchemaDefinitionId{0}, default_expression),
              },
              CatalogColumnInput{
                  .name = "Score",
                  .declared_type = "REAL",
              },
          },
      .check_constraints =
          {
              ExpressionRef(SchemaDefinitionId{0}, check_expression),
          },
      .rowid_alias = ColumnId{0},
      .rowid_primary_key_conflict = ConflictAction::kDefault,
      .statistics =
          TableStatistics{
              .has_stat1 = true,
              .estimated_rows = 100,
              .average_row_size = 24,
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{0},
      .name = "sqlite_autoindex_Items_1",
      .table = TableId{0},
      .root_page = RootPageId{3},
      .origin = IndexOrigin::kUniqueConstraint,
      .unique = true,
      .conflict_action = ConflictAction::kDefault,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(1, "NOCASE"),
              RowIdTerm(),
          },
      .statistics =
          IndexStatistics{
              .has_stat1 = true,
              .rows_per_prefix = {100, 1},
              .average_row_size = 16,
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{1},
      .name = "score_idx",
      .table = TableId{0},
      .root_page = RootPageId{4},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 2,
      .terms =
          {
              ExpressionTerm(SchemaDefinitionId{1}, indexed_expression),
              ColumnTerm(2, "BINARY", SortOrder::kDescending),
              RowIdTerm(),
          },
      .partial_predicate = ExpressionRef(SchemaDefinitionId{1}, partial_predicate),
  });
  return input;
}

[[nodiscard]] CatalogInput MakeStrictCatalog(std::string any_type = "ANY") {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 3, .generation = 2},
  };
  input.definitions.push_back(
      ParseDefinition("CREATE TABLE strict_t(a ANY, b INT, PRIMARY KEY(b)) STRICT"));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = "strict_t",
      .root_page = RootPageId{2},
      .columns =
          {
              CatalogColumnInput{
                  .name = "a",
                  .declared_type = std::move(any_type),
              },
              CatalogColumnInput{
                  .name = "b",
                  .declared_type = "INT",
                  .primary_key = true,
              },
          },
      .strict = true,
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{0},
      .name = "sqlite_autoindex_strict_t_1",
      .table = TableId{0},
      .root_page = RootPageId{3},
      .origin = IndexOrigin::kPrimaryKey,
      .unique = true,
      .conflict_action = ConflictAction::kDefault,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(1),
              RowIdTerm(),
          },
  });
  return input;
}

[[nodiscard]] CatalogInput MakeDescendingPrimaryKeyCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 5, .generation = 2},
  };
  input.definitions.push_back(
      ParseDefinition("CREATE TABLE descending_pk(id INTEGER PRIMARY KEY DESC)"));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = "descending_pk",
      .root_page = RootPageId{2},
      .columns =
          {
              CatalogColumnInput{
                  .name = "id",
                  .declared_type = "INTEGER",
                  .primary_key = true,
              },
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{0},
      .name = "sqlite_autoindex_descending_pk_1",
      .table = TableId{0},
      .root_page = RootPageId{3},
      .origin = IndexOrigin::kPrimaryKey,
      .unique = true,
      .conflict_action = ConflictAction::kDefault,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(0, "BINARY", SortOrder::kDescending),
              RowIdTerm(),
          },
  });
  return input;
}

[[nodiscard]] CatalogInput MakeWithoutRowidCatalog() {
  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 11, .generation = 6},
  };
  input.definitions.push_back(
      ParseDefinition("CREATE TABLE wr("
                      "a INT, b TEXT, c TEXT COLLATE NOCASE, "
                      "PRIMARY KEY(a DESC, b), UNIQUE(c), UNIQUE(a)"
                      ") WITHOUT ROWID"));
  input.definitions.push_back(ParseDefinition("CREATE INDEX wr_c_explicit ON wr(c)"));
  input.definitions.push_back(ParseDefinition("CREATE UNIQUE INDEX wr_a_explicit ON wr(a)"));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = "wr",
      .root_page = RootPageId{2},
      .columns =
          {
              CatalogColumnInput{
                  .name = "a",
                  .declared_type = "INT",
                  .primary_key = true,
              },
              CatalogColumnInput{
                  .name = "b",
                  .declared_type = "TEXT",
                  .primary_key = true,
              },
              CatalogColumnInput{
                  .name = "c",
                  .declared_type = "TEXT",
                  .collation_name = "NOCASE",
              },
          },
      .without_rowid = true,
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{0},
      .name = "sqlite_autoindex_wr_1",
      .table = TableId{0},
      .root_page = RootPageId{2},
      .origin = IndexOrigin::kPrimaryKey,
      .unique = true,
      .conflict_action = ConflictAction::kDefault,
      .key_term_count = 2,
      .terms =
          {
              ColumnTerm(0, "BINARY", SortOrder::kDescending),
              ColumnTerm(1),
              ColumnTerm(2, "NOCASE"),
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{0},
      .name = "sqlite_autoindex_wr_2",
      .table = TableId{0},
      .root_page = RootPageId{3},
      .origin = IndexOrigin::kUniqueConstraint,
      .unique = true,
      .conflict_action = ConflictAction::kDefault,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(2, "NOCASE"),
              ColumnTerm(0),
              ColumnTerm(1),
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{0},
      .name = "sqlite_autoindex_wr_3",
      .table = TableId{0},
      .root_page = RootPageId{4},
      .origin = IndexOrigin::kUniqueConstraint,
      .unique = true,
      .conflict_action = ConflictAction::kDefault,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(0),
              ColumnTerm(1),
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{1},
      .name = "wr_c_explicit",
      .table = TableId{0},
      .root_page = RootPageId{5},
      .origin = IndexOrigin::kCreateIndex,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(2, "NOCASE"),
              ColumnTerm(0, "BINARY", SortOrder::kDescending),
              ColumnTerm(1),
          },
  });
  input.indexes.push_back(CatalogIndexInput{
      .definition = SchemaDefinitionId{2},
      .name = "wr_a_explicit",
      .table = TableId{0},
      .root_page = RootPageId{6},
      .origin = IndexOrigin::kCreateIndex,
      .unique = true,
      .conflict_action = ConflictAction::kDefault,
      .key_term_count = 1,
      .terms =
          {
              ColumnTerm(0),
              ColumnTerm(1),
          },
  });
  return input;
}

TEST(CatalogAffinity, MatchesSQLiteDeclaredTypePriority) {
  EXPECT_EQ(TypeAffinity::kBlob, DetermineTypeAffinity(std::nullopt));
  EXPECT_EQ(TypeAffinity::kNumeric, DetermineTypeAffinity(""));
  EXPECT_EQ(TypeAffinity::kInteger, DetermineTypeAffinity("BLOBINT"));
  EXPECT_EQ(TypeAffinity::kInteger, DetermineTypeAffinity("REALINT"));
  EXPECT_EQ(TypeAffinity::kText, DetermineTypeAffinity("CLOBBER"));
  EXPECT_EQ(TypeAffinity::kText, DetermineTypeAffinity("VARCHAR(20)"));
  EXPECT_EQ(TypeAffinity::kBlob, DetermineTypeAffinity("BLOB"));
  EXPECT_EQ(TypeAffinity::kReal, DetermineTypeAffinity("DOUBLE PRECISION"));
  EXPECT_EQ(TypeAffinity::kNumeric, DetermineTypeAffinity("BOOLEAN"));
  EXPECT_EQ(TypeAffinity::kNumeric, DetermineTypeAffinity("ANY"));
}

TEST(CatalogSnapshot, OwnsDefinitionsObjectsAndStableDenseIdentity) {
  const CatalogSnapshotPtr snapshot = Build(MakeRowidCatalog());

  EXPECT_EQ("main", snapshot->schema_name());
  EXPECT_EQ((CatalogVersion{.schema_cookie = 19, .generation = 4}), snapshot->version());
  ASSERT_EQ(1U, snapshot->tables().size());
  ASSERT_EQ(2U, snapshot->indexes().size());
  ASSERT_EQ(2U, snapshot->definitions().size());

  const CatalogTable& table = snapshot->table(TableId{0});
  EXPECT_EQ("Items", table.name);
  EXPECT_EQ(2U, table.root_page.value);
  ASSERT_EQ(3U, table.columns.size());
  EXPECT_EQ(std::optional<std::string>{"INTEGER"}, table.columns[0].declared_type);
  EXPECT_EQ(TypeAffinity::kInteger, table.columns[0].affinity);
  EXPECT_EQ(TypeAffinity::kText, table.columns[1].affinity);
  EXPECT_EQ(TypeAffinity::kReal, table.columns[2].affinity);
  EXPECT_EQ(std::optional{ConflictAction::kDefault}, table.columns[1].declared_not_null_conflict);
  EXPECT_EQ(std::optional{ConflictAction::kDefault}, table.columns[1].effective_not_null_conflict);
  EXPECT_EQ(ColumnId{0}, table.rowid_alias);
  EXPECT_EQ(100U, table.statistics.estimated_rows);

  ASSERT_EQ(2U, snapshot->table_indexes(TableId{0}).size());
  EXPECT_EQ(IndexId{0}, snapshot->table_indexes(TableId{0})[0]);
  EXPECT_EQ(IndexId{1}, snapshot->table_indexes(TableId{0})[1]);
  EXPECT_FALSE(snapshot->primary_key_index(TableId{0}).has_value());

  const CatalogIndex& automatic = snapshot->index(IndexId{0});
  EXPECT_TRUE(automatic.unique_not_null);
  EXPECT_EQ(1U, automatic.key_term_count);
  EXPECT_TRUE(std::holds_alternative<ColumnId>(automatic.terms[0].target));
  EXPECT_TRUE(std::holds_alternative<RowIdIndexTerm>(automatic.terms[1].target));
  EXPECT_EQ((std::vector<std::uint64_t>{100, 1}), automatic.statistics.rows_per_prefix);

  const CatalogIndex& expression_index = snapshot->index(IndexId{1});
  EXPECT_FALSE(expression_index.unique_not_null);
  ASSERT_TRUE(std::holds_alternative<SchemaExpression>(expression_index.terms[0].target));
  const SchemaExpression expression = std::get<SchemaExpression>(expression_index.terms[0].target);
  EXPECT_TRUE(
      std::holds_alternative<FunctionCallExpression>(snapshot->expression(expression).payload));
  ASSERT_TRUE(expression_index.partial_predicate.has_value());
  EXPECT_TRUE(std::holds_alternative<BinaryExpression>(
      snapshot->expression(*expression_index.partial_predicate).payload));

  EXPECT_EQ("CREATE TABLE Items",
            snapshot->definition(SchemaDefinitionId{0}).source().bytes().substr(0, 18));
  EXPECT_EQ("CREATE INDEX score_idx",
            snapshot->definition(SchemaDefinitionId{1}).source().bytes().substr(0, 22));
}

TEST(CatalogSnapshot, PerformsAllocationFreeAsciiCaseInsensitiveLookup) {
  std::string table_name{"F"};
  table_name.push_back(static_cast<char>(0xC0));
  std::string folded_name{"f"};
  folded_name.push_back(static_cast<char>(0xC0));
  std::string different_name{"f"};
  different_name.push_back(static_cast<char>(0xE0));
  std::string column_name{"C"};
  column_name.push_back(static_cast<char>(0xC0));
  std::string folded_column{"c"};
  folded_column.push_back(static_cast<char>(0xC0));

  const CatalogSnapshotPtr snapshot =
      Build(MakeSimpleCatalog(std::move(table_name), std::move(column_name)));

  EXPECT_EQ(TableId{0}, snapshot->FindTable(folded_name));
  EXPECT_FALSE(snapshot->FindTable(different_name).has_value());
  EXPECT_EQ(ColumnId{0}, snapshot->FindColumn(TableId{0}, folded_column));
  EXPECT_FALSE(snapshot->FindIndex("missing").has_value());

  CatalogInput empty_input = MakeSimpleCatalog("", "");
  const CatalogSnapshotPtr empty_snapshot = Build(std::move(empty_input));
  EXPECT_EQ(TableId{0}, empty_snapshot->FindTable(""));
  EXPECT_EQ(ColumnId{0}, empty_snapshot->FindColumn(TableId{0}, ""));
}

TEST(CatalogSnapshot, KeepsOldVersionAliveAfterReplacement) {
  CatalogInput first_input = MakeSimpleCatalog();
  first_input.version = CatalogVersion{.schema_cookie = 5, .generation = 1};
  CatalogInput second_input = MakeSimpleCatalog();
  second_input.version = CatalogVersion{.schema_cookie = 5, .generation = 2};

  const CatalogSnapshotPtr first = Build(std::move(first_input));
  const CatalogSnapshotPtr second = Build(std::move(second_input));

  EXPECT_NE(first->version(), second->version());
  EXPECT_EQ("items", first->table(TableId{0}).name);
  EXPECT_EQ("items", second->table(TableId{0}).name);
  EXPECT_NE(first.get(), second.get());
}

TEST(CatalogSnapshot, DerivesStrictAffinityNullabilityAndConstructionTimeUniqueness) {
  const CatalogSnapshotPtr snapshot = Build(MakeStrictCatalog());
  const CatalogTable& table = snapshot->table(TableId{0});
  const CatalogIndex& primary = snapshot->index(IndexId{0});

  EXPECT_EQ(TypeAffinity::kBlob, table.columns[0].affinity);
  EXPECT_EQ(TypeAffinity::kInteger, table.columns[1].affinity);
  EXPECT_FALSE(table.columns[1].declared_not_null_conflict.has_value());
  EXPECT_EQ(ConflictAction::kAbort, table.columns[1].effective_not_null_conflict);
  EXPECT_FALSE(primary.unique_not_null);
  EXPECT_EQ(IndexId{0}, snapshot->primary_key_index(TableId{0}));

  const auto invalid = CatalogSnapshot::Create(MakeStrictCatalog("VARCHAR(10)"));
  ExpectValidationError(invalid, CatalogValidationCode::kInvalidTableShape,
                        CatalogObjectKind::kTable, 0, CatalogMemberKind::kColumn, 0);
}

TEST(CatalogSnapshot, RepresentsDescendingIntegerPrimaryKeyAsAnIndex) {
  const CatalogSnapshotPtr snapshot = Build(MakeDescendingPrimaryKeyCatalog());
  const CatalogTable& table = snapshot->table(TableId{0});

  EXPECT_FALSE(table.rowid_alias.has_value());
  EXPECT_EQ(IndexId{0}, snapshot->primary_key_index(TableId{0}));
  const CatalogIndex& primary = snapshot->index(IndexId{0});
  EXPECT_EQ(SortOrder::kDescending, primary.terms[0].order);
  EXPECT_FALSE(primary.unique_not_null);
}

TEST(CatalogSnapshot, RepresentsWithoutRowidPhysicalIndexShape) {
  const CatalogSnapshotPtr snapshot = Build(MakeWithoutRowidCatalog());
  const CatalogTable& table = snapshot->table(TableId{0});

  EXPECT_TRUE(table.without_rowid);
  EXPECT_EQ(ConflictAction::kAbort, table.columns[0].effective_not_null_conflict);
  EXPECT_EQ(ConflictAction::kAbort, table.columns[1].effective_not_null_conflict);
  EXPECT_EQ(IndexId{0}, snapshot->primary_key_index(TableId{0}));

  const CatalogIndex& primary = snapshot->index(IndexId{0});
  EXPECT_EQ(table.root_page, primary.root_page);
  EXPECT_TRUE(primary.unique_not_null);
  ASSERT_EQ(3U, primary.terms.size());
  EXPECT_EQ(SortOrder::kDescending, primary.terms[0].order);
  EXPECT_EQ(ColumnId{2}, std::get<ColumnId>(primary.terms[2].target));
  EXPECT_EQ("NOCASE", primary.terms[2].collation_name);

  const CatalogIndex& automatic_c = snapshot->index(IndexId{1});
  const CatalogIndex& automatic_a = snapshot->index(IndexId{2});
  const CatalogIndex& explicit_c = snapshot->index(IndexId{3});
  const CatalogIndex& explicit_a = snapshot->index(IndexId{4});
  EXPECT_EQ(SortOrder::kAscending, automatic_c.terms[1].order);
  EXPECT_EQ(SortOrder::kDescending, explicit_c.terms[1].order);
  EXPECT_FALSE(automatic_a.unique_not_null);
  EXPECT_TRUE(explicit_a.unique_not_null);
  EXPECT_EQ(ColumnId{1}, std::get<ColumnId>(automatic_a.terms[1].target));
}

TEST(CatalogValidation, RejectsDuplicateNamespacesAndColumns) {
  CatalogInput duplicate_tables = MakeSimpleCatalog("Alpha", "value");
  duplicate_tables.definitions.push_back(ParseDefinition("CREATE TABLE beta(value TEXT)"));
  duplicate_tables.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{1},
      .name = "aLPHA",
      .root_page = RootPageId{3},
      .columns =
          {
              CatalogColumnInput{
                  .name = "value",
                  .declared_type = "TEXT",
              },
          },
  });
  const auto table_result = CatalogSnapshot::Create(std::move(duplicate_tables));
  ExpectValidationError(table_result, CatalogValidationCode::kDuplicateName,
                        CatalogObjectKind::kTable, 1);

  CatalogInput duplicate_columns = MakeSimpleCatalog();
  duplicate_columns.tables[0].columns.push_back(CatalogColumnInput{
      .name = "VaLuE",
      .declared_type = "INT",
  });
  const auto column_result = CatalogSnapshot::Create(std::move(duplicate_columns));
  ExpectValidationError(column_result, CatalogValidationCode::kDuplicateName,
                        CatalogObjectKind::kTable, 0, CatalogMemberKind::kColumn, 1);

  CatalogInput table_index_collision = MakeRowidCatalog();
  table_index_collision.indexes[1].name = "items";
  const auto collision_result = CatalogSnapshot::Create(std::move(table_index_collision));
  ExpectValidationError(collision_result, CatalogValidationCode::kDuplicateName,
                        CatalogObjectKind::kIndex, 1);
}

TEST(CatalogValidation, RejectsInvalidStringsReferencesAndDefinitionOwnership) {
  CatalogInput embedded_null = MakeSimpleCatalog();
  embedded_null.tables[0].name = std::string{"bad\0name", 8};
  const auto string_result = CatalogSnapshot::Create(std::move(embedded_null));
  ExpectValidationError(string_result, CatalogValidationCode::kInvalidString,
                        CatalogObjectKind::kTable, 0);

  CatalogInput invalid_default = MakeRowidCatalog();
  auto& default_expression = invalid_default.tables[0].columns[1].default_expression;
  if (!default_expression.has_value()) {
    throw std::logic_error{"default expression missing"};
  }
  default_expression.value().definition = SchemaDefinitionId{1};
  const auto default_result = CatalogSnapshot::Create(std::move(invalid_default));
  ExpectValidationError(default_result, CatalogValidationCode::kDefinitionMismatch,
                        CatalogObjectKind::kTable, 0, CatalogMemberKind::kDefaultExpression, 1);

  CatalogInput unowned = MakeSimpleCatalog();
  unowned.definitions.push_back(ParseDefinition("CREATE TABLE orphan(x)"));
  const auto unowned_result = CatalogSnapshot::Create(std::move(unowned));
  ExpectValidationError(unowned_result, CatalogValidationCode::kUnownedDefinition,
                        CatalogObjectKind::kDefinition, 1);
}

TEST(CatalogValidation, RejectsInvalidRootsAndPhysicalIndexTerms) {
  CatalogInput reserved_root = MakeSimpleCatalog();
  reserved_root.tables[0].root_page = RootPageId{1};
  const auto reserved_result = CatalogSnapshot::Create(std::move(reserved_root));
  ExpectValidationError(reserved_result, CatalogValidationCode::kInvalidRootPage,
                        CatalogObjectKind::kTable, 0);

  CatalogInput oversized_root = MakeRowidCatalog();
  oversized_root.indexes[0].root_page = RootPageId{std::numeric_limits<std::uint32_t>::max()};
  const auto oversized_result = CatalogSnapshot::Create(std::move(oversized_root));
  ExpectValidationError(oversized_result, CatalogValidationCode::kInvalidRootPage,
                        CatalogObjectKind::kIndex, 0);

  CatalogInput duplicate_root = MakeRowidCatalog();
  duplicate_root.indexes[0].root_page = RootPageId{2};
  const auto root_result = CatalogSnapshot::Create(std::move(duplicate_root));
  ExpectValidationError(root_result, CatalogValidationCode::kInvalidRootPage,
                        CatalogObjectKind::kIndex, 0);

  CatalogInput wrong_suffix_order = MakeWithoutRowidCatalog();
  wrong_suffix_order.indexes[1].terms[1].order = SortOrder::kDescending;
  const auto suffix_result = CatalogSnapshot::Create(std::move(wrong_suffix_order));
  ExpectValidationError(suffix_result, CatalogValidationCode::kInvalidIndexShape,
                        CatalogObjectKind::kIndex, 1, CatalogMemberKind::kIndexTerm, 1);

  CatalogInput wrong_auto_name = MakeWithoutRowidCatalog();
  wrong_auto_name.indexes[2].name = "sqlite_autoindex_wr_4";
  const auto name_result = CatalogSnapshot::Create(std::move(wrong_auto_name));
  ExpectValidationError(name_result, CatalogValidationCode::kInvalidIndexShape,
                        CatalogObjectKind::kIndex, 2);
}

TEST(CatalogValidation, RejectsUnfoldedEquivalentAutomaticConstraints) {
  CatalogInput input = MakeWithoutRowidCatalog();
  CatalogIndexInput duplicate = input.indexes[2];
  duplicate.name = "sqlite_autoindex_wr_4";
  duplicate.root_page = RootPageId{7};
  duplicate.terms[0].order = SortOrder::kDescending;
  input.indexes.push_back(std::move(duplicate));

  const auto result = CatalogSnapshot::Create(std::move(input));
  ExpectValidationError(result, CatalogValidationCode::kInvalidIndexShape,
                        CatalogObjectKind::kIndex, 5);
}

TEST(CatalogValidation, PreservesTolerantStatisticsButRejectsInvalidShape) {
  CatalogInput tolerant = MakeRowidCatalog();
  tolerant.indexes[0].statistics.rows_per_prefix = {0};
  tolerant.indexes[0].statistics.unordered = true;
  const CatalogSnapshotPtr snapshot = Build(std::move(tolerant));
  EXPECT_EQ((std::vector<std::uint64_t>{0}),
            snapshot->index(IndexId{0}).statistics.rows_per_prefix);

  CatalogInput too_many = MakeRowidCatalog();
  too_many.indexes[0].statistics.rows_per_prefix = {10, 5, 1};
  const auto count_result = CatalogSnapshot::Create(std::move(too_many));
  ExpectValidationError(count_result, CatalogValidationCode::kInvalidStatistics,
                        CatalogObjectKind::kIndex, 0, CatalogMemberKind::kStatisticValue, 2);

  CatalogInput no_row = MakeRowidCatalog();
  no_row.indexes[1].statistics.unordered = true;
  const auto no_row_result = CatalogSnapshot::Create(std::move(no_row));
  ExpectValidationError(no_row_result, CatalogValidationCode::kInvalidStatistics,
                        CatalogObjectKind::kIndex, 1);

  CatalogInput small_size = MakeRowidCatalog();
  small_size.tables[0].statistics.average_row_size = 1;
  const auto size_result = CatalogSnapshot::Create(std::move(small_size));
  ExpectValidationError(size_result, CatalogValidationCode::kInvalidStatistics,
                        CatalogObjectKind::kTable, 0, CatalogMemberKind::kStatisticValue, 0);
}

}  // namespace
}  // namespace modern_sqlite
