#include "modern_sqlite/syntax/ast.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace modern_sqlite {
namespace {

template <class... Callables>
struct Overloaded : Callables... {
  using Callables::operator()...;
};

template <class... Callables>
Overloaded(Callables...) -> Overloaded<Callables...>;

[[nodiscard]] Status Misuse(std::string_view message) {
  return std::unexpected(Error::Create(ErrorCode::kMisuse, std::string{message}));
}

[[nodiscard]] constexpr bool Contains(SourceSpan outer, SourceSpan inner) noexcept {
  return outer.begin() <= inner.begin() && inner.end() <= outer.end();
}

[[nodiscard]] constexpr bool IsValid(LiteralKind value) noexcept {
  switch (value) {
    case LiteralKind::kNull:
    case LiteralKind::kInteger:
    case LiteralKind::kReal:
    case LiteralKind::kString:
    case LiteralKind::kBlob:
    case LiteralKind::kCurrentDate:
    case LiteralKind::kCurrentTime:
    case LiteralKind::kCurrentTimestamp:
    case LiteralKind::kTrue:
    case LiteralKind::kFalse:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool IsValid(UnaryOperator value) noexcept {
  switch (value) {
    case UnaryOperator::kPositive:
    case UnaryOperator::kNegative:
    case UnaryOperator::kBitwiseNot:
    case UnaryOperator::kNot:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool IsValid(BinaryOperator value) noexcept {
  switch (value) {
    case BinaryOperator::kOr:
    case BinaryOperator::kAnd:
    case BinaryOperator::kLess:
    case BinaryOperator::kLessOrEqual:
    case BinaryOperator::kGreater:
    case BinaryOperator::kGreaterOrEqual:
    case BinaryOperator::kEqual:
    case BinaryOperator::kNotEqual:
    case BinaryOperator::kIs:
    case BinaryOperator::kIsNot:
    case BinaryOperator::kAdd:
    case BinaryOperator::kSubtract:
    case BinaryOperator::kMultiply:
    case BinaryOperator::kDivide:
    case BinaryOperator::kRemainder:
    case BinaryOperator::kLeftShift:
    case BinaryOperator::kRightShift:
    case BinaryOperator::kBitwiseAnd:
    case BinaryOperator::kBitwiseOr:
    case BinaryOperator::kConcatenate:
    case BinaryOperator::kLike:
    case BinaryOperator::kNotLike:
    case BinaryOperator::kGlob:
    case BinaryOperator::kNotGlob:
    case BinaryOperator::kRegexp:
    case BinaryOperator::kNotRegexp:
    case BinaryOperator::kMatch:
    case BinaryOperator::kNotMatch:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool IsValid(SelectQuantifier value) noexcept {
  switch (value) {
    case SelectQuantifier::kDefault:
    case SelectQuantifier::kAll:
    case SelectQuantifier::kDistinct:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool IsValid(SortOrder value) noexcept {
  switch (value) {
    case SortOrder::kDefault:
    case SortOrder::kAscending:
    case SortOrder::kDescending:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool IsValid(NullOrder value) noexcept {
  switch (value) {
    case NullOrder::kDefault:
    case NullOrder::kFirst:
    case NullOrder::kLast:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool IsValid(ConflictAction value) noexcept {
  switch (value) {
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

[[nodiscard]] constexpr bool IsValid(LimitSyntax value) noexcept {
  switch (value) {
    case LimitSyntax::kLimitOnly:
    case LimitSyntax::kOffsetKeyword:
    case LimitSyntax::kComma:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool IsValid(BeginTransactionMode value) noexcept {
  switch (value) {
    case BeginTransactionMode::kDeferred:
    case BeginTransactionMode::kImmediate:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool IsValid(CommitTransactionSyntax value) noexcept {
  switch (value) {
    case CommitTransactionSyntax::kCommit:
    case CommitTransactionSyntax::kEnd:
      return true;
  }
  return false;
}

class Validator final {
 public:
  Validator(std::size_t source_size, std::span<const Expression> expressions)
      : source_size_(source_size),
        expressions_(expressions),
        reference_counts_(expressions.size(), 0) {}

  [[nodiscard]] Status Validate(const Statement& statement) {
    if (source_size_ == 0U) {
      return Misuse("syntax tree source must not be empty");
    }

    for (std::size_t index = 0; index < expressions_.size(); ++index) {
      const Status status = ValidateExpression(index);
      if (!status.has_value()) {
        return status;
      }
    }

    if (statement.valueless_by_exception()) {
      return Misuse("statement variant must contain a value");
    }
    const Status statement_status = std::visit(
        Overloaded{
            [this](const SelectStatement& select) { return ValidateSelect(select); },
            [this](const CreateTableStatement& table) { return ValidateCreateTable(table); },
            [this](const CreateIndexStatement& index) { return ValidateCreateIndex(index); },
            [this](const AnalyzeStatement& analyze) { return ValidateAnalyze(analyze); },
            [this](const InsertStatement& insert) { return ValidateInsert(insert); },
            [this](const UpdateStatement& update) { return ValidateUpdate(update); },
            [this](const DeleteStatement& delete_statement) {
              return ValidateDelete(delete_statement);
            },
            [this](const BeginTransactionStatement& transaction) {
              return ValidateBegin(transaction);
            },
            [this](const CommitTransactionStatement& transaction) {
              return ValidateCommit(transaction);
            },
            [this](const RollbackTransactionStatement& transaction) {
              return ValidateTransaction(transaction.span);
            },
            [this](const SavepointStatement& savepoint) {
              return ValidateSavepoint(savepoint.span, savepoint.name);
            },
            [this](const ReleaseSavepointStatement& savepoint) {
              return ValidateSavepoint(savepoint.span, savepoint.name);
            },
            [this](const RollbackToSavepointStatement& savepoint) {
              return ValidateSavepoint(savepoint.span, savepoint.name);
            },
        },
        statement);
    if (!statement_status.has_value()) {
      return statement_status;
    }

    for (const std::uint8_t count : reference_counts_) {
      if (count != 1U) {
        return Misuse("every expression must have exactly one owner");
      }
    }
    return {};
  }

 private:
  [[nodiscard]] Status ValidateRequiredSpan(SourceSpan span) const {
    if (span.empty()) {
      return Misuse("required syntax span must not be empty");
    }
    if (span.end().value() > source_size_) {
      return Misuse("syntax span exceeds owned source");
    }
    return {};
  }

  [[nodiscard]] Status ValidateContainedSpan(SourceSpan span, SourceSpan owner) const {
    const Status status = ValidateRequiredSpan(span);
    if (!status.has_value()) {
      return status;
    }
    if (!Contains(owner, span)) {
      return Misuse("syntax span lies outside its owner");
    }
    return {};
  }

  [[nodiscard]] Status ValidateOptionalSpan(const std::optional<SourceSpan>& span,
                                            SourceSpan owner) const {
    if (!span.has_value()) {
      return {};
    }
    return ValidateContainedSpan(*span, owner);
  }

  [[nodiscard]] Status ValidateRootSpan(SourceSpan span) const {
    const Status status = ValidateRequiredSpan(span);
    if (!status.has_value()) {
      return status;
    }
    if (span.begin().value() != 0U) {
      return Misuse("root statement span must begin at source offset zero");
    }
    return {};
  }

  [[nodiscard]] Status ValidateName(const QualifiedName& name, SourceSpan owner,
                                    std::size_t maximum_parts) const {
    const Status full_span_status = ValidateContainedSpan(name.span, owner);
    if (!full_span_status.has_value()) {
      return full_span_status;
    }
    if (name.parts.empty() || name.parts.size() > maximum_parts) {
      return Misuse("qualified name has an invalid part count");
    }

    ByteOffset previous_end = name.span.begin();
    for (const SourceSpan part : name.parts) {
      const Status part_status = ValidateContainedSpan(part, name.span);
      if (!part_status.has_value()) {
        return part_status;
      }
      if (part.begin() < previous_end) {
        return Misuse("qualified name parts must be ordered and nonoverlapping");
      }
      previous_end = part.end();
    }
    if (name.parts.front().begin() != name.span.begin() ||
        name.parts.back().end() != name.span.end()) {
      return Misuse("qualified name span must cover its first through last part");
    }
    return {};
  }

  [[nodiscard]] Status ReferenceExpression(ExpressionId id, SourceSpan owner,
                                           std::optional<std::size_t> parent) {
    if (id.value >= expressions_.size()) {
      return Misuse("expression reference is out of range");
    }
    if (parent.has_value() && id.value >= *parent) {
      return Misuse("expression children must precede their parent");
    }
    if (!Contains(owner, expressions_[id.value].span)) {
      return Misuse("referenced expression lies outside its owning syntax");
    }
    if (reference_counts_[id.value] != 0U) {
      return Misuse("expression must not have multiple owners");
    }
    reference_counts_[id.value] = 1U;
    return {};
  }

  [[nodiscard]] Status ValidateExpression(std::size_t index) {
    const Expression& expression = expressions_[index];
    const Status span_status = ValidateRequiredSpan(expression.span);
    if (!span_status.has_value()) {
      return span_status;
    }
    if (expression.payload.valueless_by_exception()) {
      return Misuse("expression variant must contain a value");
    }

    return std::visit(
        Overloaded{
            [this, &expression](const LiteralExpression& literal) {
              if (!IsValid(literal.kind)) {
                return Misuse("literal kind is invalid");
              }
              const Status token_status = ValidateContainedSpan(literal.token, expression.span);
              if (!token_status.has_value()) {
                return token_status;
              }
              if (literal.token != expression.span) {
                return Misuse("literal expression span must equal its token span");
              }
              return Status{};
            },
            [this, &expression](const VariableExpression& variable) {
              const Status token_status = ValidateContainedSpan(variable.token, expression.span);
              if (!token_status.has_value()) {
                return token_status;
              }
              if (variable.token != expression.span) {
                return Misuse("variable expression span must equal its token span");
              }
              return Status{};
            },
            [this, &expression](const IdentifierExpression& identifier) {
              const Status name_status = ValidateName(identifier.name, expression.span, 3U);
              if (!name_status.has_value()) {
                return name_status;
              }
              if (identifier.name.span != expression.span) {
                return Misuse("identifier expression span must equal its name span");
              }
              return Status{};
            },
            [this, &expression](const WildcardExpression& wildcard) {
              const Status asterisk_status =
                  ValidateContainedSpan(wildcard.asterisk, expression.span);
              if (!asterisk_status.has_value()) {
                return asterisk_status;
              }
              if (!wildcard.qualifier.has_value()) {
                if (wildcard.asterisk != expression.span) {
                  return Misuse("unqualified wildcard span must equal its asterisk span");
                }
                return Status{};
              }
              const Status qualifier_status =
                  ValidateName(*wildcard.qualifier, expression.span, 1U);
              if (!qualifier_status.has_value()) {
                return qualifier_status;
              }
              if (wildcard.qualifier->span.begin() != expression.span.begin() ||
                  wildcard.qualifier->span.end() > wildcard.asterisk.begin() ||
                  wildcard.asterisk.end() != expression.span.end()) {
                return Misuse("qualified wildcard spans are not ordered");
              }
              return Status{};
            },
            [this, &expression, index](const UnaryExpression& unary) {
              if (!IsValid(unary.op)) {
                return Misuse("unary operator is invalid");
              }
              const Status operator_status =
                  ValidateContainedSpan(unary.operator_span, expression.span);
              if (!operator_status.has_value()) {
                return operator_status;
              }
              const Status operand_status =
                  ReferenceExpression(unary.operand, expression.span, index);
              if (!operand_status.has_value()) {
                return operand_status;
              }
              const SourceSpan operand_span = expressions_[unary.operand.value].span;
              if (expression.span.begin() != unary.operator_span.begin() ||
                  unary.operator_span.end() > operand_span.begin() ||
                  operand_span.end() != expression.span.end()) {
                return Misuse("unary expression span must exactly cover operator and operand");
              }
              return Status{};
            },
            [this, &expression, index](const BinaryExpression& binary) {
              if (!IsValid(binary.op)) {
                return Misuse("binary operator is invalid");
              }
              const Status operator_status =
                  ValidateContainedSpan(binary.operator_span, expression.span);
              if (!operator_status.has_value()) {
                return operator_status;
              }
              const Status left_status = ReferenceExpression(binary.left, expression.span, index);
              if (!left_status.has_value()) {
                return left_status;
              }
              const Status right_status = ReferenceExpression(binary.right, expression.span, index);
              if (!right_status.has_value()) {
                return right_status;
              }
              const SourceSpan left_span = expressions_[binary.left.value].span;
              const SourceSpan right_span = expressions_[binary.right.value].span;
              if (expression.span.begin() != left_span.begin() ||
                  left_span.end() > binary.operator_span.begin() ||
                  binary.operator_span.end() > right_span.begin()) {
                return Misuse("binary operands and operator must be in source order");
              }
              if (right_span.end() != expression.span.end()) {
                return Misuse("binary expression span must exactly cover both operands");
              }
              return Status{};
            },
            [this, &expression, index](const FunctionCallExpression& call) {
              const Status name_status = ValidateName(call.name, expression.span, 1U);
              if (!name_status.has_value()) {
                return name_status;
              }
              if (call.name.span.begin() != expression.span.begin()) {
                return Misuse("function expression must begin with its name");
              }

              ByteOffset previous_end = call.name.span.end();
              for (const ExpressionId argument : call.arguments) {
                const Status argument_status =
                    ReferenceExpression(argument, expression.span, index);
                if (!argument_status.has_value()) {
                  return argument_status;
                }
                const SourceSpan argument_span = expressions_[argument.value].span;
                if (argument_span.begin() < previous_end) {
                  return Misuse("function arguments must be in source order");
                }
                previous_end = argument_span.end();
              }
              return Status{};
            },
            [this, &expression, index](const CollateExpression& collate) {
              const Status keyword_status = ValidateContainedSpan(collate.keyword, expression.span);
              if (!keyword_status.has_value()) {
                return keyword_status;
              }
              const Status collation_status =
                  ValidateContainedSpan(collate.collation, expression.span);
              if (!collation_status.has_value()) {
                return collation_status;
              }
              const Status operand_status =
                  ReferenceExpression(collate.operand, expression.span, index);
              if (!operand_status.has_value()) {
                return operand_status;
              }
              const SourceSpan operand_span = expressions_[collate.operand.value].span;
              if (expression.span.begin() != operand_span.begin() ||
                  operand_span.end() > collate.keyword.begin() ||
                  collate.keyword.end() > collate.collation.begin() ||
                  collate.collation.end() != expression.span.end()) {
                return Misuse("collation expression spans are not ordered");
              }
              return Status{};
            },
            [this, &expression, index](const ParenthesizedExpression& parenthesized) {
              const Status child_status =
                  ReferenceExpression(parenthesized.inner, expression.span, index);
              if (!child_status.has_value()) {
                return child_status;
              }
              const SourceSpan child_span = expressions_[parenthesized.inner.value].span;
              if (expression.span.begin() >= child_span.begin() ||
                  expression.span.end() <= child_span.end()) {
                return Misuse("parenthesized expression must strictly contain its child");
              }
              return Status{};
            },
        },
        expression.payload);
  }

  [[nodiscard]] Status ValidateResultColumn(const ResultColumn& column, SourceSpan statement_span) {
    const Status span_status = ValidateContainedSpan(column.span, statement_span);
    if (!span_status.has_value()) {
      return span_status;
    }
    const Status expression_status =
        ReferenceExpression(column.expression, column.span, std::nullopt);
    if (!expression_status.has_value()) {
      return expression_status;
    }
    const Status alias_status = ValidateOptionalSpan(column.alias, column.span);
    if (!alias_status.has_value()) {
      return alias_status;
    }
    if (column.alias.has_value() &&
        expressions_[column.expression.value].span.end() > column.alias->begin()) {
      return Misuse("result alias must follow its expression");
    }
    return {};
  }

  [[nodiscard]] Status ValidateTableSource(const TableSource& source,
                                           SourceSpan statement_span) const {
    const Status span_status = ValidateContainedSpan(source.span, statement_span);
    if (!span_status.has_value()) {
      return span_status;
    }
    const Status name_status = ValidateName(source.name, source.span, 2U);
    if (!name_status.has_value()) {
      return name_status;
    }
    const Status alias_status = ValidateOptionalSpan(source.alias, source.span);
    if (!alias_status.has_value()) {
      return alias_status;
    }
    if (source.alias.has_value() && source.name.span.end() > source.alias->begin()) {
      return Misuse("table alias must follow its name");
    }
    return {};
  }

  [[nodiscard]] Status ValidateLimit(const LimitClause& limit, SourceSpan statement_span) {
    const Status span_status = ValidateContainedSpan(limit.span, statement_span);
    if (!span_status.has_value()) {
      return span_status;
    }
    if (!IsValid(limit.syntax)) {
      return Misuse("limit syntax is invalid");
    }
    if ((limit.syntax == LimitSyntax::kLimitOnly) != !limit.offset.has_value()) {
      return Misuse("limit syntax and offset presence disagree");
    }

    const Status limit_status = ReferenceExpression(limit.limit, limit.span, std::nullopt);
    if (!limit_status.has_value()) {
      return limit_status;
    }
    if (limit.offset.has_value()) {
      const Status offset_status = ReferenceExpression(*limit.offset, limit.span, std::nullopt);
      if (!offset_status.has_value()) {
        return offset_status;
      }
      const SourceSpan limit_expression = expressions_[limit.limit.value].span;
      const SourceSpan offset_expression = expressions_[limit.offset->value].span;
      if (limit.syntax == LimitSyntax::kOffsetKeyword &&
          limit_expression.end() > offset_expression.begin()) {
        return Misuse("OFFSET expression must follow the LIMIT expression");
      }
      if (limit.syntax == LimitSyntax::kComma &&
          offset_expression.end() > limit_expression.begin()) {
        return Misuse("comma LIMIT offset must precede the normalized limit");
      }
    }
    return {};
  }

  [[nodiscard]] Status ValidateSelect(const SelectStatement& select) {
    const Status root_status = ValidateRootSpan(select.span);
    if (!root_status.has_value()) {
      return root_status;
    }
    if (!IsValid(select.quantifier)) {
      return Misuse("select quantifier is invalid");
    }
    if (select.result_columns.empty()) {
      return Misuse("select statement requires at least one result column");
    }

    ByteOffset previous_end = select.span.begin();
    for (const ResultColumn& column : select.result_columns) {
      const Status column_status = ValidateResultColumn(column, select.span);
      if (!column_status.has_value()) {
        return column_status;
      }
      if (column.span.begin() < previous_end) {
        return Misuse("result columns must be in source order");
      }
      previous_end = column.span.end();
    }
    ByteOffset clause_end = previous_end;
    if (select.from.has_value()) {
      const Status source_status = ValidateTableSource(*select.from, select.span);
      if (!source_status.has_value()) {
        return source_status;
      }
      if (select.from->span.begin() < clause_end) {
        return Misuse("select FROM clause is out of order");
      }
      clause_end = select.from->span.end();
    }
    if (select.where.has_value()) {
      const Status where_status = ReferenceExpression(*select.where, select.span, std::nullopt);
      if (!where_status.has_value()) {
        return where_status;
      }
      const SourceSpan where_span = expressions_[select.where->value].span;
      if (where_span.begin() < clause_end) {
        return Misuse("select WHERE clause is out of order");
      }
      clause_end = where_span.end();
    }
    ByteOffset previous_order_end = clause_end;
    for (const OrderingTerm& term : select.order_by) {
      if (!Contains(select.span, term.span) || !IsValid(term.order) || !IsValid(term.null_order)) {
        return Misuse("select ORDER BY term is invalid");
      }
      if (term.span.begin() < previous_order_end) {
        return Misuse("select ORDER BY terms overlap or are out of order");
      }
      const Status expression_status =
          ReferenceExpression(term.expression, term.span, std::nullopt);
      if (!expression_status.has_value()) {
        return expression_status;
      }
      previous_order_end = term.span.end();
      clause_end = term.span.end();
    }
    if (select.limit.has_value()) {
      if (select.limit->span.begin() < clause_end) {
        return Misuse("select LIMIT clause is out of order");
      }
      return ValidateLimit(*select.limit, select.span);
    }
    return {};
  }

  [[nodiscard]] Status ValidateIndexedTerm(const IndexedTerm& term, SourceSpan owner) {
    const Status span_status = ValidateContainedSpan(term.span, owner);
    if (!span_status.has_value()) {
      return span_status;
    }
    if (!IsValid(term.order)) {
      return Misuse("indexed term sort order is invalid");
    }
    const Status expression_status = ReferenceExpression(term.expression, term.span, std::nullopt);
    if (!expression_status.has_value()) {
      return expression_status;
    }
    const Status collation_status = ValidateOptionalSpan(term.collation, term.span);
    if (!collation_status.has_value()) {
      return collation_status;
    }
    if (term.collation.has_value() &&
        expressions_[term.expression.value].span.end() > term.collation->begin()) {
      return Misuse("indexed term collation must follow its expression");
    }
    return {};
  }

  [[nodiscard]] Status ValidateIndexedTerms(std::span<const IndexedTerm> terms, SourceSpan owner) {
    if (terms.empty()) {
      return Misuse("key or index requires at least one indexed term");
    }
    ByteOffset previous_end = owner.begin();
    for (const IndexedTerm& term : terms) {
      const Status term_status = ValidateIndexedTerm(term, owner);
      if (!term_status.has_value()) {
        return term_status;
      }
      if (term.span.begin() < previous_end) {
        return Misuse("indexed terms must be in source order");
      }
      previous_end = term.span.end();
    }
    return {};
  }

  [[nodiscard]] Status ValidateColumnConstraint(const ColumnConstraint& constraint,
                                                SourceSpan column_span) {
    const Status span_status = ValidateContainedSpan(constraint.span, column_span);
    if (!span_status.has_value()) {
      return span_status;
    }
    const Status name_status = ValidateOptionalSpan(constraint.name, constraint.span);
    if (!name_status.has_value()) {
      return name_status;
    }
    if (constraint.payload.valueless_by_exception()) {
      return Misuse("column constraint variant must contain a value");
    }

    return std::visit(
        Overloaded{
            [](const PrimaryKeyColumnConstraint& primary_key) {
              if (!IsValid(primary_key.order) || !IsValid(primary_key.conflict)) {
                return Misuse("primary-key column constraint is invalid");
              }
              return Status{};
            },
            [](const NullColumnConstraint& null_value) {
              if (!IsValid(null_value.conflict)) {
                return Misuse("null conflict action is invalid");
              }
              return Status{};
            },
            [](const NotNullColumnConstraint& not_null) {
              if (!IsValid(not_null.conflict)) {
                return Misuse("not-null conflict action is invalid");
              }
              return Status{};
            },
            [](const UniqueColumnConstraint& unique) {
              if (!IsValid(unique.conflict)) {
                return Misuse("unique conflict action is invalid");
              }
              return Status{};
            },
            [this, &constraint](const CheckColumnConstraint& check) {
              return ReferenceExpression(check.expression, constraint.span, std::nullopt);
            },
            [this, &constraint](const DefaultColumnConstraint& default_value) {
              return ReferenceExpression(default_value.expression, constraint.span, std::nullopt);
            },
            [this, &constraint](const CollateColumnConstraint& collate) {
              return ValidateContainedSpan(collate.collation, constraint.span);
            },
        },
        constraint.payload);
  }

  [[nodiscard]] Status ValidateColumn(const ColumnDefinition& column, SourceSpan statement_span) {
    const Status span_status = ValidateContainedSpan(column.span, statement_span);
    if (!span_status.has_value()) {
      return span_status;
    }
    const Status name_status = ValidateContainedSpan(column.name, column.span);
    if (!name_status.has_value()) {
      return name_status;
    }
    if (column.name.begin() != column.span.begin()) {
      return Misuse("column definition must begin with its name");
    }
    const Status type_status = ValidateOptionalSpan(column.type_name, column.span);
    if (!type_status.has_value()) {
      return type_status;
    }
    if (column.type_name.has_value() && column.name.end() > column.type_name->begin()) {
      return Misuse("column type must follow the column name");
    }

    ByteOffset previous_end =
        column.type_name.has_value() ? column.type_name->end() : column.name.end();
    for (const ColumnConstraint& constraint : column.constraints) {
      const Status constraint_status = ValidateColumnConstraint(constraint, column.span);
      if (!constraint_status.has_value()) {
        return constraint_status;
      }
      if (constraint.span.begin() < previous_end) {
        return Misuse("column constraints must be in source order");
      }
      previous_end = constraint.span.end();
    }
    if (previous_end != column.span.end()) {
      return Misuse("column definition span must end with its final component");
    }
    return {};
  }

  [[nodiscard]] Status ValidateTableConstraint(const TableConstraint& constraint,
                                               SourceSpan statement_span) {
    const Status span_status = ValidateContainedSpan(constraint.span, statement_span);
    if (!span_status.has_value()) {
      return span_status;
    }
    const Status name_status = ValidateOptionalSpan(constraint.name, constraint.span);
    if (!name_status.has_value()) {
      return name_status;
    }
    if (constraint.payload.valueless_by_exception()) {
      return Misuse("table constraint variant must contain a value");
    }

    return std::visit(Overloaded{
                          [this, &constraint](const PrimaryKeyTableConstraint& primary_key) {
                            if (!IsValid(primary_key.conflict)) {
                              return Misuse("primary-key conflict action is invalid");
                            }
                            return ValidateIndexedTerms(primary_key.terms, constraint.span);
                          },
                          [this, &constraint](const UniqueTableConstraint& unique) {
                            if (!IsValid(unique.conflict)) {
                              return Misuse("unique conflict action is invalid");
                            }
                            return ValidateIndexedTerms(unique.terms, constraint.span);
                          },
                          [this, &constraint](const CheckTableConstraint& check) {
                            return ReferenceExpression(check.expression, constraint.span,
                                                       std::nullopt);
                          },
                      },
                      constraint.payload);
  }

  [[nodiscard]] Status ValidateCreateTable(const CreateTableStatement& table) {
    const Status root_status = ValidateRootSpan(table.span);
    if (!root_status.has_value()) {
      return root_status;
    }
    const Status name_status = ValidateName(table.name, table.span, 2U);
    if (!name_status.has_value()) {
      return name_status;
    }
    if (table.columns.empty()) {
      return Misuse("create-table statement requires at least one column");
    }

    ByteOffset previous_end = table.name.span.end();
    for (const ColumnDefinition& column : table.columns) {
      const Status column_status = ValidateColumn(column, table.span);
      if (!column_status.has_value()) {
        return column_status;
      }
      if (column.span.begin() < previous_end) {
        return Misuse("table columns must be in source order");
      }
      previous_end = column.span.end();
    }
    for (const TableConstraint& constraint : table.constraints) {
      const Status constraint_status = ValidateTableConstraint(constraint, table.span);
      if (!constraint_status.has_value()) {
        return constraint_status;
      }
      if (constraint.span.begin() < previous_end) {
        return Misuse("table constraints must follow columns in source order");
      }
      previous_end = constraint.span.end();
    }
    return {};
  }

  [[nodiscard]] Status ValidateCreateIndex(const CreateIndexStatement& index) {
    const Status root_status = ValidateRootSpan(index.span);
    if (!root_status.has_value()) {
      return root_status;
    }
    const Status name_status = ValidateName(index.name, index.span, 2U);
    if (!name_status.has_value()) {
      return name_status;
    }
    const Status table_status = ValidateName(index.table, index.span, 1U);
    if (!table_status.has_value()) {
      return table_status;
    }
    const Status terms_status = ValidateIndexedTerms(index.terms, index.span);
    if (!terms_status.has_value()) {
      return terms_status;
    }
    if (index.where.has_value()) {
      return ReferenceExpression(*index.where, index.span, std::nullopt);
    }
    return {};
  }

  [[nodiscard]] Status ValidateAnalyze(const AnalyzeStatement& analyze) {
    const Status root_status = ValidateRootSpan(analyze.span);
    if (!root_status.has_value()) {
      return root_status;
    }
    if (!analyze.target.has_value()) {
      return {};
    }
    return ValidateName(*analyze.target, analyze.span, 2U);
  }

  [[nodiscard]] Status ValidateInsert(const InsertStatement& insert) {
    const Status root_status = ValidateRootSpan(insert.span);
    if (!root_status.has_value()) {
      return root_status;
    }
    const Status table_status = ValidateName(insert.table, insert.span, 2U);
    if (!table_status.has_value()) {
      return table_status;
    }
    ByteOffset previous_end = insert.table.span.end();
    for (const SourceSpan column : insert.columns) {
      const Status column_status = ValidateContainedSpan(column, insert.span);
      if (!column_status.has_value()) {
        return column_status;
      }
      if (column.begin() < previous_end) {
        return Misuse("insert columns must be in source order");
      }
      previous_end = column.end();
    }
    if (insert.source.valueless_by_exception()) {
      return Misuse("insert source variant must contain a value");
    }
    const SourceSpan source_span =
        std::visit([](const auto& source) { return source.span; }, insert.source);
    if (source_span.begin() < previous_end) {
      return Misuse("insert source must follow its target columns");
    }
    const Status source_status = std::visit(
        Overloaded{
            [this, &insert](const InsertValuesSource& values) {
              const Status span_status = ValidateContainedSpan(values.span, insert.span);
              if (!span_status.has_value()) {
                return span_status;
              }
              if (values.values.empty()) {
                return Misuse("insert values source must not be empty");
              }
              ByteOffset previous_value_end = values.span.begin();
              for (const ExpressionId expression : values.values) {
                const Status status = ReferenceExpression(expression, values.span, std::nullopt);
                if (!status.has_value()) {
                  return status;
                }
                const SourceSpan expression_span = expressions_[expression.value].span;
                if (expression_span.begin() < previous_value_end) {
                  return Misuse("insert values must be in source order");
                }
                previous_value_end = expression_span.end();
              }
              return Status{};
            },
            [this, &insert](const InsertDefaultValuesSource& defaults) {
              return ValidateContainedSpan(defaults.span, insert.span);
            },
        },
        insert.source);
    if (!source_status.has_value()) {
      return source_status;
    }
    if (source_span.end() != insert.span.end()) {
      return Misuse("insert source must end the statement");
    }
    return {};
  }

  [[nodiscard]] Status ValidateUpdate(const UpdateStatement& update) {
    const Status root_status = ValidateRootSpan(update.span);
    if (!root_status.has_value()) {
      return root_status;
    }
    const Status table_status = ValidateName(update.table, update.span, 2U);
    if (!table_status.has_value()) {
      return table_status;
    }
    if (update.assignments.empty()) {
      return Misuse("update statement requires at least one assignment");
    }
    ByteOffset previous_end = update.table.span.end();
    for (const UpdateAssignment& assignment : update.assignments) {
      const Status span_status = ValidateContainedSpan(assignment.span, update.span);
      if (!span_status.has_value()) {
        return span_status;
      }
      const Status column_status = ValidateContainedSpan(assignment.column, assignment.span);
      if (!column_status.has_value()) {
        return column_status;
      }
      if (assignment.span.begin() < previous_end) {
        return Misuse("update assignments must be in source order");
      }
      if (assignment.column.begin() != assignment.span.begin()) {
        return Misuse("update assignment must begin with its column");
      }
      const Status expression_status =
          ReferenceExpression(assignment.expression, assignment.span, std::nullopt);
      if (!expression_status.has_value()) {
        return expression_status;
      }
      const SourceSpan expression_span = expressions_[assignment.expression.value].span;
      if (assignment.column.end() > expression_span.begin() ||
          expression_span.end() != assignment.span.end()) {
        return Misuse("update assignment spans are not ordered");
      }
      previous_end = assignment.span.end();
    }
    if (update.where.has_value()) {
      const Status where_status = ReferenceExpression(*update.where, update.span, std::nullopt);
      if (!where_status.has_value()) {
        return where_status;
      }
      const SourceSpan where_span = expressions_[update.where->value].span;
      if (where_span.begin() < previous_end || where_span.end() != update.span.end()) {
        return Misuse("update predicate must follow assignments and end the statement");
      }
      return {};
    }
    if (previous_end != update.span.end()) {
      return Misuse("final update assignment must end the statement");
    }
    return {};
  }

  [[nodiscard]] Status ValidateDelete(const DeleteStatement& delete_statement) {
    const Status root_status = ValidateRootSpan(delete_statement.span);
    if (!root_status.has_value()) {
      return root_status;
    }
    const Status table_status = ValidateName(delete_statement.table, delete_statement.span, 2U);
    if (!table_status.has_value()) {
      return table_status;
    }
    if (delete_statement.where.has_value()) {
      const Status where_status =
          ReferenceExpression(*delete_statement.where, delete_statement.span, std::nullopt);
      if (!where_status.has_value()) {
        return where_status;
      }
      const SourceSpan where_span = expressions_[delete_statement.where->value].span;
      if (where_span.begin() < delete_statement.table.span.end() ||
          where_span.end() != delete_statement.span.end()) {
        return Misuse("delete predicate must follow the table and end the statement");
      }
      return {};
    }
    if (delete_statement.table.span.end() != delete_statement.span.end()) {
      return Misuse("delete table must end a predicate-free statement");
    }
    return {};
  }

  [[nodiscard]] Status ValidateTransaction(SourceSpan span) const { return ValidateRootSpan(span); }

  [[nodiscard]] Status ValidateBegin(const BeginTransactionStatement& transaction) const {
    if (!IsValid(transaction.mode)) {
      return Misuse("begin-transaction mode is invalid");
    }
    return ValidateTransaction(transaction.span);
  }

  [[nodiscard]] Status ValidateCommit(const CommitTransactionStatement& transaction) const {
    if (!IsValid(transaction.syntax)) {
      return Misuse("commit-transaction syntax is invalid");
    }
    return ValidateTransaction(transaction.span);
  }

  [[nodiscard]] Status ValidateSavepoint(SourceSpan span, SourceSpan name) const {
    const Status root_status = ValidateRootSpan(span);
    if (!root_status.has_value()) {
      return root_status;
    }
    const Status name_status = ValidateContainedSpan(name, span);
    if (!name_status.has_value()) {
      return name_status;
    }
    if (name.begin() <= span.begin() || name.end() != span.end()) {
      return Misuse("savepoint name must be the final statement component");
    }
    return {};
  }

  std::size_t source_size_;
  std::span<const Expression> expressions_;
  std::vector<std::uint8_t> reference_counts_;
};

}  // namespace

Result<SyntaxTree> SyntaxTree::Create(std::string source, std::vector<Expression> expressions,
                                      Statement statement) {
  Validator validator{source.size(), std::span<const Expression>{expressions}};
  Status status = validator.Validate(statement);
  if (!status.has_value()) {
    return std::unexpected(std::move(status.error()));
  }
  return SyntaxTree{std::move(source), std::move(expressions), std::move(statement)};
}

const Expression& SyntaxTree::expression(ExpressionId id) const noexcept {
  assert(id.value < expressions_.size());
  return expressions_[id.value];
}

}  // namespace modern_sqlite
