#ifndef MODERN_SQLITE_CATALOG_CATALOG_HPP_
#define MODERN_SQLITE_CATALOG_CATALOG_HPP_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/syntax/ast.hpp"

namespace modern_sqlite {

struct TableId {
  std::size_t value = 0;

  constexpr auto operator<=>(const TableId&) const noexcept = default;
};

struct ColumnId {
  std::size_t value = 0;

  constexpr auto operator<=>(const ColumnId&) const noexcept = default;
};

struct IndexId {
  std::size_t value = 0;

  constexpr auto operator<=>(const IndexId&) const noexcept = default;
};

struct SchemaDefinitionId {
  std::size_t value = 0;

  constexpr auto operator<=>(const SchemaDefinitionId&) const noexcept = default;
};

struct RootPageId {
  std::uint32_t value = 0;

  constexpr auto operator<=>(const RootPageId&) const noexcept = default;
};

struct CatalogVersion {
  std::uint32_t schema_cookie = 0;
  std::uint64_t generation = 0;

  constexpr auto operator<=>(const CatalogVersion&) const noexcept = default;
};

enum class CatalogValidationCode : std::uint8_t {
  kInvalidString,
  kDuplicateName,
  kInvalidReference,
  kDefinitionMismatch,
  kInvalidTableShape,
  kInvalidIndexShape,
  kInvalidRootPage,
  kInvalidStatistics,
  kUnownedDefinition,
};

enum class CatalogObjectKind : std::uint8_t {
  kSchema,
  kDefinition,
  kTable,
  kIndex,
};

enum class CatalogMemberKind : std::uint8_t {
  kNone,
  kColumn,
  kCheckExpression,
  kDefaultExpression,
  kIndexTerm,
  kPartialPredicate,
  kStatisticValue,
};

struct CatalogValidationLocation {
  CatalogObjectKind owner_kind = CatalogObjectKind::kSchema;
  std::size_t owner_index = 0;
  CatalogMemberKind member_kind = CatalogMemberKind::kNone;
  std::size_t member_index = 0;

  constexpr auto operator<=>(const CatalogValidationLocation&) const noexcept = default;
};

struct CatalogValidationError {
  CatalogValidationCode code = CatalogValidationCode::kInvalidReference;
  CatalogValidationLocation location{};
  std::string detail{};
};

struct SchemaExpression {
  SchemaDefinitionId definition{};
  ExpressionId expression{};

  constexpr auto operator<=>(const SchemaExpression&) const noexcept = default;
};

struct RowIdIndexTerm {
  constexpr auto operator<=>(const RowIdIndexTerm&) const noexcept = default;
};

using CatalogIndexTermTarget = std::variant<ColumnId, SchemaExpression, RowIdIndexTerm>;

struct CatalogIndexTerm {
  CatalogIndexTermTarget target = ColumnId{};
  std::string collation_name = "BINARY";
  SortOrder order = SortOrder::kAscending;
};

enum class IndexOrigin : std::uint8_t {
  kCreateIndex,
  kUniqueConstraint,
  kPrimaryKey,
};

struct TableStatistics {
  bool has_stat1 = false;
  std::optional<std::uint64_t> estimated_rows{};
  std::optional<std::uint64_t> average_row_size{};
};

struct IndexStatistics {
  bool has_stat1 = false;
  std::vector<std::uint64_t> rows_per_prefix{};
  std::optional<std::uint64_t> average_row_size{};
  bool unordered = false;
  bool no_skip_scan = false;
};

struct CatalogColumnInput {
  std::string name{};
  std::optional<std::string> declared_type{};
  std::string collation_name = "BINARY";
  std::optional<ConflictAction> not_null_conflict{};
  bool primary_key = false;
  std::optional<SchemaExpression> default_expression{};
};

struct CatalogTableInput {
  SchemaDefinitionId definition{};
  std::string name{};
  RootPageId root_page{};
  std::vector<CatalogColumnInput> columns{};
  std::vector<SchemaExpression> check_constraints{};
  std::optional<ColumnId> rowid_alias{};
  ConflictAction rowid_primary_key_conflict = ConflictAction::kDefault;
  bool without_rowid = false;
  bool strict = false;
  bool autoincrement = false;
  TableStatistics statistics{};
};

struct CatalogIndexInput {
  SchemaDefinitionId definition{};
  std::string name{};
  TableId table{};
  RootPageId root_page{};
  IndexOrigin origin = IndexOrigin::kCreateIndex;
  bool unique = false;
  std::optional<ConflictAction> conflict_action{};
  std::size_t key_term_count = 0;
  std::vector<CatalogIndexTerm> terms{};
  std::optional<SchemaExpression> partial_predicate{};
  IndexStatistics statistics{};
};

struct CatalogColumn {
  std::string name{};
  std::optional<std::string> declared_type{};
  TypeAffinity affinity = TypeAffinity::kBlob;
  std::string collation_name = "BINARY";
  std::optional<ConflictAction> declared_not_null_conflict{};
  std::optional<ConflictAction> effective_not_null_conflict{};
  bool primary_key = false;
  std::optional<SchemaExpression> default_expression{};
};

struct CatalogTable {
  SchemaDefinitionId definition{};
  std::string name{};
  RootPageId root_page{};
  std::vector<CatalogColumn> columns{};
  std::vector<SchemaExpression> check_constraints{};
  std::optional<ColumnId> rowid_alias{};
  ConflictAction rowid_primary_key_conflict = ConflictAction::kDefault;
  bool without_rowid = false;
  bool strict = false;
  bool autoincrement = false;
  TableStatistics statistics{};
};

struct CatalogIndex {
  SchemaDefinitionId definition{};
  std::string name{};
  TableId table{};
  RootPageId root_page{};
  IndexOrigin origin = IndexOrigin::kCreateIndex;
  bool unique = false;
  bool unique_not_null = false;
  std::optional<ConflictAction> conflict_action{};
  std::size_t key_term_count = 0;
  std::vector<CatalogIndexTerm> terms{};
  std::optional<SchemaExpression> partial_predicate{};
  IndexStatistics statistics{};
};

// Canonical records derived by the catalog loader. Create() validates model
// invariants but does not replay identifier dequoting or constraint positions.
struct CatalogInput {
  std::string schema_name{};
  CatalogVersion version{};
  std::vector<SyntaxTree> definitions{};
  std::vector<CatalogTableInput> tables{};
  std::vector<CatalogIndexInput> indexes{};
};

class CatalogSnapshot;

using CatalogSnapshotPtr = std::shared_ptr<const CatalogSnapshot>;
using CatalogSnapshotResult = std::expected<CatalogSnapshotPtr, CatalogValidationError>;

[[nodiscard]] bool CatalogNamesEqual(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] TypeAffinity DetermineTypeAffinity(
    std::optional<std::string_view> declared_type) noexcept;

class CatalogSnapshot final {
 public:
  [[nodiscard]] static CatalogSnapshotResult Create(CatalogInput input);

  CatalogSnapshot(const CatalogSnapshot&) = delete;
  CatalogSnapshot& operator=(const CatalogSnapshot&) = delete;
  CatalogSnapshot(CatalogSnapshot&&) = delete;
  CatalogSnapshot& operator=(CatalogSnapshot&&) = delete;
  ~CatalogSnapshot() = default;

  [[nodiscard]] std::string_view schema_name() const noexcept { return schema_name_; }
  [[nodiscard]] CatalogVersion version() const noexcept { return version_; }
  [[nodiscard]] std::span<const SyntaxTree> definitions() const noexcept { return definitions_; }
  [[nodiscard]] std::span<const CatalogTable> tables() const noexcept { return tables_; }
  [[nodiscard]] std::span<const CatalogIndex> indexes() const noexcept { return indexes_; }

  [[nodiscard]] const SyntaxTree& definition(SchemaDefinitionId id) const noexcept;
  [[nodiscard]] const Expression& expression(SchemaExpression expression) const noexcept;
  [[nodiscard]] const CatalogTable& table(TableId id) const noexcept;
  [[nodiscard]] const CatalogColumn& column(TableId table, ColumnId column) const noexcept;
  [[nodiscard]] const CatalogIndex& index(IndexId id) const noexcept;
  [[nodiscard]] std::span<const IndexId> table_indexes(TableId table) const noexcept;
  [[nodiscard]] std::optional<IndexId> primary_key_index(TableId table) const noexcept;

  [[nodiscard]] std::optional<TableId> FindTable(std::string_view name) const noexcept;
  [[nodiscard]] std::optional<IndexId> FindIndex(std::string_view name) const noexcept;
  [[nodiscard]] std::optional<ColumnId> FindColumn(TableId table,
                                                   std::string_view name) const noexcept;

 private:
  CatalogSnapshot(std::string schema_name, CatalogVersion version,
                  std::vector<SyntaxTree> definitions, std::vector<CatalogTable> tables,
                  std::vector<CatalogIndex> indexes, std::vector<TableId> table_lookup,
                  std::vector<IndexId> index_lookup,
                  std::vector<std::vector<ColumnId>> column_lookups,
                  std::vector<std::vector<IndexId>> table_indexes,
                  std::vector<std::optional<IndexId>> primary_key_indexes) noexcept;

  std::string schema_name_;
  CatalogVersion version_;
  std::vector<SyntaxTree> definitions_;
  std::vector<CatalogTable> tables_;
  std::vector<CatalogIndex> indexes_;
  std::vector<TableId> table_lookup_;
  std::vector<IndexId> index_lookup_;
  std::vector<std::vector<ColumnId>> column_lookups_;
  std::vector<std::vector<IndexId>> table_indexes_;
  std::vector<std::optional<IndexId>> primary_key_indexes_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_CATALOG_CATALOG_HPP_
