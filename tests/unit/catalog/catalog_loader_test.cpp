#include "modern_sqlite/catalog/catalog_loader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/native_vfs.hpp"

namespace modern_sqlite {
namespace {

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
    throw std::runtime_error(std::string{message});
  }
  return *value;
}

void TakeStatus(Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "catalog_loader" / "sqlite-3.54.0-catalog.db";
}

[[nodiscard]] std::filesystem::path CompatibilityFixturePath() {
  return FixturePath().parent_path() / "sqlite-3.54.0-expression-compatibility.db";
}

[[nodiscard]] std::filesystem::path InvalidExpressionFixturePath() {
  return FixturePath().parent_path() / "sqlite-3.54.0-invalid-expression.db";
}

[[nodiscard]] std::filesystem::path InvalidFunctionFixturePath() {
  return FixturePath().parent_path() / "sqlite-3.54.0-invalid-function.db";
}

[[nodiscard]] std::filesystem::path DuplicateAutomaticRootFixturePath() {
  return FixturePath().parent_path() / "sqlite-3.54.0-duplicate-automatic-root.db";
}

[[nodiscard]] std::filesystem::path Stat1ShapeFixturePath() {
  return FixturePath().parent_path() / "sqlite-3.54.0-stat1-shape.db";
}

[[nodiscard]] CatalogSnapshotPtr LoadDefaultFixture(
    std::string_view name = "sqlite-3.54.0-default-extraction.db") {
  NativeVfs vfs;
  auto database =
      TakeValue(BtreeDatabase::Open(vfs, (FixturePath().parent_path() / name).string()));
  TakeStatus(database->pager().BeginRead());
  CatalogSnapshotPtr catalog = TakeValue(LoadCatalog(*database));
  TakeStatus(database->pager().EndRead());
  return catalog;
}

[[nodiscard]] const SqlValue& MissingDefault(const CatalogSnapshot& catalog, TableId table,
                                             std::string_view column_name) {
  const ColumnId column =
      TakeOptional(catalog.FindColumn(table, column_name), "missing default column");
  const auto& value = catalog.column(table, column).missing_record_value;
  if (value == nullptr) {
    throw std::runtime_error{"missing extracted default value"};
  }
  return *value;
}

[[nodiscard]] std::filesystem::path UnsupportedFixturePath() {
  return FixturePath().parent_path().parent_path() / "storage_diagnostics" /
         "sqlite-3.54.0-storage.db";
}

[[nodiscard]] std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("unable to open catalog loader fixture");
  }
  const std::vector<char> characters{std::istreambuf_iterator<char>{stream},
                                     std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes(characters.size());
  if (!characters.empty()) {
    std::memcpy(bytes.data(), characters.data(), characters.size());
  }
  return bytes;
}

void Write32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{bytes.data() + offset, 4}, value);
}

class TemporaryDatabase final {
 public:
  explicit TemporaryDatabase(std::vector<std::byte> bytes) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-catalog-loader-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(1)) + ".db");
    Write(std::move(bytes));
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~TemporaryDatabase() {
    std::error_code error;
    std::filesystem::remove(path_, error);
    std::filesystem::remove(path_.string() + "-journal", error);
    std::filesystem::remove(path_.string() + "-wal", error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  void RewriteSchemaCookie(std::uint32_t value) const {
    std::fstream stream(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
      throw std::runtime_error("unable to reopen temporary catalog database");
    }
    std::array<std::byte, 4> encoded{};
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{encoded}, value);
    stream.seekp(40);
    stream.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    if (!stream) {
      throw std::runtime_error("unable to rewrite temporary database header");
    }
  }

 private:
  void Write(std::vector<std::byte> bytes) {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("unable to create temporary catalog database");
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
      throw std::runtime_error("unable to write temporary catalog database");
    }
  }

  std::filesystem::path path_;
};

class CatalogLoaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    database_ = TakeValue(BtreeDatabase::Open(vfs_, FixturePath().string()));
    pager_ = &database_->pager();
    TakeStatus(pager_->BeginRead());
  }

  NativeVfs vfs_;
  std::unique_ptr<BtreeDatabase> database_;
  Pager* pager_ = nullptr;
};

TEST(CatalogLoaderDefaultTest, PreservesNumericSpellingUntilExtractionAffinity) {
  const CatalogSnapshotPtr catalog = LoadDefaultFixture();
  const TableId spelling = TakeOptional(catalog->FindTable("spelling"), "missing spelling table");
  const std::array expected{
      std::pair{"real_text", "5.00"},
      std::pair{"negative_zero", "-0.0"},
      std::pair{"decimal_overflow", "9223372036854775808"},
      std::pair{"minimum_text", "-0009223372036854775808"},
      std::pair{"large_text", "123456789012345678901.25"},
  };
  for (const auto& [column, text] : expected) {
    SCOPED_TRACE(column);
    const SqlValue& value = MissingDefault(*catalog, spelling, column);
    ASSERT_TRUE(value.text_value().has_value());
    EXPECT_EQ(std::string_view{text}, value.text_value()->bytes());
  }
}

TEST(CatalogLoaderDefaultTest, PreservesWrappedMinimumAndStrictAnyAffinity) {
  const CatalogSnapshotPtr catalog = LoadDefaultFixture();
  const TableId spelling = TakeOptional(catalog->FindTable("spelling"), "missing spelling table");
  const TableId strict =
      TakeOptional(catalog->FindTable("strict_defaults"), "missing strict defaults table");
  EXPECT_EQ(std::numeric_limits<std::int64_t>::min(),
            MissingDefault(*catalog, spelling, "wrapped_minimum").integer_value());
  EXPECT_EQ(std::numeric_limits<std::int64_t>::min(),
            MissingDefault(*catalog, strict, "minimum").integer_value());
  EXPECT_EQ(5.0, MissingDefault(*catalog, strict, "real_value").real_value());
  EXPECT_EQ(5, MissingDefault(*catalog, spelling, "ordinary_any").integer_value());
}

TEST(CatalogLoaderDefaultTest, KeepsNumericAffinityInsideRealNegation) {
  const CatalogSnapshotPtr catalog = LoadDefaultFixture();
  const TableId zeroes = TakeOptional(catalog->FindTable("zeroes"), "missing zeroes table");
  for (const std::string_view column : {"real_zero", "nested_zero", "text_zero", "blob_zero"}) {
    SCOPED_TRACE(column);
    const double value = TakeOptional(MissingDefault(*catalog, zeroes, column).real_value(),
                                      "expected REAL default");
    EXPECT_EQ(0.0, value);
    EXPECT_FALSE(std::signbit(value));
  }
  const double blob_zero =
      TakeOptional(MissingDefault(*catalog, zeroes, "blob_negative_zero").real_value(),
                   "expected BLOB-affinity real default");
  EXPECT_EQ(0.0, blob_zero);
  EXPECT_TRUE(std::signbit(blob_zero));
}

TEST(CatalogLoaderDefaultTest, OwnsUnaryLiteralValuesAfterTheLoadingDatabaseIsReleased) {
  CatalogSnapshotPtr loaded = LoadDefaultFixture();
  const CatalogSnapshotPtr catalog = std::move(loaded);
  const TableId unary =
      TakeOptional(catalog->FindTable("unary_defaults"), "missing unary defaults table");
  EXPECT_EQ("5.0", TakeOptional(MissingDefault(*catalog, unary, "positive_text").text_value(),
                                "expected unary text")
                       .bytes());
  EXPECT_EQ(-5, MissingDefault(*catalog, unary, "negative_text").integer_value());
  const ByteView positive_blob = TakeOptional(
      MissingDefault(*catalog, unary, "positive_blob").blob_value(), "expected unary blob");
  ASSERT_EQ(3U, positive_blob.size());
  EXPECT_EQ(std::byte{0x35}, positive_blob[0]);
  EXPECT_EQ(std::byte{0x2e}, positive_blob[1]);
  EXPECT_EQ(std::byte{0x30}, positive_blob[2]);
  EXPECT_EQ("-5", TakeOptional(MissingDefault(*catalog, unary, "negative_blob").text_value(),
                               "expected numerified blob")
                      .bytes());
  EXPECT_EQ(SqlValueType::kNull, MissingDefault(*catalog, unary, "positive_null").type());
  EXPECT_EQ(SqlValueType::kNull, MissingDefault(*catalog, unary, "negative_null").type());
  EXPECT_EQ("a long immutable default value",
            TakeOptional(MissingDefault(*catalog, unary, "owned_text").text_value(),
                         "expected owned text")
                .bytes());
  const ByteView owned_blob = TakeOptional(
      MissingDefault(*catalog, unary, "owned_blob").blob_value(), "expected owned blob");
  ASSERT_EQ(16U, owned_blob.size());
  for (std::size_t index = 0; index < owned_blob.size(); ++index) {
    EXPECT_EQ(static_cast<std::byte>(index), owned_blob[index]);
  }
}

TEST(CatalogLoaderDefaultTest, ExtractsWideHexadecimalTokensWithoutCorruptingTheSchema) {
  const CatalogSnapshotPtr catalog = LoadDefaultFixture("sqlite-3.54.0-hex-default-extraction.db");
  const TableId hexadecimal =
      TakeOptional(catalog->FindTable("hexadecimal"), "missing hexadecimal table");
  EXPECT_EQ("0x10000000000000000",
            TakeOptional(MissingDefault(*catalog, hexadecimal, "positive").text_value(),
                         "expected wide hexadecimal text")
                .bytes());
  EXPECT_EQ(0, MissingDefault(*catalog, hexadecimal, "negative").integer_value());
  EXPECT_EQ(0x1p63, MissingDefault(*catalog, hexadecimal, "signed_minimum").real_value());
}

TEST(CatalogLoaderDefaultTest, RejectsInvalidStrictTypesWithoutAFallbackAffinity) {
  NativeVfs vfs;
  auto database = TakeValue(BtreeDatabase::Open(
      vfs, (FixturePath().parent_path() / "sqlite-3.54.0-invalid-strict-default.db").string()));
  TakeStatus(database->pager().BeginRead());
  const auto catalog = LoadCatalog(*database);
  ASSERT_FALSE(catalog.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, catalog.error().code());
  EXPECT_NE(std::string_view::npos, catalog.error().message().find("STRICT column"));
  TakeStatus(database->pager().EndRead());
}

TEST(CatalogLoaderDefaultTest, RecognizesUnquotedBooleanDefaultsBeforeColumnNameLookup) {
  const CatalogSnapshotPtr catalog =
      LoadDefaultFixture("sqlite-3.54.0-boolean-default-extraction.db");
  const TableId booleans = TakeOptional(catalog->FindTable("booleans"), "missing booleans table");
  EXPECT_EQ(0, MissingDefault(*catalog, booleans, "bare_false").integer_value());
  EXPECT_EQ(0, MissingDefault(*catalog, booleans, "wrapped_false").integer_value());
  EXPECT_EQ(1, MissingDefault(*catalog, booleans, "positive_true").integer_value());
  const double negative_false =
      TakeOptional(MissingDefault(*catalog, booleans, "negative_false").real_value(),
                   "expected a REAL Boolean default");
  EXPECT_EQ(0.0, negative_false);
  EXPECT_FALSE(std::signbit(negative_false));
  EXPECT_EQ("-1", TakeOptional(MissingDefault(*catalog, booleans, "negative_true").text_value(),
                               "expected a TEXT Boolean default")
                      .bytes());
  EXPECT_EQ("FALSE", TakeOptional(MissingDefault(*catalog, booleans, "quoted_text").text_value(),
                                  "expected quoted text rather than a Boolean")
                         .bytes());
  const TableId opaque = TakeOptional(catalog->FindTable("opaque"), "missing opaque table");
  const CatalogColumn& deferred = catalog->column(opaque, ColumnId{1});
  EXPECT_TRUE(deferred.default_expression.has_value());
  EXPECT_EQ(nullptr, deferred.missing_record_value);
}

TEST(CatalogLoaderDefaultTest, RejectsQuotedBooleanIdentifiersInDefaults) {
  NativeVfs vfs;
  auto database = TakeValue(BtreeDatabase::Open(
      vfs, (FixturePath().parent_path() / "sqlite-3.54.0-invalid-boolean-default.db").string()));
  TakeStatus(database->pager().BeginRead());
  const auto catalog = LoadCatalog(*database);
  ASSERT_FALSE(catalog.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, catalog.error().code());
  EXPECT_NE(std::string_view::npos,
            catalog.error().message().find("DEFAULT expression contains a column reference"));
  TakeStatus(database->pager().EndRead());
}

TEST_F(CatalogLoaderTest, LoadsPinnedSqliteSchemaIntoCanonicalSnapshot) {
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*database_, CatalogLoadOptions{.generation = 41}));

  EXPECT_EQ("main", catalog->schema_name());
  EXPECT_EQ(41U, catalog->version().generation);
  ASSERT_NE(nullptr, pager_->header());
  EXPECT_EQ(pager_->header()->schema_cookie(), catalog->version().schema_cookie);
  EXPECT_EQ(8U, catalog->tables().size());
  EXPECT_EQ(10U, catalog->indexes().size());
  EXPECT_EQ(11U, catalog->definitions().size());

  const TableId items_id = TakeOptional(catalog->FindTable("ITEMS"), "missing items table");
  const CatalogTable& items = catalog->table(items_id);
  ASSERT_EQ(4U, items.columns.size());
  EXPECT_EQ(ColumnId{0}, items.rowid_alias);
  EXPECT_TRUE(items.strict);
  EXPECT_TRUE(items.autoincrement);
  EXPECT_EQ(3U, catalog->table_indexes(items_id).size());
  EXPECT_EQ(3U, items.statistics.estimated_rows);

  const CatalogColumn& name = catalog->column(items_id, ColumnId{1});
  EXPECT_EQ("TEXT", name.declared_type);
  EXPECT_EQ("NOCASE", name.collation_name);
  EXPECT_EQ(ConflictAction::kFail, name.effective_not_null_conflict);
  ASSERT_TRUE(name.default_expression.has_value());
  ASSERT_NE(nullptr, name.missing_record_value);
  ASSERT_TRUE(name.missing_record_value->text_value().has_value());
  EXPECT_EQ("unknown", name.missing_record_value->text_value()->bytes());
  EXPECT_EQ(nullptr, catalog->column(items_id, ColumnId{2}).missing_record_value);

  const IndexId score_index_id =
      TakeOptional(catalog->FindIndex("items_score_idx"), "missing items_score_idx");
  const CatalogIndex& score_index = catalog->index(score_index_id);
  EXPECT_EQ(items_id, score_index.table);
  EXPECT_EQ(2U, score_index.key_term_count);
  ASSERT_EQ(3U, score_index.terms.size());
  EXPECT_EQ(SortOrder::kDescending, score_index.terms[0].order);
  EXPECT_TRUE(std::holds_alternative<SchemaExpression>(score_index.terms[1].target));
  EXPECT_TRUE(std::holds_alternative<RowIdIndexTerm>(score_index.terms[2].target));
  EXPECT_TRUE(score_index.partial_predicate.has_value());
  EXPECT_EQ((std::vector<std::uint64_t>{120, 12, 0}), score_index.statistics.rows_per_prefix);
  EXPECT_EQ(24U, score_index.statistics.average_row_size);
  EXPECT_TRUE(score_index.statistics.unordered);
  EXPECT_TRUE(score_index.statistics.no_skip_scan);

  const TableId wr_id = TakeOptional(catalog->FindTable("wr"), "missing wr table");
  const CatalogTable& wr = catalog->table(wr_id);
  EXPECT_TRUE(wr.without_rowid);
  EXPECT_TRUE(wr.strict);
  EXPECT_FALSE(wr.rowid_alias.has_value());
  const IndexId wr_primary_id =
      TakeOptional(catalog->primary_key_index(wr_id), "missing wr primary key");
  const CatalogIndex& wr_primary = catalog->index(wr_primary_id);
  EXPECT_EQ("sqlite_autoindex_wr_1", wr_primary.name);
  EXPECT_EQ(wr.root_page, wr_primary.root_page);
  EXPECT_EQ(IndexOrigin::kPrimaryKey, wr_primary.origin);
  EXPECT_EQ(2U, wr_primary.key_term_count);
  ASSERT_EQ(4U, wr_primary.terms.size());
  EXPECT_EQ(SortOrder::kDescending, wr_primary.terms[0].order);
  EXPECT_EQ((std::vector<std::uint64_t>{2, 1, 1}), wr_primary.statistics.rows_per_prefix);

  const CatalogIndex& wr_automatic = catalog->index(
      TakeOptional(catalog->FindIndex("sqlite_autoindex_wr_2"), "missing wr automatic index"));
  ASSERT_EQ(3U, wr_automatic.terms.size());
  EXPECT_EQ(SortOrder::kAscending, wr_automatic.terms[1].order);

  const CatalogIndex& wr_explicit =
      catalog->index(TakeOptional(catalog->FindIndex("wr_c_idx"), "missing wr explicit index"));
  ASSERT_EQ(3U, wr_explicit.terms.size());
  EXPECT_EQ(SortOrder::kDescending, wr_explicit.terms[1].order);

  const TableId descending_id =
      TakeOptional(catalog->FindTable("descending_pk"), "missing descending_pk table");
  EXPECT_FALSE(catalog->table(descending_id).rowid_alias.has_value());
  const CatalogIndex& descending_index = catalog->index(TakeOptional(
      catalog->FindIndex("sqlite_autoindex_descending_pk_1"), "missing descending primary key"));
  EXPECT_EQ(IndexOrigin::kPrimaryKey, descending_index.origin);
  EXPECT_EQ(SortOrder::kDescending, descending_index.terms[0].order);

  const TableId folded_id = TakeOptional(catalog->FindTable("folded"), "missing folded table");
  EXPECT_EQ(1U, catalog->table_indexes(folded_id).size());

  const TableId legacy_type_id =
      TakeOptional(catalog->FindTable("legacy_type"), "missing legacy_type table");
  const CatalogTable& legacy_type = catalog->table(legacy_type_id);
  EXPECT_EQ(ColumnId{0}, legacy_type.rowid_alias);
  EXPECT_EQ("INTEGER", legacy_type.columns[0].declared_type);

  const TableId legacy_quoted_id =
      TakeOptional(catalog->FindTable("legacy_quoted"), "missing legacy_quoted table");
  const CatalogIndex& legacy_quoted_automatic =
      catalog->index(TakeOptional(catalog->FindIndex("sqlite_autoindex_legacy_quoted_1"),
                                  "missing legacy quoted automatic index"));
  const CatalogIndex& legacy_quoted_explicit = catalog->index(
      TakeOptional(catalog->FindIndex("legacy_quoted_b"), "missing legacy quoted explicit index"));
  EXPECT_EQ(legacy_quoted_id, legacy_quoted_automatic.table);
  EXPECT_EQ(legacy_quoted_id, legacy_quoted_explicit.table);
  EXPECT_EQ(ColumnId{0}, std::get<ColumnId>(legacy_quoted_automatic.terms[0].target));
  EXPECT_EQ(ColumnId{1}, std::get<ColumnId>(legacy_quoted_explicit.terms[0].target));
}

TEST_F(CatalogLoaderTest, EnforcesIndependentResourceLimits) {
  for (const CatalogLoadOptions& options : {
           CatalogLoadOptions{.maximum_schema_rows = 1},
           CatalogLoadOptions{.maximum_schema_objects = 1},
           CatalogLoadOptions{.maximum_sql_bytes = 16},
           CatalogLoadOptions{.maximum_columns = 1},
       }) {
    const Result<CatalogSnapshotPtr> result = LoadCatalog(*database_, options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ErrorCode::kTooLarge, result.error().code());
  }
}

TEST_F(CatalogLoaderTest, DetectsWhetherTheActiveSnapshotNeedsReloading) {
  const CatalogSnapshotPtr catalog = TakeValue(LoadCatalog(*database_));
  const Result<bool> reload = CatalogRequiresReload(*pager_, *catalog);
  ASSERT_TRUE(reload.has_value());
  EXPECT_FALSE(*reload);
}

TEST(CatalogLoaderCompatibilityTest, NormalizesLegacyHeaderFieldsThroughTheLoader) {
  std::vector<std::byte> bytes = ReadBytes(FixturePath());
  Write32(bytes, 44, 0);
  Write32(bytes, 56, 5);
  const TemporaryDatabase database{std::move(bytes)};
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
  Pager* const pager = &pager_database->pager();
  TakeStatus(pager->BeginRead());

  const CatalogSnapshotPtr catalog = TakeValue(LoadCatalog(*pager_database));
  EXPECT_TRUE(catalog->FindTable("items").has_value());
}

TEST(CatalogLoaderCompatibilityTest, IgnoresDescendingFlagsBeforeSchemaFormatFour) {
  for (const std::uint32_t schema_format : {2U, 3U}) {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, 44, schema_format);
    const TemporaryDatabase database{std::move(bytes)};
    NativeVfs vfs;
    std::unique_ptr<BtreeDatabase> pager_database =
        TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
    Pager* const pager = &pager_database->pager();
    TakeStatus(pager->BeginRead());

    const CatalogSnapshotPtr catalog = TakeValue(LoadCatalog(*pager_database));
    const CatalogIndex& explicit_index = catalog->index(
        TakeOptional(catalog->FindIndex("items_score_idx"), "missing items_score_idx"));
    const CatalogIndex& primary_index = catalog->index(TakeOptional(
        catalog->FindIndex("sqlite_autoindex_descending_pk_1"), "missing descending primary key"));
    EXPECT_EQ(SortOrder::kAscending, explicit_index.terms.front().order);
    EXPECT_EQ(SortOrder::kAscending, primary_index.terms.front().order);
    EXPECT_FALSE(catalog
                     ->table(TakeOptional(catalog->FindTable("descending_pk"),
                                          "missing descending_pk table"))
                     .rowid_alias.has_value());
  }
}

TEST(CatalogLoaderCompatibilityTest, NormalizesLegacyDefaultVariablesAndPreservesUnknownFunctions) {
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, CompatibilityFixturePath().string()));
  Pager* const pager = &pager_database->pager();
  TakeStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog = TakeValue(LoadCatalog(*pager_database));

  const TableId table_id =
      TakeOptional(catalog->FindTable("compatibility"), "missing compatibility table");
  const CatalogColumn& column = catalog->column(table_id, ColumnId{1});
  ASSERT_TRUE(column.default_expression.has_value());
  ASSERT_NE(nullptr, column.missing_record_value);
  EXPECT_EQ(SqlValueType::kNull, column.missing_record_value->type());
  const Expression& default_expression = catalog->expression(*column.default_expression);
  const auto* parenthesized = std::get_if<ParenthesizedExpression>(&default_expression.payload);
  ASSERT_NE(nullptr, parenthesized);
  const auto* literal =
      std::get_if<LiteralExpression>(&catalog->definition(column.default_expression->definition)
                                          .expression(parenthesized->inner)
                                          .payload);
  ASSERT_NE(nullptr, literal);
  EXPECT_EQ(LiteralKind::kNull, literal->kind);

  const CatalogColumn& deferred_arity_column = catalog->column(table_id, ColumnId{2});
  ASSERT_TRUE(deferred_arity_column.default_expression.has_value());
  EXPECT_EQ(nullptr, deferred_arity_column.missing_record_value);
  const Expression& deferred_arity_expression =
      catalog->expression(*deferred_arity_column.default_expression);
  const auto* deferred_parenthesized =
      std::get_if<ParenthesizedExpression>(&deferred_arity_expression.payload);
  ASSERT_NE(nullptr, deferred_parenthesized);
  EXPECT_TRUE(std::holds_alternative<FunctionCallExpression>(
      catalog->definition(deferred_arity_column.default_expression->definition)
          .expression(deferred_parenthesized->inner)
          .payload));

  const CatalogIndex& automatic_index =
      catalog->index(TakeOptional(catalog->FindIndex("sqlite_autoindex_compatibility_1"),
                                  "missing compatibility automatic index"));
  EXPECT_EQ(ColumnId{0}, std::get<ColumnId>(automatic_index.terms.front().target));
  EXPECT_EQ("NOCASE", automatic_index.terms.front().collation_name);

  const CatalogIndex& index = catalog->index(
      TakeOptional(catalog->FindIndex("application_index"), "missing application index"));
  const SchemaExpression expression = std::get<SchemaExpression>(index.terms.front().target);
  EXPECT_TRUE(
      std::holds_alternative<FunctionCallExpression>(catalog->expression(expression).payload));

  const CatalogIndex& parenthesized_index = catalog->index(
      TakeOptional(catalog->FindIndex("parenthesized_index"), "missing parenthesized index"));
  EXPECT_EQ(ColumnId{0}, std::get<ColumnId>(parenthesized_index.terms.front().target));
  EXPECT_EQ("RTRIM", parenthesized_index.terms.front().collation_name);
  EXPECT_TRUE(parenthesized_index.partial_predicate.has_value());

  const CatalogIndex& scalar_min_index = catalog->index(
      TakeOptional(catalog->FindIndex("scalar_min_index"), "missing scalar_min_index"));
  const SchemaExpression scalar_min_expression =
      std::get<SchemaExpression>(scalar_min_index.terms.front().target);
  const auto* scalar_min =
      std::get_if<FunctionCallExpression>(&catalog->expression(scalar_min_expression).payload);
  ASSERT_NE(nullptr, scalar_min);
  EXPECT_EQ(2U, scalar_min->arguments.size());
}

TEST(CatalogLoaderCompatibilityTest, AppliesRepeatedAutomaticRootRowsInOrder) {
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, DuplicateAutomaticRootFixturePath().string()));
  Pager* const pager = &pager_database->pager();
  TakeStatus(pager->BeginRead());

  const CatalogSnapshotPtr catalog = TakeValue(LoadCatalog(*pager_database));
  ASSERT_EQ(1U, catalog->indexes().size());
  EXPECT_EQ("sqlite_autoindex_duplicate_root_1", catalog->indexes().front().name);
}

TEST(CatalogLoaderCompatibilityTest, SelectsStat1ColumnsByNameAndUsesSqliteSzParsing) {
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, Stat1ShapeFixturePath().string()));
  Pager* const pager = &pager_database->pager();
  TakeStatus(pager->BeginRead());

  const CatalogSnapshotPtr catalog = TakeValue(LoadCatalog(*pager_database));
  const CatalogIndex& index = catalog->index(
      TakeOptional(catalog->FindIndex("stat_target_index"), "missing stat target index"));
  EXPECT_EQ((std::vector<std::uint64_t>{100, 10}), index.statistics.rows_per_prefix);
  EXPECT_EQ(2U, index.statistics.average_row_size);
}

TEST(CatalogLoaderValidationTest, RejectsMissingSchemaExpressionColumns) {
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, InvalidExpressionFixturePath().string()));
  Pager* const pager = &pager_database->pager();
  TakeStatus(pager->BeginRead());

  const Result<CatalogSnapshotPtr> result = LoadCatalog(*pager_database);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, result.error().code());
}

TEST(CatalogLoaderValidationTest, RejectsKnownFunctionsWithInvalidArity) {
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, InvalidFunctionFixturePath().string()));
  Pager* const pager = &pager_database->pager();
  TakeStatus(pager->BeginRead());

  const Result<CatalogSnapshotPtr> result = LoadCatalog(*pager_database);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, result.error().code());
}

TEST(CatalogLoaderValidationTest, RejectsUnsupportedPersistentObjects) {
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, UnsupportedFixturePath().string()));
  Pager* const pager = &pager_database->pager();
  TakeStatus(pager->BeginRead());

  const Result<CatalogSnapshotPtr> result = LoadCatalog(*pager_database);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(ErrorCode::kProtocol, result.error().code());
}

TEST(CatalogLoaderReloadTest, DetectsASchemaCookieChangeAcrossSnapshots) {
  std::vector<std::byte> bytes = ReadBytes(FixturePath());
  const auto original_cookie =
      LoadBigEndian<std::uint32_t>(std::span<const std::byte, 4>{bytes.data() + 40, 4});
  const TemporaryDatabase database{std::move(bytes)};
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, database.path().string()));
  Pager* const pager = &pager_database->pager();
  TakeStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog = TakeValue(LoadCatalog(*pager_database));
  TakeStatus(pager->EndRead());

  database.RewriteSchemaCookie(original_cookie + 1U);
  TakeStatus(pager->BeginRead());
  const Result<bool> reload = CatalogRequiresReload(*pager, *catalog);
  ASSERT_TRUE(reload.has_value());
  EXPECT_TRUE(*reload);
}

TEST(CatalogLoaderTransactionTest, RequiresAnActiveReadTransaction) {
  NativeVfs vfs;
  std::unique_ptr<BtreeDatabase> pager_database =
      TakeValue(BtreeDatabase::Open(vfs, FixturePath().string()));
  Pager* const pager = &pager_database->pager();

  const Result<CatalogSnapshotPtr> load = LoadCatalog(*pager_database);
  ASSERT_FALSE(load.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, load.error().code());

  CatalogInput input{.schema_name = "main"};
  const CatalogSnapshotResult empty = CatalogSnapshot::Create(std::move(input));
  ASSERT_TRUE(empty.has_value());
  const Result<bool> reload = CatalogRequiresReload(*pager, **empty);
  ASSERT_FALSE(reload.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, reload.error().code());
}

}  // namespace
}  // namespace modern_sqlite
