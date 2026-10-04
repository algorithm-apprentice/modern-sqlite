#include "modern_sqlite/catalog/catalog_loader.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../runtime/sqlite_float.hpp"
#include "../syntax/token_text.hpp"
#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/database_format.hpp"
#include "modern_sqlite/storage/page_number.hpp"
#include "modern_sqlite/syntax/ast.hpp"
#include "modern_sqlite/syntax/lexer.hpp"
#include "modern_sqlite/syntax/parser.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {
namespace {

enum class SchemaExpressionContext : std::uint8_t {
  kDefault,
  kCheck,
  kIndex,
  kPartialIndex,
};

struct SchemaRow {
  std::int64_t rowid = 0;
  std::optional<std::string> type;
  std::string name;
  std::optional<std::string> table_name;
  std::uint32_t root_page = 0;
  std::optional<std::string> sql;
};

struct ParsedStat1 {
  std::vector<std::uint64_t> values;
  std::optional<std::uint64_t> average_row_size;
  bool unordered = false;
  bool no_skip_scan = false;
};

struct IndexedTermClassification {
  ExpressionId expression;
  std::optional<SourceSpan> collation;
};

[[nodiscard]] Error LoaderError(ErrorCode code, std::string message) {
  return Error::Create(code, std::move(message));
}

[[nodiscard]] Error Corruption(std::string message) {
  return LoaderError(ErrorCode::kCorruption, std::move(message));
}

[[nodiscard]] Error Protocol(std::string message) {
  return LoaderError(ErrorCode::kProtocol, std::move(message));
}

[[nodiscard]] Error TooLarge(std::string message) {
  return LoaderError(ErrorCode::kTooLarge, std::move(message));
}

[[nodiscard]] Error Misuse(std::string message) {
  return LoaderError(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] bool IsSqliteSpace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\v' || value == '\f' ||
         value == '\r';
}

[[nodiscard]] bool EqualsAsciiCaseInsensitive(std::string_view left,
                                              std::string_view right) noexcept {
  return CatalogNamesEqual(left, right);
}

[[nodiscard]] bool IsQuotedToken(std::string_view token) noexcept {
  return !token.empty() && (token.front() == '\'' || token.front() == '"' || token.front() == '`' ||
                            token.front() == '[');
}

[[nodiscard]] std::string FoldName(std::string_view value) {
  std::string folded{value};
  for (char& byte : folded) {
    const auto unsigned_byte = static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
    byte = static_cast<char>(SqliteToLower(unsigned_byte));
  }
  return folded;
}

[[nodiscard]] std::string_view SpanText(const SyntaxTree& tree, SourceSpan span) {
  const auto text = Slice(tree.source(), span);
  if (!text.has_value()) {
    return {};
  }
  return text->bytes();
}

[[nodiscard]] Result<std::string> Dequote(std::string_view value) {
  std::expected<std::string, internal::DequoteSqlTokenError> dequoted =
      internal::DequoteSqlToken(value);
  if (!dequoted.has_value()) {
    return std::unexpected(Corruption("schema contains an unterminated quoted name"));
  }
  return std::move(*dequoted);
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

[[nodiscard]] Result<SqlValue> MaterializeIntegerLiteral(std::string_view token) {
  const std::string stripped = StripNumericUnderscores(token);
  if (stripped.size() > 2U && stripped.front() == '0' &&
      (stripped[1] == 'x' || stripped[1] == 'X')) {
    std::string_view digits{stripped};
    digits.remove_prefix(2);
    while (!digits.empty() && digits.front() == '0') {
      digits.remove_prefix(1);
    }
    if (digits.size() > 16U) {
      return std::unexpected(Corruption("schema contains an oversized hexadecimal literal"));
    }
    std::uint64_t value = 0;
    for (const char byte : digits) {
      const std::optional<std::uint8_t> digit = HexDigit(byte);
      if (!digit.has_value()) {
        return std::unexpected(Corruption("schema contains a malformed hexadecimal literal"));
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

[[nodiscard]] Result<SqlValue> MaterializeBlobLiteral(std::string_view token) {
  if (token.size() < 3U || (token.front() != 'x' && token.front() != 'X') || token[1] != '\'' ||
      token.back() != '\'' || ((token.size() - 3U) % 2U) != 0U) {
    return std::unexpected(Corruption("schema contains a malformed blob literal"));
  }
  ByteBuffer bytes{ByteCount{(token.size() - 3U) / 2U}};
  const MutableByteView output = bytes.mutable_view();
  for (std::size_t input = 2U, index = 0; input + 1U < token.size(); input += 2U, ++index) {
    const std::optional<std::uint8_t> high = HexDigit(token[input]);
    const std::optional<std::uint8_t> low = HexDigit(token[input + 1U]);
    if (!high.has_value() || !low.has_value()) {
      return std::unexpected(Corruption("schema contains a malformed blob literal"));
    }
    output[index] = static_cast<std::byte>((static_cast<std::uint32_t>(*high) << 4U) |
                                           static_cast<std::uint32_t>(*low));
  }
  return SqlValue::Blob(std::move(bytes));
}

[[nodiscard]] Result<std::optional<SqlValue>> MaterializeDefaultValue(const SyntaxTree& tree,
                                                                      ExpressionId id) {
  const Expression& expression = tree.expression(id);
  if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
      parenthesized != nullptr) {
    return MaterializeDefaultValue(tree, parenthesized->inner);
  }
  if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
      literal != nullptr) {
    const std::string_view token = SpanText(tree, literal->token);
    switch (literal->kind) {
      case LiteralKind::kNull:
        return std::optional<SqlValue>{std::in_place};
      case LiteralKind::kInteger: {
        auto value = MaterializeIntegerLiteral(token);
        if (!value.has_value()) {
          return std::unexpected(std::move(value.error()));
        }
        return std::optional<SqlValue>{std::in_place, std::move(*value)};
      }
      case LiteralKind::kReal:
        return std::optional<SqlValue>{
            std::in_place,
            SqlValue::Real(internal::ParseSqliteReal(StripNumericUnderscores(token)))};
      case LiteralKind::kString: {
        auto value = Dequote(token);
        if (!value.has_value()) {
          return std::unexpected(std::move(value.error()));
        }
        return std::optional<SqlValue>{std::in_place, SqlValue::Text(std::move(*value))};
      }
      case LiteralKind::kBlob: {
        auto value = MaterializeBlobLiteral(token);
        if (!value.has_value()) {
          return std::unexpected(std::move(value.error()));
        }
        return std::optional<SqlValue>{std::in_place, std::move(*value)};
      }
      case LiteralKind::kTrue:
        return std::optional<SqlValue>{std::in_place, SqlValue::Integer(1)};
      case LiteralKind::kFalse:
        return std::optional<SqlValue>{std::in_place, SqlValue::Integer(0)};
      case LiteralKind::kCurrentDate:
      case LiteralKind::kCurrentTime:
      case LiteralKind::kCurrentTimestamp:
        return std::optional<SqlValue>{};
    }
  }
  if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.payload);
      identifier != nullptr && identifier->name.parts.size() == 1U) {
    const std::string_view token = SpanText(tree, identifier->name.parts.front());
    if (!IsQuotedToken(token) && EqualsAsciiCaseInsensitive(token, "true")) {
      return std::optional<SqlValue>{std::in_place, SqlValue::Integer(1)};
    }
    if (!IsQuotedToken(token) && EqualsAsciiCaseInsensitive(token, "false")) {
      return std::optional<SqlValue>{std::in_place, SqlValue::Integer(0)};
    }
    return std::optional<SqlValue>{};
  }
  if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload);
      unary != nullptr &&
      (unary->op == UnaryOperator::kPositive || unary->op == UnaryOperator::kNegative)) {
    const Expression& operand = tree.expression(unary->operand);
    if (unary->op == UnaryOperator::kNegative) {
      const auto* integer = std::get_if<LiteralExpression>(&operand.payload);
      if (integer != nullptr && integer->kind == LiteralKind::kInteger) {
        const std::string normalized = StripNumericUnderscores(SpanText(tree, integer->token));
        const std::size_t first_nonzero = normalized.find_first_not_of('0');
        if (!(normalized.starts_with("0x") || normalized.starts_with("0X")) &&
            first_nonzero != std::string::npos &&
            std::string_view{normalized}.substr(first_nonzero) == "9223372036854775808") {
          return std::optional<SqlValue>{
              std::in_place, SqlValue::Integer(std::numeric_limits<std::int64_t>::min())};
        }
      }
    }
    auto operand_value = MaterializeDefaultValue(tree, unary->operand);
    if (!operand_value.has_value()) {
      return std::unexpected(std::move(operand_value.error()));
    }
    if (!operand_value->has_value()) {
      return std::optional<SqlValue>{};
    }
    SqlValue value = std::move(**operand_value);
    if (value.type() == SqlValueType::kInteger) {
      const std::int64_t integer = value.integer_value().value_or(0);
      if (unary->op == UnaryOperator::kPositive) {
        return std::optional<SqlValue>{std::in_place, std::move(value)};
      }
      if (integer == std::numeric_limits<std::int64_t>::min()) {
        return std::optional<SqlValue>{std::in_place,
                                       SqlValue::Real(-static_cast<double>(integer))};
      }
      return std::optional<SqlValue>{std::in_place, SqlValue::Integer(-integer)};
    }
    if (value.type() == SqlValueType::kReal) {
      const double real = value.real_value().value_or(0.0);
      return std::optional<SqlValue>{
          std::in_place, SqlValue::Real(unary->op == UnaryOperator::kNegative ? -real : real)};
    }
  }
  return std::optional<SqlValue>{};
}

[[nodiscard]] Result<std::string> DequoteSpan(const SyntaxTree& tree, SourceSpan span) {
  return Dequote(SpanText(tree, span));
}

[[nodiscard]] Result<std::string> UnqualifiedName(const SyntaxTree& tree,
                                                  const QualifiedName& name) {
  if (name.parts.size() != 1U) {
    return std::unexpected(Corruption("persistent schema DDL contains a qualified name"));
  }
  return DequoteSpan(tree, name.parts.front());
}

[[nodiscard]] Result<std::optional<std::string>> NormalizeDeclaredType(
    std::optional<std::string_view> raw_type) {
  if (!raw_type.has_value()) {
    return std::optional<std::string>{};
  }
  std::size_t end = raw_type->size();
  if (end >= 16U && EqualsAsciiCaseInsensitive(raw_type->substr(end - 6U), "ALWAYS")) {
    end -= 6U;
    while (end > 0U && IsSqliteSpace((*raw_type)[end - 1U])) {
      --end;
    }
    if (end >= 9U && EqualsAsciiCaseInsensitive(raw_type->substr(end - 9U, 9U), "GENERATED")) {
      end -= 9U;
      while (end > 0U && IsSqliteSpace((*raw_type)[end - 1U])) {
        --end;
      }
    }
  }
  if (end == 0U) {
    return std::optional<std::string>{};
  }
  auto dequoted = Dequote(raw_type->substr(0, end));
  if (!dequoted.has_value()) {
    return std::unexpected(std::move(dequoted.error()));
  }
  return std::optional<std::string>{std::move(*dequoted)};
}

[[nodiscard]] SortOrder EffectiveSortOrder(SortOrder order,
                                           DatabaseSchemaFormat schema_format) noexcept {
  if (schema_format != DatabaseSchemaFormat::kFour) {
    return SortOrder::kAscending;
  }
  return order == SortOrder::kDescending ? SortOrder::kDescending : SortOrder::kAscending;
}

[[nodiscard]] std::optional<std::string> CoerceText(const RecordFieldView& field) {
  if (field.type() == SqlValueType::kNull) {
    return std::nullopt;
  }
  const SqlValue text = CastValue(field.ToOwned(), CastTarget::kText);
  const std::optional<Utf8View> value = text.text_value();
  if (!value.has_value()) {
    std::terminate();
  }
  std::string bytes{value->bytes()};
  if (const std::size_t nul = bytes.find('\0'); nul != std::string::npos) {
    bytes.resize(nul);
  }
  return bytes;
}

[[nodiscard]] std::optional<std::uint32_t> ParseRootPage(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t root = 0;
  for (const char byte : value) {
    if (byte < '0' || byte > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(byte - '0');
    constexpr std::uint64_t kMaximum = std::numeric_limits<std::uint32_t>::max();
    if (root > (kMaximum - digit) / 10U) {
      return std::nullopt;
    }
    root = root * 10U + digit;
  }
  return static_cast<std::uint32_t>(root);
}

[[nodiscard]] Result<std::array<RecordFieldView, 5>> DecodeSchemaFields(
    const ByteBuffer& payload, RecordCodecOptions options) {
  auto record = RecordView::Parse(payload.view(), options);
  if (!record.has_value() || record->field_count() != 5U) {
    return std::unexpected(Corruption("sqlite_schema row has an invalid record shape"));
  }
  std::array<RecordFieldView, 5> fields;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    auto field = record->field(index);
    if (!field.has_value()) {
      return std::unexpected(Corruption("sqlite_schema field cannot be decoded"));
    }
    fields[index] = *field;
  }
  return fields;
}

[[nodiscard]] Result<std::array<std::optional<std::string>, 3>> DecodeStat1Fields(
    const ByteBuffer& payload, RecordCodecOptions options,
    const std::array<std::size_t, 3>& columns) {
  auto record = RecordView::Parse(payload.view(), options);
  if (!record.has_value()) {
    return std::unexpected(Corruption("sqlite_stat1 row is not a valid record"));
  }
  std::array<std::optional<std::string>, 3> fields;
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (columns[index] >= record->field_count()) {
      continue;
    }
    auto field = record->field(columns[index]);
    if (!field.has_value()) {
      return std::unexpected(Corruption("sqlite_stat1 field cannot be decoded"));
    }
    fields[index] = CoerceText(*field);
  }
  return fields;
}

void NormalizeDefaultVariables(std::vector<Expression>& expressions, ExpressionId root) {
  std::vector<ExpressionId> pending{root};
  while (!pending.empty()) {
    const ExpressionId id = pending.back();
    pending.pop_back();
    Expression& expression = expressions[id.value];
    if (const auto* variable = std::get_if<VariableExpression>(&expression.payload);
        variable != nullptr) {
      expression.payload = LiteralExpression{.kind = LiteralKind::kNull, .token = variable->token};
      continue;
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload); unary != nullptr) {
      pending.push_back(unary->operand);
    } else if (const auto* binary = std::get_if<BinaryExpression>(&expression.payload);
               binary != nullptr) {
      pending.push_back(binary->left);
      pending.push_back(binary->right);
    } else if (const auto* function = std::get_if<FunctionCallExpression>(&expression.payload);
               function != nullptr) {
      pending.insert(pending.end(), function->arguments.begin(), function->arguments.end());
    } else if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
               collate != nullptr) {
      pending.push_back(collate->operand);
    } else if (const auto* parenthesized =
                   std::get_if<ParenthesizedExpression>(&expression.payload);
               parenthesized != nullptr) {
      pending.push_back(parenthesized->inner);
    }
  }
}

[[nodiscard]] bool ExpressionContainsVariable(const SyntaxTree& tree, ExpressionId root) {
  std::vector<ExpressionId> pending{root};
  while (!pending.empty()) {
    const ExpressionId id = pending.back();
    pending.pop_back();
    const Expression& expression = tree.expression(id);
    if (std::holds_alternative<VariableExpression>(expression.payload)) {
      return true;
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload); unary != nullptr) {
      pending.push_back(unary->operand);
    } else if (const auto* binary = std::get_if<BinaryExpression>(&expression.payload);
               binary != nullptr) {
      pending.push_back(binary->left);
      pending.push_back(binary->right);
    } else if (const auto* function = std::get_if<FunctionCallExpression>(&expression.payload);
               function != nullptr) {
      pending.insert(pending.end(), function->arguments.begin(), function->arguments.end());
    } else if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
               collate != nullptr) {
      pending.push_back(collate->operand);
    } else if (const auto* parenthesized =
                   std::get_if<ParenthesizedExpression>(&expression.payload);
               parenthesized != nullptr) {
      pending.push_back(parenthesized->inner);
    }
  }
  return false;
}

[[nodiscard]] bool HasOnlyStatementTail(Utf8View source, ByteOffset offset) {
  Lexer lexer{Utf8View{source.bytes().substr(offset.value())}};
  while (true) {
    const Token token = lexer.Next();
    if (token.kind == TokenKind::kEndOfInput) {
      return true;
    }
    if (!IsTrivia(token.kind) && token.kind != TokenKind::kSemicolon) {
      return false;
    }
  }
}

[[nodiscard]] std::uint32_t SqliteAtoi32(std::string_view value) noexcept {
  if (value.size() >= 3U && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
    const auto hex_value = [](char byte) -> std::optional<std::uint32_t> {
      if (byte >= '0' && byte <= '9') {
        return static_cast<std::uint32_t>(byte - '0');
      }
      if (byte >= 'a' && byte <= 'f') {
        return static_cast<std::uint32_t>(byte - 'a' + 10);
      }
      if (byte >= 'A' && byte <= 'F') {
        return static_cast<std::uint32_t>(byte - 'A' + 10);
      }
      return std::nullopt;
    };
    if (!hex_value(value[2]).has_value()) {
      return 0;
    }
    std::size_t offset = 2U;
    while (offset < value.size() && value[offset] == '0') {
      ++offset;
    }
    std::uint32_t parsed = 0;
    std::size_t digits = 0;
    while (offset + digits < value.size() && digits < 8U) {
      const std::optional<std::uint32_t> digit = hex_value(value[offset + digits]);
      if (!digit.has_value()) {
        break;
      }
      parsed = parsed * 16U + *digit;
      ++digits;
    }
    if (offset + digits < value.size() && hex_value(value[offset + digits]).has_value()) {
      return 0;
    }
    return (parsed & 0x80000000U) == 0U ? parsed : 0U;
  }

  std::size_t offset = 0;
  while (offset < value.size() && value[offset] == '0') {
    ++offset;
  }
  std::uint64_t parsed = 0;
  std::size_t digits = 0;
  while (offset + digits < value.size() && digits < 11U && value[offset + digits] >= '0' &&
         value[offset + digits] <= '9') {
    parsed = parsed * 10U + static_cast<std::uint64_t>(value[offset + digits] - '0');
    ++digits;
  }
  if (digits == 0U || digits > 10U ||
      parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
    return 0;
  }
  return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] ParsedStat1 DecodeStat1(std::string_view stat, std::size_t slots) {
  ParsedStat1 decoded;
  decoded.values.reserve(slots);
  std::size_t offset = 0;
  for (std::size_t slot = 0; offset < stat.size() && slot < slots; ++slot) {
    std::uint64_t value = 0;
    while (offset < stat.size() && stat[offset] >= '0' && stat[offset] <= '9') {
      value = value * 10U + static_cast<std::uint64_t>(stat[offset] - '0');
      ++offset;
    }
    decoded.values.push_back(value);
    if (offset < stat.size() && stat[offset] == ' ') {
      ++offset;
    }
  }

  while (offset < stat.size()) {
    const std::size_t token_begin = offset;
    while (offset < stat.size() && stat[offset] != ' ') {
      ++offset;
    }
    const std::string_view token = stat.substr(token_begin, offset - token_begin);
    if (token.starts_with("unordered")) {
      decoded.unordered = true;
    } else if (token.starts_with("noskipscan")) {
      decoded.no_skip_scan = true;
    } else if (token.starts_with("sz=")) {
      const std::string_view size_text = token.substr(3U);
      if (!size_text.empty() && size_text.front() >= '0' && size_text.front() <= '9') {
        decoded.average_row_size = std::max<std::uint64_t>(SqliteAtoi32(size_text), 2U);
      }
    }
    while (offset < stat.size() && stat[offset] == ' ') {
      ++offset;
    }
  }
  return decoded;
}

[[nodiscard]] IndexedTermClassification ClassifyIndexedTerm(const SyntaxTree& tree,
                                                            const IndexedTerm& term) {
  IndexedTermClassification classified{
      .expression = term.expression,
      .collation = term.collation,
  };
  while (true) {
    const Expression& expression = tree.expression(classified.expression);
    if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
        parenthesized != nullptr) {
      classified.expression = parenthesized->inner;
      continue;
    }
    if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
        collate != nullptr) {
      if (!classified.collation.has_value()) {
        classified.collation = collate->collation;
      }
      classified.expression = collate->operand;
      continue;
    }
    return classified;
  }
}

[[nodiscard]] bool SameIndexIdentity(const CatalogIndexTerm& left,
                                     const CatalogIndexTerm& right) noexcept {
  const auto* left_column = std::get_if<ColumnId>(&left.target);
  const auto* right_column = std::get_if<ColumnId>(&right.target);
  return left_column != nullptr && right_column != nullptr && *left_column == *right_column &&
         CatalogNamesEqual(left.collation_name, right.collation_name);
}

class CatalogLoader final {
 public:
  CatalogLoader(ReadPager& pager, CatalogLoadOptions options)
      : pager_(pager), options_(std::move(options)) {
    input_.schema_name = options_.schema_name;
    input_.version.generation = options_.generation;
  }

  [[nodiscard]] Result<CatalogSnapshotPtr> Run() {
    if (!pager_.in_read_transaction()) {
      return std::unexpected(Misuse("catalog loading requires an active read transaction"));
    }
    if (options_.maximum_schema_objects < 1U) {
      return std::unexpected(TooLarge("schema object limit excludes sqlite_schema"));
    }
    object_count_ = 1U;

    if (pager_.page_count() == 0U) {
      return Publish();
    }
    const DatabaseHeader* header = pager_.header();
    if (header == nullptr) {
      return std::unexpected(Corruption("nonempty database has no database header"));
    }
    input_.version.schema_cookie = header->schema_cookie();
    auto schema_format = NormalizeSchemaFormat(header->schema_format());
    if (!schema_format.has_value()) {
      return std::unexpected(std::move(schema_format.error()));
    }
    schema_format_ = *schema_format;
    auto text_encoding = NormalizeTextEncoding(header->text_encoding());
    if (!text_encoding.has_value()) {
      return std::unexpected(std::move(text_encoding.error()));
    }
    if (*text_encoding != DatabaseTextEncoding::kUtf8) {
      return std::unexpected(Protocol("UTF-16 catalog loading is not implemented"));
    }
    record_options_.schema_format = schema_format_;

    auto schema = ScanSchemaRows();
    if (!schema.has_value()) {
      return std::unexpected(std::move(schema.error()));
    }
    for (const CatalogIndexInput& index : input_.indexes) {
      if (index.root_page.value == 0U) {
        return std::unexpected(
            Corruption("automatic index has no sqlite_schema root row: " + index.name));
      }
    }
    auto statistics = LoadStatistics();
    if (!statistics.has_value()) {
      return std::unexpected(std::move(statistics.error()));
    }
    return Publish();
  }

 private:
  [[nodiscard]] Result<CatalogSnapshotPtr> Publish() {
    CatalogSnapshotResult snapshot = CatalogSnapshot::Create(std::move(input_));
    if (!snapshot.has_value()) {
      return std::unexpected(
          Corruption("catalog model rejected loader output: " + snapshot.error().detail));
    }
    return std::move(*snapshot);
  }

  [[nodiscard]] Status ConsumeObject() {
    if (object_count_ >= options_.maximum_schema_objects) {
      return std::unexpected(TooLarge("schema object limit exceeded"));
    }
    ++object_count_;
    return {};
  }

  [[nodiscard]] Status ValidateRoot(std::uint32_t root) const {
    if (root < 2U || root == std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(Corruption("schema object has an invalid root page"));
    }
    auto valid = pager_.ValidatePageNumber(PageNumber{root});
    if (!valid.has_value()) {
      return std::unexpected(std::move(valid.error()));
    }
    return {};
  }

  [[nodiscard]] Status ScanSchemaRows() {
    auto opened = TableBtreeCursor::Open(pager_, PageNumber{1});
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    TableBtreeCursor cursor = std::move(*opened);
    auto positioned = cursor.First();
    if (!positioned.has_value()) {
      return std::unexpected(std::move(positioned.error()));
    }

    std::size_t row_count = 0;
    while (*positioned) {
      if (row_count >= options_.maximum_schema_rows) {
        return std::unexpected(TooLarge("sqlite_schema row limit exceeded"));
      }
      ++row_count;
      auto rowid = cursor.rowid();
      if (!rowid.has_value()) {
        return std::unexpected(std::move(rowid.error()));
      }
      auto payload = cursor.CopyPayload();
      if (!payload.has_value()) {
        return std::unexpected(std::move(payload.error()));
      }
      auto fields = DecodeSchemaFields(*payload, record_options_);
      if (!fields.has_value()) {
        return std::unexpected(std::move(fields.error()));
      }

      std::optional<std::string> name = CoerceText((*fields)[1]);
      std::optional<std::string> root_text = CoerceText((*fields)[3]);
      if (!name.has_value() || !root_text.has_value()) {
        return std::unexpected(Corruption("sqlite_schema name or root page is NULL"));
      }
      const std::optional<std::uint32_t> root = ParseRootPage(*root_text);
      if (!root.has_value()) {
        return std::unexpected(
            Corruption("sqlite_schema root page is not an unsigned decimal value"));
      }
      SchemaRow row{
          .rowid = *rowid,
          .type = CoerceText((*fields)[0]),
          .name = std::move(*name),
          .table_name = CoerceText((*fields)[2]),
          .root_page = *root,
          .sql = CoerceText((*fields)[4]),
      };
      const bool blank_sql = !row.sql.has_value() || row.sql->empty();
      auto processed = blank_sql ? AttachAutomaticIndexRoot(row) : ProcessDefinitionRow(row);
      if (!processed.has_value()) {
        return processed;
      }

      positioned = cursor.Next();
      if (!positioned.has_value()) {
        return std::unexpected(std::move(positioned.error()));
      }
    }
    return {};
  }

  [[nodiscard]] Result<SyntaxTree> ParseDefinition(std::string_view sql) const {
    if (sql.size() > options_.maximum_sql_bytes) {
      return std::unexpected(TooLarge("stored schema SQL exceeds the configured limit"));
    }
    if (sql.size() < 2U ||
        SqliteToLower(static_cast<std::uint8_t>(static_cast<unsigned char>(sql[0]))) != 'c' ||
        SqliteToLower(static_cast<std::uint8_t>(static_cast<unsigned char>(sql[1]))) != 'r') {
      return std::unexpected(Corruption("stored schema SQL does not begin with CREATE"));
    }
    ParseResult parsed =
        ParseOne(Utf8View{sql}, ParseOptions{.maximum_source_bytes = options_.maximum_sql_bytes,
                                             .maximum_columns = options_.maximum_columns});
    if (!parsed.has_value()) {
      if (parsed.error().code == ParseErrorCode::kResourceLimitExceeded) {
        return std::unexpected(TooLarge("stored schema SQL exceeds a parser resource limit"));
      }
      if (parsed.error().code == ParseErrorCode::kUnsupportedSyntax) {
        return std::unexpected(Protocol("stored schema SQL uses an unsupported feature"));
      }
      return std::unexpected(Corruption("stored schema SQL cannot be parsed"));
    }
    if (!parsed->tree.has_value() || !HasOnlyStatementTail(Utf8View{sql}, parsed->next_offset)) {
      return std::unexpected(Corruption("stored schema SQL contains extra statements"));
    }

    const SyntaxTree& original = *parsed->tree;
    std::vector<ExpressionId> defaults_to_normalize;
    if (const auto* table = std::get_if<CreateTableStatement>(&original.statement());
        table != nullptr) {
      for (const ColumnDefinition& column : table->columns) {
        for (const ColumnConstraint& constraint : column.constraints) {
          if (const auto* default_constraint =
                  std::get_if<DefaultColumnConstraint>(&constraint.payload);
              default_constraint != nullptr &&
              ExpressionContainsVariable(original, default_constraint->expression)) {
            defaults_to_normalize.push_back(default_constraint->expression);
          }
        }
      }
    }
    if (defaults_to_normalize.empty()) {
      return std::move(*parsed->tree);
    }

    std::string source{original.source().bytes()};
    std::vector<Expression> expressions{original.expressions().begin(),
                                        original.expressions().end()};
    Statement statement = original.statement();
    for (const ExpressionId expression : defaults_to_normalize) {
      NormalizeDefaultVariables(expressions, expression);
    }
    auto tree = SyntaxTree::Create(std::move(source), std::move(expressions), std::move(statement));
    if (!tree.has_value()) {
      return std::unexpected(Corruption("normalized schema syntax tree is invalid"));
    }
    return std::move(*tree);
  }

  [[nodiscard]] Status ProcessDefinitionRow(const SchemaRow& row) {
    if (!row.type.has_value() || !row.table_name.has_value() || !row.sql.has_value()) {
      return std::unexpected(Corruption("nonblank sqlite_schema row has NULL required fields"));
    }
    if (EqualsAsciiCaseInsensitive(*row.type, "view") ||
        EqualsAsciiCaseInsensitive(*row.type, "trigger")) {
      return std::unexpected(Protocol("views and triggers are not supported"));
    }
    auto tree = ParseDefinition(*row.sql);
    if (!tree.has_value()) {
      return std::unexpected(std::move(tree.error()));
    }
    const SchemaDefinitionId definition{input_.definitions.size()};
    input_.definitions.push_back(std::move(*tree));
    const SyntaxTree& stored_tree = input_.definitions.back();

    if (const auto* table = std::get_if<CreateTableStatement>(&stored_tree.statement());
        table != nullptr) {
      if (!EqualsAsciiCaseInsensitive(*row.type, "table")) {
        return std::unexpected(Corruption("CREATE TABLE row has a non-table object type"));
      }
      return BuildTable(row, definition, stored_tree, *table);
    }
    if (const auto* index = std::get_if<CreateIndexStatement>(&stored_tree.statement());
        index != nullptr) {
      if (!EqualsAsciiCaseInsensitive(*row.type, "index")) {
        return std::unexpected(Corruption("CREATE INDEX row has a non-index object type"));
      }
      return BuildExplicitIndex(row, definition, stored_tree, *index);
    }
    return std::unexpected(Protocol("persistent SELECT definitions are not supported"));
  }

  [[nodiscard]] Result<ColumnId> ResolveColumn(
      const SyntaxTree& tree, const QualifiedName& name,
      const std::unordered_map<std::string, ColumnId>& columns) const {
    if (name.parts.size() != 1U) {
      return std::unexpected(Corruption("schema expression contains a qualified column reference"));
    }
    auto dequoted = DequoteSpan(tree, name.parts.front());
    if (!dequoted.has_value()) {
      return std::unexpected(std::move(dequoted.error()));
    }
    const auto found = columns.find(FoldName(*dequoted));
    if (found == columns.end()) {
      return std::unexpected(Corruption("schema expression references a nonexistent column"));
    }
    return found->second;
  }

  [[nodiscard]] Status ValidateKnownFunction(const SyntaxTree& tree,
                                             const FunctionCallExpression& function,
                                             SchemaExpressionContext context) const {
    if (function.name.parts.size() != 1U) {
      return std::unexpected(Corruption("schema expression contains a qualified function name"));
    }
    auto name = DequoteSpan(tree, function.name.parts.front());
    if (!name.has_value()) {
      return std::unexpected(std::move(name.error()));
    }
    const ScalarFunction* selected = nullptr;
    bool name_found = false;
    for (const ScalarFunction& candidate : CoreFunctionRegistry().functions()) {
      if (!CatalogNamesEqual(candidate.name(), *name)) {
        continue;
      }
      name_found = true;
      if (!candidate.arity().Accepts(function.arguments.size())) {
        continue;
      }
      if (selected == nullptr || (!selected->arity().is_exact() && candidate.arity().is_exact())) {
        selected = &candidate;
      }
    }
    if (name_found && selected == nullptr) {
      return std::unexpected(
          Corruption("schema expression calls a known function with invalid arity"));
    }
    const bool requires_determinism = context == SchemaExpressionContext::kIndex ||
                                      context == SchemaExpressionContext::kPartialIndex;
    if (selected != nullptr && requires_determinism && !selected->is_deterministic()) {
      return std::unexpected(
          Corruption("index schema expression calls a nondeterministic function"));
    }
    return {};
  }

  [[nodiscard]] Status ValidateExpression(const SyntaxTree& tree, ExpressionId root,
                                          SchemaExpressionContext context,
                                          const std::unordered_map<std::string, ColumnId>& columns,
                                          bool has_rowid) const {
    std::vector<ExpressionId> pending{root};
    while (!pending.empty()) {
      const ExpressionId id = pending.back();
      pending.pop_back();
      const Expression& expression = tree.expression(id);
      if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
          literal != nullptr) {
        const bool current_time = literal->kind == LiteralKind::kCurrentDate ||
                                  literal->kind == LiteralKind::kCurrentTime ||
                                  literal->kind == LiteralKind::kCurrentTimestamp;
        if (current_time && (context == SchemaExpressionContext::kIndex ||
                             context == SchemaExpressionContext::kPartialIndex)) {
          return std::unexpected(
              Corruption("index schema expression uses a nondeterministic time value"));
        }
        continue;
      }
      if (std::holds_alternative<VariableExpression>(expression.payload)) {
        return std::unexpected(Corruption("schema expression contains a variable"));
      }
      if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.payload);
          identifier != nullptr) {
        if (context == SchemaExpressionContext::kDefault) {
          return std::unexpected(Corruption("DEFAULT expression contains a column reference"));
        }
        if (identifier->name.parts.size() != 1U) {
          return std::unexpected(
              Corruption("schema expression contains a qualified column reference"));
        }
        auto name = DequoteSpan(tree, identifier->name.parts.front());
        if (!name.has_value()) {
          return std::unexpected(std::move(name.error()));
        }
        if (columns.contains(FoldName(*name))) {
          continue;
        }
        const bool rowid_context = context == SchemaExpressionContext::kCheck ||
                                   context == SchemaExpressionContext::kPartialIndex;
        const bool rowid_name = CatalogNamesEqual(*name, "rowid") ||
                                CatalogNamesEqual(*name, "_rowid_") ||
                                CatalogNamesEqual(*name, "oid");
        if (has_rowid && rowid_context && rowid_name) {
          continue;
        }
        return std::unexpected(Corruption("schema expression references a nonexistent column"));
      }
      if (std::holds_alternative<WildcardExpression>(expression.payload)) {
        return std::unexpected(Corruption("schema expression contains a wildcard"));
      }
      if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload); unary != nullptr) {
        pending.push_back(unary->operand);
      } else if (const auto* binary = std::get_if<BinaryExpression>(&expression.payload);
                 binary != nullptr) {
        pending.push_back(binary->left);
        pending.push_back(binary->right);
      } else if (const auto* function = std::get_if<FunctionCallExpression>(&expression.payload);
                 function != nullptr) {
        if (function->distinct) {
          return std::unexpected(Corruption("schema expression contains a DISTINCT function call"));
        }
        if (context != SchemaExpressionContext::kDefault) {
          auto valid = ValidateKnownFunction(tree, *function, context);
          if (!valid.has_value()) {
            return valid;
          }
        }
        pending.insert(pending.end(), function->arguments.begin(), function->arguments.end());
      } else if (const auto* collate = std::get_if<CollateExpression>(&expression.payload);
                 collate != nullptr) {
        pending.push_back(collate->operand);
      } else if (const auto* parenthesized =
                     std::get_if<ParenthesizedExpression>(&expression.payload);
                 parenthesized != nullptr) {
        pending.push_back(parenthesized->inner);
      }
    }
    return {};
  }

  [[nodiscard]] Result<CatalogIndexTerm> ResolveIndexedTerm(
      const SyntaxTree& tree, const IndexedTerm& term, SchemaExpressionContext context,
      SchemaDefinitionId definition, const CatalogTableInput& table,
      const std::unordered_map<std::string, ColumnId>& columns, bool require_column) const {
    const IndexedTermClassification classified = ClassifyIndexedTerm(tree, term);
    const Expression& expression = tree.expression(classified.expression);
    std::optional<ColumnId> column;
    if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.payload);
        identifier != nullptr) {
      auto resolved = ResolveColumn(tree, identifier->name, columns);
      if (!resolved.has_value()) {
        return std::unexpected(std::move(resolved.error()));
      }
      column = *resolved;
    } else if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
               literal != nullptr && literal->kind == LiteralKind::kString) {
      auto legacy_name = DequoteSpan(tree, literal->token);
      if (!legacy_name.has_value()) {
        return std::unexpected(std::move(legacy_name.error()));
      }
      const auto found = columns.find(FoldName(*legacy_name));
      if (found == columns.end()) {
        return std::unexpected(Corruption("legacy single-quoted index term names no column"));
      }
      column = found->second;
    }

    if (!column.has_value() && require_column) {
      return std::unexpected(Corruption("PRIMARY KEY and UNIQUE constraints require column terms"));
    }
    if (!column.has_value()) {
      auto valid =
          ValidateExpression(tree, term.expression, context, columns, !table.without_rowid);
      if (!valid.has_value()) {
        return std::unexpected(std::move(valid.error()));
      }
    }

    std::string collation = "BINARY";
    if (classified.collation.has_value()) {
      auto explicit_collation = DequoteSpan(tree, *classified.collation);
      if (!explicit_collation.has_value()) {
        return std::unexpected(std::move(explicit_collation.error()));
      }
      collation = std::move(*explicit_collation);
    } else if (column.has_value()) {
      collation = table.columns[column->value].collation_name;
    }

    return CatalogIndexTerm{
        .target = column.has_value()
                      ? CatalogIndexTermTarget{*column}
                      : CatalogIndexTermTarget{SchemaExpression{.definition = definition,
                                                                .expression = term.expression}},
        .collation_name = std::move(collation),
        .order = EffectiveSortOrder(term.order, schema_format_),
    };
  }

  [[nodiscard]] Status MergeConflictAction(CatalogIndexInput& existing,
                                           ConflictAction incoming) const {
    if (!existing.conflict_action.has_value()) {
      return std::unexpected(Corruption("automatic index has no conflict policy"));
    }
    ConflictAction& current = *existing.conflict_action;
    if (current == ConflictAction::kDefault) {
      current = incoming;
    } else if (incoming != ConflictAction::kDefault && incoming != current) {
      return std::unexpected(
          Corruption("equivalent constraints have incompatible conflict actions"));
    }
    return {};
  }

  [[nodiscard]] Result<std::size_t> AddAutomaticIndex(std::vector<CatalogIndexInput>& indexes,
                                                      std::string_view table_name,
                                                      SchemaDefinitionId definition, TableId table,
                                                      IndexOrigin origin, ConflictAction conflict,
                                                      std::vector<CatalogIndexTerm> terms) {
    for (std::size_t index = 0; index < indexes.size(); ++index) {
      CatalogIndexInput& existing = indexes[index];
      if (existing.key_term_count != terms.size()) {
        continue;
      }
      bool equivalent = true;
      for (std::size_t term = 0; term < terms.size(); ++term) {
        if (!SameIndexIdentity(existing.terms[term], terms[term])) {
          equivalent = false;
          break;
        }
      }
      if (!equivalent) {
        continue;
      }
      auto merged = MergeConflictAction(existing, conflict);
      if (!merged.has_value()) {
        return std::unexpected(std::move(merged.error()));
      }
      if (origin == IndexOrigin::kPrimaryKey) {
        existing.origin = IndexOrigin::kPrimaryKey;
      }
      return index;
    }

    auto consumed = ConsumeObject();
    if (!consumed.has_value()) {
      return std::unexpected(std::move(consumed.error()));
    }
    const std::size_t ordinal = indexes.size() + 1U;
    indexes.push_back(CatalogIndexInput{
        .definition = definition,
        .name = "sqlite_autoindex_" + std::string{table_name} + "_" + std::to_string(ordinal),
        .table = table,
        .origin = origin,
        .unique = true,
        .conflict_action = conflict,
        .key_term_count = terms.size(),
        .terms = std::move(terms),
    });
    return indexes.size() - 1U;
  }

  [[nodiscard]] Status FinalizeAutomaticIndexes(CatalogTableInput& table,
                                                std::vector<CatalogIndexInput>& indexes) const {
    CatalogIndexInput* primary = nullptr;
    for (CatalogIndexInput& index : indexes) {
      if (index.origin == IndexOrigin::kPrimaryKey) {
        primary = &index;
        break;
      }
    }
    if (table.without_rowid && primary == nullptr) {
      return std::unexpected(Corruption("WITHOUT ROWID table has no primary key"));
    }

    if (table.without_rowid) {
      std::vector<CatalogIndexTerm> normalized;
      normalized.reserve(primary->key_term_count);
      for (std::size_t term = 0; term < primary->key_term_count; ++term) {
        const CatalogIndexTerm& candidate = primary->terms[term];
        if (std::ranges::none_of(normalized, [&](const CatalogIndexTerm& existing) {
              return SameIndexIdentity(existing, candidate);
            })) {
          normalized.push_back(candidate);
        }
      }
      primary->terms = std::move(normalized);
      primary->key_term_count = primary->terms.size();
      primary->root_page = table.root_page;

      for (std::size_t column = 0; column < table.columns.size(); ++column) {
        const bool is_key_column =
            std::ranges::any_of(primary->terms, [column](const CatalogIndexTerm& term) {
              const auto* id = std::get_if<ColumnId>(&term.target);
              return id != nullptr && id->value == column;
            });
        if (!is_key_column) {
          primary->terms.push_back(CatalogIndexTerm{
              .target = ColumnId{column},
              .collation_name = table.columns[column].collation_name,
              .order = SortOrder::kAscending,
          });
        }
      }

      for (CatalogIndexInput& index : indexes) {
        if (&index == primary) {
          continue;
        }
        for (std::size_t term = 0; term < primary->key_term_count; ++term) {
          CatalogIndexTerm suffix = primary->terms[term];
          if (std::ranges::any_of(
                  std::span<const CatalogIndexTerm>{index.terms}.first(index.key_term_count),
                  [&](const CatalogIndexTerm& existing) {
                    return SameIndexIdentity(existing, suffix);
                  })) {
            continue;
          }
          suffix.order = SortOrder::kAscending;
          index.terms.push_back(std::move(suffix));
        }
      }
      return {};
    }

    for (CatalogIndexInput& index : indexes) {
      index.terms.push_back(CatalogIndexTerm{
          .target = RowIdIndexTerm{},
          .collation_name = "BINARY",
          .order = SortOrder::kAscending,
      });
    }
    return {};
  }

  [[nodiscard]] Status BuildTable(const SchemaRow& row, SchemaDefinitionId definition,
                                  const SyntaxTree& tree, const CreateTableStatement& statement) {
    if (statement.temporary) {
      return std::unexpected(Protocol("temporary tables are not persistent schema objects"));
    }
    auto table_name = UnqualifiedName(tree, statement.name);
    if (!table_name.has_value()) {
      return std::unexpected(std::move(table_name.error()));
    }
    if (!row.table_name.has_value()) {
      return std::unexpected(Corruption("table schema row is missing its owning table name"));
    }
    if (!EqualsAsciiCaseInsensitive(*table_name, row.name) ||
        !EqualsAsciiCaseInsensitive(*table_name, row.table_name.value())) {
      return std::unexpected(Corruption("CREATE TABLE identity disagrees with sqlite_schema"));
    }
    if (table_lookup_.contains(FoldName(*table_name))) {
      return std::unexpected(Corruption("sqlite_schema contains a duplicate table name"));
    }
    auto root_valid = ValidateRoot(row.root_page);
    if (!root_valid.has_value()) {
      return root_valid;
    }
    auto consumed = ConsumeObject();
    if (!consumed.has_value()) {
      return consumed;
    }

    const TableId table_id{input_.tables.size()};
    CatalogTableInput table{
        .definition = definition,
        .name = *table_name,
        .root_page = RootPageId{row.root_page},
        .without_rowid = statement.without_rowid,
        .strict = statement.strict,
    };
    table.columns.reserve(statement.columns.size());
    std::unordered_map<std::string, ColumnId> columns;
    columns.reserve(statement.columns.size());

    for (std::size_t index = 0; index < statement.columns.size(); ++index) {
      const ColumnDefinition& source_column = statement.columns[index];
      auto name = DequoteSpan(tree, source_column.name);
      if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
      }
      const std::string folded = FoldName(*name);
      if (columns.contains(folded)) {
        return std::unexpected(Corruption("CREATE TABLE contains duplicate column names"));
      }
      columns.emplace(folded, ColumnId{index});

      std::optional<std::string_view> raw_type;
      if (source_column.type_name.has_value()) {
        raw_type = SpanText(tree, *source_column.type_name);
      }
      auto declared_type = NormalizeDeclaredType(raw_type);
      if (!declared_type.has_value()) {
        return std::unexpected(std::move(declared_type.error()));
      }

      CatalogColumnInput column{
          .name = std::move(*name),
          .declared_type = std::move(*declared_type),
      };
      for (const ColumnConstraint& constraint : source_column.constraints) {
        if (const auto* collation = std::get_if<CollateColumnConstraint>(&constraint.payload);
            collation != nullptr) {
          auto name_value = DequoteSpan(tree, collation->collation);
          if (!name_value.has_value()) {
            return std::unexpected(std::move(name_value.error()));
          }
          column.collation_name = std::move(*name_value);
        } else if (const auto* not_null = std::get_if<NotNullColumnConstraint>(&constraint.payload);
                   not_null != nullptr) {
          column.not_null_conflict = not_null->conflict;
        }
      }
      table.columns.push_back(std::move(column));
    }

    std::vector<CatalogIndexInput> indexes;
    bool primary_seen = false;
    std::optional<std::tuple<std::vector<CatalogIndexTerm>, ConflictAction, bool, ColumnId>>
        deferred_rowid_primary;

    const auto register_primary = [&](std::vector<CatalogIndexTerm> terms, ConflictAction conflict,
                                      bool autoincrement, std::optional<ColumnId> possible_alias,
                                      bool declared_descending) -> Status {
      if (primary_seen) {
        return std::unexpected(Corruption("table declares more than one primary key"));
      }
      primary_seen = true;
      for (const CatalogIndexTerm& term : terms) {
        const auto* column = std::get_if<ColumnId>(&term.target);
        if (column == nullptr) {
          return std::unexpected(Corruption("primary key contains a non-column term"));
        }
        table.columns[column->value].primary_key = true;
      }

      bool rowid_alias = false;
      ColumnId alias{};
      if (possible_alias.has_value() && terms.size() == 1U && !declared_descending) {
        const CatalogColumnInput& column = table.columns[possible_alias->value];
        rowid_alias = column.declared_type.has_value() &&
                      EqualsAsciiCaseInsensitive(*column.declared_type, "INTEGER");
        alias = *possible_alias;
      }
      if (autoincrement && (!rowid_alias || table.without_rowid)) {
        return std::unexpected(
            Corruption("AUTOINCREMENT requires an INTEGER PRIMARY KEY rowid alias"));
      }
      if (rowid_alias && !table.without_rowid) {
        table.rowid_alias = alias;
        table.rowid_primary_key_conflict = conflict;
        table.autoincrement = autoincrement;
        return {};
      }
      if (rowid_alias && table.without_rowid) {
        deferred_rowid_primary = std::tuple{std::move(terms), conflict, autoincrement, alias};
        return {};
      }
      auto added = AddAutomaticIndex(indexes, table.name, definition, table_id,
                                     IndexOrigin::kPrimaryKey, conflict, std::move(terms));
      if (!added.has_value()) {
        return std::unexpected(std::move(added.error()));
      }
      return {};
    };

    for (std::size_t column_index = 0; column_index < statement.columns.size(); ++column_index) {
      const ColumnDefinition& source_column = statement.columns[column_index];
      for (const ColumnConstraint& constraint : source_column.constraints) {
        if (const auto* primary = std::get_if<PrimaryKeyColumnConstraint>(&constraint.payload);
            primary != nullptr) {
          auto status =
              register_primary({CatalogIndexTerm{
                                   .target = ColumnId{column_index},
                                   .collation_name = table.columns[column_index].collation_name,
                                   .order = EffectiveSortOrder(primary->order, schema_format_),
                               }},
                               primary->conflict, primary->autoincrement, ColumnId{column_index},
                               primary->order == SortOrder::kDescending);
          if (!status.has_value()) {
            return status;
          }
        } else if (const auto* unique = std::get_if<UniqueColumnConstraint>(&constraint.payload);
                   unique != nullptr) {
          auto added =
              AddAutomaticIndex(indexes, table.name, definition, table_id,
                                IndexOrigin::kUniqueConstraint, unique->conflict,
                                {CatalogIndexTerm{
                                    .target = ColumnId{column_index},
                                    .collation_name = table.columns[column_index].collation_name,
                                    .order = SortOrder::kAscending,
                                }});
          if (!added.has_value()) {
            return std::unexpected(std::move(added.error()));
          }
        } else if (const auto* check = std::get_if<CheckColumnConstraint>(&constraint.payload);
                   check != nullptr) {
          auto valid = ValidateExpression(tree, check->expression, SchemaExpressionContext::kCheck,
                                          columns, !table.without_rowid);
          if (!valid.has_value()) {
            return valid;
          }
          table.check_constraints.push_back(
              SchemaExpression{.definition = definition, .expression = check->expression});
        } else if (const auto* default_constraint =
                       std::get_if<DefaultColumnConstraint>(&constraint.payload);
                   default_constraint != nullptr) {
          auto valid =
              ValidateExpression(tree, default_constraint->expression,
                                 SchemaExpressionContext::kDefault, columns, !table.without_rowid);
          if (!valid.has_value()) {
            return valid;
          }
          table.columns[column_index].default_expression = SchemaExpression{
              .definition = definition, .expression = default_constraint->expression};
          auto missing_record_value = MaterializeDefaultValue(tree, default_constraint->expression);
          if (!missing_record_value.has_value()) {
            return std::unexpected(std::move(missing_record_value.error()));
          }
          table.columns[column_index].missing_record_value =
              missing_record_value->has_value()
                  ? std::make_shared<const SqlValue>(std::move(**missing_record_value))
                  : nullptr;
        }
      }
    }

    for (const TableConstraint& constraint : statement.constraints) {
      if (const auto* primary = std::get_if<PrimaryKeyTableConstraint>(&constraint.payload);
          primary != nullptr) {
        std::vector<CatalogIndexTerm> terms;
        terms.reserve(primary->terms.size());
        for (const IndexedTerm& term : primary->terms) {
          auto resolved = ResolveIndexedTerm(tree, term, SchemaExpressionContext::kIndex,
                                             definition, table, columns, true);
          if (!resolved.has_value()) {
            return std::unexpected(std::move(resolved.error()));
          }
          terms.push_back(std::move(*resolved));
        }
        std::optional<ColumnId> alias;
        if (terms.size() == 1U) {
          if (const auto* column = std::get_if<ColumnId>(&terms.front().target);
              column != nullptr) {
            alias = *column;
          }
        }
        auto status = register_primary(
            std::move(terms), primary->conflict, primary->autoincrement, alias,
            primary->terms.size() == 1U && primary->terms.front().order == SortOrder::kDescending);
        if (!status.has_value()) {
          return status;
        }
      } else if (const auto* unique = std::get_if<UniqueTableConstraint>(&constraint.payload);
                 unique != nullptr) {
        std::vector<CatalogIndexTerm> terms;
        terms.reserve(unique->terms.size());
        for (const IndexedTerm& term : unique->terms) {
          auto resolved = ResolveIndexedTerm(tree, term, SchemaExpressionContext::kIndex,
                                             definition, table, columns, true);
          if (!resolved.has_value()) {
            return std::unexpected(std::move(resolved.error()));
          }
          terms.push_back(std::move(*resolved));
        }
        auto added =
            AddAutomaticIndex(indexes, table.name, definition, table_id,
                              IndexOrigin::kUniqueConstraint, unique->conflict, std::move(terms));
        if (!added.has_value()) {
          return std::unexpected(std::move(added.error()));
        }
      } else if (const auto* check = std::get_if<CheckTableConstraint>(&constraint.payload);
                 check != nullptr) {
        auto valid = ValidateExpression(tree, check->expression, SchemaExpressionContext::kCheck,
                                        columns, !table.without_rowid);
        if (!valid.has_value()) {
          return valid;
        }
        table.check_constraints.push_back(
            SchemaExpression{.definition = definition, .expression = check->expression});
      }
    }

    if (deferred_rowid_primary.has_value()) {
      auto [terms, conflict, autoincrement, alias] = std::move(*deferred_rowid_primary);
      static_cast<void>(autoincrement);
      static_cast<void>(alias);
      auto added = AddAutomaticIndex(indexes, table.name, definition, table_id,
                                     IndexOrigin::kPrimaryKey, conflict, std::move(terms));
      if (!added.has_value()) {
        return std::unexpected(std::move(added.error()));
      }
    }
    if (table.without_rowid && !primary_seen) {
      return std::unexpected(Corruption("WITHOUT ROWID table has no primary key"));
    }
    auto finalized = FinalizeAutomaticIndexes(table, indexes);
    if (!finalized.has_value()) {
      return finalized;
    }

    input_.tables.push_back(std::move(table));
    table_lookup_.emplace(FoldName(*table_name), table_id);
    for (CatalogIndexInput& index : indexes) {
      auto appended = AppendIndex(std::move(index));
      if (!appended.has_value()) {
        return appended;
      }
    }
    return {};
  }

  [[nodiscard]] Status AppendIndex(CatalogIndexInput index) {
    const std::string folded = FoldName(index.name);
    if (index_lookup_.contains(folded)) {
      return std::unexpected(Corruption("sqlite_schema contains a duplicate index name"));
    }
    const IndexId id{input_.indexes.size()};
    input_.indexes.push_back(std::move(index));
    index_lookup_.emplace(folded, id);
    return {};
  }

  [[nodiscard]] const CatalogIndexInput* PrimaryIndex(TableId table) const {
    const auto found =
        std::ranges::find_if(input_.indexes, [table](const CatalogIndexInput& index) {
          return index.table == table && index.origin == IndexOrigin::kPrimaryKey;
        });
    return found == input_.indexes.end() ? nullptr : &*found;
  }

  [[nodiscard]] Status AppendSecondarySuffix(CatalogIndexInput& index,
                                             const CatalogTableInput& table,
                                             bool preserve_primary_order) const {
    if (!table.without_rowid) {
      index.terms.push_back(CatalogIndexTerm{
          .target = RowIdIndexTerm{},
          .collation_name = "BINARY",
          .order = SortOrder::kAscending,
      });
      return {};
    }
    const CatalogIndexInput* primary = PrimaryIndex(index.table);
    if (primary == nullptr) {
      return std::unexpected(Corruption("WITHOUT ROWID secondary index has no primary index"));
    }
    for (std::size_t term = 0; term < primary->key_term_count; ++term) {
      CatalogIndexTerm suffix = primary->terms[term];
      if (std::ranges::any_of(
              std::span<const CatalogIndexTerm>{index.terms}.first(index.key_term_count),
              [&](const CatalogIndexTerm& existing) {
                return SameIndexIdentity(existing, suffix);
              })) {
        continue;
      }
      if (!preserve_primary_order) {
        suffix.order = SortOrder::kAscending;
      }
      index.terms.push_back(std::move(suffix));
    }
    return {};
  }

  [[nodiscard]] Status BuildExplicitIndex(const SchemaRow& row, SchemaDefinitionId definition,
                                          const SyntaxTree& tree,
                                          const CreateIndexStatement& statement) {
    auto index_name = UnqualifiedName(tree, statement.name);
    if (!index_name.has_value()) {
      return std::unexpected(std::move(index_name.error()));
    }
    auto table_name = UnqualifiedName(tree, statement.table);
    if (!table_name.has_value()) {
      return std::unexpected(std::move(table_name.error()));
    }
    if (!row.table_name.has_value()) {
      return std::unexpected(Corruption("index schema row is missing its owning table name"));
    }
    if (!EqualsAsciiCaseInsensitive(*index_name, row.name) ||
        !EqualsAsciiCaseInsensitive(*table_name, row.table_name.value())) {
      return std::unexpected(Corruption("CREATE INDEX identity disagrees with sqlite_schema"));
    }
    const auto found_table = table_lookup_.find(FoldName(*table_name));
    if (found_table == table_lookup_.end()) {
      return std::unexpected(Corruption("CREATE INDEX refers to an unknown table"));
    }
    auto root_valid = ValidateRoot(row.root_page);
    if (!root_valid.has_value()) {
      return root_valid;
    }
    auto consumed = ConsumeObject();
    if (!consumed.has_value()) {
      return consumed;
    }

    const TableId table_id = found_table->second;
    const CatalogTableInput& table = input_.tables[table_id.value];
    std::unordered_map<std::string, ColumnId> columns;
    columns.reserve(table.columns.size());
    for (std::size_t column = 0; column < table.columns.size(); ++column) {
      columns.emplace(FoldName(table.columns[column].name), ColumnId{column});
    }

    CatalogIndexInput index{
        .definition = definition,
        .name = *index_name,
        .table = table_id,
        .root_page = RootPageId{row.root_page},
        .origin = IndexOrigin::kCreateIndex,
        .unique = statement.unique,
        .conflict_action =
            statement.unique ? std::optional{ConflictAction::kDefault} : std::nullopt,
    };
    index.terms.reserve(statement.terms.size() + 2U);
    for (const IndexedTerm& term : statement.terms) {
      auto resolved = ResolveIndexedTerm(tree, term, SchemaExpressionContext::kIndex, definition,
                                         table, columns, false);
      if (!resolved.has_value()) {
        return std::unexpected(std::move(resolved.error()));
      }
      index.terms.push_back(std::move(*resolved));
    }
    index.key_term_count = index.terms.size();
    if (statement.where.has_value()) {
      auto valid =
          ValidateExpression(tree, *statement.where, SchemaExpressionContext::kPartialIndex,
                             columns, !table.without_rowid);
      if (!valid.has_value()) {
        return valid;
      }
      index.partial_predicate =
          SchemaExpression{.definition = definition, .expression = *statement.where};
    }
    auto suffix = AppendSecondarySuffix(index, table, true);
    if (!suffix.has_value()) {
      return suffix;
    }
    return AppendIndex(std::move(index));
  }

  [[nodiscard]] Status AttachAutomaticIndexRoot(const SchemaRow& row) {
    const auto found = index_lookup_.find(FoldName(row.name));
    if (found == index_lookup_.end()) {
      return std::unexpected(Corruption("blank sqlite_schema row names no automatic index"));
    }
    CatalogIndexInput& index = input_.indexes[found->second.value];
    if (index.origin == IndexOrigin::kCreateIndex) {
      return std::unexpected(Corruption("blank sqlite_schema row refers to an explicit index"));
    }
    auto valid = ValidateRoot(row.root_page);
    if (!valid.has_value()) {
      return valid;
    }
    index.root_page = RootPageId{row.root_page};
    return {};
  }

  void ApplyDecodedStatistics(IndexStatistics& statistics, const ParsedStat1& decoded) const {
    statistics.has_stat1 = true;
    if (statistics.rows_per_prefix.size() < decoded.values.size()) {
      statistics.rows_per_prefix.resize(decoded.values.size());
    }
    std::ranges::copy(decoded.values, statistics.rows_per_prefix.begin());
    statistics.unordered = decoded.unordered;
    statistics.no_skip_scan = decoded.no_skip_scan;
    if (decoded.average_row_size.has_value()) {
      statistics.average_row_size = decoded.average_row_size;
    }
  }

  void ApplyDecodedStatistics(TableStatistics& statistics, const ParsedStat1& decoded) const {
    statistics.has_stat1 = true;
    if (!decoded.values.empty()) {
      statistics.estimated_rows = decoded.values.front();
    }
    if (decoded.average_row_size.has_value()) {
      statistics.average_row_size = decoded.average_row_size;
    }
  }

  [[nodiscard]] Status ApplyStat1Row(std::optional<std::string> table_name,
                                     std::optional<std::string> index_name,
                                     std::optional<std::string> stat) {
    if (!table_name.has_value() || !stat.has_value()) {
      return {};
    }
    const auto found_table = table_lookup_.find(FoldName(*table_name));
    if (found_table == table_lookup_.end()) {
      return {};
    }
    const TableId table_id = found_table->second;
    CatalogTableInput& table = input_.tables[table_id.value];

    std::optional<IndexId> target_index;
    if (index_name.has_value()) {
      if (CatalogNamesEqual(*index_name, *table_name)) {
        const CatalogIndexInput* primary = PrimaryIndex(table_id);
        if (primary != nullptr) {
          target_index = IndexId{static_cast<std::size_t>(primary - input_.indexes.data())};
        }
      } else {
        const auto found_index = index_lookup_.find(FoldName(*index_name));
        if (found_index != index_lookup_.end()) {
          target_index = found_index->second;
        }
      }
    }

    if (!target_index.has_value()) {
      const ParsedStat1 decoded = DecodeStat1(*stat, 1U);
      ApplyDecodedStatistics(table.statistics, decoded);
      return {};
    }

    CatalogIndexInput& index = input_.indexes[target_index->value];
    const ParsedStat1 decoded = DecodeStat1(*stat, index.key_term_count + 1U);
    ApplyDecodedStatistics(index.statistics, decoded);
    if (!index.partial_predicate.has_value()) {
      table.statistics.has_stat1 = true;
      table.statistics.estimated_rows =
          index.statistics.rows_per_prefix.empty()
              ? std::nullopt
              : std::optional{index.statistics.rows_per_prefix.front()};
    }
    return {};
  }

  [[nodiscard]] Status LoadStatistics() {
    const auto found = table_lookup_.find(FoldName("sqlite_stat1"));
    if (found == table_lookup_.end()) {
      return {};
    }
    const CatalogTableInput& stat_table = input_.tables[found->second.value];
    if (stat_table.without_rowid) {
      return std::unexpected(Protocol("WITHOUT ROWID sqlite_stat1 tables are not supported"));
    }
    std::array<std::size_t, 3> selected_columns{};
    std::array<bool, 3> column_selected{};
    constexpr std::array<std::string_view, 3> kNames{"tbl", "idx", "stat"};
    for (std::size_t column = 0; column < stat_table.columns.size(); ++column) {
      for (std::size_t selected = 0; selected < kNames.size(); ++selected) {
        if (CatalogNamesEqual(stat_table.columns[column].name, kNames[selected])) {
          selected_columns[selected] = column;
          column_selected[selected] = true;
        }
      }
    }
    if (std::ranges::any_of(column_selected, [](bool selected) { return !selected; })) {
      return std::unexpected(Corruption("sqlite_stat1 is missing a required column"));
    }
    auto opened = TableBtreeCursor::Open(pager_, PageNumber{stat_table.root_page.value});
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    TableBtreeCursor cursor = std::move(*opened);
    auto positioned = cursor.First();
    if (!positioned.has_value()) {
      return std::unexpected(std::move(positioned.error()));
    }
    while (*positioned) {
      auto payload = cursor.CopyPayload();
      if (!payload.has_value()) {
        return std::unexpected(std::move(payload.error()));
      }
      auto fields = DecodeStat1Fields(*payload, record_options_, selected_columns);
      if (!fields.has_value()) {
        return std::unexpected(std::move(fields.error()));
      }
      auto applied =
          ApplyStat1Row(std::move((*fields)[0]), std::move((*fields)[1]), std::move((*fields)[2]));
      if (!applied.has_value()) {
        return applied;
      }
      positioned = cursor.Next();
      if (!positioned.has_value()) {
        return std::unexpected(std::move(positioned.error()));
      }
    }
    return {};
  }

  ReadPager& pager_;
  CatalogLoadOptions options_;
  CatalogInput input_;
  DatabaseSchemaFormat schema_format_ = DatabaseSchemaFormat::kFour;
  RecordCodecOptions record_options_{};
  std::size_t object_count_ = 0;
  std::unordered_map<std::string, TableId> table_lookup_;
  std::unordered_map<std::string, IndexId> index_lookup_;
};

}  // namespace

Result<CatalogSnapshotPtr> LoadCatalog(ReadPager& pager) {
  try {
    return CatalogLoader{pager, CatalogLoadOptions{}}.Run();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<CatalogSnapshotPtr> LoadCatalog(ReadPager& pager, const CatalogLoadOptions& options) {
  try {
    return CatalogLoader{pager, CatalogLoadOptions{options}}.Run();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<bool> CatalogRequiresReload(const ReadPager& pager, const CatalogSnapshot& catalog) {
  if (!pager.in_read_transaction()) {
    return std::unexpected(Misuse("catalog reload detection requires an active read transaction"));
  }
  const DatabaseHeader* header = pager.header();
  const std::uint32_t schema_cookie = header == nullptr ? 0U : header->schema_cookie();
  return schema_cookie != catalog.version().schema_cookie;
}

}  // namespace modern_sqlite
