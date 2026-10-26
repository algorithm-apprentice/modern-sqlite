#ifndef MODERN_SQLITE_SYNTAX_AST_HPP_
#define MODERN_SQLITE_SYNTAX_AST_HPP_

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {

struct ExpressionId {
  std::size_t value = 0;

  constexpr auto operator<=>(const ExpressionId&) const noexcept = default;
};

struct SelectQueryId {
  std::size_t value = 0;

  constexpr auto operator<=>(const SelectQueryId&) const noexcept = default;
};

inline constexpr std::size_t kMaximumExpressionConstructionDepth = 1000;

enum class SortOrder : std::uint8_t {
  kDefault,
  kAscending,
  kDescending,
};

enum class NullOrder : std::uint8_t {
  kDefault,
  kFirst,
  kLast,
};

struct OrderingTerm {
  SourceSpan span;
  ExpressionId expression;
  SortOrder order = SortOrder::kDefault;
  NullOrder null_order = NullOrder::kDefault;
};

struct QualifiedName {
  SourceSpan span;
  std::vector<SourceSpan> parts;
};

enum class LiteralKind : std::uint8_t {
  kNull,
  kInteger,
  kReal,
  kString,
  kBlob,
  kCurrentDate,
  kCurrentTime,
  kCurrentTimestamp,
  kTrue,
  kFalse,
};

enum class UnaryOperator : std::uint8_t {
  kPositive,
  kNegative,
  kBitwiseNot,
  kNot,
};

enum class BinaryOperator : std::uint8_t {
  kOr,
  kAnd,
  kLess,
  kLessOrEqual,
  kGreater,
  kGreaterOrEqual,
  kEqual,
  kNotEqual,
  kIs,
  kIsNot,
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,
  kRemainder,
  kLeftShift,
  kRightShift,
  kBitwiseAnd,
  kBitwiseOr,
  kConcatenate,
  kLike,
  kNotLike,
  kGlob,
  kNotGlob,
  kRegexp,
  kNotRegexp,
  kMatch,
  kNotMatch,
};

struct LiteralExpression {
  LiteralKind kind = LiteralKind::kNull;
  SourceSpan token;
};

struct VariableExpression {
  SourceSpan token;
};

struct IdentifierExpression {
  QualifiedName name;
};

struct WildcardExpression {
  SourceSpan asterisk;
  std::optional<QualifiedName> qualifier{};
};

struct UnaryExpression {
  UnaryOperator op = UnaryOperator::kPositive;
  SourceSpan operator_span;
  ExpressionId operand;
};

struct BinaryExpression {
  BinaryOperator op = BinaryOperator::kAdd;
  SourceSpan operator_span;
  ExpressionId left;
  ExpressionId right;
};

struct FunctionCallExpression {
  QualifiedName name;
  std::vector<ExpressionId> arguments{};
  bool distinct = false;
  std::vector<OrderingTerm> order_by{};
  std::optional<ExpressionId> filter{};
};

struct CollateExpression {
  ExpressionId operand;
  SourceSpan keyword;
  SourceSpan collation;
};

struct ParenthesizedExpression {
  ExpressionId inner;
};

enum class SubqueryKind : std::uint8_t {
  kScalar,
  kExists,
};

struct SubqueryExpression {
  SelectQueryId query;
  SubqueryKind kind = SubqueryKind::kScalar;
};

struct InExpression {
  ExpressionId operand;
  std::vector<ExpressionId> values{};
  SourceSpan operator_span;
  SourceSpan list_span;
  bool negated = false;
  bool has_local_function = false;
};

using ExpressionPayload =
    std::variant<LiteralExpression, VariableExpression, IdentifierExpression, WildcardExpression,
                 UnaryExpression, BinaryExpression, FunctionCallExpression, CollateExpression,
                 ParenthesizedExpression, SubqueryExpression, InExpression>;

struct Expression {
  SourceSpan span;
  ExpressionPayload payload;
};

[[nodiscard]] bool IsPatternOperation(BinaryOperator operation) noexcept;
template <typename ChildPresence>
[[nodiscard]] bool ExpressionHasLocalFunction(const Expression& expression,
                                              const ChildPresence& child_presence) {
  return std::visit(
      [&](const auto& payload) -> bool {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, FunctionCallExpression>) {
          return true;
        } else if constexpr (std::is_same_v<Payload, LiteralExpression>) {
          return payload.kind == LiteralKind::kCurrentDate ||
                 payload.kind == LiteralKind::kCurrentTime ||
                 payload.kind == LiteralKind::kCurrentTimestamp;
        } else if constexpr (std::is_same_v<Payload, UnaryExpression>) {
          return child_presence(payload.operand);
        } else if constexpr (std::is_same_v<Payload, CollateExpression>) {
          // Native COLLATE initializes flags instead of propagating its operand's flags.
          return false;
        } else if constexpr (std::is_same_v<Payload, BinaryExpression>) {
          return IsPatternOperation(payload.op) || child_presence(payload.left) ||
                 child_presence(payload.right);
        } else if constexpr (std::is_same_v<Payload, ParenthesizedExpression>) {
          return child_presence(payload.inner);
        } else if constexpr (std::is_same_v<Payload, InExpression>) {
          return payload.has_local_function;
        } else {
          return false;
        }
      },
      expression.payload);
}

// Requires validated expression references; does not enter child SELECTs.
[[nodiscard]] bool ExpressionHasLocalFunction(std::span<const Expression> expressions,
                                              ExpressionId id);

enum class SelectQuantifier : std::uint8_t {
  kDefault,
  kAll,
  kDistinct,
};

enum class ConflictAction : std::uint8_t {
  kDefault,
  kRollback,
  kAbort,
  kFail,
  kIgnore,
  kReplace,
};

enum class LimitSyntax : std::uint8_t {
  kLimitOnly,
  kOffsetKeyword,
  kComma,
};

struct ResultColumn {
  SourceSpan span;
  ExpressionId expression;
  std::optional<SourceSpan> alias{};
};

inline constexpr std::size_t kMaximumSelectSources = 64;

enum class SourceJoinKind : std::uint8_t {
  kFirst,
  kComma,
  kCross,
  kInner,
  kLeft,
  kRight,
  kFull,
};

[[nodiscard]] constexpr bool SourceJoinPreservesLeft(SourceJoinKind kind) noexcept {
  return kind == SourceJoinKind::kLeft || kind == SourceJoinKind::kFull;
}

[[nodiscard]] constexpr bool SourceJoinPreservesRight(SourceJoinKind kind) noexcept {
  return kind == SourceJoinKind::kRight || kind == SourceJoinKind::kFull;
}

struct JoinUsingClause {
  SourceSpan span;
  std::vector<SourceSpan> columns;
};

struct TableSource {
  SourceSpan span;
  QualifiedName name;
  std::optional<SourceSpan> alias{};
  SourceJoinKind join = SourceJoinKind::kFirst;
  std::optional<SourceSpan> join_span{};
  std::optional<ExpressionId> on{};
  bool natural = false;
  std::optional<JoinUsingClause> using_clause{};
};

struct LimitClause {
  SourceSpan span;
  ExpressionId limit;
  std::optional<ExpressionId> offset{};
  LimitSyntax syntax = LimitSyntax::kLimitOnly;
};

template <typename ExpressionHeight>
[[nodiscard]] std::size_t LimitExpressionHeight(const LimitClause& limit,
                                                const ExpressionHeight& expression_height) {
  std::size_t height = expression_height(limit.limit);
  if (limit.offset.has_value()) {
    height = std::max(height, expression_height(*limit.offset));
  }
  return std::min(height, kMaximumExpressionConstructionDepth) + 1U;
}

enum class CompoundOperator : std::uint8_t {
  kUnion,
  kUnionAll,
  kIntersect,
  kExcept,
};

struct SelectCore {
  SourceSpan span;
  SelectQuantifier quantifier = SelectQuantifier::kDefault;
  std::vector<ResultColumn> result_columns;
  std::vector<TableSource> from{};
  std::optional<ExpressionId> where{};
  std::vector<ExpressionId> group_by{};
  std::optional<ExpressionId> having{};
};

struct ValuesCore {
  SourceSpan span;
  std::vector<std::vector<ExpressionId>> rows;
};

using QueryCore = std::variant<SelectCore, ValuesCore>;

struct CompoundTerm {
  SourceSpan span;
  CompoundOperator operation = CompoundOperator::kUnion;
  QueryCore core;
};

struct SelectStatement {
  SourceSpan span;
  QueryCore first;
  std::vector<CompoundTerm> compounds{};
  std::vector<OrderingTerm> order_by{};
  std::optional<LimitClause> limit{};
};

template <typename ExpressionHeight>
[[nodiscard]] std::size_t SelectExpressionHeight(const SelectStatement& query,
                                                 const ExpressionHeight& expression_height) {
  std::size_t height = 0;
  const auto include = [&height, &expression_height](ExpressionId expression) {
    height = std::max(height, expression_height(expression));
  };
  const auto include_core = [&include](const QueryCore& core) {
    std::visit(
        [&include](const auto& value) {
          using Core = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Core, SelectCore>) {
            for (const ResultColumn& result : value.result_columns) {
              include(result.expression);
            }
            if (value.where.has_value()) {
              include(*value.where);
            }
            for (const ExpressionId group : value.group_by) {
              include(group);
            }
            if (value.having.has_value()) {
              include(*value.having);
            }
          } else {
            for (const auto& row : value.rows) {
              for (const ExpressionId expression : row) {
                include(expression);
              }
            }
          }
        },
        core);
  };
  include_core(query.first);
  for (const CompoundTerm& compound : query.compounds) {
    include_core(compound.core);
  }
  for (const OrderingTerm& order : query.order_by) {
    include(order.expression);
  }
  if (query.limit.has_value()) {
    height = std::max(height, LimitExpressionHeight(*query.limit, expression_height));
  }
  return height;
}

struct IndexedTerm {
  SourceSpan span;
  ExpressionId expression;
  std::optional<SourceSpan> collation{};
  SortOrder order = SortOrder::kDefault;
};

struct PrimaryKeyColumnConstraint {
  SortOrder order = SortOrder::kDefault;
  ConflictAction conflict = ConflictAction::kDefault;
  bool autoincrement = false;
};

struct NullColumnConstraint {
  ConflictAction conflict = ConflictAction::kDefault;
};

struct NotNullColumnConstraint {
  ConflictAction conflict = ConflictAction::kDefault;
};

struct UniqueColumnConstraint {
  ConflictAction conflict = ConflictAction::kDefault;
};

struct CheckColumnConstraint {
  ExpressionId expression;
};

struct DefaultColumnConstraint {
  ExpressionId expression;
};

struct CollateColumnConstraint {
  SourceSpan collation;
};

using ColumnConstraintPayload =
    std::variant<PrimaryKeyColumnConstraint, NullColumnConstraint, NotNullColumnConstraint,
                 UniqueColumnConstraint, CheckColumnConstraint, DefaultColumnConstraint,
                 CollateColumnConstraint>;

struct ColumnConstraint {
  SourceSpan span;
  std::optional<SourceSpan> name{};
  ColumnConstraintPayload payload;
};

struct ColumnDefinition {
  SourceSpan span;
  SourceSpan name;
  std::optional<SourceSpan> type_name{};
  std::vector<ColumnConstraint> constraints{};
};

struct PrimaryKeyTableConstraint {
  std::vector<IndexedTerm> terms;
  ConflictAction conflict = ConflictAction::kDefault;
  bool autoincrement = false;
};

struct UniqueTableConstraint {
  std::vector<IndexedTerm> terms;
  ConflictAction conflict = ConflictAction::kDefault;
};

struct CheckTableConstraint {
  ExpressionId expression;
};

using TableConstraintPayload =
    std::variant<PrimaryKeyTableConstraint, UniqueTableConstraint, CheckTableConstraint>;

struct TableConstraint {
  SourceSpan span;
  std::optional<SourceSpan> name{};
  TableConstraintPayload payload;
};

struct CreateTableStatement {
  SourceSpan span;
  bool temporary = false;
  bool if_not_exists = false;
  QualifiedName name;
  std::vector<ColumnDefinition> columns;
  std::vector<TableConstraint> constraints{};
  bool without_rowid = false;
  bool strict = false;
};

struct CreateIndexStatement {
  SourceSpan span;
  bool unique = false;
  bool if_not_exists = false;
  QualifiedName name;
  QualifiedName table;
  std::vector<IndexedTerm> terms;
  std::optional<ExpressionId> where{};
};

struct AnalyzeStatement {
  SourceSpan span;
  std::optional<QualifiedName> target{};
};

struct InsertValuesSource {
  SourceSpan span;
  std::vector<ExpressionId> values;
};

struct InsertDefaultValuesSource {
  SourceSpan span;
};

using InsertSource = std::variant<InsertValuesSource, InsertDefaultValuesSource>;

struct InsertStatement {
  SourceSpan span;
  QualifiedName table;
  std::vector<SourceSpan> columns;
  InsertSource source;
};

struct UpdateAssignment {
  SourceSpan span;
  SourceSpan column;
  ExpressionId expression;
};

struct UpdateStatement {
  SourceSpan span;
  QualifiedName table;
  std::vector<UpdateAssignment> assignments;
  std::optional<ExpressionId> where{};
};

struct DeleteStatement {
  SourceSpan span;
  QualifiedName table;
  std::optional<ExpressionId> where{};
};

enum class BeginTransactionMode : std::uint8_t {
  kDeferred,
  kImmediate,
};

enum class CommitTransactionSyntax : std::uint8_t {
  kCommit,
  kEnd,
};

struct BeginTransactionStatement {
  SourceSpan span;
  BeginTransactionMode mode = BeginTransactionMode::kDeferred;
  bool transaction_keyword = false;
};

struct CommitTransactionStatement {
  SourceSpan span;
  CommitTransactionSyntax syntax = CommitTransactionSyntax::kCommit;
  bool transaction_keyword = false;
};

struct RollbackTransactionStatement {
  SourceSpan span;
  bool transaction_keyword = false;
};

struct SavepointStatement {
  SourceSpan span;
  SourceSpan name;
};

struct ReleaseSavepointStatement {
  SourceSpan span;
  SourceSpan name;
  bool savepoint_keyword = false;
};

struct RollbackToSavepointStatement {
  SourceSpan span;
  SourceSpan name;
  bool transaction_keyword = false;
  bool savepoint_keyword = false;
};

using Statement =
    std::variant<SelectStatement, CreateTableStatement, CreateIndexStatement, AnalyzeStatement,
                 InsertStatement, UpdateStatement, DeleteStatement, BeginTransactionStatement,
                 CommitTransactionStatement, RollbackTransactionStatement, SavepointStatement,
                 ReleaseSavepointStatement, RollbackToSavepointStatement>;

class SyntaxTree final {
 public:
  [[nodiscard]] static Result<SyntaxTree> Create(std::string source,
                                                 std::vector<Expression> expressions,
                                                 Statement statement,
                                                 std::vector<SelectStatement> select_queries = {});

  SyntaxTree(const SyntaxTree&) = delete;
  SyntaxTree& operator=(const SyntaxTree&) = delete;
  SyntaxTree(SyntaxTree&&) noexcept = default;
  SyntaxTree& operator=(SyntaxTree&&) noexcept = default;
  ~SyntaxTree() = default;

  [[nodiscard]] Utf8View source() const noexcept { return Utf8View{source_}; }
  [[nodiscard]] const Statement& statement() const noexcept { return statement_; }
  [[nodiscard]] std::span<const Expression> expressions() const noexcept {
    return std::span<const Expression>{expressions_};
  }
  [[nodiscard]] const Expression& expression(ExpressionId id) const noexcept;
  [[nodiscard]] std::span<const SelectStatement> select_queries() const noexcept {
    return select_queries_;
  }
  [[nodiscard]] const SelectStatement& select_query(SelectQueryId id) const noexcept;

 private:
  SyntaxTree(std::string source, std::vector<Expression> expressions, Statement statement,
             std::vector<SelectStatement> select_queries) noexcept
      : source_(std::move(source)),
        expressions_(std::move(expressions)),
        statement_(std::move(statement)),
        select_queries_(std::move(select_queries)) {}

  std::string source_;
  std::vector<Expression> expressions_;
  Statement statement_;
  std::vector<SelectStatement> select_queries_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_SYNTAX_AST_HPP_
