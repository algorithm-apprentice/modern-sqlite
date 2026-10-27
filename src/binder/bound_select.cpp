#include <algorithm>
#include <array>
#include <cassert>
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
namespace binder_detail {

struct ResultDefinition {
  std::optional<ExpressionId> syntax{};
  std::vector<ExpressionId> additional_syntax{};
  bool has_subqueries = false;
  bool value_checked = false;
};

}  // namespace binder_detail

namespace {

using binder_detail::ResultDefinition;

template <typename T>
using BindExpected = std::expected<T, BindError>;

struct ExpressionProperties {
  TypeAffinity affinity = TypeAffinity::kNone;
  std::optional<BoundCollationId> collation;
  bool explicit_collation = false;
  BoundTruthHint truth_hint = BoundTruthHint::kNone;
  bool contains_aggregate = false;
  bool references_aggregate = false;
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

enum class AggregatePermission : std::uint8_t {
  kProhibited,
  kAllowed,
  kGrouping,
  kOuterOnly,
  kDiscardedLocalOrder,
};

struct BindScope {
  bool source_columns = true;
  bool result_aliases = false;
  AggregatePermission aggregates = AggregatePermission::kProhibited;
  bool requires_value = true;
  bool requires_scalar_width = true;
};

struct AliasAggregatePermissions {
  AggregatePermission definition = AggregatePermission::kProhibited;
  AggregatePermission consumer = AggregatePermission::kProhibited;
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

enum class AggregateOccurrencePhase : std::uint8_t {
  kResult,
  kOrderBy,
  kHaving,
};

struct NameScopeView {
  BoundQueryId query{0};
  std::span<const SourceState> sources;
  std::span<const BoundTableSource> table_sources;
  std::span<const AliasEntry> aliases;
  std::span<ResultDefinition> result_definitions;
  const NameScopeView* outer = nullptr;
  BindScope scope{};
  std::size_t aggregate_core = 0;
  AggregateOccurrencePhase aggregate_phase = AggregateOccurrencePhase::kResult;
  AggregatePermission aggregate_permission = AggregatePermission::kProhibited;
  bool aggregate_was_established = false;
};

struct CoreBindingMetadata {
  std::vector<SourceState> source_states{};
  std::size_t source_column_begin = 0;
  std::size_t source_column_count = 0;
  std::vector<ResultDefinition> result_definitions{};
};

struct AggregateLeafAnalysisView {
  std::span<const BoundTableSource> table_sources;
  std::span<const BoundResultColumn> result_columns;
  std::optional<BoundExpressionId> where;
  std::span<const BoundOrderingTerm> order_by;
  bool order_by_uses_result_columns = false;
};

struct PendingAggregateInvocation {
  ExpressionId syntax;
  BoundExpressionId expression;
  BoundAggregateInvocation invocation;
  BoundQueryId owner_query{0};
  std::size_t owner_core = 0;
  BoundQueryId site_query{0};
  AggregateOccurrencePhase phase = AggregateOccurrencePhase::kResult;
  bool finalized = false;
};

struct DeferredAggregateReplay {
  ExpressionId syntax;
  NameScopeView definition_scope;
  BindScope scope;
  BoundQueryId owner_query{0};
  std::size_t owner_core = 0;
  AggregateOccurrencePhase owner_phase = AggregateOccurrencePhase::kResult;
  AggregatePermission owner_permission = AggregatePermission::kProhibited;
  bool owner_was_established = false;
  BoundQueryId consumer_query{0};
};

struct DiscardedOrderBinding {
  BoundQueryId query{0};
  std::size_t replay_begin = 0;
  bool rolls_back = false;
};

struct QueryBindingScope {
  BoundQueryId query{0};
  std::size_t core = 0;
  NameScopeView names;
  std::vector<BoundTableSource> table_sources;
  std::vector<BoundResultColumn> result_columns;
  std::vector<ResultDefinition> result_definitions;
  std::optional<BoundExpressionId> where;
  std::vector<SourceState> sources;
  std::vector<AliasEntry> aliases;
  std::size_t source_begin = 0;
  std::size_t source_count = 0;
  std::size_t condition_height = 0;
  AggregateOccurrencePhase aggregate_phase = AggregateOccurrencePhase::kResult;
  bool is_aggregate = false;
  std::vector<BoundGroupingTerm> group_by;
  std::optional<BoundExpressionId> having;
  const NameScopeView* outer = nullptr;
  bool binding_only = false;
};

struct BindingArenaCheckpoint {
  std::size_t expressions;
  std::size_t pending_aggregates;
  std::size_t deferred_aggregates;
  std::size_t collations;
  std::size_t functions;
  std::size_t aggregate_functions;
  std::size_t queries;
  std::size_t cores;
  std::size_t source_columns;
  std::size_t aggregate_invocations;
  std::size_t registered_collations;
  std::uint32_t next_source;
  std::size_t captures;
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

[[nodiscard]] BindError ValueBinderError(BindErrorCode code, SourceSpan span, std::string detail) {
  BindError error = BinderError(code, span, std::move(detail));
  error.value_requirement = true;
  return error;
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

enum class SpecialScalarKind : std::uint8_t {
  None,
  Coalesce,
  Conditional,
  Likelihood,
};

[[nodiscard]] SpecialScalarKind SelectSpecialScalar(std::string_view name) noexcept {
  if (NamesEqual(name, "coalesce") || NamesEqual(name, "ifnull")) {
    return SpecialScalarKind::Coalesce;
  }
  if (NamesEqual(name, "iif") || NamesEqual(name, "if")) {
    return SpecialScalarKind::Conditional;
  }
  if (NamesEqual(name, "likely") || NamesEqual(name, "unlikely") ||
      NamesEqual(name, "likelihood")) {
    return SpecialScalarKind::Likelihood;
  }
  return SpecialScalarKind::None;
}

[[nodiscard]] bool SpecialScalarAcceptsArgumentCount(SpecialScalarKind kind, std::string_view name,
                                                     std::size_t count) noexcept {
  switch (kind) {
    case SpecialScalarKind::Coalesce:
      return NamesEqual(name, "ifnull") ? count == 2U : count >= 2U;
    case SpecialScalarKind::Conditional:
      return count >= 2U;
    case SpecialScalarKind::Likelihood:
      return count == (NamesEqual(name, "likelihood") ? 2U : 1U);
    case SpecialScalarKind::None:
      return false;
  }
  return false;
}

enum class InSyntaxForm : std::uint8_t {
  List,
  EmptyDiscarded,
  EmptyPreserved,
  SingletonEquality,
};

enum class ParserConstant : std::uint8_t {
  None,
  FalseInteger,
  TrueInteger,
  FalseBoolean,
  TrueBoolean,
};

enum class ParserConstantMode : std::uint8_t {
  Normal,
  NoIs,
};

enum class ExpressionPreparationPhase : std::uint8_t {
  Ordinary,
  IncomingValues,
};

struct SyntaxNormalization {
  std::uint16_t construction_height = 0;
  InSyntaxForm membership = InSyntaxForm::List;
  ParserConstant constant = ParserConstant::None;
  bool unary_positive = false;
  bool has_local_function = false;
  std::optional<SourceSpan> explicit_collation{};
  std::optional<BoundExpressionId> construction_binding{};
};

struct ParameterAssignmentState {
  std::optional<std::unordered_map<std::string, std::size_t>> named_slots;
  std::size_t maximum_slot = 0;
};

struct NativeValuesConstruction {
  const ValuesCore* syntax = nullptr;
  std::size_t current_row = 0;
  bool coroutine = false;
  std::vector<BoundValuesChunk> chunks;
};

struct NativeConstructionEvent {
  std::size_t source_end = 0;
  std::optional<ExpressionId> expression{};
  std::size_t values = 0;
  std::size_t row = 0;
};

static_assert(kMaximumExpressionConstructionDepth <= std::numeric_limits<std::uint16_t>::max());

[[nodiscard]] BoundTruthHint IntegerLiteralTruthHint(std::string_view token) noexcept {
  if (token.empty() || token.find('_') != std::string_view::npos) {
    return BoundTruthHint::kNone;
  }
  int radix = 10;
  if (token.starts_with("0x") || token.starts_with("0X")) {
    token.remove_prefix(2);
    radix = 16;
  }
  std::uint32_t value = 0;
  const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, radix);
  if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
      value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
    return BoundTruthHint::kNone;
  }
  return value == 0 ? BoundTruthHint::kAlwaysFalse : BoundTruthHint::kAlwaysTrue;
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

struct BoundSelect::Impl final {
  std::string source;
  CatalogSnapshotPtr catalog;
  std::uint64_t registration_generation = 0;
  std::vector<BoundTableSource> table_sources;
  std::vector<BoundSourceColumn> source_columns;
  std::vector<BoundCollation> collations;
  std::vector<BoundScalarFunction> functions;
  std::vector<BoundParameter> parameters;
  std::vector<BoundExpression> expressions;
  std::vector<std::string> registered_collations;
  std::vector<BoundAggregateFunction> aggregate_functions;
  std::vector<BoundAggregateInvocation> aggregate_invocations;
  std::vector<BoundQueryCore> query_cores;
  BoundQuery root_query;
  std::vector<BoundQuery> nested_queries;
  std::vector<BoundResultColumn> result_columns;
  std::vector<ResultDefinition> result_definitions;
  std::optional<BoundExpressionId> where;
};

struct BoundInsert::Impl final {
  std::optional<BoundSelect> query_owner;
  BoundMutationTarget target;
  std::vector<BoundInsertValue> values;
  bool default_values = false;
  bool explicit_columns = false;
};

struct BoundUpdate::Impl final {
  std::optional<BoundSelect> query_owner;
  BoundMutationTarget target;
  std::vector<BoundUpdateAssignment> assignments;
  std::optional<BoundExpressionId> where;
  bool changes_rowid = false;
};

struct BoundDelete::Impl final {
  std::optional<BoundSelect> query_owner;
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

namespace {

template <typename State>
[[nodiscard]] const BoundSelect& MutationQueryOwner(const State& state) noexcept {
  if (!state.query_owner.has_value()) {
    std::terminate();
  }
  return *state.query_owner;
}

}  // namespace

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

  [[nodiscard]] bool requires_schema_check() const noexcept { return schema_check_requested_; }

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
    if (!HasMultirowValues()) {
      BindExpected<void> syntax_widths = ValidateSyntacticCompoundWidths(select);
      if (!syntax_widths.has_value()) {
        return std::unexpected(std::move(syntax_widths.error()));
      }
    }
    BindExpected<void> initialized = InitializeCommon();
    if (!initialized.has_value()) {
      return std::unexpected(std::move(initialized.error()));
    }
    if (auto bound = BindQuery(select); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
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
    if (auto source = PublishMutationSource(*target); !source.has_value()) {
      return std::unexpected(std::move(source.error()));
    }

    auto output = std::make_unique<BoundUpdate::Impl>();
    output->target = std::move(*target);
    output->assignments.reserve(update.assignments.size());
    std::optional<std::size_t> missing_target;
    for (const UpdateAssignment& assignment : update.assignments) {
      auto field = LookupMutationField(output->target, assignment.column);
      if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
      }
      BoundMutationField resolved;
      if (field->has_value()) {
        resolved = field->value();
      } else if (!missing_target.has_value()) {
        missing_target = output->assignments.size();
      }
      output->assignments.push_back(BoundUpdateAssignment{
          .target = resolved,
          .expression = BoundExpressionId{0},
      });
    }
    MarkUpdateEffectiveness(output->assignments);
    const auto priority = [](const BoundMutationField& field) -> std::size_t {
      if (field.rowid) {
        return 0U;
      }
      if (!field.column.has_value()) {
        std::terminate();
      }
      return field.column->value + 1U;
    };
    std::optional<std::pair<std::size_t, BindError>> value_error;
    for (std::size_t index = 0; index < update.assignments.size(); ++index) {
      const UpdateAssignment& assignment = update.assignments[index];
      BoundUpdateAssignment& bound = output->assignments[index];
      BindScope scope{
          .source_columns = true,
          .result_aliases = false,
          .requires_value = bound.effective && !missing_target.has_value(),
      };
      const BindingArenaCheckpoint checkpoint = SaveBindingArena();
      auto expression = BindUpdateRhs(assignment.expression, scope);
      if (!expression.has_value() && expression.error().value_requirement && scope.requires_value) {
        const std::size_t position = priority(bound.target);
        if (!value_error.has_value() || position < value_error->first) {
          value_error.emplace(position, std::move(expression.error()));
        }
        RestoreBindingArena(checkpoint);
        scope.requires_value = false;
        expression = BindUpdateRhs(assignment.expression, scope);
      }
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      bound.expression = *expression;
      if (missing_target == index) {
        auto rejected = ResolveMutationField(output->target, assignment.column);
        if (!rejected.has_value()) {
          return std::unexpected(std::move(rejected.error()));
        }
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, assignment.column,
                                           "UPDATE target changed during binding"));
      }
    }
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
    if (value_error.has_value()) {
      return std::unexpected(std::move(value_error->second));
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
    if (auto source = PublishMutationSource(*target); !source.has_value()) {
      return std::unexpected(std::move(source.error()));
    }

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
          SchemaLookupError(BindErrorCode::kObjectNameReserved, table.name.span,
                            "object name reserved for internal use: " + std::string{table_name}));
    }
    if (catalog_->FindIndex(table_name).has_value()) {
      return std::unexpected(
          SchemaLookupError(BindErrorCode::kTableAlreadyExists, table.name.span,
                            "there is already an index named " + std::string{table_name}));
    }

    auto output = std::make_unique<BoundCreateTable::Impl>();
    output->source.assign(tree_.source().bytes());
    output->catalog = catalog_;
    output->table_name = std::string{table_name};
    output->if_not_exists = table.if_not_exists;
    if (catalog_->FindTable(table_name).has_value()) {
      if (!table.if_not_exists) {
        return std::unexpected(
            SchemaLookupError(BindErrorCode::kTableAlreadyExists, table.name.span,
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
      return std::unexpected(SchemaLookupError(BindErrorCode::kNoSuchTable, index.table.span,
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
            SchemaLookupError(BindErrorCode::kNoSuchColumn, term.span,
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
          if (!selected_table.has_value()) {
            return std::unexpected(
                SchemaLookupError(BindErrorCode::kNoSuchTable, analyze.target->span,
                                  "no such table or index: " + std::string{object_name}));
          }
          if (HasSqlitePrefix(catalog_->table(*selected_table).name)) {
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
          .unique = index.unique,
          .unique_not_null = index.unique_not_null,
          .primary_without_rowid = table.without_rowid && index.origin == IndexOrigin::kPrimaryKey,
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
    const bool native_values = HasMultirowValues();
    if (!native_values) {
      if (auto parameters = AssignParameters(); !parameters.has_value()) {
        return parameters;
      }
    }
    query_bindings_.resize(tree_.select_queries().size());
    impl_->nested_queries.reserve(tree_.select_queries().size());
    const auto* root = std::get_if<SelectStatement>(&tree_.statement());
    std::size_t core_count = root != nullptr ? root->compounds.size() + 1U : 0U;
    for (const SelectStatement& query : tree_.select_queries()) {
      const std::size_t count = query.compounds.size() + 1U;
      if (count > impl_->query_cores.max_size() - core_count) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, query.span,
                                           "bound query core arena size overflow"));
      }
      core_count += count;
    }
    impl_->query_cores.reserve(core_count);
    core_metadata_.reserve(core_count);
    if (native_values) {
      return NormalizeValuesConstruction();
    }
    return {};
  }

  template <typename Output>
  void MoveCommon(Output& output) {
    output.query_owner.emplace(BoundSelect{std::move(impl_)});
  }

  [[nodiscard]] BindExpected<BoundMutationTarget> ResolveMutationTarget(const QualifiedName& name) {
    BindExpected<DecodedNameParts> parts = NameParts(name);
    if (!parts.has_value()) {
      return std::unexpected(std::move(parts.error()));
    }
    if (parts->empty() || parts->size() > 2U ||
        (parts->size() == 2U && !NamesEqual(parts->front(), catalog_->schema_name()))) {
      return std::unexpected(
          SchemaLookupError(BindErrorCode::kNoSuchTable, name.span,
                            "no such table: " + std::string{SpanText(name.span)}));
    }
    const std::string_view requested_name = parts->back();
    if (IsSchemaTableName(requested_name)) {
      return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, name.span,
                                         "sqlite_schema mutation is not supported"));
    }
    const std::optional<TableId> table_id = catalog_->FindTable(requested_name);
    if (!table_id.has_value()) {
      return std::unexpected(SchemaLookupError(BindErrorCode::kNoSuchTable, name.span,
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

  [[nodiscard]] BindExpected<void> PublishMutationSource(const BoundMutationTarget& target) {
    auto id = AllocateSourceId(target.span);
    if (!id.has_value()) {
      return std::unexpected(std::move(id.error()));
    }
    const CatalogTable& table = catalog_->table(target.table);
    source_states_.push_back(SourceState{
        .id = *id,
        .name = table.name,
        .alias = std::nullopt,
        .table = target.table,
        .schema_table = false,
        .source_column_count = target.columns.size(),
    });
    impl_->table_sources.push_back(BoundTableSource{
        .id = *id,
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
    return {};
  }

  [[nodiscard]] BindExpected<std::optional<BoundMutationField>> LookupMutationField(
      const BoundMutationTarget& target, SourceSpan name_span) {
    BindExpected<std::string> name = Dequote(name_span);
    if (!name.has_value()) {
      return std::unexpected(std::move(name.error()));
    }
    const std::optional<ColumnId> column = catalog_->FindColumn(target.table, *name);
    if (column.has_value()) {
      return std::optional{BoundMutationField{
          .column = column,
          .rowid = target.rowid_alias == *column,
      }};
    }
    if (IsRowIdName(*name)) {
      return std::optional{BoundMutationField{
          .column = target.rowid_alias,
          .rowid = true,
      }};
    }
    return std::optional<BoundMutationField>{};
  }

  [[nodiscard]] BindExpected<BoundMutationField> ResolveMutationField(
      const BoundMutationTarget& target, SourceSpan name_span) {
    auto field = LookupMutationField(target, name_span);
    if (!field.has_value()) {
      return std::unexpected(std::move(field.error()));
    }
    if (field->has_value()) {
      return field->value();
    }
    auto name = Dequote(name_span);
    if (!name.has_value()) {
      return std::unexpected(std::move(name.error()));
    }
    return std::unexpected(
        SchemaLookupError(BindErrorCode::kNoSuchColumn, name_span, "no such column: " + *name));
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

  [[nodiscard]] BoundQuery& Query(BoundQueryId id) noexcept {
    return id.value() == 0U ? impl_->root_query : impl_->nested_queries[id.value() - 1U];
  }

  [[nodiscard]] const BoundQuery& Query(BoundQueryId id) const noexcept {
    return id.value() == 0U ? impl_->root_query : impl_->nested_queries[id.value() - 1U];
  }

  [[nodiscard]] BoundQuery& ActiveQuery() noexcept { return Query(active_query_); }
  [[nodiscard]] const BoundQuery& ActiveQuery() const noexcept { return Query(active_query_); }

  [[nodiscard]] auto CaptureLookup() const noexcept {
    return [this](BoundQueryId id) -> std::optional<std::span<const BoundQueryCapture>> {
      if (id.value() > impl_->nested_queries.size()) {
        return std::nullopt;
      }
      return std::span<const BoundQueryCapture>{Query(id).captures};
    };
  }

  [[nodiscard]] bool OwnsActiveSource(BoundSourceId source) const noexcept {
    return std::ranges::find(source_states_, source, &SourceState::id) != source_states_.end();
  }

  [[nodiscard]] bool OwnsAggregateSource(BoundSourceId source) const noexcept {
    const std::span<const SourceState> sources = definition_name_scope_ != nullptr
                                                     ? definition_name_scope_->sources
                                                     : std::span<const SourceState>{source_states_};
    return std::ranges::find(sources, source, &SourceState::id) != sources.end();
  }

  [[nodiscard]] bool OwnsQuerySource(BoundSourceId source) const noexcept {
    return std::ranges::any_of(ActiveCores(), [source](const BoundQueryCore& core) {
      const auto* select = std::get_if<BoundSelectCore>(&core);
      return select != nullptr &&
             std::ranges::find(select->table_sources, source, &BoundTableSource::id) !=
                 select->table_sources.end();
    });
  }

  [[nodiscard]] bool IsQueryAncestor(BoundQueryId ancestor,
                                     BoundQueryId descendant) const noexcept {
    while (descendant != ancestor) {
      const std::optional<BoundQueryId> parent = Query(descendant).parent;
      if (!parent.has_value()) {
        return false;
      }
      descendant = *parent;
    }
    return true;
  }

  [[nodiscard]] static const BoundAggregateContextCapture* FindAggregateContextCapture(
      std::span<const BoundQueryCapture> captures, BoundQueryId owner) noexcept {
    for (const BoundQueryCapture& capture : captures) {
      const auto* aggregate = std::get_if<BoundAggregateContextCapture>(&capture);
      if (aggregate != nullptr && aggregate->query == owner) {
        return aggregate;
      }
    }
    return nullptr;
  }

  [[nodiscard]] BindExpected<void> MergeAggregateContextCapture(
      std::vector<BoundQueryCapture>& captures, const BoundAggregateContextCapture& candidate) {
    const auto found = std::ranges::find_if(captures, [&](const BoundQueryCapture& capture) {
      const auto* aggregate = std::get_if<BoundAggregateContextCapture>(&capture);
      return aggregate != nullptr && aggregate->query == candidate.query &&
             aggregate->expression == candidate.expression;
    });
    if (found == captures.end()) {
      captures.emplace_back(candidate);
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> NormalizeAggregateContextCaptures() {
    auto& captures = ActiveQuery().captures;
    for (std::size_t index = 0; index < captures.size(); ++index) {
      auto* selected = std::get_if<BoundAggregateContextCapture>(&captures[index]);
      if (selected == nullptr) {
        continue;
      }
      const auto source_anchored = [&](BoundExpressionId expression) -> std::optional<bool> {
        if (expression.value() >= impl_->expressions.size()) {
          return std::nullopt;
        }
        const auto* call = std::get_if<BoundAggregateCallExpression>(
            &impl_->expressions[expression.value()].payload);
        const BoundAggregateInvocation* invocation =
            call == nullptr ? nullptr : AggregateInvocationForExpression(expression, *call);
        return invocation == nullptr ? std::nullopt : std::optional{invocation->source_anchored};
      };
      std::optional<bool> selected_anchored = source_anchored(selected->expression);
      if (!selected_anchored.has_value()) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                           "aggregate context has no invocation"));
      }
      for (std::size_t candidate_index = index + 1U; candidate_index < captures.size();) {
        const auto* candidate =
            std::get_if<BoundAggregateContextCapture>(&captures[candidate_index]);
        if (candidate == nullptr || candidate->query != selected->query) {
          ++candidate_index;
          continue;
        }
        const std::optional<bool> candidate_anchored = source_anchored(candidate->expression);
        if (!candidate_anchored.has_value()) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                             "aggregate context has no invocation"));
        }
        if (!*selected_anchored && *candidate_anchored) {
          selected->expression = candidate->expression;
          selected_anchored = true;
        }
        captures.erase(captures.begin() + static_cast<std::ptrdiff_t>(candidate_index));
      }
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> RecordQueryCaptures(BoundExpressionId id) {
    auto visitor = [&](const BoundQueryCapture& dependency) {
      auto& captures = ActiveQuery().captures;
      if (const auto* source = std::get_if<BoundSourceCapture>(&dependency); source != nullptr) {
        if (!OwnsActiveSource(source->identity.source) &&
            FindSourceCapture(std::span{captures}, source->identity) == nullptr) {
          captures.emplace_back(BoundSourceCapture{.identity = source->identity});
        }
      } else if (const auto* context = std::get_if<BoundQueryContextCapture>(&dependency);
                 context != nullptr) {
        const bool recorded = std::ranges::any_of(captures, [context](const auto& capture) {
          const auto* existing = std::get_if<BoundQueryContextCapture>(&capture);
          return existing != nullptr && existing->query == context->query;
        });
        if (context->query != active_query_ && !recorded) {
          captures.emplace_back(*context);
        }
      } else {
        const auto aggregate = std::get<BoundAggregateContextCapture>(dependency);
        if (aggregate.query != active_query_) {
          if (const auto merged = MergeAggregateContextCapture(captures, aggregate);
              !merged.has_value()) {
            return false;
          }
        }
      }
      return true;
    };
    if (!VisitQueryDependencies(impl_->expressions, impl_->source_columns, id, CaptureLookup(),
                                BoundSourceDependency::kBinding, visitor)) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                         "captured expression has invalid query dependencies"));
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> RecordOwnerAggregateCaptures(BoundExpressionId id) {
    auto visitor = [&](const BoundQueryCapture& dependency) {
      auto& captures = ActiveQuery().captures;
      if (const auto* source = std::get_if<BoundSourceCapture>(&dependency); source != nullptr) {
        if (!OwnsActiveSource(source->identity.source) &&
            FindSourceCapture(std::span{captures}, source->identity) == nullptr) {
          captures.emplace_back(BoundSourceCapture{.identity = source->identity});
        }
        return true;
      }
      if (const auto* context = std::get_if<BoundQueryContextCapture>(&dependency);
          context != nullptr) {
        if (context->query == active_query_ || IsQueryAncestor(active_query_, context->query)) {
          return true;
        }
        if (!IsQueryAncestor(context->query, active_query_)) {
          return false;
        }
        const bool recorded = std::ranges::any_of(captures, [context](const auto& capture) {
          const auto* existing = std::get_if<BoundQueryContextCapture>(&capture);
          return existing != nullptr && existing->query == context->query;
        });
        if (!recorded) {
          captures.emplace_back(*context);
        }
        return true;
      }
      const auto aggregate = std::get<BoundAggregateContextCapture>(dependency);
      if (aggregate.query == active_query_ || IsQueryAncestor(active_query_, aggregate.query)) {
        return false;
      }
      if (!IsQueryAncestor(aggregate.query, active_query_)) {
        return false;
      }
      if (const auto merged = MergeAggregateContextCapture(captures, aggregate);
          !merged.has_value()) {
        return false;
      }
      return true;
    };
    if (!VisitQueryDependencies(impl_->expressions, impl_->source_columns, id, CaptureLookup(),
                                BoundSourceDependency::kBinding, visitor)) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                         "outer aggregate has invalid query dependencies"));
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> ReparentExpressionQueries(BoundExpressionId id,
                                                             BoundQueryId previous,
                                                             BoundQueryId owner) {
    BoundExpression& expression = impl_->expressions[id.value()];
    if (const auto* subquery = std::get_if<BoundSubqueryExpression>(&expression.payload);
        subquery != nullptr) {
      BoundQuery& query = Query(subquery->query);
      if (query.parent != previous && query.parent != owner) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, expression.span,
                                           "aggregate query has an invalid lexical parent"));
      }
      query.parent = owner;
      return {};
    }
    const auto reparent = [&](BoundExpressionId child) {
      return ReparentExpressionQueries(child, previous, owner);
    };
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      return reparent(alias->target);
    }
    if (const auto* unary = std::get_if<BoundUnaryExpression>(&expression.payload);
        unary != nullptr) {
      return reparent(unary->operand);
    }
    if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
        binary != nullptr) {
      if (auto result = reparent(binary->left); !result.has_value()) {
        return result;
      }
      return reparent(binary->right);
    }
    if (const auto* comparison = std::get_if<BoundComparisonExpression>(&expression.payload);
        comparison != nullptr) {
      if (auto result = reparent(comparison->left); !result.has_value()) {
        return result;
      }
      return reparent(comparison->right);
    }
    if (const auto* truth = std::get_if<BoundTruthTestExpression>(&expression.payload);
        truth != nullptr) {
      return reparent(truth->operand);
    }
    const auto reparent_arguments = [&](std::span<const BoundExpressionId> arguments) {
      for (const BoundExpressionId argument : arguments) {
        if (auto result = reparent(argument); !result.has_value()) {
          return result;
        }
      }
      return BindExpected<void>{};
    };
    if (const auto* call = std::get_if<BoundScalarCallExpression>(&expression.payload);
        call != nullptr) {
      return reparent_arguments(call->arguments);
    }
    if (const auto* coalesce = std::get_if<BoundCoalesceExpression>(&expression.payload);
        coalesce != nullptr) {
      return reparent_arguments(coalesce->arguments);
    }
    if (const auto* conditional = std::get_if<BoundConditionalExpression>(&expression.payload);
        conditional != nullptr) {
      return reparent_arguments(conditional->arguments);
    }
    if (const auto* membership = std::get_if<BoundInExpression>(&expression.payload);
        membership != nullptr) {
      if (auto result = reparent(membership->operand); !result.has_value()) {
        return result;
      }
      return reparent_arguments(membership->values);
    }
    if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&expression.payload);
        likelihood != nullptr) {
      return reparent(likelihood->operand);
    }
    if (const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
        collate != nullptr) {
      return reparent(collate->operand);
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> PrepareOuterAggregateInvocation(
      PendingAggregateInvocation& pending) {
    if (pending.site_query == pending.owner_query) {
      return {};
    }
    if (active_query_ != pending.owner_query || active_core_ != pending.owner_core) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                         "outer aggregate finalized under the wrong owner"));
    }
    const auto prepare = [&](BoundExpressionId id) -> BindExpected<void> {
      if (auto reparented = ReparentExpressionQueries(id, pending.site_query, pending.owner_query);
          !reparented.has_value()) {
        return reparented;
      }
      return RecordOwnerAggregateCaptures(id);
    };
    for (const BoundExpressionId argument : pending.invocation.arguments) {
      if (auto prepared = prepare(argument); !prepared.has_value()) {
        return prepared;
      }
    }
    for (const BoundOrderingTerm& term : pending.invocation.order_by) {
      if (auto prepared = prepare(term.expression); !prepared.has_value()) {
        return prepared;
      }
    }
    if (pending.invocation.filter.has_value()) {
      return prepare(*pending.invocation.filter);
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> FinalizeQueryCaptures() {
    if (auto normalized = NormalizeAggregateContextCaptures(); !normalized.has_value()) {
      return normalized;
    }
    if (ActiveQuery().captures.empty()) {
      return {};
    }
    auto visitor = [&](const BoundSourceLeafIdentity& identity,
                       std::optional<BoundExpressionId> value) {
      if (OwnsQuerySource(identity.source)) {
        return true;
      }
      auto& captures = ActiveQuery().captures;
      auto* capture = FindSourceCapture(std::span{captures}, identity);
      if (capture == nullptr || !value.has_value()) {
        return false;
      }
      if (!capture->value.has_value()) {
        capture->value = *value;
      }
      return true;
    };
    bool valid = true;
    const auto consume = [&](BoundExpressionId id) {
      valid = valid && VisitSourceLeaves(impl_->expressions, impl_->source_columns, id,
                                         CaptureLookup(), BoundSourceDependency::kValue, visitor);
    };
    const BoundQuery& query = ActiveQuery();
    const bool needs_values = BoundQueryRequiresResultValues(query, impl_->expressions);
    const auto cores = ActiveCores();
    for (const BoundQueryCore& core : cores) {
      if (const auto* select = std::get_if<BoundSelectCore>(&core); select != nullptr) {
        for (const BoundTableSource& source : select->table_sources) {
          if (source.on.has_value()) {
            consume(*source.on);
          }
        }
        if (select->where.has_value()) {
          consume(*select->where);
        }
        if (needs_values) {
          for (const BoundResultColumn& result : select->result_columns) {
            consume(result.expression);
          }
        }
        if (select->aggregate.has_value()) {
          const BoundAggregateProgram& aggregate = *select->aggregate;
          for (const BoundGroupingTerm& group : aggregate.group_by) {
            consume(group.expression);
          }
          if (const auto having = aggregate.having; having.has_value()) {
            consume(*having);
          }
          for (const BoundAggregateInvocationId id : aggregate.invocations) {
            const BoundAggregateInvocation& invocation = impl_->aggregate_invocations[id.value()];
            for (const BoundExpressionId argument : invocation.arguments) {
              consume(argument);
            }
            if (invocation.filter.has_value()) {
              consume(*invocation.filter);
            }
            if (impl_->aggregate_functions[invocation.function.value()].extremum ==
                BoundAggregateExtremumKind::kNone) {
              for (const BoundOrderingTerm& term : invocation.order_by) {
                consume(term.expression);
              }
            }
          }
        }
      } else if (needs_values) {
        for (const auto& row : std::get<BoundValuesCore>(core).rows) {
          for (const BoundExpressionId value : row) {
            consume(value);
          }
        }
      }
    }
    const auto* single_select =
        cores.size() == 1U ? std::get_if<BoundSelectCore>(&cores.front()) : nullptr;
    const bool elides_order_keys = single_select != nullptr &&
                                   single_select->aggregate.has_value() &&
                                   single_select->aggregate->group_by.empty();
    if (!elides_order_keys) {
      for (const BoundOrderingTerm& term : query.order_by) {
        consume(term.expression);
      }
    }
    if (query.limit.has_value()) {
      consume(query.limit->limit);
      if (query.limit->offset.has_value()) {
        consume(*query.limit->offset);
      }
    }
    if (!valid) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                         "query value has invalid capture ownership"));
    }
    return {};
  }

  [[nodiscard]] std::span<const BoundQueryCore> ActiveCores() const noexcept {
    const BoundQuery& query = ActiveQuery();
    return std::span<const BoundQueryCore>{impl_->query_cores}.subspan(query.core_begin,
                                                                       query.core_count);
  }

  [[nodiscard]] BindExpected<void> BindQuery(const SelectStatement& select) {
    if (auto widths = ValidateSyntacticCompoundWidths(select); !widths.has_value()) {
      return widths;
    }
    const std::size_t begin = impl_->query_cores.size();
    const std::size_t count = select.compounds.size() + 1U;
    ActiveQuery().core_begin = begin;
    ActiveQuery().core_count = count;
    ActiveQuery().compound_operators.reserve(select.compounds.size());
    impl_->query_cores.resize(begin + count);
    core_metadata_.resize(begin + count);
    active_core_ = begin;

    if (select.compounds.empty() && std::holds_alternative<SelectCore>(select.first)) {
      const auto& core = std::get<SelectCore>(select.first);
      if (auto body = BindSelectCoreBody(core); !body.has_value()) {
        return body;
      }
      if (auto order = BindQueryOrdering(select, false); !order.has_value()) {
        return order;
      }
      if (auto group = BindGroupBy(core); !group.has_value()) {
        return group;
      }
      if (core.quantifier == SelectQuantifier::kDistinct &&
          ActiveQuery().destination != BoundQueryDestination::kExists) {
        if (auto distinct = RequireResultCollations(core.span, impl_->result_columns);
            !distinct.has_value()) {
          return distinct;
        }
      }
      if (auto finished = FinishSelectQueryCore(core.quantifier); !finished.has_value()) {
        return finished;
      }
    } else {
      if (auto first = BindQueryCore(select.first); !first.has_value()) {
        return first;
      }
      for (std::size_t index = 0; index < select.compounds.size(); ++index) {
        const CompoundTerm& term = select.compounds[index];
        ActiveQuery().compound_operators.push_back(term.operation);
        active_core_ = begin + index + 1U;
        if (auto core = BindQueryCore(term.core); !core.has_value()) {
          return core;
        }
      }
      if (!select.compounds.empty()) {
        if (auto widths = ValidateCompoundWidths(select); !widths.has_value()) {
          return widths;
        }
        if (auto collations = BindCompoundCollations(select.span); !collations.has_value()) {
          return collations;
        }
        if (auto order = BindQueryOrdering(select, true); !order.has_value()) {
          return order;
        }
        if (auto distinct = RequireCompoundDistinctCollations(select); !distinct.has_value()) {
          return distinct;
        }
      }
    }
    if (auto limit = BindLimit(select.limit); !limit.has_value()) {
      return limit;
    }
    if (ActiveQuery().destination != BoundQueryDestination::kClient) {
      if (auto demand = BindSingleRowDemand(select.span); !demand.has_value()) {
        return demand;
      }
    }
    if (auto collations = RequireSetCompoundCollations(select.span); !collations.has_value()) {
      return collations;
    }
    if (auto validated = ValidateRequiredResultDefinitions(); !validated.has_value()) {
      return validated;
    }
    PublishResultColumns();
    return FinalizeQueryCaptures();
  }

  [[nodiscard]] BindExpected<void> BindSingleRowDemand(SourceSpan span) {
    auto& bound_limit = ActiveQuery().limit;
    if (!bound_limit.has_value()) {
      auto one = AppendExpression(span, BoundLiteralExpression{.value = SqlValue::Integer(1)}, {});
      if (!one.has_value()) {
        return std::unexpected(std::move(one.error()));
      }
      bound_limit = BoundLimit{.limit = *one};
      return {};
    }
    const BoundExpressionId limit = bound_limit->limit;
    const auto* literal =
        std::get_if<BoundLiteralExpression>(&impl_->expressions[limit.value()].payload);
    if (literal != nullptr && literal->value.type() == SqlValueType::kInteger &&
        (literal->value.integer_value() == 0 || literal->value.integer_value() == 1)) {
      return {};
    }
    auto zero = AppendExpression(span, BoundLiteralExpression{.value = SqlValue::Integer(0)}, {});
    auto binary = InternCollation("BINARY", span);
    if (!zero.has_value()) {
      return std::unexpected(std::move(zero.error()));
    }
    if (!binary.has_value()) {
      return std::unexpected(std::move(binary.error()));
    }
    auto nonzero = AppendExpression(span,
                                    BoundComparisonExpression{
                                        .comparison = SqlComparison::kNotEqual,
                                        .affinity = TypeAffinity::kNumeric,
                                        .collation = *binary,
                                        .left = limit,
                                        .right = *zero,
                                    },
                                    {});
    if (!nonzero.has_value()) {
      return std::unexpected(std::move(nonzero.error()));
    }
    bound_limit->limit = *nonzero;
    return {};
  }

  [[nodiscard]] BindingArenaCheckpoint SaveBindingArena() const noexcept {
    return BindingArenaCheckpoint{
        .expressions = impl_->expressions.size(),
        .pending_aggregates = pending_aggregates_.size(),
        .deferred_aggregates = deferred_aggregate_replays_.size(),
        .collations = impl_->collations.size(),
        .functions = impl_->functions.size(),
        .aggregate_functions = impl_->aggregate_functions.size(),
        .queries = impl_->nested_queries.size(),
        .cores = impl_->query_cores.size(),
        .source_columns = impl_->source_columns.size(),
        .aggregate_invocations = impl_->aggregate_invocations.size(),
        .registered_collations = impl_->registered_collations.size(),
        .next_source = next_source_id_,
        .captures = ActiveQuery().captures.size(),
    };
  }

  void RestoreBindingArena(const BindingArenaCheckpoint& checkpoint,
                           bool preserve_capture_dependencies = false,
                           bool preserve_deferred_aggregates = false) {
    impl_->expressions.resize(checkpoint.expressions);
    while (pending_aggregates_.size() > checkpoint.pending_aggregates) {
      pending_aggregates_.pop_back();
    }
    if (!preserve_deferred_aggregates) {
      deferred_aggregate_replays_.resize(checkpoint.deferred_aggregates);
    }
    impl_->collations.resize(checkpoint.collations);
    impl_->functions.resize(checkpoint.functions);
    while (impl_->aggregate_functions.size() > checkpoint.aggregate_functions) {
      impl_->aggregate_functions.pop_back();
    }
    while (impl_->nested_queries.size() > checkpoint.queries) {
      impl_->nested_queries.pop_back();
    }
    impl_->query_cores.resize(checkpoint.cores);
    core_metadata_.resize(checkpoint.cores);
    impl_->source_columns.resize(checkpoint.source_columns);
    while (impl_->aggregate_invocations.size() > checkpoint.aggregate_invocations) {
      impl_->aggregate_invocations.pop_back();
    }
    impl_->registered_collations.resize(checkpoint.registered_collations);
    next_source_id_ = checkpoint.next_source;
    if (!preserve_capture_dependencies) {
      ActiveQuery().captures.resize(checkpoint.captures);
    } else {
      std::erase_if(ActiveQuery().captures, [&](const BoundQueryCapture& capture) {
        const auto* aggregate = std::get_if<BoundAggregateContextCapture>(&capture);
        return aggregate != nullptr && (aggregate->query.value() > checkpoint.queries ||
                                        aggregate->expression.value() >= checkpoint.expressions);
      });
    }
    for (auto& binding : query_bindings_) {
      if (binding.has_value() && binding->value() > checkpoint.queries) {
        binding.reset();
      }
    }
  }

  [[nodiscard]] BindExpected<void> BindQueryOrdering(const SelectStatement& select, bool compound) {
    const bool source_free_singleton =
        !compound && impl_->table_sources.empty() && !active_core_is_aggregate_;
    if (ActiveQuery().destination != BoundQueryDestination::kExists && source_free_singleton) {
      discarded_order_bindings_.push_back(DiscardedOrderBinding{
          .query = active_query_,
          .replay_begin = deferred_aggregate_replays_.size(),
          .rolls_back = false,
      });
      auto bound = BindOrderBy(select);
      discarded_order_bindings_.pop_back();
      return bound;
    }
    if (ActiveQuery().destination != BoundQueryDestination::kExists) {
      return compound ? BindCompoundOrderBy(select) : BindOrderBy(select);
    }
    const bool owns_deferred_replay =
        std::ranges::none_of(discarded_order_bindings_, &DiscardedOrderBinding::rolls_back);
    const std::size_t replay_begin = deferred_aggregate_replays_.size();
    discarded_order_bindings_.push_back(DiscardedOrderBinding{
        .query = active_query_,
        .replay_begin = replay_begin,
        .rolls_back = true,
    });
    const BindingArenaCheckpoint checkpoint = SaveBindingArena();
    const AggregateOccurrencePhase phase = active_aggregate_phase_;
    auto bound = compound ? BindCompoundOrderBy(select) : BindOrderBy(select);
    ActiveQuery().order_by.clear();
    RestoreBindingArena(checkpoint, true, true);
    active_aggregate_phase_ = phase;
    discarded_order_bindings_.pop_back();
    if (!bound.has_value()) {
      deferred_aggregate_replays_.resize(replay_begin);
      return bound;
    }
    if (!owns_deferred_replay) {
      return {};
    }
    BindExpected<void> replayed = [&]() -> BindExpected<void> {
      for (std::size_t index = replay_begin; index < deferred_aggregate_replays_.size(); ++index) {
        const DeferredAggregateReplay replay = deferred_aggregate_replays_[index];
        if (replay.consumer_query != active_query_) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                             "discarded aggregate consumer is invalid"));
        }
        const SourceSpan replay_span = tree_.expression(replay.syntax).span;
        const std::size_t pending_begin = pending_aggregates_.size();
        const bool previous_binding_only = std::exchange(binding_only_, false);
        auto expression = BindExpressionTemplateCopy(replay.syntax, replay.definition_scope,
                                                     replay.scope, replay_span, false);
        binding_only_ = previous_binding_only;
        if (!expression.has_value()) {
          return std::unexpected(std::move(expression.error()));
        }
        const bool registered = std::ranges::any_of(
            std::span{pending_aggregates_}.subspan(pending_begin),
            [&](const PendingAggregateInvocation& pending) {
              return !pending.finalized && pending.owner_query == replay.owner_query &&
                     pending.owner_core == replay.owner_core && pending.phase == replay.owner_phase;
            });
        if (!registered) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, replay_span,
                                             "discarded aggregate replay was not registered"));
        }
      }
      return {};
    }();
    deferred_aggregate_replays_.resize(replay_begin);
    return replayed;
  }

  [[nodiscard]] NameScopeView CurrentNameScope(BindScope scope) const noexcept {
    if (definition_name_scope_ != nullptr) {
      NameScopeView view = *definition_name_scope_;
      const bool template_result_aliases = view.scope.result_aliases;
      view.scope = scope;
      view.scope.result_aliases = scope.result_aliases && template_result_aliases;
      return view;
    }
    return NameScopeView{
        .query = active_query_,
        .sources = source_states_,
        .table_sources = impl_->table_sources,
        .aliases = aliases_,
        .result_definitions = impl_->result_definitions,
        .outer = outer_scope_,
        .scope = scope,
        .aggregate_core = active_core_,
        .aggregate_phase = active_aggregate_phase_,
        .aggregate_permission = scope.aggregates,
        .aggregate_was_established = active_core_is_aggregate_,
    };
  }

  [[nodiscard]] QueryBindingScope SaveQueryScope(BindScope scope) {
    // Moving scratch preserves the element storage borrowed by the name view.
    return QueryBindingScope{
        .query = active_query_,
        .core = active_core_,
        .names = CurrentNameScope(scope),
        .table_sources = std::move(impl_->table_sources),
        .result_columns = std::move(impl_->result_columns),
        .result_definitions = std::move(impl_->result_definitions),
        .where = std::exchange(impl_->where, std::nullopt),
        .sources = std::move(source_states_),
        .aliases = std::move(aliases_),
        .source_begin = active_source_begin_,
        .source_count = active_source_count_,
        .condition_height = active_condition_height_,
        .aggregate_phase = active_aggregate_phase_,
        .is_aggregate = active_core_is_aggregate_,
        .group_by = std::move(active_group_by_),
        .having = std::exchange(active_having_, std::nullopt),
        .outer = outer_scope_,
        .binding_only = binding_only_,
    };
  }

  void RestoreQueryScope(QueryBindingScope scope) {
    active_query_ = scope.query;
    active_core_ = scope.core;
    impl_->table_sources = std::move(scope.table_sources);
    impl_->result_columns = std::move(scope.result_columns);
    impl_->result_definitions = std::move(scope.result_definitions);
    impl_->where = scope.where;
    source_states_ = std::move(scope.sources);
    aliases_ = std::move(scope.aliases);
    active_source_begin_ = scope.source_begin;
    active_source_count_ = scope.source_count;
    active_condition_height_ = scope.condition_height;
    active_aggregate_phase_ = scope.aggregate_phase;
    active_core_is_aggregate_ = scope.is_aggregate;
    active_group_by_ = std::move(scope.group_by);
    active_having_ = scope.having;
    outer_scope_ = scope.outer;
    binding_only_ = scope.binding_only;
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindUpdateRhs(ExpressionId id, BindScope scope) {
    struct RestoreMode {
      bool& mode;
      bool previous;
      ~RestoreMode() { mode = previous; }
    };
    const RestoreMode restore{
        .mode = binding_only_,
        .previous = std::exchange(binding_only_, binding_only_ || !scope.requires_value),
    };
    return BindExpression(id, scope);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindSubquery(const SubqueryExpression& syntax,
                                                             const SourceSpan& span,
                                                             BindScope scope) {
    const bool mutation_query = !std::holds_alternative<SelectStatement>(tree_.statement()) &&
                                active_query_ == BoundQueryId{0};
    const bool binding_only = binding_only_ || !scope.requires_value;
    if (syntax.query.value >= tree_.select_queries().size() ||
        query_binding_base_ > query_bindings_.size() ||
        syntax.query.value >= query_bindings_.size() - query_binding_base_) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "query binding cache index is invalid"));
    }
    const std::size_t binding_index = query_binding_base_ + syntax.query.value;
    if (!query_bindings_[binding_index].has_value()) {
      if (impl_->nested_queries.size() >= std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(
            BinderError(BindErrorCode::kInternalInvariant, span, "bound query identity overflow"));
      }
      const BoundQueryId id{static_cast<std::uint32_t>(impl_->nested_queries.size() + 1U)};
      impl_->nested_queries.push_back(BoundQuery{
          .id = id,
          .parent = active_query_,
          .destination = syntax.kind == SubqueryKind::kScalar ? BoundQueryDestination::kScalar
                                                              : BoundQueryDestination::kExists,
      });
      QueryBindingScope saved = SaveQueryScope(scope);
      const NameScopeView* definition_scope = std::exchange(definition_name_scope_, nullptr);
      outer_scope_ = &saved.names;
      active_query_ = id;
      binding_only_ = binding_only;
      auto bound = BindQuery(tree_.select_query(syntax.query));
      definition_name_scope_ = definition_scope;
      RestoreQueryScope(std::move(saved));
      if (!bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      query_bindings_[binding_index] = id;
    }
    const BoundQueryId query_id = query_bindings_[binding_index].value_or(BoundQueryId{0});
    const BoundQuery& query = Query(query_id);
    if (query.destination == BoundQueryDestination::kScalar && query.output_columns.size() != 1U &&
        scope.requires_scalar_width && !binding_only) {
      return std::unexpected(ValueBinderError(BindErrorCode::kColumnCountMismatch, span,
                                              "sub-select returns " +
                                                  std::to_string(query.output_columns.size()) +
                                                  " columns - expected 1"));
    }
    const BoundAggregateContextCapture* aggregate =
        FindAggregateContextCapture(query.captures, active_query_);
    if (aggregate != nullptr && (scope.aggregates == AggregatePermission::kProhibited ||
                                 scope.aggregates == AggregatePermission::kOuterOnly)) {
      const auto* call = std::get_if<BoundAggregateCallExpression>(
          &impl_->expressions[aggregate->expression.value()].payload);
      const BoundAggregateInvocation* invocation =
          call == nullptr ? nullptr
                          : AggregateInvocationForExpression(aggregate->expression, *call);
      if (invocation == nullptr ||
          invocation->function.value() >= impl_->aggregate_functions.size()) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                           "aggregate context has no function"));
      }
      return std::unexpected(
          BinderError(BindErrorCode::kAggregateMisuse, span,
                      "misuse of aggregate function " +
                          impl_->aggregate_functions[invocation->function.value()].name + "()"));
    }
    if (mutation_query && !query.captures.empty()) {
      return std::unexpected(
          BinderError(BindErrorCode::kUnsupportedFeature, span,
                      "mutation-target correlated subqueries are not supported"));
    }
    const bool contains_owner_aggregate = aggregate != nullptr;
    const bool references_aggregate =
        std::ranges::any_of(query.captures, [](const BoundQueryCapture& capture) {
          return std::holds_alternative<BoundAggregateContextCapture>(capture);
        });
    auto expression = AppendExpression(
        span, BoundSubqueryExpression{.query = query_id},
        ExpressionProperties{.affinity = query.destination == BoundQueryDestination::kScalar
                                             ? Properties(ScalarResultExpression(query_id)).affinity
                                             : TypeAffinity::kNone,
                             .collation = std::nullopt,
                             .contains_aggregate = contains_owner_aggregate,
                             .references_aggregate = references_aggregate});
    if (!expression.has_value()) {
      return std::unexpected(std::move(expression.error()));
    }
    if (auto captured = RecordQueryCaptures(*expression); !captured.has_value()) {
      return std::unexpected(std::move(captured.error()));
    }
    return *expression;
  }

  [[nodiscard]] BoundExpressionId ScalarResultExpression(BoundQueryId id) const {
    const BoundQuery& query = Query(id);
    const BoundQueryCore& last = impl_->query_cores[query.core_begin + query.core_count - 1U];
    if (const auto* values = std::get_if<BoundValuesCore>(&last); values != nullptr) {
      return values->rows.back().front();
    }
    return std::get<BoundSelectCore>(last).result_columns.front().expression;
  }

  [[nodiscard]] std::optional<std::string> ExpressionDeclaredType(BoundExpressionId id) const {
    while (true) {
      const BoundExpression& expression = impl_->expressions[id.value()];
      if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
          alias != nullptr) {
        id = alias->target;
        continue;
      }
      if (const auto* subquery = std::get_if<BoundSubqueryExpression>(&expression.payload);
          subquery != nullptr &&
          Query(subquery->query).destination == BoundQueryDestination::kScalar) {
        id = ScalarResultExpression(subquery->query);
        continue;
      }
      if (const auto* column = std::get_if<BoundColumnExpression>(&expression.payload);
          column != nullptr) {
        const auto type = impl_->source_columns[column->column.value()].declared_type;
        return type.has_value() ? std::optional<std::string>{std::string{*type}} : std::nullopt;
      }
      if (const auto* rowid = std::get_if<BoundRowIdExpression>(&expression.payload);
          rowid != nullptr) {
        for (const BoundSourceColumn& column : impl_->source_columns) {
          if (column.source == rowid->source && column.rowid_alias &&
              column.declared_type.has_value()) {
            return std::string{*column.declared_type};
          }
        }
        return std::string{"INTEGER"};
      }
      return std::nullopt;
    }
  }

  void BeginQueryCore() {
    impl_->table_sources.clear();
    impl_->result_columns.clear();
    impl_->result_definitions.clear();
    impl_->where.reset();
    source_states_.clear();
    aliases_.clear();
    active_source_begin_ = impl_->source_columns.size();
    active_source_count_ = 0;
    active_condition_height_ = 0;
    active_aggregate_phase_ = AggregateOccurrencePhase::kResult;
    active_core_is_aggregate_ = false;
    active_group_by_.clear();
    active_having_.reset();
  }

  void RegisterActiveSourceCollations() {
    if (!impl_->registered_collations.empty() ||
        (impl_->table_sources.size() < 2U &&
         !std::ranges::any_of(impl_->table_sources, [this](const BoundTableSource& source) {
           return source.table.has_value() && !catalog_->table_indexes(*source.table).empty();
         }))) {
      return;
    }
    impl_->registered_collations.reserve(environment_.collations().size());
    for (const Collation* collation : environment_.collations()) {
      impl_->registered_collations.emplace_back(collation->name());
    }
  }

  [[nodiscard]] std::optional<BoundSourceLeafIdentity> AggregateIdentity(
      BoundExpressionId id) const noexcept {
    return BoundSourceLeafIdentityOf(impl_->expressions[id.value()], impl_->source_columns);
  }

  [[nodiscard]] bool AggregateInvocationsEquivalent(const BoundAggregateInvocation& left,
                                                    const BoundAggregateInvocation& right) const {
    if (left.function != right.function || left.collation != right.collation ||
        left.owner != right.owner || left.distinct != right.distinct ||
        left.arguments.size() != right.arguments.size() ||
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
    if (const auto* membership = std::get_if<BoundInExpression>(&expression.payload);
        membership != nullptr) {
      return IsConstantOrBinaryGroupTerm(membership->operand) &&
             std::ranges::all_of(membership->values, [this](BoundExpressionId value) {
               return IsConstantOrBinaryGroupTerm(value);
             });
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

  [[nodiscard]] bool CollectAggregateLeaves(BoundExpressionId id,
                                            std::vector<BoundSourceCapture>& leaves) const {
    auto visitor = [&](const BoundSourceLeafIdentity& identity,
                       std::optional<BoundExpressionId> value) {
      if (!OwnsActiveSource(identity.source)) {
        return true;
      }
      const auto existing = std::ranges::find(leaves, identity, &BoundSourceCapture::identity);
      if (existing == leaves.end()) {
        leaves.push_back(BoundSourceCapture{.identity = identity, .value = value});
      } else if (!existing->value.has_value()) {
        existing->value = value;
      }
      return true;
    };
    return VisitSourceLeaves(impl_->expressions, impl_->source_columns, id, CaptureLookup(),
                             BoundSourceDependency::kBinding, visitor);
  }

  [[nodiscard]] BindExpected<void> PublishAggregateLeaves(
      std::span<const BoundSourceCapture> leaves, std::vector<BoundExpressionId>& output) {
    for (const BoundSourceCapture& leaf : leaves) {
      if (leaf.value.has_value()) {
        output.push_back(*leaf.value);
        continue;
      }
      auto value = leaf.identity.column.has_value() ? BindColumn(*leaf.identity.column, {})
                                                    : BindRowId(leaf.identity.source, {});
      if (!value.has_value()) {
        return std::unexpected(std::move(value.error()));
      }
      output.push_back(*value);
    }
    return {};
  }

  [[nodiscard]] bool CanOmitCountOrderingRepresentatives(
      const BoundAggregateProgram& program, const AggregateLeafAnalysisView& view) const {
    if (!program.group_by.empty() || program.having.has_value() || view.where.has_value() ||
        view.table_sources.size() != 1U || view.result_columns.size() != 1U ||
        program.invocations.size() != 1U) {
      return false;
    }
    const BoundSourceKind kind = view.table_sources.front().kind;
    if (kind != BoundSourceKind::kCatalogTable && kind != BoundSourceKind::kSchemaTable) {
      return false;
    }
    const auto* call = std::get_if<BoundAggregateCallExpression>(
        &impl_->expressions[view.result_columns.front().expression.value()].payload);
    if (call == nullptr || call->invocation != program.invocations.front()) {
      return false;
    }
    const BoundAggregateInvocation& invocation =
        impl_->aggregate_invocations[call->invocation.value()];
    return NamesEqual(impl_->aggregate_functions[invocation.function.value()].name, "count") &&
           invocation.arguments.empty() && !invocation.distinct && !invocation.filter.has_value() &&
           invocation.order_by.empty();
  }

  [[nodiscard]] BindExpected<void> AnalyzeAggregateLeaves(BoundAggregateProgram& program,
                                                          const AggregateLeafAnalysisView& view) {
    std::vector<BoundSourceCapture> source_leaves;
    std::vector<BoundSourceCapture> representative_leaves;
    bool valid = true;
    const auto collect = [&](BoundExpressionId id, std::vector<BoundSourceCapture>& leaves) {
      valid = valid && CollectAggregateLeaves(id, leaves);
    };
    for (const BoundGroupingTerm& term : program.group_by) {
      collect(term.expression, source_leaves);
    }
    for (const BoundAggregateInvocationId id : program.invocations) {
      const BoundAggregateInvocation& invocation = impl_->aggregate_invocations[id.value()];
      for (const BoundExpressionId argument : invocation.arguments) {
        collect(argument, source_leaves);
      }
      if (invocation.filter.has_value()) {
        collect(*invocation.filter, source_leaves);
      }
      const BoundAggregateFunction& function =
          impl_->aggregate_functions[invocation.function.value()];
      if (function.extremum == BoundAggregateExtremumKind::kNone) {
        for (const BoundOrderingTerm& term : invocation.order_by) {
          collect(term.expression, source_leaves);
        }
      }
    }
    for (const BoundResultColumn& result : view.result_columns) {
      collect(result.expression, representative_leaves);
    }
    for (const BoundOrderingTerm& term : view.order_by) {
      if (!view.order_by_uses_result_columns) {
        collect(term.expression, representative_leaves);
        continue;
      }
      if (!term.result_column.has_value() || *term.result_column >= view.result_columns.size()) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                           "compound aggregate ordering result is invalid"));
      }
      collect(view.result_columns[*term.result_column].expression, representative_leaves);
    }
    if (program.having.has_value()) {
      collect(*program.having, representative_leaves);
    }

    if (!valid) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                         "aggregate has invalid retained source dependencies"));
    }
    std::vector<BoundSourceLeafIdentity> retained;
    retained.reserve(source_leaves.size());
    for (const BoundSourceCapture& leaf : source_leaves) {
      retained.push_back(leaf.identity);
    }
    for (const BoundSourceCapture& leaf : representative_leaves) {
      if (!std::ranges::contains(retained, leaf.identity)) {
        retained.push_back(leaf.identity);
      }
    }
    if (retained.size() > options_.maximum_aggregate_terms) {
      const SourceSpan span =
          program.having.has_value() ? impl_->expressions[program.having->value()].span
          : !view.result_columns.empty() &&
                  view.result_columns.front().expression.value() < impl_->expressions.size()
              ? impl_->expressions[view.result_columns.front().expression.value()].span
              : SourceSpan{};
      return std::unexpected(BinderError(BindErrorCode::kAggregateTermLimitExceeded, span,
                                         "too many aggregate terms"));
    }
    if (CanOmitCountOrderingRepresentatives(program, view)) {
      representative_leaves.clear();
    }
    std::vector<BoundExpressionId> published_source_leaves;
    std::vector<BoundExpressionId> published_representative_leaves;
    if (auto published = PublishAggregateLeaves(source_leaves, published_source_leaves);
        !published.has_value()) {
      return published;
    }
    if (auto published =
            PublishAggregateLeaves(representative_leaves, published_representative_leaves);
        !published.has_value()) {
      return published;
    }
    program.source_leaves = std::move(published_source_leaves);
    program.representative_leaves = std::move(published_representative_leaves);
    return {};
  }

  [[nodiscard]] BindExpected<void> FinalizeAggregatePhase(BoundAggregateProgram& program,
                                                          AggregateOccurrencePhase phase) {
    if (program.having_invocation_begin > program.invocations.size()) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                         "aggregate invocation phase boundary is invalid"));
    }
    for (PendingAggregateInvocation& pending : pending_aggregates_) {
      if (pending.finalized || pending.owner_query != active_query_ ||
          pending.owner_core != active_core_ || pending.phase != phase) {
        continue;
      }
      if (auto prepared = PrepareOuterAggregateInvocation(pending); !prepared.has_value()) {
        return prepared;
      }
      std::optional<std::size_t> selected_index;
      for (std::size_t index = 0; index < program.invocations.size(); ++index) {
        const BoundAggregateInvocationId id = program.invocations[index];
        if (AggregateInvocationsEquivalent(impl_->aggregate_invocations[id.value()],
                                           pending.invocation)) {
          selected_index = index;
          break;
        }
      }

      BoundAggregateInvocationId selected{0};
      if (selected_index.has_value()) {
        selected = program.invocations[*selected_index];
        if (phase == AggregateOccurrencePhase::kOrderBy &&
            *selected_index >= program.having_invocation_begin) {
          std::rotate(
              program.invocations.begin() +
                  static_cast<std::ptrdiff_t>(program.having_invocation_begin),
              program.invocations.begin() + static_cast<std::ptrdiff_t>(*selected_index),
              program.invocations.begin() + static_cast<std::ptrdiff_t>(*selected_index + 1U));
          ++program.having_invocation_begin;
        }
      } else {
        if (program.invocations.size() >= options_.maximum_aggregate_terms) {
          return std::unexpected(BinderError(BindErrorCode::kAggregateTermLimitExceeded,
                                             pending.invocation.span, "too many aggregate terms"));
        }
        if (impl_->aggregate_invocations.size() >= std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant,
                                             pending.invocation.span,
                                             "bound aggregate invocation ID overflow"));
        }
        selected = BoundAggregateInvocationId{
            static_cast<std::uint32_t>(impl_->aggregate_invocations.size())};
        impl_->aggregate_invocations.push_back(std::move(pending.invocation));
        if (phase == AggregateOccurrencePhase::kOrderBy) {
          program.invocations.insert(
              program.invocations.begin() +
                  static_cast<std::ptrdiff_t>(program.having_invocation_begin),
              selected);
          ++program.having_invocation_begin;
        } else {
          program.invocations.push_back(selected);
        }
      }
      auto* call = std::get_if<BoundAggregateCallExpression>(
          &impl_->expressions[pending.expression.value()].payload);
      if (call == nullptr) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant,
                                           impl_->expressions[pending.expression.value()].span,
                                           "pending aggregate expression is invalid"));
      }
      call->invocation = selected;
      pending.finalized = true;
    }
    return {};
  }

  [[nodiscard]] BindExpected<std::optional<BoundAggregateProgram>> FinalizeAggregateProgram() {
    std::optional<BoundAggregateProgram> aggregate;
    const auto pending_count = static_cast<std::size_t>(std::ranges::count_if(
        pending_aggregates_, [this](const PendingAggregateInvocation& pending) {
          return !pending.finalized && pending.owner_query == active_query_ &&
                 pending.owner_core == active_core_;
        }));
    if (pending_count == 0U && active_group_by_.empty() && !active_having_.has_value()) {
      return aggregate;
    }
    BoundAggregateProgram program;
    program.invocations.reserve(pending_count);
    if (auto finalized = FinalizeAggregatePhase(program, AggregateOccurrencePhase::kResult);
        !finalized.has_value()) {
      return std::unexpected(std::move(finalized.error()));
    }
    program.having_invocation_begin = program.invocations.size();
    if (auto finalized = FinalizeAggregatePhase(program, AggregateOccurrencePhase::kOrderBy);
        !finalized.has_value()) {
      return std::unexpected(std::move(finalized.error()));
    }
    if (auto finalized = FinalizeAggregatePhase(program, AggregateOccurrencePhase::kHaving);
        !finalized.has_value()) {
      return std::unexpected(std::move(finalized.error()));
    }
    program.group_by = std::move(active_group_by_);
    program.having = active_having_;
    aggregate = std::move(program);
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
      BindExpected<void> analyzed =
          AnalyzeAggregateLeaves(**aggregate, AggregateLeafAnalysisView{
                                                  .table_sources = impl_->table_sources,
                                                  .result_columns = impl_->result_columns,
                                                  .where = impl_->where,
                                                  .order_by = ActiveQuery().order_by,
                                              });
      if (!analyzed.has_value()) {
        return analyzed;
      }
    }
    core_metadata_[active_core_] = CoreBindingMetadata{
        .source_states = std::move(source_states_),
        .source_column_begin = active_source_begin_,
        .source_column_count = active_source_count_,
        .result_definitions = std::move(impl_->result_definitions),
    };
    impl_->query_cores[active_core_] = BoundSelectCore{
        .quantifier = ActiveQuery().destination == BoundQueryDestination::kExists
                          ? SelectQuantifier::kDefault
                          : quantifier,
        .table_sources = std::move(impl_->table_sources),
        .source_column_begin = active_source_begin_,
        .source_column_count = active_source_count_,
        .result_columns = std::move(impl_->result_columns),
        .where = impl_->where,
        .aggregate = std::move(*aggregate),
    };
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
    const bool has_right = std::ranges::any_of(core.from, [](const TableSource& candidate) {
      return SourceJoinPreservesRight(candidate.join);
    });
    for (std::size_t index = 1; index < core.from.size(); ++index) {
      const auto& on = core.from[index].on;
      if (!on.has_value()) {
        continue;
      }
      auto condition = BindExpression(
          *on, BindScope{.source_columns = true, .result_aliases = true}, active_condition_height_);
      if (!condition.has_value()) {
        return std::unexpected(std::move(condition.error()));
      }
      BoundTableSource& bound_source = impl_->table_sources[index];
      bound_source.on = *condition;
      if (SourceJoinPreservesLeft(bound_source.join) || has_right) {
        bool forward = false;
        auto visitor = [&](BoundSourceId owner) { forward = forward || owner > bound_source.id; };
        if (!VisitSourceOwners(impl_->expressions, impl_->source_columns, *condition,
                               CaptureLookup(), visitor)) {
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
                                           .aggregates = AggregatePermission::kAllowed,
                                       });
      if (!having.has_value()) {
        return std::unexpected(std::move(having.error()));
      }
      active_having_ = *having;
    }
    if (core.where.has_value()) {
      BindExpected<BoundExpressionId> where =
          BindExpression(*core.where, BindScope{.source_columns = true, .result_aliases = true},
                         active_condition_height_);
      if (!where.has_value()) {
        return std::unexpected(std::move(where.error()));
      }
      impl_->where = *where;
    }
    active_aggregate_phase_ = AggregateOccurrencePhase::kResult;
    return {};
  }

  [[nodiscard]] bool IsConstantExpression(BoundExpressionId id,
                                          bool allow_truth_tests = true) const {
    const BoundExpression& expression = impl_->expressions[id.value()];
    return std::visit(
        [this, allow_truth_tests](const auto& payload) {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, BoundLiteralExpression> ||
                        std::is_same_v<Payload, BoundParameterExpression>) {
            return true;
          } else if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                               std::is_same_v<Payload, BoundLikelihoodExpression> ||
                               std::is_same_v<Payload, BoundCollateExpression>) {
            return IsConstantExpression(payload.operand, allow_truth_tests);
          } else if constexpr (std::is_same_v<Payload, BoundBinaryExpression>) {
            return IsConstantExpression(payload.left, allow_truth_tests) &&
                   IsConstantExpression(payload.right, allow_truth_tests);
          } else if constexpr (std::is_same_v<Payload, BoundComparisonExpression>) {
            return (allow_truth_tests || (payload.comparison != SqlComparison::kIs &&
                                          payload.comparison != SqlComparison::kIsNot)) &&
                   IsConstantExpression(payload.left, allow_truth_tests) &&
                   IsConstantExpression(payload.right, allow_truth_tests);
          } else if constexpr (std::is_same_v<Payload, BoundTruthTestExpression>) {
            return allow_truth_tests && IsConstantExpression(payload.operand, allow_truth_tests);
          } else if constexpr (std::is_same_v<Payload, BoundAliasReferenceExpression>) {
            return IsConstantExpression(payload.target, allow_truth_tests);
          } else if constexpr (std::is_same_v<Payload, BoundInExpression>) {
            return IsConstantExpression(payload.operand, allow_truth_tests) &&
                   std::ranges::all_of(payload.values,
                                       [this, allow_truth_tests](BoundExpressionId value) {
                                         return IsConstantExpression(value, allow_truth_tests);
                                       });
          } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression>) {
            const BoundScalarFunction& function = impl_->functions[payload.function.value()];
            return function.deterministic &&
                   std::ranges::all_of(payload.arguments,
                                       [this, allow_truth_tests](BoundExpressionId argument) {
                                         return IsConstantExpression(argument, allow_truth_tests);
                                       });
          } else if constexpr (std::is_same_v<Payload, BoundCoalesceExpression> ||
                               std::is_same_v<Payload, BoundConditionalExpression>) {
            return std::ranges::all_of(payload.arguments,
                                       [this, allow_truth_tests](BoundExpressionId argument) {
                                         return IsConstantExpression(argument, allow_truth_tests);
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
    const bool requires_value = ActiveQuery().destination != BoundQueryDestination::kExists;
    const BindScope empty_scope{
        .source_columns = true,
        .result_aliases = false,
        .requires_value = requires_value,
    };
    std::vector<std::vector<BoundExpressionId>> rows;
    rows.reserve(core.rows.size());
    impl_->result_definitions.reserve(column_count);
    for (const std::vector<ExpressionId>& syntax_row : core.rows) {
      if (syntax_row.size() != column_count) {
        return std::unexpected(BinderError(BindErrorCode::kValuesColumnCount, core.span,
                                           "all VALUES must have the same number of terms"));
      }
      std::vector<BoundExpressionId> row;
      row.reserve(syntax_row.size());
      for (std::size_t index = 0; index < syntax_row.size(); ++index) {
        const ExpressionId expression = syntax_row[index];
        const std::size_t first_query = impl_->nested_queries.size();
        BindExpected<BoundExpressionId> bound = BindExpression(expression, empty_scope);
        if (!bound.has_value()) {
          return std::unexpected(std::move(bound.error()));
        }
        row.push_back(*bound);
        if (rows.empty()) {
          impl_->result_definitions.push_back(ResultDefinition{
              .syntax = expression,
              .has_subqueries = impl_->nested_queries.size() != first_query,
              .value_checked = requires_value && !binding_only_,
          });
        } else {
          ResultDefinition& definition = impl_->result_definitions[index];
          definition.additional_syntax.push_back(expression);
          definition.has_subqueries =
              definition.has_subqueries || impl_->nested_queries.size() != first_query;
        }
      }
      rows.push_back(std::move(row));
    }

    std::vector<BoundResultColumn> results;
    results.reserve(column_count);
    for (std::size_t index = 0; index < column_count; ++index) {
      const BoundExpressionId expression = rows.front()[index];
      results.push_back(BoundResultColumn{
          .expression = expression,
          .name = "column" + std::to_string(index + 1U),
          .declared_type = ExpressionDeclaredType(expression),
          .affinity = Properties(expression).affinity,
      });
    }

    auto schedule = BoundValuesEvaluationSchedule::kConsumerFiltered;
    std::vector<BoundValuesChunk> chunks;
    if (core.rows.size() > 1U) {
      const auto construction = std::ranges::find_if(
          values_constructions_,
          [&](const NativeValuesConstruction& value) { return value.syntax == &core; });
      if (construction == values_constructions_.end() || construction->chunks.empty()) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, core.span,
                                           "VALUES construction schedule is missing"));
      }
      schedule = construction->chunks.front().schedule;
      if (std::ranges::any_of(construction->chunks, [&](const BoundValuesChunk& chunk) {
            return chunk.schedule != schedule;
          })) {
        chunks = construction->chunks;
      }
    }

    core_metadata_[active_core_] = CoreBindingMetadata{
        .source_states = {},
        .result_definitions = std::move(impl_->result_definitions),
    };
    impl_->query_cores[active_core_] = BoundValuesCore{
        .rows = std::move(rows),
        .result_columns = std::move(results),
        .schedule = schedule,
        .chunks = std::move(chunks),
    };
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

  void PublishResultColumns() {
    const auto cores = ActiveCores();
    if (cores.empty()) {
      return;
    }
    const std::span<const BoundResultColumn> leftmost = BoundCoreResultColumns(cores.front());
    ActiveQuery().output_columns.assign(leftmost.begin(), leftmost.end());
    const bool has_set_operator = std::ranges::any_of(
        ActiveQuery().compound_operators,
        [](CompoundOperator operation) { return operation != CompoundOperator::kUnionAll; });
    const bool has_explicit_order_collation =
        std::ranges::any_of(ActiveQuery().order_by,
                            [](const BoundOrderingTerm& term) { return term.explicit_collation; });
    if (!has_set_operator || !has_explicit_order_collation || cores.size() < 2U) {
      return;
    }
    const std::span<const BoundResultColumn> rightmost = BoundCoreResultColumns(cores.back());
    if (ActiveQuery().output_columns.size() != rightmost.size()) {
      return;
    }
    for (std::size_t index = 0; index < ActiveQuery().output_columns.size(); ++index) {
      ActiveQuery().output_columns[index].declared_type = rightmost[index].declared_type;
      ActiveQuery().output_columns[index].affinity = rightmost[index].affinity;
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
    const auto cores = ActiveCores();
    const std::size_t expected = BoundCoreResultColumns(cores.front()).size();
    for (std::size_t index = 1; index < cores.size(); ++index) {
      if (BoundCoreResultColumns(cores[index]).size() != expected) {
        const CompoundOperator operation = ActiveQuery().compound_operators[index - 1U];
        return std::unexpected(
            BinderError(BindErrorCode::kCompoundColumnCount, select.compounds[index - 1U].span,
                        "SELECTs to the left and right of " + std::string{CompoundName(operation)} +
                            " do not have the same number of result columns"));
      }
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> BindCompoundCollations(SourceSpan span) {
    const auto cores = ActiveCores();
    const std::size_t column_count = BoundCoreResultColumns(cores.front()).size();
    ActiveQuery().compound_collations.reserve(column_count);
    for (std::size_t column = 0; column < column_count; ++column) {
      const auto selected = BoundCompoundResultCollation(cores, impl_->expressions, column);
      BindExpected<BoundCollationId> collation = selected.has_value()
                                                     ? BindExpected<BoundCollationId>{*selected}
                                                     : InternCollation("BINARY", span);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      ActiveQuery().compound_collations.push_back(*collation);
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
    if (ActiveQuery().destination == BoundQueryDestination::kExists) {
      return {};
    }
    const auto cores = ActiveCores();
    for (std::size_t index = 0; index < cores.size(); ++index) {
      const auto* core = std::get_if<BoundSelectCore>(&cores[index]);
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

  [[nodiscard]] BindExpected<void> RequireSetCompoundCollations(SourceSpan span) {
    const BoundQuery& query = ActiveQuery();
    if (query.compound_operators.empty()) {
      return {};
    }
    struct Range {
      std::size_t first;
      std::size_t last;
      bool ordered;
      bool limit_one;
    };
    const auto cores = ActiveCores();
    const bool wrapper = BoundQueryHasCompoundOrderWrapper(query);
    Range current{
        .first = 0,
        .last = cores.size() - 1U,
        .ordered = !wrapper && !query.order_by.empty(),
        .limit_one = !wrapper && BoundQueryHasSingleRowDemand(query, impl_->expressions),
    };
    std::vector<Range> pending;
    while (true) {
      if (current.first < current.last) {
        const CompoundOperator operation = query.compound_operators[current.last - 1U];
        const bool merge =
            BoundCompoundRequiresMerge(operation, current.ordered, current.limit_one);
        if (merge && operation != CompoundOperator::kUnionAll) {
          const auto local = cores.subspan(current.first, current.last - current.first + 1U);
          for (std::size_t column = 0; column < BoundCoreResultColumns(local.front()).size();
               ++column) {
            const auto selected = BoundCompoundResultCollation(local, impl_->expressions, column);
            auto collation = selected.has_value() ? BindExpected<BoundCollationId>{*selected}
                                                  : InternCollation("BINARY", span);
            if (!collation.has_value()) {
              return std::unexpected(std::move(collation.error()));
            }
            if (auto required = RequireCollation(*collation, span); !required.has_value()) {
              return required;
            }
          }
        }
        const auto split =
            BoundCompoundSplit(std::span<const CompoundOperator>{query.compound_operators}.subspan(
                                   current.first, current.last - current.first),
                               merge);
        assert(split.has_value());
        const std::size_t middle = current.first + *split;
        if (middle < current.last) {
          pending.push_back(Range{.first = middle,
                                  .last = current.last,
                                  .ordered = merge,
                                  .limit_one = !merge && current.limit_one});
        }
        if (current.first + 1U < middle) {
          current.last = middle - 1U;
          current.ordered = merge;
          current.limit_one = !merge && current.limit_one;
          continue;
        }
      }
      if (pending.empty()) {
        break;
      }
      current = pending.back();
      pending.pop_back();
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
    const std::size_t global_index = ActiveQuery().core_begin + core_index;
    active_core_ = global_index;
    const CoreBindingMetadata& metadata = core_metadata_[global_index];
    source_states_ = metadata.source_states;
    active_source_begin_ = metadata.source_column_begin;
    active_source_count_ = metadata.source_column_count;
    aliases_.clear();
    impl_->table_sources.clear();
    if (const auto* select = std::get_if<BoundSelectCore>(&impl_->query_cores[global_index]);
        select != nullptr) {
      impl_->table_sources = select->table_sources;
      active_core_is_aggregate_ = select->aggregate.has_value();
    } else {
      active_core_is_aggregate_ = false;
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

  [[nodiscard]] bool ResultDefinitionNeedsAggregateOrderingCopy(
      const ResultDefinition& definition, const BoundResultColumn& result) const noexcept {
    return definition.has_subqueries && Properties(result.expression).references_aggregate;
  }

  [[nodiscard]] BindExpected<void> BindDirectAggregateOrderingCopy(std::size_t result_column,
                                                                   SourceSpan span) {
    if (result_column >= impl_->result_columns.size() ||
        result_column >= impl_->result_definitions.size()) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "aggregate ordering result definition is invalid"));
    }
    const ResultDefinition& definition = impl_->result_definitions[result_column];
    if (!ResultDefinitionNeedsAggregateOrderingCopy(definition,
                                                    impl_->result_columns[result_column])) {
      return {};
    }
    const BindScope copy_scope{
        .source_columns = true,
        .result_aliases = false,
        .aggregates = AggregatePermission::kAllowed,
        .requires_value = ActiveQuery().destination != BoundQueryDestination::kExists,
    };
    const NameScopeView names = CurrentNameScope(copy_scope);
    auto copied = BindResultDefinitionCopy(definition, names, copy_scope, span);
    if (!copied.has_value()) {
      return std::unexpected(std::move(copied.error()));
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> BindCompoundAggregateOrderingCopies() {
    if (ActiveQuery().destination == BoundQueryDestination::kExists) {
      return {};
    }
    QueryBindingScope saved = SaveQueryScope(BindScope{
        .source_columns = true,
        .result_aliases = false,
        .aggregates = AggregatePermission::kAllowed,
        .requires_value = true,
    });
    BindExpected<void> registered = [&]() -> BindExpected<void> {
      const std::size_t core_count = ActiveQuery().core_count;
      const std::size_t order_count = ActiveQuery().order_by.size();
      for (std::size_t core_index = 0; core_index < core_count; ++core_index) {
        ActivateQueryCore(core_index);
        active_aggregate_phase_ = AggregateOccurrencePhase::kOrderBy;
        bool copied_definition = false;
        for (std::size_t order_index = 0; order_index < order_count; ++order_index) {
          const std::optional<std::size_t> result_column =
              ActiveQuery().order_by[order_index].result_column;
          if (!result_column.has_value()) {
            return std::unexpected(
                BinderError(BindErrorCode::kInternalInvariant, {},
                            "compound aggregate ordering has no selected result column"));
          }

          const std::size_t global_index = ActiveQuery().core_begin + core_index;
          const auto* select = std::get_if<BoundSelectCore>(&impl_->query_cores[global_index]);
          if (select == nullptr || !select->aggregate.has_value()) {
            continue;
          }
          CoreBindingMetadata& metadata = core_metadata_[global_index];
          if (*result_column >= select->result_columns.size() ||
              *result_column >= metadata.result_definitions.size()) {
            return std::unexpected(
                BinderError(BindErrorCode::kInternalInvariant, {},
                            "compound aggregate ordering result definition is invalid"));
          }
          ResultDefinition& definition = metadata.result_definitions[*result_column];
          if (!ResultDefinitionNeedsAggregateOrderingCopy(definition,
                                                          select->result_columns[*result_column])) {
            continue;
          }

          const BindScope copy_scope{
              .source_columns = true,
              .result_aliases = false,
              .aggregates = AggregatePermission::kAllowed,
              .requires_value = true,
          };
          const NameScopeView names{
              .query = active_query_,
              .sources = source_states_,
              .table_sources = impl_->table_sources,
              .aliases = {},
              .result_definitions = metadata.result_definitions,
              .outer = outer_scope_,
              .scope = copy_scope,
              .aggregate_core = global_index,
              .aggregate_phase = active_aggregate_phase_,
              .aggregate_permission = copy_scope.aggregates,
              .aggregate_was_established = active_core_is_aggregate_,
          };
          const SourceSpan span = definition.syntax.has_value()
                                      ? tree_.expression(*definition.syntax).span
                                      : SourceSpan{};
          const BindingArenaCheckpoint checkpoint = SaveBindingArena();
          const std::size_t pending_begin = pending_aggregates_.size();
          auto copied = BindResultDefinitionCopy(definition, names, copy_scope, span);
          if (!copied.has_value()) {
            BindError error = std::move(copied.error());
            RestoreBindingArena(checkpoint);
            return std::unexpected(std::move(error));
          }
          std::vector<ExpressionId> local_calls;
          std::optional<SourceSpan> replay_error;
          for (const PendingAggregateInvocation& pending :
               std::span{pending_aggregates_}.subspan(pending_begin)) {
            if (pending.finalized || pending.owner_query != active_query_ ||
                pending.owner_core != global_index) {
              continue;
            }
            bool unsupported = false;
            auto dependency = [&](const BoundQueryCapture& capture) {
              if (const auto* context = std::get_if<BoundQueryContextCapture>(&capture);
                  context != nullptr) {
                unsupported = unsupported || context->query.value() > checkpoint.queries;
              } else if (const auto* aggregate =
                             std::get_if<BoundAggregateContextCapture>(&capture);
                         aggregate != nullptr) {
                unsupported = unsupported || aggregate->query != active_query_;
              }
              return !unsupported;
            };
            const auto inspect = [&](BoundExpressionId id) {
              return VisitQueryDependencies(impl_->expressions, impl_->source_columns, id,
                                            CaptureLookup(), BoundSourceDependency::kBinding,
                                            dependency);
            };
            bool valid = std::ranges::all_of(pending.invocation.arguments, inspect);
            for (const BoundOrderingTerm& ordering : pending.invocation.order_by) {
              valid = valid && inspect(ordering.expression);
            }
            if (pending.invocation.filter.has_value()) {
              valid = valid && inspect(*pending.invocation.filter);
            }
            if (!valid || unsupported) {
              replay_error = pending.invocation.span;
              break;
            }
            local_calls.push_back(pending.syntax);
          }
          RestoreBindingArena(checkpoint);
          if (replay_error.has_value()) {
            return std::unexpected(
                BinderError(BindErrorCode::kUnsupportedFeature, *replay_error,
                            "compound aggregate ordering replay is not supported"));
          }
          for (const ExpressionId local_call : local_calls) {
            const SourceSpan call_span = tree_.expression(local_call).span;
            auto replayed =
                BindExpressionTemplateCopy(local_call, names, copy_scope, call_span, true);
            if (!replayed.has_value()) {
              return std::unexpected(std::move(replayed.error()));
            }
            copied_definition = true;
          }
        }
        if (!copied_definition) {
          continue;
        }

        const std::size_t global_index = ActiveQuery().core_begin + core_index;
        auto* select = std::get_if<BoundSelectCore>(&impl_->query_cores[global_index]);
        if (select == nullptr || !select->aggregate.has_value()) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                             "compound aggregate program is unavailable"));
        }
        BoundAggregateProgram& program = *select->aggregate;
        program.invocations.reserve(program.invocations.size() + pending_aggregates_.size());
        if (auto finalized = FinalizeAggregatePhase(program, AggregateOccurrencePhase::kOrderBy);
            !finalized.has_value()) {
          return finalized;
        }
        select = std::get_if<BoundSelectCore>(&impl_->query_cores[global_index]);
        if (select == nullptr || !select->aggregate.has_value()) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                             "compound aggregate program was invalidated"));
        }
        if (auto analyzed = AnalyzeAggregateLeaves(*select->aggregate,
                                                   AggregateLeafAnalysisView{
                                                       .table_sources = select->table_sources,
                                                       .result_columns = select->result_columns,
                                                       .where = select->where,
                                                       .order_by = ActiveQuery().order_by,
                                                       .order_by_uses_result_columns = true,
                                                   });
            !analyzed.has_value()) {
          return analyzed;
        }
      }
      return {};
    }();
    RestoreQueryScope(std::move(saved));
    return registered;
  }

  [[nodiscard]] BindExpected<void> BindCompoundOrderBy(const SelectStatement& select) {
    const std::span<const BoundResultColumn> output = BoundCoreResultColumns(ActiveCores().front());
    ActiveQuery().order_by.reserve(select.order_by.size());
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
             core_index < ActiveQuery().core_count && !result_column.has_value(); ++core_index) {
          ActivateQueryCore(core_index);
          const std::span<const BoundResultColumn> results =
              BoundCoreResultColumns(ActiveCores()[core_index]);
          const auto& definitions =
              core_metadata_[ActiveQuery().core_begin + core_index].result_definitions;

          if (requested->has_value()) {
            for (std::size_t column = 0; column < results.size(); ++column) {
              bool matches = NamesEqual(results[column].name, **requested);
              if (!matches && column < definitions.size() &&
                  definitions[column].syntax.has_value()) {
                BindExpected<std::optional<std::string>> syntax_name =
                    CompoundOrderByName(*definitions[column].syntax);
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

          const BindingArenaCheckpoint checkpoint = SaveBindingArena();
          BindExpected<BoundExpressionId> bound = BindExpression(
              comparable,
              BindScope{
                  .source_columns = true,
                  .result_aliases = false,
                  .aggregates = AggregatePermission::kAllowed,
                  .requires_value = ActiveQuery().destination != BoundQueryDestination::kExists,
              });
          if (!bound.has_value()) {
            RestoreBindingArena(checkpoint);
            continue;
          }
          for (std::size_t column = 0; column < results.size(); ++column) {
            if (BoundSemanticallyEquivalent(*bound, results[column].expression)) {
              result_column = column;
              break;
            }
          }
          RestoreBindingArena(checkpoint);
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
              : ActiveQuery().compound_collations[*result_column];
      if (ActiveQuery().destination != BoundQueryDestination::kExists) {
        BindExpected<void> required = RequireCollation(collation, term.span);
        if (!required.has_value()) {
          return required;
        }
      }
      const SortOrder order =
          term.order == SortOrder::kDescending ? SortOrder::kDescending : SortOrder::kAscending;
      const BoundNullPlacement null_placement =
          term.null_order == NullOrder::kFirst  ? BoundNullPlacement::kFirst
          : term.null_order == NullOrder::kLast ? BoundNullPlacement::kLast
          : order == SortOrder::kDescending     ? BoundNullPlacement::kLast
                                                : BoundNullPlacement::kFirst;
      ActiveQuery().order_by.push_back(BoundOrderingTerm{
          .expression = *expression,
          .collation = collation,
          .order = order,
          .null_placement = null_placement,
          .result_column = result_column,
          .explicit_collation = OrderByHasExplicitCollation(term.expression),
      });
    }
    if (auto registered = BindCompoundAggregateOrderingCopies(); !registered.has_value()) {
      return registered;
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
      bool ordinal_aggregate_reference = false;
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
        const ResultDefinition& definition = impl_->result_definitions[*result_column];
        ordinal_aggregate_reference =
            Properties(impl_->result_columns[*result_column].expression).references_aggregate;
        const BindScope copy_scope{
            .source_columns = true,
            .result_aliases = false,
            .aggregates = AggregatePermission::kGrouping,
        };
        const NameScopeView names = CurrentNameScope(copy_scope);
        if (definition.has_subqueries) {
          auto copied = BindResultDefinitionCopy(definition, names, copy_scope, span);
          if (!copied.has_value()) {
            return std::unexpected(std::move(copied.error()));
          }
          expression = BindOrderByResultReference(syntax, *copied);
        } else {
          if (auto validated = ValidateResultDefinition(*result_column, names, span);
              !validated.has_value()) {
            return std::unexpected(std::move(validated.error()));
          }
          expression = BindOrderByResultReference(syntax, *result_column);
        }
      } else {
        expression = BindExpression(syntax, BindScope{
                                                .source_columns = true,
                                                .result_aliases = true,
                                                .aggregates = AggregatePermission::kGrouping,
                                            });
      }
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      if (Properties(*expression).contains_aggregate || ordinal_aggregate_reference) {
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
          .ordinal_reference = ordinal->has_value(),
      });
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> BindLimit(const std::optional<LimitClause>& syntax) {
    if (!syntax.has_value()) {
      return {};
    }
    const BindScope empty_scope{.source_columns = false, .result_aliases = false};
    const std::size_t height = LimitExpressionHeight(
        *syntax, [this](ExpressionId id) { return ExpressionSyntaxHeight(id); });
    BindExpected<BoundExpressionId> limit = BindExpression(syntax->limit, empty_scope, height);
    if (!limit.has_value()) {
      return std::unexpected(std::move(limit.error()));
    }
    BoundLimit bound_limit{.limit = *limit};
    if (syntax->offset.has_value()) {
      BindExpected<BoundExpressionId> offset = BindExpression(*syntax->offset, empty_scope, height);
      if (!offset.has_value()) {
        return std::unexpected(std::move(offset.error()));
      }
      bound_limit.offset = *offset;
    }
    ActiveQuery().limit = bound_limit;
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
    if (const auto* subquery = std::get_if<SubqueryExpression>(&expression.payload);
        subquery != nullptr) {
      return subquery->query.value < tree_.select_queries().size() &&
             (subquery->kind == SubqueryKind::kScalar || subquery->kind == SubqueryKind::kExists);
    }
    if (const auto* membership = std::get_if<InExpression>(&expression.payload);
        membership != nullptr) {
      return IdIsValid(membership->operand) && SpanIsValid(membership->operator_span) &&
             SpanIsValid(membership->list_span) &&
             !std::holds_alternative<WildcardExpression>(
                 tree_.expression(membership->operand).payload) &&
             std::ranges::all_of(membership->values,
                                 [this](ExpressionId id) { return IdIsValid(id); });
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

  [[nodiscard]] BindExpected<void> AssignParameter(const ParameterOccurrence& occurrence,
                                                   ParameterAssignmentState& assignment) {
    auto& named_slots = assignment.named_slots;
    auto& maximum_slot = assignment.maximum_slot;
    const std::string_view token = SpanText(occurrence.span);
    std::size_t slot = 0;
    std::optional<std::string> name;
    if (token == "?") {
      if (maximum_slot >= options_.maximum_parameters) {
        return std::unexpected(BinderError(BindErrorCode::kParameterLimitExceeded, occurrence.span,
                                           "too many SQL variables"));
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
      {
        const std::string probe{token};
        if (named_slots.has_value()) {
          const auto existing = named_slots->find(probe);
          if (existing != named_slots->end()) {
            slot = existing->second;
          }
        }
      }
      if (slot == 0) {
        if (maximum_slot >= options_.maximum_parameters) {
          return std::unexpected(BinderError(BindErrorCode::kParameterLimitExceeded,
                                             occurrence.span, "too many SQL variables"));
        }
        slot = maximum_slot + 1U;
        if (!named_slots.has_value()) {
          named_slots.emplace();
        }
        named_slots->emplace(std::string{token}, slot);
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
    return {};
  }

  [[nodiscard]] BindExpected<void> AssignParameters() {
    std::vector<ParameterOccurrence> occurrences;
    bool has_membership = false;
    for (std::size_t index = 0; index < tree_.expressions().size(); ++index) {
      has_membership = has_membership ||
                       std::holds_alternative<InExpression>(tree_.expressions()[index].payload);
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

    ParameterAssignmentState assignment;
    for (const ParameterOccurrence& occurrence : occurrences) {
      if (auto assigned = AssignParameter(occurrence, assignment); !assigned.has_value()) {
        return assigned;
      }
    }
    if (has_membership) {
      return NormalizeInSyntax();
    }
    return {};
  }

  [[nodiscard]] bool IsParserTimeConstant(ExpressionId id,
                                          ParserConstantMode mode = ParserConstantMode::Normal) {
    if (syntax_normalization_[id.value].constant != ParserConstant::None) {
      return true;
    }
    const Expression& expression = tree_.expression(id);
    const auto constant = [this, mode](ExpressionId child) {
      return IsParserTimeConstant(child, mode);
    };
    return std::visit(
        [&](const auto& payload) -> bool {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, LiteralExpression>) {
            if (payload.kind == LiteralKind::kTrue || payload.kind == LiteralKind::kFalse) {
              syntax_normalization_[id.value].constant = payload.kind == LiteralKind::kTrue
                                                             ? ParserConstant::TrueBoolean
                                                             : ParserConstant::FalseBoolean;
              return true;
            }
            if (payload.kind == LiteralKind::kString && SpanText(payload.token).starts_with('"')) {
              return false;
            }
            return payload.kind != LiteralKind::kCurrentDate &&
                   payload.kind != LiteralKind::kCurrentTime &&
                   payload.kind != LiteralKind::kCurrentTimestamp;
          } else if constexpr (std::is_same_v<Payload, VariableExpression>) {
            return true;
          } else if constexpr (std::is_same_v<Payload, IdentifierExpression>) {
            if (payload.name.parts.size() == 1U) {
              const auto name = SpanText(payload.name.parts.front());
              if (NamesEqual(name, "true") || NamesEqual(name, "false")) {
                syntax_normalization_[id.value].constant = NamesEqual(name, "true")
                                                               ? ParserConstant::TrueBoolean
                                                               : ParserConstant::FalseBoolean;
                return true;
              }
            }
            return false;
          } else if constexpr (std::is_same_v<Payload, UnaryExpression> ||
                               std::is_same_v<Payload, CollateExpression>) {
            return constant(payload.operand);
          } else if constexpr (std::is_same_v<Payload, ParenthesizedExpression>) {
            return constant(payload.inner);
          } else if constexpr (std::is_same_v<Payload, BinaryExpression>) {
            if (mode == ParserConstantMode::NoIs &&
                (payload.op == BinaryOperator::kIs || payload.op == BinaryOperator::kIsNot)) {
              const auto* right = std::get_if<LiteralExpression>(
                  &ParenthesesTransparentExpression(payload.right).payload);
              if (right != nullptr && right->kind == LiteralKind::kNull) {
                return constant(payload.left);
              }
              return false;
            }
            return constant(payload.left) && constant(payload.right);
          } else if constexpr (std::is_same_v<Payload, InExpression>) {
            return syntax_normalization_[id.value].membership == InSyntaxForm::EmptyDiscarded ||
                   (constant(payload.operand) && std::ranges::all_of(payload.values, constant));
          } else if constexpr (std::is_same_v<Payload, FunctionCallExpression>) {
            if (payload.filter.has_value()) {
              // Native EP_WinFunc prunes before argument boolean normalization.
              return false;
            }
            if (!std::ranges::all_of(payload.arguments, constant)) {
              return false;
            }
            auto parts = NameParts(payload.name);
            if (!parts.has_value() || parts->size() != 1U) {
              return false;
            }
            const auto name = parts->front();
            if (FunctionNameExists(name)) {
              const auto resolved =
                  environment_.functions().Resolve(name, payload.arguments.size());
              if (resolved.has_value()) {
                return (*resolved)->is_deterministic();
              }
            }
            return SpecialScalarAcceptsArgumentCount(SelectSpecialScalar(name), name,
                                                     payload.arguments.size());
          } else {
            return false;
          }
        },
        expression.payload);
  }

  [[nodiscard]] std::optional<SourceSpan> FirstConstructionCollation(
      std::span<const ExpressionId> children) const noexcept {
    for (const ExpressionId child : children) {
      const auto& collation = syntax_normalization_[child.value].explicit_collation;
      if (collation.has_value()) {
        return collation;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] BindExpected<void> NormalizeExpressionConstruction(ExpressionId id) {
    const std::size_t index = id.value;
    const auto& expression = tree_.expressions()[index];
    auto& normalized = syntax_normalization_[index];
    normalized.has_local_function =
        ExpressionHasLocalFunction(expression, [this](ExpressionId child) {
          return syntax_normalization_[child.value].has_local_function;
        });
    if (const auto* function = std::get_if<FunctionCallExpression>(&expression.payload);
        function != nullptr && function->arguments.size() > options_.maximum_function_arguments) {
      auto parts = NameParts(function->name);
      if (!parts.has_value()) {
        return std::unexpected(std::move(parts.error()));
      }
      return ValidateFunctionArgumentCount(function->arguments.size(), expression.span,
                                           parts->front());
    }
    if (const auto* membership = std::get_if<InExpression>(&expression.payload);
        membership != nullptr) {
      normalized.explicit_collation =
          syntax_normalization_[membership->operand.value].explicit_collation;
      if (!normalized.explicit_collation.has_value()) {
        normalized.explicit_collation = FirstConstructionCollation(membership->values);
      }
      if (membership->values.empty()) {
        normalized.membership = membership->has_local_function ? InSyntaxForm::EmptyPreserved
                                                               : InSyntaxForm::EmptyDiscarded;
        if (!membership->has_local_function) {
          normalized.constant =
              membership->negated ? ParserConstant::TrueBoolean : ParserConstant::FalseBoolean;
        }
      } else if (membership->values.size() == 1U &&
                 IsParserTimeConstant(membership->values.front())) {
        normalized.membership = InSyntaxForm::SingletonEquality;
      }
    } else if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
               literal != nullptr && literal->kind == LiteralKind::kInteger &&
               IntegerLiteralTruthHint(SpanText(literal->token)) == BoundTruthHint::kAlwaysFalse) {
      normalized.constant = ParserConstant::FalseInteger;
    } else if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload);
               unary != nullptr) {
      normalized.unary_positive = unary->op == UnaryOperator::kPositive;
      normalized.explicit_collation =
          syntax_normalization_[unary->operand.value].explicit_collation;
    } else if (const auto* binary = std::get_if<BinaryExpression>(&expression.payload);
               binary != nullptr) {
      normalized.explicit_collation =
          FirstConstructionCollation(std::array{binary->left, binary->right});
      const auto known_false = [this](ExpressionId child) {
        const auto constant = syntax_normalization_[child.value].constant;
        return constant == ParserConstant::FalseBoolean || constant == ParserConstant::FalseInteger;
      };
      if (binary->op == BinaryOperator::kAnd && !normalized.has_local_function &&
          (known_false(binary->left) || known_false(binary->right))) {
        normalized.constant = ParserConstant::FalseInteger;
      } else if ((binary->op == BinaryOperator::kIs || binary->op == BinaryOperator::kIsNot) &&
                 IsParserFoldedNullTestOperand(binary->left)) {
        const auto* right = std::get_if<LiteralExpression>(
            &ParenthesesTransparentExpression(binary->right).payload);
        if (right != nullptr && right->kind == LiteralKind::kNull) {
          normalized.constant = binary->op == BinaryOperator::kIs ? ParserConstant::FalseInteger
                                                                  : ParserConstant::TrueInteger;
        }
      }
    } else if (const auto* parenthesized =
                   std::get_if<ParenthesizedExpression>(&expression.payload);
               parenthesized != nullptr) {
      normalized.unary_positive = syntax_normalization_[parenthesized->inner.value].unary_positive;
      normalized.constant = syntax_normalization_[parenthesized->inner.value].constant;
      normalized.explicit_collation =
          syntax_normalization_[parenthesized->inner.value].explicit_collation;
    } else if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
               collate != nullptr) {
      normalized.explicit_collation = collate->collation;
    } else if (const auto* function = std::get_if<FunctionCallExpression>(&expression.payload);
               function != nullptr) {
      normalized.explicit_collation = FirstConstructionCollation(function->arguments);
    }
    if (normalized.constant != ParserConstant::None) {
      normalized.explicit_collation.reset();
    }
    const std::size_t height = ComputeExpressionSyntaxHeight(
        ExpressionId{index}, [this](ExpressionId child) -> std::size_t {
          return syntax_normalization_[child.value].construction_height;
        });
    if (auto admitted = ValidateResolutionHeight(height, expression.span); !admitted.has_value()) {
      return admitted;
    }
    normalized.construction_height = static_cast<std::uint16_t>(height);
    return {};
  }

  [[nodiscard]] BindExpected<void> NormalizeInSyntax() {
    syntax_normalization_.resize(tree_.expressions().size());
    for (std::size_t index = 0; index < tree_.expressions().size(); ++index) {
      if (auto normalized = NormalizeExpressionConstruction(ExpressionId{index});
          !normalized.has_value()) {
        return normalized;
      }
    }
    return {};
  }

  template <typename Visitor>
  void VisitSyntaxQueryCores(Visitor&& visitor) const {
    const auto visit_query = [&](const SelectStatement& query) {
      visitor(query.first);
      for (const CompoundTerm& term : query.compounds) {
        visitor(term.core);
      }
    };
    if (const auto* root = std::get_if<SelectStatement>(&tree_.statement()); root != nullptr) {
      visit_query(*root);
    }
    for (const SelectStatement& query : tree_.select_queries()) {
      visit_query(query);
    }
  }

  [[nodiscard]] bool HasMultirowValues() const {
    bool found = false;
    VisitSyntaxQueryCores([&](const QueryCore& core) {
      const auto* values = std::get_if<ValuesCore>(&core);
      found = found || (values != nullptr && values->rows.size() > 1U);
    });
    return found;
  }

  [[nodiscard]] BindExpected<void> PrepareConstructionValuesRow(
      const ValuesCore& core, std::size_t row,
      ExpressionPreparationPhase phase = ExpressionPreparationPhase::Ordinary) {
    if (core.rows[row].size() > options_.maximum_result_columns) {
      return std::unexpected(BinderError(BindErrorCode::kResultColumnLimitExceeded, core.span,
                                         "too many columns in result set"));
    }
    struct RestorePreparationPhase {
      ExpressionPreparationPhase& phase;
      ExpressionPreparationPhase previous;
      ~RestorePreparationPhase() { phase = previous; }
    };
    const RestorePreparationPhase restore{
        .phase = preparation_phase_,
        .previous = std::exchange(preparation_phase_, phase),
    };
    const BindScope scope{
        .source_columns = false,
        .result_aliases = false,
        .aggregates = AggregatePermission::kProhibited,
    };
    for (const ExpressionId expression : core.rows[row]) {
      auto& cached = syntax_normalization_[expression.value].construction_binding;
      if (!cached.has_value()) {
        auto bound = BindExpression(expression, scope);
        if (!bound.has_value()) {
          return std::unexpected(std::move(bound.error()));
        }
        cached = *bound;
      }
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> ConstructValuesRow(NativeValuesConstruction& construction,
                                                      std::size_t row) {
    const ValuesCore& core = *construction.syntax;
    if (row == 0U) {
      construction.chunks.push_back(BoundValuesChunk{
          .row_begin = 0,
          .row_count = 1,
          .schedule = BoundValuesEvaluationSchedule::kConsumerFiltered,
      });
      return {};
    }
    const auto constant_row = [&](std::size_t index, ParserConstantMode mode) {
      return std::ranges::all_of(core.rows[index], [&](ExpressionId expression) {
        return IsParserTimeConstant(expression, mode);
      });
    };
    bool coroutine = constant_row(row, ParserConstantMode::NoIs);
    if (coroutine && !construction.coroutine) {
      // Admitted pre-resolution constant syntax has no CAST/column affinity.
      coroutine = constant_row(construction.current_row, ParserConstantMode::Normal);
      if (coroutine) {
        if (auto prepared = PrepareConstructionValuesRow(core, construction.current_row);
            !prepared.has_value()) {
          return prepared;
        }
      }
    }
    if (!coroutine) {
      construction.coroutine = false;
      construction.current_row = row;
      construction.chunks.push_back(BoundValuesChunk{
          .row_begin = row,
          .row_count = 1,
          .schedule = BoundValuesEvaluationSchedule::kConsumerFiltered,
      });
      return {};
    }
    if (core.rows[row].size() != core.rows[construction.current_row].size()) {
      return std::unexpected(BinderError(BindErrorCode::kValuesColumnCount, core.span,
                                         "all VALUES must have the same number of terms"));
    }
    if (auto prepared =
            PrepareConstructionValuesRow(core, row, ExpressionPreparationPhase::IncomingValues);
        !prepared.has_value()) {
      return prepared;
    }
    BoundValuesChunk& chunk = construction.chunks.back();
    chunk.schedule = BoundValuesEvaluationSchedule::kProducerEager;
    ++chunk.row_count;
    construction.coroutine = true;
    return {};
  }

  [[nodiscard]] BindExpected<void> NormalizeValuesConstruction() {
    syntax_normalization_.resize(tree_.expressions().size());
    std::vector<NativeConstructionEvent> events;
    std::size_t event_count = tree_.expressions().size();
    bool overflow = event_count > events.max_size();
    VisitSyntaxQueryCores([&](const QueryCore& core) {
      const auto* values = std::get_if<ValuesCore>(&core);
      if (overflow || values == nullptr || values->rows.size() < 2U) {
        return;
      }
      if (values->rows.size() > events.max_size() - event_count ||
          values_constructions_.size() == values_constructions_.max_size()) {
        overflow = true;
        return;
      }
      event_count += values->rows.size();
      values_constructions_.push_back(NativeValuesConstruction{.syntax = values});
    });
    if (overflow) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, {},
                                         "native construction event count overflow"));
    }
    events.reserve(event_count);
    for (std::size_t index = 0; index < tree_.expressions().size(); ++index) {
      events.push_back(NativeConstructionEvent{
          .source_end = tree_.expressions()[index].span.end().value(),
          .expression = ExpressionId{index},
      });
    }
    for (std::size_t index = 0; index < values_constructions_.size(); ++index) {
      auto& construction = values_constructions_[index];
      construction.chunks.reserve(construction.syntax->rows.size());
      for (std::size_t row = 0; row < construction.syntax->rows.size(); ++row) {
        std::size_t end = 0;
        for (const ExpressionId expression : construction.syntax->rows[row]) {
          end = std::max(end, tree_.expression(expression).span.end().value());
        }
        events.push_back(NativeConstructionEvent{
            .source_end = end,
            .values = index,
            .row = row,
        });
      }
    }
    std::ranges::sort(events, [](const NativeConstructionEvent& left,
                                 const NativeConstructionEvent& right) {
      if (left.source_end != right.source_end) {
        return left.source_end < right.source_end;
      }
      if (left.expression.has_value() != right.expression.has_value()) {
        return left.expression.has_value();
      }
      if (left.expression.has_value()) {
        return left.expression->value < right.expression->value;
      }
      return left.values < right.values || (left.values == right.values && left.row < right.row);
    });
    ParameterAssignmentState parameters;
    for (const NativeConstructionEvent& event : events) {
      if (event.expression.has_value()) {
        const ExpressionId expression = *event.expression;
        if (const auto* variable =
                std::get_if<VariableExpression>(&tree_.expression(expression).payload);
            variable != nullptr) {
          if (auto assigned = AssignParameter(
                  ParameterOccurrence{.expression = expression, .span = variable->token},
                  parameters);
              !assigned.has_value()) {
            return assigned;
          }
        }
        if (auto normalized = NormalizeExpressionConstruction(expression);
            !normalized.has_value()) {
          return normalized;
        }
      } else if (auto row = ConstructValuesRow(values_constructions_[event.values], event.row);
                 !row.has_value()) {
        return row;
      }
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

  [[nodiscard]] BindExpected<BoundExpressionId> BindJoinedValue(BoundSourceColumnId id,
                                                                SourceSpan span) {
    const auto& value = impl_->source_columns[id.value()].joined_value;
    if (value.has_value()) {
      return AppendExpression(span, BoundAliasReferenceExpression{.target = *value},
                              Properties(*value));
    }
    return BindColumn(id, span);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> AppendJoinedCoalesce(
      std::vector<BoundExpressionId> arguments, SourceSpan span) {
    if (arguments.size() < 2U) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "joined coalesce requires multiple contributors"));
    }
    const ExpressionProperties properties = Properties(arguments.front());
    return AppendExpression(span, BoundCoalesceExpression{.arguments = std::move(arguments)},
                            properties);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> AppendJoinedCoalesce(BoundExpressionId left,
                                                                     BoundExpressionId right,
                                                                     SourceSpan span) {
    BoundExpressionId target = left;
    while (const auto* alias = std::get_if<BoundAliasReferenceExpression>(
               &impl_->expressions[target.value()].payload)) {
      target = alias->target;
    }
    std::vector<BoundExpressionId> arguments;
    if (const auto* coalesce =
            std::get_if<BoundCoalesceExpression>(&impl_->expressions[target.value()].payload);
        coalesce != nullptr) {
      arguments = coalesce->arguments;
    } else {
      arguments.push_back(left);
    }
    arguments.push_back(right);
    return AppendJoinedCoalesce(std::move(arguments), span);
  }

  [[nodiscard]] bool JoinedValueMatches(BoundSourceColumnId column, BoundExpressionId value) const {
    const BoundExpression& expression = impl_->expressions[value.value()];
    if (const auto* leaf = std::get_if<BoundColumnExpression>(&expression.payload);
        leaf != nullptr) {
      return JoinedColumnRepresentative(leaf->column) == JoinedColumnRepresentative(column);
    }
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      return JoinedValueMatches(column, alias->target);
    }
    if (const auto* coalesce = std::get_if<BoundCoalesceExpression>(&expression.payload);
        coalesce != nullptr) {
      return std::ranges::all_of(coalesce->arguments, [&](BoundExpressionId argument) {
        return JoinedValueMatches(column, argument);
      });
    }
    return false;
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

  [[nodiscard]] BindExpected<std::size_t> BindUsingColumn(std::size_t source_index,
                                                          std::string_view name, SourceSpan span,
                                                          bool right_core) {
    const SourceState& source = source_states_[source_index];
    const auto left = FindVisibleLeftColumn(name, source.source_column_begin);
    const auto right = FindSourceColumn(source, name);
    if (!left.has_value() || !right.has_value()) {
      return std::unexpected(BinderError(BindErrorCode::kJoinConditionScope, span,
                                         "cannot join using column " + std::string{name} +
                                             " - column not present in both tables"));
    }
    auto left_expression = right_core ? BindColumn(*left, span) : BindJoinedValue(*left, span);
    if (!left_expression.has_value()) {
      return std::unexpected(std::move(left_expression.error()));
    }
    std::size_t equality_height = 2;
    if (right_core) {
      std::vector<BoundExpressionId> contributors;
      for (std::size_t index = left->value() + 1U; index < source.source_column_begin; ++index) {
        const BoundSourceColumn& column = impl_->source_columns[index];
        if (!NamesEqual(column.name, name)) {
          continue;
        }
        if (!column.merged_into.has_value()) {
          return std::unexpected(
              BinderError(BindErrorCode::kJoinConditionScope, span,
                          "ambiguous reference to " + std::string{name} + " in USING()"));
        }
        if (contributors.empty()) {
          contributors.push_back(*left_expression);
        }
        auto argument = BindColumn(BoundSourceColumnId{static_cast<std::uint32_t>(index)}, span);
        if (!argument.has_value()) {
          return std::unexpected(std::move(argument.error()));
        }
        contributors.push_back(*argument);
      }
      if (!contributors.empty()) {
        left_expression = AppendJoinedCoalesce(std::move(contributors), span);
        if (!left_expression.has_value()) {
          return std::unexpected(std::move(left_expression.error()));
        }
        equality_height = 3;
      }
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
    const SourceJoinKind kind = impl_->table_sources[source_index].join;
    if (kind == SourceJoinKind::kFull) {
      auto current = BindJoinedValue(*left, span);
      if (!current.has_value()) {
        return std::unexpected(std::move(current.error()));
      }
      auto merged = AppendJoinedCoalesce(*current, *right_expression, span);
      if (!merged.has_value()) {
        return std::unexpected(std::move(merged.error()));
      }
      impl_->source_columns[left->value()].joined_value = *merged;
    } else if (kind == SourceJoinKind::kRight) {
      impl_->source_columns[left->value()].joined_value = *right_expression;
    }
    return equality_height;
  }

  template <typename ExpressionHeight>
  [[nodiscard]] std::size_t ComputeExpressionSyntaxHeight(
      ExpressionId id, const ExpressionHeight& expression_height) const {
    if (!syntax_normalization_.empty() &&
        syntax_normalization_[id.value].constant != ParserConstant::None) {
      return 1U;
    }
    return std::visit(
        [this, id, &expression_height](const auto& payload) -> std::size_t {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, LiteralExpression> ||
                        std::is_same_v<Payload, VariableExpression> ||
                        std::is_same_v<Payload, WildcardExpression>) {
            return 1;
          } else if constexpr (std::is_same_v<Payload, IdentifierExpression>) {
            return payload.name.parts.size();
          } else if constexpr (std::is_same_v<Payload, ParenthesizedExpression>) {
            return expression_height(payload.inner);
          } else if constexpr (std::is_same_v<Payload, CollateExpression>) {
            return syntax_normalization_.empty() ? expression_height(payload.operand) : 1U;
          } else if constexpr (std::is_same_v<Payload, InExpression>) {
            const auto form = syntax_normalization_.empty()
                                  ? InSyntaxForm::List
                                  : syntax_normalization_[id.value].membership;
            if (form == InSyntaxForm::EmptyDiscarded) {
              return 1U;
            }
            std::size_t height = expression_height(payload.operand);
            if (form == InSyntaxForm::EmptyPreserved) {
              return std::min(height + 1U, kMaximumExpressionConstructionDepth + 1U);
            }
            for (const auto value : payload.values) {
              height = std::max(height, expression_height(value) +
                                            (form == InSyntaxForm::SingletonEquality ? 1U : 0U));
            }
            return std::min(height + (payload.negated ? 2U : 1U),
                            kMaximumExpressionConstructionDepth + 1U);
          } else {
            std::size_t height = 0;
            if constexpr (std::is_same_v<Payload, UnaryExpression>) {
              height = expression_height(payload.operand);
              if (!syntax_normalization_.empty() &&
                  (payload.op == UnaryOperator::kPositive ||
                   payload.op == UnaryOperator::kNegative) &&
                  syntax_normalization_[payload.operand.value].unary_positive) {
                return height;
              }
            } else if constexpr (std::is_same_v<Payload, BinaryExpression>) {
              height = std::max(expression_height(payload.left), expression_height(payload.right));
            } else if constexpr (std::is_same_v<Payload, SubqueryExpression>) {
              height = SelectExpressionHeight(tree_.select_query(payload.query), expression_height);
            } else {
              static_assert(std::is_same_v<Payload, FunctionCallExpression>);
              for (const ExpressionId argument : payload.arguments) {
                height = std::max(height, expression_height(argument));
              }
              if (syntax_normalization_.empty()) {
                for (const OrderingTerm& term : payload.order_by) {
                  height = std::max(height, expression_height(term.expression));
                }
                if (payload.filter.has_value()) {
                  height = std::max(height, expression_height(*payload.filter));
                }
              }
            }
            return std::min(height + 1U, kMaximumExpressionConstructionDepth + 1U);
          }
        },
        tree_.expression(id).payload);
  }

  [[nodiscard]] std::size_t ExpressionSyntaxHeight(ExpressionId id) const {
    if (!syntax_normalization_.empty()) {
      return syntax_normalization_[id.value].construction_height;
    }
    return ComputeExpressionSyntaxHeight(
        id, [this](ExpressionId child) { return ExpressionSyntaxHeight(child); });
  }

  [[nodiscard]] BindExpected<void> BindJoinedColumns(const SelectCore& core) {
    if (core.from.size() < 2U) {
      return {};
    }
    std::size_t height = 0;
    const bool right_core = std::ranges::any_of(
        core.from, [](const TableSource& source) { return SourceJoinPreservesRight(source.join); });
    const auto admit = [&](std::size_t added, SourceSpan span) -> BindExpected<void> {
      height = height == 0U ? added : std::max(height, added) + 1U;
      return ValidateResolutionHeight(height, span);
    };
    if (core.where.has_value()) {
      if (auto admitted =
              admit(ExpressionSyntaxHeight(*core.where), tree_.expression(*core.where).span);
          !admitted.has_value()) {
        return admitted;
      }
    }
    for (std::size_t index = 1; index < core.from.size(); ++index) {
      const TableSource& source = core.from[index];
      if (source.natural && (source.on.has_value() || source.using_clause.has_value())) {
        return std::unexpected(BinderError(BindErrorCode::kJoinConditionScope, source.span,
                                           "a NATURAL join may not have an ON or USING clause"));
      }
      if (source.on.has_value()) {
        if (auto admitted = admit(ExpressionSyntaxHeight(*source.on), source.span);
            !admitted.has_value()) {
          return admitted;
        }
      }
      if (source.using_clause.has_value()) {
        for (const SourceSpan column : source.using_clause->columns) {
          auto name = Dequote(column);
          if (!name.has_value()) {
            return std::unexpected(std::move(name.error()));
          }
          auto joined = BindUsingColumn(index, *name, column, right_core);
          if (!joined.has_value()) {
            return std::unexpected(std::move(joined.error()));
          }
          if (auto admitted = admit(*joined, column); !admitted.has_value()) {
            return admitted;
          }
        }
      } else if (source.natural) {
        const SourceState& right = source_states_[index];
        const std::size_t end = right.source_column_begin + right.source_column_count;
        for (std::size_t column = right.source_column_begin; column < end; ++column) {
          const std::string_view name = impl_->source_columns[column].name;
          if (FindVisibleLeftColumn(name, right.source_column_begin).has_value()) {
            auto joined = BindUsingColumn(index, name, source.span, right_core);
            if (!joined.has_value()) {
              return std::unexpected(std::move(joined.error()));
            }
            if (auto admitted = admit(*joined, source.span); !admitted.has_value()) {
              return admitted;
            }
          }
        }
      }
    }
    active_condition_height_ = height;
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
        .table = table_id,
        .schema_table = false,
        .source_column_begin = column_begin,
        .source_column_count = table.columns.size(),
    });
    impl_->table_sources.push_back(BoundTableSource{
        .id = *source_id,
        .kind = BoundSourceKind::kCatalogTable,
        .table = table_id,
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

  [[nodiscard]] BindError SchemaLookupError(BindErrorCode code, SourceSpan span,
                                            std::string detail) const {
    schema_check_requested_ = true;
    return BinderError(code, span, std::move(detail));
  }

  [[nodiscard]] BindExpected<void> NoSuchTable(SourceSpan span) const {
    return std::unexpected(SchemaLookupError(BindErrorCode::kNoSuchTable, span,
                                             "no such table: " + std::string{SpanText(span)}));
  }

  [[nodiscard]] BindExpected<void> BindResults(const SelectCore& select) {
    for (const ResultColumn& result : select.result_columns) {
      const Expression& expression = tree_.expression(result.expression);
      if (const auto* wildcard = std::get_if<WildcardExpression>(&expression.payload);
          wildcard != nullptr) {
        BindExpected<void> expanded = ExpandWildcard(*wildcard, expression.span, select.from);
        if (!expanded.has_value()) {
          return std::unexpected(std::move(expanded.error()));
        }
        continue;
      }

      const std::size_t first_query = impl_->nested_queries.size();
      const bool requires_value = ActiveQuery().destination != BoundQueryDestination::kExists;
      BindExpected<BoundExpressionId> bound =
          BindExpression(result.expression, BindScope{
                                                .source_columns = true,
                                                .result_aliases = false,
                                                .aggregates = AggregatePermission::kAllowed,
                                                .requires_value = requires_value,
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
      impl_->result_definitions.push_back(ResultDefinition{
          .syntax = result.expression,
          .has_subqueries = impl_->nested_queries.size() != first_query,
          .value_checked = requires_value && !binding_only_,
      });
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
      BindExpected<BoundCollationId> collation = InternSyntaxCollation(collate->collation);
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
      if (!pending.finalized && pending.expression == expression) {
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
    if (const auto* membership = std::get_if<InExpression>(&left.payload); membership != nullptr) {
      const auto& other = std::get<InExpression>(right.payload);
      if (membership->negated != other.negated ||
          membership->values.size() != other.values.size() ||
          !SyntaxExpressionsEquivalent(membership->operand, other.operand)) {
        return false;
      }
      for (std::size_t index = 0; index < membership->values.size(); ++index) {
        if (!SyntaxExpressionsEquivalent(membership->values[index], other.values[index])) {
          return false;
        }
      }
      return true;
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
    if (const auto* subquery = std::get_if<BoundSubqueryExpression>(&left.payload);
        subquery != nullptr) {
      return subquery->query == std::get<BoundSubqueryExpression>(right.payload).query;
    }
    if (const auto* membership = std::get_if<BoundInExpression>(&left.payload);
        membership != nullptr) {
      const auto& other = std::get<BoundInExpression>(right.payload);
      return membership->negated == other.negated && membership->affinity == other.affinity &&
             membership->collation == other.collation &&
             membership->rhs_is_constant == other.rhs_is_constant &&
             BoundSemanticallyEquivalent(membership->operand, other.operand) &&
             BoundArgumentsEquivalent(membership->values, other.values);
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
    ActiveQuery().order_by.reserve(select.order_by.size());
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

      if (result_column.has_value()) {
        if (auto copied = BindDirectAggregateOrderingCopy(*result_column, term.span);
            !copied.has_value()) {
          return copied;
        }
      }
      BindExpected<BoundExpressionId> expression =
          result_column.has_value()
              ? BindOrderByResultReference(term.expression, *result_column)
              : BindExpression(term.expression,
                               BindScope{
                                   .source_columns = true,
                                   .result_aliases = true,
                                   .aggregates = active_core_is_aggregate_ ||
                                                         ActiveQuery().destination ==
                                                             BoundQueryDestination::kExists ||
                                                         !discarded_order_bindings_.empty()
                                                     ? AggregatePermission::kAllowed
                                                 : impl_->table_sources.empty()
                                                     ? AggregatePermission::kDiscardedLocalOrder
                                                     : AggregatePermission::kOuterOnly,
                                   .requires_value =
                                       ActiveQuery().destination != BoundQueryDestination::kExists,
                               });
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      if (!result_column.has_value()) {
        for (std::size_t index = impl_->result_columns.size(); index > 0U; --index) {
          const std::optional<ExpressionId>& syntax = impl_->result_definitions[index - 1U].syntax;
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
      if (ActiveQuery().destination != BoundQueryDestination::kExists) {
        BindExpected<void> required = RequireCollation(*collation, term.span);
        if (!required.has_value()) {
          return std::unexpected(std::move(required.error()));
        }
      }
      const SortOrder order =
          term.order == SortOrder::kDescending ? SortOrder::kDescending : SortOrder::kAscending;
      const BoundNullPlacement null_placement =
          term.null_order == NullOrder::kFirst  ? BoundNullPlacement::kFirst
          : term.null_order == NullOrder::kLast ? BoundNullPlacement::kLast
          : order == SortOrder::kDescending     ? BoundNullPlacement::kLast
                                                : BoundNullPlacement::kFirst;
      ActiveQuery().order_by.emplace_back(BoundOrderingTerm{
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

  void PopulateImplicitResultMetadata(
      BoundExpressionId expression, SourceSpan span, BoundResultColumn& result,
      std::optional<std::string_view> synthetic_name = std::nullopt) const {
    while (const auto* alias = std::get_if<BoundAliasReferenceExpression>(
               &impl_->expressions[expression.value()].payload)) {
      expression = alias->target;
    }
    result.declared_type = ExpressionDeclaredType(expression);
    const BoundExpression& bound = impl_->expressions[expression.value()];
    if (const auto* column = std::get_if<BoundColumnExpression>(&bound.payload);
        column != nullptr) {
      const BoundSourceColumn& source = impl_->source_columns[column->column.value()];
      result.name = std::string{synthetic_name.value_or(source.name)};
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
            result.name = std::string{synthetic_name.value_or(source.name)};
            return;
          }
        }
      }
      result.name = std::string{synthetic_name.value_or("rowid")};
      return;
    }
    result.name = std::string{synthetic_name.value_or(SpanText(span))};
  }

  [[nodiscard]] bool WildcardUsesJoinedScope(std::span<const TableSource> from, std::size_t owner,
                                             std::string_view name) const noexcept {
    bool right_boundary = false;
    bool using_name = false;
    for (std::size_t index = owner + 1U; index < from.size(); ++index) {
      right_boundary = right_boundary || SourceJoinPreservesRight(from[index].join);
      if (!using_name) {
        const auto column = FindSourceColumn(source_states_[index], name);
        using_name =
            column.has_value() && impl_->source_columns[column->value()].merged_into.has_value();
      }
      if (right_boundary && using_name) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] BindExpected<void> ExpandWildcard(const WildcardExpression& wildcard,
                                                  SourceSpan span,
                                                  std::span<const TableSource> from) {
    if (source_states_.empty()) {
      return std::unexpected(
          BinderError(BindErrorCode::kNoTablesSpecified, span, "no tables specified"));
    }
    if (from.size() != source_states_.size()) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "wildcard source metadata is inconsistent"));
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

    for (std::size_t owner_index = 0; owner_index < source_states_.size(); ++owner_index) {
      const SourceState& owner = source_states_[owner_index];
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
        const SourceState* value_owner = &owner;
        const BoundSourceColumn* value_column = &column;
        bool synthetic_value = false;
        if (!qualifier.has_value() && column.joined_value.has_value()) {
          const auto* value = std::get_if<BoundColumnExpression>(
              &impl_->expressions[column.joined_value->value()].payload);
          if (value == nullptr) {
            synthetic_value = true;
          } else {
            value_column = &impl_->source_columns[value->column.value()];
            const auto found =
                std::ranges::find(source_states_, value_column->source, &SourceState::id);
            if (found == source_states_.end()) {
              return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                                 "joined wildcard value has no source owner"));
            }
            value_owner = &*found;
          }
        }
        std::string_view source_name = value_owner->schema_table
                                           ? std::string_view{"sqlite_master"}
                                           : std::string_view{value_owner->name};
        const std::optional<std::string>& alias = value_owner->alias;
        if (alias.has_value()) {
          source_name = *alias;
        }
        BindExpected<std::optional<BoundExpressionId>> resolved =
            std::optional<BoundExpressionId>{};
        if (synthetic_value) {
          auto value = BindJoinedValue(column_id, span);
          if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
          }
          resolved = std::optional{*value};
        } else {
          const std::optional<SourceQualification> qualification =
              qualifier.has_value() && WildcardUsesJoinedScope(from, owner_index, column.name)
                  ? std::nullopt
                  : std::optional{SourceQualification{.schema = catalog_->schema_name(),
                                                      .name = source_name}};
          resolved = ResolveSourceName(value_column->name, span, qualification, source_states_,
                                       impl_->table_sources);
        }
        if (!resolved.has_value()) {
          return std::unexpected(std::move(resolved.error()));
        }
        if (!resolved->has_value()) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                             "expanded wildcard column did not resolve"));
        }
        const BoundExpressionId expression = **resolved;
        if (!JoinedValueMatches(column_id, expression)) {
          return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                             "expanded wildcard column has a different owner"));
        }
        BoundResultColumn result{
            .expression = expression,
            .affinity = Properties(expression).affinity,
        };
        PopulateImplicitResultMetadata(expression, span, result, column.name);
        impl_->result_columns.push_back(std::move(result));
        impl_->result_definitions.push_back(ResultDefinition{.value_checked = true});
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

  [[nodiscard]] bool ResolutionHeightAllowed(std::size_t height) const noexcept {
    return height <= kMaximumExpressionConstructionDepth - resolution_height_;
  }

  [[nodiscard]] BindError ResolutionHeightError(SourceSpan span) const {
    return BinderError(BindErrorCode::kExpressionDepthExceeded, span,
                       "Expression tree is too large (maximum depth " +
                           std::to_string(kMaximumExpressionConstructionDepth) + ")");
  }

  [[nodiscard]] BindExpected<void> ValidateResolutionHeight(std::size_t height,
                                                            SourceSpan span) const {
    if (!ResolutionHeightAllowed(height)) {
      return std::unexpected(ResolutionHeightError(span));
    }
    return {};
  }

  [[nodiscard]] static BindExpected<BoundExpressionId> PropagateExpressionFailure(
      BindError&& error) {
    return std::unexpected(std::move(error));
  }

  [[nodiscard]] static BindExpected<BoundExpressionId> RejectExpression(BindErrorCode code,
                                                                        const SourceSpan& span,
                                                                        std::string_view message) {
    return std::unexpected(BinderError(code, span, std::string{message}));
  }

  [[nodiscard]] BindExpected<BoundExpressionId> RejectResolutionHeight(
      const SourceSpan& span) const {
    return std::unexpected(ResolutionHeightError(span));
  }

  using ExpressionBindingMethod = BindExpected<BoundExpressionId> (StatementBinder::*)(ExpressionId,
                                                                                       BindScope);

  [[nodiscard]] BindExpected<BoundExpressionId> BindExpression(ExpressionId id, BindScope scope,
                                                               std::size_t root_height = 0) {
    if (!syntax_normalization_.empty() &&
        syntax_normalization_[id.value].construction_binding.has_value()) {
      const BoundExpressionId bound = *syntax_normalization_[id.value].construction_binding;
      if (bound.value() >= impl_->expressions.size()) {
        return RejectExpression(BindErrorCode::kInternalInvariant, tree_.expression(id).span,
                                "VALUES construction binding is no longer retained");
      }
      return bound;
    }
    const std::size_t height = binding_validated_template_ || expression_owner_ == active_query_
                                   ? 0U
                                   : (root_height == 0U ? ExpressionSyntaxHeight(id) : root_height);
    if (!ResolutionHeightAllowed(height)) {
      return RejectResolutionHeight(tree_.expression(id).span);
    }
    struct RestoreBindingState {
      StatementBinder& binder;
      std::optional<BoundQueryId> previous_owner;
      bool previous_value;
      std::size_t height;
      ~RestoreBindingState() {
        binder.resolution_height_ -= height;
        binder.expression_owner_ = previous_owner;
        binder.active_value_required_ = previous_value;
      }
    };
    const RestoreBindingState restore{
        .binder = *this,
        .previous_owner = std::exchange(expression_owner_, active_query_),
        .previous_value =
            std::exchange(active_value_required_, scope.requires_value && !binding_only_),
        .height = height,
    };
    resolution_height_ += height;
    const ExpressionBindingMethod bind = SelectExpressionBinding(id);
    return (this->*bind)(id, scope);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindNormalizedExpression(ExpressionId id,
                                                                         BindScope) {
    const Expression& expression = tree_.expression(id);
    if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
        literal != nullptr) {
      if (auto validated = ValidateLiteralToken(*literal); !validated.has_value()) {
        return std::unexpected(std::move(validated.error()));
      }
    }
    return AppendParserConstant(syntax_normalization_[id.value].constant, expression.span);
  }

  [[nodiscard]] ExpressionBindingMethod SelectExpressionBinding(ExpressionId id) const {
    const Expression& expression = tree_.expression(id);
    if (!syntax_normalization_.empty() &&
        syntax_normalization_[id.value].constant != ParserConstant::None) {
      return &StatementBinder::BindNormalizedExpression;
    }
    if (expression.payload.valueless_by_exception()) {
      return &StatementBinder::BindUnsupportedExpression;
    }
    return std::visit(
        [](const auto& payload) -> ExpressionBindingMethod {
          using Payload = std::decay_t<decltype(payload)>;
          return &StatementBinder::BindExpressionPayload<Payload>;
        },
        expression.payload);
  }

  template <typename Payload>
  [[nodiscard]] BindExpected<BoundExpressionId> BindExpressionPayload(ExpressionId id,
                                                                      BindScope scope) {
    const Expression& expression = tree_.expression(id);
    const auto& payload = std::get<Payload>(expression.payload);
    if constexpr (std::is_same_v<Payload, LiteralExpression>) {
      return BindLiteral(payload, expression.span, scope);
    } else if constexpr (std::is_same_v<Payload, VariableExpression>) {
      return BindVariable(id, payload, expression.span);
    } else if constexpr (std::is_same_v<Payload, IdentifierExpression>) {
      return BindIdentifier(payload, scope);
    } else if constexpr (std::is_same_v<Payload, UnaryExpression>) {
      return BindUnary(payload, expression.span, scope);
    } else if constexpr (std::is_same_v<Payload, BinaryExpression>) {
      return BindBinary(payload, expression.span, scope);
    } else if constexpr (std::is_same_v<Payload, FunctionCallExpression>) {
      return BindCall(id, payload, expression.span, scope);
    } else if constexpr (std::is_same_v<Payload, CollateExpression>) {
      return BindCollate(payload, expression.span, scope);
    } else if constexpr (std::is_same_v<Payload, ParenthesizedExpression>) {
      return BindExpression(payload.inner, scope);
    } else if constexpr (std::is_same_v<Payload, SubqueryExpression>) {
      return BindSubquery(payload, expression.span, scope);
    } else if constexpr (std::is_same_v<Payload, InExpression>) {
      return BindIn(payload, expression.span, scope, syntax_normalization_[id.value].membership);
    } else {
      return BindUnsupportedExpression(id, scope);
    }
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindUnsupportedExpression(ExpressionId id,
                                                                          BindScope) {
    return RejectExpression(BindErrorCode::kUnsupportedFeature, tree_.expression(id).span,
                            "wildcard expressions are not supported here");
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindLiteral(const LiteralExpression& literal,
                                                            const SourceSpan& span,
                                                            BindScope scope) {
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
      return AppendBooleanConstant(literal.kind == LiteralKind::kTrue, span);
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
    if (literal.kind == LiteralKind::kInteger) {
      properties.truth_hint = IntegerLiteralTruthHint(token);
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
                                                             const SourceSpan& span) {
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
    const SourceQualification qualification{
        .schema =
            parts->size() == 3U ? std::optional<std::string_view>{parts->front()} : std::nullopt,
        .name = (*parts)[parts->size() - 2U],
    };
    const NameScopeView current = CurrentNameScope(scope);
    auto resolved = ResolveNameInScope(parts->back(), identifier.name.span, qualification, current,
                                       scope.aggregates);
    if (!resolved.has_value()) {
      return std::unexpected(std::move(resolved.error()));
    }
    if (resolved->has_value()) {
      return **resolved;
    }
    auto outer = ResolveOuterSourceName(parts->back(), identifier.name.span, qualification,
                                        current.outer, scope.aggregates);
    if (!outer.has_value()) {
      return std::unexpected(std::move(outer.error()));
    }
    if (outer->has_value()) {
      return **outer;
    }
    return NoSuchColumn(identifier.name.span);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindExpressionTemplateCopy(
      ExpressionId syntax, const NameScopeView& definition_scope, BindScope copy_scope,
      SourceSpan span, bool hide_result_aliases) {
    const std::size_t binding_width = tree_.select_queries().size();
    const std::size_t previous_size = query_bindings_.size();
    if (binding_width > query_bindings_.max_size() - previous_size) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "query binding cache size overflow"));
    }
    const std::size_t previous_base = query_binding_base_;
    query_bindings_.resize(previous_size + binding_width);
    query_binding_base_ = previous_size;

    NameScopeView names = definition_scope;
    names.scope = copy_scope;
    names.scope.result_aliases = copy_scope.result_aliases && !hide_result_aliases;
    const NameScopeView* previous_names = std::exchange(definition_name_scope_, &names);
    const bool previous_template = std::exchange(binding_validated_template_, true);
    const std::optional<BoundQueryId> previous_owner =
        std::exchange(expression_owner_, active_query_);
    auto bound = BindExpression(syntax, copy_scope);
    expression_owner_ = previous_owner;
    binding_validated_template_ = previous_template;
    definition_name_scope_ = previous_names;
    query_binding_base_ = previous_base;
    query_bindings_.resize(previous_size);
    return bound;
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindResultDefinitionCopy(
      const ResultDefinition& definition, const NameScopeView& definition_scope,
      BindScope copy_scope, SourceSpan span) {
    if (!definition.syntax.has_value()) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "result definition copy has no syntax"));
    }
    return BindExpressionTemplateCopy(*definition.syntax, definition_scope, copy_scope, span, true);
  }

  [[nodiscard]] BindExpected<void> ValidateResultSyntax(ExpressionId syntax,
                                                        const NameScopeView& definition_scope,
                                                        SourceSpan span) {
    const BindScope validation_scope{
        .source_columns = definition_scope.scope.source_columns,
        .result_aliases = false,
        .aggregates = AggregatePermission::kAllowed,
        .requires_value = true,
    };
    const BindingArenaCheckpoint checkpoint = SaveBindingArena();
    const ResultDefinition candidate{
        .syntax = syntax,
        .has_subqueries = true,
    };
    auto bound = BindResultDefinitionCopy(candidate, definition_scope, validation_scope, span);
    RestoreBindingArena(checkpoint);
    if (!bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    return {};
  }

  [[nodiscard]] BindExpected<void> ValidateResultDefinition(std::size_t definition_index,
                                                            const NameScopeView& definition_scope,
                                                            SourceSpan span) {
    if (definition_index >= definition_scope.result_definitions.size()) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "result definition validation index is invalid"));
    }
    const ResultDefinition& definition = definition_scope.result_definitions[definition_index];
    if (definition.value_checked || !definition.syntax.has_value()) {
      return {};
    }
    const ExpressionId primary_syntax = definition.syntax.value();
    const std::size_t additional_count = definition.additional_syntax.size();
    if (auto validated = ValidateResultSyntax(primary_syntax, definition_scope, span);
        !validated.has_value()) {
      return validated;
    }
    for (std::size_t index = 0; index < additional_count; ++index) {
      const ExpressionId syntax =
          definition_scope.result_definitions[definition_index].additional_syntax[index];
      if (auto validated = ValidateResultSyntax(syntax, definition_scope, span);
          !validated.has_value()) {
        return validated;
      }
    }
    definition_scope.result_definitions[definition_index].value_checked = true;
    return {};
  }

  [[nodiscard]] BindExpected<void> ValidateResultDefinitionsInSyntaxOrder(
      const NameScopeView& definition_scope) {
    std::size_t additional_count = 0;
    for (const ResultDefinition& definition : definition_scope.result_definitions) {
      additional_count = std::max(additional_count, definition.additional_syntax.size());
      if (definition.value_checked || !definition.syntax.has_value()) {
        continue;
      }
      const ExpressionId syntax = *definition.syntax;
      if (auto validated =
              ValidateResultSyntax(syntax, definition_scope, tree_.expression(syntax).span);
          !validated.has_value()) {
        return validated;
      }
    }
    for (std::size_t row = 0; row < additional_count; ++row) {
      for (const ResultDefinition& definition : definition_scope.result_definitions) {
        if (definition.value_checked || row >= definition.additional_syntax.size()) {
          continue;
        }
        const ExpressionId syntax = definition.additional_syntax[row];
        if (auto validated =
                ValidateResultSyntax(syntax, definition_scope, tree_.expression(syntax).span);
            !validated.has_value()) {
          return validated;
        }
      }
    }
    for (ResultDefinition& definition : definition_scope.result_definitions) {
      if (definition.syntax.has_value()) {
        definition.value_checked = true;
      }
    }
    return {};
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindAliasReference(
      const AliasEntry& alias, const ResultDefinition& definition,
      const NameScopeView& definition_scope, SourceSpan span, AliasAggregatePermissions permissions,
      std::optional<BoundQueryId> context = std::nullopt) {
    const ExpressionProperties properties = Properties(alias.target);
    if (properties.references_aggregate) {
      bool definition_owns_aggregate = false;
      auto dependency = [&](const BoundQueryCapture& capture) {
        const auto* aggregate = std::get_if<BoundAggregateContextCapture>(&capture);
        definition_owns_aggregate =
            definition_owns_aggregate ||
            (aggregate != nullptr && aggregate->query == definition_scope.query);
        return true;
      };
      if (!VisitQueryDependencies(impl_->expressions, impl_->source_columns, alias.target,
                                  CaptureLookup(), BoundSourceDependency::kBinding, dependency)) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                           "aggregate alias has invalid dependencies"));
      }
      if (definition_owns_aggregate &&
          (permissions.definition == AggregatePermission::kProhibited ||
           (context.has_value() && permissions.definition == AggregatePermission::kGrouping))) {
        return std::unexpected(BinderError(BindErrorCode::kAggregateMisuse, span,
                                           "misuse of aggregate function through result alias"));
      }
      if (context.has_value() && permissions.consumer == AggregatePermission::kGrouping) {
        return std::unexpected(
            BinderError(BindErrorCode::kAggregateMisuse, span,
                        "aggregate functions are not allowed in the GROUP BY clause"));
      }
    }
    if (context.has_value()) {
      const auto* subquery =
          std::get_if<BoundSubqueryExpression>(&impl_->expressions[alias.target.value()].payload);
      if (subquery != nullptr) {
        const BoundQuery& query = Query(subquery->query);
        if (query.destination == BoundQueryDestination::kScalar &&
            query.output_columns.size() != 1U) {
          return std::unexpected(
              BinderError(BindErrorCode::kColumnCountMismatch, span, "row value misused"));
        }
      }
    }
    if (definition.has_subqueries) {
      if (!definition.syntax.has_value()) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                           "query-containing result definition has no syntax"));
      }
      const BindScope copy_scope{
          .source_columns = definition_scope.scope.source_columns,
          .result_aliases = false,
          .aggregates = permissions.definition,
          .requires_value = active_value_required_,
      };
      auto copied = BindResultDefinitionCopy(definition, definition_scope, copy_scope, span);
      if (!copied.has_value()) {
        return std::unexpected(std::move(copied.error()));
      }
      auto expression = AppendExpression(span,
                                         BoundAliasReferenceExpression{
                                             .target = *copied,
                                             .context = definition_scope.query,
                                         },
                                         Properties(*copied));
      if (!expression.has_value()) {
        return std::unexpected(std::move(expression.error()));
      }
      if (auto captured = RecordQueryCaptures(*expression); !captured.has_value()) {
        return std::unexpected(std::move(captured.error()));
      }
      return *expression;
    }
    if (active_value_required_ && !definition.value_checked) {
      if (auto validated = ValidateResultDefinition(alias.result_index, definition_scope, span);
          !validated.has_value()) {
        return std::unexpected(std::move(validated.error()));
      }
    }
    ExpressionProperties alias_properties = properties;
    if (context.has_value() && alias_properties.references_aggregate) {
      alias_properties.contains_aggregate = false;
    }
    auto expression = AppendExpression(span,
                                       BoundAliasReferenceExpression{
                                           .target = alias.target,
                                           .context = definition_scope.query,
                                       },
                                       alias_properties);
    if (!expression.has_value()) {
      return std::unexpected(std::move(expression.error()));
    }
    if (auto captured = RecordQueryCaptures(*expression); !captured.has_value()) {
      return std::unexpected(std::move(captured.error()));
    }
    return *expression;
  }

  [[nodiscard]] BindExpected<std::optional<BoundExpressionId>> ResolveSingleName(
      std::string_view name, SourceSpan span, BindScope scope) {
    const NameScopeView current = CurrentNameScope(scope);
    auto resolved = ResolveNameInScope(name, span, std::nullopt, current, scope.aggregates);
    if (!resolved.has_value() || resolved->has_value()) {
      return resolved;
    }
    if (scope.source_columns) {
      return ResolveOuterSourceName(name, span, std::nullopt, current.outer, scope.aggregates);
    }
    return std::optional<BoundExpressionId>{};
  }

  [[nodiscard]] BindExpected<std::optional<BoundExpressionId>> ResolveNameInScope(
      std::string_view name, SourceSpan span, std::optional<SourceQualification> qualification,
      const NameScopeView& scope, AggregatePermission consumer_aggregates) {
    const std::optional<BoundQueryId> context =
        scope.query == active_query_ ? std::nullopt : std::optional{scope.query};
    if (scope.scope.source_columns) {
      auto resolved =
          ResolveSourceName(name, span, qualification, scope.sources, scope.table_sources);
      if (!resolved.has_value()) {
        return std::unexpected(std::move(resolved.error()));
      }
      if (resolved->has_value()) {
        if (context.has_value()) {
          if (auto captured = RecordQueryCaptures(**resolved); !captured.has_value()) {
            return std::unexpected(std::move(captured.error()));
          }
        }
        return resolved;
      }
    }
    if (!qualification.has_value() && scope.scope.result_aliases) {
      for (const AliasEntry& alias : scope.aliases) {
        if (NamesEqual(alias.name, name)) {
          if (alias.result_index >= scope.result_definitions.size()) {
            return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                               "result alias definition is unavailable"));
          }
          auto expression =
              BindAliasReference(alias, scope.result_definitions[alias.result_index], scope, span,
                                 AliasAggregatePermissions{
                                     .definition = scope.scope.aggregates,
                                     .consumer = consumer_aggregates,
                                 },
                                 context);
          if (!expression.has_value()) {
            return std::unexpected(std::move(expression.error()));
          }
          return std::optional<BoundExpressionId>{*expression};
        }
      }
    }
    return std::optional<BoundExpressionId>{};
  }

  [[nodiscard]] BindExpected<std::optional<BoundExpressionId>> ResolveOuterSourceName(
      std::string_view name, SourceSpan span, std::optional<SourceQualification> qualification,
      const NameScopeView* outer, AggregatePermission consumer_aggregates) {
    for (const NameScopeView* scope = outer; scope != nullptr; scope = scope->outer) {
      if (!scope->scope.source_columns) {
        break;
      }
      auto resolved = ResolveNameInScope(name, span, qualification, *scope, consumer_aggregates);
      if (!resolved.has_value() || resolved->has_value()) {
        return resolved;
      }
    }
    return std::optional<BoundExpressionId>{};
  }

  [[nodiscard]] BindExpected<void> ValidateRequiredResultDefinitions() {
    if (binding_only_ || !BoundQueryRequiresResultValues(ActiveQuery(), impl_->expressions)) {
      return {};
    }
    const std::size_t core_begin = ActiveQuery().core_begin;
    const std::size_t core_count = ActiveQuery().core_count;
    for (std::size_t index = 0; index < core_count; ++index) {
      const std::size_t global_index = core_begin + index;
      CoreBindingMetadata& metadata = core_metadata_[global_index];
      BoundQueryCore& core = impl_->query_cores[global_index];
      const std::span<const BoundTableSource> table_sources = std::visit(
          [](const auto& candidate) -> std::span<const BoundTableSource> {
            using Core = std::decay_t<decltype(candidate)>;
            if constexpr (std::is_same_v<Core, BoundSelectCore>) {
              return candidate.table_sources;
            } else {
              return {};
            }
          },
          core);
      const NameScopeView names{
          .query = active_query_,
          .sources = metadata.source_states,
          .table_sources = table_sources,
          .aliases = {},
          .result_definitions = metadata.result_definitions,
          .outer = outer_scope_,
          .scope =
              BindScope{
                  .source_columns = true,
                  .result_aliases = false,
                  .aggregates = AggregatePermission::kAllowed,
                  .requires_value = true,
              },
          .aggregate_core = global_index,
          .aggregate_phase = AggregateOccurrencePhase::kResult,
          .aggregate_permission = AggregatePermission::kAllowed,
          .aggregate_was_established = std::holds_alternative<BoundSelectCore>(core) &&
                                       std::get<BoundSelectCore>(core).aggregate.has_value(),
      };
      if (auto validated = ValidateResultDefinitionsInSyntaxOrder(names); !validated.has_value()) {
        return validated;
      }
    }
    return {};
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
      std::string_view name, SourceSpan span, std::optional<SourceQualification> qualification,
      std::span<const SourceState> sources, std::span<const BoundTableSource> table_sources) {
    std::optional<BoundSourceColumnId> matched_column;
    std::optional<BoundExpressionId> matched_value;
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
      return SchemaLookupError(BindErrorCode::kAmbiguousColumn, span,
                               "ambiguous column name: " + resolved_name);
    };
    for (const SourceState& source : sources) {
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
          const auto owner = std::ranges::find(table_sources, source.id, &BoundTableSource::id);
          if (owner != table_sources.end()) {
            if (owner->join == SourceJoinKind::kRight) {
              matched_column = *column;
              matched_value.reset();
            } else if (owner->join == SourceJoinKind::kFull) {
              auto left = matched_value.has_value()
                              ? BindExpected<BoundExpressionId>{*matched_value}
                              : BindColumn(*matched_column, span);
              if (!left.has_value()) {
                return std::unexpected(std::move(left.error()));
              }
              auto right = BindColumn(*column, span);
              if (!right.has_value()) {
                return std::unexpected(std::move(right.error()));
              }
              auto merged = AppendJoinedCoalesce(*left, *right, span);
              if (!merged.has_value()) {
                return std::unexpected(std::move(merged.error()));
              }
              matched_value = *merged;
              matched_column = *column;
            }
          }
          continue;
        }
        matched_column = *column;
      }
    }
    if (matched_column.has_value()) {
      if (matched_value.has_value()) {
        return matched_value;
      }
      auto bound = qualification.has_value() ? BindColumn(*matched_column, span)
                                             : BindJoinedValue(*matched_column, span);
      if (!bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      return std::optional{*bound};
    }
    if (IsRowIdName(name)) {
      std::optional<BoundSourceId> matched_rowid;
      for (const SourceState& source : sources) {
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
        column.rowid_alias ? std::string_view{"BINARY"} : column.collation_name;
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
    return std::unexpected(SchemaLookupError(BindErrorCode::kNoSuchColumn, span,
                                             "no such column: " + std::string{SpanText(span)}));
  }

  [[nodiscard]] BindExpected<std::optional<BoundExpressionId>> BindUnaryLiteral(
      const UnaryExpression& unary, SourceSpan span) {
    if (unary.op == UnaryOperator::kNegative) {
      if (const LiteralExpression* literal = UnaryNumericLiteral(unary.operand);
          literal != nullptr && literal->kind == LiteralKind::kInteger) {
        BindExpected<void> validated = ValidateLiteralToken(*literal);
        if (!validated.has_value()) {
          return std::unexpected(std::move(validated.error()));
        }
        const std::string stripped = StripNumericUnderscores(SpanText(literal->token));
        if (IsSignedMinimumMagnitude(SpanText(literal->token))) {
          auto bound = AppendExpression(
              span,
              BoundLiteralExpression{
                  .value = SqlValue::Integer(std::numeric_limits<std::int64_t>::min())},
              {});
          if (!bound.has_value()) {
            return std::unexpected(std::move(bound.error()));
          }
          return std::optional<BoundExpressionId>{*bound};
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
    return std::optional<BoundExpressionId>{};
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindUnary(const UnaryExpression& unary,
                                                          const SourceSpan& span, BindScope scope) {
    auto literal = BindUnaryLiteral(unary, span);
    if (!literal.has_value()) {
      return PropagateExpressionFailure(std::move(literal.error()));
    }
    if (literal->has_value()) {
      return **literal;
    }
    BindExpected<BoundExpressionId> operand = BindExpression(unary.operand, scope);
    if (!operand.has_value()) {
      return PropagateExpressionFailure(std::move(operand.error()));
    }
    return AppendBoundUnary(ToBoundUnary(unary.op), *operand, span);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> AppendBoundUnary(BoundUnaryOperation operation,
                                                                 BoundExpressionId operand,
                                                                 SourceSpan span) {
    ExpressionProperties properties;
    const ExpressionProperties child = Properties(operand);
    properties.contains_aggregate = child.contains_aggregate;
    properties.references_aggregate = child.references_aggregate;
    if (operation == BoundUnaryOperation::kPositive) {
      properties = child;
      properties.affinity = TypeAffinity::kNone;
      properties.truth_hint = BoundTruthHint::kNone;
    } else if (child.explicit_collation) {
      properties.collation = child.collation;
      properties.explicit_collation = true;
    }
    return AppendExpression(span,
                            BoundUnaryExpression{
                                .operation = operation,
                                .operand = operand,
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
                                                           const SourceSpan& span,
                                                           BindScope scope) {
    if (IsPatternOperation(binary.op)) {
      return RejectExpression(BindErrorCode::kUnsupportedFeature, span,
                              "pattern operators are not supported");
    }
    BindExpected<BoundExpressionId> left = BindExpression(binary.left, scope);
    if (!left.has_value()) {
      return PropagateExpressionFailure(std::move(left.error()));
    }
    return BindBinaryRight(binary, span, scope, *left);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindBinaryRight(const BinaryExpression& binary,
                                                                const SourceSpan& span,
                                                                BindScope scope,
                                                                BoundExpressionId left) {
    BindExpected<BoundExpressionId> right = BindExpression(binary.right, scope);
    if (!right.has_value()) {
      return PropagateExpressionFailure(std::move(right.error()));
    }
    return FinishBinaryBinding(binary, span, left, *right);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> FinishBinaryBinding(const BinaryExpression& binary,
                                                                    SourceSpan span,
                                                                    BoundExpressionId left,
                                                                    BoundExpressionId right) {
    if (binary.op == BinaryOperator::kIs || binary.op == BinaryOperator::kIsNot) {
      if (const std::optional<bool> truth = TruthLiteral(right); truth.has_value()) {
        return AppendExpression(
            span,
            BoundTruthTestExpression{
                .operand = left,
                .expected = *truth ? BoundTruthValue::kTrue : BoundTruthValue::kFalse,
                .negated = binary.op == BinaryOperator::kIsNot,
            },
            PropagatedExplicitProperties(std::array{left, right}));
      }
      if (IsDirectNullLiteral(right)) {
        BindExpected<BoundCollationId> collation = InternCollation("BINARY", span);
        if (!collation.has_value()) {
          return std::unexpected(std::move(collation.error()));
        }
        ExpressionProperties properties = PropagatedExplicitProperties(std::array{left, right});
        if (IsParserFoldedNullTestOperand(binary.left)) {
          properties.truth_hint = binary.op == BinaryOperator::kIs ? BoundTruthHint::kAlwaysFalse
                                                                   : BoundTruthHint::kAlwaysTrue;
        }
        return AppendExpression(span,
                                BoundComparisonExpression{
                                    .comparison = ToSqlComparison(binary.op),
                                    .affinity = TypeAffinity::kNone,
                                    .collation = *collation,
                                    .left = left,
                                    .right = right,
                                },
                                properties);
      }
    }
    if (IsComparisonOperation(binary.op)) {
      return AppendBoundComparison(ToSqlComparison(binary.op), left, right, span);
    }
    return AppendBoundBinary(ToBoundBinary(binary.op), left, right, span);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> AppendBooleanConstant(bool value, SourceSpan span) {
    return AppendParserConstant(value ? ParserConstant::TrueBoolean : ParserConstant::FalseBoolean,
                                span);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> AppendParserConstant(ParserConstant constant,
                                                                     SourceSpan span) {
    if (constant == ParserConstant::None) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "parser constant annotation is absent"));
    }
    const bool value =
        constant == ParserConstant::TrueInteger || constant == ParserConstant::TrueBoolean;
    const ExpressionProperties properties{
        .collation = std::nullopt,
        .truth_hint = value ? BoundTruthHint::kAlwaysTrue : BoundTruthHint::kAlwaysFalse};
    return AppendExpression(
        span,
        BoundLiteralExpression{.value = SqlValue::Integer(value ? 1 : 0),
                               .boolean_keyword = constant == ParserConstant::TrueBoolean ||
                                                  constant == ParserConstant::FalseBoolean},
        properties);
  }

  [[nodiscard]] bool HasQueryMembershipRhs(const InExpression& membership) const noexcept {
    if (membership.values.size() != 1U) {
      return false;
    }
    const auto& rhs = ParenthesesTransparentExpression(membership.values.front());
    const auto* query = std::get_if<SubqueryExpression>(&rhs.payload);
    return query != nullptr && query->kind == SubqueryKind::kScalar;
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindIn(const InExpression& membership,
                                                       const SourceSpan& span, BindScope scope,
                                                       InSyntaxForm form) {
    if (form == InSyntaxForm::EmptyDiscarded) {
      return AppendBooleanConstant(membership.negated, span);
    }
    if (HasQueryMembershipRhs(membership)) {
      return RejectExpression(BindErrorCode::kUnsupportedFeature, span,
                              "IN query right-hand sides are not supported");
    }
    BindScope operand_scope = scope;
    if (form == InSyntaxForm::EmptyPreserved) {
      operand_scope.requires_value = false;
    }
    auto operand = BindExpression(membership.operand, operand_scope);
    if (!operand.has_value()) {
      return PropagateExpressionFailure(std::move(operand.error()));
    }
    return BindInValues(membership, span, scope, form, *operand);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindInValues(const InExpression& membership,
                                                             const SourceSpan& span,
                                                             BindScope scope, InSyntaxForm form,
                                                             BoundExpressionId operand) {
    std::vector<BoundExpressionId> values;
    if (form != InSyntaxForm::EmptyPreserved) {
      values.reserve(membership.values.size());
      for (const auto value : membership.values) {
        auto bound = BindExpression(value, scope);
        if (!bound.has_value()) {
          return PropagateExpressionFailure(std::move(bound.error()));
        }
        values.push_back(*bound);
      }
    }
    return FinishInBinding(membership, span, form, operand, std::move(values));
  }

  [[nodiscard]] BindExpected<BoundExpressionId> FinishInBinding(
      const InExpression& membership, SourceSpan span, InSyntaxForm form, BoundExpressionId operand,
      std::vector<BoundExpressionId> values) {
    if (form == InSyntaxForm::EmptyPreserved) {
      auto value = AppendBooleanConstant(membership.negated, span);
      if (!value.has_value()) {
        return std::unexpected(std::move(value.error()));
      }
      return AppendBoundBinary(
          membership.negated ? BoundBinaryOperation::kLogicalOr : BoundBinaryOperation::kLogicalAnd,
          *value, operand, span);
    }
    if (form == InSyntaxForm::SingletonEquality) {
      auto positive = AppendBoundUnary(BoundUnaryOperation::kPositive, values.front(),
                                       tree_.expression(membership.values.front()).span);
      if (!positive.has_value()) {
        return std::unexpected(std::move(positive.error()));
      }
      auto comparison = AppendBoundComparison(SqlComparison::kEqual, operand, *positive, span);
      if (!comparison.has_value() || !membership.negated) {
        return comparison;
      }
      return AppendBoundUnary(BoundUnaryOperation::kLogicalNot, *comparison, span);
    }
    auto collation = SelectFunctionCollation(std::array{operand}, span);
    if (!collation.has_value()) {
      return std::unexpected(std::move(collation.error()));
    }
    auto properties = PropagatedExplicitProperties(std::array{operand});
    PropagateExplicitProperties(properties, PropagatedExplicitProperties(values));
    const bool constant = std::ranges::all_of(
        values, [this](BoundExpressionId value) { return IsConstantExpression(value); });
    return AppendExpression(span,
                            BoundInExpression{
                                .operand = operand,
                                .values = std::move(values),
                                .affinity = Properties(operand).affinity,
                                .collation = *collation,
                                .rhs_is_constant = constant,
                                .negated = membership.negated,
                            },
                            properties);
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
    if (!syntax_normalization_.empty()) {
      const auto constant = syntax_normalization_[id.value].constant;
      if (constant == ParserConstant::FalseInteger || constant == ParserConstant::TrueInteger) {
        return true;
      }
    }
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
    scope.aggregates = AggregatePermission::kProhibited;
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
    if (scope.requires_value) {
      BindExpected<void> required = RequireCollation(*collation, term.span);
      if (!required.has_value()) {
        return std::unexpected(std::move(required.error()));
      }
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
      ExpressionId syntax, const AggregateFunction& function, const FunctionCallExpression& call,
      SourceSpan span, BindScope scope, bool has_wildcard) {
    const BindingArenaCheckpoint aggregate_checkpoint = SaveBindingArena();
    const std::size_t capture_prefix = ActiveQuery().captures.size();
    const bool discarded_local_order =
        !replaying_discarded_aggregate_ &&
        std::ranges::any_of(discarded_order_bindings_, [](const DiscardedOrderBinding& binding) {
          return !binding.rolls_back;
        });
    BindScope argument_scope = scope;
    argument_scope.requires_value =
        scope.requires_value || active_aggregate_phase_ != AggregateOccurrencePhase::kOrderBy;
    argument_scope.requires_scalar_width =
        !discarded_local_order && scope.aggregates != AggregatePermission::kDiscardedLocalOrder;
    argument_scope.aggregates = AggregatePermission::kProhibited;
    std::vector<BoundExpressionId> arguments;
    if (!has_wildcard) {
      BindExpected<std::vector<BoundExpressionId>> bound =
          BindArguments(call.arguments, argument_scope);
      if (!bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      arguments = std::move(*bound);
    }
    if (scope.aggregates == AggregatePermission::kProhibited) {
      return std::unexpected(
          BinderError(BindErrorCode::kAggregateMisuse, span,
                      "misuse of aggregate function " + std::string{function.name()} + "()"));
    }

    std::vector<BoundOrderingTerm> order_by;
    BindScope modifier_scope = argument_scope;
    if (discarded_local_order || scope.aggregates == AggregatePermission::kDiscardedLocalOrder) {
      modifier_scope.requires_value = false;
    }
    if (!arguments.empty()) {
      order_by.reserve(call.order_by.size());
      for (const OrderingTerm& term : call.order_by) {
        BindExpected<BoundOrderingTerm> bound = BindAggregateOrderingTerm(term, modifier_scope);
        if (!bound.has_value()) {
          return std::unexpected(std::move(bound.error()));
        }
        order_by.push_back(*bound);
      }
    }

    std::optional<BoundExpressionId> filter;
    if (call.filter.has_value()) {
      BindScope filter_scope = scope;
      filter_scope.aggregates = AggregatePermission::kProhibited;
      filter_scope.requires_value = modifier_scope.requires_value;
      filter_scope.requires_scalar_width = modifier_scope.requires_scalar_width;
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
    std::vector<BoundSourceId> source_dependencies;
    std::vector<BoundQueryContextCapture> alias_dependencies;
    std::vector<BoundAggregateContextCapture> aggregate_dependencies;
    auto dependency = [&](const BoundQueryCapture& capture) {
      if (const auto* source = std::get_if<BoundSourceCapture>(&capture); source != nullptr) {
        if (!std::ranges::contains(source_dependencies, source->identity.source)) {
          source_dependencies.push_back(source->identity.source);
        }
      } else if (const auto* context = std::get_if<BoundQueryContextCapture>(&capture);
                 context != nullptr) {
        if (!std::ranges::contains(alias_dependencies, context->query,
                                   &BoundQueryContextCapture::query)) {
          alias_dependencies.push_back(*context);
        }
      } else {
        const auto aggregate = std::get<BoundAggregateContextCapture>(capture);
        if (!std::ranges::contains(aggregate_dependencies, aggregate.expression,
                                   &BoundAggregateContextCapture::expression)) {
          aggregate_dependencies.push_back(aggregate);
        }
      }
      return true;
    };
    const auto inspect = [&](BoundExpressionId id) {
      return VisitQueryDependencies(impl_->expressions, impl_->source_columns, id, CaptureLookup(),
                                    BoundSourceDependency::kBinding, dependency);
    };
    bool valid_ownership = std::ranges::all_of(arguments, inspect);
    for (const BoundOrderingTerm& term : order_by) {
      valid_ownership = valid_ownership && inspect(term.expression);
    }
    if (filter.has_value()) {
      valid_ownership = valid_ownership && inspect(*filter);
    }
    if (!valid_ownership) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "aggregate has invalid source dependencies"));
    }

    const NameScopeView definition_scope = CurrentNameScope(scope);
    NameScopeView owner_scope = definition_scope;
    bool source_anchored = !source_dependencies.empty();
    for (const BoundAggregateContextCapture& aggregate : aggregate_dependencies) {
      const auto* call_expression = std::get_if<BoundAggregateCallExpression>(
          &impl_->expressions[aggregate.expression.value()].payload);
      const BoundAggregateInvocation* invocation =
          call_expression == nullptr
              ? nullptr
              : AggregateInvocationForExpression(aggregate.expression, *call_expression);
      if (invocation == nullptr) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                           "aggregate context has no invocation"));
      }
      source_anchored = source_anchored || invocation->source_anchored;
    }
    if (source_anchored) {
      const NameScopeView* selected = nullptr;
      for (const NameScopeView* candidate = &definition_scope; candidate != nullptr;
           candidate = candidate->outer) {
        const bool owns_source =
            std::ranges::any_of(source_dependencies, [&](BoundSourceId source) {
              return std::ranges::contains(candidate->sources, source, &SourceState::id);
            });
        const bool owns_aggregate = std::ranges::any_of(
            aggregate_dependencies, [&](const BoundAggregateContextCapture& aggregate) {
              const auto* call_expression = std::get_if<BoundAggregateCallExpression>(
                  &impl_->expressions[aggregate.expression.value()].payload);
              const BoundAggregateInvocation* invocation =
                  call_expression == nullptr
                      ? nullptr
                      : AggregateInvocationForExpression(aggregate.expression, *call_expression);
              return aggregate.query == candidate->query && invocation != nullptr &&
                     invocation->source_anchored;
            });
        if (owns_source || owns_aggregate) {
          selected = candidate;
          break;
        }
      }
      if (selected == nullptr) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                           "aggregate owner scope is unavailable"));
      }
      owner_scope = *selected;
    }
    for (const BoundAggregateContextCapture& aggregate : aggregate_dependencies) {
      if (aggregate.query == owner_scope.query ||
          IsQueryAncestor(owner_scope.query, aggregate.query)) {
        return std::unexpected(
            BinderError(BindErrorCode::kAggregateMisuse, span,
                        "misuse of aggregate function " + std::string{function.name()} + "()"));
      }
      if (!IsQueryAncestor(aggregate.query, owner_scope.query)) {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                           "aggregate context is not lexical"));
      }
    }
    if (scope.aggregates == AggregatePermission::kOuterOnly && owner_scope.query == active_query_) {
      return std::unexpected(
          BinderError(BindErrorCode::kAggregateMisuse, span,
                      "misuse of aggregate function " + std::string{function.name()} + "()"));
    }
    const auto rollback_binding =
        std::ranges::find(discarded_order_bindings_, true, &DiscardedOrderBinding::rolls_back);
    if (!replaying_discarded_aggregate_ && !discarded_order_bindings_.empty() &&
        rollback_binding == discarded_order_bindings_.end() &&
        !discarded_order_bindings_.front().rolls_back) {
      const BoundQueryId discarded_query = discarded_order_bindings_.front().query;
      bool retain = false;
      if (IsQueryAncestor(discarded_query, owner_scope.query)) {
        retain = false;
      } else if (IsQueryAncestor(owner_scope.query, discarded_query)) {
        if (owner_scope.aggregate_permission == AggregatePermission::kGrouping) {
          return std::unexpected(
              BinderError(BindErrorCode::kAggregateMisuse, span,
                          "aggregate functions are not allowed in the GROUP BY clause"));
        }
        retain = owner_scope.aggregate_permission == AggregatePermission::kAllowed &&
                 (owner_scope.aggregate_phase != AggregateOccurrencePhase::kOrderBy ||
                  owner_scope.aggregate_was_established);
      } else {
        return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                           "discarded aggregate owner is not lexical"));
      }
      RestoreBindingArena(aggregate_checkpoint);
      if (!retain) {
        return AppendExpression(span, BoundLiteralExpression{.value = SqlValue{}}, {});
      }
      BindScope replay_scope = scope;
      replay_scope.aggregates = AggregatePermission::kAllowed;
      replay_scope.requires_value = true;
      replay_scope.requires_scalar_width = true;
      const bool previous_replay = std::exchange(replaying_discarded_aggregate_, true);
      auto replayed =
          BindExpressionTemplateCopy(syntax, definition_scope, replay_scope, span, false);
      replaying_discarded_aggregate_ = previous_replay;
      return replayed;
    }
    if (scope.aggregates == AggregatePermission::kDiscardedLocalOrder &&
        owner_scope.query != active_query_) {
      if (owner_scope.aggregate_permission == AggregatePermission::kGrouping) {
        return std::unexpected(
            BinderError(BindErrorCode::kAggregateMisuse, span,
                        "aggregate functions are not allowed in the GROUP BY clause"));
      }
      const bool retain = owner_scope.aggregate_permission == AggregatePermission::kAllowed &&
                          (owner_scope.aggregate_phase != AggregateOccurrencePhase::kOrderBy ||
                           owner_scope.aggregate_was_established);
      RestoreBindingArena(aggregate_checkpoint);
      if (!retain) {
        return AppendExpression(span, BoundLiteralExpression{.value = SqlValue{}}, {});
      }
      BindScope replay_scope = scope;
      replay_scope.aggregates = AggregatePermission::kAllowed;
      replay_scope.requires_value = true;
      replay_scope.requires_scalar_width = true;
      return BindExpressionTemplateCopy(syntax, definition_scope, replay_scope, span, false);
    }
    const bool deferred_outer = rollback_binding != discarded_order_bindings_.end() &&
                                owner_scope.query != rollback_binding->query &&
                                IsQueryAncestor(owner_scope.query, rollback_binding->query);
    bool retain_deferred = false;
    if (deferred_outer) {
      const bool vanished_alias =
          std::ranges::any_of(alias_dependencies, [&](const BoundQueryContextCapture& context) {
            return context.query != rollback_binding->query &&
                   IsQueryAncestor(rollback_binding->query, context.query);
          });
      if (vanished_alias) {
        return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, span,
                                           "discarded outer aggregate aliases are not supported"));
      }
      if (!aggregate_dependencies.empty()) {
        return std::unexpected(BinderError(BindErrorCode::kUnsupportedFeature, span,
                                           "discarded nested outer aggregates are not supported"));
      }
      if (owner_scope.aggregate_permission == AggregatePermission::kGrouping) {
        return std::unexpected(
            BinderError(BindErrorCode::kAggregateMisuse, span,
                        "aggregate functions are not allowed in the GROUP BY clause"));
      }
      retain_deferred = owner_scope.aggregate_permission == AggregatePermission::kAllowed &&
                        (owner_scope.aggregate_phase != AggregateOccurrencePhase::kOrderBy ||
                         owner_scope.aggregate_was_established);
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
    if (scope.aggregates == AggregatePermission::kDiscardedLocalOrder &&
        owner_scope.query == active_query_) {
      RestoreBindingArena(aggregate_checkpoint);
      return AppendExpression(span, BoundLiteralExpression{.value = SqlValue{}}, {});
    }
    if (pending_aggregates_.size() >= std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(BinderError(BindErrorCode::kInternalInvariant, span,
                                         "pending aggregate invocation ID overflow"));
    }
    ExpressionProperties properties = PropagatedExplicitProperties(arguments);
    properties.affinity = TypeAffinity::kNone;
    properties.contains_aggregate = owner_scope.query == active_query_;
    properties.references_aggregate = true;
    const BoundAggregateInvocationId pending_id{
        static_cast<std::uint32_t>(pending_aggregates_.size())};
    BindExpected<BoundExpressionId> expression = AppendExpression(span,
                                                                  BoundAggregateCallExpression{
                                                                      .invocation = pending_id,
                                                                      .owner = owner_scope.query,
                                                                  },
                                                                  properties);
    if (!expression.has_value()) {
      return std::unexpected(std::move(expression.error()));
    }
    pending_aggregates_.push_back(PendingAggregateInvocation{
        .syntax = syntax,
        .expression = *expression,
        .invocation =
            BoundAggregateInvocation{
                .span = span,
                .function = *bound_function,
                .collation = *collation,
                .owner = owner_scope.query,
                .arguments = std::move(arguments),
                .distinct = call.distinct,
                .order_by = std::move(order_by),
                .filter = filter,
                .argument_reuses_single_order_key = argument_reuses_single_order_key,
                .source_anchored = source_anchored,
            },
        .owner_query = owner_scope.query,
        .owner_core = owner_scope.aggregate_core,
        .site_query = active_query_,
        .phase = owner_scope.aggregate_phase,
    });
    if (owner_scope.query != active_query_) {
      ActiveQuery().captures.resize(capture_prefix);
      if (!deferred_outer) {
        auto& captures = ActiveQuery().captures;
        if (auto merged = MergeAggregateContextCapture(captures,
                                                       BoundAggregateContextCapture{
                                                           .query = owner_scope.query,
                                                           .expression = *expression,
                                                       });
            !merged.has_value()) {
          return std::unexpected(std::move(merged.error()));
        }
      }
    }
    if (retain_deferred) {
      BindScope replay_scope = scope;
      replay_scope.aggregates = AggregatePermission::kAllowed;
      replay_scope.requires_value = true;
      replay_scope.requires_scalar_width = true;
      deferred_aggregate_replays_.push_back(DeferredAggregateReplay{
          .syntax = syntax,
          .definition_scope = definition_scope,
          .scope = replay_scope,
          .owner_query = owner_scope.query,
          .owner_core = owner_scope.aggregate_core,
          .owner_phase = owner_scope.aggregate_phase,
          .owner_permission = owner_scope.aggregate_permission,
          .owner_was_established = owner_scope.aggregate_was_established,
          .consumer_query = rollback_binding->query,
      });
    }
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
    if (preparation_phase_ == ExpressionPreparationPhase::IncomingValues) {
      return {};
    }
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

  [[nodiscard]] BindExpected<void> ValidateFunctionArgumentCount(std::size_t count, SourceSpan span,
                                                                 std::string_view name) const {
    if (count > options_.maximum_function_arguments) {
      return std::unexpected(BinderError(BindErrorCode::kFunctionArgumentLimitExceeded, span,
                                         "too many arguments on function " + std::string{name}));
    }
    return {};
  }

  [[nodiscard]] BindExpected<BoundExpressionId> BindCall(ExpressionId syntax,
                                                         const FunctionCallExpression& call,
                                                         const SourceSpan& span, BindScope scope) {
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
    const SpecialScalarKind special = SelectSpecialScalar(name);
    const bool has_wildcard = std::ranges::any_of(call.arguments, [this](ExpressionId id) {
      return std::holds_alternative<WildcardExpression>(tree_.expression(id).payload);
    });
    const std::size_t argument_count = has_wildcard ? 0U : call.arguments.size();
    if (auto admitted = ValidateFunctionArgumentCount(argument_count, span, name);
        !admitted.has_value()) {
      return std::unexpected(std::move(admitted.error()));
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

    if (special == SpecialScalarKind::Coalesce) {
      if (!SpecialScalarAcceptsArgumentCount(special, name, call.arguments.size())) {
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
    if (special == SpecialScalarKind::Conditional) {
      if (!SpecialScalarAcceptsArgumentCount(special, name, call.arguments.size())) {
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
    if (special == SpecialScalarKind::Likelihood &&
        preparation_phase_ == ExpressionPreparationPhase::IncomingValues) {
      if (!SpecialScalarAcceptsArgumentCount(special, name, argument_count)) {
        return WrongArity(call.name.span, name);
      }
      auto operand = BindExpression(call.arguments.front(), scope);
      if (!operand.has_value()) {
        return std::unexpected(std::move(operand.error()));
      }
      auto properties = PropagatedExplicitProperties(std::array{*operand});
      const auto& explicit_collation = syntax_normalization_[syntax.value].explicit_collation;
      if (explicit_collation.has_value()) {
        auto collation = InternSyntaxCollation(*explicit_collation);
        if (!collation.has_value()) {
          return std::unexpected(std::move(collation.error()));
        }
        properties.collation = *collation;
        properties.explicit_collation = true;
      }
      return AppendExpression(span, BoundAliasReferenceExpression{.target = *operand}, properties);
    }
    if (special == SpecialScalarKind::Likelihood && !NamesEqual(name, "likelihood")) {
      if (!SpecialScalarAcceptsArgumentCount(special, name, argument_count)) {
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
    if (special == SpecialScalarKind::Likelihood) {
      if (!SpecialScalarAcceptsArgumentCount(special, name, call.arguments.size())) {
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
      return BindAggregateCall(syntax, **aggregate, call, span, scope, has_wildcard);
    }
    if (FunctionNameExists(name) || AggregateFunctionNameExists(name) ||
        special != SpecialScalarKind::None) {
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
                                                            const SourceSpan& span,
                                                            BindScope scope) {
    BindExpected<BoundExpressionId> operand = BindExpression(collate.operand, scope);
    if (!operand.has_value()) {
      return PropagateExpressionFailure(std::move(operand.error()));
    }
    return FinishCollationBinding(collate, span, *operand);
  }

  [[nodiscard]] BindExpected<BoundCollationId> InternSyntaxCollation(SourceSpan span) {
    auto name = Dequote(span);
    if (!name.has_value()) {
      return std::unexpected(std::move(name.error()));
    }
    return InternCollation(*name, span);
  }

  [[nodiscard]] BindExpected<BoundExpressionId> FinishCollationBinding(
      const CollateExpression& collate, SourceSpan span, BoundExpressionId operand) {
    BindExpected<BoundCollationId> id = InternSyntaxCollation(collate.collation);
    if (!id.has_value()) {
      return std::unexpected(std::move(id.error()));
    }
    ExpressionProperties properties = Properties(operand);
    properties.collation = *id;
    properties.explicit_collation = true;
    properties.truth_hint = BoundTruthHint::kNone;
    return AppendExpression(span,
                            BoundCollateExpression{
                                .operand = operand,
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
                .references_aggregate = properties.references_aggregate,
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
        .references_aggregate = properties.references_aggregate,
    };
  }

  [[nodiscard]] ExpressionProperties PropagatedExplicitProperties(
      std::span<const BoundExpressionId> expressions) const noexcept {
    ExpressionProperties result;
    for (const BoundExpressionId expression : expressions) {
      PropagateExplicitProperties(result, Properties(expression));
    }
    return result;
  }

  static void PropagateExplicitProperties(ExpressionProperties& result,
                                          const ExpressionProperties& properties) noexcept {
    result.contains_aggregate = result.contains_aggregate || properties.contains_aggregate;
    result.references_aggregate = result.references_aggregate || properties.references_aggregate;
    if (!result.explicit_collation && properties.explicit_collation) {
      result.collation = properties.collation;
      result.explicit_collation = true;
    }
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
          existing.uses_collation == function.uses_collation() &&
          existing.column_argument_use == function.column_argument_use()) {
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
        .column_argument_use = function.column_argument_use(),
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
    if (binding_only_ || !active_value_required_) {
      return {};
    }
    const std::string& name = impl_->collations[id.value()].name;
    if (FindRegisteredCollation(name) == nullptr) {
      return std::unexpected(ValueBinderError(BindErrorCode::kNoSuchCollation, span,
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
  mutable bool schema_check_requested_ = false;
  BindEnvironment environment_;
  BindOptions options_;
  std::unique_ptr<BoundSelect::Impl> impl_;
  BoundQueryId active_query_{0};
  std::optional<BoundQueryId> expression_owner_;
  std::size_t resolution_height_ = 0;
  bool active_value_required_ = true;
  bool binding_only_ = false;
  bool binding_validated_template_ = false;
  ExpressionPreparationPhase preparation_phase_ = ExpressionPreparationPhase::Ordinary;
  std::size_t active_core_ = 0;
  const NameScopeView* outer_scope_ = nullptr;
  const NameScopeView* definition_name_scope_ = nullptr;
  std::size_t query_binding_base_ = 0;
  std::vector<std::optional<BoundQueryId>> query_bindings_;
  std::vector<SourceState> source_states_;
  std::vector<AliasEntry> aliases_;
  std::size_t active_source_begin_ = 0;
  std::size_t active_source_count_ = 0;
  std::size_t active_condition_height_ = 0;
  std::uint32_t next_source_id_ = 0;
  AggregateOccurrencePhase active_aggregate_phase_ = AggregateOccurrencePhase::kResult;
  bool active_core_is_aggregate_ = false;
  std::vector<BoundGroupingTerm> active_group_by_;
  std::optional<BoundExpressionId> active_having_;
  std::vector<PendingAggregateInvocation> pending_aggregates_;
  std::vector<DeferredAggregateReplay> deferred_aggregate_replays_;
  std::vector<DiscardedOrderBinding> discarded_order_bindings_;
  bool replaying_discarded_aggregate_ = false;
  std::vector<CoreBindingMetadata> core_metadata_;
  std::vector<std::optional<BoundParameterId>> parameter_bindings_;
  std::vector<SyntaxNormalization> syntax_normalization_;
  std::vector<NativeValuesConstruction> values_constructions_;
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

std::optional<std::int32_t> BoundExpressionIntegerLiteralValue(
    std::span<const BoundExpression> expressions, BoundExpressionId id) noexcept {
  bool negative = false;
  while (id.value() < expressions.size()) {
    const BoundExpression& expression = expressions[id.value()];
    if (const auto* literal = std::get_if<BoundLiteralExpression>(&expression.payload);
        literal != nullptr) {
      const std::optional<std::int64_t> value = literal->value.integer_value();
      if (literal->boolean_keyword || !value.has_value() || *value < 0 ||
          *value > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
      }
      const auto magnitude = static_cast<std::int32_t>(*value);
      return negative ? -magnitude : magnitude;
    }
    const auto* unary = std::get_if<BoundUnaryExpression>(&expression.payload);
    if (unary == nullptr ||
        (unary->operation != BoundUnaryOperation::kPositive &&
         unary->operation != BoundUnaryOperation::kNegative) ||
        unary->operand >= id) {
      return std::nullopt;
    }
    negative = negative != (unary->operation == BoundUnaryOperation::kNegative);
    id = unary->operand;
  }
  return std::nullopt;
}

bool BoundExpressionIsIntegerOne(std::span<const BoundExpression> expressions,
                                 BoundExpressionId id) noexcept {
  const auto value = BoundExpressionIntegerLiteralValue(expressions, id);
  return value.has_value() && *value == 1;
}

bool BoundExpressionContainsQuery(std::span<const BoundExpression> expressions,
                                  BoundExpressionId id,
                                  std::span<const BoundAggregateInvocation> invocations) {
  assert(id.value() < expressions.size());
  const auto contains = [&](BoundExpressionId child) {
    return BoundExpressionContainsQuery(expressions, child, invocations);
  };
  return std::visit(
      [&](const auto& payload) -> bool {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, BoundSubqueryExpression>) {
          return true;
        } else if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                             std::is_same_v<Payload, BoundTruthTestExpression> ||
                             std::is_same_v<Payload, BoundLikelihoodExpression> ||
                             std::is_same_v<Payload, BoundCollateExpression>) {
          return contains(payload.operand);
        } else if constexpr (std::is_same_v<Payload, BoundBinaryExpression> ||
                             std::is_same_v<Payload, BoundComparisonExpression>) {
          return contains(payload.left) || contains(payload.right);
        } else if constexpr (std::is_same_v<Payload, BoundAliasReferenceExpression>) {
          return contains(payload.target);
        } else if constexpr (std::is_same_v<Payload, BoundInExpression>) {
          return contains(payload.operand) || std::ranges::any_of(payload.values, contains);
        } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression> ||
                             std::is_same_v<Payload, BoundCoalesceExpression> ||
                             std::is_same_v<Payload, BoundConditionalExpression>) {
          return std::ranges::any_of(payload.arguments, contains);
        } else if constexpr (std::is_same_v<Payload, BoundAggregateCallExpression>) {
          assert(payload.invocation.value() < invocations.size());
          const BoundAggregateInvocation& invocation = invocations[payload.invocation.value()];
          return std::ranges::any_of(invocation.arguments, contains) ||
                 (invocation.filter.has_value() && contains(*invocation.filter)) ||
                 std::ranges::any_of(invocation.order_by, [&](const BoundOrderingTerm& term) {
                   return contains(term.expression);
                 });
        } else {
          return false;
        }
      },
      expressions[id.value()].payload);
}

bool BoundQueryHasSingleRowDemand(const BoundQuery& query,
                                  std::span<const BoundExpression> expressions) noexcept {
  return query.limit.has_value() && !query.limit->offset.has_value() &&
         BoundExpressionIsIntegerOne(expressions, query.limit->limit);
}

bool BoundQueryCanOmitSetComparisons(const BoundQuery& query,
                                     std::span<const BoundExpression> expressions) noexcept {
  return query.destination != BoundQueryDestination::kClient && query.order_by.empty() &&
         BoundQueryHasSingleRowDemand(query, expressions) &&
         std::ranges::all_of(query.compound_operators, [](CompoundOperator operation) {
           return operation == CompoundOperator::kUnion || operation == CompoundOperator::kUnionAll;
         });
}

bool BoundQueryRequiresResultValues(const BoundQuery& query,
                                    std::span<const BoundExpression> expressions) noexcept {
  const bool compares_rows = std::ranges::any_of(
      query.compound_operators,
      [](CompoundOperator operation) { return operation != CompoundOperator::kUnionAll; });
  return query.destination != BoundQueryDestination::kExists ||
         (compares_rows && !BoundQueryCanOmitSetComparisons(query, expressions));
}

std::span<const BoundResultColumn> BoundCoreResultColumns(const BoundQueryCore& core) noexcept {
  if (const auto* select = std::get_if<BoundSelectCore>(&core)) {
    return select->result_columns;
  }
  if (const auto* values = std::get_if<BoundValuesCore>(&core)) {
    return values->result_columns;
  }
  std::terminate();
}

std::optional<BoundCollationId> BoundCompoundResultCollation(
    std::span<const BoundQueryCore> cores, std::span<const BoundExpression> expressions,
    std::size_t column) noexcept {
  for (const BoundQueryCore& core : cores) {
    const auto results = BoundCoreResultColumns(core);
    assert(column < results.size());
    const BoundExpressionId expression = results[column].expression;
    assert(expression.value() < expressions.size());
    if (const auto collation = expressions[expression.value()].properties.collation;
        collation.has_value()) {
      return collation;
    }
  }
  return std::nullopt;
}

bool BoundQueryHasCompoundOrderWrapper(const BoundQuery& query) noexcept {
  return std::ranges::any_of(
             query.compound_operators,
             [](CompoundOperator operation) { return operation != CompoundOperator::kUnionAll; }) &&
         std::ranges::any_of(query.order_by,
                             [](const BoundOrderingTerm& term) { return term.explicit_collation; });
}

bool BoundCompoundRequiresMerge(CompoundOperator operation, bool ordered, bool limit_one) noexcept {
  return ordered || (operation != CompoundOperator::kUnionAll &&
                     (operation != CompoundOperator::kUnion || !limit_one));
}

std::optional<std::size_t> BoundCompoundSplit(std::span<const CompoundOperator> operations,
                                              bool merge) noexcept {
  if (operations.empty()) {
    return std::nullopt;
  }
  const CompoundOperator operation = operations.back();
  constexpr std::size_t kMinimumBalancedInputs = 4;
  std::size_t inputs = 1;
  if (merge &&
      (operation == CompoundOperator::kUnionAll || operation == CompoundOperator::kUnion)) {
    for (std::size_t index = operations.size(); index != 0U && operations[index - 1U] == operation;
         --index) {
      ++inputs;
    }
  }
  return inputs < kMinimumBalancedInputs ? operations.size()
                                         : operations.size() + 1U - (inputs + 1U) / 2U;
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
    case BoundExpressionKind::kSubquery:
      return "subquery";
    case BoundExpressionKind::In:
      return "in";
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
    case BindErrorCode::kExpressionDepthExceeded:
      return "expression_depth_exceeded";
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
    case BindErrorCode::kExpressionDepthExceeded:
    case BindErrorCode::kNoTablesSpecified:
    case BindErrorCode::kNoSuchFunction:
    case BindErrorCode::kWrongFunctionArity:
    case BindErrorCode::kFunctionArgumentLimitExceeded:
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
  if (impl_ == nullptr || impl_->root_query.core_count == 0U) {
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

const BoundQuery& BoundSelect::root_query() const noexcept { return impl_->root_query; }

const BoundQuery& BoundSelect::query(BoundQueryId id) const noexcept {
  if (impl_ == nullptr || id.value() >= query_count()) {
    std::terminate();
  }
  return id.value() == 0U ? impl_->root_query : impl_->nested_queries[id.value() - 1U];
}

std::size_t BoundSelect::query_count() const noexcept {
  return impl_ != nullptr ? impl_->nested_queries.size() + 1U : 0U;
}

std::span<const BoundQueryCore> BoundSelect::query_cores() const noexcept {
  return impl_ != nullptr ? std::span<const BoundQueryCore>{impl_->query_cores}
                          : std::span<const BoundQueryCore>{};
}

std::span<const CompoundOperator> BoundSelect::compound_operators() const noexcept {
  return impl_ != nullptr ? std::span<const CompoundOperator>{impl_->root_query.compound_operators}
                          : std::span<const CompoundOperator>{};
}

std::span<const BoundCollationId> BoundSelect::compound_collations() const noexcept {
  return impl_ != nullptr ? std::span<const BoundCollationId>{impl_->root_query.compound_collations}
                          : std::span<const BoundCollationId>{};
}

std::span<const BoundResultColumn> BoundSelect::result_columns() const noexcept {
  return impl_ != nullptr ? std::span<const BoundResultColumn>{impl_->root_query.output_columns}
                          : std::span<const BoundResultColumn>{};
}

std::optional<BoundExpressionId> BoundSelect::where_expression() const noexcept {
  if (impl_ == nullptr || impl_->root_query.core_count == 0U) {
    return std::nullopt;
  }
  const auto* core = std::get_if<BoundSelectCore>(&impl_->query_cores.front());
  return core != nullptr ? core->where : std::nullopt;
}

std::span<const BoundOrderingTerm> BoundSelect::order_by() const noexcept {
  return impl_ != nullptr ? std::span<const BoundOrderingTerm>{impl_->root_query.order_by}
                          : std::span<const BoundOrderingTerm>{};
}

const BoundLimit* BoundSelect::limit() const noexcept {
  return impl_ != nullptr && impl_->root_query.limit.has_value() ? &*impl_->root_query.limit
                                                                 : nullptr;
}

BindSelectResult BindSelectStatement(SyntaxTree tree, CatalogSnapshotPtr catalog,
                                     BindEnvironment environment, BindOptions options) {
  binder_detail::StatementBinder binder(std::move(tree), std::move(catalog), environment, options);
  BindSelectResult result = binder.RunSelect();
  if (!result.has_value()) {
    result.error().requires_schema_check = binder.requires_schema_check();
  }
  return result;
}

BoundInsert::BoundInsert(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

BoundInsert::BoundInsert(BoundInsert&&) noexcept = default;

BoundInsert& BoundInsert::operator=(BoundInsert&&) noexcept = default;

BoundInsert::~BoundInsert() = default;

bool BoundInsert::valid() const noexcept { return impl_ != nullptr; }

Utf8View BoundInsert::source() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).source() : Utf8View{};
}

const CatalogSnapshot* BoundInsert::catalog() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).catalog() : nullptr;
}

CatalogVersion BoundInsert::required_catalog_version() const noexcept {
  const CatalogSnapshot* snapshot = catalog();
  return snapshot != nullptr ? snapshot->version() : CatalogVersion{};
}

std::uint64_t BoundInsert::registration_generation() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).registration_generation() : 0;
}

const BoundMutationTarget& BoundInsert::target() const noexcept { return impl_->target; }

std::span<const BoundParameter> BoundInsert::parameters() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).parameters()
                          : std::span<const BoundParameter>{};
}

std::span<const BoundCollation> BoundInsert::collations() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).collations()
                          : std::span<const BoundCollation>{};
}

std::span<const BoundScalarFunction> BoundInsert::functions() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).functions()
                          : std::span<const BoundScalarFunction>{};
}

std::span<const BoundExpression> BoundInsert::expressions() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).expressions()
                          : std::span<const BoundExpression>{};
}

const BoundExpression& BoundInsert::expression(BoundExpressionId id) const noexcept {
  return MutationQueryOwner(*impl_).expression(id);
}

const BoundSelect* BoundInsert::query_owner() const noexcept {
  return impl_ != nullptr ? &MutationQueryOwner(*impl_) : nullptr;
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
  return impl_ != nullptr ? MutationQueryOwner(*impl_).source() : Utf8View{};
}

const CatalogSnapshot* BoundUpdate::catalog() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).catalog() : nullptr;
}

CatalogVersion BoundUpdate::required_catalog_version() const noexcept {
  const CatalogSnapshot* snapshot = catalog();
  return snapshot != nullptr ? snapshot->version() : CatalogVersion{};
}

std::uint64_t BoundUpdate::registration_generation() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).registration_generation() : 0;
}

const BoundMutationTarget& BoundUpdate::target() const noexcept { return impl_->target; }

std::span<const BoundSourceColumn> BoundUpdate::source_columns() const noexcept {
  return impl_ != nullptr
             ? MutationQueryOwner(*impl_).source_columns().first(impl_->target.columns.size())
             : std::span<const BoundSourceColumn>{};
}

std::span<const BoundParameter> BoundUpdate::parameters() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).parameters()
                          : std::span<const BoundParameter>{};
}

std::span<const BoundCollation> BoundUpdate::collations() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).collations()
                          : std::span<const BoundCollation>{};
}

std::span<const BoundScalarFunction> BoundUpdate::functions() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).functions()
                          : std::span<const BoundScalarFunction>{};
}

std::span<const BoundExpression> BoundUpdate::expressions() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).expressions()
                          : std::span<const BoundExpression>{};
}

const BoundExpression& BoundUpdate::expression(BoundExpressionId id) const noexcept {
  return MutationQueryOwner(*impl_).expression(id);
}

const BoundSelect* BoundUpdate::query_owner() const noexcept {
  return impl_ != nullptr ? &MutationQueryOwner(*impl_) : nullptr;
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
  return impl_ != nullptr ? MutationQueryOwner(*impl_).source() : Utf8View{};
}

const CatalogSnapshot* BoundDelete::catalog() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).catalog() : nullptr;
}

CatalogVersion BoundDelete::required_catalog_version() const noexcept {
  const CatalogSnapshot* snapshot = catalog();
  return snapshot != nullptr ? snapshot->version() : CatalogVersion{};
}

std::uint64_t BoundDelete::registration_generation() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).registration_generation() : 0;
}

const BoundMutationTarget& BoundDelete::target() const noexcept { return impl_->target; }

std::span<const BoundSourceColumn> BoundDelete::source_columns() const noexcept {
  return impl_ != nullptr
             ? MutationQueryOwner(*impl_).source_columns().first(impl_->target.columns.size())
             : std::span<const BoundSourceColumn>{};
}

std::span<const BoundParameter> BoundDelete::parameters() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).parameters()
                          : std::span<const BoundParameter>{};
}

std::span<const BoundCollation> BoundDelete::collations() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).collations()
                          : std::span<const BoundCollation>{};
}

std::span<const BoundScalarFunction> BoundDelete::functions() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).functions()
                          : std::span<const BoundScalarFunction>{};
}

std::span<const BoundExpression> BoundDelete::expressions() const noexcept {
  return impl_ != nullptr ? MutationQueryOwner(*impl_).expressions()
                          : std::span<const BoundExpression>{};
}

const BoundExpression& BoundDelete::expression(BoundExpressionId id) const noexcept {
  return MutationQueryOwner(*impl_).expression(id);
}

const BoundSelect* BoundDelete::query_owner() const noexcept {
  return impl_ != nullptr ? &MutationQueryOwner(*impl_) : nullptr;
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

const BoundSelect* BoundStatementQueryOwner(const BoundStatement& statement) noexcept {
  if (statement.valueless_by_exception()) {
    std::terminate();
  }
  if (const auto* select = std::get_if<BoundSelect>(&statement); select != nullptr) {
    return select;
  }
  if (const auto* insert = std::get_if<BoundInsert>(&statement); insert != nullptr) {
    return insert->query_owner();
  }
  if (const auto* update = std::get_if<BoundUpdate>(&statement); update != nullptr) {
    return update->query_owner();
  }
  if (const auto* deletion = std::get_if<BoundDelete>(&statement); deletion != nullptr) {
    return deletion->query_owner();
  }
  return nullptr;
}

BindStatementResult BindStatement(SyntaxTree tree, CatalogSnapshotPtr catalog,
                                  BindEnvironment environment, BindOptions options) {
  binder_detail::StatementBinder binder(std::move(tree), std::move(catalog), environment, options);
  BindStatementResult result = binder.RunStatement();
  if (!result.has_value()) {
    result.error().requires_schema_check = binder.requires_schema_check();
  }
  return result;
}

}  // namespace modern_sqlite
