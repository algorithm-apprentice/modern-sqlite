#include "modern_sqlite/binder/bound_select.hpp"

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

struct BoundSelect::Impl {
  std::string source;
  CatalogSnapshotPtr catalog;
  std::uint64_t registration_generation = 0;
  std::optional<BoundTableSource> table_source;
  std::vector<BoundSourceColumn> source_columns;
  std::vector<BoundCollation> collations;
  std::vector<BoundScalarFunction> functions;
  std::vector<BoundParameter> parameters;
  std::vector<BoundExpression> expressions;
  std::vector<BoundResultColumn> result_columns;
  std::optional<BoundExpressionId> where;
  std::optional<BoundLimit> limit;
};

namespace binder_detail {

class SelectBinder final {
 public:
  SelectBinder(SyntaxTree tree, CatalogSnapshotPtr catalog, BindEnvironment environment,
               BindOptions options)
      : tree_(std::move(tree)),
        catalog_(std::move(catalog)),
        environment_(environment),
        options_(options),
        impl_(std::make_unique<BoundSelect::Impl>()) {}

  [[nodiscard]] BindSelectResult Run() {
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

    impl_->source.assign(tree_.source().bytes());
    impl_->catalog = catalog_;
    impl_->registration_generation = environment_.registration_generation();
    parameter_bindings_.resize(tree_.expressions().size());

    BindExpected<void> parameters = AssignParameters();
    if (!parameters.has_value()) {
      return std::unexpected(std::move(parameters.error()));
    }
    BindExpected<void> source = BindSource(select);
    if (!source.has_value()) {
      return std::unexpected(std::move(source.error()));
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

 private:
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
          {});
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
    return AppendExpression(span, std::move(bound), {});
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
            {});
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
        return AppendExpression(span,
                                BoundComparisonExpression{
                                    .comparison = ToSqlComparison(binary.op),
                                    .affinity = TypeAffinity::kNone,
                                    .collation = *collation,
                                    .left = *left,
                                    .right = *right,
                                },
                                PropagatedExplicitProperties(std::array{*left, *right}));
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
  return binder_detail::SelectBinder(std::move(tree), std::move(catalog), environment, options)
      .Run();
}

}  // namespace modern_sqlite
