#include "modern_sqlite/catalog/catalog.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace modern_sqlite {
namespace {

using ValidationStatus = std::expected<void, CatalogValidationError>;

struct CatalogBuildData {
  std::string schema_name;
  CatalogVersion version;
  std::vector<SyntaxTree> definitions;
  std::vector<CatalogTable> tables;
  std::vector<CatalogIndex> indexes;
  std::vector<TableId> table_lookup;
  std::vector<IndexId> index_lookup;
  std::vector<std::vector<ColumnId>> column_lookups;
  std::vector<std::vector<IndexId>> table_indexes;
  std::vector<std::optional<IndexId>> primary_key_indexes;
};

constexpr std::size_t kEmptyLookupValue = std::numeric_limits<std::size_t>::max();
constexpr std::uint32_t kMinimumCatalogRootPage = 2U;
constexpr std::uint32_t kMaximumCatalogRootPage = 0xfffffffeU;

[[nodiscard]] bool IsValidRootPage(RootPageId root_page) noexcept {
  return root_page.value >= kMinimumCatalogRootPage && root_page.value <= kMaximumCatalogRootPage;
}

[[nodiscard]] std::size_t CatalogNameHash(std::string_view name) noexcept {
  std::uint32_t hash = 0;
  for (const char character : name) {
    const auto byte = static_cast<std::uint8_t>(static_cast<unsigned char>(character));
    hash += SqliteToLower(byte);
    hash *= 0x9e3779b1U;
  }
  return hash;
}

template <typename Id>
[[nodiscard]] std::vector<Id> CreateLookup(std::size_t entry_count) {
  if (entry_count == 0) {
    return {};
  }
  assert(entry_count <= std::numeric_limits<std::size_t>::max() / 2U);
  const std::size_t capacity = std::bit_ceil(entry_count * 2U);
  return std::vector<Id>(capacity, Id{kEmptyLookupValue});
}

template <typename Id, typename NameAt>
[[nodiscard]] std::optional<Id> InsertLookup(std::vector<Id>& lookup, Id id, std::string_view name,
                                             NameAt name_at) noexcept {
  assert(!lookup.empty());
  const std::size_t mask = lookup.size() - 1U;
  std::size_t position = CatalogNameHash(name) & mask;
  while (lookup[position].value != kEmptyLookupValue) {
    if (CatalogNamesEqual(name_at(lookup[position]), name)) {
      return lookup[position];
    }
    position = (position + 1U) & mask;
  }
  lookup[position] = id;
  return std::nullopt;
}

template <typename Id, typename NameAt>
[[nodiscard]] std::optional<Id> FindLookup(std::span<const Id> lookup, std::string_view name,
                                           NameAt name_at) noexcept {
  if (lookup.empty()) {
    return std::nullopt;
  }
  const std::size_t mask = lookup.size() - 1U;
  std::size_t position = CatalogNameHash(name) & mask;
  while (lookup[position].value != kEmptyLookupValue) {
    if (CatalogNamesEqual(name_at(lookup[position]), name)) {
      return lookup[position];
    }
    position = (position + 1U) & mask;
  }
  return std::nullopt;
}

[[nodiscard]] int CompareCatalogNames(std::string_view left, std::string_view right) noexcept {
  const std::size_t shared_size = std::min(left.size(), right.size());
  for (std::size_t index = 0; index < shared_size; ++index) {
    const auto left_byte = static_cast<std::uint8_t>(static_cast<unsigned char>(left[index]));
    const auto right_byte = static_cast<std::uint8_t>(static_cast<unsigned char>(right[index]));
    const std::uint8_t folded_left = SqliteToLower(left_byte);
    const std::uint8_t folded_right = SqliteToLower(right_byte);
    if (folded_left < folded_right) {
      return -1;
    }
    if (folded_left > folded_right) {
      return 1;
    }
  }
  if (left.size() < right.size()) {
    return -1;
  }
  if (left.size() > right.size()) {
    return 1;
  }
  return 0;
}

[[nodiscard]] bool ContainsFolded(std::string_view text, std::string_view needle) noexcept {
  if (needle.size() > text.size()) {
    return false;
  }
  for (std::size_t begin = 0; begin + needle.size() <= text.size(); ++begin) {
    bool matches = true;
    for (std::size_t offset = 0; offset < needle.size(); ++offset) {
      const auto text_byte =
          static_cast<std::uint8_t>(static_cast<unsigned char>(text[begin + offset]));
      const auto needle_byte =
          static_cast<std::uint8_t>(static_cast<unsigned char>(needle[offset]));
      if (SqliteToLower(text_byte) != SqliteToLower(needle_byte)) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool ContainsNull(std::string_view value) noexcept {
  return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool IsValid(ConflictAction action) noexcept {
  switch (action) {
    case ConflictAction::kDefault:
    case ConflictAction::kRollback:
    case ConflictAction::kAbort:
    case ConflictAction::kFail:
    case ConflictAction::kIgnore:
    case ConflictAction::kReplace:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsEffectiveSortOrder(SortOrder order) noexcept {
  return order == SortOrder::kAscending || order == SortOrder::kDescending;
}

[[nodiscard]] bool IsValid(IndexOrigin origin) noexcept {
  switch (origin) {
    case IndexOrigin::kCreateIndex:
    case IndexOrigin::kUniqueConstraint:
    case IndexOrigin::kPrimaryKey:
      return true;
  }
  return false;
}

[[nodiscard]] CatalogValidationError ValidationError(
    CatalogValidationCode code, CatalogObjectKind owner_kind, std::size_t owner_index,
    std::string detail, CatalogMemberKind member_kind = CatalogMemberKind::kNone,
    std::size_t member_index = 0) {
  return CatalogValidationError{
      .code = code,
      .location =
          CatalogValidationLocation{
              .owner_kind = owner_kind,
              .owner_index = owner_index,
              .member_kind = member_kind,
              .member_index = member_index,
          },
      .detail = std::move(detail),
  };
}

[[nodiscard]] ValidationStatus Success() { return {}; }

[[nodiscard]] ValidationStatus Failure(CatalogValidationError error) {
  return std::unexpected(std::move(error));
}

[[nodiscard]] std::optional<TypeAffinity> StrictAffinity(
    std::optional<std::string_view> declared_type) noexcept {
  if (!declared_type.has_value()) {
    return std::nullopt;
  }
  if (CatalogNamesEqual(*declared_type, "ANY") || CatalogNamesEqual(*declared_type, "BLOB")) {
    return TypeAffinity::kBlob;
  }
  if (CatalogNamesEqual(*declared_type, "INT") || CatalogNamesEqual(*declared_type, "INTEGER")) {
    return TypeAffinity::kInteger;
  }
  if (CatalogNamesEqual(*declared_type, "REAL")) {
    return TypeAffinity::kReal;
  }
  if (CatalogNamesEqual(*declared_type, "TEXT")) {
    return TypeAffinity::kText;
  }
  return std::nullopt;
}

[[nodiscard]] bool SameColumnIdentity(const CatalogIndexTerm& left,
                                      const CatalogIndexTerm& right) noexcept {
  const auto* left_column = std::get_if<ColumnId>(&left.target);
  const auto* right_column = std::get_if<ColumnId>(&right.target);
  return left_column != nullptr && right_column != nullptr && *left_column == *right_column &&
         CatalogNamesEqual(left.collation_name, right.collation_name);
}

[[nodiscard]] bool ContainsColumn(std::span<const CatalogIndexTerm> terms,
                                  ColumnId column) noexcept {
  return std::ranges::any_of(terms, [column](const CatalogIndexTerm& term) {
    const auto* target = std::get_if<ColumnId>(&term.target);
    return target != nullptr && *target == column;
  });
}

[[nodiscard]] bool ContainsColumnIdentity(std::span<const CatalogIndexTerm> terms,
                                          const CatalogIndexTerm& candidate) noexcept {
  return std::ranges::any_of(terms, [&candidate](const CatalogIndexTerm& term) {
    return SameColumnIdentity(term, candidate);
  });
}

[[nodiscard]] bool TermsMatch(const CatalogIndexTerm& actual,
                              const CatalogIndexTerm& expected) noexcept {
  return actual.target == expected.target &&
         CatalogNamesEqual(actual.collation_name, expected.collation_name) &&
         actual.order == expected.order;
}

class CatalogBuilder final {
 public:
  explicit CatalogBuilder(CatalogInput input)
      : input_(std::move(input)),
        definition_owner_counts_(input_.definitions.size(), 0),
        table_indexes_(input_.tables.size()),
        primary_key_indexes_(input_.tables.size()) {}

  [[nodiscard]] std::expected<CatalogBuildData, CatalogValidationError> Build() {
    if (ContainsNull(input_.schema_name)) {
      return std::unexpected(ValidationError(CatalogValidationCode::kInvalidString,
                                             CatalogObjectKind::kSchema, 0,
                                             "schema name contains an embedded NUL"));
    }

    if (auto status = BuildTables(); !status.has_value()) {
      return std::unexpected(std::move(status.error()));
    }
    if (auto status = BuildIndexes(); !status.has_value()) {
      return std::unexpected(std::move(status.error()));
    }
    if (auto status = ValidateObjectNames(); !status.has_value()) {
      return std::unexpected(std::move(status.error()));
    }
    if (auto status = ValidateRoots(); !status.has_value()) {
      return std::unexpected(std::move(status.error()));
    }
    if (auto status = ValidateIndexRelationships(); !status.has_value()) {
      return std::unexpected(std::move(status.error()));
    }
    if (auto status = ValidateDefinitionOwnership(); !status.has_value()) {
      return std::unexpected(std::move(status.error()));
    }

    return CatalogBuildData{
        .schema_name = std::move(input_.schema_name),
        .version = input_.version,
        .definitions = std::move(input_.definitions),
        .tables = std::move(tables_),
        .indexes = std::move(indexes_),
        .table_lookup = std::move(table_lookup_),
        .index_lookup = std::move(index_lookup_),
        .column_lookups = std::move(column_lookups_),
        .table_indexes = std::move(table_indexes_),
        .primary_key_indexes = std::move(primary_key_indexes_),
    };
  }

 private:
  [[nodiscard]] ValidationStatus ValidateDefinition(SchemaDefinitionId definition,
                                                    CatalogObjectKind owner_kind,
                                                    std::size_t owner_index, bool expect_table,
                                                    bool primary_owner) {
    if (definition.value >= input_.definitions.size()) {
      return Failure(ValidationError(CatalogValidationCode::kInvalidReference, owner_kind,
                                     owner_index, "schema definition reference is out of range"));
    }
    const Statement& statement = input_.definitions[definition.value].statement();
    const bool matches = expect_table ? std::holds_alternative<CreateTableStatement>(statement)
                                      : std::holds_alternative<CreateIndexStatement>(statement);
    if (!matches) {
      return Failure(ValidationError(
          CatalogValidationCode::kDefinitionMismatch, owner_kind, owner_index,
          expect_table ? "catalog table requires a CREATE TABLE definition"
                       : "explicit catalog index requires a CREATE INDEX definition"));
    }
    if (primary_owner) {
      if (definition_owner_counts_[definition.value] != 0U) {
        return Failure(
            ValidationError(CatalogValidationCode::kDefinitionMismatch, owner_kind, owner_index,
                            "schema definition has more than one primary catalog owner"));
      }
      definition_owner_counts_[definition.value] = 1U;
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidateExpression(SchemaExpression expression,
                                                    SchemaDefinitionId expected_definition,
                                                    CatalogObjectKind owner_kind,
                                                    std::size_t owner_index,
                                                    CatalogMemberKind member_kind,
                                                    std::size_t member_index) const {
    if (expression.definition.value >= input_.definitions.size()) {
      return Failure(ValidationError(CatalogValidationCode::kInvalidReference, owner_kind,
                                     owner_index, "schema expression definition is out of range",
                                     member_kind, member_index));
    }
    if (expression.definition != expected_definition) {
      return Failure(ValidationError(
          CatalogValidationCode::kDefinitionMismatch, owner_kind, owner_index,
          "schema expression belongs to a different DDL definition", member_kind, member_index));
    }
    if (expression.expression.value >=
        input_.definitions[expression.definition.value].expressions().size()) {
      return Failure(ValidationError(CatalogValidationCode::kInvalidReference, owner_kind,
                                     owner_index, "schema expression ID is out of range",
                                     member_kind, member_index));
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidateTableStatistics(const TableStatistics& statistics,
                                                         std::size_t table_index) const {
    if (!statistics.has_stat1 &&
        (statistics.estimated_rows.has_value() || statistics.average_row_size.has_value())) {
      return Failure(ValidationError(CatalogValidationCode::kInvalidStatistics,
                                     CatalogObjectKind::kTable, table_index,
                                     "table statistics exist without a STAT1 row"));
    }
    if (statistics.average_row_size.has_value() && *statistics.average_row_size < 2U) {
      return Failure(ValidationError(
          CatalogValidationCode::kInvalidStatistics, CatalogObjectKind::kTable, table_index,
          "table average row size must be at least two", CatalogMemberKind::kStatisticValue, 0));
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidateIndexStatistics(const IndexStatistics& statistics,
                                                         std::size_t key_term_count,
                                                         std::size_t index) const {
    if (!statistics.has_stat1 &&
        (!statistics.rows_per_prefix.empty() || statistics.average_row_size.has_value() ||
         statistics.unordered || statistics.no_skip_scan)) {
      return Failure(ValidationError(CatalogValidationCode::kInvalidStatistics,
                                     CatalogObjectKind::kIndex, index,
                                     "index statistics exist without a STAT1 row"));
    }
    if (statistics.rows_per_prefix.size() > key_term_count + 1U) {
      return Failure(ValidationError(CatalogValidationCode::kInvalidStatistics,
                                     CatalogObjectKind::kIndex, index,
                                     "index STAT1 supplies too many prefix estimates",
                                     CatalogMemberKind::kStatisticValue, key_term_count + 1U));
    }
    if (statistics.average_row_size.has_value() && *statistics.average_row_size < 2U) {
      return Failure(ValidationError(
          CatalogValidationCode::kInvalidStatistics, CatalogObjectKind::kIndex, index,
          "index average row size must be at least two", CatalogMemberKind::kStatisticValue, 0));
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus BuildTables() {
    tables_.reserve(input_.tables.size());
    column_lookups_.reserve(input_.tables.size());

    for (std::size_t table_index = 0; table_index < input_.tables.size(); ++table_index) {
      CatalogTableInput& input = input_.tables[table_index];
      if (ContainsNull(input.name)) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidString,
                                       CatalogObjectKind::kTable, table_index,
                                       "table name contains an embedded NUL"));
      }
      if (!IsValidRootPage(input.root_page)) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidRootPage,
                                       CatalogObjectKind::kTable, table_index,
                                       "table root page is outside SQLite's usable range"));
      }
      if (input.columns.empty()) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                       CatalogObjectKind::kTable, table_index,
                                       "table must contain at least one column"));
      }
      if (!IsValid(input.rowid_primary_key_conflict)) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                       CatalogObjectKind::kTable, table_index,
                                       "rowid primary-key conflict action is invalid"));
      }
      if (input.without_rowid && input.rowid_alias.has_value()) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                       CatalogObjectKind::kTable, table_index,
                                       "WITHOUT ROWID table cannot have a rowid alias"));
      }
      if (input.autoincrement && !input.rowid_alias.has_value()) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                       CatalogObjectKind::kTable, table_index,
                                       "AUTOINCREMENT requires an INTEGER PRIMARY KEY alias"));
      }
      if (input.rowid_alias.has_value() && input.rowid_alias->value >= input.columns.size()) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidReference,
                                       CatalogObjectKind::kTable, table_index,
                                       "rowid alias column is out of range",
                                       CatalogMemberKind::kColumn, input.rowid_alias->value));
      }
      if (auto status = ValidateDefinition(input.definition, CatalogObjectKind::kTable, table_index,
                                           true, true);
          !status.has_value()) {
        return status;
      }
      if (auto status = ValidateTableStatistics(input.statistics, table_index);
          !status.has_value()) {
        return status;
      }

      std::vector<CatalogColumn> columns;
      columns.reserve(input.columns.size());
      for (std::size_t column_index = 0; column_index < input.columns.size(); ++column_index) {
        CatalogColumnInput& column = input.columns[column_index];
        if (ContainsNull(column.name) ||
            (column.declared_type.has_value() && ContainsNull(*column.declared_type)) ||
            ContainsNull(column.collation_name)) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidString,
                                         CatalogObjectKind::kTable, table_index,
                                         "column metadata contains an embedded NUL",
                                         CatalogMemberKind::kColumn, column_index));
        }
        if (column.not_null_conflict.has_value() && !IsValid(*column.not_null_conflict)) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                         CatalogObjectKind::kTable, table_index,
                                         "column NOT NULL conflict action is invalid",
                                         CatalogMemberKind::kColumn, column_index));
        }
        if (column.default_expression.has_value()) {
          if (auto status = ValidateExpression(*column.default_expression, input.definition,
                                               CatalogObjectKind::kTable, table_index,
                                               CatalogMemberKind::kDefaultExpression, column_index);
              !status.has_value()) {
            return status;
          }
        }
        if (column.missing_record_value != nullptr && !column.default_expression.has_value()) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                         CatalogObjectKind::kTable, table_index,
                                         "missing-record value requires a default expression",
                                         CatalogMemberKind::kColumn, column_index));
        }

        TypeAffinity affinity = DetermineTypeAffinity(
            column.declared_type ? std::optional<std::string_view>{*column.declared_type}
                                 : std::nullopt);
        if (input.strict) {
          const auto strict_affinity = StrictAffinity(
              column.declared_type ? std::optional<std::string_view>{*column.declared_type}
                                   : std::nullopt);
          if (!strict_affinity.has_value()) {
            return Failure(ValidationError(
                CatalogValidationCode::kInvalidTableShape, CatalogObjectKind::kTable, table_index,
                "STRICT column has a missing or unsupported declared type",
                CatalogMemberKind::kColumn, column_index));
          }
          affinity = *strict_affinity;
        }

        std::optional<ConflictAction> effective_not_null = column.not_null_conflict;
        const bool is_rowid_alias =
            input.rowid_alias.has_value() && input.rowid_alias->value == column_index;
        if (!effective_not_null.has_value() && column.primary_key &&
            (input.without_rowid || (input.strict && !is_rowid_alias))) {
          effective_not_null = ConflictAction::kAbort;
        }

        std::shared_ptr<const SqlValue> missing_record_value;
        if (column.missing_record_value != nullptr) {
          missing_record_value = std::make_shared<const SqlValue>(
              ApplyAffinity(column.missing_record_value->Clone(), affinity));
        }

        columns.push_back(CatalogColumn{
            .name = std::move(column.name),
            .declared_type = std::move(column.declared_type),
            .affinity = affinity,
            .collation_name = std::move(column.collation_name),
            .declared_not_null_conflict = column.not_null_conflict,
            .effective_not_null_conflict = effective_not_null,
            .primary_key = column.primary_key,
            .default_expression = column.default_expression,
            .missing_record_value = std::move(missing_record_value),
        });
      }

      if (input.rowid_alias.has_value()) {
        const CatalogColumn& alias = columns[input.rowid_alias->value];
        if (!alias.primary_key || !alias.declared_type.has_value() ||
            !CatalogNamesEqual(*alias.declared_type, "INTEGER")) {
          return Failure(ValidationError(
              CatalogValidationCode::kInvalidTableShape, CatalogObjectKind::kTable, table_index,
              "rowid alias must be a primary-key column declared exactly INTEGER",
              CatalogMemberKind::kColumn, input.rowid_alias->value));
        }
      }

      for (std::size_t check_index = 0; check_index < input.check_constraints.size();
           ++check_index) {
        if (auto status = ValidateExpression(input.check_constraints[check_index], input.definition,
                                             CatalogObjectKind::kTable, table_index,
                                             CatalogMemberKind::kCheckExpression, check_index);
            !status.has_value()) {
          return status;
        }
      }

      std::vector<ColumnId> column_lookup = CreateLookup<ColumnId>(columns.size());
      for (std::size_t index = 0; index < columns.size(); ++index) {
        if (InsertLookup(
                column_lookup, ColumnId{index}, columns[index].name,
                [&columns](ColumnId id) -> std::string_view { return columns[id.value].name; })
                .has_value()) {
          return Failure(ValidationError(
              CatalogValidationCode::kDuplicateName, CatalogObjectKind::kTable, table_index,
              "table contains duplicate column names", CatalogMemberKind::kColumn, index));
        }
      }

      tables_.push_back(CatalogTable{
          .definition = input.definition,
          .name = std::move(input.name),
          .root_page = input.root_page,
          .columns = std::move(columns),
          .check_constraints = std::move(input.check_constraints),
          .rowid_alias = input.rowid_alias,
          .rowid_primary_key_conflict = input.rowid_primary_key_conflict,
          .without_rowid = input.without_rowid,
          .strict = input.strict,
          .autoincrement = input.autoincrement,
          .statistics = input.statistics,
      });
      column_lookups_.push_back(std::move(column_lookup));
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus BuildIndexes() {
    indexes_.reserve(input_.indexes.size());
    for (std::size_t index = 0; index < input_.indexes.size(); ++index) {
      CatalogIndexInput& input = input_.indexes[index];
      if (ContainsNull(input.name)) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidString,
                                       CatalogObjectKind::kIndex, index,
                                       "index name contains an embedded NUL"));
      }
      if (input.table.value >= tables_.size()) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidReference,
                                       CatalogObjectKind::kIndex, index,
                                       "index table reference is out of range"));
      }
      if (!IsValidRootPage(input.root_page)) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidRootPage,
                                       CatalogObjectKind::kIndex, index,
                                       "index root page is outside SQLite's usable range"));
      }
      if (!IsValid(input.origin)) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                       CatalogObjectKind::kIndex, index,
                                       "index origin is invalid"));
      }
      if (input.key_term_count == 0U || input.key_term_count > input.terms.size()) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                       CatalogObjectKind::kIndex, index,
                                       "index key term count is invalid"));
      }
      if (input.unique != input.conflict_action.has_value() ||
          (input.conflict_action.has_value() && !IsValid(*input.conflict_action))) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                       CatalogObjectKind::kIndex, index,
                                       "index uniqueness and conflict metadata disagree"));
      }
      if (input.origin != IndexOrigin::kCreateIndex && !input.unique) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                       CatalogObjectKind::kIndex, index,
                                       "automatic indexes must be unique"));
      }

      const CatalogTable& table = tables_[input.table.value];
      if (input.origin == IndexOrigin::kCreateIndex) {
        if (auto status =
                ValidateDefinition(input.definition, CatalogObjectKind::kIndex, index, false, true);
            !status.has_value()) {
          return status;
        }
      } else {
        if (input.definition != table.definition) {
          return Failure(ValidationError(CatalogValidationCode::kDefinitionMismatch,
                                         CatalogObjectKind::kIndex, index,
                                         "automatic index must use its table definition"));
        }
        if (auto status =
                ValidateDefinition(input.definition, CatalogObjectKind::kIndex, index, true, false);
            !status.has_value()) {
          return status;
        }
      }

      for (std::size_t term_index = 0; term_index < input.terms.size(); ++term_index) {
        CatalogIndexTerm& term = input.terms[term_index];
        if (ContainsNull(term.collation_name)) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidString,
                                         CatalogObjectKind::kIndex, index,
                                         "index collation contains an embedded NUL",
                                         CatalogMemberKind::kIndexTerm, term_index));
        }
        if (!IsEffectiveSortOrder(term.order)) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                         CatalogObjectKind::kIndex, index,
                                         "physical index term must have an effective sort order",
                                         CatalogMemberKind::kIndexTerm, term_index));
        }
        if (const auto* column = std::get_if<ColumnId>(&term.target); column != nullptr) {
          if (column->value >= table.columns.size()) {
            return Failure(ValidationError(CatalogValidationCode::kInvalidReference,
                                           CatalogObjectKind::kIndex, index,
                                           "index column reference is out of range",
                                           CatalogMemberKind::kIndexTerm, term_index));
          }
        } else if (const auto* expression = std::get_if<SchemaExpression>(&term.target);
                   expression != nullptr) {
          if (input.origin != IndexOrigin::kCreateIndex || term_index >= input.key_term_count) {
            return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                           CatalogObjectKind::kIndex, index,
                                           "only explicit index key terms may contain expressions",
                                           CatalogMemberKind::kIndexTerm, term_index));
          }
          if (auto status =
                  ValidateExpression(*expression, input.definition, CatalogObjectKind::kIndex,
                                     index, CatalogMemberKind::kIndexTerm, term_index);
              !status.has_value()) {
            return status;
          }
        } else if (term_index < input.key_term_count) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                         CatalogObjectKind::kIndex, index,
                                         "rowid cannot be a declared index key term",
                                         CatalogMemberKind::kIndexTerm, term_index));
        }
      }

      if (input.partial_predicate.has_value()) {
        if (input.origin != IndexOrigin::kCreateIndex) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                         CatalogObjectKind::kIndex, index,
                                         "automatic index cannot have a partial predicate",
                                         CatalogMemberKind::kPartialPredicate, 0));
        }
        if (auto status = ValidateExpression(*input.partial_predicate, input.definition,
                                             CatalogObjectKind::kIndex, index,
                                             CatalogMemberKind::kPartialPredicate, 0);
            !status.has_value()) {
          return status;
        }
      }
      if (auto status = ValidateIndexStatistics(input.statistics, input.key_term_count, index);
          !status.has_value()) {
        return status;
      }

      indexes_.push_back(CatalogIndex{
          .definition = input.definition,
          .name = std::move(input.name),
          .table = input.table,
          .root_page = input.root_page,
          .origin = input.origin,
          .unique = input.unique,
          .conflict_action = input.conflict_action,
          .key_term_count = input.key_term_count,
          .terms = std::move(input.terms),
          .partial_predicate = input.partial_predicate,
          .statistics = std::move(input.statistics),
      });
      table_indexes_[input.table.value].push_back(IndexId{index});
      if (input.origin == IndexOrigin::kPrimaryKey) {
        if (primary_key_indexes_[input.table.value].has_value()) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                         CatalogObjectKind::kIndex, index,
                                         "table has more than one primary-key index"));
        }
        primary_key_indexes_[input.table.value] = IndexId{index};
      }
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidateObjectNames() {
    table_lookup_ = CreateLookup<TableId>(tables_.size());
    for (std::size_t index = 0; index < tables_.size(); ++index) {
      if (InsertLookup(table_lookup_, TableId{index}, tables_[index].name,
                       [this](TableId id) -> std::string_view { return tables_[id.value].name; })
              .has_value()) {
        return Failure(ValidationError(CatalogValidationCode::kDuplicateName,
                                       CatalogObjectKind::kTable, index,
                                       "schema contains duplicate table names"));
      }
    }

    index_lookup_ = CreateLookup<IndexId>(indexes_.size());
    for (std::size_t index = 0; index < indexes_.size(); ++index) {
      if (InsertLookup(index_lookup_, IndexId{index}, indexes_[index].name,
                       [this](IndexId id) -> std::string_view { return indexes_[id.value].name; })
              .has_value()) {
        return Failure(ValidationError(CatalogValidationCode::kDuplicateName,
                                       CatalogObjectKind::kIndex, index,
                                       "schema contains duplicate index names"));
      }
    }

    for (std::size_t index = 0; index < indexes_.size(); ++index) {
      if (FindLookup(std::span<const TableId>{table_lookup_}, indexes_[index].name,
                     [this](TableId id) -> std::string_view { return tables_[id.value].name; })
              .has_value()) {
        return Failure(ValidationError(CatalogValidationCode::kDuplicateName,
                                       CatalogObjectKind::kIndex, index,
                                       "table and index names share the schema namespace"));
      }
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidateRoots() const {
    for (std::size_t table = 0; table < tables_.size(); ++table) {
      for (std::size_t previous = 0; previous < table; ++previous) {
        if (tables_[table].root_page == tables_[previous].root_page) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidRootPage,
                                         CatalogObjectKind::kTable, table,
                                         "two tables share the same root page"));
        }
      }
    }

    for (std::size_t index = 0; index < indexes_.size(); ++index) {
      const CatalogIndex& catalog_index = indexes_[index];
      const CatalogTable& table = tables_[catalog_index.table.value];
      const bool is_without_rowid_primary =
          table.without_rowid && catalog_index.origin == IndexOrigin::kPrimaryKey;
      for (std::size_t table_index = 0; table_index < tables_.size(); ++table_index) {
        if (catalog_index.root_page != tables_[table_index].root_page) {
          continue;
        }
        if (!is_without_rowid_primary || table_index != catalog_index.table.value) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidRootPage,
                                         CatalogObjectKind::kIndex, index,
                                         "index root page conflicts with a table root"));
        }
      }
      if (is_without_rowid_primary && catalog_index.root_page != table.root_page) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidRootPage,
                                       CatalogObjectKind::kIndex, index,
                                       "WITHOUT ROWID primary index must share the table root"));
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (catalog_index.root_page == indexes_[previous].root_page) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidRootPage,
                                         CatalogObjectKind::kIndex, index,
                                         "two indexes share the same root page"));
        }
      }
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidatePrimaryKey(std::size_t table_index) const {
    const CatalogTable& table = tables_[table_index];
    const std::optional<IndexId> primary = primary_key_indexes_[table_index];
    const bool has_primary_columns = std::ranges::any_of(
        table.columns, [](const CatalogColumn& column) { return column.primary_key; });

    if (table.rowid_alias.has_value()) {
      if (primary.has_value()) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                       CatalogObjectKind::kTable, table_index,
                                       "rowid-alias table must not have a primary-key index"));
      }
      for (std::size_t column = 0; column < table.columns.size(); ++column) {
        if (table.columns[column].primary_key != (column == table.rowid_alias->value)) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                         CatalogObjectKind::kTable, table_index,
                                         "rowid alias must be the only primary-key column",
                                         CatalogMemberKind::kColumn, column));
        }
      }
      return Success();
    }

    if (table.without_rowid && !primary.has_value()) {
      return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                     CatalogObjectKind::kTable, table_index,
                                     "WITHOUT ROWID table requires a primary-key index"));
    }
    if (has_primary_columns != primary.has_value()) {
      return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                     CatalogObjectKind::kTable, table_index,
                                     "primary-key columns and primary-key index disagree"));
    }
    if (!primary.has_value()) {
      return Success();
    }

    const CatalogIndex& primary_index = indexes_[primary->value];
    for (std::size_t term = 0; term < primary_index.key_term_count; ++term) {
      const auto* column = std::get_if<ColumnId>(&primary_index.terms[term].target);
      if (column == nullptr || !table.columns[column->value].primary_key) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                       CatalogObjectKind::kIndex, primary->value,
                                       "primary-key term is not a declared primary-key column",
                                       CatalogMemberKind::kIndexTerm, term));
      }
    }
    for (std::size_t column = 0; column < table.columns.size(); ++column) {
      if (!table.columns[column].primary_key) {
        continue;
      }
      const auto key_terms = std::span<const CatalogIndexTerm>{primary_index.terms.data(),
                                                               primary_index.key_term_count};
      if (!ContainsColumn(key_terms, ColumnId{column})) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidTableShape,
                                       CatalogObjectKind::kTable, table_index,
                                       "primary-key column is absent from the primary index",
                                       CatalogMemberKind::kColumn, column));
      }
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidateAutomaticIndexes() const {
    std::vector<std::size_t> ordinal(tables_.size(), 0);
    for (std::size_t index = 0; index < indexes_.size(); ++index) {
      const CatalogIndex& current = indexes_[index];
      if (current.origin == IndexOrigin::kCreateIndex) {
        continue;
      }
      ++ordinal[current.table.value];
      const std::string expected_name = "sqlite_autoindex_" + tables_[current.table.value].name +
                                        "_" + std::to_string(ordinal[current.table.value]);
      if (current.name != expected_name) {
        return Failure(
            ValidationError(CatalogValidationCode::kInvalidIndexShape, CatalogObjectKind::kIndex,
                            index, "automatic index name does not match its surviving ordinal"));
      }
      for (std::size_t term = 0; term < current.key_term_count; ++term) {
        if (!std::holds_alternative<ColumnId>(current.terms[term].target)) {
          return Failure(ValidationError(
              CatalogValidationCode::kInvalidIndexShape, CatalogObjectKind::kIndex, index,
              "automatic index key terms must be columns", CatalogMemberKind::kIndexTerm, term));
        }
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        const CatalogIndex& candidate = indexes_[previous];
        if (candidate.origin == IndexOrigin::kCreateIndex || candidate.table != current.table ||
            candidate.key_term_count != current.key_term_count) {
          continue;
        }
        bool equivalent = true;
        for (std::size_t term = 0; term < current.key_term_count; ++term) {
          if (!SameColumnIdentity(candidate.terms[term], current.terms[term])) {
            equivalent = false;
            break;
          }
        }
        if (equivalent) {
          return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                         CatalogObjectKind::kIndex, index,
                                         "equivalent automatic constraints must be folded"));
        }
      }
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidateTerm(std::size_t index, std::size_t term_index,
                                              const CatalogIndexTerm& expected) const {
    const CatalogIndexTerm& actual = indexes_[index].terms[term_index];
    if (!TermsMatch(actual, expected)) {
      return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                     CatalogObjectKind::kIndex, index,
                                     "physical index term does not match SQLite layout",
                                     CatalogMemberKind::kIndexTerm, term_index));
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidatePhysicalIndexShape(std::size_t index) {
    CatalogIndex& catalog_index = indexes_[index];
    const CatalogTable& table = tables_[catalog_index.table.value];
    const auto key_terms =
        std::span<const CatalogIndexTerm>{catalog_index.terms.data(), catalog_index.key_term_count};

    if (!table.without_rowid) {
      if (catalog_index.terms.size() != catalog_index.key_term_count + 1U) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                       CatalogObjectKind::kIndex, index,
                                       "rowid-table index must contain one rowid suffix"));
      }
      if (auto status = ValidateTerm(index, catalog_index.key_term_count,
                                     CatalogIndexTerm{
                                         .target = RowIdIndexTerm{},
                                         .collation_name = "BINARY",
                                         .order = SortOrder::kAscending,
                                     });
          !status.has_value()) {
        return status;
      }
    } else {
      const std::optional<IndexId> primary = primary_key_indexes_[catalog_index.table.value];
      assert(primary.has_value());
      const CatalogIndex& primary_index = indexes_[primary->value];
      std::vector<CatalogIndexTerm> expected_auxiliary;

      if (catalog_index.origin == IndexOrigin::kPrimaryKey) {
        for (std::size_t term = 0; term < catalog_index.key_term_count; ++term) {
          if (!std::holds_alternative<ColumnId>(catalog_index.terms[term].target)) {
            return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                           CatalogObjectKind::kIndex, index,
                                           "WITHOUT ROWID primary-key terms must be columns",
                                           CatalogMemberKind::kIndexTerm, term));
          }
          for (std::size_t previous = 0; previous < term; ++previous) {
            if (SameColumnIdentity(catalog_index.terms[previous], catalog_index.terms[term])) {
              return Failure(ValidationError(
                  CatalogValidationCode::kInvalidIndexShape, CatalogObjectKind::kIndex, index,
                  "WITHOUT ROWID primary-key duplicates were not normalized",
                  CatalogMemberKind::kIndexTerm, term));
            }
          }
        }
        for (std::size_t column = 0; column < table.columns.size(); ++column) {
          if (!ContainsColumn(key_terms, ColumnId{column})) {
            expected_auxiliary.push_back(CatalogIndexTerm{
                .target = ColumnId{column},
                .collation_name = table.columns[column].collation_name,
                .order = SortOrder::kAscending,
            });
          }
        }
      } else {
        const auto primary_keys = std::span<const CatalogIndexTerm>{primary_index.terms.data(),
                                                                    primary_index.key_term_count};
        for (const CatalogIndexTerm& primary_term : primary_keys) {
          if (!ContainsColumnIdentity(key_terms, primary_term)) {
            CatalogIndexTerm appended = primary_term;
            if (catalog_index.origin != IndexOrigin::kCreateIndex) {
              appended.order = SortOrder::kAscending;
            }
            expected_auxiliary.push_back(std::move(appended));
          }
        }
      }

      if (catalog_index.terms.size() != catalog_index.key_term_count + expected_auxiliary.size()) {
        return Failure(ValidationError(CatalogValidationCode::kInvalidIndexShape,
                                       CatalogObjectKind::kIndex, index,
                                       "WITHOUT ROWID index has the wrong auxiliary term count"));
      }
      for (std::size_t auxiliary = 0; auxiliary < expected_auxiliary.size(); ++auxiliary) {
        const std::size_t term_index = catalog_index.key_term_count + auxiliary;
        if (auto status = ValidateTerm(index, term_index, expected_auxiliary[auxiliary]);
            !status.has_value()) {
          return status;
        }
      }
    }

    bool unique_not_null = catalog_index.unique;
    if (unique_not_null &&
        !(table.without_rowid && catalog_index.origin == IndexOrigin::kPrimaryKey)) {
      for (std::size_t term = 0; term < catalog_index.key_term_count; ++term) {
        const auto* column = std::get_if<ColumnId>(&catalog_index.terms[term].target);
        if (column == nullptr) {
          unique_not_null = false;
          break;
        }
        if (table.rowid_alias.has_value() && *table.rowid_alias == *column) {
          continue;
        }
        const CatalogColumn& catalog_column = table.columns[column->value];
        const bool non_null = catalog_index.origin == IndexOrigin::kCreateIndex
                                  ? catalog_column.effective_not_null_conflict.has_value()
                                  : catalog_column.declared_not_null_conflict.has_value();
        if (!non_null) {
          unique_not_null = false;
          break;
        }
      }
    }
    if (table.without_rowid && catalog_index.origin == IndexOrigin::kPrimaryKey) {
      unique_not_null = true;
    }
    catalog_index.unique_not_null = unique_not_null;
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidateIndexRelationships() {
    for (std::size_t table = 0; table < tables_.size(); ++table) {
      if (auto status = ValidatePrimaryKey(table); !status.has_value()) {
        return status;
      }
    }
    if (auto status = ValidateAutomaticIndexes(); !status.has_value()) {
      return status;
    }
    for (std::size_t index = 0; index < indexes_.size(); ++index) {
      if (auto status = ValidatePhysicalIndexShape(index); !status.has_value()) {
        return status;
      }
    }
    return Success();
  }

  [[nodiscard]] ValidationStatus ValidateDefinitionOwnership() const {
    for (std::size_t definition = 0; definition < definition_owner_counts_.size(); ++definition) {
      if (definition_owner_counts_[definition] == 0U) {
        return Failure(ValidationError(CatalogValidationCode::kUnownedDefinition,
                                       CatalogObjectKind::kDefinition, definition,
                                       "schema definition has no catalog-object owner"));
      }
    }
    return Success();
  }

  CatalogInput input_;
  std::vector<std::size_t> definition_owner_counts_;
  std::vector<CatalogTable> tables_;
  std::vector<CatalogIndex> indexes_;
  std::vector<TableId> table_lookup_;
  std::vector<IndexId> index_lookup_;
  std::vector<std::vector<ColumnId>> column_lookups_;
  std::vector<std::vector<IndexId>> table_indexes_;
  std::vector<std::optional<IndexId>> primary_key_indexes_;
};

[[nodiscard]] std::expected<CatalogBuildData, CatalogValidationError> BuildCatalog(
    CatalogInput input) {
  return CatalogBuilder{std::move(input)}.Build();
}

}  // namespace

bool CatalogNamesEqual(std::string_view left, std::string_view right) noexcept {
  return CompareCatalogNames(left, right) == 0;
}

TypeAffinity DetermineTypeAffinity(std::optional<std::string_view> declared_type) noexcept {
  if (!declared_type.has_value()) {
    return TypeAffinity::kBlob;
  }
  const std::string_view type = *declared_type;
  if (ContainsFolded(type, "INT")) {
    return TypeAffinity::kInteger;
  }
  if (ContainsFolded(type, "CHAR") || ContainsFolded(type, "CLOB") ||
      ContainsFolded(type, "TEXT")) {
    return TypeAffinity::kText;
  }
  if (ContainsFolded(type, "BLOB")) {
    return TypeAffinity::kBlob;
  }
  if (ContainsFolded(type, "REAL") || ContainsFolded(type, "FLOA") ||
      ContainsFolded(type, "DOUB")) {
    return TypeAffinity::kReal;
  }
  return TypeAffinity::kNumeric;
}

CatalogSnapshotResult CatalogSnapshot::Create(CatalogInput input) {
  auto result = BuildCatalog(std::move(input));
  if (!result.has_value()) {
    return std::unexpected(std::move(result.error()));
  }
  CatalogBuildData data = std::move(*result);
  std::shared_ptr<CatalogSnapshot> snapshot{new CatalogSnapshot(
      std::move(data.schema_name), data.version, std::move(data.definitions),
      std::move(data.tables), std::move(data.indexes), std::move(data.table_lookup),
      std::move(data.index_lookup), std::move(data.column_lookups), std::move(data.table_indexes),
      std::move(data.primary_key_indexes))};
  return CatalogSnapshotPtr{std::move(snapshot)};
}

CatalogSnapshot::CatalogSnapshot(std::string schema_name, CatalogVersion version,
                                 std::vector<SyntaxTree> definitions,
                                 std::vector<CatalogTable> tables,
                                 std::vector<CatalogIndex> indexes,
                                 std::vector<TableId> table_lookup,
                                 std::vector<IndexId> index_lookup,
                                 std::vector<std::vector<ColumnId>> column_lookups,
                                 std::vector<std::vector<IndexId>> table_indexes,
                                 std::vector<std::optional<IndexId>> primary_key_indexes) noexcept
    : schema_name_(std::move(schema_name)),
      version_(version),
      definitions_(std::move(definitions)),
      tables_(std::move(tables)),
      indexes_(std::move(indexes)),
      table_lookup_(std::move(table_lookup)),
      index_lookup_(std::move(index_lookup)),
      column_lookups_(std::move(column_lookups)),
      table_indexes_(std::move(table_indexes)),
      primary_key_indexes_(std::move(primary_key_indexes)) {}

const SyntaxTree& CatalogSnapshot::definition(SchemaDefinitionId id) const noexcept {
  assert(id.value < definitions_.size());
  return definitions_[id.value];
}

const Expression& CatalogSnapshot::expression(SchemaExpression expression_ref) const noexcept {
  return definition(expression_ref.definition).expression(expression_ref.expression);
}

const CatalogTable& CatalogSnapshot::table(TableId id) const noexcept {
  assert(id.value < tables_.size());
  return tables_[id.value];
}

const CatalogColumn& CatalogSnapshot::column(TableId table_id, ColumnId column_id) const noexcept {
  const CatalogTable& catalog_table = table(table_id);
  assert(column_id.value < catalog_table.columns.size());
  return catalog_table.columns[column_id.value];
}

const CatalogIndex& CatalogSnapshot::index(IndexId id) const noexcept {
  assert(id.value < indexes_.size());
  return indexes_[id.value];
}

std::span<const IndexId> CatalogSnapshot::table_indexes(TableId table_id) const noexcept {
  assert(table_id.value < table_indexes_.size());
  return table_indexes_[table_id.value];
}

std::optional<IndexId> CatalogSnapshot::primary_key_index(TableId table_id) const noexcept {
  assert(table_id.value < primary_key_indexes_.size());
  return primary_key_indexes_[table_id.value];
}

std::optional<TableId> CatalogSnapshot::FindTable(std::string_view name) const noexcept {
  return FindLookup(std::span<const TableId>{table_lookup_}, name,
                    [this](TableId id) -> std::string_view { return tables_[id.value].name; });
}

std::optional<IndexId> CatalogSnapshot::FindIndex(std::string_view name) const noexcept {
  return FindLookup(std::span<const IndexId>{index_lookup_}, name,
                    [this](IndexId id) -> std::string_view { return indexes_[id.value].name; });
}

std::optional<ColumnId> CatalogSnapshot::FindColumn(TableId table_id,
                                                    std::string_view name) const noexcept {
  assert(table_id.value < column_lookups_.size());
  const CatalogTable& catalog_table = table(table_id);
  const std::vector<ColumnId>& lookup = column_lookups_[table_id.value];
  return FindLookup(std::span<const ColumnId>{lookup}, name,
                    [&catalog_table](ColumnId id) -> std::string_view {
                      return catalog_table.columns[id.value].name;
                    });
}

}  // namespace modern_sqlite
