#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
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
  bool contains_aggregate = false;
};

struct AliasEntry {
  std::string name;
  BoundExpressionId target;
  std::size_t result_index = 0;
};

struct ParameterOccurrence {
  ExpressionId expression;
  SourceSpan span;
};

struct BindScope {
  bool source_columns = true;
  bool result_aliases = false;
  bool allow_aggregates = false;
};

struct SourceState {
  BoundSourceId id{0};
  std::string name;
  std::optional<std::string> alias;
  std::optional<TableId> table;
  bool schema_table = false;
  std::size_t source_column_begin = 0;
  std::size_t source_column_count = 0;
};

struct SourceQualification {
  std::optional<std::string_view> schema{};
  std::string_view name;
};

struct CoreBindingMetadata {
  std::vector<SourceState> source_states{};
  std::size_t source_column_begin = 0;
  std::size_t source_column_count = 0;
  std::vector<std::optional<ExpressionId>> result_syntax{};
};

enum class AggregateOccurrencePhase : std::uint8_t {
  kResult,
  kOrderBy,
  kHaving,
};

struct PendingAggregateInvocation {
  BoundExpressionId expression;
  BoundAggregateInvocation invocation;
  AggregateOccurrencePhase phase = AggregateOccurrencePhase::kResult;
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

[[nodiscard]] BoundAggregateExtremumKind ToBoundExtremum(AggregateExtremumKind extremum) noexcept {
  switch (extremum) {
    case AggregateExtremumKind::kNone:
      return BoundAggregateExtremumKind::kNone;
    case AggregateExtremumKind::kMinimum:
      return BoundAggregateExtremumKind::kMinimum;
    case AggregateExtremumKind::kMaximum:
      return BoundAggregateExtremumKind::kMaximum;
  }
  return BoundAggregateExtremumKind::kNone;
}

}  // namespace

struct BoundExpressionState {
  std::string source;
  CatalogSnapshotPtr catalog;
  std::uint64_t registration_generation = 0;
  std::vector<BoundTableSource> table_sources;
  std::vector<BoundSourceColumn> source_columns;
  std::vector<BoundCollation> collations;
  std::vector<BoundScalarFunction> functions;
  std::vector<BoundParameter> parameters;
  std::vector<BoundExpression> expressions;
};

struct BoundSelect::Impl final : BoundExpressionState {
  std::vector<std::string> registered_collations;
  std::vector<BoundAggregateFunction> aggregate_functions;
  std::vector<BoundAggregateInvocation> aggregate_invocations;
  std::vector<BoundQueryCore> query_cores;
  std::vector<CompoundOperator> compound_operators;
  std::vector<BoundCollationId> compound_collations;
  std::vector<BoundResultColumn> result_columns;
  std::vector<BoundResultColumn> output_columns;
  std::vector<std::optional<ExpressionId>> result_syntax;
  std::optional<BoundExpressionId> where;
  std::vector<BoundOrderingTerm> order_by;
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

struct BoundAnalyze::Impl final {
  std::string source;
  CatalogSnapshotPtr catalog;
  BoundAnalyzeScope scope = BoundAnalyzeScope::kDatabase;
  std::string scope_name;
  std::optional<RootPageId> stat1_root_page;
  std::vector<BoundAnalyzeIndex> indexes;
  std::vector<BoundAnalyzeTable> tables;
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
    BindExpected<void> syntax_widths = ValidateSyntacticCompoundWidths(select);
    if (!syntax_widths.has_value()) {
      return std::unexpected(std::move(syntax_widths.error()));
    }
    BindExpected<void> initialized = InitializeCommon();
    if (!initialized.has_value()) {
      return std::unexpected(std::move(initialized.error()));
    }
    impl_->query_cores.reserve(select.compounds.size() + 1U);
    impl_->compound_operators.reserve(select.compounds.size());
    core_metadata_.reserve(select.compounds.size() + 1U);

    const bool simple_select =
        select.compounds.empty() && std::holds_alternative<SelectCore>(select.first);
    if (simple_select) {
      const auto& core = std::get<SelectCore>(select.first);
      BindExpected<void> bound = BindSelectCoreBody(core);
      if (!bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      BindExpected<void> order_by = BindOrderBy(select);
      if (!order_by.has_value()) {
        return std::unexpected(std::move(order_by.error()));
      }
      BindExpected<void> group_by = BindGroupBy(core);
      if (!group_by.has_value()) {
        return std::unexpected(std::move(group_by.error()));
      }
      if (core.quantifier == SelectQuantifier::kDistinct) {
        BindExpected<void> distinct = RequireResultCollations(core.span, impl_->result_columns);
        if (!distinct.has_value()) {
          return std::unexpected(std::move(distinct.error()));
        }
      }
      BindExpected<void> finished = FinishSelectQueryCore(core.quantifier);
      if (!finished.has_value()) {
        return std::unexpected(std::move(finished.error()));
      }
    } else {
      BindExpected<void> first = BindQueryCore(select.first);
      if (!first.has_value()) {
        return std::unexpected(std::move(first.error()));
      }
      for (const CompoundTerm& term : select.compounds) {
        impl_->compound_operators.push_back(term.operation);
        BindExpected<void> core = BindQueryCore(term.core);
        if (!core.has_value()) {
          return std::unexpected(std::move(core.error()));
        }
      }
      if (!select.compounds.empty()) {
        BindExpected<void> widths = ValidateCompoundWidths(select);
        if (!widths.has_value()) {
          return std::unexpected(std::move(widths.error()));
        }
        BindExpected<void> collations = BindCompoundCollations(select.span);
        if (!collations.has_value()) {
          return std::unexpected(std::move(collations.error()));
        }
        BindExpected<void> order_by = BindCompoundOrderBy(select);
        if (!order_by.has_value()) {
          return std::unexpected(std::move(order_by.error()));
        }
        BindExpected<void> distinct = RequireCompoundDistinctCollations(select);
        if (!distinct.has_value()) {
          return std::unexpected(std::move(distinct.error()));
        }
        BindExpected<void> set_collations = RequireSetCompoundCollations(select.span);
        if (!set_collations.has_value()) {
          return std::unexpected(std::move(set_collations.error()));
        }
      }
    }
    BindExpected<void> limit = BindLimit(select.limit);
    if (!limit.has_value()) {
      return std::unexpected(std::move(limit.error()));
    }
    PublishResultColumns();
    std::vector<BoundTableSource>{}.swap(impl_->table_sources);
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
    if (std::holds_alternative<AnalyzeStatement>(tree_.statement())) {
      BindExpected<BoundAnalyze> analyze = RunAnalyze();
      if (!analyze.has_value()) {
        return std::unexpected(std::move(analyze.error()));
      }
      return BoundStatement{std::in_place_type<BoundAnalyze>, std::move(*analyze)};
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

  [[nodiscard]] BindExpected<BoundAnalyze> RunAnalyze() {
    const auto& analyze = std::get<AnalyzeStatement>(tree_.statement());
    BindExpected<void> environment = ValidateEnvironment();
    if (!environment.has_value()) {
      return std::unexpected(std::move(environment.error()));
    }

    auto output = std::make_unique<BoundAnalyze::Impl>();
    output->source.assign(tree_.source().bytes());
    output->catalog = catalog_;
    if (const std::optional<TableId> stat1 = catalog_->FindTable("sqlite_stat1");
        stat1.has_value()) {
      const CatalogTable& table = catalog_->table(*stat1);
      if (table.without_rowid || table.root_page.value == 0U) {
        return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, analyze.span,
                                           "sqlite_stat1 has an unsupported table shape"));
      }
      output->stat1_root_page = table.root_page;
    }

    std::optional<TableId> selected_table;
    std::optional<IndexId> selected_index;
    if (analyze.target.has_value()) {
      BindExpected<DecodedNameParts> parts = NameParts(*analyze.target);
      if (!parts.has_value()) {
        return std::unexpected(std::move(parts.error()));
      }
      if (parts->empty() || parts->size() > 2U ||
          (parts->size() == 2U && !NamesEqual(parts->front(), catalog_->schema_name()))) {
        return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, analyze.target->span,
                                           "only the main schema can be analyzed"));
      }
      if (parts->size() == 1U && NamesEqual(parts->front(), catalog_->schema_name())) {
        output->scope = BoundAnalyzeScope::kDatabase;
      } else {
        const std::string_view object_name = parts->back();
        selected_index = catalog_->FindIndex(object_name);
        if (selected_index.has_value()) {
          output->scope = BoundAnalyzeScope::kIndex;
          output->scope_name = catalog_->index(*selected_index).name;
        } else {
          selected_table = catalog_->FindTable(object_name);
          if (!selected_table.has_value() ||
              HasSqlitePrefix(catalog_->table(*selected_table).name)) {
            return std::unexpected(
                BinderError(BindErrorCode::kNoSuchTable, analyze.target->span,
                            "no such table or index: " + std::string{object_name}));
          }
          output->scope = BoundAnalyzeScope::kTable;
          output->scope_name = catalog_->table(*selected_table).name;
        }
      }
    }

    const auto append_index = [&](IndexId index_id) -> BindExpected<void> {
      const CatalogIndex& index = catalog_->index(index_id);
      const CatalogTable& table = catalog_->table(index.table);
      if (index.root_page.value == 0U || index.key_term_count == 0U ||
          index.key_term_count > std::numeric_limits<std::uint32_t>::max() ||
          index.terms.size() < index.key_term_count ||
          index.terms.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, analyze.span,
                                           "catalog index metadata is invalid for ANALYZE"));
      }
      BoundAnalyzeIndex bound{
          .root_page = index.root_page,
          .table_name = table.name,
          .index_name = table.without_rowid && index.origin == IndexOrigin::kPrimaryKey
                            ? table.name
                            : index.name,
          .key_term_count = static_cast<std::uint32_t>(index.key_term_count),
          .columns = {},
          .partial = index.partial_predicate.has_value(),
      };
      bound.columns.reserve(index.terms.size());
      for (const CatalogIndexTerm& term : index.terms) {
        if (FindRegisteredCollation(term.collation_name) == nullptr) {
          return std::unexpected(BinderError(BindErrorCode::kNoSuchCollation, analyze.span,
                                             "no such collation sequence: " + term.collation_name));
        }
        bound.columns.push_back(BoundAnalyzeIndexColumn{
            .collation_name = term.collation_name,
            .order = term.order,
        });
      }
      output->indexes.push_back(std::move(bound));
      return {};
    };

    if (selected_index.has_value()) {
      if (auto appended = append_index(*selected_index); !appended.has_value()) {
        return std::unexpected(std::move(appended.error()));
      }
      return BoundAnalyze(std::move(output));
    }

    const auto append_table = [&](TableId table_id) -> BindExpected<void> {
      const CatalogTable& table = catalog_->table(table_id);
      if (HasSqlitePrefix(table.name)) {
        return {};
      }
      bool has_nonpartial_index = false;
      for (const IndexId index_id : catalog_->table_indexes(table_id)) {
        const CatalogIndex& index = catalog_->index(index_id);
        has_nonpartial_index = has_nonpartial_index || !index.partial_predicate.has_value();
        if (auto appended = append_index(index_id); !appended.has_value()) {
          return appended;
        }
      }
      if (!has_nonpartial_index && !table.without_rowid) {
        if (table.columns.empty() ||
            table.columns.size() > std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, analyze.span,
                                             "catalog table metadata is invalid for ANALYZE"));
        }
        output->tables.push_back(BoundAnalyzeTable{
            .root_page = table.root_page,
            .table_name = table.name,
            .record_field_count = static_cast<std::uint32_t>(table.columns.size()),
        });
      }
      return {};
    };

    if (selected_table.has_value()) {
      if (auto appended = append_table(*selected_table); !appended.has_value()) {
        return std::unexpected(std::move(appended.error()));
      }
      return BoundAnalyze(std::move(output));
    }

    for (std::size_t table = 0; table < catalog_->tables().size(); ++table) {
      if (auto appended = append_table(TableId{table}); !appended.has_value()) {
        return std::unexpected(std::move(appended.error()));
      }
    }
    return BoundAnalyze(std::move(output));
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
    source_states_.push_back(SourceState{
        .name = table.name,
        .alias = std::nullopt,
        .table = target.table,
        .schema_table = false,
        .source_column_count = target.columns.size(),
    });
    impl_->table_sources.push_back(BoundTableSource{
        .kind = BoundSourceKind::kCatalogTable,
        .table = target.table,
        .span = target.span,
        .source_column_count = target.columns.size(),
    });
    impl_->source_columns.reserve(target.columns.size());
    for (const BoundMutationColumn& column : target.columns) {
      impl_->source_columns.push_back(BoundSourceColumn{
          .source = impl_->table_sources.front().id,
          .rowid_alias = target.rowid_alias == column.column,
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
           options_.maximum_function_arguments <= maximum &&
           options_.maximum_aggregate_terms <= maximum;
  }

  void BeginQueryCore() {
    impl_->table_sources.clear();
    impl_->result_columns.clear();
    impl_->result_syntax.clear();
    impl_->where.reset();
    source_states_.clear();
    aliases_.clear();
    active_source_begin_ = impl_->source_columns.size();
    active_source_count_ = 0;
    pending_aggregates_.clear();
    active_aggregate_phase_ = AggregateOccurrencePhase::kResult;
    active_core_is_aggregate_ = false;
    active_group_by_.clear();
    active_having_.reset();
  }

  void RegisterActiveSourceCollations() {
    if (!impl_->registered_collations.empty() ||
        !std::ranges::any_of(impl_->table_sources, [this](const BoundTableSource& source) {
          return source.table.has_value() && !catalog_->table_indexes(*source.table).empty();
        })) {
      return;
    }
    impl_->registered_collations.reserve(environment_.collations().size());
    for (const Collation* collation : environment_.collations()) {
      impl_->registered_collations.emplace_back(collation->name());
    }
  }

  [[nodiscard]] bool IsActiveRowIdAlias(BoundSourceColumnId column) const noexcept {
    const std::size_t index = column.value();
    if (index < active_source_begin_ || index - active_source_begin_ >= active_source_count_) {
      return false;
    }
    const BoundSourceColumn& source = impl_->source_columns[index];
    return source.rowid_alias;
  }

  [[nodiscard]] std::optional<BoundSourceLeafIdentity> AggregateIdentity(
      BoundExpressionId id) const noexcept {
    return BoundSourceLeafIdentityOf(impl_->expressions[id.value()], impl_->source_columns);
  }

  [[nodiscard]] bool AggregateInvocationsEquivalent(const BoundAggregateInvocation& left,
                                                    const BoundAggregateInvocation& right) const {
    if (left.function != right.function || left.collation != right.collation ||
        left.distinct != right.distinct || left.arguments.size() != right.arguments.size() ||
        left.order_by.size() != right.order_by.size() ||
        left.filter.has_value() != right.filter.has_value()) {
      return false;
    }
    for (std::size_t index = 0; index < left.arguments.size(); ++index) {
      if (!BoundSemanticallyEquivalent(left.arguments[index], right.arguments[index])) {
        return false;
      }
    }
    for (std::size_t index = 0; index < left.order_by.size(); ++index) {
      const BoundOrderingTerm& left_term = left.order_by[index];
      const BoundOrderingTerm& right_term = right.order_by[index];
      if (left_term.collation != right_term.collation || left_term.order != right_term.order ||
          left_term.null_placement != right_term.null_placement ||
          !BoundSemanticallyEquivalent(left_term.expression, right_term.expression)) {
        return false;
      }
    }
    return !left.filter.has_value() || BoundSemanticallyEquivalent(*left.filter, *right.filter);
  }

  [[nodiscard]] BoundExpressionId StripTopCollation(BoundExpressionId id) const noexcept {
    const BoundExpression& expression = impl_->expressions[id.value()];
    const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
    return collate != nullptr ? collate->operand : id;
  }

  [[nodiscard]] bool MatchesBinaryGroupTerm(BoundExpressionId id) const {
    const BoundExpressionId comparable = StripTopCollation(id);
    for (const BoundGroupingTerm& term : active_group_by_) {
      const std::string& collation = impl_->collations[term.collation.value()].name;
      if (NamesEqual(collation, "BINARY") &&
          BoundSemanticallyEquivalent(comparable, StripTopCollation(term.expression))) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool IsConstantOrBinaryGroupTerm(BoundExpressionId id) const {
    if (MatchesBinaryGroupTerm(id)) {
      return true;
    }
    const BoundExpression& expression = impl_->expressions[id.value()];
    if (std::holds_alternative<BoundLiteralExpression>(expression.payload) ||
        std::holds_alternative<BoundParameterExpression>(expression.payload)) {
      return true;
    }
    if (std::holds_alternative<BoundColumnExpression>(expression.payload) ||
        std::holds_alternative<BoundRowIdExpression>(expression.payload) ||
        std::holds_alternative<BoundAggregateCallExpression>(expression.payload)) {
      return false;
    }
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      return IsConstantOrBinaryGroupTerm(alias->target);
    }
    if (const auto* unary = std::get_if<BoundUnaryExpression>(&expression.payload);
        unary != nullptr) {
      return IsConstantOrBinaryGroupTerm(unary->operand);
    }
    if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
        binary != nullptr) {
      return IsConstantOrBinaryGroupTerm(binary->left) &&
             IsConstantOrBinaryGroupTerm(binary->right);
    }
    if (const auto* comparison = std::get_if<BoundComparisonExpression>(&expression.payload);
        comparison != nullptr) {
      return IsConstantOrBinaryGroupTerm(comparison->left) &&
             IsConstantOrBinaryGroupTerm(comparison->right);
    }
    if (const auto* truth = std::get_if<BoundTruthTestExpression>(&expression.payload);
        truth != nullptr) {
      return IsConstantOrBinaryGroupTerm(truth->operand);
    }
    if (const auto* call = std::get_if<BoundScalarCallExpression>(&expression.payload);
        call != nullptr) {
      const BoundScalarFunction& function = impl_->functions[call->function.value()];
      return function.deterministic &&
             std::ranges::all_of(call->arguments, [this](BoundExpressionId argument) {
               return IsConstantOrBinaryGroupTerm(argument);
             });
    }
    if (const auto* coalesce = std::get_if<BoundCoalesceExpression>(&expression.payload);
        coalesce != nullptr) {
      return std::ranges::all_of(coalesce->arguments, [this](BoundExpressionId argument) {
        return IsConstantOrBinaryGroupTerm(argument);
      });
    }
    if (const auto* conditional = std::get_if<BoundConditionalExpression>(&expression.payload);
        conditional != nullptr) {
      return std::ranges::all_of(conditional->arguments, [this](BoundExpressionId argument) {
        return IsConstantOrBinaryGroupTerm(argument);
      });
    }
    if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&expression.payload);
        likelihood != nullptr) {
      return IsConstantOrBinaryGroupTerm(likelihood->operand);
    }
    const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
    return collate != nullptr && IsConstantOrBinaryGroupTerm(collate->operand);
  }

  void FlattenLogicalAnd(BoundExpressionId id, std::vector<BoundExpressionId>& leaves) const {
    const BoundExpression& expression = impl_->expressions[id.value()];
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      FlattenLogicalAnd(alias->target, leaves);
      return;
    }
    const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
    if (binary != nullptr && binary->operation == BoundBinaryOperation::kLogicalAnd) {
      FlattenLogicalAnd(binary->left, leaves);
      FlattenLogicalAnd(binary->right, leaves);
      return;
    }
    leaves.push_back(id);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> AppendLogicalAnd(BoundExpressionId left,
                                                                 BoundExpressionId right) {
    const BoundExpression& left_expression = impl_->expressions[left.value()];
    const BoundExpression& right_expression = impl_->expressions[right.value()];
    const auto span = SourceSpan::FromBounds(
        std::min(left_expression.span.begin(), right_expression.span.begin()),
        std::max(left_expression.span.end(), right_expression.span.end()));
    if (!span.has_value()) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, left_expression.span,
                                         "aggregate AND span is invalid"));
    }
    return AppendExpression(*span,
                            BoundBinaryExpression{
                                .operation = BoundBinaryOperation::kLogicalAnd,
                                .left = left,
                                .right = right,
                            },
                            PropagatedExplicitProperties(std::array{left, right}));
  }

  [[nodiscard]] BindExpected<void> TransferHavingToWhere() {
    if (active_group_by_.empty() || !active_having_.has_value()) {
      return {};
    }
    std::vector<BoundExpressionId> leaves;
    FlattenLogicalAnd(*active_having_, leaves);
    std::optional<BoundExpressionId> remaining;
    for (const BoundExpressionId leaf : leaves) {
      const ExpressionProperties properties = Properties(leaf);
      const bool movable = properties.truth_hint != BoundTruthHint::kAlwaysFalse &&
                           !properties.contains_aggregate && IsConstantOrBinaryGroupTerm(leaf);
      std::optional<BoundExpressionId>& destination = movable ? impl_->where : remaining;
      if (!destination.has_value()) {
        destination = leaf;
        continue;
      }
      BindExpected<BoundExpressionId> combined = AppendLogicalAnd(*destination, leaf);
      if (!combined.has_value()) {
        return std::unexpected(std::move(combined.error()));
      }
      destination = *combined;
    }
    active_having_ = remaining;
    return {};
  }

  void CollectAggregateLeaves(BoundExpressionId id, std::vector<BoundExpressionId>& leaves,
                              std::vector<BoundSourceLeafIdentity>& identities) const {
    const BoundExpression& expression = impl_->expressions[id.value()];
    if (const std::optional<BoundSourceLeafIdentity> identity = AggregateIdentity(id);
        identity.has_value()) {
      if (!std::ranges::contains(identities, *identity)) {
        identities.push_back(*identity);
        leaves.push_back(id);
      }
      return;
    }
    if (std::holds_alternative<BoundAggregateCallExpression>(expression.payload) ||
        std::holds_alternative<BoundLiteralExpression>(expression.payload) ||
        std::holds_alternative<BoundParameterExpression>(expression.payload)) {
      return;
    }
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      CollectAggregateLeaves(alias->target, leaves, identities);
    } else if (const auto* unary = std::get_if<BoundUnaryExpression>(&expression.payload);
               unary != nullptr) {
      CollectAggregateLeaves(unary->operand, leaves, identities);
    } else if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
               binary != nullptr) {
      CollectAggregateLeaves(binary->left, leaves, identities);
      CollectAggregateLeaves(binary->right, leaves, identities);
    } else if (const auto* comparison = std::get_if<BoundComparisonExpression>(&expression.payload);
               comparison != nullptr) {
      CollectAggregateLeaves(comparison->left, leaves, identities);
      CollectAggregateLeaves(comparison->right, leaves, identities);
    } else if (const auto* truth = std::get_if<BoundTruthTestExpression>(&expression.payload);
               truth != nullptr) {
      CollectAggregateLeaves(truth->operand, leaves, identities);
    } else if (const auto* call = std::get_if<BoundScalarCallExpression>(&expression.payload);
               call != nullptr) {
      for (const BoundExpressionId argument : call->arguments) {
        CollectAggregateLeaves(argument, leaves, identities);
      }
    } else if (const auto* coalesce = std::get_if<BoundCoalesceExpression>(&expression.payload);
               coalesce != nullptr) {
      for (const BoundExpressionId argument : coalesce->arguments) {
        CollectAggregateLeaves(argument, leaves, identities);
      }
    } else if (const auto* conditional =
                   std::get_if<BoundConditionalExpression>(&expression.payload);
               conditional != nullptr) {
      for (const BoundExpressionId argument : conditional->arguments) {
        CollectAggregateLeaves(argument, leaves, identities);
      }
    } else if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&expression.payload);
               likelihood != nullptr) {
      CollectAggregateLeaves(likelihood->operand, leaves, identities);
    } else if (const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
               collate != nullptr) {
      CollectAggregateLeaves(collate->operand, leaves, identities);
    }
  }

  [[nodiscard]] BindExpected<void> AnalyzeAggregateLeaves(BoundAggregateProgram& program) {
    std::vector<BoundSourceLeafIdentity> source_identities;
    std::vector<BoundSourceLeafIdentity> representative_identities;
    for (const BoundGroupingTerm& term : program.group_by) {
      CollectAggregateLeaves(term.expression, program.source_leaves, source_identities);
    }
    for (const BoundAggregateInvocationId id : program.invocations) {
      const BoundAggregateInvocation& invocation = impl_->aggregate_invocations[id.value()];
      for (const BoundExpressionId argument : invocation.arguments) {
        CollectAggregateLeaves(argument, program.source_leaves, source_identities);
      }
      if (invocation.filter.has_value()) {
        CollectAggregateLeaves(*invocation.filter, program.source_leaves, source_identities);
      }
      const BoundAggregateFunction& function =
          impl_->aggregate_functions[invocation.function.value()];
      if (function.extremum == BoundAggregateExtremumKind::kNone) {
        for (const BoundOrderingTerm& term : invocation.order_by) {
          CollectAggregateLeaves(term.expression, program.source_leaves, source_identities);
        }
      }
    }
    for (const BoundResultColumn& result : impl_->result_columns) {
      CollectAggregateLeaves(result.expression, program.representative_leaves,
                             representative_identities);
    }
    for (const BoundOrderingTerm& term : impl_->order_by) {
      CollectAggregateLeaves(term.expression, program.representative_leaves,
                             representative_identities);
    }
    if (program.having.has_value()) {
      CollectAggregateLeaves(*program.having, program.representative_leaves,
                             representative_identities);
    }

    std::vector<BoundSourceLeafIdentity> retained = source_identities;
    for (const BoundSourceLeafIdentity& identity : representative_identities) {
      if (!std::ranges::contains(retained, identity)) {
        retained.push_back(identity);
      }
    }
    if (retained.size() > options_.maximum_aggregate_terms) {
      const SourceSpan span =
          program.having.has_value() ? impl_->expressions[program.having->value()].span
          : impl_->result_columns.front().expression.value() < impl_->expressions.size()
              ? impl_->expressions[impl_->result_columns.front().expression.value()].span
              : SourceSpan{};
      return std::unexpected(BinderError(BindErrorCode::kAggregateTermLimitExceeded, span,
                                         "too many aggregate terms"));
    }
    return {};
  }

  [[nodiscard]] BindExpected<std::optional<BoundAggregateProgram>> FinalizeAggregateProgram() {
    std::optional<BoundAggregateProgram> aggregate;
    if (pending_aggregates_.empty() && active_group_by_.empty() && !active_having_.has_value()) {
      return aggregate;
    }
    BoundAggregateProgram program;
    program.invocations.reserve(pending_aggregates_.size());
    constexpr std::array phases{
        AggregateOccurrencePhase::kResult,
        AggregateOccurrencePhase::kOrderBy,
        AggregateOccurrencePhase::kHaving,
    };
    for (const AggregateOccurrencePhase phase : phases) {
      for (PendingAggregateInvocation& pending : pending_aggregates_) {
        if (pending.phase != phase) {
          continue;
        }
        std::optional<BoundAggregateInvocationId> selected;
        for (const BoundAggregateInvocationId id : program.invocations) {
          if (AggregateInvocationsEquivalent(impl_->aggregate_invocations[id.value()],
                                             pending.invocation)) {
            selected = id;
            break;
          }
        }
        if (!selected.has_value()) {
          if (program.invocations.size() >= options_.maximum_aggregate_terms) {
            return std::unexpected(BinderError(BindErrorCode::kAggregateTermLimitExceeded,
                                               pending.invocation.span,
                                               "too many aggregate terms"));
          }
          if (impl_->aggregate_invocations.size() >= std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(BinderError(BindErrorCode::kInternalInvariant,
                                               pending.invocation.span,
                                               "bound aggregate invocation ID overflow"));
          }
          selected = BoundAggregateInvocationId{
              static_cast<std::uint32_t>(impl_->aggregate_invocations.size())};
          impl_->aggregate_invocations.push_back(std::move(pending.invocation));
          program.invocations.push_back(*selected);
        }
        auto* call = std::get_if<BoundAggregateCallExpression>(
            &impl_->expressions[pending.expression.value()].payload);
        if (call == nullptr) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant,
                                             impl_->expressions[pending.expression.value()].span,
                                             "pending aggregate expression is invalid"));
        }
        call->invocation = *selected;
      }
    }
    program.group_by = std::move(active_group_by_);
    program.having = active_having_;
    aggregate = std::move(program);
    pending_aggregates_.clear();
    return aggregate;
  }

  [[nodiscard]] BindExpected<void> FinishSelectQueryCore(SelectQuantifier quantifier) {
    BindExpected<void> transferred = TransferHavingToWhere();
    if (!transferred.has_value()) {
      return transferred;
    }
    auto aggregate = FinalizeAggregateProgram();
    if (!aggregate.has_value()) {
      return std::unexpected(std::move(aggregate.error()));
    }
    if (aggregate->has_value()) {
      BindExpected<void> analyzed = AnalyzeAggregateLeaves(**aggregate);
      if (!analyzed.has_value()) {
        return analyzed;
      }
    }
    core_metadata_.push_back(CoreBindingMetadata{
        .source_states = std::move(source_states_),
        .source_column_begin = active_source_begin_,
        .source_column_count = active_source_count_,
        .result_syntax = std::move(impl_->result_syntax),
    });
    impl_->query_cores.emplace_back(BoundSelectCore{
        .quantifier = quantifier,
        .table_sources = std::move(impl_->table_sources),
        .source_column_begin = active_source_begin_,
        .source_column_count = active_source_count_,
        .result_columns = std::move(impl_->result_columns),
        .where = impl_->where,
        .aggregate = std::move(*aggregate),
    });
    impl_->where.reset();
    return {};
  }

  [[nodiscard]] BindExpected<void> BindSelectCoreBody(const SelectCore& core) {
    BeginQueryCore();
    if (!core.from.empty() && core.from.front().on.has_value()) {
      return std::unexpected(BinderError(BindErrorCode::kJoinConditionScope,
                                         tree_.expression(*core.from.front().on).span,
                                         "a JOIN clause is required before ON"));
    }
    if (!core.from.empty() && core.from.front().using_clause.has_value()) {
      return std::unexpected(BinderError(BindErrorCode::kJoinConditionScope,
                                         core.from.front().using_clause->span,
                                         "a JOIN clause is required before USING"));
    }
    BindExpected<void> source = BindSource(core);
    if (!source.has_value()) {
      return source;
    }
    if (auto joined = BindJoinedColumns(core); !joined.has_value()) {
      return joined;
    }
    RegisterActiveSourceCollations();
    ReserveOutputStorage(core);
    BindExpected<void> results = BindResults(core);
    if (!results.has_value()) {
      return results;
    }
    for (std::size_t index = 1; index < core.from.size(); ++index) {
      const auto& on = core.from[index].on;
      if (!on.has_value()) {
        continue;
      }
      auto condition =
          BindExpression(*on, BindScope{.source_columns = true, .result_aliases = true});
      if (!condition.has_value()) {
        return std::unexpected(std::move(condition.error()));
      }
      BoundTableSource& bound_source = impl_->table_sources[index];
      bound_source.on = *condition;
      if (bound_source.join == SourceJoinKind::kLeft) {
        bool forward = false;
        auto visitor = [&](BoundSourceId owner) { forward = forward || owner > bound_source.id; };
        if (!VisitSourceOwners(impl_->expressions, impl_->source_columns, *condition, visitor)) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant,
                                             tree_.expression(*on).span,
                                             "ON condition has invalid source dependencies"));
        }
        if (forward) {
          return std::unexpected(BinderError(BindErrorCode::kJoinConditionScope,
                                             tree_.expression(*on).span,
                                             "ON clause references tables to its right"));
        }
      }
    }
    active_core_is_aggregate_ =
        !core.group_by.empty() ||
        std::ranges::any_of(impl_->result_columns, [this](const BoundResultColumn& result) {
          return Properties(result.expression).contains_aggregate;
        });
    if (core.having.has_value()) {
      if (!active_core_is_aggregate_) {
        return std::unexpected(BinderError(BindErrorCode::kHavingWithoutAggregate,
                                           tree_.expression(*core.having).span,
                                           "HAVING clause on a non-aggregate query"));
      }
      active_aggregate_phase_ = AggregateOccurrencePhase::kHaving;
      BindExpected<BoundExpressionId> having =
          BindExpression(*core.having, BindScope{
                                           .source_columns = true,
                                           .result_aliases = true,
                                           .allow_aggregates = true,
                                       });
      if (!having.has_value()) {
        return std::unexpected(std::move(having.error()));
      }
      active_having_ = *having;
    }
    if (core.where.has_value()) {
      BindExpected<BoundExpressionId> where =
          BindExpression(*core.where, BindScope{.source_columns = true, .result_aliases = true});
      if (!where.has_value()) {
        return std::unexpected(std::move(where.error()));
      }
      impl_->where = *where;
    }
    active_aggregate_phase_ = AggregateOccurrencePhase::kResult;
    return {};
  }

  [[nodiscard]] bool IsValuesConstant(BoundExpressionId id) const {
    const BoundExpression& expression = impl_->expressions[id.value()];
    return std::visit(
        [this](const auto& payload) {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, BoundLiteralExpression> ||
                        std::is_same_v<Payload, BoundParameterExpression>) {
            return true;
          } else if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                               std::is_same_v<Payload, BoundLikelihoodExpression> ||
                               std::is_same_v<Payload, BoundCollateExpression>) {
            return IsValuesConstant(payload.operand);
          } else if constexpr (std::is_same_v<Payload, BoundBinaryExpression>) {
            return IsValuesConstant(payload.left) && IsValuesConstant(payload.right);
          } else if constexpr (std::is_same_v<Payload, BoundComparisonExpression>) {
            return payload.comparison != SqlComparison::kIs &&
                   payload.comparison != SqlComparison::kIsNot && IsValuesConstant(payload.left) &&
                   IsValuesConstant(payload.right);
          } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression>) {
            const BoundScalarFunction& function = impl_->functions[payload.function.value()];
            return function.deterministic &&
                   std::ranges::all_of(payload.arguments, [this](BoundExpressionId argument) {
                     return IsValuesConstant(argument);
                   });
          } else if constexpr (std::is_same_v<Payload, BoundCoalesceExpression> ||
                               std::is_same_v<Payload, BoundConditionalExpression>) {
            return std::ranges::all_of(payload.arguments, [this](BoundExpressionId argument) {
              return IsValuesConstant(argument);
            });
          } else {
            return false;
          }
        },
        expression.payload);
  }

  [[nodiscard]] BindExpected<void> BindValuesQueryCore(const ValuesCore& core) {
    BeginQueryCore();
    const std::size_t column_count = core.rows.front().size();
    if (column_count > options_.maximum_result_columns) {
      return std::unexpected(BinderError(BindErrorCode::kResultColumnLimitExceeded, core.span,
                                         "too many columns in result set"));
    }
    const BindScope empty_scope{.source_columns = false, .result_aliases = false};
    std::vector<std::vector<BoundExpressionId>> rows;
    rows.reserve(core.rows.size());
    for (const std::vector<ExpressionId>& syntax_row : core.rows) {
      std::vector<BoundExpressionId> row;
      row.reserve(syntax_row.size());
      for (const ExpressionId expression : syntax_row) {
        BindExpected<BoundExpressionId> bound = BindExpression(expression, empty_scope);
        if (!bound.has_value()) {
          return std::unexpected(std::move(bound.error()));
        }
        row.push_back(*bound);
      }
      rows.push_back(std::move(row));
      if (syntax_row.size() != column_count) {
        return std::unexpected(BinderError(BindErrorCode::kValuesColumnCount, core.span,
                                           "all VALUES must have the same number of terms"));
      }
    }

    std::vector<BoundResultColumn> results;
    std::vector<std::optional<ExpressionId>> result_syntax;
    results.reserve(column_count);
    result_syntax.reserve(column_count);
    for (std::size_t index = 0; index < column_count; ++index) {
      const BoundExpressionId expression = rows.front()[index];
      results.push_back(BoundResultColumn{
          .expression = expression,
          .name = "column" + std::to_string(index + 1U),
          .affinity = Properties(expression).affinity,
      });
      result_syntax.emplace_back(core.rows.front()[index]);
    }

    BoundValuesEvaluationSchedule schedule = BoundValuesEvaluationSchedule::kProducerEager;
    if (std::ranges::any_of(rows,
                            [this](const std::vector<BoundExpressionId>& row) {
                              return std::ranges::any_of(row, [this](BoundExpressionId expression) {
                                return !IsValuesConstant(expression);
                              });
                            }) ||
        std::ranges::any_of(rows.front(), [this](BoundExpressionId expression) {
          return Properties(expression).affinity != TypeAffinity::kNone;
        })) {
      schedule = BoundValuesEvaluationSchedule::kConsumerFiltered;
    }

    core_metadata_.push_back(CoreBindingMetadata{
        .source_states = {},
        .result_syntax = std::move(result_syntax),
    });
    impl_->query_cores.emplace_back(BoundValuesCore{
        .rows = std::move(rows),
        .result_columns = std::move(results),
        .schedule = schedule,
    });
    return {};
  }

  [[nodiscard]] BindExpected<void> BindQueryCore(const QueryCore& core) {
    if (const auto* select = std::get_if<SelectCore>(&core); select != nullptr) {
      BindExpected<void> bound = BindSelectCoreBody(*select);
      if (!bound.has_value()) {
        return bound;
      }
      BindExpected<void> group_by = BindGroupBy(*select);
      if (!group_by.has_value()) {
        return group_by;
      }
      return FinishSelectQueryCore(select->quantifier);
    }
    return BindValuesQueryCore(std::get<ValuesCore>(core));
  }

  [[nodiscard]] static std::span<const BoundResultColumn> CoreResultColumns(
      const BoundQueryCore& core) {
    return std::visit(
        [](const auto& value) -> std::span<const BoundResultColumn> {
          return value.result_columns;
        },
        core);
  }

  void PublishResultColumns() {
    if (impl_->query_cores.empty()) {
      return;
    }
    const std::span<const BoundResultColumn> leftmost =
        CoreResultColumns(impl_->query_cores.front());
    impl_->output_columns.assign(leftmost.begin(), leftmost.end());
    const bool has_set_operator = std::ranges::any_of(
        impl_->compound_operators,
        [](CompoundOperator operation) { return operation != CompoundOperator::kUnionAll; });
    const bool has_explicit_order_collation = std::ranges::any_of(
        impl_->order_by, [](const BoundOrderingTerm& term) { return term.explicit_collation; });
    if (!has_set_operator || !has_explicit_order_collation || impl_->query_cores.size() < 2U) {
      return;
    }
    const std::span<const BoundResultColumn> rightmost =
        CoreResultColumns(impl_->query_cores.back());
    if (impl_->output_columns.size() != rightmost.size()) {
      return;
    }
    for (std::size_t index = 0; index < impl_->output_columns.size(); ++index) {
      impl_->output_columns[index].declared_type = rightmost[index].declared_type;
      impl_->output_columns[index].affinity = rightmost[index].affinity;
    }
  }

  [[nodiscard]] static std::string_view CompoundName(CompoundOperator operation) noexcept {
    switch (operation) {
      case CompoundOperator::kUnion:
        return "UNION";
      case CompoundOperator::kUnionAll:
        return "UNION ALL";
      case CompoundOperator::kIntersect:
        return "INTERSECT";
      case CompoundOperator::kExcept:
        return "EXCEPT";
    }
    return "UNION";
  }

  [[nodiscard]] bool CoreHasWildcard(const SelectCore& core) const {
    return std::ranges::any_of(core.result_columns, [this](const ResultColumn& result) {
      return std::holds_alternative<WildcardExpression>(
          tree_.expression(result.expression).payload);
    });
  }

  [[nodiscard]] std::optional<std::size_t> SyntacticCoreWidth(const QueryCore& core) const {
    if (const auto* select = std::get_if<SelectCore>(&core); select != nullptr) {
      return CoreHasWildcard(*select) ? std::nullopt
                                      : std::optional<std::size_t>{select->result_columns.size()};
    }
    return std::get<ValuesCore>(core).rows.front().size();
  }

  [[nodiscard]] BindExpected<void> ValidateSyntacticCompoundWidths(
      const SelectStatement& select) const {
    if (select.compounds.empty()) {
      return {};
    }
    const QueryCore* left = &select.first;
    for (const CompoundTerm& term : select.compounds) {
      const std::optional<std::size_t> left_width = SyntacticCoreWidth(*left);
      const std::optional<std::size_t> right_width = SyntacticCoreWidth(term.core);
      if (left_width.has_value() && right_width.has_value() && *left_width != *right_width) {
        return std::unexpected(BinderError(BindErrorCode::kCompoundColumnCount, term.span,
                                           "SELECTs to the left and right of " +
                                               std::string{CompoundName(term.operation)} +
                                               " do not have the same number of result columns"));
      }
      left = &term.core;
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> ValidateCompoundWidths(const SelectStatement& select) const {
    const std::size_t expected = CoreResultColumns(impl_->query_cores.front()).size();
    for (std::size_t index = 1; index < impl_->query_cores.size(); ++index) {
      if (CoreResultColumns(impl_->query_cores[index]).size() != expected) {
        const CompoundOperator operation = impl_->compound_operators[index - 1U];
        return std::unexpected(
            BinderError(BindErrorCode::kCompoundColumnCount, select.compounds[index - 1U].span,
                        "SELECTs to the left and right of " + std::string{CompoundName(operation)} +
                            " do not have the same number of result columns"));
      }
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> BindCompoundCollations(SourceSpan span) {
    const std::size_t column_count = CoreResultColumns(impl_->query_cores.front()).size();
    impl_->compound_collations.reserve(column_count);
    for (std::size_t column = 0; column < column_count; ++column) {
      std::optional<BoundCollationId> selected;
      for (const BoundQueryCore& core : impl_->query_cores) {
        const BoundExpressionId expression = CoreResultColumns(core)[column].expression;
        const ExpressionProperties properties = Properties(expression);
        if (properties.collation.has_value()) {
          selected = properties.collation;
          break;
        }
      }
      BindExpected<BoundCollationId> collation = selected.has_value()
                                                     ? BindExpected<BoundCollationId>{*selected}
                                                     : InternCollation("BINARY", span);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      impl_->compound_collations.push_back(*collation);
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> RequireResultCollations(
      SourceSpan span, std::span<const BoundResultColumn> results) {
    for (const BoundResultColumn& result : results) {
      const ExpressionProperties properties = Properties(result.expression);
      BindExpected<BoundCollationId> collation =
          properties.collation.has_value() ? BindExpected<BoundCollationId>{*properties.collation}
                                           : InternCollation("BINARY", span);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      BindExpected<void> required = RequireCollation(*collation, span);
      if (!required.has_value()) {
        return required;
      }
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> RequireCompoundDistinctCollations(
      const SelectStatement& select) {
    for (std::size_t index = 0; index < impl_->query_cores.size(); ++index) {
      const auto* core = std::get_if<BoundSelectCore>(&impl_->query_cores[index]);
      if (core == nullptr || core->quantifier != SelectQuantifier::kDistinct) {
        continue;
      }
      const SourceSpan span = index == 0U
                                  ? std::get<SelectCore>(select.first).span
                                  : std::get<SelectCore>(select.compounds[index - 1U].core).span;
      BindExpected<void> required = RequireResultCollations(span, core->result_columns);
      if (!required.has_value()) {
        return required;
      }
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> RequireSetCompoundCollations(SourceSpan span) const {
    const bool compares_rows = std::ranges::any_of(
        impl_->compound_operators,
        [](CompoundOperator operation) { return operation != CompoundOperator::kUnionAll; });
    if (!compares_rows) {
      return {};
    }
    for (const BoundCollationId collation : impl_->compound_collations) {
      BindExpected<void> required = RequireCollation(collation, span);
      if (!required.has_value()) {
        return required;
      }
    }
    return {};
  }

  [[nodiscard]] BindExpected<std::optional<std::string>> CompoundOrderByName(ExpressionId id) {
    const Expression& expression = tree_.expression(OrderByCore(id));
    if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.payload);
        identifier != nullptr) {
      BindExpected<DecodedNameParts> parts = NameParts(identifier->name);
      if (!parts.has_value()) {
        return std::unexpected(std::move(parts.error()));
      }
      if (parts->size() == 1U) {
        return std::optional<std::string>{std::string{parts->front()}};
      }
    } else if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
               literal != nullptr && literal->kind == LiteralKind::kString &&
               SpanText(literal->token).starts_with('"')) {
      BindExpected<std::string> name = Dequote(literal->token);
      if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
      }
      return std::optional<std::string>{std::move(*name)};
    }
    return std::optional<std::string>{};
  }

  void ActivateQueryCore(std::size_t core_index) {
    const CoreBindingMetadata& metadata = core_metadata_[core_index];
    source_states_ = metadata.source_states;
    active_source_begin_ = metadata.source_column_begin;
    active_source_count_ = metadata.source_column_count;
    aliases_.clear();
    impl_->table_sources.clear();
    if (const auto* select = std::get_if<BoundSelectCore>(&impl_->query_cores[core_index]);
        select != nullptr) {
      impl_->table_sources = select->table_sources;
    }
  }

  [[nodiscard]] bool OrderByHasExplicitCollation(ExpressionId id) const noexcept {
    while (true) {
      const Expression& expression = tree_.expression(id);
      if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
          parenthesized != nullptr) {
        id = parenthesized->inner;
        continue;
      }
      return std::holds_alternative<CollateExpression>(expression.payload);
    }
  }

  [[nodiscard]] BindExpected<void> BindCompoundOrderBy(const SelectStatement& select) {
    const std::span<const BoundResultColumn> output = CoreResultColumns(impl_->query_cores.front());
    impl_->order_by.reserve(select.order_by.size());
    for (std::size_t term_index = 0; term_index < select.order_by.size(); ++term_index) {
      const OrderingTerm& term = select.order_by[term_index];
      std::optional<std::size_t> result_column;

      BindExpected<std::optional<std::int64_t>> ordinal = OrderByOrdinal(term.expression);
      if (!ordinal.has_value()) {
        return std::unexpected(std::move(ordinal.error()));
      }
      if (ordinal->has_value()) {
        if (**ordinal < 1 || std::cmp_greater(**ordinal, output.size())) {
          return std::unexpected(
              BinderError(BindErrorCode::kOrderByTermOutOfRange, term.span,
                          std::to_string(term_index + 1U) +
                              " ORDER BY term out of range - should be between 1 and " +
                              std::to_string(output.size())));
        }
        result_column = static_cast<std::size_t>(**ordinal - 1);
      } else {
        BindExpected<std::optional<std::string>> requested = CompoundOrderByName(term.expression);
        if (!requested.has_value()) {
          return std::unexpected(std::move(requested.error()));
        }
        const ExpressionId comparable = OrderByCore(term.expression);
        for (std::size_t core_index = 0;
             core_index < impl_->query_cores.size() && !result_column.has_value(); ++core_index) {
          ActivateQueryCore(core_index);
          const std::span<const BoundResultColumn> results =
              CoreResultColumns(impl_->query_cores[core_index]);
          const auto& syntax = core_metadata_[core_index].result_syntax;

          if (requested->has_value()) {
            for (std::size_t column = 0; column < results.size(); ++column) {
              bool matches = NamesEqual(results[column].name, **requested);
              if (!matches && column < syntax.size() && syntax[column].has_value()) {
                BindExpected<std::optional<std::string>> syntax_name =
                    CompoundOrderByName(*syntax[column]);
                if (!syntax_name.has_value()) {
                  return std::unexpected(std::move(syntax_name.error()));
                }
                matches = syntax_name->has_value() && NamesEqual(**syntax_name, **requested);
              }
              if (matches) {
                result_column = column;
                break;
              }
            }
            if (result_column.has_value()) {
              break;
            }
          }

          const std::size_t expression_count = impl_->expressions.size();
          const std::size_t pending_count = pending_aggregates_.size();
          const std::size_t collation_count = impl_->collations.size();
          const std::size_t function_count = impl_->functions.size();
          const std::size_t aggregate_function_count = impl_->aggregate_functions.size();
          BindExpected<BoundExpressionId> bound =
              BindExpression(comparable, BindScope{
                                             .source_columns = true,
                                             .result_aliases = false,
                                             .allow_aggregates = true,
                                         });
          if (!bound.has_value()) {
            impl_->expressions.resize(expression_count);
            while (pending_aggregates_.size() > pending_count) {
              pending_aggregates_.pop_back();
            }
            while (impl_->aggregate_functions.size() > aggregate_function_count) {
              impl_->aggregate_functions.pop_back();
            }
            impl_->functions.resize(function_count);
            impl_->collations.resize(collation_count);
            continue;
          }
          for (std::size_t column = 0; column < results.size(); ++column) {
            if (BoundSemanticallyEquivalent(*bound, results[column].expression)) {
              result_column = column;
              break;
            }
          }
          impl_->expressions.resize(expression_count);
          while (pending_aggregates_.size() > pending_count) {
            pending_aggregates_.pop_back();
          }
          while (impl_->aggregate_functions.size() > aggregate_function_count) {
            impl_->aggregate_functions.pop_back();
          }
          impl_->functions.resize(function_count);
          impl_->collations.resize(collation_count);
        }
      }

      if (!result_column.has_value()) {
        return std::unexpected(
            BinderError(BindErrorCode::kCompoundOrderByTerm, term.span,
                        std::to_string(term_index + 1U) +
                            " ORDER BY term does not match any column in the result set"));
      }

      const BoundExpressionId target = output[*result_column].expression;
      BindExpected<BoundExpressionId> expression =
          BindOrderByResultReference(term.expression, target);
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      const ExpressionProperties properties = Properties(*expression);
      const BoundCollationId collation =
          OrderByHasExplicitCollation(term.expression) && properties.collation.has_value()
              ? *properties.collation
              : impl_->compound_collations[*result_column];
      BindExpected<void> required = RequireCollation(collation, term.span);
      if (!required.has_value()) {
        return required;
      }
      const SortOrder order =
          term.order == SortOrder::kDescending ? SortOrder::kDescending : SortOrder::kAscending;
      const BoundNullPlacement null_placement =
          term.null_order == NullOrder::kFirst  ? BoundNullPlacement::kFirst
          : term.null_order == NullOrder::kLast ? BoundNullPlacement::kLast
          : order == SortOrder::kDescending     ? BoundNullPlacement::kLast
                                                : BoundNullPlacement::kFirst;
      impl_->order_by.push_back(BoundOrderingTerm{
          .expression = *expression,
          .collation = collation,
          .order = order,
          .null_placement = null_placement,
          .result_column = *result_column,
          .explicit_collation = OrderByHasExplicitCollation(term.expression),
      });
    }
    active_aggregate_phase_ = AggregateOccurrencePhase::kResult;
    return {};
  }

  [[nodiscard]] BindExpected<void> BindGroupBy(const SelectCore& core) {
    active_group_by_.reserve(core.group_by.size());
    for (std::size_t term_index = 0; term_index < core.group_by.size(); ++term_index) {
      const ExpressionId syntax = core.group_by[term_index];
      const SourceSpan span = tree_.expression(syntax).span;
      std::optional<std::size_t> result_column;
      BindExpected<std::optional<std::int64_t>> ordinal = ClassifyOrderByInteger(syntax, true);
      if (!ordinal.has_value()) {
        return std::unexpected(std::move(ordinal.error()));
      }

      BindExpected<BoundExpressionId> expression = std::unexpected(BinderError(
          BindErrorCode::kInternalInvariant, span, "GROUP BY expression was not bound"));
      if (ordinal->has_value()) {
        if (**ordinal < 1 || std::cmp_greater(**ordinal, impl_->result_columns.size())) {
          return std::unexpected(
              BinderError(BindErrorCode::kGroupByTermOutOfRange, span,
                          std::to_string(term_index + 1U) +
                              " GROUP BY term out of range - should be between 1 and " +
                              std::to_string(impl_->result_columns.size())));
        }
        result_column = static_cast<std::size_t>(**ordinal - 1);
        expression = BindOrderByResultReference(syntax, *result_column);
      } else {
        expression = BindExpression(syntax, BindScope{
                                                .source_columns = true,
                                                .result_aliases = true,
                                                .allow_aggregates = true,
                                            });
      }
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      if (Properties(*expression).contains_aggregate) {
        return std::unexpected(
            BinderError(BindErrorCode::kAggregateMisuse, span,
                        "aggregate functions are not allowed in the GROUP BY clause"));
      }

      if (!result_column.has_value()) {
        for (std::size_t index = impl_->result_columns.size(); index > 0U; --index) {
          if (BoundSemanticallyEquivalent(*expression,
                                          impl_->result_columns[index - 1U].expression)) {
            result_column = index - 1U;
            break;
          }
        }
      }

      const ExpressionProperties properties = Properties(*expression);
      BindExpected<BoundCollationId> collation =
          properties.collation.has_value() ? BindExpected<BoundCollationId>{*properties.collation}
                                           : InternCollation("BINARY", span);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      BindExpected<void> required = RequireCollation(*collation, span);
      if (!required.has_value()) {
        return required;
      }
      active_group_by_.push_back(BoundGroupingTerm{
          .expression = *expression,
          .collation = *collation,
          .result_column = result_column,
      });
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> BindLimit(const std::optional<LimitClause>& syntax) {
    if (!syntax.has_value()) {
      return {};
    }
    const BindScope empty_scope{.source_columns = false, .result_aliases = false};
    BindExpected<BoundExpressionId> limit = BindExpression(syntax->limit, empty_scope);
    if (!limit.has_value()) {
      return std::unexpected(std::move(limit.error()));
    }
    BoundLimit bound_limit{.limit = *limit};
    if (syntax->offset.has_value()) {
      BindExpected<BoundExpressionId> offset = BindExpression(*syntax->offset, empty_scope);
      if (!offset.has_value()) {
        return std::unexpected(std::move(offset.error()));
      }
      bound_limit.offset = *offset;
    }
    impl_->limit = bound_limit;
    return {};
  }

  void ReserveOutputStorage(const SelectCore& select) {
    const std::size_t syntax_count = tree_.expressions().size();
    const std::size_t wildcard_slack = active_source_count_;
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
    impl_->collations.reserve(active_source_count_ == std::numeric_limits<std::size_t>::max()
                                  ? active_source_count_
                                  : active_source_count_ + 1U);
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

  [[nodiscard]] bool ValidateSelectCore(const SelectCore& core) const noexcept {
    if (!SpanIsValid(core.span) || core.result_columns.empty()) {
      return false;
    }
    for (const ResultColumn& result : core.result_columns) {
      if (!SpanIsValid(result.span) || !IdIsValid(result.expression) ||
          (result.alias.has_value() && !SpanIsValid(*result.alias))) {
        return false;
      }
    }
    if (core.from.size() > kMaximumSelectSources ||
        !std::ranges::all_of(core.from, [this](const TableSource& source) {
          return SpanIsValid(source.span) && QualifiedNameIsValid(source.name) &&
                 (!source.alias.has_value() || SpanIsValid(*source.alias)) &&
                 (!source.on.has_value() || IdIsValid(*source.on)) &&
                 (!source.using_clause.has_value() ||
                  (SpanIsValid(source.using_clause->span) &&
                   std::ranges::all_of(source.using_clause->columns,
                                       [this](SourceSpan column) { return SpanIsValid(column); })));
        })) {
      return false;
    }
    if (core.where.has_value() && !IdIsValid(*core.where)) {
      return false;
    }
    if (!std::ranges::all_of(core.group_by, [this](ExpressionId id) { return IdIsValid(id); })) {
      return false;
    }
    return !core.having.has_value() || IdIsValid(*core.having);
  }

  [[nodiscard]] bool ValidateQueryCore(const QueryCore& core) const noexcept {
    if (const auto* select = std::get_if<SelectCore>(&core); select != nullptr) {
      return ValidateSelectCore(*select);
    }
    const auto* values = std::get_if<ValuesCore>(&core);
    if (values == nullptr || !SpanIsValid(values->span) || values->rows.empty()) {
      return false;
    }
    for (const std::vector<ExpressionId>& row : values->rows) {
      if (row.empty() ||
          !std::ranges::all_of(row, [this](ExpressionId id) { return IdIsValid(id); })) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool ValidateTree(const SelectStatement& select) const noexcept {
    if (!SpanIsValid(select.span) || !ValidateQueryCore(select.first)) {
      return false;
    }
    for (const CompoundTerm& term : select.compounds) {
      if (!SpanIsValid(term.span) || !ValidateQueryCore(term.core)) {
        return false;
      }
    }
    for (const OrderingTerm& term : select.order_by) {
      if (!SpanIsValid(term.span) || !IdIsValid(term.expression)) {
        return false;
      }
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
                                 [this](ExpressionId id) { return IdIsValid(id); }) &&
             std::ranges::all_of(call->order_by,
                                 [this](const OrderingTerm& term) {
                                   return SpanIsValid(term.span) && IdIsValid(term.expression);
                                 }) &&
             (!call->filter.has_value() || IdIsValid(*call->filter));
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

  [[nodiscard]] BindExpected<BoundSourceId> AllocateSourceId(SourceSpan span) {
    if (next_source_id_ == std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          BinderError(BindErrorCode::kInternalInvariant, span, "bound source ID overflow"));
    }
    return BoundSourceId{next_source_id_++};
  }

  [[nodiscard]] BindExpected<void> BindSource(const SelectCore& select) {
    impl_->table_sources.reserve(select.from.size());
    source_states_.reserve(select.from.size());
    for (const TableSource& source : select.from) {
      auto bound = BindTableSource(source);
      if (!bound.has_value()) {
        return bound;
      }
    }
    active_source_count_ = impl_->source_columns.size() - active_source_begin_;
    return {};
  }

  [[nodiscard]] BoundSourceColumnId JoinedColumnRepresentative(
      BoundSourceColumnId id) const noexcept {
    return impl_->source_columns[id.value()].merged_into.value_or(id);
  }

  [[nodiscard]] std::optional<BoundSourceColumnId> FindVisibleLeftColumn(
      std::string_view name, std::size_t end) const noexcept {
    for (std::size_t index = active_source_begin_; index < end; ++index) {
      const BoundSourceColumn& column = impl_->source_columns[index];
      if (!column.merged_into.has_value() && NamesEqual(column.name, name)) {
        return BoundSourceColumnId{static_cast<std::uint32_t>(index)};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] BindExpected<void> BindUsingColumn(std::size_t source_index, std::string_view name,
                                                   SourceSpan span) {
    const SourceState& source = source_states_[source_index];
    const auto left = FindVisibleLeftColumn(name, source.source_column_begin);
    const auto right = FindSourceColumn(source, name);
    if (!left.has_value() || !right.has_value()) {
      return std::unexpected(BinderError(BindErrorCode::kJoinConditionScope, span,
                                         "cannot join using column " + std::string{name} +
                                             " - column not present in both tables"));
    }
    auto left_expression = BindColumn(*left, span);
    if (!left_expression.has_value()) {
      return std::unexpected(std::move(left_expression.error()));
    }
    auto right_expression = BindColumn(*right, span);
    if (!right_expression.has_value()) {
      return std::unexpected(std::move(right_expression.error()));
    }
    auto equality =
        AppendBoundComparison(SqlComparison::kEqual, *left_expression, *right_expression, span);
    if (!equality.has_value()) {
      return std::unexpected(std::move(equality.error()));
    }
    auto& condition = impl_->table_sources[source_index].on;
    if (condition.has_value()) {
      auto conjunction =
          AppendBoundBinary(BoundBinaryOperation::kLogicalAnd, *condition, *equality, span);
      if (!conjunction.has_value()) {
        return std::unexpected(std::move(conjunction.error()));
      }
      condition = *conjunction;
    } else {
      condition = *equality;
    }
    impl_->source_columns[right->value()].merged_into = *left;
    return {};
  }

  [[nodiscard]] BindExpected<void> BindJoinedColumns(const SelectCore& core) {
    for (std::size_t index = 1; index < core.from.size(); ++index) {
      const TableSource& source = core.from[index];
      if (source.natural && (source.on.has_value() || source.using_clause.has_value())) {
        return std::unexpected(BinderError(BindErrorCode::kJoinConditionScope, source.span,
                                           "a NATURAL join may not have an ON or USING clause"));
      }
      if (source.using_clause.has_value()) {
        for (const SourceSpan column : source.using_clause->columns) {
          auto name = Dequote(column);
          if (!name.has_value()) {
            return std::unexpected(std::move(name.error()));
          }
          if (auto joined = BindUsingColumn(index, *name, column); !joined.has_value()) {
            return joined;
          }
        }
      } else if (source.natural) {
        const SourceState& right = source_states_[index];
        const std::size_t end = right.source_column_begin + right.source_column_count;
        for (std::size_t column = right.source_column_begin; column < end; ++column) {
          const std::string_view name = impl_->source_columns[column].name;
          if (FindVisibleLeftColumn(name, right.source_column_begin).has_value()) {
            if (auto joined = BindUsingColumn(index, name, source.span); !joined.has_value()) {
              return joined;
            }
          }
        }
      }
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> BindTableSource(const TableSource& source) {
    BindExpected<DecodedNameParts> parts = NameParts(source.name);
    if (!parts.has_value()) {
      return std::unexpected(std::move(parts.error()));
    }
    if (parts->empty() || parts->size() > 2U ||
        (parts->size() == 2U && !NamesEqual((*parts)[0], catalog_->schema_name()))) {
      return NoSuchTable(source.name.span);
    }
    std::optional<std::string> alias;
    if (source.alias.has_value()) {
      BindExpected<std::string> bound_alias = Dequote(*source.alias);
      if (!bound_alias.has_value()) {
        return std::unexpected(std::move(bound_alias.error()));
      }
      alias = std::move(*bound_alias);
    }

    const std::string_view requested_name = parts->back();
    if (IsSchemaTableName(requested_name)) {
      auto source_id = AllocateSourceId(source.span);
      if (!source_id.has_value()) {
        return std::unexpected(std::move(source_id.error()));
      }
      const std::size_t column_begin = impl_->source_columns.size();
      source_states_.push_back(SourceState{
          .id = *source_id,
          .name = "sqlite_schema",
          .alias = std::move(alias),
          .table = std::nullopt,
          .schema_table = true,
          .source_column_begin = column_begin,
          .source_column_count = kSchemaColumns.size(),
      });
      impl_->table_sources.push_back(BoundTableSource{
          .id = *source_id,
          .kind = BoundSourceKind::kSchemaTable,
          .table = std::nullopt,
          .span = source.span,
          .join = source.join,
          .source_column_begin = column_begin,
          .source_column_count = kSchemaColumns.size(),
      });
      impl_->source_columns.reserve(impl_->source_columns.size() + kSchemaColumns.size());
      for (const SchemaColumnDefinition& column : kSchemaColumns) {
        impl_->source_columns.push_back(BoundSourceColumn{
            .source = *source_id,
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
      return NoSuchTable(source.name.span);
    }
    auto source_id = AllocateSourceId(source.span);
    if (!source_id.has_value()) {
      return std::unexpected(std::move(source_id.error()));
    }
    const CatalogTable& table = catalog_->table(*table_id);
    const std::size_t column_begin = impl_->source_columns.size();
    source_states_.push_back(SourceState{
        .id = *source_id,
        .name = table.name,
        .alias = std::move(alias),
        .table = *table_id,
        .schema_table = false,
        .source_column_begin = column_begin,
        .source_column_count = table.columns.size(),
    });
    impl_->table_sources.push_back(BoundTableSource{
        .id = *source_id,
        .kind = BoundSourceKind::kCatalogTable,
        .table = *table_id,
        .span = source.span,
        .join = source.join,
        .source_column_begin = column_begin,
        .source_column_count = table.columns.size(),
    });
    impl_->source_columns.reserve(impl_->source_columns.size() + table.columns.size());
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
      const CatalogColumn& column = table.columns[index];
      impl_->source_columns.push_back(BoundSourceColumn{
          .source = *source_id,
          .rowid_alias = table.rowid_alias == ColumnId{index},
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

  [[nodiscard]] BindExpected<void> BindResults(const SelectCore& select) {
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

      BindExpected<BoundExpressionId> bound =
          BindExpression(result.expression, BindScope{
                                                .source_columns = true,
                                                .result_aliases = false,
                                                .allow_aggregates = true,
                                            });
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
        aliases_.push_back(AliasEntry{
            .name = std::move(*alias),
            .target = *bound,
            .result_index = impl_->result_columns.size(),
        });
      }

      impl_->result_columns.push_back(std::move(output));
      impl_->result_syntax.emplace_back(result.expression);
    }
    return {};
  }

  [[nodiscard]] ExpressionId OrderByCore(ExpressionId id) const noexcept {
    while (true) {
      const Expression& expression = tree_.expression(id);
      if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
          parenthesized != nullptr) {
        id = parenthesized->inner;
        continue;
      }
      if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
          collate != nullptr) {
        id = collate->operand;
        continue;
      }
      return id;
    }
  }

  [[nodiscard]] BindExpected<std::optional<std::size_t>> OrderByAlias(ExpressionId id) {
    const Expression& expression = tree_.expression(OrderByCore(id));
    const auto* identifier = std::get_if<IdentifierExpression>(&expression.payload);
    std::string owned_name;
    std::optional<std::string_view> requested;
    if (identifier != nullptr) {
      BindExpected<DecodedNameParts> parts = NameParts(identifier->name);
      if (!parts.has_value()) {
        return std::unexpected(std::move(parts.error()));
      }
      if (parts->size() == 1U) {
        owned_name = std::string{parts->front()};
        requested = owned_name;
      }
    } else if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
               literal != nullptr && literal->kind == LiteralKind::kString &&
               SpanText(literal->token).starts_with('"')) {
      BindExpected<std::string> name = Dequote(literal->token);
      if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
      }
      owned_name = std::move(*name);
      requested = owned_name;
    }
    if (!requested.has_value()) {
      return std::optional<std::size_t>{};
    }
    for (const AliasEntry& alias : aliases_) {
      if (NamesEqual(alias.name, *requested)) {
        return std::optional<std::size_t>{alias.result_index};
      }
    }
    return std::optional<std::size_t>{};
  }

  [[nodiscard]] BindExpected<std::optional<std::int64_t>> OrderByOrdinal(ExpressionId id) {
    return ClassifyOrderByInteger(id, true);
  }

  [[nodiscard]] BindExpected<std::optional<std::int64_t>> ClassifyOrderByInteger(
      ExpressionId id, bool allow_collate) {
    const Expression& expression = tree_.expression(id);
    if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
        parenthesized != nullptr) {
      return ClassifyOrderByInteger(parenthesized->inner, allow_collate);
    }
    if (allow_collate) {
      if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
          collate != nullptr) {
        return ClassifyOrderByInteger(collate->operand, true);
      }
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload);
        unary != nullptr &&
        (unary->op == UnaryOperator::kPositive || unary->op == UnaryOperator::kNegative)) {
      BindExpected<std::optional<std::int64_t>> operand =
          ClassifyOrderByInteger(unary->operand, false);
      if (!operand.has_value() || !operand->has_value()) {
        return operand;
      }
      return std::optional<std::int64_t>{unary->op == UnaryOperator::kNegative ? -operand->value()
                                                                               : operand->value()};
    }
    const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
    if (literal == nullptr || literal->kind != LiteralKind::kInteger) {
      return std::optional<std::int64_t>{};
    }
    BindExpected<SqlValue> value = MaterializeSyntaxConstant(id);
    if (!value.has_value()) {
      return std::unexpected(std::move(value.error()));
    }
    const std::optional<std::int64_t> integer = value->integer_value();
    if (!integer.has_value() || *integer < 0 ||
        *integer > std::numeric_limits<std::int32_t>::max()) {
      return std::optional<std::int64_t>{};
    }
    return integer;
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindOrderByResultReference(
      ExpressionId id, std::size_t result_index) {
    return BindOrderByResultReference(id, impl_->result_columns[result_index].expression);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindOrderByResultReference(
      ExpressionId id, BoundExpressionId target) {
    const Expression& expression = tree_.expression(id);
    if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
        parenthesized != nullptr) {
      return BindOrderByResultReference(parenthesized->inner, target);
    }
    if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
        collate != nullptr) {
      BindExpected<BoundExpressionId> operand =
          BindOrderByResultReference(collate->operand, target);
      if (!operand.has_value()) {
        return std::unexpected(std::move(operand.error()));
      }
      BindExpected<std::string> name = Dequote(collate->collation);
      if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
      }
      BindExpected<BoundCollationId> collation = InternCollation(*name, collate->collation);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      ExpressionProperties properties = Properties(*operand);
      properties.collation = *collation;
      properties.explicit_collation = true;
      return AppendExpression(expression.span,
                              BoundCollateExpression{.operand = *operand, .collation = *collation},
                              properties);
    }
    return AppendExpression(expression.span, BoundAliasReferenceExpression{.target = target},
                            Properties(target));
  }

  [[nodiscard]] bool SyntaxNamesEquivalent(const QualifiedName& left,
                                           const QualifiedName& right) const {
    BindExpected<DecodedNameParts> left_parts = NameParts(left);
    BindExpected<DecodedNameParts> right_parts = NameParts(right);
    if (!left_parts.has_value() || !right_parts.has_value() ||
        left_parts->size() != right_parts->size()) {
      return false;
    }
    for (std::size_t index = 0; index < left_parts->size(); ++index) {
      if (!NamesEqual((*left_parts)[index], (*right_parts)[index])) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] const BoundAggregateInvocation* AggregateInvocationForExpression(
      BoundExpressionId expression, const BoundAggregateCallExpression& call) const noexcept {
    for (const PendingAggregateInvocation& pending : pending_aggregates_) {
      if (pending.expression == expression) {
        return &pending.invocation;
      }
    }
    return call.invocation.value() < impl_->aggregate_invocations.size()
               ? &impl_->aggregate_invocations[call.invocation.value()]
               : nullptr;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] bool SyntaxExpressionsEquivalent(ExpressionId left_id, ExpressionId right_id) {
    const Expression& left = ParenthesesTransparentExpression(left_id);
    const Expression& right = ParenthesesTransparentExpression(right_id);
    const auto is_string_literal = [](const Expression& expression) {
      const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
      return literal != nullptr && literal->kind == LiteralKind::kString;
    };
    if ((std::holds_alternative<IdentifierExpression>(left.payload) && is_string_literal(right)) ||
        (is_string_literal(left) && std::holds_alternative<IdentifierExpression>(right.payload))) {
      return true;
    }
    if (left.payload.index() != right.payload.index()) {
      return false;
    }
    if (const auto* literal = std::get_if<LiteralExpression>(&left.payload); literal != nullptr) {
      const auto& other = std::get<LiteralExpression>(right.payload);
      if (literal->kind != other.kind) {
        return false;
      }
      if (literal->kind == LiteralKind::kInteger) {
        BindExpected<SqlValue> left_value = MaterializeSyntaxConstant(left_id);
        BindExpected<SqlValue> right_value = MaterializeSyntaxConstant(right_id);
        if (!left_value.has_value() || !right_value.has_value()) {
          return false;
        }
        const std::optional<std::int64_t> left_integer = left_value->integer_value();
        const std::optional<std::int64_t> right_integer = right_value->integer_value();
        if (left_integer.has_value() && right_integer.has_value() && *left_integer >= 0 &&
            *right_integer >= 0 && *left_integer <= std::numeric_limits<std::int32_t>::max() &&
            *right_integer <= std::numeric_limits<std::int32_t>::max()) {
          return left_integer == right_integer;
        }
      } else if (literal->kind == LiteralKind::kString || literal->kind == LiteralKind::kNull ||
                 literal->kind == LiteralKind::kTrue || literal->kind == LiteralKind::kFalse) {
        return true;
      }
      return SpanText(literal->token) == SpanText(other.token);
    }
    if (const auto* variable = std::get_if<VariableExpression>(&left.payload);
        variable != nullptr) {
      return SpanText(variable->token) ==
             SpanText(std::get<VariableExpression>(right.payload).token);
    }
    if (const auto* identifier = std::get_if<IdentifierExpression>(&left.payload);
        identifier != nullptr) {
      static_cast<void>(identifier);
      return true;
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&left.payload); unary != nullptr) {
      const auto& other = std::get<UnaryExpression>(right.payload);
      return unary->op == other.op && SyntaxExpressionsEquivalent(unary->operand, other.operand);
    }
    if (const auto* binary = std::get_if<BinaryExpression>(&left.payload); binary != nullptr) {
      const auto& other = std::get<BinaryExpression>(right.payload);
      return binary->op == other.op && SyntaxExpressionsEquivalent(binary->left, other.left) &&
             SyntaxExpressionsEquivalent(binary->right, other.right);
    }
    if (const auto* call = std::get_if<FunctionCallExpression>(&left.payload); call != nullptr) {
      const auto& other = std::get<FunctionCallExpression>(right.payload);
      if (call->distinct != other.distinct || !SyntaxNamesEquivalent(call->name, other.name) ||
          call->arguments.size() != other.arguments.size() ||
          call->order_by.size() != other.order_by.size() ||
          call->filter.has_value() != other.filter.has_value()) {
        return false;
      }
      for (std::size_t index = 0; index < call->arguments.size(); ++index) {
        if (!SyntaxExpressionsEquivalent(call->arguments[index], other.arguments[index])) {
          return false;
        }
      }
      for (std::size_t index = 0; index < call->order_by.size(); ++index) {
        const OrderingTerm& left_term = call->order_by[index];
        const OrderingTerm& right_term = other.order_by[index];
        if (left_term.order != right_term.order || left_term.null_order != right_term.null_order ||
            !SyntaxExpressionsEquivalent(left_term.expression, right_term.expression)) {
          return false;
        }
      }
      if (call->filter.has_value() && !SyntaxExpressionsEquivalent(*call->filter, *other.filter)) {
        return false;
      }
      return true;
    }
    if (const auto* collate = std::get_if<CollateExpression>(&left.payload); collate != nullptr) {
      const auto& other = std::get<CollateExpression>(right.payload);
      BindExpected<std::string> left_name = Dequote(collate->collation);
      BindExpected<std::string> right_name = Dequote(other.collation);
      return left_name.has_value() && right_name.has_value() &&
             NamesEqual(*left_name, *right_name) &&
             SyntaxExpressionsEquivalent(collate->operand, other.operand);
    }
    return false;
  }

  [[nodiscard]] bool BoundSemanticallyEquivalent(BoundExpressionId left_id,
                                                 BoundExpressionId right_id) const {
    const BoundExpression& left = impl_->expressions[left_id.value()];
    const BoundExpression& right = impl_->expressions[right_id.value()];
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&left.payload);
        alias != nullptr) {
      return BoundSemanticallyEquivalent(alias->target, right_id);
    }
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&right.payload);
        alias != nullptr) {
      return BoundSemanticallyEquivalent(left_id, alias->target);
    }
    const std::optional<BoundSourceLeafIdentity> left_identity = AggregateIdentity(left_id);
    const std::optional<BoundSourceLeafIdentity> right_identity = AggregateIdentity(right_id);
    if (left_identity.has_value() || right_identity.has_value()) {
      return left_identity.has_value() && right_identity.has_value() &&
             *left_identity == *right_identity;
    }
    if (left.payload.index() != right.payload.index()) {
      return false;
    }
    if (const auto* literal = std::get_if<BoundLiteralExpression>(&left.payload);
        literal != nullptr) {
      const SqlValue& other = std::get<BoundLiteralExpression>(right.payload).value;
      if (literal->value.type() != other.type()) {
        return false;
      }
      switch (literal->value.type()) {
        case SqlValueType::kNull:
          return true;
        case SqlValueType::kInteger:
          return literal->value.integer_value() == other.integer_value();
        case SqlValueType::kReal:
          return literal->value.real_value() == other.real_value();
        case SqlValueType::kText:
          return literal->value.text_value().value_or(Utf8View{}).bytes() ==
                 other.text_value().value_or(Utf8View{}).bytes();
        case SqlValueType::kBlob:
          return std::ranges::equal(literal->value.blob_value().value_or(ByteView{}),
                                    other.blob_value().value_or(ByteView{}));
      }
    }
    if (const auto* parameter = std::get_if<BoundParameterExpression>(&left.payload);
        parameter != nullptr) {
      return parameter->parameter == std::get<BoundParameterExpression>(right.payload).parameter;
    }
    if (const auto* unary = std::get_if<BoundUnaryExpression>(&left.payload); unary != nullptr) {
      const auto& other = std::get<BoundUnaryExpression>(right.payload);
      return unary->operation == other.operation &&
             BoundSemanticallyEquivalent(unary->operand, other.operand);
    }
    if (const auto* binary = std::get_if<BoundBinaryExpression>(&left.payload); binary != nullptr) {
      const auto& other = std::get<BoundBinaryExpression>(right.payload);
      return binary->operation == other.operation &&
             BoundSemanticallyEquivalent(binary->left, other.left) &&
             BoundSemanticallyEquivalent(binary->right, other.right);
    }
    if (const auto* comparison = std::get_if<BoundComparisonExpression>(&left.payload);
        comparison != nullptr) {
      const auto& other = std::get<BoundComparisonExpression>(right.payload);
      return comparison->comparison == other.comparison &&
             comparison->collation == other.collation &&
             BoundSemanticallyEquivalent(comparison->left, other.left) &&
             BoundSemanticallyEquivalent(comparison->right, other.right);
    }
    if (const auto* truth = std::get_if<BoundTruthTestExpression>(&left.payload);
        truth != nullptr) {
      const auto& other = std::get<BoundTruthTestExpression>(right.payload);
      return truth->expected == other.expected && truth->negated == other.negated &&
             BoundSemanticallyEquivalent(truth->operand, other.operand);
    }
    if (const auto* call = std::get_if<BoundScalarCallExpression>(&left.payload); call != nullptr) {
      const auto& other = std::get<BoundScalarCallExpression>(right.payload);
      return call->function == other.function && call->collation == other.collation &&
             BoundArgumentsEquivalent(call->arguments, other.arguments);
    }
    if (const auto* call = std::get_if<BoundAggregateCallExpression>(&left.payload);
        call != nullptr) {
      const auto& other = std::get<BoundAggregateCallExpression>(right.payload);
      const BoundAggregateInvocation* left_invocation =
          AggregateInvocationForExpression(left_id, *call);
      const BoundAggregateInvocation* right_invocation =
          AggregateInvocationForExpression(right_id, other);
      return left_invocation != nullptr && right_invocation != nullptr &&
             AggregateInvocationsEquivalent(*left_invocation, *right_invocation);
    }
    if (const auto* coalesce = std::get_if<BoundCoalesceExpression>(&left.payload);
        coalesce != nullptr) {
      return BoundArgumentsEquivalent(coalesce->arguments,
                                      std::get<BoundCoalesceExpression>(right.payload).arguments);
    }
    if (const auto* conditional = std::get_if<BoundConditionalExpression>(&left.payload);
        conditional != nullptr) {
      return BoundArgumentsEquivalent(
          conditional->arguments, std::get<BoundConditionalExpression>(right.payload).arguments);
    }
    if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&left.payload);
        likelihood != nullptr) {
      const auto& other = std::get<BoundLikelihoodExpression>(right.payload);
      return likelihood->probability == other.probability &&
             BoundSemanticallyEquivalent(likelihood->operand, other.operand);
    }
    const auto& collate = std::get<BoundCollateExpression>(left.payload);
    const auto& other = std::get<BoundCollateExpression>(right.payload);
    return collate.collation == other.collation &&
           BoundSemanticallyEquivalent(collate.operand, other.operand);
  }

  [[nodiscard]] bool BoundArgumentsEquivalent(std::span<const BoundExpressionId> left,
                                              std::span<const BoundExpressionId> right) const {
    if (left.size() != right.size()) {
      return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
      if (!BoundSemanticallyEquivalent(left[index], right[index])) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] BindExpected<void> BindOrderBy(const SelectStatement& select) {
    active_aggregate_phase_ = AggregateOccurrencePhase::kOrderBy;
    impl_->order_by.reserve(select.order_by.size());
    for (std::size_t term_index = 0; term_index < select.order_by.size(); ++term_index) {
      const OrderingTerm& term = select.order_by[term_index];
      BindExpected<std::optional<std::size_t>> alias = OrderByAlias(term.expression);
      if (!alias.has_value()) {
        return std::unexpected(std::move(alias.error()));
      }
      std::optional<std::size_t> result_column = *alias;
      if (!result_column.has_value()) {
        BindExpected<std::optional<std::int64_t>> ordinal = OrderByOrdinal(term.expression);
        if (!ordinal.has_value()) {
          return std::unexpected(std::move(ordinal.error()));
        }
        if (ordinal->has_value()) {
          if (**ordinal < 1 || std::cmp_greater(**ordinal, impl_->result_columns.size())) {
            return std::unexpected(
                BinderError(BindErrorCode::kOrderByTermOutOfRange, term.span,
                            std::to_string(term_index + 1U) +
                                " ORDER BY term out of range - should be between 1 and " +
                                std::to_string(impl_->result_columns.size())));
          }
          result_column = static_cast<std::size_t>(**ordinal - 1);
        }
      }

      BindExpected<BoundExpressionId> expression =
          result_column.has_value()
              ? BindOrderByResultReference(term.expression, *result_column)
              : BindExpression(term.expression, BindScope{
                                                    .source_columns = true,
                                                    .result_aliases = true,
                                                    .allow_aggregates = active_core_is_aggregate_,
                                                });
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      if (!result_column.has_value()) {
        for (std::size_t index = impl_->result_columns.size(); index > 0U; --index) {
          const std::optional<ExpressionId>& syntax = impl_->result_syntax[index - 1U];
          const BoundExpressionId result_expression = impl_->result_columns[index - 1U].expression;
          const bool syntax_matches =
              syntax.has_value() && SyntaxExpressionsEquivalent(term.expression, syntax.value());
          const bool wildcard_column =
              !syntax.has_value() && (std::holds_alternative<BoundColumnExpression>(
                                          impl_->expressions[result_expression.value()].payload) ||
                                      std::holds_alternative<BoundRowIdExpression>(
                                          impl_->expressions[result_expression.value()].payload));
          if ((syntax_matches || wildcard_column) &&
              BoundSemanticallyEquivalent(*expression, result_expression)) {
            result_column = index - 1U;
            break;
          }
        }
      }

      const BoundExpressionProperties& properties =
          impl_->expressions[expression->value()].properties;
      BindExpected<BoundCollationId> collation =
          properties.collation.has_value() ? BindExpected<BoundCollationId>{*properties.collation}
                                           : InternCollation("BINARY", term.span);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      BindExpected<void> required = RequireCollation(*collation, term.span);
      if (!required.has_value()) {
        return std::unexpected(std::move(required.error()));
      }
      const SortOrder order =
          term.order == SortOrder::kDescending ? SortOrder::kDescending : SortOrder::kAscending;
      const BoundNullPlacement null_placement =
          term.null_order == NullOrder::kFirst  ? BoundNullPlacement::kFirst
          : term.null_order == NullOrder::kLast ? BoundNullPlacement::kLast
          : order == SortOrder::kDescending     ? BoundNullPlacement::kLast
                                                : BoundNullPlacement::kFirst;
      impl_->order_by.emplace_back(BoundOrderingTerm{
          .expression = *expression,
          .collation = *collation,
          .order = order,
          .null_placement = null_placement,
          .result_column = result_column,
          .explicit_collation = OrderByHasExplicitCollation(term.expression),
      });
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
    if (const auto* rowid = std::get_if<BoundRowIdExpression>(&bound.payload); rowid != nullptr) {
      const auto owner = std::ranges::find(source_states_, rowid->source, &SourceState::id);
      if (owner != source_states_.end()) {
        const std::optional<TableId>& table_id = owner->table;
        if (table_id.has_value()) {
          const CatalogTable& table = catalog_->table(*table_id);
          if (table.rowid_alias.has_value() &&
              table.rowid_alias->value < owner->source_column_count) {
            const BoundSourceColumn& source =
                impl_->source_columns[owner->source_column_begin + table.rowid_alias->value];
            result.name = std::string{source.name};
            if (source.declared_type.has_value()) {
              result.declared_type = std::string{*source.declared_type};
            }
            return;
          }
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
    if (source_states_.empty()) {
      return std::unexpected(
          BinderError(BindErrorCode::kNoTablesSpecified, span, "no tables specified"));
    }
    std::optional<DecodedNameParts> qualifier;
    if (wildcard.qualifier.has_value()) {
      BindExpected<DecodedNameParts> parts = NameParts(*wildcard.qualifier);
      if (!parts.has_value()) {
        return std::unexpected(std::move(parts.error()));
      }
      if (!std::ranges::any_of(source_states_, [&](const SourceState& source) {
            return WildcardQualifierMatches(source, *parts);
          })) {
        const std::string detail = parts->empty() ? std::string{SpanText(wildcard.qualifier->span)}
                                                  : std::string{parts->back()};
        return std::unexpected(BinderError(BindErrorCode::kNoSuchTable, wildcard.qualifier->span,
                                           "no such table: " + detail));
      }
      qualifier = std::move(*parts);
    }
    std::size_t expanded_count = 0;
    for (const SourceState& source : source_states_) {
      if (!qualifier.has_value() || WildcardQualifierMatches(source, *qualifier)) {
        const std::size_t end = source.source_column_begin + source.source_column_count;
        for (std::size_t index = source.source_column_begin; index < end; ++index) {
          if (qualifier.has_value() || !impl_->source_columns[index].merged_into.has_value()) {
            ++expanded_count;
          }
        }
      }
    }
    if (impl_->result_columns.size() > options_.maximum_result_columns ||
        expanded_count > options_.maximum_result_columns - impl_->result_columns.size()) {
      return std::unexpected(BinderError(BindErrorCode::kResultColumnLimitExceeded, span,
                                         "too many columns in result set"));
    }

    for (const SourceState& owner : source_states_) {
      if (qualifier.has_value() && !WildcardQualifierMatches(owner, *qualifier)) {
        continue;
      }
      const std::size_t source_end = owner.source_column_begin + owner.source_column_count;
      for (std::size_t index = owner.source_column_begin; index < source_end; ++index) {
        const BoundSourceColumn& column = impl_->source_columns[index];
        if (!qualifier.has_value() && column.merged_into.has_value()) {
          continue;
        }
        const BoundSourceColumnId column_id{static_cast<std::uint32_t>(index)};
        std::string_view source_name =
            owner.schema_table ? std::string_view{"sqlite_master"} : std::string_view{owner.name};
        const std::optional<std::string>& alias = owner.alias;
        if (alias.has_value()) {
          source_name = *alias;
        }
        auto resolved = ResolveSourceName(
            column.name, span,
            SourceQualification{.schema = catalog_->schema_name(), .name = source_name});
        if (!resolved.has_value()) {
          return std::unexpected(std::move(resolved.error()));
        }
        if (!resolved->has_value()) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                             "expanded wildcard column did not resolve"));
        }
        const BoundExpressionId expression = **resolved;
        const auto* bound_column =
            std::get_if<BoundColumnExpression>(&impl_->expressions[expression.value()].payload);
        if (bound_column == nullptr || JoinedColumnRepresentative(bound_column->column) !=
                                           JoinedColumnRepresentative(column_id)) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                             "expanded wildcard column has a different owner"));
        }
        BoundResultColumn result{
            .expression = expression,
            .affinity = Properties(expression).affinity,
        };
        PopulateImplicitResultMetadata(expression, span, result);
        impl_->result_columns.push_back(std::move(result));
        impl_->result_syntax.emplace_back(std::nullopt);
      }
    }
    return {};
  }

  [[nodiscard]] bool WildcardQualifierMatches(const SourceState& source,
                                              const DecodedNameParts& parts) const noexcept {
    if (parts.empty() || parts.size() > 2U) {
      return false;
    }
    if (parts.size() == 2U && !NamesEqual(parts.front(), catalog_->schema_name())) {
      return false;
    }
    const std::string_view qualifier = parts.back();
    if (source.alias.has_value()) {
      return NamesEqual(qualifier, *source.alias);
    }
    if (source.schema_table) {
      return NamesEqual(qualifier, "sqlite_master");
    }
    return NamesEqual(qualifier, source.name);
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
    if (parts->size() < 2U || parts->size() > 3U || !scope.source_columns) {
      return NoSuchColumn(identifier.name.span);
    }
    auto resolved = ResolveSourceName(
        parts->back(), identifier.name.span,
        SourceQualification{
            .schema = parts->size() == 3U ? std::optional<std::string_view>{parts->front()}
                                          : std::nullopt,
            .name = (*parts)[parts->size() - 2U],
        });
    if (!resolved.has_value()) {
      return std::unexpected(std::move(resolved.error()));
    }
    if (resolved->has_value()) {
      return **resolved;
    }
    return NoSuchColumn(identifier.name.span);
  }

  [[nodiscard]] BindExpected<std::optional<BoundExpressionId>> ResolveSingleName(
      std::string_view name, SourceSpan span, BindScope scope) {
    if (scope.source_columns) {
      auto resolved = ResolveSourceName(name, span, std::nullopt);
      if (!resolved.has_value() || resolved->has_value()) {
        return resolved;
      }
    }
    if (scope.result_aliases) {
      for (const AliasEntry& alias : aliases_) {
        if (NamesEqual(alias.name, name)) {
          const ExpressionProperties properties = Properties(alias.target);
          if (properties.contains_aggregate && !scope.allow_aggregates) {
            return std::unexpected(
                BinderError(BindErrorCode::kAggregateMisuse, span,
                            "misuse of aggregate function through result alias"));
          }
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

  [[nodiscard]] bool QualifiedSourceMatches(
      const SourceState& source, const SourceQualification& qualification) const noexcept {
    if (qualification.schema.has_value() &&
        !NamesEqual(*qualification.schema, catalog_->schema_name())) {
      return false;
    }
    const std::string_view qualifier = qualification.name;
    if (source.alias.has_value()) {
      return NamesEqual(qualifier, *source.alias);
    }
    if (source.schema_table) {
      return IsSchemaTableName(qualifier);
    }
    return NamesEqual(qualifier, source.name);
  }

  [[nodiscard]] std::optional<BoundSourceColumnId> FindSourceColumn(
      const SourceState& source, std::string_view name) const noexcept {
    if (source.table.has_value()) {
      const std::optional<ColumnId> column = catalog_->FindColumn(*source.table, name);
      if (!column.has_value() ||
          column->value > std::numeric_limits<std::size_t>::max() - source.source_column_begin) {
        return std::nullopt;
      }
      const std::size_t index = source.source_column_begin + column->value;
      if (index > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
      }
      return BoundSourceColumnId{static_cast<std::uint32_t>(index)};
    }
    const std::size_t source_end = source.source_column_begin + source.source_column_count;
    for (std::size_t index = source.source_column_begin; index < source_end; ++index) {
      if (NamesEqual(impl_->source_columns[index].name, name)) {
        return BoundSourceColumnId{static_cast<std::uint32_t>(index)};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool RowIdAvailable(const SourceState& source) const noexcept {
    return source.schema_table ||
           (source.table.has_value() && !catalog_->table(*source.table).without_rowid);
  }

  [[nodiscard]] BindExpected<std::optional<BoundExpressionId>> ResolveSourceName(
      std::string_view name, SourceSpan span, std::optional<SourceQualification> qualification) {
    std::optional<BoundSourceColumnId> matched_column;
    const auto matches = [&](const SourceState& source) {
      return !qualification.has_value() || QualifiedSourceMatches(source, *qualification);
    };
    const auto ambiguous = [&]() {
      std::string resolved_name;
      if (qualification.has_value()) {
        if (qualification->schema.has_value()) {
          resolved_name.append(*qualification->schema).push_back('.');
        }
        resolved_name.append(qualification->name).push_back('.');
      }
      resolved_name.append(name);
      return BinderError(BindErrorCode::kAmbiguousColumn, span,
                         "ambiguous column name: " + resolved_name);
    };
    for (const SourceState& source : source_states_) {
      if (!matches(source)) {
        continue;
      }
      if (const auto column = FindSourceColumn(source, name); column.has_value()) {
        if (!qualification.has_value() &&
            impl_->source_columns[column->value()].merged_into.has_value()) {
          continue;
        }
        if (matched_column.has_value()) {
          if (JoinedColumnRepresentative(*matched_column) != JoinedColumnRepresentative(*column)) {
            return std::unexpected(ambiguous());
          }
          continue;
        }
        matched_column = *column;
      }
    }
    if (matched_column.has_value()) {
      auto bound = BindColumn(*matched_column, span);
      if (!bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      return std::optional{*bound};
    }
    if (IsRowIdName(name)) {
      std::optional<BoundSourceId> matched_rowid;
      for (const SourceState& source : source_states_) {
        if (matches(source) && RowIdAvailable(source)) {
          if (matched_rowid.has_value()) {
            return std::unexpected(ambiguous());
          }
          matched_rowid = source.id;
        }
      }
      if (matched_rowid.has_value()) {
        auto bound = BindRowId(*matched_rowid, span);
        if (!bound.has_value()) {
          return std::unexpected(std::move(bound.error()));
        }
        return std::optional{*bound};
      }
    }
    return std::optional<BoundExpressionId>{};
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindColumn(BoundSourceColumnId id,
                                                           SourceSpan span) {
    const BoundSourceColumn& column = impl_->source_columns[id.value()];
    const std::string_view collation_name =
        IsActiveRowIdAlias(id) ? std::string_view{"BINARY"} : column.collation_name;
    BindExpected<std::optional<BoundCollationId>> collation =
        InternOptionalCollation(collation_name, span);
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

  [[nodiscard]] BindExpected<BoundExpressionId> BindRowId(BoundSourceId source, SourceSpan span) {
    return AppendExpression(span, BoundRowIdExpression{.source = source},
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
    properties.contains_aggregate = child.contains_aggregate;
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

  [[nodiscard]] BindExpected<BoundExpressionId> AppendBoundComparison(SqlComparison comparison,
                                                                      BoundExpressionId left,
                                                                      BoundExpressionId right,
                                                                      SourceSpan span) {
    auto collation = SelectComparisonCollation(std::array{left, right}, span);
    if (!collation.has_value()) {
      return std::unexpected(std::move(collation.error()));
    }
    return AppendExpression(span,
                            BoundComparisonExpression{
                                .comparison = comparison,
                                .affinity = SelectComparisonAffinity(Properties(left).affinity,
                                                                     Properties(right).affinity),
                                .collation = *collation,
                                .left = left,
                                .right = right,
                            },
                            PropagatedExplicitProperties(std::array{left, right}));
  }

  [[nodiscard]] BindExpected<BoundExpressionId> AppendBoundBinary(BoundBinaryOperation operation,
                                                                  BoundExpressionId left,
                                                                  BoundExpressionId right,
                                                                  SourceSpan span) {
    return AppendExpression(span,
                            BoundBinaryExpression{
                                .operation = operation,
                                .left = left,
                                .right = right,
                            },
                            PropagatedExplicitProperties(std::array{left, right}));
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
      return AppendBoundComparison(ToSqlComparison(binary.op), *left, *right, span);
    }
    return AppendBoundBinary(ToBoundBinary(binary.op), *left, *right, span);
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

  [[nodiscard]] BindExpected<BoundOrderingTerm> BindAggregateOrderingTerm(const OrderingTerm& term,
                                                                          BindScope scope) {
    scope.allow_aggregates = false;
    BindExpected<BoundExpressionId> expression = BindExpression(term.expression, scope);
    if (!expression.has_value()) {
      return std::unexpected(std::move(expression.error()));
    }
    const ExpressionProperties properties = Properties(*expression);
    BindExpected<BoundCollationId> collation =
        properties.collation.has_value() ? BindExpected<BoundCollationId>{*properties.collation}
                                         : InternCollation("BINARY", term.span);
    if (!collation.has_value()) {
      return std::unexpected(std::move(collation.error()));
    }
    BindExpected<void> required = RequireCollation(*collation, term.span);
    if (!required.has_value()) {
      return std::unexpected(std::move(required.error()));
    }
    const SortOrder order =
        term.order == SortOrder::kDescending ? SortOrder::kDescending : SortOrder::kAscending;
    const BoundNullPlacement null_placement =
        term.null_order == NullOrder::kFirst  ? BoundNullPlacement::kFirst
        : term.null_order == NullOrder::kLast ? BoundNullPlacement::kLast
        : order == SortOrder::kDescending     ? BoundNullPlacement::kLast
                                              : BoundNullPlacement::kFirst;
    return BoundOrderingTerm{
        .expression = *expression,
        .collation = *collation,
        .order = order,
        .null_placement = null_placement,
        .result_column = std::nullopt,
        .explicit_collation = OrderByHasExplicitCollation(term.expression),
    };
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindAggregateCall(
      const AggregateFunction& function, const FunctionCallExpression& call, SourceSpan span,
      BindScope scope, bool has_wildcard) {
    BindScope argument_scope = scope;
    argument_scope.allow_aggregates = false;
    std::vector<BoundExpressionId> arguments;
    if (!has_wildcard) {
      BindExpected<std::vector<BoundExpressionId>> bound =
          BindArguments(call.arguments, argument_scope);
      if (!bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      arguments = std::move(*bound);
    }
    if (!scope.allow_aggregates) {
      return std::unexpected(
          BinderError(BindErrorCode::kAggregateMisuse, span,
                      "misuse of aggregate function " + std::string{function.name()} + "()"));
    }

    std::vector<BoundOrderingTerm> order_by;
    if (!arguments.empty()) {
      order_by.reserve(call.order_by.size());
      for (const OrderingTerm& term : call.order_by) {
        BindExpected<BoundOrderingTerm> bound = BindAggregateOrderingTerm(term, argument_scope);
        if (!bound.has_value()) {
          return std::unexpected(std::move(bound.error()));
        }
        order_by.push_back(*bound);
      }
    }

    std::optional<BoundExpressionId> filter;
    if (call.filter.has_value()) {
      BindScope filter_scope = scope;
      filter_scope.allow_aggregates = false;
      BindExpected<BoundExpressionId> bound = BindExpression(*call.filter, filter_scope);
      if (!bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      filter = *bound;
    }

    if (call.distinct && arguments.size() != 1U) {
      return std::unexpected(BinderError(BindErrorCode::kWrongFunctionArity, span,
                                         "DISTINCT aggregates must have exactly one argument"));
    }
    const bool argument_reuses_single_order_key =
        arguments.size() == 1U && order_by.size() == 1U &&
        BoundSemanticallyEquivalent(arguments.front(), order_by.front().expression);

    BindExpected<BoundAggregateFunctionId> bound_function = InternAggregateFunction(function, span);
    if (!bound_function.has_value()) {
      return std::unexpected(std::move(bound_function.error()));
    }
    const bool requires_argument_collation =
        (function.uses_collation() || call.distinct) && !arguments.empty();
    BindExpected<BoundCollationId> collation = requires_argument_collation
                                                   ? SelectFunctionCollation(arguments, span)
                                                   : InternCollation("BINARY", span);
    if (!collation.has_value()) {
      return std::unexpected(std::move(collation.error()));
    }
    if (requires_argument_collation) {
      BindExpected<void> required = RequireCollation(*collation, span);
      if (!required.has_value()) {
        return std::unexpected(std::move(required.error()));
      }
    }
    if (pending_aggregates_.size() >= std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "pending aggregate invocation ID overflow"));
    }
    ExpressionProperties properties = PropagatedExplicitProperties(arguments);
    properties.affinity = TypeAffinity::kNone;
    properties.contains_aggregate = true;
    const BoundAggregateInvocationId pending_id{
        static_cast<std::uint32_t>(pending_aggregates_.size())};
    BindExpected<BoundExpressionId> expression =
        AppendExpression(span, BoundAggregateCallExpression{.invocation = pending_id}, properties);
    if (!expression.has_value()) {
      return std::unexpected(std::move(expression.error()));
    }
    pending_aggregates_.push_back(PendingAggregateInvocation{
        .expression = *expression,
        .invocation =
            BoundAggregateInvocation{
                .span = span,
                .function = *bound_function,
                .collation = *collation,
                .arguments = std::move(arguments),
                .distinct = call.distinct,
                .order_by = std::move(order_by),
                .filter = filter,
                .argument_reuses_single_order_key = argument_reuses_single_order_key,
            },
        .phase = active_aggregate_phase_,
    });
    return *expression;
  }

  [[nodiscard]] BindExpected<void> BindArgumentsBeforeFunctionError(
      const FunctionCallExpression& call, BindScope scope, bool has_wildcard) {
    if (has_wildcard) {
      return {};
    }
    BindExpected<std::vector<BoundExpressionId>> arguments = BindArguments(call.arguments, scope);
    if (!arguments.has_value()) {
      return std::unexpected(std::move(arguments.error()));
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> RejectScalarModifiers(const FunctionCallExpression& call,
                                                         SourceSpan span, std::string_view name,
                                                         std::size_t argument_count) const {
    if (call.filter.has_value()) {
      return std::unexpected(
          BinderError(BindErrorCode::kAggregateMisuse, span,
                      "FILTER may not be used with non-aggregate " + std::string{name} + "()"));
    }
    if (!call.order_by.empty() && argument_count != 0U) {
      return std::unexpected(
          BinderError(BindErrorCode::kAggregateMisuse, span,
                      "ORDER BY may not be used with non-aggregate " + std::string{name} + "()"));
    }
    return {};
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
    const bool has_wildcard = std::ranges::any_of(call.arguments, [this](ExpressionId id) {
      return std::holds_alternative<WildcardExpression>(tree_.expression(id).payload);
    });
    const std::size_t argument_count = has_wildcard ? 0U : call.arguments.size();
    if (argument_count > options_.maximum_function_arguments) {
      return std::unexpected(BinderError(BindErrorCode::kFunctionArgumentLimitExceeded, span,
                                         "too many arguments on function " + std::string{name}));
    }

    const Result<const ScalarFunction*> resolved =
        environment_.functions().Resolve(name, argument_count);
    if (resolved.has_value()) {
      std::vector<BoundExpressionId> arguments;
      if (!has_wildcard) {
        BindExpected<std::vector<BoundExpressionId>> bound = BindArguments(call.arguments, scope);
        if (!bound.has_value()) {
          return std::unexpected(std::move(bound.error()));
        }
        arguments = std::move(*bound);
      }
      BindExpected<void> modifiers =
          RejectScalarModifiers(call, span, (*resolved)->name(), argument_count);
      if (!modifiers.has_value()) {
        return std::unexpected(std::move(modifiers.error()));
      }
      const ExpressionProperties properties = PropagatedExplicitProperties(arguments);
      BindExpected<BoundFunctionId> function = InternFunction(**resolved, span);
      if (!function.has_value()) {
        return std::unexpected(std::move(function.error()));
      }
      BindExpected<BoundCollationId> collation = (*resolved)->uses_collation()
                                                     ? SelectFunctionCollation(arguments, span)
                                                     : InternCollation("BINARY", span);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      return AppendExpression(span,
                              BoundScalarCallExpression{
                                  .function = *function,
                                  .collation = *collation,
                                  .arguments = std::move(arguments),
                              },
                              properties);
    }

    if (NamesEqual(name, "coalesce") || NamesEqual(name, "ifnull")) {
      const bool valid =
          NamesEqual(name, "ifnull") ? call.arguments.size() == 2U : call.arguments.size() >= 2U;
      if (!valid) {
        BindExpected<void> arguments = BindArgumentsBeforeFunctionError(call, scope, has_wildcard);
        if (!arguments.has_value()) {
          return std::unexpected(std::move(arguments.error()));
        }
        return WrongArity(call.name.span, name);
      }
      BindExpected<std::vector<BoundExpressionId>> arguments = BindArguments(call.arguments, scope);
      if (!arguments.has_value()) {
        return std::unexpected(std::move(arguments.error()));
      }
      BindExpected<void> modifiers = RejectScalarModifiers(call, span, name, call.arguments.size());
      if (!modifiers.has_value()) {
        return std::unexpected(std::move(modifiers.error()));
      }
      const ExpressionProperties properties = PropagatedExplicitProperties(*arguments);
      return AppendExpression(span, BoundCoalesceExpression{.arguments = std::move(*arguments)},
                              properties);
    }
    if (NamesEqual(name, "iif") || NamesEqual(name, "if")) {
      if (call.arguments.size() < 2U) {
        BindExpected<void> arguments = BindArgumentsBeforeFunctionError(call, scope, has_wildcard);
        if (!arguments.has_value()) {
          return std::unexpected(std::move(arguments.error()));
        }
        return WrongArity(call.name.span, name);
      }
      BindExpected<std::vector<BoundExpressionId>> arguments = BindArguments(call.arguments, scope);
      if (!arguments.has_value()) {
        return std::unexpected(std::move(arguments.error()));
      }
      BindExpected<void> modifiers = RejectScalarModifiers(call, span, name, call.arguments.size());
      if (!modifiers.has_value()) {
        return std::unexpected(std::move(modifiers.error()));
      }
      const ExpressionProperties properties = PropagatedExplicitProperties(*arguments);
      return AppendExpression(span, BoundConditionalExpression{.arguments = std::move(*arguments)},
                              properties);
    }
    if (NamesEqual(name, "likely") || NamesEqual(name, "unlikely")) {
      if (argument_count != 1U) {
        BindExpected<void> arguments = BindArgumentsBeforeFunctionError(call, scope, has_wildcard);
        if (!arguments.has_value()) {
          return std::unexpected(std::move(arguments.error()));
        }
        return WrongArity(call.name.span, name);
      }
      BindExpected<BoundExpressionId> operand = BindExpression(call.arguments.front(), scope);
      if (!operand.has_value()) {
        return std::unexpected(std::move(operand.error()));
      }
      BindExpected<void> modifiers = RejectScalarModifiers(call, span, name, call.arguments.size());
      if (!modifiers.has_value()) {
        return std::unexpected(std::move(modifiers.error()));
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
        BindExpected<void> arguments = BindArgumentsBeforeFunctionError(call, scope, has_wildcard);
        if (!arguments.has_value()) {
          return std::unexpected(std::move(arguments.error()));
        }
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
      BindExpected<void> modifiers = RejectScalarModifiers(call, span, name, call.arguments.size());
      if (!modifiers.has_value()) {
        return std::unexpected(std::move(modifiers.error()));
      }
      return AppendExpression(span,
                              BoundLikelihoodExpression{
                                  .operand = *operand,
                                  .probability = parsed,
                              },
                              PropagatedExplicitProperties(std::array{*operand}));
    }

    const Result<const AggregateFunction*> aggregate =
        environment_.functions().ResolveAggregate(name, argument_count);
    if (aggregate.has_value()) {
      return BindAggregateCall(**aggregate, call, span, scope, has_wildcard);
    }
    if (FunctionNameExists(name) || AggregateFunctionNameExists(name) ||
        NamesEqual(name, "coalesce") || NamesEqual(name, "ifnull") || NamesEqual(name, "iif") ||
        NamesEqual(name, "if") || NamesEqual(name, "likely") || NamesEqual(name, "unlikely") ||
        NamesEqual(name, "likelihood")) {
      BindExpected<void> arguments = BindArgumentsBeforeFunctionError(call, scope, has_wildcard);
      if (!arguments.has_value()) {
        return std::unexpected(std::move(arguments.error()));
      }
      return WrongArity(call.name.span, name);
    }
    BindExpected<void> arguments = BindArgumentsBeforeFunctionError(call, scope, has_wildcard);
    if (!arguments.has_value()) {
      return std::unexpected(std::move(arguments.error()));
    }
    return std::unexpected(BinderError(BindErrorCode::kNoSuchFunction, call.name.span,
                                       "no such function: " + std::string{name}));
  }

  [[nodiscard]] bool FunctionNameExists(std::string_view name) const noexcept {
    return std::ranges::any_of(
        environment_.functions().functions(),
        [name](const ScalarFunction& function) { return NamesEqual(function.name(), name); });
  }

  [[nodiscard]] bool AggregateFunctionNameExists(std::string_view name) const noexcept {
    return std::ranges::any_of(
        environment_.functions().aggregate_functions(),
        [name](const AggregateFunction& function) { return NamesEqual(function.name(), name); });
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
                .contains_aggregate = properties.contains_aggregate,
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
        .contains_aggregate = properties.contains_aggregate,
    };
  }

  [[nodiscard]] ExpressionProperties PropagatedExplicitProperties(
      std::span<const BoundExpressionId> expressions) const noexcept {
    ExpressionProperties result;
    for (const BoundExpressionId expression : expressions) {
      const ExpressionProperties properties = Properties(expression);
      result.contains_aggregate = result.contains_aggregate || properties.contains_aggregate;
      if (!result.explicit_collation && properties.explicit_collation) {
        result.collation = properties.collation;
        result.explicit_collation = true;
      }
    }
    return result;
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

  [[nodiscard]] BindExpected<BoundAggregateFunctionId> InternAggregateFunction(
      const AggregateFunction& function, SourceSpan span) {
    const BoundAggregateExtremumKind extremum = ToBoundExtremum(function.extremum_kind());
    for (std::size_t index = 0; index < impl_->aggregate_functions.size(); ++index) {
      const BoundAggregateFunction& existing = impl_->aggregate_functions[index];
      if (NamesEqual(existing.name, function.name()) &&
          existing.deterministic == function.is_deterministic() &&
          existing.uses_collation == function.uses_collation() && existing.extremum == extremum) {
        return BoundAggregateFunctionId{static_cast<std::uint32_t>(index)};
      }
    }
    if (impl_->aggregate_functions.size() >= std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "bound aggregate function ID overflow"));
    }
    const BoundAggregateFunctionId id{
        static_cast<std::uint32_t>(impl_->aggregate_functions.size())};
    impl_->aggregate_functions.push_back(BoundAggregateFunction{
        .name = std::string{function.name()},
        .deterministic = function.is_deterministic(),
        .uses_collation = function.uses_collation(),
        .extremum = extremum,
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
  std::vector<SourceState> source_states_;
  std::vector<AliasEntry> aliases_;
  std::size_t active_source_begin_ = 0;
  std::size_t active_source_count_ = 0;
  std::uint32_t next_source_id_ = 0;
  AggregateOccurrencePhase active_aggregate_phase_ = AggregateOccurrencePhase::kResult;
  bool active_core_is_aggregate_ = false;
  std::vector<BoundGroupingTerm> active_group_by_;
  std::optional<BoundExpressionId> active_having_;
  std::vector<PendingAggregateInvocation> pending_aggregates_;
  std::vector<CoreBindingMetadata> core_metadata_;
  std::vector<std::optional<BoundParameterId>> parameter_bindings_;
};

}  // namespace binder_detail

std::optional<BoundSourceLeafIdentity> BoundSourceLeafIdentityOf(
    const BoundExpression& expression, std::span<const BoundSourceColumn> source_columns) noexcept {
  if (const auto* column = std::get_if<BoundColumnExpression>(&expression.payload);
      column != nullptr) {
    if (column->column.value() >= source_columns.size()) {
      std::terminate();
    }
    const BoundSourceColumn& source = source_columns[column->column.value()];
    return BoundSourceLeafIdentity{
        .source = source.source,
        .column =
            source.rowid_alias ? std::nullopt : std::optional<BoundSourceColumnId>{column->column},
    };
  }
  if (const auto* rowid = std::get_if<BoundRowIdExpression>(&expression.payload);
      rowid != nullptr) {
    return BoundSourceLeafIdentity{.source = rowid->source};
  }
  return std::nullopt;
}

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
    case BoundExpressionKind::kAggregateCall:
      return "aggregate_call";
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
    case BindErrorCode::kAggregateMisuse:
      return "aggregate_misuse";
    case BindErrorCode::kHavingWithoutAggregate:
      return "having_without_aggregate";
    case BindErrorCode::kNoSuchCollation:
      return "no_such_collation";
    case BindErrorCode::kOrderByTermOutOfRange:
      return "order_by_term_out_of_range";
    case BindErrorCode::kGroupByTermOutOfRange:
      return "group_by_term_out_of_range";
    case BindErrorCode::kCompoundOrderByTerm:
      return "compound_order_by_term";
    case BindErrorCode::kCompoundColumnCount:
      return "compound_column_count";
    case BindErrorCode::kValuesColumnCount:
      return "values_column_count";
    case BindErrorCode::kInvalidLiteral:
      return "invalid_literal";
    case BindErrorCode::kInvalidVariableNumber:
      return "invalid_variable_number";
    case BindErrorCode::kParameterLimitExceeded:
      return "parameter_limit_exceeded";
    case BindErrorCode::kFunctionArgumentLimitExceeded:
      return "function_argument_limit_exceeded";
    case BindErrorCode::kAggregateTermLimitExceeded:
      return "aggregate_term_limit_exceeded";
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
    case BindErrorCode::kJoinConditionScope:
      return "join_condition_scope";
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
    case BindErrorCode::kAggregateTermLimitExceeded:
    case BindErrorCode::kResultColumnLimitExceeded:
      return ErrorCode::kTooLarge;
    case BindErrorCode::kOrderByTermOutOfRange:
    case BindErrorCode::kGroupByTermOutOfRange:
    case BindErrorCode::kCompoundOrderByTerm:
    case BindErrorCode::kCompoundColumnCount:
    case BindErrorCode::kValuesColumnCount:
      return ErrorCode::kGeneric;
    case BindErrorCode::kIndexedTableUnsupported:
      return ErrorCode::kProtocol;
    case BindErrorCode::kInternalInvariant:
      return ErrorCode::kInternal;
    case BindErrorCode::kUnsupportedFeature:
    case BindErrorCode::kNoSuchTable:
    case BindErrorCode::kNoSuchColumn:
    case BindErrorCode::kAmbiguousColumn:
    case BindErrorCode::kJoinConditionScope:
    case BindErrorCode::kNoTablesSpecified:
    case BindErrorCode::kNoSuchFunction:
    case BindErrorCode::kWrongFunctionArity:
    case BindErrorCode::kAggregateMisuse:
    case BindErrorCode::kHavingWithoutAggregate:
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

std::span<const BoundTableSource> BoundSelect::table_sources() const noexcept {
  if (impl_ == nullptr || impl_->query_cores.empty()) {
    return {};
  }
  const auto* core = std::get_if<BoundSelectCore>(&impl_->query_cores.front());
  return core != nullptr ? std::span<const BoundTableSource>{core->table_sources}
                         : std::span<const BoundTableSource>{};
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

std::span<const BoundAggregateFunction> BoundSelect::aggregate_functions() const noexcept {
  return impl_ != nullptr ? std::span<const BoundAggregateFunction>{impl_->aggregate_functions}
                          : std::span<const BoundAggregateFunction>{};
}

std::span<const BoundAggregateInvocation> BoundSelect::aggregate_invocations() const noexcept {
  return impl_ != nullptr ? std::span<const BoundAggregateInvocation>{impl_->aggregate_invocations}
                          : std::span<const BoundAggregateInvocation>{};
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

std::span<const BoundQueryCore> BoundSelect::query_cores() const noexcept {
  return impl_ != nullptr ? std::span<const BoundQueryCore>{impl_->query_cores}
                          : std::span<const BoundQueryCore>{};
}

std::span<const CompoundOperator> BoundSelect::compound_operators() const noexcept {
  return impl_ != nullptr ? std::span<const CompoundOperator>{impl_->compound_operators}
                          : std::span<const CompoundOperator>{};
}

std::span<const BoundCollationId> BoundSelect::compound_collations() const noexcept {
  return impl_ != nullptr ? std::span<const BoundCollationId>{impl_->compound_collations}
                          : std::span<const BoundCollationId>{};
}

std::span<const BoundResultColumn> BoundSelect::result_columns() const noexcept {
  return impl_ != nullptr ? std::span<const BoundResultColumn>{impl_->output_columns}
                          : std::span<const BoundResultColumn>{};
}

std::optional<BoundExpressionId> BoundSelect::where_expression() const noexcept {
  if (impl_ == nullptr || impl_->query_cores.empty()) {
    return std::nullopt;
  }
  const auto* core = std::get_if<BoundSelectCore>(&impl_->query_cores.front());
  return core != nullptr ? core->where : std::nullopt;
}

std::span<const BoundOrderingTerm> BoundSelect::order_by() const noexcept {
  return impl_ != nullptr ? std::span<const BoundOrderingTerm>{impl_->order_by}
                          : std::span<const BoundOrderingTerm>{};
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

BoundAnalyze::BoundAnalyze(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

BoundAnalyze::BoundAnalyze(BoundAnalyze&&) noexcept = default;

BoundAnalyze& BoundAnalyze::operator=(BoundAnalyze&&) noexcept = default;

BoundAnalyze::~BoundAnalyze() = default;

bool BoundAnalyze::valid() const noexcept { return impl_ != nullptr; }

Utf8View BoundAnalyze::source() const noexcept {
  return impl_ != nullptr ? Utf8View{impl_->source} : Utf8View{};
}

const CatalogSnapshot* BoundAnalyze::catalog() const noexcept {
  return impl_ != nullptr ? impl_->catalog.get() : nullptr;
}

CatalogVersion BoundAnalyze::required_catalog_version() const noexcept {
  return impl_ != nullptr && impl_->catalog != nullptr ? impl_->catalog->version()
                                                       : CatalogVersion{};
}

BoundAnalyzeScope BoundAnalyze::scope() const noexcept {
  return impl_ != nullptr ? impl_->scope : BoundAnalyzeScope::kDatabase;
}

std::string_view BoundAnalyze::scope_name() const noexcept {
  return impl_ != nullptr ? std::string_view{impl_->scope_name} : std::string_view{};
}

std::optional<RootPageId> BoundAnalyze::stat1_root_page() const noexcept {
  return impl_ != nullptr ? impl_->stat1_root_page : std::nullopt;
}

std::span<const BoundAnalyzeIndex> BoundAnalyze::indexes() const noexcept {
  return impl_ != nullptr ? std::span<const BoundAnalyzeIndex>{impl_->indexes}
                          : std::span<const BoundAnalyzeIndex>{};
}

std::span<const BoundAnalyzeTable> BoundAnalyze::tables() const noexcept {
  return impl_ != nullptr ? std::span<const BoundAnalyzeTable>{impl_->tables}
                          : std::span<const BoundAnalyzeTable>{};
}

BindStatementResult BindStatement(SyntaxTree tree, CatalogSnapshotPtr catalog,
                                  BindEnvironment environment, BindOptions options) {
  return binder_detail::StatementBinder(std::move(tree), std::move(catalog), environment, options)
      .RunStatement();
}

}  // namespace modern_sqlite
