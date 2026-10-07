#ifndef MODERN_SQLITE_SYNTAX_AST_HPP_
#define MODERN_SQLITE_SYNTAX_AST_HPP_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
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
};

struct CollateExpression {
  ExpressionId operand;
  SourceSpan keyword;
  SourceSpan collation;
};

struct ParenthesizedExpression {
  ExpressionId inner;
};

using ExpressionPayload =
    std::variant<LiteralExpression, VariableExpression, IdentifierExpression, WildcardExpression,
                 UnaryExpression, BinaryExpression, FunctionCallExpression, CollateExpression,
                 ParenthesizedExpression>;

struct Expression {
  SourceSpan span;
  ExpressionPayload payload;
};

enum class SelectQuantifier : std::uint8_t {
  kDefault,
  kAll,
  kDistinct,
};

enum class SortOrder : std::uint8_t {
  kDefault,
  kAscending,
  kDescending,
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

struct TableSource {
  SourceSpan span;
  QualifiedName name;
  std::optional<SourceSpan> alias{};
};

struct LimitClause {
  SourceSpan span;
  ExpressionId limit;
  std::optional<ExpressionId> offset{};
  LimitSyntax syntax = LimitSyntax::kLimitOnly;
};

struct SelectStatement {
  SourceSpan span;
  SelectQuantifier quantifier = SelectQuantifier::kDefault;
  std::vector<ResultColumn> result_columns;
  std::optional<TableSource> from{};
  std::optional<ExpressionId> where{};
  std::optional<LimitClause> limit{};
};

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
    std::variant<SelectStatement, CreateTableStatement, CreateIndexStatement, InsertStatement,
                 UpdateStatement, DeleteStatement, BeginTransactionStatement,
                 CommitTransactionStatement, RollbackTransactionStatement, SavepointStatement,
                 ReleaseSavepointStatement, RollbackToSavepointStatement>;

class SyntaxTree final {
 public:
  [[nodiscard]] static Result<SyntaxTree> Create(std::string source,
                                                 std::vector<Expression> expressions,
                                                 Statement statement);

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

 private:
  SyntaxTree(std::string source, std::vector<Expression> expressions, Statement statement) noexcept
      : source_(std::move(source)),
        expressions_(std::move(expressions)),
        statement_(std::move(statement)) {}

  std::string source_;
  std::vector<Expression> expressions_;
  Statement statement_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_SYNTAX_AST_HPP_
