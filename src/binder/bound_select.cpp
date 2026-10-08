#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../runtime/sqlite_float.hpp"
#include "../syntax/token_text.hpp"
#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/binder/bound_statement.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/syntax/ast.hpp"
#include "modern_sqlite/syntax/lexer.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {
namespace {

template <typename T>
using BindExpected = std::expected<T, BindError>;

struct ExpressionProperties {
  TypeAffinity affinity = TypeAffinity::kNone;
  std::optional<BoundCollationId> collation;
  bool explicit_collation = false;
  BoundTruthHint truth_hint = BoundTruthHint::kNone;
};

struct AliasEntry {
  std::string name;
  BoundExpressionId target;
};

struct ParameterOccurrence {
  ExpressionId expression;
  SourceSpan span;
};

struct BindScope {
  bool source_columns = true;
  bool result_aliases = false;
};

struct SourceState {
  std::string name;
  std::optional<std::string> alias;
  std::optional<TableId> table;
  bool schema_table = false;
};

struct DecodedNamePart {
  std::string_view borrowed;
  std::optional<std::string> owned;

  [[nodiscard]] std::string_view value() const noexcept {
    return owned.has_value() ? std::string_view{*owned} : borrowed;
  }
};

struct DecodedNameParts {
  std::array<DecodedNamePart, 3> parts;
  std::size_t count = 0;

  [[nodiscard]] bool empty() const noexcept { return count == 0; }
  [[nodiscard]] std::size_t size() const noexcept { return count; }
  [[nodiscard]] std::string_view operator[](std::size_t index) const noexcept {
    return parts[index].value();
  }
  [[nodiscard]] std::string_view front() const noexcept { return parts[0].value(); }
  [[nodiscard]] std::string_view back() const noexcept { return parts[count - 1U].value(); }
};

struct SchemaColumnDefinition {
  std::string_view name;
  std::string_view declared_type;
  TypeAffinity affinity;
};

constexpr std::array<SchemaColumnDefinition, 5> kSchemaColumns = {{
    {.name = "type", .declared_type = "TEXT", .affinity = TypeAffinity::kText},
    {.name = "name", .declared_type = "TEXT", .affinity = TypeAffinity::kText},
    {.name = "tbl_name", .declared_type = "TEXT", .affinity = TypeAffinity::kText},
    {.name = "rootpage", .declared_type = "INT", .affinity = TypeAffinity::kInteger},
    {.name = "sql", .declared_type = "TEXT", .affinity = TypeAffinity::kText},
}};

[[nodiscard]] BindError BinderError(BindErrorCode code, SourceSpan span, std::string detail) {
  return BindError{.code = code, .span = span, .detail = std::move(detail)};
}

[[nodiscard]] bool NamesEqual(std::string_view left, std::string_view right) noexcept {
  return CatalogNamesEqual(left, right);
}

[[nodiscard]] bool IsRowIdName(std::string_view name) noexcept {
  return NamesEqual(name, "rowid") || NamesEqual(name, "_rowid_") || NamesEqual(name, "oid");
}

[[nodiscard]] bool IsSchemaTableName(std::string_view name) noexcept {
  return NamesEqual(name, "sqlite_schema") || NamesEqual(name, "sqlite_master");
}

[[nodiscard]] bool IsQuotedToken(std::string_view token) noexcept {
  if (token.empty()) {
    return false;
  }
  return token.front() == '\'' || token.front() == '"' || token.front() == '`' ||
         token.front() == '[';
}

[[nodiscard]] bool IsExactlyTokenKind(std::string_view token, TokenKind expected) noexcept {
  Lexer lexer{Utf8View{token}};
  const Token first = lexer.Next();
  const Token second = lexer.Next();
  return first.kind == expected && first.span.begin().value() == 0U &&
         first.span.length().value() == token.size() && second.kind == TokenKind::kEndOfInput;
}

[[nodiscard]] bool IsBareDefaultNameToken(std::string_view token) noexcept {
  Lexer lexer{Utf8View{token}};
  const Token first = lexer.Next();
  const Token second = lexer.Next();
  return (first.kind == TokenKind::kIdentifier || first.kind == TokenKind::kIndexed ||
          first.kind == TokenKind::kJoinKeyword || IsKeyword(first.kind)) &&
         first.span.begin().value() == 0U && first.span.length().value() == token.size() &&
         second.kind == TokenKind::kEndOfInput;
}

[[nodiscard]] bool IsHexadecimalLiteral(std::string_view token) noexcept {
  return token.size() > 2U && token[0] == '0' && (token[1] == 'x' || token[1] == 'X');
}

[[nodiscard]] bool NumericUnderscoresAreValid(std::string_view token) noexcept {
  const bool hexadecimal = IsHexadecimalLiteral(token);
  const auto is_digit = [hexadecimal](char value) noexcept {
    if (value >= '0' && value <= '9') {
      return true;
    }
    return hexadecimal && ((value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'));
  };
  for (std::size_t index = 0; index < token.size(); ++index) {
    if (token[index] == '_' && (index == 0U || index + 1U == token.size() ||
                                !is_digit(token[index - 1U]) || !is_digit(token[index + 1U]))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool LiteralTokenMatchesKind(LiteralKind kind, std::string_view token) noexcept {
  switch (kind) {
    case LiteralKind::kNull:
      return NamesEqual(token, "null");
    case LiteralKind::kInteger:
      return IsExactlyTokenKind(token, TokenKind::kInteger) ||
             (IsExactlyTokenKind(token, TokenKind::kQuotedNumber) &&
              NumericUnderscoresAreValid(token) &&
              (IsHexadecimalLiteral(token) ||
               token.find_first_of(".eE") == std::string_view::npos));
    case LiteralKind::kReal:
      return IsExactlyTokenKind(token, TokenKind::kFloat) ||
             (IsExactlyTokenKind(token, TokenKind::kQuotedNumber) && !IsHexadecimalLiteral(token) &&
              NumericUnderscoresAreValid(token) &&
              token.find_first_of(".eE") != std::string_view::npos);
    case LiteralKind::kString:
      return !token.empty() &&
             ((token.front() == '\'' && IsExactlyTokenKind(token, TokenKind::kString)) ||
              (token.front() == '"' && IsExactlyTokenKind(token, TokenKind::kIdentifier)));
    case LiteralKind::kBlob:
      return IsExactlyTokenKind(token, TokenKind::kBlob);
    case LiteralKind::kCurrentDate:
      return NamesEqual(token, "current_date");
    case LiteralKind::kCurrentTime:
      return NamesEqual(token, "current_time");
    case LiteralKind::kCurrentTimestamp:
      return NamesEqual(token, "current_timestamp");
    case LiteralKind::kTrue:
      return NamesEqual(token, "true");
    case LiteralKind::kFalse:
      return NamesEqual(token, "false");
  }
  return false;
}

[[nodiscard]] bool IsPatternOperation(BinaryOperator operation) noexcept {
  switch (operation) {
    case BinaryOperator::kLike:
    case BinaryOperator::kNotLike:
    case BinaryOperator::kGlob:
    case BinaryOperator::kNotGlob:
    case BinaryOperator::kRegexp:
    case BinaryOperator::kNotRegexp:
    case BinaryOperator::kMatch:
    case BinaryOperator::kNotMatch:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool IsComparisonOperation(BinaryOperator operation) noexcept {
  switch (operation) {
    case BinaryOperator::kLess:
    case BinaryOperator::kLessOrEqual:
    case BinaryOperator::kGreater:
    case BinaryOperator::kGreaterOrEqual:
    case BinaryOperator::kEqual:
    case BinaryOperator::kNotEqual:
    case BinaryOperator::kIs:
    case BinaryOperator::kIsNot:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool IsKnownAggregateName(std::string_view name) noexcept {
  return NamesEqual(name, "avg") || NamesEqual(name, "count") || NamesEqual(name, "group_concat") ||
         NamesEqual(name, "sum") || NamesEqual(name, "total");
}

[[nodiscard]] std::string StripNumericUnderscores(std::string_view token) {
  std::string stripped;
  stripped.reserve(token.size());
  for (const char byte : token) {
    if (byte != '_') {
      stripped.push_back(byte);
    }
  }
  return stripped;
}

[[nodiscard]] bool IsSignedMinimumMagnitude(std::string_view token) {
  const std::string normalized = StripNumericUnderscores(token);
  if (normalized.starts_with("0x") || normalized.starts_with("0X")) {
    return false;
  }
  const std::size_t first_nonzero = normalized.find_first_not_of('0');
  return first_nonzero != std::string::npos &&
         std::string_view{normalized}.substr(first_nonzero) == "9223372036854775808";
}

[[nodiscard]] std::optional<std::uint8_t> HexDigit(char byte) noexcept {
  if (byte >= '0' && byte <= '9') {
    return static_cast<std::uint8_t>(byte - '0');
  }
  if (byte >= 'a' && byte <= 'f') {
    return static_cast<std::uint8_t>(byte - 'a' + 10);
  }
  if (byte >= 'A' && byte <= 'F') {
    return static_cast<std::uint8_t>(byte - 'A' + 10);
  }
  return std::nullopt;
}

[[nodiscard]] TypeAffinity SelectComparisonAffinity(TypeAffinity left,
                                                    TypeAffinity right) noexcept {
  const auto is_numeric = [](TypeAffinity affinity) noexcept {
    return affinity == TypeAffinity::kNumeric || affinity == TypeAffinity::kInteger ||
           affinity == TypeAffinity::kReal;
  };
  if (left != TypeAffinity::kNone && right != TypeAffinity::kNone) {
    return is_numeric(left) || is_numeric(right) ? TypeAffinity::kNumeric : TypeAffinity::kBlob;
  }
  return left != TypeAffinity::kNone ? left : right;
}

[[nodiscard]] BoundUnaryOperation ToBoundUnary(UnaryOperator operation) noexcept {
  switch (operation) {
    case UnaryOperator::kPositive:
      return BoundUnaryOperation::kPositive;
    case UnaryOperator::kNegative:
      return BoundUnaryOperation::kNegative;
    case UnaryOperator::kBitwiseNot:
      return BoundUnaryOperation::kBitwiseNot;
    case UnaryOperator::kNot:
      return BoundUnaryOperation::kLogicalNot;
  }
  return BoundUnaryOperation::kPositive;
}

[[nodiscard]] BoundBinaryOperation ToBoundBinary(BinaryOperator operation) noexcept {
  switch (operation) {
    case BinaryOperator::kAdd:
      return BoundBinaryOperation::kAdd;
    case BinaryOperator::kSubtract:
      return BoundBinaryOperation::kSubtract;
    case BinaryOperator::kMultiply:
      return BoundBinaryOperation::kMultiply;
    case BinaryOperator::kDivide:
      return BoundBinaryOperation::kDivide;
    case BinaryOperator::kRemainder:
      return BoundBinaryOperation::kRemainder;
    case BinaryOperator::kLeftShift:
      return BoundBinaryOperation::kShiftLeft;
    case BinaryOperator::kRightShift:
      return BoundBinaryOperation::kShiftRight;
    case BinaryOperator::kBitwiseAnd:
      return BoundBinaryOperation::kBitwiseAnd;
    case BinaryOperator::kBitwiseOr:
      return BoundBinaryOperation::kBitwiseOr;
    case BinaryOperator::kConcatenate:
      return BoundBinaryOperation::kConcatenate;
    case BinaryOperator::kAnd:
      return BoundBinaryOperation::kLogicalAnd;
    case BinaryOperator::kOr:
      return BoundBinaryOperation::kLogicalOr;
    default:
      return BoundBinaryOperation::kAdd;
  }
}

[[nodiscard]] SqlComparison ToSqlComparison(BinaryOperator operation) noexcept {
  switch (operation) {
    case BinaryOperator::kLess:
      return SqlComparison::kLess;
    case BinaryOperator::kLessOrEqual:
      return SqlComparison::kLessEqual;
    case BinaryOperator::kGreater:
      return SqlComparison::kGreater;
    case BinaryOperator::kGreaterOrEqual:
      return SqlComparison::kGreaterEqual;
    case BinaryOperator::kEqual:
      return SqlComparison::kEqual;
    case BinaryOperator::kNotEqual:
      return SqlComparison::kNotEqual;
    case BinaryOperator::kIs:
      return SqlComparison::kIs;
    case BinaryOperator::kIsNot:
      return SqlComparison::kIsNot;
    default:
      return SqlComparison::kEqual;
  }
}

}  // namespace

struct BoundExpressionState {
  std::string source;
  CatalogSnapshotPtr catalog;
  std::uint64_t registration_generation = 0;
  std::optional<BoundTableSource> table_source;
  std::vector<BoundSourceColumn> source_columns;
  std::vector<BoundCollation> collations;
  std::vector<BoundScalarFunction> functions;
  std::vector<BoundParameter> parameters;
  std::vector<BoundExpression> expressions;
};

struct BoundSelect::Impl final : BoundExpressionState {
  std::vector<std::string> registered_collations;
  std::vector<BoundResultColumn> result_columns;
  std::optional<BoundExpressionId> where;
  std::optional<BoundLimit> limit;
};

struct BoundInsert::Impl final : BoundExpressionState {
  BoundMutationTarget target;
  std::vector<BoundInsertValue> values;
  bool default_values = false;
  bool explicit_columns = false;
};

struct BoundUpdate::Impl final : BoundExpressionState {
  BoundMutationTarget target;
  std::vector<BoundUpdateAssignment> assignments;
  std::optional<BoundExpressionId> where;
  bool changes_rowid = false;
};

struct BoundDelete::Impl final : BoundExpressionState {
  BoundMutationTarget target;
  std::optional<BoundExpressionId> where;
};

struct BoundCreateTable::Impl final {
  std::string source;
  CatalogSnapshotPtr catalog;
  std::string table_name;
  bool if_not_exists = false;
  bool no_op = false;
  std::string canonical_sql;
  std::vector<BoundCreateColumn> columns;
  std::optional<ColumnId> rowid_alias;
};

struct BoundCreateIndex::Impl final {
  std::string source;
  CatalogSnapshotPtr catalog;
  TableId table{};
  RootPageId table_root_page{};
  std::string index_name;
  std::string table_name;
  bool unique = false;
  bool unique_not_null = false;
  bool if_not_exists = false;
  bool no_op = false;
  std::string canonical_sql;
  std::vector<BoundCreateIndexTerm> terms;
};

namespace binder_detail {

class StatementBinder final {
 public:
  StatementBinder(SyntaxTree tree, CatalogSnapshotPtr catalog, BindEnvironment environment,
                  BindOptions options)
      : tree_(std::move(tree)),
        catalog_(std::move(catalog)),
        environment_(environment),
        options_(options),
        impl_(std::make_unique<BoundSelect::Impl>()) {}

  [[nodiscard]] BindSelectResult RunSelect() {
    if (!std::holds_alternative<SelectStatement>(tree_.statement())) {
      return std::unexpected(
          BinderError(BindErrorCode::kInvalidInput, {}, "syntax tree is not a SELECT statement"));
    }
    const auto& select = std::get<SelectStatement>(tree_.statement());
    if (!ValidateTree(select)) {
      return std::unexpected(BinderError(BindErrorCode::kInvalidInput, select.span,
                                         "syntax tree contains an invalid reference"));
    }
    if (select.quantifier == SelectQuantifier::kDistinct) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, select.span,
                                         "SELECT DISTINCT is not supported"));
    }

    BindExpected<void> initialized = InitializeCommon();
    if (!initialized.has_value()) {
      return std::unexpected(std::move(initialized.error()));
    }
    BindExpected<void> source = BindSource(select);
    if (!source.has_value()) {
      return std::unexpected(std::move(source.error()));
    }
    if (impl_->table_source.has_value() &&
        impl_->table_source->kind == BoundSourceKind::kCatalogTable &&
        impl_->table_source->table.has_value() &&
        !catalog_->table_indexes(*impl_->table_source->table).empty()) {
      impl_->registered_collations.reserve(environment_.collations().size());
      for (const Collation* collation : environment_.collations()) {
        impl_->registered_collations.emplace_back(collation->name());
      }
    }
    ReserveOutputStorage(select);
    BindExpected<void> results = BindResults(select);
    if (!results.has_value()) {
      return std::unexpected(std::move(results.error()));
    }
    if (select.where.has_value()) {
      BindExpected<BoundExpressionId> where =
          BindExpression(*select.where, BindScope{.source_columns = true, .result_aliases = true});
      if (!where.has_value()) {
        return std::unexpected(std::move(where.error()));
      }
      impl_->where = *where;
    }
    if (select.limit.has_value()) {
      const BindScope empty_scope{.source_columns = false, .result_aliases = false};
      BindExpected<BoundExpressionId> limit = BindExpression(select.limit->limit, empty_scope);
      if (!limit.has_value()) {
        return std::unexpected(std::move(limit.error()));
      }
      BoundLimit bound_limit{.limit = *limit};
      if (select.limit->offset.has_value()) {
        BindExpected<BoundExpressionId> offset = BindExpression(*select.limit->offset, empty_scope);
        if (!offset.has_value()) {
          return std::unexpected(std::move(offset.error()));
        }
        bound_limit.offset = *offset;
      }
      impl_->limit = bound_limit;
    }
    return BoundSelect(std::move(impl_));
  }

  [[nodiscard]] BindStatementResult RunStatement() {
    if (std::holds_alternative<SelectStatement>(tree_.statement())) {
      BindSelectResult select = RunSelect();
      if (!select.has_value()) {
        return std::unexpected(std::move(select.error()));
      }
      return BoundStatement{std::in_place_type<BoundSelect>, std::move(*select)};
    }
    if (std::holds_alternative<InsertStatement>(tree_.statement())) {
      BindExpected<BoundInsert> insert = RunInsert();
      if (!insert.has_value()) {
        return std::unexpected(std::move(insert.error()));
      }
      return BoundStatement{std::in_place_type<BoundInsert>, std::move(*insert)};
    }
    if (std::holds_alternative<UpdateStatement>(tree_.statement())) {
      BindExpected<BoundUpdate> update = RunUpdate();
      if (!update.has_value()) {
        return std::unexpected(std::move(update.error()));
      }
      return BoundStatement{std::in_place_type<BoundUpdate>, std::move(*update)};
    }
    if (std::holds_alternative<DeleteStatement>(tree_.statement())) {
      BindExpected<BoundDelete> delete_statement = RunDelete();
      if (!delete_statement.has_value()) {
        return std::unexpected(std::move(delete_statement.error()));
      }
      return BoundStatement{std::in_place_type<BoundDelete>, std::move(*delete_statement)};
    }
    if (std::holds_alternative<CreateTableStatement>(tree_.statement())) {
      BindExpected<BoundCreateTable> create = RunCreateTable();
      if (!create.has_value()) {
        return std::unexpected(std::move(create.error()));
      }
      return BoundStatement{std::in_place_type<BoundCreateTable>, std::move(*create)};
    }
    if (std::holds_alternative<CreateIndexStatement>(tree_.statement())) {
      BindExpected<BoundCreateIndex> create = RunCreateIndex();
      if (!create.has_value()) {
        return std::unexpected(std::move(create.error()));
      }
      return BoundStatement{std::in_place_type<BoundCreateIndex>, std::move(*create)};
    }
    BindExpected<void> environment = ValidateEnvironment();
    if (!environment.has_value()) {
      return std::unexpected(std::move(environment.error()));
    }
    if (const auto* begin = std::get_if<BeginTransactionStatement>(&tree_.statement());
        begin != nullptr) {
      return BoundStatement{BoundBeginTransaction{.mode = begin->mode}};
    }
    if (const auto* commit = std::get_if<CommitTransactionStatement>(&tree_.statement());
        commit != nullptr) {
      return BoundStatement{BoundCommitTransaction{.syntax = commit->syntax}};
    }
    if (std::holds_alternative<RollbackTransactionStatement>(tree_.statement())) {
      return BoundStatement{BoundRollbackTransaction{}};
    }
    if (const auto* savepoint = std::get_if<SavepointStatement>(&tree_.statement());
        savepoint != nullptr) {
      BindExpected<std::string> name = Dequote(savepoint->name);
      if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
      }
      return BoundStatement{BoundSavepoint{.name = std::move(*name)}};
    }
    if (const auto* release = std::get_if<ReleaseSavepointStatement>(&tree_.statement());
        release != nullptr) {
      BindExpected<std::string> name = Dequote(release->name);
      if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
      }
      return BoundStatement{BoundReleaseSavepoint{.name = std::move(*name)}};
    }
    if (const auto* rollback = std::get_if<RollbackToSavepointStatement>(&tree_.statement());
        rollback != nullptr) {
      BindExpected<std::string> name = Dequote(rollback->name);
      if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
      }
      return BoundStatement{BoundRollbackToSavepoint{.name = std::move(*name)}};
    }
    const SourceSpan span =
        std::visit([](const auto& statement) { return statement.span; }, tree_.statement());
    return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, span,
                                       "statement execution is not supported"));
  }

  [[nodiscard]] BindExpected<BoundInsert> RunInsert() {
    const auto& insert = std::get<InsertStatement>(tree_.statement());
    BindExpected<void> initialized = InitializeCommon();
    if (!initialized.has_value()) {
      return std::unexpected(std::move(initialized.error()));
    }
    BindExpected<BoundMutationTarget> target = ResolveMutationTarget(insert.table);
    if (!target.has_value()) {
      return std::unexpected(std::move(target.error()));
    }

    auto output = std::make_unique<BoundInsert::Impl>();
    output->target = std::move(*target);
    output->explicit_columns = !insert.columns.empty();
    if (std::holds_alternative<InsertDefaultValuesSource>(insert.source)) {
      if (!insert.columns.empty()) {
        return std::unexpected(
            BinderError(BindErrorCode::kColumnCountMismatch, insert.span,
                        "0 values for " + std::to_string(insert.columns.size()) + " columns"));
      }
      output->default_values = true;
      MoveCommon(*output);
      return BoundInsert(std::move(output));
    }

    const auto& values = std::get<InsertValuesSource>(insert.source).values;
    const std::size_t target_count =
        insert.columns.empty() ? output->target.columns.size() : insert.columns.size();
    if (values.size() != target_count) {
      return std::unexpected(BinderError(BindErrorCode::kColumnCountMismatch, insert.span,
                                         std::to_string(values.size()) + " values for " +
                                             std::to_string(target_count) + " columns"));
    }

    output->values.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
      BindExpected<BoundMutationField> field =
          insert.columns.empty() ? BindExpected<BoundMutationField>{BoundMutationField{
                                       .column = ColumnId{index},
                                       .rowid = output->target.rowid_alias == ColumnId{index},
                                   }}
                                 : ResolveMutationField(output->target, insert.columns[index]);
      if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
      }
      BindExpected<BoundExpressionId> expression = BindExpression(
          values[index], BindScope{.source_columns = false, .result_aliases = false});
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      output->values.push_back(BoundInsertValue{
          .target = *field,
          .expression = *expression,
      });
    }
    MarkInsertEffectiveness(output->values);
    MoveCommon(*output);
    return BoundInsert(std::move(output));
  }

  [[nodiscard]] BindExpected<BoundUpdate> RunUpdate() {
    const auto& update = std::get<UpdateStatement>(tree_.statement());
    BindExpected<void> initialized = InitializeCommon();
    if (!initialized.has_value()) {
      return std::unexpected(std::move(initialized.error()));
    }
    BindExpected<BoundMutationTarget> target = ResolveMutationTarget(update.table);
    if (!target.has_value()) {
      return std::unexpected(std::move(target.error()));
    }
    PublishMutationSource(*target);

    auto output = std::make_unique<BoundUpdate::Impl>();
    output->target = std::move(*target);
    output->assignments.reserve(update.assignments.size());
    for (const UpdateAssignment& assignment : update.assignments) {
      BindExpected<BoundMutationField> field =
          ResolveMutationField(output->target, assignment.column);
      if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
      }
      BindExpected<BoundExpressionId> expression = BindExpression(
          assignment.expression, BindScope{.source_columns = true, .result_aliases = false});
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      output->assignments.push_back(BoundUpdateAssignment{
          .target = *field,
          .expression = *expression,
      });
    }
    MarkUpdateEffectiveness(output->assignments);
    output->changes_rowid =
        std::ranges::any_of(output->assignments, [](const BoundUpdateAssignment& assignment) {
          return assignment.effective && assignment.target.rowid;
        });
    if (update.where.has_value()) {
      BindExpected<BoundExpressionId> where =
          BindExpression(*update.where, BindScope{.source_columns = true, .result_aliases = false});
      if (!where.has_value()) {
        return std::unexpected(std::move(where.error()));
      }
      output->where = *where;
    }
    MoveCommon(*output);
    return BoundUpdate(std::move(output));
  }

  [[nodiscard]] BindExpected<BoundDelete> RunDelete() {
    const auto& delete_statement = std::get<DeleteStatement>(tree_.statement());
    BindExpected<void> initialized = InitializeCommon();
    if (!initialized.has_value()) {
      return std::unexpected(std::move(initialized.error()));
    }
    BindExpected<BoundMutationTarget> target = ResolveMutationTarget(delete_statement.table);
    if (!target.has_value()) {
      return std::unexpected(std::move(target.error()));
    }
    PublishMutationSource(*target);

    auto output = std::make_unique<BoundDelete::Impl>();
    output->target = std::move(*target);
    if (delete_statement.where.has_value()) {
      BindExpected<BoundExpressionId> where = BindExpression(
          *delete_statement.where, BindScope{.source_columns = true, .result_aliases = false});
      if (!where.has_value()) {
        return std::unexpected(std::move(where.error()));
      }
      output->where = *where;
    }
    MoveCommon(*output);
    return BoundDelete(std::move(output));
  }

  [[nodiscard]] BindExpected<BoundCreateTable> RunCreateTable() {
    const auto& table = std::get<CreateTableStatement>(tree_.statement());
    BindExpected<void> initialized = InitializeCommon();
    if (!initialized.has_value()) {
      return std::unexpected(std::move(initialized.error()));
    }
    BindExpected<DecodedNameParts> parts = NameParts(table.name);
    if (!parts.has_value()) {
      return std::unexpected(std::move(parts.error()));
    }
    if (parts->empty() || parts->size() > 2U ||
        (parts->size() == 2U && !NamesEqual(parts->front(), catalog_->schema_name()))) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, table.name.span,
                                         "only the main schema is writable"));
    }
    const std::string_view table_name = parts->back();
    if (HasSqlitePrefix(table_name)) {
      return std::unexpected(
          BinderError(BindErrorCode::kObjectNameReserved, table.name.span,
                      "object name reserved for internal use: " + std::string{table_name}));
    }
    if (catalog_->FindIndex(table_name).has_value()) {
      return std::unexpected(
          BinderError(BindErrorCode::kTableAlreadyExists, table.name.span,
                      "there is already an index named " + std::string{table_name}));
    }

    auto output = std::make_unique<BoundCreateTable::Impl>();
    output->source.assign(tree_.source().bytes());
    output->catalog = catalog_;
    output->table_name = std::string{table_name};
    output->if_not_exists = table.if_not_exists;
    if (catalog_->FindTable(table_name).has_value()) {
      if (!table.if_not_exists) {
        return std::unexpected(BinderError(BindErrorCode::kTableAlreadyExists, table.name.span,
                                           "table " + std::string{table_name} + " already exists"));
      }
      output->no_op = true;
      return BoundCreateTable(std::move(output));
    }

    if (table.temporary || table.without_rowid || table.strict || !table.constraints.empty()) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, table.span,
                                         "CREATE TABLE shape is not supported"));
    }

    const std::size_t retained_begin = table.name.parts.back().begin().value();
    const std::size_t retained_end = table.span.end().value();
    output->canonical_sql = "CREATE TABLE ";
    output->canonical_sql.append(
        tree_.source().bytes().substr(retained_begin, retained_end - retained_begin));
    output->columns.reserve(table.columns.size());

    std::vector<std::string> column_names;
    column_names.reserve(table.columns.size());
    std::optional<ColumnId> rowid_alias;
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
      const ColumnDefinition& column = table.columns[index];
      BindExpected<std::string> column_name = Dequote(column.name);
      if (!column_name.has_value()) {
        return std::unexpected(std::move(column_name.error()));
      }
      if (std::ranges::any_of(column_names, [&column_name](const std::string& existing) {
            return NamesEqual(existing, *column_name);
          })) {
        return std::unexpected(BinderError(BindErrorCode::kDuplicateColumn, column.name,
                                           "duplicate column name: " + *column_name));
      }
      column_names.push_back(*column_name);

      std::optional<std::string> declared_type;
      if (column.type_name.has_value()) {
        BindExpected<std::string> decoded_type = Dequote(*column.type_name);
        if (!decoded_type.has_value()) {
          return std::unexpected(std::move(decoded_type.error()));
        }
        declared_type = std::move(*decoded_type);
      }
      const TypeAffinity affinity = DetermineTypeAffinity(
          declared_type.has_value() ? std::optional<std::string_view>{*declared_type}
                                    : std::nullopt);
      bool not_null = false;
      bool primary_key = false;
      std::shared_ptr<const SqlValue> default_value;
      for (const ColumnConstraint& constraint : column.constraints) {
        if (constraint.name.has_value()) {
          return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, constraint.span,
                                             "named constraints are not supported"));
        }
        if (const auto* null_value = std::get_if<NullColumnConstraint>(&constraint.payload);
            null_value != nullptr) {
          if (!IsDefaultAbort(null_value->conflict)) {
            return std::unexpected(UnsupportedConflict(constraint.span));
          }
          continue;
        }
        if (const auto* not_null_constraint =
                std::get_if<NotNullColumnConstraint>(&constraint.payload);
            not_null_constraint != nullptr) {
          if (!IsDefaultAbort(not_null_constraint->conflict)) {
            return std::unexpected(UnsupportedConflict(constraint.span));
          }
          not_null = true;
          continue;
        }
        if (const auto* key = std::get_if<PrimaryKeyColumnConstraint>(&constraint.payload);
            key != nullptr) {
          if (primary_key || rowid_alias.has_value() || key->order == SortOrder::kDescending ||
              !IsDefaultAbort(key->conflict) || key->autoincrement || !declared_type.has_value() ||
              !NamesEqual(*declared_type, "INTEGER")) {
            return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, constraint.span,
                                               "only one INTEGER PRIMARY KEY column is supported"));
          }
          primary_key = true;
          rowid_alias = ColumnId{index};
          continue;
        }
        if (const auto* default_constraint =
                std::get_if<DefaultColumnConstraint>(&constraint.payload);
            default_constraint != nullptr) {
          if (default_value != nullptr) {
            return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, constraint.span,
                                               "multiple DEFAULT clauses are not supported"));
          }
          BindExpected<std::shared_ptr<const SqlValue>> materialized =
              MaterializeConstantDefault(default_constraint->expression, affinity);
          if (!materialized.has_value()) {
            return std::unexpected(std::move(materialized.error()));
          }
          default_value = std::move(*materialized);
          continue;
        }
        return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, constraint.span,
                                           "column constraint is not supported"));
      }

      output->columns.push_back(BoundCreateColumn{
          .column = ColumnId{index},
          .name = std::move(*column_name),
          .declared_type = std::move(declared_type),
          .affinity = affinity,
          .not_null = not_null,
          .default_value = std::move(default_value),
          .rowid_alias = primary_key,
      });
    }
    output->rowid_alias = rowid_alias;
    return BoundCreateTable(std::move(output));
  }

  [[nodiscard]] BindExpected<BoundCreateIndex> RunCreateIndex() {
    const auto& index = std::get<CreateIndexStatement>(tree_.statement());
    BindExpected<void> environment = ValidateEnvironment();
    if (!environment.has_value()) {
      return std::unexpected(std::move(environment.error()));
    }

    BindExpected<DecodedNameParts> index_parts = NameParts(index.name);
    if (!index_parts.has_value()) {
      return std::unexpected(std::move(index_parts.error()));
    }
    if (index_parts->empty() || index_parts->size() > 2U ||
        (index_parts->size() == 2U && !NamesEqual(index_parts->front(), catalog_->schema_name()))) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, index.name.span,
                                         "only the main schema is writable"));
    }
    BindExpected<DecodedNameParts> table_parts = NameParts(index.table);
    if (!table_parts.has_value()) {
      return std::unexpected(std::move(table_parts.error()));
    }
    if (table_parts->size() != 1U) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, index.table.span,
                                         "CREATE INDEX table names must be unqualified"));
    }

    const std::string_view index_name = index_parts->back();
    const std::string_view table_name = table_parts->back();
    const std::optional<TableId> table_id = catalog_->FindTable(table_name);
    if (!table_id.has_value()) {
      return std::unexpected(BinderError(BindErrorCode::kNoSuchTable, index.table.span,
                                         "no such table: main." + std::string{table_name}));
    }
    const CatalogTable& table = catalog_->table(*table_id);
    if (table.without_rowid || HasSqlitePrefix(table.name)) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, index.table.span,
                                         "table shape is not supported for CREATE INDEX"));
    }
    if (HasSqlitePrefix(index_name)) {
      return std::unexpected(
          BinderError(BindErrorCode::kObjectNameReserved, index.name.span,
                      "object name reserved for internal use: " + std::string{index_name}));
    }
    if (catalog_->FindTable(index_name).has_value()) {
      return std::unexpected(
          BinderError(BindErrorCode::kTableAlreadyExists, index.name.span,
                      "there is already a table named " + std::string{index_name}));
    }

    auto output = std::make_unique<BoundCreateIndex::Impl>();
    output->source.assign(tree_.source().bytes());
    output->catalog = catalog_;
    output->table = *table_id;
    output->table_root_page = table.root_page;
    output->index_name = std::string{index_name};
    output->table_name = table.name;
    output->unique = index.unique;
    output->unique_not_null = index.unique;
    output->if_not_exists = index.if_not_exists;

    if (catalog_->FindIndex(index_name).has_value()) {
      if (!index.if_not_exists) {
        return std::unexpected(BinderError(BindErrorCode::kIndexAlreadyExists, index.name.span,
                                           "index " + std::string{index_name} + " already exists"));
      }
      output->no_op = true;
      return BoundCreateIndex(std::move(output));
    }
    if (index.where.has_value()) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, index.span,
                                         "partial indexes are not supported"));
    }

    output->terms.reserve(index.terms.size());
    for (const IndexedTerm& term : index.terms) {
      ExpressionId expression_id = term.expression;
      std::optional<SourceSpan> collation_span = term.collation;
      while (true) {
        const Expression& expression = tree_.expression(expression_id);
        if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
            parenthesized != nullptr) {
          expression_id = parenthesized->inner;
          continue;
        }
        if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
            collate != nullptr) {
          if (!collation_span.has_value()) {
            collation_span = collate->collation;
          }
          expression_id = collate->operand;
          continue;
        }
        break;
      }

      const Expression& expression = tree_.expression(expression_id);
      const auto* identifier = std::get_if<IdentifierExpression>(&expression.payload);
      if (identifier == nullptr) {
        return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, term.span,
                                           "index expressions are not supported"));
      }
      BindExpected<DecodedNameParts> column_parts = NameParts(identifier->name);
      if (!column_parts.has_value()) {
        return std::unexpected(std::move(column_parts.error()));
      }
      if (column_parts->size() != 1U) {
        return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, term.span,
                                           "qualified index terms are not supported"));
      }
      const std::optional<ColumnId> column_id =
          catalog_->FindColumn(*table_id, column_parts->front());
      if (!column_id.has_value()) {
        return std::unexpected(
            BinderError(BindErrorCode::kNoSuchColumn, term.span,
                        "no such column: " + std::string{column_parts->front()}));
      }
      const CatalogColumn& column = table.columns[column_id->value];
      std::string collation_name = column.collation_name;
      if (collation_span.has_value()) {
        BindExpected<std::string> decoded = Dequote(*collation_span);
        if (!decoded.has_value()) {
          return std::unexpected(std::move(decoded.error()));
        }
        collation_name = std::move(*decoded);
      }
      if (FindRegisteredCollation(collation_name) == nullptr) {
        return std::unexpected(BinderError(BindErrorCode::kNoSuchCollation, term.span,
                                           "no such collation sequence: " + collation_name));
      }
      const bool rowid = table.rowid_alias == *column_id;
      const bool not_null = rowid || column.effective_not_null_conflict.has_value();
      output->unique_not_null = output->unique_not_null && not_null;
      output->terms.push_back(BoundCreateIndexTerm{
          .column = *column_id,
          .affinity = column.affinity,
          .collation_name = std::move(collation_name),
          .order =
              term.order == SortOrder::kDescending ? SortOrder::kDescending : SortOrder::kAscending,
          .rowid = rowid,
      });
    }

    const std::size_t retained_begin = index.name.parts.back().begin().value();
    const std::size_t retained_end = index.span.end().value();
    output->canonical_sql = index.unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ";
    output->canonical_sql.append(
        tree_.source().bytes().substr(retained_begin, retained_end - retained_begin));
    return BoundCreateIndex(std::move(output));
  }

 private:
  [[nodiscard]] BindExpected<void> ValidateEnvironment() const {
    if (catalog_ == nullptr) {
      return std::unexpected(
          BinderError(BindErrorCode::kInvalidInput, {}, "catalog snapshot is null"));
    }
    if (!LimitsAreRepresentable()) {
      return std::unexpected(
          BinderError(BindErrorCode::kInvalidInput, {}, "binder limit exceeds bound ID range"));
    }
    if (FindRegisteredCollation("BINARY") == nullptr) {
      return std::unexpected(
          BinderError(BindErrorCode::kInvalidInput, {}, "BINARY collation is not registered"));
    }
    if (std::ranges::any_of(environment_.collations(),
                            [](const Collation* collation) { return collation == nullptr; })) {
      return std::unexpected(BinderError(BindErrorCode::kInvalidInput, {},
                                         "collation registry contains a null descriptor"));
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> InitializeCommon() {
    BindExpected<void> environment = ValidateEnvironment();
    if (!environment.has_value()) {
      return environment;
    }
    impl_->source.assign(tree_.source().bytes());
    impl_->catalog = catalog_;
    impl_->registration_generation = environment_.registration_generation();
    parameter_bindings_.resize(tree_.expressions().size());
    return AssignParameters();
  }

  template <typename Output>
  void MoveCommon(Output& output) {
    static_cast<BoundExpressionState&>(output) =
        std::move(static_cast<BoundExpressionState&>(*impl_));
  }

  [[nodiscard]] BindExpected<BoundMutationTarget> ResolveMutationTarget(const QualifiedName& name) {
    BindExpected<DecodedNameParts> parts = NameParts(name);
    if (!parts.has_value()) {
      return std::unexpected(std::move(parts.error()));
    }
    if (parts->empty() || parts->size() > 2U ||
        (parts->size() == 2U && !NamesEqual(parts->front(), catalog_->schema_name()))) {
      return std::unexpected(BinderError(BindErrorCode::kNoSuchTable, name.span,
                                         "no such table: " + std::string{SpanText(name.span)}));
    }
    const std::string_view requested_name = parts->back();
    if (IsSchemaTableName(requested_name)) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, name.span,
                                         "sqlite_schema mutation is not supported"));
    }
    const std::optional<TableId> table_id = catalog_->FindTable(requested_name);
    if (!table_id.has_value()) {
      return std::unexpected(BinderError(BindErrorCode::kNoSuchTable, name.span,
                                         "no such table: " + std::string{requested_name}));
    }
    const CatalogTable& table = catalog_->table(*table_id);
    if (table.without_rowid || table.strict || table.autoincrement ||
        !table.check_constraints.empty()) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, name.span,
                                         "table shape is not supported for mutation"));
    }
    if (table.rowid_primary_key_conflict != ConflictAction::kDefault &&
        table.rowid_primary_key_conflict != ConflictAction::kAbort) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, name.span,
                                         "rowid conflict action is not supported"));
    }

    BoundMutationTarget target{
        .table = *table_id,
        .root_page = table.root_page,
        .span = name.span,
        .rowid_alias = table.rowid_alias,
    };
    target.columns.reserve(table.columns.size());
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
      const CatalogColumn& column = table.columns[index];
      if (column.effective_not_null_conflict.has_value() &&
          *column.effective_not_null_conflict != ConflictAction::kDefault &&
          *column.effective_not_null_conflict != ConflictAction::kAbort) {
        return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, name.span,
                                           "NOT NULL conflict action is not supported"));
      }
      if (column.default_expression.has_value() && column.missing_record_value == nullptr) {
        return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, name.span,
                                           "non-constant column defaults are not supported"));
      }
      target.columns.push_back(BoundMutationColumn{
          .column = ColumnId{index},
          .name = column.name,
          .declared_type = column.declared_type.has_value()
                               ? std::optional<std::string_view>{*column.declared_type}
                               : std::nullopt,
          .affinity = column.affinity,
          .not_null = column.effective_not_null_conflict.has_value(),
          .default_value = column.missing_record_value,
          .rowid_alias = table.rowid_alias == ColumnId{index},
      });
    }
    const std::span<const IndexId> table_indexes = catalog_->table_indexes(*table_id);
    target.indexes.reserve(table_indexes.size());
    for (const IndexId index_id : table_indexes) {
      const CatalogIndex& index = catalog_->index(index_id);
      const bool conflict_supported = !index.conflict_action.has_value() ||
                                      *index.conflict_action == ConflictAction::kDefault ||
                                      *index.conflict_action == ConflictAction::kAbort;
      if (index.partial_predicate.has_value() || index.key_term_count == 0U ||
          index.key_term_count > std::numeric_limits<std::uint32_t>::max() ||
          index.terms.size() != index.key_term_count + 1U || !conflict_supported ||
          !std::holds_alternative<RowIdIndexTerm>(index.terms.back().target)) {
        return std::unexpected(BinderError(BindErrorCode::kIndexedTableUnsupported, name.span,
                                           "index shape is not supported for mutation"));
      }
      BoundIndexMaintenance maintenance{
          .index = index_id,
          .root_page = index.root_page,
          .unique = index.unique,
          .unique_not_null = index.unique_not_null,
          .key_term_count = static_cast<std::uint32_t>(index.key_term_count),
      };
      maintenance.terms.reserve(index.terms.size());
      for (std::size_t term_index = 0; term_index < index.terms.size(); ++term_index) {
        const CatalogIndexTerm& term = index.terms[term_index];
        if (FindRegisteredCollation(term.collation_name) == nullptr) {
          return std::unexpected(BinderError(BindErrorCode::kNoSuchCollation, name.span,
                                             "no such collation sequence: " + term.collation_name));
        }
        BoundIndexTerm bound_term{
            .collation_name = term.collation_name,
            .order = term.order,
        };
        if (term_index < index.key_term_count) {
          const auto* column = std::get_if<ColumnId>(&term.target);
          if (column == nullptr || column->value >= table.columns.size()) {
            return std::unexpected(BinderError(BindErrorCode::kIndexedTableUnsupported, name.span,
                                               "index expression is not supported for mutation"));
          }
          if (target.rowid_alias == *column) {
            bound_term.rowid = true;
          } else {
            bound_term.column = *column;
          }
        } else {
          bound_term.rowid = true;
        }
        maintenance.terms.push_back(bound_term);
      }
      target.indexes.push_back(std::move(maintenance));
    }
    return target;
  }

  void PublishMutationSource(const BoundMutationTarget& target) {
    const CatalogTable& table = catalog_->table(target.table);
    source_state_ = SourceState{
        .name = table.name,
        .alias = std::nullopt,
        .table = target.table,
        .schema_table = false,
    };
    impl_->table_source = BoundTableSource{
        .kind = BoundSourceKind::kCatalogTable,
        .table = target.table,
        .span = target.span,
    };
    impl_->source_columns.reserve(target.columns.size());
    for (const BoundMutationColumn& column : target.columns) {
      impl_->source_columns.push_back(BoundSourceColumn{
          .name = column.name,
          .declared_type = column.declared_type,
          .affinity = column.affinity,
          .collation_name = catalog_->column(target.table, column.column).collation_name,
          .catalog_column = column.column,
      });
    }
  }

  [[nodiscard]] BindExpected<BoundMutationField> ResolveMutationField(
      const BoundMutationTarget& target, SourceSpan name_span) {
    BindExpected<std::string> name = Dequote(name_span);
    if (!name.has_value()) {
      return std::unexpected(std::move(name.error()));
    }
    const std::optional<ColumnId> column = catalog_->FindColumn(target.table, *name);
    if (column.has_value()) {
      return BoundMutationField{
          .column = *column,
          .rowid = target.rowid_alias == *column,
      };
    }
    if (IsRowIdName(*name)) {
      return BoundMutationField{
          .column = target.rowid_alias,
          .rowid = true,
      };
    }
    return std::unexpected(
        BinderError(BindErrorCode::kNoSuchColumn, name_span, "no such column: " + *name));
  }

  [[nodiscard]] static bool SameMutationField(const BoundMutationField& left,
                                              const BoundMutationField& right) noexcept {
    if (left.rowid || right.rowid) {
      return left.rowid && right.rowid;
    }
    return left.column == right.column;
  }

  static void MarkInsertEffectiveness(std::span<BoundInsertValue> values) noexcept {
    for (std::size_t current = 0; current < values.size(); ++current) {
      if (values[current].target.rowid) {
        for (std::size_t prior = 0; prior < current; ++prior) {
          if (SameMutationField(values[prior].target, values[current].target)) {
            values[prior].effective = false;
          }
        }
        continue;
      }
      for (std::size_t prior = 0; prior < current; ++prior) {
        if (SameMutationField(values[prior].target, values[current].target)) {
          values[current].effective = false;
          break;
        }
      }
    }
  }

  static void MarkUpdateEffectiveness(std::span<BoundUpdateAssignment> assignments) noexcept {
    for (std::size_t current = 0; current < assignments.size(); ++current) {
      for (std::size_t prior = 0; prior < current; ++prior) {
        if (SameMutationField(assignments[prior].target, assignments[current].target)) {
          assignments[prior].effective = false;
        }
      }
    }
  }

  [[nodiscard]] static bool HasSqlitePrefix(std::string_view name) noexcept {
    constexpr std::string_view prefix = "sqlite_";
    if (name.size() < prefix.size()) {
      return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      const auto byte = static_cast<std::uint8_t>(static_cast<unsigned char>(name[index]));
      if (SqliteToLower(byte) != static_cast<std::uint8_t>(prefix[index])) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static bool IsDefaultAbort(ConflictAction conflict) noexcept {
    return conflict == ConflictAction::kDefault || conflict == ConflictAction::kAbort;
  }

  [[nodiscard]] static BindError UnsupportedConflict(SourceSpan span) {
    return BinderError(BindErrorCode::kUnsupportedFeature, span,
                       "non-default conflict actions are not supported");
  }

  [[nodiscard]] BindExpected<SqlValue> MaterializeSyntaxConstant(ExpressionId id) {
    const Expression& expression = tree_.expression(id);
    if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
        parenthesized != nullptr) {
      return MaterializeSyntaxConstant(parenthesized->inner);
    }
    if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
        literal != nullptr) {
      const std::string_view token = SpanText(literal->token);
      if (literal->kind == LiteralKind::kString && !IsQuotedToken(token)) {
        if (!IsBareDefaultNameToken(token)) {
          return std::unexpected(
              BinderError(BindErrorCode::kInternalInvariant, literal->token,
                          "literal token does not match its AST kind: " + std::string{token}));
        }
      } else {
        BindExpected<void> validated = ValidateLiteralToken(*literal);
        if (!validated.has_value()) {
          return std::unexpected(std::move(validated.error()));
        }
      }
      switch (literal->kind) {
        case LiteralKind::kNull:
          return SqlValue{};
        case LiteralKind::kInteger:
          return ParseIntegerLiteral(literal->token);
        case LiteralKind::kReal:
          return SqlValue::Real(internal::ParseSqliteReal(StripNumericUnderscores(token)));
        case LiteralKind::kString: {
          BindExpected<std::string> value = Dequote(literal->token);
          if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
          }
          return SqlValue::Text(std::move(*value));
        }
        case LiteralKind::kBlob: {
          BindExpected<ByteBuffer> value = ParseBlobLiteral(literal->token);
          if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
          }
          return SqlValue::Blob(std::move(*value));
        }
        case LiteralKind::kTrue:
          return SqlValue::Integer(1);
        case LiteralKind::kFalse:
          return SqlValue::Integer(0);
        case LiteralKind::kCurrentDate:
        case LiteralKind::kCurrentTime:
        case LiteralKind::kCurrentTimestamp:
          return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, expression.span,
                                             "current-time defaults are not supported"));
      }
    }
    if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.payload);
        identifier != nullptr && identifier->name.parts.size() == 1U) {
      BindExpected<std::string> name = Dequote(identifier->name.parts.front());
      if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
      }
      if (NamesEqual(*name, "true")) {
        return SqlValue::Integer(1);
      }
      if (NamesEqual(*name, "false")) {
        return SqlValue::Integer(0);
      }
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload);
        unary != nullptr &&
        (unary->op == UnaryOperator::kPositive || unary->op == UnaryOperator::kNegative)) {
      if (unary->op == UnaryOperator::kNegative) {
        const Expression& operand_expression = tree_.expression(unary->operand);
        const auto* integer = std::get_if<LiteralExpression>(&operand_expression.payload);
        if (integer != nullptr && integer->kind == LiteralKind::kInteger &&
            IsSignedMinimumMagnitude(SpanText(integer->token))) {
          return SqlValue::Integer(std::numeric_limits<std::int64_t>::min());
        }
      }
      BindExpected<SqlValue> operand = MaterializeSyntaxConstant(unary->operand);
      if (!operand.has_value()) {
        return std::unexpected(std::move(operand.error()));
      }
      if (unary->op == UnaryOperator::kPositive) {
        return std::move(*operand);
      }
      if (operand->type() == SqlValueType::kInteger) {
        const std::int64_t value = operand->integer_value().value_or(0);
        if (value == std::numeric_limits<std::int64_t>::min()) {
          return SqlValue::Real(-static_cast<double>(value));
        }
        return SqlValue::Integer(-value);
      }
      if (operand->type() == SqlValueType::kReal) {
        return SqlValue::Real(-operand->real_value().value_or(0.0));
      }
    }
    return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, expression.span,
                                       "default value must be a constant literal"));
  }

  [[nodiscard]] BindExpected<std::shared_ptr<const SqlValue>> MaterializeConstantDefault(
      ExpressionId id, TypeAffinity affinity) {
    BindExpected<SqlValue> value = MaterializeSyntaxConstant(id);
    if (!value.has_value()) {
      return std::unexpected(std::move(value.error()));
    }
    return std::make_shared<const SqlValue>(ApplyAffinity(std::move(*value), affinity));
  }

  [[nodiscard]] bool LimitsAreRepresentable() const noexcept {
    constexpr std::size_t maximum = std::numeric_limits<std::uint32_t>::max();
    return options_.maximum_parameters <= maximum && options_.maximum_result_columns <= maximum &&
           options_.maximum_function_arguments <= maximum;
  }

  void ReserveOutputStorage(const SelectStatement& select) {
    const std::size_t syntax_count = tree_.expressions().size();
    const std::size_t wildcard_slack = impl_->source_columns.size();
    const std::size_t expression_capacity =
        syntax_count > std::numeric_limits<std::size_t>::max() - wildcard_slack
            ? syntax_count
            : syntax_count + wildcard_slack;
    const std::size_t result_capacity =
        select.result_columns.size() > std::numeric_limits<std::size_t>::max() - wildcard_slack
            ? select.result_columns.size()
            : select.result_columns.size() + wildcard_slack;
    impl_->expressions.reserve(expression_capacity);
    impl_->result_columns.reserve(std::min(options_.maximum_result_columns, result_capacity));
    impl_->collations.reserve(impl_->source_columns.size() ==
                                      std::numeric_limits<std::size_t>::max()
                                  ? impl_->source_columns.size()
                                  : impl_->source_columns.size() + 1U);
    impl_->functions.reserve(select.result_columns.size());
  }

  [[nodiscard]] bool SpanIsValid(SourceSpan span) const noexcept {
    return span.end().value() <= tree_.source().size_bytes();
  }

  [[nodiscard]] bool IdIsValid(ExpressionId id) const noexcept {
    return id.value < tree_.expressions().size();
  }

  [[nodiscard]] bool QualifiedNameIsValid(const QualifiedName& name) const noexcept {
    if (!SpanIsValid(name.span) || name.parts.empty()) {
      return false;
    }
    return std::ranges::all_of(name.parts, [this](SourceSpan part) { return SpanIsValid(part); });
  }

  [[nodiscard]] bool ValidateTree(const SelectStatement& select) const noexcept {
    if (!SpanIsValid(select.span) || select.result_columns.empty()) {
      return false;
    }
    for (const ResultColumn& result : select.result_columns) {
      if (!SpanIsValid(result.span) || !IdIsValid(result.expression) ||
          (result.alias.has_value() && !SpanIsValid(*result.alias))) {
        return false;
      }
    }
    if (select.from.has_value() &&
        (!SpanIsValid(select.from->span) || !QualifiedNameIsValid(select.from->name) ||
         (select.from->alias.has_value() && !SpanIsValid(*select.from->alias)))) {
      return false;
    }
    if (select.where.has_value() && !IdIsValid(*select.where)) {
      return false;
    }
    if (select.limit.has_value() &&
        (!SpanIsValid(select.limit->span) || !IdIsValid(select.limit->limit) ||
         (select.limit->offset.has_value() && !IdIsValid(*select.limit->offset)))) {
      return false;
    }
    for (const Expression& expression : tree_.expressions()) {
      if (!SpanIsValid(expression.span) || !ValidateExpression(expression)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool ValidateExpression(const Expression& expression) const noexcept {
    if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
        literal != nullptr) {
      return SpanIsValid(literal->token);
    }
    if (const auto* variable = std::get_if<VariableExpression>(&expression.payload);
        variable != nullptr) {
      return SpanIsValid(variable->token);
    }
    if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.payload);
        identifier != nullptr) {
      return QualifiedNameIsValid(identifier->name);
    }
    if (const auto* wildcard = std::get_if<WildcardExpression>(&expression.payload);
        wildcard != nullptr) {
      return SpanIsValid(wildcard->asterisk) &&
             (!wildcard->qualifier.has_value() || QualifiedNameIsValid(*wildcard->qualifier));
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload); unary != nullptr) {
      return SpanIsValid(unary->operator_span) && IdIsValid(unary->operand);
    }
    if (const auto* binary = std::get_if<BinaryExpression>(&expression.payload);
        binary != nullptr) {
      return SpanIsValid(binary->operator_span) && IdIsValid(binary->left) &&
             IdIsValid(binary->right);
    }
    if (const auto* call = std::get_if<FunctionCallExpression>(&expression.payload);
        call != nullptr) {
      return QualifiedNameIsValid(call->name) &&
             std::ranges::all_of(call->arguments,
                                 [this](ExpressionId id) { return IdIsValid(id); });
    }
    if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
        collate != nullptr) {
      return IdIsValid(collate->operand) && SpanIsValid(collate->keyword) &&
             SpanIsValid(collate->collation);
    }
    if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
        parenthesized != nullptr) {
      return IdIsValid(parenthesized->inner);
    }
    return false;
  }

  [[nodiscard]] std::string_view SpanText(SourceSpan span) const noexcept {
    const auto sliced = Slice(tree_.source(), span);
    return sliced.has_value() ? sliced->bytes() : std::string_view{};
  }

  [[nodiscard]] BindExpected<std::string> Dequote(SourceSpan span) const {
    std::expected<std::string, internal::DequoteSqlTokenError> dequoted =
        internal::DequoteSqlToken(SpanText(span));
    if (!dequoted.has_value()) {
      return std::unexpected(
          BinderError(BindErrorCode::kInternalInvariant, span, "unterminated quoted token"));
    }
    return std::move(*dequoted);
  }

  [[nodiscard]] BindExpected<DecodedNameParts> NameParts(const QualifiedName& name) const {
    if (name.parts.size() > DecodedNameParts{}.parts.size()) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, name.span,
                                         "qualified name has too many parts"));
    }
    DecodedNameParts result;
    for (const SourceSpan part : name.parts) {
      const std::string_view token = SpanText(part);
      DecodedNamePart& output = result.parts[result.count];
      if (IsQuotedToken(token)) {
        BindExpected<std::string> dequoted = Dequote(part);
        if (!dequoted.has_value()) {
          return std::unexpected(std::move(dequoted.error()));
        }
        output.owned = std::move(*dequoted);
      } else {
        output.borrowed = token;
      }
      ++result.count;
    }
    return result;
  }

  [[nodiscard]] BindExpected<void> AssignParameters() {
    std::vector<ParameterOccurrence> occurrences;
    for (std::size_t index = 0; index < tree_.expressions().size(); ++index) {
      const auto* variable = std::get_if<VariableExpression>(&tree_.expressions()[index].payload);
      if (variable != nullptr) {
        occurrences.push_back(ParameterOccurrence{
            .expression = ExpressionId{index},
            .span = variable->token,
        });
      }
    }
    std::ranges::sort(occurrences, {}, [](const ParameterOccurrence& occurrence) {
      return occurrence.span.begin().value();
    });

    std::unordered_map<std::string, std::size_t> named_slots;
    std::size_t maximum_slot = 0;
    for (const ParameterOccurrence& occurrence : occurrences) {
      const std::string_view token = SpanText(occurrence.span);
      std::size_t slot = 0;
      std::optional<std::string> name;
      if (token == "?") {
        if (maximum_slot >= options_.maximum_parameters) {
          return std::unexpected(BinderError(BindErrorCode::kParameterLimitExceeded,
                                             occurrence.span, "too many SQL variables"));
        }
        slot = maximum_slot + 1U;
      } else if (token.size() > 1U && token.front() == '?') {
        std::uint64_t parsed = 0;
        const auto conversion =
            std::from_chars(token.data() + 1, token.data() + token.size(), parsed, 10);
        if (conversion.ec != std::errc{} || conversion.ptr != token.data() + token.size() ||
            parsed == 0U || parsed > options_.maximum_parameters) {
          return std::unexpected(BinderError(BindErrorCode::kInvalidVariableNumber, occurrence.span,
                                             "variable number must be between ?1 and ?" +
                                                 std::to_string(options_.maximum_parameters)));
        }
        slot = static_cast<std::size_t>(parsed);
        name = std::string{token};
      } else {
        const auto existing = named_slots.find(std::string{token});
        if (existing != named_slots.end()) {
          slot = existing->second;
        } else {
          if (maximum_slot >= options_.maximum_parameters) {
            return std::unexpected(BinderError(BindErrorCode::kParameterLimitExceeded,
                                               occurrence.span, "too many SQL variables"));
          }
          slot = maximum_slot + 1U;
          named_slots.emplace(std::string{token}, slot);
        }
        name = std::string{token};
      }

      maximum_slot = std::max(maximum_slot, slot);
      if (impl_->parameters.size() < maximum_slot) {
        impl_->parameters.resize(maximum_slot);
      }
      if (name.has_value() && !impl_->parameters[slot - 1U].name.has_value()) {
        impl_->parameters[slot - 1U].name = std::move(*name);
      }
      parameter_bindings_[occurrence.expression.value] =
          BoundParameterId{static_cast<std::uint32_t>(slot - 1U)};
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> BindSource(const SelectStatement& select) {
    if (!select.from.has_value()) {
      return {};
    }
    BindExpected<DecodedNameParts> parts = NameParts(select.from->name);
    if (!parts.has_value()) {
      return std::unexpected(std::move(parts.error()));
    }
    if (parts->empty() || parts->size() > 2U ||
        (parts->size() == 2U && !NamesEqual((*parts)[0], catalog_->schema_name()))) {
      return NoSuchTable(select.from->name.span);
    }
    std::optional<std::string> alias;
    if (select.from->alias.has_value()) {
      BindExpected<std::string> bound_alias = Dequote(*select.from->alias);
      if (!bound_alias.has_value()) {
        return std::unexpected(std::move(bound_alias.error()));
      }
      alias = std::move(*bound_alias);
    }

    const std::string_view requested_name = parts->back();
    if (IsSchemaTableName(requested_name)) {
      source_state_ = SourceState{
          .name = "sqlite_schema",
          .alias = std::move(alias),
          .table = std::nullopt,
          .schema_table = true,
      };
      impl_->table_source = BoundTableSource{
          .kind = BoundSourceKind::kSchemaTable,
          .table = std::nullopt,
          .span = select.from->span,
      };
      impl_->source_columns.reserve(kSchemaColumns.size());
      for (const SchemaColumnDefinition& column : kSchemaColumns) {
        impl_->source_columns.push_back(BoundSourceColumn{
            .name = column.name,
            .declared_type = column.declared_type,
            .affinity = column.affinity,
            .collation_name = "BINARY",
            .catalog_column = std::nullopt,
        });
      }
      return {};
    }

    const std::optional<TableId> table_id = catalog_->FindTable(requested_name);
    if (!table_id.has_value()) {
      return NoSuchTable(select.from->name.span);
    }
    const CatalogTable& table = catalog_->table(*table_id);
    source_state_ = SourceState{
        .name = table.name,
        .alias = std::move(alias),
        .table = *table_id,
        .schema_table = false,
    };
    impl_->table_source = BoundTableSource{
        .kind = BoundSourceKind::kCatalogTable,
        .table = *table_id,
        .span = select.from->span,
    };
    impl_->source_columns.reserve(table.columns.size());
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
      const CatalogColumn& column = table.columns[index];
      impl_->source_columns.push_back(BoundSourceColumn{
          .name = column.name,
          .declared_type = column.declared_type.has_value()
                               ? std::optional<std::string_view>{*column.declared_type}
                               : std::nullopt,
          .affinity = column.affinity,
          .collation_name = column.collation_name,
          .catalog_column = ColumnId{index},
      });
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> NoSuchTable(SourceSpan span) const {
    return std::unexpected(BinderError(BindErrorCode::kNoSuchTable, span,
                                       "no such table: " + std::string{SpanText(span)}));
  }

  [[nodiscard]] BindExpected<void> BindResults(const SelectStatement& select) {
    for (const ResultColumn& result : select.result_columns) {
      const Expression& expression = tree_.expression(result.expression);
      if (const auto* wildcard = std::get_if<WildcardExpression>(&expression.payload);
          wildcard != nullptr) {
        BindExpected<void> expanded = ExpandWildcard(*wildcard, expression.span);
        if (!expanded.has_value()) {
          return std::unexpected(std::move(expanded.error()));
        }
        continue;
      }

      BindExpected<BoundExpressionId> bound = BindExpression(
          result.expression, BindScope{.source_columns = true, .result_aliases = false});
      if (!bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (impl_->result_columns.size() >= options_.maximum_result_columns) {
        return std::unexpected(BinderError(BindErrorCode::kResultColumnLimitExceeded, result.span,
                                           "too many columns in result set"));
      }

      BoundResultColumn output{
          .expression = *bound,
          .affinity = Properties(*bound).affinity,
      };
      PopulateImplicitResultMetadata(*bound, expression.span, output);
      if (result.alias.has_value()) {
        BindExpected<std::string> alias = Dequote(*result.alias);
        if (!alias.has_value()) {
          return std::unexpected(std::move(alias.error()));
        }
        output.name = *alias;
        aliases_.push_back(AliasEntry{.name = std::move(*alias), .target = *bound});
      }
      impl_->result_columns.push_back(std::move(output));
    }
    return {};
  }

  void PopulateImplicitResultMetadata(BoundExpressionId expression, SourceSpan span,
                                      BoundResultColumn& result) const {
    const BoundExpression& bound = impl_->expressions[expression.value()];
    if (const auto* column = std::get_if<BoundColumnExpression>(&bound.payload);
        column != nullptr) {
      const BoundSourceColumn& source = impl_->source_columns[column->column.value()];
      result.name = std::string{source.name};
      if (source.declared_type.has_value()) {
        result.declared_type = std::string{*source.declared_type};
      }
      return;
    }
    if (std::holds_alternative<BoundRowIdExpression>(bound.payload)) {
      if (impl_->table_source.has_value() && impl_->table_source->table.has_value()) {
        const CatalogTable& table = catalog_->table(*impl_->table_source->table);
        if (table.rowid_alias.has_value() &&
            table.rowid_alias->value < impl_->source_columns.size()) {
          const BoundSourceColumn& source = impl_->source_columns[table.rowid_alias->value];
          result.name = std::string{source.name};
          if (source.declared_type.has_value()) {
            result.declared_type = std::string{*source.declared_type};
          }
          return;
        }
      }
      result.name = "rowid";
      result.declared_type = "INTEGER";
      return;
    }
    result.name = std::string{SpanText(span)};
  }

  [[nodiscard]] BindExpected<void> ExpandWildcard(const WildcardExpression& wildcard,
                                                  SourceSpan span) {
    if (!source_state_.has_value()) {
      return std::unexpected(
          BinderError(BindErrorCode::kNoTablesSpecified, span, "no tables specified"));
    }
    if (wildcard.qualifier.has_value()) {
      BindExpected<DecodedNameParts> parts = NameParts(*wildcard.qualifier);
      if (!parts.has_value()) {
        return std::unexpected(std::move(parts.error()));
      }
      if (!WildcardQualifierMatches(*parts)) {
        const std::string detail = parts->empty() ? std::string{SpanText(wildcard.qualifier->span)}
                                                  : std::string{parts->back()};
        return std::unexpected(BinderError(BindErrorCode::kNoSuchTable, wildcard.qualifier->span,
                                           "no such table: " + detail));
      }
    }
    if (impl_->result_columns.size() > options_.maximum_result_columns ||
        impl_->source_columns.size() >
            options_.maximum_result_columns - impl_->result_columns.size()) {
      return std::unexpected(BinderError(BindErrorCode::kResultColumnLimitExceeded, span,
                                         "too many columns in result set"));
    }

    for (std::size_t index = 0; index < impl_->source_columns.size(); ++index) {
      const BoundSourceColumn& column = impl_->source_columns[index];
      BindExpected<std::optional<BoundCollationId>> collation =
          InternOptionalCollation(column.collation_name, span);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      BindExpected<BoundExpressionId> expression =
          AppendExpression(span,
                           BoundColumnExpression{
                               .column = BoundSourceColumnId{static_cast<std::uint32_t>(index)},
                           },
                           ExpressionProperties{
                               .affinity = column.affinity,
                               .collation = *collation,
                               .explicit_collation = false,
                           });
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      impl_->result_columns.push_back(BoundResultColumn{
          .expression = *expression,
          .name = std::string{column.name},
          .declared_type = column.declared_type.has_value()
                               ? std::optional<std::string>{std::string{*column.declared_type}}
                               : std::nullopt,
          .affinity = column.affinity,
      });
    }
    return {};
  }

  [[nodiscard]] bool WildcardQualifierMatches(const DecodedNameParts& parts) const noexcept {
    if (!source_state_.has_value() || parts.empty() || parts.size() > 2U) {
      return false;
    }
    if (parts.size() == 2U && !NamesEqual(parts.front(), catalog_->schema_name())) {
      return false;
    }
    const std::string_view qualifier = parts.back();
    if (source_state_->alias.has_value()) {
      return NamesEqual(qualifier, *source_state_->alias);
    }
    if (source_state_->schema_table) {
      return NamesEqual(qualifier, "sqlite_master");
    }
    return NamesEqual(qualifier, source_state_->name);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindExpression(ExpressionId id, BindScope scope) {
    const Expression& expression = tree_.expression(id);
    if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
        literal != nullptr) {
      return BindLiteral(*literal, expression.span, scope);
    }
    if (const auto* variable = std::get_if<VariableExpression>(&expression.payload);
        variable != nullptr) {
      return BindVariable(id, *variable, expression.span);
    }
    if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.payload);
        identifier != nullptr) {
      return BindIdentifier(*identifier, scope);
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload); unary != nullptr) {
      return BindUnary(*unary, expression.span, scope);
    }
    if (const auto* binary = std::get_if<BinaryExpression>(&expression.payload);
        binary != nullptr) {
      return BindBinary(*binary, expression.span, scope);
    }
    if (const auto* call = std::get_if<FunctionCallExpression>(&expression.payload);
        call != nullptr) {
      return BindCall(*call, expression.span, scope);
    }
    if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
        collate != nullptr) {
      return BindCollate(*collate, expression.span, scope);
    }
    if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
        parenthesized != nullptr) {
      return BindExpression(parenthesized->inner, scope);
    }
    return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, expression.span,
                                       "wildcard expressions are not supported here"));
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindLiteral(const LiteralExpression& literal,
                                                            SourceSpan span, BindScope scope) {
    BindExpected<void> validated = ValidateLiteralToken(literal);
    if (!validated.has_value()) {
      return std::unexpected(std::move(validated.error()));
    }
    const std::string_view token = SpanText(literal.token);
    if (literal.kind == LiteralKind::kTrue || literal.kind == LiteralKind::kFalse) {
      BindExpected<std::optional<BoundExpressionId>> resolved =
          ResolveSingleName(token, span, scope);
      if (!resolved.has_value()) {
        return std::unexpected(std::move(resolved.error()));
      }
      if (resolved->has_value()) {
        return **resolved;
      }
      return AppendExpression(
          span,
          BoundLiteralExpression{
              .value = SqlValue::Integer(literal.kind == LiteralKind::kTrue ? 1 : 0),
              .boolean_keyword = true,
          },
          ExpressionProperties{
              .affinity = TypeAffinity::kNone,
              .collation = std::nullopt,
              .explicit_collation = false,
              .truth_hint = literal.kind == LiteralKind::kTrue ? BoundTruthHint::kAlwaysTrue
                                                               : BoundTruthHint::kAlwaysFalse,
          });
    }
    if (literal.kind == LiteralKind::kString && !token.empty() && token.front() == '"') {
      BindExpected<std::string> name = Dequote(literal.token);
      if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
      }
      BindExpected<std::optional<BoundExpressionId>> resolved =
          ResolveSingleName(*name, span, scope);
      if (!resolved.has_value()) {
        return std::unexpected(std::move(resolved.error()));
      }
      if (resolved->has_value()) {
        return **resolved;
      }
      if (!options_.enable_double_quoted_strings) {
        return NoSuchColumn(span);
      }
      return AppendExpression(
          span, BoundLiteralExpression{.value = SqlValue::Text(std::move(*name))}, {});
    }

    BoundLiteralExpression bound;
    switch (literal.kind) {
      case LiteralKind::kNull:
        break;
      case LiteralKind::kInteger: {
        BindExpected<SqlValue> value = ParseIntegerLiteral(literal.token);
        if (!value.has_value()) {
          return std::unexpected(std::move(value.error()));
        }
        bound.value = std::move(*value);
        break;
      }
      case LiteralKind::kReal: {
        const std::string stripped = StripNumericUnderscores(token);
        bound.value = SqlValue::Real(internal::ParseSqliteReal(stripped));
        break;
      }
      case LiteralKind::kString: {
        BindExpected<std::string> value = Dequote(literal.token);
        if (!value.has_value()) {
          return std::unexpected(std::move(value.error()));
        }
        bound.value = SqlValue::Text(std::move(*value));
        break;
      }
      case LiteralKind::kBlob: {
        BindExpected<ByteBuffer> value = ParseBlobLiteral(literal.token);
        if (!value.has_value()) {
          return std::unexpected(std::move(value.error()));
        }
        bound.value = SqlValue::Blob(std::move(*value));
        break;
      }
      case LiteralKind::kCurrentDate:
      case LiteralKind::kCurrentTime:
      case LiteralKind::kCurrentTimestamp:
        return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, span,
                                           "current-time literals are not supported"));
      case LiteralKind::kTrue:
      case LiteralKind::kFalse:
        break;
    }
    ExpressionProperties properties;
    if (literal.kind == LiteralKind::kInteger && token.find('_') == std::string_view::npos) {
      const std::optional<std::int64_t> integer = bound.value.integer_value();
      if (integer.has_value() && *integer >= 0 &&
          *integer <= std::numeric_limits<std::int32_t>::max()) {
        properties.truth_hint =
            *integer == 0 ? BoundTruthHint::kAlwaysFalse : BoundTruthHint::kAlwaysTrue;
      }
    }
    return AppendExpression(span, std::move(bound), properties);
  }

  [[nodiscard]] BindExpected<void> ValidateLiteralToken(const LiteralExpression& literal) const {
    const std::string_view token = SpanText(literal.token);
    if (!LiteralTokenMatchesKind(literal.kind, token)) {
      return std::unexpected(
          BinderError(BindErrorCode::kInternalInvariant, literal.token,
                      "literal token does not match its AST kind: " + std::string{token}));
    }
    return {};
  }

  [[nodiscard]] BindExpected<SqlValue> ParseIntegerLiteral(SourceSpan span) const {
    const std::string stripped = StripNumericUnderscores(SpanText(span));
    if (stripped.size() > 2U && stripped.front() == '0' &&
        (stripped[1] == 'x' || stripped[1] == 'X')) {
      std::string_view digits{stripped};
      digits.remove_prefix(2);
      while (!digits.empty() && digits.front() == '0') {
        digits.remove_prefix(1);
      }
      if (digits.size() > 16U) {
        return std::unexpected(BinderError(BindErrorCode::kInvalidLiteral, span,
                                           "hex literal too big: " + std::string{SpanText(span)}));
      }
      std::uint64_t value = 0;
      for (const char byte : digits) {
        const std::optional<std::uint8_t> digit = HexDigit(byte);
        if (!digit.has_value()) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                             "malformed hexadecimal literal"));
        }
        value = (value << 4U) | *digit;
      }
      return SqlValue::Integer(static_cast<std::int64_t>(value));
    }

    std::uint64_t parsed = 0;
    const auto conversion =
        std::from_chars(stripped.data(), stripped.data() + stripped.size(), parsed, 10);
    if (conversion.ec == std::errc{} && conversion.ptr == stripped.data() + stripped.size() &&
        parsed <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return SqlValue::Integer(static_cast<std::int64_t>(parsed));
    }
    return SqlValue::Real(internal::ParseSqliteReal(stripped));
  }

  [[nodiscard]] BindExpected<ByteBuffer> ParseBlobLiteral(SourceSpan span) const {
    const std::string_view token = SpanText(span);
    if (token.size() < 3U || (token.front() != 'x' && token.front() != 'X') || token[1] != '\'' ||
        token.back() != '\'' || ((token.size() - 3U) % 2U) != 0U) {
      return std::unexpected(
          BinderError(BindErrorCode::kInternalInvariant, span, "malformed blob literal"));
    }
    ByteBuffer bytes{ByteCount{(token.size() - 3U) / 2U}};
    const MutableByteView output = bytes.mutable_view();
    for (std::size_t input = 2U, index = 0; input + 1U < token.size(); input += 2U, ++index) {
      const std::optional<std::uint8_t> high = HexDigit(token[input]);
      const std::optional<std::uint8_t> low = HexDigit(token[input + 1U]);
      if (!high.has_value() || !low.has_value()) {
        return std::unexpected(
            BinderError(BindErrorCode::kInternalInvariant, span, "malformed blob literal"));
      }
      const auto combined = static_cast<std::uint8_t>((static_cast<std::uint32_t>(*high) << 4U) |
                                                      static_cast<std::uint32_t>(*low));
      output[index] = static_cast<std::byte>(combined);
    }
    return bytes;
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindVariable(ExpressionId id,
                                                             const VariableExpression&,
                                                             SourceSpan span) {
    if (!parameter_bindings_[id.value].has_value()) {
      return std::unexpected(
          BinderError(BindErrorCode::kInternalInvariant, span, "parameter was not assigned"));
    }
    const std::optional<BoundParameterId>& parameter = parameter_bindings_[id.value];
    return AppendExpression(
        span, BoundParameterExpression{.parameter = parameter.value_or(BoundParameterId{0})}, {});
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindIdentifier(
      const IdentifierExpression& identifier, BindScope scope) {
    BindExpected<DecodedNameParts> parts = NameParts(identifier.name);
    if (!parts.has_value()) {
      return std::unexpected(std::move(parts.error()));
    }
    if (parts->size() == 1U) {
      BindExpected<std::optional<BoundExpressionId>> resolved =
          ResolveSingleName(parts->front(), identifier.name.span, scope);
      if (!resolved.has_value()) {
        return std::unexpected(std::move(resolved.error()));
      }
      if (resolved->has_value()) {
        return **resolved;
      }
      const std::string_view token = SpanText(identifier.name.parts.front());
      if (!IsQuotedToken(token) &&
          (NamesEqual(parts->front(), "true") || NamesEqual(parts->front(), "false"))) {
        return AppendExpression(
            identifier.name.span,
            BoundLiteralExpression{
                .value = SqlValue::Integer(NamesEqual(parts->front(), "true") ? 1 : 0),
                .boolean_keyword = true,
            },
            ExpressionProperties{
                .affinity = TypeAffinity::kNone,
                .collation = std::nullopt,
                .explicit_collation = false,
                .truth_hint = NamesEqual(parts->front(), "true") ? BoundTruthHint::kAlwaysTrue
                                                                 : BoundTruthHint::kAlwaysFalse,
            });
      }
      if (!token.empty() && token.front() == '"') {
        if (!options_.enable_double_quoted_strings) {
          return NoSuchColumn(identifier.name.span);
        }
        return AppendExpression(
            identifier.name.span,
            BoundLiteralExpression{.value = SqlValue::Text(std::string{parts->front()})}, {});
      }
      return NoSuchColumn(identifier.name.span);
    }
    if (parts->size() < 2U || parts->size() > 3U || !scope.source_columns ||
        !source_state_.has_value() || !QualifiedSourceMatches(*parts)) {
      return NoSuchColumn(identifier.name.span);
    }
    const std::string_view name = parts->back();
    if (const std::optional<BoundSourceColumnId> column = FindSourceColumn(name);
        column.has_value()) {
      return BindColumn(*column, identifier.name.span);
    }
    if (RowIdAvailable() && IsRowIdName(name)) {
      return BindRowId(identifier.name.span);
    }
    return NoSuchColumn(identifier.name.span);
  }

  [[nodiscard]] BindExpected<std::optional<BoundExpressionId>> ResolveSingleName(
      std::string_view name, SourceSpan span, BindScope scope) {
    if (scope.source_columns && source_state_.has_value()) {
      if (const std::optional<BoundSourceColumnId> column = FindSourceColumn(name);
          column.has_value()) {
        BindExpected<BoundExpressionId> expression = BindColumn(*column, span);
        if (!expression.has_value()) {
          return std::unexpected(std::move(expression.error()));
        }
        return std::optional<BoundExpressionId>{*expression};
      }
      if (RowIdAvailable() && IsRowIdName(name)) {
        BindExpected<BoundExpressionId> expression = BindRowId(span);
        if (!expression.has_value()) {
          return std::unexpected(std::move(expression.error()));
        }
        return std::optional<BoundExpressionId>{*expression};
      }
    }
    if (scope.result_aliases) {
      for (const AliasEntry& alias : aliases_) {
        if (NamesEqual(alias.name, name)) {
          const ExpressionProperties properties = Properties(alias.target);
          BindExpected<BoundExpressionId> expression = AppendExpression(
              span, BoundAliasReferenceExpression{.target = alias.target}, properties);
          if (!expression.has_value()) {
            return std::unexpected(std::move(expression.error()));
          }
          return std::optional<BoundExpressionId>{*expression};
        }
      }
    }
    return std::optional<BoundExpressionId>{};
  }

  [[nodiscard]] bool QualifiedSourceMatches(const DecodedNameParts& parts) const noexcept {
    if (!source_state_.has_value() || parts.size() < 2U || parts.size() > 3U) {
      return false;
    }
    if (parts.size() == 3U && !NamesEqual(parts.front(), catalog_->schema_name())) {
      return false;
    }
    const std::string_view qualifier = parts[parts.size() - 2U];
    if (source_state_->alias.has_value()) {
      return NamesEqual(qualifier, *source_state_->alias);
    }
    if (source_state_->schema_table) {
      return IsSchemaTableName(qualifier);
    }
    return NamesEqual(qualifier, source_state_->name);
  }

  [[nodiscard]] std::optional<BoundSourceColumnId> FindSourceColumn(
      std::string_view name) const noexcept {
    if (source_state_.has_value() && source_state_->table.has_value()) {
      const std::optional<ColumnId> column = catalog_->FindColumn(*source_state_->table, name);
      if (!column.has_value() || column->value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
      }
      return BoundSourceColumnId{static_cast<std::uint32_t>(column->value)};
    }
    for (std::size_t index = 0; index < impl_->source_columns.size(); ++index) {
      if (NamesEqual(impl_->source_columns[index].name, name)) {
        return BoundSourceColumnId{static_cast<std::uint32_t>(index)};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool RowIdAvailable() const noexcept {
    if (!impl_->table_source.has_value()) {
      return false;
    }
    if (impl_->table_source->kind == BoundSourceKind::kSchemaTable) {
      return true;
    }
    if (!impl_->table_source->table.has_value()) {
      return false;
    }
    const TableId table = impl_->table_source->table.value_or(TableId{});
    return !catalog_->table(table).without_rowid;
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindColumn(BoundSourceColumnId id,
                                                           SourceSpan span) {
    const BoundSourceColumn& column = impl_->source_columns[id.value()];
    BindExpected<std::optional<BoundCollationId>> collation =
        InternOptionalCollation(column.collation_name, span);
    if (!collation.has_value()) {
      return std::unexpected(std::move(collation.error()));
    }
    return AppendExpression(span, BoundColumnExpression{.column = id},
                            ExpressionProperties{
                                .affinity = column.affinity,
                                .collation = *collation,
                                .explicit_collation = false,
                            });
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindRowId(SourceSpan span) {
    return AppendExpression(span, BoundRowIdExpression{},
                            ExpressionProperties{
                                .affinity = TypeAffinity::kInteger,
                                .collation = std::nullopt,
                                .explicit_collation = false,
                            });
  }

  [[nodiscard]] BindExpected<BoundExpressionId> NoSuchColumn(SourceSpan span) const {
    return std::unexpected(BinderError(BindErrorCode::kNoSuchColumn, span,
                                       "no such column: " + std::string{SpanText(span)}));
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindUnary(const UnaryExpression& unary,
                                                          SourceSpan span, BindScope scope) {
    if (unary.op == UnaryOperator::kNegative) {
      if (const LiteralExpression* literal = UnaryNumericLiteral(unary.operand);
          literal != nullptr && literal->kind == LiteralKind::kInteger) {
        BindExpected<void> validated = ValidateLiteralToken(*literal);
        if (!validated.has_value()) {
          return std::unexpected(std::move(validated.error()));
        }
        const std::string stripped = StripNumericUnderscores(SpanText(literal->token));
        if (IsSignedMinimumMagnitude(SpanText(literal->token))) {
          return AppendExpression(
              span,
              BoundLiteralExpression{
                  .value = SqlValue::Integer(std::numeric_limits<std::int64_t>::min())},
              {});
        }
        if (stripped.size() > 2U && stripped.front() == '0' &&
            (stripped[1] == 'x' || stripped[1] == 'X')) {
          std::string_view digits{stripped};
          digits.remove_prefix(2);
          while (!digits.empty() && digits.front() == '0') {
            digits.remove_prefix(1);
          }
          if (digits.size() > 16U) {
            return std::unexpected(
                BinderError(BindErrorCode::kInvalidLiteral, span,
                            "hex literal too big: " + std::string{SpanText(span)}));
          }
        }
      }
    }

    BindExpected<BoundExpressionId> operand = BindExpression(unary.operand, scope);
    if (!operand.has_value()) {
      return std::unexpected(std::move(operand.error()));
    }
    ExpressionProperties properties;
    const ExpressionProperties child = Properties(*operand);
    if (unary.op == UnaryOperator::kPositive) {
      properties = child;
      properties.affinity = TypeAffinity::kNone;
      properties.truth_hint = BoundTruthHint::kNone;
    } else if (child.explicit_collation) {
      properties.collation = child.collation;
      properties.explicit_collation = true;
    }
    return AppendExpression(span,
                            BoundUnaryExpression{
                                .operation = ToBoundUnary(unary.op),
                                .operand = *operand,
                            },
                            properties);
  }

  [[nodiscard]] const LiteralExpression* UnaryNumericLiteral(ExpressionId id) const noexcept {
    while (true) {
      const Expression& expression = tree_.expression(id);
      if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
          literal != nullptr) {
        return literal;
      }
      if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
          parenthesized != nullptr) {
        id = parenthesized->inner;
        continue;
      }
      if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload);
          unary != nullptr && unary->op == UnaryOperator::kPositive) {
        id = unary->operand;
        continue;
      }
      return nullptr;
    }
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindBinary(const BinaryExpression& binary,
                                                           SourceSpan span, BindScope scope) {
    if (IsPatternOperation(binary.op)) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, span,
                                         "pattern operators are not supported"));
    }
    BindExpected<BoundExpressionId> left = BindExpression(binary.left, scope);
    if (!left.has_value()) {
      return std::unexpected(std::move(left.error()));
    }
    BindExpected<BoundExpressionId> right = BindExpression(binary.right, scope);
    if (!right.has_value()) {
      return std::unexpected(std::move(right.error()));
    }

    if (binary.op == BinaryOperator::kIs || binary.op == BinaryOperator::kIsNot) {
      if (const std::optional<bool> truth = TruthLiteral(*right); truth.has_value()) {
        return AppendExpression(
            span,
            BoundTruthTestExpression{
                .operand = *left,
                .expected = *truth ? BoundTruthValue::kTrue : BoundTruthValue::kFalse,
                .negated = binary.op == BinaryOperator::kIsNot,
            },
            PropagatedExplicitProperties(std::array{*left, *right}));
      }
      if (IsDirectNullLiteral(*right)) {
        BindExpected<BoundCollationId> collation = InternCollation("BINARY", span);
        if (!collation.has_value()) {
          return std::unexpected(std::move(collation.error()));
        }
        ExpressionProperties properties = PropagatedExplicitProperties(std::array{*left, *right});
        if (IsParserFoldedNullTestOperand(binary.left)) {
          properties.truth_hint = binary.op == BinaryOperator::kIs ? BoundTruthHint::kAlwaysFalse
                                                                   : BoundTruthHint::kAlwaysTrue;
        }
        return AppendExpression(span,
                                BoundComparisonExpression{
                                    .comparison = ToSqlComparison(binary.op),
                                    .affinity = TypeAffinity::kNone,
                                    .collation = *collation,
                                    .left = *left,
                                    .right = *right,
                                },
                                properties);
      }
    }
    if (IsComparisonOperation(binary.op)) {
      BindExpected<BoundCollationId> collation =
          SelectComparisonCollation(std::array{*left, *right}, span);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      return AppendExpression(span,
                              BoundComparisonExpression{
                                  .comparison = ToSqlComparison(binary.op),
                                  .affinity = SelectComparisonAffinity(Properties(*left).affinity,
                                                                       Properties(*right).affinity),
                                  .collation = *collation,
                                  .left = *left,
                                  .right = *right,
                              },
                              PropagatedExplicitProperties(std::array{*left, *right}));
    }
    return AppendExpression(span,
                            BoundBinaryExpression{
                                .operation = ToBoundBinary(binary.op),
                                .left = *left,
                                .right = *right,
                            },
                            PropagatedExplicitProperties(std::array{*left, *right}));
  }

  [[nodiscard]] std::optional<bool> TruthLiteral(BoundExpressionId id) const noexcept {
    const BoundExpression& expression = impl_->expressions[id.value()];
    if (const auto* literal = std::get_if<BoundLiteralExpression>(&expression.payload);
        literal != nullptr && literal->boolean_keyword) {
      return literal->value.integer_value().value_or(0) != 0;
    }
    if (const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
        collate != nullptr) {
      return TruthLiteral(collate->operand);
    }
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      return TruthLiteral(alias->target);
    }
    return std::nullopt;
  }

  [[nodiscard]] bool IsDirectNullLiteral(BoundExpressionId id) const noexcept {
    const BoundExpression& expression = impl_->expressions[id.value()];
    const auto* literal = std::get_if<BoundLiteralExpression>(&expression.payload);
    return literal != nullptr && literal->value.type() == SqlValueType::kNull;
  }

  [[nodiscard]] bool IsParserFoldedNullTestOperand(ExpressionId id) const noexcept {
    const Expression& expression = tree_.expression(id);
    if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
        parenthesized != nullptr) {
      return IsParserFoldedNullTestOperand(parenthesized->inner);
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload);
        unary != nullptr &&
        (unary->op == UnaryOperator::kPositive || unary->op == UnaryOperator::kNegative)) {
      return IsParserFoldedNullTestOperand(unary->operand);
    }
    const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
    return literal != nullptr &&
           (literal->kind == LiteralKind::kInteger || literal->kind == LiteralKind::kReal ||
            literal->kind == LiteralKind::kString || literal->kind == LiteralKind::kBlob);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindCall(const FunctionCallExpression& call,
                                                         SourceSpan span, BindScope scope) {
    BindExpected<DecodedNameParts> parts = NameParts(call.name);
    if (!parts.has_value()) {
      return std::unexpected(std::move(parts.error()));
    }
    if (parts->size() != 1U) {
      return std::unexpected(
          BinderError(BindErrorCode::kNoSuchFunction, call.name.span,
                      "no such function: " + std::string{SpanText(call.name.span)}));
    }
    const std::string_view name = parts->front();
    if (call.arguments.size() > options_.maximum_function_arguments) {
      return std::unexpected(BinderError(BindErrorCode::kFunctionArgumentLimitExceeded, span,
                                         "too many arguments on function " + std::string{name}));
    }

    const bool has_wildcard = std::ranges::any_of(call.arguments, [this](ExpressionId id) {
      return std::holds_alternative<WildcardExpression>(tree_.expression(id).payload);
    });
    const Result<const ScalarFunction*> resolved =
        environment_.functions().Resolve(name, call.arguments.size());
    if (resolved.has_value()) {
      if (has_wildcard) {
        return UnsupportedAggregate(span);
      }
      BindExpected<std::vector<BoundExpressionId>> arguments = BindArguments(call.arguments, scope);
      if (!arguments.has_value()) {
        return std::unexpected(std::move(arguments.error()));
      }
      const ExpressionProperties properties = PropagatedExplicitProperties(*arguments);
      BindExpected<BoundFunctionId> function = InternFunction(**resolved, span);
      if (!function.has_value()) {
        return std::unexpected(std::move(function.error()));
      }
      BindExpected<BoundCollationId> collation = (*resolved)->uses_collation()
                                                     ? SelectFunctionCollation(*arguments, span)
                                                     : InternCollation("BINARY", span);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      return AppendExpression(span,
                              BoundScalarCallExpression{
                                  .function = *function,
                                  .collation = *collation,
                                  .arguments = std::move(*arguments),
                              },
                              properties);
    }

    if (NamesEqual(name, "coalesce") || NamesEqual(name, "ifnull")) {
      const bool valid =
          NamesEqual(name, "ifnull") ? call.arguments.size() == 2U : call.arguments.size() >= 2U;
      if (!valid) {
        return WrongArity(call.name.span, name);
      }
      BindExpected<std::vector<BoundExpressionId>> arguments = BindArguments(call.arguments, scope);
      if (!arguments.has_value()) {
        return std::unexpected(std::move(arguments.error()));
      }
      const ExpressionProperties properties = PropagatedExplicitProperties(*arguments);
      return AppendExpression(span, BoundCoalesceExpression{.arguments = std::move(*arguments)},
                              properties);
    }
    if (NamesEqual(name, "iif") || NamesEqual(name, "if")) {
      if (call.arguments.size() < 2U) {
        return WrongArity(call.name.span, name);
      }
      BindExpected<std::vector<BoundExpressionId>> arguments = BindArguments(call.arguments, scope);
      if (!arguments.has_value()) {
        return std::unexpected(std::move(arguments.error()));
      }
      const ExpressionProperties properties = PropagatedExplicitProperties(*arguments);
      return AppendExpression(span, BoundConditionalExpression{.arguments = std::move(*arguments)},
                              properties);
    }
    if (NamesEqual(name, "likely") || NamesEqual(name, "unlikely")) {
      if (call.arguments.size() != 1U) {
        return WrongArity(call.name.span, name);
      }
      BindExpected<BoundExpressionId> operand = BindExpression(call.arguments.front(), scope);
      if (!operand.has_value()) {
        return std::unexpected(std::move(operand.error()));
      }
      return AppendExpression(span,
                              BoundLikelihoodExpression{
                                  .operand = *operand,
                                  .probability = NamesEqual(name, "likely") ? 0.9375 : 0.0625,
                              },
                              PropagatedExplicitProperties(std::array{*operand}));
    }
    if (NamesEqual(name, "likelihood")) {
      if (call.arguments.size() != 2U) {
        return WrongArity(call.name.span, name);
      }
      const Expression& probability_expression =
          ParenthesesTransparentExpression(call.arguments[1]);
      const auto* probability = std::get_if<LiteralExpression>(&probability_expression.payload);
      if (probability == nullptr || probability->kind != LiteralKind::kReal) {
        return InvalidLikelihoodProbability(tree_.expression(call.arguments[1]).span);
      }
      BindExpected<void> validated = ValidateLiteralToken(*probability);
      if (!validated.has_value()) {
        return std::unexpected(std::move(validated.error()));
      }
      const double parsed =
          internal::ParseSqliteReal(StripNumericUnderscores(SpanText(probability->token)));
      if (parsed < 0.0 || parsed > 1.0) {
        return InvalidLikelihoodProbability(probability_expression.span);
      }
      BindExpected<BoundExpressionId> operand = BindExpression(call.arguments.front(), scope);
      if (!operand.has_value()) {
        return std::unexpected(std::move(operand.error()));
      }
      return AppendExpression(span,
                              BoundLikelihoodExpression{
                                  .operand = *operand,
                                  .probability = parsed,
                              },
                              PropagatedExplicitProperties(std::array{*operand}));
    }

    if (has_wildcard || IsKnownAggregateName(name) ||
        ((NamesEqual(name, "min") || NamesEqual(name, "max")) && call.arguments.size() == 1U)) {
      return UnsupportedAggregate(span);
    }
    if (FunctionNameExists(name)) {
      return WrongArity(call.name.span, name);
    }
    return std::unexpected(BinderError(BindErrorCode::kNoSuchFunction, call.name.span,
                                       "no such function: " + std::string{name}));
  }

  [[nodiscard]] bool FunctionNameExists(std::string_view name) const noexcept {
    return std::ranges::any_of(
        environment_.functions().functions(),
        [name](const ScalarFunction& function) { return NamesEqual(function.name(), name); });
  }

  [[nodiscard]] const Expression& ParenthesesTransparentExpression(ExpressionId id) const noexcept {
    while (true) {
      const Expression& expression = tree_.expression(id);
      const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
      if (parenthesized == nullptr) {
        return expression;
      }
      id = parenthesized->inner;
    }
  }

  [[nodiscard]] BindExpected<BoundExpressionId> UnsupportedAggregate(SourceSpan span) const {
    return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, span,
                                       "aggregate functions are not supported"));
  }

  [[nodiscard]] BindExpected<BoundExpressionId> InvalidLikelihoodProbability(
      SourceSpan span) const {
    return std::unexpected(
        BinderError(BindErrorCode::kInvalidLiteral, span,
                    "second argument to likelihood() must be a constant between 0.0 and 1.0"));
  }

  [[nodiscard]] BindExpected<BoundExpressionId> WrongArity(SourceSpan span,
                                                           std::string_view name) const {
    return std::unexpected(
        BinderError(BindErrorCode::kWrongFunctionArity, span,
                    "wrong number of arguments to function " + std::string{name} + "()"));
  }

  [[nodiscard]] BindExpected<std::vector<BoundExpressionId>> BindArguments(
      std::span<const ExpressionId> arguments, BindScope scope) {
    std::vector<BoundExpressionId> bound;
    bound.reserve(arguments.size());
    for (const ExpressionId argument : arguments) {
      BindExpected<BoundExpressionId> expression = BindExpression(argument, scope);
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      bound.push_back(*expression);
    }
    return bound;
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindCollate(const CollateExpression& collate,
                                                            SourceSpan span, BindScope scope) {
    BindExpected<BoundExpressionId> operand = BindExpression(collate.operand, scope);
    if (!operand.has_value()) {
      return std::unexpected(std::move(operand.error()));
    }
    BindExpected<std::string> name = Dequote(collate.collation);
    if (!name.has_value()) {
      return std::unexpected(std::move(name.error()));
    }
    BindExpected<BoundCollationId> id = InternCollation(*name, collate.collation);
    if (!id.has_value()) {
      return std::unexpected(std::move(id.error()));
    }
    ExpressionProperties properties = Properties(*operand);
    properties.collation = *id;
    properties.explicit_collation = true;
    properties.truth_hint = BoundTruthHint::kNone;
    return AppendExpression(span,
                            BoundCollateExpression{
                                .operand = *operand,
                                .collation = *id,
                            },
                            properties);
  }

  template <typename Payload>
  [[nodiscard]] BindExpected<BoundExpressionId> AppendExpression(SourceSpan span, Payload payload,
                                                                 ExpressionProperties properties) {
    if (impl_->expressions.size() >= std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          BinderError(BindErrorCode::kInternalInvariant, span, "bound expression ID overflow"));
    }
    const BoundExpressionId id{static_cast<std::uint32_t>(impl_->expressions.size())};
    impl_->expressions.push_back(BoundExpression{
        .span = span,
        .properties =
            BoundExpressionProperties{
                .affinity = properties.affinity,
                .collation = properties.collation,
                .has_explicit_collation = properties.explicit_collation,
                .truth_hint = properties.truth_hint,
            },
        .payload = std::move(payload),
    });
    return id;
  }

  [[nodiscard]] ExpressionProperties Properties(BoundExpressionId id) const noexcept {
    const BoundExpressionProperties& properties = impl_->expressions[id.value()].properties;
    return ExpressionProperties{
        .affinity = properties.affinity,
        .collation = properties.collation,
        .explicit_collation = properties.has_explicit_collation,
        .truth_hint = properties.truth_hint,
    };
  }

  [[nodiscard]] ExpressionProperties PropagatedExplicitProperties(
      std::span<const BoundExpressionId> expressions) const noexcept {
    for (const BoundExpressionId expression : expressions) {
      const ExpressionProperties properties = Properties(expression);
      if (properties.explicit_collation) {
        return ExpressionProperties{
            .affinity = TypeAffinity::kNone,
            .collation = properties.collation,
            .explicit_collation = true,
        };
      }
    }
    return {};
  }

  template <std::size_t Size>
  [[nodiscard]] ExpressionProperties PropagatedExplicitProperties(
      const std::array<BoundExpressionId, Size>& expressions) const noexcept {
    return PropagatedExplicitProperties(
        std::span<const BoundExpressionId>{expressions.data(), expressions.size()});
  }

  [[nodiscard]] BindExpected<std::optional<BoundCollationId>> InternOptionalCollation(
      std::string_view name, SourceSpan span) {
    if (name.empty()) {
      return std::optional<BoundCollationId>{};
    }
    BindExpected<BoundCollationId> id = InternCollation(name, span);
    if (!id.has_value()) {
      return std::unexpected(std::move(id.error()));
    }
    return std::optional<BoundCollationId>{*id};
  }

  [[nodiscard]] BindExpected<BoundCollationId> InternCollation(std::string_view name,
                                                               SourceSpan span) {
    for (std::size_t index = 0; index < impl_->collations.size(); ++index) {
      if (NamesEqual(impl_->collations[index].name, name)) {
        return BoundCollationId{static_cast<std::uint32_t>(index)};
      }
    }
    if (impl_->collations.size() >= std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          BinderError(BindErrorCode::kInternalInvariant, span, "bound collation ID overflow"));
    }
    const BoundCollationId id{static_cast<std::uint32_t>(impl_->collations.size())};
    impl_->collations.push_back(BoundCollation{.name = std::string{name}});
    return id;
  }

  [[nodiscard]] BindExpected<BoundFunctionId> InternFunction(const ScalarFunction& function,
                                                             SourceSpan span) {
    for (std::size_t index = 0; index < impl_->functions.size(); ++index) {
      const BoundScalarFunction& existing = impl_->functions[index];
      if (NamesEqual(existing.name, function.name()) &&
          existing.deterministic == function.is_deterministic() &&
          existing.uses_collation == function.uses_collation()) {
        return BoundFunctionId{static_cast<std::uint32_t>(index)};
      }
    }
    if (impl_->functions.size() >= std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          BinderError(BindErrorCode::kInternalInvariant, span, "bound function ID overflow"));
    }
    const BoundFunctionId id{static_cast<std::uint32_t>(impl_->functions.size())};
    impl_->functions.push_back(BoundScalarFunction{
        .name = std::string{function.name()},
        .deterministic = function.is_deterministic(),
        .uses_collation = function.uses_collation(),
    });
    return id;
  }

  [[nodiscard]] const Collation* FindRegisteredCollation(std::string_view name) const noexcept {
    for (const Collation* collation : environment_.collations()) {
      if (collation != nullptr && NamesEqual(collation->name(), name)) {
        return collation;
      }
    }
    return nullptr;
  }

  [[nodiscard]] BindExpected<void> RequireCollation(BoundCollationId id, SourceSpan span) const {
    const std::string& name = impl_->collations[id.value()].name;
    if (FindRegisteredCollation(name) == nullptr) {
      return std::unexpected(BinderError(BindErrorCode::kNoSuchCollation, span,
                                         "no such collation sequence: " + name));
    }
    return {};
  }

  [[nodiscard]] BindExpected<BoundCollationId> SelectComparisonCollation(
      const std::array<BoundExpressionId, 2>& operands, SourceSpan span) {
    const std::array<ExpressionProperties, 2> properties{
        Properties(operands[0]),
        Properties(operands[1]),
    };
    std::optional<BoundCollationId> selected;
    for (const ExpressionProperties& candidate : properties) {
      if (candidate.explicit_collation) {
        selected = candidate.collation;
        break;
      }
    }
    if (!selected.has_value()) {
      for (const ExpressionProperties& candidate : properties) {
        if (candidate.collation.has_value()) {
          selected = candidate.collation;
          break;
        }
      }
    }
    if (!selected.has_value()) {
      BindExpected<BoundCollationId> binary = InternCollation("BINARY", span);
      if (!binary.has_value()) {
        return std::unexpected(std::move(binary.error()));
      }
      selected = *binary;
    }
    BindExpected<void> required = RequireCollation(*selected, span);
    if (!required.has_value()) {
      return std::unexpected(std::move(required.error()));
    }
    return *selected;
  }

  [[nodiscard]] BindExpected<BoundCollationId> SelectFunctionCollation(
      std::span<const BoundExpressionId> arguments, SourceSpan span) {
    for (const BoundExpressionId argument : arguments) {
      const ExpressionProperties properties = Properties(argument);
      if (properties.collation.has_value()) {
        BindExpected<void> required = RequireCollation(*properties.collation, span);
        if (!required.has_value()) {
          return std::unexpected(std::move(required.error()));
        }
        return *properties.collation;
      }
    }
    return InternCollation("BINARY", span);
  }

  SyntaxTree tree_;
  CatalogSnapshotPtr catalog_;
  BindEnvironment environment_;
  BindOptions options_;
  std::unique_ptr<BoundSelect::Impl> impl_;
  std::optional<SourceState> source_state_;
  std::vector<AliasEntry> aliases_;
  std::vector<std::optional<BoundParameterId>> parameter_bindings_;
};

}  // namespace binder_detail

BoundExpressionKind BoundExpressionKindOf(const BoundExpression& expression) noexcept {
  return static_cast<BoundExpressionKind>(expression.payload.index());
}

std::string_view BoundExpressionKindName(BoundExpressionKind kind) noexcept {
  switch (kind) {
    case BoundExpressionKind::kLiteral:
      return "literal";
    case BoundExpressionKind::kColumn:
      return "column";
    case BoundExpressionKind::kRowId:
      return "rowid";
    case BoundExpressionKind::kParameter:
      return "parameter";
    case BoundExpressionKind::kUnary:
      return "unary";
    case BoundExpressionKind::kBinary:
      return "binary";
    case BoundExpressionKind::kComparison:
      return "comparison";
    case BoundExpressionKind::kTruthTest:
      return "truth_test";
    case BoundExpressionKind::kAliasReference:
      return "alias_reference";
    case BoundExpressionKind::kScalarCall:
      return "scalar_call";
    case BoundExpressionKind::kCoalesce:
      return "coalesce";
    case BoundExpressionKind::kConditional:
      return "conditional";
    case BoundExpressionKind::kLikelihood:
      return "likelihood";
    case BoundExpressionKind::kCollate:
      return "collate";
  }
  return "unknown";
}

std::string_view BindErrorCodeName(BindErrorCode code) noexcept {
  switch (code) {
    case BindErrorCode::kInvalidInput:
      return "invalid_input";
    case BindErrorCode::kUnsupportedFeature:
      return "unsupported_feature";
    case BindErrorCode::kNoSuchTable:
      return "no_such_table";
    case BindErrorCode::kNoSuchColumn:
      return "no_such_column";
    case BindErrorCode::kAmbiguousColumn:
      return "ambiguous_column";
    case BindErrorCode::kNoTablesSpecified:
      return "no_tables_specified";
    case BindErrorCode::kNoSuchFunction:
      return "no_such_function";
    case BindErrorCode::kWrongFunctionArity:
      return "wrong_function_arity";
    case BindErrorCode::kNoSuchCollation:
      return "no_such_collation";
    case BindErrorCode::kInvalidLiteral:
      return "invalid_literal";
    case BindErrorCode::kInvalidVariableNumber:
      return "invalid_variable_number";
    case BindErrorCode::kParameterLimitExceeded:
      return "parameter_limit_exceeded";
    case BindErrorCode::kFunctionArgumentLimitExceeded:
      return "function_argument_limit_exceeded";
    case BindErrorCode::kResultColumnLimitExceeded:
      return "result_column_limit_exceeded";
    case BindErrorCode::kColumnCountMismatch:
      return "column_count_mismatch";
    case BindErrorCode::kDuplicateColumn:
      return "duplicate_column";
    case BindErrorCode::kTableAlreadyExists:
      return "table_already_exists";
    case BindErrorCode::kIndexAlreadyExists:
      return "index_already_exists";
    case BindErrorCode::kObjectNameReserved:
      return "object_name_reserved";
    case BindErrorCode::kIndexedTableUnsupported:
      return "indexed_table_unsupported";
    case BindErrorCode::kInternalInvariant:
      return "internal_invariant";
  }
  return "unknown";
}

ErrorCode BindError::base_error_code() const noexcept {
  switch (code) {
    case BindErrorCode::kInvalidInput:
      return ErrorCode::kMisuse;
    case BindErrorCode::kParameterLimitExceeded:
    case BindErrorCode::kFunctionArgumentLimitExceeded:
    case BindErrorCode::kResultColumnLimitExceeded:
      return ErrorCode::kTooLarge;
    case BindErrorCode::kIndexedTableUnsupported:
      return ErrorCode::kProtocol;
    case BindErrorCode::kInternalInvariant:
      return ErrorCode::kInternal;
    case BindErrorCode::kUnsupportedFeature:
    case BindErrorCode::kNoSuchTable:
    case BindErrorCode::kNoSuchColumn:
    case BindErrorCode::kAmbiguousColumn:
    case BindErrorCode::kNoTablesSpecified:
    case BindErrorCode::kNoSuchFunction:
    case BindErrorCode::kWrongFunctionArity:
    case BindErrorCode::kNoSuchCollation:
    case BindErrorCode::kInvalidLiteral:
    case BindErrorCode::kInvalidVariableNumber:
    case BindErrorCode::kColumnCountMismatch:
    case BindErrorCode::kDuplicateColumn:
    case BindErrorCode::kTableAlreadyExists:
    case BindErrorCode::kIndexAlreadyExists:
    case BindErrorCode::kObjectNameReserved:
      return ErrorCode::kGeneric;
  }
  return ErrorCode::kInternal;
}

BindEnvironment BindEnvironment::Core() noexcept {
  static const std::array<const Collation*, 3> collations{
      &BinaryCollation(),
      &NoCaseCollation(),
      &RTrimCollation(),
  };
  return BindEnvironment{CoreFunctionRegistry(), collations, 0};
}

BoundSelect::BoundSelect(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

BoundSelect::BoundSelect(BoundSelect&&) noexcept = default;

BoundSelect& BoundSelect::operator=(BoundSelect&&) noexcept = default;

BoundSelect::~BoundSelect() = default;

bool BoundSelect::valid() const noexcept { return impl_ != nullptr; }

Utf8View BoundSelect::source() const noexcept {
  return impl_ != nullptr ? Utf8View{impl_->source} : Utf8View{};
}

const CatalogSnapshot* BoundSelect::catalog() const noexcept {
  return impl_ != nullptr ? impl_->catalog.get() : nullptr;
}

std::uint64_t BoundSelect::registration_generation() const noexcept {
  return impl_ != nullptr ? impl_->registration_generation : 0;
}

const BoundTableSource* BoundSelect::table_source() const noexcept {
  return impl_ != nullptr && impl_->table_source.has_value() ? &*impl_->table_source : nullptr;
}

std::span<const BoundSourceColumn> BoundSelect::source_columns() const noexcept {
  return impl_ != nullptr ? std::span<const BoundSourceColumn>{impl_->source_columns}
                          : std::span<const BoundSourceColumn>{};
}

std::span<const std::string> BoundSelect::registered_collations() const noexcept {
  return impl_ != nullptr ? std::span<const std::string>{impl_->registered_collations}
                          : std::span<const std::string>{};
}

std::span<const BoundCollation> BoundSelect::collations() const noexcept {
  return impl_ != nullptr ? std::span<const BoundCollation>{impl_->collations}
                          : std::span<const BoundCollation>{};
}

std::span<const BoundScalarFunction> BoundSelect::functions() const noexcept {
  return impl_ != nullptr ? std::span<const BoundScalarFunction>{impl_->functions}
                          : std::span<const BoundScalarFunction>{};
}

std::span<const BoundParameter> BoundSelect::parameters() const noexcept {
  return impl_ != nullptr ? std::span<const BoundParameter>{impl_->parameters}
                          : std::span<const BoundParameter>{};
}

std::span<const BoundExpression> BoundSelect::expressions() const noexcept {
  return impl_ != nullptr ? std::span<const BoundExpression>{impl_->expressions}
                          : std::span<const BoundExpression>{};
}

const BoundExpression& BoundSelect::expression(BoundExpressionId id) const noexcept {
  return impl_->expressions[id.value()];
}

std::span<const BoundResultColumn> BoundSelect::result_columns() const noexcept {
  return impl_ != nullptr ? std::span<const BoundResultColumn>{impl_->result_columns}
                          : std::span<const BoundResultColumn>{};
}

std::optional<BoundExpressionId> BoundSelect::where_expression() const noexcept {
  return impl_ != nullptr ? impl_->where : std::nullopt;
}

const BoundLimit* BoundSelect::limit() const noexcept {
  return impl_ != nullptr && impl_->limit.has_value() ? &*impl_->limit : nullptr;
}

BindSelectResult BindSelectStatement(SyntaxTree tree, CatalogSnapshotPtr catalog,
                                     BindEnvironment environment, BindOptions options) {
  return binder_detail::StatementBinder(std::move(tree), std::move(catalog), environment, options)
      .RunSelect();
}

BoundInsert::BoundInsert(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

BoundInsert::BoundInsert(BoundInsert&&) noexcept = default;

BoundInsert& BoundInsert::operator=(BoundInsert&&) noexcept = default;

BoundInsert::~BoundInsert() = default;

bool BoundInsert::valid() const noexcept { return impl_ != nullptr; }

Utf8View BoundInsert::source() const noexcept {
  return impl_ != nullptr ? Utf8View{impl_->source} : Utf8View{};
}

const CatalogSnapshot* BoundInsert::catalog() const noexcept {
  return impl_ != nullptr ? impl_->catalog.get() : nullptr;
}

CatalogVersion BoundInsert::required_catalog_version() const noexcept {
  return impl_ != nullptr && impl_->catalog != nullptr ? impl_->catalog->version()
                                                       : CatalogVersion{};
}

std::uint64_t BoundInsert::registration_generation() const noexcept {
  return impl_ != nullptr ? impl_->registration_generation : 0;
}

const BoundMutationTarget& BoundInsert::target() const noexcept { return impl_->target; }

std::span<const BoundParameter> BoundInsert::parameters() const noexcept {
  return impl_ != nullptr ? std::span<const BoundParameter>{impl_->parameters}
                          : std::span<const BoundParameter>{};
}

std::span<const BoundCollation> BoundInsert::collations() const noexcept {
  return impl_ != nullptr ? std::span<const BoundCollation>{impl_->collations}
                          : std::span<const BoundCollation>{};
}

std::span<const BoundScalarFunction> BoundInsert::functions() const noexcept {
  return impl_ != nullptr ? std::span<const BoundScalarFunction>{impl_->functions}
                          : std::span<const BoundScalarFunction>{};
}

std::span<const BoundExpression> BoundInsert::expressions() const noexcept {
  return impl_ != nullptr ? std::span<const BoundExpression>{impl_->expressions}
                          : std::span<const BoundExpression>{};
}

const BoundExpression& BoundInsert::expression(BoundExpressionId id) const noexcept {
  return impl_->expressions[id.value()];
}

std::span<const BoundInsertValue> BoundInsert::values() const noexcept {
  return impl_ != nullptr ? std::span<const BoundInsertValue>{impl_->values}
                          : std::span<const BoundInsertValue>{};
}

bool BoundInsert::default_values() const noexcept {
  return impl_ != nullptr && impl_->default_values;
}

bool BoundInsert::explicit_columns() const noexcept {
  return impl_ != nullptr && impl_->explicit_columns;
}

BoundUpdate::BoundUpdate(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

BoundUpdate::BoundUpdate(BoundUpdate&&) noexcept = default;

BoundUpdate& BoundUpdate::operator=(BoundUpdate&&) noexcept = default;

BoundUpdate::~BoundUpdate() = default;

bool BoundUpdate::valid() const noexcept { return impl_ != nullptr; }

Utf8View BoundUpdate::source() const noexcept {
  return impl_ != nullptr ? Utf8View{impl_->source} : Utf8View{};
}

const CatalogSnapshot* BoundUpdate::catalog() const noexcept {
  return impl_ != nullptr ? impl_->catalog.get() : nullptr;
}

CatalogVersion BoundUpdate::required_catalog_version() const noexcept {
  return impl_ != nullptr && impl_->catalog != nullptr ? impl_->catalog->version()
                                                       : CatalogVersion{};
}

std::uint64_t BoundUpdate::registration_generation() const noexcept {
  return impl_ != nullptr ? impl_->registration_generation : 0;
}

const BoundMutationTarget& BoundUpdate::target() const noexcept { return impl_->target; }

std::span<const BoundSourceColumn> BoundUpdate::source_columns() const noexcept {
  return impl_ != nullptr ? std::span<const BoundSourceColumn>{impl_->source_columns}
                          : std::span<const BoundSourceColumn>{};
}

std::span<const BoundParameter> BoundUpdate::parameters() const noexcept {
  return impl_ != nullptr ? std::span<const BoundParameter>{impl_->parameters}
                          : std::span<const BoundParameter>{};
}

std::span<const BoundCollation> BoundUpdate::collations() const noexcept {
  return impl_ != nullptr ? std::span<const BoundCollation>{impl_->collations}
                          : std::span<const BoundCollation>{};
}

std::span<const BoundScalarFunction> BoundUpdate::functions() const noexcept {
  return impl_ != nullptr ? std::span<const BoundScalarFunction>{impl_->functions}
                          : std::span<const BoundScalarFunction>{};
}

std::span<const BoundExpression> BoundUpdate::expressions() const noexcept {
  return impl_ != nullptr ? std::span<const BoundExpression>{impl_->expressions}
                          : std::span<const BoundExpression>{};
}

const BoundExpression& BoundUpdate::expression(BoundExpressionId id) const noexcept {
  return impl_->expressions[id.value()];
}

std::span<const BoundUpdateAssignment> BoundUpdate::assignments() const noexcept {
  return impl_ != nullptr ? std::span<const BoundUpdateAssignment>{impl_->assignments}
                          : std::span<const BoundUpdateAssignment>{};
}

std::optional<BoundExpressionId> BoundUpdate::where_expression() const noexcept {
  return impl_ != nullptr ? impl_->where : std::nullopt;
}

bool BoundUpdate::changes_rowid() const noexcept {
  return impl_ != nullptr && impl_->changes_rowid;
}

BoundDelete::BoundDelete(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

BoundDelete::BoundDelete(BoundDelete&&) noexcept = default;

BoundDelete& BoundDelete::operator=(BoundDelete&&) noexcept = default;

BoundDelete::~BoundDelete() = default;

bool BoundDelete::valid() const noexcept { return impl_ != nullptr; }

Utf8View BoundDelete::source() const noexcept {
  return impl_ != nullptr ? Utf8View{impl_->source} : Utf8View{};
}

const CatalogSnapshot* BoundDelete::catalog() const noexcept {
  return impl_ != nullptr ? impl_->catalog.get() : nullptr;
}

CatalogVersion BoundDelete::required_catalog_version() const noexcept {
  return impl_ != nullptr && impl_->catalog != nullptr ? impl_->catalog->version()
                                                       : CatalogVersion{};
}

std::uint64_t BoundDelete::registration_generation() const noexcept {
  return impl_ != nullptr ? impl_->registration_generation : 0;
}

const BoundMutationTarget& BoundDelete::target() const noexcept { return impl_->target; }

std::span<const BoundSourceColumn> BoundDelete::source_columns() const noexcept {
  return impl_ != nullptr ? std::span<const BoundSourceColumn>{impl_->source_columns}
                          : std::span<const BoundSourceColumn>{};
}

std::span<const BoundParameter> BoundDelete::parameters() const noexcept {
  return impl_ != nullptr ? std::span<const BoundParameter>{impl_->parameters}
                          : std::span<const BoundParameter>{};
}

std::span<const BoundCollation> BoundDelete::collations() const noexcept {
  return impl_ != nullptr ? std::span<const BoundCollation>{impl_->collations}
                          : std::span<const BoundCollation>{};
}

std::span<const BoundScalarFunction> BoundDelete::functions() const noexcept {
  return impl_ != nullptr ? std::span<const BoundScalarFunction>{impl_->functions}
                          : std::span<const BoundScalarFunction>{};
}

std::span<const BoundExpression> BoundDelete::expressions() const noexcept {
  return impl_ != nullptr ? std::span<const BoundExpression>{impl_->expressions}
                          : std::span<const BoundExpression>{};
}

const BoundExpression& BoundDelete::expression(BoundExpressionId id) const noexcept {
  return impl_->expressions[id.value()];
}

std::optional<BoundExpressionId> BoundDelete::where_expression() const noexcept {
  return impl_ != nullptr ? impl_->where : std::nullopt;
}

BoundCreateTable::BoundCreateTable(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

BoundCreateTable::BoundCreateTable(BoundCreateTable&&) noexcept = default;

BoundCreateTable& BoundCreateTable::operator=(BoundCreateTable&&) noexcept = default;

BoundCreateTable::~BoundCreateTable() = default;

bool BoundCreateTable::valid() const noexcept { return impl_ != nullptr; }

Utf8View BoundCreateTable::source() const noexcept {
  return impl_ != nullptr ? Utf8View{impl_->source} : Utf8View{};
}

const CatalogSnapshot* BoundCreateTable::catalog() const noexcept {
  return impl_ != nullptr ? impl_->catalog.get() : nullptr;
}

CatalogVersion BoundCreateTable::required_catalog_version() const noexcept {
  return impl_ != nullptr && impl_->catalog != nullptr ? impl_->catalog->version()
                                                       : CatalogVersion{};
}

std::string_view BoundCreateTable::table_name() const noexcept {
  return impl_ != nullptr ? std::string_view{impl_->table_name} : std::string_view{};
}

bool BoundCreateTable::if_not_exists() const noexcept {
  return impl_ != nullptr && impl_->if_not_exists;
}

bool BoundCreateTable::no_op() const noexcept { return impl_ != nullptr && impl_->no_op; }

std::string_view BoundCreateTable::canonical_sql() const noexcept {
  return impl_ != nullptr ? std::string_view{impl_->canonical_sql} : std::string_view{};
}

std::span<const BoundCreateColumn> BoundCreateTable::columns() const noexcept {
  return impl_ != nullptr ? std::span<const BoundCreateColumn>{impl_->columns}
                          : std::span<const BoundCreateColumn>{};
}

std::optional<ColumnId> BoundCreateTable::rowid_alias() const noexcept {
  return impl_ != nullptr ? impl_->rowid_alias : std::nullopt;
}

BoundCreateIndex::BoundCreateIndex(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

BoundCreateIndex::BoundCreateIndex(BoundCreateIndex&&) noexcept = default;

BoundCreateIndex& BoundCreateIndex::operator=(BoundCreateIndex&&) noexcept = default;

BoundCreateIndex::~BoundCreateIndex() = default;

bool BoundCreateIndex::valid() const noexcept { return impl_ != nullptr; }

Utf8View BoundCreateIndex::source() const noexcept {
  return impl_ != nullptr ? Utf8View{impl_->source} : Utf8View{};
}

const CatalogSnapshot* BoundCreateIndex::catalog() const noexcept {
  return impl_ != nullptr ? impl_->catalog.get() : nullptr;
}

CatalogVersion BoundCreateIndex::required_catalog_version() const noexcept {
  return impl_ != nullptr && impl_->catalog != nullptr ? impl_->catalog->version()
                                                       : CatalogVersion{};
}

TableId BoundCreateIndex::table() const noexcept {
  return impl_ != nullptr ? impl_->table : TableId{};
}

RootPageId BoundCreateIndex::table_root_page() const noexcept {
  return impl_ != nullptr ? impl_->table_root_page : RootPageId{};
}

std::string_view BoundCreateIndex::index_name() const noexcept {
  return impl_ != nullptr ? std::string_view{impl_->index_name} : std::string_view{};
}

std::string_view BoundCreateIndex::table_name() const noexcept {
  return impl_ != nullptr ? std::string_view{impl_->table_name} : std::string_view{};
}

bool BoundCreateIndex::unique() const noexcept { return impl_ != nullptr && impl_->unique; }

bool BoundCreateIndex::unique_not_null() const noexcept {
  return impl_ != nullptr && impl_->unique_not_null;
}

bool BoundCreateIndex::if_not_exists() const noexcept {
  return impl_ != nullptr && impl_->if_not_exists;
}

bool BoundCreateIndex::no_op() const noexcept { return impl_ != nullptr && impl_->no_op; }

std::string_view BoundCreateIndex::canonical_sql() const noexcept {
  return impl_ != nullptr ? std::string_view{impl_->canonical_sql} : std::string_view{};
}

std::span<const BoundCreateIndexTerm> BoundCreateIndex::terms() const noexcept {
  return impl_ != nullptr ? std::span<const BoundCreateIndexTerm>{impl_->terms}
                          : std::span<const BoundCreateIndexTerm>{};
}

BindStatementResult BindStatement(SyntaxTree tree, CatalogSnapshotPtr catalog,
                                  BindEnvironment environment, BindOptions options) {
  return binder_detail::StatementBinder(std::move(tree), std::move(catalog), environment, options)
      .RunStatement();
}

}  // namespace modern_sqlite
