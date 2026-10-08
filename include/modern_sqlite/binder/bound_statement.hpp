#ifndef MODERN_SQLITE_BINDER_BOUND_STATEMENT_HPP_
#define MODERN_SQLITE_BINDER_BOUND_STATEMENT_HPP_

#include <compare>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "modern_sqlite/binder/bound_select.hpp"

namespace modern_sqlite {

namespace binder_detail {
class StatementBinder;
}  // namespace binder_detail

struct BoundMutationColumn {
  ColumnId column{};
  std::string_view name{};
  std::optional<std::string_view> declared_type{};
  TypeAffinity affinity = TypeAffinity::kNone;
  bool not_null = false;
  std::shared_ptr<const SqlValue> default_value{};
  bool rowid_alias = false;
};

struct BoundIndexTerm {
  std::optional<ColumnId> column{};
  bool rowid = false;
  std::string_view collation_name = "BINARY";
  SortOrder order = SortOrder::kAscending;
};

struct BoundIndexMaintenance {
  IndexId index{};
  RootPageId root_page{};
  bool unique = false;
  bool unique_not_null = false;
  std::uint32_t key_term_count = 0;
  std::vector<BoundIndexTerm> terms{};
};

struct BoundMutationTarget {
  TableId table{};
  RootPageId root_page{};
  SourceSpan span{};
  std::vector<BoundMutationColumn> columns{};
  std::optional<ColumnId> rowid_alias{};
  std::vector<BoundIndexMaintenance> indexes{};
};

struct BoundMutationField {
  std::optional<ColumnId> column{};
  bool rowid = false;

  constexpr auto operator<=>(const BoundMutationField&) const noexcept = default;
};

struct BoundInsertValue {
  BoundMutationField target{};
  BoundExpressionId expression;
  bool effective = true;
};

struct BoundUpdateAssignment {
  BoundMutationField target{};
  BoundExpressionId expression;
  bool effective = true;
};

class BoundInsert final {
 public:
  BoundInsert(const BoundInsert&) = delete;
  BoundInsert& operator=(const BoundInsert&) = delete;
  BoundInsert(BoundInsert&&) noexcept;
  BoundInsert& operator=(BoundInsert&&) noexcept;
  ~BoundInsert();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Utf8View source() const noexcept;
  [[nodiscard]] const CatalogSnapshot* catalog() const noexcept;
  [[nodiscard]] CatalogVersion required_catalog_version() const noexcept;
  [[nodiscard]] std::uint64_t registration_generation() const noexcept;
  [[nodiscard]] const BoundMutationTarget& target() const noexcept;
  [[nodiscard]] std::span<const BoundParameter> parameters() const noexcept;
  [[nodiscard]] std::span<const BoundCollation> collations() const noexcept;
  [[nodiscard]] std::span<const BoundScalarFunction> functions() const noexcept;
  [[nodiscard]] std::span<const BoundExpression> expressions() const noexcept;
  [[nodiscard]] const BoundExpression& expression(BoundExpressionId id) const noexcept;
  [[nodiscard]] std::span<const BoundInsertValue> values() const noexcept;
  [[nodiscard]] bool default_values() const noexcept;
  [[nodiscard]] bool explicit_columns() const noexcept;

 private:
  friend class binder_detail::StatementBinder;

  struct Impl;

  explicit BoundInsert(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

class BoundUpdate final {
 public:
  BoundUpdate(const BoundUpdate&) = delete;
  BoundUpdate& operator=(const BoundUpdate&) = delete;
  BoundUpdate(BoundUpdate&&) noexcept;
  BoundUpdate& operator=(BoundUpdate&&) noexcept;
  ~BoundUpdate();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Utf8View source() const noexcept;
  [[nodiscard]] const CatalogSnapshot* catalog() const noexcept;
  [[nodiscard]] CatalogVersion required_catalog_version() const noexcept;
  [[nodiscard]] std::uint64_t registration_generation() const noexcept;
  [[nodiscard]] const BoundMutationTarget& target() const noexcept;
  [[nodiscard]] std::span<const BoundSourceColumn> source_columns() const noexcept;
  [[nodiscard]] std::span<const BoundParameter> parameters() const noexcept;
  [[nodiscard]] std::span<const BoundCollation> collations() const noexcept;
  [[nodiscard]] std::span<const BoundScalarFunction> functions() const noexcept;
  [[nodiscard]] std::span<const BoundExpression> expressions() const noexcept;
  [[nodiscard]] const BoundExpression& expression(BoundExpressionId id) const noexcept;
  [[nodiscard]] std::span<const BoundUpdateAssignment> assignments() const noexcept;
  [[nodiscard]] std::optional<BoundExpressionId> where_expression() const noexcept;
  [[nodiscard]] bool changes_rowid() const noexcept;

 private:
  friend class binder_detail::StatementBinder;

  struct Impl;

  explicit BoundUpdate(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

class BoundDelete final {
 public:
  BoundDelete(const BoundDelete&) = delete;
  BoundDelete& operator=(const BoundDelete&) = delete;
  BoundDelete(BoundDelete&&) noexcept;
  BoundDelete& operator=(BoundDelete&&) noexcept;
  ~BoundDelete();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Utf8View source() const noexcept;
  [[nodiscard]] const CatalogSnapshot* catalog() const noexcept;
  [[nodiscard]] CatalogVersion required_catalog_version() const noexcept;
  [[nodiscard]] std::uint64_t registration_generation() const noexcept;
  [[nodiscard]] const BoundMutationTarget& target() const noexcept;
  [[nodiscard]] std::span<const BoundSourceColumn> source_columns() const noexcept;
  [[nodiscard]] std::span<const BoundParameter> parameters() const noexcept;
  [[nodiscard]] std::span<const BoundCollation> collations() const noexcept;
  [[nodiscard]] std::span<const BoundScalarFunction> functions() const noexcept;
  [[nodiscard]] std::span<const BoundExpression> expressions() const noexcept;
  [[nodiscard]] const BoundExpression& expression(BoundExpressionId id) const noexcept;
  [[nodiscard]] std::optional<BoundExpressionId> where_expression() const noexcept;

 private:
  friend class binder_detail::StatementBinder;

  struct Impl;

  explicit BoundDelete(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

struct BoundCreateColumn {
  ColumnId column{};
  std::string name{};
  std::optional<std::string> declared_type{};
  TypeAffinity affinity = TypeAffinity::kNone;
  bool not_null = false;
  std::shared_ptr<const SqlValue> default_value{};
  bool rowid_alias = false;
};

class BoundCreateTable final {
 public:
  BoundCreateTable(const BoundCreateTable&) = delete;
  BoundCreateTable& operator=(const BoundCreateTable&) = delete;
  BoundCreateTable(BoundCreateTable&&) noexcept;
  BoundCreateTable& operator=(BoundCreateTable&&) noexcept;
  ~BoundCreateTable();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Utf8View source() const noexcept;
  [[nodiscard]] const CatalogSnapshot* catalog() const noexcept;
  [[nodiscard]] CatalogVersion required_catalog_version() const noexcept;
  [[nodiscard]] std::string_view table_name() const noexcept;
  [[nodiscard]] bool if_not_exists() const noexcept;
  [[nodiscard]] bool no_op() const noexcept;
  [[nodiscard]] std::string_view canonical_sql() const noexcept;
  [[nodiscard]] std::span<const BoundCreateColumn> columns() const noexcept;
  [[nodiscard]] std::optional<ColumnId> rowid_alias() const noexcept;

 private:
  friend class binder_detail::StatementBinder;

  struct Impl;

  explicit BoundCreateTable(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

struct BoundCreateIndexTerm {
  ColumnId column{};
  TypeAffinity affinity = TypeAffinity::kNone;
  std::string collation_name{};
  SortOrder order = SortOrder::kAscending;
  bool rowid = false;
};

class BoundCreateIndex final {
 public:
  BoundCreateIndex(const BoundCreateIndex&) = delete;
  BoundCreateIndex& operator=(const BoundCreateIndex&) = delete;
  BoundCreateIndex(BoundCreateIndex&&) noexcept;
  BoundCreateIndex& operator=(BoundCreateIndex&&) noexcept;
  ~BoundCreateIndex();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Utf8View source() const noexcept;
  [[nodiscard]] const CatalogSnapshot* catalog() const noexcept;
  [[nodiscard]] CatalogVersion required_catalog_version() const noexcept;
  [[nodiscard]] TableId table() const noexcept;
  [[nodiscard]] RootPageId table_root_page() const noexcept;
  [[nodiscard]] std::string_view index_name() const noexcept;
  [[nodiscard]] std::string_view table_name() const noexcept;
  [[nodiscard]] bool unique() const noexcept;
  [[nodiscard]] bool unique_not_null() const noexcept;
  [[nodiscard]] bool if_not_exists() const noexcept;
  [[nodiscard]] bool no_op() const noexcept;
  [[nodiscard]] std::string_view canonical_sql() const noexcept;
  [[nodiscard]] std::span<const BoundCreateIndexTerm> terms() const noexcept;

 private:
  friend class binder_detail::StatementBinder;

  struct Impl;

  explicit BoundCreateIndex(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

enum class BoundAnalyzeScope : std::uint8_t {
  kDatabase,
  kTable,
  kIndex,
};

struct BoundAnalyzeIndexColumn {
  std::string collation_name{};
  SortOrder order = SortOrder::kAscending;
};

struct BoundAnalyzeIndex {
  RootPageId root_page{};
  std::string table_name{};
  std::string index_name{};
  std::uint32_t key_term_count = 0;
  std::vector<BoundAnalyzeIndexColumn> columns{};
  bool partial = false;
};

struct BoundAnalyzeTable {
  RootPageId root_page{};
  std::string table_name{};
  std::uint32_t record_field_count = 0;
};

class BoundAnalyze final {
 public:
  BoundAnalyze(const BoundAnalyze&) = delete;
  BoundAnalyze& operator=(const BoundAnalyze&) = delete;
  BoundAnalyze(BoundAnalyze&&) noexcept;
  BoundAnalyze& operator=(BoundAnalyze&&) noexcept;
  ~BoundAnalyze();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Utf8View source() const noexcept;
  [[nodiscard]] const CatalogSnapshot* catalog() const noexcept;
  [[nodiscard]] CatalogVersion required_catalog_version() const noexcept;
  [[nodiscard]] BoundAnalyzeScope scope() const noexcept;
  [[nodiscard]] std::string_view scope_name() const noexcept;
  [[nodiscard]] std::optional<RootPageId> stat1_root_page() const noexcept;
  [[nodiscard]] std::span<const BoundAnalyzeIndex> indexes() const noexcept;
  [[nodiscard]] std::span<const BoundAnalyzeTable> tables() const noexcept;

 private:
  friend class binder_detail::StatementBinder;

  struct Impl;

  explicit BoundAnalyze(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

struct BoundBeginTransaction {
  BeginTransactionMode mode = BeginTransactionMode::kDeferred;
};

struct BoundCommitTransaction {
  CommitTransactionSyntax syntax = CommitTransactionSyntax::kCommit;
};

struct BoundRollbackTransaction {};

struct BoundSavepoint {
  std::string name{};
};

struct BoundReleaseSavepoint {
  std::string name{};
};

struct BoundRollbackToSavepoint {
  std::string name{};
};

using BoundStatement =
    std::variant<BoundSelect, BoundInsert, BoundUpdate, BoundDelete, BoundCreateTable,
                 BoundCreateIndex, BoundAnalyze, BoundBeginTransaction, BoundCommitTransaction,
                 BoundRollbackTransaction, BoundSavepoint, BoundReleaseSavepoint,
                 BoundRollbackToSavepoint>;

using BindStatementResult = std::expected<BoundStatement, BindError>;

[[nodiscard]] BindStatementResult BindStatement(
    SyntaxTree tree, CatalogSnapshotPtr catalog,
    BindEnvironment environment = BindEnvironment::Core(), BindOptions options = {});

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_BINDER_BOUND_STATEMENT_HPP_
